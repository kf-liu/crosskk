#include "KkShelfActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <WeReadStore.h>
#include <Xtc.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "activities/apps/weread/WeReadTouchGeometry.h"
#include "components/UITheme.h"
#include "components/themes/inx/InxTheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr int kPortraitColumns = 3;
constexpr int kPortraitRows = 3;
constexpr int kLandscapeColumns = 5;
constexpr int kLandscapeRows = 2;
constexpr unsigned long kPageHoldMs = 700;
constexpr int kFilterCount = 3;

struct Slot {
  bool weread = false;
  uint32_t sourceIndex = 0;
};

int wereadItems(const uint32_t count) { return static_cast<int>(std::min<uint32_t>(count, INT32_MAX)); }

Slot slotFor(const KkShelfActivity::Filter filter, const int weread, const int item) {
  switch (filter) {
    case KkShelfActivity::Filter::WeRead:
      return {true, static_cast<uint32_t>(item)};
    case KkShelfActivity::Filter::Local:
      return {false, static_cast<uint32_t>(item)};
    case KkShelfActivity::Filter::All:
      if (item < weread) return {true, static_cast<uint32_t>(item)};
      return {false, static_cast<uint32_t>(item - weread)};
  }
  return {};
}

WeReadShelfGridLayout shelfGridLayout(const GfxRenderer& renderer, const Rect& content, const int sidePadding,
                                      const int spacing) {
  WeReadShelfGridLayout layout;
  const int titleHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int minimumGap = std::max(4, spacing / 2);
  const bool landscape = renderer.getOrientation() == GfxRenderer::Orientation::LandscapeClockwise ||
                         renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise;
  layout.columns = landscape ? kLandscapeColumns : kPortraitColumns;
  layout.rows = landscape ? kLandscapeRows : kPortraitRows;
  layout.itemsPerPage = layout.columns * layout.rows;
  layout.titleGap = minimumGap;
  layout.availableX = content.x + sidePadding;
  layout.availableWidth = std::max(1, content.width - sidePadding * 2);
  const int availableHeight = std::max(1, content.height);
  const int maxCoverWidth = std::max(1, (layout.availableWidth - minimumGap * (layout.columns + 1)) / layout.columns);
  const int maxCoverHeight =
      std::max(1, (availableHeight - minimumGap * (layout.rows + 1)) / layout.rows - layout.titleGap - titleHeight);
  const float scale = std::min({1.0f, static_cast<float>(maxCoverWidth) / WeReadStore::kCoverThumbWidth,
                                static_cast<float>(maxCoverHeight) / WeReadStore::kCoverThumbHeight});
  layout.coverWidth = std::max(1, static_cast<int>(WeReadStore::kCoverThumbWidth * scale));
  layout.coverHeight = std::max(1, static_cast<int>(WeReadStore::kCoverThumbHeight * scale));
  layout.itemHeight = layout.coverHeight + layout.titleGap + titleHeight;
  layout.columnGap = std::max(0, (layout.availableWidth - layout.columns * layout.coverWidth) / (layout.columns + 1));
  layout.rowGap = std::max(0, (availableHeight - layout.rows * layout.itemHeight) / (layout.rows + 1));
  return layout;
}

bool resolveHeightToken(const std::string& source, const int height, char* output, const size_t outputSize) {
  if (source.empty() || outputSize == 0) return false;
  constexpr const char token[] = "[HEIGHT]";
  const size_t tokenAt = source.find(token);
  if (tokenAt == std::string::npos) {
    if (source.size() >= outputSize) return false;
    memcpy(output, source.c_str(), source.size() + 1);
    return true;
  }
  char heightText[12];
  const int heightLength = snprintf(heightText, sizeof(heightText), "%d", height);
  const size_t suffixAt = tokenAt + sizeof(token) - 1;
  const size_t suffixLength = source.size() - suffixAt;
  if (heightLength <= 0 || tokenAt + static_cast<size_t>(heightLength) + suffixLength >= outputSize) return false;
  memcpy(output, source.data(), tokenAt);
  memcpy(output + tokenAt, heightText, static_cast<size_t>(heightLength));
  memcpy(output + tokenAt + heightLength, source.data() + suffixAt, suffixLength);
  output[tokenAt + heightLength + suffixLength] = '\0';
  return true;
}

