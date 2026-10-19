#include "WeReadComment.h"

#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "../../../src/util/StringUtils.h"
#include "WeReadHighlight.h"
#include "WeReadProtocol.h"
#include "WeReadStore.h"

namespace WeReadComment {
namespace {

constexpr char kRoot[] = "/.crosspoint/weread/comments";
constexpr uint8_t kTextTruncated = 1U << 0;

bool parseUnsigned(const char* text, const char* end, uint32_t& value, const char** next) {
  if (!text || text == end || *text < '0' || *text > '9') return false;
  uint32_t parsed = 0;
  const char* cursor = text;
  while (cursor != end && *cursor >= '0' && *cursor <= '9') {
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (parsed > (UINT32_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
    ++cursor;
  }
  if (cursor == text) return false;
  value = parsed;
  if (next) *next = cursor;
  return true;
}

uint32_t parseUint32(const char* value, const size_t len) {
  uint32_t parsed = 0;
  const char* next = nullptr;
  if (!parseUnsigned(value, value + len, parsed, &next) || next != value + len) return 0;
  return parsed;
}

uint64_t parseUint64(const char* value, const size_t len) {
  if (!value || len == 0) return 0;
  uint64_t result = 0;
  for (size_t i = 0; i < len; ++i) {
    if (value[i] < '0' || value[i] > '9') return 0;
    const uint64_t digit = static_cast<uint64_t>(value[i] - '0');
    if (result > (UINT64_MAX - digit) / 10) return 0;
    result = result * 10 + digit;
  }
  return result;
}

bool blank(const char* text, const size_t len) {
  if (!text || len == 0) return true;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return false;
  }
  return true;
}

const char* baseName(const char* name) {
  const char* base = name ? name : "";
  for (const char* cursor = base; *cursor; ++cursor) {
    if (*cursor == '/') base = cursor + 1;
  }
  return base;
}

bool parseRangeFileName(const char* name, uint32_t& start, uint32_t& end) {
  const char* file = baseName(name);
  const char* dot = strrchr(file, '.');
  if (!dot || strcmp(dot, ".wrn") != 0) return false;
  const char* sep = strchr(file, '_');
  if (!sep || sep > dot) return false;
  const char* tail = nullptr;
  return parseUnsigned(file, sep, start, &tail) && tail == sep && parseUnsigned(sep + 1, dot, end, &tail) &&
         tail == dot && end > start;
}

bool chapterDirectory(const char* bookId, const uint32_t chapterUid, std::string& directory, const bool create) {
  if (!isStandardBook(bookId)) return false;
  const std::string book = StringUtils::sanitizeFilename(bookId, 56);
  if (book.empty()) return false;
  char chapter[16];
  const int length = snprintf(chapter, sizeof(chapter), "%u", chapterUid);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(chapter)) return false;
  directory = std::string(kRoot) + "/" + book + "/" + chapter;
  if (!create) return true;
  const std::string bookDir = std::string(kRoot) + "/" + book;
  return Storage.ensureDirectoryExists(kRoot) && Storage.ensureDirectoryExists(bookDir.c_str()) &&
         Storage.ensureDirectoryExists(directory.c_str());
}

bool copyBlob(HalFile& input, HalFile& output, size_t bytes) {
  uint8_t buffer[128];
  while (bytes > 0) {
    const size_t chunk = bytes < sizeof(buffer) ? bytes : sizeof(buffer);
    if (input.read(buffer, chunk) != static_cast<int>(chunk) || output.write(buffer, chunk) != chunk) return false;
    bytes -= chunk;
  }
  return true;
}

enum class Frame : uint8_t { Root, Reviews, Group, PageReviews, Item, Review, Author, Skip };
enum class Expect : uint8_t {
  None,
  Error,
  Range,
  Reviews,
  HasMore,
  MaxIdx,
  SyncKey,
  PageReviews,
  Review,
  Content,
  Author,
  AuthorName,
  MarkCount,
  TotalCount
};

}  // namespace

bool isStandardBook(const char* bookId) { return bookId && bookId[0] && strncmp(bookId, "MP_WXS_", 7) != 0; }

std::string rangePath(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end) {
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, false) || end <= start) return "";
  char file[40];
  const int length = snprintf(file, sizeof(file), "%u_%u.wrn", start, end);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(file)) return "";
  return directory + "/" + file;
}

bool RangeFile::begin(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end,
                      const bool copyExisting, const bool stage, const bool keepStaged) {
  abort();
  stage_ = stage || keepStaged;
  keepStaged_ = keepStaged;
  carried_ = false;
  carriedMaxIdx_ = 0;
  carriedSyncKey_ = 0;
  if (end <= start) return false;
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, true)) return false;
  char file[40];
  const int length = snprintf(file, sizeof(file), "%u_%u.wrn", start, end);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(file)) return false;
  finalPath_ = directory + "/" + file;
  partPath_ = finalPath_ + ".part";
  if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  if (!Storage.openFileForWrite("WR", partPath_, file_)) return false;

  Header header;
  header.chapterUid = chapterUid;
  header.rangeStart = start;
  header.rangeEnd = end;
  if (file_.write(&header, sizeof(header)) != sizeof(header)) {
    abort();
    return false;
  }

  chapterUid_ = chapterUid;
  start_ = start;
  end_ = end;
  count_ = 0;
  capped_ = false;
  active_ = true;

  // 重新拉取时接着 .new 写，不把已有 .wrn 抄进来，避免失败时把旧评论改掉。
  const std::string source = stage_ ? finalPath_ + ".new" : finalPath_;
  if (!copyExisting || !Storage.exists(source.c_str())) return true;
  HalFile existing;
  Header previous;
  if (!Storage.openFileForRead("WR", source, existing) ||
      existing.read(&previous, sizeof(previous)) != static_cast<int>(sizeof(previous)) || previous.magic != kMagic ||
      previous.version != kVersion || previous.headerSize != kHeaderSize || previous.chapterUid != chapterUid ||
      previous.rangeStart != start || previous.rangeEnd != end || previous.commentCount > kMaxStoredComments) {
    return true;
  }
  carried_ = true;
  carriedMaxIdx_ = previous.maxIdx;
  carriedSyncKey_ = previous.syncKey;
  for (uint32_t i = 0; i < previous.commentCount; ++i) {
    CommentHead head;
    if (existing.read(&head, sizeof(head)) != static_cast<int>(sizeof(head)) || head.authorLen > kMaxAuthorBytes ||
        head.textLen > kMaxCommentBytes ||
        file_.write(&head, sizeof(head)) != sizeof(head) || !copyBlob(existing, file_, head.authorLen + head.textLen)) {
      abort();
      return false;
    }
    ++count_;
  }
  return true;
}

