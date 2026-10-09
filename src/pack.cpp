#define _CRT_SECURE_NO_WARNINGS
#include "pack.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "util.h"

void serviceLog(const std::string& line);  // main.cpp

namespace pack {

const Release kRelease = {
    3,
    L"https://github.com/Otzaria/otzaria-voice/releases/download/voice-he-v3/otzaria-voice-he-v3.zip",
    334167487,
    "bde4cfd19a2a581f7836267ddd7ece700bf93c51e122306d6a567958f4c7eb1e",
};

namespace fs = std::filesystem;

namespace {

const wchar_t* kRequired[] = {L"ctc\\model.int8.onnx", L"ctc\\vocab.json",
                              L"ctc\\preprocessor.json", L"lm\\library.otzlm", L"LICENSES.txt"};

std::wstring env(const wchar_t* name) {
  wchar_t buf[2048];
  DWORD n = GetEnvironmentVariableW(name, buf, 2048);
  return n && n < 2048 ? std::wstring(buf, n) : L"";
}

std::wstring root() {
  auto d = dir();
  return d.substr(0, d.find_last_of(L'\\'));
}

std::mutex mu;
Progress current;
std::atomic<bool> running{false};

void set(const char* state, uint64_t received, uint64_t total, const std::string& error = "") {
  std::lock_guard<std::mutex> lock(mu);
  current.state = state;
  current.received = received;
  current.total = total;
  current.error = error;
}

// ---------------------------------------------------------------- SHA-256

class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0)
      throw std::runtime_error("בדיקת התקינות אינה זמינה במחשב");
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void add(const void* p, size_t n) {
    BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(p)), ULONG(n), 0);
  }
  std::string hex() {
    unsigned char d[32];
    BCryptFinishHash(hash_, d, 32, 0);
    static const char* x = "0123456789abcdef";
    std::string s;
    for (unsigned char c : d) {
      s += x[c >> 4];
      s += x[c & 15];
    }
    return s;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// ---------------------------------------------------------------- download

struct Http {
  HINTERNET session = nullptr, connect = nullptr, request = nullptr;
  ~Http() {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
  }
};

std::string lastError(const char* what) { return std::string(what) + " (" + std::to_string(GetLastError()) + ")"; }

// Appends the rest of [url] to [part], resuming from its current size.
void download(const std::wstring& url, const std::wstring& part, uint64_t expected) {
  uint64_t have = 0;
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (GetFileAttributesExW(part.c_str(), GetFileExInfoStandard, &a))
    have = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  if (have > expected) {
    DeleteFileW(part.c_str());
    have = 0;
  }
  if (have == expected) return;

  URL_COMPONENTS u{sizeof u};
  wchar_t host[256], path[2048];
  u.lpszHostName = host;
  u.dwHostNameLength = 256;
  u.lpszUrlPath = path;
  u.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) throw std::runtime_error("כתובת ההורדה שגויה");

  Http h;
  h.session = WinHttpOpen(L"OtzariaVoice/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h.session)  // before Windows 8.1
    h.session = WinHttpOpen(L"OtzariaVoice/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h.session) throw std::runtime_error(lastError("אין חיבור לאינטרנט"));
  WinHttpSetTimeouts(h.session, 15000, 15000, 30000, 60000);
  h.connect = WinHttpConnect(h.session, host, u.nPort, 0);
  if (!h.connect) throw std::runtime_error(lastError("אין חיבור לשרת ההורדה"));
  h.request = WinHttpOpenRequest(h.connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!h.request) throw std::runtime_error(lastError("אין חיבור לשרת ההורדה"));
  std::wstring range;
  if (have) range = L"Range: bytes=" + std::to_wstring(have) + L"-";
  if (!WinHttpSendRequest(h.request, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
                          range.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(h.request, nullptr))
    throw std::runtime_error(lastError("ההורדה נכשלה"));
  DWORD status = 0, size = sizeof status;
  WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status == 200 && have) {  // the server ignored the range: start over
    have = 0;
  } else if (status != 200 && status != 206) {
    throw std::runtime_error("שרת ההורדה השיב בשגיאה " + std::to_string(status));
  }
  FILE* f = _wfopen(part.c_str(), have ? L"ab" : L"wb");
  if (!f) throw std::runtime_error("לא ניתן לכתוב לדיסק");
  std::vector<char> buf(1 << 16);
  uint64_t got = have;
  set("downloading", got, expected);
  while (true) {
    DWORD n = 0;
    if (!WinHttpReadData(h.request, buf.data(), DWORD(buf.size()), &n)) {
      fclose(f);
      throw std::runtime_error(lastError("החיבור לאינטרנט נקטע; לחצו שוב והורדה תמשיך מאותה נקודה"));
    }
    if (n == 0) break;
    if (fwrite(buf.data(), 1, n, f) != n) {
      fclose(f);
      throw std::runtime_error("הדיסק מלא");
    }
    got += n;
    set("downloading", got, expected);
  }
  fclose(f);
  if (got != expected) throw std::runtime_error("ההורדה לא הושלמה; לחצו שוב והיא תמשיך מאותה נקודה");
}

std::string hashFile(const std::wstring& file, uint64_t total) {
  Sha256 sha;
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) throw std::runtime_error("לא ניתן לקרוא את הקובץ שהורד");
  std::vector<char> buf(1 << 20);
  uint64_t done = 0;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
    sha.add(buf.data(), n);
    done += n;
    set("verifying", done, total);
  }
  fclose(f);
  return sha.hex();
}

