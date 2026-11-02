#include "handoff.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>

namespace handoff {

const char* name(Arch a) {
  switch (a) {
    case Arch::X64: return "x64";
    case Arch::Arm64: return "arm64";
    default: return "unknown";
  }
}

Arch self() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return Arch::Arm64;
#elif defined(_M_X64) || defined(__x86_64__)
  return Arch::X64;
#else
  return Arch::Unknown;
#endif
}

Arch machine(const platform::Path& file) {
  std::ifstream f(file, std::ios::binary);
  unsigned char dos[64];
  if (!f.read(reinterpret_cast<char*>(dos), sizeof dos) || dos[0] != 'M' || dos[1] != 'Z')
    return Arch::Unknown;
  const uint32_t pe = dos[60] | dos[61] << 8 | dos[62] << 16 | uint32_t(dos[63]) << 24;
  unsigned char head[6];
  if (!f.seekg(pe) || !f.read(reinterpret_cast<char*>(head), sizeof head) || head[0] != 'P' ||
      head[1] != 'E' || head[2] || head[3])
    return Arch::Unknown;
  switch (head[4] | head[5] << 8) {
    case 0x8664: return Arch::X64;
    case 0xAA64: return Arch::Arm64;
    default: return Arch::Unknown;
  }
}

const wchar_t* exeName(Arch a) {
  return a == Arch::Arm64 ? L"otzaria-voice-arm64.exe" : L"otzaria-voice.exe";
}

Arch wanted(Arch self, Arch library) {
  return library == Arch::Unknown || library == self ? Arch::Unknown : library;
}

Flags take(std::vector<std::string>& args) {
  Flags flags;
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); i++) {
    if (i > 0 && args[i] == "--no-handoff") {
      flags.noHandoff = true;
    } else if (i > 0 && args[i] == "--expect-version" && i + 1 < args.size()) {
      flags.expectVersion = args[++i];
    } else if (i > 0 && args[i] == "--after-pid" && i + 1 < args.size()) {
      flags.afterPid = strtoul(args[++i].c_str(), nullptr, 10);
    } else {
      rest.push_back(args[i]);
    }
  }
  args = rest;
  return flags;
}

std::vector<std::string> forward(const std::vector<std::string>& args, const std::string& version) {
  std::vector<std::string> out(args.size() > 1 ? args.begin() + 1 : args.end(), args.end());
  if (out.empty()) out.push_back("--serve");
  out.insert(out.end(), {"--no-handoff", "--expect-version", version});
  return out;
}

std::wstring quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      slashes++;
    } else if (c == L'"') {
      out.append(slashes * 2 + 1, L'\\');
      slashes = 0;
    } else {
      out.append(slashes, L'\\');
      slashes = 0;
    }
    if (c != L'\\') out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

}  // namespace handoff
