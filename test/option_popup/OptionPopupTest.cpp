#include <gtest/gtest.h>

#include "components/OptionPopup.h"

namespace {

constexpr const char* OPTIONS[] = {"Small", "Large"};

TEST(OptionPopupTest, ConfirmReleaseDismissesAReleasePopupWithoutUsingThePress) {
  MappedInputManager input;
  OptionPopup popup;
  int selected = -1;
  int updates = 0;

  popup.show("Font size", OPTIONS, 2, 1, [&selected](int index) { selected = index; });

  input.setPressed(MappedInputManager::Button::Confirm);
  EXPECT_TRUE(popup.handleInput(input, [&updates] { ++updates; }, OptionPopup::DismissEvent::Release));
  EXPECT_TRUE(popup.isActive());
  EXPECT_EQ(selected, -1);
  EXPECT_EQ(updates, 0);

  input.setReleased(MappedInputManager::Button::Confirm);
  EXPECT_TRUE(popup.handleInput(input, [&updates] { ++updates; }, OptionPopup::DismissEvent::Release));
  EXPECT_FALSE(popup.isActive());
  EXPECT_EQ(selected, 1);
  EXPECT_EQ(updates, 1);
}

TEST(OptionPopupTest, BackReleaseDismissesAReleasePopupWithoutSelecting) {
  MappedInputManager input;
  OptionPopup popup;
  bool selected = false;
  int updates = 0;

  popup.show("Font size", OPTIONS, 2, 0, [&selected](int) { selected = true; });

  input.setPressed(MappedInputManager::Button::Back);
  EXPECT_TRUE(popup.handleInput(input, [&updates] { ++updates; }, OptionPopup::DismissEvent::Release));
  EXPECT_TRUE(popup.isActive());
  EXPECT_FALSE(selected);
  EXPECT_EQ(updates, 0);

  input.setReleased(MappedInputManager::Button::Back);
  EXPECT_TRUE(popup.handleInput(input, [&updates] { ++updates; }, OptionPopup::DismissEvent::Release));
  EXPECT_FALSE(popup.isActive());
  EXPECT_FALSE(selected);
  EXPECT_EQ(updates, 1);
}

TEST(OptionPopupTest, DefaultDismissalStillUsesThePressEvent) {
  MappedInputManager input;
  OptionPopup popup;
  int selected = -1;
  int updates = 0;

  popup.show("Font size", OPTIONS, 2, 0, [&selected](int index) { selected = index; });

  input.setReleased(MappedInputManager::Button::Confirm);
  EXPECT_TRUE(popup.handleInput(input, [&updates] { ++updates; }));
  EXPECT_TRUE(popup.isActive());
  EXPECT_EQ(selected, -1);
  EXPECT_EQ(updates, 0);

  input.setPressed(MappedInputManager::Button::Confirm);
  EXPECT_TRUE(popup.handleInput(input, [&updates] { ++updates; }));
  EXPECT_FALSE(popup.isActive());
  EXPECT_EQ(selected, 0);
  EXPECT_EQ(updates, 1);
}

}  // namespace
