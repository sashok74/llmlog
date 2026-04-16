#pragma once

#include <gtest/gtest.h>

#include <fbpp/core/connection.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace llmlog::test {

using ConnectionParams = fbpp::core::ConnectionParams;

/// Returns ConnectionParams for a scoped temp DB name derived from @p scope.
/// Base params come from fbpp::util::getConnectionParams("tests.temp_db").
ConnectionParams makeScopedParams(std::string_view scope);

/// Drops (if exists) and creates the database described by @p params, then
/// applies llmlog::core::bootstrapSchema on a fresh connection that is
/// returned to the caller.
std::unique_ptr<fbpp::core::Connection>
setupFreshDatabase(const ConnectionParams& params);

/// Best-effort drop; swallows any error.
void dropQuietly(const ConnectionParams& params) noexcept;

/// gtest fixture that provisions a per-test temp database with the llmlog
/// schema already applied. `connection_` is ready to use in the test body.
class LlmlogTestBase : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    ConnectionParams                        params_;
    std::unique_ptr<fbpp::core::Connection> connection_;
};

} // namespace llmlog::test
