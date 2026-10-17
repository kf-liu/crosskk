#pragma once

#include <cstdint>

#include "WeReadBackend.h"

struct WeReadProgressContext {
  float localFraction = 0.0f;
  uint32_t localTocIndex = 0;
  uint32_t localOffset = 0;
  uint16_t localSpineIndex = 0;
  uint16_t localPageNumber = 0;
  uint16_t localPageCount = 0;
  WeReadBackend::Client::LocalOffsetBasis localOffsetBasis = WeReadBackend::Client::LocalOffsetBasis::None;
};

// 一次进度同步是怎么被打开的。进入和退出在后台走，不换阅读画面；菜单仍走手动比较。
enum class WeReadSyncOccasion : uint8_t {
  Manual,
  Enter,
  Exit,
  Duration,
};

static_assert(sizeof(WeReadProgressContext) == 20);
