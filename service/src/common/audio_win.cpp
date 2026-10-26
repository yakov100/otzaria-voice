// Windows microphone through waveIn: 64 buffers of 100 ms (6.4 s of room while
// the model loads), signalled through an event.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include "audio.h"

namespace {

class WaveInDevice : public Microphone::Device {
 public:
  static constexpr int kBuffers = 64;
  static constexpr int kSamples = Microphone::kChunk;

  ~WaveInDevice() override { close(); }

  bool open(std::string& error) override {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = Microphone::kSampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = Microphone::kSampleRate * 2;
    MMRESULT r = waveInOpen(&wave_, WAVE_MAPPER, &fmt, DWORD_PTR(event_), 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
      wave_ = nullptr;
      error = r == MMSYSERR_BADDEVICEID || r == MMSYSERR_NODRIVER ? "no microphone"
              : r == MMSYSERR_ALLOCATED                         ? "microphone in use"
                                                                : "cannot open microphone";
      return false;
    }
    data_.assign(size_t(kBuffers) * kSamples, 0);
    headers_.assign(kBuffers, WAVEHDR{});
    for (int i = 0; i < kBuffers; i++) {
      auto& h = headers_[i];
      h.lpData = reinterpret_cast<LPSTR>(&data_[size_t(i) * kSamples]);
      h.dwBufferLength = kSamples * 2;
      waveInPrepareHeader(wave_, &h, sizeof h);
      waveInAddBuffer(wave_, &h, sizeof h);
    }
    if (waveInStart(wave_) != MMSYSERR_NOERROR) {
      error = "cannot start microphone";
      close();
      return false;
    }
    return true;
  }

  void wait(unsigned ms) override { WaitForSingleObject(event_, ms); }

  void drain(const Microphone::Use& use, bool requeue) override {
    while (wave_) {
      auto& h = headers_[next_];
      if (!(h.dwFlags & WHDR_DONE)) return;
      const size_t n = h.dwBytesRecorded / 2;
      std::vector<float> pcm(n);
      const int16_t* s = &data_[size_t(next_) * kSamples];
      for (size_t i = 0; i < n; i++) pcm[i] = s[i] / 32768.0f;
      h.dwFlags &= ~WHDR_DONE;
      if (requeue) waveInAddBuffer(wave_, &h, sizeof h);
      next_ = (next_ + 1) % kBuffers;
      if (n) use(pcm);
      if (!requeue && h.dwBytesRecorded == 0) return;
    }
  }

  void stop() override {
    if (wave_) waveInReset(wave_);
  }

  void close() override {
    if (wave_) {
      waveInReset(wave_);
      for (auto& h : headers_) waveInUnprepareHeader(wave_, &h, sizeof h);
      waveInClose(wave_);
      wave_ = nullptr;
    }
    if (event_) {
      CloseHandle(event_);
      event_ = nullptr;
    }
  }

 private:
  HWAVEIN wave_ = nullptr;
  HANDLE event_ = nullptr;
  std::vector<int16_t> data_;
  std::vector<WAVEHDR> headers_;
  int next_ = 0;
};

}  // namespace

std::unique_ptr<Microphone::Device> makeMicrophoneDevice() {
  return std::make_unique<WaveInDevice>();
}