// Windows 10 and later ship bsdtar, which reads zip archives.
void extract(const std::wstring& zip, const std::wstring& to) {
  const std::wstring tar = env(L"SystemRoot") + L"\\System32\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + to + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi))
    throw std::runtime_error(lastError("לא ניתן לפתוח את החבילה"));
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) throw std::runtime_error("פתיחת החבילה נכשלה (" + std::to_string(code) + ")");
}

void run(std::function<void()> release) {
  const Release& r = kRelease;
  auto override = env(L"OTZARIA_VOICE_PACK_URL");
  const std::wstring url = override.empty() ? r.url : override;
  const std::wstring base = root();
  const std::wstring work = base + L"\\download";
  const std::wstring part = work + L"\\voice-pack-v" + std::to_wstring(r.version) + L".zip.part";
  const std::wstring staging = base + L"\\pack.new";
  try {
    std::error_code ec;
    fs::create_directories(work, ec);
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(base.c_str(), &free, nullptr, nullptr) &&
        free.QuadPart < r.size * 5 / 2)
      throw std::runtime_error("אין מספיק מקום בדיסק (צריך כ-800 מגה-בייט פנויים)");
    serviceLog("pack: downloading " + util::narrow(url));
    download(url, part, r.size);
    if (hashFile(part, r.size) != r.sha256) {
      DeleteFileW(part.c_str());
      throw std::runtime_error("הקובץ שהורד פגום; לחצו שוב כדי להוריד מחדש");
    }
    set("extracting", r.size, r.size);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    extract(part, staging);
    for (auto f : kRequired)
      if (!fs::exists(staging + L"\\" + f)) throw std::runtime_error("בחבילה חסרים קבצים");
    if (FILE* v = _wfopen((staging + L"\\version.txt").c_str(), L"wb")) {
      fprintf(v, "%d", r.version);
      fclose(v);
    }
    release();  // the old pack's files may be open
    const std::wstring target = dir(), old = base + L"\\pack.old";
    fs::remove_all(old, ec);
    if (fs::exists(target)) fs::rename(target, old);
    fs::rename(staging, target);
    fs::remove_all(old, ec);
    DeleteFileW(part.c_str());
    serviceLog("pack: installed version " + std::to_string(r.version));
    set("done", r.size, r.size);
  } catch (const std::exception& e) {
    serviceLog(std::string("pack: ") + e.what());
    std::error_code ec;
    fs::remove_all(staging, ec);
    Progress p = progress();
    set("error", p.received, p.total, e.what());
  }
  running = false;
}

}  // namespace

std::wstring dir() {
  auto e = env(L"OTZARIA_VOICE_PACK_DIR");
  return e.empty() ? env(L"APPDATA") + L"\\otzaria\\stt\\pack" : e;
}

bool ready() {
  const auto d = dir();
  for (auto f : kRequired)
    if (GetFileAttributesW((d + L"\\" + f).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  return true;
}

int installedVersion() {
  FILE* f = _wfopen((dir() + L"\\version.txt").c_str(), L"rb");
  if (!f) return 0;
  int v = 0;
  if (fscanf(f, "%d", &v) != 1) v = 0;
  fclose(f);
  return v;
}

Progress progress() {
  std::lock_guard<std::mutex> lock(mu);
  return current;
}

void startInstall(std::function<void()> release) {
  if (running.exchange(true)) return;
  set("downloading", 0, kRelease.size);
  std::thread(run, std::move(release)).detach();
}

}  // namespace pack
