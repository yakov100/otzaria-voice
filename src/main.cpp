// Otzaria voice service: local Hebrew speech recognition for Otzaria plugins.
//
// Listens on http://127.0.0.1:47811 only. A plugin starts dictation with
// POST /dictate (network.fetchStream) and receives newline-delimited JSON while
// the user speaks; POST /stop ends it after the last sentence is recognized.
// Idle, the service holds no model and trims its memory; the model is loaded on
// the first dictation and released after a few idle minutes.
//
//   otzaria-voice.exe              run (normally started at login)
//   otzaria-voice.exe --install    start at login (current user) and run now
//   otzaria-voice.exe --uninstall  stop and remove from login

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "json.h"
#include "pack.h"
#include "util.h"

namespace {

const char* kVersion = "1.0.0";
const wchar_t* kRunValue = L"OtzariaVoice";
constexpr auto kUnloadAfter = std::chrono::minutes(3);
constexpr auto kMaxSession = std::chrono::seconds(100);  // fetchStream caps a request at 120s

// ---------------------------------------------------------------- paths & log

std::wstring env(const wchar_t* name) {
  wchar_t buf[2048];
  DWORD n = GetEnvironmentVariableW(name, buf, 2048);
  return n && n < 2048 ? std::wstring(buf, n) : L"";
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring exePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  return std::wstring(buf, n);
}

std::wstring exeDir() {
  auto p = exePath();
  return p.substr(0, p.find_last_of(L'\\'));
}

std::mutex logMu;
void log(const std::string& line) {
  std::lock_guard<std::mutex> lock(logMu);
  std::wstring dir = env(L"LOCALAPPDATA") + L"\\OtzariaVoice";
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = dir + L"\\service.log";
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > (1u << 20))
    DeleteFileW(path.c_str());
  FILE* f = _wfopen(path.c_str(), L"ab");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
          t.wSecond, line.c_str());
  fclose(f);
}

std::wstring packDir() { return pack::dir(); }
bool packReady() { return pack::ready(); }

