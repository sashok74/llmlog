# llmlog

[![CI Linux](https://github.com/sashok74/llmlog/actions/workflows/ci-linux-gcc.yml/badge.svg)](https://github.com/sashok74/llmlog/actions/workflows/ci-linux-gcc.yml)
[![CI Windows](https://github.com/sashok74/llmlog/actions/workflows/ci-windows-msvc.yml/badge.svg)](https://github.com/sashok74/llmlog/actions/workflows/ci-windows-msvc.yml)

Compact, local-first usage-and-cost tracker for LLM API calls.

Drop-in OpenAI / Anthropic / Gemini / DeepSeek / Ollama reverse-proxy.
Point your SDK's `base_url` at `http://127.0.0.1:7788` and every upstream
call is logged to an embedded Firebird 5 database with **exact** decimal
arithmetic — no float drift on fractional-cent costs, no token-count
overflow, no bloated server.

## Why Firebird instead of SQLite / Postgres?

- **`DECFLOAT(34)`** preserves every digit of unit prices like
  `$0.000003 / token` through unlimited aggregation without rounding
  error. SQLite has no native decimal; in llmlog, a sum of a million
  sub-cent costs stays exact.
- **Stored procedures** keep cost-calculation logic next to the data —
  the proxy just inserts raw `usage` fields and `SP_LOG_REQUEST` does
  the exact math in DECFLOAT.
- **MVCC reads never block ingestion** — you can run live reports
  against an actively-logging proxy without performance lock-in.
- **Embedded deployment** — one `.fdb` file, `<20 MB` RAM footprint
  on a 1-vCPU VPS.

## Quick start

### 1. Build

```bash
conan install . --output-folder=build --build=missing -s build_type=RelWithDebInfo -s compiler.cppstd=20
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/build/RelWithDebInfo/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Produces `build/src/llmlog` — one binary, ~8 MB stripped.

### 2. Configure

```bash
sudo install -o root -g root -m 0640 config/providers.example.json /etc/llmlog/config.json
sudoedit /etc/llmlog/config.json   # fill in your provider API keys
```

Or use external key files (recommended):

```json
{
  "providers": {
    "anthropic": {
      "base_url":     "https://api.anthropic.com",
      "api_key_file": "/etc/llmlog/secrets/anthropic.key",
      "auth_header":  "x-api-key",
      "extra_headers": {"anthropic-version": "2023-06-01"}
    }
  }
}
```

See `docs/config.md` for the full config schema.

### 3. Apply pricing seed + run

```bash
llmlog seed apply --file seed/pricing.json --config /etc/llmlog/config.json
llmlog proxy --config /etc/llmlog/config.json
```

### 4. Point any LLM SDK at llmlog

```python
from anthropic import Anthropic
# Only change — ONE line:
client = Anthropic(base_url="http://127.0.0.1:7788/anthropic")

msg = client.messages.create(
    model="claude-haiku-4-5", max_tokens=50,
    messages=[{"role":"user","content":"hi"}]
)
```

See `docs/clients.md` for more clients (OpenAI SDK, Cursor, Aider,
Claude Code, Open WebUI, ...).

## Component status

| Phase | Status | What's in |
|---|---|---|
| 1 — Foundation   | ✅ merged-ready | Firebird schema, stored procedures, SSE framer, config loader |
| 2 — Data layer   | ✅ merged-ready | PricingDao, RequestLogDao, seed loader, default prices |
| 3a — Proxy       | ✅ CI green | cpp-httplib server, upstream forwarding, usage capture, DB logging |
| 3b — UX          | 🚧 next    | `llmlog`/`report`/`tail`/`use`/`doctor` CLI |
| 4 — Multi-user   | 📋 planned | Web UI, per-user bearer tokens, quota alerts |

## Supported providers out of the box

All four families, through path-prefix routing `/<provider>/...`:

- **Anthropic** — named-event SSE, `message_delta` usage extraction
- **OpenAI**    — `stream_options.include_usage=true`, final chunk with `choices=[]`
- **DeepSeek / Moonshot / Qwen / Zhipu / Ollama / LM Studio** — OpenAI-compatible
- **Google Gemini** — treated as OpenAI-compat today; dedicated adapter in a later phase

## Documentation

- **[Clients — how to connect](docs/clients.md)** — SDKs, CLIs, IDE extensions, web UIs
- **[Deployment](docs/deploy.md)** — Linux VPS setup end-to-end
- **[Config reference](docs/config.md)** — every field in `config.json`

## License

MIT — see `LICENSE`.
