// The voice pack's install flow, the same on every system: download (resumed),
// verify, unpack beside the old pack, then swap. The system-specific pieces are
// in pack_win.cpp, pack_posix.cpp and pack_android.cpp.
#include "pack.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pack {

const Release kRelease = {
    3,
    "https://github.com/yakov100/otzaria-voice/releases/download/voice-pack-v3/otzaria-voice-he-v3.zip",
    334167487,
    "bde4cfd19a2a581f7836267ddd7ece700bf93c51e122306d6a567958f4c7eb1e",
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
    set("extracting", r.size, r.size);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    sys::extract(part, staging);
    for (int i = 0; i < kRequiredCount; i++)
      if (!fs::exists(staging / required(i))) throw std::runtime_error("בחבילה חסרים קבצים");
    {
      std::ofstream v(staging / "version.txt", std::ios::binary);
      v << r.version;
    }
    release();  // the old pack's files may be open
    const Path old = base / "pack.old";
    fs::remove_all(old, ec);
    if (fs::exists(target)) fs::rename(target, old);
    fs::rename(staging, target);
    fs::remove_all(old, ec);
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

}  // namespace pack
