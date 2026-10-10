// Linux and macOS: installed per user, without administrator rights.
//   Linux  ~/.local/lib/otzaria-voice/otzaria-voice, started at login by an XDG
//          autostart entry (~/.config/autostart/otzaria-voice.desktop), which
//          every common desktop (GNOME, KDE, Xfce, Cinnamon, MATE...) honours.
//   macOS  ~/Library/Application Support/OtzariaVoice/otzaria-voice, started at
//          login by a LaunchAgent (~/Library/LaunchAgents/org.otzaria.voice.plist).
// An ONNX Runtime library lying next to the program (the installer's copy) is
// installed with it.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "install.h"
#include "service.h"

namespace install {

namespace fs = std::filesystem;
using platform::Path;

namespace {

Path home() {
  auto h = platform::env("HOME");
  return h.empty() ? Path("/tmp") : Path(h);
}

Path installDir() {
#ifdef __APPLE__
  return home() / "Library" / "Application Support" / "OtzariaVoice";
#else
  auto x = platform::env("XDG_DATA_HOME");  // ~/.local/share -> ~/.local/lib next to it
  return (x.empty() ? home() / ".local" / "lib" : Path(x).parent_path() / "lib") / "otzaria-voice";
#endif
}

#ifdef __APPLE__
const char* kLabel = "org.otzaria.voice";
Path agentFile() { return home() / "Library" / "LaunchAgents" / (std::string(kLabel) + ".plist"); }
std::string guiDomain() { return "gui/" + std::to_string(getuid()); }
#else
Path autostartFile() {
  auto x = platform::env("XDG_CONFIG_HOME");
  return (x.empty() ? home() / ".config" : Path(x)) / "autostart" / "otzaria-voice.desktop";
}
#endif

void say(bool quiet, const std::string& text) {
  if (!quiet) std::cout << text << std::endl;
}

bool isRuntimeLibrary(const Path& p) {
  const auto name = p.filename().string();
  return name.rfind("libonnxruntime", 0) == 0;
}

// Starts [exe] --serve in its own session, detached from this terminal.
void launchDetached(const Path& exe) {
  const pid_t pid = fork();
  if (pid != 0) return;
  setsid();
  const int null = open("/dev/null", O_RDWR);
  if (null >= 0) {
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
  }
  const std::string path = exe.string();
  execl(path.c_str(), path.c_str(), "--serve", static_cast<char*>(nullptr));
  _exit(127);
}

}  // namespace

Path installedExe() { return installDir() / "otzaria-voice"; }

int install(bool quiet) {
  const Path target = installedExe();
  const Path self = platform::exePath();
  service::postQuit();  // a running copy is replaced
  std::error_code ec;
  fs::create_directories(installDir(), ec);
  if (!fs::equivalent(self, target, ec)) {
    fs::copy_file(self, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      std::cerr << "install failed: cannot copy to " << target << ": " << ec.message() << std::endl;
      platform::log("install: copy failed");
      return 1;
    }
    // ONNX Runtime comes as one file plus version symlinks: keep the links.
    for (const auto& entry : fs::directory_iterator(self.parent_path(), ec)) {
      if (!isRuntimeLibrary(entry.path())) continue;
      const Path dest = installDir() / entry.path().filename();
      std::error_code e2;
      fs::remove(dest, e2);
      if (fs::is_symlink(entry.symlink_status(e2)))
        fs::copy_symlink(entry.path(), dest, e2);
      else
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, e2);
    }
  }
  fs::permissions(target, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec,
                  ec);
#ifdef __APPLE__
  fs::create_directories(agentFile().parent_path(), ec);
  {
    std::ofstream f(agentFile(), std::ios::binary | std::ios::trunc);
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n<dict>\n"
         "  <key>Label</key><string>" << kLabel << "</string>\n"
         "  <key>ProgramArguments</key>\n  <array>\n"
         "    <string>" << target.string() << "</string>\n"
         "    <string>--serve</string>\n  </array>\n"
         "  <key>RunAtLoad</key><true/>\n"
         "  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n"
         "  <key>ProcessType</key><string>Interactive</string>\n"
         "</dict>\n</plist>\n";
  }
  platform::run({"launchctl", "bootout", guiDomain() + "/" + kLabel});
  if (platform::run({"launchctl", "bootstrap", guiDomain(), agentFile().string()}) != 0) {
    platform::run({"launchctl", "load", "-w", agentFile().string()});  // before macOS 10.11
  }
#else
  fs::create_directories(autostartFile().parent_path(), ec);
  {
    std::ofstream f(autostartFile(), std::ios::binary | std::ios::trunc);
    f << "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=Otzaria Voice\n"
         "Name[he]=\xD7\xA9\xD7\x99\xD7\xA8\xD7\x95\xD7\xAA \xD7\x94\xD7\x93\xD7\x99\xD7\x91\xD7\x95\xD7\xA8 "
         "\xD7\xA9\xD7\x9C \xD7\x90\xD7\x95\xD7\xA6\xD7\xA8\xD7\x99\xD7\x90\n"  // שירות הדיבור של אוצריא
         "Comment=Local Hebrew speech recognition for Otzaria\n"
         "Exec=\"" << target.string() << "\" --serve\n"
         "Terminal=false\n"
         "NoDisplay=true\n"
         "X-GNOME-Autostart-enabled=true\n";
  }
  launchDetached(target);
#endif
  platform::log(std::string("installed ") + service::kVersion + " for login start");
  say(quiet, "Otzaria voice service installed: it now starts with every login.");
  return 0;
}

int uninstall(bool quiet) {
  std::error_code ec;
#ifdef __APPLE__
  platform::run({"launchctl", "bootout", guiDomain() + "/" + kLabel});
  fs::remove(agentFile(), ec);
#else
  fs::remove(autostartFile(), ec);
#endif
  service::postQuit();
  platform::log("uninstalled");
  fs::remove(installedExe(), ec);
  for (const auto& entry : fs::directory_iterator(installDir(), ec))
    if (isRuntimeLibrary(entry.path())) fs::remove(entry.path(), ec);
  fs::remove(installDir(), ec);  // only when empty
  say(quiet, "Otzaria voice service removed. The voice pack stays in Otzaria's data folder.");
  return 0;
}

}  // namespace install
