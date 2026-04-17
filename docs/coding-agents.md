# Connecting coding agents to llmlog

LLM-driven coding agents are the highest-value use case for llmlog —
they make many small API calls while working on a task, and their cost
can surprise users. Routing them through llmlog gives exact per-session
cost plus a full transcript of what the agent actually asked.

llmlog is a transparent reverse-proxy: any agent that honours a custom
`base_url` / `api_base` / API endpoint setting works without code
changes. This page groups the popular agents by how well the
out-of-the-box integration lands, ordered by current market momentum
(as of Q1 2026) within each group.

---

## At a glance

| Agent | Works? | 2026 market signal |
|---|---|---|
| **Claude Code** | ✅ with API-key accounts | #1 by developer-love (46%) and daily use; $2.5B ARR, 300K+ business customers; ~4% of public GitHub commits |
| **Cline** (VS Code) | ✅ out of the box | 🔥 fastest-growing OSS agent — 60K+ stars, 5M+ installs, 2.5M monthly developers |
| **OpenAI Codex** | ✅ out of the box | 🚀 fastest relative growth — 5% → 40% of Claude Code usage in 4 months, 2M+ weekly active users |
| **Roo Code** | ✅ out of the box | Cline fork with extra orchestration, ~30K stars |
| **OpenHands** (ex-OpenDevin) | ✅ out of the box | OSS lead in stars (71K+), Docker-sandboxed |
| **Aider** | ✅ out of the box | Mature, plateauing — 43.5K stars; last release Aug 2025; still processes 15B tokens/week |
| **Continue.dev** | ✅ out of the box | VS Code / JetBrains; growing in enterprise-policy-constrained environments |
| **Zed AI** | ✅ out of the box | Native to Zed editor; niche but high satisfaction |
| **Goose** (Block) | ✅ out of the box | MCP-native terminal agent |
| **Plandex** | ✅ out of the box | Long-running task planner |
| **Cursor** | ⚠️ partial | $2B+ ARR, $50B valuation — enterprise giant, but Tab / Composer / Agent skip custom base URL; only the custom-slot "Ask" flow goes through llmlog |
| **Windsurf** (Codeium → OpenAI) | ⚠️ partial | Same architecture as Cursor; folded into the OpenAI-Codex stack in 2025 |
| **GitHub Copilot** | ❌ | No custom base URL — traffic goes only to `api.githubcopilot.com` |
| **Devin** / **Manus** / **Replit Agent** | ❌ | Hosted agents; no user-side API config |
| **Amazon Q Developer**, **Tabnine** | ❌ | Proprietary backend, no custom endpoint |
| **Claude desktop app** | ❌ | No `base_url` in settings UI |

---

## Works immediately (set env → run)

Ordered by 2026 momentum — if you're going to demo one, pick from the
top.

### 1. [Claude Code](https://claude.com/claude-code) — market leader

```bash
export ANTHROPIC_BASE_URL=http://127.0.0.1:7788/anthropic
claude
```

Anthropic's CLI agent. Zero to #1 in 8 months since its May 2025
launch. The demo everyone will recognize.

**Caveat:** works with API-key accounts (`ANTHROPIC_API_KEY`).
**Breaks** with Max / Pro subscription because session auth uses
OAuth tokens Anthropic issued to Claude Code's public OAuth client,
and the proxy can't re-sign those. See the "Claude Code auth detail"
section below.

### 2. [Cline](https://github.com/cline/cline) — OSS growth leader

Settings → "OpenAI Compatible" →
Base URL: `http://127.0.0.1:7788/openai/v1`
(or `http://127.0.0.1:7788/anthropic` with the Anthropic provider.)

Autonomous VS Code agent. 60K+ GitHub stars, 6K forks, active releases
(v3.79.0 as of April 2026). Every file-edit / terminal-run / tool-use
round-trip lands as one REQUESTS row — visually compelling for a demo
because the IDE UI shows each step.

### 3. [OpenAI Codex CLI](https://github.com/openai/codex) — fastest riser

```bash
export OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1
codex
```

OpenAI's own CLI agent (relaunched in 2025 around GPT-5-Codex). Usage
went from 5% of Claude Code's to 40% in four months. 2M+ weekly
active users by March 2026. GPT-5.4-Codex added 1M-token context in
March 2026.

### 4. [Roo Code](https://github.com/RooCodeInc/Roo-Code) — Cline fork

Settings → "OpenAI Compatible" → Base URL: `http://127.0.0.1:7788/openai/v1`

Cline fork with extra orchestration (multi-agent flows, planner/
executor split). Setup is identical to Cline.

### 5. [OpenHands](https://github.com/All-Hands-AI/OpenHands) — autonomous SWE

```bash
export LLM_BASE_URL=http://127.0.0.1:7788/openai/v1
```

Docker-sandboxed autonomous SWE agent (ex-OpenDevin). 71K+ stars — the
largest star count in this list — though release cadence is monthly
rather than weekly. Good fit when you want an agent running unattended
inside isolation.

