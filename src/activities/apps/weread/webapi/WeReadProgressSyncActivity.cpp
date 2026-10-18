#include "WeReadProgressSyncActivity.h"

#ifdef ENABLE_CHINESE_VERSION

#include <Epub.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "NetworkStartup.h"
#include "ProgressMapper.h"
#include "ReadingStatsStore.h"
#include "SilentRestart.h"
#include "WeReadXhtmlCodec.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
static_assert(sizeof(WeReadClient::Operation) <= 8 * 1024, "WeRead progress workspace exceeds its fixed heap budget");

#if !CROSSPOINT_EMULATED
// FAT 目录时间是系统时区下的钟面（写入时用 localtime，和设置里的系统时间同一套规则）。
// 年从 1980 起，秒被折半存，这里只取到分钟。再按同一时区折回 UTC，显示时不会多偏一个时区。
uint32_t fatPackedToUnix(const uint32_t packed) {
  if (packed == 0) return 0;
  const uint16_t date = static_cast<uint16_t>(packed >> 16);
  const uint16_t time = static_cast<uint16_t>(packed & 0xFFFF);
  const int year = 1980 + static_cast<int>(date >> 9);
  const unsigned month = (date >> 5) & 0x0Fu;
  const unsigned day = date & 0x1Fu;
  const unsigned hour = time >> 11;
  const unsigned minute = (time >> 5) & 0x3Fu;
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return 0;
  uint32_t epoch = 0;
  if (!TimeUtils::localDateTimeToUtcEpoch(year, month, day, hour, minute, epoch)) return 0;
  return epoch;
}
#endif

bool formatProgressWhen(const uint32_t epoch, char* buffer, const size_t size) {
  if (!buffer || size == 0) return false;
  buffer[0] = '\0';
  std::tm local{};
  if (epoch == 0 || !TimeUtils::isClockValid(epoch) || !TimeUtils::getLocalDateTime(epoch, local)) return false;
  const int length = snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
                              local.tm_mday, local.tm_hour, local.tm_min);
  return length > 0 && static_cast<size_t>(length) < size;
}
}

WeReadProgressSyncActivity::WeReadProgressSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string epubPath, const char* bookId,
                                                       const WeReadProgressContext& context,
                                                       const WeReadSyncOccasion occasion,
                                                       const uint32_t localProgressEpoch, const char* continuePath)
    : Activity(occasion == WeReadSyncOccasion::Duration ? "WeReadDurationReport" : "WeReadProgressSync", renderer,
               mappedInput),
      occasion_(occasion),
      syncMode_(modeFor(occasion)),
      durationOnly_(occasion == WeReadSyncOccasion::Duration),
      epubPath_(std::move(epubPath)),
      localProgressEpoch_(localProgressEpoch) {
  if (bookId) strncpy(bookId_, bookId, sizeof(bookId_) - 1);
  if (continuePath && continuePath[0] != '\0') continuePath_ = continuePath;
  input_.localFraction = context.localFraction;
  input_.localTocIndex = context.localTocIndex;
  input_.localOffset = context.localOffset;
  input_.localOffsetBasis = context.localOffsetBasis;
  localSpineIndex_ = context.localSpineIndex;
  localPageCount_ = context.localPageCount;
}

WeReadProgressContext WeReadProgressSyncActivity::makeContext(const Epub& epub, const char* bookId,
                                                              const float localFraction,
                                                              const CrossPointPosition& localPosition) {
  WeReadProgressContext context;
  context.localFraction = localFraction;
  context.localSpineIndex = static_cast<uint16_t>(localPosition.spineIndex);
  context.localPageNumber = static_cast<uint16_t>(localPosition.pageNumber);
  context.localPageCount = static_cast<uint16_t>(localPosition.totalPages);
  if (!localPosition.hasVisibleTextOffset) return context;

  const std::string bookDir = WeReadStore::bookDirectory(bookId);
  WeReadStore::BookOptions options;
  if (!WeReadStore::loadBookOptions(bookDir, options) ||
      !WeReadStore::parseGeneratedChapterHref(epub.getSpineItem(localPosition.spineIndex).href,
                                              context.localTocIndex)) {
    return context;
  }

  context.localOffset = localPosition.visibleTextOffset;
  context.localOffsetBasis = WeReadClient::LocalOffsetBasis::VisibleText;
  uint32_t nativeOffset = 0;
  if (WeReadXhtmlCodec::visibleToNativeOffset(WeReadStore::chapterPath(bookDir, context.localTocIndex),
                                              context.localOffset, nativeOffset)) {
    context.localOffset = nativeOffset;
    context.localOffsetBasis = WeReadClient::LocalOffsetBasis::RawXhtmlUtf16;
  }
  return context;
}

