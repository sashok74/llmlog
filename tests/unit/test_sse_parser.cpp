// Unit tests for SseFramer + UsageAccumulator.
//
// Tests fall into two categories:
//   1. Protocol-level tests with hand-crafted byte strings exercising edge
//      cases of SSE framing (multi-line data, CR/LF, comments, partial feeds).
//   2. End-to-end tests that feed tests/fixtures/*.sse byte-for-byte and
//      verify the extracted usage matches known-good numbers from live
//      upstream captures.

#include <gtest/gtest.h>

#include "llmlog/core/sse_parser.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using llmlog::core::ProviderFamily;
using llmlog::core::SseEvent;
using llmlog::core::SseFramer;
using llmlog::core::UsageAccumulator;
using llmlog::core::UsageInfo;

namespace {

std::string readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

/// Locates the tests/fixtures directory regardless of where the test binary
/// is invoked from. Tries CMake-injected define first, then walks upward
/// from the current path looking for the repo layout.
fs::path findFixturesDir() {
#ifdef LLMLOG_FIXTURES_DIR
    fs::path p{LLMLOG_FIXTURES_DIR};
    if (fs::exists(p)) return p;
#endif
    for (auto p = fs::current_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        auto candidate = p / "tests" / "fixtures";
        if (fs::exists(candidate / "anthropic_stream.sse")) return candidate;
    }
    return {};
}

std::vector<SseEvent> feedAll(std::string_view bytes) {
    std::vector<SseEvent> out;
    SseFramer fr;
    auto cb = [&](const SseEvent& e) { out.push_back(e); };
    fr.feed(bytes, cb);
    fr.finish(cb);
    return out;
}

} // namespace

// ============================================================================
//  Protocol-level framer tests
// ============================================================================
TEST(SseFramer, SingleEventSingleDataLine) {
    auto events = feedAll("data: hello\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "");
    EXPECT_EQ(events[0].data, "hello");
}

TEST(SseFramer, NamedEvent) {
    auto events = feedAll("event: message_delta\ndata: {\"a\":1}\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "message_delta");
    EXPECT_EQ(events[0].data, "{\"a\":1}");
}

TEST(SseFramer, MultipleEvents) {
    auto events = feedAll(
        "event: a\ndata: 1\n\n"
        "event: b\ndata: 2\n\n"
        "data: 3\n\n");
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].name, "a"); EXPECT_EQ(events[0].data, "1");
    EXPECT_EQ(events[1].name, "b"); EXPECT_EQ(events[1].data, "2");
    EXPECT_EQ(events[2].name, "");  EXPECT_EQ(events[2].data, "3");
}

TEST(SseFramer, MultilineDataJoinsWithNewline) {
    auto events = feedAll("data: line one\ndata: line two\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "line one\nline two");
}

TEST(SseFramer, CommentsIgnored) {
    auto events = feedAll(
        ": this is a heartbeat\n"
        "data: real\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "real");
}

TEST(SseFramer, CarriageReturnsStripped) {
    auto events = feedAll("event: x\r\ndata: y\r\n\r\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "x");
    EXPECT_EQ(events[0].data, "y");
}

TEST(SseFramer, PartialFeedsReassemble) {
    std::vector<SseEvent> events;
    SseFramer fr;
    auto cb = [&](const SseEvent& e) { events.push_back(e); };
    fr.feed("event: del", cb);         // no event yet
    EXPECT_TRUE(events.empty());
    fr.feed("ta\ndata: {\"x\":", cb);  // still incomplete
    EXPECT_TRUE(events.empty());
    fr.feed("1}\n\n", cb);             // complete
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "delta");
    EXPECT_EQ(events[0].data, "{\"x\":1}");
}

TEST(SseFramer, UnterminatedFinalEventFlushedOnFinish) {
    std::vector<SseEvent> events;
    SseFramer fr;
    auto cb = [&](const SseEvent& e) { events.push_back(e); };
    fr.feed("event: late\ndata: payload\n", cb);  // no empty-line terminator
    EXPECT_TRUE(events.empty());
    fr.finish(cb);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].name, "late");
    EXPECT_EQ(events[0].data, "payload");
}

TEST(SseFramer, DoneSentinelDeliveredAsEvent) {
    // Parser itself doesn't interpret [DONE]; it's just an event whose
    // data is literally "[DONE]". UsageAccumulator filters it.
    auto events = feedAll("data: [DONE]\n\n");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "[DONE]");
}

