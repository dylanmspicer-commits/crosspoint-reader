#include "VocabStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include "ClippingStore.h"
#include "util/DictionarySet.h"
#include "util/HtmlToPlainText.h"

VocabStore VocabStore::instance;

namespace {
// MAX_WORDS lines of word, tab, gloss and newline, with margin.
constexpr size_t MAX_FILE_BYTES = VocabStore::MAX_WORDS * (vocabText::MAX_WORD_BYTES + vocabText::MAX_GLOSS_BYTES + 2);
}  // namespace

void VocabStore::setBook(const std::string& bookFilePath) {
  path = ClippingStore::vocabPathForBook(bookFilePath);
  std::string text;
  wordCount = readFile(text) ? vocabText::parse(text).size() : 0;
}

void VocabStore::clearBook() {
  path.clear();
  wordCount = 0;
}

bool VocabStore::readFile(std::string& out) const {
  out.clear();
  if (path.empty() || !Storage.exists(path.c_str())) return true;  // no words yet
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open %s", path.c_str());
    return false;
  }
  const size_t size = file.fileSize();
  if (size > MAX_FILE_BYTES) {
    LOG_ERR("VOCAB", "Vocabulary file too large (%u bytes)", static_cast<unsigned>(size));
    return false;
  }
  out.resize(size);
  if (size != 0 && file.read(out.data(), size) != static_cast<int>(size)) {
    LOG_ERR("VOCAB", "Short read of %s", path.c_str());
    out.clear();
    return false;
  }
  return true;
}

bool VocabStore::writeFile(const std::string& text) {
  // Write a temporary file and rename it over the list, so a failed write keeps
  // the old list intact.
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/.crosspoint/clippings");
  const std::string tmp = path + ".tmp";
  {
    HalFile file = Storage.open(tmp.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!file || file.write(text.data(), text.size()) != text.size()) {
      LOG_ERR("VOCAB", "Cannot write %s", tmp.c_str());
      Storage.remove(tmp.c_str());
      return false;
    }
    file.flush();
  }
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return Storage.rename(tmp.c_str(), path.c_str());
}

bool VocabStore::contains(const std::string& word) const {
  std::string text;
  return readFile(text) && vocabText::contains(text, word);
}

VocabStore::AddResult VocabStore::add(const std::string& word, const std::string& gloss) {
  const std::string cleanWord = vocabText::sanitize(word, vocabText::MAX_WORD_BYTES);
  if (path.empty() || cleanWord.empty()) return AddResult::Failed;
  std::string text;
  if (!readFile(text)) return AddResult::Failed;
  if (vocabText::contains(text, cleanWord)) return AddResult::AlreadySaved;
  const size_t existing = vocabText::parse(text).size();
  if (existing >= MAX_WORDS) return AddResult::Full;
  text += vocabText::formatLine({cleanWord, vocabText::sanitize(gloss, vocabText::MAX_GLOSS_BYTES)});
  if (!writeFile(text)) return AddResult::Failed;
  wordCount = existing + 1;
  return AddResult::Added;
}

bool VocabStore::readAll(std::vector<vocabText::Entry>& out) const {
  std::string text;
  if (!readFile(text)) return false;
  out = vocabText::parse(text);
  return true;
}

bool VocabStore::removeAt(const size_t index) {
  std::vector<vocabText::Entry> entries;
  if (!readAll(entries) || index >= entries.size()) return false;
  entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
  std::string text;
  for (const auto& entry : entries) text += vocabText::formatLine(entry);
  if (!writeFile(text)) return false;
  wordCount = entries.size();
  return true;
}

std::string VocabStore::glossFor(DictionarySet& dictionaries, const std::string& word) {
  std::string definition;
  std::string headword;
  if (!dictionaries.lookup(word.c_str(), definition, headword)) return {};
  if (dictionaries.definitionsAreHtml()) {
    definition = htmlToPlainText(definition, DICTIONARY_PLAIN_HEADWORD_MARKER);
  }
  return vocabText::makeGloss(definition, word, DICTIONARY_PLAIN_HEADWORD_MARKER, DICTIONARY_SECTION_MARKER);
}
