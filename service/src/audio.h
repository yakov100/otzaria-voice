// The microphone: 16 kHz mono, handed out in chunks of about 100 ms.
//
// Windows records with waveIn (audio_win.cpp); Linux, macOS and Android with
// miniaudio (audio_miniaudio.cpp), which picks PulseAudio/ALSA, Core Audio or
// AAudio/OpenSL ES at run time. Building with OTZARIA_VOICE_MINIAUDIO=ON uses
// miniaudio on Windows too.
//
// Testing: OTZARIA_VOICE_TEST_WAV=<16 kHz mono WAV> plays that recording in real
// time instead of the microphone, then silence.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "platform.h"

// 16 kHz mono PCM16 WAV as floats in [-1, 1).
std::vector<float> readWav(const platform::Path& path);

class Microphone {
 public:
  static constexpr int kSampleRate = 16000;
  static constexpr int kChunk = 1600;  // 100 ms
  using Use = std::function<void(const std::vector<float>&)>;

  // One recording device; see audio_win.cpp and audio_miniaudio.cpp.
  class Device {
   public:
    virtual ~Device() = default;
    virtual bool open(std::string& error) = 0;
    virtual void wait(unsigned ms) = 0;
    virtual void drain(const Use& use, bool requeue) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
  };

  Microphone();
  ~Microphone();

  bool open(std::string& error);
  // Waits up to [ms] for more sound.
  void wait(unsigned ms);
  // Recorded chunks, in order. With [requeue] false (after stop) it returns
  // what is left in the buffers.
  void drain(const Use& use, bool requeue = true);
  // Stops recording; what was already recorded stays readable through drain().
  void stop();
  void close();

 private:
  std::unique_ptr<Device> device_;
  bool testMode_ = false;
  std::vector<float> test_;
  size_t testPos_ = 0;
  uint64_t testStart_ = 0;
};

std::unique_ptr<Microphone::Device> makeMicrophoneDevice();
