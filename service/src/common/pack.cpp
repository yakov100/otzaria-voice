// The voice pack's install flow, the same on every system: download (resumed),
// verify, unpack beside the old pack, then swap. The system-specific pieces are
// in pack_win.cpp, pack_posix.cpp and pack_android.cpp.
#include "pack.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pack {

const Release kRelease = {
    5,
    "https://github.com/yakov100/otzaria-voice/releases/download/voice-pack-v5/otzaria-voice-he-v5.zip",
    334199960,
    "be7a546f67fb8c90d5db9ac3fee940c43854b1e322809499bd3fee46c07c0ef7",
};

namespace fs = std::filesystem;

namespace {

Path required(int i) {
  switch (i) {
    case 0: return Path("ctc") / "model.int8.onnx";
    case 1: return Path("ctc") / "vocab.json";
    case 2: return Path("ctc") / "preprocessor.json";
    case 3: return Path("lm") / "library.otzlm";
    default: return Path("LICENSES.txt");
  }
}
constexpr int kRequiredCount = 5;

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

// A failed install, with the exit code --install-pack reports (installLocal).
struct InstallError : std::runtime_error {
  int code;
  InstallError(int c, const std::string& message) : std::runtime_error(message), code(c) {}
};

// Renames [from] to [to], retrying for a few seconds while a file in it is still
// being closed (a memory-mapped language model, a scanner).
bool renamePatiently(const Path& from, const Path& to) {
  std::error_code ec;
  for (int i = 0; i < 20; i++) {
    fs::rename(from, to, ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  platform::log("pack: cannot rename " + platform::toUtf8(from) + ": " + ec.message());
  return false;
}

// Unpacks the verified [zip] beside the target pack and swaps it in: the same
// for a download and for a zip an installer carries. [release] runs right before
// the swap (the old pack's files may be open). A swap that fails puts the old
// pack back, so a failure never leaves the user with no pack at all.
void unpackAndSwap(const Path& zip, const std::function<void()>& release) {
  const Release& r = kRelease;
  const Path target = installTarget();
  const Path base = target.parent_path();
  const Path staging = base / "pack.new";
  std::error_code ec;
  set("extracting", r.size, r.size);
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  sys::extract(zip, staging);
  for (int i = 0; i < kRequiredCount; i++)
    if (!fs::exists(staging / required(i))) throw InstallError(4, "בחבילה חסרים קבצים");
  {
    std::ofstream v(staging / "version.txt", std::ios::binary);
    v << r.version;
  }
  release();
  const Path old = base / "pack.old";
  fs::remove_all(old, ec);
  const bool hadOld = fs::exists(target, ec);
  if (hadOld && !renamePatiently(target, old))
    throw InstallError(6, "חבילת הדיבור הקודמת בשימוש; סגרו את אוצריא ונסו שוב");
  if (!renamePatiently(staging, target)) {
    if (hadOld) renamePatiently(old, target);
    throw InstallError(6, "לא ניתן להחליף את חבילת הדיבור; סגרו את אוצריא ונסו שוב");
  }
  fs::remove_all(old, ec);
}

void run(std::function<void()> release) {
  const Release& r = kRelease;
  auto override = platform::env("OTZARIA_VOICE_PACK_URL");
  const std::string url = override.empty() ? r.url : override;
  const Path target = installTarget();
  const Path base = target.parent_path();
  const Path work = base / "download";
  const Path part = work / ("voice-pack-v" + std::to_string(r.version) + ".zip.part");
  const Path staging = base / "pack.new";
  try {
    std::error_code ec;
    fs::create_directories(work, ec);
    const auto space = fs::space(base, ec);
    if (!ec && space.available < r.size * 5 / 2)
      throw std::runtime_error("אין מספיק מקום בדיסק (צריך כ-800 מגה-בייט פנויים)");
    platform::log("pack: downloading " + url);
    set("downloading", 0, r.size);
    sys::download(url, part, r.size, [&](uint64_t got) { set("downloading", got, r.size); });
    if (sys::hashFile(part, [&](uint64_t done) { set("verifying", done, r.size); }) != r.sha256) {
      fs::remove(part, ec);
      throw std::runtime_error("הקובץ שהורד פגום; לחצו שוב כדי להוריד מחדש");
    }
    unpackAndSwap(part, release);
    fs::remove(part, ec);
    platform::log("pack: installed version " + std::to_string(r.version));
    set("done", r.size, r.size);
  } catch (const std::exception& e) {
    platform::log(std::string("pack: ") + e.what());
    std::error_code ec;
    fs::remove_all(staging, ec);
    Progress p = progress();
    set("error", p.received, p.total, e.what());
  }
  running = false;
}

bool complete(const Path& d) {
  std::error_code ec;
  for (int i = 0; i < kRequiredCount; i++)
    if (!fs::exists(d / required(i), ec)) return false;
  return true;
}

// Where a pack may already be, in order of preference: the user's own copy,
// then copies installed for everyone (read-only).
std::vector<Path> candidates() {
  std::vector<Path> out;
  auto e = platform::env("OTZARIA_VOICE_PACK_DIR");
  if (!e.empty()) out.push_back(platform::fromUtf8(e));
  out.push_back(userDir());
#ifndef _WIN32
  // Shared with Otzaria's own data folder, as on Windows.
  out.push_back(platform::otzariaDataRoot() / "stt" / "pack");
#endif
#if defined(__APPLE__)
  out.push_back(platform::exeDir().parent_path() / "Resources" / "pack");
  out.push_back(Path("/Applications/Otzaria Voice.app/Contents/Resources/pack"));
#elif defined(__linux__) && !defined(__ANDROID__)
  out.push_back(Path("/usr/share/otzaria-voice/pack"));
#endif
  return out;
}

bool writable(const Path& d) {
  std::error_code ec;
  fs::create_directories(d.parent_path(), ec);
  const Path probe = d.parent_path() / ".otzaria-voice-write-test";
  {
    std::ofstream f(probe, std::ios::binary);
    if (!f) return false;
  }
  fs::remove(probe, ec);
  return true;
}

}  // namespace

Path userDir() {
#if defined(_WIN32) || defined(__ANDROID__)
  return platform::otzariaDataRoot() / "stt" / "pack";
#elif defined(__APPLE__)
  auto h = platform::env("HOME");
  return Path(h) / "Library" / "Application Support" / "OtzariaVoice" / "pack";
#else
  auto x = platform::env("XDG_DATA_HOME");
  return (x.empty() ? Path(platform::env("HOME")) / ".local" / "share" : Path(x)) / "otzaria-voice" /
         "pack";
#endif
}

Path dir() {
  for (const auto& d : candidates())
    if (complete(d)) return d;
  return installTarget();
}

Path installTarget() {
  auto e = platform::env("OTZARIA_VOICE_PACK_DIR");
  if (!e.empty() && writable(platform::fromUtf8(e))) return platform::fromUtf8(e);
  return userDir();
}

bool ready() { return complete(dir()); }

int installedVersion() {
  std::ifstream f(dir() / "version.txt", std::ios::binary);
  int v = 0;
  if (!(f >> v)) v = 0;
  return v;
}

bool outdated() { return ready() && installedVersion() < kRelease.version; }

Progress progress() {
  std::lock_guard<std::mutex> lock(mu);
  return current;
}

void startInstall(std::function<void()> release) {
  if (running.exchange(true)) return;
  set("downloading", 0, kRelease.size);
  std::thread(run, std::move(release)).detach();
}

uint64_t removeTree(const Path& dir) {
  std::error_code ec;
  uint64_t bytes = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) bytes += e.file_size(ec);
  fs::remove_all(dir, ec);
  return bytes;
}

uint64_t removeInstalled() {
  const Path base = userDir().parent_path();
  uint64_t bytes = removeTree(userDir());
  for (const char* d : {"download", "pack.new", "pack.old"}) bytes += removeTree(base / d);
  return bytes;
}

int installLocal(const Path& zip) {
  const Release& r = kRelease;
  std::error_code ec;
  if (!fs::is_regular_file(zip, ec)) {
    platform::log("install-pack: no such file: " + platform::toUtf8(zip));
    return 2;
  }
  const auto env = platform::env("OTZARIA_VOICE_PACK_DIR");
  if (!env.empty() && !writable(platform::fromUtf8(env))) {
    platform::log("install-pack: OTZARIA_VOICE_PACK_DIR is not writable: " + env);
    return 7;
  }
  if (ready()) {
    platform::log("install-pack: a complete pack (version " + std::to_string(installedVersion()) +
                  ") is already in " + platform::toUtf8(dir()) + "; left as is");
    return 0;
  }
  const Path target = installTarget();
  const Path base = target.parent_path();
  fs::create_directories(base, ec);
  const auto space = fs::space(base, ec);
  if (!ec && space.available < r.size * 3 / 2) {
    platform::log("install-pack: not enough disk space in " + platform::toUtf8(base));
    return 5;
  }
  if (fs::file_size(zip, ec) != r.size || ec || sys::hashFile(zip, [](uint64_t) {}) != r.sha256) {
    platform::log("install-pack: " + platform::toUtf8(zip) + " is not voice pack " +
                  std::to_string(r.version) + " (size or SHA-256)");
    return 3;
  }
  try {
    unpackAndSwap(zip, [] {});
  } catch (const InstallError& e) {
    platform::log(std::string("install-pack: ") + e.what());
    fs::remove_all(base / "pack.new", ec);
    return e.code;
  } catch (const std::exception& e) {
    platform::log(std::string("install-pack: ") + e.what());
    fs::remove_all(base / "pack.new", ec);
    return 4;
  }
  fs::remove_all(base / "download", ec);  // a partial download of an earlier attempt
  platform::log("install-pack: installed version " + std::to_string(r.version) + " in " +
                platform::toUtf8(target));
  return 0;
}

}  // namespace pack
