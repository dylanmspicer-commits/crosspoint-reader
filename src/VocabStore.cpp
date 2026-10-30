#include "VocabStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include <algorithm>

#include "ClippingStore.h"
#include "util/DictionarySet.h"
#include "util/HtmlToPlainText.h"

VocabStore VocabStore::instance;

namespace {
constexpr char VOCAB_DIR_PARENT[] = "/.crosspoint";
constexpr char VOCAB_DIR[] = "/.crosspoint/clippings";

std::string_view wordOf(const std::string_view line) { return line.substr(0, line.find('\t')); }

size_t codepointCount(const std::string_view text) {
  size_t count = 0;
  for (const char c : text) count += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return count;
}
}  // namespace

bool VocabStore::forEachLine(const std::function<void(std::string_view, uint32_t)>& fn) const {
  if (path.empty() || !Storage.exists(path.c_str())) return true;  // no words yet
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open %s", path.c_str());
    return false;
  }
  char chunk[256];
  std::string line;
  line.reserve(vocabText::MAX_LINE_BYTES);
  uint32_t offset = 0;
  uint32_t lineStart = 0;
  bool overlong = false;
  while (true) {
    const int got = file.read(chunk, sizeof(chunk));
    if (got < 0) return false;
    if (got == 0) break;
    for (int i = 0; i < got; ++i, ++offset) {
      if (chunk[i] == '\n') {
        if (!line.empty()) fn(line, lineStart);
        line.clear();
        overlong = false;
        lineStart = offset + 1;
      } else if (line.size() < vocabText::MAX_LINE_BYTES) {
        line.push_back(chunk[i]);
      } else if (!overlong) {
        overlong = true;  // keep the start of a damaged line; its word is still usable
        LOG_ERR("VOCAB", "Overlong line at %lu", static_cast<unsigned long>(lineStart));
      }
    }
  }
  if (!line.empty()) fn(line, lineStart);
  return true;
}

void VocabStore::remember(const std::string& word) {
  const auto at = std::lower_bound(words.begin(), words.end(), word);
  if (at != words.end() && *at == word) return;
  words.insert(at, word);
  longestChars = std::max(longestChars, codepointCount(word));
}

void VocabStore::setBook(const std::string& bookFilePath) {
  path = ClippingStore::vocabPathForBook(bookFilePath);
  words.clear();
  longestChars = 0;
  forEachLine([this](const std::string_view line, uint32_t) {
    const std::string_view word = wordOf(line);
    if (word.empty() || words.size() >= MAX_WORDS) return;
    words.emplace_back(word);
    longestChars = std::max(longestChars, codepointCount(word));
  });
  std::sort(words.begin(), words.end());
  words.erase(std::unique(words.begin(), words.end()), words.end());
}

void VocabStore::clearBook() {
  path.clear();
  std::vector<std::string>{}.swap(words);
  longestChars = 0;
}

bool VocabStore::contains(const std::string& word) const {
  return std::binary_search(words.begin(), words.end(), word);
}

