#include "audio.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
uint64_t nowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace

std::vector<float> readWav(const platform::Path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("missing " + platform::toUtf8(path));
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t o = 12;
  while (o + 8 <= b.size()) {
    uint32_t size;
    std::memcpy(&size, &b[o + 4], 4);
    if (std::memcmp(&b[o], "data", 4) == 0) {
      std::vector<float> pcm(std::min<size_t>(size, b.size() - o - 8) / 2);
      for (size_t i = 0; i < pcm.size(); i++) {
        int16_t v;
        std::memcpy(&v, &b[o + 8 + i * 2], 2);
        pcm[i] = v / 32768.0f;
      }
      return pcm;
    }
    o += 8 + size + (size & 1);
  }
  throw std::runtime_error("no data chunk");
}

Microphone::Microphone() = default;
Microphone::~Microphone() { close(); }

bool Microphone::open(std::string& error) {
  if (auto wav = platform::env("OTZARIA_VOICE_TEST_WAV"); !wav.empty()) {
    try {
      test_ = readWav(platform::fromUtf8(wav));
    } catch (...) {
      error = "test recording missing";
      return false;
    }
    test_.insert(test_.end(), kSampleRate * 2, 0.0f);
    testStart_ = nowMs();
    testMode_ = true;
    return true;
  }
  device_ = makeMicrophoneDevice();
  if (!device_->open(error)) {
    device_.reset();
    return false;
  }
  return true;
}

void Microphone::wait(unsigned ms) {
  if (testMode_) platform::sleepMs(ms);
  else if (device_) device_->wait(ms);
}

void Microphone::drain(const Use& use, bool requeue) {
  if (testMode_) {
    // Whatever "was recorded" by now; silence after the recording ends.
    const size_t due = requeue ? size_t((nowMs() - testStart_) * 16) : testPos_ + kChunk;
    while (testPos_ + kChunk <= due) {
      std::vector<float> pcm(kChunk, 0.0f);
      for (size_t i = 0; i < size_t(kChunk) && testPos_ + i < test_.size(); i++)
        pcm[i] = test_[testPos_ + i];
      testPos_ += kChunk;
      use(pcm);
    }
    return;
  }
  if (device_) device_->drain(use, requeue);
}

void Microphone::stop() {
  if (device_) device_->stop();
}

void Microphone::close() {
  if (device_) {
    device_->close();
    device_.reset();
  }
}
