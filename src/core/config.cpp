#include "llmlog/core/config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <utility>

namespace llmlog::core {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// JSON-pointer-style path tracker — used to produce actionable error messages
// that point right at the offending field.
// ---------------------------------------------------------------------------
class PathScope {
public:
    explicit PathScope(std::string* sink, std::string_view segment)
        : sink_(sink), addedLen_(segment.size() + 1) {
        sink_->push_back('/');
        sink_->append(segment);
    }
    ~PathScope() {
        sink_->resize(sink_->size() - addedLen_);
    }
    PathScope(const PathScope&) = delete;
    PathScope& operator=(const PathScope&) = delete;

private:
    std::string* sink_;
    std::size_t  addedLen_;
};

void requireField(const json& obj, std::string_view field, const std::string& path) {
    if (!obj.contains(field)) {
        throw ConfigError("missing required field '" + std::string(field) + "'", path);
    }
}

std::string readString(const json& obj, std::string_view field, const std::string& path) {
    requireField(obj, field, path);
    const auto& v = obj.at(field);
    if (!v.is_string()) {
        throw ConfigError("field '" + std::string(field) + "' must be a string",
                          path + "/" + std::string(field));
    }
    return v.get<std::string>();
}

std::string readStringOr(const json& obj, std::string_view field, std::string def) {
    if (!obj.contains(field) || obj.at(field).is_null()) return def;
    const auto& v = obj.at(field);
    if (!v.is_string()) {
        throw ConfigError("expected string", "/" + std::string(field));
    }
    return v.get<std::string>();
}

int readIntOr(const json& obj, std::string_view field, int def) {
    if (!obj.contains(field) || obj.at(field).is_null()) return def;
    const auto& v = obj.at(field);
    if (!v.is_number_integer()) {
        throw ConfigError("expected integer", "/" + std::string(field));
    }
    return v.get<int>();
}

bool readBoolOr(const json& obj, std::string_view field, bool def) {
    if (!obj.contains(field) || obj.at(field).is_null()) return def;
    const auto& v = obj.at(field);
    if (!v.is_boolean()) {
        throw ConfigError("expected boolean", "/" + std::string(field));
    }
    return v.get<bool>();
}

// ---------------------------------------------------------------------------
// Source resolution for secret fields.
// Returns the resolved secret and remembers which source variant was used.
// ---------------------------------------------------------------------------
struct ResolvedSecret {
    std::string value;
    std::string source; ///< "literal" | "env" | "file"
};

std::string readFileContents(const fs::path& file, const std::string& path) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        throw ConfigError("referenced file does not exist: " + file.string(), path);
    }
    std::ifstream f(file);
    if (!f) {
        throw ConfigError("cannot open referenced file: " + file.string(), path);
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string s = buf.str();
    // Strip trailing whitespace/newline — secret key files commonly have a
    // stray \n at end from `echo ... > file`.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
    if (s.empty()) {
        throw ConfigError("referenced file is empty after trimming: " + file.string(), path);
    }
    return s;
}

std::string readEnvVar(const std::string& name, const std::string& path) {
    const char* v = std::getenv(name.c_str());
    if (!v || *v == '\0') {
        throw ConfigError("environment variable '" + name + "' is not set or empty", path);
    }
    return std::string{v};
}

/// Resolves a trio of fields: <prefix>, <prefix>_env, <prefix>_file.
/// Exactly one must be present when `required` is true; returns empty
/// ResolvedSecret with source="" if none present and required=false.
ResolvedSecret resolveSecret(const json& obj,
                             std::string_view prefix,
                             const std::string& path,
                             const fs::path& baseDir,
                             bool required) {
    const std::string fld       = std::string{prefix};
    const std::string fldEnv    = fld + "_env";
    const std::string fldFile   = fld + "_file";

    const int presenceCount =
        (obj.contains(fld)     && !obj.at(fld).is_null()     ? 1 : 0) +
        (obj.contains(fldEnv)  && !obj.at(fldEnv).is_null()  ? 1 : 0) +
        (obj.contains(fldFile) && !obj.at(fldFile).is_null() ? 1 : 0);

    if (presenceCount == 0) {
        if (required) {
            throw ConfigError("missing '" + fld + "' (or '" + fldEnv +
                              "' / '" + fldFile + "')", path);
        }
        return {};
    }
    if (presenceCount > 1) {
        throw ConfigError(
            "exactly one of '" + fld + "', '" + fldEnv + "', '" + fldFile +
            "' may be set; found " + std::to_string(presenceCount), path);
    }

    if (obj.contains(fldFile) && !obj.at(fldFile).is_null()) {
        const auto ref = obj.at(fldFile).get<std::string>();
        fs::path p{ref};
        if (p.is_relative()) p = baseDir / p;
        return {readFileContents(p, path + "/" + fldFile), "file"};
    }
    if (obj.contains(fldEnv) && !obj.at(fldEnv).is_null()) {
        const auto name = obj.at(fldEnv).get<std::string>();
        return {readEnvVar(name, path + "/" + fldEnv), "env"};
    }
    // literal
    const auto v = obj.at(fld).get<std::string>();
    if (v.empty()) {
        throw ConfigError("literal '" + fld + "' must not be empty", path + "/" + fld);
    }
    return {v, "literal"};
}

