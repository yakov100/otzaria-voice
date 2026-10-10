// The Hebrew voice pack (speech model + library language model): downloaded
// once in the background, verified by SHA-256, and unpacked next to Otzaria's
// own data in <Otzaria data folder>/stt/pack (see platform::otzariaDataRoot).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "platform.h"

namespace pack {

using platform::Path;

struct Release {
  int version;
  const char* url;
  uint64_t size;
  const char* sha256;  // lower-case hex
};

// The pack this service version installs (tool/voice_pack/make_pack.py).
extern const Release kRelease;

// The pack in use: the first complete one of OTZARIA_VOICE_PACK_DIR, the
// user's copy, Otzaria's data folder (Linux, macOS) and a copy installed for
// everyone (/usr/share/otzaria-voice/pack; Otzaria Voice.app/Contents/Resources
// /pack), which may be read-only. installTarget() otherwise.
Path dir();
// Where a download goes: OTZARIA_VOICE_PACK_DIR when writable, else userDir().
Path installTarget();
// Windows %APPDATA%\otzaria\stt\pack; Linux ~/.local/share/otzaria-voice/pack;
// macOS ~/Library/Application Support/OtzariaVoice/pack; Android <files>/stt/pack.
Path userDir();
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

// What each system does its own way (pack_win.cpp, pack_posix.cpp,
// pack_android.cpp). Errors are std::runtime_error with a Hebrew message the
// plugin shows as is.
namespace sys {

using OnProgress = std::function<void(uint64_t done)>;

// Appends the rest of [url] to [part], resuming from its current size.
void download(const std::string& url, const Path& part, uint64_t expected,
              const OnProgress& progress);
// Lower-case hex SHA-256 of [file].
std::string hashFile(const Path& file, const OnProgress& progress);
// Unpacks the zip archive [zip] into the existing folder [to].
void extract(const Path& zip, const Path& to);

}  // namespace sys

#ifdef __ANDROID__
// Android downloads and unpacks in Java (HttpURLConnection, java.util.zip);
// jni.cpp installs these before the service starts.
struct AndroidHooks {
  std::function<void(const std::string& url, const std::string& part, uint64_t expected,
                     const sys::OnProgress& progress)>
      download;
  std::function<void(const std::string& zip, const std::string& to)> extract;
};
void setAndroidHooks(AndroidHooks hooks);
#endif

}  // namespace pack
