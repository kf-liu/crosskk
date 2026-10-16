#include <ObfuscationUtils.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>

#include "WeReadComment.h"

namespace obfuscation {

void xorTransform(std::string&) {}

void xorTransform(std::string& data, const uint8_t* key, const size_t keyLen) {
  if (!key || keyLen == 0) return;
  for (size_t i = 0; i < data.size(); ++i) data[i] ^= key[i % keyLen];
}

String obfuscateToBase64(const std::string& plaintext) { return String(plaintext.c_str()); }

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  if (ok) *ok = encoded != nullptr;
  return encoded ? encoded : "";
}

void selfTest() {}

}  // namespace obfuscation

namespace {

class WeReadCommentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> serial{0};
    root_ = std::filesystem::temp_directory_path() /
            ("crossmux-weread-comments-" + std::to_string(serial.fetch_add(1)));
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    ASSERT_TRUE(std::filesystem::create_directories(root_, error));
    ASSERT_FALSE(error);
    ASSERT_EQ(setenv("CROSSPOINT_SIM_SD", root_.c_str(), 1), 0);
    ASSERT_TRUE(Storage.begin());
  }

  void TearDown() override {
    unsetenv("CROSSPOINT_SIM_SD");
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  bool feed(WeReadComment::UnderlineParser& parser, const std::string& json) {
    for (size_t offset = 0; offset < json.size();) {
      const size_t chunk = std::min<size_t>((offset % 5) + 1, json.size() - offset);
      if (!parser.feed(reinterpret_cast<const uint8_t*>(json.data() + offset), chunk)) return false;
      offset += chunk;
    }
    return parser.finish();
  }

  bool feed(WeReadComment::ReviewPageParser& parser, const std::string& json) {
    for (size_t offset = 0; offset < json.size();) {
      const size_t chunk = std::min<size_t>((offset % 5) + 1, json.size() - offset);
      if (!parser.feed(reinterpret_cast<const uint8_t*>(json.data() + offset), chunk)) return false;
      offset += chunk;
    }
    return true;
  }

  std::filesystem::path root_;
};

std::string nineUnderlines() {
  std::string json = "{\"count\":9,\"underlines\":[";
  for (int i = 0; i < 9; ++i) {
    if (i) json += ',';
    json += "{\"count\":" + std::to_string(i + 1) + ",\"score\":8,\"type\":1,\"range\":\"" + std::to_string(i * 10) +
            "-" + std::to_string(i * 10 + 4) + "\"}";
  }
  json += "]}";
  return json;
}

std::string reviewItem(const int index, const char* authorKey) {
  return std::string("{\"review\":{\"abstract\":\"ignored\",\"content\":\"评论") + std::to_string(index) +
         "\",\"author\":{\"" + authorKey + "\":\"读者" + std::to_string(index) + "\"}}}";
}

std::string reviewPage(const char* range, const bool hasMore, const uint32_t maxIdx, const uint64_t syncKey,
                       const int first, const int count, const char* authorKey) {
  std::string json = std::string("{\"reviews\":[{\"range\":\"") + range + "\",\"totalCount\":9,\"hasMore\":" +
                     (hasMore ? "true" : "false") + ",\"maxIdx\":" + std::to_string(maxIdx) +
                     ",\"synckey\":" + std::to_string(syncKey) + ",\"pageReviews\":[";
  for (int i = 0; i < count; ++i) {
    if (i) json += ',';
    json += reviewItem(first + i, authorKey);
  }
  json += "]}]}";
  return json;
}

struct ScriptedPages {
  const char* pages[4] = {};
  uint32_t expectMax[4] = {};
  uint64_t expectSync[4] = {};
  int calls = 0;
};

bool scriptedFetch(void* raw, const uint32_t maxIdx, const uint64_t syncKey, WeReadComment::RangeFile& file,
                   WeReadComment::Cursor& cursor) {
  auto* script = static_cast<ScriptedPages*>(raw);
  if (script->calls >= 4 || !script->pages[script->calls]) return false;
  EXPECT_EQ(maxIdx, script->expectMax[script->calls]);
  EXPECT_EQ(syncKey, script->expectSync[script->calls]);
  const std::string json = script->pages[script->calls];
  ++script->calls;
  WeReadComment::ReviewPageParser parser;
  if (!parser.begin(&file, "10-40")) return false;
  for (size_t offset = 0; offset < json.size();) {
    const size_t chunk = std::min<size_t>((offset % 5) + 1, json.size() - offset);
    if (!parser.feed(reinterpret_cast<const uint8_t*>(json.data() + offset), chunk)) return false;
    offset += chunk;
  }
  return parser.finish(cursor);
}

