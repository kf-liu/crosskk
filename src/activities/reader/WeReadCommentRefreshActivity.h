#pragma once

#include <atomic>
#include <cstdint>

#include "activities/Activity.h"
#include "activities/reader/WeReadCommentOverlay.h"

// 重新拉取本章评论时单独占一屏，拉完再回到阅读页。
class WeReadCommentRefreshActivity final : public Activity {
 public:
  WeReadCommentRefreshActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t { WifiSelection, Running, Succeeded, Failed, LoginRequired, Leave };

  State state_ = State::WifiSelection;
  WeReadCommentOverlay::ManualRefresh outcome_ = WeReadCommentOverlay::ManualRefresh::Idle;
  bool leaving_ = false;
  bool waitConfirmRelease_ = false;
  bool waitBackRelease_ = false;
  std::atomic<bool> fullRefreshPending_{true};

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void armRefresh();
  void leave();
  const char* statusText() const;
};
