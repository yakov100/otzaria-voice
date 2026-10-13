#include "narrator.h"

#include <algorithm>
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

#include "../third_party/onnxruntime_c_api.h"
#include "json.h"

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

// The Phonikud/Dicta output classes (BertForDiacritization).
const char32_t* const kNikudClasses[] = {
    U"", U"<MAT_LECT>", U"ּ", U"ְ", U"ֱ", U"ֲ", U"ֳ", U"ִ",
    U"ֵ", U"ֶ", U"ַ", U"ָ", U"ֹ", U"ֺ", U"ֻ",
    U"ְּ", U"ֱּ", U"ֲּ", U"ֳּ", U"ִּ",
    U"ֵּ", U"ֶּ", U"ַּ", U"ָּ", U"ֹּ",
    U"ֺּ", U"ֻּ", U"ׇ", U"ׇּ"};
constexpr int kNikudClassCount = 29;

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

std::vector<std::string> installedVoices() {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(voicesDir(), ec))
    if (e.path().extension() == ".otzvoice") out.push_back(platform::toUtf8(e.path().stem()));
  std::sort(out.begin(), out.end());
  return out;
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
  std::vector<std::string> lmInputNames, lmOutputNames;

  std::unordered_map<std::u32string, int64_t> nakdanVocab;
  int64_t nakdanCls = 0, nakdanSep = 0, nakdanUnk = 0;
  std::unordered_map<std::u32string, int64_t> textVocab;
  std::map<std::pair<std::u32string, std::u32string>, int> merges;  // pair -> rank

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
    check(api->Run(s, nullptr, ip.data(), in.data(), in.size(), op.data(), op.size(), out.data()));
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

