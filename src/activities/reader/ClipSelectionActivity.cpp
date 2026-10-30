#include "ClipSelectionActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "VocabStore.h"
#include "activities/ActivityResult.h"
#include "clippings/ClippingText.h"
#include "clippings/SelectionGeometry.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "util/DictionarySet.h"
#include "util/VocabText.h"
#include "util/WordDetect.h"
#include "util/WordSelectionInput.h"

using Input = WordSelectionInput;

namespace {

constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr unsigned long TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
constexpr int TOUCH_PAGE_END_DWELL_SLOP_PX = 8;
// Toolbar order, left to right.
constexpr ClippingResult::Action READER_ACTIONS[] = {ClippingResult::Action::Clip, ClippingResult::Action::Note,
                                                     ClippingResult::Action::Lookup};
// Chinese selections can also be saved to the book's vocabulary list.
constexpr ClippingResult::Action CHINESE_READER_ACTIONS[] = {ClippingResult::Action::Clip, ClippingResult::Action::Note,
                                                             ClippingResult::Action::Lookup,
                                                             ClippingResult::Action::SaveWord};
constexpr ClippingResult::Action DEFINITION_ACTIONS[] = {ClippingResult::Action::Lookup};

const char* actionLabel(const ClippingResult::Action action) {
  switch (action) {
    case ClippingResult::Action::Clip:
      return tr(STR_HIGHLIGHT);
    case ClippingResult::Action::Note:
      return tr(STR_ADD_NOTE);
    case ClippingResult::Action::Bookmark:
      return tr(STR_BOOKMARK_OPTION);
    case ClippingResult::Action::SaveWord:
      return tr(STR_SAVE_WORD);
    case ClippingResult::Action::Lookup:
      break;
  }
  return tr(STR_LOOKUP);
}

const char* cleanWordStart(const char* text) {
  if (!text) return "";
  if (clippingText::hasEmSpacePrefix(text)) text += 3;
  while (*text != '\0' && (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t' ||
                           (static_cast<uint8_t>(text[0]) == 0xC2 && static_cast<uint8_t>(text[1]) == 0xA0))) {
    text += static_cast<uint8_t>(text[0]) == 0xC2 ? 2 : 1;
  }
  return text;
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::unique_ptr<Page>> pages, const int marginLeft,
                                             const int marginTop, const int initialX, const int initialY,
                                             const Mode mode)
    : Activity("ClipSelection", renderer, mappedInput),
      pages(std::move(pages)),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY),
      mode(mode) {}

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::span<const std::unique_ptr<Page>> borrowedPages, const int marginLeft,
                                             const int marginTop, const Selection selection, const int initialX,
                                             const int initialY)
    : Activity("ClipSelection", renderer, mappedInput),
      borrowedPages(borrowedPages),
      initialSelection(selection),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY),
      mode(Mode::DefinitionLookup) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  const bool extracted = extractWords();
  if (!extracted && mode == Mode::DefinitionLookup) {
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    actionPopup.show(StrId::STR_DICT_LOW_MEMORY, OPTIONS, 1, 0, [this](int) { cancel(); });
    requestUpdate();
    return;
  }
  if (!extracted || wordCount == 0) {
    if (wordCount == 0) LOG_ERR("CLIP", "No selectable words on current page");
    cancel();
    return;
  }
  uint16_t firstPageRows = 0;
  for (size_t i = 0; i < wordCount; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != 0) break;
    firstPageRows = std::max<uint16_t>(firstPageRows, static_cast<uint16_t>(word.row + 1));
  }
  const int middle = closestInRow(firstPageRows / 2, renderer.getScreenWidth() / 2);
  if (middle >= 0) selected = middle;
  if (initialSelection.valid) {
    for (size_t i = 0; i < wordCount; ++i) {
      const auto& word = words[i];
      if (word.pageOffset == initialSelection.firstPage && word.pageWordIndex == initialSelection.firstWord)
        rangeStart = static_cast<int>(i);
      if (word.pageOffset == initialSelection.lastPage && word.pageWordIndex == initialSelection.lastWord)
        selected = static_cast<int>(i);
    }
  }
  currentPageOffset = words[selected].pageOffset;
  if (initialX >= 0) {
    const int hit = wordAt(initialX, initialY);
    if (hit < 0) {
      cancel();
      return;
    }
    selected = rangeStart = hit;
    selectDictionaryWord(hit);
    refreshPreview();
    // A definition page already holds its own dictionaries; do not keep a second set open.
    if (mode == Mode::DefinitionLookup) dictionaries.close();
    ignoreInitialTouch = true;
  }
  requestUpdate();
}

