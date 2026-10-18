#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Station join and scan used by the Wi-Fi selection screen. Reading's silent
// auto-connect calls the same entry points instead of a second driver sequence.
namespace SavedStationConnect {

// WifiCredentialStore keeps at most this many networks.
constexpr size_t kMaxSavedNetworks = 8;

// Unattended auto-connect waits this long before trying the next saved network.
// The selection screen's manual connect uses a longer timeout of its own.
constexpr unsigned long kAutoJoinTimeoutMs = 7000;

enum class JoinWait : uint8_t { Pending, Connected, Failed, TimedOut };

struct RankedSsid {
  const char* ssid;
  int32_t rssi;
  bool saved;
};

// Empty password, or passPassword false, joins an open network.
// False means STA mode failed to start: no disconnect and no begin.
bool begin(GfxRenderer& renderer, const char* ssid, const char* password, bool passPassword);

// Async scan. False means STA mode failed (no disconnect, no scan) or the scan
// itself failed after the station disconnect.
bool startScan(GfxRenderer& renderer);

JoinWait pollJoin(unsigned long startedMs, unsigned long timeoutMs);

// Strongest saved SSID not listed in skip. Equal RSSI keeps the earlier row.
// nullptr when none qualify. Pointers in the result alias `rows`.
const RankedSsid* pickStrongestSaved(const RankedSsid* rows, size_t count, const char* const* skip, size_t skipCount);

}  // namespace SavedStationConnect
