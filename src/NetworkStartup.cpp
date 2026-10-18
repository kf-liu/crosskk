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
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  LOG_INF("NET", "reading radio off");
}

bool beginSavedNetwork(GfxRenderer& renderer) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (last.empty()) {
    LOG_INF("NET", "stay connected: no saved network");
    return false;
  }
  const auto cred = WIFI_STORE.findCredential(last);
  if (!cred) {
    LOG_INF("NET", "stay connected: saved network has no credential");
    return false;
  }
  // Copy off the store's strings before the driver starts. Password stays off the log.
  char ssid[33];
  char pass[64];
  std::strncpy(ssid, cred->ssid.c_str(), sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  std::strncpy(pass, cred->password.c_str(), sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = '\0';

  WiFi.persistent(false);
  if (!setMode(renderer, WIFI_STA)) return false;
  // begin() returns the current status. WL_IDLE_STATUS is 0 and means the
  // join was accepted, not that it failed, so the return value is not a bool.
  if (pass[0] == '\0') {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, pass);
  }
  LOG_INF("NET", "reading radio: joining %s", ssid);
  return true;
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
  if (WiFi.status() == WL_CONNECTED) return;
  // One join at a time. A failed or dropped link retries a minute later,
  // instead of calling begin() on the reading heartbeat.
  const unsigned long now = millis();
  if (stayConnectAttemptAtMs != 0 && static_cast<unsigned long>(now - stayConnectAttemptAtMs) < 60000UL) return;
  stayConnectAttemptAtMs = now;
  beginSavedNetwork(renderer);
}

void releaseOwnedRadio() {
  if (readingHold) {
    LOG_DBG("NET", "Wi-Fi kept: reading session");
    return;
  }
  WiFi.disconnect(false);
}

}  // namespace NetworkStartup
