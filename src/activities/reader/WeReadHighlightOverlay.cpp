#include "WeReadHighlightOverlay.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdint>
#include <cstring>

#include "WeReadHighlight.h"
#include "WeReadStore.h"
#include "WeReadXhtmlCodec.h"

namespace WeReadHighlightOverlay {
namespace {

uint32_t visibleCodepoints(const char* text) {
  if (!text) return 0;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  uint32_t count = 0;
  bool leadingIndent = true;
  while (*cursor) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) break;
    if (leadingIndent && codepoint == 0x2003) continue;
    leadingIndent = false;
    if (codepoint == 0x00AD) continue;
    ++count;
  }
  return count;
}

struct HitSegment {
  uint8_t markIndex = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
};

constexpr size_t kMaxHitSegments = 96;

struct PageMarks {
  char bookId[64] = {};
  int spineIndex = -1;
  uint32_t pageOffset = 0;
  size_t count = 0;
  size_t segmentCount = 0;
  bool valid = false;
  WeReadHighlight::ChapterMark marks[WeReadHighlight::kMaxChapterRanges] = {};
  HitSegment segments[kMaxHitSegments] = {};
};

PageMarks& pageMarks() {
  // One reader render task. Reused across grayscale passes so a page does not
  // re-read the highlight index for every plane. 64 ranges are 512 bytes.
  static PageMarks marks;
  return marks;
}

bool sameChapter(const PageMarks& marks, const char* bookId, const int spineIndex) {
  return marks.valid && marks.spineIndex == spineIndex && strncmp(marks.bookId, bookId, sizeof(marks.bookId)) == 0;
}

// Bookmark ranges are wr-co offsets in the chapter file. Page text uses the smaller visible
// offset. Convert once per chapter; the buffers stay off the 8KB reader stack.
bool mapMarksToPageOffsets(const char* bookId, const uint32_t tocIndex, PageMarks& marks) {
  if (marks.count == 0) return true;
  static uint32_t nativeOffsets[WeReadHighlight::kMaxChapterRanges * 2];
  static uint32_t visibleOffsets[WeReadHighlight::kMaxChapterRanges * 2];
  const size_t count = marks.count;
  for (size_t i = 0; i < count; ++i) {
    nativeOffsets[i * 2] = marks.marks[i].start;
    nativeOffsets[i * 2 + 1] = marks.marks[i].end;
  }
  const std::string path = WeReadStore::chapterPath(WeReadStore::bookDirectory(bookId), tocIndex);
  if (!WeReadXhtmlCodec::nativeOffsetsToVisible(path, nativeOffsets, visibleOffsets, count * 2)) {
    LOG_ERR("WR", "Highlight ranges could not be mapped onto the chapter");
    return false;
  }
  size_t kept = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t start = visibleOffsets[i * 2];
    const uint32_t end = visibleOffsets[i * 2 + 1];
    if (start == UINT32_MAX || end == UINT32_MAX || end <= start) continue;
    marks.marks[kept] = marks.marks[i];
    marks.marks[kept].start = start;
    marks.marks[kept].end = end;
    ++kept;
  }
  marks.count = kept;
  return true;
}

void loadPageMarks(const char* bookId, const Epub& epub, const int spineIndex, const uint32_t pageOffset) {
  PageMarks& marks = pageMarks();
  marks.valid = false;
  marks.count = 0;
  marks.segmentCount = 0;
  marks.spineIndex = spineIndex;
  marks.pageOffset = pageOffset;
  marks.bookId[0] = '\0';
  strncpy(marks.bookId, bookId, sizeof(marks.bookId) - 1);
  if (spineIndex < 0 || spineIndex >= epub.getSpineItemsCount()) return;

  uint32_t tocIndex = 0;
  const std::string href = epub.getSpineItem(spineIndex).href;
  if (!WeReadStore::parseGeneratedChapterHref(href, tocIndex)) return;

  HalFile toc;
  uint32_t tocCount = 0;
  static WeReadStore::TocRecord chapter;
  memset(&chapter, 0, sizeof(chapter));
  const std::string tocFile = WeReadStore::tocPath(bookId);
  if (!WeReadStore::openToc(tocFile, toc, tocCount) || tocIndex >= tocCount ||
      !WeReadStore::readTocRecord(toc, tocIndex, chapter)) {
    return;
  }
  uint32_t chapterUid = 0;
  if (!WeReadHighlight::parseChapterUid(chapter.chapterUid, chapterUid)) return;

  bool truncated = false;
  if (!WeReadHighlight::loadChapterMarks(bookId, chapterUid, marks.marks, WeReadHighlight::kMaxChapterRanges,
                                         marks.count, truncated)) {
    marks.count = 0;
    return;
  }
  if (truncated) {
    LOG_DBG("WR", "Highlight overlay kept %u ranges for chapter %u", static_cast<unsigned>(marks.count), chapterUid);
  }
  if (!mapMarksToPageOffsets(bookId, tocIndex, marks)) {
    marks.count = 0;
    return;
  }
  marks.valid = true;
}