// ============================================================================
//  Semantic tests — real-captured upstream fixtures
// ============================================================================
class FixtureTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = findFixturesDir();
        if (dir_.empty()) GTEST_SKIP() << "tests/fixtures not found from cwd";
    }
    fs::path dir_;
};

TEST_F(FixtureTest, AnthropicFixtureYieldsExpectedUsage) {
    const auto bytes = readBytes(dir_ / "anthropic_stream.sse");
    ASSERT_FALSE(bytes.empty()) << "fixture file missing or empty";

    SseFramer         fr;
    UsageAccumulator  acc(ProviderFamily::Anthropic);
    fr.feed(bytes, [&](const SseEvent& e) { acc.onEvent(e); });
    fr.finish([&](const SseEvent& e) { acc.onEvent(e); });

    ASSERT_TRUE(acc.usage().has_value()) << "no usage extracted from anthropic fixture";
    EXPECT_EQ(acc.usage()->input_tokens,  20);
    EXPECT_EQ(acc.usage()->output_tokens, 12);
    EXPECT_EQ(acc.usage()->cache_read_tokens,  0);
    EXPECT_EQ(acc.usage()->cache_write_tokens, 0);

    ASSERT_TRUE(acc.modelId().has_value());
    // Anthropic may return either the alias or the dated form
    // ('claude-haiku-4-5-20251001'); match on the prefix.
    EXPECT_NE(acc.modelId()->find("claude-haiku-4-5"), std::string::npos);
}

TEST_F(FixtureTest, OpenAIFixtureYieldsExpectedUsage) {
    const auto bytes = readBytes(dir_ / "openai_stream.sse");
    ASSERT_FALSE(bytes.empty());

    SseFramer         fr;
    UsageAccumulator  acc(ProviderFamily::OpenAICompat);
    fr.feed(bytes, [&](const SseEvent& e) { acc.onEvent(e); });
    fr.finish([&](const SseEvent& e) { acc.onEvent(e); });

    ASSERT_TRUE(acc.usage().has_value());
    EXPECT_EQ(acc.usage()->input_tokens,  18);
    EXPECT_EQ(acc.usage()->output_tokens, 11);
    EXPECT_EQ(acc.usage()->cache_read_tokens, 0);

    ASSERT_TRUE(acc.modelId().has_value());
    // OpenAI returns the dated model id in the chunk, so we get the full
    // version suffix back.
    EXPECT_NE(acc.modelId()->find("gpt-5.4-mini"), std::string::npos);
}

TEST_F(FixtureTest, DeepSeekFixtureYieldsExpectedUsage) {
    const auto bytes = readBytes(dir_ / "deepseek_stream.sse");
    ASSERT_FALSE(bytes.empty());

    SseFramer         fr;
    UsageAccumulator  acc(ProviderFamily::OpenAICompat);
    fr.feed(bytes, [&](const SseEvent& e) { acc.onEvent(e); });
    fr.finish([&](const SseEvent& e) { acc.onEvent(e); });

    ASSERT_TRUE(acc.usage().has_value());
    EXPECT_EQ(acc.usage()->input_tokens,  16);
    EXPECT_EQ(acc.usage()->output_tokens,  3);

    ASSERT_TRUE(acc.modelId().has_value());
    EXPECT_EQ(*acc.modelId(), "deepseek-chat");
}

// Final usage must "win" over earlier nulls — important for DeepSeek
// since every intermediate chunk has usage:null and only the last is
// populated. OpenAI similar — usage is null until the final choices=[]
// chunk.
TEST(UsageAccumulator, LastNonNullUsageWins_OpenAICompat) {
    UsageAccumulator acc(ProviderFamily::OpenAICompat);
    acc.onEvent({"", R"({"id":"x","model":"gpt","choices":[{}],"usage":null})"});
    acc.onEvent({"", R"({"id":"x","model":"gpt","choices":[{}],"usage":null})"});
    acc.onEvent({"", R"({"id":"x","model":"gpt","choices":[],
                          "usage":{"prompt_tokens":7,"completion_tokens":3,"total_tokens":10}})"});
    ASSERT_TRUE(acc.usage().has_value());
    EXPECT_EQ(acc.usage()->input_tokens,  7);
    EXPECT_EQ(acc.usage()->output_tokens, 3);
}

TEST(UsageAccumulator, MalformedJsonIgnored) {
    UsageAccumulator acc(ProviderFamily::OpenAICompat);
    acc.onEvent({"", "not-json"});      // must not throw
    acc.onEvent({"", "[\"array\"]"});   // valid JSON but wrong shape
    EXPECT_FALSE(acc.usage().has_value());
}
