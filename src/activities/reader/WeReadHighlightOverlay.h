#pragma once

#include <cstddef>
#include <cstdint>

class Epub;
class GfxRenderer;
class Page;

namespace WeReadHighlightOverlay {

// Draws solid underlines for the current user's WeRead highlights that intersect this page.
// The chapter index is read from SD once per page; grayscale passes reuse that result.
void drawForBook(GfxRenderer& renderer, int fontId, const Page& page, int xOffset, int yOffset, const char* bookId,
                 const Epub& epub, int spineIndex);

// 划线索引更新后丢掉按页缓存，下一帧重新读 SD。
void invalidate();

// 点在当前页某条划线的文字上时，写出它的 bookmarkId。
bool hitBookmark(int x, int y, char* bookmarkId, size_t bookmarkIdSize);

enum class WordPlace : uint8_t { AtPoint, FirstOnPage, LastOnPage };

// 当前页某个字的可见偏移。AtPoint 用 x、y；页首和页尾忽略坐标。
bool wordOnPage(GfxRenderer& renderer, int fontId, const Page& page, int xOffset, int yOffset, const char* bookId,
                const Epub& epub, int spineIndex, WordPlace place, int x, int y, uint32_t& visibleStart,
                uint16_t& span);

// 把正在选择的可见区间画成下划线。区间外的字不画。
void drawSelection(GfxRenderer& renderer, int fontId, const Page& page, int xOffset, int yOffset, const char* bookId,
                   const Epub& epub, int spineIndex, uint32_t visibleStart, uint32_t visibleEnd);

// 这一章的 xhtml 路径、chapterUid 和 chapterIdx。路径写不进缓冲时失败。
bool chapterFile(const char* bookId, const Epub& epub, int spineIndex, char* path, size_t pathSize,
                 uint32_t& chapterUid, uint32_t& chapterIdx);

}  // namespace WeReadHighlightOverlay