### 6. [Aider](https://aider.chat) — still the easiest to demo

```bash
pip install aider-chat
export OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1
# or: export ANTHROPIC_API_BASE=http://127.0.0.1:7788/anthropic
```

Terminal pair-programmer. 43.5K stars, but last release was August
2025 — the momentum shifted to Cline for the OSS VS Code slot. Still
very much usable and probably the **simplest** 30-second install for a
demo (see the "Recommended demo" section below).

### 7. [Continue.dev](https://continue.dev) — policy-friendly

In `~/.continue/config.json`, set `apiBase` per model entry. Works in
VS Code and JetBrains; common choice in environments where custom
proxies are policy-required.

### 8. [Zed AI](https://zed.dev) — editor-native

Settings → Assistant → API URL. Tight integration with Zed's own
editor; niche footprint but very high satisfaction among users.

### 9. [Goose](https://github.com/block/goose) (Block) — MCP-native

```bash
export GOOSE_OPENAI_API_URL=http://127.0.0.1:7788/openai/v1
```

Terminal agent with deep MCP tool integration. Picking up adoption in
shops already invested in MCP.

### 10. [Plandex](https://plandex.ai) — long-running tasks

```bash
export OPENAI_API_BASE=http://127.0.0.1:7788/openai/v1
```

Built for hours-long multi-file tasks where the agent runs as a
background task with a persistent plan.

### 11. [Smol Developer](https://github.com/smol-ai/developer) / [GPT Engineer](https://github.com/AntonOsika/gpt-engineer) — one-shot generators

Both use the standard OpenAI SDK, so
`OpenAI(base_url="http://127.0.0.1:7788/openai/v1")` wires them up.
One-shot project scaffolding rather than iterative agents.

---

## Works with caveats

### Cursor — $50B valuation, partial routing

Settings → OpenAI API → Base URL: `http://127.0.0.1:7788/openai/v1`

Cursor is the **largest paid agent** by revenue ($2B+ ARR, 67% of
Fortune 500 customers, reportedly raising at a $50B valuation in Q1
2026). However, its Tab-completion, Composer, and built-in Agent mode
use Cursor's own gateway and **skip** the custom slot entirely. Only
explicit "Ask" with a model from the custom slot flows through
llmlog — so you'll see a small fraction of real Cursor usage.

### Windsurf (Codeium, now part of OpenAI's Codex stack)

Same architecture as Cursor; same partial-routing limitation. Codeium
was acquired by OpenAI in 2025 and Windsurf is now part of the unified
Codex product line.

### Cody (Sourcegraph)

Supports a custom LLM gateway through enterprise config, but the free
/ solo tier doesn't expose it. If you're on Sourcegraph Enterprise
you can point its LLM gateway at llmlog.

---

## Doesn't work (no custom base URL)

Traffic goes only to a fixed vendor endpoint. Without reverse-engineering
their traffic (usually a EULA violation), llmlog can't observe these:

- **GitHub Copilot** — `api.githubcopilot.com` only
- **Amazon Q Developer** / **Tabnine** — proprietary backend
- **Devin** / **Manus** / **Replit Agent** — hosted agents; no
  API-level user-side configuration
- **Claude desktop app** — no `base_url` control exposed in the
  settings UI

---

## Claude Code auth detail

Claude Code supports two authentication modes; they behave differently
with llmlog:

1. **API-key** (`ANTHROPIC_API_KEY`) — **fully compatible**. The proxy
   strips the client's `Authorization` header and injects its own
   configured key, so whatever value Claude Code sends is discarded.

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

## Recommended demo: Aider (still the easiest)

Despite the Aider momentum story, it remains the **simplest** 30-second
install for a demo — no VS Code extension, no IDE settings, just `pip`.
Good if your audience cares about "does it work at all" before "is
it hot".

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
llmlog report --today --by-model --tag=aider-demo
```

...gives the total cost of that session.

## Alternative demo: Cline (most visually compelling)

If your audience has VS Code, **Cline** is the stronger story because
every tool-use call has a visible IDE panel and a corresponding
REQUESTS row — the cause-effect is impossible to miss. Setup: install
Cline extension → Settings → "OpenAI Compatible" → Base URL
`http://127.0.0.1:7788/openai/v1` → pick a model → start.

---

## The recursive case (dogfooding)

If you run the llmlog proxy on your dev machine and set
`ANTHROPIC_BASE_URL=http://127.0.0.1:7788/anthropic` in your shell
before starting Claude Code, **every prompt you send to Claude Code
itself gets logged into llmlog's own database**. Works only with
API-key accounts (see auth detail above), but it's a clean closed
loop: llmlog logs the API calls of the agent that's writing llmlog's
own code. With Claude Code already responsible for ~4% of public
GitHub commits, the proportion of its traffic that could realistically
flow through a self-hosted llmlog in the next year is non-trivial.
