// How words of Talmudic Hebrew and Aramaic are vocalized, learnt from
// vocalized texts (tools/tts/build_nikud_lexicon.py): the narrator's
// diacritizer was trained on modern Hebrew and misreads Aramaic, so words the
// lexicon knows take their marks from it and only the rest from the model.
//
// nikud_lexicon.bin: "OTZNIKUD", u32 version 1, u32 words, u32 bigrams, then
// the entries. A word: u8 n, n letters (byte = code point - 0x05D0), n
// pattern bytes. A bigram (the pattern of the second word after the first):
// u8 n1, n1 letters, u8 n2, n2 letters, n2 pattern bytes. A pattern byte is
// the diacritizer's class of the letter's marks (kClasses, 0 = none) plus
// 0x20 for a sin dot and 0x40 for a shin dot.
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "platform.h"

namespace nikud {

// The Phonikud/Dicta output classes (BertForDiacritization), in order.
constexpr int kClassCount = 29;
extern const char32_t* const kClasses[kClassCount];
constexpr unsigned char kSin = 0x20, kShin = 0x40;

class Lexicon {
 public:
  // Reads [file]; false (and the lexicon empty) when it is missing or malformed.
  bool load(const platform::Path& file);
  bool empty() const { return words_.empty(); }
  size_t words() const { return words_.size(); }
  size_t bigrams() const { return bigrams_.size(); }

  // The pattern of the plain letters [word] after the plain word [prev]
  // (empty at the start): the bigram's if known, else the word's, else null.
  const std::string* find(const std::u32string& prev, const std::u32string& word) const;

 private:
  std::unordered_map<std::u32string, std::string> words_;
  std::unordered_map<std::u32string, std::string> bigrams_;  // prev + U'\0' + word
};

// The letters [plain] with the marks of [pattern] (one byte a letter).
std::u32string apply(const std::u32string& plain, const std::string& pattern);

}  // namespace nikud