bool RangeFile::add(const char* author, const char* text, const size_t textLen, const bool truncated) {
  if (!active_ || !text || textLen == 0 || textLen > kMaxCommentBytes) return false;
  if (count_ >= kMaxStoredComments) {
    capped_ = true;
    return true;
  }
  const size_t authorLen = author ? strnlen(author, kMaxAuthorBytes) : 0;
  CommentHead head;
  head.authorLen = static_cast<uint16_t>(authorLen);
  head.textLen = static_cast<uint16_t>(textLen);
  head.flags = truncated ? kTextTruncated : 0;
  if (file_.write(&head, sizeof(head)) != sizeof(head)) return false;
  if (authorLen > 0 && file_.write(author, authorLen) != authorLen) return false;
  if (file_.write(text, textLen) != textLen) return false;
  ++count_;
  return true;
}

bool RangeFile::finish(const uint32_t maxIdx, const uint64_t syncKey, const bool hasMore) {
  if (!active_ || count_ == 0) {
    abort();
    return false;
  }
  Header header;
  header.chapterUid = chapterUid_;
  header.rangeStart = start_;
  header.rangeEnd = end_;
  header.commentCount = count_;
  header.maxIdx = maxIdx;
  header.syncKey = syncKey;
  if (hasMore) header.flags |= kFlagHasMore;
  if (capped_) header.flags |= kFlagCapped;
  if (!file_.seek(0) || file_.write(&header, sizeof(header)) != sizeof(header)) {
    abort();
    return false;
  }
  file_.flush();
  file_.close();
  active_ = false;
  // 还有下一页，或整章要等全部成功再换时，只更新 .new。已有 .wrn 继续给界面用。
  const bool leaveNew = stage_ && (hasMore || keepStaged_);
  const std::string target = leaveNew ? finalPath_ + ".new" : finalPath_;
  if (!WeReadStore::atomicReplace(partPath_, target)) {
    Storage.remove(partPath_.c_str());
    return false;
  }
  partPath_.clear();
  if (stage_ && !hasMore && !keepStaged_) {
    const std::string staging = finalPath_ + ".new";
    if (Storage.exists(staging.c_str())) Storage.remove(staging.c_str());
  }
  stage_ = false;
  keepStaged_ = false;
  return true;
}

void RangeFile::discardStaging() {
  if (finalPath_.empty()) return;
  const std::string staging = finalPath_ + ".new";
  if (Storage.exists(staging.c_str())) Storage.remove(staging.c_str());
}

RangeFile::~RangeFile() { abort(); }

void RangeFile::abort() {
  if (file_.isOpen()) file_.close();
  active_ = false;
  count_ = 0;
  capped_ = false;
  if (!partPath_.empty() && Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  partPath_.clear();
}

struct DiskCandidate {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t markCount = 0;
  uint32_t commentTotal = 0;
  uint32_t query = 0;
};
static_assert(sizeof(DiskCandidate) == 20, "candidate records stay 20 bytes");

std::string candidatePath(const char* bookId, const uint32_t chapterUid) {
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, false)) return "";
  return directory + "/ranges.wri";
}

bool readDisk(HalFile& file, const uint32_t index, DiskCandidate& record) {
  const uint32_t offset = kCandidateHeaderSize + index * static_cast<uint32_t>(sizeof(record));
  return file.seek(offset) && file.read(&record, sizeof(record)) == static_cast<int>(sizeof(record));
}

bool writeDisk(HalFile& file, const uint32_t index, const DiskCandidate& record) {
  const uint32_t offset = kCandidateHeaderSize + index * static_cast<uint32_t>(sizeof(record));
  return file.seek(offset) && file.write(&record, sizeof(record)) == sizeof(record);
}

Candidate toPublic(const DiskCandidate& record) {
  Candidate item;
  item.start = record.start;
  item.end = record.end;
  item.markCount = record.markCount;
  item.commentTotal = record.commentTotal;
  item.query = static_cast<CandidateQuery>(record.query);
  return item;
}

DiskCandidate toDisk(const Candidate& item) {
  DiskCandidate record;
  record.start = item.start;
  record.end = item.end;
  record.markCount = item.markCount;
  record.commentTotal = item.commentTotal;
  record.query = static_cast<uint32_t>(item.query);
  return record;
}

std::string candidateFilePath(const char* bookId, const uint32_t chapterUid, const bool staged) {
  std::string path = candidatePath(bookId, chapterUid);
  if (path.empty() || !staged) return path;
  path += ".new";
  return path;
}

bool openCandidate(const char* bookId, const uint32_t chapterUid, HalFile& file, CandidateHeader& header,
                   const bool staged = false) {
  const std::string path = candidateFilePath(bookId, chapterUid, staged);
  if (path.empty() || !Storage.openFileForRead("WR", path, file)) return false;
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kCandidateMagic ||
      header.version != kCandidateVersion || header.headerSize != kCandidateHeaderSize ||
      header.chapterUid != chapterUid) {
    return false;
  }
  const size_t expect = kCandidateHeaderSize + static_cast<size_t>(header.count) * sizeof(DiskCandidate);
  return file.fileSize() == expect;
}

bool CandidateFile::begin(const char* bookId, const uint32_t chapterUid) {
  abort();
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, true)) return false;
  finalPath_ = directory + "/ranges.wri";
  partPath_ = finalPath_ + ".part";
  if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  if (!Storage.openFileForWrite("WR", partPath_, file_)) return false;
  CandidateHeader header;
  header.chapterUid = chapterUid;
  if (file_.write(&header, sizeof(header)) != sizeof(header)) {
    abort();
    return false;
  }
  chapterUid_ = chapterUid;
  count_ = 0;
  active_ = true;
  return true;
}

bool CandidateFile::add(const uint32_t start, const uint32_t end, const uint32_t markCount) {
  if (!active_ || end <= start) return false;
  for (uint32_t i = 0; i < count_; ++i) {
    DiskCandidate record;
    if (!readDisk(file_, i, record)) return false;
    if (record.start != start || record.end != end) continue;
    if (markCount > record.markCount) {
      record.markCount = markCount;
      if (!writeDisk(file_, i, record)) return false;
    }
    return true;
  }
  DiskCandidate record;
  record.start = start;
  record.end = end;
  record.markCount = markCount;
  const auto slash = finalPath_.rfind('/');
  if (slash != std::string::npos) {
    char name[40];
    const int length = snprintf(name, sizeof(name), "%u_%u.wrn", start, end);
    if (length > 0 && static_cast<size_t>(length) < sizeof(name)) {
      const std::string existingPath = finalPath_.substr(0, slash) + "/" + name;
      HalFile existing;
      Header header;
      if (Storage.openFileForRead("WR", existingPath, existing) &&
          existing.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) && header.magic == kMagic &&
          header.version == kVersion && header.commentCount > 0) {
        record.query = static_cast<uint32_t>((header.flags & kFlagHasMore) != 0 ? CandidateQuery::Partial
                                                                                : CandidateQuery::Stored);
      }
    }
  }
  if (record.query == static_cast<uint32_t>(CandidateQuery::Unknown)) {
    // 重建期间旧索引还完好（先写 .part，finish 才原子替换）。把上次点开确认无评论的
    // 判定搬过来，否则 48 小时后索引一刷新，已确认空段的虚线会复活。
    HalFile previous;
    CandidateHeader old;
    if (Storage.openFileForRead("WR", finalPath_, previous) &&
        previous.read(&old, sizeof(old)) == static_cast<int>(sizeof(old)) && old.magic == kCandidateMagic &&
        old.version == kCandidateVersion && old.headerSize == kCandidateHeaderSize && old.chapterUid == chapterUid_ &&
        previous.fileSize() == kCandidateHeaderSize + static_cast<size_t>(old.count) * sizeof(DiskCandidate)) {
      for (uint32_t i = 0; i < old.count; ++i) {
        DiskCandidate prior;
        if (!readDisk(previous, i, prior)) break;
        if (prior.start != start || prior.end != end) continue;
        if (prior.query == static_cast<uint32_t>(CandidateQuery::Empty)) record.query = prior.query;
        break;
      }
    }
  }
  if (!writeDisk(file_, count_, record)) return false;
  ++count_;
  return true;
}

