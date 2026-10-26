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
// The nikud lexicon (tools/tts/build_nikud_lexicon.py), a small zip holding
// nikud_lexicon.bin, which goes into the pack beside the models.
extern const pack::Release kLexicon;

bool ready();             // every required file present (narrator::modelReady)
int installedVersion();   // 0 when none

// Fetches the lexicon into the pack, quietly in the background, when the
// pack is installed and the lexicon is not (at start-up and after an
// install). Fails silently: the narrator works without it.
void ensureLexicon();

pack::Progress progress();

// Deletes the downloaded model pack and partial downloads; voices the user
// made (tts/voices) stay. Returns the bytes freed.
uint64_t removeInstalled();

// Starts the download in a background thread unless one is running. [release]
// is called before the new files replace old ones (open model sessions).
void startInstall(std::function<void()> release);

// ---- The light voice (light.h): a small zip with <id>-light.onnx and
// <id>-light.json, and, for a computer without the recorded voice's pack, the
// diacritizer's own zip (nakdan.onnx, nakdan_tokenizer.json) into tts/nakdan.
// Until the voice is published kLight.size is 0 and the install reports
// "not_published"; OTZARIA_LIGHT_MODEL_URL/_SIZE/_SHA256 (and the same with
// OTZARIA_NAKDAN_PACK_) replace the releases for testing.
extern const pack::Release kLight;
extern const pack::Release kNakdanPack;

bool lightReady();            // at least one light voice installed
bool nakdanReady();           // a diacritizer is available (pack or own folder)
bool lightPublished();        // kLight (or its override) can be downloaded
// What a download would fetch now: the voice, plus the diacritizer if missing.
uint64_t lightDownloadSize();
pack::Progress lightProgress();
void startInstallLight(std::function<void()> release);
// Deletes the light voices, and the diacritizer's own folder when the
// recorded voice's pack is not installed either (nothing else uses it).
uint64_t removeLight();

}  // namespace voicemodel
