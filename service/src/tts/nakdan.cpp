#include "nakdan.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "hebrew_text.h"
#include "json.h"
#include "nikud_lexicon.h"
#include "ort_session.h"

namespace nakdan {

namespace {

using platform::Path;
using namespace hebrew;

// The diacritizer (BertForDiacritization) gives one of nikud::kClasses per
// letter; class 1 (mater lectionis) means no mark. It takes 2048 tokens, so
// text goes in windows of this many characters, cut at a space.
constexpr size_t kWindow = 2000;

bool hasModel(const Path& d) {
  std::error_code ec;
  return std::filesystem::exists(d / "nakdan.onnx", ec) &&
         std::filesystem::exists(d / "nakdan_tokenizer.json", ec);
}

}  // namespace

Path dir() {
  const auto e = platform::env("OTZARIA_NAKDAN_DIR");
  if (!e.empty()) return platform::fromUtf8(e);
  const Path own = platform::otzariaDataRoot() / "tts" / "nakdan";
  if (hasModel(own)) return own;
  const auto packEnv = platform::env("OTZARIA_NARRATOR_DIR");
  return packEnv.empty() ? platform::otzariaDataRoot() / "tts" / "pack" : platform::fromUtf8(packEnv);
}

bool ready() { return hasModel(dir()); }

Path lexiconPath() { return dir() / "nikud_lexicon.bin"; }

bool lexiconReady() {
  std::error_code ec;
  return std::filesystem::exists(lexiconPath(), ec);
}

struct Nakdan::Impl {
  ort::Runtime rt;
  OrtSession* session = nullptr;
  std::unordered_map<std::u32string, int64_t> vocab;
  int64_t cls = 0, sep = 0, unk = 0;
  nikud::Lexicon lexicon;  // empty without nikud_lexicon.bin

