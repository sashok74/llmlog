#pragma once

#include <fbpp/core/connection.hpp>

namespace llmlog::core {

/// Applies the llmlog schema and stored procedures to an already-open
/// Firebird connection. The operation is idempotent: running it twice on the
/// same database is a no-op (it uses CREATE TABLE IF NOT EXISTS and
/// CREATE OR ALTER PROCEDURE).
///
/// Throws fbpp::core::FirebirdException on any DDL or PSQL failure.
void bootstrapSchema(fbpp::core::Connection& connection);

} // namespace llmlog::core