bool CandidateFile::finish(const bool stage) {
  if (!active_) return false;
  CandidateHeader header;
  header.chapterUid = chapterUid_;
  header.count = count_;
  if (!file_.seek(0) || file_.write(&header, sizeof(header)) != sizeof(header)) {
    abort();
    return false;
  }
  file_.flush();
  file_.close();
  active_ = false;
  const std::string target = stage ? finalPath_ + ".new" : finalPath_;
  if (!WeReadStore::atomicReplace(partPath_, target)) {
    Storage.remove(partPath_.c_str());
    return false;
  }
  return true;
}

void CandidateFile::abort() {
  if (file_.isOpen()) file_.close();
  active_ = false;
  count_ = 0;
  if (!partPath_.empty() && Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  partPath_.clear();
  finalPath_.clear();
}

CandidateFile::~CandidateFile() { abort(); }

bool candidatesReady(const char* bookId, const uint32_t chapterUid) {
  HalFile file;
  CandidateHeader header;
  return openCandidate(bookId, chapterUid, file, header);
}

constexpr uint32_t kFetchedMagic = 0x31465257;  // WRF1

struct FetchedStamp {
  uint32_t magic = kFetchedMagic;
  uint32_t epoch = 0;
};
static_assert(sizeof(FetchedStamp) == 8, "chapter fetch stamp is 8 bytes");

std::string fetchedPath(const char* bookId, const uint32_t chapterUid, const bool create) {
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, create)) return "";
  return directory + "/fetched.at";
}

uint32_t chapterFetchedAt(const char* bookId, const uint32_t chapterUid) {
  const std::string path = fetchedPath(bookId, chapterUid, false);
  if (path.empty() || !Storage.exists(path.c_str())) return 0;
  HalFile file;
  FetchedStamp stamp;
  if (!Storage.openFileForRead("WR", path, file) ||
      file.read(&stamp, sizeof(stamp)) != static_cast<int>(sizeof(stamp)) || stamp.magic != kFetchedMagic) {
    return 0;
  }
  return stamp.epoch;
}

bool markChapterFetched(const char* bookId, const uint32_t chapterUid, const uint32_t epochSeconds) {
  if (epochSeconds == 0) return false;
  const std::string finalPath = fetchedPath(bookId, chapterUid, true);
  if (finalPath.empty()) return false;
  const std::string partPath = finalPath + ".part";
  HalFile file;
  if (!Storage.openFileForWrite("WR", partPath, file)) return false;
  FetchedStamp stamp;
  stamp.epoch = epochSeconds;
  if (file.write(&stamp, sizeof(stamp)) != sizeof(stamp)) {
    file.close();
    Storage.remove(partPath.c_str());
    return false;
  }
  file.flush();
  file.close();
  if (!WeReadStore::atomicReplace(partPath, finalPath)) {
    Storage.remove(partPath.c_str());
    return false;
  }
  return true;
}

bool clearChapterFetched(const char* bookId, const uint32_t chapterUid) {
  const std::string path = fetchedPath(bookId, chapterUid, false);
  if (path.empty() || !Storage.exists(path.c_str())) return true;
  return Storage.remove(path.c_str());
}

bool visitCandidates(const char* bookId, const uint32_t chapterUid,
                     bool (*visit)(void* ctx, uint32_t index, const Candidate& item), void* ctx, const bool staged) {
  if (!visit || !isStandardBook(bookId)) return false;
  const std::string path = candidateFilePath(bookId, chapterUid, staged);
  if (path.empty() || !Storage.exists(path.c_str())) return true;
  HalFile file;
  CandidateHeader header;
  if (!openCandidate(bookId, chapterUid, file, header, staged)) return false;
  for (uint32_t i = 0; i < header.count; ++i) {
    DiskCandidate record;
    if (!readDisk(file, i, record)) return false;
    if (!visit(ctx, i, toPublic(record))) return true;
  }
  return true;
}

bool readCandidate(const char* bookId, const uint32_t chapterUid, const uint32_t index, Candidate& item,
                   const bool staged) {
  HalFile file;
  CandidateHeader header;
  if (!openCandidate(bookId, chapterUid, file, header, staged) || index >= header.count) return false;
  DiskCandidate record;
  if (!readDisk(file, index, record)) return false;
  item = toPublic(record);
  return true;
}

bool updateCandidate(const char* bookId, const uint32_t chapterUid, const uint32_t index, const Candidate& item,
                     const bool staged) {
  const std::string path = candidateFilePath(bookId, chapterUid, staged);
  if (path.empty()) return false;
  HalFile file = Storage.open(path.c_str(), O_RDWR);
  if (!file) return false;
  CandidateHeader header;
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kCandidateMagic ||
      header.version != kCandidateVersion || header.headerSize != kCandidateHeaderSize ||
      header.chapterUid != chapterUid || index >= header.count) {
    return false;
  }
  return writeDisk(file, index, toDisk(item));
}

bool hasSuffix(const char* name, const char* suffix) {
  if (!name || !suffix) return false;
  const size_t nameLen = strlen(name);
  const size_t suffixLen = strlen(suffix);
  return nameLen >= suffixLen && memcmp(name + (nameLen - suffixLen), suffix, suffixLen) == 0;
}

bool stagedIndexHas(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end,
                    bool& readable) {
  struct Hit {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
  } hit{start, end, false};
  auto match = [](void* raw, uint32_t, const Candidate& item) {
    auto* wanted = static_cast<Hit*>(raw);
    if (item.start != wanted->start || item.end != wanted->end) return true;
    wanted->found = true;
    return false;
  };
  readable = visitCandidates(bookId, chapterUid, match, &hit, true);
  return hit.found;
}

bool queriesCanCommit(void* raw, uint32_t, const Candidate& item) {
  if (item.query == CandidateQuery::Stored || item.query == CandidateQuery::Empty) return true;
  *static_cast<bool*>(raw) = false;
  return true;
}

// 换文件时先把旧的改名为 .bak。整章没换完就改回去，避免只换上一部分。
std::string backupPath(const std::string& live) { return live + ".bak"; }

std::string pathWithoutBak(const std::string& backup) {
  constexpr size_t kBakLen = 4;
  if (backup.size() < kBakLen) return backup;
  return backup.substr(0, backup.size() - kBakLen);
}

