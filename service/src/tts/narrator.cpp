#include "narrator.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "../../third_party/onnxruntime_c_api.h"
#include "json.h"
#include "nikud_lexicon.h"

namespace narrator {

namespace {

using platform::Path;

// ---------------------------------------------------------------- text helpers

std::u32string toU32(const std::string& s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
    else { i++; continue; }  // stray continuation byte
    if (i + n > s.size()) break;
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

std::string encode(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

std::string encode(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) out += encode(c);
  return out;
}

bool isHebrewLetter(char32_t c) { return c >= 0x05D0 && c <= 0x05EA; }
// Nikud proper (not teamim): sheva .. qubuts, dagesh, shin/sin dots, qamats qatan.
bool isNikud(char32_t c) { return (c >= 0x05B0 && c <= 0x05BC) || c == 0x05C1 || c == 0x05C2 || c == 0x05C7; }
bool isSpace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0; }

std::string readFile(const Path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("missing file: " + platform::toUtf8(path));
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The diacritizer (BertForDiacritization) gives one of nikud::kClasses per
// letter; class 1 (mater lectionis) means no mark. It takes 2048 tokens, so
// text goes in windows of this many characters, cut at a space.
constexpr size_t kNakdanWindow = 2000;

// Chatterbox text/speech token ids.
constexpr int64_t kStop = 0, kUnk = 1, kSpace = 2, kStart = 255, kHebrew = 2110;
constexpr int64_t kStartSpeech = 6561, kStopSpeech = 6562, kExaggeration = 6563;
constexpr int kLayers = 30, kKvHeads = 16, kHeadDim = 64, kHidden = 1024;
constexpr int kMaxSpeechTokens = 1000;  // 40 seconds; a piece is far shorter
// The ONNX export's example uses 1.2; with greedy choice that sometimes locks
// into silence after the first words (one sentence of six in a Mishnah test).
// 2.0, the original Chatterbox Multilingual setting, read all six through.
constexpr float kRepetitionPenalty = 2.0f;
constexpr float kExaggerationValue = 0.5f;
// Pieces generated together (generateBatch). The first batch is small so
// that the first sentence is heard soon; the later ones fill the batch.
// OTZARIA_NARRATOR_BATCH overrides the size (evaluation only).
constexpr size_t kFirstBatch = 2;
size_t batchSize() {
  const auto e = platform::env("OTZARIA_NARRATOR_BATCH");
  const int n = e.empty() ? 0 : atoi(e.c_str());
  return n > 0 ? size_t(std::min(n, 16)) : 6;
}
bool timing() { return !platform::env("OTZARIA_NARRATOR_TIMING").empty(); }
// Pieces decoded in one call by speakAll's decoder thread. One, because the
// decoder gains nothing from a batch on a processor: it is compute bound
// (about 28 ms a token at 6 threads, no per-call cost to share), so a batch
// costs the sum of its rows plus the padding to the longest (a third more
// on a batch of 6 measured 2026-10-15; grouping rows by length did not help
// either). Also, the decoder.onnx in the pack tiles its mask by the batch
// size once too often, so it fails on a batch of two until re-exported.
// OTZARIA_NARRATOR_DECODE_BATCH=1 decodes a whole batch at once (evaluation
// only, with a fixed decoder).
bool decodeBatched() { return platform::env("OTZARIA_NARRATOR_DECODE_BATCH") == "1"; }

}  // namespace

// ---------------------------------------------------------------- voice file

Voice Voice::load(const Path& path) {
  const std::string data = readFile(path);
  size_t at = 0;
  auto take = [&](size_t n) {
    if (at + n > data.size()) throw std::runtime_error("voice file is truncated");
    const char* p = data.data() + at;
    at += n;
    return p;
  };
  auto u32 = [&] { uint32_t v; memcpy(&v, take(4), 4); return v; };
  if (std::string(take(8), 8) != "OTZVOICE") throw std::runtime_error("not a voice file");
  if (u32() != 1) throw std::runtime_error("unknown voice file version");
  Voice v;
  const uint32_t count = u32();
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t nameLen = u32();
    const std::string name(take(nameLen), nameLen);
    const uint32_t type = u32();
    const uint32_t ndim = u32();
    std::vector<int64_t> dims(ndim);
    size_t n = 1;
    for (auto& d : dims) {
      memcpy(&d, take(8), 8);
      n *= size_t(d);
    }
    const size_t elem = type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ? 8 : 4;
    const char* p = take(n * elem);
    if (name == "cond_emb" && type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && ndim == 3) {
      v.condEmb.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + n);
      v.condLen = dims[1];
    } else if (name == "prompt_token" && type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
      v.promptTokens.assign(reinterpret_cast<const int64_t*>(p), reinterpret_cast<const int64_t*>(p) + n);
    } else if (name == "ref_x_vector" && type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      v.xVector.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + n);
    } else if (name == "prompt_feat" && type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && ndim == 3) {
      v.promptFeat.assign(reinterpret_cast<const float*>(p), reinterpret_cast<const float*>(p) + n);
      v.promptFrames = dims[1];
    }
  }
  if (v.condEmb.empty() || v.promptTokens.empty() || v.xVector.size() != 192 || v.promptFeat.empty())
    throw std::runtime_error("voice file is incomplete");
  return v;
}

