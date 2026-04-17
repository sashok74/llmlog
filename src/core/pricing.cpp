#include "llmlog/core/pricing.hpp"

#include <fbpp/core/connection.hpp>
#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>
#include <fbpp/core/exception.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace llmlog::core {

namespace {

std::string makeKey(std::string_view a, std::string_view b) {
    std::string s;
    s.reserve(a.size() + 1 + b.size());
    s.append(a); s.push_back('|'); s.append(b);
    return s;
}

/// DECFLOAT parameters reach Firebird as strings via CAST(? AS DECFLOAT(34)).
/// Empty-string input is mapped to NULL via a SQL CASE. We never pass raw
/// doubles.
const char* kUpsertPriceSql = R"SQL(
UPDATE OR INSERT INTO PRICING
    (MODEL_ID, EFFECTIVE_FROM,
     INPUT_PER_MTOK, OUTPUT_PER_MTOK,
     CACHE_READ_PER_MTOK, CACHE_WRITE_PER_MTOK, IMAGE_PER_MTOK, NOTE)
VALUES (
    ?,
    CAST(? AS TIMESTAMP WITH TIME ZONE),
    CAST(? AS DECFLOAT(34)),
    CAST(? AS DECFLOAT(34)),
    CASE WHEN ? = '' THEN NULL ELSE CAST(? AS DECFLOAT(34)) END,
    CASE WHEN ? = '' THEN NULL ELSE CAST(? AS DECFLOAT(34)) END,
    CASE WHEN ? = '' THEN NULL ELSE CAST(? AS DECFLOAT(34)) END,
    CASE WHEN ? = '' THEN NULL ELSE ? END
)
MATCHING (MODEL_ID, EFFECTIVE_FROM)
)SQL";

} // namespace

// ---------------------------------------------------------------------------
// Providers: manual SMALLINT ID, upsert by UNIQUE(NAME).
// We do NOT short-circuit on cache here — the caller may have updated
// base_url / kind and expects that to land. The cache is for other
// methods (upsertModel / resolveModelId) to skip the NAME→ID lookup.
// ---------------------------------------------------------------------------
std::int16_t PricingDao::upsertProvider(const ProviderMeta& meta) {
    // Try to find an existing row. If present, UPDATE and cache its ID.
    auto tra  = conn_->StartTransaction();
    auto find = conn_->prepareStatement(
        "SELECT ID FROM PROVIDERS WHERE NAME = ?");
    auto rs   = tra->openCursor(find, std::make_tuple(meta.name));
    std::tuple<std::int16_t> row;
    if (rs->fetch(row)) {
        const auto id = std::get<0>(row);
        tra->Commit();

        auto tra2 = conn_->StartTransaction();
        auto upd = conn_->prepareStatement(
            "UPDATE PROVIDERS SET BASE_URL = ?, KIND = ? WHERE ID = ?");
        tra2->execute(upd, std::make_tuple(meta.base_url, meta.kind, id));
        tra2->Commit();

        providerIds_[meta.name] = id;
        return id;
    }
    tra->Commit();

    // Not found — pick the next id and insert.
    auto tra2     = conn_->StartTransaction();
    auto selMaxId = conn_->prepareStatement(
        "SELECT COALESCE(MAX(ID), 0) + 1 FROM PROVIDERS");
    auto rs2      = tra2->openCursor(selMaxId);
    std::tuple<std::int16_t> idRow;
    rs2->fetch(idRow);
    const auto newId = std::get<0>(idRow);

    auto ins = conn_->prepareStatement(
        "INSERT INTO PROVIDERS (ID, NAME, BASE_URL, KIND) VALUES (?, ?, ?, ?)");
    tra2->execute(ins, std::make_tuple(newId, meta.name, meta.base_url, meta.kind));
    tra2->Commit();

    providerIds_.emplace(meta.name, newId);
    return newId;
}

