#include "WeReadHighlightOverlay.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstring>

#include "WeReadHighlight.h"
#include "WeReadStore.h"

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

struct PageMarks {
  char bookId[64] = {};
  int spineIndex = -1;
  uint32_t pageOffset = 0;
  size_t count = 0;
  bool valid = false;
  WeReadHighlight::Range ranges[WeReadHighlight::kMaxChapterRanges] = {};
};

PageMarks& pageMarks() {
  // One reader render task. Reused across grayscale passes so a page does not
  // re-read the highlight index for every plane. 64 ranges are 512 bytes.
  static PageMarks marks;
  return marks;
}

bool samePage(const PageMarks& marks, const char* bookId, const int spineIndex, const uint32_t pageOffset) {
  return marks.valid && marks.spineIndex == spineIndex && marks.pageOffset == pageOffset &&
         strncmp(marks.bookId, bookId, sizeof(marks.bookId)) == 0;
}

void loadPageMarks(const char* bookId, const Epub& epub, const int spineIndex, const uint32_t pageOffset) {
  PageMarks& marks = pageMarks();
  marks.valid = false;
  marks.count = 0;
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
  if (!WeReadHighlight::loadChapter(bookId, chapterUid, marks.ranges, WeReadHighlight::kMaxChapterRanges, marks.count,
                                    truncated)) {
    marks.count = 0;
    return;
  }
  if (truncated) {
    LOG_DBG("WR", "Highlight overlay kept %u ranges for chapter %u", static_cast<unsigned>(marks.count), chapterUid);
  }
  marks.valid = true;
}

void drawRanges(GfxRenderer& renderer, const int fontId, const Page& page, const int xOffset, const int yOffset,
                const WeReadHighlight::Range* ranges, const size_t rangeCount) {
  const int ascender = renderer.getFontAscenderSize(fontId);
  uint32_t offset = page.visibleTextOffset;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const TextBlock& block = *line.getBlock();
    if (!block.valid()) continue;
    const int lineY = line.yPos + yOffset + block.getRubyShift(ascender) + ascender + 2;
    const int8_t tracking = block.getBlockStyle().characterSpacing;
    for (uint16_t wordIndex = 0; wordIndex < block.wordCount(); ++wordIndex) {
      const char* word = block.wordText(wordIndex);
      const uint32_t length = visibleCodepoints(word);
      const uint32_t wordEnd = offset + length;
      bool covered = false;
      for (size_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
        if (WeReadHighlight::rangesOverlap(offset, wordEnd, ranges[rangeIndex].start, ranges[rangeIndex].end)) {
          covered = true;
          break;
        }
      }
      if (covered && length > 0) {
        const int width = renderer.getTextAdvanceX(fontId, word, block.wordStyle(wordIndex), tracking);
        if (width > 0) {
          const int x = line.xPos + xOffset + block.wordXpos(wordIndex);
          renderer.drawLine(x, lineY, x + width - 1, lineY, 1, true);
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
  if (!samePage(marks, bookId, spineIndex, page.visibleTextOffset)) {
    loadPageMarks(bookId, epub, spineIndex, page.visibleTextOffset);
  }
  if (!marks.valid || marks.count == 0) return;
  drawRanges(renderer, fontId, page, xOffset, yOffset, marks.ranges, marks.count);
}

void invalidate() { pageMarks().valid = false; }

}  // namespace WeReadHighlightOverlay