Voice Voice::shortened(double seconds) const {
  Voice v(*this);
  const int64_t tokens = std::max<int64_t>(1, int64_t(seconds * 25));
  if (int64_t(v.promptTokens.size()) > tokens) {
    v.promptTokens.erase(v.promptTokens.begin(), v.promptTokens.end() - tokens);
    const int64_t frames = std::min<int64_t>(v.promptFrames, 2 * tokens);  // 50 mel frames a second
    v.promptFeat.erase(v.promptFeat.begin(), v.promptFeat.end() - frames * 80);
    v.promptFrames = frames;
  }
  return v;
}

// ---------------------------------------------------------------- folders

Path modelDir() {
  const auto e = platform::env("OTZARIA_NARRATOR_DIR");
  return e.empty() ? platform::otzariaDataRoot() / "tts" / "pack" : platform::fromUtf8(e);
}

Path voicesDir() { return platform::otzariaDataRoot() / "tts" / "voices"; }

bool modelReady() {
  std::error_code ec;
  const Path d = modelDir();
  for (const char* f : {"nakdan.onnx", "nakdan_tokenizer.json", "text_tokenizer.json", "embed_tokens.onnx",
                        "language_model.onnx", "decoder.onnx"})
    if (!std::filesystem::exists(d / f, ec)) return false;
  return true;
}

Path lexiconPath() { return modelDir() / "nikud_lexicon.bin"; }

bool lexiconReady() {
  std::error_code ec;
  return std::filesystem::exists(lexiconPath(), ec);
}

std::vector<std::string> installedVoices() {
  std::vector<std::string> out;
  for (const Path& dir : {voicesDir(), modelDir() / "voices"}) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
      if (e.path().extension() == ".otzvoice") out.push_back(platform::toUtf8(e.path().stem()));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Path voicePath(const std::string& id) {
  // A voice id is a plain file name: nothing that could leave the folders.
  if (id.empty() || id.find_first_of("/\\:.") != std::string::npos) return {};
  for (const Path& dir : {voicesDir(), modelDir() / "voices"}) {
    const Path p = dir / platform::fromUtf8(id + ".otzvoice");
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return p;
  }
  return {};
}

// ---------------------------------------------------------------- pieces

std::vector<std::string> pieces(const std::string& text) {
  // Letters per piece, not counting nikud. With 160 the model lost its way
  // inside long unpunctuated stretches: in a Josephus passage one piece came
  // out as unrelated words and others dropped their endings (46% of words
  // wrong when transcribed back); halved to about 75, 16%.
  constexpr size_t kTarget = 80;
  const std::u32string s = toU32(text);
  std::vector<std::string> out;
  std::u32string cur;
  size_t letters = 0;
  size_t lastComma = std::u32string::npos, lastSpace = std::u32string::npos;
  auto flush = [&](size_t upto) {
    std::u32string head = cur.substr(0, upto);
    std::u32string rest = cur.substr(upto);
    const auto a = head.find_first_not_of(U" \t\r\n");
    if (a != std::u32string::npos) {
      const auto b = head.find_last_not_of(U" \t\r\n");
      out.push_back(encode(head.substr(a, b - a + 1)));
    }
    cur = rest;
    letters = 0;
    for (char32_t c : cur)
      if (!isNikud(c) && !(c >= 0x0591 && c <= 0x05AF)) letters++;
    lastComma = lastSpace = std::u32string::npos;
  };
  for (char32_t c : s) {
    cur.push_back(c == '\n' || c == '\r' || c == '\t' ? U' ' : c);
    if (!isNikud(c) && !(c >= 0x0591 && c <= 0x05AF)) letters++;
    if (c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == 0x05C3 || c == '\n') {
      flush(cur.size());
      continue;
    }
    if (c == ',') lastComma = cur.size();
    if (c == ' ') lastSpace = cur.size();
    if (letters >= kTarget) {
      if (lastComma != std::u32string::npos && lastComma > cur.size() / 3) flush(lastComma);
      else if (lastSpace != std::u32string::npos) flush(lastSpace);
      else flush(cur.size());
    }
  }
  flush(cur.size());
  return out;
}

// ---------------------------------------------------------------- ONNX Runtime

struct Narrator::Impl {
  std::unique_ptr<platform::DynamicLibrary> dll;
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtMemoryInfo* memory = nullptr;
  OrtAllocator* allocator = nullptr;
  OrtSession* nakdan = nullptr;
  OrtSession* embed = nullptr;
  OrtSession* lm = nullptr;
  OrtSession* decoder = nullptr;
  // The decoder's runs can be terminated from another thread (a watchdog):
  // one token sequence was seen to loop inside the decoder graph for ever.
  OrtRunOptions* decoderRun = nullptr;
  std::vector<std::string> lmInputNames, lmOutputNames;

  std::unordered_map<std::u32string, int64_t> nakdanVocab;
  int64_t nakdanCls = 0, nakdanSep = 0, nakdanUnk = 0;
  std::unordered_map<std::u32string, int64_t> textVocab;
  std::map<std::pair<std::u32string, std::u32string>, int> merges;  // pair -> rank
  nikud::Lexicon lexicon;  // empty without nikud_lexicon.bin

  void check(OrtStatus* s) {
    if (!s) return;
    std::string msg = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    throw std::runtime_error("ONNX Runtime: " + msg);
  }

  OrtSession* open(const Path& path, int threads) {
    OrtSessionOptions* opts = nullptr;
    check(api->CreateSessionOptions(&opts));
    struct Guard {
      const OrtApi* api;
      OrtSessionOptions* p;
      ~Guard() { api->ReleaseSessionOptions(p); }
    } guard{api, opts};
    check(api->SetIntraOpNumThreads(opts, threads));
    check(api->SetInterOpNumThreads(opts, 1));
    check(api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL));
    check(api->AddSessionConfigEntry(opts, "session.intra_op.allow_spinning", "0"));
    OrtSession* s = nullptr;
    check(api->CreateSession(env, path.c_str(), opts, &s));
    return s;
  }

  std::vector<std::string> names(OrtSession* s, bool inputs) {
    size_t n = 0;
    check(inputs ? api->SessionGetInputCount(s, &n) : api->SessionGetOutputCount(s, &n));
    std::vector<std::string> out;
    for (size_t i = 0; i < n; i++) {
      char* name = nullptr;
      check(inputs ? api->SessionGetInputName(s, i, allocator, &name)
                   : api->SessionGetOutputName(s, i, allocator, &name));
      out.push_back(name);
      allocator->Free(allocator, name);
    }
    return out;
  }

  // A tensor over caller-owned memory (kept alive by the caller during Run).
  template <typename T>
  OrtValue* tensor(T* data, size_t count, std::vector<int64_t> shape, ONNXTensorElementDataType type) {
    OrtValue* v = nullptr;
    check(api->CreateTensorWithDataAsOrtValue(memory, data, count * sizeof(T), shape.data(), shape.size(),
                                              type, &v));
    return v;
  }

  OrtValue* emptyTensor(std::vector<int64_t> shape) {
    OrtValue* v = nullptr;
    check(api->CreateTensorAsOrtValue(allocator, shape.data(), shape.size(),
                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v));
    return v;
  }

  std::vector<int64_t> shapeOf(OrtValue* v) {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    check(api->GetTensorTypeAndShape(v, &info));
    size_t dims = 0;
    OrtStatus* s = api->GetDimensionsCount(info, &dims);
    std::vector<int64_t> shape(dims);
    if (!s && dims) s = api->GetDimensions(info, shape.data(), dims);
    api->ReleaseTensorTypeAndShapeInfo(info);
    check(s);
    return shape;
  }

  template <typename T>
  T* dataOf(OrtValue* v) {
    void* p = nullptr;
    check(api->GetTensorMutableData(v, &p));
    return static_cast<T*>(p);
  }

  // Runs [s] and returns its outputs (owned by the caller).
  std::vector<OrtValue*> run(OrtSession* s, const std::vector<std::string>& inNames,
                             const std::vector<OrtValue*>& in, const std::vector<std::string>& outNames) {
    std::vector<const char*> ip, op;
    for (auto& n : inNames) ip.push_back(n.c_str());
    for (auto& n : outNames) op.push_back(n.c_str());
    std::vector<OrtValue*> out(outNames.size(), nullptr);
    check(api->Run(s, s == decoder ? decoderRun : nullptr, ip.data(), in.data(), in.size(), op.data(), op.size(),
                   out.data()));
    return out;
  }

  void release(std::vector<OrtValue*>& values) {
    for (auto*& v : values)
      if (v) {
        api->ReleaseValue(v);
        v = nullptr;
      }
  }

  ~Impl() {
    if (!api) return;
    for (OrtSession* s : {nakdan, embed, lm, decoder})
      if (s) api->ReleaseSession(s);
    if (decoderRun) api->ReleaseRunOptions(decoderRun);
    if (memory) api->ReleaseMemoryInfo(memory);
    if (env) api->ReleaseEnv(env);
  }
};

