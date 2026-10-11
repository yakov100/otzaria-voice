#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "../third_party/onnxruntime_c_api.h"
#include "json.h"
#include "platform.h"
#include "util.h"

namespace voice {

// ---------------------------------------------------------------- features

Features::Features(int sampleRate, int nFft, int hop, int nMels, double fMin, double fMax)
    : sampleRate_(sampleRate), nFft_(nFft), hop_(hop), nMels_(nMels) {
  const double pi = 3.14159265358979323846;
  const int bins = nFft / 2 + 1;
  window_.resize(nFft);
  for (int i = 0; i < nFft; i++) window_[i] = 0.5 - 0.5 * std::cos(2 * pi * i / nFft);
  cos_.resize(size_t(bins) * nFft);
  sin_.resize(size_t(bins) * nFft);
  for (int k = 0; k < bins; k++) {
    for (int i = 0; i < nFft; i++) {
      const double a = 2 * pi * double((k * i) % nFft) / nFft;
      cos_[size_t(k) * nFft + i] = std::cos(a);
      sin_[size_t(k) * nFft + i] = std::sin(a);
    }
  }
  auto hzToMel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
  auto melToHz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
  const double lo = hzToMel(fMin), hi = hzToMel(fMax);
  std::vector<double> pts(nMels + 2);
  for (int i = 0; i < nMels + 2; i++) pts[i] = melToHz(lo + (hi - lo) * i / (nMels + 1));
  filters_.assign(size_t(bins) * nMels, 0.0);
  for (int k = 0; k < bins; k++) {
    const double freq = k * (sampleRate / 2.0) / (bins - 1);
    for (int m = 0; m < nMels; m++) {
      const double up = (freq - pts[m]) / (pts[m + 1] - pts[m]);
      const double down = (pts[m + 2] - freq) / (pts[m + 2] - pts[m + 1]);
      filters_[size_t(k) * nMels + m] = std::max(0.0, std::min(up, down));
    }
  }
}

int Features::frames(size_t samples) const {
  return samples < size_t(nFft_) ? 0 : int(1 + (samples - nFft_) / hop_);
}

std::vector<float> Features::compute(const float* audio, size_t n) const {
  const int t = frames(n);
  const int bins = nFft_ / 2 + 1;
  std::vector<float> out(size_t(nMels_) * t);
  std::vector<double> frame(nFft_), power(bins);
  for (int f = 0; f < t; f++) {
    const size_t start = size_t(f) * hop_;
    for (int i = 0; i < nFft_; i++) frame[i] = audio[start + i] * window_[i];
    for (int k = 0; k < bins; k++) {
      double re = 0, im = 0;
      const double* c = &cos_[size_t(k) * nFft_];
      const double* s = &sin_[size_t(k) * nFft_];
      for (int i = 0; i < nFft_; i++) {
        re += frame[i] * c[i];
        im -= frame[i] * s[i];
      }
      power[k] = re * re + im * im;
    }
    for (int m = 0; m < nMels_; m++) {
      double sum = 0;
      for (int k = 0; k < bins; k++) sum += power[k] * filters_[size_t(k) * nMels_ + m];
      out[size_t(m) * t + f] = float(std::log(std::clamp(sum, 1e-9, 1e9)));
    }
  }
  return out;
}

// ---------------------------------------------------------------- ONNX Runtime

struct OrtState {
  std::unique_ptr<platform::DynamicLibrary> dll;
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtSession* session = nullptr;
  OrtMemoryInfo* memory = nullptr;

  void check(OrtStatus* s) {
    if (!s) return;
    std::string msg = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    throw std::runtime_error("ONNX Runtime: " + msg);
  }

  ~OrtState() {
    if (api) {
      if (memory) api->ReleaseMemoryInfo(memory);
      if (session) api->ReleaseSession(session);
      if (env) api->ReleaseEnv(env);
    }
  }
};

static std::string readFile(const Path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("missing file: " + platform::toUtf8(path));
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

CtcModel::CtcModel(const Path& library, const Path& modelDir, int threads)
    : ort_(std::make_unique<OrtState>()) {
  auto& o = *ort_;
  o.dll = std::make_unique<platform::DynamicLibrary>(library);
  using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
  auto getBase = reinterpret_cast<GetApiBase>(o.dll->symbol("OrtGetApiBase"));
  if (!getBase) throw std::runtime_error("OrtGetApiBase missing");
  // The OrtApi table only grows, so an older runtime serves the calls used here.
  for (uint32_t v = ORT_API_VERSION; v >= 11 && !o.api; v--) o.api = getBase()->GetApi(v);
  if (!o.api) throw std::runtime_error("ONNX Runtime too old");
  auto* api = o.api;

  o.check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "otzaria-voice", &o.env));
  OrtSessionOptions* opts = nullptr;
  o.check(api->CreateSessionOptions(&opts));
  struct OptsGuard {
    const OrtApi* api;
    OrtSessionOptions* p;
    ~OptsGuard() { api->ReleaseSessionOptions(p); }
  } guard{api, opts};
  o.check(api->SetIntraOpNumThreads(opts, threads));
  o.check(api->SetInterOpNumThreads(opts, 1));
  o.check(api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL));
  // Threads sleep instead of spinning between updates: idle CPU stays at zero.
  o.check(api->AddSessionConfigEntry(opts, "session.intra_op.allow_spinning", "0"));
  const Path model = modelDir / "model.int8.onnx";
  o.check(api->CreateSession(o.env, model.c_str(), opts, &o.session));
  o.check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &o.memory));

  const auto pre = json::parse(readFile(modelDir / "preprocessor.json"));
  const double fMax = pre.has("f_max") && !pre["f_max"].isNull()
                          ? pre["f_max"].num()
                          : pre["sample_rate"].num() / 2;
  features_ = std::make_unique<Features>(int(pre["sample_rate"].num()), int(pre["n_fft"].num()),
                                         int(pre["hop_length"].num()), int(pre["n_mels"].num()),
                                         pre["f_min"].num(), fMax);
  blank_ = int(pre["blank_id"].num());
  // Keep the parsed document alive: arr() refers into it.
  const auto vocab = json::parse(readFile(modelDir / "vocab.json"));
  for (const auto& c : vocab.arr()) vocab_.push_back(c.str());
  // The blank comes after the characters; its slot makes the list one row wide.
  vocab_.push_back("");
}

