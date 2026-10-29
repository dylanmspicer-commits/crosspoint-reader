#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/VocabText.h"

class DictionarySet;

// The open book's vocabulary list: Chinese words saved while reading, each with
// a one-line gloss. One small text file per book next to its clippings, read
// and rewritten whole; every call runs on the reader's loop task.
class VocabStore {
 public:
  static constexpr size_t MAX_WORDS = 2000;

  enum class AddResult : uint8_t { Added, AlreadySaved, Full, Failed };

  static VocabStore& getInstance() { return instance; }

  void setBook(const std::string& bookFilePath);
  void clearBook();
  bool hasBook() const { return !path.empty(); }
  size_t count() const { return wordCount; }

  bool contains(const std::string& word) const;
  AddResult add(const std::string& word, const std::string& gloss);
  bool readAll(std::vector<vocabText::Entry>& out) const;
  bool removeAt(size_t index);

  // The gloss saved with word: the start of its definition in dictionaries,
  // or empty when no dictionary has it.
  static std::string glossFor(DictionarySet& dictionaries, const std::string& word);

 private:
  static VocabStore instance;

  bool readFile(std::string& out) const;
  bool writeFile(const std::string& text);

  std::string path;
  size_t wordCount = 0;
};

#define VOCAB VocabStore::getInstance()
