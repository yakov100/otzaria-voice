// Otzaria voice service: local Hebrew speech recognition for Otzaria plugins.
//
// Listens on http://127.0.0.1:47811 only. A plugin starts dictation with
// POST /dictate (network.fetchStream) and receives newline-delimited JSON while
// the user speaks; POST /stop ends it after the last sentence is recognized.
// Idle, the service holds no model and trims its memory; the model is loaded on
// the first dictation and released after a few idle minutes.
#define _CRT_SECURE_NO_WARNINGS
#include "service.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

#include "json.h"
#include "pack.h"
#include "talmud_phrases.h"
#include "util.h"

namespace service {

const char* kVersion = "1.1.0";

namespace {

constexpr auto kUnloadAfter = std::chrono::minutes(3);
constexpr auto kMaxSession = std::chrono::seconds(100);  // fetchStream caps a request at 120s

#if defined(_WIN32)
const char* kPlatform = "windows";
#elif defined(__ANDROID__)
const char* kPlatform = "android";
#elif defined(__APPLE__)
const char* kPlatform = "macos";
#else
const char* kPlatform = "linux";
#endif

// ---------------------------------------------------------------- sockets

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void closeSocket(Socket s) { ::close(s); }
#endif

bool sendAll(Socket s, const std::string& data) {
  size_t sent = 0;
#if defined(MSG_NOSIGNAL)
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  while (sent < data.size()) {
    const auto n = send(s, data.data() + sent, int(data.size() - sent), flags);
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

void setReceiveTimeout(Socket s, int ms) {
#ifdef _WIN32
  DWORD timeout = DWORD(ms);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
#else
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

void setNoDelay(Socket s) {
  int on = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef SO_NOSIGPIPE
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

bool clientGone(Socket s) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv{0, 0};
  if (select(int(s) + 1, &set, nullptr, nullptr, &tv) <= 0) return false;
  char c;
  return recv(s, &c, 1, MSG_PEEK) <= 0;
}

void netStartup() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#else
  signal(SIGPIPE, SIG_IGN);
#endif
}

sockaddr_in loopback(int port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return addr;
}

// ---------------------------------------------------------------- HTTP

struct Request {
  std::string method, path, body;
  std::map<std::string, std::string> headers;  // lower-case names
};

bool readRequest(Socket s, Request& req) {
  std::string buf;
  char tmp[4096];
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > 16384) return false;
    const auto n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, size_t(n));
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
  if (req.headers.count("content-length")) {
    try {
      length = std::stoul(req.headers["content-length"]);
    } catch (...) {
      return false;
    }
  }
  if (length > 65536) return false;
  req.body = buf.substr(end + 4);
  while (req.body.size() < length) {
    const auto n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    req.body.append(tmp, size_t(n));
  }
  req.body.resize(length);
  return true;
}

void respond(Socket s, int status, const std::string& body) {
  const char* reason = status == 200   ? "OK"
                       : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found"
                       : status == 503 ? "Service Unavailable"
                                       : "Error";
  sendAll(s, "HTTP/1.1 " + std::to_string(status) + " " + reason +
                 "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                 std::to_string(body.size()) +
                 "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
}

std::string errorBody(const std::string& code, const std::string& message) {
  return "{\"error\":" + json::quote(code) + ",\"message\":" + json::quote(message) + "}";
}

// ---------------------------------------------------------------- engine

struct Engine {
  std::unique_ptr<voice::CtcModel> model;
  std::unique_ptr<voice::NgramLm> lm;
  std::unique_ptr<voice::PhraseSet> phrases;
  std::unique_ptr<voice::CtcDecoder> decoder;
  std::unique_ptr<voice::PhraseRescorer> rescorer;  // null = second pass off
};

std::mutex engineMu;
std::unique_ptr<Engine> engine;
std::chrono::steady_clock::time_point lastUse = std::chrono::steady_clock::now();

Engine& loadEngine() {
  std::lock_guard<std::mutex> lock(engineMu);
  lastUse = std::chrono::steady_clock::now();
  if (engine) return *engine;
  const auto ort = platform::findOnnxRuntime();
  if (ort.empty()) throw std::runtime_error("ONNX Runtime not found (is Otzaria installed?)");
  const auto t0 = std::chrono::steady_clock::now();
  auto e = std::make_unique<Engine>();
  const int threads = std::max(2, std::min(6, int(std::thread::hardware_concurrency() / 2)));
  const auto packDir = pack::dir();
  e->model = std::make_unique<voice::CtcModel>(ort, packDir / "ctc", threads);
  e->lm = std::make_unique<voice::NgramLm>(packDir / "lm" / "library.otzlm");
  e->phrases = std::make_unique<voice::PhraseSet>(packDir / "lm" / "phrases.txt", *e->lm);
  voice::LmWeights weights;
  weights.phrasePair = 2.0;
  weights.phraseTriple = 1.0;
  // Evaluation only: try other weights without rebuilding.
  if (auto v = platform::env("OTZARIA_VOICE_PHRASE_PAIR"); !v.empty()) weights.phrasePair = atof(v.c_str());
  if (auto v = platform::env("OTZARIA_VOICE_PHRASE_TRIPLE"); !v.empty()) weights.phraseTriple = atof(v.c_str());
  e->decoder = std::make_unique<voice::CtcDecoder>(e->model->vocab(), e->model->blank(), *e->lm,
                                                   e->phrases.get(), weights);
  // Evaluation only: OTZARIA_VOICE_NO_RESCORE=1 turns the second pass off.
  if (platform::env("OTZARIA_VOICE_NO_RESCORE").empty())
    e->rescorer = std::make_unique<voice::PhraseRescorer>(
        e->model->vocab(), e->model->blank(), *e->lm, voice::talmudPhrases(),
        voice::PhraseRescorer::loadSounds(packDir / "phrases" / "talmud_sounds.tsv"));
  engine = std::move(e);
  platform::log("engine loaded in " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count()) +
                " ms, ort " + platform::toUtf8(ort) + ", phrases " +
                std::to_string(engine->phrases->size()) + ", sounded phrases " +
                std::to_string(engine->rescorer ? engine->rescorer->sounded() : 0));
  return *engine;
}

// ---------------------------------------------------------------- dictation

void dictate(Socket s, const Request& req) {
  if (active) cancelRequested = true;  // a new dictation replaces a stale one
  std::lock_guard<std::mutex> session(sessionMu);
  active = true;
  stopRequested = false;
  cancelRequested = false;
  struct Done {
    ~Done() {
      active = false;
      touchEngine();
    }
  } done;

  if (!pack::ready()) {
    respond(s, 503, errorBody("pack_missing", "Hebrew voice pack is not installed"));
    return;
  }
  if (!microphoneAllowed) {
    respond(s, 503, errorBody("microphone_background",
                              "open the Otzaria voice app once so it may use the microphone"));
    return;
  }
  std::set<std::string> hotwords;
  std::vector<std::string> context;
  AutoEnd autoEnd;
  if (!req.body.empty()) {
    try {
      auto b = json::parse(req.body);
      auto millis = [&](const char* key) {
        if (!b.has(key) || b[key].kind != json::Value::Number) return std::chrono::milliseconds(0);
        return std::chrono::milliseconds(long(std::clamp(b[key].num(), 0.0, 60000.0)));
      };
      autoEnd.afterSilence = millis("endAfterSilenceMs");
      autoEnd.noSpeech = millis("noSpeechMs");
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
           ",\"text\":" + json::quote(u.text) + ",\"final\":" + (u.isFinal ? "true" : "false") +
           "}");
  };
  line("{\"type\":\"listening\"}");

  std::string reason;
  try {
    reason = recognizeLive(
        mic, hotwords, context, sendUpdates, [&] { return alive && !clientGone(s); },
        [&] { line("{\"type\":\"ready\"}"); }, kMaxSession, autoEnd);
    if (reason == "gone") alive = false;
  } catch (const std::exception& ex) {
    platform::log(std::string("dictation error: ") + ex.what());
    line("{\"type\":\"error\",\"message\":" + json::quote(ex.what()) + "}");
    reason = "error";
  }
  mic.close();
  line("{\"type\":\"end\",\"reason\":" + json::quote(reason) + "}");
  if (alive) sendAll(s, "0\r\n\r\n");
}

// ---------------------------------------------------------------- server

std::atomic<bool> quitting{false};
Socket listener = kNoSocket;

std::string packJob() {
  const auto p = pack::progress();
  return "{\"state\":" + json::quote(p.state) + ",\"received\":" + std::to_string(p.received) +
         ",\"total\":" + std::to_string(p.total) + ",\"error\":" + json::quote(p.error) + "}";
}

bool packBusy() {
  const auto st = pack::progress().state;
  return st == "downloading" || st == "verifying" || st == "extracting";
}

bool runtimeFound() {
#ifdef _WIN32
  return !platform::findOnnxRuntime().empty();
#else
  // Looking it up may load the library to test it: once is enough.
  static std::atomic<int> cached{-1};
  if (cached < 0) cached = platform::findOnnxRuntime().empty() ? 0 : 1;
  return cached == 1;
#endif
}

void handle(Socket s) {
  setReceiveTimeout(s, 10000);
  setNoDelay(s);
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
                  "\",\"platform\":\"" + kPlatform + "\",\"pack\":" +
                  (pack::ready() ? "true" : "false") +
                  ",\"packVersion\":" + std::to_string(pack::installedVersion()) +
                  ",\"packSize\":" + std::to_string(pack::kRelease.size) +
                  ",\"packJob\":" + packJob() + ",\"hotkey\":" + json::quote(typingHotkey()) +
                  ",\"runtime\":" + (runtimeFound() ? "true" : "false") +
                  ",\"microphone\":" + (microphoneAllowed ? "true" : "false") +
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
      requestQuit();
    } else {
      respond(s, 404, errorBody("not_found", "unknown endpoint"));
    }
  }
#ifdef _WIN32
  shutdown(s, SD_BOTH);
#else
  shutdown(s, SHUT_RDWR);
#endif
  closeSocket(s);
}

