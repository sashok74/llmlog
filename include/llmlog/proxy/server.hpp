#pragma once

// HTTP reverse-proxy server.
//
// Binds a single HTTP listener (by default 127.0.0.1:7788) that dispatches
// incoming requests to configured upstream LLM providers by path prefix:
//
//   POST http://127.0.0.1:7788/anthropic/v1/messages
//     → POST https://api.anthropic.com/v1/messages  (api_key injected)
//
//   POST http://127.0.0.1:7788/openai/v1/chat/completions
//     → POST https://api.openai.com/v1/chat/completions
//
// Each response stream is piped back to the client while being parsed into
// a UsageInfo block; at stream end the proxy writes one REQUESTS row via
// RequestLogDao. Full behavior lands incrementally — this header is the
// stable surface for all Phase 3 commits.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace llmlog::core {
struct Config;
}

namespace llmlog::proxy {

/// Thin wrapper around cpp-httplib::Server. Owns the listener thread.
class Server {
public:
    /// Constructs but does NOT start. Caller must call start() then
    /// either wait() (blocking) or stop() from another thread.
    explicit Server(const llmlog::core::Config& config);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// Starts the listener in a background thread. Returns immediately.
    /// Throws std::runtime_error if the port is unavailable.
    void start();

    /// Blocks until stop() is called (or a fatal error occurs).
    void wait();

    /// Requests an orderly shutdown. Safe to call from any thread,
    /// including signal handlers.
    void stop();

    /// Port the server is actually listening on. Valid after start()
    /// has returned. When Config::bind.port is 0, the OS picks a free
    /// port and this returns that chosen number — useful for tests.
    std::uint16_t boundPort() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace llmlog::proxy
