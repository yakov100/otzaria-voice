// Windows: WinHTTP downloads, BCrypt verifies, and the bsdtar that ships with
// Windows 10 and later unpacks.
#define _CRT_SECURE_NO_WARNINGS
#include "pack.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "util.h"

namespace pack::sys {

namespace {

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

struct Http {
  HINTERNET session = nullptr, connect = nullptr, request = nullptr;
  ~Http() {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
  }
};

std::string lastError(const char* what) {
  return std::string(what) + " (" + std::to_string(GetLastError()) + ")";
}

}  // namespace

void download(const std::string& urlUtf8, const Path& part, uint64_t expected,
              const OnProgress& progress) {
  const std::wstring url = util::widen(urlUtf8);
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
  progress(got);
  while (true) {
    DWORD n = 0;
    if (!WinHttpReadData(h.request, buf.data(), DWORD(buf.size()), &n)) {
      fclose(f);
      throw std::runtime_error(
          lastError("החיבור לאינטרנט נקטע; לחצו שוב והורדה תמשיך מאותה נקודה"));
    }
    if (n == 0) break;
    if (fwrite(buf.data(), 1, n, f) != n) {
      fclose(f);
      throw std::runtime_error("הדיסק מלא");
    }
    got += n;
    progress(got);
  }
  fclose(f);
  if (got != expected)
    throw std::runtime_error("ההורדה לא הושלמה; לחצו שוב והיא תמשיך מאותה נקודה");
}

std::string hashFile(const Path& file, const OnProgress& progress) {
  Sha256 sha;
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) throw std::runtime_error("לא ניתן לקרוא את הקובץ שהורד");
  std::vector<char> buf(1 << 20);
  uint64_t done = 0;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
    sha.add(buf.data(), n);
    done += n;
    progress(done);
  }
  fclose(f);
  return sha.hex();
}

// Windows 10 and later ship bsdtar, which reads zip archives.
void extract(const Path& zip, const Path& to) {
  const std::wstring tar = util::widen(platform::env("SystemRoot")) + L"\\System32\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + to.wstring() + L"\"";
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

}  // namespace pack::sys
