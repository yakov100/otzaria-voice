// Installing the service for the current user, without administrator rights,
// so that it starts by itself when the user logs in:
//   Windows  %LOCALAPPDATA%\Programs\OtzariaVoice + HKCU Run + "Installed apps"
//   Linux    ~/.local/lib/otzaria-voice + ~/.config/autostart (XDG)
//   macOS    ~/Library/Application Support/OtzariaVoice + a LaunchAgent
// Android has no command line: the app's boot receiver starts the service.
#pragma once

#include "platform.h"

namespace install {

// Copies this program (and an ONNX Runtime library lying next to it) to the
// per-user folder, registers the login start and starts the installed copy.
int install(bool quiet);
// Stops the service, removes the login start and the installed files. The
// downloaded voice pack (shared with Otzaria) stays.
int uninstall(bool quiet);

platform::Path installedExe();

#ifdef _WIN32
// This program runs from the install folder (either build, handoff.h).
bool isInstalledCopy();

// Asks a running copy on this port to quit, and waits (up to 10 s) until its
// one-instance mutex is gone.
void stopRunning();

// --deliver-plugin <path>: the store's installer (installer/windows/otzaria-voice.nsi)
// has put the plugin it carries in a folder of its own under %TEMP%; this asks
// Otzaria to install it (otzaria://plugin/install-local). Otzaria may read the
// file only after the user agrees, so it stays; uninstall removes it.
// Returns 0 when Otzaria was asked, 2 for a bad path, 3 when no program opens
// otzaria:// links (Otzaria is not installed).
int deliverPlugin(const platform::Path& plugin);
#endif

}  // namespace install
