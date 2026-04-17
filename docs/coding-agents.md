# Connecting coding agents to llmlog

LLM-driven coding agents are the highest-value use case for llmlog —
they make many small API calls while working on a task, and their cost
can surprise users. Routing them through llmlog gives exact per-session
cost plus a full transcript of what the agent actually asked.

llmlog is a transparent reverse-proxy: any agent that honours a custom
`base_url` / `api_base` / API endpoint setting works without code
changes. This page groups the popular agents by how well the
out-of-the-box integration lands.

---

## Works immediately (set env → run)

| Agent | Config | Notes |
|---|---|---|
| **[Claude Code](https://claude.com/claude-code)** | `export ANTHROPIC_BASE_URL=http://127.0.0.1:7788/anthropic` | Anthropic's official CLI. Works with API-key accounts; **breaks** with Max / Pro subscription because OAuth tokens can't be re-signed by the proxy (see caveat below). |
| **[Aider](https://aider.chat)** | `OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1`<br>or `ANTHROPIC_API_BASE=http://127.0.0.1:7788/anthropic` | Terminal pair-programmer. `pip install aider-chat` — probably the easiest agent to demo with. |
| **[Cline](https://github.com/cline/cline)** | Settings → "OpenAI Compatible" → Base URL: `http://127.0.0.1:7788/openai/v1` | Autonomous VS Code agent with file-edit + terminal tools. |
| **[Roo Code](https://github.com/RooCodeInc/Roo-Code)** | Same as Cline (it's a fork) | Cline fork with extra orchestration. |
| **[Continue.dev](https://continue.dev)** | `~/.continue/config.json` → per-model `apiBase` | VS Code / JetBrains. Chat, inline-edit, and agent mode. |
| **[OpenHands](https://github.com/All-Hands-AI/OpenHands)** (ex-OpenDevin) | `LLM_BASE_URL=http://127.0.0.1:7788/openai/v1` | Docker-sandboxed autonomous SWE-agent. |
| **[Goose](https://github.com/block/goose)** (Block) | `GOOSE_OPENAI_API_URL=http://127.0.0.1:7788/openai/v1` | Terminal agent with MCP tool integration. |
| **[Plandex](https://plandex.ai)** | `OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1` | Long-running task planner for multi-file changes. |
| **Zed AI** | Settings → Assistant → API URL | Editor-integrated assistant. |
| **[Smol Developer](https://github.com/smol-ai/developer)** / **[GPT Engineer](https://github.com/AntonOsika/gpt-engineer)** | Standard OpenAI SDK → `OpenAI(base_url=...)` | One-shot codebase generators from a prompt. |

All of these log **every individual API call** the agent makes,
including the tool-use and planning round-trips the user never sees.

---

## Works with caveats

| Agent | What's different |
|---|---|
| **Cursor** | The "Custom OpenAI" slot in Settings accepts a base URL, but Tab-completion, Composer, and the built-in Agent mode use Cursor's own gateway and **skip the custom slot entirely**. Only explicit "Ask" with a model from the custom slot flows through llmlog. |
| **Windsurf** (Codeium) | Same architecture as Cursor — partial routing at best. |
| **Cody** (Sourcegraph) | Supports custom LLM gateway through enterprise config, but the free/solo tier doesn't expose it. |

---

## Doesn't work (no custom base URL)

These agents ship with a fixed upstream and can't be pointed at a
reverse-proxy without reverse-engineering their traffic (which the
vendor EULAs usually forbid):

- **GitHub Copilot** — traffic goes only to `api.githubcopilot.com`
- **Amazon Q Developer** / **Tabnine** — proprietary backend
- **Devin** / **Manus** / **Replit Agent** — hosted agents; no
  API-level configuration exposed to the user
- **Claude desktop app** — no `base_url` control in the settings UI

---

## Caveat for Claude Code specifically

Claude Code supports two auth modes:

1. **API-key** (`ANTHROPIC_API_KEY`) — fully compatible with llmlog.
   The proxy strips the client's `Authorization` header and injects
   its own configured key, so whatever value Claude Code sends is
   discarded. Works end-to-end.

2. **Max / Pro subscription** (OAuth device-code flow) — the session
   uses short-lived bearer tokens Anthropic issued to Claude Code's
   public OAuth client. The proxy can't re-sign those, and Anthropic
   expects the session token (not an API key) on subscription
   accounts. Routing through llmlog in this mode will return `401
   Unauthorized` from upstream.

For subscription users who still want per-session cost tracking, the
workaround today is to also have a lightweight API-key account for
agent traffic and keep subscription usage in the native client.

---

## Recommended demo: Aider

Aider is the most compelling "agent through llmlog" demo because it's
free, `pip`-installable, produces real file edits, and every step
corresponds to exactly one visible API call.

Setup (once):

```bash
pip install aider-chat
```

Demo (three terminals):

**Terminal A — VPS, run the tail:**
```bash
ssh vps
llmlog tail -f
```

**Terminal B — SSH-tunnel the proxy port from your laptop:**
```bash
ssh -L 7788:127.0.0.1:7788 vps
```

**Terminal C — your laptop, run Aider:**
```bash
cd ~/some-small-repo
export ANTHROPIC_API_BASE=http://127.0.0.1:7788/anthropic
aider --model anthropic/claude-opus-4-7
# or for OpenAI:
# export OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1
# aider --model openai/gpt-5.4-mini
```

As you ask Aider to refactor, fix a bug, add tests — each round-trip
appears in Terminal A within a second, with exact token counts and
cost. At the end:

```bash
llmlog report --today --by-model --tag=$(whoami)-aider-demo
```

...gives the total cost of that session.

---

## The recursive case (dogfooding)

If you run the llmlog proxy on your dev machine and set
`ANTHROPIC_BASE_URL=http://127.0.0.1:7788/anthropic` in your shell
before starting Claude Code, **every prompt you send to Claude Code
itself gets logged into llmlog's own database**. Works only with
API-key accounts (see caveat above), but it's a clean closed loop:
llmlog logs the API calls of the agent that's writing llmlog's own
code.
