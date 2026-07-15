#pragma once

enum class StrId { STR_BACK, STR_SELECT, STR_DIR_UP, STR_DIR_DOWN };

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }

  const char* get(StrId) const { return ""; }
};

#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
