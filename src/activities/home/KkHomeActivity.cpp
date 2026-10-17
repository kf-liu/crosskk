#include "KkHomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadStore.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/UiHighDpiProfile.h"
#include "components/themes/inx/InxTheme.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"

namespace {
constexpr int kThumbHeight = InxMetrics::values.homeCoverHeight;
constexpr int kGap = 12;

const RecentBook* currentBook() {
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!RecentBooksStore::isMissing(book)) return &book;
  }
  return nullptr;
}

void formatDuration(const uint64_t totalMs, char* out, const size_t outSize) {
  const uint64_t totalMinutes = totalMs / 60000ULL;
  const uint64_t hours = totalMinutes / 60ULL;
  const uint64_t minutes = totalMinutes % 60ULL;
  if (hours == 0) {
    snprintf(out, outSize, "%llum", static_cast<unsigned long long>(minutes));
    return;
  }
  snprintf(out, outSize, "%lluh %llum", static_cast<unsigned long long>(hours),
           static_cast<unsigned long long>(minutes));
}

// 行缓冲按封面宽度分配，三次绘制复用，不放进渲染任务栈。
struct CoverScratch {
  std::unique_ptr<uint8_t[]> bytes;
  int packedBytes = 0;
  int rawBytes = 0;
  int accumOffset = 0;
  int dstW = 0;
};

bool copyExistingPath(const char* path, char* output, const size_t outputSize) {
  if (!path || path[0] == '\0') return false;
  const size_t length = strlen(path);
  if (length >= outputSize || !Storage.exists(path)) return false;
  memcpy(output, path, length + 1);
  return true;
}

// 小图 thumb_226.bmp 是 1 位噪声抖动，放大后全是噪点。灰度封面在同一缓存目录。
bool grayCoverBeside(const std::string& source, char* output, const size_t outputSize) {
  constexpr char name[] = "cover_legacy_v2.bmp";
  if (source.size() >= sizeof(name) - 1 &&
      source.compare(source.size() - (sizeof(name) - 1), sizeof(name) - 1, name) == 0) {
    return copyExistingPath(source.c_str(), output, outputSize);
  }
  const size_t slash = source.rfind('/');
  if (slash == std::string::npos) return false;
  if (slash + sizeof(name) >= outputSize) return false;
  memcpy(output, source.data(), slash + 1);
  memcpy(output + slash + 1, name, sizeof(name));
  return Storage.exists(output);
}

void fittedCoverSize(const int srcW, const int srcH, const int maxW, const int maxH, int& dstW, int& dstH) {
  dstW = std::max(1, srcW);
  dstH = std::max(1, srcH);
  if (dstH > maxH) {
    dstW = std::max(1, srcW * maxH / srcH);
    dstH = maxH;
  }
  if (dstW > maxW) {
    dstH = std::max(1, dstH * maxW / dstW);
    dstW = maxW;
  }
  if (dstW > srcW) dstW = srcW;
  if (dstH > srcH) dstH = srcH;
}

bool prepareCoverScratch(const Bitmap& bitmap, const int dstW, CoverScratch& scratch) {
  const int packedBytes = (bitmap.getWidth() + 3) / 4;
  const int rawBytes = bitmap.getRowBytes();
  if (packedBytes <= 0 || rawBytes <= 0 || dstW <= 0) return false;
  const size_t rowBytes = static_cast<size_t>(packedBytes) + static_cast<size_t>(rawBytes);
  const size_t accumOffset = (rowBytes + 3u) & ~size_t{3};
  const size_t bytes = accumOffset + static_cast<size_t>(dstW) * sizeof(uint32_t) * 2;
  // 一行原始数据加目标列的和与计数。整张封面进内存会超过 C3 的连续堆。
  if (bytes > 16384 || accumOffset > 16384) {
    LOG_ERR("KK", "Cover row scratch too large (%u)", static_cast<unsigned>(bytes));
    return false;
  }
  scratch.bytes = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!scratch.bytes) {
    LOG_ERR("KK", "OOM: cover row scratch (%u)", static_cast<unsigned>(bytes));
    return false;
  }
  scratch.packedBytes = packedBytes;
  scratch.rawBytes = rawBytes;
  scratch.accumOffset = static_cast<int>(accumOffset);
  scratch.dstW = dstW;
  return true;
}

