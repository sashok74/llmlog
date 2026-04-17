#include "llmlog/proxy/upstream_gateway.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <fbpp/core/exception.hpp>
#include <fbpp/core/firebird_compat.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace llmlog::proxy {

namespace {

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

core::ProviderFamily mapFamily(std::string_view kind) {
    return (kind == "anthropic") ? core::ProviderFamily::Anthropic
                                  : core::ProviderFamily::OpenAICompat;
}

/// Splits "https://api.anthropic.com/v1" into {"https://api.anthropic.com",
/// "/v1"} — cpp-httplib's Client wants only scheme+host+port, with the
/// path reattached per-request.
std::pair<std::string, std::string> splitBaseUrl(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {std::string{url}, {}};
    const auto pathStart = url.find('/', scheme + 3);
    if (pathStart == std::string_view::npos) return {std::string{url}, {}};
    return {std::string{url.substr(0, pathStart)}, std::string{url.substr(pathStart)}};
}

/// Formats a system_clock time_point as "YYYY-MM-DD HH:MM:SS.nnnn +00:00"
/// — the format Firebird's CAST AS TIMESTAMP WITH TIME ZONE parses
/// cleanly (per the schema change in Phase 2).
std::string formatCallTimeUtc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto t    = system_clock::to_time_t(tp);
    const auto frac = duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;

    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02d %02d:%02d:%02d.%03lld +00:00",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(frac));
    return {buf, buf + (n > 0 ? static_cast<std::size_t>(n) : 0)};
}

/// Hop-by-hop headers that should never be forwarded to the upstream
/// (or back to the client). Also includes Authorization / x-api-key so
/// llmlog's own injected auth replaces whatever the client sent.
bool isHopByHopOrAuth(std::string_view name) {
    const auto lower = toLower(std::string{name});
    return lower == "host"               || lower == "connection"
        || lower == "content-length"     || lower == "transfer-encoding"
        || lower == "authorization"      || lower == "x-api-key"
        || lower == "proxy-authenticate" || lower == "proxy-authorization"
        || lower == "te"                 || lower == "trailers"
        || lower == "upgrade"
        || lower == "x-llmlog-tag";      // purely a llmlog-side annotation
}

} // namespace

UpstreamAdapter buildAdapter(std::string_view name,
                             const core::ProviderConfig& cfg,
                             const std::string& kind) {
    auto [scheme_host, base_path] = splitBaseUrl(cfg.base_url);
    UpstreamAdapter a;
    a.name          = std::string{name};
    a.scheme_host   = std::move(scheme_host);
    a.base_path     = std::move(base_path);
    a.api_key       = cfg.api_key;
    a.auth_header   = cfg.auth_header;
    a.auth_scheme   = cfg.auth_scheme;
    a.extra_headers = cfg.extra_headers;
    a.family        = mapFamily(kind);
    return a;
}

