#pragma once

#include <I18nKeys.h>

#include <array>

#include "CrossPointSettings.h"

namespace SleepScreenOptions {

inline constexpr std::array<StrId, CrossPointSettings::SLEEP_SCREEN_MODE_COUNT> LABELS_BY_MODE = {
    StrId::STR_DARK,      StrId::STR_LIGHT,        StrId::STR_CUSTOM,      StrId::STR_COVER,
    StrId::STR_NONE_OPT,  StrId::STR_COVER_CUSTOM, StrId::STR_QUICK_RESUME,
};

}  // namespace SleepScreenOptions