// ---------------------------------------------------------------------------
// Section parsers
// ---------------------------------------------------------------------------
BindConfig parseBind(const json& obj) {
    BindConfig b;
    if (!obj.is_object()) return b;
    b.host = readStringOr(obj, "host", b.host);
    b.port = static_cast<std::uint16_t>(readIntOr(obj, "port", b.port));
    return b;
}

DatabaseConfig parseDatabase(const json& obj, const fs::path& baseDir) {
    if (!obj.is_object()) {
        throw ConfigError("'database' must be an object", "/database");
    }
    DatabaseConfig d;
    d.server  = readStringOr(obj, "server",  d.server);
    d.port    = static_cast<std::uint16_t>(readIntOr(obj, "port", d.port));
    d.path    = readString   (obj, "path", "/database");
    d.user    = readString   (obj, "user", "/database");
    d.charset = readStringOr(obj, "charset", d.charset);
    auto pw   = resolveSecret(obj, "password", "/database", baseDir, /*required=*/true);
    d.password = std::move(pw.value);
    return d;
}

ProxyAuthConfig parseProxyAuth(const json& obj) {
    ProxyAuthConfig p;
    if (!obj.is_object()) return p;
    p.mode                    = readStringOr(obj, "mode", p.mode);
    p.required                = readBoolOr  (obj, "required", p.required);
    p.initial_bootstrap_token = readStringOr(obj, "initial_bootstrap_token", "");
    return p;
}

LoggingConfig parseLogging(const json& obj) {
    LoggingConfig l;
    if (!obj.is_object()) return l;
    l.level               = readStringOr(obj, "level", l.level);
    l.file                = readStringOr(obj, "file",  "");
    l.rotate_max_size_mb  = readIntOr   (obj, "rotate_max_size_mb",  l.rotate_max_size_mb);
    l.rotate_max_files    = readIntOr   (obj, "rotate_max_files",    l.rotate_max_files);
    return l;
}

ProviderConfig parseProvider(const std::string& name,
                             const json& obj,
                             const fs::path& baseDir) {
    if (!obj.is_object()) {
        throw ConfigError("provider entry must be an object", "/providers/" + name);
    }
    ProviderConfig p;
    const std::string path = "/providers/" + name;
    p.base_url    = readString   (obj, "base_url", path);
    p.auth_header = readStringOr(obj, "auth_header", p.auth_header);
    p.auth_scheme = readStringOr(obj, "auth_scheme", p.auth_scheme);
    p.kind        = readStringOr(obj, "kind",        p.kind);

    auto key      = resolveSecret(obj, "api_key", path, baseDir, /*required=*/true);
    p.api_key     = std::move(key.value);

    if (obj.contains("extra_headers") && !obj.at("extra_headers").is_null()) {
        const auto& h = obj.at("extra_headers");
        if (!h.is_object()) {
            throw ConfigError("'extra_headers' must be an object",
                              path + "/extra_headers");
        }
        for (auto it = h.begin(); it != h.end(); ++it) {
            if (!it.value().is_string()) {
                throw ConfigError("extra header value must be a string",
                                  path + "/extra_headers/" + it.key());
            }
            p.extra_headers.emplace(it.key(), it.value().get<std::string>());
        }
    }
    return p;
}

std::unordered_map<std::string, ProviderConfig>
parseProviders(const json& obj, const fs::path& baseDir) {
    if (!obj.is_object()) {
        throw ConfigError("'providers' must be an object", "/providers");
    }
    std::unordered_map<std::string, ProviderConfig> out;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        // Skip comment-prefixed keys like "_comment" or templates "_anthropic_template".
        if (!it.key().empty() && it.key().front() == '_') continue;
        out.emplace(it.key(), parseProvider(it.key(), it.value(), baseDir));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
Config loadConfigFromString(std::string_view jsonText,
                            const fs::path& baseDir) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const json::parse_error& e) {
        throw ConfigError(std::string{"malformed JSON: "} + e.what());
    }
    if (!root.is_object()) {
        throw ConfigError("top-level JSON value must be an object");
    }

    Config cfg;
    if (root.contains("bind"))       cfg.bind       = parseBind      (root["bind"]);
    if (root.contains("database"))   cfg.database   = parseDatabase  (root["database"], baseDir);
    else throw ConfigError("missing required section 'database'");
    if (root.contains("proxy_auth")) cfg.proxy_auth = parseProxyAuth (root["proxy_auth"]);
    if (root.contains("providers"))  cfg.providers  = parseProviders (root["providers"], baseDir);
    if (root.contains("logging"))    cfg.logging    = parseLogging   (root["logging"]);
    return cfg;
}

Config loadConfigFile(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        throw ConfigError("config file not found: " + file.string());
    }
    std::ifstream f(file);
    if (!f) {
        throw ConfigError("cannot open config file: " + file.string());
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    const auto base = file.parent_path();
    return loadConfigFromString(buf.str(), base.empty() ? fs::current_path() : base);
}

} // namespace llmlog::core
