#pragma once

#include <HalStorage.h>
#include <LibraryIndexFile.h>

#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"

class KkShelfActivity final : public Activity {
 public:
  enum class Filter : uint8_t { All, WeRead, Local };

  explicit KkShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KkShelf", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  MainTab mainTab() const override { return MainTab::Bookshelf; }
  bool mainTabBackReturnsToTabs() const override { return !uncachedNotice; }
  void selectMainTabContentEdge(MainTabContentEdge edge) override;

 private:
  enum class MoveGesture : uint8_t { Idle, Previous, Next, PreviousPage, NextPage };

  library::LibraryIndexFile libraryIndex;
  HalFile shelfFile;
  uint32_t wereadCount = 0;
  uint16_t localCount = 0;
  int selected = 0;
  Filter filter = Filter::All;
  bool uncachedNotice = false;
  MoveGesture moveGesture = MoveGesture::Idle;
  uint32_t lastPageTurnAt = 0;
  // 翻页时反复读取书名和路径，进页面时留好容量，退出时释放。
  std::string titleScratch;
  std::string pathScratch;
  // 当前页灰阶封面的绘制位置。三次刷新复用，不放进渲染任务栈。
  struct CoverPaint {
    char path[192];
    int x = 0;
    int y = 0;
    int dstW = 0;
    int dstH = 0;
  };
  static constexpr int kCoverSlots = 10;
  CoverPaint coverPaints[kCoverSlots]{};
  int coverPaintCount = 0;
  int coverPreparedPage = -1;
  // 一行封面数据。整张图进内存会超过 C3 的连续堆，所以按行复用。
  std::unique_ptr<uint8_t[]> coverRow;
  size_t coverRowBytes = 0;

  int itemCount() const;
  void setFilter(Filter next);
  void stepFilter(int direction);
  void moveSelection(int index);
  void openSelected();
  Rect filterRect(const Rect& content) const;
  Rect gridRect(const Rect& content) const;
  bool ensureCoverRow(size_t bytes);
  bool drawShelfCover(const char* path, const Rect& bounds, bool allowGray, bool recordGray);
  void prepareDownloadedCovers(int pageStart, int pageEnd);
};
