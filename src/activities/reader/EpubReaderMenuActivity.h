#pragma once
#include <Epub.h>
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DICTIONARY,
    MORE  // Opens the tier-3 "More" submenu in-place; never returned to the reader.
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, bool hasBookmarks);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  // The active zone within the two-tier menu. In the More submenu the zone is always List.
  enum class Zone { Quick, List };

  // Tier 2 (the familiar list) and tier 3 (the "More" submenu) are built once at construction.
  static std::vector<MenuItem> buildListItems(bool hasFootnotes, bool hasBookmarks);
  static std::vector<MenuItem> buildMoreItems();

  const std::vector<MenuItem>& currentList() const { return inMoreSubmenu ? moreItems : listItems; }
  void activate(MenuAction action);
  void openMoreSubmenu();
  void closeMoreSubmenu();
  std::string valueForRow(MenuAction action) const;
  void closeCancelled();

  // Tier 1: fixed quick-action row (Toggle Bookmark, Chapters, Settings).
  const std::array<MenuItem, 3> quickActions = {{{MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK},
                                                 {MenuAction::SELECT_CHAPTER, StrId::STR_MENU_CHAPTERS},
                                                 {MenuAction::TEXT_SETTINGS, StrId::STR_MENU_SETTINGS}}};

  // Tier 2 / tier 3 lists.
  const std::vector<MenuItem> listItems;
  const std::vector<MenuItem> moreItems;

  Zone zone = Zone::Quick;   // Menu opens with the first quick action selected.
  bool inMoreSubmenu = false;
  int quickIndex = 0;        // Selected cell in the quick row (0..2).
  int listIndex = 0;         // Selected row in the active list (tier 2 or tier 3).

  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  // True while the button press that closed the popup is still held; its release
  // must not fall through to the menu's own Back/Confirm handlers.
  bool popupClosing = false;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::array<StrId, 4> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                  StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
