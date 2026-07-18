#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PerformanceBenchmark.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "LibraryStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// The Readest "Settings & transfer" submenu: {[OPDS Browser], File Transfer, Settings}.
// File browsing now lives in the Library screen's Files tab, so it is no longer listed here.
int readestMoreItemCount(const bool hasOpdsServers) { return hasOpdsServers ? 3 : 2; }

HomeMenuItem readestMoreIndexToItem(const int index, const bool hasOpdsServers) {
  int i = 0;
  if (hasOpdsServers && index == i++) return HomeMenuItem::OPDS_BROWSER;
  if (index == i++) return HomeMenuItem::FILE_TRANSFER;
  if (index == i) return HomeMenuItem::SETTINGS_MENU;
  return HomeMenuItem::NONE;
}

int readestMoreItemToIndex(const HomeMenuItem item, const bool hasOpdsServers) {
  int i = 0;
  if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsServers ? i : 0;
  if (hasOpdsServers) ++i;
  if (item == HomeMenuItem::FILE_TRANSFER) return i;
  ++i;
  if (item == HomeMenuItem::SETTINGS_MENU) return i;
  return 0;
}

}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  if (isReadestHome()) {
    // Read the cached book count for the Library row. loadFromFile only reads the
    // pre-built cache JSON — it never triggers an SD re-index. If the cache is
    // already populated in the singleton (e.g. the user visited the Library this
    // session) that value is reused as-is.
    if (LIBRARY_STORE.getBooks().empty()) {
      LIBRARY_STORE.loadFromFile();
    }
    libraryBookCount = LIBRARY_STORE.getBooks().size();

    readestBookIndex = 0;
    if (initialMenuItem == HomeMenuItem::NONE) {
      readestMenuIndex = 0;
    } else if (initialMenuItem == HomeMenuItem::RECENTS || initialMenuItem == HomeMenuItem::FILE_BROWSER) {
      // FILE_BROWSER has no home row on the Readest layout (file browsing moved into the
      // Library screen's Files tab), but ActivityManager::goHome still emits it when the
      // standalone file browser exits — reachable via the reader's delete-book flow. Land
      // on the main home with the Library row selected instead of the stale submenu at 0.
      readestMenuIndex = recentBooks.empty() ? 0 : 1;
    } else {
      readestMoreOpen = true;
      selectorIndex = readestMoreItemToIndex(initialMenuItem, hasOpdsServers);
    }
  } else {
    const auto base = static_cast<int>(recentBooks.size());
    selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

bool HomeActivity::isReadestHome() const {
  return SETTINGS.uiTheme == CrossPointSettings::UI_THEME::READEST;
}

void HomeActivity::setReadestBookIndex(const int index) {
  if (readestBookIndex != index) {
    readestBookIndex = index;
    coverRendered = false;
    freeCoverBuffer();
  }
}

void HomeActivity::loop() {
  if (isReadestHome()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

    if (readestMoreOpen) {
      const int menuCount = readestMoreItemCount(hasOpdsServers);
      const auto activateSelection = [this] {
        switch (readestMoreIndexToItem(selectorIndex, hasOpdsServers)) {
          case HomeMenuItem::OPDS_BROWSER:
            onOpdsBrowserOpen();
            break;
          case HomeMenuItem::FILE_TRANSFER:
            onFileTransferOpen();
            break;
          case HomeMenuItem::SETTINGS_MENU:
            onSettingsOpen();
            break;
          default:
            break;
        }
      };

      buttonNavigator.onNext([this, menuCount] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this, menuCount] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
        requestUpdate();
      });
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
        requestUpdate();
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
        requestUpdate();
        return;
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen) {
        readestMoreOpen = false;
        readestMenuIndex = recentBooks.empty() ? 1 : 2;
        requestUpdate();
        return;
      }

      const auto& metrics = UITheme::getInstance().getMetrics();
      const int menuTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      int menuRow = -1;
      const auto menuTouch =
          mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing, menuCount, 0,
                               renderer.getScreenWidth(), metrics.menuRowHeight);
      if (menuTouch != MappedInputManager::RowTouch::None) {
        if (menuTouch == MappedInputManager::RowTouch::Down) {
          if (selectorIndex != menuRow) {
            selectorIndex = menuRow;
            requestUpdate();
          }
        } else {
          selectorIndex = menuRow;
          activateSelection();
        }
        return;
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activateSelection();
      }
      return;
    }

    const int recentCount = static_cast<int>(recentBooks.size());
    const int menuCount = recentBooks.empty() ? 2 : 3;  // Continue reading, Library, Settings & transfer
    const auto activateSelection = [this, recentCount] {
      if (recentCount > 0 && readestMenuIndex == 0) {
        onSelectBook(recentBooks[readestBookIndex].path);
        return;
      }

      const int menuIndex = readestMenuIndex - (recentCount > 0 ? 1 : 0);
      if (menuIndex == 0) {
        onRecentsOpen();
      } else {
        readestMoreOpen = true;
        selectorIndex = 0;
        requestUpdate();
      }
    };

    // The hero owns Left/Right only while it can cycle books. Otherwise the
    // front pair joins the side buttons for vertical menu navigation.
    const bool continueReadingSelected = recentCount > 0 && readestMenuIndex == 0;
    const bool heroCyclesBooks = continueReadingSelected && recentCount > 1;
    if (heroCyclesBooks && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      setReadestBookIndex(ButtonNavigator::nextIndex(readestBookIndex, recentCount));
      requestUpdate();
    } else if (heroCyclesBooks && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      setReadestBookIndex(ButtonNavigator::previousIndex(readestBookIndex, recentCount));
      requestUpdate();
    }

    const auto nextMenuButton = heroCyclesBooks ? MappedInputManager::Button::Down
                                                : MappedInputManager::Button::NavNext;
    const auto previousMenuButton = heroCyclesBooks ? MappedInputManager::Button::Up
                                                    : MappedInputManager::Button::NavPrevious;
    if (mappedInput.wasPressed(nextMenuButton)) {
      readestMenuIndex = ButtonNavigator::nextIndex(readestMenuIndex, menuCount);
      requestUpdate();
    } else if (mappedInput.wasPressed(previousMenuButton)) {
      readestMenuIndex = ButtonNavigator::previousIndex(readestMenuIndex, menuCount);
      requestUpdate();
    }

    const auto swipe = mappedInput.wasSwipe();
    if (continueReadingSelected && recentCount > 1 && swipe == MappedInputManager::SwipeDir::Left) {
      setReadestBookIndex(ButtonNavigator::nextIndex(readestBookIndex, recentCount));
      requestUpdate();
      return;
    }
    if (continueReadingSelected && recentCount > 1 && swipe == MappedInputManager::SwipeDir::Right) {
      setReadestBookIndex(ButtonNavigator::previousIndex(readestBookIndex, recentCount));
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Up) {
      readestMenuIndex = ButtonNavigator::nextIndex(readestMenuIndex, menuCount);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      readestMenuIndex = ButtonNavigator::previousIndex(readestMenuIndex, menuCount);
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
      onSelectBook(recentBooks[0].path);
      return;
    }

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const int menuHeight = metrics.menuRowHeight * 2 + 8;
    const int heroHeight =
        std::max(280, pageHeight - metrics.buttonHintsHeight - metrics.homeMenuTopOffset - menuHeight - 8);

    int tx = 0;
    int ty = 0;
    if (recentCount > 0 && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
        ty >= 0 && ty < heroHeight) {
      if (readestMenuIndex != 0) {
        readestMenuIndex = 0;
        requestUpdate();
      }
      return;
    }

    if (recentCount > 0 && mappedInput.wasTapInRect(0, 0, renderer.getScreenWidth(), heroHeight)) {
      readestMenuIndex = 0;
      activateSelection();
      return;
    }

    const int menuTop = heroHeight + metrics.homeMenuTopOffset;
    int menuRow = -1;
    const auto menuTouch =
        mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing, 2, 0,
                             renderer.getScreenWidth(), metrics.menuRowHeight);
    if (menuTouch != MappedInputManager::RowTouch::None) {
      const int touchedIndex = menuRow + (recentCount > 0 ? 1 : 0);
      if (menuTouch == MappedInputManager::RowTouch::Down) {
        if (readestMenuIndex != touchedIndex) {
          readestMenuIndex = touchedIndex;
          requestUpdate();
        }
      } else {
        readestMenuIndex = touchedIndex;
        activateSelection();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
    }
    return;
  }

  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card). backPressSeen guards against the stale
  // release of the Back press that closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
      ty >= metrics.homeTopPadding && ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (selectorIndex != 0) {
      selectorIndex = 0;
      requestUpdate();
    }
    return;
  }

  if (!recentBooks.empty() &&
      mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight)) {
    selectorIndex = 0;
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing,
                                              renderedMenuCount, 0, INT32_MAX, metrics.menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  if (isReadestHome()) {
    renderReadestHome();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
  PerformanceBenchmark::recordHomePaint();

  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::renderReadestHome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (readestMoreOpen) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_SETTINGS_AND_TRANSFER));
    std::vector<const char*> menuItems = {tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE)};
    std::vector<UIIcon> menuIcons = {Transfer, Settings};
    if (hasOpdsServers) {
      menuItems.insert(menuItems.begin(), tr(STR_OPDS_BROWSER));
      menuIcons.insert(menuIcons.begin(), Library);
    }
    const int menuTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    GUI.drawButtonMenu(renderer,
                       Rect{0, menuTop, pageWidth,
                            pageHeight - menuTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
                       static_cast<int>(menuItems.size()), selectorIndex,
                       [&menuItems](int index) { return std::string(menuItems[index]); },
                       [&menuIcons](int index) { return menuIcons[index]; });
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    PerformanceBenchmark::recordHomePaint();
    return;
  }

  const int menuHeight = metrics.menuRowHeight * 2 + 8;
  const int heroHeight =
      std::max(280, pageHeight - metrics.buttonHintsHeight - metrics.homeMenuTopOffset - menuHeight - 8);
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  const int recentCount = static_cast<int>(recentBooks.size());
  const bool continueReadingSelected = recentCount > 0 && readestMenuIndex == 0;

  // Cache only the maximum cover-art envelope. The whole hero is close to a
  // full framebuffer and can fragment the device heap.
  const int tileX = metrics.contentSidePadding;
  const int tileWidth = pageWidth - metrics.contentSidePadding * 2;
  const int coverAreaY = 36;
  const int coverAreaHeight = std::max(180, std::min(metrics.homeCoverHeight, heroHeight - 202));
  const int maxCoverWidth = std::min(240, tileWidth - 40);
  coverRectX = tileX + (tileWidth - maxCoverWidth) / 2;
  coverRectY = coverAreaY;
  coverRectW = maxCoverWidth;
  coverRectH = coverAreaHeight;
  GUI.drawRecentBookCover(renderer, Rect{0, 0, pageWidth, heroHeight}, recentBooks, readestBookIndex,
                          coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this), continueReadingSelected ? 1 : 0);

  const std::vector<const char*> menuItems = {tr(STR_LIBRARY), tr(STR_SETTINGS_AND_TRANSFER)};
  const std::vector<UIIcon> menuIcons = {Library, Settings};
  const int menuTop = heroHeight + metrics.homeMenuTopOffset;
  const int menuSelectorIndex = readestMenuIndex - (recentCount > 0 ? 1 : 0);

  // Right-aligned book count on the Library row, sourced from the already-loaded
  // library cache (loaded once at onEnter — no SD re-index here). Blank when the
  // library has never been indexed (count 0).
  char libraryCountText[24] = {0};
  if (libraryBookCount > 0) {
    std::snprintf(libraryCountText, sizeof(libraryCountText), tr(STR_BOOK_COUNT_FORMAT),
                  static_cast<unsigned>(libraryBookCount));
  }
  GUI.drawButtonMenu(renderer, Rect{0, menuTop, pageWidth, menuHeight}, static_cast<int>(menuItems.size()),
                     menuSelectorIndex,
                     [&menuItems](int index) { return std::string(menuItems[index]); },
                     [&menuIcons](int index) { return menuIcons[index]; },
                     [&libraryCountText](int index) {
                       return index == 0 ? std::string(libraryCountText) : std::string();
                     });

  const bool heroCyclesBooks = continueReadingSelected && recentCount > 1;
  const auto labels = mappedInput.mapLabels("", continueReadingSelected ? tr(STR_OPEN) : tr(STR_SELECT),
                                            heroCyclesBooks ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP),
                                            heroCyclesBooks ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  PerformanceBenchmark::recordHomePaint();

  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
