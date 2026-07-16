#include <iostream>

#include "components/themes/readest/ReadestLayout.h"

namespace {

constexpr int COLUMN_X = 20;
constexpr int COLUMN_Y = 70;
constexpr int COLUMN_WIDTH = 180;
constexpr int COLUMN_HEIGHT = 276;
constexpr int MAX_WIDTH = 160;
constexpr int MAX_HEIGHT = 250;

bool isContained(const ReadestLayout::CoverLayout& cover) {
  return cover.x >= COLUMN_X && cover.y >= COLUMN_Y && cover.x + cover.width <= COLUMN_X + COLUMN_WIDTH &&
         cover.y + cover.height <= COLUMN_Y + COLUMN_HEIGHT;
}

bool checkLayout(const char* name, int sourceWidth, int sourceHeight, int expectedWidth, int expectedHeight) {
  const auto cover = ReadestLayout::fitCover(sourceWidth, sourceHeight, COLUMN_X, COLUMN_Y, COLUMN_WIDTH,
                                             COLUMN_HEIGHT, MAX_WIDTH, MAX_HEIGHT);
  if (cover.width == expectedWidth && cover.height == expectedHeight && isContained(cover)) {
    return true;
  }

  std::cerr << name << " produced " << cover.width << 'x' << cover.height << " at (" << cover.x << ", "
            << cover.y << ")\n";
  return false;
}

bool checkFooterLayout() {
  constexpr int PAGE_TEXT_WIDTH = 36;
  constexpr int PERCENTAGE_TEXT_WIDTH = 22;
  constexpr int BOOKMARK_WIDTH = 16;
  constexpr int BOOKMARK_GAP = 4;
  const auto layout = ReadestLayout::layoutReaderFooter(480, 800, 34, 5, 0, 0, 0, 0, PAGE_TEXT_WIDTH,
                                                        PERCENTAGE_TEXT_WIDTH, 4, 18, 2, true, BOOKMARK_WIDTH,
                                                        BOOKMARK_GAP);
  const bool positioned = layout.pageX == 6 && layout.valueX == 453 && layout.bookmarkX == 46 &&
                          layout.markerX == 30 && layout.textY == 779 && layout.lineX == 6 &&
                          layout.lineY == 771 && layout.lineWidth == 468 && layout.progressWidth == 104;
  const bool separated = layout.bookmarkX + BOOKMARK_WIDTH + BOOKMARK_GAP <= layout.valueX;
  const auto narrowLayout = ReadestLayout::layoutReaderFooter(80, 120, 34, 5, 0, 0, 0, 0, PAGE_TEXT_WIDTH,
                                                              PERCENTAGE_TEXT_WIDTH, 4, 18, 2, true, BOOKMARK_WIDTH,
                                                              BOOKMARK_GAP);
  const bool suppressesCollidingBookmark = narrowLayout.bookmarkX == -1;
  if (positioned && separated && suppressesCollidingBookmark) {
    return true;
  }

  std::cerr << "Session Folio layout failed\n";
  return false;
}

bool checkReadingPace() {
  ReadestLayout::ReadingPace pace;
  ReadestLayout::beginReadingPace(pace, 1000);
  ReadestLayout::recordPageTurn(pace, 31000, true);
  ReadestLayout::recordPageTurn(pace, 61000, true);
  ReadestLayout::recordPageTurn(pace, 91000, true);
  if (pace.samples != 3 || pace.averageSecondsPerPage != 30.0f ||
      ReadestLayout::estimateMinutesLeft(pace, 4, 18) != 7) {
    std::cerr << "Reading pace estimate failed\n";
    return false;
  }

  ReadestLayout::recordPageTurn(pace, 700000, true);
  if (pace.samples != 3 || ReadestLayout::estimateMinutesLeft(pace, 18, 18) != -1) {
    std::cerr << "Reading pace confidence bounds failed\n";
    return false;
  }
  return true;
}

bool checkSleepCoverLayout() {
  const auto layout = ReadestLayout::layoutSleepCover(480, 800, 400, 600, 37);
  const bool passed = layout.cover.x == 40 && layout.cover.y == 61 && layout.cover.width == 400 &&
                      layout.cover.height == 600 && layout.lineX == 24 && layout.lineY == 722 &&
                      layout.lineWidth == 432 && layout.progressWidth == 159 && layout.textY == 737;
  if (!passed) {
    std::cerr << "Sleep cover layout failed\n";
  }
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= checkLayout("portrait", 600, 1000, 150, 250);
  passed &= checkLayout("wide portrait", 800, 1000, 160, 200);
  passed &= checkLayout("landscape", 1600, 900, 160, 90);
  passed &= checkLayout("extremely tall", 1, 10000, 1, 250);
  passed &= checkLayout("extremely wide", 10000, 1, 160, 1);
  passed &= checkLayout("missing cover", 0, 0, 150, 250);
  passed &= checkFooterLayout();
  passed &= checkReadingPace();
  passed &= checkSleepCoverLayout();
  return passed ? 0 : 1;
}