std::wstring regString(HKEY key, const wchar_t* sub, const wchar_t* name) {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  if (RegGetValueW(key, sub, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  return buf;
}

// Otzaria's own installation already carries ONNX Runtime (for its semantic
// search), so the service does not ship a copy of its own.
std::wstring findOnnxRuntime() {
  std::vector<std::wstring> dirs;
  auto e = env(L"OTZARIA_ORT_PATH");
  if (!e.empty()) return e;
  dirs.push_back(exeDir());
  const wchar_t* uninstall = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    HKEY h;
    if (RegOpenKeyExW(root, uninstall, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
      DWORD len = 256;
      if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        break;
      std::wstring sub = std::wstring(uninstall) + L"\\" + name;
      std::wstring display = regString(root, sub.c_str(), L"DisplayName");
      if (display.find(L"אוצריא") == std::wstring::npos &&  // אוצריא
          _wcsnicmp(display.c_str(), L"otzaria", 7) != 0)
        continue;
      std::wstring loc = regString(root, sub.c_str(), L"InstallLocation");
      if (!loc.empty()) dirs.push_back(loc);
    }
    RegCloseKey(h);
  }
  dirs.push_back(env(L"LOCALAPPDATA") + L"\\Programs\\Otzaria");
  dirs.push_back(env(L"ProgramFiles") + L"\\Otzaria");
  for (auto d : dirs) {
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    auto p = d + L"\\onnxruntime\\onnxruntime.dll";
    if (exists(p)) return p;
  }
  return L"";
}

void trimMemory() {
  HeapCompact(GetProcessHeap(), 0);
  SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

// ---------------------------------------------------------------- engine

struct Engine {
  std::unique_ptr<voice::CtcModel> model;
  std::unique_ptr<voice::NgramLm> lm;
  std::unique_ptr<voice::PhraseSet> phrases;
  std::unique_ptr<voice::CtcDecoder> decoder;
};

std::mutex engineMu;
std::unique_ptr<Engine> engine;
std::chrono::steady_clock::time_point lastUse = std::chrono::steady_clock::now();

Engine& loadEngine() {
  std::lock_guard<std::mutex> lock(engineMu);
  lastUse = std::chrono::steady_clock::now();
  if (engine) return *engine;
  const auto ort = findOnnxRuntime();
  if (ort.empty()) throw std::runtime_error("ONNX Runtime not found (is Otzaria installed?)");
  const auto t0 = std::chrono::steady_clock::now();
  auto e = std::make_unique<Engine>();
  const int threads = std::max(2, std::min(6, int(std::thread::hardware_concurrency() / 2)));
  e->model = std::make_unique<voice::CtcModel>(ort, packDir() + L"\\ctc", threads);
  e->lm = std::make_unique<voice::NgramLm>(packDir() + L"\\lm\\library.otzlm");
  e->phrases = std::make_unique<voice::PhraseSet>(packDir() + L"\\lm\\phrases.txt", *e->lm);
  voice::LmWeights weights;
  weights.phrasePair = 2.0;
  weights.phraseTriple = 1.0;
  // Evaluation only: try other weights without rebuilding.
  if (auto v = env(L"OTZARIA_VOICE_PHRASE_PAIR"); !v.empty()) weights.phrasePair = _wtof(v.c_str());
  if (auto v = env(L"OTZARIA_VOICE_PHRASE_TRIPLE"); !v.empty()) weights.phraseTriple = _wtof(v.c_str());
  e->decoder = std::make_unique<voice::CtcDecoder>(e->model->vocab(), e->model->blank(), *e->lm,
                                                   e->phrases.get(), weights);
  engine = std::move(e);
  log("engine loaded in " +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count()) +
      " ms, ort " + util::narrow(ort) + ", phrases " + std::to_string(engine->phrases->size()));
  return *engine;
}

// ---------------------------------------------------------------- microphone

class Microphone {
 public:
  static constexpr int kBuffers = 64;          // 6.4s: room while the model loads
  static constexpr int kSamples = 1600;        // 100ms at 16kHz

  ~Microphone() { close(); }

  bool open(std::string& error) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 16000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = 32000;
    MMRESULT r = waveInOpen(&wave_, WAVE_MAPPER, &fmt, DWORD_PTR(event_), 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
      wave_ = nullptr;
      error = r == MMSYSERR_BADDEVICEID || r == MMSYSERR_NODRIVER ? "no microphone"
              : r == MMSYSERR_ALLOCATED                         ? "microphone in use"
                                                                : "cannot open microphone";
      return false;
    }
    data_.assign(size_t(kBuffers) * kSamples, 0);
    headers_.assign(kBuffers, WAVEHDR{});
    for (int i = 0; i < kBuffers; i++) {
      auto& h = headers_[i];
      h.lpData = reinterpret_cast<LPSTR>(&data_[size_t(i) * kSamples]);
      h.dwBufferLength = kSamples * 2;
      waveInPrepareHeader(wave_, &h, sizeof h);
      waveInAddBuffer(wave_, &h, sizeof h);
    }
    if (waveInStart(wave_) != MMSYSERR_NOERROR) {
      error = "cannot start microphone";
      close();
      return false;
    }
    return true;
  }

  void wait(DWORD ms) { WaitForSingleObject(event_, ms); }

  // Recorded buffers, in order; each is handed back to the device after [use].
  template <typename F>
  void drain(F use, bool requeue = true) {
    while (wave_) {
      auto& h = headers_[next_];
      if (!(h.dwFlags & WHDR_DONE)) return;
      const size_t n = h.dwBytesRecorded / 2;
      std::vector<float> pcm(n);
      const int16_t* s = &data_[size_t(next_) * kSamples];
      for (size_t i = 0; i < n; i++) pcm[i] = s[i] / 32768.0f;
      h.dwFlags &= ~WHDR_DONE;
      if (requeue) waveInAddBuffer(wave_, &h, sizeof h);
      next_ = (next_ + 1) % kBuffers;
      if (n) use(pcm);
      if (!requeue && h.dwBytesRecorded == 0) return;
    }
  }

  // Stops recording; buffers already filled stay readable through drain().
  void stop() {
    if (wave_) waveInReset(wave_);
  }

  void close() {
    if (wave_) {
      waveInReset(wave_);
      for (auto& h : headers_) waveInUnprepareHeader(wave_, &h, sizeof h);
      waveInClose(wave_);
      wave_ = nullptr;
    }
    if (event_) {
      CloseHandle(event_);
      event_ = nullptr;
    }
  }

 private:
  HWAVEIN wave_ = nullptr;
  HANDLE event_ = nullptr;
  std::vector<int16_t> data_;
  std::vector<WAVEHDR> headers_;
  int next_ = 0;
};

// ---------------------------------------------------------------- HTTP

struct Request {
  std::string method, path, body;
  std::map<std::string, std::string> headers;  // lower-case names
};

bool sendAll(SOCKET s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    int n = send(s, data.data() + sent, int(data.size() - sent), 0);
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

bool readRequest(SOCKET s, Request& req) {
  std::string buf;
  char tmp[4096];
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > 16384) return false;
    int n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, n);
  }
  auto lines = util::split(buf.substr(0, end), '\n');
  auto first = util::split(util::trim(lines[0].substr(0, lines[0].find('\r'))), ' ');
  if (first.size() < 2) return false;
  req.method = first[0];
  req.path = first[1];
  for (size_t i = 1; i < lines.size(); i++) {
    std::string l = lines[i];
    if (!l.empty() && l.back() == '\r') l.pop_back();
    auto c = l.find(':');
    if (c == std::string::npos) continue;
    std::string name = l.substr(0, c);
    for (auto& ch : name) ch = char(tolower(static_cast<unsigned char>(ch)));
    req.headers[name] = util::trim(l.substr(c + 1));
  }
  size_t length = 0;
  if (req.headers.count("content-length")) length = std::stoul(req.headers["content-length"]);
  if (length > 65536) return false;
  req.body = buf.substr(end + 4);
  while (req.body.size() < length) {
    int n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    req.body.append(tmp, n);
  }
  req.body.resize(length);
  return true;
}

