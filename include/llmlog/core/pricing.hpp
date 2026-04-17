#pragma once

// Pricing data-access layer.
//
// Three tables, three upsert operations, one in-memory cache on top.
// Prices live as DECFLOAT(34) in Firebird; C++ sends them as strings so the
// round-trip (JSON seed file → DB → C++ arithmetic) never drops digits to a
// double.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <fbpp/core/connection.hpp>

namespace llmlog::core {

/// Metadata for a PROVIDERS row.
struct ProviderMeta {
    std::string name;        ///< 'anthropic', 'openai', 'deepseek', ...
    std::string base_url;    ///< upstream base URL
    std::string kind;        ///< 'anthropic' | 'openai' | 'openai_compat' | 'gemini'
};

/// One pricing row. `effective_from` is an ISO-8601-ish timestamp string
/// that Firebird will parse (e.g. "2026-04-01 00:00:00.0000 UTC"). Rate
/// strings are plain decimals ("15.00", "0.000003"); empty string means
/// the optional column is NULL.
struct Price {
    std::string provider_name;
    std::string model_id;
    std::string family;                   ///< may be empty
    std::string input_per_mtok;           ///< required
    std::string output_per_mtok;          ///< required
    std::string cache_read_per_mtok;      ///< "" = NULL
    std::string cache_write_per_mtok;     ///< "" = NULL
    std::string image_per_mtok;           ///< "" = NULL
    std::string effective_from;           ///< required, parseable by FB
    std::string note;                     ///< "" = NULL
};

/// Thin wrapper over fbpp::Connection that implements the upsert/resolve
/// operations the proxy and CLI need. Not thread-safe — one DAO per
/// thread, connection-bound.
class PricingDao {
public:
    explicit PricingDao(fbpp::core::Connection& connection) noexcept
        : conn_(&connection) {}

    /// Returns the PROVIDERS.ID for @p meta.name, creating or updating the
    /// row as needed. Provider IDs are picked as MAX(ID)+1 on insert.
    std::int16_t upsertProvider(const ProviderMeta& meta);

    /// Returns MODELS.ID for (providerName, modelId), auto-inserting via
    /// MODELS.ID IDENTITY when missing. FAMILY is updated if it differs.
    std::int32_t upsertModel(std::string_view providerName,
                             std::string_view modelId,
                             std::string_view family);

    /// Upserts one PRICING row, keyed by (MODEL_ID, EFFECTIVE_FROM). Will
    /// upsert the parent PROVIDERS / MODELS rows if they're missing.
    void upsertPrice(const Price& price);

    /// Model-id lookup with in-memory cache. If the (provider, model)
    /// pair has never been seen, inserts new PROVIDERS / MODELS rows.
    /// Throws fbpp::core::FirebirdException on DB error.
    std::int32_t resolveModelId(std::string_view providerName,
                                std::string_view modelId);

    /// Drops the in-memory caches (for tests or long-running procs where
    /// the DB may have been mutated out-of-process).
    void clearCache() noexcept;

private:
    std::int16_t insertProviderRow(const ProviderMeta& meta);
    std::int16_t queryProviderId (std::string_view name);

    fbpp::core::Connection* conn_;
    std::unordered_map<std::string, std::int16_t> providerIds_;
    std::unordered_map<std::string, std::int32_t> modelIds_; ///< key = "provider|model"
};

} // namespace llmlog::core
