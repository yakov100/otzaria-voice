#include "light.h"

#include <cmath>

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "hebrew_text.h"
#include "json.h"
#include "ort_session.h"

namespace light {

namespace {

using platform::Path;
using namespace hebrew;

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

Path dir() {
  const auto e = platform::env("OTZARIA_LIGHT_DIR");
  return e.empty() ? platform::otzariaDataRoot() / "tts" / "light" : platform::fromUtf8(e);
}

bool isLightId(const std::string& id) {
  // A plain file name: nothing that could leave the folder.
  return endsWith(id, kSuffix) && id.size() > std::string(kSuffix).size() &&
         id.find_first_of("/\\:.") == std::string::npos;
}

std::vector<std::string> installedVoices() {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir(), ec)) {
    if (e.path().extension() != ".onnx") continue;
    const std::string id = platform::toUtf8(e.path().stem());
    if (isLightId(id) && std::filesystem::exists(e.path().parent_path() / platform::fromUtf8(id + ".json"), ec))
      out.push_back(id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

Path modelPath(const std::string& id) {
  if (!isLightId(id)) return {};
  const Path onnx = dir() / platform::fromUtf8(id + ".onnx");
  const Path config = dir() / platform::fromUtf8(id + ".json");
  std::error_code ec;
  if (std::filesystem::exists(onnx, ec) && std::filesystem::exists(config, ec)) return onnx;
  return {};
}

bool anyReady() { return !installedVoices().empty(); }

struct LightVoice::Impl {
  ort::Runtime rt;
  OrtSession* session = nullptr;
  std::unordered_map<char32_t, std::vector<int64_t>> symbols;
  std::vector<int64_t> bos, eos, pad;
  int sampleRate = 22050;
  float scales[3] = {0.667f, 1.0f, 0.8f};  // noise, length, noise_w
  std::vector<std::string> inputNames, outputNames;
  bool multiSpeaker = false;
  std::set<char32_t> warned;  // characters without a symbol, logged once each
  std::vector<float> lowpass;  // FIR taps (inference.lowpass_hz); empty = none

  Impl(const Path& ortLibrary) : rt(ortLibrary, "otzaria-light") {}
  ~Impl() { rt.close(session); }
};

LightVoice::LightVoice(const Path& ortLibrary, const Path& model, int threads)
    : impl_(std::make_unique<Impl>(ortLibrary)) {
  auto& o = *impl_;
  Path config = model;
  config.replace_extension(".json");
  const auto doc = json::parse(readFile(config));
  const auto& map = doc["phoneme_id_map"];
  if (map.kind != json::Value::Object) throw std::runtime_error("light voice: no phoneme_id_map in the config");
  for (const auto& [k, v] : map.o) {
    const std::u32string key = toU32(k);
    if (key.size() != 1) continue;  // the voice's symbols are single characters
    std::vector<int64_t> ids;
    if (v.kind == json::Value::Array)
      for (const auto& x : v.a) ids.push_back(int64_t(x.num()));
    else if (v.kind == json::Value::Number)
      ids.push_back(int64_t(v.num()));
    if (key[0] == U'^') o.bos = ids;
    else if (key[0] == U'$') o.eos = ids;
    else if (key[0] == U'_') o.pad = ids;
    else o.symbols[key[0]] = ids;
  }
  if (o.pad.empty()) o.pad = {0};
  if (doc["audio"]["sample_rate"].kind == json::Value::Number) o.sampleRate = int(doc["audio"]["sample_rate"].num());
  const auto& inf = doc["inference"];
  if (inf["noise_scale"].kind == json::Value::Number) o.scales[0] = float(inf["noise_scale"].num());
  if (inf["length_scale"].kind == json::Value::Number) o.scales[1] = float(inf["length_scale"].num());
  if (inf["noise_w"].kind == json::Value::Number) o.scales[2] = float(inf["noise_w"].num());
  if (doc["num_speakers"].kind == json::Value::Number) o.multiSpeaker = doc["num_speakers"].num() > 1;
  // A small VITS decoder adds a buzz around 6-8 kHz that its teacher does not
  // have (the voice sounds metallic); the speaker chose a fade from 5.5 to
  // 6.5 kHz by ear (19.10). inference.lowpass_hz is the middle of the fade.
  if (inf["lowpass_hz"].kind == json::Value::Number) {
    const double fc = inf["lowpass_hz"].num() / o.sampleRate;  // cycles per sample
    const double width = (inf["lowpass_width_hz"].kind == json::Value::Number ? inf["lowpass_width_hz"].num() : 1000.0) / o.sampleRate;
    int n = int(std::ceil(5.5 / width)) | 1;  // Blackman: transition ~5.5/N
    n = std::clamp(n, 31, 511);
    o.lowpass.resize(size_t(n));
    const double pi = 3.14159265358979323846;
    double sum = 0;
    for (int i = 0; i < n; ++i) {
      const double t = i - (n - 1) / 2.0;
      const double sinc = t == 0 ? 2 * fc : std::sin(2 * pi * fc * t) / (pi * t);
      const double w = 0.42 - 0.5 * std::cos(2 * pi * i / (n - 1)) + 0.08 * std::cos(4 * pi * i / (n - 1));
      o.lowpass[size_t(i)] = float(sinc * w);
      sum += sinc * w;
    }
    for (auto& v : o.lowpass) v = float(v / sum);  // unity gain at 0 Hz
  }
  o.session = o.rt.open(model, threads);
  o.inputNames = o.rt.names(o.session, true);
  o.outputNames = o.rt.names(o.session, false);
  if (o.inputNames.size() < 3 || o.outputNames.empty())
    throw std::runtime_error("light voice: unexpected model inputs");
}

LightVoice::~LightVoice() = default;

int LightVoice::sampleRate() const { return impl_->sampleRate; }

std::vector<int64_t> LightVoice::ids(const std::string& text) const {
  auto& o = *impl_;
  std::vector<int64_t> out(o.bos);
  out.insert(out.end(), o.pad.begin(), o.pad.end());
  for (char32_t c : canonicalOrder(toU32(text))) {
    auto it = o.symbols.find(c);
    if (it == o.symbols.end()) {
      if (o.warned.insert(c).second) platform::log("light voice: no symbol for U+" + [&] {
        char buf[16];
        snprintf(buf, sizeof buf, "%04X", unsigned(c));
        return std::string(buf);
      }());
      continue;
    }
    out.insert(out.end(), it->second.begin(), it->second.end());
    out.insert(out.end(), o.pad.begin(), o.pad.end());
  }
  out.insert(out.end(), o.eos.begin(), o.eos.end());
  return out;
}

std::vector<float> LightVoice::speak(const std::string& text, double rate) {
  auto& o = *impl_;
  float scales[3] = {o.scales[0], o.scales[1] / float(std::clamp(rate, 0.5, 3.0)), o.scales[2]};
  std::vector<int64_t> input = ids(text);
  if (input.size() <= o.bos.size() + o.pad.size() + o.eos.size()) return {};
  std::vector<int64_t> lengths{int64_t(input.size())};
  int64_t sid = 0;
  std::vector<OrtValue*> in{
      o.rt.tensor(input.data(), input.size(), {1, int64_t(input.size())}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
      o.rt.tensor(lengths.data(), 1, {1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64),
      o.rt.tensor(scales, 3, {3}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)};
  std::vector<std::string> names{"input", "input_lengths", "scales"};
  if (o.multiSpeaker || o.inputNames.size() > 3) {
    in.push_back(o.rt.tensor(&sid, 1, {1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64));
    names.push_back("sid");
  }
  std::vector<OrtValue*> out;
  ort::ValueGuard guard{o.rt, in, out};
  out = o.rt.run(o.session, names, in, {o.outputNames[0]});
  size_t count = 1;
  for (int64_t d : o.rt.shapeOf(out[0])) count *= size_t(d);
  const float* data = o.rt.dataOf<float>(out[0]);
  if (o.lowpass.empty()) return std::vector<float>(data, data + count);
  // the filter, centred (no delay), zeros beyond the ends
  const size_t n = o.lowpass.size(), half = n / 2;
  std::vector<float> y(count, 0.0f);
  for (size_t i = 0; i < count; ++i) {
    const size_t k0 = i + half >= count ? i + half - count + 1 : 0;
    const size_t k1 = std::min(n, i + half + 1);
    float acc = 0.0f;
    for (size_t k = k0; k < k1; ++k) acc += o.lowpass[k] * data[i + half - k];
    y[i] = acc;
  }
  return y;
}

}  // namespace light
