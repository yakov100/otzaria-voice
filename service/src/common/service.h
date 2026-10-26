// The service itself, shared by every system: the HTTP endpoints, the
// recognition engine (loaded on first use, released when idle) and dictation.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "audio.h"
#include "engine.h"
#include "platform.h"

namespace service {

extern const char* kVersion;

int port();  // 47811, or OTZARIA_VOICE_PORT

// Runs the HTTP server until POST /quit (or requestQuit). Returns the exit code.
int serve();
void requestQuit();

// Asks a running copy on this port to quit (used before replacing it).
void postQuit();

// --eval <manifest.jsonl> <out.jsonl>: offline and live-simulated transcripts.
int evaluate(const platform::Path& manifest, const platform::Path& out);

// --vocalize <models> <text.txt> <out.txt> [--marks]: each line of the text
// with nikud added (lexicon and diacritizer; [models] is any folder holding
// nakdan.onnx, nakdan_tokenizer.json and optionally nikud_lexicon.bin), for
// measuring the nikud's accuracy; with --marks also the stress and vocal-shva marks
// (training text for the light voice).
int vocalizeTest(const platform::Path& models, const platform::Path& text, const platform::Path& out,
                 bool withMarks);

// --narrate <models> <voice> <text.txt> <out dir>: reads each line of the text
// in the voice; writes NN.wav and report.jsonl (vocalized text, tokens, times).
int narrateTest(const platform::Path& models, const platform::Path& voice, const platform::Path& text,
                const platform::Path& outDir);

// --light <model.onnx> <text.txt> <out dir> [--raw]: reads each line with the
// light voice (light.h), vocalized by the installed diacritizer first unless
// --raw (then the lines are the voice's symbols as they are); writes NN.wav
// and report.jsonl.
int lightTest(const platform::Path& model, const platform::Path& text, const platform::Path& outDir, bool raw);

// One dictation at a time: plugin (POST /dictate). The service never types into other programs.
extern std::mutex sessionMu;
extern std::atomic<bool> active;
extern std::atomic<bool> stopRequested;
extern std::atomic<bool> cancelRequested;

// Android: recording is possible only while the app's foreground service holds
// the microphone; elsewhere always true.
extern std::atomic<bool> microphoneAllowed;

// Marks the engine as used now (idle unloading counts from here).
void touchEngine();

// Optional automatic ends of a dictation (0 = off).
struct AutoEnd {
  // After a finished sentence, this long without new speech ends it.
  std::chrono::milliseconds afterSilence{0};
  // Nothing said at all for this long ends it.
  std::chrono::milliseconds noSpeech{0};
};

// Listens until stopped, cancelled or [maxDuration]; [sink] receives every
// update. [alive] returning false aborts at once (the listener went away).
// Returns why it ended: stopped | cancelled | time_limit | gone | silence |
// no_speech.
std::string recognizeLive(Microphone& mic, const std::set<std::string>& hotwords,
                          const std::vector<std::string>& context,
                          const std::function<void(const std::vector<voice::LiveUpdate>&)>& sink,
                          const std::function<bool()>& alive, const std::function<void()>& onReady,
                          std::chrono::seconds maxDuration, AutoEnd autoEnd = {});

}  // namespace service
