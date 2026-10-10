#define _CRT_SECURE_NO_WARNINGS
#include "platform.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "util.h"
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
extern char** environ;
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace platform {

namespace fs = std::filesystem;

Path fromUtf8(const std::string& s) { return fs::u8path(s); }
std::string toUtf8(const Path& p) { return p.u8string(); }

#ifdef _WIN32

std::string env(const char* name) {
  wchar_t buf[2048];
  DWORD n = GetEnvironmentVariableW(util::widen(name).c_str(), buf, 2048);
  return n && n < 2048 ? util::narrow(std::wstring(buf, n)) : "";
}

Path exePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  return Path(std::wstring(buf, n));
}

Path otzariaDataRoot() { return fromUtf8(env("APPDATA")) / L"otzaria"; }
Path stateDir() { return fromUtf8(env("LOCALAPPDATA")) / L"OtzariaVoice"; }
void setAndroidDirs(const Path&, const Path&) {}

#else

std::string env(const char* name) {
  const char* v = getenv(name);
  return v ? v : "";
}

namespace {
Path androidFiles, androidLibs;
Path home() {
  auto h = env("HOME");
  return h.empty() ? Path("/tmp") : Path(h);
}
}  // namespace

void setAndroidDirs(const Path& filesDir, const Path& nativeLibDir) {
  androidFiles = filesDir;
  androidLibs = nativeLibDir;
}

Path exePath() {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0) return Path();
  std::error_code ec;
  auto p = fs::canonical(buf, ec);
  return ec ? Path(buf) : p;
#elif defined(__ANDROID__)
  return androidLibs / "libotzaria_voice.so";
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? Path() : p;
#endif
}

Path otzariaDataRoot() {
#if defined(__ANDROID__)
  return androidFiles;
#elif defined(__APPLE__)
  return home() / "Library" / "Application Support" / "otzaria";
#else
  return home() / ".local" / "share" / "otzaria";
#endif
}

Path stateDir() {
#if defined(__ANDROID__)
  return androidFiles;
#elif defined(__APPLE__)
  return home() / "Library" / "Logs" / "OtzariaVoice";
#else
  auto x = env("XDG_STATE_HOME");
  return (x.empty() ? home() / ".local" / "state" : Path(x)) / "otzaria-voice";
#endif
}

#endif

// ---------------------------------------------------------------- log

namespace {
std::mutex logMu;
}

void log(const std::string& line) {
  std::lock_guard<std::mutex> lock(logMu);
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_INFO, "OtzariaVoice", "%s", line.c_str());
#endif
  std::error_code ec;
  const Path dir = stateDir();
  fs::create_directories(dir, ec);
  const Path path = dir / "service.log";
  if (fs::exists(path, ec) && fs::file_size(path, ec) > (1u << 20)) fs::remove(path, ec);
#ifdef _WIN32
  FILE* f = _wfopen(path.c_str(), L"ab");
#else
  FILE* f = fopen(path.c_str(), "ab");
#endif
  if (!f) return;
  const std::time_t now = std::time(nullptr);
  std::tm t{};
#ifdef _WIN32
  localtime_s(&t, &now);
#else
  localtime_r(&now, &t);
#endif
  fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
          t.tm_hour, t.tm_min, t.tm_sec, line.c_str());
  fclose(f);
}

// ---------------------------------------------------------------- ONNX Runtime

#ifdef _WIN32

namespace {
std::wstring regString(HKEY key, const wchar_t* sub, const wchar_t* name) {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  if (RegGetValueW(key, sub, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  return buf;
}
}  // namespace

// Otzaria's own installation already carries ONNX Runtime (for its semantic
// search), so the service does not ship a copy of its own.
Path findOnnxRuntime() {
  auto e = env("OTZARIA_ORT_PATH");
  if (!e.empty()) return fromUtf8(e);
  std::vector<std::wstring> dirs;
  dirs.push_back(exeDir().wstring());
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
  dirs.push_back(fromUtf8(env("LOCALAPPDATA")).wstring() + L"\\Programs\\Otzaria");
  dirs.push_back(fromUtf8(env("ProgramFiles")).wstring() + L"\\Otzaria");
  for (auto d : dirs) {
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    auto p = d + L"\\onnxruntime\\onnxruntime.dll";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return Path(p);
  }
  return Path();
}

DynamicLibrary::DynamicLibrary(const Path& path) {
  handle_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!handle_) throw std::runtime_error("cannot load " + toUtf8(path));
}

DynamicLibrary::~DynamicLibrary() {
  if (handle_) FreeLibrary(static_cast<HMODULE>(handle_));
}

void* DynamicLibrary::symbol(const char* name) const {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
}

MappedFile::MappedFile(const Path& path) {
  file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    file_ = nullptr;
    throw std::runtime_error("missing file: " + toUtf8(path));
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(file_, &size);
  size_ = uint64_t(size.QuadPart);
  mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping_) throw std::runtime_error("cannot map " + toUtf8(path));
  data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
  if (!data_) throw std::runtime_error("cannot map " + toUtf8(path));
}