void paintCoverLevel(const GfxRenderer& renderer, const int x, const int y, const uint8_t level) {
  const auto mode = renderer.getRenderMode();
  if (mode == GfxRenderer::BW) {
    const auto pixel = GfxRenderer::mapTwoBitPixel(mode, level);
    if (pixel.draw) renderer.drawPixel(x, y, pixel.state);
    return;
  }
  const auto plane = grayPlanePixel(level, mode == GfxRenderer::GRAYSCALE_MSB, renderer.grayPlanesAreAbsolute());
  if (plane.write) renderer.drawPixel(x, y, plane.black);
}

// 按目标像素分块平均后再量化成 4 级，去掉 1 位缩略图的噪点。
bool paintAreaAveragedCover(const GfxRenderer& renderer, const Bitmap& bitmap, const int originX, const int originY,
                            const int dstW, const int dstH, CoverScratch& scratch) {
  if (dstW <= 0 || dstH <= 0 || scratch.dstW != dstW || !scratch.bytes) return false;
  if (bitmap.rewindToData() != BmpReaderError::Ok) return false;
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  if (srcW <= 0 || srcH <= 0) return false;

  uint8_t* const packed = scratch.bytes.get();
  uint8_t* const raw = packed + scratch.packedBytes;
  auto* const sum = reinterpret_cast<uint32_t*>(packed + scratch.accumOffset);
  auto* const count = sum + dstW;
  int currentDest = -1;

  const auto reset = [&]() {
    memset(sum, 0, static_cast<size_t>(dstW) * sizeof(uint32_t));
    memset(count, 0, static_cast<size_t>(dstW) * sizeof(uint32_t));
  };
  const auto emit = [&](const int destY) {
    if (destY < 0) return;
    for (int dx = 0; dx < dstW; ++dx) {
      if (count[dx] == 0) continue;
      const uint32_t level = (sum[dx] + count[dx] / 2) / count[dx];
      paintCoverLevel(renderer, originX + dx, originY + destY, static_cast<uint8_t>(level > 3 ? 3 : level));
    }
  };

  for (int fileY = 0; fileY < srcH; ++fileY) {
    if (bitmap.readNextRow(packed, raw) != BmpReaderError::Ok) return false;
    const int screenY = bitmap.isTopDown() ? fileY : srcH - 1 - fileY;
    const int destY = screenY * dstH / srcH;
    if (destY != currentDest) {
      emit(currentDest);
      reset();
      currentDest = destY;
    }
    for (int x = 0; x < srcW; ++x) {
      const uint8_t gray = packed[x / 4] >> (6 - ((x * 2) % 8)) & 0x3;
      const int destX = x * dstW / srcW;
      sum[destX] += gray;
      count[destX] += 1;
    }
  }
  emit(currentDest);
  return true;
}

void drawStat(const GfxRenderer& renderer, const Rect cell, const char* label, const char* value) {
  const int innerWidth = std::max(1, cell.width);
  const GfxRenderer::ClipScope clip(renderer, cell.x, cell.y, innerWidth, std::max(1, cell.height));
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const std::string shownLabel = renderer.truncatedText(SMALL_FONT_ID, label, innerWidth);
  const std::string shownValue = renderer.truncatedText(UI_10_FONT_ID, value, innerWidth, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, cell.x, cell.y, shownLabel.c_str());
  renderer.drawText(UI_10_FONT_ID, cell.x, cell.y + labelHeight + 2, shownValue.c_str(), true, EpdFontFamily::BOLD);
}
}  // namespace

static ReaderRenderSpec readerSpecForPreview(const GfxRenderer& renderer, const MappedInputManager& input,
                                             int& marginLeft) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  top += SETTINGS.screenMargin;
  left += SETTINGS.screenMargin;
  right += SETTINGS.screenMargin;
  int bottomReserve = UITheme::getInstance().getStatusBarHeight();
  if (UiHighDpiProfile::enabled && SETTINGS.statusBarSpec().textLaneVisible()) {
    const int reclaim = UITheme::getStatusBarTextTopPadding(renderer) - UiHighDpiProfile::readerContentStatusGap;
    bottomReserve -= std::max(0, reclaim);
  }
  bottom += std::max(static_cast<int>(SETTINGS.screenMargin), bottomReserve);
#if FREEINK_DEVICE_EEGO_A4
  bottom += 4;
