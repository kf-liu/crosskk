#pragma once

#include <cstddef>
#include <cstdint>

class Epub;
class GfxRenderer;
class MappedInputManager;
class Page;

namespace WeReadCommentOverlay {

// 进入一章时拉候选 range，只对和当前页相交、这一轮还没查过的 range 翻页拉评论。
// 返回 true 表示点线需要重画。不打开 Wi-Fi。
bool syncPage(const char* bookId, const Epub& epub, int spineIndex, int pageNumber, uint32_t pageStart,
              uint32_t pageEnd);

// 有他人评论的片段画点状下划线。坐标和划线相同，灰阶各遍复用这一页的结果。
void drawForBook(GfxRenderer& renderer, int fontId, const Page& page, int xOffset, int yOffset, const char* bookId,
                 const Epub& epub, int spineIndex);

bool openAt(int x, int y);
bool panelOpen();
// 面板打开时由阅读循环调用。返回 true 表示画面变了。
bool handlePanel(MappedInputManager& input, const GfxRenderer& renderer);
void drawPanel(GfxRenderer& renderer);

void reset();

}  // namespace WeReadCommentOverlay