bool nextBackup(const std::string& directory, std::string& backup) {
  HalFile folder = Storage.open(directory.c_str());
  if (!folder || !folder.isDirectory()) return false;
  while (true) {
    HalFile entry = folder.openNextFile();
    if (!entry) return false;
    char name[64];
    const size_t nameLen = entry.getName(name, sizeof(name));
    if (nameLen == 0 || nameLen >= sizeof(name)) continue;
    name[nameLen] = '\0';
    const char* file = baseName(name);
    if (!hasSuffix(file, ".wrn.bak") && strcmp(file, "ranges.wri.bak") != 0) continue;
    backup = directory + "/" + file;
    return true;
  }
}

// 上次换到一半留下的备份：新文件还在就丢掉备份，只剩备份就把旧评论放回去。
bool settleLeftoverBackups(const std::string& directory) {
  for (;;) {
    std::string backup;
    if (!nextBackup(directory, backup)) return true;
    const std::string live = pathWithoutBak(backup);
    if (Storage.exists(live.c_str())) {
      if (!Storage.remove(backup.c_str())) return false;
    } else if (!Storage.rename(backup.c_str(), live.c_str())) {
      return false;
    }
  }
}

bool parkLive(const std::string& live) {
  const std::string backup = backupPath(live);
  if (Storage.exists(backup.c_str())) return false;
  if (!Storage.exists(live.c_str())) return true;
  return Storage.rename(live.c_str(), backup.c_str());
}

// 失败时新文件让位，.bak 回到原来的名字。
bool undoBackups(const std::string& directory) {
  for (;;) {
    std::string backup;
    if (!nextBackup(directory, backup)) return true;
    const std::string live = pathWithoutBak(backup);
    if (Storage.exists(live.c_str()) && !Storage.remove(live.c_str())) return false;
    if (!Storage.rename(backup.c_str(), live.c_str())) return false;
  }
}

bool dropBackups(const std::string& directory) {
  for (;;) {
    std::string backup;
    if (!nextBackup(directory, backup)) return true;
    if (!Storage.remove(backup.c_str())) return false;
  }
}

struct StageApply {
  const char* bookId = nullptr;
  uint32_t chapterUid = 0;
  bool ok = true;
};

bool applyOneStaged(void* raw, uint32_t, const Candidate& item) {
  auto* apply = static_cast<StageApply*>(raw);
  if (!apply->ok) return false;
  const std::string live = rangePath(apply->bookId, apply->chapterUid, item.start, item.end);
  if (live.empty()) {
    apply->ok = false;
    return false;
  }
  const std::string staged = live + ".new";
  if (item.query == CandidateQuery::Empty) {
    if (!parkLive(live)) apply->ok = false;
    if (Storage.exists(staged.c_str()) && !Storage.remove(staged.c_str())) apply->ok = false;
    return apply->ok;
  }
  if (item.query != CandidateQuery::Stored) {
    apply->ok = false;
    return false;
  }
  // 超过一轮能重拉的段没有 .new，保留原来的评论文件。
  if (!Storage.exists(staged.c_str())) return true;
  if (!parkLive(live) || !Storage.rename(staged.c_str(), live.c_str())) {
    apply->ok = false;
    return false;
  }
  return true;
}

bool parkRetiredRanges(const char* bookId, const uint32_t chapterUid, const std::string& directory) {
  for (;;) {
    HalFile folder = Storage.open(directory.c_str());
    if (!folder || !folder.isDirectory()) return true;
    std::string victim;
    while (true) {
      HalFile entry = folder.openNextFile();
      if (!entry) break;
      char name[64];
      const size_t nameLen = entry.getName(name, sizeof(name));
      if (nameLen == 0 || nameLen >= sizeof(name)) continue;
      name[nameLen] = '\0';
      uint32_t start = 0;
      uint32_t end = 0;
      if (!parseRangeFileName(name, start, end)) continue;
      bool readable = false;
      if (stagedIndexHas(bookId, chapterUid, start, end, readable)) continue;
      if (!readable) return false;
      victim = directory + "/" + baseName(name);
      break;
    }
    if (victim.empty()) return true;
    if (!parkLive(victim)) return false;
  }
}

bool swapIndexKeepingBackup(const std::string& stagedIndex, const std::string& liveIndex) {
  const std::string backup = backupPath(liveIndex);
  if (Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) return false;
  const bool hadLive = Storage.exists(liveIndex.c_str());
  if (hadLive && !Storage.rename(liveIndex.c_str(), backup.c_str())) return false;
  if (Storage.rename(stagedIndex.c_str(), liveIndex.c_str())) return true;
  if (hadLive) Storage.rename(backup.c_str(), liveIndex.c_str());
  return false;
}

bool stillWantedNow(bool (*stillWanted)(void*), void* stillCtx) { return !stillWanted || stillWanted(stillCtx); }

void discardStagedChapter(const char* bookId, const uint32_t chapterUid) {
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, false)) return;
  const std::string index = candidateFilePath(bookId, chapterUid, true);
  if (!index.empty() && Storage.exists(index.c_str())) Storage.remove(index.c_str());
  if (!Storage.exists(directory.c_str())) return;
  for (;;) {
    HalFile folder = Storage.open(directory.c_str());
    if (!folder || !folder.isDirectory()) return;
    std::string victim;
    while (true) {
      HalFile entry = folder.openNextFile();
      if (!entry) break;
      char name[64];
      const size_t nameLen = entry.getName(name, sizeof(name));
      if (nameLen == 0 || nameLen >= sizeof(name)) continue;
      name[nameLen] = '\0';
      if (!hasSuffix(baseName(name), ".wrn.new")) continue;
      victim = directory + "/" + baseName(name);
      break;
    }
    if (victim.empty()) return;
    if (!Storage.remove(victim.c_str())) return;
  }
}

bool commitStagedChapter(const char* bookId, const uint32_t chapterUid, bool (*stillWanted)(void*), void* stillCtx) {
  const std::string stagedIndex = candidateFilePath(bookId, chapterUid, true);
  const std::string liveIndex = candidateFilePath(bookId, chapterUid, false);
  if (stagedIndex.empty() || liveIndex.empty() || !Storage.exists(stagedIndex.c_str())) return false;
  bool queriesOk = true;
  if (!visitCandidates(bookId, chapterUid, queriesCanCommit, &queriesOk, true) || !queriesOk) return false;
  if (!stillWantedNow(stillWanted, stillCtx)) return false;

  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, false)) return false;
  if (!settleLeftoverBackups(directory)) return false;

  StageApply apply{bookId, chapterUid, true};
  if (!visitCandidates(bookId, chapterUid, applyOneStaged, &apply, true) || !apply.ok ||
      !parkRetiredRanges(bookId, chapterUid, directory) || !stillWantedNow(stillWanted, stillCtx) ||
      !swapIndexKeepingBackup(stagedIndex, liveIndex) || !stillWantedNow(stillWanted, stillCtx)) {
    if (!undoBackups(directory)) LOG_ERR("WR", "本章评论退回失败");
    return false;
  }
  // 索引已经是新的。备份清不掉也算换上了，下次再清，不能把已经换上的说成失败。
  if (!dropBackups(directory)) LOG_ERR("WR", "本章评论备份没有删掉");
  discardStagedChapter(bookId, chapterUid);
  return true;
}

