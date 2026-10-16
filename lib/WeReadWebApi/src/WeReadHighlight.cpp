#include "WeReadHighlight.h"

#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include "WeReadProtocol.h"
#include "WeReadStore.h"

#include <cstdlib>
#include <cstring>
#include <memory>

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

bool loadChapterMarks(const char* bookId, const uint32_t chapterUid, ChapterMark* marks, const size_t capacity,
                      size_t& count, bool& truncated) {
  count = 0;
  truncated = false;
  if (!marks || capacity == 0) return false;
  const std::string path = indexPath(bookId);
  if (path.empty() || !Storage.exists(path.c_str())) return true;

  HalFile file;
  Header header;
  if (!Storage.openFileForRead("WR", path, file) ||
      file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kMagic ||
      header.version != kVersion || header.recordSize != sizeof(Mark) || header.count > kMaxRecords ||
      file.fileSize64() != sizeof(Header) + static_cast<uint64_t>(header.count) * sizeof(Mark)) {
    return false;
  }

  for (uint32_t i = 0; i < header.count; ++i) {
    Mark mark;
    if (file.read(&mark, sizeof(mark)) != static_cast<int>(sizeof(mark))) return false;
    if (mark.chapterUid != chapterUid || (mark.flags & Mine) == 0 || mark.rangeEnd <= mark.rangeStart ||
        !mark.bookmarkId[0]) {
      continue;
    }
    if (count >= capacity) {
      truncated = true;
      continue;
    }
    strncpy(marks[count].bookmarkId, mark.bookmarkId, sizeof(marks[count].bookmarkId) - 1);
    marks[count].bookmarkId[sizeof(marks[count].bookmarkId) - 1] = '\0';
    marks[count].start = mark.rangeStart;
    marks[count].end = mark.rangeEnd;
    ++count;
  }
  return true;
}

bool removeMine(const char* bookId, const char* bookmarkId) {
  if (!bookId || !bookId[0] || !bookmarkId || !bookmarkId[0]) return false;
  const std::string path = indexPath(bookId);
  HalFile file;
  Header header;
  if (path.empty() || !Storage.openFileForRead("WR", path, file) ||
      file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kMagic ||
      header.version != kVersion || header.recordSize != sizeof(Mark) || header.count > kMaxRecords ||
      file.fileSize64() != sizeof(Header) + static_cast<uint64_t>(header.count) * sizeof(Mark)) {
    return false;
  }

  Writer writer;
  if (!writer.begin(bookId)) return false;
  bool removed = false;
  for (uint32_t i = 0; i < header.count; ++i) {
    Mark mark;
    if (file.read(&mark, sizeof(mark)) != static_cast<int>(sizeof(mark))) {
      writer.abort();
      return false;
    }
    if (!removed && strcmp(mark.bookmarkId, bookmarkId) == 0) {
      removed = true;
      continue;
    }
    if (!writer.append(mark)) {
      writer.abort();
      return false;
    }
  }
  if (!removed) {
    writer.abort();
    return false;
  }
  return writer.finish(header.syncKey, header.bookVersion, header.ownerVid);
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

namespace {

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

void copyField(char* destination, const size_t capacity, const char* value, const size_t len) {
  WeReadProtocol::decodeJsonString(value, len, destination, capacity);
}

}  // namespace

struct BookmarkListImporter::State {
  std::unique_ptr<StreamingJsonParser> parser;
  Writer writer;
  int depth = 0;
  int recordsDepth = -1;
  int recordDepth = -1;
  bool inRecords = false;
  bool inRecord = false;
  bool sawRecords = false;
  bool sawAnyKey = false;
  bool rootClosed = false;
  bool failed = false;
  int errorCode = 0;
  uint32_t bookVersion = 0;
  uint64_t syncKey = 0;
  bool typeSeen = false;
  uint8_t type = 0;
  enum class Field : uint8_t {
    None,
    SyncKey,
    ErrorCode,
    Records,
    BookmarkId,
    ChapterUid,
    Range,
    Type,
    BookVersion,
  } field = Field::None;
  char ownerVid[64] = {};
  char bookmarkId[32] = {};
  char chapterUid[24] = {};
  char rangeText[32] = {};
};

BookmarkListImporter::BookmarkListImporter() : state_(nullptr) {
  // State 含 2KB JSON 词法缓冲，不能放进阅读任务栈。一次拉取结束即释放。
  auto state = makeUniqueNoThrow<State>();
  if (!state) {
    LOG_ERR("WR", "OOM: highlight importer state (%zu bytes)", sizeof(State));
    return;
  }
  JsonCallbacks callbacks{};
  callbacks.ctx = state.get();
  callbacks.onKey = &BookmarkListImporter::onKey;
  callbacks.onString = &BookmarkListImporter::onString;
  callbacks.onNumber = &BookmarkListImporter::onNumber;
  callbacks.onBool = &BookmarkListImporter::onBool;
  callbacks.onNull = &BookmarkListImporter::onNull;
  callbacks.onObjectStart = &BookmarkListImporter::onObjectStart;
  callbacks.onObjectEnd = &BookmarkListImporter::onObjectEnd;
  callbacks.onArrayStart = &BookmarkListImporter::onArrayStart;
  callbacks.onArrayEnd = &BookmarkListImporter::onArrayEnd;
  state->parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks);
  if (!state->parser) {
    LOG_ERR("WR", "OOM: highlight JSON parser (%zu bytes)", sizeof(StreamingJsonParser));
    return;
  }
  state_ = state.release();
}

