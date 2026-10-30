#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Picks the dictionary word under a long-press in CJK text, where each character
// is its own layout token and nothing marks where a word ends.
//
// The caller hands over the run of tokens around the pressed one (in reading
// order, already cut at paragraph or source gaps, and across line or page breaks)
// plus the pressed index. candidates() lists every span of 2..MAX_CHARS ideographs
// that contains the pressed token; the caller checks them against its
// dictionaries in one batch and choose() keeps the longest hit, so an idiom wins
// over the two-character word inside it.
namespace wordDetect {

inline constexpr size_t MAX_CHARS = 6;

struct Token {
  std::string_view text;
};

struct Candidate {
  size_t first = 0;  // token indices into the run, inclusive
  size_t last = 0;
  std::string text;  // the ideographs of the span, without edge punctuation
};

struct Span {
  size_t first = 0;
  size_t last = 0;
};

inline bool isIdeograph(const uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFF) || cp == 0x3007;
}

// Decodes one UTF-8 codepoint at text[i] and advances i. Invalid bytes decode as
// U+FFFD so they never count as ideographs.
inline uint32_t nextCodepoint(const std::string_view text, size_t& i) {
  const auto byte = [&text](const size_t at) { return static_cast<uint8_t>(text[at]); };
  const uint8_t lead = byte(i);
  size_t extra = 0;
  uint32_t cp = 0;
  if (lead < 0x80) {
    ++i;
    return lead;
  } else if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    cp = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    cp = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    cp = lead & 0x07;
  } else {
    ++i;
    return 0xFFFD;
  }
  if (i + extra >= text.size()) {  // sequence cut off by the end of the token
    i = text.size();
    return 0xFFFD;
  }
  for (size_t k = 1; k <= extra; ++k) {
    if ((byte(i + k) & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (byte(i + k) & 0x3F);
  }
  i += extra + 1;
  return cp;
}

inline bool hasIdeograph(const std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    if (isIdeograph(nextCodepoint(text, i))) return true;
  }
  return false;
}

// How a token can join a run: its single ideograph, and whether punctuation on
// either side stops the run from growing past it (e.g. "“中" or "国。").
struct TokenShape {
  std::string_view ideograph;  // empty when the token is not exactly one ideograph
  bool openLeft = false;
  bool openRight = false;
};

inline TokenShape shapeOf(const std::string_view text) {
  TokenShape shape;
  size_t i = 0;
  size_t ideographStart = 0;
  size_t ideographEnd = 0;
  size_t ideographs = 0;
  bool before = false;
  bool after = false;
  while (i < text.size()) {
    const size_t start = i;
    const uint32_t cp = nextCodepoint(text, i);
    if (isIdeograph(cp)) {
      if (++ideographs > 1 || after) return {};
      ideographStart = start;
      ideographEnd = i;
    } else if (ideographs == 0) {
      before = true;
    } else {
      after = true;
    }
  }
  if (ideographs != 1) return {};
  shape.ideograph = text.substr(ideographStart, ideographEnd - ideographStart);
  shape.openLeft = !before;
  shape.openRight = !after;
  return shape;
}

struct ByteRange {
  size_t begin = 0;
  size_t end = 0;
};

// The part of a token to highlight when it ends a selection: its ideograph
// without the punctuation attached on the trimmed sides ("国。" -> "国").
// The whole token when it is not a single ideograph.
inline ByteRange highlightRange(const std::string_view text, const bool trimLeft, const bool trimRight) {
  ByteRange range{0, text.size()};
  const TokenShape shape = shapeOf(text);
  if (shape.ideograph.empty()) return range;
  const size_t at = static_cast<size_t>(shape.ideograph.data() - text.data());
  if (trimLeft) range.begin = at;
  if (trimRight) range.end = at + shape.ideograph.size();
  return range;
}

// The ideographs of a selection, without any punctuation: tokens with no
// ideograph at either end are dropped, and every other token must be a single
// ideograph. Empty when the selection is not Chinese text.
inline std::string ideographText(const std::vector<Token>& tokens) {
  size_t first = 0;
  size_t last = tokens.size();
  while (first < last && !hasIdeograph(tokens[first].text)) ++first;
  while (last > first && !hasIdeograph(tokens[last - 1].text)) --last;
  std::string out;
  for (size_t i = first; i < last; ++i) {
    const TokenShape shape = shapeOf(tokens[i].text);
    if (shape.ideograph.empty()) return {};
    out.append(shape.ideograph);
  }
  return out;
}

// Every span of 2..MAX_CHARS ideograph tokens that contains run[hit] and stays
// inside punctuation boundaries. Empty when run[hit] is not a single ideograph.
inline std::vector<Candidate> candidates(const std::vector<Token>& run, const size_t hit) {
  std::vector<Candidate> out;
  if (hit >= run.size()) return out;
  std::vector<TokenShape> shapes;
  shapes.reserve(run.size());
  for (const auto& token : run) shapes.push_back(shapeOf(token.text));
  if (shapes[hit].ideograph.empty()) return out;

  // How far the run reaches on each side of the hit before a non-ideograph token
  // or edge punctuation stops it.
  size_t lo = hit;
  while (lo > 0 && hit - lo + 1 < MAX_CHARS && shapes[lo].openLeft && !shapes[lo - 1].ideograph.empty() &&
         shapes[lo - 1].openRight) {
    --lo;
  }
  size_t hi = hit;
  while (hi + 1 < run.size() && hi - hit + 1 < MAX_CHARS && shapes[hi].openRight && !shapes[hi + 1].ideograph.empty() &&
         shapes[hi + 1].openLeft) {
    ++hi;
  }

  for (size_t first = lo; first <= hit; ++first) {
    for (size_t last = hit; last <= hi; ++last) {
      const size_t length = last - first + 1;
      if (length < 2 || length > MAX_CHARS) continue;
      Candidate candidate;
      candidate.first = first;
      candidate.last = last;
      for (size_t k = first; k <= last; ++k) candidate.text.append(shapes[k].ideograph);
      out.push_back(std::move(candidate));
    }
  }
  return out;
}

// The longest found candidate; ties go to the one starting earliest, which reads
// most naturally in left-to-right text. Falls back to the pressed token alone.
inline Span choose(const std::vector<Candidate>& list, const std::vector<bool>& found, const size_t hit) {
  Span best{hit, hit};
  size_t bestLength = 1;
  for (size_t i = 0; i < list.size() && i < found.size(); ++i) {
    if (!found[i]) continue;
    const size_t length = list[i].last - list[i].first + 1;
    if (length > bestLength || (length == bestLength && length > 1 && list[i].first < best.first)) {
      best = {list[i].first, list[i].last};
      bestLength = length;
    }
  }
  return best;
}

}  // namespace wordDetect
