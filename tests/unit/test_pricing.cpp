// Integration tests for llmlog::core::PricingDao.
// Hits a live Firebird 5 server (the one the test fixture provisions per
// test via fbpp::test::LlmlogTestBase).

#include <gtest/gtest.h>

#include "../test_base.hpp"

#include "llmlog/core/pricing.hpp"

#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>

#include <string>
#include <tuple>

using llmlog::core::PricingDao;
using llmlog::core::Price;
using llmlog::core::ProviderMeta;
using llmlog::test::LlmlogTestBase;

class PricingTest : public LlmlogTestBase {};

TEST_F(PricingTest, UpsertProviderInsertsThenUpdates) {
    PricingDao dao(*connection_);

    ProviderMeta anthropic{"anthropic", "https://api.anthropic.com", "anthropic"};
    const auto id1 = dao.upsertProvider(anthropic);
    EXPECT_GT(id1, 0);

    // Re-upsert with a changed base_url → same id, updated row.
    anthropic.base_url = "https://api-eu.anthropic.com";
    const auto id2 = dao.upsertProvider(anthropic);
    EXPECT_EQ(id2, id1);

    // A different provider gets a different id.
    const auto openaiId = dao.upsertProvider({"openai", "https://api.openai.com/v1", "openai"});
    EXPECT_NE(openaiId, id1);

    // Verify the UPDATE took effect.
    auto tra = connection_->StartTransaction();
    auto st  = connection_->prepareStatement("SELECT BASE_URL FROM PROVIDERS WHERE ID = ?");
    auto rs  = tra->openCursor(st, std::make_tuple(id1));
    std::tuple<std::string> row;
    ASSERT_TRUE(rs->fetch(row));
    EXPECT_EQ(std::get<0>(row), "https://api-eu.anthropic.com");
    tra->Commit();
}

TEST_F(PricingTest, UpsertModelAutoGeneratesIdAndIsIdempotent) {
    PricingDao dao(*connection_);
    dao.upsertProvider({"anthropic", "https://api.anthropic.com", "anthropic"});

    const auto id1 = dao.upsertModel("anthropic", "claude-opus-4-7", "claude-opus");
    const auto id2 = dao.upsertModel("anthropic", "claude-opus-4-7", "claude-opus");
    EXPECT_EQ(id1, id2);

    const auto id3 = dao.upsertModel("anthropic", "claude-haiku-4-5", "claude-haiku");
    EXPECT_NE(id3, id1);
}

TEST_F(PricingTest, ResolveModelIdCreatesRowsOnFirstCall) {
    PricingDao dao(*connection_);
    dao.upsertProvider({"openai", "https://api.openai.com/v1", "openai"});

    const auto id = dao.resolveModelId("openai", "gpt-5.4-mini");
    EXPECT_GT(id, 0);

    // Second call returns cached id (no DB round-trip; we can't observe that
    // directly, but idempotence is enough for this test).
    EXPECT_EQ(dao.resolveModelId("openai", "gpt-5.4-mini"), id);
}

TEST_F(PricingTest, UpsertPriceStoresDecfloatExactly) {
    PricingDao dao(*connection_);
    dao.upsertProvider({"anthropic", "https://api.anthropic.com", "anthropic"});

    Price p{
        .provider_name       = "anthropic",
        .model_id            = "claude-opus-4-7",
        .family              = "claude-opus",
        .input_per_mtok      = "15.00",
        .output_per_mtok     = "75.00",
        .cache_read_per_mtok = "1.50",
        .cache_write_per_mtok= "18.75",
        .image_per_mtok      = "",
        .effective_from      = "2026-01-01 00:00:00.0000 UTC",
        .note                = "Tier 1"
    };
    ASSERT_NO_THROW(dao.upsertPrice(p));

    // SP_GET_EFFECTIVE_PRICING must return exactly what we stored.
    auto tra = connection_->StartTransaction();
    auto st  = connection_->prepareStatement(
        "SELECT INPUT_PER_MTOK, OUTPUT_PER_MTOK, CACHE_READ_PER_MTOK, "
        "       CACHE_WRITE_PER_MTOK, IMAGE_PER_MTOK "
        "FROM SP_GET_EFFECTIVE_PRICING(?, CAST(? AS TIMESTAMP WITH TIME ZONE))");
    const auto modelId = dao.resolveModelId("anthropic", "claude-opus-4-7");
    auto rs = tra->openCursor(st, std::make_tuple(modelId,
                                                  std::string{"2026-03-01 12:00:00 UTC"}));

    // Read as strings so we can EXPECT_EQ the exact textual DECFLOAT.
    std::tuple<std::string, std::string, std::string, std::string, std::string> row;
    ASSERT_TRUE(rs->fetch(row));
    EXPECT_EQ(std::get<0>(row), "15.00");
    EXPECT_EQ(std::get<1>(row), "75.00");
    EXPECT_EQ(std::get<2>(row), "1.50");
    EXPECT_EQ(std::get<3>(row), "18.75");
    // IMAGE_PER_MTOK was NULL; fbpp string reader returns empty-string for NULL.
    EXPECT_EQ(std::get<4>(row), "");
    tra->Commit();
}

TEST_F(PricingTest, PriceTimeTravelLookup) {
    PricingDao dao(*connection_);
    dao.upsertProvider({"openai", "https://api.openai.com/v1", "openai"});

    dao.upsertPrice({"openai", "gpt-5.4", "gpt-5", "5.00",  "15.00", "", "", "",
                     "2026-01-01 00:00:00 UTC", ""});
    dao.upsertPrice({"openai", "gpt-5.4", "gpt-5", "3.00",  "10.00", "", "", "",
                     "2026-03-01 00:00:00 UTC", "Q1 price cut"});

    // Call before second-price → gets the first-price numbers.
    const auto modelId = dao.resolveModelId("openai", "gpt-5.4");
    auto tra = connection_->StartTransaction();
    auto st  = connection_->prepareStatement(
        "SELECT INPUT_PER_MTOK, OUTPUT_PER_MTOK "
        "FROM SP_GET_EFFECTIVE_PRICING(?, CAST(? AS TIMESTAMP WITH TIME ZONE))");
    auto rs = tra->openCursor(st, std::make_tuple(modelId,
                                                  std::string{"2026-02-15 00:00:00 UTC"}));
    std::tuple<std::string, std::string> row;
    ASSERT_TRUE(rs->fetch(row));
    EXPECT_EQ(std::get<0>(row), "5.00");
    EXPECT_EQ(std::get<1>(row), "15.00");
    tra->Commit();

    // Call after second-price → gets the second-price numbers.
    auto tra2 = connection_->StartTransaction();
    auto rs2  = tra2->openCursor(st, std::make_tuple(modelId,
                                                     std::string{"2026-04-15 00:00:00 UTC"}));
    ASSERT_TRUE(rs2->fetch(row));
    EXPECT_EQ(std::get<0>(row), "3.00");
    EXPECT_EQ(std::get<1>(row), "10.00");
    tra2->Commit();
}
