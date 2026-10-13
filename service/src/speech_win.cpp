// Windows: the text is spoken by the system's Hebrew voice through
// Windows.Media.SpeechSynthesis (the OneCore voices, "Microsoft Asaf"; the old
// SAPI voices are not offered there) and encoded to MP3 by Media Foundation's
// own encoder. Nothing is downloaded and nothing leaves the computer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "speech.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "narrator.h"
#include "platform.h"

namespace speech {
namespace {

using winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer;
using winrt::Windows::Media::SpeechSynthesis::VoiceInformation;
using winrt::Windows::Storage::Streams::DataReader;

constexpr size_t kPieceChars = 1200;     // one synthesis call; progress and cancel between
constexpr UINT32 kBitrate = 64000;       // mono speech: small files, clear enough
constexpr size_t kMaxResult = 95u << 20; // the plugin's upload limit is 100MB
constexpr double kExampleSeconds = 5;    // of the voice's recording, for the decoder (see Voice::shortened)

std::mutex mu;      // current and result
std::mutex jobMu;   // starting and cancelling (the worker thread itself)
Status current;
std::string result;
std::atomic<bool> cancelFlag{false};
std::thread worker;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

/** Pieces of at most kPieceChars, cut after a sentence end where possible. */
std::vector<std::wstring> piecesOf(const std::wstring& text) {
  std::vector<std::wstring> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t stop = std::min(start + kPieceChars, text.size());
    if (stop < text.size()) {
      size_t cut = std::wstring::npos;
      for (size_t i = stop; i > start + kPieceChars / 4; i--) {
        const wchar_t c = text[i - 1];
        if (c == L'.' || c == L':' || c == L'?' || c == L'!' || c == L';' || c == L'\n' ||
            c == 0x05C3 /* sof pasuq */) {
          cut = i;
          break;
        }
      }
      if (cut == std::wstring::npos) {
        const size_t sp = text.rfind(L' ', stop);
        if (sp != std::wstring::npos && sp > start) cut = sp + 1;
      }
      if (cut != std::wstring::npos) stop = cut;
    }
    std::wstring piece = text.substr(start, stop - start);
    if (piece.find_first_not_of(L" \t\r\n") != std::wstring::npos) out.push_back(std::move(piece));
    start = stop;
  }
  return out;
}

struct Pcm {
  std::vector<int16_t> samples;  // mono
  UINT32 rate = 0;
};

/** Appends the samples of a 16-bit PCM WAV file (mono, or stereo averaged). */
void appendWav(const std::vector<uint8_t>& wav, Pcm& pcm) {
  auto u16 = [&](size_t o) { return uint16_t(wav[o] | wav[o + 1] << 8); };
  auto u32 = [&](size_t o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
  if (wav.size() < 12 || memcmp(wav.data(), "RIFF", 4) || memcmp(wav.data() + 8, "WAVE", 4))
    throw std::runtime_error("the voice did not return WAV audio");
  UINT32 channels = 0, rate = 0, bits = 0;
  for (size_t o = 12; o + 8 <= wav.size();) {
    const uint32_t size = u32(o + 4);
    const size_t body = o + 8;
    if (!memcmp(wav.data() + o, "fmt ", 4) && body + 16 <= wav.size()) {
      channels = u16(body + 2);
      rate = u32(body + 4);
      bits = u16(body + 14);
    } else if (!memcmp(wav.data() + o, "data", 4)) {
      if (bits != 16 || channels < 1 || channels > 2)
        throw std::runtime_error("unexpected audio format from the voice");
      if (pcm.rate && pcm.rate != rate) throw std::runtime_error("the voice changed sample rate");
      pcm.rate = rate;
      const size_t end = std::min(wav.size(), body + size_t(size));
      for (size_t p = body; p + 2 * channels <= end; p += 2 * channels) {
        int v = int16_t(u16(p));
        if (channels == 2) v = (v + int16_t(u16(p + 2))) / 2;
        pcm.samples.push_back(int16_t(v));
      }
      return;
    }
    o = body + size + (size & 1);
  }
  throw std::runtime_error("the voice returned no audio");
}

/** MP3 (MPEG-1 layer 3) takes 32, 44.1 or 48 kHz; speech voices give 16-24 kHz. */
Pcm resampleForMp3(const Pcm& in) {
  if (in.rate == 32000 || in.rate == 44100 || in.rate == 48000) return in;
  Pcm out;
  out.rate = (in.rate % 8000 == 0) ? 48000 : 44100;
  if (in.samples.empty()) return out;
  const double step = double(in.rate) / out.rate;
  const size_t n = size_t(in.samples.size() / step);
  out.samples.resize(n);
  for (size_t i = 0; i < n; i++) {
    const double x = i * step;
    const size_t a = size_t(x);
    const size_t b = std::min(a + 1, in.samples.size() - 1);
    const double f = x - a;
    out.samples[i] = int16_t(std::lround(in.samples[a] * (1 - f) + in.samples[b] * f));
  }
  return out;
}

void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    throw std::runtime_error(buf);
  }
}

