#pragma once

// Gateway that forwards one HTTP request to an upstream LLM provider,
// captures the usage block from the response stream, and logs a
// REQUESTS row via RequestLogDao on completion.
//
// Designed so each upstream call is a self-contained unit of work —
// no shared state between concurrent requests beyond the Config and
// the PricingDao / RequestLogDao caches (protected by a mutex at the
// Server level).

#include "llmlog/core/config.hpp"
#include "llmlog/core/pricing.hpp"
#include "llmlog/core/request_log.hpp"
#include "llmlog/core/sse_parser.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace httplib {
struct Request;
struct Response;
}

namespace llmlog::proxy {

/// Runtime adapter derived from a ProviderConfig — has base_url split
/// into host and path, plus the resolved auth pieces and the SSE
/// provider-family used to pick the usage extractor.
struct UpstreamAdapter {
    std::string                          name;        ///< e.g. "anthropic"
    std::string                          scheme_host; ///< e.g. "https://api.anthropic.com"
    std::string                          base_path;   ///< "" or e.g. "/v1"
    std::string                          api_key;
    std::string                          auth_header;
    std::string                          auth_scheme;
    std::map<std::string, std::string>   extra_headers;
    core::ProviderFamily                 family = core::ProviderFamily::OpenAICompat;
};

/// Builds an UpstreamAdapter from the corresponding ProviderConfig, also
/// mapping the config kind string to ProviderFamily.
///   "anthropic"                        → ProviderFamily::Anthropic
///   "openai" / "openai_compat" /
///   "gemini" / anything else           → ProviderFamily::OpenAICompat
UpstreamAdapter buildAdapter(std::string_view name,
                             const core::ProviderConfig& cfg,
                             const std::string& kind);

/// Forwards one request to @p adapter's upstream and (a) writes the
/// response back into @p res with streaming passthrough, (b) captures
/// the usage block via UsageAccumulator, (c) calls
/// logger.insert() on stream end with the captured metrics.
///
/// dbMu serializes access to @p pricing / @p logger — a single mutex
/// for the whole DAO pair since the proxy holds one Firebird
/// connection across all threads.
///
/// @p upstreamPath is the path the client hit AFTER stripping the
/// "/{provider}" prefix — e.g. for "POST /anthropic/v1/messages" this
/// is "/v1/messages".
void forwardRequest(const UpstreamAdapter&     adapter,
                    std::string_view           upstreamPath,
                    const httplib::Request&    req,
                    httplib::Response&         res,
                    core::PricingDao&          pricing,
                    core::RequestLogDao&       logger,
                    std::mutex&                dbMu);

} // namespace llmlog::proxy
