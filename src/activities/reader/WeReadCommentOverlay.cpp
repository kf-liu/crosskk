#include "WeReadCommentOverlay.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <MappedInputManager.h>
#include <Utf8.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "WeReadClient.h"
#include "WeReadComment.h"
#include "WeReadHighlight.h"
#include "WeReadHttpClient.h"
#include "WeReadStore.h"
#include "WeReadXhtmlCodec.h"

namespace WeReadCommentOverlay {
namespace {

enum class Query : uint8_t { Unknown, Empty, Stored, Failed };

struct Round {
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  bool haveChapter = false;
  bool underlinesReady = false;
  bool underlinesFailed = false;
  bool sessionMissing = false;
  int settledSpine = -1;
  int settledPage = -1;
  int lastSpine = -1;
  int lastPage = -1;
  size_t count = 0;
  WeReadComment::Range ranges[WeReadComment::kMaxChapterRanges] = {};
  Query query[WeReadComment::kMaxChapterRanges] = {};
};

Round& round() {
  // 一个阅读任务。候选 range 留在静态区，不进 8KB 栈，也不每帧分配。
  static Round state;
  return state;
}

struct HitSegment {
  uint8_t rangeIndex = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
};

constexpr size_t kMaxHitSegments = 96;

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
  DrawRange ranges[WeReadComment::kMaxChapterRanges] = {};
  HitSegment segments[kMaxHitSegments] = {};
};

PageComments& pageComments() {
  static PageComments comments;
  return comments;
}

struct Panel {
  bool open = false;
  char bookId[64] = {};
  uint32_t chapterUid = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t index = 0;
  uint32_t window = 0;
  uint32_t shown = 0;
  uint32_t total = 0;
  int fontId = 0;
  WeReadComment::StoredComment comment;
  // 一条评论最长 2KB。静态缓冲给灰阶各遍复用，不放进阅读栈。
  char text[WeReadComment::kMaxCommentBytes] = {};
};

Panel& panel() {
  static Panel state;
  return state;
}

bool hiddenThisRound(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end) {
  const Round& state = round();
  if (!state.haveChapter || state.chapterUid != chapterUid ||
      strncmp(state.bookId, bookId, sizeof(state.bookId)) != 0) {
    return false;
  }
  for (size_t i = 0; i < state.count; ++i) {
    if (state.query[i] == Query::Empty && state.ranges[i].start == start && state.ranges[i].end == end) return true;
  }
  return false;
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

void beginChapter(Round& state, const char* bookId, const uint32_t chapterUid) {
  const int lastSpine = state.lastSpine;
  const int lastPage = state.lastPage;
  const bool sessionMissing = state.sessionMissing;
  state = {};
  state.settledSpine = -1;
  state.settledPage = -1;
  state.lastSpine = lastSpine;
  state.lastPage = lastPage;
  state.sessionMissing = sessionMissing;
  strncpy(state.bookId, bookId, sizeof(state.bookId) - 1);
  state.chapterUid = chapterUid;
  state.haveChapter = true;
}

bool loadPanelComment(Panel& view, const uint32_t index) {
  WeReadComment::StoredComment comment;
  if (!WeReadComment::readComment(view.bookId, view.chapterUid, view.start, view.end, index, view.text,
                                  sizeof(view.text), comment)) {
    return false;
  }
  view.comment = comment;
  view.index = index;
  view.window = 0;
  view.shown = 0;
  view.total = 0;
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

bool advancePanel(Panel& view) {
  if (view.window + view.shown < view.total) {
    view.window += view.shown > 0 ? view.shown : 1;
    return true;
  }
  if (view.index + 1 < view.comment.count) return loadPanelComment(view, view.index + 1);
  if (!view.comment.hasMore || !WeReadHttpClient::networkReady()) return false;
  char rangeText[32];
  const int length = snprintf(rangeText, sizeof(rangeText), "%u-%u", view.start, view.end);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(rangeText)) return false;
  FetchCtx ctx;
  ctx.bookId = view.bookId;
  ctx.chapterUid = view.chapterUid;
  ctx.rangeText = rangeText;
  const uint32_t previous = view.comment.count;
  const auto result =
      WeReadComment::collectRange(view.bookId, view.chapterUid, view.start, view.end, view.comment.maxIdx,
                                  view.comment.syncKey, true, fetchPage, &ctx);
  if (result != WeReadComment::CollectResult::Stored) return false;
  if (!loadPanelComment(view, previous)) return loadPanelComment(view, previous > 0 ? previous - 1 : 0);
  return true;
}

bool retreatPanel(Panel& view) {
  if (view.window > 0) {
    view.window = view.window > view.shown && view.shown > 0 ? view.window - view.shown : 0;
    return true;
  }
  if (view.index == 0) return false;
  return loadPanelComment(view, view.index - 1);
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
  if (walk.comments->segmentCount < kMaxHitSegments && top < lineY + 4) {
    HitSegment& segment = walk.comments->segments[walk.comments->segmentCount++];
    segment.rangeIndex = static_cast<uint8_t>(coveredIndex);
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

  WeReadComment::Range native[WeReadComment::kMaxChapterRanges];
  size_t nativeCount = 0;
  bool truncated = false;
  if (!WeReadComment::loadChapter(bookId, chapterUid, native, WeReadComment::kMaxChapterRanges, nativeCount,
                                  truncated)) {
    return;
  }
  size_t kept = 0;
  for (size_t i = 0; i < nativeCount; ++i) {
    if (hiddenThisRound(bookId, chapterUid, native[i].start, native[i].end)) continue;
    comments.ranges[kept].nativeStart = native[i].start;
    comments.ranges[kept].nativeEnd = native[i].end;
    comments.ranges[kept].start = native[i].start;
    comments.ranges[kept].end = native[i].end;
    ++kept;
  }
  comments.count = kept;
  if (kept == 0) {
    comments.valid = true;
    return;
  }
  static uint32_t nativeOffsets[WeReadComment::kMaxChapterRanges * 2];
  static uint32_t visibleOffsets[WeReadComment::kMaxChapterRanges * 2];
  for (size_t i = 0; i < kept; ++i) {
    nativeOffsets[i * 2] = comments.ranges[i].nativeStart;
    nativeOffsets[i * 2 + 1] = comments.ranges[i].nativeEnd;
  }
  if (!WeReadXhtmlCodec::nativeOffsetsToVisible(path, nativeOffsets, visibleOffsets, kept * 2)) {
    LOG_ERR("WR", "评论范围无法对到章节");
    comments.count = 0;
    return;
  }
  size_t visibleCount = 0;
  for (size_t i = 0; i < kept; ++i) {
    const uint32_t start = visibleOffsets[i * 2];
    const uint32_t end = visibleOffsets[i * 2 + 1];
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

}  // namespace

bool syncPage(const char* bookId, const Epub& epub, const int spineIndex, const int pageNumber,
              const uint32_t pageStart, const uint32_t pageEnd) {
  if (panel().open || !WeReadComment::isStandardBook(bookId)) return false;
  Round& state = round();
  const bool backward = state.lastSpine >= 0 && (spineIndex < state.lastSpine ||
                                                  (spineIndex == state.lastSpine && pageNumber < state.lastPage));
  state.lastSpine = spineIndex;
  state.lastPage = pageNumber;
  if (backward) return false;
  if (state.sessionMissing) return false;
  if (state.settledSpine == spineIndex && state.settledPage == pageNumber &&
      (state.underlinesReady || state.underlinesFailed)) {
    return false;
  }
  if (!WeReadHttpClient::networkReady()) return false;

  uint32_t tocIndex = 0;
  uint32_t chapterUid = 0;
  std::string path;
  if (!lookupChapter(bookId, epub, spineIndex, tocIndex, chapterUid, path)) return false;
  if (!state.haveChapter || state.chapterUid != chapterUid ||
      strncmp(state.bookId, bookId, sizeof(state.bookId)) != 0) {
    beginChapter(state, bookId, chapterUid);
  }
  if (state.underlinesFailed) {
    state.settledSpine = spineIndex;
    state.settledPage = pageNumber;
    return false;
  }
  if (!state.underlinesReady) {
    size_t count = 0;
#ifdef ENABLE_CHINESE_VERSION
    const bool fetched = WeReadClient::fetchCommentUnderlines(bookId, chapterUid, state.ranges,
                                                              WeReadComment::kMaxChapterRanges, count);
#else
    const bool fetched = false;
    (void)count;
#endif
    if (!fetched) {
      if (!WeReadHttpClient::networkReady()) return false;
      WeReadStore::Session session;
      if (!WeReadStore::loadSession(session) || !session.valid()) state.sessionMissing = true;
      else state.underlinesFailed = true;
      state.settledSpine = spineIndex;
      state.settledPage = pageNumber;
      return false;
    }
    state.count = count;
    for (size_t i = 0; i < count; ++i) state.query[i] = Query::Unknown;
    state.underlinesReady = true;
  }

  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  const bool bounds = pageNativeBounds(path, pageStart, pageEnd, nativeStart, nativeEnd);
  bool changed = false;
  if (bounds) {
    for (size_t i = 0; i < state.count; ++i) {
      if (state.query[i] != Query::Unknown) continue;
      if (!WeReadHighlight::rangesOverlap(nativeStart, nativeEnd, state.ranges[i].start, state.ranges[i].end)) continue;
      char rangeText[32];
      const int length =
          snprintf(rangeText, sizeof(rangeText), "%u-%u", state.ranges[i].start, state.ranges[i].end);
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(rangeText)) {
        state.query[i] = Query::Failed;
        continue;
      }
      FetchCtx ctx;
      ctx.bookId = bookId;
      ctx.chapterUid = chapterUid;
      ctx.rangeText = rangeText;
      switch (WeReadComment::collectRange(bookId, chapterUid, state.ranges[i].start, state.ranges[i].end, 0, 0, false,
                                           fetchPage, &ctx)) {
        case WeReadComment::CollectResult::Empty:
          state.query[i] = Query::Empty;
          changed = true;
          break;
        case WeReadComment::CollectResult::Stored:
          state.query[i] = Query::Stored;
          changed = true;
          break;
        case WeReadComment::CollectResult::Failed:
          state.query[i] = Query::Failed;
          break;
      }
    }
  }
  state.settledSpine = spineIndex;
  state.settledPage = pageNumber;
  if (changed) pageComments().valid = false;
  return changed;
}

void drawForBook(GfxRenderer& renderer, const int fontId, const Page& page, const int xOffset, const int yOffset,
                 const char* bookId, const Epub& epub, const int spineIndex) {
  panel().fontId = fontId;
  if (!WeReadComment::isStandardBook(bookId)) return;
  PageComments& comments = pageComments();
  if (!sameChapter(comments, bookId, spineIndex)) loadPageComments(bookId, epub, spineIndex);
  if (!comments.valid || comments.count == 0) return;
  drawRanges(renderer, fontId, page, xOffset, yOffset, comments);
}

bool openAt(const int x, const int y) {
  const int fontId = panel().fontId;
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
    view.fontId = fontId;
    strncpy(view.bookId, comments.bookId, sizeof(view.bookId) - 1);
    view.chapterUid = comments.chapterUid;
    view.start = comments.ranges[segment.rangeIndex].nativeStart;
    view.end = comments.ranges[segment.rangeIndex].nativeEnd;
    if (!loadPanelComment(view, 0)) {
      view.open = false;
      return false;
    }
    return true;
  }
  return false;
}

bool panelOpen() { return panel().open; }

bool handlePanel(MappedInputManager& input, const GfxRenderer& renderer) {
  Panel& view = panel();
  if (!view.open) return false;
  int x = 0;
  int y = 0;
  if (input.wasScreenTapped(x, y)) {
    if (y < 48) {
      view.open = false;
      return true;
    }
    return advancePanel(view);
  }
  if (input.wasReleased(MappedInputManager::Button::Back)) {
    view.open = false;
    return true;
  }
  if (input.wasReleased(MappedInputManager::Button::PageForward)) return advancePanel(view);
  if (input.wasReleased(MappedInputManager::Button::PageBack)) return retreatPanel(view);
  (void)renderer;
  return false;
}

void drawPanel(GfxRenderer& renderer) {
  Panel& view = panel();
  if (!view.open) return;
  const int fontId = view.fontId;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int margin = 16;
  renderer.fillRect(0, 0, width, height, false);
  renderer.drawText(fontId, margin, margin, tr(STR_BACK));
  renderer.drawText(fontId, margin, margin + renderer.getLineHeight(fontId) + 8, tr(STR_WEREAD_PASSAGE_COMMENTS));
  char position[24];
  snprintf(position, sizeof(position), "%u/%u", view.index + 1, view.comment.count == 0 ? 1 : view.comment.count);
  const int positionWidth = renderer.getTextAdvanceX(fontId, position, EpdFontFamily::REGULAR);
  renderer.drawText(fontId, width - margin - positionWidth, margin + renderer.getLineHeight(fontId) + 8, position);
  int y = margin + (renderer.getLineHeight(fontId) + 8) * 2;
  if (view.comment.author[0]) {
    renderer.drawText(fontId, margin, y, view.comment.author);
    y += renderer.getLineHeight(fontId) + 6;
  }
  const int bottom = height - margin - renderer.getLineHeight(fontId) - 8;
  uint32_t shown = 0;
  drawWrapped(renderer, fontId, view.text, view.window, margin, y, width - margin * 2, bottom, shown);
  view.shown = shown;
  view.total = countCodepoints(view.text);
  if (view.comment.hasMore && view.index + 1 >= view.comment.count && view.window + view.shown >= view.total) {
    const char* hint = WeReadHttpClient::networkReady() ? tr(STR_WEREAD_PASSAGE_COMMENT_MORE)
                                                        : tr(STR_WEREAD_PASSAGE_COMMENT_OFFLINE);
    renderer.drawText(fontId, margin, height - margin - renderer.getLineHeight(fontId), hint);
  }
}

void reset() {
  round() = {};
  round().settledSpine = -1;
  round().settledPage = -1;
  round().lastSpine = -1;
  round().lastPage = -1;
  pageComments() = {};
  pageComments().spineIndex = -1;
  panel() = {};
}

}  // namespace WeReadCommentOverlay
