// Unit + integration tests for the pricing seed loader.

#include <gtest/gtest.h>

#include "../test_base.hpp"

#include "llmlog/core/pricing.hpp"
#include "llmlog/core/pricing_seed.hpp"

#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

namespace fs = std::filesystem;
using llmlog::core::loadPricingSeedFile;
using llmlog::core::parsePricingSeed;
using llmlog::core::PricingDao;
using llmlog::core::PricingSeed;
using llmlog::core::applyPricingSeed;

// ===========================================================================
//  Parse-only unit tests (no DB)
// ===========================================================================
TEST(PricingSeedParse, MinimalSeed) {
    constexpr const char* kJson = R"JSON({
      "providers": [
        {"name":"anthropic","base_url":"https://api.anthropic.com","kind":"anthropic"}
      ],
      "prices": [
        {"provider":"anthropic","model":"claude-opus-4-7",
         "input_per_mtok":"15.00","output_per_mtok":"75.00",
         "effective_from":"2026-01-01 00:00:00 UTC"}
      ]
    })JSON";
    auto seed = parsePricingSeed(kJson);
    ASSERT_EQ(seed.providers.size(), 1u);
    EXPECT_EQ(seed.providers[0].name, "anthropic");
    ASSERT_EQ(seed.prices.size(), 1u);
    EXPECT_EQ(seed.prices[0].model_id, "claude-opus-4-7");
    EXPECT_EQ(seed.prices[0].input_per_mtok,  "15.00");
    EXPECT_EQ(seed.prices[0].output_per_mtok, "75.00");
    // Optional fields default to empty.
    EXPECT_EQ(seed.prices[0].cache_read_per_mtok,  "");
    EXPECT_EQ(seed.prices[0].cache_write_per_mtok, "");
    EXPECT_EQ(seed.prices[0].note, "");
}

TEST(PricingSeedParse, OptionalFieldsPropagate) {
    constexpr const char* kJson = R"JSON({
      "providers":[],
      "prices":[
        {"provider":"a","model":"m",
         "input_per_mtok":"1","output_per_mtok":"2",
         "cache_read_per_mtok":"0.1","cache_write_per_mtok":"0.5",
         "note":"hi",
         "effective_from":"2026-01-01 00:00:00 UTC"}
      ]
    })JSON";
    auto seed = parsePricingSeed(kJson);
    ASSERT_EQ(seed.prices.size(), 1u);
    EXPECT_EQ(seed.prices[0].cache_read_per_mtok,  "0.1");
    EXPECT_EQ(seed.prices[0].cache_write_per_mtok, "0.5");
    EXPECT_EQ(seed.prices[0].note, "hi");
}

TEST(PricingSeedParse, NullOptionalsBecomeEmpty) {
    constexpr const char* kJson = R"JSON({
      "providers":[],
      "prices":[
        {"provider":"a","model":"m",
         "input_per_mtok":"1","output_per_mtok":"2",
         "cache_read_per_mtok":null,
         "effective_from":"2026-01-01 00:00:00 UTC"}
      ]
    })JSON";
    auto seed = parsePricingSeed(kJson);
    EXPECT_EQ(seed.prices[0].cache_read_per_mtok, "");
}

TEST(PricingSeedParse, MissingProviderArrayIsOk) {
    constexpr const char* kJson = R"JSON({"prices":[]})JSON";
    auto seed = parsePricingSeed(kJson);
    EXPECT_TRUE(seed.providers.empty());
    EXPECT_TRUE(seed.prices.empty());
}

TEST(PricingSeedParse, MalformedJsonThrows) {
    EXPECT_THROW(parsePricingSeed("not json"), std::runtime_error);
    EXPECT_THROW(parsePricingSeed("[]"),       std::runtime_error);
}

TEST(PricingSeedParse, MissingRequiredPriceFieldThrows) {
    constexpr const char* kJson = R"JSON({
      "providers":[], "prices":[{"provider":"a","model":"m"}]
    })JSON";
    EXPECT_THROW(parsePricingSeed(kJson), std::runtime_error);
}

// ===========================================================================
//  The bundled seed/pricing.json must be a valid seed.
// ===========================================================================
TEST(PricingSeedParse, BundledDefaultSeedParses) {
#ifdef LLMLOG_SEED_DIR
    fs::path p = fs::path{LLMLOG_SEED_DIR} / "pricing.json";
    if (!fs::exists(p)) GTEST_SKIP() << "seed not present at " << p;
    PricingSeed seed;
    ASSERT_NO_THROW(seed = loadPricingSeedFile(p));
    EXPECT_GE(seed.providers.size(), 3u);
    EXPECT_GE(seed.prices.size(),    5u);
#else
    GTEST_SKIP() << "LLMLOG_SEED_DIR not defined at compile time";
#endif
}

// ===========================================================================
//  End-to-end application against a real Firebird DB.
// ===========================================================================
class PricingSeedApplyTest : public llmlog::test::LlmlogTestBase {};

TEST_F(PricingSeedApplyTest, ApplySeedThenRereadViaProcedure) {
    constexpr const char* kJson = R"JSON({
      "providers": [
        {"name":"anthropic","base_url":"https://api.anthropic.com","kind":"anthropic"},
        {"name":"openai",   "base_url":"https://api.openai.com/v1","kind":"openai"}
      ],
      "prices": [
        {"provider":"anthropic","model":"claude-haiku-4-5","family":"claude-haiku",
         "input_per_mtok":"1.00","output_per_mtok":"5.00",
         "effective_from":"2026-01-01 00:00:00 UTC"},
        {"provider":"openai","model":"gpt-5.4-mini","family":"gpt-5",
         "input_per_mtok":"0.30","output_per_mtok":"1.20",
         "cache_read_per_mtok":"0.15",
         "effective_from":"2026-01-01 00:00:00 UTC"}
      ]
    })JSON";

    auto seed = parsePricingSeed(kJson);
    PricingDao dao(*connection_);
    const auto applied = applyPricingSeed(dao, seed);
    EXPECT_EQ(applied, 2u);

    // Re-apply = no duplicates.
    const auto applied2 = applyPricingSeed(dao, seed);
    EXPECT_EQ(applied2, 2u);

    // Verify the PROVIDERS and PRICING counts.
    auto tra = connection_->StartTransaction();
    {
        auto st = connection_->prepareStatement("SELECT COUNT(*) FROM PROVIDERS");
        auto rs = tra->openCursor(st);
        std::tuple<std::int64_t> row;
        ASSERT_TRUE(rs->fetch(row));
        EXPECT_EQ(std::get<0>(row), 2);
    }
    {
        auto st = connection_->prepareStatement("SELECT COUNT(*) FROM PRICING");
        auto rs = tra->openCursor(st);
        std::tuple<std::int64_t> row;
        ASSERT_TRUE(rs->fetch(row));
        EXPECT_EQ(std::get<0>(row), 2);
    }
    tra->Commit();
}