void forwardRequest(const UpstreamAdapter&       adapter,
                    std::string_view             upstreamSuffix,
                    const httplib::Request&      req,
                    httplib::Response&           res,
                    core::PricingDao&            pricing,
                    core::RequestLogDao&         logger,
                    std::mutex&                  dbMu) {
    // Build the upstream path: "/v1/messages" for plain POST.
    std::string upstreamPath;
    upstreamPath.reserve(adapter.base_path.size() + upstreamSuffix.size());
    upstreamPath.append(adapter.base_path);
    upstreamPath.append(upstreamSuffix);
    if (upstreamPath.empty() || upstreamPath.front() != '/') {
        upstreamPath.insert(upstreamPath.begin(), '/');
    }

    // Compose upstream headers.
    httplib::Headers headers;
    for (const auto& kv : req.headers) {
        if (isHopByHopOrAuth(kv.first)) continue;
        headers.emplace(kv.first, kv.second);
    }
    // Auth: e.g. "Authorization: Bearer sk-..." or "x-api-key: sk-ant-..."
    const std::string authValue =
        adapter.auth_scheme.empty() ? adapter.api_key
                                    : adapter.auth_scheme + " " + adapter.api_key;
    headers.emplace(adapter.auth_header, authValue);
    for (const auto& [k, v] : adapter.extra_headers) headers.emplace(k, v);

    const std::string contentType = req.get_header_value("Content-Type");

    // SSE framer + usage accumulator for the response body.
    core::SseFramer        framer;
    core::UsageAccumulator acc(adapter.family);
    auto feedEvent = [&acc](const core::SseEvent& ev) { acc.onEvent(ev); };

    std::string buffered;           ///< accumulated response body
    const auto  callTime = std::chrono::system_clock::now();
    const auto  start    = std::chrono::steady_clock::now();

    httplib::Client cli(adapter.scheme_host);
    cli.set_connection_timeout(10, 0);   // 10s to connect upstream
    cli.set_read_timeout(300, 0);        // up to 5 min for long responses
    cli.set_follow_location(false);

    // MVP buffers the full response body in cpp-httplib's Result; true
    // streaming passthrough is Phase 3.5 (requires a custom send/recv
    // pairing). The SSE framer parses the whole buffer once the
    // upstream call returns — equivalent correctness, one-shot parse.
    auto upstreamResult =
        cli.Post(upstreamPath.c_str(), headers, req.body, contentType);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (upstreamResult) {
        buffered = upstreamResult->body;
        framer.feed(buffered, feedEvent);
        framer.finish(feedEvent);
    }

    if (!upstreamResult) {
        res.status = 502;
        res.set_content(
            R"({"error":"upstream_error","provider":")" + adapter.name +
            R"(","detail":")" + httplib::to_string(upstreamResult.error()) + R"("})",
            "application/json");
        return;
    }

    // Mirror upstream status + content-type back to the client.
    res.status = upstreamResult->status;
    std::string respCt = upstreamResult->get_header_value("Content-Type");
    if (respCt.empty()) respCt = "application/json";
    res.set_content(buffered, respCt);

    // Log the request — but only if we got a usage block. An
    // authentication error or early timeout yields no usage;
    // skipping log avoids inserting spurious $0 rows.
    if (!acc.usage() || !acc.modelId()) return;

    core::RequestLogEntry entry{};
    entry.call_time     = formatCallTimeUtc(callTime);
    entry.request_id    = acc.requestId().value_or("");
    entry.input_tokens  = acc.usage()->input_tokens;
    entry.output_tokens = acc.usage()->output_tokens;
    entry.cache_read    = acc.usage()->cache_read_tokens;
    entry.cache_write   = acc.usage()->cache_write_tokens;
    entry.image_tokens  = acc.usage()->image_tokens;
    entry.latency_ms    = static_cast<std::int32_t>(elapsed);
    entry.http_status   = static_cast<std::int16_t>(upstreamResult->status);
    entry.tag           = req.get_header_value("X-LlmLog-Tag");

    std::lock_guard lk(dbMu);
    try {
        entry.model_id = pricing.resolveModelId(adapter.name, *acc.modelId());
        logger.insert(entry);
    } catch (const fbpp::core::FirebirdException& e) {
        std::fprintf(stderr,
            "[llmlog] request-log insert: FirebirdException provider=%s model=%s: %s\n",
            adapter.name.c_str(), acc.modelId()->c_str(), e.what());
    } catch (const Firebird::FbException& e) {
        // Raw OO-API exception — fbpp sometimes lets this escape
        // directly rather than wrapping it.
        std::fprintf(stderr,
            "[llmlog] request-log insert: Firebird::FbException provider=%s model=%s\n",
            adapter.name.c_str(), acc.modelId()->c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[llmlog] request-log insert: std::exception provider=%s model=%s: %s\n",
            adapter.name.c_str(), acc.modelId()->c_str(), e.what());
    } catch (...) {
        std::fprintf(stderr,
            "[llmlog] request-log insert: unknown exception provider=%s model=%s\n",
            adapter.name.c_str(), acc.modelId()->c_str());
    }
}

} // namespace llmlog::proxy
