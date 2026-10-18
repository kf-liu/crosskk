#pragma once

#include <WiFi.h>

class GfxRenderer;

namespace NetworkStartup {

// One snapshot per network lifecycle transition; no persistent buffers.
void logMemory(const char* stage);

// Preserve render memory when PSRAM/internal SRAM headroom is healthy; otherwise
// release it before the Wi-Fi driver starts allocating.
void prepare(GfxRenderer& renderer);

// The only entry point for enabling STA/AP mode in application code.
bool setMode(GfxRenderer& renderer, wifi_mode_t mode);

// 下拉窗「智能在线阅读」。只表示用户选了这个模式，主页不会因此连着无线电。
bool smartOnlineReading();

// STA 已经连上一个网络。与「在线阅读 / 离线阅读」无关，热点模式也不算。
bool stationConnected();

// 一次阅读会话还握着这条连接（含退出后的进度同步）。其它画面用完无线电就关掉。
bool stayConnected();

// 阅读开始时握住；退出且进度同步完成后松开。松开会关掉无线电。
void holdReadingRadio(bool hold);
void releaseReadingRadio();

// Join the last saved network without waiting. Already-connected is success.
// No saved network returns false and leaves the radio as it was.
bool beginSavedNetwork(GfxRenderer& renderer);

// 记下模式。关掉时立即断无线电。打开时不在主页主动连接，等进入阅读。
void setStayConnected(GfxRenderer& renderer, bool on);

// 只有阅读会话握着连接时才补连，大约每分钟一次。
void serviceStayConnected(GfxRenderer& renderer);

// Task-exit helper. Disconnects unless a reading session is holding the radio.
void releaseOwnedRadio();

}  // namespace NetworkStartup
