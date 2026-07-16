#pragma once

#include <algorithm>
#include <cstdint>

namespace ReadestLayout {

struct CoverLayout {
  int x;
  int y;
  int width;
  int height;
};

struct SleepCoverLayout {
  CoverLayout cover;
  int lineX;
  int lineY;
  int lineWidth;
  int progressWidth;
  int textY;
};

struct ReaderFooterLayout {
  int pageX;
  int valueX;
  int bookmarkX;
  int markerX;
  int textY;
  int lineX;
  int lineY;
  int lineWidth;
  int progressWidth;
};

constexpr ReaderFooterLayout layoutReaderFooter(int screenWidth, int screenHeight, int statusBarHeight,
                                                int horizontalMargin, int orientedLeft, int orientedRight,
                                                int orientedBottom, int paddingBottom, int pageTextWidth,
                                                int valueTextWidth, int currentPage, int pageCount,
                                                int sessionStartPage, bool isPageBookmarked, int bookmarkWidth,
                                                int bookmarkGap) {
  const int pageX = horizontalMargin + orientedLeft + 1;
  const int valueX = screenWidth - horizontalMargin - orientedRight - valueTextWidth;
  const int proposedBookmarkX = pageX + pageTextWidth + bookmarkGap;
  const bool bookmarkFits =
      isPageBookmarked && proposedBookmarkX + bookmarkWidth + bookmarkGap <= valueX;
  const int lineX = pageX;
  const int lineWidth = std::max(1, screenWidth - horizontalMargin * 2 - orientedLeft - orientedRight - 2);
  const int safePageCount = std::max(1, pageCount);
  const int safeCurrentPage = std::clamp(currentPage, 0, safePageCount);
  const int progressWidth = std::max(1, lineWidth * safeCurrentPage / safePageCount);
  const bool markerVisible = sessionStartPage > 0 && sessionStartPage <= safePageCount;
  const int rawMarkerX = markerVisible ? lineX + lineWidth * (sessionStartPage - 1) / safePageCount - 2 : -1;
  const int markerX = markerVisible ? std::clamp(rawMarkerX, lineX, lineX + lineWidth - 5) : -1;
  const int lineY = screenHeight - statusBarHeight - orientedBottom - paddingBottom + 5;

  return {
      .pageX = pageX,
      .valueX = valueX,
      .bookmarkX = bookmarkFits ? proposedBookmarkX : -1,
      .markerX = markerX,
      .textY = lineY + 8,
      .lineX = lineX,
      .lineY = lineY,
      .lineWidth = lineWidth,
      .progressWidth = progressWidth,
  };
}

struct ReadingPace {
  std::uint32_t lastTurnMs = 0;
  float averageSecondsPerPage = 0.0f;
  std::uint8_t samples = 0;
};

inline void beginReadingPace(ReadingPace& pace, const std::uint32_t nowMs) { pace.lastTurnMs = nowMs; }

inline void recordPageTurn(ReadingPace& pace, const std::uint32_t nowMs, const bool countSample) {
  if (pace.lastTurnMs == 0) {
    pace.lastTurnMs = nowMs;
    return;
  }

  const std::uint32_t elapsedMs = nowMs - pace.lastTurnMs;
  pace.lastTurnMs = nowMs;
  if (!countSample || elapsedMs < 3000 || elapsedMs > 300000) {
    return;
  }

  const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
  if (pace.samples == 0) {
    pace.averageSecondsPerPage = elapsedSeconds;
  } else {
    pace.averageSecondsPerPage = pace.averageSecondsPerPage * 0.75f + elapsedSeconds * 0.25f;
  }
  if (pace.samples < UINT8_MAX) {
    pace.samples++;
  }
}

inline int estimateMinutesLeft(const ReadingPace& pace, const int currentPage, const int pageCount) {
  if (pace.samples < 3 || pace.averageSecondsPerPage <= 0.0f || pageCount <= currentPage) {
    return -1;
  }
  const float secondsLeft = static_cast<float>(pageCount - currentPage) * pace.averageSecondsPerPage;
  return std::clamp(static_cast<int>((secondsLeft + 59.0f) / 60.0f), 1, 999);
}

constexpr CoverLayout centerCover(int width, int height, int columnX, int columnY, int columnWidth,
                                  int columnHeight) {
  return {
      .x = columnX + (columnWidth - width) / 2,
      .y = columnY + (columnHeight - height) / 2,
      .width = width,
      .height = height,
  };
}

constexpr CoverLayout fitCover(int sourceWidth, int sourceHeight, int columnX, int columnY, int columnWidth,
                               int columnHeight, int maxWidth, int maxHeight) {
  const int availableWidth = maxWidth < columnWidth ? maxWidth : columnWidth;
  const int availableHeight = maxHeight < columnHeight ? maxHeight : columnHeight;

  if (sourceWidth <= 0 || sourceHeight <= 0) {
    sourceWidth = 3;
    sourceHeight = 5;
  }

  int width;
  int height;
  if (static_cast<std::int64_t>(sourceWidth) * availableHeight >
      static_cast<std::int64_t>(sourceHeight) * availableWidth) {
    width = availableWidth;
    height = static_cast<int>(static_cast<std::int64_t>(sourceHeight) * availableWidth / sourceWidth);
    if (height < 1) {
      height = 1;
    }
  } else {
    height = availableHeight;
    width = static_cast<int>(static_cast<std::int64_t>(sourceWidth) * availableHeight / sourceHeight);
    if (width < 1) {
      width = 1;
    }
  }

  return centerCover(width, height, columnX, columnY, columnWidth, columnHeight);
}

constexpr SleepCoverLayout layoutSleepCover(int screenWidth, int screenHeight, int sourceWidth, int sourceHeight,
                                            int progressPercent) {
  constexpr int horizontalMargin = 24;
  constexpr int topMargin = 28;
  constexpr int bottomPanelHeight = 78;
  constexpr int coverToLineGap = 28;

  const int lineY = screenHeight - bottomPanelHeight;
  const int columnWidth = std::max(1, screenWidth - horizontalMargin * 2);
  const int columnHeight = std::max(1, lineY - topMargin - coverToLineGap);
  const int maxWidth = std::min(std::max(1, sourceWidth), columnWidth);
  const int maxHeight = std::min(std::max(1, sourceHeight), columnHeight);
  const int safeProgress = std::clamp(progressPercent, 0, 100);

  return {
      .cover = fitCover(sourceWidth, sourceHeight, horizontalMargin, topMargin, columnWidth, columnHeight, maxWidth,
                        maxHeight),
      .lineX = horizontalMargin,
      .lineY = lineY,
      .lineWidth = columnWidth,
      .progressWidth = columnWidth * safeProgress / 100,
      .textY = lineY + 15,
  };
}

}  // namespace ReadestLayout