uint32_t WeReadProgressSyncActivity::readProgressTimestamp(const std::string& cachePath) {
  HalFile file;
  if (!Storage.openFileForRead("WRSync", cachePath + "/progress.bin", file)) return 0;
  const uint32_t stored = file.modificationTime();
#if CROSSPOINT_EMULATED
  // 模拟器的 modificationTime() 已经是 Unix 秒。再按 FAT 打包解码会把 2026 读成 2033。
  return stored;
#else
  return fatPackedToUnix(stored);
#endif
}

void WeReadProgressSyncActivity::onEnter() {
  Activity::onEnter();
  fullRefreshPending_.store(true);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  WeReadStore::Session session;
  const bool loggedIn = WeReadStore::loadSession(session) && session.valid();
  session.clear();
  if (!loggedIn) {
    state_ = State::LoginRequired;
    requestUpdate();
    return;
  }

  // The button sends one live slice, the same size as the in-reader heartbeat.
  // Reading itself reports every interval while Wi-Fi is already up.
  if (durationOnly_) {
    input_.readingSeconds = WeReadClient::Operation::kLiveReadingIntervalSeconds;
    pendingSeconds_ = input_.readingSeconds;
    LOG_INF("WRSync", "manual live reading seconds=%u", static_cast<unsigned>(pendingSeconds_));
  }

  // 进入和退出也会打开 Wi-Fi。已经连着就直接同步，否则仍先选网络。
  NetworkStartup::prepare(renderer);
  wifiActivated_ = true;
  if (WiFi.status() == WL_CONNECTED) {
    LOG_INF("WRSync", "Wi-Fi already connected");
    state_ = State::Starting;
    requestUpdate();
    return;
  }
  launchWifiSelection();
}

void WeReadProgressSyncActivity::onExit() {
  if (workNoted_) {
    BackgroundWork::end(workKind_);
    workNoted_ = false;
  }
  operation_.reset();
  Activity::onExit();
  // This screen opened the radio. Rebooting drops it, unless the control
  // center is holding the association. The reader replace is already queued.
  if (!wifiActivated_ || NetworkStartup::stayConnected()) return;
  WiFi.disconnect(false);
  delay(30);
  if (returnHome_) silentRestart();
  else silentRestartToReader();
}

bool WeReadProgressSyncActivity::preventAutoSleep() { return state_ == State::Starting || state_ == State::Syncing; }

void WeReadProgressSyncActivity::launchWifiSelection() {
  state_ = State::WifiSelection;
  // ActivityManager owns this fixed-size activity until its result returns.
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
  if (!wifi) {
    LOG_ERR("WRSync", "OOM: WifiSelectionActivity (%u bytes)", static_cast<unsigned>(sizeof(WifiSelectionActivity)));
    error_ = WeReadClient::Error::OutOfMemory;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WeReadProgressSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected || WiFi.status() != WL_CONNECTED) {
    leaveSync();
    return;
  }
  fullRefreshPending_.store(true);
  state_ = State::Starting;
  requestUpdate();
}

