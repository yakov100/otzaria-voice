// Windows: live reading (live.h) through waveOut, 16-bit mono at the light
// voice's own rate. One worker thread vocalizes and speaks a few pieces ahead
// of the speakers and hands each to the device as one buffer; what is being
// heard comes from the device's position (waveOutGetPosition) against where
// each piece starts and ends, so it follows pauses and gaps by itself.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "live.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "light.h"
#include "nakdan.h"
#include "narrator.h"
#include "platform.h"

namespace live {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kAhead = 3;          // pieces handed to the device beyond the one heard
constexpr size_t kVocalizeChunk = 8;  // pieces vocalized together, for context
constexpr auto kKeepModel = std::chrono::minutes(5);  // after the last reading
constexpr auto kKeepDevice = std::chrono::seconds(60); // after everything was said
// A pause left this long ends the reading: the plugin that paused it may be
// gone (Otzaria shuts an idle plugin down), and it starts afresh from its
// saved place.
constexpr auto kPauseLimit = std::chrono::minutes(30);

struct Piece {
  std::string text;
  uint64_t from = 0, to = 0;  // UTF-16, in the segment's text
};

struct Segment {
  int64_t id = 0;
  std::vector<Piece> pieces;
};

// A piece handed to the device: samples [start, end) of the reading.
struct Heard {
  size_t seg = 0;
  size_t piece = 0;
  uint64_t start = 0, end = 0;
};

struct Model {
  std::string id;
  std::unique_ptr<nakdan::Nakdan> nakdan;
  std::unique_ptr<light::LightVoice> voice;
};

std::mutex controlMu;  // start, append and stop (the worker thread itself)
std::thread worker;

std::mutex mu;  // everything below
std::condition_variable wake;
std::string phase = "idle";  // idle | loading | running | error
std::string error, voiceId;
double rate = 1.0;
bool stopFlag = false, paused = false;
Clock::time_point pausedAt;
std::atomic<bool> workerAlive{false};
std::vector<Segment> segments;
size_t nextSeg = 0, nextPiece = 0;  // the next piece to speak
std::deque<Heard> timeline;         // the latest pieces handed to the device
HWAVEOUT wave = nullptr;
uint64_t written = 0;               // samples handed to the device
uint64_t heardAtClose = 0;          // the position when the device was closed

std::mutex modelMu;
std::shared_ptr<Model> model;
Clock::time_point lastUse = Clock::now();

std::shared_ptr<Model> loadModel(const std::string& id) {
  std::lock_guard<std::mutex> lock(modelMu);
  lastUse = Clock::now();
  if (model && model->id == id) return model;
  model.reset();  // one voice in memory at a time
  const auto path = light::modelPath(id);
  if (path.empty()) throw std::runtime_error("light_model_missing");
  if (!nakdan::ready()) throw std::runtime_error("nakdan_missing");
  const auto ort = platform::findOnnxRuntime();
  if (ort.empty()) throw std::runtime_error("no_runtime");
  const int threads = std::clamp(int(std::thread::hardware_concurrency() / 4), 2, 4);
  auto m = std::make_shared<Model>();
  m->id = id;
  m->nakdan = std::make_unique<nakdan::Nakdan>(ort, nakdan::dir(), threads);
  m->voice = std::make_unique<light::LightVoice>(ort, path, threads);
  model = m;
  return m;
}

Segment segmentOf(const std::string& text, int64_t id) {
  Segment s;
  s.id = id;
  const auto pieces = narrator::pieces(text);
  const auto spans = narrator::pieceSpans(text, pieces);
  for (size_t i = 0; i < pieces.size(); i++) s.pieces.push_back({pieces[i], spans[i].first, spans[i].second});
  return s;
}

/** Where the device is, in samples of the reading; mu held. */
uint64_t positionLocked() {
  if (!wave) return heardAtClose;
  MMTIME t{};
  t.wType = TIME_SAMPLES;
  if (waveOutGetPosition(wave, &t, sizeof t) != MMSYSERR_NOERROR) return 0;
  if (t.wType == TIME_SAMPLES) return t.u.sample;
  if (t.wType == TIME_BYTES) return t.u.cb / 2;
  return 0;
}

/** [texts] vocalized together (for context), one result per text. */
std::vector<std::string> vocalize(nakdan::Nakdan& nk, const std::vector<std::string>& texts) {
  std::string joined;
  for (const auto& t : texts) joined += t + "\n";
  std::vector<std::string> out;
  std::stringstream ss(nk.vocalize(joined, true));
  std::string line;
  while (std::getline(ss, line)) out.push_back(line);
  if (out.size() != texts.size()) {
    out.clear();
    for (const auto& t : texts) out.push_back(nk.vocalize(t, true));
  }
  return out;
}

struct Buffer {
  WAVEHDR header{};
  std::vector<int16_t> pcm;
};

void run() {
  std::shared_ptr<Model> m;
  std::string voice;
  {
    std::lock_guard<std::mutex> lock(mu);
    voice = voiceId;
  }
  try {
    m = loadModel(voice);
  } catch (const std::exception& e) {
    platform::log(std::string("live reading: ") + e.what());
    std::lock_guard<std::mutex> lock(mu);
    phase = "error";
    error = e.what();
    workerAlive = false;
    return;
  }
  const int sampleRate = m->voice->sampleRate();
  HWAVEOUT device = nullptr;
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = 1;
  fmt.nSamplesPerSec = DWORD(sampleRate);
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = 2;
  fmt.nAvgBytesPerSec = DWORD(sampleRate) * 2;
  if (waveOutOpen(&device, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    platform::log("live reading: cannot open the output device");
    std::lock_guard<std::mutex> lock(mu);
    phase = "error";
    error = "no_audio_device";
    workerAlive = false;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    wave = device;
    if (paused) waveOutPause(device);
    if (phase == "loading") phase = "running";
  }

  std::deque<std::unique_ptr<Buffer>> buffers;
  size_t vocSeg = SIZE_MAX, vocFrom = 0;  // which pieces [voc] holds
  std::vector<std::string> voc;
  bool firstPiece = true;
  auto quietSince = Clock::now();
  while (true) {
    while (!buffers.empty() && (buffers.front()->header.dwFlags & WHDR_DONE)) {
      waveOutUnprepareHeader(device, &buffers.front()->header, sizeof(WAVEHDR));
      buffers.pop_front();
    }
    size_t seg = 0, piece = 0;
    std::vector<std::string> texts;  // the pieces to vocalize, when [voc] lacks this one
    double speed = 1.0;
    {
      std::unique_lock<std::mutex> lock(mu);
      if (stopFlag) break;
      while (nextSeg < segments.size() && nextPiece >= segments[nextSeg].pieces.size()) {
        nextSeg++;
        nextPiece = 0;
      }
      if (nextSeg >= segments.size() || buffers.size() > kAhead) {
        // Everything said and nothing appended for a while: let the device go.
        if (nextSeg >= segments.size() && buffers.empty() && !paused && Clock::now() - quietSince > kKeepDevice) break;
        if (paused && Clock::now() - pausedAt > kPauseLimit) {
          platform::log("live reading: paused too long, ended");
          phase = "idle";
          paused = false;
          segments.clear();
          nextSeg = nextPiece = 0;
          break;
        }
        wake.wait_for(lock, std::chrono::milliseconds(30));
        continue;
      }
      seg = nextSeg;
      piece = nextPiece;
      speed = rate;
      if (seg != vocSeg || piece < vocFrom || piece >= vocFrom + voc.size()) {
        // The first piece of a reading alone, so that it starts at once.
        const size_t count = firstPiece ? 1 : kVocalizeChunk;
        const auto& all = segments[seg].pieces;
        for (size_t i = piece; i < all.size() && i < piece + count; i++) texts.push_back(all[i].text);
      }
    }
    firstPiece = false;
    std::vector<float> audio;
    try {
      if (!texts.empty()) {
        voc = vocalize(*m->nakdan, texts);
        vocSeg = seg;
        vocFrom = piece;
      }
      audio = m->voice->speak(voc[piece - vocFrom], speed);
    } catch (const std::exception& e) {
      platform::log(std::string("live reading: ") + e.what());  // the piece is skipped
    }
    auto buffer = std::make_unique<Buffer>();
    if (!audio.empty()) {
      // Peaks above 0.89 are brought down, as in the MP3 (speech_win.cpp).
      float peak = 0;
      for (float x : audio) peak = std::max(peak, std::fabs(x));
      const float gain = peak > 0.89f ? 0.89f / peak : 1.0f;
      buffer->pcm.reserve(audio.size() + size_t(sampleRate / 6));
      for (float x : audio) buffer->pcm.push_back(int16_t(std::lround(std::clamp(x * gain, -1.0f, 1.0f) * 32767)));
      buffer->pcm.insert(buffer->pcm.end(), size_t(sampleRate / 6 / speed), 0);  // a breath between sentences
    }
    std::lock_guard<std::mutex> lock(mu);
    if (stopFlag) break;
    if (seg == nextSeg && piece == nextPiece) nextPiece++;
    quietSince = Clock::now();
    if (buffer->pcm.empty()) continue;
    buffer->header.lpData = reinterpret_cast<LPSTR>(buffer->pcm.data());
    buffer->header.dwBufferLength = DWORD(buffer->pcm.size() * 2);
    waveOutPrepareHeader(device, &buffer->header, sizeof(WAVEHDR));
    waveOutWrite(device, &buffer->header, sizeof(WAVEHDR));
    timeline.push_back({seg, piece, written, written + buffer->pcm.size()});
    while (timeline.size() > 64) timeline.pop_front();
    written += buffer->pcm.size();
    buffers.push_back(std::move(buffer));
  }

  {
    std::lock_guard<std::mutex> lock(mu);
    heardAtClose = stopFlag ? 0 : positionLocked();
    wave = nullptr;
    if (phase == "idle") {
      timeline.clear();
      written = heardAtClose = 0;
    }
  }
  waveOutReset(device);
  for (auto& b : buffers) waveOutUnprepareHeader(device, &b->header, sizeof(WAVEHDR));
  waveOutClose(device);
  {
    std::lock_guard<std::mutex> lock(modelMu);
    lastUse = Clock::now();
  }
  workerAlive = false;
}

/** Ends the worker; controlMu held. */
void stopWorker() {
  {
    std::lock_guard<std::mutex> lock(mu);
    stopFlag = true;
  }
  wake.notify_all();
  if (worker.joinable()) worker.join();
}

/** Starts the worker for the reading in [segments]; controlMu held, no worker running. */
void startWorker() {
  if (worker.joinable()) worker.join();
  {
    std::lock_guard<std::mutex> lock(mu);
    stopFlag = false;
    heardAtClose = 0;
    written = 0;  // the device starts counting again
    timeline.clear();
    if (phase != "running") phase = "loading";
  }
  workerAlive = true;
  worker = std::thread(run);
}

}  // namespace

bool supported() { return true; }

bool start(const std::string& text, const std::string& voice, double speed, int64_t id, std::string& error_) {
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
    error_ = "empty";
    return false;
  }
  if (light::modelPath(voice).empty()) {
    error_ = "light_model_missing";
    return false;
  }
  if (!nakdan::ready()) {
    error_ = "nakdan_missing";
    return false;
  }
  Segment segment = segmentOf(text, id);
  std::lock_guard<std::mutex> control(controlMu);
  stopWorker();
  {
    std::lock_guard<std::mutex> lock(mu);
    phase = "loading";
    error.clear();
    voiceId = voice;
    rate = std::clamp(speed, 0.5, 3.0);
    paused = false;
    segments.clear();
    segments.push_back(std::move(segment));
    nextSeg = nextPiece = 0;
    timeline.clear();
    written = 0;
  }
  startWorker();
  return true;
}

