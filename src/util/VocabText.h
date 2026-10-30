#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "WordDetect.h"

// Text side of a book's vocabulary list: the line format of the per-book file
// and the short gloss shown under each word. One entry per line:
//   word \t gloss \t sentence \t spineIndex \t page \t textOffset
// Lines written before the sentence and position were added have only the
// first two fields. Sanitized fields never hold a tab or newline.
namespace vocabText {

inline constexpr size_t MAX_WORD_BYTES = 64;
inline constexpr size_t MAX_GLOSS_BYTES = 96;
inline constexpr size_t MAX_SENTENCE_BYTES = 200;
// Longest saved word in characters: MAX_WORD_BYTES of 3-byte CJK characters.
inline constexpr size_t MAX_WORD_CHARS = MAX_WORD_BYTES / 3;
// Longest line formatLine() writes (three text fields, three numbers, separators), with margin.
inline constexpr size_t MAX_LINE_BYTES = 512;
inline constexpr uint32_t NO_POSITION = UINT32_MAX;

struct Entry {
  std::string word;
  std::string gloss;
  std::string sentence;  // where the word was found; empty when unknown
  uint32_t spineIndex = NO_POSITION;
  uint32_t page = NO_POSITION;
  uint32_t textOffset = NO_POSITION;  // visible-text offset in the chapter, as clippings use

  bool hasPosition() const { return spineIndex != NO_POSITION && page != NO_POSITION; }
};

inline bool hasIdeograph(const std::string_view text) { return wordDetect::hasIdeograph(text); }

// True when a layout token ends a sentence: it holds 。！？ (or ASCII ! ?),
// possibly with a closing quote attached, as in "了。”".
inline bool endsSentence(const std::string_view token) {
  size_t i = 0;
  while (i < token.size()) {
    const uint32_t cp = wordDetect::nextCodepoint(token, i);
    if (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == '!' || cp == '?') return true;
  }
  return false;
}

// Cut text to at most maxBytes without splitting a UTF-8 sequence.
inline void truncateUtf8(std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  text.resize(cut);
}

// Tabs, newlines and control bytes become single spaces; edges are trimmed.
inline std::string sanitize(const std::string_view text, const size_t maxBytes) {
  std::string out;
  out.reserve(std::min(text.size(), maxBytes + 4));
  bool space = false;
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < 0x20 || c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  truncateUtf8(out, maxBytes);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

inline std::string formatLine(const Entry& entry) {
  const auto number = [](const uint32_t value) { return value == NO_POSITION ? std::string() : std::to_string(value); };
  return entry.word + '\t' + entry.gloss + '\t' + entry.sentence + '\t' + number(entry.spineIndex) + '\t' +
         number(entry.page) + '\t' + number(entry.textOffset) + '\n';
}

// A decimal field, or NO_POSITION when empty or not a number.
inline uint32_t parseNumber(const std::string_view field) {
  if (field.empty() || field.size() > 10) return NO_POSITION;
  uint64_t value = 0;
  for (const char c : field) {
    if (c < '0' || c > '9') return NO_POSITION;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value >= NO_POSITION ? NO_POSITION : static_cast<uint32_t>(value);
}

// Every entry in a vocabulary file's text, in saved order. Blank lines are skipped.
inline std::vector<Entry> parse(const std::string_view text) {
  std::vector<Entry> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(start, end - start);
    std::string_view fields[6];
    size_t count = 0;
    while (count < 6) {
      const size_t tab = line.find('\t');
      fields[count++] = line.substr(0, tab);
      if (tab == std::string_view::npos) break;
      line.remove_prefix(tab + 1);
    }
    Entry entry;
    entry.word = std::string(fields[0]);
    entry.gloss = std::string(fields[1]);
    entry.sentence = std::string(fields[2]);
    entry.spineIndex = parseNumber(fields[3]);
    entry.page = parseNumber(fields[4]);
    entry.textOffset = parseNumber(fields[5]);
    if (!entry.word.empty()) out.push_back(std::move(entry));
    start = end + 1;
  }
  return out;
}

inline bool contains(const std::string_view text, const std::string_view word) {
  for (const auto& entry : parse(text)) {
    if (entry.word == word) return true;
  }
  return false;
}

// One-line summary of a plain-text definition: its first two lines of content,
// such as a pinyin line and the first sense, joined with " · ". Heading lines
// (headword labels and dictionary names, marked with headingMarker) and lines
// that only repeat the word are skipped; sectionMarker bytes are ignored.
inline std::string makeGloss(const std::string_view definition, const std::string_view word, const char headingMarker,
                             const char sectionMarker) {
  std::string gloss;
  int taken = 0;
  size_t start = 0;
  while (start < definition.size() && taken < 2) {
    size_t end = definition.find('\n', start);
    if (end == std::string_view::npos) end = definition.size();
    std::string_view line = definition.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && line.front() == sectionMarker) line.remove_prefix(1);
    if (!line.empty() && line.front() == headingMarker) continue;
    // Headroom past the cap so an overlong gloss is detected and marked below.
    const std::string cleaned = sanitize(line, MAX_GLOSS_BYTES + 1);
    if (cleaned.empty() || cleaned == word) continue;
    if (!gloss.empty()) gloss += " \xC2\xB7 ";  // " · "
    gloss += cleaned;
    ++taken;
  }
  if (gloss.size() > MAX_GLOSS_BYTES) {
    truncateUtf8(gloss, MAX_GLOSS_BYTES - 3);
    while (!gloss.empty() && gloss.back() == ' ') gloss.pop_back();
    gloss += "\xE2\x80\xA6";  // "…"
  }
  return gloss;
}

// Which tokens of a page belong to a saved word. joinedBefore[i] is true when
// token i continues token i-1 with no gap in the text, across a line break too.
// Words are matched longest first and never across punctuation, so 国。家 does
// not match 国家.
// saved must be sorted.
inline std::vector<bool> markSavedWords(const std::vector<wordDetect::Token>& tokens,
                                        const std::vector<bool>& joinedBefore, const std::vector<std::string>& saved,
                                        const size_t maxChars) {
  std::vector<bool> marked(tokens.size(), false);
  if (saved.empty() || maxChars == 0) return marked;
  std::vector<wordDetect::TokenShape> shapes;
  shapes.reserve(tokens.size());
  for (const auto& token : tokens) shapes.push_back(wordDetect::shapeOf(token.text));
  std::string candidate;
  candidate.reserve(maxChars * 4);
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (shapes[i].ideograph.empty()) continue;
    // How far the run of characters reaches from token i.
    size_t reach = 1;
    while (reach < maxChars && i + reach < tokens.size() && joinedBefore[i + reach] &&
           shapes[i + reach - 1].openRight && !shapes[i + reach].ideograph.empty() && shapes[i + reach].openLeft) {
      ++reach;
    }
    for (size_t length = reach; length >= 1; --length) {
      candidate.clear();
      for (size_t k = i; k < i + length; ++k) candidate.append(shapes[k].ideograph);
      if (!std::binary_search(saved.begin(), saved.end(), candidate)) continue;
      for (size_t k = i; k < i + length; ++k) marked[k] = true;
      i += length - 1;
      break;
    }
  }
  return marked;
}

}  // namespace vocabText