bool loadChapter(const char* bookId, const uint32_t chapterUid, Range* ranges, const size_t capacity, size_t& count,
                 bool& truncated) {
  count = 0;
  truncated = false;
  if (!ranges || capacity == 0 || !isStandardBook(bookId)) return false;
  std::string directory;
  if (!chapterDirectory(bookId, chapterUid, directory, false)) return false;
  if (!Storage.exists(directory.c_str())) return true;

  HalFile folder = Storage.open(directory.c_str());
  if (!folder || !folder.isDirectory()) return true;
  while (true) {
    HalFile entry = folder.openNextFile();
    if (!entry) break;
    char name[64];
    const size_t nameLen = entry.getName(name, sizeof(name));
    if (nameLen == 0 || nameLen >= sizeof(name)) continue;
    name[nameLen] = '\0';
    uint32_t start = 0;
    uint32_t end = 0;
    if (!parseRangeFileName(name, start, end)) continue;
    const std::string path = directory + "/" + baseName(name);
    HalFile file;
    Header header;
    if (!Storage.openFileForRead("WR", path, file) ||
        file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kMagic ||
        header.version != kVersion || header.headerSize != sizeof(header) || header.chapterUid != chapterUid ||
        header.rangeStart != start || header.rangeEnd != end || header.commentCount == 0 ||
        header.commentCount > kMaxStoredComments) {
      continue;
    }
    if (count >= capacity) {
      truncated = true;
      continue;
    }
    ranges[count].start = start;
    ranges[count].end = end;
    ++count;
  }
  return true;
}

bool readComment(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end,
                 const uint32_t index, char* text, const size_t textSize, StoredComment& comment) {
  comment = {};
  if (!text || textSize < 2) return false;
  text[0] = '\0';
  const std::string path = rangePath(bookId, chapterUid, start, end);
  if (path.empty()) return false;
  HalFile file;
  Header header;
  if (!Storage.openFileForRead("WR", path, file) ||
      file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kMagic ||
      header.version != kVersion || header.headerSize != sizeof(header) || header.commentCount == 0 ||
      header.commentCount > kMaxStoredComments || index >= header.commentCount) {
    return false;
  }
  for (uint32_t i = 0; i <= index; ++i) {
    CommentHead head;
    if (file.read(&head, sizeof(head)) != static_cast<int>(sizeof(head)) || head.authorLen > kMaxAuthorBytes ||
        head.textLen > kMaxCommentBytes) {
      return false;
    }
    if (i != index) {
      if (!file.seekCur(static_cast<int64_t>(head.authorLen) + head.textLen)) return false;
      continue;
    }
    if (head.authorLen > 0 &&
        file.read(comment.author, head.authorLen) != static_cast<int>(head.authorLen)) {
      return false;
    }
    comment.author[head.authorLen] = '\0';
    const size_t take = head.textLen < textSize - 1 ? head.textLen : textSize - 1;
    if (file.read(text, take) != static_cast<int>(take)) return false;
    text[take] = '\0';
    comment.index = index;
    comment.count = header.commentCount;
    comment.maxIdx = header.maxIdx;
    comment.syncKey = header.syncKey;
    comment.hasMore = (header.flags & kFlagHasMore) != 0;
    comment.textLen = static_cast<uint16_t>(take);
    return true;
  }
  return false;
}