MappedFile::~MappedFile() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(mapping_);
  if (file_) CloseHandle(file_);
}

void trimMemory() {
  HeapCompact(GetProcessHeap(), 0);
  SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

#else  // POSIX

Path findOnnxRuntime() {
  auto e = env("OTZARIA_ORT_PATH");
  if (!e.empty()) return Path(e);
#if defined(__ANDROID__)
  // Packed in the APK by onnxruntime-android; the loader finds it by name.
  return Path("libonnxruntime.so");
#else
#if defined(__APPLE__)
  const char* name = "libonnxruntime.dylib";
#else
  const char* name = "libonnxruntime.so";
#endif
  const Path here = exeDir();
  std::vector<Path> candidates = {
      here / name,
#if !defined(__APPLE__)
      here / "libonnxruntime.so.1",  // packages may ship only the versioned name
#endif
      here / "lib" / name,
      here / "onnxruntime" / name,
      here.parent_path() / "lib" / "otzaria-voice" / name,
  };
#if defined(__APPLE__)
  // Otzaria.app embeds it in Contents/Frameworks.
  candidates.push_back(here.parent_path() / "Frameworks" / name);
  candidates.push_back(Path("/Applications/Otzaria Voice.app/Contents/Frameworks") / name);
  candidates.push_back(Path("/Applications/Otzaria.app/Contents/Frameworks") / name);
  candidates.push_back(Path("/Applications/otzaria.app/Contents/Frameworks") / name);
  candidates.push_back(home() / "Applications" / "Otzaria.app" / "Contents" / "Frameworks" / name);
#else
  // Otzaria's Linux bundle keeps it in <bundle>/onnxruntime/.
  candidates.push_back(Path("/usr/lib/otzaria-voice") / name);
  for (const char* d : {"/opt/otzaria", "/usr/lib/otzaria", "/usr/share/otzaria",
                        "/usr/local/lib/otzaria", "/app/otzaria"})
    candidates.push_back(Path(d) / "onnxruntime" / name);
#endif
  std::error_code ec;
  for (const auto& c : candidates)
    if (fs::exists(c, ec)) return c;
  // Finally whatever the system's loader finds (a distribution package).
  if (void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
    dlclose(h);
    return Path(name);
  }
  return Path();
#endif
}

DynamicLibrary::DynamicLibrary(const Path& path) {
  handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle_) {
    const char* why = dlerror();
    throw std::runtime_error("cannot load " + path.string() + (why ? std::string(": ") + why : ""));
  }
}

DynamicLibrary::~DynamicLibrary() {
  if (handle_) dlclose(handle_);
}

void* DynamicLibrary::symbol(const char* name) const { return dlsym(handle_, name); }

MappedFile::MappedFile(const Path& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("missing file: " + path.string());
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    throw std::runtime_error("cannot read " + path.string());
  }
  size_ = uint64_t(st.st_size);
  void* p = mmap(nullptr, size_t(size_), PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);  // the mapping keeps the file
  if (p == MAP_FAILED) throw std::runtime_error("cannot map " + path.string());
  data_ = static_cast<const uint8_t*>(p);
}

MappedFile::~MappedFile() {
  if (data_) munmap(const_cast<uint8_t*>(data_), size_t(size_));
}

void trimMemory() {
#if defined(__GLIBC__)
  malloc_trim(0);
#elif defined(__APPLE__)
  malloc_zone_pressure_relief(nullptr, 0);
#endif
}

int run(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace platform
