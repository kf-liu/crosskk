#pragma once

#include <cstddef>
#include <cstdint>

class Epub;
class GfxRenderer;
class MappedInputManager;
class Page;

namespace WeReadCommentOverlay {

// 把当前章登记给后台任务：索引过期时在后台拉一次「哪里有评论」。
// 评论正文不提前拉。点开弹层拉一页就显示，继续往下翻再拉下一页。
// 翻页不发网络。返回 true 表示点线需要重画。
bool syncPage(const char* bookId, const Epub& epub, int spineIndex, int pageNumber, uint32_t pageStart,
              uint32_t pageEnd);

// 手动重拉当前章。没有章节时是 Idle。成功前不替换已有评论。
enum class ManualRefresh : uint8_t { Idle, Running, Succeeded, Failed, LoginRequired, Offline };
ManualRefresh beginManualRefresh();
ManualRefresh manualRefreshState();
void cancelManualRefresh();

// 弹层的评论内容请求已排队或在途。心跳等其它后台网络让位，别和它抢。
bool panelFetchActive();
// 评论这条 TLS 还留着（弹层开着，或刚关上还没断开）。心跳不要再开一条。
bool commentConnectionHeld();

// 后台写入了新评论。阅读循环用来刷新画面，本身不发请求。
bool takeDirty();

// 弹层还在加载、后台又空了出来时补一次排队。不发网络，阅读循环每圈调用。
bool pumpPanel();

// 有他人评论的片段画点状下划线。坐标和划线相同，灰阶各遍复用这一页的结果。
void drawForBook(GfxRenderer& renderer, int fontId, const Page& page, int xOffset, int yOffset, const char* bookId,
                 const Epub& epub, int spineIndex);

// 阅读页底栏文案。这一页还在拉、加载失败，或本章候选被截断时返回句子，否则 nullptr。
const char* chapterStatus();

bool openAt(int x, int y);
// 划线的可见区间盖住了已同步的评论段时，写出该段的原生区间。点在某一段上时优先用那一段。
bool commentInside(uint32_t visibleStart, uint32_t visibleEnd, int x, int y, uint32_t& nativeStart,
                   uint32_t& nativeEnd);
bool openStored(uint32_t nativeStart, uint32_t nativeEnd);
bool panelOpen();
// 全文页占满屏幕。弹窗则画在已经画好的阅读页上。
bool replacesPage();
bool paintsOverPage();
// 阅读页重新画完、弹窗盖上去之后调用，后面的翻页只重画弹窗。
void markPageUnderlay();
// 面板打开时由阅读循环调用。返回 true 表示画面变了。
bool handlePanel(MappedInputManager& input, const GfxRenderer& renderer);
void drawPanel(GfxRenderer& renderer);

void reset();

}  // namespace WeReadCommentOverlay
