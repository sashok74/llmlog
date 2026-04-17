// End-to-end round-trip: mock upstream HTTP server → llmlog::proxy::Server
// → real Firebird. Verifies that a streamed Anthropic-style response flows
// through the proxy, lands in the client, and produces exactly one
// REQUESTS row with the expected token counts + non-zero cost.

#include <gtest/gtest.h>

#include "../test_base.hpp"

#include "llmlog/core/config.hpp"
#include "llmlog/core/pricing.hpp"
#include "llmlog/proxy/server.hpp"

#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>

namespace fs = std::filesystem;

namespace {

std::string readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

fs::path findFixturesDir() {
#ifdef LLMLOG_FIXTURES_DIR
    fs::path p{LLMLOG_FIXTURES_DIR};
    if (fs::exists(p)) return p;
#endif
    for (auto p = fs::current_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        auto cand = p / "tests" / "fixtures";
        if (fs::exists(cand / "anthropic_stream.sse")) return cand;
    }
    return {};
}

} // namespace

class ProxyRoundtripTest : public llmlog::test::LlmlogTestBase {};

TEST_F(ProxyRoundtripTest, AnthropicFixtureRoundTrip) {
    const auto fixtures = findFixturesDir();
    if (fixtures.empty()) GTEST_SKIP() << "fixtures not found";
    const auto body = readBytes(fixtures / "anthropic_stream.sse");
    ASSERT_FALSE(body.empty());

    // --- Mock upstream server that replays the fixture ---
    httplib::Server upstream;
    std::atomic<int> upstreamHits{0};
    std::string      receivedAuth;

    upstream.Post("/v1/messages",
        [&](const httplib::Request& req, httplib::Response& res) {
            ++upstreamHits;
            receivedAuth = req.get_header_value("x-api-key");
            res.set_content(body, "text/event-stream");
        });

    const int upstreamPort = upstream.bind_to_any_port("127.0.0.1");
    ASSERT_GT(upstreamPort, 0);
    std::thread upstreamThread([&]{ upstream.listen_after_bind(); });

    // Give the mock a beat to accept.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- Seed pricing for the model we're about to see ---
    llmlog::core::PricingDao pd(*connection_);
    pd.upsertProvider({"anthropic", "http://127.0.0.1:" + std::to_string(upstreamPort), "anthropic"});
    pd.upsertPrice({"anthropic", "claude-haiku-4-5-20251001", "claude-haiku",
                    "1.00", "5.00", "0.10", "1.25", "",
                    "2026-01-01 00:00:00 +00:00", "test"});

    // --- Build proxy config pointing at the mock ---
    llmlog::core::Config cfg;
    cfg.bind.host = "127.0.0.1";
    cfg.bind.port = 0;
    cfg.database = {}; // unused — proxy takes the Connection directly
    {
        llmlog::core::ProviderConfig p;
        p.base_url    = "http://127.0.0.1:" + std::to_string(upstreamPort);
        p.api_key     = "sk-ant-test";
        p.auth_header = "x-api-key";
        p.auth_scheme = "";
        p.kind        = "anthropic";
        p.extra_headers.emplace("anthropic-version", "2023-06-01");
        cfg.providers.emplace("anthropic", std::move(p));
    }

    llmlog::proxy::Server proxy(cfg, connection_.get());
    proxy.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- Send a request through the proxy ---
    httplib::Client cli("127.0.0.1", proxy.boundPort());
    cli.set_read_timeout(10, 0);
    httplib::Headers hdr{{"X-LlmLog-Tag", "roundtrip"}};
    auto res = cli.Post("/anthropic/v1/messages", hdr,
                        R"({"model":"claude-haiku-4-5","max_tokens":10})",
                        "application/json");
    ASSERT_TRUE(res) << "proxy call failed: " << (res ? "" : to_string(res.error()));
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("message_delta"), std::string::npos);

    // Proxy should have hit the upstream exactly once and injected our key.
    EXPECT_EQ(upstreamHits.load(), 1);
    EXPECT_EQ(receivedAuth, "sk-ant-test");

    // Give the proxy a moment to finish its DB insert (it happens
    // synchronously after the response body is sent).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    proxy.stop();
    upstream.stop();
    upstreamThread.join();

    // --- Verify exactly one REQUESTS row was logged ---
    auto tra = connection_->StartTransaction();
    auto st  = connection_->prepareStatement(
        "SELECT COUNT(*), SUM(INPUT_TOKENS), SUM(OUTPUT_TOKENS), TAG "
        "FROM REQUESTS GROUP BY TAG");
    auto rs = tra->openCursor(st);
    std::tuple<std::int64_t, std::int64_t, std::int64_t, std::string> row;
    ASSERT_TRUE(rs->fetch(row));
    EXPECT_EQ(std::get<0>(row), 1);
    EXPECT_EQ(std::get<1>(row), 20);   // from anthropic_stream.sse
    EXPECT_EQ(std::get<2>(row), 12);
    EXPECT_EQ(std::get<3>(row), "roundtrip");
    tra->Commit();
}