void respond(SOCKET s, int status, const std::string& body) {
  const char* reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 404 ? "Not Found"
                     : status == 503 ? "Service Unavailable" : "Error";
  sendAll(s, "HTTP/1.1 " + std::to_string(status) + " " + reason +
                 "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                 std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" +
                 body);
}

std::string errorBody(const std::string& code, const std::string& message) {
  return "{\"error\":" + json::quote(code) + ",\"message\":" + json::quote(message) + "}";
}

bool clientGone(SOCKET s) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{0, 0};
  if (select(0, &set, nullptr, nullptr, &tv) <= 0) return false;
  char c;
  return recv(s, &c, 1, MSG_PEEK) <= 0;
}

// ---------------------------------------------------------------- dictation

std::mutex sessionMu;              // held for the whole session
std::atomic<bool> active{false};
std::atomic<bool> stopRequested{false};
std::atomic<bool> cancelRequested{false};

void dictate(SOCKET s, const Request& req) {
  if (active) cancelRequested = true;  // a new dictation replaces a stale one
  std::lock_guard<std::mutex> session(sessionMu);
  active = true;
  stopRequested = false;
  cancelRequested = false;
  struct Done {
    ~Done() {
      active = false;
      std::lock_guard<std::mutex> lock(engineMu);
      lastUse = std::chrono::steady_clock::now();
    }
  } done;

  if (!packReady()) {
    respond(s, 503, errorBody("pack_missing", "Hebrew voice pack is not installed"));
    return;
  }
  std::set<std::string> hotwords;
  std::vector<std::string> context;
  if (!req.body.empty()) {
    try {
      auto b = json::parse(req.body);
      if (b["hotwords"].kind == json::Value::Array)
        for (auto& w : b["hotwords"].arr())
          if (w.kind == json::Value::String && hotwords.size() < 2000)
            hotwords.insert(voice::normalizeLmWord(w.str()));
      if (b["context"].kind == json::Value::Array)
        for (auto& w : b["context"].arr())
          if (w.kind == json::Value::String) context.push_back(voice::normalizeLmWord(w.str()));
      if (context.size() > 2) context.erase(context.begin(), context.end() - 2);
    } catch (...) {
      respond(s, 400, errorBody("bad_request", "body must be JSON"));
      return;
    }
  }
  // The microphone opens first, so nothing said while the model loads is lost.
  Microphone mic;
  std::string micError;
  if (!mic.open(micError)) {
    respond(s, 503, errorBody("microphone", micError));
    return;
  }
  if (!sendAll(s,
               "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson; charset=utf-8\r\n"
               "Transfer-Encoding: chunked\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"))
    return;
  bool alive = true;
  auto line = [&](const std::string& l) {
    if (!alive) return;
    const std::string data = l + "\n";
    char size[16];
    snprintf(size, sizeof size, "%zx\r\n", data.size());
    alive = sendAll(s, std::string(size) + data + "\r\n");
  };
  auto sendUpdates = [&](const std::vector<voice::LiveUpdate>& ups) {
    for (auto& u : ups)
      line("{\"type\":\"update\",\"segment\":" + std::to_string(u.segment) +
           ",\"text\":" + json::quote(u.text) + ",\"final\":" + (u.isFinal ? "true" : "false") + "}");
  };
  line("{\"type\":\"listening\"}");

  std::string reason = "stopped";
  try {
    Engine& e = loadEngine();
    voice::LiveTranscriber live(*e.model, *e.decoder, hotwords, context);
    line("{\"type\":\"ready\"}");
    const auto started = std::chrono::steady_clock::now();
    while (alive) {
      if (cancelRequested) {
        reason = "cancelled";
        break;
      }
      if (stopRequested) break;
      if (std::chrono::steady_clock::now() - started > kMaxSession) {
        reason = "time_limit";
        break;
      }
      if (clientGone(s)) {
        alive = false;
        break;
      }
      mic.wait(50);
      mic.drain([&](const std::vector<float>& pcm) { sendUpdates(live.add(pcm.data(), pcm.size())); });
    }
    if (alive && reason != "cancelled") {
      mic.stop();
      mic.drain([&](const std::vector<float>& pcm) { sendUpdates(live.add(pcm.data(), pcm.size())); },
                false);
      sendUpdates(live.finish());
    }
  } catch (const std::exception& ex) {
    log(std::string("dictation error: ") + ex.what());
    line("{\"type\":\"error\",\"message\":" + json::quote(ex.what()) + "}");
    reason = "error";
  }
  mic.close();
  line("{\"type\":\"end\",\"reason\":" + json::quote(reason) + "}");
  if (alive) sendAll(s, "0\r\n\r\n");
}