/** Encodes with Media Foundation's MP3 encoder into a temporary file, read back. */
std::string encodeMp3(const Pcm& pcm) {
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  const std::wstring path = std::wstring(dir) + L"otzaria-voice-" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L".mp3";
  check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
  struct Shutdown {
    std::wstring path;
    ~Shutdown() {
      MFShutdown();
      DeleteFileW(path.c_str());
    }
  } shutdown{path};

  winrt::com_ptr<IMFSinkWriter> writer;
  check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, writer.put()), "MP3 writer");

  winrt::com_ptr<IMFMediaType> outType;
  check(MFCreateMediaType(outType.put()), "MFCreateMediaType");
  outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
  outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
  outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pcm.rate);
  outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kBitrate / 8);
  DWORD stream = 0;
  check(writer->AddStream(outType.get(), &stream), "MP3 encoder");

  winrt::com_ptr<IMFMediaType> inType;
  check(MFCreateMediaType(inType.put()), "MFCreateMediaType");
  inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
  inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pcm.rate);
  inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
  inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pcm.rate * 2);
  check(writer->SetInputMediaType(stream, inType.get(), nullptr), "MP3 input format");
  check(writer->BeginWriting(), "BeginWriting");

  const size_t block = pcm.rate / 10;  // 100 ms per sample
  for (size_t at = 0; at < pcm.samples.size(); at += block) {
    if (cancelFlag) throw std::runtime_error("cancelled");
    const size_t count = std::min(block, pcm.samples.size() - at);
    const DWORD bytes = DWORD(count * 2);
    winrt::com_ptr<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(bytes, buffer.put()), "MFCreateMemoryBuffer");
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    memcpy(dst, pcm.samples.data() + at, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    winrt::com_ptr<IMFSample> sample;
    check(MFCreateSample(sample.put()), "MFCreateSample");
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(LONGLONG(at) * 10000000 / pcm.rate);
    sample->SetSampleDuration(LONGLONG(count) * 10000000 / pcm.rate);
    check(writer->WriteSample(stream, sample.get()), "WriteSample");
  }
  check(writer->Finalize(), "Finalize");
  writer = nullptr;

  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot read the MP3 back");
  LARGE_INTEGER size{};
  GetFileSizeEx(f, &size);
  std::string mp3(size_t(size.QuadPart), '\0');
  DWORD got = 0;
  const BOOL ok = ReadFile(f, mp3.data(), DWORD(mp3.size()), &got, nullptr);
  CloseHandle(f);
  if (!ok || got != mp3.size()) throw std::runtime_error("cannot read the MP3 back");
  // Media Foundation puts an ID3v2.3 tag in front (TFLT, and a COMM frame
  // with iTunSMPB). Windows' own Media Player stays at 00:00 on such a file;
  // the same frames without the tag play everywhere. Drop it.
  if (mp3.size() > 10 && mp3.compare(0, 3, "ID3") == 0) {
    const auto b = [&](size_t i) { return size_t(static_cast<unsigned char>(mp3[i])); };
    size_t tag = 10 + ((b(6) & 0x7F) << 21 | (b(7) & 0x7F) << 14 | (b(8) & 0x7F) << 7 | (b(9) & 0x7F));
    if (b(5) & 0x10) tag += 10;  // footer
    if (tag < mp3.size()) mp3.erase(0, tag);
  }
  return mp3;
}

