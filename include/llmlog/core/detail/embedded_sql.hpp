#pragma once

// Compile-time embedded SQL source. The three char arrays are populated from
// sql/*.sql via CMake configure_file at build time; their contents include the
// original 'SET TERM ^' directives so llmlog::core::splitIsqlScript can route
// procedure bodies to Firebird as single statements.

namespace llmlog::core::detail {

extern const char kSqlExceptions[];
extern const char kSqlSchema[];
extern const char kSqlProcedures[];

} // namespace llmlog::core::detail
