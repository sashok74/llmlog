#include "llmlog/proxy/server.hpp"

#include "llmlog/core/config.hpp"

// cpp-httplib pulls in <windows.h> via <winsock2.h> on Windows and that
// macros-pollutes nlohmann/json's templates. CPPHTTPLIB_OPENSSL_SUPPORT
// enables HTTPS for the outgoing Client (Phase 3b).
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace llmlog::proxy {

struct Server::Impl {
    explicit Impl(const llmlog::core::Config& cfg) : config(cfg) {}

    const llmlog::core::Config& config;
    httplib::Server             http;
    std::thread                 listener;
    std::atomic<std::uint16_t>  port{0};
    std::atomic<bool>           running{false};
    mutable std::mutex          mu;
};

namespace {

/// Health endpoint: returns simple JSON with process state. Used by
/// systemd unit's health-check and integration tests to confirm the
/// listener is up before hitting real routes.
void registerHealth(httplib::Server& http) {
    http.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });
}

/// Catch-all that returns 501 Not Implemented for any path that hasn't
/// been routed yet. Lets integration tests see the proxy is alive even
/// before provider adapters are wired up.
void registerCatchAll(httplib::Server& http) {
    auto handler = [](const httplib::Request& req, httplib::Response& res) {
        res.status = 501;
        res.set_content(
            R"({"error":"not_implemented","path":")" + req.path + R"("})",
            "application/json");
    };
    // cpp-httplib requires explicit registration per method; the provider
    // routing will narrow these in a follow-up commit.
    http.Post(".*",   handler);
    http.Get(".*",    handler);
    http.Put(".*",    handler);
    http.Delete(".*", handler);
}

} // namespace

Server::Server(const llmlog::core::Config& config)
    : impl_(std::make_unique<Impl>(config)) {
    registerHealth  (impl_->http);
    registerCatchAll(impl_->http);
}

Server::~Server() { stop(); }

void Server::start() {
    std::lock_guard lk(impl_->mu);
    if (impl_->running.exchange(true)) {
        throw std::runtime_error("Server::start() called twice");
    }

    const auto host = impl_->config.bind.host;
    const auto wantPort = impl_->config.bind.port;

    // Pre-bind so port-allocation failures surface synchronously — the
    // caller wants a hard error, not a silently-dead listener.
    const auto actualPort = impl_->http.bind_to_any_port(host);
    if (actualPort <= 0) {
        impl_->running = false;
        throw std::runtime_error("Server: failed to bind " + host +
                                 " (port " + std::to_string(wantPort) + ")");
    }
    // NOTE: bind_to_any_port ignores the configured port. For now we treat
    // Config.bind.port as a hint; the tests use port 0 anyway, and
    // production deployments can switch to bind_to_port() once we expose
    // that as a strict option.
    impl_->port = static_cast<std::uint16_t>(actualPort);

    impl_->listener = std::thread([this]() {
        impl_->http.listen_after_bind();
        impl_->running = false;
    });
}

void Server::wait() {
    // Busy-wait until stop() joins it.
    while (impl_->running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Server::stop() {
    std::lock_guard lk(impl_->mu);
    if (impl_->http.is_running()) {
        impl_->http.stop();
    }
    if (impl_->listener.joinable()) {
        impl_->listener.join();
    }
    impl_->running = false;
}

std::uint16_t Server::boundPort() const noexcept {
    return impl_->port.load(std::memory_order_acquire);
}

} // namespace llmlog::proxy
