// The Hebrew voice pack (speech model + library language model): downloaded
// once in the background, verified by SHA-256, and unpacked next to Otzaria's
// own data in %APPDATA%\otzaria\stt\pack.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace pack {

struct Release {
  int version;
  const wchar_t* url;
  uint64_t size;
  const char* sha256;  // lower-case hex
};

// The pack this service version installs (tool/voice_pack/make_pack.py).
extern const Release kRelease;

std::wstring dir();          // installed pack
bool ready();                // every required file present
int installedVersion();      // 0 when none

struct Progress {
  std::string state = "idle";  // idle | downloading | verifying | extracting | done | error
  uint64_t received = 0;
  uint64_t total = 0;
  std::string error;
};

Progress progress();

// Starts the download in a background thread unless one is running. Before the
// new pack replaces the old one, [release] is called so the caller can let go of
// files it has open (the language model is memory-mapped).
void startInstall(std::function<void()> release);

}  // namespace pack
