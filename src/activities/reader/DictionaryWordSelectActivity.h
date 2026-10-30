#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/DictionarySet.h"
#include "util/VocabText.h"

// Controller for dictionary lookup: renders the background page, runs the
// lookup against the active StarDict dictionary, and either opens
// DictionaryDefinitionActivity or displays a status popup (Looking up,
// Indexing, Not found, Error).
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string lookupText = {})
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        lookupText(std::move(lookupText)) {}

  // Where the looked-up word was found, for Save Word on the definition page.
  void setVocabSource(vocabText::Entry source) { vocabSource = std::move(source); }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void performLookup();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;

  std::string lookupText;
  vocabText::Entry vocabSource;
  bool lookupPending = false;
  DictionarySet dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
