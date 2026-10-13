// The reading-aloud model pack (narrator.h): Chatterbox Multilingual, the
// Phonikud diacritizer and the recorded voices that ship with it, about 1GB.
// Downloaded only when a user first picks a recorded voice, verified by
// SHA-256 and unpacked into <Otzaria data folder>/tts/pack, with the same
// download/verify/unpack pieces as the dictation pack (pack.h).
#pragma once

#include <cstdint>
#include <functional>

#include "pack.h"

namespace voicemodel {

// The pack this service version installs (tools/make_voice_model.py).
extern const pack::Release kRelease;

bool ready();             // every required file present (narrator::modelReady)
int installedVersion();   // 0 when none

pack::Progress progress();

// Deletes the downloaded model pack and partial downloads; voices the user
// made (tts/voices) stay. Returns the bytes freed.
uint64_t removeInstalled();

// Starts the download in a background thread unless one is running. [release]
// is called before the new files replace old ones (open model sessions).
void startInstall(std::function<void()> release);

}  // namespace voicemodel
