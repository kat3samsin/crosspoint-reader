#include "DictionaryWordSelectActivity.h"

#include <BidiUtils.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "util/HighlightStore.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// DictionaryHighlight mode: Confirm held at least this long looks the word
// up; a shorter press anchors/saves the highlight selection.
constexpr unsigned long DICT_LOOKUP_HOLD_MS = 400;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  extractWords();
  buildReadingOrder();
  resetCursorToMiddle();
  if (mode != Mode::Dictionary && !HighlightStore::loadRanges(bookPath, savedRanges)) savedRanges.clear();
  requestUpdate();
}

// Start on the middle row's word nearest mid-screen instead of top-left:
// any word on the page is then at most half a page of moves away.
void DictionaryWordSelectActivity::resetCursorToMiddle() {
  selected = 0;
  if (words.empty()) return;
  const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
  if (initial >= 0) selected = initial;
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  words.reserve(128);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (!isSelectableToken(text)) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      box.style = block->wordStyle(i);
      box.sourceOrdinal = block->wordSourceOrdinal(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.text = text;
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
  }
}

// Index of the word whose box (with finger-sized slop) contains the touch
// point; -1 when the touch lands on no word. Boxes never overlap after the
// slop grows them, at worst they touch, so first hit wins.
int DictionaryWordSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;  // matches the highlight box (+2) plus finger error
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + lineHeight + SLOP) {
      return i;
    }
  }
  return -1;
}

