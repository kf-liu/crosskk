#include "WeReadHighlight.h"

#ifdef ENABLE_CHINESE_VERSION

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "WeReadHttpClient.h"
#include "WeReadStore.h"
#include "WeReadXhtmlCodec.h"

namespace WeReadHighlight {
namespace {

constexpr const char* kHost = "https://weread.qq.com";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 "
    "Safari/537.36 Edg/135.0.0.0";
constexpr char kRenewBody[] = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";

bool bookIdJsonSafe(const char* bookId) {
  if (!bookId || !bookId[0]) return false;
  for (const char* cursor = bookId; *cursor; ++cursor) {
    const unsigned char ch = static_cast<unsigned char>(*cursor);
    if (ch <= 0x20 || ch >= 0x7F || ch == '"' || ch == '\\' || ch == '<' || ch == '>') return false;
  }
  return true;
}

bool headerNameIs(const char* name, const char* expected) {
  if (!name || !expected) return false;
  for (size_t i = 0; expected[i]; ++i) {
    unsigned char ch = static_cast<unsigned char>(name[i]);
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch - 'A' + 'a');
    if (ch != static_cast<unsigned char>(expected[i])) return false;
  }
  return name[strlen(expected)] == '\0';
}

void absorbSetCookie(WeReadStore::Session& session, const char* name, const char* value) {
  if (!headerNameIs(name, "set-cookie") || !value) return;
  while (*value == ' ') ++value;
  const char* equals = strchr(value, '=');
  if (!equals || equals - value < 3 || memcmp(value, "wr_", 3) != 0) return;
  size_t nameLen = static_cast<size_t>(equals - value);
  while (nameLen > 0 && value[nameLen - 1] == ' ') --nameLen;
  char cookieName[16];
  if (nameLen == 0 || nameLen >= sizeof(cookieName)) return;
  memcpy(cookieName, value, nameLen);
  cookieName[nameLen] = '\0';
  if (strcmp(cookieName, "wr_vid") != 0 && strcmp(cookieName, "wr_skey") != 0 && strcmp(cookieName, "wr_rt") != 0) {
    return;
  }
  const char* start = equals + 1;
  while (*start == ' ') ++start;
  const char* end = strchr(start, ';');
  if (!end) end = start + strlen(start);
  while (end > start && end[-1] == ' ') --end;
  session.setCookie(cookieName, start, static_cast<size_t>(end - start));
}

int jsonErrorCode(const uint8_t* json, const size_t len) {
  const char* text = reinterpret_cast<const char*>(json);
  for (size_t i = 0; i + 9 < len; ++i) {
    if (memcmp(text + i, "\"errCode\"", 9) != 0 && memcmp(text + i, "\"errcode\"", 9) != 0) continue;
    size_t j = i + 9;
    while (j < len && (text[j] == ' ' || text[j] == ':')) ++j;
    int sign = 1;
    if (j < len && text[j] == '-') {
      sign = -1;
      ++j;
    }
    if (j >= len || text[j] < '0' || text[j] > '9') return 0;
    int value = 0;
    while (j < len && text[j] >= '0' && text[j] <= '9') {
      value = value * 10 + (text[j] - '0');
      ++j;
    }
    return sign * value;
  }
  return 0;
}

bool jsonSucc(const uint8_t* json, const size_t len) {
  if (jsonErrorCode(json, len) != 0) return false;
  const char* text = reinterpret_cast<const char*>(json);
  for (size_t i = 0; i + 6 < len; ++i) {
    if (memcmp(text + i, "\"succ\"", 6) != 0) continue;
    size_t j = i + 6;
    while (j < len && (text[j] == ' ' || text[j] == ':')) ++j;
    if (j < len && text[j] == '1') return true;
    if (j + 4 <= len && memcmp(text + j, "true", 4) == 0) return true;
  }
  return false;
}

size_t base64Encode(const uint8_t* input, const size_t inputLen, char* out, const size_t outSize) {
  static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t need = 4 * ((inputLen + 2) / 3) + 1;
  if (!out || need > outSize) return 0;
  size_t written = 0;
  for (size_t i = 0; i < inputLen; i += 3) {
    const uint32_t combined = (static_cast<uint32_t>(input[i]) << 16) |
                              (i + 1 < inputLen ? static_cast<uint32_t>(input[i + 1]) << 8 : 0) |
                              (i + 2 < inputLen ? static_cast<uint32_t>(input[i + 2]) : 0);
    out[written++] = kTable[(combined >> 18) & 63];
    out[written++] = kTable[(combined >> 12) & 63];
    out[written++] = i + 1 < inputLen ? kTable[(combined >> 6) & 63] : '=';
    out[written++] = i + 2 < inputLen ? kTable[combined & 63] : '=';
  }
  out[written] = '\0';
  return written;
}

struct HttpBody {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t len = 0;
  bool overflow = false;
};

bool postJson(WeReadStore::Session& session, const char* url, const char* referer, const uint8_t* payload,
              const size_t payloadLen, char* cookie, const size_t cookieSize, uint8_t* readBuffer,
              const size_t readSize, HttpBody& response) {
  if (!session.cookieHeader(cookie, cookieSize)) return false;
  response.len = 0;
  response.overflow = false;
  WeReadHttpClient::Header headers[6] = {};
  size_t headerCount = 0;
  headers[headerCount++] = {"User-Agent", kUserAgent};
  headers[headerCount++] = {"Accept", "application/json, text/plain, */*"};
  headers[headerCount++] = {"Origin", kHost};
  headers[headerCount++] = {"Referer", referer};
  if (payloadLen > 0) headers[headerCount++] = {"Content-Type", "application/json;charset=UTF-8"};
  if (cookie[0]) headers[headerCount++] = {"Cookie", cookie};

  WeReadHttpClient::RequestOptions options;
  options.method = "POST";
  options.body = payload;
  options.bodySize = payloadLen;
  options.headers = headers;
  options.headerCount = headerCount;
  options.timeoutMs = 20000;
  options.readBuffer = readBuffer;
  options.readBufferSize = readSize;

  const auto onData = [&response](const uint8_t* data, const size_t len) {
    if (!data && len != 0) return false;
    if (response.len + len > response.capacity) {
      response.overflow = true;
      return false;
    }
    if (len > 0) memcpy(response.data + response.len, data, len);
    response.len += len;
    return true;
  };
  const auto onHeader = [&session](const char* name, const char* value) { absorbSetCookie(session, name, value); };
  int status = 0;
  const auto result = WeReadHttpClient::request(url, options, onData, onHeader, status);
  return result == WeReadHttpClient::Result::Ok && status == 200 && !response.overflow;
}

bool renewSession(WeReadStore::Session& session, char* cookie, const size_t cookieSize, uint8_t* readBuffer,
                  const size_t readSize, uint8_t* response, const size_t responseSize) {
  if (!session.rt[0]) return false;
  char url[96];
  const int urlLen = snprintf(url, sizeof(url), "%s/web/login/renewal", kHost);
  if (urlLen <= 0 || static_cast<size_t>(urlLen) >= sizeof(url)) return false;
  HttpBody body;
  body.data = response;
  body.capacity = responseSize;
  if (!postJson(session, url, kHost, reinterpret_cast<const uint8_t*>(kRenewBody), sizeof(kRenewBody) - 1, cookie,
                cookieSize, readBuffer, readSize, body)) {
    return false;
  }
  if (!jsonSucc(response, body.len) || !session.valid()) return false;
  return WeReadStore::saveSession(session);
}

uint32_t storedBookVersion(const char* bookId) {
  const std::string path = indexPath(bookId);
  HalFile file;
  Header header;
  if (path.empty() || !Storage.openFileForRead("WR", path, file) ||
      file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != kMagic ||
      header.version != kVersion) {
    return 0;
  }
  return header.bookVersion;
}

}  // namespace

