// Android: the app's Java side downloads (HttpURLConnection) and unpacks
// (java.util.zip); jni.cpp passes those in. Verification is the portable SHA-256.
#include "pack.h"

#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "sha256.h"

namespace pack {

namespace {
std::mutex hooksMu;
AndroidHooks hooks;
}  // namespace

void setAndroidHooks(AndroidHooks h) {
  std::lock_guard<std::mutex> lock(hooksMu);
  hooks = std::move(h);
}

namespace sys {

void download(const std::string& url, const Path& part, uint64_t expected,
              const OnProgress& progress) {
  decltype(hooks.download) fn;
  {
    std::lock_guard<std::mutex> lock(hooksMu);
    fn = hooks.download;
  }
  if (!fn) throw std::runtime_error("ההורדה אינה זמינה");
  fn(url, part.string(), expected, progress);
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
  decltype(hooks.extract) fn;
  {
    std::lock_guard<std::mutex> lock(hooksMu);
    fn = hooks.extract;
  }
  if (!fn) throw std::runtime_error("פתיחת החבילה אינה זמינה");
  fn(zip.string(), to.string());
}

}  // namespace sys
}  // namespace pack
