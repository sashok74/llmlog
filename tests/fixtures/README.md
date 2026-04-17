# SSE fixtures — real streaming responses from upstream providers

These files are verbatim server-sent-event streams captured on 2026-04-17
from live upstream endpoints with cheap `"hi"`-class prompts (≤40 output
tokens, cost per run ≈ $0.00005). They serve as the ground truth for
llmlog's provider adapters — the parsers are tested against these bytes
instead of against synthetic mock streams that might drift from reality.

## Files

| File | Model | Provider | Stream framing |
|---|---|---|---|
| `anthropic_stream.sse` | `claude-haiku-4-5` | Anthropic | named events (`event:` + `data:`) |
| `openai_stream.sse`    | `gpt-5.4-mini`     | OpenAI    | `data:` only |
| `deepseek_stream.sse`  | `deepseek-chat`    | DeepSeek  | `data:` only (OpenAI-compatible) |

## Where the `usage` block lives — this is the key question

**Anthropic:** emitted as a dedicated `event: message_delta` near end of stream,
just before `event: message_stop`. Fields:
```json
"usage": {
  "input_tokens": 20,
  "cache_creation_input_tokens": 0,
  "cache_read_input_tokens": 0,
  "output_tokens": 12
}
```
Parser: watch for events named `message_delta`, extract the nested
`data.usage`. Note the input token count appears here even though
`message_start` already reported it (for non-streaming it's only in
`message_delta`). We use the `message_delta` value since it's canonical.

**OpenAI:** when the caller sets `stream_options.include_usage: true`, OpenAI
emits an extra final `data:` chunk where `choices` is an **empty array**
and `usage` is fully populated. Fields:
```json
"usage": {
  "prompt_tokens": 18,
  "completion_tokens": 11,
  "total_tokens": 29,
  "prompt_tokens_details":    {"cached_tokens":0, "audio_tokens":0},
  "completion_tokens_details":{"reasoning_tokens":0, "audio_tokens":0,
                               "accepted_prediction_tokens":0,
                               "rejected_prediction_tokens":0}
}
```
Parser: watch for a `data:` chunk whose JSON has `choices == []` AND
`usage != null`. That's the sentinel.

**DeepSeek:** piggy-backs `usage` onto the **last content chunk** (the one
with `finish_reason: "stop"`), NOT into a separate empty-choices chunk
like OpenAI. Fields:
```json
"usage": {
  "prompt_tokens": 16,
  "completion_tokens": 3,
  "total_tokens": 19,
  "prompt_tokens_details": {"cached_tokens":0},
  "prompt_cache_hit_tokens": 0,
  "prompt_cache_miss_tokens": 16
}
```
Parser: track `usage` on every chunk and always keep the last non-null.
Same generic logic works for DeepSeek and OpenAI — the DeepSeek case
just happens to fire a chunk earlier. We never need provider-specific
heuristics for "which chunk carries usage", only the same "last non-null
usage wins" rule.

## Quirks worth documenting

- **OpenAI's `obfuscation` field** appears on every chunk, holds random
  base64-ish data of varying length. Anti-timing-attack padding; parsers
  must ignore it.
- **Anthropic JSON payloads have trailing spaces** inside the closing
  brace (`}      }`). Valid JSON; most parsers don't care, but ad-hoc
  regex splitters might.
- **Anthropic model string** returned is `claude-haiku-4-5` even when the
  request asked for the same alias — no version-suffix appended (unlike
  OpenAI which returns `gpt-5.4-mini-2026-03-17`).
- **DeepSeek cache-hit/miss** adds `prompt_cache_hit_tokens` +
  `prompt_cache_miss_tokens` on top of the standard OpenAI-style
  `prompt_tokens_details.cached_tokens`. We map the detail.cached_tokens
  field into llmlog's `cache_read` column for consistency across
  providers.

## How to regenerate

Captured via `curl -sN` against each provider's chat-completion endpoint
with `stream: true` and `stream_options.include_usage: true` (for OpenAI
and DeepSeek). The SSE files include blank-line chunk terminators and
should be treated as raw bytes — do NOT normalize line endings.

See `docs/provider-quirks.md` (Phase 2+) for the full capture command.
