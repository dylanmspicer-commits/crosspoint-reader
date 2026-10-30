#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "util/VocabText.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

class DictionarySet;

// The open book's vocabulary list: Chinese words saved while reading, each with
// a gloss, the sentence it came from and its position. One text file per book
// next to its clippings. The file is streamed a line at a time and a save
// appends one line, so memory does not grow with sentences; only the words are
// held, sorted, for duplicate checks and the page underlines.
//
// The reader's page render reads savedWords(): change the list under RenderLock
// while the reader is the current activity.
class VocabStore {
 public:
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
  static constexpr size_t MAX_WORDS = 2000;
#else
  static constexpr size_t MAX_WORDS = 500;
#endif

  enum class AddResult : uint8_t { Added, AlreadySaved, Full, Failed };

  static VocabStore& getInstance() { return instance; }

  void setBook(const std::string& bookFilePath);
  void clearBook();
  bool hasBook() const { return !path.empty(); }
  size_t count() const { return words.size(); }

  bool contains(const std::string& word) const;
  // Sorted saved words.
  const std::vector<std::string>& savedWords() const { return words; }
  // Characters in the longest saved word; 0 when the list is empty.
  size_t longestWordChars() const { return longestChars; }

  AddResult add(vocabText::Entry entry);
  // Byte offset of every entry's line, in saved order.
  bool lineOffsets(std::vector<uint32_t>& out) const;
  bool readEntryAt(uint32_t offset, vocabText::Entry& out) const;
  // Remove the entry at index in saved order.
  bool removeAt(size_t index);

  // The gloss saved with word: the start of its entry in the first dictionary
  // that has it, or empty when none does.
  static std::string glossFor(DictionarySet& dictionaries, const std::string& word);

 private:
  static VocabStore instance;

  // Calls fn(line, offset) for every non-empty line; false on a read error.
  bool forEachLine(const std::function<void(std::string_view, uint32_t)>& fn) const;
  void remember(const std::string& word);

  std::string path;
  std::vector<std::string> words;
  size_t longestChars = 0;
};

#define VOCAB VocabStore::getInstance()
