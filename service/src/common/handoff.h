// Windows on ARM: the installer puts two programs side by side,
// otzaria-voice.exe (x64) and otzaria-voice-arm64.exe. ONNX Runtime is loaded
// from Otzaria, which is either build, and a program can only load a library
// built for its own processor. Before anything that loads it, the program
// looks at the library's processor and, when it is the other one and the
// other program lies next to it, runs that one instead.
//
// The pure parts live here (they are tested on any machine); starting and
// waiting for processes is in main.cpp and service.cpp.
#pragma once

#include <string>
#include <vector>

#include "platform.h"

namespace handoff {

enum class Arch { Unknown, X64, Arm64 };

const char* name(Arch a);  // "x64", "arm64", "unknown"

// The processor this program was built for.
Arch self();

// The processor a Windows DLL or EXE was built for, from its PE header.
// Unknown for anything else (x86, a damaged or missing file).
Arch machine(const platform::Path& file);

// The other build's file name, next to this one.
const wchar_t* exeName(Arch a);  // otzaria-voice.exe, otzaria-voice-arm64.exe

// The build that can load a library built for [library]; Unknown when this
// one can (or nothing is known).
Arch wanted(Arch self, Arch library);

// Flags one build adds when it runs the other. They come last and are taken
// out before the command is read.
struct Flags {
  bool noHandoff = false;     // --no-handoff: never pass on again
  std::string expectVersion;  // --expect-version V: refuse to run as another version
  unsigned long afterPid = 0; // --after-pid N: wait for that process to end first
};
Flags take(std::vector<std::string>& args);

// The command line (without the program) for the other build: [args] after
// the program name, or --serve when there were none, then the flags.
std::vector<std::string> forward(const std::vector<std::string>& args, const std::string& version);

// One Windows command-line argument, quoted as CommandLineToArgvW reads it.
std::wstring quote(const std::wstring& arg);

// Exit code of a build started with --expect-version that is another version.
constexpr int kOtherVersion = 75;

}  // namespace handoff