VoiceInformation hebrewVoice() {
  for (const auto& v : SpeechSynthesizer::AllVoices()) {
    const std::wstring lang{v.Language()};
    if (lang.rfind(L"he", 0) == 0 || lang.rfind(L"iw", 0) == 0) return v;
  }
  return nullptr;
}

/** The system voice: Windows.Media.SpeechSynthesis, piece by piece. */
Pcm speakSystem(const std::wstring& text, double rate) {
  const VoiceInformation voice = hebrewVoice();
  if (!voice) throw std::runtime_error("no_hebrew_voice");
  {
    std::lock_guard<std::mutex> lock(mu);
    current.voice = narrow(std::wstring(voice.DisplayName()));
  }
  SpeechSynthesizer synth;
  synth.Voice(voice);
  synth.Options().SpeakingRate(std::clamp(rate, 0.5, 3.0));

  Pcm pcm;
  uint64_t done = 0;
  for (const auto& piece : piecesOf(text)) {
    if (cancelFlag) throw std::runtime_error("cancelled");
    auto audio = synth.SynthesizeTextToStreamAsync(piece).get();
    const auto size = uint32_t(audio.Size());
    DataReader reader(audio.GetInputStreamAt(0));
    reader.LoadAsync(size).get();
    std::vector<uint8_t> wav(size);
    reader.ReadBytes(wav);
    appendWav(wav, pcm);
    done += piece.size();
    std::lock_guard<std::mutex> lock(mu);
    current.doneChars = done;
    current.seconds = pcm.rate ? double(pcm.samples.size()) / pcm.rate : 0;
  }
  return pcm;
}

/** A neural voice (narrator.h): slower, about twice the length of the audio. */
Pcm speakNeural(const std::wstring& text, const std::string& voiceId) {
  if (!narrator::modelReady()) throw std::runtime_error("voice_model_missing");
  const auto voicePath = narrator::voicePath(voiceId);
  if (voicePath.empty()) throw std::runtime_error("unknown_voice");
  const auto ort = platform::findOnnxRuntime();
  if (ort.empty()) throw std::runtime_error("ONNX Runtime not found");
  {
    std::lock_guard<std::mutex> lock(mu);
    current.voice = voiceId;
  }
  // The language model and the decoder run at the same time (speakAll), so
  // each gets part of the processor.
  const int cores = int(std::max(4u, std::thread::hardware_concurrency()));
  const int lmThreads = std::max(2, std::min(6, cores * 3 / 8));
  const int decoderThreads = std::max(2, std::min(6, cores * 3 / 8));
  narrator::Narrator model(ort, narrator::modelDir(), lmThreads, decoderThreads);
  const narrator::Voice voice = narrator::Voice::load(voicePath).shortened(kExampleSeconds);
  const auto pieces = narrator::pieces(narrow(text));
  std::vector<float> audioAll;
  uint64_t done = 0;
  const bool finished = model.speakAll(pieces, voice, [] { return bool(cancelFlag); },
                                       [&](size_t i, std::vector<float> audio) {
    audioAll.insert(audioAll.end(), audio.begin(), audio.end());
    audioAll.insert(audioAll.end(), narrator::kSampleRate / 6, 0.0f);  // a breath between sentences
    done += widen(pieces[i]).size();
    std::lock_guard<std::mutex> lock(mu);
    current.doneChars = std::min<uint64_t>(done, current.totalChars);
    current.seconds = double(audioAll.size()) / narrator::kSampleRate;
  });
  if (!finished || cancelFlag) throw std::runtime_error("cancelled");
  // The decoder's peaks pass 1.0 (1.2 in a test), which clipped and then
  // overshot further in the MP3. One gain for the whole file keeps the
  // loudest peak at 0.89 and the sentences at the same level as each other.
  float peak = 0;
  for (float x : audioAll) peak = std::max(peak, std::fabs(x));
  const float gain = peak > 0.89f ? 0.89f / peak : 1.0f;
  Pcm pcm;
  pcm.rate = narrator::kSampleRate;
  pcm.samples.reserve(audioAll.size());
  for (float x : audioAll) pcm.samples.push_back(int16_t(std::lround(std::clamp(x * gain, -1.0f, 1.0f) * 32767)));
  return pcm;
}

