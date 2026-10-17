#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace WeReadComment {

constexpr uint32_t kMagic = 0x314E5257;  // WRN1
constexpr uint16_t kVersion = 1;
constexpr uint16_t kHeaderSize = 40;
// 一章候选索引，不设条数上限。长章整表在 SD 上，不进内存。
constexpr uint32_t kCandidateMagic = 0x31495257;  // WRI1
constexpr uint16_t kCandidateVersion = 1;
constexpr uint16_t kCandidateHeaderSize = 16;
constexpr uint32_t kMaxStoredComments = 200;
constexpr uint16_t kMaxCommentBytes = 2048;
constexpr uint8_t kMaxAuthorBytes = 47;
constexpr uint32_t kReviewPageSize = 20;
constexpr int kMaxReviewPages = 16;

constexpr uint32_t kFlagHasMore = 1U << 0;
constexpr uint32_t kFlagCapped = 1U << 1;

// 只服务标准微信读书书籍。公众号和普通 EPUB 不请求、不画。
bool isStandardBook(const char* bookId);

struct Range {
  uint32_t start = 0;
  uint32_t end = 0;
  // 这一段有多少人划过。只在内存里，不进 WRN1 文件。
  uint32_t markCount = 0;
};

struct Header {
  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t headerSize = kHeaderSize;
  uint32_t chapterUid = 0;
  uint32_t rangeStart = 0;
  uint32_t rangeEnd = 0;
  uint32_t commentCount = 0;
  uint32_t maxIdx = 0;
  uint32_t flags = 0;
  uint64_t syncKey = 0;
};
static_assert(sizeof(Header) == kHeaderSize, "comment file header is part of the on-disk format");
static_assert(offsetof(Header, syncKey) % 8 == 0, "syncKey must stay 8-byte aligned");

struct CommentHead {
  uint16_t authorLen = 0;
  uint16_t textLen = 0;
  uint8_t flags = 0;
  uint8_t reserved[3] = {};
};
static_assert(sizeof(CommentHead) == 8, "comment record head is part of the on-disk format");

struct Cursor {
  uint32_t maxIdx = 0;
  uint64_t syncKey = 0;
  uint32_t totalCount = 0;
  // readreviews 这一段的划线人数。underlines.count 为 0 时以它为准。
  uint32_t markCount = 0;
  bool hasMore = false;
  int errorCode = 0;
  uint32_t added = 0;
};

enum class CollectResult : uint8_t { Empty, Stored, Failed };

enum class CandidateQuery : uint8_t { Unknown = 0, Empty = 1, Stored = 2, Partial = 3, Failed = 4 };

struct CandidateHeader {
  uint32_t magic = kCandidateMagic;
  uint16_t version = kCandidateVersion;
  uint16_t headerSize = kCandidateHeaderSize;
  uint32_t chapterUid = 0;
  uint32_t count = 0;
};
static_assert(sizeof(CandidateHeader) == kCandidateHeaderSize, "candidate index header is 16 bytes");

struct Candidate {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t markCount = 0;
  uint32_t commentTotal = 0;
  CandidateQuery query = CandidateQuery::Unknown;
};

// 整章候选。追加写入，条数不设上限；重复 range 只抬高划线人数。
class CandidateFile {
 public:
  bool begin(const char* bookId, uint32_t chapterUid);
  bool add(uint32_t start, uint32_t end, uint32_t markCount);
  bool finish();
  void abort();
  ~CandidateFile();
  uint32_t count() const { return count_; }

 private:
  HalFile file_;
  std::string finalPath_;
  std::string partPath_;
  uint32_t chapterUid_ = 0;
  uint32_t count_ = 0;
  bool active_ = false;
};

bool candidatesReady(const char* bookId, uint32_t chapterUid);
// visit 返回 false 时停下。文件不存在视为空表，返回 true。
bool visitCandidates(const char* bookId, uint32_t chapterUid,
                     bool (*visit)(void* ctx, uint32_t index, const Candidate& item), void* ctx);
