#pragma once

#include <array>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "util/DictionarySet.h"
#include "util/VocabText.h"

// The open book's saved Chinese words, oldest first, each with its one-line
// gloss. Tap a word for its sentence, definition and passage; hold it to delete it.
class VocabularyListActivity final : public UiListActivity {
 public:
  VocabularyListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  void refreshRowWindow(int start);
  void reloadOffsets();
  // Reads the entry at nav.selected into selectedEntry.
  bool loadSelected();
  void openSelected();  // the box with the sentence, Definition and Go to Passage
  void showDefinition();
  void goToPassage();
  void showDeleteConfirmation();
  void deleteSelected();

  int listCount() const override { return static_cast<int>(offsets.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;

  // Only each entry's position in the file is held; the rows on screen are read
  // from the SD card, so a long list costs a few bytes per word.
  static constexpr int ROW_WINDOW = 10;
  std::vector<uint32_t> offsets;
  std::array<vocabText::Entry, ROW_WINDOW> windowEntries;
  std::array<freeink::ui::ListItem, ROW_WINDOW> rowItems;
  vocabText::Entry selectedEntry;
  int windowStart = -1;
  int windowCount = 0;
  // Kept open while a definition is shown: its nested lookups use it.
  DictionarySet dictionaries;
  bool dictionariesOpened = false;
  OptionPopup popup;
  bool confirmingDelete = false;
};
