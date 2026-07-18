#include "RecentBooksActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <numeric>
#include <string_view>
#include <utility>

#include "CrossPointSettings.h"
#include "LibraryStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TaskWatchdog.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr uint8_t TAB_RECENT = 0;
constexpr uint8_t TAB_BY_BOOK = 1;
constexpr uint8_t TAB_BY_AUTHOR = 2;
constexpr uint8_t TAB_FILES = 3;
constexpr uint8_t TAB_COUNT = 4;

// Cap entries listed per directory in the Files tab to bound RAM on huge folders.
constexpr size_t MAX_FILE_ENTRIES = 512;

bool isSupportedBookFile(const std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name);
}

// Display title for a Files-tab entry: strip the extension from books; for folders
// drop the trailing '/' and (on icon-less themes) wrap in brackets, matching the
// standalone file browser's presentation.
std::string fileEntryTitle(const std::string& entry) {
  if (!entry.empty() && entry.back() == '/') {
    std::string name = entry.substr(0, entry.size() - 1);
    if (!UITheme::getInstance().getTheme().showsFileIcons()) return "[" + name + "]";
    return name;
  }
  const size_t dot = entry.rfind('.');
  return dot == std::string::npos ? entry : entry.substr(0, dot);
}

// Right-aligned value for a Files-tab entry: the file extension (blank for folders).
std::string fileEntryExtension(const std::string& entry) {
  if (!entry.empty() && entry.back() == '/') return "";
  const size_t dot = entry.rfind('.');
  return dot == std::string::npos ? "" : entry.substr(dot);
}

std::string filenameWithoutExtension(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.resize(dot);
  return name;
}

