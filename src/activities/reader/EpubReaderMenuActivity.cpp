#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Height of the tier-1 quick-action band (holds ~56px cells with a little vertical breathing room).
constexpr int kQuickRowHeight = 60;
}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      listItems(buildListItems(hasFootnotes, hasBookmarks)),
      moreItems(buildMoreItems()),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

// Tier 2: frequently used actions, followed by a "More…" row that opens tier 3.
std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildListItems(bool hasFootnotes,
                                                                                    bool hasBookmarks) {
  std::vector<MenuItem> items;
  items.reserve(6);
  items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::MORE, StrId::STR_MORE});
  return items;
}

// Tier 3: rarely used actions, reached via the "More…" row.
std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMoreItems() {
  std::vector<MenuItem> items;
  items.reserve(6);
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::openMoreSubmenu() {
  inMoreSubmenu = true;
  zone = Zone::List;
  listIndex = 0;
  requestUpdate();
}

void EpubReaderMenuActivity::closeMoreSubmenu() {
  inMoreSubmenu = false;
  zone = Zone::List;
  // Return to the list with the "More…" row (always the last tier-2 entry) selected.
  listIndex = static_cast<int>(listItems.size()) - 1;
  requestUpdate();
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activate(MenuAction action) {
  if (action == MenuAction::MORE) {
    openMoreSubmenu();
    return;
  }

  if (action == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (action == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(action), pendingOrientation, selectedPageTurnOption});
  finish();
}

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    // The popup acts on button press; if that input closed it, the trailing
    // release must be swallowed below (Back would close the menu, Confirm
    // would re-activate the selected item).
    popupClosing = !optionPopup.isActive();
    return;
  }
  if (popupClosing) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return;  // closing press still held
    }
    popupClosing = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return;  // swallow the release that closed the popup
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (inMoreSubmenu) {
      closeMoreSubmenu();
    } else {
      closeCancelled();
    }
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int quickTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;

  if (!inMoreSubmenu) {
    constexpr int cellGap = 8;
    const int quickCount = static_cast<int>(quickActions.size());
    const int quickAvailable = screen.width - metrics.contentSidePadding * 2 - cellGap * (quickCount - 1);
    const int cellWidth = quickAvailable / quickCount;
    int touchedQuick = quickIndex;
    const auto quickTouch =
        mappedInput.colTouch(touchedQuick, screen.x + metrics.contentSidePadding, cellWidth + cellGap, quickCount,
                             quickTop, quickTop + kQuickRowHeight, cellWidth);
    if (quickTouch != MappedInputManager::RowTouch::None) {
      quickIndex = touchedQuick;
      zone = Zone::Quick;
      if (quickTouch == MappedInputManager::RowTouch::Tap) {
        activate(quickActions[quickIndex].action);
      } else {
        requestUpdate();
      }
      return;
    }
  }

  const int listTop = quickTop + (inMoreSubmenu ? 0 : kQuickRowHeight + metrics.verticalSpacing);
  const int listHeight = screen.height - listTop - metrics.verticalSpacing;
  int touchedList = listIndex;
  const auto& activeList = currentList();
  const auto listTouch =
      handleListTouch(touchedList, static_cast<int>(activeList.size()), listTop, listHeight, false);
  if (listTouch != ListTouchResult::None) {
    const bool zoneChanged = !inMoreSubmenu && zone != Zone::List;
    listIndex = touchedList;
    if (!inMoreSubmenu) zone = Zone::List;
    if (listTouch == ListTouchResult::Activated) {
      activate(activeList[listIndex].action);
    } else if (zoneChanged) {
      requestUpdate();
    }
    return;
  }

  const auto swipe = mappedInput.wasSwipe();

  // Tier 3 (More submenu): plain list navigation. Back returns to tier 2.
  if (inMoreSubmenu) {
    const int listSize = static_cast<int>(moreItems.size());
    if (swipe == MappedInputManager::SwipeDir::Up) {
      listIndex = ButtonNavigator::nextIndex(listIndex, listSize);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      listIndex = ButtonNavigator::previousIndex(listIndex, listSize);
      requestUpdate();
      return;
    }
    buttonNavigator.onNext([this, listSize] {
      listIndex = ButtonNavigator::nextIndex(listIndex, listSize);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, listSize] {
      listIndex = ButtonNavigator::previousIndex(listIndex, listSize);
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activate(moreItems[listIndex].action);
    }
    return;
  }

  if (zone == Zone::Quick) {
    const bool swapped = mappedInput.isNavDirectionSwapped();
    const auto previousButton = swapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
    const auto nextButton = swapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
    if (swipe == MappedInputManager::SwipeDir::Left) {
      quickIndex = ButtonNavigator::nextIndex(quickIndex, static_cast<int>(quickActions.size()));
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Right) {
      quickIndex = ButtonNavigator::previousIndex(quickIndex, static_cast<int>(quickActions.size()));
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Up) {
      zone = Zone::List;
      listIndex = 0;
      requestUpdate();
      return;
    }

    // Front buttons move between cells; the orientation-aware side Down button descends into the list.
    if (mappedInput.wasPressed(previousButton)) {
      quickIndex = ButtonNavigator::previousIndex(quickIndex, static_cast<int>(quickActions.size()));
      requestUpdate();
    } else if (mappedInput.wasPressed(nextButton)) {
      quickIndex = ButtonNavigator::nextIndex(quickIndex, static_cast<int>(quickActions.size()));
      requestUpdate();
    } else if (mappedInput.wasPressed(swapped ? MappedInputManager::Button::Up
                                              : MappedInputManager::Button::Down)) {
      // Descend into the list on the physical side button that means "down/next" for the
      // current orientation. In INVERTED / LANDSCAPE_CCW that is the Up side button, matching
      // the swap the tier-2/3 list already follows via NavNext. Front Left/Right stay on cell
      // movement, so we gate on the side-button axis only to avoid double-firing with Right.
      zone = Zone::List;
      listIndex = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activate(quickActions[quickIndex].action);
    }
    return;
  }

  // Zone::List (tier 2): existing list navigation, but Up from the first row returns to the quick row.
  const int listSize = static_cast<int>(listItems.size());
  if (swipe == MappedInputManager::SwipeDir::Up) {
    listIndex = ButtonNavigator::nextIndex(listIndex, listSize);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    if (listIndex == 0) {
      zone = Zone::Quick;
    } else {
      listIndex = ButtonNavigator::previousIndex(listIndex, listSize);
    }
    requestUpdate();
    return;
  }
  buttonNavigator.onNext([this, listSize] {
    listIndex = ButtonNavigator::nextIndex(listIndex, listSize);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, listSize] {
    if (listIndex == 0) {
      zone = Zone::Quick;
    } else {
      listIndex = ButtonNavigator::previousIndex(listIndex, listSize);
    }
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(listItems[listIndex].action);
  }
}

std::string EpubReaderMenuActivity::valueForRow(MenuAction action) const {
  switch (action) {
    case MenuAction::ROTATE_SCREEN:
      return I18N.get(orientationLabels[pendingOrientation]);
    case MenuAction::AUTO_PAGE_TURN:
      return pageTurnLabels[selectedPageTurnOption];
    case MenuAction::GO_TO_PERCENT:
      return std::to_string(bookProgressPercent) + "%";
    case MenuAction::MORE:
      return std::to_string(moreItems.size());
    default:
      return "";
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const char* headerTitle = inMoreSubmenu ? tr(STR_MORE) : title.c_str();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 headerTitle);

  // Progress summary (chapter page + book %). Kept above the quick row as its natural home.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;

  // Tier 1: quick-action row (hidden while the More submenu is open).
  if (!inMoreSubmenu) {
    GUI.drawQuickActionRow(renderer, Rect{screen.x, contentTop, screen.width, kQuickRowHeight},
                           static_cast<int>(quickActions.size()), zone == Zone::Quick ? quickIndex : -1,
                           [this](int index) { return std::string(I18N.get(quickActions[index].labelId)); });
    contentTop += kQuickRowHeight + metrics.verticalSpacing;
  }

  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  // Tier 2 / tier 3 list. Selection is suppressed (-1) while focus is on the quick row.
  const auto& list = currentList();
  const int listSelection = (inMoreSubmenu || zone == Zone::List) ? listIndex : -1;
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(list.size()), listSelection,
      [&list](int index) { return I18N.get(list[index].labelId); }, nullptr, nullptr,
      [this, &list](int index) { return valueForRow(list[index].action); }, true);

  // Footer / Hints: horizontal cues in the quick row, vertical cues in the lists.
  MappedInputManager::Labels labels;
  if (!inMoreSubmenu && zone == Zone::Quick) {
    labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  } else {
    labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
