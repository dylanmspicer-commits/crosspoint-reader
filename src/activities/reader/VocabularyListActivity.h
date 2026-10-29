#pragma once

#include <array>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "util/DictionarySet.h"
#include "util/VocabText.h"

// The open book's saved Chinese words, oldest first, each with its one-line
// gloss. Tap a word for its full entry; hold it to delete it.
class VocabularyListActivity final : public UiListActivity {
 public:
  VocabularyListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  void refreshRowWindow(int start);
  void openSelected();
  void showDeleteConfirmation();
  void deleteSelected();

  int listCount() const override { return static_cast<int>(entries.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;

  // Rows handed to the list are a window around the viewport, so a long list
  // costs one ListItem per visible row rather than one per word.
  static constexpr int ROW_WINDOW = 10;
  std::vector<vocabText::Entry> entries;
  std::array<freeink::ui::ListItem, ROW_WINDOW> rowItems;
  int windowStart = -1;
  int windowCount = 0;
  // Kept open while a definition is shown: its nested lookups use it.
  DictionarySet dictionaries;
  bool dictionariesOpened = false;
  OptionPopup popup;
  bool confirmingDelete = false;
};