namespace {

bool pushFrame(Frame* stack, uint8_t& depth, const Frame frame) {
  if (depth >= 16) return false;
  stack[depth++] = frame;
  return true;
}

Frame popFrame(Frame* stack, uint8_t& depth) {
  if (depth == 0) return Frame::Skip;
  return stack[--depth];
}

Frame parentFrame(const Frame* stack, const uint8_t depth) { return depth == 0 ? Frame::Skip : stack[depth - 1]; }

struct UnderlineState {
  std::unique_ptr<StreamingJsonParser> parser;
  Range* ranges = nullptr;
  size_t capacity = 0;
  bool (*sink)(void*, uint32_t, uint32_t, uint32_t) = nullptr;
  void* sinkCtx = nullptr;
  size_t count = 0;
  bool truncated = false;
  bool failed = false;
  bool rootClosed = false;
  int errorCode = 0;
  Frame stack[16] = {};
  uint8_t depth = 0;
  Expect expect = Expect::None;
  char rangeText[32] = {};
  uint32_t markCount = 0;
};

void noteMarkCount(UnderlineState& state, const uint32_t value) {
  if (value > state.markCount) state.markCount = value;
}

void acceptUnderline(UnderlineState& state) {
  uint32_t start = 0;
  uint32_t end = 0;
  if (!WeReadHighlight::parseRange(state.rangeText, start, end)) return;
  if (state.sink) {
    if (!state.sink(state.sinkCtx, start, end, state.markCount)) state.failed = true;
    else ++state.count;
    return;
  }
  for (size_t i = 0; i < state.count; ++i) {
    if (state.ranges[i].start != start || state.ranges[i].end != end) continue;
    // 同一段可能按 type 拆成多条，先到的那条 count 经常是 0。
    if (state.markCount > state.ranges[i].markCount) state.ranges[i].markCount = state.markCount;
    return;
  }
  if (state.count >= state.capacity) {
    state.truncated = true;
    return;
  }
  state.ranges[state.count].start = start;
  state.ranges[state.count].end = end;
  state.ranges[state.count].markCount = state.markCount;
  ++state.count;
}

void underlineKey(void* raw, const char* key, size_t) {
  auto& state = *static_cast<UnderlineState*>(raw);
  const Frame frame = parentFrame(state.stack, state.depth);
  state.expect = Expect::None;
  if (!key) return;
  if (frame == Frame::Root && (strcmp(key, "errCode") == 0 || strcmp(key, "errcode") == 0)) {
    state.expect = Expect::Error;
  } else if (frame == Frame::Root && strcmp(key, "underlines") == 0) {
    state.expect = Expect::Reviews;
  } else if (frame == Frame::Item && strcmp(key, "range") == 0) {
    state.expect = Expect::Range;
  } else if (frame == Frame::Item && (strcmp(key, "count") == 0 || strcmp(key, "totalCount") == 0)) {
    // 划线人数是 count。网页回包里普通划线的 count 就是 0，热门（type 2）才有人数。
    state.expect = Expect::MarkCount;
  }
}

void underlineString(void* raw, const char* value, size_t len) {
  auto& state = *static_cast<UnderlineState*>(raw);
  if (state.expect == Expect::Range) {
    WeReadProtocol::decodeJsonString(value, len, state.rangeText, sizeof(state.rangeText));
  } else if (state.expect == Expect::MarkCount) {
    noteMarkCount(state, parseUint32(value, len));
  }
  state.expect = Expect::None;
}

void underlineNumber(void* raw, const char* value, size_t len) {
  auto& state = *static_cast<UnderlineState*>(raw);
  if (state.expect == Expect::Error) state.errorCode = static_cast<int>(parseUint32(value, len));
  if (state.expect == Expect::Error && value && len > 0 && value[0] == '-') {
    state.errorCode = -static_cast<int>(parseUint32(value + 1, len - 1));
  }
  if (state.expect == Expect::MarkCount) noteMarkCount(state, parseUint32(value, len));
  state.expect = Expect::None;
}

void underlineIgnore(void* raw) { static_cast<UnderlineState*>(raw)->expect = Expect::None; }

void underlineObjectStart(void* raw) {
  auto& state = *static_cast<UnderlineState*>(raw);
  Frame next = Frame::Skip;
  if (state.depth == 0) next = Frame::Root;
  else if (parentFrame(state.stack, state.depth) == Frame::Reviews) next = Frame::Item;
  if (!pushFrame(state.stack, state.depth, next)) state.failed = true;
  if (next == Frame::Item) {
    state.rangeText[0] = '\0';
    state.markCount = 0;
  }
  state.expect = Expect::None;
}

void underlineObjectEnd(void* raw) {
  auto& state = *static_cast<UnderlineState*>(raw);
  const Frame ended = popFrame(state.stack, state.depth);
  if (ended == Frame::Item) acceptUnderline(state);
  if (ended == Frame::Root) state.rootClosed = true;
  state.expect = Expect::None;
}

void underlineArrayStart(void* raw) {
  auto& state = *static_cast<UnderlineState*>(raw);
  const Frame next = state.expect == Expect::Reviews ? Frame::Reviews : Frame::Skip;
  if (!pushFrame(state.stack, state.depth, next)) state.failed = true;
  state.expect = Expect::None;
}

void underlineArrayEnd(void* raw) {
  auto& state = *static_cast<UnderlineState*>(raw);
  popFrame(state.stack, state.depth);
  state.expect = Expect::None;
}

struct ReviewState {
  std::unique_ptr<StreamingJsonParser> parser;
  RangeFile* file = nullptr;
  char expectedRange[32] = {};
  char rangeText[32] = {};
  char author[kMaxAuthorBytes + 1] = {};
  char content[kMaxCommentBytes] = {};
  uint16_t contentLen = 0;
  bool contentTruncated = false;
  bool skipGroup = false;
  bool failed = false;
  bool rootClosed = false;
  bool hasMore = false;
  bool groupHasMore = false;
  int errorCode = 0;
  uint32_t maxIdx = 0;
  uint32_t groupMaxIdx = 0;
  uint32_t totalCount = 0;
  uint32_t groupTotalCount = 0;
  uint32_t markCount = 0;
  uint32_t groupMarkCount = 0;
  uint64_t syncKey = 0;
  uint64_t groupSyncKey = 0;
  uint32_t added = 0;
  Frame stack[16] = {};
  uint8_t depth = 0;
  Expect expect = Expect::None;
};

void clearReviewItem(ReviewState& state) {
  state.author[0] = '\0';
  state.content[0] = '\0';
  state.contentLen = 0;
  state.contentTruncated = false;
}

void appendContent(ReviewState& state, const char* value, const size_t len) {
  if (!value || state.skipGroup || state.contentTruncated) return;
  for (size_t i = 0; i < len; ++i) {
    if (state.contentLen + 1 >= kMaxCommentBytes) {
      while (state.contentLen > 0 && (static_cast<unsigned char>(state.content[state.contentLen - 1]) & 0xC0) == 0x80) {
        --state.contentLen;
      }
      if (state.contentLen > 0 && (static_cast<unsigned char>(state.content[state.contentLen - 1]) & 0x80) != 0) {
        --state.contentLen;
      }
      state.content[state.contentLen] = '\0';
      state.contentTruncated = true;
      return;
    }
    state.content[state.contentLen++] = value[i];
  }
  state.content[state.contentLen] = '\0';
}

void commitReview(ReviewState& state) {
  if (state.skipGroup || state.failed || !state.file) {
    clearReviewItem(state);
    return;
  }
  if (blank(state.content, state.contentLen)) {
    clearReviewItem(state);
    return;
  }
  const uint32_t before = state.file->count();
  if (!state.file->add(state.author, state.content, state.contentLen, state.contentTruncated)) {
    state.failed = true;
  } else if (state.file->count() > before) {
    ++state.added;
  }
  clearReviewItem(state);
}

void reviewKey(void* raw, const char* key, size_t) {
  auto& state = *static_cast<ReviewState*>(raw);
  const Frame frame = parentFrame(state.stack, state.depth);
  state.expect = Expect::None;
  if (!key) return;
  if (frame == Frame::Root && (strcmp(key, "errCode") == 0 || strcmp(key, "errcode") == 0)) {
    state.expect = Expect::Error;
  } else if (frame == Frame::Root && strcmp(key, "reviews") == 0) {
    state.expect = Expect::Reviews;
  } else if (frame == Frame::Group && strcmp(key, "range") == 0) {
    state.expect = Expect::Range;
  } else if (frame == Frame::Group && strcmp(key, "hasMore") == 0) {
    state.expect = Expect::HasMore;
  } else if (frame == Frame::Group && strcmp(key, "maxIdx") == 0) {
    state.expect = Expect::MaxIdx;
  } else if (frame == Frame::Group && strcmp(key, "totalCount") == 0) {
    state.expect = Expect::TotalCount;
  } else if (frame == Frame::Group && strcmp(key, "bookMarkCount") == 0) {
    state.expect = Expect::MarkCount;
  } else if (frame == Frame::Group && (strcmp(key, "synckey") == 0 || strcmp(key, "syncKey") == 0)) {
    state.expect = Expect::SyncKey;
  } else if (frame == Frame::Group && strcmp(key, "pageReviews") == 0) {
    state.expect = Expect::PageReviews;
  } else if ((frame == Frame::Item || frame == Frame::Review) && strcmp(key, "review") == 0) {
    state.expect = Expect::Review;
  } else if (frame == Frame::Review && strcmp(key, "content") == 0) {
    state.expect = Expect::Content;
  } else if (frame == Frame::Review && strcmp(key, "author") == 0) {
    state.expect = Expect::Author;
  } else if (frame == Frame::Author &&
             (strcmp(key, "name") == 0 || strcmp(key, "nick") == 0 || strcmp(key, "userName") == 0)) {
    state.expect = Expect::AuthorName;
  }
}

void reviewString(void* raw, const char* value, size_t len) {
  auto& state = *static_cast<ReviewState*>(raw);
  if (state.expect == Expect::Range) {
    WeReadProtocol::decodeJsonString(value, len, state.rangeText, sizeof(state.rangeText));
    state.skipGroup = state.rangeText[0] && strcmp(state.rangeText, state.expectedRange) != 0;
  } else if (state.expect == Expect::HasMore && !state.skipGroup) {
    // 组字段既有字符串编码的先例（bookMarkCount 按字符串收），hasMore 等游标两种编码都收。
    state.groupHasMore = !(len == 1 && value && value[0] == '0');
  } else if (state.expect == Expect::MaxIdx && !state.skipGroup) {
    state.groupMaxIdx = parseUint32(value, len);
  } else if (state.expect == Expect::TotalCount && !state.skipGroup) {
    state.groupTotalCount = parseUint32(value, len);
  } else if (state.expect == Expect::SyncKey && !state.skipGroup) {
    state.groupSyncKey = parseUint64(value, len);
  } else if (state.expect == Expect::MarkCount && !state.skipGroup) {
    const uint32_t parsed = parseUint32(value, len);
    if (parsed > state.groupMarkCount) state.groupMarkCount = parsed;
  } else if (state.expect == Expect::Content) {
    appendContent(state, value, len);
  } else if (state.expect == Expect::AuthorName) {
    WeReadProtocol::decodeJsonString(value, len, state.author, sizeof(state.author));
  }
  state.expect = Expect::None;
}

void reviewChunk(void* raw, const char* value, const size_t len, const bool final) {
  auto& state = *static_cast<ReviewState*>(raw);
  if (state.expect == Expect::Content) appendContent(state, value, len);
  if (final) state.expect = Expect::None;
}

void reviewNumber(void* raw, const char* value, size_t len) {
  auto& state = *static_cast<ReviewState*>(raw);
  if (state.expect == Expect::Error) {
    if (value && len > 0 && value[0] == '-') state.errorCode = -static_cast<int>(parseUint32(value + 1, len - 1));
    else state.errorCode = static_cast<int>(parseUint32(value, len));
  } else if (state.expect == Expect::HasMore && !state.skipGroup) {
    state.groupHasMore = !(len == 1 && value && value[0] == '0');
  } else if (state.expect == Expect::MaxIdx && !state.skipGroup) {
    state.groupMaxIdx = parseUint32(value, len);
  } else if (state.expect == Expect::TotalCount && !state.skipGroup) {
    state.groupTotalCount = parseUint32(value, len);
  } else if (state.expect == Expect::MarkCount && !state.skipGroup) {
    const uint32_t parsed = parseUint32(value, len);
    if (parsed > state.groupMarkCount) state.groupMarkCount = parsed;
  } else if (state.expect == Expect::SyncKey && !state.skipGroup) {
    state.groupSyncKey = parseUint64(value, len);
  }
  state.expect = Expect::None;
}

void reviewBool(void* raw, const bool value) {
  auto& state = *static_cast<ReviewState*>(raw);
  if (state.expect == Expect::HasMore && !state.skipGroup) state.groupHasMore = value;
  state.expect = Expect::None;
}

void reviewNull(void* raw) { static_cast<ReviewState*>(raw)->expect = Expect::None; }

void reviewObjectStart(void* raw) {
  auto& state = *static_cast<ReviewState*>(raw);
  Frame next = Frame::Skip;
  if (state.depth == 0) next = Frame::Root;
  else if (state.expect == Expect::Review) next = Frame::Review;
  else if (state.expect == Expect::Author) next = Frame::Author;
  else if (parentFrame(state.stack, state.depth) == Frame::Reviews) next = Frame::Group;
  else if (parentFrame(state.stack, state.depth) == Frame::PageReviews) next = Frame::Item;
  if (!pushFrame(state.stack, state.depth, next)) state.failed = true;
  if (next == Frame::Group) {
    state.rangeText[0] = '\0';
    state.skipGroup = false;
    state.groupHasMore = false;
    state.groupMaxIdx = 0;
    state.groupTotalCount = 0;
    state.groupMarkCount = 0;
    state.groupSyncKey = 0;
  }
  if (next == Frame::Item) clearReviewItem(state);
  state.expect = Expect::None;
}

void reviewObjectEnd(void* raw) {
  auto& state = *static_cast<ReviewState*>(raw);
  const Frame ended = popFrame(state.stack, state.depth);
  if (ended == Frame::Group && !state.skipGroup) {
    state.hasMore = state.groupHasMore;
    state.maxIdx = state.groupMaxIdx;
    state.totalCount = state.groupTotalCount;
    state.markCount = state.groupMarkCount;
    state.syncKey = state.groupSyncKey;
  }
  if (ended == Frame::Item) commitReview(state);
  if (ended == Frame::Root) state.rootClosed = true;
  state.expect = Expect::None;
}

void reviewArrayStart(void* raw) {
  auto& state = *static_cast<ReviewState*>(raw);
  Frame next = Frame::Skip;
  if (state.expect == Expect::Reviews) next = Frame::Reviews;
  else if (state.expect == Expect::PageReviews) next = Frame::PageReviews;
  if (!pushFrame(state.stack, state.depth, next)) state.failed = true;
  state.expect = Expect::None;
}

void reviewArrayEnd(void* raw) {
  auto& state = *static_cast<ReviewState*>(raw);
  popFrame(state.stack, state.depth);
  state.expect = Expect::None;
}

JsonCallbacks underlineCallbacks(UnderlineState* state) {
  JsonCallbacks callbacks{};
  callbacks.ctx = state;
  callbacks.onKey = underlineKey;
  callbacks.onString = underlineString;
  callbacks.onNumber = underlineNumber;
  callbacks.onBool = [](void* raw, bool) { underlineIgnore(raw); };
  callbacks.onNull = [](void* raw) { underlineIgnore(raw); };
  callbacks.onObjectStart = underlineObjectStart;
  callbacks.onObjectEnd = underlineObjectEnd;
  callbacks.onArrayStart = underlineArrayStart;
  callbacks.onArrayEnd = underlineArrayEnd;
  return callbacks;
}

JsonCallbacks reviewCallbacks(ReviewState* state) {
  JsonCallbacks callbacks{};
  callbacks.ctx = state;
  callbacks.onKey = reviewKey;
  callbacks.onString = reviewString;
  callbacks.onNumber = reviewNumber;
  callbacks.onBool = reviewBool;
  callbacks.onNull = reviewNull;
  callbacks.onObjectStart = reviewObjectStart;
  callbacks.onObjectEnd = reviewObjectEnd;
  callbacks.onArrayStart = reviewArrayStart;
  callbacks.onArrayEnd = reviewArrayEnd;
  callbacks.onStringChunk = reviewChunk;
  return callbacks;
}

}  // namespace