void ClipSelectionActivity::selectDictionaryWord(const int hit) {
  if (words[hit].isRtl) return;
  const auto joined = [this](const int a, const int b) {
    // Same test buildSelectedText() uses to decide that no space separates two tokens.
    return words[a].endOffset != UINT32_MAX && words[b].startOffset != UINT32_MAX &&
           words[b].startOffset <= words[a].endOffset && !words[b].paragraphStart && !words[b].isRtl;
  };

  // A word hyphenated at a line end is two tokens; select both halves.
  if (wordDetect::shapeOf(cleanWordStart(words[hit].text)).ideograph.empty()) {
    int start = hit;
    while (start > 0 && words[start - 1].discretionaryHyphen && joined(start - 1, start)) --start;
    int end = hit;
    while (end + 1 < static_cast<int>(wordCount) && words[end].discretionaryHyphen && joined(end, end + 1)) ++end;
    rangeStart = start;
    selected = end;
    return;
  }
  if (SETTINGS.dictionaryName[0] == '\0') return;

  // The tokens a word could span: neighbours with no gap in the source text, so a
  // word broken across lines or onto the next preloaded page is still found.
  constexpr int REACH = static_cast<int>(wordDetect::MAX_CHARS) - 1;
  int first = hit;
  while (first > 0 && hit - first < REACH && joined(first - 1, first)) --first;
  int last = hit;
  while (last + 1 < static_cast<int>(wordCount) && last - hit < REACH && joined(last, last + 1)) ++last;
  if (first == last) return;

  std::vector<wordDetect::Token> run;
  run.reserve(last - first + 1);
  for (int i = first; i <= last; ++i) run.push_back({cleanWordStart(words[i].text)});
  const size_t runHit = static_cast<size_t>(hit - first);
  const auto candidates = wordDetect::candidates(run, runHit);
  if (candidates.empty()) return;

  const unsigned long started = millis();
  // A missing .qidx would make every probe scan the whole index; the first Look Up builds it.
  if (!readyDictionaries()) {
    LOG_INF("CLIP", "Word detection skipped: dictionary not ready");
    return;
  }
  std::vector<const char*> texts;
  texts.reserve(candidates.size());
  for (const auto& candidate : candidates) texts.push_back(candidate.text.c_str());
  auto found = makeUniqueNoThrow<bool[]>(texts.size());
  if (!found || !dictionaries.hasHeadwords(texts.data(), texts.size(), found.get())) return;
  const std::vector<bool> hits(found.get(), found.get() + texts.size());
  const auto span = wordDetect::choose(candidates, hits, runHit);
  rangeStart = first + static_cast<int>(span.first);
  selected = first + static_cast<int>(span.last);
  LOG_INF("CLIP", "Word detection: %u candidates, %u chars, %lu ms", static_cast<unsigned>(candidates.size()),
          static_cast<unsigned>(span.last - span.first + 1), millis() - started);
}

bool ClipSelectionActivity::extractWords() {
  wordCount = 0;
  size_t capacity = MAX_SELECTABLE_WORDS;
  if (mode == Mode::DefinitionLookup) {
    capacity = 0;
    for (size_t i = 0; i < pageCount(); ++i) {
      for (const auto& element : pageAt(i).elements) {
        if (element->getTag() == TAG_PageLine) {
          const auto* block = static_cast<const PageLine&>(*element).getBlock();
          if (block && block->valid()) capacity += block->wordCount();
        }
      }
    }
    const size_t bytes = capacity * sizeof(WordBox);
    const auto heap = HalMemory::getDefaultHeap();
    if (capacity > UINT16_MAX || heap.largestBlockBytes < bytes || heap.freeBytes < bytes + 8192) {
      LOG_ERR("DICT", "Low heap for selection (%u bytes)", static_cast<unsigned>(bytes));
      return false;
    }
  }
  words = makeUniqueNoThrow<WordBox[]>(capacity);
  if (!words) {
    LOG_ERR("CLIP", "OOM: selection words (%u bytes)", static_cast<unsigned>(capacity * sizeof(WordBox)));
    return false;
  }
  rowCount = 0;
  const bool needsFontPrewarm = renderer.isSdCardFont(fontId);
  bool paragraphStartPending = false;

  for (size_t pageOffset = 0; pageOffset < pageCount(); ++pageOffset) {
    uint16_t pageWordIndex = 0;
    for (const auto& element : pageAt(pageOffset).elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block || !block->valid()) continue;

      if (needsFontPrewarm && block->wordCount() != 0) {
        uint8_t styleMask = 0;
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          styleMask |= static_cast<uint8_t>(1U << (static_cast<uint8_t>(block->wordStyle(i)) & 0x03));
        }
        // Borrow the line's packed words to prepare advances before measuring, without copying or truncating text.
        const char* text = block->wordText(0);
        const uint16_t lastWord = block->wordCount() - 1;
        const size_t textLength = block->wordText(lastWord) + block->wordTextLen(lastWord) + 1 - text;
        renderer.ensureSdCardFontReady(fontId, &text, &textLength, 1, false, false, styleMask);
      }

      const size_t lineStart = wordCount;
      const bool isRtl = block->getBlockStyle().isRtl;
      const int8_t characterSpacing = block->getBlockStyle().characterSpacing;
      const size_t remaining = capacity - lineStart;
      size_t rtlWordCount = 0;
      const int lineFont = TextBlock::renderFontId(renderer, block->blockFontId(fontId), fontId);
      const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(lineFont));
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        if (block->wordStartsParagraph(i)) paragraphStartPending = true;
        const char* text = block->wordText(i);
        if (!clippingText::hasVisibleText(text)) continue;

        const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        const int wordFont = TextBlock::renderFontId(renderer, block->wordFontId(i, fontId), fontId);
        int width = renderer.getTextAdvanceX(wordFont, text, style, characterSpacing);
        if (width <= 0) continue;
        if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
          width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i)));
        }

        if (!isRtl && wordCount == capacity) break;

        WordBox& word = isRtl ? words[lineStart + (rtlWordCount < remaining ? rtlWordCount : rtlWordCount % remaining)]
                              : words[wordCount++];
        word.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i));
        word.y = static_cast<int16_t>(marginTop + line.yPos + rubyShift + block->wordYOffset(renderer, fontId, i));
        word.width = static_cast<int16_t>(width);
        word.height = static_cast<int16_t>(renderer.getLineHeight(wordFont));
        word.fontId = wordFont;
        word.row = rowCount;
        word.pageOffset = static_cast<uint8_t>(pageOffset);
        word.pageWordIndex = pageWordIndex++;
        word.startOffset = block->wordSourceRange(i).start;
        word.endOffset = block->wordSourceRange(i).end;
        word.text = text;
        word.style = style;
        word.characterSpacing = characterSpacing;
        word.paragraphStart = false;
        word.isRtl = isRtl;
        word.discretionaryHyphen = block->wordHasDiscretionaryHyphen(i);
        if (isRtl) ++rtlWordCount;
      }
      if (isRtl) {
        const size_t stored = std::min(remaining, rtlWordCount);
        wordCount = lineStart + stored;
        if (rtlWordCount > remaining) {
          std::rotate(words.get() + lineStart, words.get() + lineStart + rtlWordCount % remaining,
                      words.get() + wordCount);
        }
        std::reverse(words.get() + lineStart, words.get() + wordCount);
      }
      if (wordCount > lineStart && paragraphStartPending) {
        // Whitespace-only tokens are skipped; keep their boundary on the first selectable logical word.
        const auto first =
            std::min_element(words.get() + lineStart, words.get() + wordCount,
                             [](const WordBox& a, const WordBox& b) { return a.startOffset < b.startOffset; });
        first->paragraphStart = true;
        paragraphStartPending = false;
      }
      if (wordCount != lineStart) ++rowCount;
      if (wordCount == capacity) {
        if (mode != Mode::DefinitionLookup) {
          LOG_ERR("CLIP", "Selectable word cap hit (%u); multi-page selection was truncated",
                  static_cast<unsigned>(capacity));
        }
        break;
      }
    }
    if (wordCount == capacity) break;
  }

  return true;
}

int ClipSelectionActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

int ClipSelectionActivity::wordAt(const int x, int y) const {
  y -= textOffset();
  constexpr int SLOP = 4;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + word.height + SLOP) {
      return i;
    }
  }
  return -1;
}

int ClipSelectionActivity::dragWordAt(const int x, int y) const {
  y -= textOffset();
  int best = -1;
  Rect nearest{};
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    const Rect candidate{word.x, word.y, word.width, word.height};
    if (best < 0 || selectionGeometry::nearerWord(candidate, nearest, x, y)) {
      nearest = candidate;
      best = i;
    }
  }
  return best;
}

bool ClipSelectionActivity::selectionContains(const int x, const int y) const {
  const int offset = textOffset();
  const WordBox* previous = nullptr;
  for (int i = std::min(rangeStart, selected); i <= std::max(rangeStart, selected); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    int left = word.x;
    int right = word.x + word.width;
    if (previous && previous->row == word.row) {
      left = std::min(left, static_cast<int>(previous->x));
      right = std::max(right, previous->x + previous->width);
    }
    if (selectionGeometry::contains(Rect{left, word.y + offset, right - left, word.height}, x, y)) return true;
    previous = &word;
  }
  return false;
}

int ClipSelectionActivity::nextPageStartIndexForTouchDrag() const {
  if (!touchDragHasMoved || rangeStart < 0 || selected < rangeStart) return -1;

  const uint8_t currentPage = words[selected].pageOffset;
  for (int i = selected + 1; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset > currentPage) return i;
  }
  return -1;
}

bool ClipSelectionActivity::isWithinCurrentPageEndDwellSlop(const int x, const int y) const {
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  if (selectionGeometry::atBottomEdge(safe, lineHeight, x, y)) return true;
  if (selected + 1 < static_cast<int>(wordCount) && words[selected + 1].pageOffset == currentPageOffset) return false;
  const WordBox& word = words[selected];
  const int wordY = word.y + textOffset();
  return word.pageOffset == currentPageOffset && x >= word.x - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         x < word.x + word.width + TOUCH_PAGE_END_DWELL_SLOP_PX && y >= wordY - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         y < wordY + word.height + TOUCH_PAGE_END_DWELL_SLOP_PX;
}

void ClipSelectionActivity::moveVertical(const int direction) {
  const int targetRow = static_cast<int>(words[selected].row) + direction;
  if (targetRow < 0 || targetRow >= rowCount) return;
  const int next = closestInRow(static_cast<uint16_t>(targetRow), words[selected].x + words[selected].width / 2);
  if (next >= 0 && next != selected) {
    selectIndex(next);
  }
}

void ClipSelectionActivity::selectIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(wordCount) || index == selected) return;
  selected = index;
  currentPageOffset = words[selected].pageOffset;
  requestUpdate();
}

void ClipSelectionActivity::moveToPage(const int pageOffset) {
  if (pageOffset < 0 || pageOffset >= static_cast<int>(pageCount()) || pageOffset == currentPageOffset) return;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset == pageOffset) {
      selectIndex(i);
      return;
    }
  }
}

bool ClipSelectionActivity::buildSelectedText(const int first, const int last, std::string& text) const {
  text.clear();
  text.reserve(CLIPPING_TEXT_MAX);
  const size_t count = last - first + 1;
  auto order = makeUniqueNoThrow<uint16_t[]>(count);
  if (!order) {
    LOG_ERR("CLIP", "OOM: selection export order");
    return false;
  }
  bool allSourcesKnown = true;
  for (size_t i = 0; i < count; ++i) {
    order[i] = static_cast<uint16_t>(first + i);
    allSourcesKnown &= words[order[i]].startOffset != UINT32_MAX && words[order[i]].endOffset != UINT32_MAX;
  }
  // Keep navigation in visual order and export in logical source order.
  if (allSourcesKnown) {
    std::sort(order.get(), order.get() + count, [this](const uint16_t a, const uint16_t b) {
      if (words[a].startOffset == words[b].startOffset) return a < b;
      return words[a].startOffset < words[b].startOffset;
    });
  }
  for (size_t i = 0; i < count; ++i) {
    const WordBox& current = words[order[i]];
    const char* word = cleanWordStart(current.text);
    char separator = '\0';
    if (i > 0) {
      const WordBox& previous = words[order[i - 1]];
      const bool sourceKnown = previous.endOffset != UINT32_MAX && current.startOffset != UINT32_MAX;
      const bool separated = sourceKnown ? current.startOffset > previous.endOffset
                                         : current.row != previous.row || current.x > previous.x + previous.width + 2;
      if (current.paragraphStart) {
        separator = '\n';
      } else if (separated) {
        separator = ' ';
      }
    }
    if (!clippingText::append(text, word, separator, CLIPPING_TEXT_MAX, current.discretionaryHyphen)) {
      return false;
    }
  }
  return true;
}