CtcModel::~CtcModel() = default;

CtcOutput CtcModel::run(const float* audio, size_t n) {
  CtcOutput r;
  r.vocab = int(vocab_.size());
  const int frames = features_->frames(n);
  if (frames < 4) return r;
  auto& o = *ort_;
  auto* api = o.api;
  std::vector<float> feats = features_->compute(audio, n);
  int64_t featShape[3] = {1, features_->nMels(), frames};
  int64_t len = frames;
  int64_t lenShape[1] = {1};
  OrtValue* in[2] = {nullptr, nullptr};
  OrtValue* out = nullptr;
  struct Guard {
    const OrtApi* api;
    OrtValue** in;
    OrtValue** out;
    ~Guard() {
      if (in[0]) api->ReleaseValue(in[0]);
      if (in[1]) api->ReleaseValue(in[1]);
      if (*out) api->ReleaseValue(*out);
    }
  } guard{api, in, &out};
  o.check(api->CreateTensorWithDataAsOrtValue(o.memory, feats.data(), feats.size() * sizeof(float),
                                              featShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                              &in[0]));
  o.check(api->CreateTensorWithDataAsOrtValue(o.memory, &len, sizeof(len), lenShape, 1,
                                              ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[1]));
  const char* inNames[2] = {"features", "feature_lengths"};
  const char* outNames[1] = {"log_probs"};
  o.check(api->Run(o.session, nullptr, inNames, in, 2, outNames, 1, &out));

  OrtTensorTypeAndShapeInfo* info = nullptr;
  o.check(api->GetTensorTypeAndShape(out, &info));
  size_t dims = 0;
  int64_t shape[3] = {0, 0, 0};
  OrtStatus* s = api->GetDimensionsCount(info, &dims);
  if (!s && dims == 3) s = api->GetDimensions(info, shape, 3);
  api->ReleaseTensorTypeAndShapeInfo(info);
  o.check(s);
  if (dims != 3) throw std::runtime_error("unexpected model output");
  float* data = nullptr;
  o.check(api->GetTensorMutableData(out, reinterpret_cast<void**>(&data)));
  r.frames = int(shape[1]);
  r.vocab = int(shape[2]);
  r.logProbs.assign(data, data + size_t(r.frames) * r.vocab);
  for (int t = 0; t < r.frames; t++) {
    float* row = &r.logProbs[size_t(t) * r.vocab];
    double mx = -std::numeric_limits<double>::infinity();
    for (int c = 0; c < r.vocab; c++) mx = std::max(mx, double(row[c]));
    double sum = 0;
    for (int c = 0; c < r.vocab; c++) sum += std::exp(row[c] - mx);
    const double lse = mx + std::log(sum);
    for (int c = 0; c < r.vocab; c++) row[c] = float(row[c] - lse);
  }
  return r;
}

// ---------------------------------------------------------------- language model

struct MappedLm : platform::MappedFile {
  using platform::MappedFile::MappedFile;
};

NgramLm::NgramLm(const Path& path) {
  file_ = std::make_unique<MappedLm>(path);
  base_ = file_->data();
  const uint64_t fileSize = file_->size();
  static const uint8_t magic[8] = {'O', 'T', 'Z', 'L', 'M', 0, 1, 0};
  if (fileSize < 40 || std::memcmp(base_, magic, 8) != 0)
    throw std::runtime_error("not an otzlm file");
  auto u32at = [&](size_t off) {
    uint32_t v;
    std::memcpy(&v, base_ + off, 4);
    return v;
  };
  auto f32at = [&](size_t off) {
    float v;
    std::memcpy(&v, base_ + off, 4);
    return v;
  };
  v_ = u32at(8);
  nb_ = u32at(12);
  nt_ = u32at(16);
  const uint32_t wb = u32at(20), db = u32at(24);
  step_ = f32at(28);
  backoff_ = f32at(32);
  unk_ = f32at(36);
  size_t o = 40;
  auto align = [&] { o = (o + 3) & ~size_t(3); };
  wOff_ = reinterpret_cast<const uint32_t*>(base_ + o);
  o += size_t(v_ + 1) * 4;
  wRaw_ = reinterpret_cast<const char*>(base_ + o);
  o += wb;
  align();
  dOff_ = reinterpret_cast<const uint32_t*>(base_ + o);
  o += size_t(v_ + 1) * 4;
  dRaw_ = reinterpret_cast<const char*>(base_ + o);
  o += db;
  align();
  uniQ_ = base_ + o;
  o += v_;
  align();
  biRow_ = reinterpret_cast<const uint32_t*>(base_ + o);
  o += size_t(v_ + 1) * 4;
  biCol_ = reinterpret_cast<const uint32_t*>(base_ + o);
  o += size_t(nb_) * 4;
  biQ_ = base_ + o;
  o += nb_;
  align();
  triRow_ = reinterpret_cast<const uint32_t*>(base_ + o);
  o += size_t(nb_ + 1) * 4;
  triCol_ = reinterpret_cast<const uint32_t*>(base_ + o);
  o += size_t(nt_) * 4;
  triQ_ = base_ + o;
  o += nt_;
  if (o > fileSize) throw std::runtime_error("truncated language model");
}

NgramLm::~NgramLm() = default;

