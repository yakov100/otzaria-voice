// Hebrew speech recognition: GigaAM-He CTC on ONNX Runtime, a prefix beam search
// fused with the library word LM, and a live transcriber that re-decodes the open
// segment while the user speaks. A C++ port of lib/voice_input/engine in Otzaria.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <string>
#include <string_view>
#include <vector>

namespace voice {

struct OrtState;
struct MappedLm;
using Path = std::filesystem::path;

// log-mel features as in the pack's preprocessor.json (GigaAM).
class Features {
 public:
  Features(int sampleRate, int nFft, int hop, int nMels, double fMin, double fMax);
  int frames(size_t samples) const;
  // nMels x T, row-major (channel first), as the model expects.
  std::vector<float> compute(const float* audio, size_t n) const;
  int nMels() const { return nMels_; }

 private:
  int sampleRate_, nFft_, hop_, nMels_;
  std::vector<double> window_, cos_, sin_, filters_;
};

struct CtcOutput {
  std::vector<float> logProbs;  // frames x vocab, row-major
  int frames = 0;
  int vocab = 0;
};

class CtcModel {
 public:
  // Throws std::runtime_error with a readable message.
  CtcModel(const Path& onnxRuntimeLibrary, const Path& modelDir, int threads);
  ~CtcModel();
  CtcModel(const CtcModel&) = delete;
  CtcModel& operator=(const CtcModel&) = delete;

  CtcOutput run(const float* audio, size_t n);
  const std::vector<std::string>& vocab() const { return vocab_; }
  int blank() const { return blank_; }

 private:
  std::unique_ptr<OrtState> ort_;
  std::unique_ptr<Features> features_;
  std::vector<std::string> vocab_;
  int blank_ = 0;
};

// library.otzlm (tool/voice_pack/build_lm.py), memory-mapped.
class NgramLm {
 public:
  explicit NgramLm(const Path& path);
  ~NgramLm();
  NgramLm(const NgramLm&) = delete;
  NgramLm& operator=(const NgramLm&) = delete;

  int idOf(const std::string& word) const;  // -1 when absent
  bool isPrefix(const std::string& prefix) const;
  std::string displayOf(int id) const;  // "" = same as the word
  double score(int prev2, int prev1, int word) const;  // log10

 private:
  std::string_view word(uint32_t i) const;
  static int64_t find(const uint32_t* rows, const uint32_t* cols, uint32_t row, uint32_t key);

  std::unique_ptr<MappedLm> file_;
  const uint8_t* base_ = nullptr;
  uint32_t v_ = 0, nb_ = 0, nt_ = 0;
  float step_ = 0, backoff_ = 0, unk_ = 0;
  const uint32_t* wOff_ = nullptr;
  const char* wRaw_ = nullptr;
  const uint32_t* dOff_ = nullptr;
  const char* dRaw_ = nullptr;
  const uint8_t* uniQ_ = nullptr;
  const uint32_t *biRow_ = nullptr, *biCol_ = nullptr;
  const uint8_t* biQ_ = nullptr;
  const uint32_t *triRow_ = nullptr, *triCol_ = nullptr;
  const uint8_t* triQ_ = nullptr;
};

std::string normalizeLmWord(const std::string& w);

// Fixed expressions (lm/phrases.txt, tools/build_phrases.py): consecutive word
// pairs and triples of frequent Talmud phrases, as LM ids.
class PhraseSet {
 public:
  PhraseSet() = default;
  PhraseSet(const Path& path, const NgramLm& lm);  // a missing file = empty
  bool pair(int a, int b) const;
  bool triple(int a, int b, int c) const;
  size_t size() const { return count_; }

 private:
  std::unordered_set<uint64_t> pairs_;
  std::unordered_set<uint64_t> triples_;
  size_t count_ = 0;
};

struct LmWeights {
  double alpha = 0.5, beta = 2.0, oov = -8.0, nonPrefix = -6.0, hotword = 3.0, prune = -12.0;
  // Each further letter of a word already off the known prefixes: gluing known
  // words onto an unknown one ("ומנתתאמרהדתניא...") is then not free.
  double oovChar = -1.0;
  double phrasePair = 0.0, phraseTriple = 0.0;  // bonus for continuing a fixed phrase
};

class CtcDecoder {
 public:
  CtcDecoder(const std::vector<std::string>& vocab, int blank, const NgramLm& lm,
             const PhraseSet* phrases = nullptr, LmWeights weights = {});
  // Normalized text (no display forms). [context]: last written words, normalized.
  std::string decode(const CtcOutput& out, int beam, const std::set<std::string>& hotwords,
                     const std::vector<std::string>& context) const;
  std::string display(const std::string& text) const;

