#include "ReadestTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/themes/readest/ReadestLayout.h"
#include "fontIds.h"

namespace {
constexpr int kCoverRadius = 3;
constexpr int kInteractiveInsetX = 20;
constexpr int kSelectableRowGap = 0;
constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kSubtitleFontId = SMALL_FONT_ID;
constexpr int kGuideFontId = SMALL_FONT_ID;

void drawScrollBar(const GfxRenderer& renderer, Rect rect, int itemCount, int pageStartIndex, int pageItems) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) {
    return;
  }

  const int barW = ReadestMetrics::values.scrollBarWidth;
  const int barX = rect.x + rect.width - ReadestMetrics::values.scrollBarRightOffset - barW;
  const int barY = rect.y;
  const int barH = rect.height;

  const int thumbH = std::max(10, (barH * pageItems) / itemCount);
  const int maxStart = std::max(1, itemCount - pageItems);
  const int maxTravel = std::max(1, barH - thumbH);
  const int clampedStart = std::clamp(pageStartIndex, 0, maxStart);
  const int thumbY = barY + (clampedStart * maxTravel) / maxStart;

  renderer.fillRect(barX, thumbY, barW, thumbH);
}

}  // namespace

void ReadestTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                              const char* subtitle) const {
  (void)subtitle;
  // Home screen header is custom-rendered in drawRecentBookCover.
  if (title == nullptr) {
    return;
  }
  const int sidePadding = ReadestMetrics::values.contentSidePadding;
  const int titleX = rect.x + sidePadding;
  const int titleY = rect.y + 17;

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryIconX = rect.x + rect.width - sidePadding - ReadestMetrics::values.batteryWidth;

  // Reserve space for the widest possible percentage text to avoid title/battery overlap
  int batteryGroupLeftX = batteryIconX;
  if (showBatteryPercentage) {
    // Clear a fixed-width area for the battery percentage to avoid ghosting when digit count changes (e.g. 100% -> 99%)
    const int maxTextWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
    batteryGroupLeftX -= maxTextWidth + batteryPercentSpacing;

    const int clearW = maxTextWidth + batteryPercentSpacing + ReadestMetrics::values.batteryWidth;
    const int clearH = std::max(renderer.getTextHeight(SMALL_FONT_ID), ReadestMetrics::values.batteryHeight + 8);
    renderer.fillRect(batteryIconX - maxTextWidth - batteryPercentSpacing, rect.y + 17, clearW, clearH, false);
  }

  const int maxTitleWidth = std::max(0, batteryGroupLeftX - 20 - titleX);
  auto headerTitle = renderer.truncatedText(kTitleFontId, title, maxTitleWidth, EpdFontFamily::BOLD);
  renderer.drawText(kTitleFontId, titleX, titleY, headerTitle.c_str(), true, EpdFontFamily::BOLD);
  drawBatteryRight(renderer,
                   Rect{batteryIconX, rect.y + 17, ReadestMetrics::values.batteryWidth,
                        ReadestMetrics::values.batteryHeight},
                   showBatteryPercentage);
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void ReadestTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                              bool selected) const {
  if (tabs.empty()) {
    return;
  }

  const int slotWidth = rect.width / static_cast<int>(tabs.size());
  const int tabY = rect.y;
  const int tabHeight = rect.height - 4;

  for (size_t i = 0; i < tabs.size(); i++) {
    const int slotX = rect.x + static_cast<int>(i) * slotWidth;
    const auto& tab = tabs[i];

    const int textWidth = renderer.getTextWidth(kTitleFontId, tab.label, EpdFontFamily::BOLD);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = tabY + (tabHeight - renderer.getLineHeight(kTitleFontId)) / 2;
    renderer.drawText(kTitleFontId, textX, textY, tab.label, true,
                      tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (tab.selected) {
      renderer.drawLine(slotX + 16, rect.y + rect.height - 4, slotX + slotWidth - 17, rect.y + rect.height - 4,
                        selected ? 3 : 1, true);
    }
  }

  // Full-width divider between tabs and setting rows.
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

bool ReadestTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect,
                                     const std::vector<TabInfo>& tabs, const int x, const int y,
                                     int& index) const {
  (void)renderer;
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height || x < rect.x || x >= rect.x + rect.width) {
    return false;
  }

  const int slotWidth = std::max(1, rect.width / static_cast<int>(tabs.size()));
  index = std::min(static_cast<int>(tabs.size()) - 1, (x - rect.x) / slotWidth);
  return true;
}

void ReadestTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                                 const int pageCount, std::string title, const int paddingBottom,
                                 const int textYOffset, const bool fillMargin, const bool isPageBookmarked,
                                 const bool pageCountEstimated, const ReaderFooterInfo footerInfo) const {
  (void)textYOffset;
  (void)fillMargin;
  char pageText[24];
  std::snprintf(pageText, sizeof(pageText), "%s%d/%d", pageCountEstimated ? "~" : "", currentPage, pageCount);
  std::string valueText;
  if (!title.empty()) {
    valueText = title;
  } else if (footerInfo.minutesLeftInChapter > 0) {
    char minutesText[48];
    std::snprintf(minutesText, sizeof(minutesText), tr(STR_MIN_LEFT_IN_CHAPTER_FORMAT),
                  footerInfo.minutesLeftInChapter);
    valueText = minutesText;
  } else {
    char progressText[16];
    std::snprintf(progressText, sizeof(progressText), "%.0f%%", bookProgress);
    valueText = progressText;
  }

  int orientedTop;
  int orientedRight;
  int orientedBottom;
  int orientedLeft;
  renderer.getOrientedViewableTRBL(&orientedTop, &orientedRight, &orientedBottom, &orientedLeft);
  (void)orientedTop;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageTextWidth = renderer.getTextWidth(SMALL_FONT_ID, pageText);
  const int bookmarkReserve = isPageBookmarked ? bookmarkStatusIconWidth + bookmarkStatusIconGap * 2 : 0;
  const int maxValueWidth =
      std::max(0, renderer.getScreenWidth() - metrics.statusBarHorizontalMargin * 2 - orientedLeft - orientedRight -
                      pageTextWidth - bookmarkReserve - 20);
  valueText = maxValueWidth > 0 ? renderer.truncatedText(SMALL_FONT_ID, valueText.c_str(), maxValueWidth) : "";
  const auto layout = ReadestLayout::layoutReaderFooter(
      renderer.getScreenWidth(), renderer.getScreenHeight(), UITheme::getInstance().getStatusBarHeight(),
      metrics.statusBarHorizontalMargin, orientedLeft, orientedRight, orientedBottom, paddingBottom,
      pageTextWidth, renderer.getTextWidth(SMALL_FONT_ID, valueText.c_str()), currentPage, pageCount,
      footerInfo.sessionStartPage, isPageBookmarked, bookmarkStatusIconWidth, bookmarkStatusIconGap);

  renderer.drawLine(layout.lineX, layout.lineY, layout.lineX + layout.lineWidth - 1, layout.lineY, true);
  renderer.drawLine(layout.lineX, layout.lineY, layout.lineX + layout.progressWidth - 1, layout.lineY, 3, true);
  if (layout.markerX >= 0) {
    renderer.fillRect(layout.markerX, layout.lineY - 3, 5, 7, false);
    renderer.drawRect(layout.markerX, layout.lineY - 3, 5, 7, true);
  }
  renderer.drawText(SMALL_FONT_ID, layout.pageX, layout.textY, pageText);
  renderer.drawText(SMALL_FONT_ID, layout.valueX, layout.textY, valueText.c_str());
  if (layout.bookmarkX >= 0) {
    drawBookmarkStatusIcon(renderer, layout.bookmarkX, layout.textY + 5);
  }
}

void ReadestTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, std::function<bool()> storeCoverBuffer,
                                       int selectionOverride) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool indexInRange =
      hasContinueReading && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size());
  const int activeBookIndex = indexInRange ? selectorIndex : 0;
  // selectionOverride (-1 = derive from index) lets the caller show an in-range book while
  // the tile is not the active selection, so the metadata/cover never desync from book 0.
  const bool isSelected = selectionOverride >= 0 ? selectionOverride != 0 : indexInRange;
  const int sidePadding = ReadestMetrics::values.contentSidePadding;
  const int tileX = rect.x + sidePadding;
  const int tileY = rect.y + 8;
  const int tileWidth = rect.width - sidePadding * 2;
  const int tileHeight = rect.height - 12;
  const int targetCoverHeight = ReadestMetrics::values.homeCoverHeight;
  const int actionHeight = 44;
  const int coverAreaY = tileY + 28;
  const int coverAreaHeight = std::max(180, std::min(targetCoverHeight, tileHeight - 190));
  const int maxCoverWidth = std::min(240, tileWidth - 40);

  if (renderedCoverWidth == 0 || renderedCoverHeight == 0) {
    const auto defaultCover = ReadestLayout::fitCover(0, 0, tileX, coverAreaY, tileWidth, coverAreaHeight,
                                                      maxCoverWidth, coverAreaHeight);
    renderedCoverWidth = defaultCover.width;
    renderedCoverHeight = defaultCover.height;
  }

  if (hasContinueReading) {
    const RecentBook& book = recentBooks[activeBookIndex];
    if (!coverRendered) {
      auto coverLayout = ReadestLayout::fitCover(0, 0, tileX, coverAreaY, tileWidth, coverAreaHeight, maxCoverWidth,
                                                coverAreaHeight);
      renderedCoverWidth = coverLayout.width;
      renderedCoverHeight = coverLayout.height;
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, targetCoverHeight);

        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            const int bitmapWidth = bitmap.getWidth();
            const int bitmapHeight = bitmap.getHeight();
            if (bitmapWidth > 0 && bitmapHeight > 0) {
              coverLayout = ReadestLayout::fitCover(bitmapWidth, bitmapHeight, tileX, coverAreaY, tileWidth,
                                                    coverAreaHeight, maxCoverWidth, coverAreaHeight);
              renderedCoverWidth = coverLayout.width;
              renderedCoverHeight = coverLayout.height;
              renderer.drawBitmap(bitmap, coverLayout.x, coverLayout.y, coverLayout.width, coverLayout.height);
              renderer.maskRoundedRectOutsideCorners(coverLayout.x, coverLayout.y, coverLayout.width,
                                                     coverLayout.height, kCoverRadius, Color::White);
            } else {
              hasCover = false;
            }
          } else {
            hasCover = false;
          }
          file.close();
        } else {
          hasCover = false;
        }
      }

      if (!hasCover) {
        renderer.drawRoundedRect(coverLayout.x, coverLayout.y, coverLayout.width, coverLayout.height, 1,
                                 kCoverRadius, true);
        renderer.fillRect(coverLayout.x, coverLayout.y + coverLayout.height * 2 / 3, coverLayout.width,
                          coverLayout.height / 3, true);
        renderer.drawIcon(CoverIcon, coverLayout.x + 20, coverLayout.y + 20, 32);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;
    }

    const auto coverLayout = ReadestLayout::centerCover(renderedCoverWidth, renderedCoverHeight, tileX, coverAreaY,
                                                       tileWidth, coverAreaHeight);
    renderer.drawRoundedRect(coverLayout.x, coverLayout.y, coverLayout.width, coverLayout.height, 1, kCoverRadius,
                             true);

    char positionText[24];
    std::snprintf(positionText, sizeof(positionText), tr(STR_BOOK_POSITION_FORMAT), activeBookIndex + 1,
                  static_cast<int>(recentBooks.size()));
    renderer.drawText(kSubtitleFontId, tileX, tileY + 4, positionText);

    const bool showBatteryPercentage =
        SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    const int batteryX = tileX + tileWidth - ReadestMetrics::values.batteryWidth;
    drawBatteryRight(renderer,
                     Rect{batteryX, tileY + 4, ReadestMetrics::values.batteryWidth,
                          ReadestMetrics::values.batteryHeight},
                     showBatteryPercentage);

    // Compute where the block below the metadata begins BEFORE drawing the title/author,
    // so both can be clamped to never overdraw it. The block below is the stats block when
    // any stats are shown, otherwise the Continue Reading action bar. Deterministic geometry.
    const int actionY = tileY + tileHeight - actionHeight;
    const bool hasProgress = book.progressPercent >= 0;
    const bool hasChapterTime = book.minutesLeftInChapter > 0;
    const bool hasStatsBlock = hasProgress || hasChapterTime;
    const int statsTop = hasStatsBlock ? ReadestLayout::layoutHomeStats(actionY).y : actionY;
    const int metadataBottomLimit = statsTop - 4;  // 4px gap above whatever sits below.

    const int metadataY = coverAreaY + coverAreaHeight + 12;
    const auto titleLines = renderer.wrappedText(kTitleFontId, book.title.c_str(), tileWidth, 2);
    const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
    int textY = metadataY;
    for (const auto& line : titleLines) {
      // Drop any title line (including a wrapped 2nd line) that would cross the block below.
      if (textY + titleLineHeight > metadataBottomLimit) break;
      const int lineX =
          tileX + (tileWidth - renderer.getTextWidth(kTitleFontId, line.c_str(), EpdFontFamily::BOLD)) / 2;
      renderer.drawText(kTitleFontId, lineX, textY, line.c_str(), true, EpdFontFamily::BOLD);
      textY += titleLineHeight;
    }
    if (!book.author.empty()) {
      const int authorLineHeight = renderer.getLineHeight(kSubtitleFontId);
      // Draw the author only when the whole line fits above the block below; else drop it.
      if (textY + 4 + authorLineHeight <= metadataBottomLimit) {
        textY += 4;
        const auto author = renderer.truncatedText(kSubtitleFontId, book.author.c_str(), tileWidth);
        const int authorX = tileX + (tileWidth - renderer.getTextWidth(kSubtitleFontId, author.c_str())) / 2;
        renderer.drawText(kSubtitleFontId, authorX, textY, author.c_str());
        textY += authorLineHeight + 6;
      }
    }

    if (hasStatsBlock) {
      constexpr int statColumnGap = 8;
      constexpr int progressBarHeight = 6;
      const auto statsLayout = ReadestLayout::layoutHomeStats(actionY);
      const int statColumnWidth = (tileWidth - statColumnGap) / 2;

      renderer.drawLine(tileX, statsLayout.y, tileX + tileWidth - 1, statsLayout.y, true);

      if (hasProgress) {
        char completionText[32];
        std::snprintf(completionText, sizeof(completionText), tr(STR_READEST_PROGRESS_ONLY_FORMAT),
                      book.progressPercent);
        const auto completion = renderer.truncatedText(kSubtitleFontId, completionText, statColumnWidth);
        renderer.drawText(kSubtitleFontId, tileX, statsLayout.summaryTextY, completion.c_str());
      }

      if (hasChapterTime) {
        char chapterTimeText[48];
        std::snprintf(chapterTimeText, sizeof(chapterTimeText), tr(STR_MIN_LEFT_IN_CHAPTER_FORMAT),
                      book.minutesLeftInChapter);
        const auto chapterTime = renderer.truncatedText(kSubtitleFontId, chapterTimeText, statColumnWidth);
        const int chapterTimeX = tileX + tileWidth - renderer.getTextWidth(kSubtitleFontId, chapterTime.c_str());
        renderer.drawText(kSubtitleFontId, chapterTimeX, statsLayout.summaryTextY, chapterTime.c_str());
      }

      if (hasProgress) {
        renderer.drawRect(tileX, statsLayout.progressBarY, tileWidth, progressBarHeight);
        const int fillWidth = (tileWidth - 4) * std::clamp(book.progressPercent, 0, 100) / 100;
        if (fillWidth > 0) {
          renderer.fillRect(tileX + 2, statsLayout.progressBarY + 2, fillWidth, progressBarHeight - 4);
        }
      }

    }

    if (isSelected) {
      renderer.fillRect(tileX, actionY, tileWidth, actionHeight);
    }
    const int actionTextWidth =
        renderer.getTextWidth(kTitleFontId, tr(STR_CONTINUE_READING), EpdFontFamily::BOLD);
    renderer.drawText(kTitleFontId, tileX + (tileWidth - actionTextWidth) / 2,
                      actionY + (actionHeight - renderer.getLineHeight(kTitleFontId)) / 2, tr(STR_CONTINUE_READING),
                      !isSelected, EpdFontFamily::BOLD);
    renderer.drawLine(tileX, tileY + tileHeight - 1, tileX + tileWidth - 1, tileY + tileHeight - 1, true);
  } else {
    (void)bufferRestored;
    renderer.drawCenteredText(kTitleFontId, rect.y + rect.height / 2 - renderer.getLineHeight(kTitleFontId),
                              tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(kSubtitleFontId, rect.y + rect.height / 2 + 12, tr(STR_START_READING));
  }
}

void ReadestTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon,
                                  const std::function<std::string(int index)>& rowValue) const {
  (void)rowIcon;
  const int sidePadding = ReadestMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowHeight = ReadestMetrics::values.menuRowHeight;
  const int rowGap = kSelectableRowGap;
  const int rowStep = rowHeight + rowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int safeSelectedIndex = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelectedIndex / pageItems) * pageItems;
  const int menuTop = rect.y;
  const int textLineHeight = renderer.getLineHeight(kTitleFontId);
  const int menuMaxWidth = std::max(0, rect.width - sidePadding * 2);

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const std::string label = buttonLabel(i);
    const int rowY = menuTop + (i - pageStartIndex) * rowStep;
    const int rowWidth = menuMaxWidth;
    const bool isSelected = selectedIndex == i;
    if (isSelected) {
      renderer.fillRect(rowX, rowY, rowWidth, rowHeight);
    } else if (i > pageStartIndex) {
      renderer.drawLine(rowX, rowY, rowX + rowWidth - 1, rowY, true);
    }
    const int textY = rowY + (rowHeight - textLineHeight) / 2;
    const int textX = rowX + kInteractiveInsetX;

    // Optional right-aligned value (e.g. book count). Regular weight, inverted when
    // the row is selected; its width is subtracted from the label truncation budget.
    int maxLabelWidth = std::max(0, menuMaxWidth - kInteractiveInsetX * 2);
    if (rowValue) {
      const std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int valueW = renderer.getTextWidth(kTitleFontId, valueText.c_str(), EpdFontFamily::REGULAR);
        renderer.drawText(kTitleFontId, rowX + rowWidth - kInteractiveInsetX - valueW, textY, valueText.c_str(),
                          !isSelected, EpdFontFamily::REGULAR);
        maxLabelWidth = std::max(0, maxLabelWidth - valueW - kInteractiveInsetX);
      }
    }

    const std::string truncatedLabel =
        renderer.truncatedText(kTitleFontId, label.c_str(), maxLabelWidth, EpdFontFamily::REGULAR);
    renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), !isSelected,
                      isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  drawScrollBar(renderer, rect, buttonCount, pageStartIndex, pageItems);
}

void ReadestTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                                 int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? 3 : 2;

  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth - 1, lineY, thickness, true);
    return;
  }

  constexpr int hPadding = 8;
  const int lineW = textWidth + hPadding * 2;
  const int lineStart = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineStart, lineY, lineStart + lineW - 1, lineY, thickness, true);
}

int ReadestTheme::getListRowStep(const bool hasSubtitle) const {
  const int rowHeight = hasSubtitle ? ReadestMetrics::values.listWithSubtitleRowHeight
                                    : ReadestMetrics::values.listRowHeight;
  return rowHeight + kSelectableRowGap;
}

int ReadestTheme::getListPageItems(const int contentHeight, const bool hasSubtitle) const {
  return std::max(1, contentHeight / getListRowStep(hasSubtitle));
}

void ReadestTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue, bool highlightValue,
                            const std::function<bool(int index)>& rowDimmed) const {
  (void)rowIcon;
  (void)highlightValue;
  const bool hasSubtitle = static_cast<bool>(rowSubtitle);
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int subtitleLineHeight = renderer.getLineHeight(kSubtitleFontId);
  constexpr int subtitleTopPadding = 9;
  constexpr int subtitleBottomPadding = 9;
  constexpr int subtitleInterLineGap = 4;
  const int subtitleRowHeight =
      subtitleTopPadding + titleLineHeight + subtitleInterLineGap + subtitleLineHeight + subtitleBottomPadding;
  const int rowHeight = hasSubtitle ? subtitleRowHeight : ReadestMetrics::values.listRowHeight;
  const int rowStep = rowHeight + kSelectableRowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int pageStartIndex = std::max(0, selectedIndex / pageItems) * pageItems;

  const int sidePadding = ReadestMetrics::values.contentSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowWidth = rect.width - sidePadding * 2;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int rowY = rect.y + (i % pageItems) * rowStep;
    const bool isSelected = i == selectedIndex;
    const bool isDimmed = rowDimmed && rowDimmed(i);
    if (isSelected) {
      renderer.fillRect(rowX, rowY, rowWidth, rowHeight);
    } else if (isDimmed) {
      renderer.fillRectDither(rowX, rowY, rowWidth, rowHeight, Color::LightGray);
    }
    if (!isSelected && i > pageStartIndex) {
      renderer.drawLine(rowX, rowY, rowX + rowWidth - 1, rowY, true);
    }

    constexpr int kMinTitleWidth = 40;
    constexpr int kMinValueGap = kInteractiveInsetX;
    int textAreaWidth = rowWidth - kInteractiveInsetX * 2;
    if (rowValue) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValueWidth = std::max(0, rowWidth - kInteractiveInsetX * 2 - kMinValueGap - kMinTitleWidth);
        if (maxValueWidth > 0) {
          const std::string truncatedValue =
              renderer.truncatedText(kTitleFontId, valueText.c_str(), maxValueWidth, EpdFontFamily::REGULAR);
          const int valueW = renderer.getTextWidth(kTitleFontId, truncatedValue.c_str(), EpdFontFamily::REGULAR);
          renderer.drawText(kTitleFontId, rowX + rowWidth - kInteractiveInsetX - valueW,
                            rowY + (rowHeight - renderer.getLineHeight(kTitleFontId)) / 2, truncatedValue.c_str(),
                            !isSelected, EpdFontFamily::REGULAR);
          textAreaWidth = std::max(0, textAreaWidth - valueW - kMinValueGap);
        }
      }
    }

    if (hasSubtitle) {
      const std::string subtitleRaw = rowSubtitle(i);
      auto title = renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, EpdFontFamily::BOLD);

      if (subtitleRaw.empty()) {
        // If there is no subtitle/author, center title vertically in the full row.
        const int centeredTitleY = rowY + (rowHeight - titleLineHeight) / 2;
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, centeredTitleY, title.c_str(), !isSelected,
                          EpdFontFamily::BOLD);
      } else {
        const int titleY = rowY + subtitleTopPadding;
        const int subtitleY = titleY + titleLineHeight + subtitleInterLineGap;
        auto subtitle =
            renderer.truncatedText(kSubtitleFontId, subtitleRaw.c_str(), textAreaWidth, EpdFontFamily::REGULAR);
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, titleY, title.c_str(), !isSelected,
                          EpdFontFamily::BOLD);
        renderer.drawText(kSubtitleFontId, rowX + kInteractiveInsetX, subtitleY, subtitle.c_str(), !isSelected,
                          EpdFontFamily::REGULAR);
      }
    } else {
      const auto style = isSelected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      auto title = renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, style);
      renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX,
                        rowY + (rowHeight - renderer.getLineHeight(kTitleFontId)) / 2, title.c_str(), !isSelected,
                        style);
    }
  }

  drawScrollBar(renderer, rect, itemCount, pageStartIndex, pageItems);
}

void ReadestTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = 20;
  const int hintHeight = ReadestMetrics::values.buttonHintsHeight;
  const int hintY = pageHeight - hintHeight;
  const int textY = hintY + (hintHeight - renderer.getLineHeight(kGuideFontId)) / 2;

  const std::string backLabel = (btn1 && btn1[0] != '\0') ? std::string(btn1) : "";
  const std::string selectText = (btn2 && btn2[0] != '\0') ? std::string(btn2) : "";
  const std::string upText = (btn3 && btn3[0] != '\0') ? std::string(btn3) : "";
  const std::string downText = (btn4 && btn4[0] != '\0') ? std::string(btn4) : "";

  renderer.fillRect(0, hintY, pageWidth, hintHeight, false);
  renderer.drawLine(0, hintY, pageWidth - 1, hintY, true);
  const int selectWidth = renderer.getTextWidth(kGuideFontId, selectText.c_str(), EpdFontFamily::REGULAR);
  const int downWidth = renderer.getTextWidth(kGuideFontId, downText.c_str(), EpdFontFamily::REGULAR);
  const int backX = sidePadding;
  const int selectX = pageWidth / 2 - sidePadding - selectWidth;
  const int upX = pageWidth / 2 + sidePadding;
  const int downX = pageWidth - sidePadding - downWidth;

  if (!backLabel.empty()) {
    renderer.drawText(kGuideFontId, backX, textY, backLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
  renderer.drawText(kGuideFontId, selectX, textY, selectText.c_str(), true, EpdFontFamily::REGULAR);
  renderer.drawText(kGuideFontId, upX, textY, upText.c_str(), true, EpdFontFamily::REGULAR);
  renderer.drawText(kGuideFontId, downX, textY, downText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.setOrientation(origOrientation);
}
