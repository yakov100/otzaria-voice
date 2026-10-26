// UTF-8 <-> code points, and what counts as a Hebrew letter or a mark, shared
// by the diacritizer, the narrator and the light voice. Header-only.
#pragma once

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "platform.h"

namespace hebrew {

inline std::u32string toU32(const std::string& s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
    else { i++; continue; }  // stray continuation byte
    if (i + n > s.size()) break;
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

inline std::string encode(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

inline std::string encode(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) out += encode(c);
  return out;
}

inline bool isLetter(char32_t c) { return c >= 0x05D0 && c <= 0x05EA; }
// Nikud proper (not teamim): sheva .. qubuts, dagesh, shin/sin dots, qamats qatan.
inline bool isNikud(char32_t c) { return (c >= 0x05B0 && c <= 0x05BC) || c == 0x05C1 || c == 0x05C2 || c == 0x05C7; }
// Any combining mark of the Hebrew block: teamim, nikud, meteg, rafe, dots.
inline bool isMark(char32_t c) {
  return (c >= 0x0591 && c <= 0x05BD) || c == 0x05BF || c == 0x05C1 || c == 0x05C2 || c == 0x05C4 ||
         c == 0x05C5 || c == 0x05C7;
}
inline bool isSpace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0; }

// The Unicode canonical combining class of a Hebrew mark (0 for anything
// else). NFD orders the marks after a letter by it, with a stable sort.
inline int combiningClass(char32_t c) {
  static const unsigned char kTeamim[0x1F] = {  // U+0591 .. U+05AF
      220, 230, 230, 230, 230, 220, 230, 230, 230, 222, 220, 230, 230, 230, 230, 230,
      230, 220, 220, 220, 220, 220, 220, 230, 230, 220, 230, 230, 222, 228, 230};
  if (c >= 0x0591 && c <= 0x05AF) return kTeamim[c - 0x0591];
  if (c >= 0x05B0 && c <= 0x05B8) return 10 + int(c - 0x05B0);  // sheva 10 .. qamats 18
  switch (c) {
    case 0x05B9: case 0x05BA: return 19;  // holam
    case 0x05BB: return 20;               // qubuts
    case 0x05BC: return 21;               // dagesh
    case 0x05BD: return 22;               // meteg
    case 0x05BF: return 23;               // rafe
    case 0x05C1: return 24;               // shin dot
    case 0x05C2: return 25;               // sin dot
    case 0x05C4: return 230;
    case 0x05C5: return 220;
    case 0x05C7: return 18;               // qamats qatan
    default: return 0;
  }
}

// The text with the marks after each letter in canonical order (what
// Python's unicodedata.normalize("NFD") gives for text made of letters and
// combining marks; nothing here is decomposed). The light voice was trained
// on NFD text, so it must see the same order at run time.
inline std::u32string canonicalOrder(std::u32string s) {
  for (size_t i = 0; i < s.size();) {
    if (combiningClass(s[i]) == 0) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < s.size() && combiningClass(s[j]) != 0) j++;
    std::stable_sort(s.begin() + long(i), s.begin() + long(j),
                     [](char32_t a, char32_t b) { return combiningClass(a) < combiningClass(b); });
    i = j;
  }
  return s;
}

inline std::string readFile(const platform::Path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("missing file: " + platform::toUtf8(path));
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace hebrew
