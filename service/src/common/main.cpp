// Otzaria voice service: command line.
//
//   otzaria-voice --serve               run the service (what login start runs)
//   otzaria-voice --install [--quiet]   install for this user, start at login, run now
//   otzaria-voice --uninstall [--quiet] stop, remove login start and program files
//   otzaria-voice --eval <manifest.jsonl> <out.jsonl>   accuracy evaluation
//   otzaria-voice --narrate <models> <voice> <text.txt> <out dir>   reading-aloud test
//   otzaria-voice --light <model.onnx> <text.txt> <out dir> [--raw]  light-voice test
//   otzaria-voice --version
//
// Windows: started without arguments (double-clicked from a download) it
// installs itself; the installed copy, started at login, serves. Elsewhere no
// arguments means --serve.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "handoff.h"
#include "install.h"
#include "platform.h"
#include "service.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

#ifdef _WIN32
// Starts [exe] with [args], inheriting no handles (the service's sockets stay
// its own). Returns the process, or null.
HANDLE start(const platform::Path& exe, const std::vector<std::string>& args) {
  std::wstring line = handoff::quote(exe.wstring());
  for (const auto& a : args) line += L" " + handoff::quote(platform::fromUtf8(a).wstring());
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                      &pi))
    return nullptr;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

// --after-pid: the copy that started this one is quitting; wait for it and
// for its hold on the port to go.
void waitFor(unsigned long pid) {
  if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
    if (WaitForSingleObject(p, 30000) == WAIT_TIMEOUT)
      platform::log("handoff: process " + std::to_string(pid) + " still running after 30 s");
    CloseHandle(p);
  }
  for (int i = 0; i < 120 && service::running(); i++) platform::sleepMs(1000);
}

// The other build, when ONNX Runtime is built for its processor and it lies
// next to this one; empty otherwise.
platform::Path otherBuild() {
  const auto ort = platform::findOnnxRuntime();
  if (ort.empty()) return {};
  const auto want = handoff::wanted(handoff::self(), handoff::machine(ort));
  if (want == handoff::Arch::Unknown) return {};
  std::error_code ec;
  const auto other = platform::exeDir() / handoff::exeName(want);
  return std::filesystem::exists(other, ec) ? other : platform::Path();
}

// Commands that load ONNX Runtime run in the build that can load it
// (handoff.h). Returns the exit code when the other build ran the command,
// or -1 for this build to run it.
int passOn(const std::vector<std::string>& args, const handoff::Flags& flags) {
  const std::string arg = args.size() > 1 ? args[1] : "";
  const bool serving = arg == "--serve" || (arg.empty() && install::isInstalledCopy());
  if (!serving && arg != "--eval" && arg != "--narrate" && arg != "--light" && arg != "--vocalize")
    return -1;
  if (flags.afterPid) waitFor(flags.afterPid);
  if (flags.noHandoff) return -1;
  if (serving && service::running()) return 0;  // as serve() would
  const auto other = otherBuild();
  if (other.empty()) return -1;
  const auto log = [&](const std::string& what) {
    platform::log("handoff to " + platform::toUtf8(other) + ": " + what);
  };
  HANDLE p = start(other, handoff::forward(args, service::kVersion));
  if (!p) {
    log("cannot start it");
    if (serving) service::handoffFailed();
    return -1;
  }
  DWORD code = 1;
  if (!serving) {
    WaitForSingleObject(p, INFINITE);
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    if (code != DWORD(handoff::kOtherVersion)) return int(code);
    log("another version");
    return -1;
  }
  // Serving: the other build takes over once it holds the port's mutex.
  bool took = false;
  for (int i = 0; i < 100 && !took; i++) {
    took = service::running();
    if (!took && WaitForSingleObject(p, 100) == WAIT_OBJECT_0) break;
  }
  took = took || service::running();
  if (!took && GetExitCodeProcess(p, &code) && code == STILL_ACTIVE) {
    TerminateProcess(p, 1);
    code = 1;
  }
  CloseHandle(p);
  if (took) {
    log("serving");
    return 0;
  }
  log("did not start serving (exit " + std::to_string(code) + "); serving here");
  service::handoffFailed();
  return -1;
}
#endif

int run(std::vector<std::string> args) {
#ifdef _WIN32
  const auto flags = handoff::take(args);
  if (!flags.expectVersion.empty() && flags.expectVersion != service::kVersion) {
    platform::log(std::string("handoff: this build is ") + service::kVersion + ", not " +
                  flags.expectVersion);
    return handoff::kOtherVersion;
  }
  if (const int code = passOn(args, flags); code >= 0) return code;
#endif
  const std::string arg = args.size() > 1 ? args[1] : "";
  if (arg == "--eval" && args.size() > 3) {
    try {
      return service::evaluate(platform::fromUtf8(args[2]), platform::fromUtf8(args[3]));
    } catch (const std::exception& ex) {
      platform::log(std::string("eval: ") + ex.what());
      return 3;
    }
  }
  if (arg == "--narrate" && args.size() > 5) {
    try {
      return service::narrateTest(platform::fromUtf8(args[2]), platform::fromUtf8(args[3]),
                                  platform::fromUtf8(args[4]), platform::fromUtf8(args[5]));
    } catch (const std::exception& ex) {
      platform::log(std::string("narrate: ") + ex.what());
      return 3;
    }
  }
  if (arg == "--light" && args.size() > 4) {
    try {
      return service::lightTest(platform::fromUtf8(args[2]), platform::fromUtf8(args[3]),
                                platform::fromUtf8(args[4]), args.size() > 5 && args[5] == "--raw");
    } catch (const std::exception& ex) {
      platform::log(std::string("light: ") + ex.what());
      return 3;
    }
  }
  if (arg == "--vocalize" && args.size() > 4) {
    try {
      return service::vocalizeTest(platform::fromUtf8(args[2]), platform::fromUtf8(args[3]),
                                   platform::fromUtf8(args[4]), args.size() > 5 && args[5] == "--marks");
    } catch (const std::exception& ex) {
      platform::log(std::string("vocalize: ") + ex.what());
      return 3;
    }
  }
  if (arg == "--version") {
    std::printf("otzaria-voice %s\n", service::kVersion);
    return 0;
  }
  const bool quiet = args.size() > 2 && args[2] == "--quiet";
  if (arg == "--install") return install::install(quiet);
  if (arg == "--uninstall") return install::uninstall(quiet);
  if (arg == "--serve") return service::serve();
#ifdef _WIN32
  if (!install::isInstalledCopy()) return install::install(false);
#endif
  return service::serve();
}

}  // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++) args.push_back(platform::toUtf8(platform::Path(argv[i])));
  LocalFree(argv);
  return run(args);
}
#else
int main(int argc, char** argv) {
  const int code = run(std::vector<std::string>(argv, argv + argc));
  // Leave without running static destructors: ONNX Runtime (loaded with
  // dlopen) tears its own statics down before ours, so releasing the model
  // afterwards aborts on macOS ("mutex lock failed"), and service threads may
  // still hold global mutexes. Nothing here needs those destructors: the log
  // is closed after every line and the eval output is closed explicitly.
  std::fflush(nullptr);
  std::_Exit(code);
}
#endif