// Rebuilds the reading order of the page's words: rows top to bottom, RTL
// rows right to left. Needed because TextBlock keeps only the visual
// (left-to-right) order, so a highlight saved by visual index would come out
// reversed for Arabic/Hebrew text. Row direction is a majority vote of each
// word's first strong bidi class, so embedded LTR words (numbers, Latin
// names) inside an RTL row don't flip it.
void DictionaryWordSelectActivity::buildReadingOrder() {
  const size_t count = words.size();
  readingOrder.clear();
  readingOrder.reserve(count);
  readingPos.assign(count, 0);

  size_t rowStart = 0;
  while (rowStart < count) {
    size_t rowEnd = rowStart + 1;
    while (rowEnd < count && words[rowEnd].row == words[rowStart].row) rowEnd++;

    int rtlStrong = 0;
    int ltrStrong = 0;
    for (size_t i = rowStart; i < rowEnd; i++) {
      // fallback 0 answers "is the first strong char R/AL?"; fallback 1
      // answers "is it L?"; words with no strong char count for neither.
      if (BidiUtils::detectParagraphLevel(words[i].text, 0) == 1) {
        rtlStrong++;
      } else if (BidiUtils::detectParagraphLevel(words[i].text, 1) == 0) {
        ltrStrong++;
      }
    }

    if (rtlStrong > ltrStrong) {
      for (size_t i = rowEnd; i > rowStart; i--) readingOrder.push_back(static_cast<uint16_t>(i - 1));
    } else {
      for (size_t i = rowStart; i < rowEnd; i++) readingOrder.push_back(static_cast<uint16_t>(i));
    }
    rowStart = rowEnd;
  }

  for (size_t pos = 0; pos < readingOrder.size(); pos++) {
    readingPos[readingOrder[pos]] = static_cast<uint16_t>(pos);
  }
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::performLookup() {
  popup = Popup::Busy;
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build
    // the sidecar ourselves, which is handled below.
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(words[selected].text, definition, headword, &result);

  if (found) {
    popup = Popup::None;
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                          std::move(definition)),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  // Name the failure: a genuine miss is "Not found"; a word that WAS found but
  // couldn't be read is a real error — and we distinguish decompression from a
  // low-memory allocation from a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::LookupResult::NotFound:
      default:
        popup = Popup::NotFound;
        popupMsg = StrId::STR_DICT_NOT_FOUND;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::handleConfirmRelease() {
  switch (mode) {
    case Mode::Dictionary:
      performLookup();
      break;
    case Mode::Highlight:
      if (anchor < 0 && selectedSavedRange() >= 0) {
        deleteSelectedHighlight();
      } else {
        toggleHighlight();
      }
      break;
    case Mode::DictionaryHighlight:
      if (mappedInput.getHeldTime() >= DICT_LOOKUP_HOLD_MS) {
        performLookup();
      } else if (anchor < 0 && selectedSavedRange() >= 0) {
        deleteSelectedHighlight();
      } else {
        toggleHighlight();
      }
      break;
  }
}

// True when the row's stored (visual) order runs opposite to reading order.
bool DictionaryWordSelectActivity::rowIsRtl(const uint16_t row) const {
  int first = -1;
  int last = -1;
  for (size_t i = 0; i < words.size(); i++) {
    if (words[i].row != row) continue;
    if (first < 0) first = static_cast<int>(i);
    last = static_cast<int>(i);
  }
  return first >= 0 && readingPos[first] > readingPos[last];
}

void DictionaryWordSelectActivity::resetCarried() {
  carriedText.clear();
  carriedText.shrink_to_fit();
  carriedLens.clear();
  firstPageSelStart = -1;
}

// Loads and displays another page of the section, rebuilding the word list.
// On failure (bad index, load error, page with nothing selectable) the
// current page stays and false is returned.
bool DictionaryWordSelectActivity::showPage(const int pageIndex) {
  if (!section || pageIndex < 0 || pageIndex >= static_cast<int>(section->pageCount)) return false;
  auto next = section->loadPage(pageIndex);
  if (!next) return false;
  auto prev = std::move(page);
  page = std::move(next);
  extractWords();
  buildReadingOrder();
  if (words.empty()) {
    // An image-only page would strand the selection; back out.
    page = std::move(prev);
    extractWords();
    buildReadingOrder();
    return false;
  }
  sectionPageIndex = pageIndex;
  snapshotIdx = -1;
  drawnLo = drawnHi = -1;
  requestUpdate();
  return true;
}

// Extends the anchored selection onto the next page: everything from the
// selection start to the end of the current page joins carriedText and the
// selection re-anchors at the top of the new page.
bool DictionaryWordSelectActivity::advancePage() {
  if (carriedLens.size() >= MAX_CARRIED_PAGES) return false;
  const int lo = std::min(readingPos[anchor], readingPos[selected]);
  const size_t lenBefore = carriedText.size();
  for (size_t p = lo; p < words.size(); p++) {
    if (!carriedText.empty()) carriedText += ' ';
    carriedText += words[readingOrder[p]].text;
  }
  if (!showPage(sectionPageIndex + 1)) {
    carriedText.resize(lenBefore);
    return false;
  }
  if (carriedLens.empty()) firstPageSelStart = lo;
  carriedLens.push_back(lenBefore);
  anchor = readingOrder[0];
  selected = anchor;
  return true;
}

// Undoes one page advance: the current page's part of the selection is
// dropped and the previous page is shown again, selected from where the
// passage entered it through its end.
bool DictionaryWordSelectActivity::retreatPage() {
  if (carriedLens.empty()) return false;
  if (!showPage(sectionPageIndex - 1)) return false;
  carriedText.resize(carriedLens.back());
  carriedLens.pop_back();
  const int startPos =
      carriedLens.empty() ? std::min(std::max(firstPageSelStart, 0), static_cast<int>(words.size()) - 1) : 0;
  anchor = readingOrder[startPos];
  selected = readingOrder[words.size() - 1];
  return true;
}

// Cross-page selection: while anchored, a forward move past the last word
// (or Down on the last row) continues the selection onto the next page; the
// symmetric backward move on the first word/row returns. True when the key
// press was consumed.
bool DictionaryWordSelectActivity::handleCrossPageNavigation() {
  if (anchor < 0 || !section || words.empty()) return false;
  const uint16_t row = words[selected].row;
  const bool rtl = rowIsRtl(row);
  const bool fwdKey =
      mappedInput.wasPressed(rtl ? MappedInputManager::Button::ScreenLeft : MappedInputManager::Button::ScreenRight);
  const bool backKey =
      mappedInput.wasPressed(rtl ? MappedInputManager::Button::ScreenRight : MappedInputManager::Button::ScreenLeft);
  const int pos = readingPos[selected];
  if ((fwdKey && pos == static_cast<int>(words.size()) - 1) ||
      (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown) && row == rowCount - 1)) {
    return advancePage();
  }
  if (!carriedLens.empty() &&
      ((backKey && pos == 0) || (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp) && row == 0))) {
    return retreatPage();
  }
  return false;
}

