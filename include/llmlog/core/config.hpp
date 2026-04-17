#pragma once

// llmlog runtime configuration.
//
// Config comes from a single JSON file (typically /etc/llmlog/config.json on
// deployment, or $XDG_CONFIG_HOME/llmlog/config.json for a single-user dev
// setup). The loader performs:
//
//   1. Structural validation — missing required fields produce a
//      llmlog::core::ConfigError that names the offending JSON pointer path.
//   2. Source resolution — secret fields (api_key, database.password) accept
//      three forms:
//          "api_key":       "sk-literal"              — literal value
//          "api_key_env":   "ANTHROPIC_API_KEY"        — read std::getenv
//          "api_key_file":  "/etc/llmlog/secrets/x"    — read file contents
//      Exactly one of the three must be present for each provider; the
//      loader rejects ambiguity with a clear error. Literal values trigger a
//      warning on stderr (encourages moving secrets out of the file).
//   3. Post-resolution values never contain the `*_env` / `*_file` fields;
//      callers see only the fully-resolved plain string in api_key /
//      database.password.
//
// Unknown top-level keys are ignored (forward-compat); unknown keys inside a
// provider block produce a warning but do not fail the load.

#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace llmlog::core {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(std::string message, std::string jsonPath = {})
        : std::runtime_error(buildWhat(message, jsonPath))
        , path_(std::move(jsonPath)) {}

    /// JSON-pointer-like path to the offending field, e.g. "/providers/anthropic/api_key".
    /// Empty for whole-file / top-level errors.
    std::string_view jsonPath() const noexcept { return path_; }

private:
    static std::string buildWhat(const std::string& msg, const std::string& path) {
        if (path.empty()) return msg;
        return "at " + path + ": " + msg;
    }
    std::string path_;
};

struct DatabaseConfig {
    std::string server   = "localhost";
    std::uint16_t port   = 3050;
    std::string path;                        ///< Full fb path or alias
    std::string user;
    std::string password;                    ///< resolved
    std::string charset  = "UTF8";
};

struct ProviderConfig {
    std::string base_url;
    std::string api_key;                     ///< resolved
    std::string auth_header = "Authorization";
    std::string auth_scheme = "Bearer";      ///< "" for no scheme prefix
    /// Upstream wire-protocol family; controls which usage-extractor the
    /// proxy runs against the response stream.
    ///   "anthropic"       — named-event SSE, usage in message_delta
    ///   "openai"          — data-only SSE, final usage chunk
    ///   "openai_compat"   — same shape as openai (DeepSeek, Moonshot,
    ///                       Ollama, LM Studio, Qwen, Zhipu, ...)
    ///   "gemini"          — treated as openai_compat for now; proper
    ///                       Gemini streaming support is a later phase
    /// Defaults to "openai_compat" so plain OpenAI-compatible providers
    /// need no extra config.
    std::string kind = "openai_compat";
    std::map<std::string, std::string> extra_headers;
};

struct BindConfig {
    std::string host      = "127.0.0.1";
    std::uint16_t port    = 7788;
};

struct ProxyAuthConfig {
    std::string mode        = "bearer";      ///< "bearer" | "none"
    bool        required    = true;
    std::string initial_bootstrap_token;     ///< optional one-time token
};

struct LoggingConfig {
    std::string level   = "info";
    std::string file;                        ///< empty → stderr only
    int rotate_max_size_mb = 10;
    int rotate_max_files   = 5;
};

struct Config {
    BindConfig       bind;
    DatabaseConfig   database;
    ProxyAuthConfig  proxy_auth;
    /// Keyed by provider name ("anthropic", "openai", ...). Case-sensitive.
    std::unordered_map<std::string, ProviderConfig> providers;
    LoggingConfig    logging;
};

/// Load and validate a config file. Throws ConfigError on any schema or
/// resolution failure. The returned Config has all secret sources resolved.
Config loadConfigFile(const std::filesystem::path& file);

/// Parse+validate a JSON string (same semantics as loadConfigFile, used for
/// tests and programmatic config). `baseDir` is used to resolve relative
/// file-sources; pass std::filesystem::current_path() if unsure.
Config loadConfigFromString(std::string_view json,
                            const std::filesystem::path& baseDir);

} // namespace llmlog::core
