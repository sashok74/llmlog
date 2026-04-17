cmake_minimum_required(VERSION 3.20)
project(llmlog
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Compact local-first LLM API usage and cost tracker"
)

# -----------------------------------------------------------------------------
# Standards, defaults
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Default build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(LLMLOG_BUILD_TESTING "Build llmlog tests" ON)

# -----------------------------------------------------------------------------
# fbpp dependency
#
# Two modes:
#   1. Local sibling checkout for fast dev iteration:
#        cmake -S . -B build -DFBPP_SOURCE_DIR=../fbpp
#   2. CMake FetchContent (default) — CI and external consumers.
# -----------------------------------------------------------------------------
include(FetchContent)

# fbpp's own CMake defaults to BUILD_TESTING=ON and BUILD_EXAMPLES=OFF.
# Force both OFF here so we don't pull fbpp's 266-test suite into our build.
set(BUILD_TESTING     OFF CACHE BOOL "Disable tests in sub-builds" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "Disable examples in sub-builds" FORCE)
# fbpp uses FIREBIRD_* CMake options; fine to leave at defaults.

if(DEFINED FBPP_SOURCE_DIR)
    get_filename_component(FBPP_SOURCE_DIR "${FBPP_SOURCE_DIR}" ABSOLUTE)
    message(STATUS "llmlog: using local fbpp from ${FBPP_SOURCE_DIR}")
    if(NOT EXISTS "${FBPP_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "FBPP_SOURCE_DIR=${FBPP_SOURCE_DIR} does not contain a CMakeLists.txt")
    endif()
    # Expose fbpp's cmake module path so FindFirebird.cmake is discoverable.
    list(APPEND CMAKE_MODULE_PATH "${FBPP_SOURCE_DIR}/cmake")
    add_subdirectory("${FBPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/fbpp-build" EXCLUDE_FROM_ALL)
else()
    # Pin to a specific SHA on the RAII-guards + Windows-CI branch until
    # PR #13 (sashok74/fbpp#13) merges to main, at which point this should
    # switch to a stable tag.
    FetchContent_Declare(fbpp
        GIT_REPOSITORY https://github.com/sashok74/fbpp.git
        GIT_TAG        claude/firebird-raii-and-windows-ci
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(fbpp)
    list(APPEND CMAKE_MODULE_PATH "${fbpp_SOURCE_DIR}/cmake")
endif()

# Re-enable our own testing switch after forcing fbpp's off.
set(BUILD_TESTING ${LLMLOG_BUILD_TESTING} CACHE BOOL "" FORCE)

# -----------------------------------------------------------------------------
# Our own deps (Conan-provided)
# -----------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
add_subdirectory(src)

if(LLMLOG_BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
message(STATUS "llmlog: configuration summary")
message(STATUS "  Version:     ${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Tests:       ${LLMLOG_BUILD_TESTING}")
message(STATUS "  fbpp source: ${fbpp_SOURCE_DIR}${FBPP_SOURCE_DIR}")
