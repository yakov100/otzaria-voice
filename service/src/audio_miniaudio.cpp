// Microphone through miniaudio (Linux: PulseAudio/PipeWire, ALSA, JACK; macOS:
// Core Audio; Android: AAudio, OpenSL ES). miniaudio converts whatever the
// device gives to 16 kHz mono float; the callback queues it, and drain() hands
// it out in 100 ms chunks like the Windows recorder.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#if defined(__APPLE__)
// Linked against Core Audio instead of loading it at run time (notarization).
#define MA_NO_RUNTIME_LINKING
#endif
#include "../third_party/miniaudio/miniaudio.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "audio.h"

namespace {

class MiniaudioDevice : public Microphone::Device {
 public:
  // At most 60 s waiting to be read (the model may still be loading).
  static constexpr size_t kMaxQueued = size_t(Microphone::kSampleRate) * 60;

  ~MiniaudioDevice() override { close(); }

  bool open(std::string& error) override {
    if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
      error = "no audio system";
      return false;
    }
    hasContext_ = true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.sampleRate = Microphone::kSampleRate;
    cfg.periodSizeInMilliseconds = 100;
    cfg.dataCallback = &MiniaudioDevice::onData;
    cfg.pUserData = this;
    const ma_result r = ma_device_init(&context_, &cfg, &device_);
    if (r != MA_SUCCESS) {
      error = r == MA_NO_DEVICE              ? "no microphone"
              : r == MA_ACCESS_DENIED        ? "microphone access denied"
              : r == MA_BUSY || r == MA_ALREADY_IN_USE ? "microphone in use"
                                              : "cannot open microphone";
      close();
      return false;
    }
    hasDevice_ = true;
    if (ma_device_start(&device_) != MA_SUCCESS) {
      error = "cannot start microphone";
      close();
      return false;
    }
    return true;
  }

  void wait(unsigned ms) override {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms),
                 [&] { return queue_.size() >= size_t(Microphone::kChunk) || stopped_; });
  }

  void drain(const Microphone::Use& use, bool requeue) override {
    while (true) {
      std::vector<float> pcm;
      {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t n = queue_.size() >= size_t(Microphone::kChunk) ? size_t(Microphone::kChunk)
                         : requeue                                   ? 0
                                                                     : queue_.size();
        if (!n) return;
        pcm.assign(queue_.begin(), queue_.begin() + std::ptrdiff_t(n));
        queue_.erase(queue_.begin(), queue_.begin() + std::ptrdiff_t(n));
      }
      use(pcm);
    }
  }

  void stop() override {
    if (hasDevice_) ma_device_stop(&device_);  // waits for the last callback
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
    cv_.notify_all();
  }

  void close() override {
    if (hasDevice_) {
      ma_device_uninit(&device_);
      hasDevice_ = false;
    }
    if (hasContext_) {
      ma_context_uninit(&context_);
      hasContext_ = false;
    }
  }

 private:
  static void onData(ma_device* d, void*, const void* input, ma_uint32 frames) {
    auto* self = static_cast<MiniaudioDevice*>(d->pUserData);
    const auto* samples = static_cast<const float*>(input);
    if (!samples) return;
    std::lock_guard<std::mutex> lock(self->mu_);
    if (self->queue_.size() + frames > kMaxQueued) return;  // nobody is reading: drop
    self->queue_.insert(self->queue_.end(), samples, samples + frames);
    self->cv_.notify_all();
  }

  ma_context context_{};
  ma_device device_{};
  bool hasContext_ = false, hasDevice_ = false;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<float> queue_;
  bool stopped_ = false;
};

}  // namespace

std::unique_ptr<Microphone::Device> makeMicrophoneDevice() {
  return std::make_unique<MiniaudioDevice>();
}
