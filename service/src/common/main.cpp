// Otzaria voice service: command line.
//
//   otzaria-voice --serve               run the service (what login start runs)
//   otzaria-voice --install [--quiet]   install for this user, start at login, run now
//   otzaria-voice --uninstall [--quiet] stop, remove login start and program files
//   otzaria-voice --eval <manifest.jsonl> <out.jsonl>   accuracy evaluation
//   otzaria-voice --narrate <models> <voice> <text.txt> <out dir>   reading-aloud test
//   otzaria-voice --version
//
// Windows: started without arguments (double-clicked from a download) it
// installs itself; the installed copy, started at login, serves. Elsewhere no
// arguments means --serve.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

int run(const std::vector<std::string>& args) {
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
  if (arg == "--vocalize" && args.size() > 4) {
    try {
      return service::vocalizeTest(platform::fromUtf8(args[2]), platform::fromUtf8(args[3]),
                                   platform::fromUtf8(args[4]));
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
  if (_wcsicmp(platform::exePath().c_str(), install::installedExe().c_str()) != 0)
    return install::install(false);
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
