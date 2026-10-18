#include "NetworkStartup.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"

#if !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#endif

#include "BleInput.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
#include "network/SavedStationConnect.h"

namespace {

constexpr size_t MIN_FREE_PSRAM = 256 * 1024;
constexpr size_t MIN_FREE_INTERNAL = 64 * 1024;
constexpr size_t MIN_LARGEST_INTERNAL_BLOCK = 32 * 1024;

struct MemorySnapshot {
  size_t freePsram;
  size_t freeInternal;
  size_t largestInternalBlock;
};

constexpr bool shouldReleaseRenderMemory(const MemorySnapshot& memory) {
  return memory.freePsram < MIN_FREE_PSRAM || memory.freeInternal < MIN_FREE_INTERNAL ||
         memory.largestInternalBlock < MIN_LARGEST_INTERNAL_BLOCK;
}

static_assert(shouldReleaseRenderMemory({0, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM - 1, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL - 1, MIN_LARGEST_INTERNAL_BLOCK}));
static_assert(shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK - 1}));
static_assert(!shouldReleaseRenderMemory({MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK}));

MemorySnapshot readMemorySnapshot() {
#if defined(SIMULATOR)
  return {MIN_FREE_PSRAM, MIN_FREE_INTERNAL, MIN_LARGEST_INTERNAL_BLOCK};
#elif defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_USE_MALLOC
  constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(INTERNAL_CAPS),
          heap_caps_get_largest_free_block(INTERNAL_CAPS)};
#else
  return {};
#endif
}

}  // namespace

namespace {
// Last time a stay-connected join was started. Keeps the per-loop service
// from calling begin() again while that join is still in progress.
unsigned long stayConnectAttemptAtMs = 0;
}  // namespace

namespace NetworkStartup {

void logMemory(const char* stage) {
  const auto internal = HalMemory::getInternalHeap();
#if defined(SIMULATOR)
  const HalMemory::HeapStats dma{};  // No hardware DMA heap in the desktop simulator.
#else
  const auto dma = HalMemory::getInternalDmaHeap();
#endif
  const auto psram = HalMemory::getPsramHeap();
  LOG_INF("NET", "%s: internal free=%u largest=%u, DMA free=%u largest=%u, PSRAM free=%u", stage,
          static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.largestBlockBytes),
          static_cast<unsigned>(dma.freeBytes), static_cast<unsigned>(dma.largestBlockBytes),
          static_cast<unsigned>(psram.freeBytes));
}

void prepare(GfxRenderer& renderer) {
  logMemory("before font reclaim");
  const MemorySnapshot before = readMemorySnapshot();
  if (!shouldReleaseRenderMemory(before)) return;

  {
    RenderLock lock;
    sdFontSystem.releaseLoadedFont(renderer);
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
  }
  logMemory("after font reclaim");
}

bool setMode(GfxRenderer& renderer, const wifi_mode_t mode) {
  bleinput::stop();
  logMemory("after BLE stop");
  prepare(renderer);
  const bool started = WiFi.mode(mode);
  logMemory(started ? "WiFi mode ready" : "WiFi mode failed");
  return started;
}