Narrator::Narrator(const Path& ortLibrary, const Path& modelDir, int lmThreads, int decoderThreads)
    : impl_(std::make_unique<Impl>()) {
  auto& o = *impl_;
  o.dll = std::make_unique<platform::DynamicLibrary>(ortLibrary);
  using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
  auto getBase = reinterpret_cast<GetApiBase>(o.dll->symbol("OrtGetApiBase"));
  if (!getBase) throw std::runtime_error("OrtGetApiBase missing");
  for (uint32_t v = ORT_API_VERSION; v >= 11 && !o.api; v--) o.api = getBase()->GetApi(v);
  if (!o.api) throw std::runtime_error("ONNX Runtime too old");
  o.check(o.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "otzaria-narrator", &o.env));
  o.check(o.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &o.memory));
  o.check(o.api->CreateRunOptions(&o.decoderRun));
  o.check(o.api->GetAllocatorWithDefaultOptions(&o.allocator));

  // Vocabularies.
  {
    const auto doc = json::parse(readFile(modelDir / "nakdan_tokenizer.json"));
    for (const auto& [k, v] : doc["model"]["vocab"].o) o.nakdanVocab[toU32(k)] = int64_t(v.num());
    o.nakdanCls = o.nakdanVocab.at(U"[CLS]");
    o.nakdanSep = o.nakdanVocab.at(U"[SEP]");
    o.nakdanUnk = o.nakdanVocab.at(U"[UNK]");
  }
  {
    const auto doc = json::parse(readFile(modelDir / "text_tokenizer.json"));
    for (const auto& [k, v] : doc["model"]["vocab"].o) o.textVocab[toU32(k)] = int64_t(v.num());
    int rank = 0;
    for (const auto& m : doc["model"]["merges"].arr()) {
      const std::u32string pair = toU32(m.str());
      const auto sp = pair.find(U' ');
      if (sp != std::u32string::npos) o.merges[{pair.substr(0, sp), pair.substr(sp + 1)}] = rank;
      rank++;
    }
  }

  if (o.lexicon.load(modelDir / "nikud_lexicon.bin"))
    platform::log("nikud lexicon: " + std::to_string(o.lexicon.words()) + " words, " +
                  std::to_string(o.lexicon.bigrams()) + " pairs");
  o.nakdan = o.open(modelDir / "nakdan.onnx", lmThreads);
  o.embed = o.open(modelDir / "embed_tokens.onnx", lmThreads);
  o.lm = o.open(modelDir / "language_model.onnx", lmThreads);
  o.decoder = o.open(modelDir / "decoder.onnx", decoderThreads);
  o.lmInputNames = o.names(o.lm, true);
  o.lmOutputNames = o.names(o.lm, false);
  if (o.lmInputNames.size() != 2 + 2 * kLayers || o.lmOutputNames.size() != 1 + 2 * kLayers)
    throw std::runtime_error("unexpected language model");
}

