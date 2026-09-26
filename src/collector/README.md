# mh report collector (tracker `dist:RP2`)

A small FastAPI service that accepts crash/bug report zips from the launcher
(tracker `dist:RP1`, Rust -- not this package), authenticates them with a
shared-secret HMAC, stores them under a server-minted ULID path, and indexes
them in SQLite with sha256-based de-duplication. It sits behind Caddy, which
terminates TLS and enforces a 64 MB body cap at the edge.

Design source: the MP refinement plan's decisions D13 and D14 (2026-09-17): reports are
zips with a required description, the collector is FastAPI behind Caddy, storage is by
server-minted id, drain is read-only rsync. Tracker item: `dist:RP2`.

## Endpoints

- `POST /v1/reports` -- multipart upload. Fields: `report` (the zip file),
  `description` (required, non-empty text), `meta` (a JSON object string with
  `match_id`, `version`, `build`, `exit_code`, `os`). Headers: `X-Report-Token`
  (the shared secret), `X-Report-Timestamp` (unix seconds),
  `X-Report-Signature` (hex HMAC-SHA256 over `"<timestamp>\n<sha256(body)>"`,
  keyed by the same shared token -- see `collector/signing.py`). Any auth
  failure is a 401, written nothing.
- `GET /healthz` -- 200 + `{"status": "ok", "reports_total": N, "duplicates_total": M}`.

## Run it locally without Docker

```sh
python -m venv .venv
. .venv/Scripts/activate       # or: source .venv/bin/activate
pip install -r requirements.txt -r requirements-dev.txt

export REPORT_TOKEN="a-local-dev-token"     # PowerShell: $env:REPORT_TOKEN="..."
export REPORTS_DIR="./data/reports"
uvicorn collector.app:create_app --factory --reload --port 8000
```

Then, from another shell:

```sh
python scripts/send_report.py --url http://127.0.0.1:8000/v1/reports \
    --token-file <(echo -n "a-local-dev-token") \
    --zip path/to/some.zip --description "manual smoke test"
```

