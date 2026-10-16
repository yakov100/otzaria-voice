// The light voice: a small VITS model (trained with Piper's training code,
// GPL-3, on speech the recorded voice produced; see docs/tts/light-voice.md)
// through ONNX Runtime. About 60MB, and 20-50 times faster than real time on
// a processor, against the recorded voice's 900MB and real-time speed.
//
// A light voice is two files in <Otzaria data>/tts/light: <id>.onnx and
// <id>.json (Piper's voice config: the symbol table, the sample rate, the
// inference scales). Its id ends in "-light" ("chaim-light"), which is how
// the service tells it from a recorded voice. OTZARIA_LIGHT_DIR replaces
// the folder.
//
// Its input is vocalized text with Phonikud's pronunciation marks, one
// symbol per character (no phonemizer): what nakdan::Nakdan::vocalize gives
// with withMarks. The symbols go in NFD order, as in training.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "platform.h"

namespace light {

constexpr const char* kSuffix = "-light";

platform::Path dir();
// The voices installed: every <id>.onnx with its <id>.json, id ending in
// kSuffix (sorted).
std::vector<std::string> installedVoices();
// The model file of voice [id]; empty if the id is not a light voice's or
// its files are missing.
platform::Path modelPath(const std::string& id);
bool isLightId(const std::string& id);
bool anyReady();

class LightVoice {
 public:
  // Loads [model] and the config beside it; throws std::runtime_error.
  LightVoice(const platform::Path& ortLibrary, const platform::Path& model, int threads);
  ~LightVoice();
  LightVoice(const LightVoice&) = delete;
  LightVoice& operator=(const LightVoice&) = delete;

  int sampleRate() const;

  // Speaks one piece of vocalized text (with marks): mono samples in -1..1
  // at sampleRate(). Characters the voice has no symbol for are skipped.
  std::vector<float> speak(const std::string& text);

  // Exposed for tests: the symbol ids [text] becomes (bos, pad, each symbol
  // followed by pad, eos, as Piper's phonemes_to_ids makes them).
  std::vector<int64_t> ids(const std::string& text) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace light