// ---------------------------------------------------------------- server

std::atomic<bool> quitting{false};
SOCKET listener = INVALID_SOCKET;

std::string packJob() {
  const auto p = pack::progress();
  return "{\"state\":" + json::quote(p.state) + ",\"received\":" + std::to_string(p.received) +
         ",\"total\":" + std::to_string(p.total) + ",\"error\":" + json::quote(p.error) + "}";
}

bool packBusy() {
  const auto st = pack::progress().state;
  return st == "downloading" || st == "verifying" || st == "extracting";
}

void handle(SOCKET s) {
  DWORD timeout = 10000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
  BOOL nodelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
  Request req;
  if (readRequest(s, req)) {
    // Web pages always send Origin and cannot add custom headers without a
    // preflight, which is never answered: only local programs get through.
    if (req.headers.count("origin") || !req.headers.count("x-otzaria-voice")) {
      respond(s, 403, errorBody("forbidden", "requests must come from an Otzaria plugin"));
    } else if (req.method == "GET" && req.path == "/status") {
      bool loaded;
      {
        std::lock_guard<std::mutex> lock(engineMu);
        loaded = engine != nullptr;
      }
      respond(s, 200,
              std::string("{\"service\":\"otzaria-voice\",\"version\":\"") + kVersion +
                  "\",\"pack\":" + (packReady() ? "true" : "false") +
                  ",\"packVersion\":" + std::to_string(pack::installedVersion()) +
                  ",\"packSize\":" + std::to_string(pack::kRelease.size) +
                  ",\"packJob\":" + packJob() +
                  ",\"runtime\":" + (findOnnxRuntime().empty() ? "false" : "true") +
                  ",\"loaded\":" + (loaded ? "true" : "false") +
                  ",\"busy\":" + (active ? "true" : "false") + "}");
    } else if (req.method == "POST" && req.path == "/dictate") {
      if (packBusy())
        respond(s, 503, errorBody("pack_installing", "the voice pack is being installed"));
      else
        dictate(s, req);
    } else if (req.method == "POST" && req.path == "/install-pack") {
      if (active) {
        respond(s, 409, errorBody("busy", "stop dictation first"));
      } else {
        // The new pack replaces files the loaded engine has open.
        pack::startInstall([] {
          std::lock_guard<std::mutex> lock(engineMu);
          engine.reset();
        });
        respond(s, 200, "{\"started\":true,\"packJob\":" + packJob() + "}");
      }
    } else if (req.method == "POST" && req.path == "/stop") {
      const bool was = active;
      stopRequested = true;
      respond(s, 200, std::string("{\"stopped\":") + (was ? "true" : "false") + "}");
    } else if (req.method == "POST" && req.path == "/cancel") {
      const bool was = active;
      cancelRequested = true;
      respond(s, 200, std::string("{\"cancelled\":") + (was ? "true" : "false") + "}");
    } else if (req.method == "POST" && req.path == "/quit") {
      respond(s, 200, "{\"quitting\":true}");
      quitting = true;
      cancelRequested = true;
      closesocket(listener);
    } else {
      respond(s, 404, errorBody("not_found", "unknown endpoint"));
    }
  }
  shutdown(s, SD_BOTH);
  closesocket(s);
}