/** Writes [mp3] into [dir] as [name].mp3, or "name (2).mp3" etc. if taken. */
std::wstring saveInto(const std::wstring& dir, std::wstring name, const std::string& mp3) {
  std::wstring clean;
  for (wchar_t c : name)
    if (c >= 32 && !wcschr(L"\\/:*?\"<>|", c)) clean += c;
  while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.')) clean.pop_back();
  if (clean.empty()) clean = L"reading";
  if (clean.size() > 100) clean.resize(100);
  std::filesystem::path path;
  for (int n = 1; n < 1000; n++) {
    path = std::filesystem::path(dir) / (clean + (n == 1 ? L"" : L" (" + std::to_wstring(n) + L")") + L".mp3");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) break;
  }
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot_write");
  DWORD wrote = 0;
  const BOOL ok = WriteFile(f, mp3.data(), DWORD(mp3.size()), &wrote, nullptr);
  CloseHandle(f);
  if (!ok || wrote != mp3.size()) {
    DeleteFileW(path.c_str());
    throw std::runtime_error("cannot_write");
  }
  return path.wstring();
}

void run(std::wstring text, double rate, std::string voiceId, std::wstring saveDir, std::wstring saveName) {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  std::string error;
  std::string mp3;
  bool cancelled = false;
  try {
    const bool system = voiceId.empty() || voiceId == "system";
    Pcm pcm = system ? speakSystem(text, rate) : speakNeural(text, voiceId);
    if (pcm.samples.empty()) throw std::runtime_error("nothing to read");
    mp3 = encodeMp3(resampleForMp3(pcm));
    if (!saveDir.empty()) {
      const std::wstring path = saveInto(saveDir, saveName, mp3);
      std::lock_guard<std::mutex> lock(mu);
      current.savedPath = narrow(path);
    } else if (mp3.size() > kMaxResult) {
      throw std::runtime_error("too_large");
    }
  } catch (const winrt::hresult_error& e) {
    error = narrow(std::wstring(e.message()));
  } catch (const std::exception& e) {
    error = e.what();
    cancelled = error == "cancelled";
  }
  winrt::uninit_apartment();
  if (!error.empty() && !cancelled) platform::log("speak error: " + error);
  std::lock_guard<std::mutex> lock(mu);
  if (cancelled) {
    current.state = "cancelled";
  } else if (!error.empty()) {
    current.state = "error";
    current.error = error;
  } else {
    current.state = "done";
    current.doneChars = current.totalChars;
    current.bytes = mp3.size();
    if (current.savedPath.empty()) result = std::move(mp3);
  }
}

void cancelLocked() {
  cancelFlag = true;
  if (worker.joinable()) worker.join();
  std::lock_guard<std::mutex> lock(mu);
  if (current.state == "working") current.state = "cancelled";
}

}  // namespace

bool supported() { return true; }

bool start(const std::string& text, double rate, const std::string& voice, const std::string& saveDir,
           const std::string& saveName, std::string& error) {
  std::lock_guard<std::mutex> job(jobMu);
  cancelLocked();
  std::wstring wide = widen(text);
  if (wide.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
    error = "empty";
    return false;
  }
  const std::wstring dir = widen(saveDir);
  if (!dir.empty()) {
    std::error_code ec;
    const std::filesystem::path p(dir);
    if (!p.is_absolute() || !std::filesystem::is_directory(p, ec)) {
      error = "bad_folder";
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(mu);
  cancelFlag = false;
  current = Status{};
  current.state = "working";
  current.totalChars = wide.size();
  result.clear();
  worker = std::thread(run, std::move(wide), rate, voice, dir, widen(saveName));
  return true;
}

Status status() {
  std::lock_guard<std::mutex> lock(mu);
  return current;
}

std::string takeResult() {
  std::lock_guard<std::mutex> lock(mu);
  if (current.state != "done") return {};
  std::string out = std::move(result);
  result.clear();
  current = Status{};
  return out;
}

void cancel() {
  std::lock_guard<std::mutex> job(jobMu);
  cancelLocked();
}

}  // namespace speech