void idleLoop() {
  bool first = true;
  while (!quitting) {
    for (int i = 0; i < (first ? 20 : 150) && !quitting; i++) platform::sleepMs(100);
    first = false;
    bool unloaded = false;
    {
      std::lock_guard<std::mutex> lock(engineMu);
      if (engine && !active && std::chrono::steady_clock::now() - lastUse > kUnloadAfter) {
        engine.reset();
        unloaded = true;
      }
    }
    if (unloaded) platform::log("engine unloaded");
    if (!active) platform::trimMemory();
  }
}

}  // namespace

std::mutex sessionMu;
std::atomic<bool> active{false};
std::atomic<bool> stopRequested{false};
std::atomic<bool> cancelRequested{false};
std::atomic<bool> microphoneAllowed{true};

void touchEngine() {
  std::lock_guard<std::mutex> lock(engineMu);
  lastUse = std::chrono::steady_clock::now();
}

std::string recognizeLive(Microphone& mic, const std::set<std::string>& hotwords,
                          const std::vector<std::string>& context,
                          const std::function<void(const std::vector<voice::LiveUpdate>&)>& sink,
                          const std::function<bool()>& alive, const std::function<void()>& onReady,
                          std::chrono::seconds maxDuration, AutoEnd autoEnd) {
  Engine& e = loadEngine();
  voice::LiveTranscriber live(*e.model, *e.decoder, hotwords, context, e.rescorer.get());
  onReady();
  const auto started = std::chrono::steady_clock::now();
  auto lastUpdate = started;
  bool heard = false, sentenceDone = false;
  auto take = [&](const std::vector<voice::LiveUpdate>& ups) {
    if (!ups.empty()) {
      lastUpdate = std::chrono::steady_clock::now();
      heard = true;
      sentenceDone = ups.back().isFinal;
    }
    sink(ups);
  };
  std::string reason = "stopped";
  while (true) {
    if (cancelRequested) return "cancelled";
    if (stopRequested) break;
    const auto now = std::chrono::steady_clock::now();
    if (now - started > maxDuration) {
      reason = "time_limit";
      break;
    }
    if (autoEnd.afterSilence.count() && sentenceDone && now - lastUpdate > autoEnd.afterSilence) {
      reason = "silence";
      break;
    }
    if (autoEnd.noSpeech.count() && !heard && now - started > autoEnd.noSpeech) {
      reason = "no_speech";
      break;
    }
    if (!alive()) return "gone";
    mic.wait(50);
    mic.drain([&](const std::vector<float>& pcm) { take(live.add(pcm.data(), pcm.size())); });
  }
  mic.stop();
  mic.drain([&](const std::vector<float>& pcm) { sink(live.add(pcm.data(), pcm.size())); }, false);
  sink(live.finish());
  return reason;
}

