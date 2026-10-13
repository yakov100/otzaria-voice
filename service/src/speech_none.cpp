// Systems other than Windows: no MP3 reading yet (see speech.h).
#include "speech.h"

namespace speech {

bool supported() { return false; }

bool start(const std::string&, double, const std::string&, std::string& error) {
  error = "unsupported";
  return false;
}

Status status() { return {}; }

std::string takeResult() { return {}; }

void cancel() {}

}  // namespace speech
