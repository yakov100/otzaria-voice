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
//   nikud_lexicon.bin            (optional) vocalized Talmudic words, see
//                                nikud_lexicon.h; without it the diacritizer
//                                alone vocalizes, and reads Aramaic poorly
// A voice is a small .otzvoice file (see Voice) made once from a recording.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
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

  // The same voice with only the last [seconds] of the recording as the
  // decoder's example. The decoder works on the example and the new speech
  // together, so a shorter example is faster; the language model's
  // conditioning (condEmb) is kept whole.
  Voice shortened(double seconds) const;
};

// Where the models and the voices live: <Otzaria data>/tts/pack (with the
// voices that ship in it, pack/voices) and <Otzaria data>/tts/voices for
// voices made on this computer. OTZARIA_NARRATOR_DIR replaces the pack folder.
platform::Path modelDir();
platform::Path voicesDir();
bool modelReady();
// The lexicon file in the pack, and whether it is there.
platform::Path lexiconPath();
bool lexiconReady();
// The voices installed, by file name without ".otzvoice" (sorted, unique).
std::vector<std::string> installedVoices();
// The file of voice [id] (the computer's own voices first); empty if none.
platform::Path voicePath(const std::string& id);

// The text split into sentences short enough for one generation each
// (a sentence end, else a comma, else a space). UTF-8 in and out.
std::vector<std::string> pieces(const std::string& text);

// Where each of [pieces] (from pieces(text)) lies in [text]: [from, to) in
// UTF-16 units, the offsets a plugin's JavaScript string uses. A piece is the
// text's own characters with line breaks and tabs made spaces, so it is
// found by walking the text; a piece that does not match ends where the
// match stopped.
std::vector<std::pair<size_t, size_t>> pieceSpans(const std::string& text, const std::vector<std::string>& pieces);

class Narrator {
 public:
  // Loads every model (about 1.1GB of memory); throws std::runtime_error.
  // The language model and the decoder get their own thread counts, since
  // speakAll runs them at the same time.
  Narrator(const platform::Path& ortLibrary, const platform::Path& modelDir, int lmThreads,
           int decoderThreads);
  ~Narrator();
  Narrator(const Narrator&) = delete;
  Narrator& operator=(const Narrator&) = delete;

  // Adds nikud to the words that have none: from the lexicon when it knows
  // the word (after the word before it), else from the diacritizer, which
  // reads the whole text (in windows of about 2000 characters) for context.
  // Words already vocalized are kept as they are. With [withMarks] the
  // diacritizer's pronunciation marks are added after the letter's nikud:
  // U+05AB (ole) on the stressed letter and U+05BD (meteg) on a vocal
  // shva, as Phonikud defines them; the light voice is trained on them.
  std::string vocalize(const std::string& text, bool withMarks = false);

  // Speaks one piece (vocalize it first) in [voice]: 24 kHz mono samples.
  // [cancelled] is polled between tokens; when it returns true the result
  // is empty.
  std::vector<float> speak(const std::string& vocalized, const Voice& voice,
                           const std::function<bool()>& cancelled);

  // The two halves of speak: speech tokens for a piece, then its audio.
  std::vector<int64_t> generate(const std::string& vocalized, const Voice& voice,
                                const std::function<bool()>& cancelled);
  std::vector<float> decode(const std::vector<int64_t>& tokens, const Voice& voice);
  // Ends the decoder's current run from another thread (it fails with an
  // error); resumeDecoder lets the next run go. speakAll's watchdog uses it.
  void terminateDecoder();
  void resumeDecoder();

  // The audio of several pieces in one decoder call, one result per piece
  // (in order). Rows are padded on the right to the longest, each with its
  // own last token, and the padding's audio is cut off (960 samples a
  // token). One piece goes through decode unchanged. On a processor this
  // is no faster than one piece at a time (see decodeBatched in the .cpp),
  // and the pack's decoder.onnx takes one row only until re-exported.
  std::vector<std::vector<float>> decodeBatch(const std::vector<std::vector<int64_t>>& pieces,
                                              const Voice& voice);

  // Speech tokens for several pieces at once, one result per piece. A step
  // of the language model reads all its weights once however many rows it
  // has, so on a processor a batch of pieces costs little more than one.
  // [onProgress] gets the share of the batch's steps done so far (0..1).
  // Empty when cancelled.
  std::vector<std::vector<int64_t>> generateBatch(const std::vector<std::string>& vocalized,
                                                  const Voice& voice,
                                                  const std::function<bool()>& cancelled,
                                                  const std::function<void(double)>& onProgress = {});

  // Speaks every piece in order, generating the next batch of pieces while a
  // second thread decodes the previous one. [onAudio] receives each piece's
  // samples in order (from the decoding thread). [onProgress], if given, is
  // told how far generation is: pieces [first, first+count) are the batch in
  // flight, [fraction] of it done. Returns false if cancelled.
  bool speakAll(const std::vector<std::string>& pieces, const Voice& voice,
                const std::function<bool()>& cancelled,
                const std::function<void(size_t piece, std::vector<float> audio)>& onAudio,
                const std::function<void(size_t first, size_t count, double fraction)>& onProgress = {});

  // Exposed for tests: the token ids the models receive.
  std::vector<int64_t> textTokens(const std::string& vocalized) const;
  std::vector<int64_t> nakdanTokens(const std::u32string& plain) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace narrator