std::string_view NgramLm::word(uint32_t i) const {
  return std::string_view(wRaw_ + wOff_[i], wOff_[i + 1] - wOff_[i]);
}

// Words are sorted by code point; for UTF-8 that is plain byte order.
int NgramLm::idOf(const std::string& w) const {
  uint32_t lo = 0, hi = v_;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (word(mid) < std::string_view(w)) lo = mid + 1;
    else hi = mid;
  }
  return lo < v_ && word(lo) == w ? int(lo) : -1;
}

bool NgramLm::isPrefix(const std::string& prefix) const {
  uint32_t lo = 0, hi = v_;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (word(mid) < std::string_view(prefix)) lo = mid + 1;
    else hi = mid;
  }
  return lo < v_ && word(lo).substr(0, prefix.size()) == prefix;
}

std::string NgramLm::displayOf(int id) const {
  if (id < 0) return "";
  return std::string(dRaw_ + dOff_[id], dOff_[id + 1] - dOff_[id]);
}

int64_t NgramLm::find(const uint32_t* rows, const uint32_t* cols, uint32_t row, uint32_t key) {
  uint32_t lo = rows[row], hi = rows[row + 1];
  const uint32_t end = hi;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (cols[mid] < key) lo = mid + 1;
    else hi = mid;
  }
  return lo < end && cols[lo] == key ? int64_t(lo) : -1;
}

double NgramLm::score(int p2, int p1, int w) const {
  if (w < 0) return unk_;
  double penalty = 0;
  if (p1 >= 0 && p2 >= 0) {
    const int64_t pair = find(biRow_, biCol_, uint32_t(p2), uint32_t(p1));
    if (pair >= 0) {
      const int64_t k = find(triRow_, triCol_, uint32_t(pair), uint32_t(w));
      if (k >= 0) return -double(step_) * triQ_[k];
    }
    penalty += backoff_;
  }
  if (p1 >= 0) {
    const int64_t k = find(biRow_, biCol_, uint32_t(p1), uint32_t(w));
    if (k >= 0 && biQ_[k] != 255) return penalty - double(step_) * biQ_[k];
    penalty += backoff_;
  }
  return penalty - double(step_) * uniQ_[w];
}

// ---------------------------------------------------------------- decoder

static const char* const kFinals[][2] = {
    {"\xD7\x9A", "\xD7\x9B"},  // ך -> כ
    {"\xD7\x9D", "\xD7\x9E"},  // ם -> מ
    {"\xD7\x9F", "\xD7\xA0"},  // ן -> נ
    {"\xD7\xA3", "\xD7\xA4"},  // ף -> פ
    {"\xD7\xA5", "\xD7\xA6"},  // ץ -> צ
};

static std::string normChar(const std::string& ch) {
  for (const auto& f : kFinals)
    if (ch == f[0]) return f[1];
  return ch;
}

std::string normalizeLmWord(const std::string& w) {
  std::string out;
  for (const auto& ch : util::utf8Chars(w)) out += normChar(ch);
  return out;
}

static std::string restoreFinal(const std::string& w) {
  auto chars = util::utf8Chars(w);
  if (chars.size() < 2) return w;
  for (const auto& f : kFinals) {
    if (chars.back() == f[1]) {
      chars.back() = f[0];
      std::string out;
      for (const auto& c : chars) out += c;
      return out;
    }
  }
  return w;
}

static inline double logAddExp(double a, double b) {
  const double ninf = -std::numeric_limits<double>::infinity();
  if (a == ninf) return b;
  if (b == ninf) return a;
  const double m = std::max(a, b);
  return m + std::log(std::exp(a - m) + std::exp(b - m));
}

PhraseSet::PhraseSet(const Path& path, const NgramLm& lm) {
  std::ifstream f(path, std::ios::binary);
  std::string line;
  auto key2 = [](int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
  auto key3 = [](int a, int b, int c) {
    return (uint64_t(uint32_t(a)) << 42) ^ (uint64_t(uint32_t(b)) << 21) ^ uint64_t(uint32_t(c));
  };
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    line = line.substr(0, line.find('\t'));
    std::vector<int> ids;
    bool known = true;
    for (const auto& w : util::split(line, ' ')) {
      if (w.empty()) continue;
      const int id = lm.idOf(normalizeLmWord(w));
      if (id < 0) known = false;
      ids.push_back(id);
    }
    if (!known || ids.size() < 2) continue;
    for (size_t i = 1; i < ids.size(); i++) pairs_.insert(key2(ids[i - 1], ids[i]));
    for (size_t i = 2; i < ids.size(); i++) triples_.insert(key3(ids[i - 2], ids[i - 1], ids[i]));
    count_++;
  }
}

bool PhraseSet::pair(int a, int b) const {
  return a >= 0 && b >= 0 && pairs_.count((uint64_t(uint32_t(a)) << 32) | uint32_t(b));
}

bool PhraseSet::triple(int a, int b, int c) const {
  return a >= 0 && b >= 0 && c >= 0 &&
         triples_.count((uint64_t(uint32_t(a)) << 42) ^ (uint64_t(uint32_t(b)) << 21) ^
                        uint64_t(uint32_t(c)));
}

CtcDecoder::CtcDecoder(const std::vector<std::string>& vocab, int blank, const NgramLm& lm,
                       const PhraseSet* phrases, LmWeights weights)
    : vocab_(vocab), blank_(blank), space_(-1), lm_(lm), phrases_(phrases), w_(weights) {
  for (size_t i = 0; i < vocab_.size(); i++) {
    vocabNorm_.push_back(normChar(vocab_[i]));
    latin_.push_back(vocab_[i].size() == 1 && vocab_[i][0] >= 'a' && vocab_[i][0] <= 'z');
    if (vocab_[i] == " ") space_ = int(i);
  }
}

