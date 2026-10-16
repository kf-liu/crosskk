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

uint32_t countCodepoints(const char* text) {
  if (!text) return 0;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  uint32_t count = 0;
  while (*cursor) {
    if (utf8NextCodepoint(&cursor) == 0) break;
    ++count;
  }
  return count;
}

bool copyCodepoints(const char* text, const uint32_t skip, const uint32_t take, char* dest, const size_t destSize) {
  if (!text || !dest || destSize == 0) return false;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  for (uint32_t i = 0; i < skip; ++i) {
    if (!*cursor || utf8NextCodepoint(&cursor) == 0) return false;
  }
  const unsigned char* begin = cursor;
  for (uint32_t i = 0; i < take; ++i) {
    if (!*cursor || utf8NextCodepoint(&cursor) == 0) return false;
  }
  const size_t bytes = static_cast<size_t>(cursor - begin);
  if (bytes + 1 > destSize) return false;
  memcpy(dest, begin, bytes);
  dest[bytes] = '\0';
  return true;
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
  char chapterPath[128] = {};
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
  if (path.size() >= sizeof(marks.chapterPath)) {
    marks.chapterPath[0] = '\0';
  } else {
    memcpy(marks.chapterPath, path.c_str(), path.size() + 1);
  }
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
  marks.chapterPath[0] = '\0';
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

struct DrawWalk {
  GfxRenderer* renderer = nullptr;
  PageMarks* marks = nullptr;
  const Page* page = nullptr;
  int fontId = 0;
  int xOffset = 0;
  int yOffset = 0;
  int ascender = 0;
  size_t elementIndex = 0;
  uint16_t wordIndex = 0;
  const PageLine* line = nullptr;
  const TextBlock* block = nullptr;
  const char* word = nullptr;
  bool accepted = false;
  uint32_t cursor = 0;
};

void paintWord(DrawWalk& walk, const uint16_t wordIndex, const uint32_t start, const uint32_t span) {
  if (!walk.renderer || !walk.marks || !walk.line || !walk.block || !walk.word || span == 0) return;
  const uint32_t wordEnd = start + span;
  int coveredIndex = -1;
  uint32_t overlapStart = start;
  uint32_t overlapEnd = wordEnd;
  for (size_t rangeIndex = 0; rangeIndex < walk.marks->count; ++rangeIndex) {
    const WeReadHighlight::ChapterMark& mark = walk.marks->marks[rangeIndex];
    if (!WeReadHighlight::rangesOverlap(start, wordEnd, mark.start, mark.end)) continue;
    coveredIndex = static_cast<int>(rangeIndex);
    overlapStart = start > mark.start ? start : mark.start;
    overlapEnd = wordEnd < mark.end ? wordEnd : mark.end;
    break;
  }
  if (coveredIndex < 0 || overlapEnd <= overlapStart) return;

  const int8_t tracking = walk.block->getBlockStyle().characterSpacing;
  const EpdFontFamily::Style style = walk.block->wordStyle(wordIndex);
  int prefixWidth = 0;
  int sliceWidth = 0;
  const bool wholeWord = overlapStart == start && overlapEnd == wordEnd;
  if (wholeWord || countCodepoints(walk.word) != span) {
    sliceWidth = walk.renderer->getTextAdvanceX(walk.fontId, walk.word, style, tracking);
  } else {
    // One reader render task. Two slice buffers stay off the 8KB stack.
    static char prefix[208];
    static char covered[208];
    const uint32_t skip = overlapStart - start;
    const uint32_t take = overlapEnd - start;
    if (!copyCodepoints(walk.word, 0, skip, prefix, sizeof(prefix)) ||
        !copyCodepoints(walk.word, 0, take, covered, sizeof(covered))) {
      sliceWidth = walk.renderer->getTextAdvanceX(walk.fontId, walk.word, style, tracking);
    } else {
      prefixWidth = prefix[0] ? walk.renderer->getTextAdvanceX(walk.fontId, prefix, style, tracking) : 0;
      const int coveredWidth = walk.renderer->getTextAdvanceX(walk.fontId, covered, style, tracking);
      sliceWidth = coveredWidth - prefixWidth;
    }
  }
  if (sliceWidth <= 0) return;

  const int lineY = walk.line->yPos + walk.yOffset + walk.block->getRubyShift(walk.ascender) + walk.ascender + 2;
  const int top = walk.line->yPos + walk.yOffset;
  const int x = walk.line->xPos + walk.xOffset + walk.block->wordXpos(wordIndex) + prefixWidth;
  walk.renderer->drawLine(x, lineY, x + sliceWidth - 1, lineY, 1, true);
  if (walk.marks->segmentCount < kMaxHitSegments && top < lineY + 4) {
    HitSegment& segment = walk.marks->segments[walk.marks->segmentCount++];
    segment.markIndex = static_cast<uint8_t>(coveredIndex);
    segment.x = static_cast<int16_t>(x);
    segment.y = static_cast<int16_t>(top);
    segment.w = static_cast<int16_t>(sliceWidth);
    segment.h = static_cast<int16_t>(lineY + 4 - top);
  }
}

const char* pullWord(void* raw) {
  auto* walk = static_cast<DrawWalk*>(raw);
  if (!walk || !walk->page) return nullptr;
  walk->accepted = false;
  while (walk->elementIndex < walk->page->elements.size()) {
    const auto& element = walk->page->elements[walk->elementIndex];
    if (!element || element->getTag() != TAG_PageLine) {
      ++walk->elementIndex;
      walk->wordIndex = 0;
      continue;
    }
    walk->line = static_cast<const PageLine*>(element.get());
    walk->block = walk->line->getBlock().get();
    if (!walk->block || !walk->block->valid() || walk->wordIndex >= walk->block->wordCount()) {
      ++walk->elementIndex;
      walk->wordIndex = 0;
      walk->line = nullptr;
      walk->block = nullptr;
      continue;
    }
    walk->word = walk->block->wordText(walk->wordIndex);
    ++walk->wordIndex;
    return walk->word;
  }
  walk->word = nullptr;
  walk->line = nullptr;
  walk->block = nullptr;
  return nullptr;
}

void acceptWord(const uint32_t start, const uint16_t span, void* raw) {
  auto* walk = static_cast<DrawWalk*>(raw);
  if (!walk || walk->wordIndex == 0) return;
  walk->accepted = true;
  if (span == 0) return;
  walk->cursor = start + span;
  paintWord(*walk, static_cast<uint16_t>(walk->wordIndex - 1), start, span);
}

void paintFallback(DrawWalk& walk) {
  if (!walk.word || walk.wordIndex == 0) return;
  const uint32_t span = countCodepoints(walk.word);
  paintWord(walk, static_cast<uint16_t>(walk.wordIndex - 1), walk.cursor, span);
  walk.cursor += span;
  walk.accepted = true;
}

void drawRanges(GfxRenderer& renderer, const int fontId, const Page& page, const int xOffset, const int yOffset,
                PageMarks& marks) {
  marks.segmentCount = 0;
  DrawWalk walk;
  walk.renderer = &renderer;
  walk.marks = &marks;
  walk.page = &page;
  walk.fontId = fontId;
  walk.xOffset = xOffset;
  walk.yOffset = yOffset;
  walk.ascender = renderer.getFontAscenderSize(fontId);
  walk.cursor = page.visibleTextOffset;
  // Paragraph breaks are visible offsets but not stored in page words. Aligning
  // each word to the chapter keeps a highlight from sliding by those gaps.
  if (marks.chapterPath[0]) {
    WeReadXhtmlCodec::alignRenderedWords(marks.chapterPath, page.visibleTextOffset, pullWord, acceptWord, &walk);
  }
  if (walk.word && !walk.accepted) paintFallback(walk);
  while (pullWord(&walk)) paintFallback(walk);
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