TEST_F(WeReadCommentTest, KeepsNineUnderlineRangesAndIgnoresHeat) {
  WeReadComment::UnderlineParser parser;
  WeReadComment::Range ranges[16];
  ASSERT_TRUE(parser.begin(ranges, 16));
  ASSERT_TRUE(feed(parser, nineUnderlines()));
  EXPECT_EQ(parser.count(), 9u);
  EXPECT_FALSE(parser.truncated());
  EXPECT_EQ(ranges[8].start, 80u);
  EXPECT_EQ(ranges[8].end, 84u);
}

TEST_F(WeReadCommentTest, StoresNineCommentsWithoutStoppingAtEight) {
  WeReadComment::RangeFile file;
  ASSERT_TRUE(file.begin("3300003224", 12, 10, 40, false));
  WeReadComment::ReviewPageParser parser;
  ASSERT_TRUE(parser.begin(&file, "10-40"));
  const std::string json = reviewPage("10-40", false, 9, 77, 1, 9, "nick");
  ASSERT_TRUE(feed(parser, json));
  WeReadComment::Cursor cursor;
  ASSERT_TRUE(parser.finish(cursor));
  EXPECT_EQ(cursor.added, 9u);
  EXPECT_EQ(cursor.maxIdx, 9u);
  EXPECT_EQ(cursor.syncKey, 77u);
  EXPECT_FALSE(cursor.hasMore);
  ASSERT_TRUE(file.finish(cursor.maxIdx, cursor.syncKey, cursor.hasMore));

  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 8, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论9");
  EXPECT_STREQ(comment.author, "读者9");
  EXPECT_EQ(comment.count, 9u);
}

TEST_F(WeReadCommentTest, PagesWithMaxIdxAndSyncKey) {
  const std::string first = reviewPage("10-40", true, 2, 11, 1, 2, "name");
  const std::string second = reviewPage("10-40", false, 4, 19, 3, 2, "userName");
  ScriptedPages script;
  script.pages[0] = first.c_str();
  script.pages[1] = second.c_str();
  script.expectMax[0] = 0;
  script.expectSync[0] = 0;
  script.expectMax[1] = 2;
  script.expectSync[1] = 11;

  EXPECT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &script),
            WeReadComment::CollectResult::Stored);
  EXPECT_EQ(script.calls, 2);

  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 3, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论4");
  EXPECT_STREQ(comment.author, "读者4");
  EXPECT_EQ(comment.count, 4u);
  EXPECT_FALSE(comment.hasMore);
  EXPECT_EQ(comment.maxIdx, 4u);
  EXPECT_EQ(comment.syncKey, 19u);
}

TEST_F(WeReadCommentTest, EmptyResultDoesNotCreateOrReplaceAFile) {
  const std::string existing = reviewPage("10-40", false, 1, 5, 1, 1, "name");
  ScriptedPages first;
  first.pages[0] = existing.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &first),
            WeReadComment::CollectResult::Stored);

  const std::string empty =
      "{\"reviews\":[{\"range\":\"10-40\",\"hasMore\":0,\"maxIdx\":0,\"synckey\":0,\"pageReviews\":[]}]}";
  ScriptedPages second;
  second.pages[0] = empty.c_str();
  EXPECT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &second),
            WeReadComment::CollectResult::Empty);

  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 0, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论1");

  const std::string fresh = "{\"reviews\":[]}";
  ScriptedPages third;
  third.pages[0] = fresh.c_str();
  EXPECT_EQ(WeReadComment::collectRange("3300003225", 3, 1, 2, 0, 0, false, scriptedFetch, &third),
            WeReadComment::CollectResult::Empty);
  EXPECT_TRUE(WeReadComment::rangePath("3300003225", 3, 1, 2).size() > 0);
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003225/3/1_2.wrn"));
}

TEST_F(WeReadCommentTest, FailedPageKeepsThePreviousFile) {
  const std::string existing = reviewPage("10-40", false, 1, 5, 1, 1, "name");
  ScriptedPages first;
  first.pages[0] = existing.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &first),
            WeReadComment::CollectResult::Stored);

  ScriptedPages failed;
  EXPECT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, true, scriptedFetch, &failed),
            WeReadComment::CollectResult::Failed);

  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 0, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论1");
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003224/12/10_40.wrn.part"));
}

TEST_F(WeReadCommentTest, RejectsOfficialAccountBooks) {
  EXPECT_FALSE(WeReadComment::isStandardBook("MP_WXS_123"));
  EXPECT_FALSE(WeReadComment::isStandardBook(""));
  EXPECT_TRUE(WeReadComment::isStandardBook("3300003224"));
  EXPECT_EQ(WeReadComment::collectRange("MP_WXS_123", 1, 10, 20, 0, 0, false, scriptedFetch, nullptr),
            WeReadComment::CollectResult::Failed);
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments"));
}

}  // namespace