void idleLoop() {
  bool first = true;
  while (!quitting) {
    Sleep(first ? 2000 : 15000);
    first = false;
    bool unloaded = false;
    {
      std::lock_guard<std::mutex> lock(engineMu);
      if (engine && !active && std::chrono::steady_clock::now() - lastUse > kUnloadAfter) {
        engine.reset();
        unloaded = true;
      }
    }
    if (unloaded) log("engine unloaded");
    if (!active) trimMemory();
  }
}

int port() {
  auto e = env(L"OTZARIA_VOICE_PORT");
  return e.empty() ? 47811 : _wtoi(e.c_str());
}

int serve() {
  // One instance per port (a test instance may run beside the installed one).
  const std::wstring mutexName = L"Local\\OtzariaVoiceService-" + std::to_wstring(port());
  HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
             sizeof exclusive);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(u_short(port()));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      listen(listener, 8) != 0) {
    log("cannot listen on port " + std::to_string(port()));
    return 1;
  }
  log(std::string("service ") + kVersion + " listening on 127.0.0.1:" + std::to_string(port()));
  std::thread(idleLoop).detach();
  trimMemory();
  while (!quitting) {
    SOCKET c = accept(listener, nullptr, nullptr);
    if (c == INVALID_SOCKET) {
      if (quitting) break;
      Sleep(100);
      continue;
    }
    std::thread(handle, c).detach();
  }
  log("service stopped");
  Sleep(300);  // let the last responses go out
  CloseHandle(single);
  return 0;
}

// ---------------------------------------------------------------- install

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

void postQuit() {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(u_short(port()));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
    sendAll(s, "POST /quit HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Otzaria-Voice: 1\r\nContent-Length: 0\r\n\r\n");
    char buf[256];
    recv(s, buf, sizeof buf, 0);
  }
  closesocket(s);
}

// Installed per user, without administrator rights: the program copies itself
// to %LOCALAPPDATA%\Programs\OtzariaVoice, starts at login (HKCU Run), and is
// listed in Windows' "Installed apps" with an uninstall entry.
const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OtzariaVoice";
const wchar_t* kTitle = L"\u05E9\u05D9\u05E8\u05D5\u05EA \u05D4\u05D3\u05D9\u05D1\u05D5\u05E8 \u05E9\u05DC \u05D0\u05D5\u05E6\u05E8\u05D9\u05D0";  // שירות הדיבור של אוצריא

std::wstring installDir() { return env(L"LOCALAPPDATA") + L"\\Programs\\OtzariaVoice"; }
std::wstring installedExe() { return installDir() + L"\\otzaria-voice.exe"; }

bool sameFile(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

void message(const std::wstring& text, UINT icon) {
  MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon | MB_RTLREADING | MB_RIGHT | MB_SETFOREGROUND);
}

void setString(HKEY key, const wchar_t* name, const std::wstring& value) {
  RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 DWORD((value.size() + 1) * sizeof(wchar_t)));
}

void setDword(HKEY key, const wchar_t* name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
}

