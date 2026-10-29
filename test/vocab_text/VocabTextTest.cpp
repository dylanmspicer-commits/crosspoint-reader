#include <gtest/gtest.h>

#include <string>

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