void ClipSelectionActivity::confirmSelection(const ClippingResult::Action action) {
  if (rangeStart < 0) {
    rangeStart = selected;
    requestUpdate();
    return;
  }

  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  ClippingResult result;
  result.action = action;
  const auto heap = HalMemory::getDefaultHeap();
  // reserve() aborts on OOM; leave room for the sort order and error popup too.
  const size_t requiredHeap = CLIPPING_TEXT_MAX + 1 + (last - first + 1) * sizeof(uint16_t) + 1024;
  const bool lowMemory = heap.largestBlockBytes < CLIPPING_TEXT_MAX + 1 || heap.freeBytes < requiredHeap;
  if (lowMemory || !buildSelectedText(first, last, result.text)) {
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    actionPopup.show(lowMemory ? StrId::STR_MEMORY_ERROR : StrId::STR_CLIPPING_TOO_LONG, OPTIONS, 1, 0, [](int) {});
    requestUpdate();
    return;
  }
  if (action == ClippingResult::Action::Lookup || action == ClippingResult::Action::SaveWord) {
    // Search with the characters alone: punctuation attached to them is not part of the word.
    std::string chinese = selectionIdeographs(first, last);
    if (!chinese.empty()) {
      result.text = std::move(chinese);
      result.sentence = sentenceAround(first, last);
    }
  }
  result.startPageOffset = words[first].pageOffset;
  result.endPageOffset = words[last].pageOffset;
  result.startWordIndex = words[first].pageWordIndex;
  result.endWordIndex = words[last].pageWordIndex;
  result.wordCount = static_cast<uint16_t>(last - first + 1);
  result.startOffset = UINT32_MAX;
  result.endOffset = 0;
  for (int i = first; i <= last; ++i) {
    if (words[i].startOffset == UINT32_MAX || words[i].endOffset == UINT32_MAX) {
      result.startOffset = result.endOffset = UINT32_MAX;
      break;
    }
    result.startOffset = std::min(result.startOffset, words[i].startOffset);
    result.endOffset = std::max(result.endOffset, words[i].endOffset);
  }
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool ClipSelectionActivity::handleHomeGesture() {
  cancel();
  return true;
}

void ClipSelectionActivity::loopButtons() {
  const uint8_t buttons = selectionInput.pollButtons(mappedInput, millis());
  if (buttons) {
    if (pendingButtonCount < pendingButtons.size()) {
      pendingButtons[pendingButtonCount++] = buttons;
    } else {
      LOG_ERR("CLIP", "Selection button queue full");
    }
  }

  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  size_t processed = 0;
  while (processed < pendingButtonCount) {
    if (handleButtons(pendingButtons[processed++])) break;
  }
  std::move(pendingButtons.begin() + processed, pendingButtons.begin() + pendingButtonCount, pendingButtons.begin());
  pendingButtonCount -= processed;
}

void ClipSelectionActivity::loop() {
  if (wordCount == 0 && !actionPopup.isActive()) {
    cancel();
    return;
  }
  if (!mappedInput.hasTouch()) {
    loopButtons();
    return;
  }

  RenderLock lock;
  if (actionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  int touchX = 0;
  int touchY = 0;
  if (ignoreInitialTouch) {
    if (!mappedInput.isScreenTouchHeld(touchX, touchY)) ignoreInitialTouch = false;
    return;
  }
  if (!touchDragSelecting && mappedInput.wasScreenTapped(touchX, touchY)) {
    if (rangeStart >= 0) {
      const int action = actionAt(touchX, touchY);
      if (action >= 0) {
        confirmSelection(selectionActions()[action]);
        return;
      }
      // The preview line is part of the toolbar: a tap on it keeps the selection.
      if (!preview.empty() && selectionGeometry::contains(previewRect(), touchX, touchY)) return;
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      const bool onStart = words[first].pageOffset == currentPageOffset &&
                           selectionGeometry::contains(handleRect(first, true), touchX, touchY);
      const bool onEnd = words[last].pageOffset == currentPageOffset &&
                         selectionGeometry::contains(handleRect(last, false), touchX, touchY);
      if (!onStart && !onEnd && !selectionContains(touchX, touchY)) cancel();
    } else {
      const int hit = wordAt(touchX, touchY);
      if (hit < 0) {
        cancel();
      } else {
        selected = rangeStart = hit;
        refreshPreview();
        requestUpdate();
      }
    }
    return;
  }
  if (touchDragSelecting) {
    if (mappedInput.isScreenTouchHeld(touchX, touchY)) {
      const int deltaX = touchX - touchDragStartX;
      const int deltaY = touchY - touchDragStartY;
      touchDragHasMoved = touchDragHasMoved || deltaX >= TOUCH_DRAG_MOVEMENT_PX || deltaX <= -TOUCH_DRAG_MOVEMENT_PX ||
                          deltaY >= TOUCH_DRAG_MOVEMENT_PX || deltaY <= -TOUCH_DRAG_MOVEMENT_PX;

      const int hit = dragWordAt(touchX + dragOffsetX, touchY + dragOffsetY);
      if (hit >= 0) {
        selectIndex(hit);
      }

      // Hold at the bottom edge or on the final word to extend onto the next page.
      const int nextPageStart = nextPageStartIndexForTouchDrag();
      if (nextPageStart >= 0 && (isWithinCurrentPageEndDwellSlop(touchX, touchY) ||
                                 isWithinCurrentPageEndDwellSlop(touchX + dragOffsetX, touchY + dragOffsetY))) {
        const unsigned long now = millis();
        if (touchDragPageEndIndex != nextPageStart) {
          touchDragPageEndIndex = nextPageStart;
          touchDragPageEndHeldSince = now;
        } else if (now - touchDragPageEndHeldSince >= TOUCH_PAGE_ADVANCE_HOLD_MS) {
          touchDragPageEndIndex = -1;
          selectIndex(nextPageStart);
          dragOffsetX = dragOffsetY = 0;
        }
      } else {
        touchDragPageEndIndex = -1;
      }
      return;
    }
    touchDragSelecting = false;
    touchDragHasMoved = false;
    touchDragPageEndIndex = -1;
    refreshPreview();
    requestUpdate();
    return;
  } else if (mappedInput.wasScreenTouchPressed(touchX, touchY)) {
    if (rangeStart >= 0) {
      if (actionAt(touchX, touchY) >= 0) return;
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      for (int endpoint = 0; endpoint < 2; ++endpoint) {
        const int index = endpoint == 0 ? first : last;
        if (words[index].pageOffset != currentPageOffset) continue;
        const Rect handle = handleRect(index, endpoint == 0);
        if (touchX < handle.x || touchX >= handle.x + handle.width || touchY < handle.y ||
            touchY >= handle.y + handle.height)
          continue;
        selected = index;
        rangeStart = endpoint == 0 ? last : first;
        dragOffsetX = words[index].x + words[index].width / 2 - touchX;
        dragOffsetY = words[index].y + textOffset() + words[index].height / 2 - touchY;
        touchDragSelecting = true;
        touchDragHasMoved = false;
        touchDragStartX = touchX;
        touchDragStartY = touchY;
        touchDragPageEndIndex = -1;
        return;
      }
      return;
    }
    const int hit = wordAt(touchX, touchY);
    if (hit >= 0) {
      selected = rangeStart = hit;
      dragOffsetX = 0;
      dragOffsetY = 0;
      touchDragSelecting = true;
      touchDragHasMoved = false;
      touchDragStartX = touchX;
      touchDragStartY = touchY;
      touchDragPageEndIndex = -1;
      requestUpdate();
    }
    return;
  }

  const uint8_t buttons = Input::buttonEdges(mappedInput);
  if (buttons & (Input::INPUT_BACK | Input::INPUT_CONFIRM)) {
    handleButtons(buttons);
    syncPreview();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    moveToPage(static_cast<int>(currentPageOffset) + 1);
    syncPreview();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    moveToPage(static_cast<int>(currentPageOffset) - 1);
    syncPreview();
    return;
  }

  handleButtons(buttons);
  syncPreview();
}

bool ClipSelectionActivity::handleButtons(const uint8_t buttons) {
  if (actionPopup.handleButtons(buttons & Input::INPUT_PREVIOUS, buttons & Input::INPUT_NEXT,
                                buttons & Input::INPUT_CONFIRM, buttons & Input::INPUT_BACK,
                                [this] { requestUpdate(); }))
    return true;

  if (buttons & Input::INPUT_BACK) {
    if (rangeStart >= 0 && mode != Mode::DefinitionLookup) {
      rangeStart = -1;
      requestUpdate();
    } else {
      cancel();
    }
    return true;
  }

  if (buttons & Input::INPUT_CONFIRM) {
    confirmSelection(isLookup() ? ClippingResult::Action::Lookup : ClippingResult::Action::Clip);
    return true;
  }

  const int next =
      selectionGeometry::horizontalIndex(selected, static_cast<int>(wordCount), buttons & Input::INPUT_LEFT,
                                         buttons & Input::INPUT_RIGHT, words[selected].isRtl);
  if (next != selected) {
    selectIndex(next);
  } else if (buttons & Input::INPUT_UP) {
    moveVertical(-1);
  } else if (buttons & Input::INPUT_DOWN) {
    moveVertical(1);
  }
  return false;
}

Rect ClipSelectionActivity::handleRect(const int index, const bool start) const {
  const WordBox& word = words[index];
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int size = std::max(24, UITheme::getInstance().getMetrics().verticalSpacing * 2);
  const bool left = start != word.isRtl;
  int boxX = 0;
  int boxWidth = 0;
  // Sit the handle at the character, not at punctuation attached to it.
  highlightSpan(index, start, !start, boxX, boxWidth);
  const int edge = left ? boxX : boxX + boxWidth;
  return Rect{std::clamp(edge - (left ? size : 0), safe.x, safe.x + safe.width - size),
              std::clamp(word.y + textOffset() + word.height, safe.y, safe.y + safe.height - size), size, size};
}

int ClipSelectionActivity::selectionTop() const {
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  int top = renderer.getScreenHeight();
  for (int i = first; i <= last; ++i) {
    if (words[i].pageOffset == currentPageOffset) top = std::min(top, static_cast<int>(words[i].y));
  }
  return top;
}

int ClipSelectionActivity::actionAt(const int x, const int y) const {
  return selectionGeometry::actionAt(actionRect(), UITheme::getInstance().getMetrics().menuSpacing, x, y,
                                     static_cast<int>(selectionActions().size()));
}

std::span<const ClippingResult::Action> ClipSelectionActivity::selectionActions() const {
  if (mode == Mode::DefinitionLookup) return DEFINITION_ACTIONS;
  return selectionIsWord() ? std::span<const ClippingResult::Action>(CHINESE_READER_ACTIONS)
                           : std::span<const ClippingResult::Action>(READER_ACTIONS);
}

bool ClipSelectionActivity::selectionIsWord() const {
  if (rangeStart < 0) return false;
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  // A word, not a passage: a few characters, every one of them Chinese.
  if (last - first + 1 > static_cast<int>(vocabText::MAX_WORD_CHARS)) return false;
  return !selectionIdeographs(first, last).empty();
}

bool ClipSelectionActivity::readyDictionaries() {
  if (!dictionariesChecked) {
    dictionariesChecked = true;
    dictionariesReady =
        SETTINGS.dictionaryName[0] != '\0' && dictionaries.open(SETTINGS.dictionaryName) && !dictionaries.needsIndex();
  }
  return dictionariesReady;
}

void ClipSelectionActivity::syncPreview() {
  if (rangeStart != previewRangeStart || selected != previewSelected) refreshPreview();
}

void ClipSelectionActivity::refreshPreview() {
  preview.clear();
  previewRangeStart = rangeStart;
  previewSelected = selected;
  if (mode == Mode::DefinitionLookup || !mappedInput.hasTouch() || rangeStart < 0) return;
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  // A word or short phrase, not a passage.
  if (last - first + 1 > static_cast<int>(vocabText::MAX_WORD_CHARS)) return;
  std::string word = selectionIdeographs(first, last);
  if (word.empty()) {
    if (last - first + 1 > 4) return;
    std::string text;
    if (!buildSelectedText(first, last, text)) return;
    word = Dictionary::cleanWord(text.c_str());
  }
  if (word.empty() || !readyDictionaries()) return;
  preview = VocabStore::glossFor(dictionaries, word);
}

std::string ClipSelectionActivity::sentenceAround(const int first, const int last) const {
  // Walk out to the sentence ends, staying inside the paragraph; a long
  // sentence is cut to a window around the word and marked with ellipses.
  constexpr int REACH = 30;
  int start = first;
  while (start > 0 && first - start < REACH && !words[start].paragraphStart &&
         !vocabText::endsSentence(cleanWordStart(words[start - 1].text))) {
    --start;
  }
  int end = last;
  while (end + 1 < static_cast<int>(wordCount) && end - last < REACH &&
         !vocabText::endsSentence(cleanWordStart(words[end].text)) && !words[end + 1].paragraphStart) {
    ++end;
  }
  const bool cutStart =
      !words[start].paragraphStart && !(start > 0 && vocabText::endsSentence(cleanWordStart(words[start - 1].text)));
  const bool cutEnd = !vocabText::endsSentence(cleanWordStart(words[end].text)) &&
                      !(end + 1 < static_cast<int>(wordCount) && words[end + 1].paragraphStart);
  std::string text;
  if (!buildSelectedText(start, end, text)) return {};
  std::string sentence = vocabText::sanitize(text, vocabText::MAX_SENTENCE_BYTES - 6);
  if (cutStart) sentence.insert(0, "\xE2\x80\xA6");  // "…"
  if (cutEnd) sentence += "\xE2\x80\xA6";
  return sentence;
}

std::string ClipSelectionActivity::selectionIdeographs(const int first, const int last) const {
  std::vector<wordDetect::Token> tokens;
  tokens.reserve(static_cast<size_t>(last - first + 1));
  for (int i = first; i <= last; ++i) tokens.push_back({cleanWordStart(words[i].text)});
  return wordDetect::ideographText(tokens);
}

void ClipSelectionActivity::highlightSpan(const int index, const bool trimLeft, const bool trimRight, int& x,
                                          int& width) const {
  const WordBox& word = words[index];
  x = word.x;
  width = word.width;
  if (word.isRtl || !word.text) return;
  const char* clean = cleanWordStart(word.text);
  const auto range = wordDetect::highlightRange(clean, trimLeft, trimRight);
  const size_t cleanLength = strlen(clean);
  if (range.begin == 0 && range.end == cleanLength) return;
  // Measure the token text before and through the kept part, from the start the token is drawn at.
  // EpubReaderActivity::drawVocabUnderlines trims the same way for the page underlines.
  const size_t lead = static_cast<size_t>(clean - word.text);
  char buffer[64];
  const auto advance = [&](const size_t bytes) {
    if (bytes == 0) return 0;
    if (bytes >= sizeof(buffer)) return -1;
    memcpy(buffer, word.text, bytes);
    buffer[bytes] = '\0';
    return renderer.getTextAdvanceX(word.fontId, buffer, word.style, word.characterSpacing);
  };
  const int left = range.begin == 0 ? 0 : advance(lead + range.begin);
  const int right = range.end == cleanLength ? word.width : advance(lead + range.end);
  const int fullWidth = word.width;
  if (left < 0 || right < 0 || right <= left || fullWidth <= 1) return;
  const int kept = std::clamp(left, 0, fullWidth - 1);
  x = word.x + kept;
  width = std::clamp(right, kept + 1, fullWidth) - kept;
}

Rect ClipSelectionActivity::actionRect() const {
  const Rect toolbar = toolbarRect();
  const int stripHeight = previewHeight();
  return Rect{toolbar.x, toolbar.y + stripHeight, toolbar.width, toolbar.height - stripHeight};
}

Rect ClipSelectionActivity::previewRect() const {
  const Rect toolbar = toolbarRect();
  return Rect{toolbar.x, toolbar.y, toolbar.width, previewHeight()};
}

int ClipSelectionActivity::previewHeight() const {
  if (preview.empty()) return 0;
  return renderer.getLineHeight(fontId) + UITheme::getInstance().getMetrics().menuSpacing * 2;
}

Rect ClipSelectionActivity::toolbarRect() const {
  Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int rightEdge = std::min(safe.x + safe.width, renderer.getScreenWidth() - right);
  const int bottomEdge = std::min(safe.y + safe.height, renderer.getScreenHeight() - bottom);
  safe.x = std::max(safe.x, left);
  safe.y = std::max(safe.y, top) + std::max(metrics.topPadding, metrics.verticalSpacing);
  safe.width = rightEdge - safe.x;
  safe.height = bottomEdge - safe.y;
  const int font = uiScaleSpec().smallFontId;
  const auto actions = selectionActions();
  int labelWidth = 0;
  for (const auto action : actions) labelWidth = std::max(labelWidth, renderer.getTextWidth(font, actionLabel(action)));
  const int padding = metrics.menuSpacing;
  const int count = static_cast<int>(actions.size());
  const int buttonsWidth = std::min(safe.width, count * (labelWidth + padding * 2) + padding * (count + 1));
  const int lines = labelWidth > (buttonsWidth - padding * (count + 1)) / count ? 2 : 1;
  const int height = std::max(36, renderer.getLineHeight(font) * lines + padding * 2);
  // Widen for the preview line, up to the screen width; a longer one is cut with "…".
  int width = buttonsWidth;
  if (!preview.empty()) {
    width = std::max(width, std::min(safe.width, renderer.getTextWidth(fontId, preview.c_str()) + padding * 4));
  }
  int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  while (first < last && words[first].pageOffset < currentPageOffset) ++first;
  return selectionGeometry::actions(safe, selectionTop(), height + padding * 2 + previewHeight(),
                                    metrics.verticalSpacing, width, words[first].x - padding);
}

void ClipSelectionActivity::drawActions() const {
  if (!preview.empty()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect strip = previewRect();
    const int padding = metrics.menuSpacing;
    const Rect box{strip.x + padding, strip.y + padding / 2, strip.width - padding * 2, strip.height - padding};
    renderer.fillRoundedRect(box.x, box.y, box.width, box.height, metrics.controlRadius, Color::White);
    renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 1, metrics.controlRadius, true);
    // Drawn in the reading font: the line mixes pinyin, English and Chinese.
    const std::string line = renderer.truncatedText(fontId, preview.c_str(), box.width - padding * 2);
    renderer.getFontCacheManager()->prewarmCache(fontId, line.c_str(), 0x01);
    renderer.drawText(fontId, box.x + padding, box.y + (box.height - renderer.getLineHeight(fontId)) / 2, line.c_str());
  }
  const auto actions = selectionActions();
  std::array<const char*, std::size(CHINESE_READER_ACTIONS)> labels{};
  for (size_t i = 0; i < actions.size(); ++i) labels[i] = actionLabel(actions[i]);
  GUI.drawSelectionActions(renderer, actionRect(), labels.data(), static_cast<int>(actions.size()));
}

int ClipSelectionActivity::textOffset() const {
  if (!mappedInput.hasTouch() && wordCount) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const WordBox& cursor = words[selected];
    return selectionGeometry::keepVisible(cursor.y, cursor.height, safe.y, safe.height);
  }
  return 0;
}