UnderlineParser::UnderlineParser() : state_(nullptr) {
  auto state = makeUniqueNoThrow<UnderlineState>();
  if (!state) {
    LOG_ERR("WR", "内存不足：划线候选解析状态 %u 字节", static_cast<unsigned>(sizeof(UnderlineState)));
    return;
  }
  state->parser = makeUniqueNoThrow<StreamingJsonParser>(underlineCallbacks(state.get()));
  if (!state->parser) {
    LOG_ERR("WR", "内存不足：评论候选 JSON 解析器 %u 字节",
            static_cast<unsigned>(sizeof(StreamingJsonParser)));
    return;
  }
  state_ = state.release();
}

UnderlineParser::~UnderlineParser() { delete static_cast<UnderlineState*>(state_); }

void resetUnderline(UnderlineState& state) {
  state.parser->reset();
  state.ranges = nullptr;
  state.capacity = 0;
  state.sink = nullptr;
  state.sinkCtx = nullptr;
  state.count = 0;
  state.truncated = false;
  state.failed = false;
  state.rootClosed = false;
  state.errorCode = 0;
  state.depth = 0;
  state.expect = Expect::None;
  state.rangeText[0] = '\0';
}

bool UnderlineParser::begin(Range* ranges, const size_t capacity) {
  auto* state = static_cast<UnderlineState*>(state_);
  if (!state || !state->parser || !ranges || capacity == 0) return false;
  resetUnderline(*state);
  state->ranges = ranges;
  state->capacity = capacity;
  return true;
}

