#include "llmlog/core/database.hpp"

#include "llmlog/core/detail/embedded_sql.hpp"
#include "llmlog/core/isql_splitter.hpp"

#include <fbpp/core/transaction.hpp>

namespace llmlog::core {

namespace {

void applyScript(fbpp::core::Connection& connection, const char* script) {
    // One transaction per script so a mid-script failure rolls back the
    // partial state cleanly. Each DDL statement is executed on its own
    // sub-transaction via Connection::Execute which commits on success.
    for (const auto& statement : splitIsqlScript(script)) {
        auto tra = connection.Execute(statement);
        tra->Commit();
    }
}

} // namespace

void bootstrapSchema(fbpp::core::Connection& connection) {
    applyScript(connection, detail::kSqlExceptions);
    applyScript(connection, detail::kSqlSchema);
    applyScript(connection, detail::kSqlProcedures);
}

} // namespace llmlog::core
