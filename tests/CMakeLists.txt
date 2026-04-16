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
