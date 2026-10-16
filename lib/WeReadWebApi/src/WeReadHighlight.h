#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace WeReadHighlight {

constexpr uint32_t kMagic = 0x31485257;  // WRH1
constexpr uint16_t kVersion = 1;
constexpr uint32_t kMaxRecords = 4096;
constexpr size_t kMaxChapterRanges = 64;

enum Flag : uint8_t {
  Mine = 1U << 0,
};

struct Mark {
  char bookmarkId[32] = {};
  uint32_t chapterUid = 0;
  uint32_t rangeStart = 0;
  uint32_t rangeEnd = 0;
  uint8_t flags = 0;
  uint8_t style = 0;
  uint16_t reserved = 0;
};
static_assert(sizeof(Mark) == 48, "highlight mark size is part of the on-disk format");

struct Header {
  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t recordSize = sizeof(Mark);
  uint32_t count = 0;
  uint32_t bookVersion = 0;
  uint64_t syncKey = 0;
  char ownerVid[64] = {};
};
static_assert(sizeof(Header) == 88, "highlight header size is part of the on-disk format");
static_assert(offsetof(Header, syncKey) % 8 == 0, "syncKey must stay 8-byte aligned");

struct Range {
  uint32_t start = 0;
  uint32_t end = 0;
};

struct ChapterMark {
  char bookmarkId[32] = {};
  uint32_t start = 0;
  uint32_t end = 0;
};

bool parseRange(const char* text, uint32_t& start, uint32_t& end);
bool parseChapterUid(const char* text, uint32_t& chapterUid);
bool rangesOverlap(uint32_t start, uint32_t end, uint32_t otherStart, uint32_t otherEnd);

enum class RefreshResult : uint8_t {
  Skipped,
  Updated,
  Failed,
};

// 把 bookmarklist 的 JSON 流写成 marks.bin。
// 解析器在堆上，避免把 2KB 词法缓冲放进阅读任务栈。
class BookmarkListImporter {
 public:
  BookmarkListImporter();
  ~BookmarkListImporter();
  BookmarkListImporter(const BookmarkListImporter&) = delete;
  BookmarkListImporter& operator=(const BookmarkListImporter&) = delete;

  bool begin(const char* bookId, const char* ownerVid);
  bool feed(const uint8_t* data, size_t len);
  bool finish();
  void abort();
  int errorCode() const;

 private:
  struct State;
  State* state_;

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);
  static void acceptValue(State& state, const char* value, size_t len);
  static void startRecord(State& state);
  static void finishRecord(State& state);
};

bool replaceMineFromBookmarkList(const char* bookId, const char* ownerVid, const uint8_t* json, size_t len);

// 已有网络且已登录时拉取一次「我的划线」。不打开 Wi-Fi。失败时保留上一份 marks.bin。
RefreshResult refreshMine(const char* bookId);

// 从已有索引里去掉一条划线。找不到或写失败时不替换原文件。
bool removeMine(const char* bookId, const char* bookmarkId);
// 解析 removeBookmark 的响应。succ 为 1 且没有错误码才算成功。
bool removalSucceeded(const uint8_t* json, size_t len);
// 已有网络时删除服务器上的划线，成功后再改本地索引。不打开 Wi-Fi。
RefreshResult deleteMine(const char* bookId, const char* bookmarkId);

// 在已有索引末尾加一条。原文件损坏或写失败时不替换 marks.bin。
bool insertMine(const char* bookId, const Mark& mark);
// addBookmark 成功时写出 data.bookmarkId。有非零错误码、或 id 不能进索引时失败。
bool creationBookmarkId(const uint8_t* json, size_t len, char* bookmarkId, size_t bookmarkIdSize);
// 已有网络时提交一条划线，服务器返回 bookmarkId 后再写入本地索引。不打开 Wi-Fi。
// visible 区间是阅读页的可见偏移，含段落空白；函数内部换成 wr-co。
RefreshResult createMine(const char* bookId, const char* chapterPath, uint32_t chapterUid, uint32_t chapterIdx,
                         uint32_t visibleStart, uint32_t visibleEnd);

std::string indexPath(const char* bookId);
bool clearAll();
bool loadChapter(const char* bookId, uint32_t chapterUid, Range* ranges, size_t capacity, size_t& count,
                 bool& truncated);
bool loadChapterMarks(const char* bookId, uint32_t chapterUid, ChapterMark* marks, size_t capacity, size_t& count,
                      bool& truncated);

class Writer {
 public:
  bool begin(const char* bookId);
  bool append(const Mark& mark);
  bool finish(uint64_t syncKey, uint32_t bookVersion, const char* ownerVid);
  void abort();
  bool active() const { return active_; }
  bool truncated() const { return truncated_; }

 private:
  HalFile file_;
  std::string finalPath_;
  std::string partPath_;
  uint32_t count_ = 0;
  bool active_ = false;
  bool truncated_ = false;
};

}  // namespace WeReadHighlight
