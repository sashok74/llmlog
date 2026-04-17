// llmlog CLI entry point.
//
// Currently-wired subcommands:
//   llmlog version
//   llmlog proxy  --config /etc/llmlog/config.json [--port 7788]
//   llmlog seed apply --file seed/pricing.json --config /etc/llmlog/config.json
//
// More subcommands (report, provider add, import-env, env) land in Phase 3b.

#include "llmlog/core/config.hpp"
#include "llmlog/core/pricing.hpp"
#include "llmlog/core/pricing_seed.hpp"
#include "llmlog/core/database.hpp"
#include "llmlog/proxy/server.hpp"

#include <fbpp/core/connection.hpp>

#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr const char* kLlmlogVersion = "0.1.0";

// Single atomic pointer used by the signal handler to request shutdown.
// We only ever set it once per process lifetime.
std::atomic<llmlog::proxy::Server*> g_activeServer{nullptr};

extern "C" void handleSignal(int /*signo*/) {
    auto* s = g_activeServer.load(std::memory_order_acquire);
    if (s) s->stop();
}

fbpp::core::ConnectionParams toParams(const llmlog::core::DatabaseConfig& db) {
    fbpp::core::ConnectionParams p;
    const auto hostPart =
        db.server.empty() ? std::string{"localhost"} : db.server;
    const auto portPart =
        (db.port == 0 || db.port == 3050) ? std::string{} : "/" + std::to_string(db.port);
    p.database = hostPart + portPart + ":" + db.path;
    p.user     = db.user;
    p.password = db.password;
    p.charset  = db.charset;
    return p;
}

int runVersion() {
    std::cout << "llmlog " << kLlmlogVersion << '\n';
    return 0;
}

int runProxy(const std::string& configPath) {
    namespace fs = std::filesystem;
    auto cfg = llmlog::core::loadConfigFile(fs::path{configPath});

    // Open the DB; bootstrap schema is idempotent per the sql/schema.sql
    // current design (plain CREATE) — so we bootstrap only when the
    // caller explicitly opts in via --init later. Here we trust the
    // schema is already applied.
    fbpp::core::Connection conn(toParams(cfg.database));

    llmlog::proxy::Server server(cfg, &conn);
    g_activeServer.store(&server, std::memory_order_release);
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::cout << "llmlog proxy listening on " << cfg.bind.host << ":"
              << cfg.bind.port << " (config " << configPath << ")\n";
    server.start();
    std::cout << "  actual port: " << server.boundPort() << '\n';
    server.wait();
    std::cout << "llmlog proxy stopped.\n";
    g_activeServer.store(nullptr, std::memory_order_release);
    return 0;
}

int runSeedApply(const std::string& configPath, const std::string& seedPath) {
    namespace fs = std::filesystem;
    auto cfg = llmlog::core::loadConfigFile(fs::path{configPath});

    fbpp::core::Connection conn(toParams(cfg.database));
    llmlog::core::PricingDao dao(conn);
    const auto seed    = llmlog::core::loadPricingSeedFile(fs::path{seedPath});
    const auto applied = llmlog::core::applyPricingSeed(dao, seed);
    std::cout << "applied " << applied << " price rows across "
              << seed.providers.size() << " providers\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"llmlog — local-first LLM API usage & cost tracker"};
    app.set_version_flag("--version", kLlmlogVersion);
    app.require_subcommand(1);

    // llmlog version
    auto* cmdVersion = app.add_subcommand("version", "Print version and exit");

    // llmlog proxy --config path
    std::string proxyConfig = "/etc/llmlog/config.json";
    auto* cmdProxy = app.add_subcommand("proxy", "Run the HTTP proxy");
    cmdProxy->add_option("-c,--config", proxyConfig,
                         "Path to llmlog config.json")->capture_default_str();

    // llmlog seed apply --file path --config path
    std::string seedConfig = "/etc/llmlog/config.json";
    std::string seedFile   = "seed/pricing.json";
    auto* cmdSeed = app.add_subcommand("seed", "Pricing-seed operations");
    cmdSeed->require_subcommand(1);
    auto* cmdSeedApply = cmdSeed->add_subcommand(
        "apply", "Load a pricing seed JSON and upsert it into the DB");
    cmdSeedApply->add_option("-c,--config", seedConfig,
                             "Path to llmlog config.json")->capture_default_str();
    cmdSeedApply->add_option("-f,--file", seedFile,
                             "Seed JSON file")->capture_default_str();

    CLI11_PARSE(app, argc, argv);

    try {
        if (*cmdVersion)    return runVersion();
        if (*cmdProxy)      return runProxy(proxyConfig);
        if (*cmdSeedApply)  return runSeedApply(seedConfig, seedFile);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "error: unknown exception\n";
        return 1;
    }
    return 0;
}
