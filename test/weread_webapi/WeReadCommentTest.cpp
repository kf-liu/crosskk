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
  std::string json = std::string("{\"reviews\":[{\"range\":\"") + range + "\",\"bookMarkCount\":12,\"totalCount\":9,\"hasMore\":" +
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
  EXPECT_EQ(ranges[0].markCount, 1u);
  EXPECT_EQ(ranges[8].start, 80u);
  EXPECT_EQ(ranges[8].end, 84u);
  EXPECT_EQ(ranges[8].markCount, 9u);
}

TEST_F(WeReadCommentTest, ReadsPeopleCountFromTotalCountWhenCountIsZero) {
  WeReadComment::UnderlineParser parser;
  WeReadComment::Range ranges[8] = {};
  ASSERT_TRUE(parser.begin(ranges, 8));
  const std::string json =
      R"({"count":2,"underlines":[{"range":"10-20","count":0,"score":8,"type":1,"totalCount":42},)"
      R"({"range":"10-20","count":"7","type":2},{"range":"30-40","count":"15","score":1}]})";
  ASSERT_TRUE(feed(parser, json));
  EXPECT_EQ(parser.count(), 2u);
  EXPECT_EQ(ranges[0].start, 10u);
  EXPECT_EQ(ranges[0].end, 20u);
  EXPECT_EQ(ranges[0].markCount, 42u);
  EXPECT_EQ(ranges[1].start, 30u);
  EXPECT_EQ(ranges[1].end, 40u);
  EXPECT_EQ(ranges[1].markCount, 15u);
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
  EXPECT_EQ(cursor.markCount, 12u);
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

TEST_F(WeReadCommentTest, StoresEveryCandidateWithoutACap) {
  WeReadComment::CandidateFile file;
  ASSERT_TRUE(file.begin("3300003224", 9));
  for (uint32_t i = 0; i < 70; ++i) {
    ASSERT_TRUE(file.add(i * 10, i * 10 + 4, i == 3 ? 1u : i + 1));
  }
  ASSERT_TRUE(file.add(30, 34, 99));
  EXPECT_EQ(file.count(), 70u);
  ASSERT_TRUE(file.finish());
  ASSERT_TRUE(WeReadComment::candidatesReady("3300003224", 9));
  WeReadComment::Candidate item;
  ASSERT_TRUE(WeReadComment::readCandidate("3300003224", 9, 3, item));
  EXPECT_EQ(item.start, 30u);
  EXPECT_EQ(item.end, 34u);
  EXPECT_EQ(item.markCount, 99u);
  uint32_t seen = 0;
  auto visit = [](void* raw, uint32_t, const WeReadComment::Candidate&) {
    ++*static_cast<uint32_t*>(raw);
    return true;
  };
  ASSERT_TRUE(WeReadComment::visitCandidates("3300003224", 9, visit, &seen));
  EXPECT_EQ(seen, 70u);
}

TEST_F(WeReadCommentTest, StagedRefreshReplacesOnlyAfterCommit) {
  const std::string oldPage = reviewPage("10-40", false, 1, 5, 1, 1, "name");
  ScriptedPages oldScript;
  oldScript.pages[0] = oldPage.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &oldScript),
            WeReadComment::CollectResult::Stored);

  const std::string fresh = reviewPage("10-40", false, 2, 8, 9, 1, "name");
  ScriptedPages next;
  next.pages[0] = fresh.c_str();
  EXPECT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &next,
                                        WeReadComment::kMaxReviewPages, nullptr, nullptr, true, true),
            WeReadComment::CollectResult::Stored);

  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 0, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论1");
  EXPECT_TRUE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003224/12/10_40.wrn.new"));

  WeReadComment::CandidateFile index;
  ASSERT_TRUE(index.begin("3300003224", 12));
  ASSERT_TRUE(index.add(10, 40, 1));
  ASSERT_TRUE(index.finish(true));
  ASSERT_TRUE(WeReadComment::commitStagedChapter("3300003224", 12));
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 0, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论9");
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003224/12/10_40.wrn.new"));
  EXPECT_TRUE(WeReadComment::candidatesReady("3300003224", 12));
}

