#pragma once

// Bulk pricing + provider seed loader.
//
// Reads a seed JSON file (e.g. seed/pricing.json) that declares a set of
// providers and their models' historical price points, then applies the
// whole thing idempotently through PricingDao. Running the loader twice
// on the same DB updates any rows whose rate changed and leaves the rest
// alone — safe for init-time bootstrap and for admin "re-seed after editing
// the JSON" workflows.

#include <filesystem>
#include <string>
#include <vector>

#include "llmlog/core/pricing.hpp"

namespace llmlog::core {

struct PricingSeed {
    std::vector<ProviderMeta> providers;
    std::vector<Price>        prices;
};

/// Parse a seed JSON file. Validation mirrors config.cpp — missing or
/// wrong-typed fields throw std::runtime_error with an actionable message.
///
/// Expected schema (see seed/pricing.json):
/// {
///   "providers": [
///     {"name":"anthropic","base_url":"...","kind":"anthropic"}
///   ],
///   "prices": [
///     {
///       "provider":"anthropic",
///       "model":"claude-opus-4-7",
///       "family":"claude-opus",
///       "input_per_mtok":"15.00",
///       "output_per_mtok":"75.00",
///       "cache_read_per_mtok":"1.50",    // optional
///       "cache_write_per_mtok":"18.75",  // optional
///       "image_per_mtok":null,            // optional
///       "effective_from":"2025-05-22 00:00:00 UTC",
///       "note":"Tier 1"                  // optional
///     }
///   ]
/// }
PricingSeed loadPricingSeedFile(const std::filesystem::path& file);
PricingSeed parsePricingSeed(const std::string& jsonText);

/// Applies the seed to the DB: upsertProvider for each, upsertPrice for
/// each price row. Returns the number of price rows touched.
std::size_t applyPricingSeed(PricingDao& dao, const PricingSeed& seed);

} // namespace llmlog::core