int port() {
  auto e = platform::env("OTZARIA_VOICE_PORT");
  return e.empty() ? 47811 : atoi(e.c_str());
}

void requestQuit() {
  quitting = true;
  cancelRequested = true;
#ifdef _WIN32
  closesocket(listener);  // wakes accept()
#endif
}

int serve() {
#ifdef _WIN32
  // One instance per port (a test instance may run beside the installed one).
  const std::wstring mutexName = L"Local\\OtzariaVoiceService-" + std::to_wstring(port());
  HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
#endif
  netStartup();
  quitting = false;
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
             sizeof exclusive);
#else
  // Lets a restarted service bind while old connections linger; POSIX still
  // refuses a second listener on the same port, which keeps one instance.
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
  sockaddr_in addr = loopback(port());
  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      listen(listener, 8) != 0) {
    platform::log("cannot listen on port " + std::to_string(port()));
    closeSocket(listener);
    return 1;
  }
  platform::log(std::string("service ") + kVersion + " (" + kPlatform +
                ") listening on 127.0.0.1:" + std::to_string(port()));
  std::thread idle(idleLoop);
  startTyping();
  platform::trimMemory();
  while (!quitting) {
#ifndef _WIN32
    // accept() is not woken by closing the socket everywhere: poll instead.
    pollfd p{listener, POLLIN, 0};
    if (poll(&p, 1, 500) <= 0) continue;
#endif
    Socket c = accept(listener, nullptr, nullptr);
    if (c == kNoSocket) {
      if (quitting) break;
      platform::sleepMs(100);
      continue;
    }
    std::thread(handle, c).detach();
  }
