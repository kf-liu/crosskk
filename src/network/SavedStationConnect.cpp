#include "network/SavedStationConnect.h"

#include <Logging.h>
#include <WiFi.h>
#if CROSSPOINT_EMULATED == 0
#include <esp_mac.h>
#endif

#include <cstdio>
#include <cstring>

#include "NetworkStartup.h"

namespace {

bool readStationMac(uint8_t (&mac)[6]) {
#if CROSSPOINT_EMULATED
  return WiFi.macAddress(mac);
#else
  return esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;
#endif
}

}  // namespace

namespace SavedStationConnect {

bool begin(GfxRenderer& renderer, const char* ssid, const char* password, const bool passPassword) {
  if (!ssid || ssid[0] == '\0') {
    LOG_ERR("WIFI", "Station join missing SSID");
    return false;
  }

  WiFi.persistent(false);  // Credentials live in WifiCredentialStore; suppress SDK NVS auto-connect.
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) {
    LOG_ERR("WIFI", "Station initialization failed");
    return false;
  }
  WiFi.disconnect(true, true);  // Abort any in-progress SDK auto-connect and clear the NVS SSID.
  delay(100);

  // All-channel scan plus signal sort picks the strongest BSSID when one SSID
  // is advertised by several APs. The default fast scan stops at the first.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  uint8_t mac[6] = {};
  if (readStationMac(mac)) {
    char hostname[sizeof("CrossPoint-Reader-") + 12];
    snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    WiFi.setHostname(hostname);
  } else {
    LOG_ERR("WIFI", "Failed to read station MAC for hostname");
  }

  if (passPassword && password && password[0] != '\0') {
    WiFi.begin(ssid, password);
  } else {
    WiFi.begin(ssid);
  }
  return true;
}

bool startScan(GfxRenderer& renderer) {
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) {
    LOG_ERR("WIFI", "Station initialization failed");
    return false;
  }
  WiFi.disconnect();
  delay(100);

  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
    LOG_ERR("WIFI", "Could not start scan");
    return false;
  }
  return true;
}

JoinWait pollJoin(const unsigned long startedMs, const unsigned long timeoutMs) {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) return JoinWait::Connected;
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) return JoinWait::Failed;
  if (millis() - startedMs > timeoutMs) return JoinWait::TimedOut;
  return JoinWait::Pending;
}

const RankedSsid* pickStrongestSaved(const RankedSsid* rows, const size_t count, const char* const* skip,
                                     const size_t skipCount) {
  const RankedSsid* best = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const RankedSsid& row = rows[i];
    if (!row.saved || !row.ssid || row.ssid[0] == '\0') continue;
    bool seen = false;
    for (size_t s = 0; s < skipCount; ++s) {
      if (skip && skip[s] && std::strcmp(skip[s], row.ssid) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) continue;
    if (!best || row.rssi > best->rssi) best = &row;
  }
  return best;
}

}  // namespace SavedStationConnect
