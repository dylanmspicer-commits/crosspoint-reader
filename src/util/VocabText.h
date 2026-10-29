#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "WordDetect.h"

// Text side of a book's vocabulary list: the line format of the per-book file
// and the short gloss shown under each word. One entry per line, "word\tgloss";
// neither field can hold a tab or newline once sanitized.
namespace vocabText {

inline constexpr size_t MAX_WORD_BYTES = 64;
inline constexpr size_t MAX_GLOSS_BYTES = 96;

struct Entry {
  std::string word;
  std::string gloss;
};

inline bool hasIdeograph(const std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    if (wordDetect::isIdeograph(wordDetect::nextCodepoint(text, i))) return true;
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

inline std::string formatLine(const Entry& entry) { return entry.word + '\t' + entry.gloss + '\n'; }

// Every entry in a vocabulary file's text, in saved order. Blank lines are skipped.
inline std::vector<Entry> parse(const std::string_view text) {
  std::vector<Entry> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view line = text.substr(start, end - start);
    const size_t tab = line.find('\t');
    Entry entry;
    entry.word = std::string(line.substr(0, tab));
    if (tab != std::string_view::npos) entry.gloss = std::string(line.substr(tab + 1));
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

}  // namespace vocabText
