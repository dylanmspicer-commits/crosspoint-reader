#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Dictionary.h"

struct DictionaryEntry;

// The dictionaries a lookup searches: the one chosen in Settings, or every
// dictionary under /dictionaries when "All dictionaries" is chosen, in the
// user's Dictionary Order. With several dictionaries a lookup gathers the entry
// from each one that has the word, so a Chinese and an English dictionary work
// together and a new dictionary is used as soon as its folder is added.
class DictionarySet {
 public:
  // Stored in SETTINGS.dictionaryName for "All dictionaries"; not a valid folder name.
  static constexpr const char* ALL_DICTIONARIES = "*";
  // Each open dictionary keeps a ~3 KB entry list; this caps that cost.
  static constexpr size_t MAX_DICTIONARIES = 8;

  // Every installed dictionary, in the saved search order.
  static void discoverOrdered(std::vector<DictionaryEntry>& out);
  // Save the search order shown on the Dictionary Order screen.
  static void saveOrder(const std::vector<DictionaryEntry>& ordered);

  // Open the dictionary named by setting, or all of them for ALL_DICTIONARIES.
  // True when at least one opened.
  bool open(const char* setting);
  bool isOpen() const { return count != 0; }

  bool needsIndex();
  bool buildIndex(void (*yieldFn)(void*) = nullptr, void* ctx = nullptr, Dictionary::IndexResult* outResult = nullptr);

  // Gathers the definition from every dictionary that has the word, in order.
  // With more than one, each part starts with DICTIONARY_SECTION_MARKER and the
  // dictionary's name, and parts that do not fit MAX_DEFINITION_BYTES are left
  // off. The headword is the first dictionary's. A miss everywhere reports the
  // first real failure (read, memory, decompression) ahead of NotFound.
  bool lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut,
              Dictionary::LookupResult* outResult = nullptr);

  // Format of the definition the last successful lookup() returned: HTML only
  // when every part was HTML; mixed parts are converted to plain text.
  bool definitionsAreHtml() const { return lastHtml; }

  // found[i] is set when any indexed dictionary has words[i] as an exact
  // headword. found must hold wordCount entries. False when no dictionary could be probed.
  bool hasHeadwords(const char* const* words, size_t wordCount, bool* found);

 private:
  std::array<std::unique_ptr<Dictionary>, MAX_DICTIONARIES> dictionaries;
  std::array<std::string, MAX_DICTIONARIES> names;  // shown above each dictionary's part
  size_t count = 0;
  bool lastHtml = false;
};