// ---------------------------------------------------------------------------
// Models: IDENTITY on ID; upsert by UNIQUE(PROVIDER_ID, MODEL_ID).
// ---------------------------------------------------------------------------
std::int32_t PricingDao::upsertModel(std::string_view providerName,
                                     std::string_view modelId,
                                     std::string_view family) {
    const auto cacheKey = makeKey(providerName, modelId);
    if (auto it = modelIds_.find(cacheKey); it != modelIds_.end()) {
        return it->second;
    }

    // Need a provider_id first. If we don't know it, bail — caller should
    // have created the provider via upsertProvider(). resolveModelId does
    // that for us.
    auto pit = providerIds_.find(std::string(providerName));
    if (pit == providerIds_.end()) {
        // Fallback: try to look it up.
        auto tra = conn_->StartTransaction();
        auto st  = conn_->prepareStatement("SELECT ID FROM PROVIDERS WHERE NAME = ?");
        auto rs  = tra->openCursor(st, std::make_tuple(providerName));
        std::tuple<std::int16_t> row;
        if (!rs->fetch(row)) {
            tra->Commit();
            throw std::runtime_error("unknown provider: " + std::string(providerName));
        }
        tra->Commit();
        providerIds_.emplace(std::string(providerName), std::get<0>(row));
        pit = providerIds_.find(std::string(providerName));
    }
    const auto providerId = pit->second;

    // Two-step approach because UPDATE OR INSERT ... RETURNING returns an
    // EMPTY result set on the UPDATE branch in Firebird 5 (RETURNING fires
    // only when INSERT happens). We therefore look up first and insert
    // only when missing.
    auto tra  = conn_->StartTransaction();
    auto find = conn_->prepareStatement(
        "SELECT ID FROM MODELS WHERE PROVIDER_ID = ? AND MODEL_ID = ?");
    auto rsF  = tra->openCursor(find, std::make_tuple(providerId, std::string(modelId)));
    std::tuple<std::int32_t> rowF;
    if (rsF->fetch(rowF)) {
        const auto existingId = std::get<0>(rowF);
        tra->Commit();

        // Update FAMILY if changed (only when caller supplied a value).
        if (!family.empty()) {
            auto tra2 = conn_->StartTransaction();
            auto upd  = conn_->prepareStatement(
                "UPDATE MODELS SET FAMILY = ? WHERE ID = ?");
            tra2->execute(upd, std::make_tuple(std::string(family), existingId));
            tra2->Commit();
        }
        modelIds_[cacheKey] = existingId;
        return existingId;
    }
    tra->Commit();

    // Not found — INSERT and capture IDENTITY via RETURNING.
    auto tra2 = conn_->StartTransaction();
    auto ins  = conn_->prepareStatement(
        "INSERT INTO MODELS (PROVIDER_ID, MODEL_ID, FAMILY) "
        "VALUES (?, ?, ?) RETURNING ID");
    auto rsI = tra2->openCursor(ins, std::make_tuple(
        providerId, std::string(modelId), std::string(family)));
    std::tuple<std::int32_t> idRow;
    if (!rsI->fetch(idRow)) {
        tra2->Rollback();
        throw std::runtime_error("INSERT RETURNING ID did not produce a row");
    }
    tra2->Commit();

    const auto id = std::get<0>(idRow);
    modelIds_.emplace(cacheKey, id);
    return id;
}

// ---------------------------------------------------------------------------
// Pricing rows
// ---------------------------------------------------------------------------
void PricingDao::upsertPrice(const Price& price) {
    // Make sure parents exist.
    // Provider metadata we don't have here — require caller to have added
    // the provider already. We still create MODELS on demand.
    const auto modelId = upsertModel(price.provider_name, price.model_id, price.family);

    auto tra = conn_->StartTransaction();
    auto st  = conn_->prepareStatement(kUpsertPriceSql);
    // The CASE-based NULL mapping duplicates each optional field in the
    // bind tuple: once for the comparison, once as the CAST source.
    tra->execute(st, std::make_tuple(
        modelId,
        price.effective_from,
        price.input_per_mtok,
        price.output_per_mtok,
        price.cache_read_per_mtok,  price.cache_read_per_mtok,
        price.cache_write_per_mtok, price.cache_write_per_mtok,
        price.image_per_mtok,       price.image_per_mtok,
        price.note,                 price.note
    ));
    tra->Commit();
}

// ---------------------------------------------------------------------------
// Resolver: the hot path for the proxy.
// ---------------------------------------------------------------------------
std::int32_t PricingDao::resolveModelId(std::string_view providerName,
                                        std::string_view modelId) {
    if (auto it = modelIds_.find(makeKey(providerName, modelId)); it != modelIds_.end()) {
        return it->second;
    }
    // Fallback: upsertModel handles missing PROVIDER / MODEL rows and
    // populates the in-memory cache. FAMILY unknown at this point.
    return upsertModel(providerName, modelId, /*family=*/ "");
}

void PricingDao::clearCache() noexcept {
    providerIds_.clear();
    modelIds_.clear();
}

} // namespace llmlog::core
