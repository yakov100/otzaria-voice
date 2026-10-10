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

namespace pack {

const Release kRelease = {
    3,
    "https://github.com/Otzaria/otzaria-voice/releases/download/voice-he-v3/otzaria-voice-he-v3.zip",
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
  const Path base = dir().parent_path();
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
    const Path target = dir(), old = base / "pack.old";
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

}  // namespace

Path dir() {
  auto e = platform::env("OTZARIA_VOICE_PACK_DIR");
  return e.empty() ? platform::otzariaDataRoot() / "stt" / "pack" : platform::fromUtf8(e);
}

bool ready() {
  const auto d = dir();
  std::error_code ec;
  for (int i = 0; i < kRequiredCount; i++)
    if (!fs::exists(d / required(i), ec)) return false;
  return true;
}

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

}  // namespace pack
