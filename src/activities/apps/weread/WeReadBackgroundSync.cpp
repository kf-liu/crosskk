#include "WeReadBackgroundSync.h"

#ifdef ENABLE_CHINESE_VERSION

#include <HalClock.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadHttpClient.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "util/BackgroundWork.h"
#include "util/TimeUtils.h"

namespace WeReadBackgroundSync {
namespace {

static_assert(sizeof(WeReadClient::Operation) <= 8 * 1024, "WeRead progress workspace exceeds its fixed heap budget");

constexpr unsigned long kOfflineGiveUpMs = 60UL * 1000UL;
constexpr unsigned long kOfflineRetryMs = 1000UL;

enum class Phase : uint8_t { Idle, Syncing, Choice, Apply };

struct Job {
  std::unique_ptr<WeReadClient::Operation> operation;
  Phase phase = Phase::Idle;
  WeReadClient::ProgressSyncMode mode = WeReadClient::ProgressSyncMode::Compare;
  bool paused = false;
  bool exitJob = false;
  bool exitRetried = false;
  bool choiceHeld = false;
  char bookId[64] = {};
  WeReadClient::ProgressSyncInput input{};
  WeReadProtocol::RemoteProgress remote{};
  uint32_t localEpoch = 0;
  uint16_t localSpine = 0;
  uint16_t localPages = 0;
  unsigned long nextStepMs = 0;
  unsigned long offlineSinceMs = 0;
  unsigned long clockSinceMs = 0;
  bool progressNoted = false;
};

enum class PendingKind : uint8_t { None, Enter, Exit, Direction, Release };

struct PendingCmd {
  PendingKind kind = PendingKind::None;
  WeReadClient::ProgressSyncMode mode = WeReadClient::ProgressSyncMode::Compare;
  WeReadClient::ProgressSyncInput input{};
  uint32_t localEpoch = 0;
  uint16_t localSpine = 0;
  uint16_t localPages = 0;
  char bookId[64] = {};
};

enum class StepPace : uint8_t { Idle, Wait, Again };

Job job;
PendingCmd pending;
std::atomic<bool> pauseRequested{false};
std::atomic<bool> inFlight{false};
std::atomic<bool> clockNeeded{false};
SemaphoreHandle_t jobMu = nullptr;
TaskHandle_t worker = nullptr;
std::atomic<IdleHook> idleHook{nullptr};
bool clockRequestArmed = false;

void rememberBook(const char* bookId) {
  job.bookId[0] = '\0';
  if (!bookId) return;
  strncpy(job.bookId, bookId, sizeof(job.bookId) - 1);
  job.bookId[sizeof(job.bookId) - 1] = '\0';
}

bool sameBook(const char* bookId) { return bookId && job.bookId[0] != '\0' && strcmp(job.bookId, bookId) == 0; }

void noteProgress(const bool on) {
  if (on == job.progressNoted) return;
  if (on) BackgroundWork::begin(BackgroundWork::Kind::SyncProgress);
  else BackgroundWork::end(BackgroundWork::Kind::SyncProgress);
  job.progressNoted = on;
}

void idle() {
  noteProgress(false);
  if (job.operation && job.operation->active()) job.operation->reset();
  job.phase = Phase::Idle;
  job.exitJob = false;
  job.exitRetried = false;
  job.choiceHeld = false;
  job.clockSinceMs = 0;
  job.nextStepMs = 0;
  job.offlineSinceMs = 0;
}

bool ensureOperation() {
  if (job.operation) return true;
  job.operation = makeUniqueNoThrow<WeReadClient::Operation>();
  if (!job.operation) {
    LOG_ERR("WRSync", "OOM: background progress (%u bytes)", static_cast<unsigned>(sizeof(WeReadClient::Operation)));
    return false;
  }
  return true;
}

void queue(const WeReadClient::ProgressSyncMode mode, const bool exitJob) {
  job.mode = mode;
  job.exitJob = exitJob;
  job.exitRetried = false;
  job.choiceHeld = false;
  job.clockSinceMs = 0;
  job.nextStepMs = 0;
  job.offlineSinceMs = 0;
  job.phase = Phase::Syncing;
  if (job.operation && job.operation->active()) job.operation->reset();
  noteProgress(true);
}

void parkChoice(const WeReadProtocol::RemoteProgress& remote) {
  noteProgress(false);
  job.remote = remote;
  job.phase = Phase::Choice;
  LOG_INF("WRSync", "background enter differs: remote=%.2f local=%.2f", static_cast<double>(remote.percent),
          static_cast<double>(job.input.localFraction * 100.0f));
}

void parkApply(const WeReadProtocol::RemoteProgress& remote) {
  job.remote = remote;
  job.phase = Phase::Apply;
}

void finishOutcome(const WeReadClient::ProgressSyncResult& result) {
  switch (result.outcome) {
    case WeReadClient::ProgressSyncOutcome::Pending:
      LOG_ERR("WRSync", "background sync finished without an outcome");
      idle();
      return;
    case WeReadClient::ProgressSyncOutcome::AlreadySynced:
    case WeReadClient::ProgressSyncOutcome::LocalUploaded:
    case WeReadClient::ProgressSyncOutcome::DurationReported:
      LOG_INF("WRSync", "background sync done: outcome=%u", static_cast<unsigned>(result.outcome));
      idle();
      return;
    case WeReadClient::ProgressSyncOutcome::SelectionRequired:
      if (job.exitJob) {
        if (!job.exitRetried) {
          job.exitRetried = true;
          job.mode = WeReadClient::ProgressSyncMode::UploadLocal;
          job.phase = Phase::Syncing;
          job.clockSinceMs = 0;
          job.nextStepMs = 0;
          LOG_INF("WRSync", "exit upload conflicted; retrying once");
          return;
        }
        LOG_ERR("WRSync", "exit upload still conflicts; keeping local progress");
        idle();
        return;
      }
      parkChoice(result.remote);
      return;
    case WeReadClient::ProgressSyncOutcome::ApplyRemote:
      if (job.exitJob) {
        LOG_INF("WRSync", "background exit finished");
        idle();
        return;
      }
      parkApply(result.remote);
      return;
  }
}

bool lockJob() {
  if (!jobMu) jobMu = xSemaphoreCreateMutex();
  if (!jobMu) {
    LOG_ERR("WRSync", "进度同步锁创建失败");
    return false;
  }
  xSemaphoreTake(jobMu, portMAX_DELAY);
  return true;
}

void unlockJob() {
  if (jobMu) xSemaphoreGive(jobMu);
}

void copyPendingBook(const char* bookId) {
  pending.bookId[0] = '\0';
  if (!bookId) return;
  strncpy(pending.bookId, bookId, sizeof(pending.bookId) - 1);
  pending.bookId[sizeof(pending.bookId) - 1] = '\0';
}

bool syncBusyLocked() {
  return inFlight.load() || job.phase != Phase::Idle || job.choiceHeld || pending.kind != PendingKind::None;
}

void applyPendingLocked() {
  if (inFlight.load()) return;
  switch (pending.kind) {
    case PendingKind::None:
      return;
    case PendingKind::Release:
      idle();
      job.operation.reset();
      job.bookId[0] = '\0';
      pending.kind = PendingKind::None;
      return;
    case PendingKind::Enter:
      rememberBook(pending.bookId);
      job.input = pending.input;
      job.localEpoch = pending.localEpoch;
      job.localSpine = pending.localSpine;
      job.localPages = pending.localPages;
      queue(WeReadClient::ProgressSyncMode::Compare, false);
      pending.kind = PendingKind::None;
      LOG_INF("WRSync", "background enter queued");
      return;
    case PendingKind::Exit:
      if (job.phase == Phase::Syncing && job.exitJob && sameBook(pending.bookId)) {
        pending.kind = PendingKind::None;
        return;
      }
      rememberBook(pending.bookId);
      job.input = pending.input;
      queue(WeReadClient::ProgressSyncMode::UploadLocal, true);
      pending.kind = PendingKind::None;
      LOG_INF("WRSync", "background exit queued");
      return;
    case PendingKind::Direction:
      if (job.bookId[0] == '\0') {
        pending.kind = PendingKind::None;
        return;
      }
      queue(pending.mode, false);
      pending.kind = PendingKind::None;
      LOG_INF("WRSync", "background direction queued: mode=%u", static_cast<unsigned>(pending.mode));
      return;
  }
}

StepPace stepWork() {
  if (!lockJob()) return StepPace::Idle;
  job.paused = pauseRequested.load();
  applyPendingLocked();
  if (inFlight.load()) {
    unlockJob();
    return StepPace::Wait;
  }
  if (job.paused || job.phase != Phase::Syncing || !job.operation) {
    const StepPace pace = job.phase == Phase::Syncing ? StepPace::Wait : StepPace::Idle;
    unlockJob();
    return pace;
  }
  if (job.nextStepMs != 0 && static_cast<long>(millis() - job.nextStepMs) < 0) {
    unlockJob();
    return StepPace::Wait;
  }

  if (!WeReadHttpClient::networkReady()) {
    if (job.offlineSinceMs == 0) job.offlineSinceMs = millis();
    else if (static_cast<long>(millis() - job.offlineSinceMs) > static_cast<long>(kOfflineGiveUpMs)) {
      LOG_INF("WRSync", "background sync skipped: still offline");
      idle();
      unlockJob();
      return StepPace::Idle;
    }
    job.nextStepMs = millis() + kOfflineRetryMs;
    unlockJob();
    return StepPace::Wait;
  }
  job.offlineSinceMs = 0;

  if (!TimeUtils::isClockValid()) {
    if (job.clockSinceMs == 0) job.clockSinceMs = millis();
    if (static_cast<long>(millis() - job.clockSinceMs) > 10000) {
      LOG_ERR("WRSync", "后台对时失败");
      clockNeeded.store(false);
      idle();
      unlockJob();
      return StepPace::Idle;
    }
    // 对时留给主循环。这里不调用 syncNow，避免和 HalClock::update 抢 SNTP。
    clockNeeded.store(true);
    job.nextStepMs = millis() + 250;
    unlockJob();
    return StepPace::Wait;
  }
  job.clockSinceMs = 0;
  clockNeeded.store(false);

  if (!job.operation->active()) {
    if (!job.operation->beginProgressSync(job.bookId, job.input, job.mode)) {
      LOG_ERR("WRSync", "background sync could not start: error=%u", static_cast<unsigned>(job.operation->error()));
      idle();
      unlockJob();
      return StepPace::Idle;
    }
    unlockJob();
    return StepPace::Again;
  }

  inFlight.store(true);
  unlockJob();
  const auto event = job.operation->step();
  if (!lockJob()) {
    inFlight.store(false);
    return StepPace::Idle;
  }
  inFlight.store(false);
  if (pending.kind != PendingKind::None) {
    applyPendingLocked();
    unlockJob();
    return StepPace::Again;
  }

  StepPace pace = StepPace::Idle;
  switch (event) {
    case WeReadClient::Operation::Event::None:
      pace = StepPace::Wait;
      break;
    case WeReadClient::Operation::Event::Authenticated:
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterComplete:
    case WeReadClient::Operation::Event::QrReady:
    case WeReadClient::Operation::Event::ChapterRangeReady:
      pace = StepPace::Again;
      break;
    case WeReadClient::Operation::Event::Cancelled:
      LOG_INF("WRSync", "background sync cancelled");
      idle();
      pace = StepPace::Idle;
      break;
    case WeReadClient::Operation::Event::Failed:
      LOG_ERR("WRSync", "background sync failed: error=%u", static_cast<unsigned>(job.operation->error()));
      idle();
      pace = StepPace::Idle;
      break;
    case WeReadClient::Operation::Event::Complete: {
      const auto result = job.operation->progressSyncResult();
      job.operation->reset();
      finishOutcome(result);
      pace = job.phase == Phase::Syncing ? StepPace::Again : StepPace::Idle;
      break;
    }
  }
  unlockJob();
  return pace;
}

void workerMain(void*) {
  for (;;) {
    const StepPace pace = stepWork();
    bool more = false;
    if (pace != StepPace::Again) {
      if (const IdleHook hook = idleHook.load()) more = hook();
    }
    if (pace == StepPace::Again || more) continue;
    const uint32_t waitMs = pace == StepPace::Wait ? 200 : 1000;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
  }
}

void kickWorker() {
  if (worker) xTaskNotify(worker, 0, eIncrement);
}

}  // namespace

bool formatWhen(const uint32_t epoch, char* buffer, const size_t size) {
  if (!buffer || size == 0) return false;
  buffer[0] = '\0';
  std::tm local{};
  if (epoch == 0 || !TimeUtils::isClockValid(epoch) || !TimeUtils::getLocalDateTime(epoch, local)) return false;
  const int length = snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
                              local.tm_mday, local.tm_hour, local.tm_min);
  return length > 0 && static_cast<size_t>(length) < size;
}

