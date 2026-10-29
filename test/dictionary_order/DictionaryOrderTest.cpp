#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/DictionaryOrder.h"

namespace {
std::vector<std::string> ordered(std::vector<std::string> names, const std::string& order) {
  dictionaryOrder::apply(names, order, [](const std::string& name) -> const std::string& { return name; });
  return names;
}
}  // namespace

TEST(DictionaryOrder, EmptyOrderKeepsNameOrder) {
  EXPECT_EQ(ordered({"cedict", "english"}, ""), (std::vector<std::string>{"cedict", "english"}));
}

TEST(DictionaryOrder, StoredOrderWins) {
  EXPECT_EQ(ordered({"cedict", "english", "moe"}, "english/moe/cedict"),
            (std::vector<std::string>{"english", "moe", "cedict"}));
}

TEST(DictionaryOrder, NewDictionariesFollowInNameOrder) {
  // "moe" and "abc" were added after the order was saved; "gone" was removed.
  EXPECT_EQ(ordered({"abc", "cedict", "english", "moe"}, "english/gone/cedict"),
            (std::vector<std::string>{"english", "cedict", "abc", "moe"}));
}

TEST(DictionaryOrder, NamesMustMatchWhole) {
  EXPECT_EQ(dictionaryOrder::rank("english-big/english", "english"), 1u);
  EXPECT_EQ(dictionaryOrder::rank("english-big", "english"), SIZE_MAX);
  EXPECT_EQ(dictionaryOrder::rank("", "english"), SIZE_MAX);
}

TEST(DictionaryOrder, JoinRoundTripsAndRespectsCapacity) {
  EXPECT_EQ(dictionaryOrder::join({"english", "cedict"}, 64), "english/cedict");
  // "cedict" does not fit in 12 bytes after "english/" plus the terminator.
  EXPECT_EQ(dictionaryOrder::join({"english", "cedict", "a"}, 12), "english/a");
  EXPECT_EQ(dictionaryOrder::join({"bad/name", "ok"}, 64), "ok");
}

TEST(DictionaryOrder, DisplayNameDropsSortPrefix) {
  EXPECT_EQ(dictionaryOrder::displayName("1-cedict"), "cedict");
  EXPECT_EQ(dictionaryOrder::displayName("02_english"), "english");
  EXPECT_EQ(dictionaryOrder::displayName("3 moe"), "moe");
  EXPECT_EQ(dictionaryOrder::displayName("2024dict"), "2024dict");
  EXPECT_EQ(dictionaryOrder::displayName("english"), "english");
  EXPECT_EQ(dictionaryOrder::displayName("1-"), "1-");
}