void WeReadProgressSyncActivity::startSync() {
  requestUpdateAndWait();
  if (!TimeUtils::isClockValid() && !halClock.syncNow()) {
    LOG_ERR("WRSync", "Clock sync failed");
    error_ = WeReadClient::Error::Clock;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (!operation_.beginProgressSync(bookId_, input_, syncMode_)) {
    error_ = operation_.error();
    state_ = error_ == WeReadClient::Error::SessionExpired ? State::LoginRequired : State::Failed;
    requestUpdate();
    return;
  }
  state_ = State::Syncing;
  requestUpdate();
}

void WeReadProgressSyncActivity::advanceSync() {
  // Serialize synchronous TLS work with the renderer task and release glyph
  // caches before the handshake, matching the main WeRead activity.
  RenderLock renderBarrier(*this);
  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
  const auto event = operation_.step();
  switch (event) {
    case WeReadClient::Operation::Event::None:
    case WeReadClient::Operation::Event::Authenticated:
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterComplete:
    case WeReadClient::Operation::Event::QrReady:
      return;
    case WeReadClient::Operation::Event::Cancelled:
      leaveSync();
      return;
    case WeReadClient::Operation::Event::Failed:
      error_ = operation_.error();
      commitReportedTime();
      if (operation_.readingTimeConsumed()) input_.readingSeconds = 0;
      state_ = error_ == WeReadClient::Error::SessionExpired ? State::LoginRequired : State::Failed;
      requestUpdate();
      return;
    case WeReadClient::Operation::Event::Complete:
      break;
  }

  const auto result = operation_.progressSyncResult();
  outcome_ = result.outcome;
  LOG_INF("WRSync", "sync complete: outcome=%u", static_cast<unsigned>(outcome_));
  commitReportedTime();
  if (operation_.readingTimeConsumed()) input_.readingSeconds = 0;
  operation_.reset();
  if (outcome_ == WeReadClient::ProgressSyncOutcome::SelectionRequired) {
    remoteFraction_ = result.remote.percent / 100.0f;
    remoteUpdateTime_ = result.remote.hasUpdateTime ? result.remote.updateTime : 0;
    // 退出时不再询问，直接再传一次当前进度。仍冲突就离开，本地进度保持不变。
    if (occasion_ == WeReadSyncOccasion::Exit) {
      if (!exitUploadRetried_) {
        exitUploadRetried_ = true;
        selectedDirection_ = DirectionOption::UploadLocal;
        beginSelectedDirection();
        return;
      }
      LOG_ERR("WRSync", "exit upload still conflicts; leaving with local progress");
      finishLeaving();
      return;
    }
    uploadConflict_ = syncMode_ == WeReadClient::ProgressSyncMode::UploadLocal;
    selectedDirection_ = DirectionOption::ApplyRemote;
    syncMode_ = WeReadClient::ProgressSyncMode::Compare;
    state_ = State::ChoosingDirection;
    requestUpdate();
    return;
  }
  if (outcome_ == WeReadClient::ProgressSyncOutcome::ApplyRemote && !durationOnly_) {
    applyRemoteProgress(result.remote);
    return;
  }
  showOrLeave();
}

void WeReadProgressSyncActivity::beginSelectedDirection() {
  switch (selectedDirection_) {
    case DirectionOption::ApplyRemote:
      syncMode_ = WeReadClient::ProgressSyncMode::ApplyRemote;
      break;
    case DirectionOption::UploadLocal:
      syncMode_ = WeReadClient::ProgressSyncMode::UploadLocal;
      break;
  }
  state_ = State::Starting;
  requestUpdate();
}

void WeReadProgressSyncActivity::applyRemoteProgress(const WeReadProtocol::RemoteProgress& remoteProgress) {
  // Mapping needs EPUB metadata after TLS has been released. The object is
  // fallible and activity-scoped; a task-stack Epub is too large.
  auto uniqueEpub = makeUniqueNoThrow<Epub>(epubPath_, "/.crosspoint");
  if (!uniqueEpub) {
    LOG_ERR("WRSync", "OOM: Epub (%u bytes)", static_cast<unsigned>(sizeof(Epub)));
    error_ = WeReadClient::Error::OutOfMemory;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  uniqueEpub->setupCacheDir();
  if (!uniqueEpub->load(false, true)) {
    LOG_ERR("WRSync", "Failed to load EPUB metadata");
    error_ = WeReadClient::Error::Integrity;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  // ProgressMapper currently accepts shared_ptr, but ownership stays here. The
  // aliasing view has no control block and therefore performs no allocation.
  const std::shared_ptr<Epub> epubView(std::shared_ptr<Epub>(), uniqueEpub.get());
  SavedProgressPosition fallback{"", std::max(0.0f, std::min(100.0f, remoteProgress.percent)) / 100.0f};
  bool preciseRemote = false;
  uint32_t remoteTocIndex = 0;
  float chapterFraction = 0.0f;
  float remoteBookFraction = 0.0f;
  int exactSpineIndex = -1;
  WeReadStore::BookOptions options;
  const bool generatedBook = WeReadStore::loadBookOptions(WeReadStore::bookDirectory(bookId_), options);
  const bool canonicalRemote = generatedBook && remoteProgress.hasChapterOffset &&
                               WeReadStore::mapChapterToPosition(
                                   WeReadStore::tocPath(bookId_), remoteProgress.chapterUid,
                                   remoteProgress.chapterOffset, remoteTocIndex, chapterFraction, remoteBookFraction);
  if (canonicalRemote) {
    for (int spine = 0; spine < uniqueEpub->getSpineItemsCount(); ++spine) {
      uint32_t tocIndex = 0;
      if (!WeReadStore::parseGeneratedChapterHref(uniqueEpub->getSpineItem(spine).href, tocIndex) ||
          tocIndex != remoteTocIndex) {
        continue;
      }
      exactSpineIndex = spine;
      preciseRemote = true;
      break;
    }
    if (exactSpineIndex < 0) {
      LOG_ERR("WRSync", "Remote chapter %u is not present in the downloaded range",
              static_cast<unsigned>(remoteTocIndex));
      error_ = WeReadClient::Error::Unavailable;
      state_ = State::Failed;
      requestUpdate();
      return;
    }
  }
  LOG_INF("WRSync", "remote progress mapping: precise=%u toc=%u chapter=%s offset=%u fraction=%lu",
          static_cast<unsigned>(preciseRemote), static_cast<unsigned>(remoteTocIndex), remoteProgress.chapterUid,
          static_cast<unsigned>(remoteProgress.chapterOffset),
          static_cast<unsigned long>(
              (preciseRemote ? uniqueEpub->calculateProgress(exactSpineIndex, chapterFraction) : fallback.percentage) *
                  1000000.0f +
              0.5f));
  const CrossPointPosition target =
      preciseRemote ? ProgressMapper::fromSpineProgress(epubView, exactSpineIndex, chapterFraction, renderer,
                                                        localSpineIndex_, localPageCount_)
                    : ProgressMapper::toCrossPoint(epubView, fallback, renderer, localSpineIndex_, localPageCount_);
  if (target.totalPages <= 0) {
    error_ = WeReadClient::Error::Unavailable;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  std::optional<uint32_t> visibleTextOffset;
  uint32_t resolvedVisibleOffset = 0;
  if (preciseRemote && WeReadXhtmlCodec::nativeToVisibleOffset(
                           WeReadStore::chapterPath(WeReadStore::bookDirectory(bookId_), remoteTocIndex),
                           remoteProgress.chapterOffset, resolvedVisibleOffset)) {
    visibleTextOffset = resolvedVisibleOffset;
  }
  if (!EpubReaderUtils::saveProgress(*uniqueEpub, target.spineIndex, target.pageNumber, target.totalPages,
                                     visibleTextOffset)) {
    error_ = WeReadClient::Error::SdCard;
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  uniqueEpub.reset();
  showOrLeave();
}

void WeReadProgressSyncActivity::returnToReader() { activityManager.goToReader(epubPath_); }

void WeReadProgressSyncActivity::leaveSync() {
  if (occasion_ == WeReadSyncOccasion::Exit) finishLeaving();
  else returnToReader();
}

void WeReadProgressSyncActivity::finishLeaving() {
  if (leaving_) return;
  leaving_ = true;
  if (occasion_ == WeReadSyncOccasion::Exit) {
    // 本轮阅读结束。下一本若接着打开，进入同步会重新比较。
    APP_STATE.wereadEnterSyncedPath.clear();
    if (!continuePath_.empty()) APP_STATE.openEpubPath = continuePath_;
    if (!APP_STATE.saveToFile()) LOG_ERR("WRSync", "could not clear enter-sync marker");
    returnHome_ = continuePath_.empty();
    if (returnHome_) activityManager.goHome();
    else activityManager.goToReader(continuePath_);
    return;
  }
  returnToReader();
}

void WeReadProgressSyncActivity::showOrLeave() {
  // 进入时一致、退出时上传完成，都不再多停一屏。手动同步仍显示结果。
  if (occasion_ == WeReadSyncOccasion::Enter || occasion_ == WeReadSyncOccasion::Exit) {
    finishLeaving();
    return;
  }
  state_ = State::Success;
  requestUpdate();
}

void WeReadProgressSyncActivity::commitReportedTime() {
  if (!durationOnly_ || reportedMarked_ || pendingSeconds_ == 0 || !operation_.readingTimeConsumed()) return;
  reportedMarked_ = READING_STATS.markReadingReported(epubPath_, pendingSeconds_);
  if (!reportedMarked_) {
    LOG_ERR("WRSync", "could not store reported reading seconds=%u", static_cast<unsigned>(pendingSeconds_));
  }
}

const char* WeReadProgressSyncActivity::resultMessage() const {
  if (noReadingTime_) return tr(STR_WEREAD_NO_READING_TIME);
  switch (outcome_) {
    case WeReadClient::ProgressSyncOutcome::DurationReported:
      return tr(STR_WEREAD_READING_TIME_REPORTED);
    case WeReadClient::ProgressSyncOutcome::Pending:
    case WeReadClient::ProgressSyncOutcome::AlreadySynced:
      return tr(STR_ALREADY_SYNCED);
    case WeReadClient::ProgressSyncOutcome::SelectionRequired:
      return tr(STR_WEREAD_PROGRESS_CHANGED);
    case WeReadClient::ProgressSyncOutcome::ApplyRemote:
      return tr(STR_WEREAD_REMOTE_PROGRESS_APPLIED);
    case WeReadClient::ProgressSyncOutcome::LocalUploaded:
      return tr(STR_WEREAD_LOCAL_PROGRESS_UPLOADED);
  }
  return tr(STR_ALREADY_SYNCED);
}

const char* WeReadProgressSyncActivity::errorMessage() const {
  switch (error_) {
    case WeReadClient::Error::Unavailable:
      return tr(STR_WEREAD_PROGRESS_UNAVAILABLE);
    case WeReadClient::Error::Network:
      return tr(STR_WEREAD_HTTP_ERROR);
    case WeReadClient::Error::SdCard:
      return tr(STR_SAVE_PROGRESS_FAILED);
    case WeReadClient::Error::Clock:
      return tr(STR_CLOCK_SYNC_FAIL);
    case WeReadClient::Error::SessionExpired:
      return tr(STR_WEREAD_LOGIN_REQUIRED);
    case WeReadClient::Error::Ok:
    case WeReadClient::Error::Cancelled:
    case WeReadClient::Error::LoginFailed:
    case WeReadClient::Error::Protocol:
    case WeReadClient::Error::Integrity:
    case WeReadClient::Error::OutOfMemory:
      return tr(STR_SYNC_FAILED_MSG);
  }
  return tr(STR_SYNC_FAILED_MSG);
}

void WeReadProgressSyncActivity::loop() {
  const bool working = state_ == State::Starting || state_ == State::Syncing;
  if (working != workNoted_) {
    if (workNoted_) BackgroundWork::end(workKind_);
    workKind_ = durationOnly_ ? BackgroundWork::Kind::SyncReadingTime : BackgroundWork::Kind::SyncProgress;
    if (working) BackgroundWork::begin(workKind_);
    workNoted_ = working;
  }
  switch (state_) {
    case State::WifiSelection:
      return;
    case State::Starting:
      startSync();
      return;
    case State::Syncing:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        operation_.cancel();
      }
      advanceSync();
      return;
    case State::ChoosingDirection: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        leaveSync();
        return;
      }
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const Rect content = SubpageLayout::contentRect(screen, metrics);
      const int optionStep = metrics.menuRowHeight + metrics.menuSpacing;
      const int optionTop = content.y + content.height - optionStep * 2;
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, optionTop, optionStep, 2, content.x,
                                              content.x + content.width, metrics.menuRowHeight);
      if (touch != MappedInputManager::RowTouch::None) {
        selectedDirection_ = touchedOption == 0 ? DirectionOption::ApplyRemote : DirectionOption::UploadLocal;
        if (touch == MappedInputManager::RowTouch::Tap) {
          beginSelectedDirection();
        } else {
          requestUpdate();
        }
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
          mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
        selectedDirection_ = selectedDirection_ == DirectionOption::ApplyRemote ? DirectionOption::UploadLocal
                                                                                : DirectionOption::ApplyRemote;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        beginSelectedDirection();
      }
      return;
    }
    case State::Success:
    case State::LoginRequired: {
      int x = 0;
      int y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        leaveSync();
      }
      return;
    }
    case State::Failed:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        leaveSync();
        return;
      }
      {
        int x = 0;
        int y = 0;
        if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) &&
            (error_ == WeReadClient::Error::Network || error_ == WeReadClient::Error::Clock ||
             error_ == WeReadClient::Error::Unavailable)) {
          state_ = State::Starting;
          requestUpdate();
        }
        return;
      }
  }
}

void WeReadProgressSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const char* title = durationOnly_ ? tr(STR_WEREAD_REPORT_READING_TIME) : tr(STR_WEREAD_SYNC_PROGRESS);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight}, title);
  const Rect content = SubpageLayout::contentRect(screen, metrics);
  const Rect textBounds = SubpageLayout::insetHorizontal(content, metrics.contentSidePadding);
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);

  switch (state_) {
    case State::WifiSelection:
    case State::Starting:
    case State::Syncing: {
      const char* status = durationOnly_ ? tr(STR_WEREAD_REPORTING_READING_TIME) : tr(STR_WEREAD_SYNCING_PROGRESS);
      UITheme::drawCenteredText(renderer, textBounds, UI_12_FONT_ID, SubpageLayout::centeredTop(content, titleHeight),
                                status, true, EpdFontFamily::BOLD);
      break;
    }
    case State::ChoosingDirection: {
      const int contentTop = content.y + SubpageLayout::sectionGap(metrics);
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const char* comparisonTitle =
          uploadConflict_ ? tr(STR_WEREAD_PROGRESS_CHANGED) : tr(STR_WEREAD_PROGRESS_DIFFERS);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, contentTop, comparisonTitle, true,
                                EpdFontFamily::BOLD);

      char remoteWhen[24];
      char localWhen[24];
      const char* remoteWhenText = formatProgressWhen(remoteUpdateTime_, remoteWhen, sizeof(remoteWhen))
                                      ? remoteWhen
                                      : tr(STR_PROGRESS_TIME_UNKNOWN);
      const char* localWhenText = formatProgressWhen(localProgressEpoch_, localWhen, sizeof(localWhen))
                                     ? localWhen
                                     : tr(STR_PROGRESS_TIME_UNKNOWN);
      char remoteValue[24];
      char localValue[24];
      snprintf(remoteValue, sizeof(remoteValue), "%.2f%%", remoteFraction_ * 100.0f);
      snprintf(localValue, sizeof(localValue), "%.2f%%", input_.localFraction * 100.0f);
      const int remoteLabelY = contentTop + lineHeight + SubpageLayout::sectionGap(metrics);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, remoteLabelY, tr(STR_REMOTE_LABEL), true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, remoteLabelY + lineHeight, remoteWhenText);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, remoteLabelY + lineHeight * 2, remoteValue);
      const int localLabelY = remoteLabelY + lineHeight * 3 + SubpageLayout::sectionGap(metrics);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, localLabelY, tr(STR_LOCAL_LABEL), true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, localLabelY + lineHeight, localWhenText);
      UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, localLabelY + lineHeight * 2, localValue);

      const int optionStep = metrics.menuRowHeight + metrics.menuSpacing;
      const int optionTop = content.y + content.height - optionStep * 2;
      const int optionTextOffset = (metrics.menuRowHeight - lineHeight) / 2;
      const DirectionOption options[] = {DirectionOption::ApplyRemote, DirectionOption::UploadLocal};
      for (int index = 0; index < 2; ++index) {
        const int optionY = optionTop + optionStep * index;
        const bool selected = selectedDirection_ == options[index];
        if (selected) {
          renderer.fillRect(textBounds.x, optionY, textBounds.width, metrics.menuRowHeight);
        }
        const char* label =
            options[index] == DirectionOption::ApplyRemote ? tr(STR_APPLY_REMOTE) : tr(STR_UPLOAD_LOCAL);
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, optionY + optionTextOffset, label, !selected);
      }
      break;
    }
    case State::Success: {
      const bool showSpan = durationOnly_ && beforeLabel_[0] && afterLabel_[0];
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int blockHeight = showSpan ? titleHeight + lineHeight * 2 + 16 : titleHeight;
      int top = SubpageLayout::centeredTop(content, blockHeight);
      UITheme::drawCenteredText(renderer, textBounds, UI_12_FONT_ID, top, resultMessage(), true, EpdFontFamily::BOLD);
      if (showSpan) {
        top += titleHeight + 8;
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, top, beforeLabel_);
        UITheme::drawCenteredText(renderer, textBounds, UI_10_FONT_ID, top + lineHeight, afterLabel_);
      }
      break;
    }
    case State::LoginRequired:
      UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, tr(STR_WEREAD_LOGIN_REQUIRED), 2, true,
                                       EpdFontFamily::BOLD);
      break;
    case State::Failed:
      UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, errorMessage(), 2, true,
                                       EpdFontFamily::BOLD);
      break;
  }

  if (state_ == State::ChoosingDirection) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Success || state_ == State::LoginRequired || state_ == State::Failed) {
    const bool retryable =
        state_ == State::Failed && (error_ == WeReadClient::Error::Network || error_ == WeReadClient::Error::Clock ||
                                    error_ == WeReadClient::Error::Unavailable);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", retryable ? tr(STR_RETRY) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  // Consume this here so a Wi-Fi child cannot use up the sync page's clean refresh.
  renderer.displayBuffer(fullRefreshPending_.exchange(false) ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}

#endif