bool launchDetached(const std::wstring& exe) {
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  std::wstring line = L"\"" + exe + L"\"";
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                      nullptr, &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

int install(bool quiet) {
  const std::wstring target = installedExe();
  // A running copy (an older version, say) holds the file: ask it to quit first.
  postQuit();
  CreateDirectoryW((env(L"LOCALAPPDATA") + L"\\Programs").c_str(), nullptr);
  CreateDirectoryW(installDir().c_str(), nullptr);
  if (!sameFile(exePath(), target)) {
    bool copied = false;
    for (int i = 0; i < 20 && !copied; i++) {
      copied = CopyFileW(exePath().c_str(), target.c_str(), FALSE) != 0;
      if (!copied) Sleep(250);
    }
    if (!copied) {
      if (!quiet) message(L"\u05D4\u05D4\u05EA\u05E7\u05E0\u05D4 \u05E0\u05DB\u05E9\u05DC\u05D4: \u05DC\u05D0 \u05E0\u05D9\u05EA\u05DF \u05DC\u05D4\u05E2\u05EA\u05D9\u05E7 \u05D0\u05EA \u05D4\u05E7\u05D5\u05D1\u05E5.", MB_ICONERROR);  // ההתקנה נכשלה: לא ניתן להעתיק את הקובץ.
      log("install: copy failed");
      return 1;
    }
  }
  const std::wstring quoted = L"\"" + target + L"\"";
  HKEY h;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h,
                      nullptr) == ERROR_SUCCESS) {
    setString(h, kRunValue, quoted);
    RegCloseKey(h);
  }
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h,
                      nullptr) == ERROR_SUCCESS) {
    setString(h, L"DisplayName", kTitle);
    setString(h, L"DisplayVersion", util::widen(kVersion));
    setString(h, L"Publisher", L"Otzaria");
    setString(h, L"DisplayIcon", target);
    setString(h, L"InstallLocation", installDir());
    setString(h, L"UninstallString", quoted + L" --uninstall");
    setString(h, L"QuietUninstallString", quoted + L" --uninstall --quiet");
    setDword(h, L"NoModify", 1);
    setDword(h, L"NoRepair", 1);
    setDword(h, L"EstimatedSize", 1024);  // KB, without the downloaded voice pack
    RegCloseKey(h);
  }
  launchDetached(target);
  log(std::string("installed ") + kVersion + " for login start");
  if (!quiet)
    message(L"\u05E9\u05D9\u05E8\u05D5\u05EA \u05D4\u05D3\u05D9\u05D1\u05D5\u05E8 \u05D4\u05D5\u05EA\u05E7\u05DF \u05D5\u05E4\u05D5\u05E2\u05DC.\n"
            L"\u05DE\u05E2\u05DB\u05E9\u05D9\u05D5 \u05D4\u05D5\u05D0 \u05D9\u05E2\u05DC\u05D4 \u05DE\u05E2\u05E6\u05DE\u05D5 \u05D1\u05DB\u05DC \u05D4\u05E4\u05E2\u05DC\u05D4 \u05E9\u05DC \u05D4\u05DE\u05D7\u05E9\u05D1, \u05D5\u05D4\u05EA\u05D5\u05E1\u05E4\u05D9\u05DD \u05E9\u05DC \u05D0\u05D5\u05E6\u05E8\u05D9\u05D0 \u05D9\u05D5\u05DB\u05DC\u05D5 \u05DC\u05D4\u05E9\u05EA\u05DE\u05E9 \u05D1\u05D5.\n"
            L"\u05DC\u05D4\u05E1\u05E8\u05D4: \u05D4\u05D2\u05D3\u05E8\u05D5\u05EA \u05D5\u05D9\u05E0\u05D3\u05D5\u05E1 \u2190 \u05D0\u05E4\u05DC\u05D9\u05E7\u05E6\u05D9\u05D5\u05EA \u05DE\u05D5\u05EA\u05E7\u05E0\u05D5\u05EA.",
            MB_ICONINFORMATION);
  // שירות הדיבור הותקן ופועל.
  // מעכשיו הוא יעלה מעצמו בכל הפעלה של המחשב, והתוספים של אוצריא יוכלו להשתמש בו.
  // להסרה: הגדרות וינדוס ← אפליקציות מותקנות.
  return 0;
}

int uninstall(bool quiet) {
  HKEY h;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
    RegDeleteValueW(h, kRunValue);
    RegCloseKey(h);
  }
  RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
  postQuit();
  log("uninstalled");
  // A running program cannot delete its own file: a hidden shell does it a
  // moment after this process exits.
  const std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + installedExe() +
                           L"\" & rmdir \"" + installDir() + L"\"";
  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring line = cmd;
  if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (!quiet)
    message(L"\u05E9\u05D9\u05E8\u05D5\u05EA \u05D4\u05D3\u05D9\u05D1\u05D5\u05E8 \u05D4\u05D5\u05E1\u05E8.", MB_ICONINFORMATION);  // שירות הדיבור הוסר.
  return 0;
}