RefreshResult createMine(const char* bookId, const char* chapterPath, const uint32_t chapterUid,
                         const uint32_t chapterIdx, const uint32_t visibleStart, const uint32_t visibleEnd) {
  if (!bookIdJsonSafe(bookId) || !chapterPath || !chapterPath[0] || visibleEnd <= visibleStart) {
    return RefreshResult::Failed;
  }
  if (!WeReadHttpClient::networkReady()) return RefreshResult::Skipped;
  WeReadStore::Session session;
  if (!WeReadStore::loadSession(session) || !session.valid()) return RefreshResult::Skipped;

  uint32_t nativeStart = 0;
  uint32_t nativeEnd = 0;
  if (!WeReadXhtmlCodec::visibleToNativeOffset(chapterPath, visibleStart, nativeStart) ||
      !WeReadXhtmlCodec::visibleToNativeOffset(chapterPath, visibleEnd, nativeEnd) || nativeEnd <= nativeStart) {
    LOG_ERR("WR", "highlight range could not be mapped");
    return RefreshResult::Failed;
  }

  // 阅读栈 8KB。正文、Base64 和响应都放堆上，请求结束就释放。
  constexpr size_t kCookieSize = 896;
  constexpr size_t kReadSize = 1024;
  constexpr size_t kResponseSize = 2048;
  constexpr size_t kTextSize = 1024;
  constexpr size_t kBodySize = 2048;
  constexpr size_t kTotal = kCookieSize + kReadSize + kResponseSize + kTextSize + kBodySize;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kTotal);
  if (!buffer) {
    LOG_ERR("WR", "OOM: %u-byte highlight create buffers", static_cast<unsigned>(kTotal));
    return RefreshResult::Failed;
  }
  char* cookie = reinterpret_cast<char*>(buffer.get());
  uint8_t* readBuffer = buffer.get() + kCookieSize;
  uint8_t* response = readBuffer + kReadSize;
  char* text = reinterpret_cast<char*>(response + kResponseSize);
  char* body = text + kTextSize;
  cookie[0] = '\0';

  size_t textLen = 0;
  if (!WeReadXhtmlCodec::copyVisibleText(chapterPath, visibleStart, visibleEnd, text, kTextSize, textLen) ||
      textLen == 0) {
    LOG_ERR("WR", "highlight text could not be read");
    return RefreshResult::Failed;
  }
  // Base64 先写进响应缓冲，组好 JSON 后再拿这块缓冲收响应。
  if (base64Encode(reinterpret_cast<const uint8_t*>(text), textLen, reinterpret_cast<char*>(response),
                   kResponseSize) == 0) {
    return RefreshResult::Failed;
  }
  const uint32_t bookVersion = storedBookVersion(bookId);
  const int bodyLen = snprintf(body, kBodySize,
                               "{\"bookId\":\"%s\",\"chapterUid\":%u,\"chapterIdx\":%u,\"bookVersion\":%u,"
                               "\"type\":1,\"style\":0,\"colorStyle\":0,\"range\":\"%u-%u\",\"markText\":\"%s\"}",
                               bookId, chapterUid, chapterIdx, bookVersion, nativeStart, nativeEnd,
                               reinterpret_cast<char*>(response));
  if (bodyLen <= 0 || static_cast<size_t>(bodyLen) >= kBodySize) return RefreshResult::Failed;

  char referer[160];
  const int refererLen = snprintf(referer, sizeof(referer), "%s/web/reader/%s", kHost, bookId);
  if (refererLen <= 0 || static_cast<size_t>(refererLen) >= sizeof(referer)) return RefreshResult::Failed;
  char url[192];
  const int urlLen = snprintf(url, sizeof(url), "%s/web/book/addBookmark", kHost);
  if (urlLen <= 0 || static_cast<size_t>(urlLen) >= sizeof(url)) return RefreshResult::Failed;

  HttpBody httpBody;
  httpBody.data = response;
  httpBody.capacity = kResponseSize;
  bool posted = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    posted = postJson(session, url, referer, reinterpret_cast<const uint8_t*>(body), static_cast<size_t>(bodyLen),
                      cookie, kCookieSize, readBuffer, kReadSize, httpBody);
    if (posted && jsonErrorCode(response, httpBody.len) != -2012) break;
    if (attempt == 0 && renewSession(session, cookie, kCookieSize, readBuffer, kReadSize, response, kResponseSize)) {
      continue;
    }
    posted = false;
    break;
  }
  if (!posted) {
    LOG_ERR("WR", "highlight create failed");
    return RefreshResult::Failed;
  }
  if (!WeReadStore::saveSession(session)) LOG_ERR("WR", "highlight created but the session was not saved");

  Mark mark;
  if (!creationBookmarkId(response, httpBody.len, mark.bookmarkId, sizeof(mark.bookmarkId))) {
    LOG_ERR("WR", "highlight create response had no bookmark id");
    return RefreshResult::Failed;
  }
  mark.chapterUid = chapterUid;
  mark.rangeStart = nativeStart;
  mark.rangeEnd = nativeEnd;
  mark.flags = Flag::Mine;
  if (!insertMine(bookId, mark)) {
    LOG_ERR("WR", "highlight created on the server but the local index was kept");
    return RefreshResult::Failed;
  }
  return RefreshResult::Updated;
}

}  // namespace WeReadHighlight

#endif