bool naturallyEqual(const std::string& left, const std::string& right) {
  return !FsHelpers::naturalLess(left, right) && !FsHelpers::naturalLess(right, left);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

void RecentBooksActivity::loadLibraryBooks() {
  LIBRARY_STORE.loadFromFile();
  const auto& cachedBooks = LIBRARY_STORE.getBooks();

  std::vector<LibraryBook> discoveredBooks;
  // Conservative pre-reservation so the discovery push_back loop below does not
  // trigger repeated 2x growth reallocs (fragmentation). At least the cached count.
  discoveredBooks.reserve(std::max<size_t>(cachedBooks.size(), 64));
  std::vector<std::string> directories = {"/"};

  // 500-byte name buffer on the heap: the stack budget is <256B (CLAUDE.md). Reused
  // across the whole directory walk, so this is a single allocation, not per-entry.
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!name) {
    LOG_ERR("RBA", "OOM: name buffer (%u bytes)", static_cast<unsigned>(NAME_BUFFER_SIZE));
    return;
  }

  while (!directories.empty()) {
    resetTaskWatchdogIfSubscribed();
    std::string directoryPath = std::move(directories.back());
    directories.pop_back();

    HalFile directory = Storage.open(directoryPath.c_str());
    if (!directory || !directory.isDirectory()) continue;
    directory.rewindDirectory();

    for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      resetTaskWatchdogIfSubscribed();
      entry.getName(name.get(), NAME_BUFFER_SIZE);
      const bool hidden = name[0] == '.';
      if (strcmp(name.get(), ".") == 0 || strcmp(name.get(), "..") == 0 || strcmp(name.get(), ".crosspoint") == 0 ||
          strcmp(name.get(), "System Volume Information") == 0 || (!SETTINGS.showHiddenFiles && hidden)) {
        entry.close();
        continue;
      }

      std::string path = directoryPath;
      if (path.back() != '/') path += '/';
      path += name.get();
      if (entry.isDirectory()) {
        directories.push_back(std::move(path));
      } else if (isSupportedBookFile(name.get())) {
        discoveredBooks.push_back({std::move(path), "", "", entry.fileSize64()});
      }
      entry.close();
    }
    directory.close();
  }

  std::sort(discoveredBooks.begin(), discoveredBooks.end(), [](const LibraryBook& left, const LibraryBook& right) {
    return FsHelpers::naturalLess(left.path, right.path);
  });

  bool popupShown = false;
  Rect popupRect;
  for (size_t i = 0; i < discoveredBooks.size(); i++) {
    resetTaskWatchdogIfSubscribed();
    LibraryBook& book = discoveredBooks[i];
    const auto cached = std::find_if(cachedBooks.begin(), cachedBooks.end(), [&book](const LibraryBook& candidate) {
      return candidate.path == book.path && candidate.size == book.size;
    });
    if (cached != cachedBooks.end() && !cached->title.empty()) {
      book.title = cached->title;
      book.author = cached->author;
      continue;
    }

    if (!popupShown) {
      popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING_LIBRARY));
      popupShown = true;
    }

    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      BookMetadataCache::BookMetadata metadata;
      if (epub.readMetadata(metadata)) {
        book.title = std::move(metadata.title);
        book.author = std::move(metadata.author);
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        book.title = xtc.getTitle();
        book.author = xtc.getAuthor();
      }
    }

    if (book.title.empty()) book.title = filenameWithoutExtension(book.path);
    GUI.fillPopupProgress(renderer, popupRect,
                          static_cast<int>((i + 1) * 100 / std::max<size_t>(1, discoveredBooks.size())));
  }

  LIBRARY_STORE.replaceBooks(std::move(discoveredBooks));
  libraryBooks = LIBRARY_STORE.getBooks();

  booksByTitle.resize(libraryBooks.size());
  std::iota(booksByTitle.begin(), booksByTitle.end(), 0);
  std::sort(booksByTitle.begin(), booksByTitle.end(), [this](const size_t leftIndex, const size_t rightIndex) {
    const auto& left = libraryBooks[leftIndex];
    const auto& right = libraryBooks[rightIndex];
    if (!naturallyEqual(left.title, right.title)) return FsHelpers::naturalLess(left.title, right.title);
    if (!naturallyEqual(left.author, right.author)) return FsHelpers::naturalLess(left.author, right.author);
    return FsHelpers::naturalLess(left.path, right.path);
  });

  booksByAuthor = booksByTitle;
  std::sort(booksByAuthor.begin(), booksByAuthor.end(), [this](const size_t leftIndex, const size_t rightIndex) {
    const auto& left = libraryBooks[leftIndex];
    const auto& right = libraryBooks[rightIndex];
    if (left.author.empty() != right.author.empty()) return !left.author.empty();
    if (!naturallyEqual(left.author, right.author)) return FsHelpers::naturalLess(left.author, right.author);
    if (!naturallyEqual(left.title, right.title)) return FsHelpers::naturalLess(left.title, right.title);
    return FsHelpers::naturalLess(left.path, right.path);
  });
}

void RecentBooksActivity::loadFileEntries() {
  fileEntries.clear();
  fileEntries.reserve(64);

  HalFile directory = Storage.open(filesBasePath.c_str());
  if (!directory || !directory.isDirectory()) return;
  directory.rewindDirectory();

  // Heap name buffer (stack budget is <256B); reused for every entry in this listing.
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!name) {
    LOG_ERR("RBA", "OOM: name buffer (%u bytes)", static_cast<unsigned>(NAME_BUFFER_SIZE));
    return;
  }
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    resetTaskWatchdogIfSubscribed();
    if (fileEntries.size() >= MAX_FILE_ENTRIES) {
      entry.close();
      break;
    }
    entry.getName(name.get(), NAME_BUFFER_SIZE);
    const bool hidden = name[0] == '.';
    if (strcmp(name.get(), ".") == 0 || strcmp(name.get(), "..") == 0 || strcmp(name.get(), ".crosspoint") == 0 ||
        strcmp(name.get(), "System Volume Information") == 0 || (!SETTINGS.showHiddenFiles && hidden)) {
      entry.close();
      continue;
    }
    if (entry.isDirectory()) {
      fileEntries.emplace_back(std::string(name.get()) + "/");
    } else if (isSupportedBookFile(name.get())) {
      fileEntries.emplace_back(name.get());
    }
    entry.close();
  }
  directory.close();

  // Folders first, then books, each in natural order.
  FsHelpers::sortFileList(fileEntries);
}

