#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ClipSelectionActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/DictionarySet.h"
#include "util/VocabText.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        content{.headword = std::move(headword), .text = std::move(definition)},
        htmlDefinition(htmlDefinition) {}

  bool enableLookup(DictionarySet& dictionary, std::string&& query);
  // Where the first entry's word was found in the book; Save Word keeps it.
  void setVocabSource(vocabText::Entry source) { vocabSource = std::move(source); }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct PlainSelectionLine;

  struct Line {
    uint32_t start;
    uint16_t len;
    bool bold;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  BodyArea bodyArea() const;

  struct Definition {
    std::string headword;
    std::string text;
    std::vector<std::unique_ptr<Page>> pages;
    std::vector<Line> lines;
    int currentPage = 0;
    int totalPages = 1;
    int linesPerPage = 1;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;

    void releasePayload() {
      std::string{}.swap(text);
      std::vector<std::unique_ptr<Page>>{}.swap(pages);
      std::vector<Line>{}.swap(lines);
    }
  };

  // Replay the original query: the displayed headword may be a stem or synonym.
  struct HistoryEntry {
    std::string query;
    int page = 0;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;
    bool styled = false;
  };

  struct LookupState {
    explicit LookupState(DictionarySet& dictionary) : dictionary(dictionary) {}
    DictionarySet& dictionary;  // Owned by the retained DictionaryWordSelectActivity parent.
    std::array<HistoryEntry, 16> history;
    size_t depth = 0;
    std::string query;
    std::string pendingWord;
    std::vector<std::unique_ptr<Page>> selectionPages;
  };

  bool prepareDefinition(Definition& next, bool forcePlain = false);
  bool layoutHtmlPages(Definition& next);
  bool wrapText(Definition& next);
  bool reserveLines(Definition& next, size_t count);
  int measureSpan(int fontId, const char* text, size_t len, EpdFontFamily::Style family = EpdFontFamily::REGULAR) const;
  void drawBody(int fontId, int x, int startY, int currentPage) const;
  void startSelection(int x = -1, int y = -1);
  bool buildPlainSelectionPages();
  void navigateDefinition(bool back);
  bool restoreCurrentDefinition(bool styled);
  void showMessage(StrId message);
  // The Save Word strip under the body exists whenever a book is open, so the
  // layout stays the same across every entry of one lookup chain.
  bool reservesSaveStrip() const;
  bool canSaveWord() const;
  Rect saveButtonRect() const;
  void saveHeadword();
  void refreshSavedState();

  Definition content;
  // Follows the dictionary of the shown entry: nested lookups can land in another one.
  bool htmlDefinition;
  std::unique_ptr<LookupState> lookup;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;
  bool messageVisible = false;
  bool initialLayoutFailed = false;
  bool headwordSaved = false;  // content.headword is in the book's vocabulary list
  std::string savedStateFor;   // headword headwordSaved was read for
  vocabText::Entry vocabSource;
  ButtonNavigator buttonNavigator;
};