#ifndef _WIN32
  closeSocket(listener);
#endif
  listener = kNoSocket;
  idle.join();
  platform::log("service stopped");
  platform::sleepMs(300);  // let the last responses go out
#ifdef _WIN32
  CloseHandle(single);
#endif
  return 0;
}

void postQuit() {
  netStartup();
  Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr = loopback(port());
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
    sendAll(s,
            "POST /quit HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Otzaria-Voice: 1\r\nContent-Length: "
            "0\r\n\r\n");
    char buf[256];
    recv(s, buf, sizeof buf, 0);
  }
  closeSocket(s);
}

// --eval <manifest.jsonl> <out.jsonl>: offline (beam 16) and live-simulated
// transcripts for each recording, for tool/voice_eval/metrics.py.
int evaluate(const platform::Path& manifest, const platform::Path& outPath) {
  Engine& e = loadEngine();
  const platform::Path base = manifest.parent_path();
  std::ifstream in(manifest, std::ios::binary);
#ifdef _WIN32
  FILE* out = _wfopen(outPath.c_str(), L"wb");
#else
  FILE* out = fopen(outPath.c_str(), "wb");
#endif
  if (!in || !out) return 2;
  std::string raw;
  while (std::getline(in, raw)) {
    const std::string line = raw.substr(0, raw.find_last_not_of("\r\n") + 1);
    if (!util::trim(line).size() || line[0] != '{') continue;
    auto row = json::parse(util::trim(line));
    auto pcm = readWav(base / platform::fromUtf8(row["wav"].str()));
    auto t0 = std::chrono::steady_clock::now();
    auto r = e.model->run(pcm.data(), pcm.size());
    std::string offline;
    if (r.frames) {
      std::vector<std::string> words;
      for (auto& w : util::split(e.decoder->decode(r, 16, {}, {}), ' '))
        if (!w.empty()) words.push_back(std::move(w));
      if (e.rescorer) words = e.rescorer->rescore(r, std::move(words));
      std::string text;
      for (const auto& w : words) text += (text.empty() ? "" : " ") + w;
      offline = e.decoder->display(text);
    }
    auto t1 = std::chrono::steady_clock::now();
    voice::LiveTranscriber live(*e.model, *e.decoder, {}, {}, e.rescorer.get());
    std::map<int, std::string> segs;
    // Half a second of silence after the end, as when the user stops talking.
    pcm.insert(pcm.end(), 8000, 0.0f);
    for (size_t o = 0; o < pcm.size(); o += 1600)
      for (auto& u : live.add(&pcm[o], std::min<size_t>(1600, pcm.size() - o)))
        segs[u.segment] = u.text;
    for (auto& u : live.finish()) segs[u.segment] = u.text;
    std::string streamed;
    for (auto& [k, t] : segs)
      if (!t.empty()) streamed += (streamed.empty() ? "" : " ") + t;
    std::string tags = "[";
    for (auto& t : row["tags"].arr()) tags += (tags.size() > 1 ? "," : "") + json::quote(t.str());
    tags += "]";
    fprintf(out,
            "{\"id\":%s,\"ref\":%s,\"hyp\":%s,\"hyp_live\":%s,\"ms\":%lld,\"tags\":%s,\"split\":%s}\n",
            json::quote(row["id"].str()).c_str(), json::quote(row["ref"].str()).c_str(),
            json::quote(offline).c_str(), json::quote(streamed).c_str(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            tags.c_str(), json::quote(row["split"].str()).c_str());
    fflush(out);
  }
  fclose(out);
  return 0;
}

#ifndef _WIN32
// Typing into other programs is Windows-only for now (see docs/platforms.md).
void startTyping() {}
const char* typingHotkey() { return ""; }
#endif

}  // namespace service
