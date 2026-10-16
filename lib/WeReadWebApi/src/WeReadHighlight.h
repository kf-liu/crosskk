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

std::string indexPath(const char* bookId);
bool clearAll();
bool loadChapter(const char* bookId, uint32_t chapterUid, Range* ranges, size_t capacity, size_t& count,
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
