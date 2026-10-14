// Linux and macOS: the system's curl downloads (resuming with -C -), a portable
// SHA-256 verifies, and unzip (or bsdtar) unpacks. curl and unzip come with
// macOS and with nearly every Linux desktop; the docs list them as requirements.
#include "pack.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "sha256.h"

namespace pack::sys {

namespace fs = std::filesystem;

namespace {
uint64_t sizeOf(const Path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : uint64_t(n);
}
}  // namespace

void download(const std::string& url, const Path& part, uint64_t expected,
              const OnProgress& progress) {
  std::error_code ec;
  if (sizeOf(part) > expected) fs::remove(part, ec);
  if (sizeOf(part) == expected) return;

  // curl writes the file itself; a watcher reports its growth.
  std::atomic<bool> done{false};
  std::thread watch([&] {
    while (!done) {
      progress(sizeOf(part));
      for (int i = 0; i < 5 && !done; i++) platform::sleepMs(100);
    }
  });
  auto attempt = [&](bool resume) {
    std::vector<std::string> args = {"curl", "--fail", "--location", "--silent", "--show-error",
                                     "--connect-timeout", "15", "--speed-time", "60",
                                     "--speed-limit", "1", "--retry", "2"};
    if (resume) {
      args.push_back("--continue-at");
      args.push_back("-");
    }
    args.push_back("--output");
    args.push_back(part.string());
    args.push_back(url);
    return platform::run(args);
  };
  int code = attempt(sizeOf(part) > 0);
  if (code == 33) {  // the server cannot resume: start over
    fs::remove(part, ec);
    code = attempt(false);
  }
  done = true;
  watch.join();
  progress(sizeOf(part));
  if (code == -1 || code == 127)
    throw std::runtime_error("התוכנה curl אינה מותקנת במחשב; התקינו אותה ונסו שוב");
  if (code == 6 || code == 7)
    throw std::runtime_error("אין חיבור לשרת ההורדה");
  if (code == 22)
    throw std::runtime_error("שרת ההורדה השיב בשגיאה");
  if (code != 0)
    throw std::runtime_error("החיבור לאינטרנט נקטע; לחצו שוב והורדה תמשיך מאותה נקודה (" +
                             std::to_string(code) + ")");
  if (sizeOf(part) != expected)
    throw std::runtime_error("ההורדה לא הושלמה; לחצו שוב והיא תמשיך מאותה נקודה");
}

std::string hashFile(const Path& file, const OnProgress& progress) {
  std::ifstream f(file, std::ios::binary);
  if (!f) throw std::runtime_error("לא ניתן לקרוא את הקובץ שהורד");
  PortableSha256 sha;
  std::vector<char> buf(1 << 20);
  uint64_t done = 0;
  while (f) {
    f.read(buf.data(), std::streamsize(buf.size()));
    const auto n = size_t(f.gcount());
    if (!n) break;
    sha.add(buf.data(), n);
    done += n;
    progress(done);
  }
  return sha.hex();
}

void extract(const Path& zip, const Path& to) {
  int code = platform::run({"unzip", "-q", "-o", zip.string(), "-d", to.string()});
  if (code == -1 || code == 127)  // no unzip: bsdtar reads zip archives (macOS's tar is bsdtar)
    code = platform::run({"bsdtar", "-xf", zip.string(), "-C", to.string()});
#ifdef __APPLE__
  if (code == -1 || code == 127) code = platform::run({"tar", "-xf", zip.string(), "-C", to.string()});
#endif
  if (code == -1 || code == 127)
    throw std::runtime_error("התוכנה unzip אינה מותקנת במחשב; התקינו אותה ונסו שוב");
  if (code != 0) throw std::runtime_error("פתיחת החבילה נכשלה (" + std::to_string(code) + ")");
}

}  // namespace pack::sys