namespace {
struct Beam {
  std::string text;
  int prev2 = -1, prev1 = -1;  // LM ids; -1 = none or out of vocabulary
  std::string partialNorm;     // current word, normalized
  int last = -1;               // last character id of text
  double pb = -std::numeric_limits<double>::infinity();
  double pnb = -std::numeric_limits<double>::infinity();
  double lm = 0;
  bool hasLm = false;
  void mergeLm(double v) {
    lm = hasLm ? std::max(lm, v) : v;
    hasLm = true;
  }
  double total() const { return logAddExp(pb, pnb) + lm; }
};
}  // namespace

std::string CtcDecoder::decode(const CtcOutput& out, int beamWidth,
                               const std::set<std::string>& hotwords,
                               const std::vector<std::string>& context) const {
  const int v = out.vocab;
  const double ln10 = std::log(10.0);
  auto wordScore = [&](int p2, int p1, const std::string& word) {
    if (hotwords.count(word)) return w_.beta + w_.hotword;
    const int id = lm_.idOf(word);
    if (id < 0) return w_.beta + w_.oov;
    double bonus = 0;
    if (phrases_ && phrases_->pair(p1, id)) {
      bonus += w_.phrasePair;
      if (phrases_->triple(p2, p1, id)) bonus += w_.phraseTriple;
    }
    return w_.alpha * lm_.score(p2, p1, id) * ln10 + w_.beta + bonus;
  };

  // The text alone identifies a beam: context, previous words and the open word
  // all follow from it.
  std::unordered_map<std::string, Beam> beams;
  {
    Beam s;
    s.prev2 = context.size() > 1 ? lm_.idOf(context[context.size() - 2]) : -1;
    s.prev1 = context.empty() ? -1 : lm_.idOf(context.back());
    s.pb = 0;
    s.mergeLm(0);
    beams.emplace("", s);
  }
  for (int t = 0; t < out.frames; t++) {
    const float* row = &out.logProbs[size_t(t) * v];
    std::unordered_map<std::string, Beam> next;
    next.reserve(beams.size() * 8);
    auto at = [&](const std::string& text, const Beam& from) -> Beam& {
      auto it = next.find(text);
      if (it != next.end()) return it->second;
      Beam b;
      b.text = text;
      b.prev2 = from.prev2;
      b.prev1 = from.prev1;
      b.partialNorm = from.partialNorm;
      b.last = from.last;
      return next.emplace(text, std::move(b)).first->second;
    };
    for (const auto& [key, b] : beams) {
      const double pAll = logAddExp(b.pb, b.pnb);
      for (int c = 0; c < v; c++) {
        const double p = row[c];
        if (p <= w_.prune || (size_t(c) < latin_.size() && latin_[c])) continue;
        if (c == blank_) {
          Beam& e = at(b.text, b);
          e.pb = logAddExp(e.pb, pAll + p);
          e.mergeLm(b.lm);
          continue;
        }
        if (c == space_) {
          if (b.partialNorm.empty()) {
            Beam& e = at(b.text, b);
            e.pnb = logAddExp(e.pnb, pAll + p);
            e.mergeLm(b.lm);
            continue;
          }
          const std::string text = b.text + " ";
          const bool fresh = next.find(text) == next.end();
          Beam& e = at(text, b);
          if (fresh) {
            e.prev2 = b.prev1;
            e.prev1 = lm_.idOf(b.partialNorm);
            e.partialNorm.clear();
            e.last = c;
          }
          e.pnb = logAddExp(e.pnb, pAll + p);
          e.mergeLm(b.lm + wordScore(b.prev2, b.prev1, b.partialNorm));
          continue;
        }
        if (c == b.last) {
          // A doubled letter only after a blank.
          Beam& same = at(b.text, b);
          same.pnb = logAddExp(same.pnb, b.pnb + p);
          same.mergeLm(b.lm);
          const std::string text = b.text + vocab_[c];
          const bool fresh = next.find(text) == next.end();
          Beam& e = at(text, b);
          if (fresh) {
            e.partialNorm = b.partialNorm + vocabNorm_[c];
            e.last = c;
          }
          e.pnb = logAddExp(e.pnb, b.pb + p);
          e.mergeLm(b.lm);
          continue;
        }
        const std::string grown = b.partialNorm + vocabNorm_[c];
        // Leaving the known prefixes costs once; each further letter of a word that
        // is already unknown costs a little, so gluing known words onto an unknown
        // one is not cheaper than spacing them.
        double pen = 0;
        if (!lm_.isPrefix(grown) && !hotwords.count(grown))
          pen = (b.partialNorm.empty() || lm_.isPrefix(b.partialNorm)) ? w_.nonPrefix : w_.oovChar;
        const std::string text = b.text + vocab_[c];
        const bool fresh = next.find(text) == next.end();
        Beam& e = at(text, b);
        if (fresh) {
          e.partialNorm = grown;
          e.last = c;
        }
        e.pnb = logAddExp(e.pnb, pAll + p);
        e.mergeLm(b.lm + pen);
      }
    }
    std::vector<Beam*> sorted;
    sorted.reserve(next.size());
    for (auto& [k, b] : next) sorted.push_back(&b);
    const size_t keep = std::min(sorted.size(), size_t(beamWidth));
    std::partial_sort(sorted.begin(), sorted.begin() + keep, sorted.end(),
                      [](const Beam* a, const Beam* b) { return a->total() > b->total(); });
    std::unordered_map<std::string, Beam> kept;
    kept.reserve(keep);
    for (size_t i = 0; i < keep; i++) kept.emplace(sorted[i]->text, std::move(*sorted[i]));
    beams = std::move(kept);
  }

  const Beam* best = nullptr;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (const auto& [k, b] : beams) {
    double s = b.total();
    if (!b.partialNorm.empty()) s += wordScore(b.prev2, b.prev1, b.partialNorm);
    if (s > bestScore) {
      bestScore = s;
      best = &b;
    }
  }
  return best ? util::trim(best->text) : "";
}