bool append(const std::string& text, int64_t id, std::string& error_) {
  Segment segment = segmentOf(text, id);
  std::lock_guard<std::mutex> control(controlMu);
  {
    std::lock_guard<std::mutex> lock(mu);
    if (phase != "running" && phase != "loading") {
      error_ = "not_reading";
      return false;
    }
    segments.push_back(std::move(segment));
  }
  // The worker lets the device go some time after everything was said.
  if (!workerAlive) startWorker();
  wake.notify_all();
  return true;
}

void pause() {
  std::lock_guard<std::mutex> lock(mu);
  if (paused || (phase != "running" && phase != "loading")) return;
  paused = true;
  pausedAt = Clock::now();
  if (wave) waveOutPause(wave);
}

void resume() {
  std::lock_guard<std::mutex> lock(mu);
  if (!paused) return;
  paused = false;
  if (wave) waveOutRestart(wave);
  wake.notify_all();
}

void stop() {
  std::lock_guard<std::mutex> control(controlMu);
  stopWorker();
  std::lock_guard<std::mutex> lock(mu);
  phase = "idle";
  error.clear();
  paused = false;
  segments.clear();
  timeline.clear();
  nextSeg = nextPiece = 0;
  written = 0;
}

Status status() {
  std::lock_guard<std::mutex> lock(mu);
  Status st;
  st.voice = voiceId;
  st.error = error;
  if (!segments.empty()) st.lastSegment = segments.back().id;
  if (phase != "running") {
    st.state = phase;
    if (phase == "loading" && paused) st.state = "paused";
    return st;
  }
  const uint64_t pos = positionLocked();
  const Heard* heard = nullptr;
  for (auto it = timeline.rbegin(); it != timeline.rend(); ++it) {
    if (it->start <= pos) {
      heard = &*it;
      break;
    }
  }
  const bool more = nextSeg < segments.size();
  if (paused) st.state = "paused";
  else if (pos < written) st.state = "playing";
  else st.state = more ? "loading" : "done";
  if (heard) {
    const Segment& s = segments[heard->seg];
    st.segment = s.id;
    st.piece = int(heard->piece);
    st.pieces = int(s.pieces.size());
    st.from = s.pieces[heard->piece].from;
    st.to = s.pieces[heard->piece].to;
    st.queued = int(segments.size() - 1 - heard->seg);
  } else {
    st.queued = int(segments.size()) - 1;
  }
  return st;
}

void trim() {
  if (workerAlive) return;
  std::lock_guard<std::mutex> lock(modelMu);
  if (model && Clock::now() - lastUse > kKeepModel) {
    model.reset();
    platform::log("live reading: model unloaded");
  }
}

void release() {
  stop();
  std::lock_guard<std::mutex> lock(modelMu);
  model.reset();
}

}  // namespace live
