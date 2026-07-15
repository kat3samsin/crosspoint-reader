#include <gtest/gtest.h>

#include <array>

#include "SleepScreenOptions.h"

namespace {

TEST(SleepScreenOptionsTest, LabelsMatchPersistedModeValues) {
  constexpr std::array<StrId, CrossPointSettings::SLEEP_SCREEN_MODE_COUNT> expected = {
      StrId::STR_DARK,      StrId::STR_LIGHT,        StrId::STR_CUSTOM,      StrId::STR_COVER,
      StrId::STR_NONE_OPT,  StrId::STR_COVER_CUSTOM, StrId::STR_QUICK_RESUME,
  };

  EXPECT_EQ(SleepScreenOptions::LABELS_BY_MODE, expected);
  EXPECT_EQ(SleepScreenOptions::LABELS_BY_MODE[CrossPointSettings::BLANK], StrId::STR_NONE_OPT);
  EXPECT_EQ(SleepScreenOptions::LABELS_BY_MODE[CrossPointSettings::COVER_CUSTOM], StrId::STR_COVER_CUSTOM);
}

}  // namespace
