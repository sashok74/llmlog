#pragma once

// RequestLog data-access layer.
// One insert() call = one upstream API request recorded. The insert invokes
// SP_LOG_REQUEST which computes COST_USD exactly via DECFLOAT and returns
// both the auto-generated row id and the computed cost.

#include <cstdint>
#include <string>

#include <fbpp/core/connection.hpp>

namespace llmlog::core {

/// Input to RequestLogDao::insert(). Mirrors the REQUESTS table columns,
/// minus the auto-assigned ID and the DB-computed COST_USD. Empty-string
/// optional fields become SQL NULL.
struct RequestLogEntry {
    /// Firebird-parseable timestamp: "YYYY-MM-DD HH:MM:SS[.nnnn] [TZ]".
    std::string  call_time;

    /// MODELS.ID — obtain via PricingDao::resolveModelId.
    std::int32_t model_id = 0;

    /// Upstream request id (anthropic: msg_*, openai: chatcmpl-*). "" = NULL.
    std::string  request_id;

    std::int64_t input_tokens   = 0;
    std::int64_t output_tokens  = 0;
    std::int64_t cache_read     = 0;
    std::int64_t cache_write    = 0;
    std::int64_t image_tokens   = 0;

    std::int32_t latency_ms   = 0;
    std::int16_t http_status  = 200;

    /// User-supplied label for grouping reports (project/env/user). "" = NULL.
    std::string  tag;
};

/// What SP_LOG_REQUEST returns.
struct LoggedRequest {
    std::int64_t id        = 0;
    std::string  cost_usd;   ///< DECFLOAT as text; parse only if you need numeric
};

class RequestLogDao {
public:
    explicit RequestLogDao(fbpp::core::Connection& connection) noexcept
        : conn_(&connection) {}

    /// Inserts one request via SP_LOG_REQUEST. Throws
    /// fbpp::core::FirebirdException if the model has no pricing row
    /// effective at call_time (SP raises EX_PRICING_NOT_FOUND).
    LoggedRequest insert(const RequestLogEntry& entry);

private:
    fbpp::core::Connection* conn_;
};

} // namespace llmlog::core
