#pragma once

#include <cstddef>

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

}  // namespace WeReadHighlightOverlay
