#include "WeReadCommentRefreshActivity.h"

#ifdef ENABLE_CHINESE_VERSION

#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "NetworkStartup.h"
#include "ReaderUtils.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

WeReadCommentRefreshActivity::WeReadCommentRefreshActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("WeReadCommentRefresh", renderer, mappedInput) {}

void WeReadCommentRefreshActivity::onEnter() {
  Activity::onEnter();
  fullRefreshPending_.store(true);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  waitConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);

  NetworkStartup::prepare(renderer);
  if (WiFi.status() == WL_CONNECTED) {
    armRefresh();
    return;
  }
  launchWifiSelection();
}

void WeReadCommentRefreshActivity::onExit() {
  if (WeReadCommentOverlay::manualRefreshState() == WeReadCommentOverlay::ManualRefresh::Running) {
    WeReadCommentOverlay::cancelManualRefresh();
  }
  Activity::onExit();
}

bool WeReadCommentRefreshActivity::preventAutoSleep() { return state_ == State::Running; }

void WeReadCommentRefreshActivity::launchWifiSelection() {
  state_ = State::WifiSelection;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    LOG_ERR("WR", "内存不足：选网页面 %u 字节", static_cast<unsigned>(sizeof(WifiSelectionActivity)));
    outcome_ = WeReadCommentOverlay::ManualRefresh::Failed;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WeReadCommentRefreshActivity::onWifiSelectionComplete(const bool connected) {
  waitConfirmRelease_ = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
  if (!connected || WiFi.status() != WL_CONNECTED) {
    leave();
    return;
  }
  fullRefreshPending_.store(true);
  armRefresh();
}

void WeReadCommentRefreshActivity::armRefresh() {
  outcome_ = WeReadCommentOverlay::beginManualRefresh();
  switch (outcome_) {
    case WeReadCommentOverlay::ManualRefresh::Running:
      state_ = State::Running;
      break;
    case WeReadCommentOverlay::ManualRefresh::LoginRequired:
      state_ = State::LoginRequired;
      break;
    case WeReadCommentOverlay::ManualRefresh::Offline:
      state_ = State::Failed;
      break;
    case WeReadCommentOverlay::ManualRefresh::Idle:
      state_ = State::Leave;
      return;
    case WeReadCommentOverlay::ManualRefresh::Succeeded:
    case WeReadCommentOverlay::ManualRefresh::Failed:
      state_ = State::Failed;
      outcome_ = WeReadCommentOverlay::ManualRefresh::Failed;
      break;
  }
  requestUpdate();
}

void WeReadCommentRefreshActivity::leave() {
  if (leaving_) return;
  leaving_ = true;
  finish();
}

const char* WeReadCommentRefreshActivity::statusText() const {
  switch (outcome_) {
    case WeReadCommentOverlay::ManualRefresh::Succeeded:
      return tr(STR_WEREAD_COMMENTS_REFRESHED);
    case WeReadCommentOverlay::ManualRefresh::LoginRequired:
      return tr(STR_WEREAD_LOGIN_REQUIRED);
    case WeReadCommentOverlay::ManualRefresh::Offline:
      return tr(STR_WEREAD_HTTP_ERROR);
    case WeReadCommentOverlay::ManualRefresh::Failed:
      return tr(STR_WEREAD_COMMENTS_REFRESH_FAILED);
    case WeReadCommentOverlay::ManualRefresh::Idle:
    case WeReadCommentOverlay::ManualRefresh::Running:
      return tr(STR_WEREAD_REFRESHING_COMMENTS);
  }
  return tr(STR_WEREAD_REFRESHING_COMMENTS);
}

void WeReadCommentRefreshActivity::loop() {
  if (waitConfirmRelease_ || waitBackRelease_) {
    if (waitConfirmRelease_ && !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      waitConfirmRelease_ = false;
    }
    if (waitBackRelease_ && !mappedInput.isPressed(MappedInputManager::Button::Back)) waitBackRelease_ = false;
    if (waitConfirmRelease_ || waitBackRelease_) return;
    return;
  }

  switch (state_) {
    case State::WifiSelection:
      return;
    case State::Leave:
      leave();
      return;
    case State::Running: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        WeReadCommentOverlay::cancelManualRefresh();
        leave();
        return;
      }
      const auto now = WeReadCommentOverlay::manualRefreshState();
      if (now == WeReadCommentOverlay::ManualRefresh::Running) return;
      outcome_ = now;
      switch (now) {
        case WeReadCommentOverlay::ManualRefresh::Succeeded:
          state_ = State::Succeeded;
          break;
        case WeReadCommentOverlay::ManualRefresh::LoginRequired:
          state_ = State::LoginRequired;
          break;
        case WeReadCommentOverlay::ManualRefresh::Idle:
          leave();
          return;
        case WeReadCommentOverlay::ManualRefresh::Running:
        case WeReadCommentOverlay::ManualRefresh::Failed:
        case WeReadCommentOverlay::ManualRefresh::Offline:
          state_ = State::Failed;
          break;
      }
      requestUpdate();
      return;
    }
    case State::Succeeded:
    case State::LoginRequired: {
      int x = 0;
      int y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        leave();
      }
      return;
    }
    case State::Failed: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        leave();
        return;
      }
      int x = 0;
      int y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        if (WiFi.status() != WL_CONNECTED) launchWifiSelection();
        else armRefresh();
      }
      return;
    }
  }
}

void WeReadCommentRefreshActivity::render(RenderLock&&) {
  if (state_ == State::WifiSelection || state_ == State::Leave) return;
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WEREAD_REFRESH_COMMENTS));
  const Rect content = SubpageLayout::contentRect(screen, metrics);
  const Rect textBounds = SubpageLayout::insetHorizontal(content, metrics.contentSidePadding);
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const bool waiting = state_ == State::Running;
  if (waiting) {
    UITheme::drawCenteredText(renderer, textBounds, UI_12_FONT_ID, SubpageLayout::centeredTop(content, titleHeight),
                              statusText(), true, EpdFontFamily::BOLD);
  } else {
    UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, statusText(), 3, true, EpdFontFamily::BOLD);
  }

  if (!waiting) {
    const bool retryable = state_ == State::Failed;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", retryable ? tr(STR_RETRY) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(fullRefreshPending_.exchange(false) ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}

#endif
