# Deploying llmlog on a Linux VPS

This guide walks through deploying llmlog on a clean Debian 12 VPS.
Other distros work the same way; the commands that differ are the
package manager invocations.

Verified end-to-end on a 1-vCPU / 960 MB Debian 12 box — the target is
a self-hosted setup that stays comfortable within that envelope.

---

## 1. Install Firebird 5 (portable tarball)

Debian's `apt` ships Firebird 3; llmlog requires 5.x for `DECFLOAT`.

```bash
sudo apt-get update
sudo apt-get install -y wget ca-certificates libatomic1 libicu72 libtommath1 libncurses6

cd /root
wget -q https://github.com/FirebirdSQL/firebird/releases/download/v5.0.0/Firebird-5.0.0.1306-0-linux-x64.tar.gz
tar xzf Firebird-5.0.0.1306-0-linux-x64.tar.gz
cd Firebird-5.0.0.1306-0-linux-x64
sudo ./install.sh -silent </dev/null
```

The installer generates a random SYSDBA password and writes it to
`/opt/firebird/SYSDBA.password`. Save it — llmlog needs it in the
config.

```bash
REAL_PW=$(grep ISC_PASSWORD /opt/firebird/SYSDBA.password | cut -d= -f2)
echo "$REAL_PW" > /root/.fb_sysdba && chmod 600 /root/.fb_sysdba
```

### Register SYSDBA in Srp plugin (required for TCP auth)

The installer stages the password but doesn't register it in the
security-database's Srp256 plugin. Do that once via isql in embedded
mode:

```bash
sudo systemctl stop firebird
sudo /opt/firebird/bin/isql -q -user sysdba <<EOF
connect 'employee';
create user SYSDBA password '$REAL_PW';
commit;
quit;
EOF
sudo systemctl start firebird
```

### Bind to loopback only

The default listens on `*:3050` — on a public VPS that's a security
hole. Edit `/opt/firebird/firebird.conf`:

```ini
RemoteBindAddress = 127.0.0.1
```

Then `sudo systemctl restart firebird`.

Optional firewall on `ufw`:

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow from YOUR.DEV.IP.HERE to any port 22
sudo ufw enable
```

---

## 2. Prepare llmlog database + system user

```bash
sudo useradd -r -s /usr/sbin/nologin -d /var/lib/llmlog -m llmlog
sudo mkdir -p /etc/llmlog/secrets /var/lib/llmlog /var/log/llmlog
sudo chown firebird:firebird /var/lib/llmlog
sudo chmod 755 /var/lib/llmlog
sudo chown root:root /etc/llmlog/secrets
sudo chmod 700 /etc/llmlog/secrets

