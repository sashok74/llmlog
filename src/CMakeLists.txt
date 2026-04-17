# llmlog_core — Phase 1: schema bootstrap + SQL splitter.
# Pricing / request-log APIs land in Phase 2.

# ---------------------------------------------------------------------------
# Embed schema / procedure SQL into the binary at configure time.
# Any edit to sql/*.sql triggers re-configure.
# ---------------------------------------------------------------------------
set(LLMLOG_SQL_DIR "${CMAKE_SOURCE_DIR}/sql")
file(READ "${LLMLOG_SQL_DIR}/exceptions.sql" LLMLOG_SQL_EXCEPTIONS)
file(READ "${LLMLOG_SQL_DIR}/schema.sql"     LLMLOG_SQL_SCHEMA)
file(READ "${LLMLOG_SQL_DIR}/procedures.sql" LLMLOG_SQL_PROCEDURES)

set(LLMLOG_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/llmlog")
file(MAKE_DIRECTORY "${LLMLOG_GENERATED_DIR}/core")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/embedded_sql.cpp.in"
    "${LLMLOG_GENERATED_DIR}/core/embedded_sql.cpp"
    @ONLY
)

# Track the SQL files as dependencies so edits trigger rebuilds.
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${LLMLOG_SQL_DIR}/exceptions.sql"
    "${LLMLOG_SQL_DIR}/schema.sql"
    "${LLMLOG_SQL_DIR}/procedures.sql"
)

# ---------------------------------------------------------------------------
# llmlog_core target
# ---------------------------------------------------------------------------
add_library(llmlog_core STATIC
    core/isql_splitter.cpp
    core/database.cpp
    core/config.cpp
    core/sse_parser.cpp
    "${LLMLOG_GENERATED_DIR}/core/embedded_sql.cpp"
)

target_include_directories(llmlog_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${LLMLOG_GENERATED_DIR}>
)

target_link_libraries(llmlog_core
    PUBLIC
        fbpp::fbpp_core
        nlohmann_json::nlohmann_json
)

target_compile_features(llmlog_core PUBLIC cxx_std_20)

add_library(llmlog::core ALIAS llmlog_core)