BookmarkListImporter::~BookmarkListImporter() {
  if (!state_) return;
  if (state_->writer.active()) state_->writer.abort();
  delete state_;
}

bool BookmarkListImporter::begin(const char* bookId, const char* ownerVid) {
  if (!state_ || !state_->parser) return false;
  if (ownerVid && ownerVid[0]) {
    strncpy(state_->ownerVid, ownerVid, sizeof(state_->ownerVid) - 1);
    state_->ownerVid[sizeof(state_->ownerVid) - 1] = '\0';
  } else {
    state_->ownerVid[0] = '\0';
  }
  state_->depth = 0;
  state_->recordsDepth = -1;
  state_->recordDepth = -1;
  state_->inRecords = false;
  state_->inRecord = false;
  state_->sawRecords = false;
  state_->sawAnyKey = false;
  state_->rootClosed = false;
  state_->failed = false;
  state_->errorCode = 0;
  state_->bookVersion = 0;
  state_->syncKey = 0;
  state_->typeSeen = false;
  state_->type = 0;
  state_->field = State::Field::None;
  state_->bookmarkId[0] = '\0';
  state_->chapterUid[0] = '\0';
  state_->rangeText[0] = '\0';
  state_->parser->reset();
  return state_->writer.begin(bookId);
}

bool BookmarkListImporter::feed(const uint8_t* data, const size_t len) {
  if (!state_ || !state_->parser || state_->failed) return false;
  if (!data && len != 0) return false;
  state_->parser->feed(reinterpret_cast<const char*>(data), len);
  return !state_->parser->hasError() && !state_->failed;
}

bool BookmarkListImporter::finish() {
  if (!state_ || !state_->parser || !state_->writer.active()) return false;
  const bool emptyObject = !state_->sawAnyKey && !state_->sawRecords;
  // {} 是错误 Referer 的空响应，不能覆盖已有 marks.bin。真正的空列表是 {"updated":[]}。
  if (state_->errorCode != 0 || state_->inRecord || !state_->rootClosed || emptyObject || !state_->sawRecords ||
      state_->parser->hasError() || state_->failed) {
    state_->writer.abort();
    return false;
  }
  return state_->writer.finish(state_->syncKey, state_->bookVersion, state_->ownerVid);
}

void BookmarkListImporter::abort() {
  if (state_) state_->writer.abort();
}

int BookmarkListImporter::errorCode() const { return state_ ? state_->errorCode : 0; }

void BookmarkListImporter::onKey(void* ctx, const char* key, size_t) {
  auto& state = *static_cast<State*>(ctx);
  state.sawAnyKey = true;
  if (state.depth == 1 && key && strcmp(key, "updated") == 0) {
    state.field = State::Field::Records;
  } else if (state.depth == 1 && key && (strcmp(key, "errcode") == 0 || strcmp(key, "errCode") == 0)) {
    state.field = State::Field::ErrorCode;
  } else if (state.depth == 1 && key && strcmp(key, "synckey") == 0) {
    state.field = State::Field::SyncKey;
  } else if (state.inRecord && key && strcmp(key, "bookmarkId") == 0) {
    state.field = State::Field::BookmarkId;
  } else if (state.inRecord && key && strcmp(key, "chapterUid") == 0) {
    state.field = State::Field::ChapterUid;
  } else if (state.inRecord && key && strcmp(key, "range") == 0) {
    state.field = State::Field::Range;
  } else if (state.inRecord && key && strcmp(key, "type") == 0) {
    state.field = State::Field::Type;
  } else if (key && strcmp(key, "bookVersion") == 0) {
    state.field = State::Field::BookVersion;
  } else {
    state.field = State::Field::None;
  }
}

