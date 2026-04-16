#include "test_base.hpp"

#include "llmlog/core/database.hpp"

#include <fbpp_util/connection_helper.hpp>

#include <atomic>
#include <string>

namespace llmlog::test {

namespace {

std::atomic<unsigned> gCounter{0};

std::string scopedPath(std::string path, std::string_view scope, unsigned counter) {
    const auto colon = path.find(':');
    std::string head;
    std::string body = std::move(path);
    if (colon != std::string::npos) {
        head = body.substr(0, colon + 1);
        body = body.substr(colon + 1);
    }
    const auto dot = body.rfind('.');
    std::string stem = (dot != std::string::npos) ? body.substr(0, dot) : body;
    std::string ext  = (dot != std::string::npos) ? body.substr(dot)    : std::string{".fdb"};
    return head + stem + "_" + std::string(scope) + "_" + std::to_string(counter) + ext;
}

} // namespace

ConnectionParams makeScopedParams(std::string_view scope) {
    auto params = fbpp::util::getConnectionParams("tests.temp_db");
    const unsigned counter = gCounter.fetch_add(1, std::memory_order_relaxed);
    params.database = scopedPath(params.database, scope, counter);
    return params;
}

std::unique_ptr<fbpp::core::Connection>
setupFreshDatabase(const ConnectionParams& params) {
    dropQuietly(params);
    fbpp::core::Connection::createDatabase(params);
    auto conn = std::make_unique<fbpp::core::Connection>(params);
    llmlog::core::bootstrapSchema(*conn);
    return conn;
}

void dropQuietly(const ConnectionParams& params) noexcept {
    try {
        fbpp::core::Connection::dropDatabase(params);
    } catch (...) {
        // best-effort only
    }
}

void LlmlogTestBase::SetUp() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string scope = std::string(info->test_suite_name()) + "_" + info->name();
    params_     = makeScopedParams(scope);
    connection_ = setupFreshDatabase(params_);
}

void LlmlogTestBase::TearDown() {
    connection_.reset();
    dropQuietly(params_);
}

} // namespace llmlog::test
