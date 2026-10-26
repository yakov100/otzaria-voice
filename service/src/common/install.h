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

}  // namespace install
