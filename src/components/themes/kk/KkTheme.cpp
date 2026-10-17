#include "KkTheme.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "activities/MainTab.h"
#include "components/UiHighDpiProfile.h"
#include "fontIds.h"

namespace {
constexpr int kUnderlineHeight = 4;

const char* labelFor(const MainTab tab) {
  switch (tab) {
    case MainTab::Home:
      return tr(STR_KK_TAB_HOME);
    case MainTab::Bookshelf:
      return tr(STR_KK_TAB_SHELF);
    case MainTab::Apps:
      return tr(STR_APPS_TITLE);
    case MainTab::Library:
      return tr(STR_KK_TAB_FILES);
    case MainTab::Settings:
      return tr(STR_SETTINGS_TITLE);
    case MainTab::Recent:
    case MainTab::Statistics:
    case MainTab::None:
      return "";
  }
  return "";
}
}  // namespace

void KkTheme::drawMainTabBar(const GfxRenderer& renderer, const Rect rect, const MainTab selected) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const int fontId = UiHighDpiProfile::enabled ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int textY = rect.y + std::max(0, (rect.height - kUnderlineHeight - lineHeight) / 2);

  for (size_t i = 0; i < MainTabs::kkValues.size(); ++i) {
    const MainTab tab = MainTabs::kkValues[i];
    const auto bounds = MainTabs::tabBounds(static_cast<int>(i), rect.width);
    const int left = rect.x + bounds.left;
    const int right = rect.x + bounds.right;
    const char* label = labelFor(tab);
    const bool active = tab == selected;
    const auto style = active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(fontId, label, style);
    const int slotWidth = std::max(1, right - left);
    const int textX = left + std::max(0, (slotWidth - textWidth) / 2);
    const GfxRenderer::ClipScope clip(renderer, left, rect.y, slotWidth, rect.height);
    renderer.drawText(fontId, textX, textY, label, true, style);
    if (active) {
      const int markWidth = std::min(slotWidth, std::max(textWidth, 12));
      const int markX = left + std::max(0, (slotWidth - markWidth) / 2);
      // 底栏时下划线贴在靠近内容的上沿，和 INX 底栏一致。
      renderer.fillRect(markX, rect.y, markWidth, kUnderlineHeight);
    }
  }
}