bool UnderlineParser::begin(const CandidateSink sink, void* ctx) {
  auto* state = static_cast<UnderlineState*>(state_);
  if (!state || !state->parser || !sink) return false;
  resetUnderline(*state);
  state->sink = sink;
  state->sinkCtx = ctx;
  return true;
}

bool UnderlineParser::feed(const uint8_t* data, const size_t len) {
  auto* state = static_cast<UnderlineState*>(state_);
  if (!state || !state->parser || state->failed || (!data && len != 0)) return false;
  state->parser->feed(reinterpret_cast<const char*>(data), len);
  if (state->parser->hasError()) state->failed = true;
  return !state->failed;
}

bool UnderlineParser::finish() {
  auto* state = static_cast<UnderlineState*>(state_);
  if (!state || !state->parser) return false;
  return state->rootClosed && !state->failed && !state->parser->hasError() && state->errorCode == 0;
}

size_t UnderlineParser::count() const {
  auto* state = static_cast<UnderlineState*>(state_);
  return state ? state->count : 0;
}
bool UnderlineParser::truncated() const {
  auto* state = static_cast<UnderlineState*>(state_);
  return state && state->truncated;
}
int UnderlineParser::errorCode() const {
  auto* state = static_cast<UnderlineState*>(state_);
  return state ? state->errorCode : 0;
}

ReviewPageParser::ReviewPageParser() : state_(nullptr) {
  // 正文缓冲 2KB，加上解析器，不能放进 8KB 阅读栈。请求结束即释放。
  auto state = makeUniqueNoThrow<ReviewState>();
  if (!state) {
    LOG_ERR("WR", "内存不足：评论页解析状态 %u 字节", static_cast<unsigned>(sizeof(ReviewState)));
    return;
  }
  state->parser = makeUniqueNoThrow<StreamingJsonParser>(reviewCallbacks(state.get()));
  if (!state->parser) {
    LOG_ERR("WR", "内存不足：评论页 JSON 解析器 %u 字节",
            static_cast<unsigned>(sizeof(StreamingJsonParser)));
    return;
  }
  state_ = state.release();
}

ReviewPageParser::~ReviewPageParser() { delete static_cast<ReviewState*>(state_); }

bool ReviewPageParser::begin(RangeFile* file, const char* rangeText) {
  auto* state = static_cast<ReviewState*>(state_);
  if (!state || !state->parser || !file || !rangeText || !rangeText[0]) return false;
  state->parser->reset();
  state->file = file;
  strncpy(state->expectedRange, rangeText, sizeof(state->expectedRange) - 1);
  state->expectedRange[sizeof(state->expectedRange) - 1] = '\0';
  state->rangeText[0] = '\0';
  clearReviewItem(*state);
  state->skipGroup = false;
  state->failed = false;
  state->rootClosed = false;
  state->hasMore = false;
  state->groupHasMore = false;
  state->errorCode = 0;
  state->maxIdx = 0;
  state->groupMaxIdx = 0;
  state->totalCount = 0;
  state->groupTotalCount = 0;
  state->markCount = 0;
  state->groupMarkCount = 0;
  state->syncKey = 0;
  state->groupSyncKey = 0;
  state->added = 0;
  state->depth = 0;
  state->expect = Expect::None;
  return true;
}

bool ReviewPageParser::feed(const uint8_t* data, const size_t len) {
  auto* state = static_cast<ReviewState*>(state_);
  if (!state || !state->parser || state->failed || (!data && len != 0)) return false;
  state->parser->feed(reinterpret_cast<const char*>(data), len);
  if (state->parser->hasError()) state->failed = true;
  return !state->failed;
}

bool ReviewPageParser::finish(Cursor& cursor) {
  cursor = {};
  auto* state = static_cast<ReviewState*>(state_);
  if (!state || !state->parser) return false;
  cursor.maxIdx = state->maxIdx;
  cursor.syncKey = state->syncKey;
  cursor.totalCount = state->totalCount;
  cursor.markCount = state->markCount;
  cursor.hasMore = state->hasMore;
  cursor.errorCode = state->errorCode;
  cursor.added = state->added;
  return state->rootClosed && !state->failed && !state->parser->hasError() && state->errorCode == 0;
}

CollectResult collectRange(const char* bookId, const uint32_t chapterUid, const uint32_t start, const uint32_t end,
                           const uint32_t startMaxIdx, const uint64_t startSyncKey, const bool copyExisting,
                           const ReviewPageFetch fetch, void* ctx, const int maxPages, uint32_t* totalCount,
                           uint32_t* markCount, const bool stage, const bool keepStaged) {
  if (!fetch || end <= start || maxPages <= 0) return CollectResult::Failed;
  RangeFile file;
  if (!file.begin(bookId, chapterUid, start, end, copyExisting, stage, keepStaged)) return CollectResult::Failed;

  Cursor cursor;
  cursor.maxIdx = file.carriedCursor() ? file.carriedMaxIdx() : startMaxIdx;
  cursor.syncKey = file.carriedCursor() ? file.carriedSyncKey() : startSyncKey;
  bool hasMore = false;
  for (int page = 0; page < maxPages && file.count() < kMaxStoredComments; ++page) {
    const uint32_t before = file.count();
    const uint32_t requestMax = cursor.maxIdx;
    const uint64_t requestSync = cursor.syncKey;
    Cursor pageCursor;
    if (!fetch(ctx, requestMax, requestSync, file, pageCursor) || pageCursor.errorCode != 0) {
      file.abort();
      file.discardStaging();
      return CollectResult::Failed;
    }
    if (pageCursor.hasMore && pageCursor.maxIdx == requestMax && pageCursor.syncKey == requestSync &&
        file.count() == before) {
      LOG_ERR("WR", "评论翻页游标没有前进：maxIdx=%u", requestMax);
      file.abort();
      file.discardStaging();
      return CollectResult::Failed;
    }
    cursor = pageCursor;
    hasMore = pageCursor.hasMore;
    if (totalCount && pageCursor.totalCount > 0) *totalCount = pageCursor.totalCount;
    if (markCount && pageCursor.markCount > 0) *markCount = pageCursor.markCount;
    if (!hasMore) break;
  }
  if (file.capped()) hasMore = true;
  if (file.count() == 0) {
    file.abort();
    if (stage) file.discardStaging();
    return CollectResult::Empty;
  }
  if (!file.finish(cursor.maxIdx, cursor.syncKey, hasMore || file.capped())) {
    file.abort();
    file.discardStaging();
    return CollectResult::Failed;
  }
  return CollectResult::Stored;
}

}  // namespace WeReadComment
