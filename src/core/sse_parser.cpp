#include "llmlog/core/sse_parser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace llmlog::core {

namespace {
using json = nlohmann::json;

// Extract a signed integer field without throwing if absent / wrong type.
std::int64_t intOrZero(const json& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number())         return static_cast<std::int64_t>(it->get<double>());
    return 0;
}

std::string stringOr(const json& obj, std::string_view key, std::string def = {}) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

} // namespace

// ===========================================================================
//  SseFramer
// ===========================================================================
void SseFramer::feed(std::string_view chunk, const Callback& onEvent) {
    buffer_.append(chunk);

    std::size_t start = 0;
    while (true) {
        const auto nl = buffer_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string_view line(buffer_.data() + start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        processLine(line, onEvent);
        start = nl + 1;
    }
    if (start > 0) buffer_.erase(0, start);
}

void SseFramer::finish(const Callback& onEvent) {
    // A stream that ends without a final blank line still has a pending
    // event to flush.
    if (!buffer_.empty()) {
        // Treat any remaining non-terminated line as the last line of the
        // current event.
        std::string_view tail(buffer_);
        if (!tail.empty() && tail.back() == '\r') tail.remove_suffix(1);
        processLine(tail, onEvent);
        buffer_.clear();
    }
    dispatchPending(onEvent);
}

void SseFramer::processLine(std::string_view line, const Callback& onEvent) {
    // Empty line → event boundary.
    if (line.empty()) {
        dispatchPending(onEvent);
        return;
    }
    // Comment line (leading colon).
    if (line.front() == ':') return;

    // Field parsing: "field:value" or "field: value" (one optional leading space).
    std::string_view field;
    std::string_view value;
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        field = line;
        value = {};
    } else {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    }

    if (field == "event") {
        pending_.name.assign(value);
        hasPending_ = true;
    } else if (field == "data") {
        if (!pending_.data.empty()) pending_.data.push_back('\n');
        pending_.data.append(value);
        hasPending_ = true;
    }
    // `id:` / `retry:` intentionally ignored — nothing in our stack needs them.
}

void SseFramer::dispatchPending(const Callback& onEvent) {
    if (!hasPending_) return;
    onEvent(pending_);
    pending_ = SseEvent{};
    hasPending_ = false;
}

// ===========================================================================
//  UsageAccumulator
// ===========================================================================
void UsageAccumulator::onEvent(const SseEvent& event) {
    switch (family_) {
        case ProviderFamily::Anthropic:     onAnthropicEvent(event);     break;
        case ProviderFamily::OpenAICompat:  onOpenAICompatEvent(event);  break;
    }
}

void UsageAccumulator::onAnthropicEvent(const SseEvent& event) {
    // The [DONE] sentinel some proxies emit is not valid JSON; skip.
    if (event.data.empty() || event.data == "[DONE]") return;

    json j;
    try {
        j = json::parse(event.data);
    } catch (const json::parse_error&) {
        return; // malformed payload — silently ignore, upstream bug not ours
    }
    if (!j.is_object()) return;

    // message_start carries model id and initial input-tokens estimate.
    if (event.name == "message_start") {
        if (j.contains("message") && j["message"].is_object()) {
            const auto& m = j["message"];
            if (!modelId_) {
                auto id = stringOr(m, "model");
                if (!id.empty()) modelId_ = std::move(id);
            }
            if (!requestId_) {
                auto rid = stringOr(m, "id");
                if (!rid.empty()) requestId_ = std::move(rid);
            }
        }
    }

    // message_delta carries the canonical final usage.
    if (event.name == "message_delta") {
        auto it = j.find("usage");
        if (it != j.end() && it->is_object()) {
            UsageInfo u = usage_.value_or(UsageInfo{});
            u.input_tokens       = intOrZero(*it, "input_tokens");
            u.output_tokens      = intOrZero(*it, "output_tokens");
            u.cache_read_tokens  = intOrZero(*it, "cache_read_input_tokens");
            u.cache_write_tokens = intOrZero(*it, "cache_creation_input_tokens");
            usage_ = u;
        }
    }
}

void UsageAccumulator::onOpenAICompatEvent(const SseEvent& event) {
    if (event.data.empty() || event.data == "[DONE]") return;

    json j;
    try {
        j = json::parse(event.data);
    } catch (const json::parse_error&) {
        return;
    }
    if (!j.is_object()) return;

    // Every chunk has the model id; capture on first sight.
    if (!modelId_) {
        auto id = stringOr(j, "model");
        if (!id.empty()) modelId_ = std::move(id);
    }
    if (!requestId_) {
        auto rid = stringOr(j, "id");
        if (!rid.empty()) requestId_ = std::move(rid);
    }

    auto it = j.find("usage");
    if (it == j.end() || it->is_null() || !it->is_object()) return;

    UsageInfo u;
    u.input_tokens  = intOrZero(*it, "prompt_tokens");
    u.output_tokens = intOrZero(*it, "completion_tokens");
    // Cached input tokens — OpenAI puts them nested, DeepSeek also nests.
    auto pdetails = it->find("prompt_tokens_details");
    if (pdetails != it->end() && pdetails->is_object()) {
        u.cache_read_tokens = intOrZero(*pdetails, "cached_tokens");
    }
    // OpenAI audio/image splits live in completion_tokens_details; fold
    // audio_tokens into image_tokens for now — keeping one slot for
    // non-text output until we have a richer schema.
    auto cdetails = it->find("completion_tokens_details");
    if (cdetails != it->end() && cdetails->is_object()) {
        u.image_tokens = intOrZero(*cdetails, "audio_tokens");
    }
    usage_ = u;
}

} // namespace llmlog::core
