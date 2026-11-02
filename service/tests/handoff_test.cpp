// Tests of handoff.h, the choice between the x64 and the ARM64 build. Runs on
// any machine: the processors are read from small PE headers written here.
//
//   ctest -C Release   (or run otzaria-voice-tests; exit code 0 = passed)
//
// OTZARIA_TEST_ORT=<onnxruntime.dll> also reads a real library's processor.
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "handoff.h"

namespace fs = std::filesystem;
using handoff::Arch;

static int failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

// A file that starts like a PE image for [machine].
static fs::path writePe(const fs::path& dir, const char* name, unsigned machine, bool valid = true) {
  std::vector<unsigned char> b(0x80 + 6, 0);
  b[0] = 'M';
  b[1] = 'Z';
  b[60] = 0x80;  // e_lfanew
  b[0x80] = 'P';
  b[0x81] = valid ? 'E' : 'X';
  b[0x84] = machine & 0xff;
  b[0x85] = (machine >> 8) & 0xff;
  const auto p = dir / name;
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), b.size());
  return p;
}

static std::vector<std::string> v(std::initializer_list<const char*> items) {
  return std::vector<std::string>(items.begin(), items.end());
}

int main() {
  const auto dir = fs::temp_directory_path() / "otzaria-voice-handoff-test";
  fs::create_directories(dir);

  // machine(): the PE header's processor.
  CHECK(handoff::machine(writePe(dir, "x64.dll", 0x8664)) == Arch::X64);
  CHECK(handoff::machine(writePe(dir, "arm64.dll", 0xAA64)) == Arch::Arm64);
  CHECK(handoff::machine(writePe(dir, "x86.dll", 0x014C)) == Arch::Unknown);
  CHECK(handoff::machine(writePe(dir, "broken.dll", 0x8664, false)) == Arch::Unknown);
  CHECK(handoff::machine(dir / "missing.dll") == Arch::Unknown);
  std::ofstream(dir / "short.dll", std::ios::binary) << "MZ";
  CHECK(handoff::machine(dir / "short.dll") == Arch::Unknown);
  if (const char* ort = std::getenv("OTZARIA_TEST_ORT")) {
    const Arch a = handoff::machine(ort);
    std::printf("OTZARIA_TEST_ORT is %s\n", handoff::name(a));
    CHECK(a != Arch::Unknown);
  }

  // wanted(): the other build only for a library of the other processor.
  CHECK(handoff::wanted(Arch::X64, Arch::Arm64) == Arch::Arm64);
  CHECK(handoff::wanted(Arch::Arm64, Arch::X64) == Arch::X64);
  CHECK(handoff::wanted(Arch::X64, Arch::X64) == Arch::Unknown);
  CHECK(handoff::wanted(Arch::Arm64, Arch::Arm64) == Arch::Unknown);
  CHECK(handoff::wanted(Arch::X64, Arch::Unknown) == Arch::Unknown);
  CHECK(handoff::self() != Arch::Unknown);
  CHECK(std::wstring(handoff::exeName(Arch::X64)) == L"otzaria-voice.exe");
  CHECK(std::wstring(handoff::exeName(Arch::Arm64)) == L"otzaria-voice-arm64.exe");

  // take(): the flags come out wherever they are; the command stays as it was,
  // so positional checks in main.cpp (args[2] == "--quiet", args[5] == "--raw")
  // still see the same words.
  {
    auto args = v({"exe", "--light", "m.onnx", "t.txt", "out", "--raw", "--no-handoff",
                   "--expect-version", "9.9.9"});
    const auto f = handoff::take(args);
    CHECK(f.noHandoff);
    CHECK(f.expectVersion == "9.9.9");
    CHECK(f.afterPid == 0);
    CHECK(args == v({"exe", "--light", "m.onnx", "t.txt", "out", "--raw"}));
  }
  {
    auto args = v({"exe", "--serve", "--after-pid", "1234"});
    const auto f = handoff::take(args);
    CHECK(!f.noHandoff && f.expectVersion.empty() && f.afterPid == 1234);
    CHECK(args == v({"exe", "--serve"}));
  }
  {
    auto args = v({"exe", "--install", "--quiet"});
    const auto f = handoff::take(args);
    CHECK(!f.noHandoff && f.expectVersion.empty() && f.afterPid == 0);
    CHECK(args == v({"exe", "--install", "--quiet"}));
  }
  {
    auto args = v({"exe", "--expect-version"});  // no value: left as it is
    handoff::take(args);
    CHECK(args == v({"exe", "--expect-version"}));
  }

  // forward(): the same command, or --serve for none, then the flags; and
  // take() on the other side gives the command back.
  {
    const auto out = handoff::forward(v({"exe"}), "2.5.2");
    CHECK(out == v({"--serve", "--no-handoff", "--expect-version", "2.5.2"}));
    auto back = v({"other"});
    back.insert(back.end(), out.begin(), out.end());
    const auto f = handoff::take(back);
    CHECK(f.noHandoff && f.expectVersion == "2.5.2");
    CHECK(back == v({"other", "--serve"}));
  }
  {
    const auto out = handoff::forward(v({"exe", "--eval", "a b.jsonl", "out.jsonl"}), "2.5.2");
    CHECK(out == v({"--eval", "a b.jsonl", "out.jsonl", "--no-handoff", "--expect-version", "2.5.2"}));
  }

  // quote(): as CommandLineToArgvW reads it.
  CHECK(handoff::quote(L"plain") == L"plain");
  CHECK(handoff::quote(L"") == L"\"\"");
  CHECK(handoff::quote(L"C:\\Program Files\\x.exe") == L"\"C:\\Program Files\\x.exe\"");
  CHECK(handoff::quote(L"a\"b") == L"\"a\\\"b\"");
  CHECK(handoff::quote(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
  CHECK(handoff::quote(L"x\\\\\"y z") == L"\"x\\\\\\\\\\\"y z\"");

  std::error_code ec;
  fs::remove_all(dir, ec);
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("handoff tests passed\n");
  return 0;
}
