#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  constexpr int ruleWidth = 180;
  constexpr int ruleGap = 28;
  const int brandY = pageHeight / 2 - renderer.getLineHeight(NOTOSERIF_18_FONT_ID);
  renderer.drawLine((pageWidth - ruleWidth) / 2, brandY - ruleGap, (pageWidth + ruleWidth) / 2, brandY - ruleGap);
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, brandY, tr(STR_READEST), true, EpdFontFamily::BOLD);
  renderer.drawLine((pageWidth - ruleWidth) / 2, brandY + renderer.getLineHeight(NOTOSERIF_18_FONT_ID) + ruleGap,
                    (pageWidth + ruleWidth) / 2, brandY + renderer.getLineHeight(NOTOSERIF_18_FONT_ID) + ruleGap);
  renderer.drawCenteredText(SMALL_FONT_ID, brandY + renderer.getLineHeight(NOTOSERIF_18_FONT_ID) + ruleGap + 18,
                            tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
