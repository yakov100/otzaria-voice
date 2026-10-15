// The reading-aloud model pack's install flow: the dictation pack's (pack.cpp)
// download, verify, unpack beside, swap; only the files and the place differ.
#include "voicemodel.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

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
  if (!narrator::modelReady() || narrator::lexiconReady() || running) return;
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

}  // namespace voicemodel