  Impl(const Path& ortLibrary) : rt(ortLibrary, "otzaria-nakdan") {}
  ~Impl() { rt.close(session); }
};

Nakdan::Nakdan(const Path& ortLibrary, const Path& dir, int threads) : impl_(std::make_unique<Impl>(ortLibrary)) {
  auto& o = *impl_;
  const auto doc = json::parse(readFile(dir / "nakdan_tokenizer.json"));
  for (const auto& [k, v] : doc["model"]["vocab"].o) o.vocab[toU32(k)] = int64_t(v.num());
  o.cls = o.vocab.at(U"[CLS]");
  o.sep = o.vocab.at(U"[SEP]");
  o.unk = o.vocab.at(U"[UNK]");
  if (o.lexicon.load(dir / "nikud_lexicon.bin"))
    platform::log("nikud lexicon: " + std::to_string(o.lexicon.words()) + " words, " +
                  std::to_string(o.lexicon.bigrams()) + " pairs");
  o.session = o.rt.open(dir / "nakdan.onnx", threads);
}

Nakdan::~Nakdan() = default;

size_t Nakdan::lexiconWords() const { return impl_->lexicon.words(); }

std::vector<int64_t> Nakdan::tokens(const std::u32string& plain) const {
  // DictaBERT-char: one token per character, ASCII lower-cased (the
  // tokenizer's NFKC and accent stripping matter only for rare characters).
  const auto& o = *impl_;
  std::vector<int64_t> ids{o.cls};
  for (char32_t c : plain) {
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    auto it = o.vocab.find(std::u32string(1, c));
    ids.push_back(it == o.vocab.end() ? o.unk : it->second);
  }
  ids.push_back(o.sep);
  return ids;
}

std::string Nakdan::vocalize(const std::string& text, bool withMarks) {
  auto& o = *impl_;
  const std::u32string s = toU32(text);

  // The model sees the letters only: [plain] is s without marks (and without
  // '|', which the tokenizer has no token for); plainAt maps s into it.
  std::u32string plain;
  std::vector<size_t> plainAt(s.size(), 0);
  for (size_t i = 0; i < s.size(); i++) {
    plainAt[i] = plain.size();
    const char32_t c = s[i];
    if (!(c >= 0x0590 && c <= 0x05C7) && c != '|') plain.push_back(c);
  }

  // The words (runs of letters and marks): kept when vocalized already,
  // else the lexicon's pattern, else the model's classes.
  struct Word {
    size_t from, to;
    bool keep;
    const std::string* pattern;
    std::u32string letters;
  };
  std::vector<Word> words;
  std::u32string prev;
  bool needModel = false;
  for (size_t i = 0; i < s.size();) {
    if (!isLetter(s[i])) {
      i++;
      continue;
    }
    Word w{i, i, false, nullptr, {}};
    while (i < s.size() && (isLetter(s[i]) || (s[i] >= 0x0590 && s[i] <= 0x05C7))) {
      if (isLetter(s[i])) w.letters.push_back(s[i]);
      else if (isNikud(s[i])) w.keep = true;
      i++;
    }
    w.to = i;
    if (!w.keep) {
      w.pattern = o.lexicon.find(prev, w.letters);
      if (!w.pattern) needModel = true;
    }
    prev = w.letters;
    words.push_back(w);
  }
  if (words.empty()) return text;
  if (withMarks) needModel = true;  // the marks come from the model whatever gives the nikud

  // The model's class (and kSin for a sin dot) for each character of plain,
  // and its pronunciation marks (kStress, kVocalShva) when asked for.
  constexpr unsigned char kStress = 1, kVocalShva = 2;
  std::vector<unsigned char> classes(plain.size(), 0), marks(plain.size(), 0);
  auto runModel = [&](size_t from, size_t to) {
    std::vector<int64_t> ids = tokens(plain.substr(from, to - from));
    std::vector<int64_t> mask(ids.size(), 1), types(ids.size(), 0);
    const std::vector<int64_t> shape{1, int64_t(ids.size())};
    std::vector<OrtValue*> in{o.rt.tensor(ids.data(), ids.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
                              o.rt.tensor(mask.data(), mask.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
                              o.rt.tensor(types.data(), types.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)};
    std::vector<OrtValue*> out;
    ort::ValueGuard guard{o.rt, in, out};
    out = o.rt.run(o.session, {"input_ids", "attention_mask", "token_type_ids"}, in,
                   {"nikud_logits", "shin_logits", "additional_logits"});
    const float* nikud = o.rt.dataOf<float>(out[0]);
    const float* shin = o.rt.dataOf<float>(out[1]);
    // Phonikud's extra heads, one binary classifier each (logit > 0):
    // 0 stress (hat'ama), 1 vocal shva, 2 prefix boundary (not used).
    const float* extra = o.rt.dataOf<float>(out[2]);
    for (size_t i = from; i < to; i++) {
      if (!isLetter(plain[i])) continue;
      const size_t tok = i - from + 1;  // after [CLS]
      const float* row = nikud + tok * nikud::kClassCount;
      const int cls = int(std::max_element(row, row + nikud::kClassCount) - row);
      classes[i] = (unsigned char)cls | (shin[tok * 2 + 1] > shin[tok * 2] ? nikud::kSin : 0);
      marks[i] = (extra[tok * 3] > 0 ? kStress : 0) | (extra[tok * 3 + 1] > 0 ? kVocalShva : 0);
    }
  };
  if (needModel) {
    for (size_t from = 0; from < plain.size();) {
      size_t to = std::min(plain.size(), from + kWindow);
      if (to < plain.size()) {
        const auto sp = plain.rfind(U' ', to);
        if (sp != std::u32string::npos && sp > from + kWindow / 2) to = sp;
      }
      runModel(from, to);
      from = to;
    }
  }

  std::u32string result;
  size_t at = 0;
  auto copy = [&](size_t from, size_t to) {
    for (size_t i = from; i < to; i++)
      if (s[i] != '|') result.push_back(s[i]);
  };
  // After a letter and its nikud: the pronunciation marks, if asked for. A
  // vocal shva is only meaningful on a shva.
  auto addMarks = [&](size_t plainIndex, const std::u32string& lettersMarks) {
    if (!withMarks) return;
    const unsigned char m = marks[plainIndex];
    if (m & kStress) result.push_back(0x05AB);
    if ((m & kVocalShva) && lettersMarks.find(U'ְ') != std::u32string::npos) result.push_back(0x05BD);
  };
  for (const Word& w : words) {
    copy(at, w.from);
    if (w.keep) {
      // Letter by letter: the letter, its own marks (teamim and '|' dropped
      // only when marks are added, to keep the output clean), then ours.
      for (size_t i = w.from; i < w.to;) {
        const size_t letterAt = i;
        const char32_t c = s[i++];
        std::u32string own;
        while (i < w.to && !isLetter(s[i])) {
          if (withMarks ? isNikud(s[i]) : s[i] != '|') own.push_back(s[i]);
          i++;
        }
        result.push_back(c);
        result += own;
        addMarks(plainAt[letterAt], own);
      }
    } else if (w.pattern) {
      size_t letterIndex = 0;
      for (size_t i = w.from; i < w.to; i++) {
        if (!isLetter(s[i])) continue;
        const std::u32string one = nikud::apply(w.letters.substr(letterIndex, 1), w.pattern->substr(letterIndex, 1));
        result += one;
        addMarks(plainAt[i], one);
        letterIndex++;
      }
    } else {
      for (size_t i = w.from; i < w.to; i++) {
        const char32_t c = s[i];
        if (!isLetter(c)) continue;  // teamim on an unvocalized word: dropped
        result.push_back(c);
        const unsigned char p = classes[plainAt[i]];
        std::u32string own;
        if (c == U'ש') own.push_back(p & nikud::kSin ? 0x05C2 : 0x05C1);
        const int cls = p & 0x1F;
        if (cls != 1) own += nikud::kClasses[cls];  // 1: mater lectionis, read as a vowel, no mark
        result += own;
        addMarks(plainAt[i], own);
      }
    }
    at = w.to;
  }
  copy(at, s.size());
  return encode(result);
}

}  // namespace nakdan
