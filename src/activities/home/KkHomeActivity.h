#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"

class Page;

class KkHomeActivity final : public Activity {
  enum class CoverPrep : uint8_t { Pending, Ready, Failed };
  enum class PreviewPrep : uint8_t { Pending, Ready, Failed };

  Rect bookPanel() const;
  Rect readingPreviewRect() const;
  void openCurrentBook();
  bool rememberExistingCover(const RecentBook& book);
  bool ensureCover(const RecentBook& book);
  bool ensureReadingPreview(const RecentBook& book, int previewHeight);
  void drawReadingPreview(const Rect& area);
  uint32_t wereadReadingSeconds(const RecentBook& book);

  CoverPrep coverPrep = CoverPrep::Pending;
  int coverPixelWidth = 0;
  int coverPixelHeight = 0;
  // 封面路径和当前书路径按活动生命周期留在对象里，避免每帧分配。
  char coverBook[256] = {};
  char coverFile[320] = {};
  bool wereadTimeKnown = false;
  char wereadTimeBook[256] = {};
  uint32_t wereadSeconds = 0;
  PreviewPrep previewPrep = PreviewPrep::Pending;
  int previewSpine = -1;
  int previewPage = -1;
  int previewMarginLeft = 0;
  float previewLineCompression = 1.0f;
  char previewBook[256] = {};
  // 最多两页已排好的正文。进入主页时读一次，离开时释放。
  std::vector<std::unique_ptr<Page>> previewPages;

 public:
  explicit KkHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~KkHomeActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  MainTab mainTab() const override { return MainTab::Home; }
};
