#pragma once

#include <string>
#include <vector>

namespace util {

std::string narrow(const std::wstring& w);  // UTF-16 -> UTF-8
std::wstring widen(const std::string& s);   // UTF-8 -> UTF-16

// One string per UTF-8 character.
inline std::vector<std::string> utf8Chars(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

inline std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == sep) {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

inline std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

}  // namespace util
