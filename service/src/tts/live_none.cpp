// Systems other than Windows: no live reading yet (see live.h).
#include "live.h"

namespace live {

bool supported() { return false; }

bool start(const std::string&, const std::string&, double, int64_t, std::string& error) {
  error = "unsupported";
  return false;
}

bool append(const std::string&, int64_t, std::string& error) {
  error = "not_reading";
  return false;
}

void pause() {}
void resume() {}
void stop() {}
Status status() { return {}; }
void trim() {}
void release() {}

}  // namespace live
