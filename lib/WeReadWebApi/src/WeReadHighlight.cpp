#include "WeReadHighlight.h"

#include <Logging.h>

#include "WeReadStore.h"

#include <cstring>

#include "../../../src/util/StringUtils.h"

namespace WeReadHighlight {
namespace {

constexpr char kRoot[] = "/.crosspoint/weread/highlights";

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

bool ensureBookDirectory(const char* bookId, std::string& directory) {
  if (!bookId || !bookId[0]) return false;
  directory = std::string(kRoot) + "/" + StringUtils::sanitizeFilename(bookId, 56);
  return Storage.ensureDirectoryExists(kRoot) && Storage.ensureDirectoryExists(directory.c_str());
}

}  // namespace

bool parseRange(const char* text, uint32_t& start, uint32_t& end) {
  if (!text || !text[0]) return false;
  const char* cursor = nullptr;
  uint32_t parsedStart = 0;
  uint32_t parsedEnd = 0;
  if (!parseUnsigned(text, text + strlen(text), parsedStart, &cursor) || !cursor || *cursor != '-') return false;
  const char* tail = nullptr;
  if (!parseUnsigned(cursor + 1, text + strlen(text), parsedEnd, &tail) || !tail || *tail != '\0') return false;
  if (parsedEnd <= parsedStart) return false;
  start = parsedStart;
  end = parsedEnd;
  return true;
}

bool parseChapterUid(const char* text, uint32_t& chapterUid) {
  if (!text || !text[0]) return false;
  const char* tail = nullptr;
  return parseUnsigned(text, text + strlen(text), chapterUid, &tail) && tail && *tail == '\0';
}

bool rangesOverlap(const uint32_t start, const uint32_t end, const uint32_t otherStart, const uint32_t otherEnd) {
  return start < otherEnd && otherStart < end;
}

std::string indexPath(const char* bookId) {
  if (!bookId || !bookId[0]) return "";
  return std::string(kRoot) + "/" + StringUtils::sanitizeFilename(bookId, 56) + "/marks.bin";
}

bool clearAll() {
  if (!Storage.exists(kRoot)) return true;
  if (Storage.removeDir(kRoot)) return true;
  LOG_ERR("WR", "Failed to remove highlight cache: %s", kRoot);
  return false;
}

bool loadChapter(const char* bookId, const uint32_t chapterUid, Range* ranges, const size_t capacity, size_t& count,
                 bool& truncated) {
  count = 0;
  truncated = false;
  if (!ranges || capacity == 0) return false;
  const std::string path = indexPath(bookId);
  if (path.empty() || !Storage.exists(path.c_str())) return true;

  HalFile file;
  Header header;
  if (!Storage.openFileForRead("WR", path, file) || file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      header.magic != kMagic || header.version != kVersion || header.recordSize != sizeof(Mark) ||
      header.count > kMaxRecords ||
      file.fileSize64() != sizeof(Header) + static_cast<uint64_t>(header.count) * sizeof(Mark)) {
    return false;
  }

  for (uint32_t i = 0; i < header.count; ++i) {
    Mark mark;
    if (file.read(&mark, sizeof(mark)) != static_cast<int>(sizeof(mark))) return false;
    if (mark.chapterUid != chapterUid || (mark.flags & Mine) == 0 || mark.rangeEnd <= mark.rangeStart) continue;
    if (count >= capacity) {
      truncated = true;
      continue;
    }
    ranges[count].start = mark.rangeStart;
    ranges[count].end = mark.rangeEnd;
    ++count;
  }
  return true;
}

bool Writer::begin(const char* bookId) {
  abort();
  std::string directory;
  if (!ensureBookDirectory(bookId, directory)) return false;
  finalPath_ = directory + "/marks.bin";
  partPath_ = finalPath_ + ".part";
  if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  if (!Storage.openFileForWrite("WR", partPath_, file_)) return false;
  Header header;
  if (file_.write(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    abort();
    return false;
  }
  count_ = 0;
  truncated_ = false;
  active_ = true;
  return true;
}

bool Writer::append(const Mark& mark) {
  if (!active_ || !mark.bookmarkId[0] || mark.rangeEnd <= mark.rangeStart) return false;
  if (count_ >= kMaxRecords) {
    truncated_ = true;
    return true;
  }
  if (file_.write(&mark, sizeof(mark)) != static_cast<int>(sizeof(mark))) return false;
  ++count_;
  return true;
}

bool Writer::finish(const uint64_t syncKey, const uint32_t bookVersion, const char* ownerVid) {
  if (!active_) return false;
  Header header;
  header.count = count_;
  header.syncKey = syncKey;
  header.bookVersion = bookVersion;
  if (ownerVid && ownerVid[0]) {
    strncpy(header.ownerVid, ownerVid, sizeof(header.ownerVid) - 1);
  }
  if (!file_.seek(0) || file_.write(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    abort();
    return false;
  }
  file_.flush();
  file_.close();
  active_ = false;
  if (!WeReadStore::atomicReplace(partPath_, finalPath_)) {
    Storage.remove(partPath_.c_str());
    return false;
  }
  return true;
}

void Writer::abort() {
  if (file_.isOpen()) file_.close();
  active_ = false;
  if (!partPath_.empty() && Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  count_ = 0;
  truncated_ = false;
}

}  // namespace WeReadHighlight
