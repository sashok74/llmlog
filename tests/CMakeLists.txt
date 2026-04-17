include(GoogleTest)

# Shared test-support: per-test scoped temp DB, schema bootstrap.
add_library(llmlog_test_support STATIC
    test_base.cpp
)

target_include_directories(llmlog_test_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(llmlog_test_support
    PUBLIC
        llmlog::core
        fbpp::fbpp_core
        fbpp::fbpp_test_support
        GTest::gtest
)

# Copy test config next to test binaries so the standard connection-helper
# search paths resolve.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/data/test_config.json"
    "${CMAKE_BINARY_DIR}/config/test_config.json"
    COPYONLY
)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
function(llmlog_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        llmlog::core
        llmlog_test_support
        GTest::gtest
        GTest::gtest_main
    )
    gtest_discover_tests(${name})
endfunction()

llmlog_add_test(test_schema          unit/test_schema.cpp)
llmlog_add_test(test_isql_splitter   unit/test_isql_splitter.cpp)
llmlog_add_test(test_config          unit/test_config.cpp)
llmlog_add_test(test_sse_parser      unit/test_sse_parser.cpp)
llmlog_add_test(test_pricing         unit/test_pricing.cpp)

# SSE-parser fixture tests need to locate tests/fixtures/*.sse at runtime.
# Inject the source-tree path so the test binary can find them regardless
# of the cwd chosen by ctest.
target_compile_definitions(test_sse_parser PRIVATE
    LLMLOG_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