int ClipSelectionActivity::textXOffset() const {
  if (mappedInput.hasTouch() || !wordCount) return 0;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const WordBox& cursor = words[selected];
  return selectionGeometry::keepVisible(cursor.x, cursor.width, safe.x, safe.width);
}

void ClipSelectionActivity::prewarmWord(const int index) const {
  if (index >= 0 && index < static_cast<int>(wordCount) && words[index].text) {
    renderer.getFontCacheManager()->prewarmCache(
        words[index].fontId, words[index].text,
        static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[index].style) & 0x03)));
  }
}

void ClipSelectionActivity::ditherGapBetween(const WordBox& a, const WordBox& b, const int offsetX,
                                             const int offset) const {
  if (a.row != b.row || a.pageOffset != b.pageOffset) return;
  const int leftRight = std::min(a.x + a.width, b.x + b.width);
  const int rightLeft = std::max(a.x, b.x);
  if (leftRight < rightLeft) {
    renderer.fillRectDither(leftRight + offsetX, a.y + offset, rightLeft - leftRight, a.height, Color::LightGray);
  }
}

void ClipSelectionActivity::clearGapBetween(const WordBox& a, const WordBox& b, const int offsetX,
                                            const int offset) const {
  if (a.row != b.row || a.pageOffset != b.pageOffset) return;
  const int leftRight = std::min(a.x + a.width, b.x + b.width);
  const int rightLeft = std::max(a.x, b.x);
  if (leftRight < rightLeft) {
    renderer.fillRect(leftRight + offsetX, a.y + offset, rightLeft - leftRight, a.height, false);
  }
}