void BookmarkListImporter::acceptValue(State& state, const char* value, const size_t len) {
  switch (state.field) {
    case State::Field::ErrorCode: {
      char number[24] = {};
      if (len < sizeof(number) && value) {
        memcpy(number, value, len);
        state.errorCode = atoi(number);
      }
      break;
    }
    case State::Field::SyncKey:
      state.syncKey = parseUint64(value, len);
      break;
    case State::Field::BookmarkId:
      copyField(state.bookmarkId, sizeof(state.bookmarkId), value, len);
      break;
    case State::Field::ChapterUid:
      copyField(state.chapterUid, sizeof(state.chapterUid), value, len);
      break;
    case State::Field::Range:
      copyField(state.rangeText, sizeof(state.rangeText), value, len);
      break;
    case State::Field::BookVersion:
      state.bookVersion = WeReadProtocol::parseUint32OrZero(value, len);
      break;
    case State::Field::Type: {
      const uint32_t type = WeReadProtocol::parseUint32OrZero(value, len);
      state.type = static_cast<uint8_t>(type > 255U ? 255U : type);
      state.typeSeen = true;
      break;
    }
    case State::Field::None:
    case State::Field::Records:
      break;
  }
  state.field = State::Field::None;
}

void BookmarkListImporter::onString(void* ctx, const char* value, const size_t len) {
  acceptValue(*static_cast<State*>(ctx), value, len);
}

void BookmarkListImporter::onNumber(void* ctx, const char* value, const size_t len) {
  acceptValue(*static_cast<State*>(ctx), value, len);
}

void BookmarkListImporter::onBool(void* ctx, bool) { static_cast<State*>(ctx)->field = State::Field::None; }

void BookmarkListImporter::onNull(void* ctx) { static_cast<State*>(ctx)->field = State::Field::None; }

void BookmarkListImporter::onObjectStart(void* ctx) {
  auto& state = *static_cast<State*>(ctx);
  ++state.depth;
  if (state.inRecords && !state.inRecord && state.depth == state.recordsDepth + 1) startRecord(state);
  state.field = State::Field::None;
}

void BookmarkListImporter::onObjectEnd(void* ctx) {
  auto& state = *static_cast<State*>(ctx);
  if (state.inRecord && state.depth == state.recordDepth) finishRecord(state);
  if (state.depth == 1) state.rootClosed = true;
  if (state.depth > 0) --state.depth;
  state.field = State::Field::None;
}

void BookmarkListImporter::onArrayStart(void* ctx) {
  auto& state = *static_cast<State*>(ctx);
  ++state.depth;
  if (state.field == State::Field::Records && !state.inRecords) {
    state.inRecords = true;
    state.sawRecords = true;
    state.recordsDepth = state.depth;
  }
  state.field = State::Field::None;
}

void BookmarkListImporter::onArrayEnd(void* ctx) {
  auto& state = *static_cast<State*>(ctx);
  if (state.inRecords && state.depth == state.recordsDepth) {
    state.inRecords = false;
    state.recordsDepth = -1;
  }
  if (state.depth > 0) --state.depth;
  state.field = State::Field::None;
}

void BookmarkListImporter::startRecord(State& state) {
  state.bookmarkId[0] = '\0';
  state.chapterUid[0] = '\0';
  state.rangeText[0] = '\0';
  state.type = 0;
  state.typeSeen = false;
  state.inRecord = true;
  state.recordDepth = state.depth;
}

void BookmarkListImporter::finishRecord(State& state) {
  if (state.typeSeen && state.type == 1 && state.bookmarkId[0] && state.rangeText[0]) {
    Mark mark;
    uint32_t chapterUid = 0;
    if (parseChapterUid(state.chapterUid, chapterUid) && parseRange(state.rangeText, mark.rangeStart, mark.rangeEnd)) {
      mark.chapterUid = chapterUid;
      mark.flags = Mine;
      strncpy(mark.bookmarkId, state.bookmarkId, sizeof(mark.bookmarkId) - 1);
      mark.bookmarkId[sizeof(mark.bookmarkId) - 1] = '\0';
      if (!state.writer.append(mark)) state.failed = true;
    }
  }
  state.inRecord = false;
  state.recordDepth = -1;
}