bool readCandidate(const char* bookId, uint32_t chapterUid, uint32_t index, Candidate& item);
bool updateCandidate(const char* bookId, uint32_t chapterUid, uint32_t index, const Candidate& item);
// 已经写过评论的片段，不设条数上限。
bool visitStoredRanges(const char* bookId, uint32_t chapterUid, bool (*visit)(void* ctx, uint32_t start, uint32_t end),
                       void* ctx);

// 流式写入一个 range 的评论。失败时 abort，不替换已有文件。
class RangeFile {
 public:
  bool begin(const char* bookId, uint32_t chapterUid, uint32_t start, uint32_t end, bool copyExisting);
  bool add(const char* author, const char* text, size_t textLen, bool truncated);
  bool finish(uint32_t maxIdx, uint64_t syncKey, bool hasMore);
  void abort();
  ~RangeFile();
  uint32_t count() const { return count_; }
  bool capped() const { return capped_; }
  bool active() const { return active_; }

 private:
  HalFile file_;
  std::string finalPath_;
  std::string partPath_;
  uint32_t chapterUid_ = 0;
  uint32_t start_ = 0;
  uint32_t end_ = 0;
  uint32_t count_ = 0;
  bool capped_ = false;
  bool active_ = false;
};

// 解析器放堆上，避免把 2KB 词法缓冲放进阅读任务栈。
class UnderlineParser {
 public:
  UnderlineParser();
  ~UnderlineParser();
  UnderlineParser(const UnderlineParser&) = delete;
  UnderlineParser& operator=(const UnderlineParser&) = delete;

  bool begin(Range* ranges, size_t capacity);
  // 不把整章收进数组。sink 返回 false 时解析失败。
  using CandidateSink = bool (*)(void* ctx, uint32_t start, uint32_t end, uint32_t markCount);
  bool begin(CandidateSink sink, void* ctx);
  bool feed(const uint8_t* data, size_t len);
  bool finish();
  size_t count() const;
  bool truncated() const;
  int errorCode() const;

 private:
  // 解析状态只在 cpp 里完整，避免把 JSON 词法缓冲带进头文件。
  void* state_;
};

class ReviewPageParser {
 public:
  ReviewPageParser();
  ~ReviewPageParser();
  ReviewPageParser(const ReviewPageParser&) = delete;
  ReviewPageParser& operator=(const ReviewPageParser&) = delete;

  bool begin(RangeFile* file, const char* rangeText);
  bool feed(const uint8_t* data, size_t len);
  bool finish(Cursor& cursor);

 private:
  void* state_;
};

using ReviewPageFetch = bool (*)(void* ctx, uint32_t maxIdx, uint64_t syncKey, RangeFile& file, Cursor& cursor);

// 按 maxIdx / synckey 翻页，直到没有下一页或达到存储上限。空结果不写文件。
// maxPages 限制这一次调用最多请求几页。阅读界面每次只拉一页，避免一次把主循环堵住。
CollectResult collectRange(const char* bookId, uint32_t chapterUid, uint32_t start, uint32_t end, uint32_t startMaxIdx,
                           uint64_t startSyncKey, bool copyExisting, ReviewPageFetch fetch, void* ctx,
                           int maxPages = kMaxReviewPages, uint32_t* totalCount = nullptr,
                           uint32_t* markCount = nullptr);

std::string rangePath(const char* bookId, uint32_t chapterUid, uint32_t start, uint32_t end);
bool loadChapter(const char* bookId, uint32_t chapterUid, Range* ranges, size_t capacity, size_t& count,
                 bool& truncated);

struct StoredComment {
  char author[kMaxAuthorBytes + 1] = {};
  uint32_t index = 0;
  uint32_t count = 0;
  uint32_t maxIdx = 0;
  uint64_t syncKey = 0;
  bool hasMore = false;
  uint16_t textLen = 0;
};

// text 由调用方提供。阅读界面用静态缓冲，不把评论正文放进任务栈。
bool readComment(const char* bookId, uint32_t chapterUid, uint32_t start, uint32_t end, uint32_t index, char* text,
                 size_t textSize, StoredComment& comment);

}  // namespace WeReadComment
