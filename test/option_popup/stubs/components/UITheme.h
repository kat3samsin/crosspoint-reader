#pragma once

#include <string>
#include <vector>

class GfxRenderer;

class StubTheme {
 public:
  void drawButtonHints(const GfxRenderer&, const char*, const char*, const char*, const char*) const {}
  void drawOptionPopup(const GfxRenderer&, const char*, const std::vector<std::string>&, int) const {}
};

class UITheme {
 public:
  static UITheme& getInstance() {
    static UITheme instance;
    return instance;
  }

  const StubTheme& getTheme() const { return theme; }

 private:
  StubTheme theme;
};

#define GUI UITheme::getInstance().getTheme()