// First short press anchors a selection at the current word; the second saves
// the anchored range as a highlight and shows the outcome popup.
void DictionaryWordSelectActivity::toggleHighlight() {
  if (anchor < 0) {
    anchor = selected;
    anchoredSourceOrdinal = words[selected].sourceOrdinal;
    resetCarried();
    // The cursor word is already highlighted; when the framebuffer is clean
    // (snapshot tracks it) seed the painted range from it so extending the
    // selection takes the incremental path without a repaint.
    if (snapshotIdx == selected) {
      drawnLo = drawnHi = readingPos[selected];
    } else {
      drawnLo = drawnHi = -1;
    }
    snapshotIdx = -1;  // the single-word snapshot is not maintained while selecting
    requestUpdate();
    return;
  }
  const bool ok = saveHighlight();
  resetCarried();
  anchor = -1;
  anchoredSourceOrdinal = Highlights::NO_WORD_ORDINAL;
  drawnLo = drawnHi = -1;
  popup = ok ? Popup::Saved : Popup::Error;
  popupMsg = ok ? StrId::STR_HIGHLIGHT_SAVED : StrId::STR_HIGHLIGHT_SAVE_FAILED;
  popupTime = millis();
  requestUpdate();
}

// Joins the selected words in reading order (see buildReadingOrder), after
// any text carried over from previous pages, and appends the passage to the
// highlights markdown file.
bool DictionaryWordSelectActivity::saveHighlight() {
  if (anchoredSourceOrdinal == Highlights::NO_WORD_ORDINAL ||
      words[selected].sourceOrdinal == Highlights::NO_WORD_ORDINAL) {
    return false;
  }
  const int lo = std::min(readingPos[anchor], readingPos[selected]);
  const int hi = std::max(readingPos[anchor], readingPos[selected]);
  size_t length = carriedText.size();
  for (int p = lo; p <= hi; p++) length += strlen(words[readingOrder[p]].text) + 1;
  std::string passage;
  passage.reserve(length);
  passage.append(carriedText);
  for (int p = lo; p <= hi; p++) {
    if (!passage.empty()) passage += ' ';
    passage += words[readingOrder[p]].text;
  }
  Highlights::Range range;
  range.spineIndex = spineIndex;
  range.startWord = std::min(anchoredSourceOrdinal, words[selected].sourceOrdinal);
  range.endWord = std::max(anchoredSourceOrdinal, words[selected].sourceOrdinal);
  return HighlightStore::save(bookPath, bookTitle, chapterTitle, passage, range);
}

int DictionaryWordSelectActivity::selectedSavedRange() const {
  if (words.empty() || words[selected].sourceOrdinal == Highlights::NO_WORD_ORDINAL) return -1;
  const auto match = std::find_if(savedRanges.begin(), savedRanges.end(), [&](const Highlights::Range& range) {
    return Highlights::contains(range, spineIndex, words[selected].sourceOrdinal);
  });
  return match == savedRanges.end() ? -1 : static_cast<int>(match - savedRanges.begin());
}

void DictionaryWordSelectActivity::deleteSelectedHighlight() {
  const int index = selectedSavedRange();
  if (index < 0) return;

  const bool ok = HighlightStore::remove(bookPath, savedRanges[index]);
  if (ok) savedRanges.erase(savedRanges.begin() + index);
  popup = ok ? Popup::Deleted : Popup::Error;
  popupMsg = ok ? StrId::STR_HIGHLIGHT_DELETED : StrId::STR_HIGHLIGHT_DELETE_FAILED;
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::drawHighlightControls() {
  if (mode == Mode::Dictionary || words.empty()) {
    drawHints();
    return;
  }
  const char* back = anchor >= 0 ? tr(STR_CANCEL) : tr(STR_BACK);
  const char* confirm = tr(STR_HIGHLIGHT_START);
  if (anchor >= 0) {
    confirm = tr(STR_HIGHLIGHT_SAVE);
  } else if (selectedSavedRange() >= 0) {
    confirm = tr(STR_DELETE);
  }
  const auto labels = mappedInput.mapLabels(back, confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error || popup == Popup::Saved || popup == Popup::Deleted) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      if (popup == Popup::Saved || popup == Popup::Deleted) {
        // Saving or deleting completes the task: return straight to the reader.
        finish();
        return;
      }
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (anchor >= 0) {
      // Cancel the in-progress selection; a full repaint clears its boxes.
      // A cross-page selection also returns to the page it started from.
      anchor = -1;
      anchoredSourceOrdinal = Highlights::NO_WORD_ORDINAL;
      drawnLo = drawnHi = -1;
      resetCarried();
      if (sectionPageIndex != originalPageIndex && showPage(originalPageIndex)) {
        resetCursorToMiddle();
      }
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen && !words.empty()) {
    handleConfirmRelease();
    return;
  }

  if (words.empty()) return;
  // Touch: a touch-down moves the highlight to the touched word (differential
  // repaint), a tap on a word selects and looks it up in one go.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != selected) {
      selected = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      selected = hit;
      handleConfirmRelease();
    }
    return;
  }

  if (handleCrossPageNavigation()) return;
  const bool hasNextWord = selected + 1 < static_cast<int>(words.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) && selected > 0) {
    selected--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) && hasNextWord) {
    selected++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box) — the highlight is drawn regardless, but the
