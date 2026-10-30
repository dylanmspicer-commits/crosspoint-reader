#include "VocabularyListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "VocabStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
}  // namespace

VocabularyListActivity::VocabularyListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Vocabulary", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void VocabularyListActivity::onEnter() {
  UiListActivity::onEnter();
  reloadOffsets();
}

void VocabularyListActivity::reloadOffsets() {
  if (!VOCAB.lineOffsets(offsets)) LOG_ERR("VOCAB", "Failed to read the vocabulary list");
  windowStart = -1;
  windowCount = 0;
}

bool VocabularyListActivity::loadSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return false;
  return VOCAB.readEntryAt(offsets[static_cast<size_t>(nav.selected)], selectedEntry);
}

const char* VocabularyListActivity::headerTitle() const { return tr(STR_VOCABULARY); }

void VocabularyListActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = std::max(0, start);
  if (clamped > total - ROW_WINDOW) clamped = std::max(0, total - ROW_WINDOW);
  if (clamped == windowStart) return;
  windowCount = std::min(ROW_WINDOW, total - clamped);
  for (int slot = 0; slot < windowCount; ++slot) {
    auto& entry = windowEntries[static_cast<size_t>(slot)];
    if (!VOCAB.readEntryAt(offsets[static_cast<size_t>(clamped + slot)], entry)) entry = {};
    fui::ListItem item;
    item.label = entry.word.c_str();
    item.subtitle = entry.gloss.empty() ? nullptr : entry.gloss.c_str();
    item.actionValue = static_cast<int16_t>(clamped + slot);
    rowItems[slot] = item;
  }
  windowStart = clamped;
}

void VocabularyListActivity::openSelected() {
  if (popup.isActive() || !loadSelected()) return;
  const auto& entry = selectedEntry;
  // The word as the title, the sentence it came from as the body (its gloss for
  // words saved before sentences were kept).
  const char* options[] = {tr(STR_DEFINITION), tr(STR_GO_TO_PASSAGE)};
  popup.showMessage(entry.word.c_str(), entry.sentence.empty() ? entry.gloss.c_str() : entry.sentence.c_str(), options,
                    entry.hasPosition() ? 2 : 1, 0, [this](const int choice) {
                      if (choice == 0) {
                        showDefinition();
                      } else {
                        goToPassage();
                      }
                    });
  requestUpdate();
}

void VocabularyListActivity::goToPassage() {
  const auto& entry = selectedEntry;
  if (!entry.hasPosition()) return;
  ProgressChangeResult result;
  result.spineIndex = static_cast<int>(entry.spineIndex);
  result.page = static_cast<int>(entry.page);
  if (entry.textOffset != vocabText::NO_POSITION) {
    // The text offset survives font and layout changes; the page is the fallback.
    result.hasVisibleTextOffset = true;
    result.visibleTextOffset = entry.textOffset;
  }
  setResult(std::move(result));
  finish();
}

void VocabularyListActivity::showDefinition() {
  const std::string word = selectedEntry.word;
  if (word.empty()) return;
  if (!dictionariesOpened) {
    dictionariesOpened = true;
    if (dictionaries.open(SETTINGS.dictionaryName) && dictionaries.needsIndex()) dictionaries.buildIndex();
  }
  std::string definition;
  std::string headword;
  auto result = Dictionary::LookupResult::NotFound;
  if (!dictionaries.isOpen() || !dictionaries.lookup(word.c_str(), definition, headword, &result)) {
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    popup.show(result == Dictionary::LookupResult::LowMemory ? StrId::STR_DICT_LOW_MEMORY : StrId::STR_DICT_NOT_FOUND,
               OPTIONS, 1, 0, [](int) {});
    requestUpdate();
    return;
  }
  auto detail = makeUniqueNoThrow<DictionaryDefinitionActivity>(
      renderer, mappedInput, std::move(headword), std::move(definition), dictionaries.definitionsAreHtml());
  if (!detail || !detail->enableLookup(dictionaries, std::string(word))) {
    LOG_ERR("VOCAB", "OOM: definition for saved word");
    return;
  }
  startActivityForResult(std::move(detail), [this](const ActivityResult&) {
    // Words looked up from inside the entry may have been saved; show them too.
    RenderLock lock;
    reloadOffsets();
    if (nav.selected >= listCount()) nav.selected = std::max(0, listCount() - 1);
    requestUpdate();
  });
}

void VocabularyListActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

void VocabularyListActivity::onRowLongPress(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showDeleteConfirmation();
}

void VocabularyListActivity::showDeleteConfirmation() {
  if (listCount() == 0 || popup.isActive()) return;
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  popup.show(tr(STR_DELETE_WORD_CONFIRM), options, 2, 0, [this](const int index) {
    confirmingDelete = false;
    if (index == 1) deleteSelected();
    requestUpdate();
  });
  requestUpdate();
}

void VocabularyListActivity::deleteSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  if (!VOCAB.removeAt(static_cast<size_t>(nav.selected))) {
    LOG_ERR("VOCAB", "Failed to delete word %d", static_cast<int>(nav.selected));
    return;
  }
  RenderLock lock;  // buildScreen() reads the rows on the render task
  reloadOffsets();
  if (listCount() == 0) {
    finish();
    return;
  }
  if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  nav.follow(listCount());
}

bool VocabularyListActivity::handleCustomInput() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool VocabularyListActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_DELETE_MODE_MS)) {
    showDeleteConfirmation();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return true;
  }
  return false;
}

void VocabularyListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_VOCABULARY), screen.theme().bodyText);
    return;
  }

  if (!mappedInput.hasTouch()) {
    const int helpHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpHeight},
                     tr(STR_HOLD_SELECT_TO_DELETE));
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  refreshRowWindow(nav.top);
  props.items = rowItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(windowCount);
  screen.list(props);
}

void VocabularyListActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
