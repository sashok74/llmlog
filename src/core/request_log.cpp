#include "llmlog/core/request_log.hpp"

#include <fbpp/core/transaction.hpp>
#include <fbpp/core/statement.hpp>
#include <fbpp/core/result_set.hpp>
#include <fbpp/core/exception.hpp>

#include <stdexcept>
#include <string>
#include <tuple>

namespace llmlog::core {

namespace {

// SP_LOG_REQUEST signature (from sql/procedures.sql):
//   (call_time, model_id, request_id, input, output, cache_r, cache_w,
//    image, latency_ms, http_status, tag) → (id, cost_usd)
//
// We use CAST(? AS TIMESTAMP WITH TIME ZONE) for the timestamp and a
// CASE-based NULL mapping for the two optional VARCHAR inputs. Note each
// optional field is bound twice — once for the equality test, once as
// the non-NULL value — so the tuple has 13 entries to fill 11 logical
// parameters.
const char* kLogRequestSql = R"SQL(
SELECT ID, COST_USD FROM SP_LOG_REQUEST(
    CAST(? AS TIMESTAMP WITH TIME ZONE),
    ?,
    CASE WHEN ? = '' THEN NULL ELSE ? END,
    ?, ?, ?, ?, ?,
    ?, ?,
    CASE WHEN ? = '' THEN NULL ELSE ? END
)
)SQL";

} // namespace

LoggedRequest RequestLogDao::insert(const RequestLogEntry& entry) {
    auto tra = conn_->StartTransaction();
    auto st  = conn_->prepareStatement(kLogRequestSql);

    auto rs = tra->openCursor(st, std::make_tuple(
        entry.call_time,
        entry.model_id,
        entry.request_id, entry.request_id,
        entry.input_tokens, entry.output_tokens,
        entry.cache_read,  entry.cache_write, entry.image_tokens,
        entry.latency_ms,  entry.http_status,
        entry.tag, entry.tag
    ));

    std::tuple<std::int64_t, std::string> row;
    if (!rs->fetch(row)) {
        tra->Rollback();
        throw std::runtime_error(
            "SP_LOG_REQUEST returned no rows (unexpected — SUSPEND missing?)");
    }
    tra->Commit();

    return LoggedRequest{std::get<0>(row), std::get<1>(row)};
}

} // namespace llmlog::core