void ClipSelectionActivity::drawWordClean(const int index, const int offsetX, const int offset) const {
  if (index < 0 || index >= static_cast<int>(wordCount)) return;
  const WordBox& word = words[index];
  if (word.pageOffset != currentPageOffset) return;

  prewarmWord(index);

  renderer.fillRect(word.x + offsetX, word.y + offset, word.width, word.height, false);

  if (index > 0) {
    clearGapBetween(words[index - 1], word, offsetX, offset);
  }
  if (index + 1 < static_cast<int>(wordCount)) {
    clearGapBetween(word, words[index + 1], offsetX, offset);
  }

  renderer.drawText(word.fontId, word.x + offsetX, word.y + offset, word.text, true, word.style,
                    BidiUtils::BidiBaseDir::AUTO, word.characterSpacing);
}

void ClipSelectionActivity::drawWordHighlight(const int index, const int firstSelected, const int lastSelected,
                                              const int offsetX, const int offset) const {
  if (index < 0 || index >= static_cast<int>(wordCount)) return;
  const WordBox& word = words[index];
  if (word.pageOffset != currentPageOffset) return;

  prewarmWord(index);

  if (index > firstSelected) {
    ditherGapBetween(words[index - 1], word, offsetX, offset);
  }
  if (index < lastSelected) {
    ditherGapBetween(word, words[index + 1], offsetX, offset);
  }

  renderer.fillRectDither(word.x + offsetX, word.y + offset, word.width, word.height, Color::LightGray);
  renderer.drawText(word.fontId, word.x + offsetX, word.y + offset, word.text, true, word.style,
                    BidiUtils::BidiBaseDir::AUTO, word.characterSpacing);
}