bool coverFileReady(const char* path) {
  if (!path || path[0] == '\0' || !Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("KK", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;
  const auto rowBytes = static_cast<uint32_t>(bitmap.getRowBytes());
  const auto rows = static_cast<uint32_t>(bitmap.getHeight());
  return rowBytes > 0 && file.fileSize() / rowBytes >= rows;
}

bool rememberReady(const std::string& candidate, char* output, const size_t outputSize) {
  if (candidate.empty() || candidate.size() >= outputSize || !coverFileReady(candidate.c_str())) return false;
  memcpy(output, candidate.c_str(), candidate.size() + 1);
  return true;
}

bool readyWereadCover(const char* bookId, char* output, const size_t outputSize) {
  if (!bookId || bookId[0] == '\0') return false;
  return rememberReady(WeReadStore::coverPath(WeReadStore::bookDirectory(bookId)), output, outputSize);
}

bool readyLocalCover(const std::string& bookPath, const std::string& statsCover, const int preferredHeight,
                     char* output, const size_t outputSize) {
  const int heights[] = {preferredHeight, InxMetrics::values.homeCoverHeight, WeReadStore::kCoverThumbHeight};
  char tokenPath[320];
  for (const int height : heights) {
    if (height <= 0 || !resolveHeightToken(statsCover, height, tokenPath, sizeof(tokenPath))) continue;
    if (rememberReady(tokenPath, output, outputSize)) return true;
  }
  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    for (const int height : heights) {
      if (height > 0 && rememberReady(epub.getThumbBmpPath(height), output, outputSize)) return true;
    }
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    for (const int height : heights) {
      if (height > 0 && rememberReady(xtc.getThumbBmpPath(height), output, outputSize)) return true;
    }
  }
  return false;
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

struct CoverScratch {
  uint8_t* bytes = nullptr;
  int packedBytes = 0;
  int rawBytes = 0;
  int accumOffset = 0;
  int dstW = 0;
};

size_t coverScratchBytes(const Bitmap& bitmap, const int dstW) {
  const int packedBytes = (bitmap.getWidth() + 3) / 4;
  const int rawBytes = bitmap.getRowBytes();
  if (packedBytes <= 0 || rawBytes <= 0 || dstW <= 0) return 0;
  const size_t rowBytes = static_cast<size_t>(packedBytes) + static_cast<size_t>(rawBytes);
  const size_t accumOffset = (rowBytes + 3u) & ~size_t{3};
  const size_t bytes = accumOffset + static_cast<size_t>(dstW) * sizeof(uint32_t) * 2;
  if (bytes > 16384) return 0;
  return bytes;
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

// 按目标格子平均成 4 级，灰阶保持文件里的原值。
bool paintAveragedCover(const GfxRenderer& renderer, const Bitmap& bitmap, const int originX, const int originY,
                        const int dstW, const int dstH, CoverScratch& scratch) {
  if (dstW <= 0 || dstH <= 0 || scratch.dstW != dstW || !scratch.bytes) return false;
  if (bitmap.rewindToData() != BmpReaderError::Ok) return false;
  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  if (srcW <= 0 || srcH <= 0) return false;

  uint8_t* const packed = scratch.bytes;
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

int filterIndexFromPoint(const Rect& rect, const int x, const int y) {
  if (rect.width <= 0 || x < rect.x || x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height) return -1;
  return std::min(kFilterCount - 1, (x - rect.x) * kFilterCount / rect.width);
}

void drawFilterBar(const GfxRenderer& renderer, const Rect& rect, const KkShelfActivity::Filter filter) {
  const char* labels[kFilterCount] = {tr(STR_KK_SHELF_ALL), tr(STR_WEREAD_TITLE), tr(STR_KK_SHELF_LOCAL)};
  for (int segment = 0; segment < kFilterCount; ++segment) {
    const int left = rect.x + rect.width * segment / kFilterCount;
    const int right = rect.x + rect.width * (segment + 1) / kFilterCount;
    const bool active = static_cast<int>(filter) == segment;
    if (active) renderer.fillRect(left, rect.y, right - left, std::max(1, rect.height - 1), true);
    const auto style = active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[segment], style);
    const int textX = left + std::max(0, (right - left - textWidth) / 2);
    const GfxRenderer::ClipScope clip(renderer, left, rect.y, std::max(1, right - left), std::max(1, rect.height - 1));
    const int textY = rect.y + std::max(0, (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2);
    renderer.drawText(UI_10_FONT_ID, textX, textY, labels[segment], !active, style);
  }
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

// 已下载的封面原图转成可显示的位图。没有原图的书不在这里处理。
bool convertDownloadedCover(const char* bookId) {
  const std::string dir = WeReadStore::bookDirectory(bookId);
  const std::string jpg = dir + "/cover.source.jpg";
  const std::string png = dir + "/cover.source.png";
  const bool jpeg = Storage.exists(jpg.c_str());
  const bool pngSource = !jpeg && Storage.exists(png.c_str());
  if (!jpeg && !pngSource) return false;

  const std::string finalPath = WeReadStore::coverPath(dir);
  const std::string part = finalPath + ".part";
  if (Storage.exists(part.c_str())) Storage.remove(part.c_str());
  bool converted = false;
  {
    HalFile input;
    HalFile output;
    const std::string& source = jpeg ? jpg : png;
    if (Storage.openFileForRead("KK", source, input) && Storage.openFileForWrite("KK", part, output)) {
      converted = jpeg ? JpegToBmpConverter::jpegFileToBmpStreamWithSize(
                             input, output, WeReadStore::kCoverThumbWidth, WeReadStore::kCoverThumbHeight)
                       : PngToBmpConverter::pngFileToBmpStreamWithSize(input, output, WeReadStore::kCoverThumbWidth,
                                                                       WeReadStore::kCoverThumbHeight);
      output.flush();
    }
  }
  if (!converted || !coverFileReady(part.c_str()) || !WeReadStore::atomicReplace(part, finalPath)) {
    if (Storage.exists(part.c_str())) Storage.remove(part.c_str());
    LOG_ERR("KK", "Downloaded cover is not displayable: %s", bookId);
    return false;
  }
  return true;
}
}  // namespace

void KkShelfActivity::onEnter() {
  Activity::onEnter();
  titleScratch.reserve(192);
  pathScratch.reserve(256);
  wereadCount = 0;
  localCount = 0;
  selected = 0;
  filter = Filter::All;
  uncachedNotice = false;
  moveGesture = MoveGesture::Idle;
  coverPreparedPage = -1;
  if (shelfFile.isOpen()) shelfFile.close();
  if (!WeReadStore::openShelf(shelfFile, wereadCount)) {
    if (shelfFile.isOpen()) shelfFile.close();
    wereadCount = 0;
    LOG_DBG("KK", "WeRead shelf is not available");
  }
  if (!libraryIndex.open(library::libraryIndexPath())) {
    LOG_DBG("KK", "library index is not available");
  } else {
    localCount = libraryIndex.bookCount();
  }
  requestUpdate();
}

void KkShelfActivity::onExit() {
  if (shelfFile.isOpen()) shelfFile.close();
  if (libraryIndex.isOpen()) libraryIndex.close();
  coverRow.reset();
  coverRowBytes = 0;
  coverPaintCount = 0;
  coverPreparedPage = -1;
  std::string().swap(titleScratch);
  std::string().swap(pathScratch);
  Activity::onExit();
}

int KkShelfActivity::itemCount() const {
  const int weread = wereadItems(wereadCount);
  const int local = static_cast<int>(localCount);
  switch (filter) {
    case Filter::WeRead:
      return weread;
    case Filter::Local:
      return local;
    case Filter::All:
      return weread > INT32_MAX - local ? INT32_MAX : weread + local;
  }
  return 0;
}

void KkShelfActivity::setFilter(const Filter next) {
  if (next == filter) return;
  filter = next;
  selected = 0;
  uncachedNotice = false;
  moveGesture = MoveGesture::Idle;
  coverPreparedPage = -1;
  requestUpdate();
}

void KkShelfActivity::stepFilter(const int direction) {
  const int nextFilter = (static_cast<int>(filter) + (direction < 0 ? kFilterCount - 1 : 1)) % kFilterCount;
  setFilter(static_cast<Filter>(nextFilter));
}

void KkShelfActivity::moveSelection(const int item) {
  const int count = itemCount();
  if (count <= 0) return;
  const int next = std::clamp(item, 0, count - 1);
  if (next == selected) return;
  selected = next;
  requestUpdate();
}

void KkShelfActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  selected = MainTabs::contentEdgeIndex(edge, itemCount());
}

Rect KkShelfActivity::filterRect(const Rect& content) const {
  const int height = std::min(content.height, UITheme::getInstance().getMetrics().tabBarHeight);
  return Rect{content.x, content.y, content.width, std::max(0, height)};
}

Rect KkShelfActivity::gridRect(const Rect& content) const {
  const Rect filters = filterRect(content);
  constexpr int gap = 8;
  const int top = filters.y + filters.height + gap;
  return Rect{content.x, top, content.width, std::max(0, content.y + content.height - top)};
}

void KkShelfActivity::openSelected() {
  const int count = itemCount();
  if (count <= 0 || selected < 0 || selected >= count) return;
  const Slot slot = slotFor(filter, wereadItems(wereadCount), selected);
  std::string path;
  bool missingWeread = false;
  {
    RenderLock lock(*this);
    if (slot.weread) {
      WeReadStore::ShelfRecord book;
      if (!shelfFile.isOpen() || !WeReadStore::readShelfRecord(shelfFile, slot.sourceIndex, book)) return;
      path = WeReadStore::finalBookPath(book);
      missingWeread = path.empty() || !Storage.exists(path.c_str());
    } else if (libraryIndex.isOpen()) {
      const uint16_t ordinal =
          libraryIndex.ordinalForRow(library::SortOrder::RecentDesc, static_cast<uint16_t>(slot.sourceIndex));
      library::ClixRecord record;
      if (ordinal == 0xFFFF || !libraryIndex.readRecord(ordinal, record) || !libraryIndex.readPath(record, path)) {
        return;
      }
    }
  }
  if (missingWeread) {
    uncachedNotice = true;
    requestUpdate();
    return;
  }
  if (!path.empty()) activityManager.goToReader(std::move(path));
}

void KkShelfActivity::loop() {
  if (uncachedNotice) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      uncachedNotice = false;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    stepFilter(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    stepFilter(1);
    return;
  }

  const int count = itemCount();
  const Rect content = pageContentRect();
  const Rect grid = gridRect(content);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = shelfGridLayout(renderer, grid, metrics.contentSidePadding, metrics.verticalSpacing);
  const int perPage = std::max(1, layout.itemsPerPage);

  switch (moveGesture) {
    case MoveGesture::Idle:
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        moveGesture = MoveGesture::Previous;
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        moveGesture = MoveGesture::Next;
      } else {
        break;
      }
      [[fallthrough]];
    case MoveGesture::Previous:
    case MoveGesture::Next:
    case MoveGesture::PreviousPage:
    case MoveGesture::NextPage: {
      const bool previous = moveGesture == MoveGesture::Previous || moveGesture == MoveGesture::PreviousPage;
      const bool pageHandled = moveGesture == MoveGesture::PreviousPage || moveGesture == MoveGesture::NextPage;
      const auto button = previous ? MappedInputManager::Button::Up : MappedInputManager::Button::Down;
      if (mappedInput.wasReleased(button)) {
        moveGesture = MoveGesture::Idle;
        lastPageTurnAt = 0;
        if (!pageHandled && count > 0) {
          moveSelection(previous ? ButtonNavigator::previousIndex(selected, count)
                                 : ButtonNavigator::nextIndex(selected, count));
        }
        return;
      }
      if (!mappedInput.isPressed(button)) {
        moveGesture = MoveGesture::Idle;
        lastPageTurnAt = 0;
        return;
      }
      const uint32_t now = millis();
      if ((!pageHandled && mappedInput.getHeldTime() < kPageHoldMs) ||
          (pageHandled && now - lastPageTurnAt < kPageHoldMs)) {
        return;
      }
      moveGesture = previous ? MoveGesture::PreviousPage : MoveGesture::NextPage;
      lastPageTurnAt = now;
      if (count > perPage) {
        moveSelection(previous ? ButtonNavigator::previousPageIndex(selected, count, perPage)
                               : ButtonNavigator::nextPageIndex(selected, count, perPage));
      }
      return;
    }
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    const int filterIndex = filterIndexFromPoint(filterRect(content), x, y);
    if (filterIndex >= 0) {
      setFilter(static_cast<Filter>(filterIndex));
      return;
    }
    const int touched = weReadShelfIndexFromPoint(grid, layout, selected, count, x, y);
    if (touched >= 0) {
      moveSelection(touched);
      openSelected();
    }
    return;
  }
  if (mappedInput.wasScreenTouchDown(x, y)) {
    const int touched = weReadShelfIndexFromPoint(grid, layout, selected, count, x, y);
    if (touched >= 0) moveSelection(touched);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (count > perPage && swipe == MappedInputManager::SwipeDir::Up) {
    moveSelection(ButtonNavigator::nextPageIndex(selected, count, perPage));
    return;
  }
  if (count > perPage && swipe == MappedInputManager::SwipeDir::Down) {
    moveSelection(ButtonNavigator::previousPageIndex(selected, count, perPage));
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelected();
}

bool KkShelfActivity::ensureCoverRow(const size_t bytes) {
  if (bytes == 0 || bytes > 16384) return false;
  if (coverRow && coverRowBytes >= bytes) return true;
  auto next = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!next) {
    LOG_ERR("KK", "OOM: shelf cover row (%u)", static_cast<unsigned>(bytes));
    return false;
  }
  coverRow = std::move(next);
  coverRowBytes = bytes;
  return true;
}

bool KkShelfActivity::drawShelfCover(const char* path, const Rect& bounds, const bool allowGray,
                                    const bool recordGray) {
  if (!path || path[0] == '\0' || bounds.width <= 0 || bounds.height <= 0) return false;
  HalFile file;
  if (!Storage.openFileForRead("KK", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) return false;

  const bool gray = allowGray && bitmap.hasGreyscale();
  if (!gray) {
    if (renderer.getRenderMode() != GfxRenderer::BW) return true;
    const float scale = std::min({1.0f, static_cast<float>(bounds.width) / bitmap.getWidth(),
                                  static_cast<float>(bounds.height) / bitmap.getHeight()});
    const int width = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
    const int height = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
    return renderer.drawBitmap(bitmap, bounds.x + (bounds.width - width) / 2,
                              bounds.y + (bounds.height - height) / 2, bounds.width, bounds.height);
  }

  int dstW = 0;
  int dstH = 0;
  fittedCoverSize(bitmap.getWidth(), bitmap.getHeight(), bounds.width, bounds.height, dstW, dstH);
  const size_t bytes = coverScratchBytes(bitmap, dstW);
  if (bytes == 0 || !ensureCoverRow(bytes)) return false;
  CoverScratch scratch;
  scratch.bytes = coverRow.get();
  scratch.packedBytes = (bitmap.getWidth() + 3) / 4;
  scratch.rawBytes = bitmap.getRowBytes();
  const size_t rowBytes = static_cast<size_t>(scratch.packedBytes) + static_cast<size_t>(scratch.rawBytes);
  scratch.accumOffset = static_cast<int>((rowBytes + 3u) & ~size_t{3});
  scratch.dstW = dstW;
  const int originX = bounds.x + std::max(0, (bounds.width - dstW) / 2);
  const int originY = bounds.y + std::max(0, (bounds.height - dstH) / 2);
  if (!paintAveragedCover(renderer, bitmap, originX, originY, dstW, dstH, scratch)) return false;
  if (!recordGray || coverPaintCount >= kCoverSlots) return true;
  auto& job = coverPaints[coverPaintCount];
  const size_t length = strlen(path);
  if (length >= sizeof(job.path)) return true;
  memcpy(job.path, path, length + 1);
  job.x = originX;
  job.y = originY;
  job.dstW = dstW;
  job.dstH = dstH;
  ++coverPaintCount;
  return true;
}

void KkShelfActivity::prepareDownloadedCovers(const int pageStart, const int pageEnd) {
  if (coverPreparedPage == pageStart || !shelfFile.isOpen() || pageEnd <= pageStart) return;
  coverPreparedPage = pageStart;

  char ids[kCoverSlots][64] = {};
  int pending = 0;
  const int weread = wereadItems(wereadCount);
  for (int item = pageStart; item < pageEnd && pending < kCoverSlots; ++item) {
    const Slot slot = slotFor(filter, weread, item);
    if (!slot.weread) continue;
    WeReadStore::ShelfRecord book;
    if (!WeReadStore::readShelfRecord(shelfFile, slot.sourceIndex, book)) continue;
    char path[192];
    if (readyWereadCover(book.bookId, path, sizeof(path))) continue;
    const std::string dir = WeReadStore::bookDirectory(book.bookId);
    const bool hasSource = Storage.exists((dir + "/cover.source.jpg").c_str()) ||
                           Storage.exists((dir + "/cover.source.png").c_str());
    if (!hasSource || book.bookId[0] == '\0') continue;
    memcpy(ids[pending], book.bookId, sizeof(ids[pending]) - 1);
    ++pending;
  }
  if (pending == 0) return;

  GfxRenderer::FrameBufferLoan loan(renderer);
  for (int index = 0; index < pending; ++index) convertDownloadedCover(ids[index]);
}

void KkShelfActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  const Rect filters = filterRect(content);
  const Rect grid = gridRect(content);
  const int count = itemCount();
  if (count > 0 && selected >= count) selected = count - 1;
  if (count <= 0) selected = 0;

  const bool haveGrid = grid.width > 0 && grid.height > 0 && count > 0;
  WeReadShelfGridLayout layout;
  int pageStart = 0;
  int pageEnd = 0;
  if (haveGrid) {
    layout = shelfGridLayout(renderer, grid, metrics.contentSidePadding, metrics.verticalSpacing);
    const int perPage = std::max(1, layout.itemsPerPage);
    pageStart = selected / perPage * perPage;
    pageEnd = std::min(pageStart + perPage, count);
    prepareDownloadedCovers(pageStart, pageEnd);
  }
  const bool allowGray = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Overlay).supported();
  coverPaintCount = 0;

  const auto drawFrame = [&](const bool recordCovers) {
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_KK_TAB_SHELF));
    if (filters.height > 0) drawFilterBar(renderer, filters, filter);
    if (grid.height > 0 && count <= 0) {
      UITheme::drawCenteredWrappedText(renderer, grid, UI_12_FONT_ID, tr(STR_KK_SHELF_EMPTY), 2);
    } else if (haveGrid) {
      const bool showSelection = showMainTabContentSelection();
      const int weread = wereadItems(wereadCount);
      for (int item = pageStart; item < pageEnd; ++item) {
        const auto geometry = weReadShelfItemGeometry(grid, layout, pageStart, pageEnd, item);
        const Rect cover = geometry.cover;
        const Rect hit = geometry.hit;
        const bool focused = showSelection && item == selected;
        bool foregroundBlack = true;
        if (focused) {
          foregroundBlack = GUI.drawSelectionBackground(renderer, hit);
          renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
        }

        const Slot slot = slotFor(filter, weread, item);
        titleScratch.clear();
        char coverPath[320];
        coverPath[0] = '\0';
        if (slot.weread && shelfFile.isOpen()) {
          WeReadStore::ShelfRecord book;
          if (WeReadStore::readShelfRecord(shelfFile, slot.sourceIndex, book)) {
            titleScratch = book.title;
            readyWereadCover(book.bookId, coverPath, sizeof(coverPath));
          }
        } else if (!slot.weread && libraryIndex.isOpen()) {
          const uint16_t ordinal =
              libraryIndex.ordinalForRow(library::SortOrder::RecentDesc, static_cast<uint16_t>(slot.sourceIndex));
          library::ClixRecord record;
          if (ordinal != 0xFFFF && libraryIndex.readRecord(ordinal, record)) {
            libraryIndex.readTitle(record, titleScratch);
            if (libraryIndex.readPath(record, pathScratch)) {
              const ReadingBookStats* stats = READING_STATS.findBook(pathScratch);
              static const std::string kNoCover;
              readyLocalCover(pathScratch, stats ? stats->coverBmpPath : kNoCover, cover.height, coverPath,
                              sizeof(coverPath));
            }
          }
        }

        if (coverPath[0] != '\0') drawShelfCover(coverPath, cover, allowGray, recordCovers);
        renderer.drawRect(cover.x, cover.y, cover.width, cover.height);
        const std::string shown = renderer.truncatedText(SMALL_FONT_ID, titleScratch.c_str(), layout.coverWidth);
        const int titleWidth = renderer.getTextAdvanceX(SMALL_FONT_ID, shown.c_str(), EpdFontFamily::REGULAR);
        renderer.drawText(SMALL_FONT_ID, cover.x + std::max(0, (cover.width - titleWidth) / 2),
                          cover.y + cover.height + layout.titleGap, shown.c_str(), foregroundBlack);
      }
      GUI.drawSideScrollBar(renderer, grid, count, pageStart, layout.itemsPerPage);
    }

    const auto hints = mainTabButtonLabels(tr(STR_BACK), count > 0 ? tr(STR_OPEN) : "", count > 1);
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
    if (uncachedNotice) GUI.drawPopup(renderer, tr(STR_WEREAD_NOT_CACHED));
  };

  drawFrame(true);
  if (coverPaintCount == 0) {
    renderer.displayBuffer();
    return;
  }
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  bool planesReady = true;
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(mode);
    for (int index = 0; index < coverPaintCount; ++index) {
      const auto& job = coverPaints[index];
      if (!drawShelfCover(job.path, Rect{job.x, job.y, job.dstW, job.dstH}, true, false)) {
        planesReady = false;
        break;
      }
    }
    if (!planesReady) break;
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  if (!planesReady) {
    drawFrame(false);
    renderer.displayBuffer();
    return;
  }
  renderer.displayGrayBuffer();
  drawFrame(false);
  renderer.cleanupGrayscaleWithFrameBuffer();
}
