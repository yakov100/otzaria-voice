// Reading aloud in a recorded person's voice: Chatterbox Multilingual (Resemble
// AI, MIT) through ONNX Runtime, the same runtime the recognizer uses.
//
// Text without nikud is vocalized first by the Phonikud diacritizer (MIT,
// based on DictaBERT-char-menaked, CC-BY-4.0): the model reads vocalized
// Hebrew far better. Then, for each sentence, a 0.5B language model turns the
// letters into speech tokens (25 per second) in the voice of a short
// recording, and a one-step decoder (from Chatterbox Turbo, MIT) turns the
// tokens into 24 kHz audio.
//
// The model folder holds:
//   nakdan.onnx                  Phonikud diacritizer (int8)
//   nakdan_tokenizer.json        its character vocabulary (DictaBERT)
//   text_tokenizer.json          Chatterbox's text vocabulary and merges
//   embed_tokens.onnx(.data)     text and speech token embeddings
//   language_model.onnx(.data)   the 0.5B model, 4-bit weights
//   decoder.onnx(.data)          speech tokens -> audio, one step, int8
// A voice is a small .otzvoice file (see Voice) made once from a recording.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "platform.h"

namespace narrator {

constexpr int kSampleRate = 24000;

// What the speech encoder derives from a recording of the speaker; the
// language model and the decoder are conditioned on it.
struct Voice {
  std::vector<float> condEmb;        // [1, condLen, 1024]
  int64_t condLen = 0;
  std::vector<int64_t> promptTokens; // [1, n] speech tokens of the recording
  std::vector<float> xVector;        // [1, 192] speaker embedding
  std::vector<float> promptFeat;     // [1, frames, 80] mel features
  int64_t promptFrames = 0;

  // "OTZVOICE" file: version, then named arrays. Throws on a bad file.
  static Voice load(const platform::Path& path);
};

// Where the models and the voices live: <Otzaria data>/tts/pack and
// <Otzaria data>/tts/voices (OTZARIA_NARRATOR_DIR replaces the pack folder).
platform::Path modelDir();
platform::Path voicesDir();
bool modelReady();
// The voices installed, by file name without ".otzvoice" (sorted).
std::vector<std::string> installedVoices();

// The text split into sentences short enough for one generation each
// (a sentence end, else a comma, else a space). UTF-8 in and out.
std::vector<std::string> pieces(const std::string& text);

class Narrator {
 public:
  // Loads every model (about 1.1GB of memory); throws std::runtime_error.
  Narrator(const platform::Path& ortLibrary, const platform::Path& modelDir, int threads);
  ~Narrator();
  Narrator(const Narrator&) = delete;
  Narrator& operator=(const Narrator&) = delete;

  // Adds nikud to text that has none; vocalized text is returned unchanged.
  std::string vocalize(const std::string& text);

  // Speaks one piece (vocalize it first) in [voice]: 24 kHz mono samples.
  // [cancelled] is polled between tokens; when it returns true the result
  // is empty.
  std::vector<float> speak(const std::string& vocalized, const Voice& voice,
                           const std::function<bool()>& cancelled);

  // Exposed for tests: the token ids the models receive.
  std::vector<int64_t> textTokens(const std::string& vocalized) const;
  std::vector<int64_t> nakdanTokens(const std::u32string& plain) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace narrator
