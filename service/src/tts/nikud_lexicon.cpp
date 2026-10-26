#include "nikud_lexicon.h"

#include <cstdint>
#include <fstream>
#include <sstream>

namespace nikud {

// Written as escapes so the order (dagesh, then the vowel) is plain to see:
// the speech model takes every mark as a token, so the order must not vary.
const char32_t* const kClasses[kClassCount] = {
    U"", U"", U"ּ", U"ְ", U"ֱ", U"ֲ", U"ֳ", U"ִ",
    U"ֵ", U"ֶ", U"ַ", U"ָ", U"ֹ", U"ֺ", U"ֻ",
    U"ְּ", U"ֱּ", U"ֲּ", U"ֳּ", U"ִּ",
    U"ֵּ", U"ֶּ", U"ַּ", U"ָּ", U"ֹּ",
    U"ֺּ", U"ֻּ", U"ׇ", U"ׇּ"};

bool Lexicon::load(const platform::Path& file) {
  words_.clear();
  bigrams_.clear();
  std::ifstream f(file, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string data = ss.str();
  size_t at = 0;
  auto u32 = [&](uint32_t& v) {
    if (at + 4 > data.size()) return false;
    v = uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 | uint32_t(uint8_t(data[at + 2])) << 16 |
        uint32_t(uint8_t(data[at + 3])) << 24;
    at += 4;
    return true;
  };
  auto letters = [&](std::u32string& out) {
    if (at >= data.size()) return false;
    const size_t n = uint8_t(data[at++]);
    if (n == 0 || at + n > data.size()) return false;
    out.clear();
    for (size_t i = 0; i < n; i++) out.push_back(char32_t(0x05D0 + uint8_t(data[at + i])));
    at += n;
    return true;
  };
  auto pattern = [&](size_t n, std::string& out) {
    if (at + n > data.size()) return false;
    out.assign(data, at, n);
    at += n;
    return true;
  };
  uint32_t version = 0, words = 0, bigrams = 0;
  if (data.compare(0, 8, "OTZNIKUD") != 0) return false;
  at = 8;
  if (!u32(version) || version != 1 || !u32(words) || !u32(bigrams)) return false;
  words_.reserve(words);
  bigrams_.reserve(bigrams);
  std::u32string a, b;
  std::string pat;
  for (uint32_t i = 0; i < words; i++) {
    if (!letters(a) || !pattern(a.size(), pat)) return load(platform::Path()), false;
    words_.emplace(a, pat);
  }
  for (uint32_t i = 0; i < bigrams; i++) {
    if (!letters(a) || !letters(b) || !pattern(b.size(), pat)) return load(platform::Path()), false;
    bigrams_.emplace(a + U'\0' + b, pat);
  }
  return true;
}

const std::string* Lexicon::find(const std::u32string& prev, const std::u32string& word) const {
  if (!prev.empty()) {
    const auto it = bigrams_.find(prev + U'\0' + word);
    if (it != bigrams_.end()) return &it->second;
  }
  const auto it = words_.find(word);
  return it == words_.end() ? nullptr : &it->second;
}

std::u32string apply(const std::u32string& plain, const std::string& pattern) {
  std::u32string out;
  for (size_t i = 0; i < plain.size() && i < pattern.size(); i++) {
    const unsigned char p = uint8_t(pattern[i]);
    out.push_back(plain[i]);
    if (p & kShin) out.push_back(0x05C1);
    if (p & kSin) out.push_back(0x05C2);
    const int cls = p & 0x1F;
    if (cls < kClassCount) out += kClasses[cls];
  }
  return out;
}

}  // namespace nikud