namespace {

struct RemovalParse {
  int depth = 0;
  int errorCode = 0;
  bool sawSucc = false;
  bool succ = false;
  bool rootClosed = false;
  enum class Field : uint8_t { None, Error, Succ } field = Field::None;
};

void removalKey(void* raw, const char* key, size_t) {
  auto& state = *static_cast<RemovalParse*>(raw);
  if (key && (strcmp(key, "errcode") == 0 || strcmp(key, "errCode") == 0)) {
    state.field = RemovalParse::Field::Error;
  } else if (key && strcmp(key, "succ") == 0) {
    state.field = RemovalParse::Field::Succ;
  } else {
    state.field = RemovalParse::Field::None;
  }
}

void removalNumber(void* raw, const char* value, const size_t len) {
  auto& state = *static_cast<RemovalParse*>(raw);
  if (state.field == RemovalParse::Field::Error) {
    char number[24] = {};
    if (value && len < sizeof(number)) {
      memcpy(number, value, len);
      state.errorCode = atoi(number);
    }
  } else if (state.field == RemovalParse::Field::Succ) {
    state.sawSucc = true;
    state.succ = WeReadProtocol::parseUint32OrZero(value, len) != 0;
  }
  state.field = RemovalParse::Field::None;
}

void removalBool(void* raw, const bool value) {
  auto& state = *static_cast<RemovalParse*>(raw);
  if (state.field == RemovalParse::Field::Succ) {
    state.sawSucc = true;
    state.succ = value;
  }
  state.field = RemovalParse::Field::None;
}

void removalIgnore(void* raw) { static_cast<RemovalParse*>(raw)->field = RemovalParse::Field::None; }

void removalObjectStart(void* raw) {
  auto& state = *static_cast<RemovalParse*>(raw);
  ++state.depth;
  state.field = RemovalParse::Field::None;
}

void removalObjectEnd(void* raw) {
  auto& state = *static_cast<RemovalParse*>(raw);
  if (state.depth == 1) state.rootClosed = true;
  if (state.depth > 0) --state.depth;
  state.field = RemovalParse::Field::None;
}

void removalArrayStart(void* raw) {
  auto& state = *static_cast<RemovalParse*>(raw);
  ++state.depth;
  state.field = RemovalParse::Field::None;
}

void removalArrayEnd(void* raw) {
  auto& state = *static_cast<RemovalParse*>(raw);
  if (state.depth > 0) --state.depth;
  state.field = RemovalParse::Field::None;
}

}  // namespace

bool removalSucceeded(const uint8_t* json, const size_t len) {
  if (!json && len != 0) return false;
  auto state = makeUniqueNoThrow<RemovalParse>();
  if (!state) {
    LOG_ERR("WR", "OOM: highlight removal parser");
    return false;
  }
  JsonCallbacks callbacks{};
  callbacks.ctx = state.get();
  callbacks.onKey = &removalKey;
  callbacks.onString = &removalNumber;
  callbacks.onNumber = &removalNumber;
  callbacks.onBool = &removalBool;
  callbacks.onNull = &removalIgnore;
  callbacks.onObjectStart = &removalObjectStart;
  callbacks.onObjectEnd = &removalObjectEnd;
  callbacks.onArrayStart = &removalArrayStart;
  callbacks.onArrayEnd = &removalArrayEnd;
  // 词法缓冲 2KB，不放进调用方栈。解析完即释放。
  auto parser = makeUniqueNoThrow<StreamingJsonParser>(callbacks);
  if (!parser) {
    LOG_ERR("WR", "OOM: highlight removal JSON parser (%zu bytes)", sizeof(StreamingJsonParser));
    return false;
  }
  parser->feed(reinterpret_cast<const char*>(json), len);
  return !parser->hasError() && state->rootClosed && state->errorCode == 0 && state->sawSucc && state->succ;
}

bool replaceMineFromBookmarkList(const char* bookId, const char* ownerVid, const uint8_t* json, const size_t len) {
  BookmarkListImporter importer;
  if (!importer.begin(bookId, ownerVid)) return false;
  if (!importer.feed(json, len)) {
    importer.abort();
    return false;
  }
  return importer.finish();
}

}  // namespace WeReadHighlight