bool ClipSelectionActivity::renderIncremental() {
  if (actionPopup.isActive() || mappedInput.hasTouch()) return false;
  if (lastRenderedPageOffset < 0 || lastRenderedPageOffset != currentPageOffset) return false;
  const int offset = textOffset();
  const int offsetX = textXOffset();
  if (offset != lastRenderedTextOffset || offsetX != lastRenderedTextXOffset) return false;
  if (lastRenderedSelected < 0 || lastRenderedSelected >= static_cast<int>(wordCount)) return false;
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;

  // Case 1: Moving cursor before range selection starts (rangeStart < 0)
  if (rangeStart < 0 && lastRenderedRangeStart < 0) {
    if (selected == lastRenderedSelected) return true;

    drawWordClean(lastRenderedSelected, offsetX, offset);
    drawWordHighlight(selected, selected, selected, offsetX, offset);
    const WordBox& cursor = words[selected];
    renderer.drawRect(cursor.x + offsetX, cursor.y + offset, cursor.width, cursor.height, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedSelected = selected;
    return true;
  }

  // Case 2: Transition from rangeStart < 0 to rangeStart >= 0 (user just confirmed rangeStart)
  if (rangeStart >= 0 && lastRenderedRangeStart < 0 && rangeStart == selected && selected == lastRenderedSelected) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), isLookup() ? tr(STR_LOOKUP) : tr(STR_DONE),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedRangeStart = rangeStart;
    return true;
  }

  // Case 3: Moving cursor to choose end word (rangeStart >= 0, expanding or shrinking selection)
  if (rangeStart >= 0 && lastRenderedRangeStart == rangeStart) {
    if (selected == lastRenderedSelected) return true;

    const int firstOld = std::min(rangeStart, lastRenderedSelected);
    const int lastOld = std::max(rangeStart, lastRenderedSelected);
    const int firstNew = std::min(rangeStart, selected);
    const int lastNew = std::max(rangeStart, selected);

    if (words[firstOld].pageOffset != currentPageOffset || words[lastOld].pageOffset != currentPageOffset ||
        words[firstNew].pageOffset != currentPageOffset || words[lastNew].pageOffset != currentPageOffset) {
      return false;
    }

    for (int i = firstOld; i <= lastOld; ++i) {
      if (i < firstNew || i > lastNew) {
        drawWordClean(i, offsetX, offset);
      }
    }

    for (int i = firstNew; i <= lastNew; ++i) {
      if (i < firstOld || i > lastOld) {
        drawWordHighlight(i, firstNew, lastNew, offsetX, offset);
      }
    }

    if (lastRenderedSelected >= firstNew && lastRenderedSelected <= lastNew) {
      drawWordHighlight(lastRenderedSelected, firstNew, lastNew, offsetX, offset);
    }

    const WordBox& cursor = words[selected];
    renderer.drawRect(cursor.x + offsetX, cursor.y + offset, cursor.width, cursor.height, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), isLookup() ? tr(STR_LOOKUP) : tr(STR_DONE),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedSelected = selected;
    return true;
  }

  return false;
}

