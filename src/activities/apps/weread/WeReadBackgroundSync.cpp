#include "WeReadBackgroundSync.h"

#ifdef ENABLE_CHINESE_VERSION

#include <HalClock.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadHttpClient.h>

#include <cstdio>
#include <cstring>
#include <ctime>

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
  bool clockTried = false;
  char bookId[64] = {};
  WeReadClient::ProgressSyncInput input{};
  WeReadProtocol::RemoteProgress remote{};
  uint32_t localEpoch = 0;
  uint16_t localSpine = 0;
  uint16_t localPages = 0;
  unsigned long nextStepMs = 0;
  unsigned long offlineSinceMs = 0;
  bool progressNoted = false;
};

Job job;

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
  job.clockTried = false;
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
  job.clockTried = false;
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
          job.clockTried = false;
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

bool busy() { return job.phase != Phase::Idle || job.choiceHeld; }

void setPaused(const bool paused) { job.paused = paused; }

void release() {
  idle();
  job.operation.reset();
  job.bookId[0] = '\0';
}

BeginResult beginEnter(const char* bookId, const WeReadClient::ProgressSyncInput& input, const uint32_t localEpoch,
                       const uint16_t localSpine, const uint16_t localPages) {
  if (busy()) return BeginResult::Busy;
  if (!ensureOperation()) return BeginResult::Failed;
  rememberBook(bookId);
  job.input = input;
  job.localEpoch = localEpoch;
  job.localSpine = localSpine;
  job.localPages = localPages;
  queue(WeReadClient::ProgressSyncMode::Compare, false);
  LOG_INF("WRSync", "background enter queued");
  return BeginResult::Started;
}

BeginResult beginExit(const char* bookId, const WeReadClient::ProgressSyncInput& input) {
  if (job.phase == Phase::Syncing && job.exitJob && sameBook(bookId)) return BeginResult::Started;
  if (!ensureOperation()) return BeginResult::Failed;
  rememberBook(bookId);
  job.input = input;
  queue(WeReadClient::ProgressSyncMode::UploadLocal, true);
  LOG_INF("WRSync", "background exit queued");
  return BeginResult::Started;
}

BeginResult beginDirection(const WeReadClient::ProgressSyncMode mode) {
  if (job.bookId[0] == '\0' || job.phase == Phase::Syncing) return BeginResult::Busy;
  if (!ensureOperation()) return BeginResult::Failed;
  queue(mode, false);
  LOG_INF("WRSync", "background direction queued: mode=%u", static_cast<unsigned>(mode));
  return BeginResult::Started;
}

bool takeChoice(Choice& out) {
  if (job.phase != Phase::Choice) return false;
  out.remote = job.remote;
  out.localFraction = job.input.localFraction;
  out.localEpoch = job.localEpoch;
  out.remoteEpoch = job.remote.hasUpdateTime ? job.remote.updateTime : 0;
  out.localSpineIndex = job.localSpine;
  out.localPageCount = job.localPages;
  job.phase = Phase::Idle;
  // 选择框还开着时不要让心跳把本地位置先传上去。
  job.choiceHeld = true;
  return true;
}

void dismissChoice() { job.choiceHeld = false; }

bool takeApply(ApplyRequest& out) {
  if (job.phase != Phase::Apply) return false;
  out.remote = job.remote;
  out.localSpineIndex = job.localSpine;
  out.localPageCount = job.localPages;
  job.phase = Phase::Idle;
  noteProgress(false);
  return true;
}

void step() {
  if (job.paused || job.phase != Phase::Syncing || !job.operation) return;
  if (job.nextStepMs != 0 && static_cast<long>(millis() - job.nextStepMs) < 0) return;

  if (!WeReadHttpClient::networkReady()) {
    if (job.offlineSinceMs == 0) job.offlineSinceMs = millis();
    else if (static_cast<long>(millis() - job.offlineSinceMs) > static_cast<long>(kOfflineGiveUpMs)) {
      LOG_INF("WRSync", "background sync skipped: still offline");
      idle();
      return;
    }
    job.nextStepMs = millis() + kOfflineRetryMs;
    return;
  }
  job.offlineSinceMs = 0;

  if (!TimeUtils::isClockValid()) {
    if (job.clockTried) {
      LOG_ERR("WRSync", "background clock sync failed");
      idle();
      return;
    }
    job.clockTried = true;
    if (!halClock.syncNow()) {
      LOG_ERR("WRSync", "background clock sync failed");
      idle();
      return;
    }
  }

  if (!job.operation->active()) {
    if (!job.operation->beginProgressSync(job.bookId, job.input, job.mode)) {
      LOG_ERR("WRSync", "background sync could not start: error=%u", static_cast<unsigned>(job.operation->error()));
      idle();
      return;
    }
    // 开始和第一段网络拆开，避免和打开或离开挤在同一次循环里。
    return;
  }

  const auto event = job.operation->step();
  switch (event) {
    case WeReadClient::Operation::Event::None:
    case WeReadClient::Operation::Event::Authenticated:
    case WeReadClient::Operation::Event::DetailReady:
    case WeReadClient::Operation::Event::ChapterComplete:
    case WeReadClient::Operation::Event::QrReady:
    case WeReadClient::Operation::Event::ChapterRangeReady:
      return;
    case WeReadClient::Operation::Event::Cancelled:
      LOG_INF("WRSync", "background sync cancelled");
      idle();
      return;
    case WeReadClient::Operation::Event::Failed:
      LOG_ERR("WRSync", "background sync failed: error=%u", static_cast<unsigned>(job.operation->error()));
      idle();
      return;
    case WeReadClient::Operation::Event::Complete:
      break;
  }

  const auto result = job.operation->progressSyncResult();
  job.operation->reset();
  finishOutcome(result);
}

}  // namespace WeReadBackgroundSync

#endif
