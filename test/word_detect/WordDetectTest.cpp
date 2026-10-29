#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "util/WordDetect.h"

namespace {

using wordDetect::Candidate;
using wordDetect::Span;
using wordDetect::Token;

std::vector<Token> tokens(const std::vector<std::string>& texts) {
  std::vector<Token> run;
  for (const auto& text : texts) run.push_back({text});
  return run;
}

// Runs the full detection against a fake dictionary.
Span detect(const std::vector<std::string>& texts, const size_t hit, const std::set<std::string>& dictionary) {
  const auto run = tokens(texts);
  const auto list = wordDetect::candidates(run, hit);
  std::vector<bool> found;
  for (const auto& candidate : list) found.push_back(dictionary.count(candidate.text) != 0);
  return wordDetect::choose(list, found, hit);
}

}  // namespace

TEST(WordDetect, PicksTwoCharacterWordFromEitherCharacter) {
  const std::vector<std::string> text = {"我", "在", "图", "书", "馆", "看", "书"};
  const std::set<std::string> dict = {"我", "图书", "图书馆", "看书"};
  Span span = detect(text, 2, dict);
  EXPECT_EQ(span.first, 2u);
  EXPECT_EQ(span.last, 4u);  // 图书馆 beats 图书
  span = detect(text, 4, dict);
  EXPECT_EQ(span.first, 2u);
  EXPECT_EQ(span.last, 4u);
}

TEST(WordDetect, FourCharacterIdiomBeatsInnerWord) {
  const std::vector<std::string> text = {"他", "一", "心", "一", "意", "地"};
  const std::set<std::string> dict = {"一心", "一心一意", "一意"};
  const Span span = detect(text, 2, dict);
  EXPECT_EQ(span.first, 1u);
  EXPECT_EQ(span.last, 4u);
}

TEST(WordDetect, FallsBackToPressedCharacter) {
  const std::vector<std::string> text = {"的", "了", "吗"};
  const Span span = detect(text, 1, {});
  EXPECT_EQ(span.first, 1u);
  EXPECT_EQ(span.last, 1u);
}

TEST(WordDetect, StopsAtPunctuationAttachedToToken) {
  // "国。" ends a sentence: 国家 must not be built across the full stop.
  const std::vector<std::string> text = {"中", "国。", "家", "人"};
  const std::set<std::string> dict = {"中国", "国家", "家人"};
  Span span = detect(text, 1, dict);
  EXPECT_EQ(span.first, 0u);
  EXPECT_EQ(span.last, 1u);
  span = detect(text, 2, dict);
  EXPECT_EQ(span.first, 2u);
  EXPECT_EQ(span.last, 3u);
}

TEST(WordDetect, LeadingQuoteStartsRun) {
  const std::vector<std::string> text = {"说",
                                         "\xE2\x80\x9C"
                                         "你",
                                         "好"};  // 说“你好
  const std::set<std::string> dict = {"说你", "你好"};
  const Span span = detect(text, 1, dict);
  EXPECT_EQ(span.first, 1u);
  EXPECT_EQ(span.last, 2u);
}

TEST(WordDetect, IgnoresLatinAndMixedTokens) {
  EXPECT_TRUE(wordDetect::candidates(tokens({"hello", "world"}), 0).empty());
  EXPECT_TRUE(wordDetect::candidates(tokens({"中国", "人"}), 0).empty());
  // A Latin neighbour bounds the run.
  const auto list = wordDetect::candidates(tokens({"A", "中", "国"}), 1);
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0].text, "中国");
}

TEST(WordDetect, CandidatesStayWithinMaxLength) {
  std::vector<std::string> text(20, "字");
  const auto list = wordDetect::candidates(tokens(text), 10);
  ASSERT_FALSE(list.empty());
  for (const auto& candidate : list) {
    const size_t length = candidate.last - candidate.first + 1;
    EXPECT_GE(length, 2u);
    EXPECT_LE(length, wordDetect::MAX_CHARS);
    EXPECT_LE(candidate.first, 10u);
    EXPECT_GE(candidate.last, 10u);
  }
  // 2+3+4+5+6 spans contain the hit.
  EXPECT_EQ(list.size(), 20u);
}

TEST(WordDetect, TieGoesToEarlierStart) {
  const std::vector<std::string> text = {"研", "究", "生", "命"};
  const std::set<std::string> dict = {"研究", "研究生", "生命", "究生命"};
  // Both 研究生 and 究生命 contain 生 and are three characters long.
  const Span span = detect(text, 2, dict);
  EXPECT_EQ(span.first, 0u);
  EXPECT_EQ(span.last, 2u);
}

TEST(WordDetect, InvalidUtf8IsNotAnIdeograph) {
  EXPECT_TRUE(wordDetect::candidates(tokens({"\xE4\xB8"}), 0).empty());  // truncated 中
}