void ClipSelectionActivity::drawSelection() const {
  const int offset = textOffset();
  const int offsetX = textXOffset();
  const int first = rangeStart < 0 ? selected : std::min(rangeStart, selected);
  const int last = rangeStart < 0 ? selected : std::max(rangeStart, selected);
  const WordBox* previous = nullptr;
  for (int i = first; i <= last; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (previous) ditherGapBetween(*previous, word, offsetX, offset);
    // Punctuation attached to the first or last character stays unshaded.
    int boxX = word.x;
    int boxWidth = word.width;
    if (rangeStart >= 0 && (i == first || i == last)) highlightSpan(i, i == first, i == last, boxX, boxWidth);
    renderer.fillRectDither(boxX + offsetX, word.y + offset, boxWidth, word.height, Color::LightGray);
    renderer.drawText(word.fontId, word.x + offsetX, word.y + offset, word.text, true, word.style,
                      BidiUtils::BidiBaseDir::AUTO, word.characterSpacing);
    previous = &word;
  }
  if (rangeStart >= 0 && mappedInput.hasTouch()) {
    if (words[first].pageOffset == currentPageOffset)
      GUI.drawSelectionHandle(renderer, handleRect(first, true), !words[first].isRtl);
    if (words[last].pageOffset == currentPageOffset)
      GUI.drawSelectionHandle(renderer, handleRect(last, false), words[last].isRtl);
    if (!touchDragSelecting) drawActions();
  } else {
    const WordBox& cursor = words[selected];
    renderer.drawRect(cursor.x + offsetX, cursor.y + offset, cursor.width, cursor.height, true);
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (actionPopup.processRender(renderer, mappedInput)) {
    lastRenderedPageOffset = -1;
    return;
  }

  if (renderIncremental()) {
    return;
  }

  const int offset = textOffset();
  const int offsetX = textXOffset();
  renderer.clearScreen();
  const auto clip = renderer.getClipRect();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.setClipRect(safe.x, safe.y, safe.width, safe.height);
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  pageAt(currentPageOffset).render(renderer, fontId, marginLeft + offsetX, marginTop + offset);
  scope.endScanAndPrewarm();
  pageAt(currentPageOffset).render(renderer, fontId, marginLeft + offsetX, marginTop + offset);
  if (wordCount != 0) drawSelection();
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);

  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), rangeStart < 0 ? tr(STR_SELECT) : (isLookup() ? tr(STR_LOOKUP) : tr(STR_DONE)), tr(STR_DIR_LEFT),
      tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  lastRenderedPageOffset = currentPageOffset;
  lastRenderedTextOffset = offset;
  lastRenderedTextXOffset = offsetX;
  lastRenderedRangeStart = rangeStart;
  lastRenderedSelected = selected;
}
