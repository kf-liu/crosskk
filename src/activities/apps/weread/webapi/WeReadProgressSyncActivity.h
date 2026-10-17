#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/apps/weread/WeReadBackend.h"
#include "activities/apps/weread/WeReadProgressContext.h"
#include "util/BackgroundWork.h"

class Epub;
struct CrossPointPosition;

class WeReadProgressSyncActivity final : public Activity {
 public:
  WeReadProgressSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                             const char* bookId, const WeReadProgressContext& context, WeReadSyncOccasion occasion,
                             uint32_t localProgressEpoch, const char* continuePath = nullptr);

  static WeReadProgressContext makeContext(const Epub& epub, const char* bookId, float localFraction,
                                           const CrossPointPosition& localPosition);
  // progress.bin 的 FAT 修改时间，转成 Unix 秒。没有文件或时间不可用时返回 0。
  static uint32_t readProgressTimestamp(const std::string& cachePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t {
    WifiSelection,
    Starting,
    Syncing,
    ChoosingDirection,
    Success,
    Failed,
    LoginRequired,
  };

  enum class DirectionOption : uint8_t {
    ApplyRemote,
    UploadLocal,
  };

  static constexpr WeReadClient::ProgressSyncMode modeFor(const WeReadSyncOccasion occasion) {
    switch (occasion) {
      case WeReadSyncOccasion::Manual:
      case WeReadSyncOccasion::Enter:
        return WeReadClient::ProgressSyncMode::Compare;
      case WeReadSyncOccasion::Exit:
        return WeReadClient::ProgressSyncMode::UploadLocal;
      case WeReadSyncOccasion::Duration:
        return WeReadClient::ProgressSyncMode::LiveReading;
    }
    return WeReadClient::ProgressSyncMode::Compare;
  }

  State state_ = State::WifiSelection;
  WeReadClient::Operation operation_;
  WeReadClient::Error error_ = WeReadClient::Error::Ok;
  WeReadSyncOccasion occasion_ = WeReadSyncOccasion::Manual;
  WeReadClient::ProgressSyncMode syncMode_ = WeReadClient::ProgressSyncMode::Compare;
  bool durationOnly_ = false;
  bool noReadingTime_ = false;
  bool reportedMarked_ = false;
  uint32_t pendingSeconds_ = 0;
  char beforeLabel_[48] = {};
  char afterLabel_[48] = {};
  WeReadClient::ProgressSyncOutcome outcome_ = WeReadClient::ProgressSyncOutcome::Pending;
  DirectionOption selectedDirection_ = DirectionOption::ApplyRemote;
  std::string epubPath_;
  // 退出当前书后要接着打开的路径。空表示回到首页。只在换书时才有内容。
  std::string continuePath_;
  char bookId_[64] = {};
  WeReadClient::ProgressSyncInput input_;
  uint16_t localSpineIndex_ = 0;
  uint16_t localPageCount_ = 0;
  float remoteFraction_ = 0.0f;
  uint32_t localProgressEpoch_ = 0;
  uint32_t remoteUpdateTime_ = 0;
  bool uploadConflict_ = false;
  bool exitUploadRetried_ = false;
  // 退出同步结束时回到首页。进入和手动同步仍回到这本书。
  bool returnHome_ = false;
  bool leaving_ = false;
  // Set when this screen starts Wi-Fi. Stay-connected leaves the radio up; otherwise exit tears it down.
  bool wifiActivated_ = false;
  bool workNoted_ = false;
  BackgroundWork::Kind workKind_ = BackgroundWork::Kind::SyncProgress;
  std::atomic<bool> fullRefreshPending_{true};

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void startSync();
  void advanceSync();
  void beginSelectedDirection();
  void applyRemoteProgress(const WeReadProtocol::RemoteProgress& remote);
  void returnToReader();
  void leaveSync();
  void finishLeaving();
  void showOrLeave();
  void commitReportedTime();
  const char* resultMessage() const;
  const char* errorMessage() const;
};
