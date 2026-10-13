// Reading text aloud into an MP3 file with the system's Hebrew voice
// ("Microsoft Asaf" on Windows). Windows only for now; elsewhere every call
// reports "unsupported".
//
// One job at a time, in the background: the plugin starts it (POST /speak),
// polls its progress (GET /speak-status) and then takes the file
// (POST /speak-result). A request through network.fetchStream may not run
// past 120 seconds, so a long chapter could not be read in a single request.
#pragma once

#include <cstdint>
#include <string>

namespace speech {

struct Status {
  std::string state = "idle";  // idle | working | done | error | cancelled
  std::string error;
  std::string voice;           // the voice used, e.g. "Microsoft Asaf"
  uint64_t doneChars = 0, totalChars = 0;
  double seconds = 0;          // length of the audio made so far
  uint64_t bytes = 0;          // size of the finished MP3
};

bool supported();

// Starts reading [text] (UTF-8) at [rate] (1 = normal, 0.5 .. 3). A running
// job is cancelled first. Returns false with [error] when it cannot start.
bool start(const std::string& text, double rate, std::string& error);

Status status();

// Moves the finished MP3 out (empty when there is none) and resets to idle.
std::string takeResult();

void cancel();

}  // namespace speech
