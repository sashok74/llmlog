# llmlog

> Status: early WIP. Phase 1 (schema + core library) in progress.

Compact, precise, local-first usage-and-cost tracker for LLM API calls.

Drop-in OpenAI/Anthropic-compatible reverse-proxy. Point your `base_url` at
`http://127.0.0.1:7788` and every upstream call is logged to an embedded
Firebird 5 database with **exact** decimal arithmetic — no float drift on
fractional-cent costs, no token-count overflow, no bloated server.

Built on top of [`fbpp`](https://github.com/sashok74/fbpp) — a modern C++20
Firebird wrapper with compile-time query codegen.

## Why Firebird instead of SQLite?

- `DECFLOAT(34)` preserves every digit of unit prices like `$0.000003 / token`
  through unlimited aggregation without rounding error.
- `NUMERIC(38, x)` / `INT128` give satoshi-level precision for token counts
  across years of use without resorting to `TEXT` trickery.
- PSQL stored procedures keep cost-calculation logic next to the data, not
  in the application tier — meaning the proxy just inserts raw `usage`
  fields and the database does the exact math.
- MVCC reads never block ingestion, so you can run live reports against an
  actively-logging proxy.

## Providers supported (planned)

- Anthropic (SSE streaming + non-stream)
- OpenAI (SSE streaming + non-stream)
- Google Gemini
- OpenAI-compatible: Ollama, LM Studio, DeepSeek, Moonshot/Kimi, Zhipu, Qwen, etc.

## License

MIT — see `LICENSE`.
