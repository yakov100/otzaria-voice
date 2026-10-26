// Nikud for text that has none: the Phonikud diacritizer (MIT, based on
// DictaBERT-char-menaked, CC-BY-4.0) through ONNX Runtime, with the
// Talmudic lexicon (nikud_lexicon.h) ahead of it for the words it knows.
// Both the recorded voice (narrator.h) and the light voice (light.h) need
// vocalized text, so the diacritizer stands on its own here.
//
// Its files, nakdan.onnx and nakdan_tokenizer.json (and the optional
// nikud_lexicon.bin), live in <Otzaria data>/tts/nakdan when installed on
// their own (for the light voice), else in the recorded voice's pack,
// <Otzaria data>/tts/pack. OTZARIA_NAKDAN_DIR replaces both.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "platform.h"

namespace nakdan {

// The folder in use: tts/nakdan when it holds nakdan.onnx, else tts/pack.
platform::Path dir();
// nakdan.onnx and its tokenizer are present in dir().
bool ready();
// The lexicon file beside the model, and whether it is there.
platform::Path lexiconPath();
bool lexiconReady();

class Nakdan {
 public:
  // Loads the model (about 350MB of memory) from [dir]; throws std::runtime_error.
  Nakdan(const platform::Path& ortLibrary, const platform::Path& dir, int threads);
  ~Nakdan();
  Nakdan(const Nakdan&) = delete;
  Nakdan& operator=(const Nakdan&) = delete;

  // Adds nikud to the words that have none: from the lexicon when it knows
  // the word (after the word before it), else from the diacritizer, which
  // reads the whole text (in windows of about 2000 characters) for context.
  // Words already vocalized are kept as they are. With [withMarks] the
  // diacritizer's pronunciation marks are added after the letter's nikud:
  // U+05AB (ole) on the stressed letter and U+05BD (meteg) on a vocal
  // shva, as Phonikud defines them; the light voice is trained on them.
  std::string vocalize(const std::string& text, bool withMarks = false);

  // The lexicon's size (0 without nikud_lexicon.bin).
  size_t lexiconWords() const;

  // Exposed for tests: the token ids the model receives for [plain].
  std::vector<int64_t> tokens(const std::u32string& plain) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nakdan