#endif
  const int width = std::max(0, renderer.getScreenWidth() - left - right);
  const int height = std::max(0, renderer.getScreenHeight() - top - bottom);
  marginLeft = left;
  auto spec = SETTINGS.readerRenderSpec(static_cast<uint16_t>(width), static_cast<uint16_t>(height));
  spec.collectTouchLinks = input.hasTouch();
  return spec;
}

static int textLineHeight(const GfxRenderer& renderer, const TextBlock& block, const int fontId,
                          const float compression) {
  return renderer.getLineHeight(fontId, compression) + block.getRubyShift(renderer.getFontAscenderSize(fontId));
}

// 图片占位不计入预览高度，后面的正文往上接。
static int textSpan(const GfxRenderer& renderer, const Page& page, const int fontId, const float compression) {
  int first = -1;
  int lastBottom = 0;
  int imageHeight = 0;
  for (const auto& element : page.elements) {
    if (!element) continue;
    if (element->getTag() == TAG_PageImage) {
      if (first >= 0) imageHeight += static_cast<const PageImage&>(*element).getImageBlock().getHeight();
      continue;
    }
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || block->wordCount() == 0) continue;
    if (first < 0) first = line.yPos;
    lastBottom = line.yPos + textLineHeight(renderer, *block, fontId, compression);
  }
  if (first < 0 || lastBottom <= first) return 0;
  return std::max(0, lastBottom - first - imageHeight);
}

static bool readProgress(const std::string& cachePath, int& spine, int& page) {
  HalFile progress;
  if (!Storage.openFileForRead("KK", cachePath + "/progress.bin", progress)) return false;
  uint8_t data[10] = {};
  const int size = progress.read(data, sizeof(data));
  if (size != 4 && size != 6 && size != 10) return false;
  spine = data[0] | (data[1] << 8);
  page = data[2] | (data[3] << 8);
  if (page == 0xFFFF) page = 0;
  return spine >= 0 && page >= 0;
}

KkHomeActivity::KkHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("KkHome", renderer, mappedInput) {}

KkHomeActivity::~KkHomeActivity() = default;

void KkHomeActivity::onEnter() {
  Activity::onEnter();
  coverPrep = CoverPrep::Pending;
  coverPixelWidth = 0;
  coverPixelHeight = 0;
  coverBook[0] = '\0';
  coverFile[0] = '\0';
  wereadTimeKnown = false;
  wereadTimeBook[0] = '\0';
  wereadSeconds = 0;
  previewPrep = PreviewPrep::Pending;
  previewSpine = -1;
  previewPage = -1;
  previewBook[0] = '\0';
  previewPages.clear();
  previewPages.reserve(2);
}

void KkHomeActivity::onExit() {
  previewPages.clear();
  previewPrep = PreviewPrep::Pending;
  Activity::onExit();
}

Rect KkHomeActivity::bookPanel() const {
  const Rect content = pageContentRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int imageHeight = coverPixelHeight > 0 ? coverPixelHeight : kThumbHeight;
  const int height = std::min(content.height, imageHeight + metrics.contentSidePadding * 2);
  return Rect{content.x, content.y, content.width, std::max(1, height)};
}

Rect KkHomeActivity::readingPreviewRect() const {
  const Rect content = pageContentRect();
  const Rect panel = bookPanel();
  const int top = panel.y + panel.height;
  const int height = content.y + content.height - top;
  return Rect{content.x, top, content.width, std::max(0, height)};
}

