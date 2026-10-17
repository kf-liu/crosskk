#pragma once

#ifdef ENABLE_CHINESE_VERSION

#include <cstddef>
#include <cstdint>

#include "WeReadClient.h"

// 进入和退出时的进度同步。只保留一个 Operation：阅读器销毁后上传还要继续，
// 不能挂在阅读器成员上。手动同步会把它释放掉，再自己占用这块工作区。
namespace WeReadBackgroundSync {

enum class BeginResult : uint8_t { Started, Busy, Failed };

struct Choice {
  WeReadProtocol::RemoteProgress remote;
  float localFraction = 0.0f;
  uint32_t localEpoch = 0;
  uint32_t remoteEpoch = 0;
  uint16_t localSpineIndex = 0;
  uint16_t localPageCount = 0;
};

struct ApplyRequest {
  WeReadProtocol::RemoteProgress remote;
  uint16_t localSpineIndex = 0;
  uint16_t localPageCount = 0;
};

// 网络步骤进行中，或远程进度已经取回、还等阅读器落位。
bool busy();
// 章节正在分页时不要碰 SD。调用方每轮设置，阅读器销毁后要清掉。
void setPaused(bool paused);
// 停掉任务并释放 8KB 工作区。手动同步在打开自己的画面之前调用。
void release();

BeginResult beginEnter(const char* bookId, const WeReadClient::ProgressSyncInput& input, uint32_t localEpoch,
                       uint16_t localSpine, uint16_t localPages);
// 替换还在进行的进入比较。同一本书的退出上传已经在跑时直接返回 Started。
BeginResult beginExit(const char* bookId, const WeReadClient::ProgressSyncInput& input);
// 进入比较出现差异后，按用户的选择再走一次。
BeginResult beginDirection(WeReadClient::ProgressSyncMode mode);

bool takeChoice(Choice& out);
bool takeApply(ApplyRequest& out);
// 用户关掉选择框、没有选方向。心跳可以继续。
void dismissChoice();
bool formatWhen(uint32_t epoch, char* buffer, size_t size);

// 每轮主循环推进一步。不占阅读画面，也不清空字形缓存。
void step();

}  // namespace WeReadBackgroundSync

#endif
