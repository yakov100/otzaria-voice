// Live reading in a light voice (light.h): the service speaks to the default
// output device itself, sentence by sentence, while the plugin follows along
// and highlights the sentence being said. A WebView plugin cannot be relied
// on to play audio, and the light voice makes a sentence in about 0.2 s on a
// processor, so the service can stay a few sentences ahead of the speakers.
//
// One reading at a time. The plugin starts it with a segment of text
// (POST /speak-live: a section of the book), appends the next segments while
// it plays (POST /speak-live-append), so that the reading runs on without a
// gap, and follows it through status(): which segment and which of its
// pieces (narrator::pieces) is being heard, as UTF-16 offsets into the
// segment's text. Windows only for now (waveOut); elsewhere start() reports
// "unsupported".
#pragma once

#include <cstdint>
#include <string>

namespace live {

struct Status {
  // idle | loading (the model, or waiting for the next sentence) | playing |
  // paused | done (everything appended was said) | error
  std::string state = "idle";
  std::string error;
  std::string voice;
  int64_t segment = -1;       // the id the plugin gave the segment being heard
  int piece = -1, pieces = 0; // the piece being heard, of the segment's pieces
  uint64_t from = 0, to = 0;  // its UTF-16 range in the segment's text
  int64_t lastSegment = -1;   // the id of the last segment appended
  int queued = 0;             // segments appended and not yet begun
};

bool supported();

// Starts reading [text] (UTF-8) as segment [id] in light voice [voice] at
// [rate] (1 = normal, 0.5 .. 3), ending the reading before it. Returns false
// with [error] ("unsupported", "empty", "light_model_missing",
// "nakdan_missing", "no_runtime") when it cannot start.
bool start(const std::string& text, const std::string& voice, double rate, int64_t id, std::string& error);
// Adds segment [id] after the ones already appended. With no reading
// ("not_reading") nothing is added.
bool append(const std::string& text, int64_t id, std::string& error);
void pause();
void resume();
void stop();
Status status();
// Releases the model when nothing has been read for a while; the service's
// idle loop calls it.
void trim();
// Stops the reading and releases the model now, so that its files can be
// replaced or removed.
void release();

}  // namespace live
