// The reading-aloud model pack's install flow: the dictation pack's (pack.cpp)
// download, verify, unpack beside, swap; only the files and the place differ.
#include "voicemodel.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "light.h"
#include "nakdan.h"
#include "narrator.h"

namespace voicemodel {

const pack::Release kRelease = {
    1,
    "https://github.com/yakov100/otzaria-voice/releases/download/voice-model-v1/otzaria-voice-model-v1.zip",
    914037582,
    "3a757b8888699a48661bb1b43096bf5cf00035b6b23d8e3fdf4152d5b4c2af06",
};

const pack::Release kLexicon = {
    1,
    "https://github.com/yakov100/otzaria-voice/releases/download/voice-model-v1/nikud_lexicon-v1.zip",
    1230701,
    "fbea9a7537bd5cab387d4c0068bde48f1752de18221e016554a057e1444fe7fe",
};

namespace fs = std::filesystem;
using platform::Path;

namespace {

const char* const kRequired[] = {
    "nakdan.onnx",       "nakdan_tokenizer.json", "text_tokenizer.json", "embed_tokens.onnx",
    "language_model.onnx", "decoder.onnx",        "LICENSES.txt"};

std::mutex mu;
pack::Progress current;
std::atomic<bool> running{false};
std::atomic<bool> lexiconRunning{false};
std::atomic<bool> lightRunning{false};

/** Downloads, verifies and unpacks the lexicon into the pack (throws). */
void installLexicon() {
  const pack::Release& r = kLexicon;
  auto override = platform::env("OTZARIA_NIKUD_LEXICON_URL");
  const std::string url = override.empty() ? r.url : override;
  const Path target = narrator::lexiconPath();
  const Path base = narrator::modelDir().parent_path();
  const Path work = base / "download";
  const Path part = work / ("nikud-lexicon-v" + std::to_string(r.version) + ".zip.part");
  const Path staging = base / "lexicon.new";
  std::error_code ec;
  fs::create_directories(work, ec);
  platform::log("nikud lexicon: downloading " + url);
  pack::sys::download(url, part, r.size, [](uint64_t) {});
  if (pack::sys::hashFile(part, [](uint64_t) {}) != r.sha256) {
    fs::remove(part, ec);
    throw std::runtime_error("the downloaded lexicon is corrupt");
  }
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  pack::sys::extract(part, staging);
  const Path file = staging / target.filename();
  if (!fs::exists(file)) throw std::runtime_error("the lexicon zip holds no lexicon");
  fs::remove(target, ec);
  fs::rename(file, target);
  fs::remove_all(staging, ec);
  fs::remove(part, ec);
  platform::log("nikud lexicon: installed version " + std::to_string(r.version));
}

void set(const char* state, uint64_t received, uint64_t total, const std::string& error = "") {
  std::lock_guard<std::mutex> lock(mu);
  current.state = state;
  current.received = received;
  current.total = total;
  current.error = error;
}

void run(std::function<void()> release) {
  const pack::Release& r = kRelease;
  auto override = platform::env("OTZARIA_VOICE_MODEL_URL");
  const std::string url = override.empty() ? r.url : override;
  const Path target = narrator::modelDir();
  const Path base = target.parent_path();
  const Path work = base / "download";
  const Path part = work / ("voice-model-v" + std::to_string(r.version) + ".zip.part");
  const Path staging = base / "pack.new";
  try {
    std::error_code ec;
    fs::create_directories(work, ec);
    const auto space = fs::space(base, ec);
    if (!ec && space.available < r.size * 5 / 2)
      throw std::runtime_error("אין מספיק מקום בדיסק (צריך כ-2.5 ג'יגה פנויים)");
    platform::log("voice model: downloading " + url);
    set("downloading", 0, r.size);
    pack::sys::download(url, part, r.size, [&](uint64_t got) { set("downloading", got, r.size); });
    if (pack::sys::hashFile(part, [&](uint64_t done) { set("verifying", done, r.size); }) != r.sha256) {
      fs::remove(part, ec);
      throw std::runtime_error("הקובץ שהורד פגום; יש לנסות שוב כדי להוריד מחדש");
    }
    set("extracting", r.size, r.size);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    pack::sys::extract(part, staging);
    for (const char* f : kRequired)
      if (!fs::exists(staging / f)) throw std::runtime_error("בחבילת הקול חסרים קבצים");
    {
      std::ofstream v(staging / "version.txt", std::ios::binary);
      v << r.version;
    }
    release();
    const Path old = base / "pack.old";
    fs::remove_all(old, ec);
    if (fs::exists(target)) fs::rename(target, old);
    fs::rename(staging, target);
    fs::remove_all(old, ec);
    fs::remove(part, ec);
    platform::log("voice model: installed version " + std::to_string(r.version));
    set("done", r.size, r.size);
    // The lexicon is a separate, small download, so that the pack need not
    // be fetched again when the lexicon changes.
    try {
      installLexicon();
    } catch (const std::exception& e) {
      platform::log(std::string("nikud lexicon: ") + e.what());
    }
  } catch (const std::exception& e) {
    platform::log(std::string("voice model: ") + e.what());
    std::error_code ec;
    fs::remove_all(staging, ec);
    pack::Progress p = progress();
    set("error", p.received, p.total, e.what());
  }
  running = false;
}

}  // namespace

bool ready() { return narrator::modelReady(); }

int installedVersion() {
  std::ifstream f(narrator::modelDir() / "version.txt", std::ios::binary);
  int v = 0;
  if (!(f >> v)) v = 0;
  return v;
}

pack::Progress progress() {
  std::lock_guard<std::mutex> lock(mu);
  return current;
}

void startInstall(std::function<void()> release) {
  if (running.exchange(true)) return;
  set("downloading", 0, kRelease.size);
  std::thread(run, std::move(release)).detach();
}

void ensureLexicon() {
  if (!nakdan::ready() || nakdan::lexiconReady() || running || lightRunning) return;
  if (lexiconRunning.exchange(true)) return;
  std::thread([] {
    try {
      installLexicon();
    } catch (const std::exception& e) {
      platform::log(std::string("nikud lexicon: ") + e.what());
    }
    lexiconRunning = false;
  }).detach();
}

uint64_t removeInstalled() {
  const Path target = narrator::modelDir();
  const Path base = target.parent_path();
  uint64_t bytes = pack::removeTree(target);
  for (const char* d : {"download", "pack.new", "pack.old"}) bytes += pack::removeTree(base / d);
  set("idle", 0, 0);
  return bytes;
}

// ---------------------------------------------------------------- light voice

const pack::Release kLight = {
    1,
    "https://github.com/yakov100/otzaria-voice/releases/download/light-voice-v1/chaim-light-v1.zip",
    0,  // filled in when the voice is published
    "",
};

const pack::Release kNakdanPack = {
    1,
    "https://github.com/yakov100/otzaria-voice/releases/download/light-voice-v1/nakdan-v1.zip",
    0,
    "",
};

namespace {

std::mutex lightMu;
pack::Progress lightCurrent;

void setLight(const char* state, uint64_t received, uint64_t total, const std::string& error = "") {
  std::lock_guard<std::mutex> lock(lightMu);
  lightCurrent.state = state;
  lightCurrent.received = received;
  lightCurrent.total = total;
  lightCurrent.error = error;
}

/** [r] with its OTZARIA_<prefix>_URL/_SIZE/_SHA256 overrides applied (testing). */
pack::Release effective(const pack::Release& r, const std::string& prefix) {
  static std::mutex mu;
  static std::vector<std::unique_ptr<std::string>> keep;  // the overrides' storage
  pack::Release out = r;
  const auto url = platform::env((prefix + "_URL").c_str());
  const auto size = platform::env((prefix + "_SIZE").c_str());
  const auto sha = platform::env((prefix + "_SHA256").c_str());
  std::lock_guard<std::mutex> lock(mu);
  if (!url.empty()) {
    keep.push_back(std::make_unique<std::string>(url));
    out.url = keep.back()->c_str();
  }
  if (!size.empty()) out.size = std::strtoull(size.c_str(), nullptr, 10);
  if (!sha.empty()) {
    keep.push_back(std::make_unique<std::string>(sha));
    out.sha256 = keep.back()->c_str();
  }
  return out;
}

/**
 * Downloads, verifies and unpacks one zip, then moves its files into [target]
 * (made if missing; files of the same name are replaced, others kept).
 * [required] must all be in the zip. Progress: [offset] bytes count as done
 * already, out of [total]. Throws.
 */
void installZip(const pack::Release& r, const char* partName, const char* stagingName, const Path& target,
                const std::vector<const char*>& required, uint64_t offset, uint64_t total) {
  const Path base = platform::otzariaDataRoot() / "tts";
  const Path work = base / "download";
  const Path part = work / partName;
  const Path staging = base / stagingName;
  std::error_code ec;
  fs::create_directories(work, ec);
  const auto space = fs::space(base, ec);
  if (!ec && space.available < r.size * 5 / 2) throw std::runtime_error("אין מספיק מקום בדיסק");
  platform::log(std::string("light voice: downloading ") + r.url);
  setLight("downloading", offset, total);
  pack::sys::download(r.url, part, r.size, [&](uint64_t got) { setLight("downloading", offset + got, total); });
  if (pack::sys::hashFile(part, [&](uint64_t done) { setLight("verifying", offset + done, total); }) != r.sha256) {
    fs::remove(part, ec);
    throw std::runtime_error("הקובץ שהורד פגום; יש לנסות שוב כדי להוריד מחדש");
  }
  setLight("extracting", offset + r.size, total);
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  pack::sys::extract(part, staging);
  for (const char* f : required)
    if (!fs::exists(staging / f)) throw std::runtime_error(std::string("בחבילה חסר הקובץ ") + f);
  fs::create_directories(target, ec);
  for (const auto& e : fs::directory_iterator(staging, ec)) {
    if (!e.is_regular_file()) continue;
    const Path to = target / e.path().filename();
    fs::remove(to, ec);
    ec.clear();
    fs::rename(e.path(), to, ec);
    if (ec) throw std::runtime_error("cannot place " + platform::toUtf8(to) + ": " + ec.message());
  }
  fs::remove_all(staging, ec);
  fs::remove(part, ec);
}

void runLight(std::function<void()> release) {
  const pack::Release voice = effective(kLight, "OTZARIA_LIGHT_MODEL");
  const pack::Release nk = effective(kNakdanPack, "OTZARIA_NAKDAN_PACK");
  const bool needNakdan = !nakdan::ready();
  const uint64_t total = voice.size + (needNakdan ? nk.size : 0);
  try {
    if (voice.size == 0 || (needNakdan && nk.size == 0)) throw std::runtime_error("not_published");
    release();
    if (needNakdan)
      installZip(nk, "nakdan.zip.part", "nakdan.new", platform::otzariaDataRoot() / "tts" / "nakdan",
                 {"nakdan.onnx", "nakdan_tokenizer.json"}, 0, total);
    installZip(voice, "light-voice.zip.part", "light.new", light::dir(), {}, needNakdan ? nk.size : 0, total);
    if (!light::anyReady()) throw std::runtime_error("בחבילת הקול הקל אין קול");
    platform::log("light voice: installed " + std::to_string(light::installedVoices().size()) + " voice(s)");
    setLight("done", total, total);
    // The lexicon goes beside whichever diacritizer is in use.
    if (!nakdan::lexiconReady()) {
      try {
        installLexicon();
      } catch (const std::exception& e) {
        platform::log(std::string("nikud lexicon: ") + e.what());
      }
    }
  } catch (const std::exception& e) {
    platform::log(std::string("light voice: ") + e.what());
    pack::Progress p = lightProgress();
    setLight("error", p.received, p.total, e.what());
  }
  lightRunning = false;
}

}  // namespace

bool lightReady() { return light::anyReady(); }
bool nakdanReady() { return nakdan::ready(); }
bool lightPublished() { return effective(kLight, "OTZARIA_LIGHT_MODEL").size > 0; }

uint64_t lightDownloadSize() {
  return effective(kLight, "OTZARIA_LIGHT_MODEL").size +
         (nakdan::ready() ? 0 : effective(kNakdanPack, "OTZARIA_NAKDAN_PACK").size);
}

pack::Progress lightProgress() {
  std::lock_guard<std::mutex> lock(lightMu);
  return lightCurrent;
}

void startInstallLight(std::function<void()> release) {
  if (lightRunning.exchange(true)) return;
  setLight("downloading", 0, lightDownloadSize());
  std::thread(runLight, std::move(release)).detach();
}

uint64_t removeLight() {
  const Path base = platform::otzariaDataRoot() / "tts";
  uint64_t bytes = pack::removeTree(light::dir());
  for (const char* d : {"light.new", "nakdan.new"}) bytes += pack::removeTree(base / d);
  if (!narrator::modelReady()) bytes += pack::removeTree(base / "nakdan");
  setLight("idle", 0, 0);
  return bytes;
}

}  // namespace voicemodel