bool KkHomeActivity::ensureReadingPreview(const RecentBook& book, const int previewHeight) {
  const bool sameBook = book.path.size() < sizeof(previewBook) && strcmp(previewBook, book.path.c_str()) == 0;
  if (sameBook && previewPrep == PreviewPrep::Failed) return false;
  if (previewHeight <= 0) return false;
  if (sameBook && previewPrep == PreviewPrep::Ready && !previewPages.empty()) return true;

  previewPages.clear();
  previewPages.reserve(2);
  previewPrep = PreviewPrep::Failed;
  previewBook[0] = '\0';
  if (book.path.size() >= sizeof(previewBook)) return false;
  memcpy(previewBook, book.path.c_str(), book.path.size() + 1);

  auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("KK", "OOM: preview epub");
    return false;
  }
  int spine = 0;
  int page = 0;
  if (!readProgress(epub->getCachePath(), spine, page)) return false;

  // 空控制块的别名指针，避免 make_shared 在失败时 abort。Epub 仍由 unique_ptr 释放。
  const std::shared_ptr<Epub> view(std::shared_ptr<Epub>(), epub.get());
  auto section = makeUniqueNoThrow<Section>(view, spine, renderer);
  if (!section) {
    LOG_ERR("KK", "OOM: preview section (%u bytes)", static_cast<unsigned>(sizeof(Section)));
    return false;
  }

  int marginLeft = 0;
  const ReaderRenderSpec spec = readerSpecForPreview(renderer, mappedInput, marginLeft);
  int filled = 0;
  for (int index = 0; index < 2; ++index) {
    auto loaded = section->loadCachedPage(spec, page + index);
    if (!loaded) break;
    const int span = textSpan(renderer, *loaded, spec.fontId, spec.lineCompression);
    previewPages.push_back(std::move(loaded));
    filled += span;
    if (filled >= previewHeight) break;
  }
  if (previewPages.empty()) return false;
  previewSpine = spine;
  previewPage = page;
  previewMarginLeft = marginLeft;
  previewLineCompression = spec.lineCompression;
  previewPrep = PreviewPrep::Ready;
  return true;
}

void KkHomeActivity::drawReadingPreview(const Rect& area) {
  if (previewPrep != PreviewPrep::Ready || previewPages.empty() || area.height <= 8) return;
  const Rect textArea{area.x, area.y + 8, area.width, area.height - 8};
  const GfxRenderer::ClipScope clip(renderer, textArea.x, textArea.y, textArea.width, textArea.height);
  const GfxRenderer::SyntheticBoldScope syntheticBold(renderer, SETTINGS.fakeBold);
  const int fontId = SETTINGS.getReaderFontId();
  const int limit = textArea.y + textArea.height;
  int cursor = textArea.y;

  for (const auto& page : previewPages) {
    if (!page || cursor >= limit) return;
    int prevY = 0;
    int prevTop = cursor;
    int imageBetween = 0;
    bool haveLine = false;
    for (const auto& element : page->elements) {
      if (!element) continue;
      if (element->getTag() == TAG_PageImage) {
        imageBetween += static_cast<const PageImage&>(*element).getImageBlock().getHeight();
        continue;
      }
      if (element->getTag() != TAG_PageLine) continue;
      auto& line = static_cast<PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block || block->wordCount() == 0) continue;
      const int lineHeight = textLineHeight(renderer, *block, fontId, previewLineCompression);
      int top = cursor;
      if (haveLine) {
        int advance = line.yPos - prevY - imageBetween;
        if (advance < lineHeight) advance = lineHeight;
        top = prevTop + advance;
      }
      if (top >= limit) return;
      line.render(renderer, fontId, previewMarginLeft, top - line.yPos);
      prevY = line.yPos;
      prevTop = top;
      imageBetween = 0;
      haveLine = true;
      cursor = top + lineHeight;
    }
  }
}

bool KkHomeActivity::rememberExistingCover(const RecentBook& book) {
  const bool sameBook = book.path.size() < sizeof(coverBook) && strcmp(coverBook, book.path.c_str()) == 0;
  if (sameBook && coverPrep == CoverPrep::Ready && coverFile[0] != '\0' && Storage.exists(coverFile)) return true;
  const ReadingBookStats* stats = READING_STATS.findBook(book.path);
  const std::string& source = !book.coverBmpPath.empty() ? book.coverBmpPath
                               : stats && !stats->coverBmpPath.empty() ? stats->coverBmpPath
                                                                       : book.coverBmpPath;
  if (!grayCoverBeside(source, coverFile, sizeof(coverFile))) {
    coverFile[0] = '\0';
    return false;
  }
  if (book.path.size() < sizeof(coverBook)) memcpy(coverBook, book.path.c_str(), book.path.size() + 1);
  coverPrep = CoverPrep::Ready;
  return true;
}

bool KkHomeActivity::ensureCover(const RecentBook& book) {
  const bool sameBook = book.path.size() < sizeof(coverBook) && strcmp(coverBook, book.path.c_str()) == 0;
  if (sameBook && coverPrep == CoverPrep::Failed) return false;

  renderer.displayBuffer();
  std::string generated;
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    generated = BookCoverLoader::ensureFullCover(book.path);
  }
  if (book.path.size() < sizeof(coverBook)) memcpy(coverBook, book.path.c_str(), book.path.size() + 1);
  if (!generated.empty() && generated.size() < sizeof(coverFile)) {
    memcpy(coverFile, generated.c_str(), generated.size() + 1);
    coverPrep = CoverPrep::Ready;
  } else {
    coverFile[0] = '\0';
    coverPrep = CoverPrep::Failed;
  }
  requestUpdate();
  return true;
}

