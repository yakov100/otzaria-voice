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

// One dictation at a time: plugin (POST /dictate) or typing (Windows hotkey).
extern std::mutex sessionMu;
extern std::atomic<bool> active;
extern std::atomic<bool> stopRequested;
extern std::atomic<bool> cancelRequested;

// Android: recording is possible only while the app's foreground service holds
// the microphone; elsewhere always true.
extern std::atomic<bool> microphoneAllowed;

// Marks the engine as used now (idle unloading counts from here).
void touchEngine();

// Listens until stopped, cancelled or [maxDuration]; [sink] receives every
// update. [alive] returning false aborts at once (the listener went away).
// Returns why it ended: stopped | cancelled | time_limit | gone.
std::string recognizeLive(Microphone& mic, const std::set<std::string>& hotwords,
                          const std::vector<std::string>& context,
                          const std::function<void(const std::vector<voice::LiveUpdate>&)>& sink,
                          const std::function<bool()>& alive, const std::function<void()>& onReady,
                          std::chrono::seconds maxDuration);

// Typing into any focused window with a global key combination (typing_win.cpp).
// Elsewhere these do nothing and the combination is "".
void startTyping();
const char* typingHotkey();

}  // namespace service
