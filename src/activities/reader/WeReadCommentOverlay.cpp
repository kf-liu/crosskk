#include "WeReadCommentOverlay.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <MappedInputManager.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "WeReadClient.h"
#include "WeReadComment.h"
#include "WeReadHighlight.h"
#include "WeReadHttpClient.h"
#include "WeReadStore.h"
#include "WeReadXhtmlCodec.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

#include <Memory.h>

#if defined(SIMULATOR)
// 桌面模拟器的 FreeRTOS 垫片有 xTaskNotify，没有 xTaskNotifyGive / pdPASS。
inline void xTaskNotifyGive(TaskHandle_t task) { xTaskNotify(task, 0, eIncrement); }
#ifndef pdPASS
#define pdPASS pdTRUE
#endif
#endif

namespace WeReadCommentOverlay {
namespace {

enum class Phase : uint8_t { Idle, Loading, Failed, Offline };

struct Round {
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  bool haveChapter = false;
  bool underlinesReady = false;
  bool underlinesFailed = false;
  bool sessionMissing = false;
  // 手动重拉时，已经存过的片段也要再请求一遍。
  bool refreshStored = false;
  // 菜单里的重拉先写到旁边，整章成功才替换。
  bool holdCommit = false;
  ManualRefresh manual = ManualRefresh::Idle;
  Phase phase = Phase::Idle;
};

Round& round() {
  // 候选整章在 SD 的 ranges.wri 里。这里只留这一章的状态，不按条数占内存。
  static Round state;
  return state;
}

struct HitSegment {
  uint16_t rangeIndex = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
};

// 一页上的点击段，不是一章的条数上限。
// 一屏词不够时点线仍会画，只是多出来的点不到。
constexpr size_t kMaxHitSegments = 512;

struct DrawRange {
  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  uint32_t start = 0;
  uint32_t end = 0;
};

struct PageComments {
  char bookId[64] = {};
  int spineIndex = -1;
  uint32_t chapterUid = 0;
  size_t count = 0;
  size_t segmentCount = 0;
  bool valid = false;
  char chapterPath[128] = {};
  // 已落盘的评论段。按本章实际段数增长并复用，不设章上限。
  std::unique_ptr<DrawRange[]> ranges;
  size_t capacity = 0;
  std::unique_ptr<uint32_t[]> nativeOffsets;
  std::unique_ptr<uint32_t[]> visibleOffsets;
  size_t offsetCapacity = 0;
  HitSegment segments[kMaxHitSegments] = {};
};

PageComments& pageComments() {
  static PageComments comments;
  return comments;
}

struct BoxHit {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint8_t kind = 0;
  uint8_t truncated = 0;
  uint16_t index = 0;
};

// 点开或向后翻时，原文先出来，评论在后台到齐后再换这一态。
enum class Load : uint8_t { Ready = 0, Loading, Failed, Offline, Empty };

struct Panel {
  bool open = false;
  bool full = false;
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t index = 0;
  uint32_t window = 0;
  uint32_t shown = 0;
  uint32_t total = 0;
  uint32_t quoteWindow = 0;
  uint32_t quoteShown = 0;
  uint32_t quoteTotal = 0;
  // 整页内容从上往下铺。scrollY 是已经翻过去的高度。
  int scrollY = 0;
  int nextScroll = -1;
  int historyY[24] = {};
  uint8_t historyCount = 0;
  int16_t cardX = 0;
  int16_t cardY = 0;
  int16_t cardW = 0;
  int16_t cardH = 0;
  uint8_t hitCount = 0;
  BoxHit hits[12] = {};
  WeReadComment::StoredComment comment;
  Load load = Load::Ready;
  // 这次打开已经交给后台。worker 正忙时先记着，空闲后再排队，避免把请求丢掉。
  bool fetchQueued = false;
  // 一条评论最长 2KB。静态缓冲给灰阶各遍复用，不放进阅读栈。
  char text[WeReadComment::kMaxCommentBytes] = {};
  // 当前页划线原文。阅读页画完就丢了，点开时拷进这里。
  char quote[1536] = {};
};

// 全文页清掉阅读页之后为 false。弹窗直接盖在还在帧缓冲里的阅读页上。
bool underlayReady = true;

Panel& panel() {
  static Panel state;
  return state;
}

SemaphoreHandle_t stateLock = nullptr;

void lockState() {
  if (stateLock) xSemaphoreTake(stateLock, portMAX_DELAY);
}

void unlockState() {
  if (stateLock) xSemaphoreGive(stateLock);
}

bool lookupChapter(const char* bookId, const Epub& epub, const int spineIndex, uint32_t& tocIndex, uint32_t& chapterUid,
                   std::string& xhtmlPath) {
  if (spineIndex < 0 || spineIndex >= epub.getSpineItemsCount()) return false;
  if (!WeReadStore::parseGeneratedChapterHref(epub.getSpineItem(spineIndex).href, tocIndex)) return false;
  HalFile toc;
  uint32_t tocCount = 0;
  WeReadStore::TocRecord chapter;
  memset(&chapter, 0, sizeof(chapter));
  if (!WeReadStore::openToc(WeReadStore::tocPath(bookId), toc, tocCount) || tocIndex >= tocCount ||
      !WeReadStore::readTocRecord(toc, tocIndex, chapter) ||
      !WeReadHighlight::parseChapterUid(chapter.chapterUid, chapterUid)) {
    return false;
  }
  const std::string path = WeReadStore::chapterPath(WeReadStore::bookDirectory(bookId), tocIndex);
  if (path.empty() || path.size() >= 128) return false;
  xhtmlPath = path;
  return true;
}

bool pageNativeBounds(const std::string& path, const uint32_t pageStart, const uint32_t pageEnd, uint32_t& nativeStart,
                      uint32_t& nativeEnd) {
  if (!WeReadXhtmlCodec::visibleToNativeOffset(path, pageStart, nativeStart)) return false;
  if (pageEnd == UINT32_MAX) {
    nativeEnd = UINT32_MAX;
    return true;
  }
  return WeReadXhtmlCodec::visibleToNativeOffset(path, pageEnd, nativeEnd);
}

struct FetchCtx {
  const char* bookId = nullptr;
  uint32_t chapterUid = 0;
  const char* rangeText = nullptr;
};

bool fetchPage(void* raw, const uint32_t maxIdx, const uint64_t syncKey, WeReadComment::RangeFile& file,
               WeReadComment::Cursor& cursor) {
#ifdef ENABLE_CHINESE_VERSION
  const auto* ctx = static_cast<const FetchCtx*>(raw);
  if (!ctx) return false;
  return WeReadClient::fetchCommentReviewPage(ctx->bookId, ctx->chapterUid, ctx->rangeText, maxIdx, syncKey, file,
                                              cursor);
#else
  (void)raw;
  (void)maxIdx;
  (void)syncKey;
  (void)file;
  (void)cursor;
  return false;
#endif
}

bool queueMore(const Panel& view);
void markPanel(bool open);

void beginChapter(Round& state, const char* bookId, const uint32_t chapterUid) {
  const bool sessionMissing = state.sessionMissing;
  state = {};
  state.sessionMissing = sessionMissing;
  strncpy(state.bookId, bookId, sizeof(state.bookId) - 1);
  state.chapterUid = chapterUid;
  state.haveChapter = true;
}

bool readPanelText(Panel& view, const uint32_t index) {
  WeReadComment::StoredComment comment;
  if (!WeReadComment::readComment(view.bookId, view.chapterUid, view.start, view.end, index, view.text,
                                  sizeof(view.text), comment)) {
    return false;
  }
  view.comment = comment;
  view.index = index;
  return true;
}

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

const char* skipCodepoints(const char* text, const uint32_t count) {
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text ? text : "");
  for (uint32_t i = 0; i < count && *cursor; ++i) {
    if (utf8NextCodepoint(&cursor) == 0) break;
  }
  return reinterpret_cast<const char*>(cursor);
}

bool scrollComments(Panel& view, const bool forward) {
  if (!forward) {
    if (view.historyCount == 0) return false;
    view.scrollY = view.historyY[--view.historyCount];
    return true;
  }
  if (view.nextScroll > view.scrollY) {
    if (view.historyCount < 24) view.historyY[view.historyCount++] = view.scrollY;
    view.scrollY = view.nextScroll;
    return true;
  }
  // 正在拉时翻页只保持加载态，不在阅读循环里等网络。
  if (view.load == Load::Loading) return false;
  const bool wantFetch = view.comment.hasMore || view.load == Load::Failed || view.load == Load::Empty;
  if (!wantFetch) return false;
  if (!WeReadHttpClient::networkReady()) {
    view.load = Load::Offline;
    return true;
  }
  view.load = Load::Loading;
  view.fetchQueued = queueMore(view);
  return true;
}

bool pageFullText(Panel& view, const bool forward) {
  if (forward) {
    if (view.window + view.shown >= view.total) return false;
    view.window += view.shown > 0 ? view.shown : 1;
    return true;
  }
  if (view.window == 0) return false;
  view.window = view.window > view.shown && view.shown > 0 ? view.window - view.shown : 0;
  return true;
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

// 一页上的划线原文。静态区，因为翻页之后 Page 就没了，点开时再拷进弹窗。
constexpr size_t kQuoteBytes = 1536;
constexpr size_t kQuoteSpans = 12;

struct QuoteSpan {
  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  uint16_t offset = 0;
  uint16_t length = 0;
  bool clipped = false;
};

struct QuoteBook {
  char bytes[kQuoteBytes] = {};
  size_t used = 0;
  size_t count = 0;
  QuoteSpan spans[kQuoteSpans] = {};
};

QuoteBook& quotes() {
  static QuoteBook book;
  return book;
}

void resetQuotes() { quotes() = {}; }

void notePassage(const uint32_t nativeStart, const uint32_t nativeEnd, const char* slice) {
  if (!slice || slice[0] == '\0' || nativeEnd <= nativeStart) return;
  QuoteBook& book = quotes();
  QuoteSpan* span = nullptr;
  for (size_t i = 0; i < book.count; ++i) {
    if (book.spans[i].nativeStart == nativeStart && book.spans[i].nativeEnd == nativeEnd) {
      span = &book.spans[i];
      break;
    }
  }
  if (!span) {
    if (book.count >= kQuoteSpans) return;
    span = &book.spans[book.count++];
    span->nativeStart = nativeStart;
    span->nativeEnd = nativeEnd;
    span->offset = static_cast<uint16_t>(book.used);
  }
  if (span->clipped || span->offset + span->length != book.used) {
    span->clipped = true;
    return;
  }
  const size_t add = strlen(slice);
  bool space = false;
  if (span->length > 0 && book.used > 0) {
    const char prev = book.bytes[book.used - 1];
    const auto next = static_cast<unsigned char>(slice[0]);
    if (static_cast<unsigned char>(prev) < 128 && next < 128 && prev != ' ' && next != ' ') space = true;
  }
  if (book.used + (space ? 1 : 0) + add + 1 > kQuoteBytes) {
    span->clipped = true;
    return;
  }
  if (space) book.bytes[book.used++] = ' ';
  memcpy(book.bytes + book.used, slice, add);
  book.used += add;
  book.bytes[book.used] = '\0';
  span->length = static_cast<uint16_t>(book.used - span->offset);
}

void copyQuote(const uint32_t nativeStart, const uint32_t nativeEnd, char* dest, const size_t destSize) {
  if (!dest || destSize == 0) return;
  dest[0] = '\0';
  const QuoteBook& book = quotes();
  for (size_t i = 0; i < book.count; ++i) {
    const QuoteSpan& span = book.spans[i];
    if (span.nativeStart != nativeStart || span.nativeEnd != nativeEnd || span.length == 0) continue;
    size_t count = span.length;
    if (count >= destSize) count = destSize - 1;
    memcpy(dest, book.bytes + span.offset, count);
    dest[count] = '\0';
    return;
  }
}

struct DrawWalk {
  GfxRenderer* renderer = nullptr;
  PageComments* comments = nullptr;
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
  if (!walk.renderer || !walk.comments || !walk.line || !walk.block || !walk.word || span == 0) return;
  const uint32_t wordEnd = start + span;
  int coveredIndex = -1;
  uint32_t overlapStart = start;
  uint32_t overlapEnd = wordEnd;
  for (size_t rangeIndex = 0; rangeIndex < walk.comments->count; ++rangeIndex) {
    const DrawRange& range = walk.comments->ranges[rangeIndex];
    if (!WeReadHighlight::rangesOverlap(start, wordEnd, range.start, range.end)) continue;
    coveredIndex = static_cast<int>(rangeIndex);
    overlapStart = start > range.start ? start : range.start;
    overlapEnd = wordEnd < range.end ? wordEnd : range.end;
    break;
  }
  if (coveredIndex < 0 || overlapEnd <= overlapStart) return;

  {
    const DrawRange& range = walk.comments->ranges[coveredIndex];
    char slice[208];
    const uint32_t skip = overlapStart > start ? overlapStart - start : 0;
    const uint32_t take = overlapEnd - overlapStart;
    if (countCodepoints(walk.word) != span || !copyCodepoints(walk.word, skip, take, slice, sizeof(slice))) {
      notePassage(range.nativeStart, range.nativeEnd, walk.word);
    } else {
      notePassage(range.nativeStart, range.nativeEnd, slice);
    }
  }

  const int8_t tracking = walk.block->getBlockStyle().characterSpacing;
  const EpdFontFamily::Style style = walk.block->wordStyle(wordIndex);
  int prefixWidth = 0;
  int sliceWidth = 0;
  const bool wholeWord = overlapStart == start && overlapEnd == wordEnd;
  if (wholeWord || countCodepoints(walk.word) != span) {
    sliceWidth = walk.renderer->getTextAdvanceX(walk.fontId, walk.word, style, tracking);
  } else {
    static char prefix[208];
    static char covered[208];
    const uint32_t skip = overlapStart - start;
    const uint32_t take = overlapEnd - start;
    if (!copyCodepoints(walk.word, 0, skip, prefix, sizeof(prefix)) ||
        !copyCodepoints(walk.word, 0, take, covered, sizeof(covered))) {
      sliceWidth = walk.renderer->getTextAdvanceX(walk.fontId, walk.word, style, tracking);
    } else {
      prefixWidth = prefix[0] ? walk.renderer->getTextAdvanceX(walk.fontId, prefix, style, tracking) : 0;
      sliceWidth = walk.renderer->getTextAdvanceX(walk.fontId, covered, style, tracking) - prefixWidth;
    }
  }
  if (sliceWidth <= 0) return;

  const int lineY = walk.line->yPos + walk.yOffset + walk.block->getRubyShift(walk.ascender) + walk.ascender + 2;
  const int top = walk.line->yPos + walk.yOffset;
  const int x = walk.line->xPos + walk.xOffset + walk.block->wordXpos(wordIndex) + prefixWidth;
  for (int dot = 0; dot < sliceWidth; dot += 4) {
    const int dotWidth = sliceWidth - dot >= 2 ? 2 : sliceWidth - dot;
    walk.renderer->fillRect(x + dot, lineY, dotWidth, 1, true);
  }
  if (walk.comments->segmentCount < kMaxHitSegments && top < lineY + 4 && coveredIndex >= 0 &&
      coveredIndex <= static_cast<int>(UINT16_MAX)) {
    HitSegment& segment = walk.comments->segments[walk.comments->segmentCount++];
    segment.rangeIndex = static_cast<uint16_t>(coveredIndex);
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
                PageComments& comments) {
  comments.segmentCount = 0;
  resetQuotes();
  DrawWalk walk;
  walk.renderer = &renderer;
  walk.comments = &comments;
  walk.page = &page;
  walk.fontId = fontId;
  walk.xOffset = xOffset;
  walk.yOffset = yOffset;
  walk.ascender = renderer.getFontAscenderSize(fontId);
  walk.cursor = page.visibleTextOffset;
  if (comments.chapterPath[0]) {
    WeReadXhtmlCodec::alignRenderedWords(comments.chapterPath, page.visibleTextOffset, pullWord, acceptWord, &walk);
  }
  if (walk.word && !walk.accepted) paintFallback(walk);
  while (pullWord(&walk)) paintFallback(walk);
}

bool sameChapter(const PageComments& comments, const char* bookId, const int spineIndex) {
  return comments.valid && comments.spineIndex == spineIndex &&
         strncmp(comments.bookId, bookId, sizeof(comments.bookId)) == 0;
}

bool ensureDrawRanges(PageComments& comments, const size_t needed) {
  if (needed <= comments.capacity) return true;
  // 本章已存评论的段数，扫目录之后才知道。缓冲复用，不按章设上限。
  auto next = makeUniqueNoThrow<DrawRange[]>(needed);
  if (!next) {
    LOG_ERR("WR", "内存不足：已存评论 %u 段，每段 %u 字节", static_cast<unsigned>(needed),
            static_cast<unsigned>(sizeof(DrawRange)));
    return false;
  }
  comments.ranges = std::move(next);
  comments.capacity = needed;
  return true;
}

bool ensureOffsets(PageComments& comments, const size_t pairs) {
  if (pairs <= comments.offsetCapacity) return true;
  auto native = makeUniqueNoThrow<uint32_t[]>(pairs * 2);
  auto visible = makeUniqueNoThrow<uint32_t[]>(pairs * 2);
  if (!native || !visible) {
    LOG_ERR("WR", "内存不足：评论坐标 %u 对", static_cast<unsigned>(pairs));
    return false;
  }
  comments.nativeOffsets = std::move(native);
  comments.visibleOffsets = std::move(visible);
  comments.offsetCapacity = pairs;
  return true;
}

// 点线直接按候选索引画：索引一到，本章哪里可能有讨论整章可见，不用等每段内容都下载。
// 点开拉过并确认没有评论的段（Empty）不再画。评论数只有打开拉取后才知道，
// 所以索引阶段宁可多画，打开一次空段就会收敛掉。
bool candidateDrawable(const WeReadComment::Candidate& item) {
  return item.end > item.start && item.query != WeReadComment::CandidateQuery::Empty;
}

struct CandidateCount {
  size_t count = 0;
};

bool countCandidates(void* raw, const uint32_t, const WeReadComment::Candidate& item) {
  if (candidateDrawable(item)) ++static_cast<CandidateCount*>(raw)->count;
  return true;
}

bool takeCandidate(void* raw, const uint32_t, const WeReadComment::Candidate& item) {
  auto* comments = static_cast<PageComments*>(raw);
  if (!candidateDrawable(item)) return true;
  if (comments->count >= comments->capacity) return false;
  DrawRange& range = comments->ranges[comments->count++];
  range.nativeStart = item.start;
  range.nativeEnd = item.end;
  range.start = item.start;
  range.end = item.end;
  return true;
}

void loadPageComments(const char* bookId, const Epub& epub, const int spineIndex) {
  PageComments& comments = pageComments();
  comments.valid = false;
  comments.count = 0;
  comments.segmentCount = 0;
  comments.spineIndex = spineIndex;
  comments.chapterUid = 0;
  comments.bookId[0] = '\0';
  comments.chapterPath[0] = '\0';
  strncpy(comments.bookId, bookId, sizeof(comments.bookId) - 1);

  uint32_t tocIndex = 0;
  uint32_t chapterUid = 0;
  std::string path;
  if (!lookupChapter(bookId, epub, spineIndex, tocIndex, chapterUid, path)) return;
  comments.chapterUid = chapterUid;
  memcpy(comments.chapterPath, path.c_str(), path.size() + 1);

  CandidateCount candidates;
  if (!WeReadComment::visitCandidates(bookId, chapterUid, countCandidates, &candidates)) return;
  if (candidates.count > 0 && !ensureDrawRanges(comments, candidates.count)) return;
  WeReadComment::visitCandidates(bookId, chapterUid, takeCandidate, &comments);
  if (comments.count == 0) {
    comments.valid = true;
    return;
  }
  if (!ensureOffsets(comments, comments.count)) {
    comments.count = 0;
    return;
  }
  for (size_t i = 0; i < comments.count; ++i) {
    comments.nativeOffsets[i * 2] = comments.ranges[i].nativeStart;
    comments.nativeOffsets[i * 2 + 1] = comments.ranges[i].nativeEnd;
  }
  if (!WeReadXhtmlCodec::nativeOffsetsToVisible(path, comments.nativeOffsets.get(), comments.visibleOffsets.get(),
                                                comments.count * 2)) {
    LOG_ERR("WR", "评论范围无法对到章节");
    comments.count = 0;
    return;
  }
  size_t visibleCount = 0;
  for (size_t i = 0; i < comments.count; ++i) {
    const uint32_t start = comments.visibleOffsets[i * 2];
    const uint32_t end = comments.visibleOffsets[i * 2 + 1];
    if (start == UINT32_MAX || end == UINT32_MAX || end <= start) continue;
    comments.ranges[visibleCount] = comments.ranges[i];
    comments.ranges[visibleCount].start = start;
    comments.ranges[visibleCount].end = end;
    ++visibleCount;
  }
  comments.count = visibleCount;
  comments.valid = true;
}

void drawWrapped(const GfxRenderer& renderer, const int fontId, const char* text, const uint32_t window, const int x,
                 int y, const int maxWidth, const int bottom, uint32_t& shown) {
  shown = 0;
  const char* cursor = skipCodepoints(text, window);
  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0 || maxWidth <= 0) return;
  while (*cursor && y + lineHeight <= bottom) {
    char line[180];
    size_t bytes = 0;
    uint32_t taken = 0;
    const unsigned char* probe = reinterpret_cast<const unsigned char*>(cursor);
    while (*probe) {
      const unsigned char* mark = probe;
      if (utf8NextCodepoint(&probe) == 0) break;
      const size_t nextBytes = bytes + static_cast<size_t>(probe - mark);
      if (nextBytes >= sizeof(line)) break;
      memcpy(line + bytes, mark, static_cast<size_t>(probe - mark));
      line[nextBytes] = '\0';
      if (renderer.getTextAdvanceX(fontId, line, EpdFontFamily::REGULAR) > maxWidth) break;
      bytes = nextBytes;
      ++taken;
    }
    if (taken == 0) break;
    line[bytes] = '\0';
    renderer.drawText(fontId, x, y, line);
    cursor += bytes;
    shown += taken;
    y += lineHeight;
  }
}

constexpr uint8_t kMoreIdle = 0;
constexpr uint8_t kMoreWanted = 1;
constexpr uint8_t kMoreFailed = 2;
// 网络请求跑在独立任务里。8192 字节是这一帧 HTTP 调用的栈，不占阅读循环。
constexpr uint32_t kWorkerStackBytes = 8192;

struct Job {
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  bool bounds = false;
  bool fetch = false;
  bool active = false;
  int spine = -1;
  int page = -1;
  uint32_t generation = 0;
};

struct MoreRequest {
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t maxIdx = 0;
  uint64_t syncKey = 0;
  uint32_t generation = 0;
};

Job& job() {
  static Job state;
  return state;
}

MoreRequest& moreRequest() {
  static MoreRequest state;
  return state;
}

std::atomic<bool> dirty{false};
// 正在把整章评论换上。这段时间取消只做标记，不能同时删 .new。
std::atomic<bool> committing{false};
// 后台任务正在发请求或收尾（索引、评论内容、按段提交）。取消/退出据此不删它正写的暂存文件。
std::atomic<bool> workerBusy{false};
std::atomic<bool> panelShown{false};
std::atomic<uint8_t> moreState{kMoreIdle};
TaskHandle_t worker = nullptr;

void markPanel(const bool open) { panelShown.store(open); }

void notifyWorker() {
  if (worker) xTaskNotifyGive(worker);
}

bool readerLoggedIn() {
  WeReadStore::Session session;
  const bool loggedIn = WeReadStore::loadSession(session) && session.valid();
  session.clear();
  return loggedIn;
}

bool queueMore(const Panel& view) {
  if (moreState.load() != kMoreIdle) return false;
  lockState();
  MoreRequest& request = moreRequest();
  strncpy(request.bookId, view.bookId, sizeof(request.bookId) - 1);
  request.bookId[sizeof(request.bookId) - 1] = '\0';
  request.chapterUid = view.chapterUid;
  request.start = view.start;
  request.end = view.end;
  request.maxIdx = view.comment.maxIdx;
  request.syncKey = view.comment.syncKey;
  request.generation = job().generation;
  unlockState();
  moreState.store(kMoreWanted);
  notifyWorker();
  return true;
}

bool rangeHasMore(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end,
                  const bool staged) {
  std::string path = WeReadComment::rangePath(bookId, chapterUid, start, end);
  if (path.empty()) return false;
  if (staged) path += ".new";
  HalFile file;
  WeReadComment::Header header;
  if (!Storage.openFileForRead("WR", path, file) ||
      file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != WeReadComment::kMagic ||
      header.version != WeReadComment::kVersion || header.headerSize != WeReadComment::kHeaderSize) {
    return false;
  }
  return (header.flags & WeReadComment::kFlagHasMore) != 0;
}

// 「哪里有评论」的索引拉完后 48 小时内不再请求。覆盖两天内的反复打开；过期后再进章会在后台刷新索引。
constexpr uint32_t kCommentFreshSeconds = 48 * 60 * 60;

enum class WorkKind : uint8_t { None, Underlines, Range, More };

struct Assignment {
  WorkKind kind = WorkKind::None;
  uint32_t generation = 0;
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  bool bounds = false;
  size_t rangeIndex = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t maxIdx = 0;
  uint64_t syncKey = 0;
  bool copyExisting = false;
  // 已有评论先留着。新内容写到旁边，整段成功才替换。
  bool stage = false;
  // 整章重拉时，这一段拉完也先留在 .new。
  bool keepStaged = false;
  bool holdCommit = false;
};

struct PendingHit {
  uint32_t pageStart = 0;
  uint32_t pageEnd = 0;
  bool found = false;
  uint32_t index = 0;
  WeReadComment::Candidate item;
};

struct ChapterScan {
  bool refreshStored = false;
  bool found = false;
  bool anyFailed = false;
  uint32_t generation = 0;
  uint32_t index = 0;
  WeReadComment::Candidate item;
};

// 一章里这一轮已经拉完的段。静态位图，不随段数在堆上增长。
// 超过 1024 段的已存评论这一轮不再重拉，避免完成后再被选中、反复请求。
constexpr uint32_t kSettledRangeCap = 1024;
uint8_t settledBits[kSettledRangeCap / 8] = {};
uint32_t settledGeneration = 0;

bool rangeSettled(const uint32_t generation, const uint32_t index) {
  if (generation != settledGeneration || index >= kSettledRangeCap) return false;
  return (settledBits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

void markRangeSettled(const uint32_t generation, const uint32_t index) {
  if (generation != settledGeneration) {
    memset(settledBits, 0, sizeof(settledBits));
    settledGeneration = generation;
  }
  if (index >= kSettledRangeCap) return;
  settledBits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

bool scanChapter(void* raw, const uint32_t index, const WeReadComment::Candidate& item) {
  auto* hit = static_cast<ChapterScan*>(raw);
  if (item.query == WeReadComment::CandidateQuery::Failed) {
    hit->anyFailed = true;
    return true;
  }
  // 这一轮拉完的已存段不再选。否则刷新标志还在时，成功写回的段会立刻被再拉一次。
  if (hit->refreshStored && item.query == WeReadComment::CandidateQuery::Stored &&
      (index >= kSettledRangeCap || rangeSettled(hit->generation, index))) {
    return true;
  }
  const bool open = item.query == WeReadComment::CandidateQuery::Unknown ||
                    item.query == WeReadComment::CandidateQuery::Partial ||
                    (hit->refreshStored && item.query == WeReadComment::CandidateQuery::Stored);
  if (!open) return true;
  hit->found = true;
  hit->index = index;
  hit->item = item;
  return false;
}

bool chapterFetchFresh(const char* bookId, const uint32_t chapterUid) {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (now == 0 || !WeReadComment::candidatesReady(bookId, chapterUid)) return false;
  const uint32_t fetchedAt = WeReadComment::chapterFetchedAt(bookId, chapterUid);
  if (fetchedAt == 0 || fetchedAt > now) return false;
  return now - fetchedAt < kCommentFreshSeconds;
}

bool rememberCandidate(const char* bookId, const uint32_t chapterUid, const uint32_t index, const uint32_t start,
                       const uint32_t end, const WeReadComment::CandidateQuery query, const uint32_t totalCount,
                       const uint32_t markCount, const bool staged) {
  WeReadComment::Candidate item;
  if (!WeReadComment::readCandidate(bookId, chapterUid, index, item, staged)) return false;
  if (item.start != start || item.end != end) return false;
  item.query = query;
  if (totalCount > item.commentTotal) item.commentTotal = totalCount;
  if (markCount > item.markCount) item.markCount = markCount;
  return WeReadComment::updateCandidate(bookId, chapterUid, index, item, staged);
}

uint8_t failedBits[kSettledRangeCap / 8] = {};

bool noteFailedCandidate(void* raw, const uint32_t index, const WeReadComment::Candidate& item) {
  if (item.query != WeReadComment::CandidateQuery::Failed) return true;
  if (index >= kSettledRangeCap) {
    *static_cast<bool*>(raw) = true;
    return true;
  }
  failedBits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
  return true;
}

void reopenOneFailed(const char* bookId, const uint32_t chapterUid, const uint32_t index,
                     const WeReadComment::Candidate& item) {
  WeReadComment::Candidate next = item;
  const std::string path = WeReadComment::rangePath(bookId, chapterUid, item.start, item.end);
  // 有旧文件就按已存重拉，这样新内容写到旁边，失败不会盖掉已有评论。
  next.query = (!path.empty() && Storage.exists(path.c_str())) ? WeReadComment::CandidateQuery::Stored
                                                                : WeReadComment::CandidateQuery::Unknown;
  WeReadComment::updateCandidate(bookId, chapterUid, index, next);
}

void reopenFailedRanges(const char* bookId, const uint32_t chapterUid) {
  // 遍历时文件还开着，不能在回调里改索引。先记下失败段，关文件后再写。
  memset(failedBits, 0, sizeof(failedBits));
  bool overflow = false;
  WeReadComment::visitCandidates(bookId, chapterUid, noteFailedCandidate, &overflow);
  for (uint32_t index = 0; index < kSettledRangeCap; ++index) {
    if ((failedBits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) == 0) continue;
    WeReadComment::Candidate item;
    if (!WeReadComment::readCandidate(bookId, chapterUid, index, item) ||
        item.query != WeReadComment::CandidateQuery::Failed) {
      continue;
    }
    reopenOneFailed(bookId, chapterUid, index, item);
  }
  if (!overflow) return;
  for (uint32_t index = kSettledRangeCap;; ++index) {
    WeReadComment::Candidate item;
    if (!WeReadComment::readCandidate(bookId, chapterUid, index, item)) return;
    if (item.query == WeReadComment::CandidateQuery::Failed) reopenOneFailed(bookId, chapterUid, index, item);
  }
}

bool pickWork(Assignment& work) {
  work = {};
  if (!stateLock) return false;
  lockState();
  Job& current = job();
  work.generation = current.generation;
  bool scanChapterWork = false;
  bool refreshStored = false;
  if (moreState.load() == kMoreWanted) {
    const MoreRequest& request = moreRequest();
    if (request.generation != current.generation || request.end <= request.start) {
      moreState.store(kMoreIdle);
    } else {
      work.kind = WorkKind::More;
      strncpy(work.bookId, request.bookId, sizeof(work.bookId) - 1);
      work.chapterUid = request.chapterUid;
      work.start = request.start;
      work.end = request.end;
      work.maxIdx = request.maxIdx;
      work.syncKey = request.syncKey;
      work.copyExisting = true;
    }
  } else if (current.fetch && current.active && current.bookId[0] != '\0' && !round().sessionMissing) {
    strncpy(work.bookId, current.bookId, sizeof(work.bookId) - 1);
    work.chapterUid = current.chapterUid;
    work.nativeStart = current.nativeStart;
    work.nativeEnd = current.nativeEnd;
    work.bounds = current.bounds;
    Round& state = round();
    work.holdCommit = state.holdCommit;
    if (!state.underlinesReady && !state.underlinesFailed) {
      work.kind = WorkKind::Underlines;
    } else if (state.underlinesReady && work.holdCommit) {
      // 自动进章到索引为止。按段爬完整章内容只发生在手动重拉。
      scanChapterWork = true;
      refreshStored = state.refreshStored;
    }
  }
  unlockState();
  if (scanChapterWork) {
    ChapterScan hit;
    hit.refreshStored = refreshStored;
    hit.generation = work.generation;
    const bool visited =
        WeReadComment::visitCandidates(work.bookId, work.chapterUid, scanChapter, &hit, work.holdCommit);
    if (visited && hit.found) {
      work.kind = WorkKind::Range;
      work.rangeIndex = hit.index;
      work.start = hit.item.start;
      work.end = hit.item.end;
      const std::string live = WeReadComment::rangePath(work.bookId, work.chapterUid, work.start, work.end);
      const bool staging = !live.empty() && Storage.exists((live + ".new").c_str());
      work.keepStaged = work.holdCommit;
      work.stage = work.holdCommit || staging ||
                   (refreshStored && hit.item.query == WeReadComment::CandidateQuery::Stored);
      work.copyExisting = hit.item.query == WeReadComment::CandidateQuery::Partial || staging;
    } else if (visited && !hit.found) {
      lockState();
      const bool live = work.generation == job().generation && round().holdCommit;
      unlockState();
      if (!live) {
        WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
      } else {
        lockState();
        const bool liveNow = work.generation == job().generation && round().holdCommit;
        if (liveNow) committing.store(true);
        unlockState();
        bool commitOk = false;
        if (!liveNow) {
          WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
        } else {
          uint32_t generation = work.generation;
          auto stillWanted = [](void* raw) {
            const uint32_t wanted = *static_cast<const uint32_t*>(raw);
            lockState();
            const bool ok = wanted == job().generation && round().holdCommit;
            unlockState();
            return ok;
          };
          commitOk = !hit.anyFailed &&
                     WeReadComment::commitStagedChapter(work.bookId, work.chapterUid, stillWanted, &generation);
          committing.store(false);
          if (!commitOk) WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
        }
        const uint32_t now = TimeUtils::getCurrentValidTimestamp();
        if (commitOk && now != 0) WeReadComment::markChapterFetched(work.bookId, work.chapterUid, now);
        lockState();
        if (work.generation != job().generation) {
          if (commitOk) pageComments().valid = false;
          unlockState();
          if (!commitOk) WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
        } else {
          job().fetch = false;
          round().refreshStored = false;
          round().holdCommit = false;
          round().phase = Phase::Idle;
          round().manual = commitOk ? ManualRefresh::Succeeded : ManualRefresh::Failed;
          // 回到阅读页时由那一屏重画。成功则丢掉这一页的旧点线，按新文件再载入。
          if (commitOk) pageComments().valid = false;
          dirty.store(false);
          unlockState();
          if (commitOk) LOG_INF("WR", "本章评论已拉完");
          else LOG_INF("WR", "本章评论没有替换");
        }
      }
    }
  }
  if (work.kind == WorkKind::Range && work.copyExisting && !work.stage) {
    char scratch[2] = {};
    WeReadComment::StoredComment comment;
    if (WeReadComment::readComment(work.bookId, work.chapterUid, work.start, work.end, 0, scratch, sizeof(scratch),
                                   comment)) {
      work.maxIdx = comment.maxIdx;
      work.syncKey = comment.syncKey;
    }
  }
  return work.kind != WorkKind::None;
}

void applyRangeResult(const Assignment& work, const WeReadComment::CollectResult result, const uint32_t totalCount,
                      const uint32_t markCount) {
  // 工作项到这里已收尾，请求不在途。评论翻页共用的 TLS 到此断开，不跨段常驻。
  WeReadClient::closeCommentPageSession();
  const bool hasMore = result == WeReadComment::CollectResult::Stored &&
                       rangeHasMore(work.bookId, work.chapterUid, work.start, work.end, work.keepStaged);
  lockState();
  if (work.generation != job().generation) {
    unlockState();
    if (work.holdCommit) WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
    return;
  }
  unlockState();
  WeReadComment::CandidateQuery query = WeReadComment::CandidateQuery::Stored;
  if (result == WeReadComment::CollectResult::Empty) query = WeReadComment::CandidateQuery::Empty;
  else if (result == WeReadComment::CollectResult::Failed) query = WeReadComment::CandidateQuery::Failed;
  else if (hasMore) query = WeReadComment::CandidateQuery::Partial;
  if (work.kind != WorkKind::More) {
    rememberCandidate(work.bookId, work.chapterUid, static_cast<uint32_t>(work.rangeIndex), work.start, work.end, query,
                      totalCount, markCount, work.holdCommit);
  } else if (totalCount > 0 || markCount > 0) {
    PendingHit hit;
    hit.pageStart = work.start;
    hit.pageEnd = work.end;
    hit.found = false;
    // 翻更多评论时按下标对不上，按起止找到这一段再写人数。
    auto match = [](void* raw, const uint32_t index, const WeReadComment::Candidate& item) {
      auto* wanted = static_cast<PendingHit*>(raw);
      if (item.start != wanted->pageStart || item.end != wanted->pageEnd) return true;
      wanted->found = true;
      wanted->index = index;
      wanted->item = item;
      return false;
    };
    if (WeReadComment::visitCandidates(work.bookId, work.chapterUid, match, &hit) && hit.found) {
      rememberCandidate(work.bookId, work.chapterUid, hit.index, work.start, work.end, query, totalCount, markCount,
                        work.holdCommit);
    }
  }
  lockState();
  if (work.generation != job().generation) {
    unlockState();
    if (work.holdCommit) WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
    return;
  }
  if (work.kind == WorkKind::More) {
    moreState.store(result == WeReadComment::CollectResult::Failed ? kMoreFailed : kMoreIdle);
  } else if (query == WeReadComment::CandidateQuery::Stored || query == WeReadComment::CandidateQuery::Empty) {
    markRangeSettled(work.generation, static_cast<uint32_t>(work.rangeIndex));
  }
  // 手动重拉还没整章换上。阅读页继续用旧缓存，成功后再丢掉。
  if (!work.holdCommit) dirty.store(true);
  unlockState();
}

bool stepOnce() {
#ifdef ENABLE_CHINESE_VERSION
  Assignment work;
  if (!pickWork(work) || work.generation != job().generation) {
    lockState();
    Round& state = round();
    if (state.underlinesReady && !state.underlinesFailed && !state.sessionMissing && job().fetch) {
      state.phase = Phase::Idle;
    }
    unlockState();
    return false;
  }
  static bool deferredLogged = false;
  if (!WeReadHttpClient::heapAllowsNewTls()) {
    // 底线之下不开新 TLS（PHY PLL 定时器分配贴地板会 assert 重启）。工作留在队列里，
    // 200ms 后重试；只在进入低堆时记一次日志，避免刷屏。
    if (!deferredLogged) {
      LOG_INF("WR", "comment fetch deferred: free=%u largest=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      deferredLogged = true;
    }
    return false;
  }
  deferredLogged = false;
  workerBusy.store(true);
  if (work.kind == WorkKind::Underlines) {
    const bool ok = WeReadClient::fetchCommentUnderlines(work.bookId, work.chapterUid, work.holdCommit);
    lockState();
    if (work.generation != job().generation) {
      unlockState();
      if (work.holdCommit) WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
      return true;
    }
    Round& state = round();
    if (!ok) {
      const bool hold = state.holdCommit;
      const bool onlineNow = WeReadHttpClient::networkReady();
      if (onlineNow) {
        WeReadStore::Session session;
        if (!WeReadStore::loadSession(session) || !session.valid()) state.sessionMissing = true;
        else state.underlinesFailed = true;
        session.clear();
      }
      // 这一轮已经结束。不在本章马上再拉。状态栏回到章节名，进行中的标记当场清掉。
      state.refreshStored = false;
      state.holdCommit = false;
      job().fetch = false;
      if (hold) {
        state.manual = ManualRefresh::Failed;
        state.phase = Phase::Idle;
      } else {
        state.phase = (!onlineNow && !state.sessionMissing) ? Phase::Offline : Phase::Idle;
      }
      unlockState();
      if (hold) WeReadComment::discardStagedChapter(work.bookId, work.chapterUid);
      else WeReadComment::clearChapterFetched(work.bookId, work.chapterUid);
      dirty.store(!hold);
      return false;
    }
    state.underlinesReady = true;
    const bool hold = state.holdCommit;
    char bookId[64];
    strncpy(bookId, work.bookId, sizeof(bookId) - 1);
    bookId[sizeof(bookId) - 1] = '\0';
    const uint32_t chapterUid = work.chapterUid;
    unlockState();
    if (hold) {
      // 手动重拉接着按段爬，整章成功才一起换上。
      lockState();
      if (work.generation == job().generation) state.phase = Phase::Loading;
      unlockState();
      return true;
    }
    // 自动进章到索引为止：哪里有评论已完整落盘，具体内容点开弹层再按页拉。
    const uint32_t now = TimeUtils::getCurrentValidTimestamp();
    if (now != 0) WeReadComment::markChapterFetched(bookId, chapterUid, now);
    lockState();
    if (work.generation == job().generation) {
      job().fetch = false;
      state.phase = Phase::Idle;
    }
    unlockState();
    // 索引换上了，阅读页重画点线。
    dirty.store(true);
    return true;
  }

  char rangeText[32];
  const int length = snprintf(rangeText, sizeof(rangeText), "%u-%u", work.start, work.end);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(rangeText)) {
    if (work.kind == WorkKind::More) moreState.store(kMoreFailed);
    else {
      rememberCandidate(work.bookId, work.chapterUid, static_cast<uint32_t>(work.rangeIndex), work.start, work.end,
                        WeReadComment::CandidateQuery::Failed, 0, 0, work.holdCommit);
    }
    dirty.store(true);
    return true;
  }
  FetchCtx ctx;
  ctx.bookId = work.bookId;
  ctx.chapterUid = work.chapterUid;
  ctx.rangeText = rangeText;
  uint32_t totalCount = 0;
  uint32_t markCount = 0;
  // 一次只拉一页。弹层写完就显示；整章重拉也是一页一页往后走，不攒三页。
  auto result = WeReadComment::collectRange(work.bookId, work.chapterUid, work.start, work.end, work.maxIdx,
                                            work.syncKey, work.copyExisting, fetchPage, &ctx, 1, &totalCount,
                                            &markCount, work.stage, work.keepStaged);
  // 后台重拉没成功时不要把已有评论标成空。整章重拉由提交时再决定换不换。
  if (work.stage && !work.keepStaged && result != WeReadComment::CollectResult::Stored) {
    result = WeReadComment::CollectResult::Failed;
  }
  applyRangeResult(work, result, totalCount, markCount);
  return true;
#else
  return false;
#endif
}

void workerMain(void*) {
  for (;;) {
    if (!stepOnce()) {
      workerBusy.store(false);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
      continue;
    }
    workerBusy.store(false);
    // 手动重拉一段一段爬，段与段之间留空隙给翻页。弹层正等着评论内容时不拖这 300ms。
    uint32_t pauseMs = moreState.load() == kMoreWanted ? 20 : 300;
    if (stateLock) {
      lockState();
      if (round().holdCommit) pauseMs = 20;
      unlockState();
    }
    vTaskDelay(pdMS_TO_TICKS(pauseMs));
  }
}

void ensureWorker() {
#ifdef ENABLE_CHINESE_VERSION
  if (!stateLock) stateLock = xSemaphoreCreateMutex();
  if (!stateLock) {
    LOG_ERR("WR", "评论同步锁创建失败");
    return;
  }
  if (worker) return;
  if (xTaskCreate(workerMain, "wrCmt", kWorkerStackBytes, nullptr, 1, &worker) != pdPASS) {
    LOG_ERR("WR", "评论同步任务创建失败，栈 %u 字节", static_cast<unsigned>(kWorkerStackBytes));
    worker = nullptr;
  }
#else
  (void)0;
#endif
}

bool publishPage(const char* bookId, const Epub& epub, const int spineIndex, const int pageNumber,
                 const uint32_t pageStart, const uint32_t pageEnd) {
  if (!WeReadComment::isStandardBook(bookId)) return false;
  ensureWorker();
  const bool online = WeReadHttpClient::networkReady();
  lockState();
  Round& state = round();
  Job& current = job();
  const bool sessionMissing = state.sessionMissing;
  // 同一章翻页不再发请求。换章才决定要不要在后台刷新「哪里有评论」的索引。
  const bool sameChapter = current.active && current.spine == spineIndex &&
                           strncmp(current.bookId, bookId, sizeof(current.bookId)) == 0;
  // 离线进来之后网络恢复，补一次本章索引。翻页本身不再发请求。
  const bool resumeOffline = sameChapter && state.phase == Phase::Offline && online && !sessionMissing;
  unlockState();
  if (sameChapter && !resumeOffline) return false;

  uint32_t tocIndex = 0;
  uint32_t chapterUid = 0;
  std::string path;
  if (!lookupChapter(bookId, epub, spineIndex, tocIndex, chapterUid, path)) {
    lockState();
    job().fetch = false;
    job().spine = spineIndex;
    job().page = pageNumber;
    job().active = true;
    round().phase = Phase::Idle;
    unlockState();
    return false;
  }
  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  const bool bounds = pageNativeBounds(path, pageStart, pageEnd, nativeStart, nativeEnd);
  const bool fresh = chapterFetchFresh(bookId, chapterUid);
  const bool haveCandidates = WeReadComment::candidatesReady(bookId, chapterUid);
  // 在线时以卡上的会话为准。未登录不发请求，也不把失败写进状态栏。
  bool noSession = sessionMissing;
  if (online && !fresh) noSession = !readerLoggedIn();
  const bool blocked = !online || noSession;

  lockState();
  const Phase previous = state.phase;
  if (!state.haveChapter || state.chapterUid != chapterUid ||
      strncmp(state.bookId, bookId, sizeof(state.bookId)) != 0) {
    beginChapter(state, bookId, chapterUid);
    current.generation++;
  }
  strncpy(current.bookId, bookId, sizeof(current.bookId) - 1);
  current.bookId[sizeof(current.bookId) - 1] = '\0';
  current.chapterUid = chapterUid;
  current.nativeStart = nativeStart;
  current.nativeEnd = nativeEnd;
  current.bounds = bounds;
  current.spine = spineIndex;
  current.page = pageNumber;
  current.active = true;
  if (fresh) {
    state.underlinesReady = true;
    state.refreshStored = false;
    current.fetch = false;
    state.phase = Phase::Idle;
    LOG_INF("WR", "本章评论索引 48 小时内已拉过，跳过");
  } else if (blocked) {
    state.sessionMissing = noSession;
    state.underlinesReady = haveCandidates;
    state.refreshStored = false;
    current.fetch = false;
    if (noSession) state.phase = Phase::Idle;
    else state.phase = haveCandidates ? Phase::Idle : Phase::Offline;
  } else {
    state.sessionMissing = false;
    state.underlinesReady = false;
    state.underlinesFailed = false;
    state.refreshStored = false;
    current.fetch = true;
    state.phase = Phase::Loading;
  }
  const bool phaseChanged = state.phase != previous;
  const bool fetch = current.fetch;
  unlockState();
  if (fetch) {
    reopenFailedRanges(bookId, chapterUid);
    notifyWorker();
  }
  return phaseChanged;
}

void showRange(Panel& view) {
  copyQuote(view.start, view.end, view.quote, sizeof(view.quote));
  view.quoteTotal = countCodepoints(view.quote);
  view.fetchQueued = false;
  const bool stored = readPanelText(view, 0);
  if (stored && view.comment.count > 0) {
    view.load = Load::Ready;
  } else if (!WeReadHttpClient::networkReady()) {
    if (!stored) {
      view.comment = {};
      view.text[0] = '\0';
    }
    view.load = stored ? Load::Empty : Load::Offline;
  } else if (stored) {
    view.load = Load::Empty;
  } else {
    view.comment = {};
    view.text[0] = '\0';
    view.load = Load::Loading;
    view.fetchQueued = queueMore(view);
  }
  underlayReady = false;
  markPanel(true);
}

}  // namespace

bool syncPage(const char* bookId, const Epub& epub, const int spineIndex, const int pageNumber,
              const uint32_t pageStart, const uint32_t pageEnd) {
  // 只登记当前章。underlines 索引在 wrCmt 任务里拉；评论内容点开弹层后按页在这里拉。
  return publishPage(bookId, epub, spineIndex, pageNumber, pageStart, pageEnd);
}

ManualRefresh beginManualRefresh() {
  ensureWorker();
  if (!stateLock) return ManualRefresh::Failed;
  lockState();
  Round& state = round();
  if (!state.haveChapter || state.bookId[0] == '\0') {
    unlockState();
    return ManualRefresh::Idle;
  }
  char bookId[64];
  strncpy(bookId, state.bookId, sizeof(bookId) - 1);
  bookId[sizeof(bookId) - 1] = '\0';
  const uint32_t chapterUid = state.chapterUid;
  unlockState();

  const bool loggedIn = readerLoggedIn();
  const bool online = WeReadHttpClient::networkReady();
  if (!loggedIn || !online) {
    const ManualRefresh outcome = loggedIn ? ManualRefresh::Offline : ManualRefresh::LoginRequired;
    lockState();
    if (!state.haveChapter || state.chapterUid != chapterUid ||
        strncmp(state.bookId, bookId, sizeof(state.bookId)) != 0) {
      unlockState();
      return ManualRefresh::Idle;
    }
    state.manual = outcome;
    state.holdCommit = false;
    state.sessionMissing = !loggedIn;
    unlockState();
    return outcome;
  }

  WeReadComment::discardStagedChapter(bookId, chapterUid);
  lockState();
  if (!state.haveChapter || state.chapterUid != chapterUid ||
      strncmp(state.bookId, bookId, sizeof(state.bookId)) != 0) {
    unlockState();
    return ManualRefresh::Idle;
  }
  Job& current = job();
  state.sessionMissing = false;
  state.underlinesReady = false;
  state.underlinesFailed = false;
  state.refreshStored = true;
  state.holdCommit = true;
  state.manual = ManualRefresh::Running;
  current.generation++;
  current.fetch = true;
  state.phase = Phase::Loading;
  unlockState();
  notifyWorker();
  return ManualRefresh::Running;
}

ManualRefresh manualRefreshState() {
  if (!stateLock) return ManualRefresh::Idle;
  lockState();
  const ManualRefresh state = round().manual;
  unlockState();
  return state;
}

void cancelManualRefresh() {
  if (!stateLock) return;
  lockState();
  char bookId[64] = {};
  strncpy(bookId, round().bookId, sizeof(bookId) - 1);
  const uint32_t chapterUid = round().chapterUid;
  const bool hold = round().holdCommit || round().manual == ManualRefresh::Running;
  round().holdCommit = false;
  round().refreshStored = false;
  round().manual = ManualRefresh::Idle;
  round().phase = Phase::Idle;
  job().fetch = false;
  job().generation++;
  // 提交进行中由那一侧退回并删 .new；任务还在拉取时删暂存会拆掉它正写的文件。两种情况都先不删，
  // 暂存遗留由下次 beginManualRefresh 开头的 discardStagedChapter 清掉。
  const bool busy = committing.load() || workerBusy.load();
  unlockState();
  if (hold && !busy && bookId[0] != '\0') WeReadComment::discardStagedChapter(bookId, chapterUid);
  dirty.store(false);
}

// 弹层的评论内容请求已排队或在途（moreState 在请求结束才离开 Wanted）。
bool panelFetchActive() { return moreState.load() == kMoreWanted; }

const char* chapterStatus() {
  Phase phase = Phase::Idle;
  if (stateLock) {
    lockState();
    phase = round().phase;
    unlockState();
  }
  switch (phase) {
    case Phase::Offline:
      return tr(STR_WEREAD_PASSAGE_COMMENT_OFFLINE);
    case Phase::Loading:
    case Phase::Failed:
    case Phase::Idle:
      // 整章拉取要很多段。进行中和失败都不占阅读页底部，章节名留着。
      break;
  }
  return nullptr;
}

bool takeDirty() {
  if (!dirty.exchange(false)) return false;
  pageComments().valid = false;
  const bool moreFailed = moreState.load() == kMoreFailed;
  if (moreFailed) moreState.store(kMoreIdle);
  Panel& view = panel();
  if (!view.open) return true;
  if (view.load != Load::Loading && view.load != Load::Ready && view.load != Load::Failed) return true;
  const uint32_t index = view.full ? view.index : 0;
  const uint32_t previous = view.comment.count;
  bool got = readPanelText(view, index);
  if (!got && previous > 0) got = readPanelText(view, 0);
  if (!got || view.comment.count == 0) {
    // 还没轮到这次请求，或完成的是别的范围。保持加载，空闲后再排。
    if (view.load != Load::Loading || moreState.load() == kMoreWanted || !view.fetchQueued) return true;
    view.fetchQueued = false;
    if (moreFailed) view.load = Load::Failed;
    else if (!WeReadHttpClient::networkReady()) view.load = Load::Offline;
    else view.load = Load::Empty;
    return true;
  }
  if (view.load == Load::Loading && moreFailed) view.load = Load::Failed;
  else if (view.load == Load::Loading) view.load = Load::Ready;
  view.fetchQueued = false;
  return true;
}

bool pumpPanel() {
  Panel& view = panel();
  if (!view.open || view.load != Load::Loading || view.fetchQueued) return false;
  if (moreState.load() != kMoreIdle) return false;
  if (!WeReadHttpClient::networkReady()) {
    view.load = Load::Offline;
    return true;
  }
  view.fetchQueued = queueMore(view);
  return false;
}

void drawForBook(GfxRenderer& renderer, const int fontId, const Page& page, const int xOffset, const int yOffset,
                 const char* bookId, const Epub& epub, const int spineIndex) {
  if (!WeReadComment::isStandardBook(bookId)) return;
  PageComments& comments = pageComments();
  if (!sameChapter(comments, bookId, spineIndex)) loadPageComments(bookId, epub, spineIndex);
  if (!comments.valid || comments.count == 0) return;
  drawRanges(renderer, fontId, page, xOffset, yOffset, comments);
}

bool openAt(const int x, const int y) {
  if (panel().open) return false;
  const PageComments& comments = pageComments();
  if (!comments.valid) return false;
  for (size_t i = 0; i < comments.segmentCount; ++i) {
    const HitSegment& segment = comments.segments[i];
    if (segment.rangeIndex >= comments.count || segment.w <= 0 || segment.h <= 0) continue;
    if (x < segment.x || x >= segment.x + segment.w || y < segment.y || y >= segment.y + segment.h) continue;
    Panel& view = panel();
    view = {};
    view.open = true;
    strncpy(view.bookId, comments.bookId, sizeof(view.bookId) - 1);
    view.chapterUid = comments.chapterUid;
    view.start = comments.ranges[segment.rangeIndex].nativeStart;
    view.end = comments.ranges[segment.rangeIndex].nativeEnd;
    // 下一帧先把阅读页画进帧缓冲，再盖弹窗。不能直接画在灰阶残帧上。
    showRange(view);
    return true;
  }
  return false;
}

bool commentInside(const uint32_t visibleStart, const uint32_t visibleEnd, const int x, const int y,
                   uint32_t& nativeStart, uint32_t& nativeEnd) {
  nativeStart = 0;
  nativeEnd = 0;
  const PageComments& comments = pageComments();
  if (!comments.valid || visibleEnd <= visibleStart) return false;
  const DrawRange* chosen = nullptr;
  for (size_t i = 0; i < comments.segmentCount; ++i) {
    const HitSegment& segment = comments.segments[i];
    if (segment.rangeIndex >= comments.count || segment.w <= 0 || segment.h <= 0) continue;
    if (x < segment.x || x >= segment.x + segment.w || y < segment.y || y >= segment.y + segment.h) continue;
    const DrawRange& range = comments.ranges[segment.rangeIndex];
    if (visibleStart < range.end && range.start < visibleEnd && range.nativeEnd > range.nativeStart) {
      chosen = &range;
      break;
    }
  }
  if (!chosen) {
    for (size_t i = 0; i < comments.count; ++i) {
      const DrawRange& range = comments.ranges[i];
      if (range.nativeEnd <= range.nativeStart) continue;
      if (visibleStart < range.end && range.start < visibleEnd) {
        chosen = &range;
        break;
      }
    }
  }
  if (!chosen) return false;
  nativeStart = chosen->nativeStart;
  nativeEnd = chosen->nativeEnd;
  return nativeEnd > nativeStart;
}

bool openStored(const uint32_t nativeStart, const uint32_t nativeEnd) {
  const PageComments& comments = pageComments();
  if (!comments.valid || nativeEnd <= nativeStart || comments.bookId[0] == '\0') return false;
  Panel& view = panel();
  view = {};
  view.open = true;
  strncpy(view.bookId, comments.bookId, sizeof(view.bookId) - 1);
  view.chapterUid = comments.chapterUid;
  view.start = nativeStart;
  view.end = nativeEnd;
  showRange(view);
  return true;
}

bool panelOpen() { return panel().open; }

bool replacesPage() { return panel().open && panel().full; }

bool paintsOverPage() { return panel().open && !panel().full && underlayReady; }

void markPageUnderlay() { underlayReady = true; }

bool handlePanel(MappedInputManager& input, const GfxRenderer& renderer) {
  Panel& view = panel();
  if (!view.open) return false;
  (void)renderer;
  const auto swipe = input.wasSwipe();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool down = swipe == MappedInputManager::SwipeDir::Up ||
                    input.wasReleased(MappedInputManager::Button::Down) ||
                    input.wasReleased(MappedInputManager::Button::PageForward) || input.wasReleased(nextButton);
  const bool up = swipe == MappedInputManager::SwipeDir::Down || input.wasReleased(MappedInputManager::Button::Up) ||
                  input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton);
  if (view.full) {
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      view.full = false;
      underlayReady = false;
      return true;
    }
    int x = 0;
    int y = 0;
    if (input.wasScreenTapped(x, y) && y < 48) {
      view.full = false;
      underlayReady = false;
      return true;
    }
    if (down) return pageFullText(view, true);
    if (up) return pageFullText(view, false);
    return false;
  }
  if (input.wasReleased(MappedInputManager::Button::Back)) {
    view.open = false;
    view.full = false;
    underlayReady = true;
    markPanel(false);
    return true;
  }
  // 第一帧还没画出弹窗，点空白不能当成点在外面。
  if (view.cardW <= 0) return false;
  int x = 0;
  int y = 0;
  if (input.wasScreenTapped(x, y)) {
    if (x < view.cardX || x >= view.cardX + view.cardW || y < view.cardY || y >= view.cardY + view.cardH) {
      view.open = false;
      view.full = false;
      underlayReady = true;
      markPanel(false);
      return true;
    }
    for (uint8_t i = 0; i < view.hitCount; ++i) {
      const BoxHit& hit = view.hits[i];
      if (hit.kind != 2 || !hit.truncated || hit.w <= 0 || hit.h <= 0) continue;
      if (x < hit.x || x >= hit.x + hit.w || y < hit.y || y >= hit.y + hit.h) continue;
      if (!readPanelText(view, hit.index)) return false;
      view.full = true;
      view.index = hit.index;
      view.window = 0;
      view.shown = 0;
      view.total = 0;
      underlayReady = false;
      return true;
    }
    return false;
  }
  if (down) return scrollComments(view, true);
  if (up) return scrollComments(view, false);
  return false;
}

// 界面短句留在内置小号。引用、作者和评论走当前阅读字体：完整字库装上之后，
// 常用字表以外的字和正文是同一套字形，不再画成方框。
int proseFontId(const GfxRenderer& renderer) {
  const int readerId = SETTINGS.getReaderFontId();
  if (readerId != 0 && renderer.getFontMap().count(readerId) != 0) return readerId;
  return UI_10_FONT_ID;
}

void warmProse(const GfxRenderer& renderer, const int fontId, const char* text) {
  if (!text || text[0] == '\0') return;
  if (FontCacheManager* cache = renderer.getFontCacheManager()) {
    cache->prewarmCache(fontId, text, 0x01, true);
  }
  renderer.ensureSdCardFontReady(fontId, text, 0x01);
}

void drawFullPage(GfxRenderer& renderer, Panel& view) {
  const int uiFont = UI_10_FONT_ID;
  const int proseFont = proseFontId(renderer);
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int margin = 16;
  const int uiHeight = renderer.getLineHeight(uiFont);
  const int proseHeight = renderer.getLineHeight(proseFont);
  renderer.fillRect(0, 0, width, height, false);
  renderer.drawText(uiFont, margin, margin, tr(STR_BACK));
  int y = margin + uiHeight + 8;
  warmProse(renderer, proseFont, view.comment.author);
  warmProse(renderer, proseFont, view.text);
  if (view.comment.author[0] && proseHeight > 0) {
    renderer.drawText(proseFont, margin, y, view.comment.author);
    y += proseHeight + 6;
  }
  uint32_t shown = 0;
  const int bottom = height - margin;
  drawWrapped(renderer, proseFont, view.text, view.window, margin, y, width - margin * 2, bottom, shown);
  view.shown = shown;
  view.total = countCodepoints(view.text);
}

struct StatsHit {
  uint32_t start = 0;
  uint32_t end = 0;
  bool found = false;
  WeReadComment::Candidate item;
};

bool matchStats(void* raw, const uint32_t, const WeReadComment::Candidate& item) {
  auto* hit = static_cast<StatsHit*>(raw);
  if (item.start != hit->start || item.end != hit->end) return true;
  hit->found = true;
  hit->item = item;
  return false;
}

void passageStats(const Panel& view, uint32_t& marks, uint32_t& comments) {
  comments = view.comment.count;
  marks = 0;
  StatsHit hit;
  hit.start = view.start;
  hit.end = view.end;
  if (!WeReadComment::visitCandidates(view.bookId, view.chapterUid, matchStats, &hit) || !hit.found) return;
  marks = hit.item.markCount;
  if (hit.item.commentTotal > comments) comments = hit.item.commentTotal;
}

template <typename LineFn>
void forWrappedLines(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth,
                     LineFn onLine) {
  const char* cursor = text ? text : "";
  while (*cursor) {
    char line[180];
    size_t bytes = 0;
    const unsigned char* probe = reinterpret_cast<const unsigned char*>(cursor);
    while (*probe) {
      const unsigned char* mark = probe;
      if (utf8NextCodepoint(&probe) == 0) break;
      const size_t nextBytes = bytes + static_cast<size_t>(probe - mark);
      if (nextBytes >= sizeof(line)) break;
      memcpy(line + bytes, mark, static_cast<size_t>(probe - mark));
      line[nextBytes] = '\0';
      if (renderer.getTextAdvanceX(fontId, line, EpdFontFamily::REGULAR) > maxWidth) break;
      bytes = nextBytes;
    }
    if (bytes == 0) break;
    line[bytes] = '\0';
    if (!onLine(line)) return;
    cursor += bytes;
  }
}

void paintRoundBox(GfxRenderer& renderer, const int x, const int y, const int width, const int height, const int radius,
                   const int stroke) {
  renderer.fillRoundedRect(x, y, width, height, radius, Color::White);
  renderer.drawRoundedRect(x, y, width, height, stroke, radius, true);
}

void drawPanel(GfxRenderer& renderer) {
  Panel& view = panel();
  if (!view.open) return;
  if (view.full) {
    drawFullPage(renderer, view);
    return;
  }
  // 「返回」、人数、全文这些界面短句用小号。引用和评论用阅读字体。
  const int fontId = UI_10_FONT_ID;
  const int proseId = proseFontId(renderer);
  const int lineHeight = renderer.getLineHeight(fontId);
  const int proseHeight = renderer.getLineHeight(proseId);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  constexpr int side = 16;
  constexpr int pad = 12;
  constexpr int cardRadius = 12;
  const int topInset = lineHeight + 8;
  view.cardX = side;
  view.cardY = static_cast<int16_t>(topInset);
  view.cardW = static_cast<int16_t>(screenW - side * 2);
  view.cardH = static_cast<int16_t>(screenH - topInset - side);
  view.hitCount = 0;
  if (lineHeight <= 0 || proseHeight <= 0 || view.cardW < 40 || view.cardH < 40) return;

  paintRoundBox(renderer, view.cardX, view.cardY, view.cardW, view.cardH, cardRadius, 1);

  const int innerX = view.cardX + pad;
  const int innerW = view.cardW - pad * 2;
  const int top = view.cardY + pad;
  const int bottom = view.cardY + view.cardH - pad;
  const int viewH = bottom - top;
  const int quoteX = innerX + 10;
  const int quoteW = innerW - 10;
  const int smallHeight = lineHeight;
  int contentY = 0;
  view.nextScroll = -1;
  int overlap = std::max(lineHeight, proseHeight) * 2;
  if (overlap > viewH / 4) overlap = viewH / 4;
  int overlapRow = -1;

  auto place = [&](const int height, const auto& draw) -> bool {
    const int row = contentY;
    contentY += height;
    if (row + height <= view.scrollY) return false;
    if (row < view.scrollY || row + height > view.scrollY + viewH) {
      if (view.nextScroll < 0) {
        int target = overlapRow > view.scrollY ? overlapRow : row;
        if (target <= view.scrollY) target = row + height;
        view.nextScroll = target;
      }
      return false;
    }
    draw(top + row - view.scrollY);
    if (row > view.scrollY && row <= view.scrollY + viewH - overlap) overlapRow = row;
    return true;
  };

  int quoteTop = -1;
  int quoteBottom = -1;
  warmProse(renderer, proseId, view.quote);
  forWrappedLines(renderer, proseId, view.quote, quoteW, [&](const char* line) {
    place(proseHeight, [&](const int y) {
      if (quoteTop < 0) quoteTop = y;
      quoteBottom = y + proseHeight;
      renderer.drawText(proseId, quoteX, y, line);
    });
    return true;
  });
  if (quoteTop >= 0 && quoteBottom > quoteTop) {
    renderer.fillRect(innerX, quoteTop, 2, quoteBottom - quoteTop, true);
  }

  if (view.comment.count == 0) {
    const char* status = tr(STR_WEREAD_HIGHLIGHT_NO_COMMENTS);
    switch (view.load) {
      case Load::Loading:
        status = tr(STR_LOADING_POPUP);
        break;
      case Load::Failed:
        status = tr(STR_WEREAD_COMMENTS_FAILED);
        break;
      case Load::Offline:
        status = tr(STR_WEREAD_PASSAGE_COMMENT_OFFLINE);
        break;
      case Load::Empty:
      case Load::Ready:
        break;
    }
    place(8, [](const int) {});
    place(smallHeight, [&](const int y) { renderer.drawText(fontId, innerX, y, status); });
    return;
  }

  uint32_t marks = 0;
  uint32_t comments = 0;
  passageStats(view, marks, comments);
  char stats[64];
  snprintf(stats, sizeof(stats), tr(STR_WEREAD_PASSAGE_STATS), marks, comments);
  place(4, [](const int) {});
  place(smallHeight, [&](const int y) { renderer.drawText(fontId, innerX, y, stats); });
  place(10, [&](const int y) { renderer.fillRect(innerX, y + 4, innerW, 1, true); });

  constexpr int kPreviewLines = 4;
  const char* fullLabel = tr(STR_WEREAD_PASSAGE_FULL);
  const int fullWidth = renderer.getTextAdvanceX(fontId, fullLabel, EpdFontFamily::REGULAR);
  bool anyComment = false;
  for (uint32_t index = 0; index < view.comment.count && view.nextScroll < 0; ++index) {
    if (!readPanelText(view, index)) continue;
    warmProse(renderer, proseId, view.comment.author);
    warmProse(renderer, proseId, view.text);
    if (anyComment) {
      place(10, [&](const int y) { renderer.fillRect(innerX, y + 4, innerW, 1, true); });
    }
    if (view.nextScroll >= 0) break;
    anyComment = true;

    int bodyLines = 0;
    forWrappedLines(renderer, proseId, view.text, innerW, [&](const char*) {
      ++bodyLines;
      return true;
    });
    const bool truncated = bodyLines > kPreviewLines;
    int hitTop = -1;
    int hitBottom = -1;
    int shown = 0;
    if (view.comment.author[0] || truncated) {
      const int rowHeight = view.comment.author[0] ? proseHeight : lineHeight;
      place(rowHeight, [&](const int y) {
        if (hitTop < 0) hitTop = y;
        hitBottom = y + rowHeight;
        int authorRight = innerX + innerW;
        if (truncated) {
          authorRight = innerX + innerW - fullWidth - 8;
          const int labelY = y + std::max(0, (rowHeight - smallHeight) / 2);
          renderer.drawText(fontId, innerX + innerW - fullWidth, labelY, fullLabel);
        }
        if (view.comment.author[0] &&
            renderer.getTextAdvanceX(proseId, view.comment.author, EpdFontFamily::REGULAR) <= authorRight - innerX) {
          renderer.drawText(proseId, innerX, y, view.comment.author);
        }
      });
    }
    forWrappedLines(renderer, proseId, view.text, innerW, [&](const char* line) {
      if (shown >= kPreviewLines || view.nextScroll >= 0) return false;
      place(proseHeight, [&](const int y) {
        if (hitTop < 0) hitTop = y;
        hitBottom = y + proseHeight;
        renderer.drawText(proseId, innerX, y, line);
      });
      ++shown;
      return view.nextScroll < 0 && shown < kPreviewLines;
    });
    if (truncated && hitTop >= 0 && hitBottom > hitTop && view.hitCount < 12) {
      BoxHit& hit = view.hits[view.hitCount++];
      hit = {};
      hit.x = static_cast<int16_t>(innerX);
      hit.y = static_cast<int16_t>(hitTop);
      hit.w = static_cast<int16_t>(innerW);
      hit.h = static_cast<int16_t>(hitBottom - hitTop);
      hit.kind = 2;
      hit.truncated = 1;
      hit.index = static_cast<uint16_t>(index > 0xFFFF ? 0xFFFF : index);
    }
    place(4, [](const int) {});
  }
  if (view.nextScroll < 0 && !view.comment.hasMore && view.load != Load::Loading && view.load != Load::Failed) {
    const char* endMark = tr(STR_WEREAD_PASSAGE_COMMENT_END);
    place(8, [](const int) {});
    place(smallHeight, [&](const int y) {
      const int markWidth = renderer.getTextAdvanceX(fontId, endMark, EpdFontFamily::REGULAR);
      const int x = innerX + (innerW > markWidth ? (innerW - markWidth) / 2 : 0);
      renderer.drawText(fontId, x, y, endMark);
    });
  } else if (view.nextScroll < 0) {
    const char* hint = tr(STR_WEREAD_PASSAGE_COMMENT_MORE);
    if (view.load == Load::Loading) hint = tr(STR_LOADING_POPUP);
    else if (view.load == Load::Failed) hint = tr(STR_WEREAD_COMMENTS_FAILED);
    else if (view.load == Load::Offline || !WeReadHttpClient::networkReady())
      hint = tr(STR_WEREAD_PASSAGE_COMMENT_OFFLINE);
    place(smallHeight, [&](const int y) { renderer.drawText(fontId, innerX, y, hint); });
  }
}

void reset() {
  char abandonedId[64] = {};
  uint32_t abandonedUid = 0;
  bool hold = false;
  lockState();
  if (round().holdCommit && job().bookId[0] != '\0') {
    strncpy(abandonedId, job().bookId, sizeof(abandonedId) - 1);
    abandonedUid = job().chapterUid;
    hold = true;
  }
  const uint32_t generation = job().generation + 1;
  round() = {};
  job() = {};
  job().generation = generation;
  // 提交或拉取进行中不删暂存，理由同 cancelManualRefresh。
  const bool busy = committing.load() || workerBusy.load();
  unlockState();
  if (hold && !busy) WeReadComment::discardStagedChapter(abandonedId, abandonedUid);
  moreState.store(kMoreIdle);
  markPanel(false);
  underlayReady = true;
  resetQuotes();
  pageComments() = {};
  pageComments().spineIndex = -1;
  panel() = {};
}

}  // namespace WeReadCommentOverlay