uint32_t KkHomeActivity::wereadReadingSeconds(const RecentBook& book) {
  if (wereadTimeKnown && book.path.size() < sizeof(wereadTimeBook) && strcmp(wereadTimeBook, book.path.c_str()) == 0) {
    return wereadSeconds;
  }
  wereadSeconds = 0;
  wereadTimeKnown = true;
  wereadTimeBook[0] = '\0';
  if (book.path.size() < sizeof(wereadTimeBook)) memcpy(wereadTimeBook, book.path.c_str(), book.path.size() + 1);
  char bookId[64] = {};
  uint32_t seconds = 0;
  if (WeReadStore::findBookIdForPath(book.path, bookId, sizeof(bookId)) &&
      WeReadStore::loadBookReadingSeconds(bookId, seconds)) {
    wereadSeconds = seconds;
  }
  return wereadSeconds;
}

void KkHomeActivity::openCurrentBook() {
  const RecentBook* book = currentBook();
  if (!book || book->path.empty()) return;
  activityManager.goToReader(book->path);
}

void KkHomeActivity::loop() {
  if (!currentBook()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openCurrentBook();
    return;
  }
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  const Rect panel = bookPanel();
  const Rect preview = readingPreviewRect();
  const bool inPanel = x >= panel.x && x < panel.x + panel.width && y >= panel.y && y < panel.y + panel.height;
  const bool inPreview = preview.height > 0 && x >= preview.x && x < preview.x + preview.width && y >= preview.y &&
                         y < preview.y + preview.height;
  if (inPanel || inPreview) openCurrentBook();
}