namespace {
bool readingHold = false;

enum class ReadingRadioPhase : uint8_t { Idle, Joining, Scanning };

constexpr unsigned long kReadingJoinRetryMs = 60000;
constexpr unsigned long kReadingScanTimeoutMs = 15000;

ReadingRadioPhase readingPhase = ReadingRadioPhase::Idle;
unsigned long readingPhaseStartedMs = 0;
bool readingScanDone = false;
// Fixed cap matches the credential store. Kept in BSS so a scan does not fragment the reader heap.
char readingJoinSsid[33] = {};
char readingAttempted[SavedStationConnect::kMaxSavedNetworks][33] = {};
uint8_t readingAttemptedCount = 0;
char readingPending[SavedStationConnect::kMaxSavedNetworks][33] = {};
uint8_t readingPendingCount = 0;
uint8_t readingPendingNext = 0;

void copyFixed(char* dest, const size_t size, const char* src) {
  if (!dest || size == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  std::strncpy(dest, src, size - 1);
  dest[size - 1] = '\0';
}

void clearReadingCandidates() {
  readingAttemptedCount = 0;
  readingPendingCount = 0;
  readingPendingNext = 0;
  readingJoinSsid[0] = '\0';
}

bool alreadyAttempted(const char* ssid) {
  for (uint8_t i = 0; i < readingAttemptedCount; ++i) {
    if (std::strcmp(readingAttempted[i], ssid) == 0) return true;
  }
  return false;
}

void rememberAttempt(const char* ssid) {
  if (!ssid || ssid[0] == '\0' || alreadyAttempted(ssid)) return;
  if (readingAttemptedCount >= SavedStationConnect::kMaxSavedNetworks) return;
  copyFixed(readingAttempted[readingAttemptedCount], sizeof(readingAttempted[0]), ssid);
  ++readingAttemptedCount;
}

void quietReadingRadio() {
  if (readingPhase == ReadingRadioPhase::Scanning) WiFi.scanDelete();
  readingPhase = ReadingRadioPhase::Idle;
  readingPendingCount = 0;
  readingPendingNext = 0;
  readingJoinSsid[0] = '\0';
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void finishReadingAttempt(const char* reason) {
  if (reason) LOG_INF("NET", "%s", reason);
  quietReadingRadio();
}

void noteReadingConnected();

bool joinReadingSsid(GfxRenderer& renderer, const char* ssid, const char* password) {
  const bool passPassword = password && password[0] != '\0';
  if (!SavedStationConnect::begin(renderer, ssid, password, passPassword)) {
    readingPhase = ReadingRadioPhase::Idle;
    return false;
  }
  rememberAttempt(ssid);
  copyFixed(readingJoinSsid, sizeof(readingJoinSsid), ssid);
  readingPhase = ReadingRadioPhase::Joining;
  readingPhaseStartedMs = millis();
  LOG_INF("NET", "reading radio: joining %s", readingJoinSsid);
  return true;
}

bool joinNextPending(GfxRenderer& renderer) {
  while (readingPendingNext < readingPendingCount) {
    const char* ssid = readingPending[readingPendingNext];
    ++readingPendingNext;
    char pass[64] = {};
    const auto cred = WIFI_STORE.findCredential(ssid);
    if (!cred) continue;
    copyFixed(pass, sizeof(pass), cred->password.c_str());
    if (joinReadingSsid(renderer, ssid, pass)) return true;
    return false;
  }
  return false;
}

bool beginReadingScan(GfxRenderer& renderer) {
  readingScanDone = true;
  if (!SavedStationConnect::startScan(renderer)) {
    finishReadingAttempt(nullptr);
    return false;
  }
  readingPhase = ReadingRadioPhase::Scanning;
  readingPhaseStartedMs = millis();
  return true;
}

void fillPendingFromScan(const int16_t count) {
  constexpr size_t kMax = SavedStationConnect::kMaxSavedNetworks;
  // Eight saved SSIDs, copied once. The scan String from WiFi.SSID() does not outlive this function.
  char ssids[kMax][33] = {};
  SavedStationConnect::RankedSsid rows[kMax] = {};
  size_t rowCount = 0;

  for (int16_t i = 0; i < count; ++i) {
    char ssid[33];
    copyFixed(ssid, sizeof(ssid), WiFi.SSID(static_cast<uint8_t>(i)).c_str());
    if (ssid[0] == '\0' || alreadyAttempted(ssid) || !WIFI_STORE.hasSavedCredential(ssid)) continue;

    size_t existing = rowCount;
    for (size_t row = 0; row < rowCount; ++row) {
      if (std::strcmp(ssids[row], ssid) == 0) {
        existing = row;
        break;
      }
    }
    const int32_t rssi = WiFi.RSSI(static_cast<uint8_t>(i));
    if (existing < rowCount) {
      if (rssi > rows[existing].rssi) rows[existing].rssi = rssi;
      continue;
    }
    if (rowCount >= kMax) {
      size_t weakest = 0;
      for (size_t row = 1; row < rowCount; ++row) {
        if (rows[row].rssi < rows[weakest].rssi) weakest = row;
      }
      if (rssi <= rows[weakest].rssi) continue;
      copyFixed(ssids[weakest], sizeof(ssids[weakest]), ssid);
      rows[weakest].ssid = ssids[weakest];
      rows[weakest].rssi = rssi;
      rows[weakest].saved = true;
      continue;
    }
    copyFixed(ssids[rowCount], sizeof(ssids[rowCount]), ssid);
    rows[rowCount] = {ssids[rowCount], rssi, true};
    ++rowCount;
  }

  const char* skip[kMax] = {};
  size_t skipCount = 0;
  readingPendingCount = 0;
  readingPendingNext = 0;
  while (readingPendingCount < kMax && skipCount < kMax) {
    const SavedStationConnect::RankedSsid* picked =
        SavedStationConnect::pickStrongestSaved(rows, rowCount, skip, skipCount);
    if (!picked) break;
    copyFixed(readingPending[readingPendingCount], sizeof(readingPending[0]), picked->ssid);
    skip[skipCount++] = readingPending[readingPendingCount];
    ++readingPendingCount;
  }
}

void advanceAfterJoinFailure(GfxRenderer& renderer) {
  if (!readingScanDone) {
    beginReadingScan(renderer);
    return;
  }
  if (joinNextPending(renderer)) return;
  finishReadingAttempt("reading radio: no reachable saved network");
}

void pumpReadingJoin(GfxRenderer& renderer) {
  switch (SavedStationConnect::pollJoin(readingPhaseStartedMs, SavedStationConnect::kAutoJoinTimeoutMs)) {
    case SavedStationConnect::JoinWait::Pending:
      return;
    case SavedStationConnect::JoinWait::Connected:
      noteReadingConnected();
      return;
    case SavedStationConnect::JoinWait::Failed:
    case SavedStationConnect::JoinWait::TimedOut:
      advanceAfterJoinFailure(renderer);
      return;
  }
}

void pumpReadingScan(GfxRenderer& renderer) {
  const int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    if (static_cast<unsigned long>(millis() - readingPhaseStartedMs) > kReadingScanTimeoutMs) {
      finishReadingAttempt("reading radio: scan timed out");
    }
    return;
  }
  if (result < 0) {
    finishReadingAttempt("reading radio: scan failed");
    return;
  }

  fillPendingFromScan(result);
  WiFi.scanDelete();
  readingPhase = ReadingRadioPhase::Idle;
  if (joinNextPending(renderer)) return;
  finishReadingAttempt("reading radio: no saved network in range");
}

bool startReadingJoin(GfxRenderer& renderer) {
  if (readingPhase != ReadingRadioPhase::Idle) return true;
  stayConnectAttemptAtMs = millis();
  readingScanDone = false;
  clearReadingCandidates();

  if (WIFI_STORE.getCredentialCount() == 0) {
    RenderLock lock;
    WIFI_STORE.loadFromFile();
  }
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (last.empty() && WIFI_STORE.getCredentialCount() == 0) {
    LOG_INF("NET", "stay connected: no saved network");
    return false;
  }
  if (!last.empty()) {
    char ssid[33];
    char pass[64] = {};
    copyFixed(ssid, sizeof(ssid), last.c_str());
    const auto cred = WIFI_STORE.findCredential(ssid);
    if (cred) copyFixed(pass, sizeof(pass), cred->password.c_str());
    return joinReadingSsid(renderer, ssid, pass);
  }
  return beginReadingScan(renderer);
}

void noteReadingConnected() {
  if (readingPhase != ReadingRadioPhase::Joining || readingJoinSsid[0] == '\0') {
    if (readingPhase == ReadingRadioPhase::Scanning) WiFi.scanDelete();
    readingPhase = ReadingRadioPhase::Idle;
    readingJoinSsid[0] = '\0';
    return;
  }
  {
    RenderLock lock;
    WIFI_STORE.setLastConnectedSsid(readingJoinSsid);
  }
  LOG_INF("NET", "reading radio: connected %s", readingJoinSsid);
  readingPhase = ReadingRadioPhase::Idle;
  readingJoinSsid[0] = '\0';
}
}

bool smartOnlineReading() { return SETTINGS.wifiStayConnected != 0; }

bool stationConnected() {
  const wifi_mode_t mode = WiFi.getMode();
  return (mode & WIFI_MODE_STA) != 0 && WiFi.status() == WL_CONNECTED;
}

bool stayConnected() { return readingHold; }

void holdReadingRadio(const bool hold) { readingHold = hold; }

void releaseReadingRadio() {
  readingHold = false;
  stayConnectAttemptAtMs = 0;
  readingScanDone = false;
  clearReadingCandidates();
  const bool radioOn = WiFi.getMode() != WIFI_MODE_NULL || readingPhase == ReadingRadioPhase::Scanning;
  quietReadingRadio();
  if (!radioOn) return;
  LOG_INF("NET", "reading radio off");
}

bool beginSavedNetwork(GfxRenderer& renderer) {
  if (WiFi.status() == WL_CONNECTED) return true;
  return startReadingJoin(renderer);
}

void setStayConnected(GfxRenderer& renderer, const bool on) {
  (void)renderer;
  SETTINGS.wifiStayConnected = on ? 1 : 0;
  SETTINGS.saveToFile();
  if (!on) {
    releaseReadingRadio();
    LOG_INF("NET", "smart online off");
    return;
  }
  LOG_INF("NET", "smart online on");
}

void serviceStayConnected(GfxRenderer& renderer) {
  if (!readingHold || !smartOnlineReading()) return;
  if (WiFi.status() == WL_CONNECTED) {
    noteReadingConnected();
    return;
  }
  switch (readingPhase) {
    case ReadingRadioPhase::Idle: {
      // One attempt at a time. A miss or a dropped link retries a minute later,
      // instead of calling begin() on the reading heartbeat.
      const unsigned long now = millis();
      if (stayConnectAttemptAtMs != 0 &&
          static_cast<unsigned long>(now - stayConnectAttemptAtMs) < kReadingJoinRetryMs) {
        return;
      }
      startReadingJoin(renderer);
      return;
    }
    case ReadingRadioPhase::Joining:
      pumpReadingJoin(renderer);
      return;
    case ReadingRadioPhase::Scanning:
      pumpReadingScan(renderer);
      return;
  }
}

void releaseOwnedRadio() {
  if (readingHold) {
    LOG_DBG("NET", "Wi-Fi kept: reading session");
    return;
  }
  WiFi.disconnect(false);
}

}  // namespace NetworkStartup
