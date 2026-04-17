# `config.json` — full reference

llmlog loads a single JSON configuration file on startup. Location:

1. `--config <path>` on the CLI, if given
2. `$LLMLOG_CONFIG` env variable, if set
3. `~/.config/llmlog/config.json` (per-user deployments)
4. `/etc/llmlog/config.json` (system-wide deployments)

Unknown top-level keys are ignored for forward-compat; unknown keys
inside a provider block produce a warning but do not fail the load.

---

## Secret-source pattern (applies to `api_key` and `database.password`)

Each secret-bearing field accepts three mutually-exclusive forms:

| Form | Example | Security |
|---|---|---|
| `api_key` | `"api_key": "sk-literal-value"` | in-file; discouraged for anything but dev |
| `api_key_env` | `"api_key_env": "ANTHROPIC_API_KEY"` | resolved via `getenv()` at load time |
| `api_key_file` | `"api_key_file": "/etc/llmlog/secrets/anthropic.key"` | file read at load time, trailing whitespace stripped |

Setting more than one of the three triggers a `ConfigError` identifying
the offending JSON-pointer path. The resolved secret is then stored in
memory; the raw `*_env` / `*_file` indirection never reaches callers.

---

## Top-level schema

```json
{
  "bind":       { ... },
  "database":   { ... },
  "proxy_auth": { ... },
  "providers":  { "<name>": { ... }, ... },
  "logging":    { ... }
}
```

### `bind`

| Field | Type | Default | Notes |
|---|---|---|---|
| `host` | string | `"127.0.0.1"` | Listener interface. **Never bind `0.0.0.0` without a reverse proxy in front** — see `docs/deploy.md`. |
| `port` | integer | `7788` | TCP port. |

### `database`

Firebird 5 connection parameters. `server`/`port`/`charset` default to
localhost:3050/UTF8; everything else is required.

| Field | Type | Required | Notes |
|---|---|---|---|
| `server` | string | no | default `"localhost"` |
| `port`   | int    | no | default `3050` |
| `path`   | string | **yes** | either a filesystem path or an alias from `databases.conf` |
| `user`   | string | **yes** | |
| `password` / `password_env` / `password_file` | string | **yes (exactly one)** | see secret-source pattern |
| `charset` | string | no | default `"UTF8"` |

### `proxy_auth`

Inbound authentication for clients connecting TO the proxy. Hardening
is minimal in Phase 3a; a full bearer-token system with key hashes in a
`API_KEYS` table lands in Phase 3b.

| Field | Type | Default |
|---|---|---|
| `mode` | string | `"bearer"` |
| `required` | bool | `true` |
| `initial_bootstrap_token` | string \| null | `null` |

### `providers` — keyed by provider name

Each key is the name the client uses in the URL prefix
(`/anthropic/...` → `providers.anthropic`). Provider names starting
with underscore are skipped (so the bundled template's
`_anthropic_template` is inert).

| Field | Type | Default | Notes |
|---|---|---|---|
| `base_url` | string | **required** | upstream URL; scheme must be `https://` for real providers, `http://` for local (Ollama, LM Studio) |
| `api_key` / `api_key_env` / `api_key_file` | string | **required (exactly one)** | see secret-source pattern |
| `auth_header` | string | `"Authorization"` | e.g. `"x-api-key"` for Anthropic |
| `auth_scheme` | string | `"Bearer"` | `""` when the value goes raw (Anthropic's `x-api-key: sk-ant-...`) |
| `extra_headers` | object | `{}` | additional request headers; Anthropic needs `{"anthropic-version": "2023-06-01"}` |
| `kind` | string | `"openai_compat"` | controls SSE usage-extraction family: `"anthropic"` (named events) vs `"openai"` / `"openai_compat"` / `"gemini"` (OpenAI-style data chunks) |

### `logging`

| Field | Type | Default |
|---|---|---|
| `level` | string | `"info"` |
| `file`  | string | `""` (→ stderr) |
| `rotate_max_size_mb` | int | `10` |
| `rotate_max_files` | int | `5` |

---

## Minimal working example

```json
{
  "bind": { "host": "127.0.0.1", "port": 7788 },
  "database": {
    "path": "/var/lib/llmlog/db.fdb",
    "user": "SYSDBA",
    "password_file": "/root/.fb_sysdba"
  },
  "providers": {
    "anthropic": {
      "base_url": "https://api.anthropic.com",
      "api_key_file": "/etc/llmlog/secrets/anthropic.key",
      "auth_header": "x-api-key",
      "auth_scheme": "",
      "extra_headers": { "anthropic-version": "2023-06-01" },
      "kind": "anthropic"
    },
    "openai": {
      "base_url": "https://api.openai.com/v1",
      "api_key_file": "/etc/llmlog/secrets/openai.key",
      "kind": "openai"
    }
  }
}
```

`config/providers.example.json` in the repo has entries for all
currently-supported provider families (Anthropic, OpenAI, Gemini,
DeepSeek, Moonshot, Ollama, LM Studio).

---

## Validation errors

`llmlog proxy` fails fast on config errors with a JSON-pointer-style
path to the offending field:

```
error: at /providers/anthropic/api_key: exactly one of 'api_key',
'api_key_env', 'api_key_file' may be set; found 2
```

```
error: at /database: missing required field 'path'
```

Use `llmlog doctor` (Phase 3b) to sanity-check the config without
starting the listener.