// ---------------------------------------------------------------- evaluation

// Reads 16kHz mono PCM16 WAV.
std::vector<float> readWav(const std::wstring& path) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) throw std::runtime_error("missing " + util::narrow(path));
  std::vector<char> b;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  size_t o = 12;
  while (o + 8 <= b.size()) {
    uint32_t size;
    memcpy(&size, &b[o + 4], 4);
    if (memcmp(&b[o], "data", 4) == 0) {
      std::vector<float> pcm(std::min<size_t>(size, b.size() - o - 8) / 2);
      for (size_t i = 0; i < pcm.size(); i++) {
        int16_t v;
        memcpy(&v, &b[o + 8 + i * 2], 2);
        pcm[i] = v / 32768.0f;
      }
      return pcm;
    }
    o += 8 + size + (size & 1);
  }
  throw std::runtime_error("no data chunk");
}

// --eval <manifest.jsonl> <out.jsonl>: offline (beam 16) and live-simulated
// transcripts for each recording, for tool/voice_eval/metrics.py.
int evaluate(const std::wstring& manifest, const std::wstring& outPath) {
  Engine& e = loadEngine();
  const std::wstring base = manifest.substr(0, manifest.find_last_of(L"\\/"));
  FILE* in = _wfopen(manifest.c_str(), L"rb");
  FILE* out = _wfopen(outPath.c_str(), L"wb");
  if (!in || !out) return 2;
  char line[8192];
  while (fgets(line, sizeof line, in)) {
    if (!util::trim(line).size() || line[0] != '{') continue;
    auto row = json::parse(util::trim(std::string(line).substr(0, std::string(line).find_last_not_of("\r\n") + 1)));
    auto pcm = readWav(base + L"\\" + util::widen(row["wav"].str()));
    auto t0 = std::chrono::steady_clock::now();
    auto r = e.model->run(pcm.data(), pcm.size());
    std::string offline = r.frames ? e.decoder->display(e.decoder->decode(r, 16, {}, {})) : "";
    auto t1 = std::chrono::steady_clock::now();
    voice::LiveTranscriber live(*e.model, *e.decoder, {}, {});
    std::map<int, std::string> segs;
    // Half a second of silence after the end, as when the user stops talking.
    pcm.insert(pcm.end(), 8000, 0.0f);
    for (size_t o = 0; o < pcm.size(); o += 1600)
      for (auto& u : live.add(&pcm[o], std::min<size_t>(1600, pcm.size() - o))) segs[u.segment] = u.text;
    for (auto& u : live.finish()) segs[u.segment] = u.text;
    std::string streamed;
    for (auto& [k, t] : segs)
      if (!t.empty()) streamed += (streamed.empty() ? "" : " ") + t;
    std::string tags = "[";
    for (auto& t : row["tags"].arr()) tags += (tags.size() > 1 ? "," : "") + json::quote(t.str());
    tags += "]";
    fprintf(out, "{\"id\":%s,\"ref\":%s,\"hyp\":%s,\"hyp_live\":%s,\"ms\":%lld,\"tags\":%s,\"split\":%s}\n",
            json::quote(row["id"].str()).c_str(), json::quote(row["ref"].str()).c_str(),
            json::quote(offline).c_str(), json::quote(streamed).c_str(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            tags.c_str(), json::quote(row["split"].str()).c_str());
    fflush(out);
  }
  fclose(in);
  fclose(out);
  return 0;
}

}  // namespace

void serviceLog(const std::string& line) { log(line); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::wstring arg = argc > 1 ? argv[1] : L"";
  if (arg == L"--eval" && argc > 3) {
    std::wstring m = argv[2], o = argv[3];
    LocalFree(argv);
    try {
      return evaluate(m, o);
    } catch (const std::exception& ex) {
      log(std::string("eval: ") + ex.what());
      return 3;
    }
  }
  const bool quiet = argc > 2 && std::wstring(argv[2]) == L"--quiet";
  LocalFree(argv);
  if (arg == L"--install") return install(quiet);
  if (arg == L"--uninstall") return uninstall(quiet);
  if (arg == L"--serve") return serve();
  // Double-clicked from a download: install. The installed copy, started at
  // login, serves.
  if (!sameFile(exePath(), installedExe())) return install(false);
  return serve();
}