// next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = lineHeight + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  return saved;
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: the full-bleed reader page has no
// spare gutter for them, so a hint box there would hide text.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm and navigation are all no-ops
  // (guarded by words.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT),
                                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Paints (black box, white text) or clears (white box, black text) one word's
// highlight box directly, without snapshots. When a visually adjacent word on
// the same row also lies inside the [rangeLo, rangeHi] reading-position
// range, the box is stretched across the inter-word gap so the passage shows
// as one connected highlight (clearing passes the previously drawn range so
// stale bridges are erased too). Clearing can clip a few pixels of adjacent
// glyphs that overlap the padded box, which the next full repaint restores.
void DictionaryWordSelectActivity::paintWordBox(const int idx, const bool highlighted, const int rangeLo,
                                                const int rangeHi) {
  const WordBox& word = words[idx];
  int hx = word.x - 2;
  int hxEnd = word.x + word.width + 2;
  int hy = word.y - 2;
  int hh = lineHeight + 4;

  const auto inRange = [&](const int n) {
    return n >= 0 && n < static_cast<int>(words.size()) && words[n].row == word.row && readingPos[n] >= rangeLo &&
           readingPos[n] <= rangeHi;
  };
  // Words within a row are stored left to right, so idx-1/idx+1 are the
  // visual neighbours regardless of the text's reading direction.
  if (inRange(idx - 1)) hx = std::min(hx, static_cast<int>(words[idx - 1].x) + words[idx - 1].width + 2);
  if (inRange(idx + 1)) hxEnd = std::max(hxEnd, static_cast<int>(words[idx + 1].x) - 2);

  if (hx < 0) hx = 0;
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }
  if (hxEnd <= hx || hh <= 0) return;
  renderer.fillRect(hx, hy, hxEnd - hx, hh, highlighted);
  // Outside a PrewarmScope the glyph cache may be empty; batch-load this word.
  renderer.getFontCacheManager()->prewarmCache(fontId, word.text,
                                               static_cast<uint8_t>(1u << (static_cast<uint8_t>(word.style) & 0x03)));
  renderer.drawText(fontId, word.x, word.y, word.text, !highlighted, word.style);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Incremental selection repaint: the framebuffer holds a clean page with
  // reading positions [drawnLo, drawnHi] highlighted; repaint only the words
  // entering or leaving the selection instead of re-running the two-pass
  // page render.
  if (popup == Popup::None && anchor >= 0 && drawnLo >= 0 && !words.empty()) {
    const int lo = std::min(readingPos[anchor], readingPos[selected]);
    const int hi = std::max(readingPos[anchor], readingPos[selected]);
    for (int p = drawnLo; p <= drawnHi; p++) {
      if (p < lo || p > hi) paintWordBox(readingOrder[p], false, drawnLo, drawnHi);
    }
    for (int p = lo; p <= hi; p++) {
      if (p < drawnLo || p > drawnHi) paintWordBox(readingOrder[p], true, lo, hi);
    }
    drawnLo = lo;
    drawnHi = hi;
    drawHighlightControls();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && anchor < 0 && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHighlightControls();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) {
    if (anchor >= 0) {
      const int lo = std::min(readingPos[anchor], readingPos[selected]);
      const int hi = std::max(readingPos[anchor], readingPos[selected]);
      for (int p = lo; p <= hi; p++) paintWordBox(readingOrder[p], true, lo, hi);
      drawnLo = lo;
      drawnHi = hi;
      snapshotIdx = -1;
    } else {
      drawHighlightWithSnapshot();
      drawnLo = drawnHi = -1;
    }
  }

  drawHighlightControls();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    drawnLo = drawnHi = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
