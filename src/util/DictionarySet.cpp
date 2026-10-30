#include "DictionarySet.h"

#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>
#include <TxtToHtml.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "DictionaryOrder.h"
#include "DictionaryRegistry.h"
#include "HtmlToPlainText.h"

void DictionarySet::discoverOrdered(std::vector<DictionaryEntry>& out) {
  DictionaryRegistry::discover(out);  // name order
  dictionaryOrder::apply(out, SETTINGS.dictionaryOrder,
                         [](const DictionaryEntry& entry) -> const std::string& { return entry.name; });
}

void DictionarySet::saveOrder(const std::vector<DictionaryEntry>& ordered) {
  std::vector<std::string> names;
  names.reserve(ordered.size());
  for (const auto& entry : ordered) names.push_back(entry.name);
  const std::string joined = dictionaryOrder::join(names, sizeof(SETTINGS.dictionaryOrder));
  strncpy(SETTINGS.dictionaryOrder, joined.c_str(), sizeof(SETTINGS.dictionaryOrder) - 1);
  SETTINGS.dictionaryOrder[sizeof(SETTINGS.dictionaryOrder) - 1] = '\0';
  SETTINGS.saveToFile();
}

bool DictionarySet::open(const char* setting) {
  for (auto& dictionary : dictionaries) dictionary.reset();
  count = 0;
  lastHtml = false;
  if (!setting || setting[0] == '\0') return false;

  const auto openOne = [this](const std::string& folder) {
    if (count == MAX_DICTIONARIES) {
      LOG_ERR("DICT", "More than %u dictionaries; skipping %s", static_cast<unsigned>(MAX_DICTIONARIES),
              folder.c_str());
      return;
    }
    auto dictionary = makeUniqueNoThrow<Dictionary>();
    if (!dictionary) {
      LOG_ERR("DICT", "OOM: dictionary %s", folder.c_str());
      return;
    }
    if (!dictionary->open(folder.c_str())) {
      LOG_ERR("DICT", "Skipping unusable dictionary %s", folder.c_str());
      return;
    }
    names[count] = dictionaryOrder::displayName(folder);
    dictionaries[count++] = std::move(dictionary);
  };

  if (strcmp(setting, ALL_DICTIONARIES) != 0) {
    openOne(setting);
    return isOpen();
  }
  std::vector<DictionaryEntry> found;
  discoverOrdered(found);
  for (const auto& entry : found) openOne(entry.name);
  return isOpen();
}

bool DictionarySet::needsIndex() {
  for (size_t i = 0; i < count; ++i) {
    if (dictionaries[i]->needsIndex()) return true;
  }
  return false;
}

bool DictionarySet::buildIndex(void (*yieldFn)(void*), void* ctx, Dictionary::IndexResult* outResult) {
  if (outResult) *outResult = Dictionary::IndexResult::Ok;
  bool anyUsable = false;
  for (size_t i = 0; i < count; ++i) {
    Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
    // One broken dictionary must not block lookups in the others.
    if (!dictionaries[i]->needsIndex() || dictionaries[i]->buildIndex(yieldFn, ctx, &result)) {
      anyUsable = true;
    } else if (outResult && *outResult == Dictionary::IndexResult::Ok) {
      *outResult = result;
    }
  }
  return anyUsable;
}

bool DictionarySet::lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut,
                           Dictionary::LookupResult* outResult) {
  struct Hit {
    size_t dictionary;
    std::string text;
    bool html;
  };
  std::vector<Hit> hits;
  hits.reserve(count);
  size_t gathered = 0;
  auto firstFailure = Dictionary::LookupResult::NotFound;
  for (size_t i = 0; i < count; ++i) {
    // Each part costs up to MAX_DEFINITION_BYTES; stop before the total could pass it.
    if (gathered >= Dictionary::MAX_DEFINITION_BYTES) break;
    std::string text;
    std::string headword;
    auto result = Dictionary::LookupResult::NotFound;
    if (dictionaries[i]->lookup(word, text, headword, &result)) {
      if (hits.empty()) matchedHeadwordOut = std::move(headword);
      gathered += text.size();
      hits.push_back({i, std::move(text), dictionaries[i]->definitionsAreHtml()});
    } else if (result != Dictionary::LookupResult::NotFound && firstFailure == Dictionary::LookupResult::NotFound) {
      firstFailure = result;
    }
  }
  if (hits.empty()) {
    if (outResult) *outResult = firstFailure;
    return false;
  }
  if (outResult) *outResult = Dictionary::LookupResult::Found;
  if (hits.size() == 1) {
    definitionOut = std::move(hits.front().text);
    lastHtml = hits.front().html;
    return true;
  }

  bool allHtml = true;
  for (const auto& hit : hits) allHtml = allHtml && hit.html;
  definitionOut.clear();
  for (auto& hit : hits) {
    // Mixed formats are shown as plain text; HTML parts are converted first.
    if (!allHtml && hit.html) hit.text = htmlToPlainText(hit.text, DICTIONARY_PLAIN_HEADWORD_MARKER);
    std::string part;
    part.push_back(DICTIONARY_SECTION_MARKER);
    if (allHtml) {
      part += "<h1>";
      TxtToHtml::appendEscapedHtml(part, names[hit.dictionary]);
      part += "</h1>";
    } else {
      part.push_back(DICTIONARY_PLAIN_HEADWORD_MARKER);
      part += names[hit.dictionary];
      part.push_back('\n');
    }
    part += hit.text;
    if (!part.empty() && part.back() != '\n') part.push_back('\n');
    if (!definitionOut.empty() && definitionOut.size() + part.size() > Dictionary::MAX_DEFINITION_BYTES) {
      LOG_INF("DICT", "Combined definition full; later dictionaries omitted");
      break;
    }
    definitionOut += part;
  }
  lastHtml = allHtml;
  return true;
}

bool DictionarySet::lookupFirst(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut) {
  for (size_t i = 0; i < count; ++i) {
    if (dictionaries[i]->lookup(word, definitionOut, matchedHeadwordOut)) {
      lastHtml = dictionaries[i]->definitionsAreHtml();
      return true;
    }
  }
  return false;
}

bool DictionarySet::hasHeadwords(const char* const* words, const size_t wordCount, bool* found) {
  for (size_t i = 0; i < wordCount; ++i) found[i] = false;
  auto scratch = makeUniqueNoThrow<bool[]>(wordCount);
  if (!scratch) return false;
  bool probed = false;
  for (size_t i = 0; i < count; ++i) {
    if (!dictionaries[i]->hasHeadwords(words, wordCount, scratch.get())) continue;
    probed = true;
    for (size_t w = 0; w < wordCount; ++w) found[w] = found[w] || scratch[w];
  }
  return probed;
}
