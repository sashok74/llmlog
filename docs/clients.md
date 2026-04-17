# Connecting clients to llmlog

llmlog works with any client that honours a custom `base_url` — which is
every major SDK and most AI dev-tools. You never need a bespoke llmlog
library; you just point existing code at the proxy and it logs every
call without touching anything else.

The proxy routes by path prefix: `/<provider>/<...>` → configured
upstream for that provider. Provider names match keys in
`config.json → providers`.

---

## Quickest demo: `curl`

```bash
curl -N http://127.0.0.1:7788/anthropic/v1/messages \
  -H "content-type: application/json" \
  -H "X-LlmLog-Tag: sandbox" \
  -d '{"model":"claude-haiku-4-5","max_tokens":40,
       "messages":[{"role":"user","content":"Привет"}]}'
```

The `X-LlmLog-Tag` header survives into `REQUESTS.TAG` so you can group
this call in reports. Everything else is a normal Anthropic request.

For OpenAI / DeepSeek replace the prefix:

```bash
curl http://127.0.0.1:7788/openai/v1/chat/completions -H "content-type: application/json" \
     -d '{"model":"gpt-5.4-mini","stream":true,
          "stream_options":{"include_usage":true},
          "messages":[{"role":"user","content":"hi"}]}'

curl http://127.0.0.1:7788/deepseek/v1/chat/completions -H "content-type: application/json" \
     -d '{"model":"deepseek-chat","stream":true,
          "stream_options":{"include_usage":true},
          "messages":[{"role":"user","content":"hi"}]}'
```

The proxy injects the provider's real API key server-side, so your
client sends no `Authorization` header at all.

---

## Official SDKs (one-line change)

### Anthropic Python / JS

```python
from anthropic import Anthropic
client = Anthropic(base_url="http://127.0.0.1:7788/anthropic")
# rest of the code is identical to using the real api.anthropic.com
```

```javascript
import Anthropic from "@anthropic-ai/sdk";
const client = new Anthropic({ baseURL: "http://127.0.0.1:7788/anthropic" });
```

### OpenAI / DeepSeek / any OpenAI-compatible

```python
from openai import OpenAI
client = OpenAI(
    base_url="http://127.0.0.1:7788/openai/v1",
    api_key="not-used-but-required-by-sdk")

# Same for DeepSeek, Moonshot, Qwen, LM Studio, Ollama, ...
ds = OpenAI(base_url="http://127.0.0.1:7788/deepseek/v1", api_key="x")
```

### Google Gemini (Python `google-genai`)

```python
from google import genai
client = genai.Client(http_options={"base_url":"http://127.0.0.1:7788/gemini"})
```

---

## Dev-tools (zero code change, one env var)

All of these support pointing at a custom base URL. Set the env and
restart the tool.

| Tool | Env variable |
|---|---|
| **Claude Code** | `ANTHROPIC_BASE_URL=http://127.0.0.1:7788/anthropic` |
| **Aider** | `OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1` |
| **Cursor** | Settings → OpenAI API → Base URL: `http://127.0.0.1:7788/openai/v1` |
| **Continue.dev** | In `~/.continue/config.json`, set `apiBase` per model entry |
| **Zed AI** | Settings → Assistant → API URL |
| **Goose** (Block) | `GOOSE_OPENAI_API_URL=http://127.0.0.1:7788/openai/v1` |

---

## CLI tools

| Tool | How |
|---|---|
| **[llm](https://llm.datasette.io)** (Simon Willison) | `LLM_ANTHROPIC_API_BASE=http://127.0.0.1:7788/anthropic llm -m claude-opus-4-7 "..."` |
| **[aichat](https://github.com/sigoden/aichat)** | `~/.config/aichat/config.yaml` → `api_base: http://127.0.0.1:7788/openai/v1` |
| **[chatblade](https://github.com/npiv/chatblade)** | `OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1` |
| **[fabric](https://github.com/danielmiessler/fabric)** | `~/.config/fabric/.env` → `OPENAI_API_BASE_URL=...` |

---

## Web UIs (demoing to non-developers)

| UI | Where to set base URL |
|---|---|
| **[Open WebUI](https://openwebui.com)** | Admin → Settings → Connections → "OpenAI API" endpoint |
| **[LibreChat](https://librechat.ai)** | `librechat.yaml` → `endpoints.openAI.baseURL` |
| **[BigAGI](https://big-agi.com)** | Settings → Models → "OpenAI" → API host |
| **[TypingMind](https://www.typingmind.com)** | Settings → Models → Custom Provider → endpoint |
| **[Chatbox](https://chatboxai.app)** | Model Settings → API Host |

Any of these running against llmlog gives you a ChatGPT-style UI plus
exact cost tracking behind the scenes.

---

## Multi-profile shell (planned for Phase 3b)

After `llmlog use work`, your shell gets `ANTHROPIC_BASE_URL`,
`OPENAI_BASE_URL`, `X-LlmLog-Tag=work`, etc. exported so any subsequent
`python app.py` or `curl ...` is routed + tagged automatically.
Switching projects is one word:

```bash
llmlog use project-a   # tag=project-a, providers=[anthropic, openai]
llmlog use ollama-only # local-only profile, no cloud providers
```

---

## What the proxy does NOT need from the client

- No special `Authorization` header — the proxy injects the real
  upstream key server-side. You can still send `Authorization: Bearer
  llmlog-...` if you enable bearer auth in the proxy (Phase 3b), but
  it's optional.
- No modified request body — the entire JSON passes through unchanged.
- No streaming-format translation — SSE goes through as-is; the proxy
  only observes the `usage` block on its way to the client.
