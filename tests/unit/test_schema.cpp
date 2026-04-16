// Verifies llmlog::core::bootstrapSchema produces the expected objects in a
// fresh Firebird 5 database. The fixture (LlmlogTestBase) already runs
// bootstrapSchema during SetUp; the tests just inspect RDB$ metadata.

#include <gtest/gtest.h>

#include "../test_base.hpp"

#include "llmlog/core/database.hpp"

#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

using llmlog::test::LlmlogTestBase;

namespace {

std::vector<std::string> listUserTables(fbpp::core::Connection& conn) {
    auto tra = conn.StartTransaction();
    auto stmt = conn.prepareStatement(
        "SELECT TRIM(RDB$RELATION_NAME) "
        "FROM RDB$RELATIONS "
        "WHERE RDB$SYSTEM_FLAG = 0 AND RDB$VIEW_SOURCE IS NULL "
        "ORDER BY RDB$RELATION_NAME");
    auto rs = tra->openCursor(stmt);
    std::vector<std::string> out;
    std::tuple<std::string> row;
    while (rs->fetch(row)) out.emplace_back(std::get<0>(row));
    tra->Commit();
    return out;
}

std::vector<std::string> listUserProcedures(fbpp::core::Connection& conn) {
    auto tra = conn.StartTransaction();
    auto stmt = conn.prepareStatement(
        "SELECT TRIM(RDB$PROCEDURE_NAME) "
        "FROM RDB$PROCEDURES "
        "WHERE RDB$SYSTEM_FLAG = 0 "
        "ORDER BY RDB$PROCEDURE_NAME");
    auto rs = tra->openCursor(stmt);
    std::vector<std::string> out;
    std::tuple<std::string> row;
    while (rs->fetch(row)) out.emplace_back(std::get<0>(row));
    tra->Commit();
    return out;
}

std::vector<std::string> listUserExceptions(fbpp::core::Connection& conn) {
    auto tra = conn.StartTransaction();
    auto stmt = conn.prepareStatement(
        "SELECT TRIM(RDB$EXCEPTION_NAME) "
        "FROM RDB$EXCEPTIONS "
        "WHERE RDB$SYSTEM_FLAG = 0 "
        "ORDER BY RDB$EXCEPTION_NAME");
    auto rs = tra->openCursor(stmt);
    std::vector<std::string> out;
    std::tuple<std::string> row;
    while (rs->fetch(row)) out.emplace_back(std::get<0>(row));
    tra->Commit();
    return out;
}

bool contains(const std::vector<std::string>& v, std::string_view needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

} // namespace

class SchemaBootstrapTest : public LlmlogTestBase {};

TEST_F(SchemaBootstrapTest, AllTablesPresent) {
    auto tables = listUserTables(*connection_);
    EXPECT_TRUE(contains(tables, "PROVIDERS"));
    EXPECT_TRUE(contains(tables, "MODELS"));
    EXPECT_TRUE(contains(tables, "PRICING"));
    EXPECT_TRUE(contains(tables, "REQUESTS"));
}

TEST_F(SchemaBootstrapTest, AllProceduresPresent) {
    auto procs = listUserProcedures(*connection_);
    EXPECT_TRUE(contains(procs, "SP_GET_EFFECTIVE_PRICING"));
    EXPECT_TRUE(contains(procs, "SP_LOG_REQUEST"));
    EXPECT_TRUE(contains(procs, "SP_REPORT_BY_MODEL"));
}

TEST_F(SchemaBootstrapTest, AllExceptionsPresent) {
    auto exc = listUserExceptions(*connection_);
    EXPECT_TRUE(contains(exc, "EX_PRICING_NOT_FOUND"));
}

// Note: re-running bootstrap on an already-initialized database is NOT a
// supported operation in Phase 1 — DDL in schema.sql uses plain CREATE, not
// CREATE IF NOT EXISTS, because Firebird 5.0.0 (the version under CI) does
// not accept IF NOT EXISTS on CREATE TABLE. Callers are expected to start
// from a freshly-created database (see setupFreshDatabase in test_base).
// Idempotent migrations will land in a later phase with a version table.