TEST_F(WeReadCommentTest, DiscardingAStagedRefreshKeepsThePreviousComments) {
  const std::string oldPage = reviewPage("10-40", false, 1, 5, 1, 1, "name");
  ScriptedPages oldScript;
  oldScript.pages[0] = oldPage.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &oldScript),
            WeReadComment::CollectResult::Stored);

  const std::string fresh = reviewPage("10-40", false, 2, 8, 9, 1, "name");
  ScriptedPages next;
  next.pages[0] = fresh.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &next,
                                        WeReadComment::kMaxReviewPages, nullptr, nullptr, true, true),
            WeReadComment::CollectResult::Stored);
  WeReadComment::CandidateFile index;
  ASSERT_TRUE(index.begin("3300003224", 12));
  ASSERT_TRUE(index.add(10, 40, 1));
  ASSERT_TRUE(index.finish(true));

  WeReadComment::discardStagedChapter("3300003224", 12);
  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 0, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论1");
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003224/12/10_40.wrn.new"));
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003224/12/ranges.wri.new"));
}

TEST_F(WeReadCommentTest, FailedStagedChapterDoesNotReplaceComments) {
  const std::string oldPage = reviewPage("10-40", false, 1, 5, 1, 1, "name");
  ScriptedPages oldScript;
  oldScript.pages[0] = oldPage.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &oldScript),
            WeReadComment::CollectResult::Stored);

  WeReadComment::CandidateFile index;
  ASSERT_TRUE(index.begin("3300003224", 12));
  ASSERT_TRUE(index.add(10, 40, 1));
  ASSERT_TRUE(index.finish(true));
  WeReadComment::Candidate item;
  ASSERT_TRUE(WeReadComment::readCandidate("3300003224", 12, 0, item, true));
  item.query = WeReadComment::CandidateQuery::Failed;
  ASSERT_TRUE(WeReadComment::updateCandidate("3300003224", 12, 0, item, true));

  EXPECT_FALSE(WeReadComment::commitStagedChapter("3300003224", 12));
  char text[64];
  WeReadComment::StoredComment comment;
  ASSERT_TRUE(WeReadComment::readComment("3300003224", 12, 10, 40, 0, text, sizeof(text), comment));
  EXPECT_STREQ(text, "评论1");
}

TEST_F(WeReadCommentTest, SuccessfulEmptyRangeClearsThePreviousComments) {
  const std::string oldPage = reviewPage("10-40", false, 1, 5, 1, 1, "name");
  ScriptedPages oldScript;
  oldScript.pages[0] = oldPage.c_str();
  ASSERT_EQ(WeReadComment::collectRange("3300003224", 12, 10, 40, 0, 0, false, scriptedFetch, &oldScript),
            WeReadComment::CollectResult::Stored);

  WeReadComment::CandidateFile index;
  ASSERT_TRUE(index.begin("3300003224", 12));
  ASSERT_TRUE(index.add(10, 40, 1));
  ASSERT_TRUE(index.finish(true));
  WeReadComment::Candidate item;
  ASSERT_TRUE(WeReadComment::readCandidate("3300003224", 12, 0, item, true));
  item.query = WeReadComment::CandidateQuery::Empty;
  ASSERT_TRUE(WeReadComment::updateCandidate("3300003224", 12, 0, item, true));

  ASSERT_TRUE(WeReadComment::commitStagedChapter("3300003224", 12));
  EXPECT_FALSE(std::filesystem::exists(root_ / ".crosspoint/weread/comments/3300003224/12/10_40.wrn"));
}

TEST_F(WeReadCommentTest, RecordsWhenAChapterFetchFinishes) {
  EXPECT_EQ(WeReadComment::chapterFetchedAt("3300003224", 9), 0u);
  EXPECT_FALSE(WeReadComment::markChapterFetched("3300003224", 9, 0));
  ASSERT_TRUE(WeReadComment::markChapterFetched("3300003224", 9, 1700000000u));
  EXPECT_EQ(WeReadComment::chapterFetchedAt("3300003224", 9), 1700000000u);
  ASSERT_TRUE(WeReadComment::clearChapterFetched("3300003224", 9));
  EXPECT_EQ(WeReadComment::chapterFetchedAt("3300003224", 9), 0u);
  EXPECT_TRUE(WeReadComment::clearChapterFetched("3300003224", 9));
}

}  // namespace