std::string CtcDecoder::display(const std::string& text) const {
  std::string out;
  for (const auto& w : util::split(text, ' ')) {
    if (w.empty()) continue;
    std::string d = lm_.displayOf(lm_.idOf(normalizeLmWord(w)));
    if (d.empty()) d = restoreFinal(w);
    if (!out.empty()) out += ' ';
    out += d;
  }
  return out;
}

// ---------------------------------------------------------------- phrase rescorer

namespace {
std::u32string toU32(const std::string& s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (size_t k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    out += cp;
    i += n;
  }
  return out;
}

// Letters the model swaps or drops by sound; only to pick candidates.
std::u32string skeletonOf(const std::string& text) {
  std::u32string out;
  for (char32_t c : toU32(text)) {
    switch (c) {
      case 0x05DA: c = 0x05DB; break;  // ך -> כ
      case 0x05DD: c = 0x05DE; break;  // ם -> מ
      case 0x05DF: c = 0x05E0; break;  // ן -> נ
      case 0x05E3: c = 0x05E4; break;  // ף -> פ
      case 0x05E5: c = 0x05E6; break;  // ץ -> צ
    }
    switch (c) {
      case 0x05D0: case 0x05D4: case 0x05E2: case 0x05D5: case 0x05D9:  // א ה ע ו י
      case U'"': case U'\'': case U' ':
        continue;
      case 0x05D7: case 0x05E7: c = 0x05DB; break;  // ח ק -> כ
      case 0x05D8: case 0x05D3: c = 0x05EA; break;  // ט ד -> ת
      case 0x05E1: c = 0x05E9; break;               // ס -> ש
    }
    out += c;
  }
  return out;
}

size_t editDistance(const std::u32string& a, const std::u32string& b) {
  std::vector<size_t> d(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++) d[j] = j;
  for (size_t i = 1; i <= a.size(); i++) {
    size_t prev = d[0];
    d[0] = i;
    for (size_t j = 1; j <= b.size(); j++) {
      const size_t cur = d[j];
      d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
      prev = cur;
    }
  }
  return d[b.size()];
}

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  for (auto& w : util::split(s, ' '))
    if (!w.empty()) out.push_back(std::move(w));
  return out;
}

std::string join(const std::vector<std::string>& ws, size_t from, size_t to) {
  std::string s;
  for (size_t i = from; i < to; i++) {
    if (i > from) s += ' ';
    s += ws[i];
  }
  return s;
}

constexpr size_t kMaxSpan = 6;
constexpr int kMaxRounds = 4;
constexpr double kLoose = 0.5;
}  // namespace

PhraseRescorer::PhraseRescorer(const std::vector<std::string>& vocab, int blank,
                               const NgramLm& lm, const std::vector<std::string>& phrases,
                               const Sounds& sounds, Sounds wordSounds, LmWeights weights)
    : blank_(blank), lm_(lm), w_(weights), wordSounds_(std::move(wordSounds)) {
  for (size_t i = 0; i < vocab.size(); i++) index_.emplace(vocab[i], int(i));
  for (const auto& p : phrases) {
    Phrase ph{words(p), normalizeLmWord(p), {}};
    if (ph.words.size() < 2 || skeletonOf(p).empty()) continue;
    ph.spellings.push_back({ph.words, skeletonOf(p)});
    if (auto it = sounds.find(p); it != sounds.end()) {
      for (const auto& s : it->second) {
        if (s == p || skeletonOf(s).empty()) continue;
        // A sound with fewer words leaves the last ones empty ("בוייסאמ" for
        // איבעית אימא); one with more joins its tail into the last.
        auto sw = words(s);
        if (sw.size() < ph.words.size()) {
          sw.resize(ph.words.size());
        } else if (sw.size() > ph.words.size()) {
          std::string tail = join(sw, ph.words.size() - 1, sw.size());
          sw.resize(ph.words.size());
          sw.back() = tail;
        }
        ph.spellings.push_back({std::move(sw), skeletonOf(s)});
      }
    }
    if (ph.spellings.size() > 1) sounded_++;
    phrases_.push_back(std::move(ph));
  }
  for (size_t pi = 0; pi < phrases_.size(); pi++)
    for (size_t si = 0; si < phrases_[pi].spellings.size(); si++) {
      byLength_[phrases_[pi].spellings[si].skeleton.size()].push_back(flat_.size());
      flat_.push_back({pi, si});
    }
}

// Spellings whose skeleton length is within 3 of [n], in list order.
std::vector<size_t> PhraseRescorer::closeTo(size_t n) const {
  std::vector<size_t> ids;
  for (size_t len = n > 3 ? n - 3 : 0; len <= n + 3; len++)
    if (auto it = byLength_.find(len); it != byLength_.end())
      ids.insert(ids.end(), it->second.begin(), it->second.end());
  std::sort(ids.begin(), ids.end());
  return ids;
}

PhraseRescorer::Sounds PhraseRescorer::loadWordSounds(const std::vector<Path>& paths) {
  Sounds out;
  for (auto& [k, vs] : loadSounds(paths)) {
    auto& list = out[normalizeLmWord(k)];
    for (auto& v : vs)
      if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
  }
  return out;
}

PhraseRescorer::Sounds PhraseRescorer::loadSounds(const std::vector<Path>& paths) {
  Sounds out;
  for (const auto& path : paths) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (util::trim(line).empty() || line[0] == '#') continue;
      auto parts = util::split(line, '\t');
      auto& list = out[util::trim(parts[0])];
      for (size_t i = 1; i < parts.size(); i++) {
        const std::string v = util::trim(parts[i]);
        if (!v.empty() && std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
      }
    }
  }
  return out;
}