void KkHomeActivity::render(RenderLock&&) {
  const RecentBook* book = currentBook();
  if (book) ensureReadingPreview(*book, readingPreviewRect().height);
  const bool coverReady = book && rememberExistingCover(*book);
  HalFile cover;
  const bool coverOpen = coverReady && Storage.openFileForRead("KK", coverFile, cover);

  const auto present = [&](Bitmap* bitmap, bool grayCover, int dstW, int dstH, CoverScratch* scratch) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    int coverX = 0;
    int coverY = 0;
    const auto drawFrame = [&]() {
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.clearScreen();
      drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_KK_TAB_HOME));

      const Rect panel = bookPanel();
      if (!book) {
        UITheme::drawCenteredWrappedText(renderer, panel, UI_12_FONT_ID, tr(STR_KK_NO_BOOK), 2);
      } else {
        const int pad = metrics.contentSidePadding;
        const int innerX = panel.x + pad;
        const int innerY = panel.y + pad;
        const int innerW = std::max(1, panel.width - pad * 2);
        const int coverW = coverPixelWidth > 0 ? coverPixelWidth : kThumbHeight * 3 / 5;
        const int coverH = coverPixelHeight > 0 ? coverPixelHeight : kThumbHeight;
        coverX = innerX;
        coverY = innerY;
        bool coverDrawn = false;
        if (bitmap && grayCover && scratch) {
          coverDrawn = paintAreaAveragedCover(renderer, *bitmap, coverX, coverY, dstW, dstH, *scratch);
        }
        if (!coverDrawn) {
          renderer.drawRect(innerX, innerY, coverW, coverH);
          if (coverW >= 32 && coverH >= 32) {
            renderer.drawIcon(CoverIcon, innerX + (coverW - 32) / 2, innerY + (coverH - 32) / 2, 32);
          }
        }

        const int statsX = innerX + coverW + kGap;
        const int statsW = std::max(1, innerX + innerW - statsX);
        const char* title = book->title.empty() ? book->path.c_str() : book->title.c_str();
        const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
        const std::string shownTitle = renderer.truncatedText(UI_12_FONT_ID, title, statsW, EpdFontFamily::BOLD);
        renderer.drawText(UI_12_FONT_ID, statsX, innerY, shownTitle.c_str(), true, EpdFontFamily::BOLD);

        const ReadingBookStats* stats = READING_STATS.findBook(book->path);
        const uint64_t totalMs = stats ? stats->totalReadingMs : 0;
        const uint64_t reportedMs = stats ? stats->reportedReadingMs : 0;
        const uint64_t unreportedMs = totalMs > reportedMs ? totalMs - reportedMs : 0;
        char totalText[24];
        char unreportedText[24];
        char wereadText[24];
        char progressText[16];
        formatDuration(totalMs, totalText, sizeof(totalText));
        formatDuration(unreportedMs, unreportedText, sizeof(unreportedText));
        formatDuration(static_cast<uint64_t>(wereadReadingSeconds(*book)) * 1000ULL, wereadText, sizeof(wereadText));
        snprintf(progressText, sizeof(progressText), "%u%%",
                 static_cast<unsigned>(stats ? stats->lastProgressPercent : 0));

        const int gridY = innerY + titleHeight + 8;
        const int gridH = std::max(1, coverH - titleHeight - 8);
        const int cellW = std::max(1, (statsW - kGap) / 2);
        const int cellH = std::max(1, (gridH - kGap) / 2);
        const char* statLabels[] = {tr(STR_KK_LOCAL_TIME), tr(STR_KK_UNREPORTED_TIME), tr(STR_KK_WEREAD_TIME),
                                    tr(STR_KK_TOTAL_PROGRESS)};
        const char* values[] = {totalText, unreportedText, wereadText, progressText};
        for (int i = 0; i < 4; ++i) {
          const int column = i % 2;
          const int row = i / 2;
          drawStat(renderer, Rect{statsX + column * (cellW + kGap), gridY + row * (cellH + kGap), cellW, cellH},
                   statLabels[i], values[i]);
        }
      }

      renderer.drawLine(panel.x, panel.y + panel.height, panel.x + panel.width - 1, panel.y + panel.height, true);
      drawReadingPreview(readingPreviewRect());
      const auto labels = mainTabButtonLabels(SETTINGS.standbyShortcutEnabled ? tr(STR_STANDBY_TITLE) : "",
                                              book ? tr(STR_OPEN) : "", false);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    };

    drawFrame();
    if (book && !coverReady) {
      if (ensureCover(*book)) return;
    }
    if (bitmap && grayCover && scratch) {
      renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
      bool planesReady = true;
      for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
        renderer.clearScreen(0x00);
        renderer.setRenderMode(mode);
        if (!paintAreaAveragedCover(renderer, *bitmap, coverX, coverY, dstW, dstH, *scratch)) {
          planesReady = false;
          break;
        }
        if (mode == GfxRenderer::GRAYSCALE_LSB) {
          renderer.copyGrayscaleLsbBuffers();
        } else {
          renderer.copyGrayscaleMsbBuffers();
        }
      }
      if (planesReady) {
        renderer.displayGrayBuffer();
        drawFrame();
        renderer.cleanupGrayscaleWithFrameBuffer();
        return;
      }
      drawFrame();
    }
    renderer.displayBuffer();
  };

  if (!coverOpen) {
    coverPixelWidth = 0;
    coverPixelHeight = 0;
    present(nullptr, false, 0, 0, nullptr);
    return;
  }

  Bitmap bitmap(cover);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
    coverPixelWidth = 0;
    coverPixelHeight = 0;
    present(nullptr, false, 0, 0, nullptr);
    return;
  }

  const Rect content = pageContentRect();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int maxW = std::max(1, (content.width - metrics.contentSidePadding * 2) / 2);
  const bool overlay = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Overlay).supported();
  int dstW = bitmap.getWidth();
  int dstH = bitmap.getHeight();
  bool grayCover = bitmap.hasGreyscale() && overlay;
  if (grayCover) fittedCoverSize(bitmap.getWidth(), bitmap.getHeight(), maxW, kThumbHeight, dstW, dstH);
  coverPixelWidth = dstW;
  coverPixelHeight = dstH;

  CoverScratch scratch;
  if (grayCover && !prepareCoverScratch(bitmap, dstW, scratch)) grayCover = false;
  // 灰阶失败时退回占位，避免把 2 位封面按黑白阈值画成一块黑。
  if (!grayCover) {
    coverPixelWidth = 0;
    coverPixelHeight = 0;
    present(nullptr, false, 0, 0, nullptr);
    return;
  }
  present(&bitmap, true, dstW, dstH, &scratch);
}