VocabStore::AddResult VocabStore::add(vocabText::Entry entry) {
  entry.word = vocabText::sanitize(entry.word, vocabText::MAX_WORD_BYTES);
  entry.gloss = vocabText::sanitize(entry.gloss, vocabText::MAX_GLOSS_BYTES);
  entry.sentence = vocabText::sanitize(entry.sentence, vocabText::MAX_SENTENCE_BYTES);
  if (path.empty() || entry.word.empty()) return AddResult::Failed;
  if (contains(entry.word)) return AddResult::AlreadySaved;
  if (words.size() >= MAX_WORDS) return AddResult::Full;

  Storage.mkdir(VOCAB_DIR_PARENT);
  Storage.mkdir(VOCAB_DIR);
  // Append one line; on a short write cut the file back so no partial line is left.
  HalFile file = Storage.open(path.c_str(), O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("VOCAB", "Cannot open %s for append", path.c_str());
    return AddResult::Failed;
  }
  const size_t originalSize = file.fileSize();
  std::string line = vocabText::formatLine(entry);
  // A last line without a newline (a hand edit, or an earlier write that could not be
  // rolled back) must not swallow the new entry.
  if (originalSize > 0) {
    char last = '\n';
    if (file.seekSet(originalSize - 1) && file.read(&last, 1) == 1 && last != '\n') line.insert(0, 1, '\n');
    if (!file.seekSet(originalSize)) {
      LOG_ERR("VOCAB", "Cannot seek to the end of %s", path.c_str());
      return AddResult::Failed;
    }
  }
  const bool ok = file.write(line.data(), line.size()) == line.size();
  if (!ok && !file.truncate(originalSize)) LOG_ERR("VOCAB", "Cannot roll back a partial save");
  file.flush();
  if (!ok) return AddResult::Failed;
  remember(entry.word);
  return AddResult::Added;
}

bool VocabStore::lineOffsets(std::vector<uint32_t>& out) const {
  out.clear();
  out.reserve(words.size());
  return forEachLine([&out](const std::string_view line, const uint32_t offset) {
    if (!wordOf(line).empty()) out.push_back(offset);
  });
}

bool VocabStore::readEntryAt(const uint32_t offset, vocabText::Entry& out) const {
  if (path.empty()) return false;
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file || !file.seekSet(offset)) return false;
  std::string buffer(vocabText::MAX_LINE_BYTES, '\0');
  const int got = file.read(buffer.data(), buffer.size());
  if (got <= 0) return false;
  const std::string_view text(buffer.data(), static_cast<size_t>(got));
  auto entries = vocabText::parse(text.substr(0, text.find('\n')));
  if (entries.empty()) return false;
  out = std::move(entries.front());
  return true;
}

bool VocabStore::removeAt(const size_t index) {
  if (path.empty()) return false;
  // Copy every other line to a new file, then swap it in, so a failure keeps the list.
  const std::string tmp = path + ".tmp";
  std::string removed;
  bool stillListed = false;  // another line has the same word (only after a hand edit)
  {
    size_t current = 0;
    forEachLine([&](const std::string_view line, uint32_t) {
      if (!wordOf(line).empty() && current++ == index) removed = std::string(wordOf(line));
    });
  }
  if (removed.empty()) return false;
  bool writeOk = true;
  {
    HalFile out = Storage.open(tmp.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!out) return false;
    size_t current = 0;
    const bool readOk = forEachLine([&](const std::string_view line, uint32_t) {
      if (wordOf(line).empty()) return;
      if (current++ == index) return;
      stillListed = stillListed || wordOf(line) == removed;
      writeOk = writeOk && out.write(line.data(), line.size()) == line.size() && out.write("\n", 1) == 1;
    });
    out.flush();
    writeOk = writeOk && readOk;
  }
  if (!writeOk || (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) ||
      !Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    LOG_ERR("VOCAB", "Cannot remove word %u", static_cast<unsigned>(index));
    return false;
  }
  const auto at = std::lower_bound(words.begin(), words.end(), removed);
  if (!stillListed && at != words.end() && *at == removed) words.erase(at);
  longestChars = 0;
  for (const auto& word : words) longestChars = std::max(longestChars, codepointCount(word));
  return true;
}

std::string VocabStore::glossFor(DictionarySet& dictionaries, const std::string& word) {
  std::string definition;
  std::string headword;
  if (!dictionaries.lookupFirst(word.c_str(), definition, headword)) return {};
  if (dictionaries.definitionsAreHtml()) {
    definition = htmlToPlainText(definition, DICTIONARY_PLAIN_HEADWORD_MARKER);
  }
  return vocabText::makeGloss(definition, word, DICTIONARY_PLAIN_HEADWORD_MARKER, DICTIONARY_SECTION_MARKER);
}