std::vector<int> PhraseRescorer::labels(const std::vector<std::string>& sounded) const {
  // A sounded "word" may be empty or hold several words (a personal sound).
  std::vector<std::string> ws;
  for (const auto& w : sounded)
    for (auto& x : words(w)) ws.push_back(std::move(x));
  std::vector<int> out;
  const auto space = index_.find(" ");
  for (size_t k = 0; k < ws.size(); k++) {
    if (k > 0 && space != index_.end()) out.push_back(space->second);
    // The last letter of a word in its final form, as the model writes it.
    for (const auto& ch : util::utf8Chars(restoreFinal(ws[k])))
      if (auto it = index_.find(ch); it != index_.end()) out.push_back(it->second);
  }
  return out;
}

double PhraseRescorer::lmScore(const std::vector<std::string>& ws) const {
  const double ln10 = std::log(10.0);
  int p2 = -1, p1 = -1;
  double s = 0;
  for (const auto& raw : ws) {
    std::string w;
    for (char c : raw)
      if (c != '"' && c != '\'') w += c;
    const int id = lm_.idOf(normalizeLmWord(w));
    s += id < 0 ? w_.oov + w_.beta : w_.alpha * lm_.score(p2, p1, id) * ln10 + w_.beta;
    p2 = p1;
    p1 = id;
  }
  return s;
}

// log P(labels | audio): the CTC forward pass in probabilities, normalized at
// every frame (no exp/log in the inner loop).
double PhraseRescorer::ctcLogProb(const std::vector<double>& p, int frames, int v,
                                  const std::vector<int>& labels) const {
  const double ninf = -std::numeric_limits<double>::infinity();
  const size_t s = labels.size() * 2 + 1;
  std::vector<int> ext(s, blank_);
  for (size_t k = 0; k < labels.size(); k++) ext[2 * k + 1] = labels[k];
  // A label may skip the blank before it unless it repeats the label two back.
  std::vector<char> skip(s, 0);
  for (size_t i = 2; i < s; i++) skip[i] = ext[i] != blank_ && ext[i] != ext[i - 2];
  std::vector<double> alpha(s, 0.0), next(s, 0.0);
  alpha[0] = p[blank_];
  if (s > 1) alpha[1] = p[ext[1]];
  double logScale = 0;
  for (int t = 1; t < frames; t++) {
    double sum = 0;
    for (size_t i = 0; i < s; i++) sum += alpha[i];
    if (sum <= 0) return ninf;
    logScale += std::log(sum);
    const double* row = &p[size_t(t) * v];
    for (size_t i = 0; i < s; i++) {
      double a = alpha[i];
      if (i >= 1) a += alpha[i - 1];
      if (skip[i]) a += alpha[i - 2];
      next[i] = a / sum * row[ext[i]];
    }
    std::swap(alpha, next);
  }
  const double end = s > 1 ? alpha[s - 1] + alpha[s - 2] : alpha[s - 1];
  return end <= 0 ? ninf : logScale + std::log(end);
}