bool busy() {
  if (inFlight.load()) return true;
  if (!lockJob()) return true;
  const bool held = syncBusyLocked();
  unlockJob();
  return held;
}

void setPaused(const bool paused) { pauseRequested.store(paused); }

bool isPaused() { return pauseRequested.load(); }

void setIdleHook(const IdleHook hook) { idleHook.store(hook); }

void release() {
  if (!lockJob()) return;
  pending.kind = PendingKind::Release;
  const bool hasWorker = worker != nullptr;
  if (!hasWorker) applyPendingLocked();
  unlockJob();
  if (!hasWorker) return;
  kickWorker();
  // 手动同步要独占这条 TLS。只等到当前这一拍 HTTPS 结束，不在阅读循环里主动调用。
  const unsigned long start = millis();
  while (static_cast<long>(millis() - start) < 25000) {
    if (!lockJob()) return;
    const bool done =
        !inFlight.load() && pending.kind == PendingKind::None && job.phase == Phase::Idle && !job.operation;
    unlockJob();
    if (done) return;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  LOG_ERR("WRSync", "手动同步等待后台请求结束超时");
}

BeginResult beginEnter(const char* bookId, const WeReadClient::ProgressSyncInput& input, const uint32_t localEpoch,
                       const uint16_t localSpine, const uint16_t localPages) {
  if (!lockJob()) return BeginResult::Failed;
  if (syncBusyLocked()) {
    unlockJob();
    return BeginResult::Busy;
  }
  if (!ensureOperation()) {
    unlockJob();
    return BeginResult::Failed;
  }
  pending.kind = PendingKind::Enter;
  pending.input = input;
  pending.localEpoch = localEpoch;
  pending.localSpine = localSpine;
  pending.localPages = localPages;
  copyPendingBook(bookId);
  unlockJob();
  kickWorker();
  return BeginResult::Started;
}

BeginResult beginExit(const char* bookId, const WeReadClient::ProgressSyncInput& input) {
  if (!lockJob()) return BeginResult::Failed;
  if (!ensureOperation()) {
    unlockJob();
    return BeginResult::Failed;
  }
  const bool sameExit = (job.phase == Phase::Syncing && job.exitJob && sameBook(bookId)) ||
                        (pending.kind == PendingKind::Exit && bookId && strcmp(pending.bookId, bookId) == 0);
  if (sameExit) {
    unlockJob();
    return BeginResult::Started;
  }
  pending.kind = PendingKind::Exit;
  pending.input = input;
  copyPendingBook(bookId);
  unlockJob();
  kickWorker();
  return BeginResult::Started;
}

BeginResult beginDirection(const WeReadClient::ProgressSyncMode mode) {
  if (!lockJob()) return BeginResult::Failed;
  if (job.bookId[0] == '\0' || job.phase == Phase::Syncing || inFlight.load() || pending.kind != PendingKind::None) {
    unlockJob();
    return BeginResult::Busy;
  }
  if (!ensureOperation()) {
    unlockJob();
    return BeginResult::Failed;
  }
  pending.kind = PendingKind::Direction;
  pending.mode = mode;
  unlockJob();
  kickWorker();
  return BeginResult::Started;
}

bool takeChoice(Choice& out) {
  if (!lockJob()) return false;
  if (job.phase != Phase::Choice) {
    unlockJob();
    return false;
  }
  out.remote = job.remote;
  out.localFraction = job.input.localFraction;
  out.localEpoch = job.localEpoch;
  out.remoteEpoch = job.remote.hasUpdateTime ? job.remote.updateTime : 0;
  out.localSpineIndex = job.localSpine;
  out.localPageCount = job.localPages;
  job.phase = Phase::Idle;
  // 选择框还开着时不要让心跳把本地位置先传上去。
  job.choiceHeld = true;
  unlockJob();
  return true;
}

void dismissChoice() {
  if (!lockJob()) return;
  job.choiceHeld = false;
  unlockJob();
}

bool takeApply(ApplyRequest& out) {
  if (!lockJob()) return false;
  if (job.phase != Phase::Apply) {
    unlockJob();
    return false;
  }
  out.remote = job.remote;
  out.localSpineIndex = job.localSpine;
  out.localPageCount = job.localPages;
  job.phase = Phase::Idle;
  noteProgress(false);
  unlockJob();
  return true;
}

bool ensureWorker() {
#if defined(SIMULATOR)
  return false;
#else
  if (worker) return true;
  // 8192 与评论任务相同：一拍 HTTPS 的缓冲在堆上，这条栈只跑请求本身。阅读循环不再调用。
  if (xTaskCreate(workerMain, "wrBeat", 8192, nullptr, 1, &worker) != pdPASS) {
    LOG_ERR("WRSync", "阅读后台任务创建失败，栈 8192 字节");
    worker = nullptr;
    return false;
  }
  return true;
#endif
}

void serviceOnMain() {
  if (clockNeeded.load()) {
    if (TimeUtils::isClockValid()) {
      clockNeeded.store(false);
      clockRequestArmed = false;
    } else if (!clockRequestArmed) {
      clockRequestArmed = halClock.requestSync();
    }
  } else {
    clockRequestArmed = false;
  }
#if defined(SIMULATOR)
  if (clockNeeded.load() || busy()) stepWork();
#else
  // 没有进度同步、对时或已经建好的任务时，不占那 8KB 栈。
  if (!worker && !clockNeeded.load() && !busy()) return;
  if (!ensureWorker()) {
    stepWork();
    return;
  }
  kickWorker();
#endif
}

}  // namespace WeReadBackgroundSync

#endif