 private:
  std::vector<std::string> vocab_;
  std::vector<std::string> vocabNorm_;
  int blank_, space_;
  const NgramLm& lm_;
  const PhraseSet* phrases_;
  LmWeights w_;
};

// Second pass over a finished segment: tries known Talmud phrases over its spans.
// The beam builds words letter by letter and prunes early, so "הכא במאי עסקינן
// כגון" may never be tried although the whole sentence scores better with it. A
// candidate is a span whose consonant skeleton is close to a phrase; a swap is kept
// only when acoustic CTC log-prob + LM of the whole sentence rises. [sounds] gives
// how a phrase sounds (yeshivish, "תו שמע" for "תא שמע"): the audio is scored
// against the best spelling, the LM and the output use the phrase as written.
// Port of lib/voice_input/engine/phrase_rescorer.dart in Otzaria (spec:
// tool/voice_eval/phrase_rescore.py there).
class PhraseRescorer {
 public:
  using Sounds = std::map<std::string, std::vector<std::string>>;
  PhraseRescorer(const std::vector<std::string>& vocab, int blank, const NgramLm& lm,
                 const std::vector<std::string>& phrases, const Sounds& sounds = {},
                 LmWeights weights = {});
  std::vector<std::string> rescore(const CtcOutput& out, std::vector<std::string> words) const;
  // phrases/talmud_sounds.tsv of the pack: phrase TAB sound [TAB sound]; # = comment.
  // A missing file is empty.
  static Sounds loadSounds(const Path& path);
  size_t sounded() const { return sounded_; }

 private:
  struct Spelling {
    std::vector<std::string> words;
    std::u32string skeleton;
  };
  struct Phrase {
    std::vector<std::string> words;
    std::string norm;
    std::vector<Spelling> spellings;  // the phrase itself first
  };
  std::vector<int> labels(const std::vector<std::string>& words) const;
  double lmScore(const std::vector<std::string>& words) const;
  double ctcLogProb(const std::vector<double>& p, int frames, int v,
                    const std::vector<int>& labels) const;

  std::map<std::string, int> index_;
  int blank_;
  const NgramLm& lm_;
  LmWeights w_;
  std::vector<Phrase> phrases_;
  size_t sounded_ = 0;
};

struct LiveUpdate {
  int segment;
  std::string text;
  bool isFinal;
};

class LiveTranscriber {
 public:
  // [rescorer]: second pass at the end of each segment; null = none. The final
  // update may then revise committed words once.
  LiveTranscriber(CtcModel& model, const CtcDecoder& decoder, std::set<std::string> hotwords,
                  std::vector<std::string> context, const PhraseRescorer* rescorer = nullptr);
  std::vector<LiveUpdate> add(const float* chunk, size_t n);
  std::vector<LiveUpdate> finish();

 private:
  void step(const float* part, size_t n, std::vector<LiveUpdate>& out);
  std::vector<std::string> recognize(int beam, CtcOutput* keep = nullptr);
  void update(std::vector<LiveUpdate>& out);
  void endSegment(std::vector<LiveUpdate>& out);
  std::string display(const std::vector<std::string>& words, bool lastIsDraft) const;

  CtcModel& model_;
  const CtcDecoder& decoder_;
  const PhraseRescorer* rescorer_;
  std::set<std::string> hotwords_;
  std::vector<std::string> context_;

  double floor_ = -1;
  bool inSpeech_ = false;
  std::vector<std::vector<float>> preroll_;
  double prerollSec_ = 0;
  std::vector<float> segment_;
  double speechSec_ = 0, silenceSec_ = 0;
  size_t sinceUpdate_ = 0;
  int segmentId_ = 0;
  std::vector<std::string> committed_;
  std::vector<std::string> previous_;
  bool hasPrevious_ = false;
};

}  // namespace voice