void RecentBooksActivity::selectTab(const uint8_t newTab) {
  activeTab = newTab;
  selectorIndex = 0;
  if (activeTab == TAB_FILES) {
    filesBasePath = "/";
    loadFileEntries();
  } else {
    // Release the file listing when leaving the Files tab to keep RAM flat.
    fileEntries.clear();
    fileEntries.shrink_to_fit();
    filesBasePath = "/";
  }
  requestUpdate();
}

size_t RecentBooksActivity::activeBookCount() const {
  if (activeTab == TAB_FILES) return fileEntries.size();
  return activeTab == TAB_RECENT ? recentBooks.size() : libraryBooks.size();
}

const std::string& RecentBooksActivity::activeBookPath(const size_t index) const {
  if (activeTab == TAB_RECENT) return recentBooks[index].path;
  const size_t libraryIndex = activeTab == TAB_BY_AUTHOR ? booksByAuthor[index] : booksByTitle[index];
  return libraryBooks[libraryIndex].path;
}

std::string RecentBooksActivity::activeRowTitle(const size_t index) const {
  if (activeTab == TAB_RECENT) return recentBooks[index].title;
  const size_t libraryIndex = activeTab == TAB_BY_AUTHOR ? booksByAuthor[index] : booksByTitle[index];
  const auto& book = libraryBooks[libraryIndex];
  return activeTab == TAB_BY_AUTHOR && !book.author.empty() ? book.author : book.title;
}

std::string RecentBooksActivity::activeRowSubtitle(const size_t index) const {
  if (activeTab == TAB_RECENT) return recentBooks[index].author;
  const size_t libraryIndex = activeTab == TAB_BY_AUTHOR ? booksByAuthor[index] : booksByTitle[index];
  const auto& book = libraryBooks[libraryIndex];
  return activeTab == TAB_BY_AUTHOR && !book.author.empty() ? book.title : book.author;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone before displaying the complete
  // list. addBook also prunes only when a new entry would otherwise evict one.
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();
  loadLibraryBooks();

  selectorIndex = 0;
  activeTab = TAB_RECENT;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  libraryBooks.clear();
  booksByTitle.clear();
  booksByAuthor.clear();
  fileEntries.clear();
  filesBasePath = "/";
}

void RecentBooksActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  const int contentTop = tabTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const bool hasSubtitle = activeTab != TAB_FILES;
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (activeTab == TAB_RECENT && !recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  const auto activateSelection = [this] {
    if (activeTab == TAB_FILES) {
      if (selectorIndex < fileEntries.size()) {
        const std::string entry = fileEntries[selectorIndex];
        if (!entry.empty() && entry.back() == '/') {
          // Drill into the folder.
          if (filesBasePath.back() != '/') filesBasePath += '/';
          filesBasePath += entry.substr(0, entry.size() - 1);
          loadFileEntries();
          selectorIndex = 0;
          requestUpdate();
        } else {
          std::string path = filesBasePath;
          if (path.back() != '/') path += '/';
          path += entry;
          LOG_DBG("RBA", "Selected file: %s", path.c_str());
          onSelectBook(path);
        }
      }
      return;
    }

    if (selectorIndex < activeBookCount()) {
      const std::string& path = activeBookPath(selectorIndex);
      LOG_DBG("RBA", "Selected library book: %s", path.c_str());
      onSelectBook(path);
    }
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Files tab: Back inside a subfolder climbs one level; at root it leaves the activity.
    if (activeTab == TAB_FILES && filesBasePath != "/") {
      const std::string oldPath = filesBasePath;
      filesBasePath.replace(filesBasePath.find_last_of('/'), std::string::npos, "");
      if (filesBasePath.empty()) filesBasePath = "/";
      loadFileEntries();

      // Restore selection onto the folder we came from.
      const std::string dirName = oldPath.substr(oldPath.find_last_of('/') + 1) + "/";
      selectorIndex = 0;
      for (size_t i = 0; i < fileEntries.size(); i++) {
        if (fileEntries[i] == dirName) {
          selectorIndex = i;
          break;
        }
      }
      requestUpdate();
      return;
    }
    onGoHome();
    return;
  }

  const std::vector<TabInfo> tabs = {{tr(STR_LIBRARY_TAB_RECENT), activeTab == TAB_RECENT},
                                     {tr(STR_LIBRARY_TAB_BY_BOOK), activeTab == TAB_BY_BOOK},
                                     {tr(STR_LIBRARY_TAB_BY_AUTHOR), activeTab == TAB_BY_AUTHOR},
                                     {tr(STR_LIBRARY_TAB_FILES), activeTab == TAB_FILES}};
  int touchX = 0;
  int touchY = 0;
  int touchedTab = -1;
  const bool tabTouched = mappedInput.wasScreenTouchDown(touchX, touchY) || mappedInput.wasScreenTapped(touchX, touchY);
  if (tabTouched &&
      GUI.tabIndexFromPoint(renderer, Rect{0, tabTop, renderer.getScreenWidth(), metrics.tabBarHeight}, tabs, touchX,
                            touchY, touchedTab)) {
    if (touchedTab != activeTab) selectTab(static_cast<uint8_t>(touchedTab));
    return;
  }

  int touchedRow = static_cast<int>(selectorIndex);
  const auto listTouch =
      handleListTouch(touchedRow, static_cast<int>(activeBookCount()), contentTop, contentHeight, hasSubtitle);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchedRow);
    if (listTouch == ListTouchResult::Activated) activateSelection();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectTab(static_cast<uint8_t>((activeTab + 1) % TAB_COUNT));
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectTab(static_cast<uint8_t>((activeTab + TAB_COUNT - 1) % TAB_COUNT));
    return;
  }

  const int listSize = static_cast<int>(activeBookCount());
  const int pageItems =
      UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, true, true, hasSubtitle);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    selectTab(static_cast<uint8_t>((activeTab + 1) % TAB_COUNT));
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    selectTab(static_cast<uint8_t>((activeTab + TAB_COUNT - 1) % TAB_COUNT));
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }

  // Vertical list navigation via the fixed side buttons only (front Left/Right drive the
  // tab bar above). Mirrors the pre-tab behaviour: a quick release steps one row while a
  // held button page-jumps. Front buttons are excluded here so a Left/Right tab switch
  // does not also double-fire as list movement.
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (activeBookCount() == 0) {
        selectorIndex = 0;
      } else if (selectorIndex >= activeBookCount()) {
        selectorIndex = activeBookCount() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LIBRARY));

  const std::vector<TabInfo> tabs = {{tr(STR_LIBRARY_TAB_RECENT), activeTab == TAB_RECENT},
                                     {tr(STR_LIBRARY_TAB_BY_BOOK), activeTab == TAB_BY_BOOK},
                                     {tr(STR_LIBRARY_TAB_BY_AUTHOR), activeTab == TAB_BY_AUTHOR},
                                     {tr(STR_LIBRARY_TAB_FILES), activeTab == TAB_FILES}};
  const int tabTop = metrics.topPadding + metrics.headerHeight;
  GUI.drawTabBar(renderer, Rect{0, tabTop, pageWidth, metrics.tabBarHeight}, tabs, true);

  const int contentTop = tabTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const size_t bookCount = activeBookCount();
  if (bookCount == 0) {
    const char* emptyMsg = activeTab == TAB_RECENT   ? tr(STR_NO_RECENT_BOOKS)
                           : activeTab == TAB_FILES  ? tr(STR_NO_FILES_FOUND)
                                                     : tr(STR_NO_BOOKS);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else if (activeTab == TAB_FILES) {
    // Plain file/folder names only — no covers or metadata, to keep RAM flat.
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, bookCount, selectorIndex,
        [this](int index) { return fileEntryTitle(fileEntries[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(fileEntries[index]); },
        [this](int index) { return fileEntryExtension(fileEntries[index]); }, false);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, bookCount, selectorIndex,
        [this](int index) { return activeRowTitle(index); }, [this](int index) { return activeRowSubtitle(index); },
        [this](int index) { return UITheme::getFileIcon(activeBookPath(index)); });
  }

  // Help text
  const char* backLabel = (activeTab == TAB_FILES && filesBasePath != "/") ? tr(STR_BACK) : tr(STR_HOME);
  const auto labels = mappedInput.mapLabels(backLabel, tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
