#pragma once

#include "components/themes/inx/InxTheme.h"

// KK 复用 INX 的列表、弹层和度量，只换底部五个主标签。
class KkTheme final : public InxTheme {
 public:
  void drawMainTabBar(const GfxRenderer& renderer, Rect rect, MainTab selected) const override;
};