std::string Narrator::vocalize(const std::string& text) {
  std::u32string s = toU32(text);
  if (std::any_of(s.begin(), s.end(), isNikud)) return text;
  // The model sees the letters only.
  std::u32string plain;
  for (char32_t c : s)
    if (!(c >= 0x0590 && c <= 0x05C7) && c != '|') plain.push_back(c);
  if (plain.size() > 2046) plain.resize(2046);
  auto& o = *impl_;
  std::vector<int64_t> ids = nakdanTokens(plain);
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
  out = o.run(o.nakdan, {"input_ids", "attention_mask", "token_type_ids"}, in, {"nikud_logits", "shin_logits"});
  const float* nikud = o.dataOf<float>(out[0]);
  const float* shin = o.dataOf<float>(out[1]);

  std::u32string result;
  for (size_t i = 0; i < plain.size(); i++) {
    const char32_t c = plain[i];
    result.push_back(c);
    if (!isHebrewLetter(c)) continue;
    const size_t tok = i + 1;  // after [CLS]
    const float* row = nikud + tok * kNikudClassCount;
    const int cls = int(std::max_element(row, row + kNikudClassCount) - row);
    if (c == U'ש') result.push_back(shin[tok * 2 + 1] > shin[tok * 2] ? 0x05C2 : 0x05C1);
    if (cls == 1) continue;  // mater lectionis: the letter is read as a vowel, no mark
    result += kNikudClasses[cls];
  }
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
                        const std::function<void(size_t, std::vector<float>)>& onAudio) {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::pair<size_t, std::vector<int64_t>>> queue;
  bool finished = false;
  std::exception_ptr failure;
  std::thread decoderThread([&] {
    try {
      while (true) {
        std::pair<size_t, std::vector<int64_t>> job;
        {
          std::unique_lock<std::mutex> lock(mu);
          cv.wait(lock, [&] { return !queue.empty() || finished; });
          if (queue.empty()) return;
          job = std::move(queue.front());
          queue.pop_front();
        }
        cv.notify_all();
        if (cancelled()) continue;
        onAudio(job.first, decode(job.second, voice));
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mu);
      failure = std::current_exception();
    }
  });
  try {
    for (size_t i = 0; i < pieces.size() && !cancelled(); i++) {
      auto tokens = generate(vocalize(pieces[i]), voice, cancelled);
      std::unique_lock<std::mutex> lock(mu);
      if (failure) break;
      // At most one piece waits: the generator stays a piece ahead, no more.
      cv.wait(lock, [&] { return queue.empty() || failure; });
      queue.emplace_back(i, std::move(tokens));
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
  auto& o = *impl_;
  std::vector<int64_t> ids = textTokens(vocalized);
  const int64_t textLen = int64_t(ids.size());
  std::vector<int64_t> pos(ids.size());
  for (size_t i = 0; i < ids.size(); i++) pos[i] = ids[i] >= kStartSpeech ? 0 : int64_t(i) - 1;
  float exaggeration = kExaggerationValue;

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
  for (auto& p : past) p = o.emptyTensor({1, kKvHeads, 0, kHeadDim});

  auto embedTokens = [&](std::vector<int64_t>& tokenIds, std::vector<int64_t>& positions) {
    const std::vector<int64_t> shape{1, int64_t(tokenIds.size())};
    std::vector<OrtValue*> in{
        o.tensor(tokenIds.data(), tokenIds.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
        o.tensor(positions.data(), positions.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
        o.tensor(&exaggeration, 1, {1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)};
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

  // First step: the voice's conditioning, then the text.
  std::vector<float> inputs(voice.condEmb);
  {
    const float* text = embedTokens(ids, pos);
    inputs.insert(inputs.end(), text, text + size_t(textLen) * kHidden);
  }
  int64_t seqLen = voice.condLen + textLen;
  std::vector<int64_t> mask(size_t(seqLen), 1);
  std::vector<int64_t> generated{kStartSpeech};
  bool stopped = false;

  // A sentence is read at roughly 12 letters a second; text the model does
  // not understand can make it talk on past the sentence, so stop at about
  // twice the expected length (25 tokens a second).
  int letters = 0;
  for (char32_t c : toU32(vocalized))
    if (!isNikud(c) && !isSpace(c) && !(c >= 0x0591 && c <= 0x05AF)) letters++;
  const int maxTokens = std::min(kMaxSpeechTokens, 75 + letters * 4);

  for (int i = 0; i < maxTokens; i++) {
    if (cancelled()) return {};
    const int64_t inLen = int64_t(inputs.size() / kHidden);
    std::vector<OrtValue*> in;
    in.push_back(o.tensor(inputs.data(), inputs.size(), {1, inLen, kHidden}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));
    in.push_back(o.tensor(mask.data(), mask.size(), {1, int64_t(mask.size())}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64));
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
    std::vector<float> logits(o.dataOf<float>(step[0]) + (shape[1] - 1) * vocab,
                              o.dataOf<float>(step[0]) + shape[1] * vocab);
    std::vector<int64_t> seen(generated);
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    for (int64_t t : seen) {
      float& l = logits[size_t(t)];
      l = l < 0 ? l * kRepetitionPenalty : l / kRepetitionPenalty;
    }
    const int64_t next = int64_t(std::max_element(logits.begin(), logits.end()) - logits.begin());
    generated.push_back(next);
    if (next == kStopSpeech) {
      stopped = true;
      break;
    }
    // Stuck on one token (silence, as a rule) for two seconds: the sentence
    // is over or lost; drop the run and end here.
    constexpr size_t kMaxRun = 50;
    if (generated.size() > kMaxRun &&
        std::all_of(generated.end() - kMaxRun, generated.end(), [&](int64_t t) { return t == next; })) {
      generated.erase(generated.end() - kMaxRun, generated.end());
      break;
    }
    std::vector<int64_t> nextIds{next}, nextPos{int64_t(i) + 1};
    const float* e = embedTokens(nextIds, nextPos);
    inputs.assign(e, e + kHidden);
    mask.push_back(1);
  }

  // The new speech tokens, without start and stop.
  return std::vector<int64_t>(generated.begin() + 1, generated.end() - (stopped ? 1 : 0));
}

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

}  // namespace narrator