# Create the empty DB (schema is bootstrapped on first llmlog start).
sudo /opt/firebird/bin/isql -q -user sysdba -password "$REAL_PW" <<EOF
create database 'localhost:/var/lib/llmlog/db.fdb' default character set UTF8;
commit;
quit;
EOF
sudo chown firebird:firebird /var/lib/llmlog/db.fdb
sudo chmod 660 /var/lib/llmlog/db.fdb
```

---

## 3. Drop in API keys (never commit these)

```bash
# Each key in its own 0600 file, owned by root. The llmlog config only
# stores the FILE PATH; the secret itself never lands in /etc/llmlog/config.json.
echo -n 'sk-ant-api03-...'            | sudo tee /etc/llmlog/secrets/anthropic.key
echo -n 'sk-proj-...'                 | sudo tee /etc/llmlog/secrets/openai.key
echo -n 'sk-...'                      | sudo tee /etc/llmlog/secrets/deepseek.key
sudo chmod 600 /etc/llmlog/secrets/*.key
```

---

## 4. Build llmlog

Requires a C++20 compiler, CMake ≥ 3.20, Conan 2.

```bash
git clone https://github.com/sashok74/llmlog.git
cd llmlog
conan profile detect --force
conan install . --output-folder=build --build=missing \
  -s build_type=RelWithDebInfo -s compiler.cppstd=20
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/build/RelWithDebInfo/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel

sudo install -o root -g root -m 0755 build/src/llmlog /usr/local/bin/llmlog
```

(Pre-built releases will come later; building from source is the only
path for now.)

---

## 5. Write `/etc/llmlog/config.json`

See `docs/config.md` for the full schema. Minimal example:

```bash
REAL_PW=$(cat /root/.fb_sysdba)
sudo tee /etc/llmlog/config.json <<JSON
{
  "bind":     { "host": "127.0.0.1", "port": 7788 },
  "database": {
    "server": "localhost", "port": 3050,
    "path":   "/var/lib/llmlog/db.fdb",
    "user":   "SYSDBA",
    "password": "${REAL_PW}",
    "charset": "UTF8"
  },
  "proxy_auth": { "mode": "bearer", "required": false },
  "providers": {
    "anthropic": {
      "base_url": "https://api.anthropic.com",
      "api_key_file": "/etc/llmlog/secrets/anthropic.key",
      "auth_header": "x-api-key",
      "extra_headers": {"anthropic-version": "2023-06-01"},
      "kind": "anthropic"
    },
    "openai": {
      "base_url": "https://api.openai.com/v1",
      "api_key_file": "/etc/llmlog/secrets/openai.key",
      "kind": "openai"
    },
    "deepseek": {
      "base_url": "https://api.deepseek.com/v1",
      "api_key_file": "/etc/llmlog/secrets/deepseek.key",
      "kind": "openai_compat"
    }
  },
  "logging": { "level": "info", "file": "/var/log/llmlog/llmlog.log" }
}
JSON
sudo chown root:llmlog /etc/llmlog/config.json
sudo chmod 640 /etc/llmlog/config.json
```

---

## 6. Seed pricing + run

```bash
sudo -u llmlog llmlog seed apply --file seed/pricing.json --config /etc/llmlog/config.json
sudo -u llmlog llmlog proxy --config /etc/llmlog/config.json
# → "llmlog proxy listening on 127.0.0.1:7788"
```

Verify:

```bash
curl http://127.0.0.1:7788/healthz      # → {"status":"ok"}
```

---

## 7. Run as a systemd service (recommended)

A first-party `llmlog.service` ships in Phase 3b. Until then, a minimal
hand-rolled unit:

```ini
# /etc/systemd/system/llmlog.service
[Unit]
Description=llmlog — LLM API usage tracker proxy
After=network.target firebird.service
Requires=firebird.service

[Service]
Type=simple
User=llmlog
Group=llmlog
ExecStart=/usr/local/bin/llmlog proxy --config /etc/llmlog/config.json
Restart=on-failure
RestartSec=5s

# Hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/log/llmlog
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now llmlog
sudo systemctl status llmlog
journalctl -u llmlog -f
```

---

## 8. Point clients at the proxy

From your dev machine, either:

- SSH-tunnel the proxy port: `ssh -L 7788:127.0.0.1:7788 youruser@vps`, then
  point SDKs at `http://127.0.0.1:7788`, or
- Run WireGuard / Tailscale across your dev box and the VPS, point
  SDKs at `http://<tailnet-ip>:7788`, or
- Put Caddy / nginx in front with Let's Encrypt + bearer-auth and
  expose `https://llmlog.example.com/` on the public internet.

Never bind llmlog directly on `0.0.0.0:7788` — it has no inbound-auth
enforcement in the current phase (comes in 3b).

---

## Upgrades

Pull the repo, rebuild, restart:

```bash
cd llmlog
git pull
cmake --build build --parallel
sudo install -o root -g root -m 0755 build/src/llmlog /usr/local/bin/llmlog
sudo systemctl restart llmlog
```

Schema migrations (when they become necessary) will land as versioned
SQL scripts under `sql/migrations/` and have a dedicated `llmlog
migrate` CLI command.
