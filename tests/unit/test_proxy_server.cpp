// Smoke tests for llmlog::proxy::Server. Doesn't hit real upstreams —
// just verifies the HTTP listener comes up, responds to /healthz, and
// returns 501 for anything that's not yet routed.

#include <gtest/gtest.h>

#include "llmlog/core/config.hpp"
#include "llmlog/proxy/server.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

llmlog::core::Config makeEphemeralConfig() {
    llmlog::core::Config cfg;
    cfg.bind.host = "127.0.0.1";
    cfg.bind.port = 0; // OS-chosen
    // DB section is required by loader/validation but unused by the proxy
    // smoke tests, so it's populated only to keep future full-config
    // construction paths happy.
    cfg.database.path     = "unused";
    cfg.database.user     = "SYSDBA";
    cfg.database.password = "unused";
    return cfg;
}

} // namespace

TEST(ProxyServer, StartsAndAnswersHealthz) {
    auto cfg = makeEphemeralConfig();
    llmlog::proxy::Server srv(cfg);
    ASSERT_NO_THROW(srv.start());

    // Give the listener thread a beat to actually accept.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto port = srv.boundPort();
    ASSERT_GT(port, 0) << "Server did not report a bound port";

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto res = cli.Get("/healthz");
    ASSERT_TRUE(res) << "healthz probe failed: "
                     << (res ? "" : to_string(res.error()));
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("ok"), std::string::npos);

    srv.stop();
}

TEST(ProxyServer, UnknownProviderReturns404) {
    auto cfg = makeEphemeralConfig();  // no providers configured
    llmlog::proxy::Server srv(cfg);
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    httplib::Client cli("127.0.0.1", srv.boundPort());
    cli.set_connection_timeout(2, 0);
    auto res = cli.Post("/anthropic/v1/messages",
                        R"({"model":"x"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    EXPECT_NE(res->body.find("unknown_provider"), std::string::npos);

    srv.stop();
}

TEST(ProxyServer, ConfiguredProviderButNoDbReturns503) {
    auto cfg = makeEphemeralConfig();
    llmlog::core::ProviderConfig pcfg;
    pcfg.base_url    = "https://example.invalid";
    pcfg.api_key     = "sk-fake";
    pcfg.auth_header = "x-api-key";
    pcfg.auth_scheme = "";
    pcfg.kind        = "anthropic";
    cfg.providers.emplace("anthropic", std::move(pcfg));

    llmlog::proxy::Server srv(cfg);   // dbConn = nullptr
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    httplib::Client cli("127.0.0.1", srv.boundPort());
    cli.set_connection_timeout(2, 0);
    auto res = cli.Post("/anthropic/v1/messages",
                        R"({"model":"x"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    EXPECT_NE(res->body.find("no_database_configured"), std::string::npos);

    srv.stop();
}

TEST(ProxyServer, StopIsIdempotent) {
    auto cfg = makeEphemeralConfig();
    llmlog::proxy::Server srv(cfg);
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    srv.stop();
    srv.stop(); // second call must not crash
    EXPECT_NO_THROW(srv.stop());
}
