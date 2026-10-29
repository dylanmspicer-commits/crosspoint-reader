#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "util/DictionaryRegistry.h"

class MappedInputManager;

// Sets the order "All dictionaries" searches in, which is also the order their
// definitions are shown. Select a dictionary to pick it up, then select the row
// to move it to.
class DictionaryOrderActivity final : public UiListActivity {
 public:
  explicit DictionaryOrderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("DictionaryOrder", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(dictionaries.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override;

  std::vector<DictionaryEntry> dictionaries;
  std::vector<std::string> labels;  // "1. name"; rows point into these
  std::vector<freeink::ui::ListItem> rowItems;
  int moving = -1;  // row picked up, or -1
  bool edited = false;
};