(`--token-file` wants a real file; on Windows write the token to a temp file
instead of using `<(...)` process substitution, which PowerShell doesn't have.)

## Run it with `docker compose` (once Docker is installed -- UNVERIFIED here)

Docker is **not installed on the machine this was built on** (2026-09-17,
user action pending). The `Dockerfile`, `docker-compose.yml` and `Caddyfile`
are written and their syntax is checked (`python -c "import yaml;
yaml.safe_load(open('docker-compose.yml'))"` for the compose file; the
Caddyfile's directives are written to Caddy's documented syntax but were not
run through `caddy validate`, since neither `docker` nor a `caddy` binary is
on this machine either). Everything the actual **service** does was instead
proven directly against `uvicorn` -- see "What is verified vs. not" below.

Once Docker exists:

```sh
cd src/collector
mkdir -p secrets
python -c "import secrets; print(secrets.token_urlsafe(32))" > secrets/report_token.txt
MH_REPORTS_DIR=./data/reports docker compose up --build   # Windows/macOS: no /srv here
```

`MH_REPORTS_DIR` is this dev stack's one knob (tracker `dist:RP7`): the
collector's `/data/reports` is a **bind mount** of `${MH_REPORTS_DIR:-/srv/reports}`,
the default being the VPS's drain root so the two compose files agree
(`tools/lint_compose.py` rule 4 refuses anything else for `/data/reports`
in either file). Whatever directory you point it at must be writable by uid
10001, the container's fixed user.

Then exercise the done_when clauses:

```sh
# 40 MB zip accepted + indexed
python scripts/send_report.py --url https://localhost/v1/reports --insecure \
    --token-file secrets/report_token.txt --zip <a 40MB zip> --description "..."

# 70 MB body -> 413 (Caddy's request_body max_size rejects it before proxying)
python scripts/send_report.py --url https://localhost/v1/reports --insecure \
    --token-file secrets/report_token.txt --zip <a 70MB zip> --description "..."

# re-send the same zip -> {"status": "duplicate", ...}
python scripts/send_report.py --url https://localhost/v1/reports --insecure \
    --token-file secrets/report_token.txt --zip <the same 40MB zip> --description "retry"

# bad HMAC -> 401
python scripts/send_report.py --url https://localhost/v1/reports --insecure \
    --token-file secrets/report_token.txt --zip <any zip> --description "..." --bad-signature

# health
curl -k https://localhost/healthz
```

## What is verified vs. not

Verified on this machine (`python -m pytest src/collector -q`, a real
`uvicorn` process + `send_report.py`, `ruff format --check` / `ruff check`,
`python tools/lint_repo.py`): every behavior the service itself is
responsible for -- auth, storage, dedupe, the two size-limit layers'
application-side half, `/healthz`, malformed-multipart survival, retention
pruning.

**Not verified** (Docker absent): that the `Dockerfile` actually builds, that
the non-root user / `HEALTHCHECK` behave as written inside a real container,
that `docker-compose.yml` actually brings both services up and Caddy really
proxies to `collector:8000`, and that the Caddyfile's `tls internal` +
`request_body max_size 64MB` behave as documented. Once Docker is installed,
`docker compose up --build` followed by the commands above (plus `docker
compose config` to have Compose itself validate the YAML) closes that gap.

## SPKI pinning (no domain yet -- plan D14 / section 6 answer 4)

With no domain registered, Caddy cannot get a publicly-trusted certificate, so
the `Caddyfile` uses `tls internal` (Caddy's own local CA, generated on first
run) instead. The launcher (RP1) does not trust that CA -- it pins the
certificate's SPKI (Subject Public Key Info) hash instead, the way DDNet's
updater does. To get the hash once the stack is running:

```sh
openssl s_client -connect <vps-host>:443 </dev/null 2>/dev/null \
  | openssl x509 -pubkey -noout \
  | openssl pkey -pubin -outform der \
  | openssl dgst -sha256 -binary | base64
```

That hash is what gets baked into the launcher next to its `minisign` public
key. Revisit this whole section once a domain exists: swap `tls internal` for
a bare Caddy site block and automatic HTTPS (Let's Encrypt) takes over, and
the launcher goes back to normal CA trust.

## Draining reports (tracker `dist:RP3`, built -- `tools/drain_reports.py`)

RP2's job stops at accepting and indexing reports; getting them off the VPS
onto a dev machine is `tools/drain_reports.py` (tracker `dist:RP3`), which
pulls new report directories over a **dedicated, READ-ONLY** SSH key and
prints one summary line per report it actually drains. It is a separate tool
from this package on purpose: the drain key must never be the VPS's admin
key, and must never be able to do anything but read `/srv/reports`.

### Minting the drain key

```sh
ssh-keygen -t ed25519 -f drain_key -N "" -C "mh-drain-readonly"
```

Then, on the VPS (as the admin, using the real `VPS_SSH_KEY` -- this step is
NOT something `drain_reports.py` itself ever does):

```sh
mkdir -p /srv/reports && chown -R 10001:10001 /srv/reports
echo 'restrict,command="rrsync -ro /srv/reports" '"$(cat drain_key.pub)" \
    >> ~/.ssh/authorized_keys
```

`/srv/reports` is BOTH the drain root and the collector's store: the compose
files bind-mount it at `/data/reports`, and the `chown` matters because the
container writes as uid 10001 (its Dockerfile). Until 2026-09-20 the deployed
stack mounted a named docker volume there instead, so every real upload
returned 201 into `/var/lib/docker/volumes/mh-deploy_reports/_data` and the
drain printed `nothing new` (tracker `dist:RP7`, dead-ends G250).

`rrsync` ships with `rsync` itself (`man rrsync`; if `/usr/bin/rrsync` is not
already there, `dpkg -L rsync | grep rrsync` finds it, or install the
`rsync` package). `restrict` disables every SSH feature the key does not need
(port/agent/X11 forwarding, ptys, SFTP); `command="rrsync -ro /srv/reports"`
**forces** every session opened with this key to run exactly that, regardless
of what the client asks for -- so the key can only read report directories
under `/srv/reports`, and a client that tries to push through it (a
write-direction rsync) is refused by `rrsync` itself:

```
rrsync error: sending to read-only server is not allowed
```

(verified live against a real VPS, 2026-09-17 -- see
`tools/drain_reports.py --selftest` and its module docstring for the
Windows-specific rsync/ssh transport gotchas this needed).

### Running the drain

```sh
export MH_DRAIN_KEY=/path/to/drain_key      # the READ-ONLY key, never VPS_SSH_KEY
python tools/drain_reports.py                # -> tmp/drained_reports (gitignored)
python tools/drain_reports.py --json
python tools/drain_reports.py --selftest      # hermetic, no VPS needed
```

The host comes from `machine_config.VPS_HOST` (same source `tools/mh_tunnel.ps1`
uses); the key from `MH_DRAIN_KEY`, falling back to `machine_config.VPS_SSH_KEY`
(the ADMIN key) only as a quick smoke-test convenience before a dedicated
drain key exists -- a real deployment should always set `MH_DRAIN_KEY`.
With the admin key there is no rrsync forced command, so the tool drains
`host:/srv/reports/` instead of `host:/` and warns on stderr. Every pull also
carries a filter that admits only `<YYYY-MM>/` trees, so a wrong root copies
nothing rather than the whole filesystem (dead-ends G320).
`machine_config.py` itself is not touched by this tool (the drain key is
deliberately NOT a `machine_config` constant, so a config file typo can never
substitute the admin key for the read-only one without an explicit
`MH_DRAIN_KEY` override). Two consecutive runs pull each report exactly
once: rsync only transfers what changed, and `drain_reports.py` additionally
tracks which report directories it has already summarized (a local
`.drain_manifest.json` next to the drained tree) so a report is never printed
twice either.

### The `crash` field contract (for `dist:LA4`, the launcher's uploader)

`meta.json` (the client-supplied `meta` object POSTed to `/v1/reports`, plus
the server's own `_received_at`/`_client_ip`/`_sha256`) may carry an
**optional** `crash` object:

```json
{
  "match_id": "...", "version": "...", "build": "...",
  "exit_code": -1073741819, "os": "windows",
  "crash": {"module": "mh.dll", "offset": "0x000175b0"}
}
```

`module` is `"mh.dll"`, `"mh.exe"`, or `"mh.focus.exe"`; `offset`'s MEANING is
module-dependent (this is the part LA4 must get right when it starts writing
this field): for the game exe it is IMAGE-RELATIVE (the WER/event-log "fault
offset", i.e. `VA - 0x00400000` on the EN build); for `mh.dll` it is already
an **RVA** (`mh.dll` relocates, so there is no fixed base to subtract --
Windows itself already reports a DLL fault as an offset from wherever the
loader placed it, which is exactly what an RVA is). A report with no `crash`
key is an ordinary bug report, not an error.

`tools/crash_report.py --report <drained dir>` reads this field and resolves
it to a C++ function name -- `docs/symbols.md` for the exe, or
`src/mh_dll/Release/mh.map` (parsed by RVA) for `mh.dll` (tracker
`TL-GATE1`, closed by this work: `crash_report.py` used to only ever print
`mh.dll+<offset>`). It also cross-checks the report's `match_id` against the
`; [session] match_id=` line(s) SES0 writes into the peers' own
`mh_net.log`s inside `report.zip`, so the printed match_id is proven correct,
not merely present.

## The R2 presigned-PUT escape hatch (documented, not built)

RP2's done_when is explicit that this is **documented only** -- there is no
code for it in this package. If report volume or zip size ever makes the
"stream every upload through this one Python process" design a bottleneck,
the escape hatch (plan D14 / D14's research digest, Cloudflare R2 presigned
URLs) is:

1. The collector (or a tiny sibling endpoint) mints a short-lived presigned
   `PUT` URL for an R2 object key it chooses (still server-minted, never
   client-supplied, same reasoning as the ULID path today).
2. The launcher `PUT`s the zip directly to R2, bypassing this process and its
   64 MB in-memory buffering entirely.
3. R2 notifies the collector (or the collector polls / the launcher makes a
   small follow-up POST with the object key + sha256) so the SQLite index
   still gets a row -- dedupe would then key off a `HEAD` request's ETag/sha256
   metadata instead of bytes already in hand.

Nothing about the current storage layout (`REPORTS_DIR/<YYYY-MM>/<ulid>/`) or
the SQLite schema blocks adding this later; it would be an additional
upload path, not a rewrite of this one.

## Rate limiting: a known limitation

`RATE_PER_MIN` (plan D14 "per-IP and per-token limits in app code") is
enforced with an in-process token bucket (`collector/ratelimit.py`). With
`--workers 2` (the Dockerfile's default), each worker has its own bucket, so
the effective limit per key is up to `2 x RATE_PER_MIN`. Acceptable at this
service's expected scale (a handful of players' crash reports); a shared
store would be the fix if that ever stops being true.

## Layout

```
collector/          the FastAPI application package
  app.py             POST /v1/reports, GET /healthz, request wiring
  auth.py            HMAC + timestamp verification (fails closed, before storage)
  signing.py         the HMAC scheme itself, shared with scripts/send_report.py
  config.py          env-var settings (REPORTS_DIR, TOKEN_FILE, MAX_BODY_MB, RATE_PER_MIN, ...)
  storage.py         atomic on-disk writes (temp-in-dir -> fsync -> os.replace -> fsync dir)
  db.py              the SQLite index (sha256 dedupe, prune queries)
  ratelimit.py        the in-process token bucket
  middleware.py       the app-level body-size guard (belt-and-suspenders with Caddy)
  ulid.py             server-side ULID minting (never a client-supplied id in a path)
  cli.py              `python -m collector.cli prune --days 180`
scripts/
  send_report.py      dependency-free manual test client (NOT the launcher's uploader)
tests/                 pytest + httpx (FastAPI TestClient) -- one clause per done_when item
Dockerfile             python:3.12-slim, non-root, HEALTHCHECK
docker-compose.yml      collector + caddy, secrets via compose `secrets:`, /data/reports bound to ${MH_REPORTS_DIR:-/srv/reports}
Caddyfile               tls internal, request_body max_size 64MB, reverse_proxy
requirements.txt         runtime deps (installed into the image)
requirements-dev.txt      + pytest/httpx, local test-running only
```