void drawRanges(GfxRenderer& renderer, const int fontId, const Page& page, const int xOffset, const int yOffset,
                PageMarks& marks) {
  marks.segmentCount = 0;
  const int ascender = renderer.getFontAscenderSize(fontId);
  uint32_t offset = page.visibleTextOffset;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const TextBlock& block = *line.getBlock();
    if (!block.valid()) continue;
    const int lineY = line.yPos + yOffset + block.getRubyShift(ascender) + ascender + 2;
    const int top = line.yPos + yOffset;
    const int8_t tracking = block.getBlockStyle().characterSpacing;
    for (uint16_t wordIndex = 0; wordIndex < block.wordCount(); ++wordIndex) {
      const char* word = block.wordText(wordIndex);
      const uint32_t length = visibleCodepoints(word);
      const uint32_t wordEnd = offset + length;
      int coveredIndex = -1;
      for (size_t rangeIndex = 0; rangeIndex < marks.count; ++rangeIndex) {
        if (WeReadHighlight::rangesOverlap(offset, wordEnd, marks.marks[rangeIndex].start,
                                           marks.marks[rangeIndex].end)) {
          coveredIndex = static_cast<int>(rangeIndex);
          break;
        }
      }
      if (coveredIndex >= 0 && length > 0) {
        const int width = renderer.getTextAdvanceX(fontId, word, block.wordStyle(wordIndex), tracking);
        if (width > 0) {
          const int x = line.xPos + xOffset + block.wordXpos(wordIndex);
          renderer.drawLine(x, lineY, x + width - 1, lineY, 1, true);
          if (marks.segmentCount < kMaxHitSegments && top < lineY + 4) {
            HitSegment& segment = marks.segments[marks.segmentCount++];
            segment.markIndex = static_cast<uint8_t>(coveredIndex);
            segment.x = static_cast<int16_t>(x);
            segment.y = static_cast<int16_t>(top);
            segment.w = static_cast<int16_t>(width);
            segment.h = static_cast<int16_t>(lineY + 4 - top);
          }
        }
      }
      offset = wordEnd;
    }
  }
}

}  // namespace

void drawForBook(GfxRenderer& renderer, const int fontId, const Page& page, const int xOffset, const int yOffset,
                 const char* bookId, const Epub& epub, const int spineIndex) {
  if (!bookId || !bookId[0]) return;
  PageMarks& marks = pageMarks();
  if (!sameChapter(marks, bookId, spineIndex)) {
    loadPageMarks(bookId, epub, spineIndex, page.visibleTextOffset);
  }
  if (!marks.valid || marks.count == 0) return;
  drawRanges(renderer, fontId, page, xOffset, yOffset, marks);
}

bool hitBookmark(const int x, const int y, char* bookmarkId, const size_t bookmarkIdSize) {
  if (!bookmarkId || bookmarkIdSize < 2) return false;
  bookmarkId[0] = '\0';
  const PageMarks& marks = pageMarks();
  if (!marks.valid) return false;
  for (size_t i = 0; i < marks.segmentCount; ++i) {
    const HitSegment& segment = marks.segments[i];
    if (segment.markIndex >= marks.count || segment.w <= 0 || segment.h <= 0) continue;
    if (x < segment.x || x >= segment.x + segment.w || y < segment.y || y >= segment.y + segment.h) continue;
    strncpy(bookmarkId, marks.marks[segment.markIndex].bookmarkId, bookmarkIdSize - 1);
    bookmarkId[bookmarkIdSize - 1] = '\0';
    return bookmarkId[0] != '\0';
  }
  return false;
}

void invalidate() { pageMarks().valid = false; }

}  // namespace WeReadHighlightOverlay
