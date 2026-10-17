#pragma once

#include <cstddef>
#include <cstdint>

// 进行中的后台动作。下拉控制中心在快捷按钮下方的任务区列出全部。阅读页底部用最近
// 开始的一条替换章节或书名。顶栏只放时钟和电量。
namespace BackgroundWork {

enum class Kind : uint8_t { FetchComments, SyncProgress, SyncReadingTime, Count };

inline constexpr int kMax = static_cast<int>(Kind::Count);

void begin(Kind kind);
void end(Kind kind);

uint32_t generation();
// 最近开始的一条，空闲时为 nullptr。文案已经是「正在xxx」。
const char* foregroundLabel();
// 按开始时间从新到旧写入 out，最多 capacity 条。
int copyLabels(const char** out, int capacity);

}  // namespace BackgroundWork
