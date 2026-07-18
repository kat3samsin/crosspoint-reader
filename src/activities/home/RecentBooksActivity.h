#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "LibraryStore.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;
  size_t selectorIndex = 0;
  uint8_t activeTab = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  std::vector<LibraryBook> libraryBooks;
  std::vector<size_t> booksByTitle;
  std::vector<size_t> booksByAuthor;

  // Files tab state: plain SD file/folder listing for the current directory.
  // Folder entries carry a trailing '/'; book entries are bare filenames.
  // Only populated while the Files tab is active (cleared on tab switch / exit)
  // to keep RAM flat.
  std::string filesBasePath = "/";
  std::vector<std::string> fileEntries;

  // Data loading
  void loadRecentBooks();
  void loadLibraryBooks();
  void loadFileEntries();
  void selectTab(uint8_t newTab);
  size_t activeBookCount() const;
  const std::string& activeBookPath(size_t index) const;
  std::string activeRowTitle(size_t index) const;
  std::string activeRowSubtitle(size_t index) const;

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
