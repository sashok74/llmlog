// Integration tests for RequestLogDao. Each test seeds a pricing row
// and then exercises SP_LOG_REQUEST end-to-end — including the exact
// DECFLOAT cost arithmetic that's the whole point of using Firebird
// instead of SQLite.

#include <gtest/gtest.h>

#include "../test_base.hpp"

#include "llmlog/core/pricing.hpp"
#include "llmlog/core/request_log.hpp"

#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>
#include <fbpp/core/exception.hpp>

#include <string>
#include <tuple>

using llmlog::core::PricingDao;
using llmlog::core::Price;
using llmlog::core::ProviderMeta;
using llmlog::core::RequestLogDao;
using llmlog::core::RequestLogEntry;
using llmlog::test::LlmlogTestBase;

class RequestLogTest : public LlmlogTestBase {
protected:
    void SetUp() override {
        LlmlogTestBase::SetUp();

        // Seed minimal pricing: 1 provider, 1 model with known rates.
        PricingDao pd(*connection_);
        pd.upsertProvider({"anthropic", "https://api.anthropic.com", "anthropic"});
        pd.upsertPrice({"anthropic", "claude-opus-4-7", "claude-opus",
                        /* in  */  "15.00",
                        /* out */  "75.00",
                        /* cr  */  "1.50",
                        /* cw  */  "18.75",
                        /* img */  "",
                        "2026-01-01 00:00:00 UTC",
                        "test"});
        modelId_ = pd.resolveModelId("anthropic", "claude-opus-4-7");
    }

    std::int32_t modelId_{};
};

TEST_F(RequestLogTest, InsertReturnsIdAndExactCost) {
    RequestLogDao dao(*connection_);

    // 1000 input tokens @ $15/M + 200 output @ $75/M
    //  = 0.015 + 0.015 = 0.030
    RequestLogEntry e{};
    e.call_time     = "2026-04-17 12:00:00 UTC";
    e.model_id      = modelId_;
    e.input_tokens  = 1000;
    e.output_tokens = 200;
    e.latency_ms    = 742;
    e.http_status   = 200;
    e.tag           = "unit-test";

    auto res = dao.insert(e);
    EXPECT_GT(res.id, 0);
    // DECFLOAT representation — exact with no trailing float noise.
    // 1000 * 15 / 1_000_000 = 0.015, 200 * 75 / 1_000_000 = 0.015, sum = 0.030
    EXPECT_EQ(res.cost_usd, "0.030");
}

TEST_F(RequestLogTest, InsertCacheReadDiscountApplies) {
    RequestLogDao dao(*connection_);

    // Pure cache read — 1000 tokens @ $1.50/M → 0.0015
    RequestLogEntry e{};
    e.call_time   = "2026-04-17 12:00:00 UTC";
    e.model_id    = modelId_;
    e.cache_read  = 1000;
    e.latency_ms  = 100;
    e.http_status = 200;

    auto res = dao.insert(e);
    EXPECT_EQ(res.cost_usd, "0.00150");
}

TEST_F(RequestLogTest, InsertRowsAreQueryable) {
    RequestLogDao dao(*connection_);

    for (int i = 1; i <= 3; ++i) {
        dao.insert(RequestLogEntry{
            .call_time    = "2026-04-17 12:00:00 UTC",
            .model_id     = modelId_,
            .input_tokens = 100 * i,
            .output_tokens= 20  * i,
            .latency_ms   = 500,
            .http_status  = 200,
            .tag          = "batch"
        });
    }

    // COUNT + SUM across the batch.
    auto tra = connection_->StartTransaction();
    auto st  = connection_->prepareStatement(
        "SELECT COUNT(*), SUM(INPUT_TOKENS), SUM(OUTPUT_TOKENS), SUM(COST_USD) "
        "FROM REQUESTS WHERE TAG = ?");
    auto rs  = tra->openCursor(st, std::make_tuple(std::string{"batch"}));
    std::tuple<std::int64_t, std::int64_t, std::int64_t, std::string> row;
    ASSERT_TRUE(rs->fetch(row));
    EXPECT_EQ(std::get<0>(row), 3);
    EXPECT_EQ(std::get<1>(row), 600);    // 100+200+300
    EXPECT_EQ(std::get<2>(row), 120);    // 20+40+60
    // cost: sum of (input*15 + output*75)/1M for each row.
    //   row1 = (100*15 + 20*75)/1M = (1500 + 1500)/1M = 0.003
    //   row2 = (200*15 + 40*75)/1M = (3000 + 3000)/1M = 0.006
    //   row3 = (300*15 + 60*75)/1M = (4500 + 4500)/1M = 0.009
    //   sum  = 0.018
    EXPECT_EQ(std::get<3>(row), "0.018");
    tra->Commit();
}

TEST_F(RequestLogTest, MissingPricingThrows) {
    // Model without any PRICING rows → SP raises EX_PRICING_NOT_FOUND.
    PricingDao pd(*connection_);
    pd.upsertProvider({"deepseek", "https://api.deepseek.com/v1", "openai_compat"});
    const auto id = pd.resolveModelId("deepseek", "deepseek-chat");

    RequestLogDao dao(*connection_);
    RequestLogEntry e{};
    e.call_time    = "2026-04-17 12:00:00 UTC";
    e.model_id     = id;
    e.input_tokens = 10;
    e.latency_ms   = 100;
    e.http_status  = 200;

    EXPECT_THROW(dao.insert(e), fbpp::core::FirebirdException);
}
