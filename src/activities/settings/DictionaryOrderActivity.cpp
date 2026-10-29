#include "DictionaryOrderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/DictionaryOrder.h"
#include "util/DictionarySet.h"

namespace fui = freeink::ui;

void DictionaryOrderActivity::onEnter() {
  UiListActivity::onEnter();
  DictionarySet::discoverOrdered(dictionaries);
  labels.resize(dictionaries.size());
  rowItems.resize(dictionaries.size());
  moving = -1;
  edited = false;
}

void DictionaryOrderActivity::onExit() {
  if (edited) DictionarySet::saveOrder(dictionaries);
  Activity::onExit();
}

const char* DictionaryOrderActivity::headerTitle() const { return tr(STR_DICT_ORDER); }

void DictionaryOrderActivity::onBackButton() {
  // Back first puts a picked-up dictionary down where it was.
  if (moving >= 0) {
    RenderLock lock(*this);
    moving = -1;
    requestUpdate();
    return;
  }
  finish();
}

void DictionaryOrderActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  RenderLock lock(*this);  // buildScreen() reads the rows on the render task
  if (moving < 0) {
    moving = index;
  } else {
    if (index != moving) {
      // Take the picked-up dictionary out and insert it at the chosen row.
      DictionaryEntry entry = std::move(dictionaries[moving]);
      dictionaries.erase(dictionaries.begin() + moving);
      dictionaries.insert(dictionaries.begin() + index, std::move(entry));
      edited = true;
    }
    moving = -1;
  }
  moveSelectionTo(index);
  requestUpdate();
}

void DictionaryOrderActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (size_t i = 0; i < dictionaries.size(); ++i) {
    labels[i] = std::to_string(i + 1) + ". " + dictionaryOrder::displayName(dictionaries[i].name);
    rowItems[i] = {};
    rowItems[i].label = labels[i].c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
    const bool pickedUp = static_cast<int>(i) == moving;
    rowItems[i].bold = pickedUp;
    rowItems[i].value = pickedUp ? tr(STR_DICT_MOVING) : nullptr;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<int16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
