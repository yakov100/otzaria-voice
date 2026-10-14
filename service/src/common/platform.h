// What differs between Windows, Linux, macOS and Android, behind one small
// interface: paths, environment, the log, loading ONNX Runtime, mapping a file
// into memory, and giving memory back to the system.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

using Path = std::filesystem::path;

// UTF-8 in and out on every platform.
std::string env(const char* name);
Path fromUtf8(const std::string& s);
std::string toUtf8(const Path& p);

Path exePath();
inline Path exeDir() { return exePath().parent_path(); }

// Otzaria's own data folder (the voice pack lives in <dataRoot>/stt/pack):
//   Windows  %APPDATA%\otzaria
//   macOS    ~/Library/Application Support/otzaria
//   Linux    ~/.local/share/otzaria
//   Android  the service app's private files folder
Path otzariaDataRoot();

// The service's own folder for its log.
Path stateDir();

// Android sets these before the service starts (JNI); unused elsewhere.
void setAndroidDirs(const Path& filesDir, const Path& nativeLibDir);

void log(const std::string& line);

// ONNX Runtime's shared library: OTZARIA_ORT_PATH, next to the program, or in
// Otzaria's own installation. Empty when none is found.
Path findOnnxRuntime();

class DynamicLibrary {
 public:
  explicit DynamicLibrary(const Path& path);  // throws std::runtime_error
  ~DynamicLibrary();
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  void* symbol(const char* name) const;

 private:
  void* handle_ = nullptr;
};

// A read-only file mapped into memory.
class MappedFile {
 public:
  explicit MappedFile(const Path& path);  // throws std::runtime_error
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  const uint8_t* data() const { return data_; }
  uint64_t size() const { return size_; }

 private:
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
#ifdef _WIN32
  void* file_ = nullptr;
  void* mapping_ = nullptr;
#endif
};

// Returns freed heap pages and the working set to the system.
void trimMemory();

void sleepMs(unsigned ms);

#ifndef _WIN32
// Runs a program (looked up in PATH) with [args] and no shell, and waits.
// Returns its exit code, or -1 when it could not start.
int run(const std::vector<std::string>& args);
#endif

}  // namespace platform
