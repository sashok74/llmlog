#pragma once

// Server-Sent-Event (SSE) framer + usage extractor for LLM provider streams.
//
// SseFramer          — protocol-level: bytes in, discrete SseEvent out.
// UsageAccumulator   — semantic-level: watches events flow through, keeps
//                      track of the latest usage block and model id.
//
// Both are streaming: you can feed partial chunks as they arrive from the
// upstream socket without waiting for the full response. Events and usage
// updates are emitted eagerly.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace llmlog::core {

/// One decoded SSE event. `name` is whatever followed `event:` (empty for
/// unnamed events, which is how OpenAI / DeepSeek frame everything). `data`
/// is the concatenation of all `data:` lines in the event, joined by `\n`.
struct SseEvent {
    std::string name;
    std::string data;
};

/// Upstream-provider families, for usage-extraction dispatch.
enum class ProviderFamily {
    /// Named events (`event: message_delta` etc.). Usage is nested at
    /// data.usage inside `message_delta` events. Token fields:
    /// input_tokens / output_tokens / cache_creation_input_tokens /
    /// cache_read_input_tokens.
    Anthropic,

    /// Unnamed events. Every event's JSON may contain a top-level `usage`
    /// field with OpenAI's prompt_tokens / completion_tokens / ... shape.
    /// Different providers in this family dispatch usage at different
    /// points in the stream (OpenAI: final choices=[] chunk; DeepSeek:
    /// last content chunk with finish_reason:"stop"). The last non-null
    /// `usage` seen is canonical.
    OpenAICompat,
};

/// Normalized usage block. Fields absent from the upstream payload stay 0.
struct UsageInfo {
    std::int64_t input_tokens       = 0;
    std::int64_t output_tokens      = 0;
    std::int64_t cache_read_tokens  = 0;   ///< Anthropic: cache_read_input_tokens.
                                           ///< OpenAI:    prompt_tokens_details.cached_tokens.
                                           ///< DeepSeek:  same as OpenAI.
    std::int64_t cache_write_tokens = 0;   ///< Anthropic-specific (cache_creation_input_tokens).
    std::int64_t image_tokens       = 0;   ///< OpenAI completion_tokens_details.audio/image splits.
};

/// Stateful byte-stream framer. Call feed() zero or more times, then
/// finish() once the upstream half-closes the stream. The callback
/// passed to both fires synchronously for each completed event.
class SseFramer {
public:
    using Callback = std::function<void(const SseEvent&)>;

    SseFramer() = default;

    /// Push more bytes into the framer. Completed events are delivered to
    /// @p onEvent before feed() returns.
    void feed(std::string_view chunk, const Callback& onEvent);

    /// Flush an unterminated trailing event, if any. Some upstreams close
    /// the connection without the final empty line.
    void finish(const Callback& onEvent);

private:
    void processLine(std::string_view line, const Callback& onEvent);
    void dispatchPending(const Callback& onEvent);

    std::string buffer_;        ///< unterminated-line tail carried between feeds
    SseEvent    pending_;       ///< event currently being assembled
    bool        hasPending_ = false;
};

/// Scrapes SseEvents as they flow through and maintains the latest usage +
/// model id. Always reports "what the stream has disclosed so far". After
/// the stream ends, usage() returns the canonical final usage block.
class UsageAccumulator {
public:
    explicit UsageAccumulator(ProviderFamily family) : family_(family) {}

    /// Process one event. Safe to call with any event — non-usage events
    /// are ignored.
    void onEvent(const SseEvent& event);

    ProviderFamily family() const noexcept { return family_; }
    const std::optional<UsageInfo>&   usage()     const noexcept { return usage_; }
    const std::optional<std::string>& modelId()   const noexcept { return modelId_; }
    const std::optional<std::string>& requestId() const noexcept { return requestId_; }

private:
    void onAnthropicEvent  (const SseEvent&);
    void onOpenAICompatEvent(const SseEvent&);

    ProviderFamily family_;
    std::optional<UsageInfo>   usage_;
    std::optional<std::string> modelId_;
    std::optional<std::string> requestId_;
};

} // namespace llmlog::core