std::vector<std::string> PhraseRescorer::rescore(const CtcOutput& out,
                                                 std::vector<std::string> written) const {
  if (out.frames == 0 || written.empty()) return written;
  const int v = out.vocab;
  std::vector<double> probs(out.logProbs.size());
  for (size_t k = 0; k < probs.size(); k++) probs[k] = std::exp(double(out.logProbs[k]));
  // The same candidate comes back from several spans and every round.
  std::unordered_map<std::string, double> memo;
  auto keyOf = [](const std::vector<std::string>& w, const std::vector<std::string>& s) {
    return join(s, 0, s.size()) + '\x01' + join(w, 0, w.size());
  };
  auto score = [&](const std::vector<std::string>& w, const std::vector<std::string>& s) {
    return ctcLogProb(probs, out.frames, v, labels(s)) + lmScore(w);
  };
  struct Cand {
    std::vector<std::string> w, s;
    std::string key;
  };
  const unsigned threads = std::max(1u, std::min(6u, std::thread::hardware_concurrency()));

  // Each word is written one way and scored against the audio another; only the
  // words of a swapped-in phrase differ.
  std::vector<std::string> sounded = written;
  for (int round = 0; round < kMaxRounds; round++) {
    const std::string curKey = keyOf(written, sounded);
    if (!memo.count(curKey)) memo.emplace(curKey, score(written, sounded));
    double best = memo[curKey];
    // Collect this round's candidates in order, score the new ones on several
    // threads, then take the first best in order: the same pick as one by one.
    std::vector<Cand> cands;
    auto add = [&](std::vector<std::string> cw, std::vector<std::string> cs) {
      std::string key = keyOf(cw, cs);
      cands.push_back({std::move(cw), std::move(cs), std::move(key)});
    };
    auto close = [&](const std::u32string& ss, const std::u32string& sp) {
      return double(editDistance(ss, sp)) <= kLoose * double(sp.size());
    };

    // 1. A word written exactly as a frequent word sounds ("קרו" = קְרָא): the
    // audio stays, only the LM judges the spelling.
    for (size_t k = 0; k < written.size(); k++) {
      if (written[k] != sounded[k]) continue;
      auto it = wordSounds_.find(normalizeLmWord(written[k]));
      if (it == wordSounds_.end()) continue;
      for (const auto& x : it->second) {
        auto cw = written;
        cw[k] = x;
        add(std::move(cw), sounded);
      }
    }

    // 2. Inside a word the LM does not know, glued by the beam ("הבוייסאמקרו"):
    // part of it becomes a phrase, the rest stays as words ("ה" + איבעית אימא +
    // "קרו"); a leftover of one or two letters may also be dropped. A leftover
    // may also take its word-sounds spelling in the same step: "קרו" alone
    // after the phrase scores worse than the glued word, "קרא" better.
    // (written, sounded) choices for a leftover; {"", ""} = none.
    auto leftovers = [&](const std::string& piece, bool keep) {
      std::vector<std::pair<std::string, std::string>> out;
      if (piece.empty() || !keep) return decltype(out){{"", ""}};
      out.push_back({piece, piece});
      if (auto it = wordSounds_.find(normalizeLmWord(piece)); it != wordSounds_.end())
        for (const auto& x : it->second) out.push_back({x, piece});
      return out;
    };
    for (size_t k = 0; k < written.size(); k++) {
      const std::string& w = written[k];
      if (w != sounded[k]) continue;
      const auto chars = util::utf8Chars(w);
      std::string stripped;
      for (char c : w)
        if (c != '"' && c != '\'') stripped += c;
      if (chars.size() < 4 || lm_.idOf(normalizeLmWord(stripped)) >= 0) continue;
      auto piece = [&](size_t a, size_t b) {
        std::string s;
        for (size_t c = a; c < b; c++) s += chars[c];
        return s;
      };
      for (size_t a = 0; a < chars.size(); a++) {
        for (size_t b = a + 1; b <= chars.size(); b++) {
          if (a == 0 && b == chars.size()) continue;
          const std::u32string ss = skeletonOf(piece(a, b));
          const std::string pre = piece(0, a), post = piece(b, chars.size());
          const size_t preLen = a, postLen = chars.size() - b;
          for (size_t id : closeTo(ss.size())) {
            const auto& ph = phrases_[flat_[id].first];
            const auto& sp = ph.spellings[flat_[id].second];
            if (!close(ss, sp.skeleton)) continue;
            for (int keepPre = 1; keepPre >= (preLen > 0 && preLen <= 2 ? 0 : 1); keepPre--) {
              for (int keepPost = 1; keepPost >= (postLen > 0 && postLen <= 2 ? 0 : 1); keepPost--) {
                for (const auto& left : leftovers(pre, preLen && keepPre)) {
                  for (const auto& right : leftovers(post, postLen && keepPost)) {
                    std::vector<std::string> cw(written.begin(), written.begin() + k);
                    std::vector<std::string> cs(sounded.begin(), sounded.begin() + k);
                    if (!left.first.empty()) cw.push_back(left.first), cs.push_back(left.second);
                    cw.insert(cw.end(), ph.words.begin(), ph.words.end());
                    cs.insert(cs.end(), sp.words.begin(), sp.words.end());
                    if (!right.first.empty()) cw.push_back(right.first), cs.push_back(right.second);
                    cw.insert(cw.end(), written.begin() + k + 1, written.end());
                    cs.insert(cs.end(), sounded.begin() + k + 1, sounded.end());
                    add(std::move(cw), std::move(cs));
                  }
                }
              }
            }
          }
        }
      }
    }

    // 3. Spans of whole words.
    for (size_t i = 0; i < written.size(); i++) {
      for (size_t j = i + 1; j <= std::min(written.size(), i + kMaxSpan); j++) {
        const std::string span = join(written, i, j);
        const std::u32string ss = skeletonOf(span);
        const std::string spanNorm = normalizeLmWord(span);
        for (size_t id : closeTo(ss.size())) {
          const auto& ph = phrases_[flat_[id].first];
          if (spanNorm == ph.norm) continue;
          {
            const auto& sp = ph.spellings[flat_[id].second];
            if (!close(ss, sp.skeleton)) continue;
            std::vector<std::string> cw(written.begin(), written.begin() + i);
            cw.insert(cw.end(), ph.words.begin(), ph.words.end());
            cw.insert(cw.end(), written.begin() + j, written.end());
            std::vector<std::string> cs(sounded.begin(), sounded.begin() + i);
            cs.insert(cs.end(), sp.words.begin(), sp.words.end());
            cs.insert(cs.end(), sounded.begin() + j, sounded.end());
            std::string key = keyOf(cw, cs);
            cands.push_back({std::move(cw), std::move(cs), std::move(key)});
          }
        }
      }
    }
    std::vector<size_t> todo;
    {
      std::unordered_set<std::string> queued;
      for (size_t k = 0; k < cands.size(); k++)
        if (!memo.count(cands[k].key) && queued.insert(cands[k].key).second) todo.push_back(k);
    }
    std::vector<double> scores(todo.size());
    if (todo.size() < 8 || threads == 1) {
      for (size_t n = 0; n < todo.size(); n++) scores[n] = score(cands[todo[n]].w, cands[todo[n]].s);
    } else {
      std::vector<std::thread> pool;
      for (unsigned t = 1; t < threads; t++) pool.emplace_back([&, t] {
        for (size_t n = t; n < todo.size(); n += threads)
          scores[n] = score(cands[todo[n]].w, cands[todo[n]].s);
      });
      for (size_t n = 0; n < todo.size(); n += threads)
        scores[n] = score(cands[todo[n]].w, cands[todo[n]].s);
      for (auto& th : pool) th.join();
    }
    for (size_t n = 0; n < todo.size(); n++) memo.emplace(cands[todo[n]].key, scores[n]);
    const Cand* pick = nullptr;
    for (const auto& c : cands) {
      const double s = memo[c.key];
      if (s > best) {
        best = s;
        pick = &c;
      }
    }
    if (!pick) break;
    written = pick->w;
    sounded = pick->s;
  }
  return written;
}

// ---------------------------------------------------------------- live transcriber

namespace {
constexpr int kRate = 16000;
constexpr double kUpdateEvery = 0.25;
constexpr double kSilenceToEnd = 0.6;
constexpr double kMaxSegment = 15.0;
constexpr double kPrerollSec = 0.3;
constexpr double kMinSpeechSec = 0.25;
}  // namespace