Narrator::~Narrator() = default;

// ---------------------------------------------------------------- tokens

std::vector<int64_t> Narrator::nakdanTokens(const std::u32string& plain) const {
  // DictaBERT-char: one token per character, ASCII lower-cased (the
  // tokenizer's NFKC and accent stripping matter only for rare characters).
  const auto& o = *impl_;
  std::vector<int64_t> ids{o.nakdanCls};
  for (char32_t c : plain) {
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    auto it = o.nakdanVocab.find(std::u32string(1, c));
    ids.push_back(it == o.nakdanVocab.end() ? o.nakdanUnk : it->second);
  }
  ids.push_back(o.nakdanSep);
  return ids;
}

std::vector<int64_t> Narrator::textTokens(const std::string& vocalized) const {
  // Chatterbox's tokenizer: every Hebrew letter and mark is its own token, a
  // space is [SPACE], other white space is dropped, and the only merges are
  // between lower-case Latin letters (applied here by rank, as BPE does).
  const auto& o = *impl_;
  std::vector<int64_t> ids{kExaggeration, kStart, kHebrew};
  const std::u32string s = toU32(vocalized);
  auto lookup = [&](const std::u32string& t) {
    auto it = o.textVocab.find(t);
    return it == o.textVocab.end() ? kUnk : it->second;
  };
  for (size_t i = 0; i < s.size();) {
    const char32_t c = s[i];
    if (c == ' ') {
      ids.push_back(kSpace);
      i++;
    } else if (isSpace(c)) {
      i++;
    } else if (c >= 'a' && c <= 'z') {
      std::vector<std::u32string> parts;
      while (i < s.size() && s[i] >= 'a' && s[i] <= 'z') parts.push_back(std::u32string(1, s[i++]));
      while (parts.size() > 1) {
        int best = -1;
        size_t at = 0;
        for (size_t k = 0; k + 1 < parts.size(); k++) {
          auto it = o.merges.find({parts[k], parts[k + 1]});
          if (it != o.merges.end() && (best < 0 || it->second < best)) {
            best = it->second;
            at = k;
          }
        }
        if (best < 0) break;
        parts[at] += parts[at + 1];
        parts.erase(parts.begin() + long(at) + 1);
      }
      for (auto& p : parts) ids.push_back(lookup(p));
    } else {
      ids.push_back(lookup(std::u32string(1, c)));
      i++;
    }
  }
  ids.insert(ids.end(), {kStop, kStartSpeech, kStartSpeech});
  return ids;
}

// ---------------------------------------------------------------- nikud

