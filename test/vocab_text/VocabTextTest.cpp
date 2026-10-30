#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "util/VocabText.h"

namespace {
constexpr char HEADING = '\x01';
constexpr char SECTION = '\x02';
}  // namespace

TEST(VocabText, RoundTripsEntries) {
  const std::string file =
      vocabText::formatLine({"图书馆", "túshūguǎn · library"}) + vocabText::formatLine({"一心一意", ""}) + "\n";
  const auto entries = vocabText::parse(file);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "图书馆");
  EXPECT_EQ(entries[0].gloss, "túshūguǎn · library");
  EXPECT_EQ(entries[1].word, "一心一意");
  EXPECT_EQ(entries[1].gloss, "");
  EXPECT_TRUE(vocabText::contains(file, "图书馆"));
  EXPECT_FALSE(vocabText::contains(file, "图书"));  // whole words only
}

TEST(VocabText, SanitizeRemovesTabsAndNewlines) {
  EXPECT_EQ(vocabText::sanitize("  a\tb\n\nc  ", 64), "a b c");
  EXPECT_EQ(vocabText::sanitize("", 64), "");
}

TEST(VocabText, SanitizeNeverSplitsUtf8) {
  // Each ideograph is 3 bytes; a 7-byte cap keeps two whole characters.
  EXPECT_EQ(vocabText::sanitize("图书馆", 7), "图书");
}

TEST(VocabText, GlossTakesPinyinAndFirstSense) {
  const std::string definition = "tú shū guǎn\nlibrary\nCL:家[jia1]\n";
  EXPECT_EQ(vocabText::makeGloss(definition, "图书馆", HEADING, SECTION), "tú shū guǎn \xC2\xB7 library");
}

TEST(VocabText, GlossSkipsHeadingsAndRepeatedWord) {
  // Gathered from two dictionaries: section markers, dictionary names, headword labels.
  const std::string definition = std::string(1, SECTION) + HEADING + "cedict\n" + HEADING + "图书馆\n图书馆\n\n" +
                                 "[tu2 shu1 guan3]\n/library/\n" + SECTION + HEADING + "english\nnot used\n";
  EXPECT_EQ(vocabText::makeGloss(definition, "图书馆", HEADING, SECTION), "[tu2 shu1 guan3] \xC2\xB7 /library/");
}

TEST(VocabText, LongGlossEndsWithEllipsis) {
  const std::string definition = std::string(200, 'a') + "\n";
  const std::string gloss = vocabText::makeGloss(definition, "x", HEADING, SECTION);
  EXPECT_LE(gloss.size(), vocabText::MAX_GLOSS_BYTES);
  EXPECT_EQ(gloss.substr(gloss.size() - 3), "\xE2\x80\xA6");
}

TEST(VocabText, DetectsChinese) {
  EXPECT_TRUE(vocabText::hasIdeograph("图书馆"));
  EXPECT_TRUE(vocabText::hasIdeograph("T恤"));
  EXPECT_FALSE(vocabText::hasIdeograph("library"));
}

TEST(VocabText, RoundTripsSentenceAndPosition) {
  vocabText::Entry entry{"图书馆", "túshūguǎn · library", "他在图书馆待了一整天。", 3, 17, 1234};
  const auto entries = vocabText::parse(vocabText::formatLine(entry));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].sentence, "他在图书馆待了一整天。");
  EXPECT_EQ(entries[0].spineIndex, 3u);
  EXPECT_EQ(entries[0].page, 17u);
  EXPECT_EQ(entries[0].textOffset, 1234u);
  EXPECT_TRUE(entries[0].hasPosition());
}

TEST(VocabText, ReadsListsSavedBeforeSentences) {
  const auto entries = vocabText::parse("图书馆\ttúshūguǎn · library\n一心一意\t\n");
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].gloss, "túshūguǎn · library");
  EXPECT_EQ(entries[0].sentence, "");
  EXPECT_FALSE(entries[0].hasPosition());
  EXPECT_EQ(entries[1].textOffset, vocabText::NO_POSITION);
}

TEST(VocabText, MissingPositionStaysMissing) {
  vocabText::Entry entry{"好", "hǎo", "", vocabText::NO_POSITION, vocabText::NO_POSITION, vocabText::NO_POSITION};
  const auto entries = vocabText::parse(vocabText::formatLine(entry));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_FALSE(entries[0].hasPosition());
  EXPECT_EQ(vocabText::parseNumber("12x"), vocabText::NO_POSITION);
  EXPECT_EQ(vocabText::parseNumber("99999999999"), vocabText::NO_POSITION);
}

TEST(VocabText, FindsSentenceEnds) {
  EXPECT_TRUE(vocabText::endsSentence("天\xE3\x80\x82"));              // 天。
  EXPECT_TRUE(vocabText::endsSentence("了\xE3\x80\x82\xE2\x80\x9D"));  // 了。”
  EXPECT_TRUE(vocabText::endsSentence("吗\xEF\xBC\x9F"));              // 吗？
  EXPECT_TRUE(vocabText::endsSentence("done?"));
  EXPECT_FALSE(vocabText::endsSentence("馆\xEF\xBC\x8C"));  // 馆， (a comma does not end it)
  EXPECT_FALSE(vocabText::endsSentence("图"));
}

namespace {
std::vector<bool> mark(const std::vector<std::string>& texts, std::vector<std::string> saved,
                       std::vector<bool> joined = {}) {
  std::sort(saved.begin(), saved.end());
  std::vector<wordDetect::Token> tokens;
  for (const auto& text : texts) tokens.push_back({text});
  if (joined.empty()) joined.assign(texts.size(), true);
  return vocabText::markSavedWords(tokens, joined, saved, 6);
}
}  // namespace

TEST(VocabText, MarksSavedWordsLongestFirst) {
  const auto marked = mark({"我", "在", "图", "书", "馆", "看", "书"}, {"图书", "图书馆", "看书"});
  EXPECT_EQ(marked, (std::vector<bool>{false, false, true, true, true, true, true}));
}

TEST(VocabText, MarksAcrossLineBreaksButNotPunctuation) {
  // 国。家: the full stop is attached to 国, so 国家 is not there.
  EXPECT_EQ(mark({"中", "国\xE3\x80\x82", "家"}, {"国家"}), (std::vector<bool>{false, false, false}));
  // A word split over two lines still matches: the tokens are joined in the text.
  EXPECT_EQ(mark({"图", "书"}, {"图书"}, {true, true}), (std::vector<bool>{true, true}));
  // A gap in the text (another paragraph) breaks it.
  EXPECT_EQ(mark({"图", "书"}, {"图书"}, {true, false}), (std::vector<bool>{false, false}));
}

TEST(VocabText, MarksWordWithAttachedPunctuation) {
  EXPECT_EQ(mark({"去", "图", "书", "馆\xE3\x80\x82"}, {"图书馆"}), (std::vector<bool>{false, true, true, true}));
  EXPECT_EQ(mark({"hello", "world"}, {"图书馆"}), (std::vector<bool>{false, false}));
  EXPECT_EQ(mark({"图"}, {}), (std::vector<bool>{false}));
}