LiveTranscriber::LiveTranscriber(CtcModel& model, const CtcDecoder& decoder,
                                 std::set<std::string> hotwords, std::vector<std::string> context,
                                 const PhraseRescorer* rescorer)
    : model_(model), decoder_(decoder), rescorer_(rescorer), hotwords_(std::move(hotwords)),
      context_(std::move(context)) {}

std::vector<LiveUpdate> LiveTranscriber::add(const float* chunk, size_t n) {
  std::vector<LiveUpdate> out;
  constexpr size_t frame = 320;  // 20ms
  for (size_t o = 0; o < n; o += frame) step(chunk + o, std::min(frame, n - o), out);
  return out;
}

std::vector<LiveUpdate> LiveTranscriber::finish() {
  std::vector<LiveUpdate> out;
  if (inSpeech_) endSegment(out);
  return out;
}

void LiveTranscriber::step(const float* part, size_t n, std::vector<LiveUpdate>& out) {
  const double sec = double(n) / kRate;
  double sum = 0;
  for (size_t i = 0; i < n; i++) sum += double(part[i]) * part[i];
  const double level = std::sqrt(sum / n);
  // Noise floor: drops at once, rises slowly.
  floor_ = floor_ < 0 ? level : std::min(level, floor_ * 1.02 + 1e-5);
  const bool speaking = level > std::max(0.012, floor_ * 3);

  if (!inSpeech_) {
    preroll_.emplace_back(part, part + n);
    prerollSec_ += sec;
    while (prerollSec_ > kPrerollSec && preroll_.size() > 1) {
      prerollSec_ -= double(preroll_.front().size()) / kRate;
      preroll_.erase(preroll_.begin());
    }
    if (!speaking) return;
    inSpeech_ = true;
    segmentId_++;
    segment_.clear();
    for (const auto& p : preroll_) segment_.insert(segment_.end(), p.begin(), p.end());
    preroll_.clear();
    prerollSec_ = 0;
    speechSec_ = 0;
    silenceSec_ = 0;
    sinceUpdate_ = 0;
    committed_.clear();
    previous_.clear();
    hasPrevious_ = false;
    return;
  }

  segment_.insert(segment_.end(), part, part + n);
  sinceUpdate_ += n;
  if (speaking) {
    speechSec_ += sec;
    silenceSec_ = 0;
  } else {
    silenceSec_ += sec;
  }
  if (silenceSec_ >= kSilenceToEnd || double(segment_.size()) / kRate >= kMaxSegment) {
    endSegment(out);
  } else if (double(sinceUpdate_) >= kUpdateEvery * kRate) {
    sinceUpdate_ = 0;
    update(out);
  }
}

std::vector<std::string> LiveTranscriber::recognize(int beam, CtcOutput* keep) {
  CtcOutput r = model_.run(segment_.data(), segment_.size());
  if (r.frames == 0) return {};
  std::vector<std::string> words;
  for (auto& w : util::split(decoder_.decode(r, beam, hotwords_, context_), ' '))
    if (!w.empty()) words.push_back(std::move(w));
  if (keep) *keep = std::move(r);
  return words;
}

void LiveTranscriber::update(std::vector<LiveUpdate>& out) {
  const auto hyp = recognize(8);
  if (hasPrevious_) {
    size_t agree = 0;
    const long limit = long(std::min(previous_.size(), hyp.size())) - 1;
    while (long(agree) < limit && previous_[agree] == hyp[agree]) agree++;
    if (agree > committed_.size()) committed_.assign(hyp.begin(), hyp.begin() + agree);
  }
  previous_ = hyp;
  hasPrevious_ = true;
  std::vector<std::string> shown = committed_;
  if (hyp.size() > committed_.size())
    shown.insert(shown.end(), hyp.begin() + committed_.size(), hyp.end());
  out.push_back({segmentId_, display(shown, true), false});
}

void LiveTranscriber::endSegment(std::vector<LiveUpdate>& out) {
  inSpeech_ = false;
  if (speechSec_ >= kMinSpeechSec) {
    CtcOutput r;
    const auto finalWords = recognize(16, &r);
    // With a second pass, it gets the final decode of the whole segment (the
    // final update may revise committed words). Without one, the final decode
    // only completes what follows the committed words. Joining like that loses
    // words when the final decode glues differently: "אי" committed + a final
    // decode of one word = "אי" alone.
    std::vector<std::string> words;
    if (rescorer_ && r.frames) {
      words = rescorer_->rescore(r, finalWords);
    } else {
      words = committed_;
      if (finalWords.size() > committed_.size())
        words.insert(words.end(), finalWords.begin() + committed_.size(), finalWords.end());
    }
    out.push_back({segmentId_, display(words, false), true});
    for (const auto& w : words) context_.push_back(normalizeLmWord(w));
    if (context_.size() > 2) context_.erase(context_.begin(), context_.end() - 2);
  } else if (hasPrevious_) {
    // Too short (a cough, a click) after a draft was shown: clear it.
    out.push_back({segmentId_, "", true});
  }
  segment_.clear();
  segment_.shrink_to_fit();
  committed_.clear();
  previous_.clear();
  hasPrevious_ = false;
}

// The last draft word may be cut ("חו" of "חושן"); a library spelling would turn
// it into an abbreviation, so it is shown as is.
std::string LiveTranscriber::display(const std::vector<std::string>& words,
                                     bool lastIsDraft) const {
  auto join = [](const std::vector<std::string>& ws, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
      if (i) s += ' ';
      s += ws[i];
    }
    return s;
  };
  if (!lastIsDraft || words.empty()) return decoder_.display(join(words, words.size()));
  const std::string head = decoder_.display(join(words, words.size() - 1));
  return head.empty() ? words.back() : head + " " + words.back();
}

}  // namespace voice