std::string Narrator::vocalize(const std::string& text, bool withMarks) {
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
    if (!isHebrewLetter(s[i])) {
      i++;
      continue;
    }
    Word w{i, i, false, nullptr, {}};
    while (i < s.size() && (isHebrewLetter(s[i]) || (s[i] >= 0x0590 && s[i] <= 0x05C7))) {
      if (isHebrewLetter(s[i])) w.letters.push_back(s[i]);
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
    std::vector<int64_t> ids = nakdanTokens(plain.substr(from, to - from));
    std::vector<int64_t> mask(ids.size(), 1), types(ids.size(), 0);
    const std::vector<int64_t> shape{1, int64_t(ids.size())};
    std::vector<OrtValue*> in{o.tensor(ids.data(), ids.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
                              o.tensor(mask.data(), mask.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
                              o.tensor(types.data(), types.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)};
    std::vector<OrtValue*> out;
    struct Guard {
      Impl& o;
      std::vector<OrtValue*>& a;
      std::vector<OrtValue*>& b;
      ~Guard() {
        o.release(a);
        o.release(b);
      }
    } guard{o, in, out};
    out = o.run(o.nakdan, {"input_ids", "attention_mask", "token_type_ids"}, in,
                {"nikud_logits", "shin_logits", "additional_logits"});
    const float* nikud = o.dataOf<float>(out[0]);
    const float* shin = o.dataOf<float>(out[1]);
    // Phonikud's extra heads, one binary classifier each (logit > 0):
    // 0 stress (hat'ama), 1 vocal shva, 2 prefix boundary (not used).
    const float* extra = o.dataOf<float>(out[2]);
    for (size_t i = from; i < to; i++) {
      if (!isHebrewLetter(plain[i])) continue;
      const size_t tok = i - from + 1;  // after [CLS]
      const float* row = nikud + tok * nikud::kClassCount;
      const int cls = int(std::max_element(row, row + nikud::kClassCount) - row);
      classes[i] = (unsigned char)cls | (shin[tok * 2 + 1] > shin[tok * 2] ? nikud::kSin : 0);
      marks[i] = (extra[tok * 3] > 0 ? kStress : 0) | (extra[tok * 3 + 1] > 0 ? kVocalShva : 0);
    }
  };
  if (needModel) {
    for (size_t from = 0; from < plain.size();) {
      size_t to = std::min(plain.size(), from + kNakdanWindow);
      if (to < plain.size()) {
        const auto sp = plain.rfind(U' ', to);
        if (sp != std::u32string::npos && sp > from + kNakdanWindow / 2) to = sp;
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
        while (i < w.to && !isHebrewLetter(s[i])) {
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
        if (!isHebrewLetter(s[i])) continue;
        const std::u32string one = nikud::apply(w.letters.substr(letterIndex, 1), w.pattern->substr(letterIndex, 1));
        result += one;
        addMarks(plainAt[i], one);
        letterIndex++;
      }
    } else {
      for (size_t i = w.from; i < w.to; i++) {
        const char32_t c = s[i];
        if (!isHebrewLetter(c)) continue;  // teamim on an unvocalized word: dropped
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

// ---------------------------------------------------------------- speech

std::vector<float> Narrator::speak(const std::string& vocalized, const Voice& voice,
                                   const std::function<bool()>& cancelled) {
  auto tokens = generate(vocalized, voice, cancelled);
  if (cancelled()) return {};
  return decode(tokens, voice);
}

bool Narrator::speakAll(const std::vector<std::string>& pieces, const Voice& voice,
                        const std::function<bool()>& cancelled,
                        const std::function<void(size_t, std::vector<float>)>& onAudio,
                        const std::function<void(size_t, size_t, double)>& onProgress) {
  std::mutex mu;
  std::condition_variable cv, watchdogCv;
  std::deque<std::pair<size_t, std::vector<int64_t>>> queue;
  bool finished = false, decoding = false;
  std::exception_ptr failure;
  const size_t kBatch = batchSize();
  const size_t decodeAtOnce = decodeBatched() ? kBatch : 1;
  std::thread decoderThread([&] {
    try {
      while (true) {
        // The pieces waiting, one as a rule (see decodeBatched), in one call.
        std::vector<size_t> ids;
        std::vector<std::vector<int64_t>> tokens;
        {
          std::unique_lock<std::mutex> lock(mu);
          cv.wait(lock, [&] { return !queue.empty() || finished; });
          if (queue.empty()) return;
          while (!queue.empty() && ids.size() < decodeAtOnce) {
            ids.push_back(queue.front().first);
            tokens.push_back(std::move(queue.front().second));
            queue.pop_front();
          }
        }
        cv.notify_all();
        if (cancelled()) continue;
        // A piece the model gave no tokens for (it stopped at once, or the
        // run of one token was all there was) becomes a short silence: the
        // decoder is not run on an empty sequence.
        for (auto& t : tokens)
          if (t.size() < 2) t.clear();
        if (timing()) {
          std::string dump;
          for (const auto& t : tokens) {
            dump += dump.empty() ? "" : " | ";
            for (auto id : t) dump += std::to_string(id) + ",";
          }
          platform::log("decoding: " + dump);
        }
        // The batch's pieces enter the queue in order, so the audio is too.
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::vector<float>> audio(tokens.size());
        {
          std::vector<std::vector<int64_t>> real;
          std::vector<size_t> at;
          size_t longest = 0;
          for (size_t k = 0; k < tokens.size(); k++)
            if (tokens[k].empty()) audio[k].assign(size_t(kSampleRate / 4), 0.0f);
            else {
              real.push_back(tokens[k]);
              at.push_back(k);
              longest = std::max(longest, tokens[k].size());
            }
          // A watchdog: the decoder takes about 40 ms a token; one sequence
          // was seen to loop inside the graph for ever. Past the limit the
          // run is terminated and the pieces become silence (logged, with
          // their tokens, so the case can be studied).
          decoding = true;
          std::thread watchdog([&, limit = std::chrono::milliseconds(20000 + 300 * (long long)longest * (long long)real.size())] {
            std::unique_lock<std::mutex> lock(mu);
            if (!watchdogCv.wait_for(lock, limit, [&] { return !decoding; })) {
              platform::log("decoder watchdog: terminating a run of " + std::to_string(real.size()) + " pieces after " +
                            std::to_string(limit.count() / 1000) + " s");
              terminateDecoder();
            }
          });
          std::vector<std::vector<float>> decoded;
          std::string problem;
          try {
            if (!real.empty()) decoded = decodeBatch(real, voice);
          } catch (const std::exception& e) {
            problem = e.what();
          }
          {
            std::lock_guard<std::mutex> lock(mu);
            decoding = false;
          }
          watchdogCv.notify_all();
          watchdog.join();
          resumeDecoder();
          if (!problem.empty()) {
            std::string dump;
            for (const auto& t : real) {
              dump += dump.empty() ? "" : " | ";
              for (auto id : t) dump += std::to_string(id) + ",";
            }
            platform::log("decoder failed (" + problem + "); silence instead. tokens: " + dump);
            decoded.assign(real.size(), std::vector<float>(size_t(kSampleRate / 4), 0.0f));
          }
          for (size_t k = 0; k < at.size(); k++) audio[at[k]] = std::move(decoded[k]);
        }
        if (timing()) {
          size_t n = 0, longest = 0;
          for (const auto& t : tokens) {
            n += t.size();
            longest = std::max(longest, t.size());
          }
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
          platform::log(ids.size() == 1 ? "decode " + std::to_string(n) + " tokens in " + std::to_string(ms.count()) + " ms"
                                        : "decode " + std::to_string(ids.size()) + " pieces, " + std::to_string(n) +
                                              " tokens (" + std::to_string(longest * ids.size()) + " padded) in " +
                                              std::to_string(ms.count()) + " ms");
        }
        for (size_t k = 0; k < ids.size(); k++) onAudio(ids[k], std::move(audio[k]));
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mu);
      failure = std::current_exception();
    }
  });
  try {
    size_t next = 0;
    for (size_t batch = std::min(kFirstBatch, kBatch); next < pieces.size() && !cancelled(); batch = kBatch) {
      const size_t count = std::min(batch, pieces.size() - next);
      // The batch's pieces are vocalized together, so that the diacritizer
      // sees the sentences around each one (pieces hold no line breaks).
      std::string joined;
      for (size_t k = 0; k < count; k++) joined += (k ? "\n" : "") + pieces[next + k];
      const auto t0 = std::chrono::steady_clock::now();
      std::vector<std::string> vocalized;
      {
        std::stringstream ss(vocalize(joined));
        std::string line;
        while (std::getline(ss, line)) vocalized.push_back(line);
      }
      if (vocalized.size() != count) {
        vocalized.clear();
        for (size_t k = 0; k < count; k++) vocalized.push_back(vocalize(pieces[next + k]));
      }
      const auto t1 = std::chrono::steady_clock::now();
      auto tokens = generateBatch(vocalized, voice, cancelled, [&](double fraction) {
        if (onProgress) onProgress(next, count, fraction);
      });
      if (tokens.size() != count) break;  // cancelled
      if (timing()) {
        size_t n = 0;
        for (const auto& t : tokens) n += t.size();
        const auto t2 = std::chrono::steady_clock::now();
        platform::log("batch of " + std::to_string(count) + ": nikud " +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()) +
                      " ms, " + std::to_string(n) + " tokens in " +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()) + " ms");
      }
      std::unique_lock<std::mutex> lock(mu);
      if (failure) break;
      // The decoder keeps up with about a batch; the generator waits rather
      // than running far ahead, so a cancel is felt soon.
      cv.wait(lock, [&] { return queue.size() <= kBatch || failure; });
      for (size_t k = 0; k < count; k++) queue.emplace_back(next + k, std::move(tokens[k]));
      next += count;
      cv.notify_all();
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(mu);
    if (!failure) failure = std::current_exception();
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    finished = true;
  }
  cv.notify_all();
  decoderThread.join();
  if (failure) std::rethrow_exception(failure);
  return !cancelled();
}

std::vector<int64_t> Narrator::generate(const std::string& vocalized, const Voice& voice,
                                        const std::function<bool()>& cancelled) {
  auto out = generateBatch({vocalized}, voice, cancelled);
  return out.empty() ? std::vector<int64_t>{} : std::move(out[0]);
}

std::vector<std::vector<int64_t>> Narrator::generateBatch(const std::vector<std::string>& vocalized,
                                                          const Voice& voice,
                                                          const std::function<bool()>& cancelled,
                                                          const std::function<void(double)>& onProgress) {
  auto& o = *impl_;
  const size_t rows = vocalized.size();
  if (rows == 0) return {};
  const int64_t batch = int64_t(rows);

  // Every row: the voice's conditioning, then its text. Rows are padded on
  // the right to the longest; the language model's attention takes each
  // row's length from the count of ones in its mask (seqlens_k), and later
  // tokens are written right after that row's real tokens, over the padding.
  std::vector<std::vector<int64_t>> ids(rows);
  std::vector<int64_t> rowLen(rows);
  std::vector<int> maxTokens(rows);
  int64_t maxLen = 0;
  int maxSteps = 0, expectedSteps = 1;  // the limit, and the likely length (for progress)
  for (size_t b = 0; b < rows; b++) {
    ids[b] = textTokens(vocalized[b]);
    rowLen[b] = voice.condLen + int64_t(ids[b].size());
    maxLen = std::max(maxLen, rowLen[b]);
    // A sentence is read at roughly 12 letters a second; text the model does
    // not understand can make it talk on past the sentence, so stop at about
    // twice the expected length (25 tokens a second).
    int letters = 0;
    for (char32_t c : toU32(vocalized[b]))
      if (!isNikud(c) && !isSpace(c) && !(c >= 0x0591 && c <= 0x05AF)) letters++;
    maxTokens[b] = std::min(kMaxSpeechTokens, 75 + letters * 4);
    maxSteps = std::max(maxSteps, maxTokens[b]);
    expectedSteps = std::max(expectedSteps, std::min(maxTokens[b], 25 + letters * 2));
  }

  std::vector<OrtValue*> past(2 * kLayers, nullptr), step, embedOut;
  struct Guard {
    Impl& o;
    std::vector<OrtValue*>& a;
    std::vector<OrtValue*>& b;
    std::vector<OrtValue*>& c;
    ~Guard() {
      o.release(a);
      o.release(b);
      o.release(c);
    }
  } guard{o, past, step, embedOut};
  for (auto& p : past) p = o.emptyTensor({batch, kKvHeads, 0, kHeadDim});

  // Embeds [count] tokens for each of [n] rows (the same count per row).
  auto embedTokens = [&](std::vector<int64_t>& tokenIds, std::vector<int64_t>& positions, int64_t n) {
    std::vector<float> exaggeration(size_t(n), kExaggerationValue);
    const std::vector<int64_t> shape{n, int64_t(tokenIds.size()) / n};
    std::vector<OrtValue*> in{
        o.tensor(tokenIds.data(), tokenIds.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
        o.tensor(positions.data(), positions.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
        o.tensor(exaggeration.data(), exaggeration.size(), {n}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)};
    o.release(embedOut);
    try {
      embedOut = o.run(o.embed, {"input_ids", "position_ids", "exaggeration"}, in, {"inputs_embeds"});
    } catch (...) {
      o.release(in);
      throw;
    }
    o.release(in);
    return o.dataOf<float>(embedOut[0]);
  };

  // First step: [batch, maxLen, hidden], zero past the row's end.
  std::vector<float> inputs(size_t(batch * maxLen) * kHidden, 0.0f);
  std::vector<int64_t> mask(size_t(batch * maxLen), 0);
  for (size_t b = 0; b < rows; b++) {
    float* row = inputs.data() + size_t(b) * size_t(maxLen) * kHidden;
    std::copy(voice.condEmb.begin(), voice.condEmb.end(), row);
    std::vector<int64_t> pos(ids[b].size());
    for (size_t i = 0; i < pos.size(); i++) pos[i] = ids[b][i] >= kStartSpeech ? 0 : int64_t(i) - 1;
    const float* text = embedTokens(ids[b], pos, 1);
    std::copy(text, text + ids[b].size() * kHidden, row + size_t(voice.condLen) * kHidden);
    std::fill(mask.begin() + long(b * size_t(maxLen)), mask.begin() + long(b * size_t(maxLen) + size_t(rowLen[b])), 1);
  }
  int64_t inLen = maxLen;
  int64_t maskLen = maxLen;

  std::vector<std::vector<int64_t>> generated(rows, std::vector<int64_t>{kStartSpeech});
  std::vector<bool> stopped(rows, false), done(rows, false);
  size_t remaining = rows;

  for (int i = 0; i < maxSteps && remaining > 0; i++) {
    if (cancelled()) return {};
    std::vector<OrtValue*> in;
    in.push_back(o.tensor(inputs.data(), inputs.size(), {batch, inLen, kHidden}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));
    in.push_back(o.tensor(mask.data(), mask.size(), {batch, maskLen}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64));
    for (auto* p : past) in.push_back(p);
    o.release(step);
    try {
      step = o.run(o.lm, o.lmInputNames, in, o.lmOutputNames);
    } catch (...) {
      o.api->ReleaseValue(in[0]);
      o.api->ReleaseValue(in[1]);
      throw;
    }
    o.api->ReleaseValue(in[0]);
    o.api->ReleaseValue(in[1]);
    // The new cache replaces the old one; the logits stay in step[0].
    for (int k = 0; k < 2 * kLayers; k++) {
      o.api->ReleaseValue(past[k]);
      past[k] = step[k + 1];
      step[k + 1] = nullptr;
    }
    const auto shape = o.shapeOf(step[0]);
    const int64_t vocab = shape[2];
    const float* allLogits = o.dataOf<float>(step[0]);

    // One new token a row: the logits of the row's last real position.
    std::vector<int64_t> nextIds(rows, kStartSpeech), nextPos(rows, int64_t(i) + 1);
    std::vector<int64_t> newMask(size_t(batch) * size_t(maskLen + 1), 0);
    for (size_t b = 0; b < rows; b++) {
      // The row's mask so far, then 1 for a row still going (its new token).
      std::copy(mask.begin() + long(b * size_t(maskLen)), mask.begin() + long((b + 1) * size_t(maskLen)),
                newMask.begin() + long(b * size_t(maskLen + 1)));
      if (done[b]) continue;
      auto& gen = generated[b];
      const int64_t at = i == 0 ? rowLen[b] - 1 : 0;
      std::vector<float> logits(allLogits + (int64_t(b) * shape[1] + at) * vocab,
                                allLogits + (int64_t(b) * shape[1] + at + 1) * vocab);
      std::vector<int64_t> seen(gen);
      std::sort(seen.begin(), seen.end());
      seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
      for (int64_t t : seen) {
        float& l = logits[size_t(t)];
        l = l < 0 ? l * kRepetitionPenalty : l / kRepetitionPenalty;
      }
      const int64_t next = int64_t(std::max_element(logits.begin(), logits.end()) - logits.begin());
      gen.push_back(next);
      if (next == kStopSpeech) {
        stopped[b] = true;
        done[b] = true;
      } else {
        // Stuck on one token (silence, as a rule) for two seconds: the
        // sentence is over or lost; drop the run and end here.
        constexpr size_t kMaxRun = 50;
        if (gen.size() > kMaxRun &&
            std::all_of(gen.end() - kMaxRun, gen.end(), [&](int64_t t) { return t == next; })) {
          gen.erase(gen.end() - kMaxRun, gen.end());
          done[b] = true;
        } else if (int(gen.size()) - 1 >= maxTokens[b]) {
          done[b] = true;
        }
      }
      if (done[b]) {
        remaining--;
        continue;
      }
      nextIds[b] = next;
      newMask[b * size_t(maskLen + 1) + size_t(maskLen)] = 1;
    }
    if (onProgress) onProgress(std::min(1.0, double(i + 1) / expectedSteps));
    if (remaining == 0) break;
    // A finished row keeps getting a token so the batch stays rectangular;
    // its mask stops growing and its result is already taken.
    const float* e = embedTokens(nextIds, nextPos, batch);
    inputs.assign(e, e + size_t(batch) * kHidden);
    inLen = 1;
    mask.swap(newMask);
    maskLen++;
  }

  // The new speech tokens, without start and stop. A run of one token
  // longer than half a second is cut to that: such runs are silence or a
  // held sound the model got stuck on, and one of them (54 tokens, mostly
  // one id) hung the decoder inside a kernel for ever.
  constexpr size_t kMaxSameRun = 12;
  std::vector<std::vector<int64_t>> out(rows);
  for (size_t b = 0; b < rows; b++) {
    size_t run = 0;
    for (auto it = generated[b].begin() + 1; it != generated[b].end() - (stopped[b] ? 1 : 0); ++it) {
      run = (!out[b].empty() && out[b].back() == *it) ? run + 1 : 1;
      if (run <= kMaxSameRun) out[b].push_back(*it);
    }
  }
  return out;
}

void Narrator::terminateDecoder() { impl_->api->RunOptionsSetTerminate(impl_->decoderRun); }
void Narrator::resumeDecoder() { impl_->api->RunOptionsUnsetTerminate(impl_->decoderRun); }

std::vector<float> Narrator::decode(const std::vector<int64_t>& tokens, const Voice& voice) {
  auto& o = *impl_;
  // The recording's tokens first: the decoder continues its voice.
  std::vector<int64_t> speech(voice.promptTokens);
  speech.insert(speech.end(), tokens.begin(), tokens.end());
  std::vector<float> xVector(voice.xVector), feat(voice.promptFeat);
  std::vector<OrtValue*> in{
      o.tensor(speech.data(), speech.size(), {1, int64_t(speech.size())}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
      o.tensor(xVector.data(), xVector.size(), {1, 192}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT),
      o.tensor(feat.data(), feat.size(), {1, voice.promptFrames, 80}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)};
  std::vector<OrtValue*> wav;
  try {
    wav = o.run(o.decoder, {"speech_tokens", "speaker_embeddings", "speaker_features"}, in, {"waveform"});
  } catch (...) {
    o.release(in);
    throw;
  }
  o.release(in);
  const auto shape = o.shapeOf(wav[0]);
  const float* w = o.dataOf<float>(wav[0]);
  std::vector<float> out(w, w + shape.back());
  o.release(wav);
  return out;
}

std::vector<std::vector<float>> Narrator::decodeBatch(const std::vector<std::vector<int64_t>>& pieces,
                                                      const Voice& voice) {
  auto& o = *impl_;
  const size_t rows = pieces.size();
  if (rows == 0) return {};
  if (rows == 1) return {decode(pieces[0], voice)};

  // Every row: the recording's tokens, the piece, then the piece's last
  // token again up to the longest row. The decoder is not causal, so what
  // follows a token colours its audio a little; a repeat of the last token
  // (silence, as a rule, at the end of a sentence) is the most natural
  // continuation, and the rows' last tokens came out as quiet as when
  // decoded alone.
  size_t longest = 0;
  for (const auto& p : pieces) longest = std::max(longest, p.size());
  const size_t len = longest;
  const size_t promptLen = voice.promptTokens.size();
  const size_t width = promptLen + len;
  std::vector<int64_t> speech(rows * width);
  std::vector<float> xVector, feat;
  xVector.reserve(rows * voice.xVector.size());
  feat.reserve(rows * voice.promptFeat.size());
  for (size_t b = 0; b < rows; b++) {
    int64_t* row = speech.data() + b * width;
    std::copy(voice.promptTokens.begin(), voice.promptTokens.end(), row);
    std::copy(pieces[b].begin(), pieces[b].end(), row + promptLen);
    const int64_t pad = pieces[b].empty() ? voice.promptTokens.back() : pieces[b].back();
    std::fill(row + promptLen + pieces[b].size(), row + width, pad);
    xVector.insert(xVector.end(), voice.xVector.begin(), voice.xVector.end());
    feat.insert(feat.end(), voice.promptFeat.begin(), voice.promptFeat.end());
  }
  const int64_t batch = int64_t(rows);
  std::vector<OrtValue*> in{
      o.tensor(speech.data(), speech.size(), {batch, int64_t(width)}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
      o.tensor(xVector.data(), xVector.size(), {batch, 192}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT),
      o.tensor(feat.data(), feat.size(), {batch, voice.promptFrames, 80}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)};
  std::vector<OrtValue*> wav;
  try {
    wav = o.run(o.decoder, {"speech_tokens", "speaker_embeddings", "speaker_features"}, in, {"waveform"});
  } catch (...) {
    o.release(in);
    throw;
  }
  o.release(in);
  const auto shape = o.shapeOf(wav[0]);
  const size_t samples = size_t(shape.back());
  const float* w = o.dataOf<float>(wav[0]);
  // Each row's audio is its own tokens' share of the row (960 samples a
  // token: 24 kHz at 25 tokens a second).
  const size_t perToken = len ? samples / len : 0;
  if (perToken * len != samples)
    platform::log("decode: " + std::to_string(samples) + " samples for " + std::to_string(len) +
                  " tokens a row is not a whole number a token");
  std::vector<std::vector<float>> out(rows);
  for (size_t b = 0; b < rows; b++) {
    const float* row = w + b * samples;
    out[b].assign(row, row + std::min(samples, perToken * pieces[b].size()));
  }
  o.release(wav);
  return out;
}

}  // namespace narrator
