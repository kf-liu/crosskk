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

// 和 Wi-Fi 选择界面同一条自动连接：先试上次的网络，失败再扫描，只连当前可见且已保存的。
// 没有就结束，不弹出选择界面。已经连上算成功。不等待连接结果。
bool beginSavedNetwork(GfxRenderer& renderer);

// 记下模式。关掉时立即断无线电。打开时不在主页主动连接，等进入阅读。
void setStayConnected(GfxRenderer& renderer, bool on);

// 阅读会话握着连接时，把进行中的扫描或连接收完。失败大约每分钟再试一次。
// 这一轮没有可见的已保存网络就停。
void serviceStayConnected(GfxRenderer& renderer);

// Task-exit helper. Disconnects unless a reading session is holding the radio.
void releaseOwnedRadio();

}  // namespace NetworkStartup
