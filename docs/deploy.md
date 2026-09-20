# Images and deploy (dist RP4)

How the two server-side pieces — the multiplayer relay (`src/relay`, dist R1) and the report
collector (`src/collector`, dist RP2) — become container images on GHCR and end up running on the
VPS. Three workflows and one compose stack:

- [`.github/workflows/images.yml`](../.github/workflows/images.yml) — builds both images on every
  PR (build-only) and push to `master` (build + push to GHCR), and gates every PR touching the
  Rust workspace with `cargo audit` + `cargo deny`.
- [`.github/workflows/deploy.yml`](../.github/workflows/deploy.yml) — manual, gated by a protected
  GitHub Environment — syncs [`deploy/`](../deploy) to the VPS and runs `docker compose pull &&
  docker compose up -d`.
- [`deploy/docker-compose.yml`](../deploy/docker-compose.yml) + [`deploy/Caddyfile`](../deploy/Caddyfile)
  — the production stack: relay (host networking) + collector (default bridge, behind Caddy) + Caddy
  (self-signed cert until a domain exists).

Plan: the MP refinement plan's decision D15 (b)+(c) (CI/CD: images to GHCR, ssh deploy gated by an Environment). Related: this repo's
[release.md](release.md) (the player-facing zips — a *different* pipeline, same repo), the
`src/collector/README.md` (the collector service itself, RP2), `src/relay/README.md` (the relay
crate, R1 — in flight in parallel with this work; see "Assumptions" below).

- [1. The image pipeline](#1-the-image-pipeline)
- [2. The supply-chain gate](#2-the-supply-chain-gate)
- [3. One-time: the `vps` Environment](#3-one-time-the-vps-environment)
- [4. Running a deploy](#4-running-a-deploy)
- [5. The compose stack, and why it is not `src/collector`'s file](#5-the-compose-stack-and-why-it-is-not-srccollectors-file)
- [6. Assumptions this work made about R1](#6-assumptions-this-work-made-about-r1)
- [7. What is verified vs. not](#7-what-is-verified-vs-not)

## 1. The image pipeline

`images.yml` builds two images from a matrix (`relay`, `collector`), each with its own
`type=gha` buildx cache scope so one image's layers never evict the other's:

| image | context | Dockerfile | base |
| --- | --- | --- | --- |
| `mh-relay` | repo root (workspace) | [`src/relay/Dockerfile`](../src/relay/Dockerfile) | `clux/muslrust` builder → `gcr.io/distroless/static-debian12:nonroot` runtime |
| `mh-collector` | `src/collector` | [`src/collector/Dockerfile`](../src/collector/Dockerfile) | `python:3.12-slim` (RP2, unchanged by this work) |

Both are **amd64 only** — no `platforms:` matrix, no QEMU: the VPS this deploys to is a single
amd64 box.

**Every PR** touching the Rust workspace or either Dockerfile builds both images (catches a break
before merge) but never pushes. **A push to `master`** (or a manual `workflow_dispatch`) builds
*and* pushes, tagged with the short commit sha; a `master` build additionally gets `latest`. Image
refs: `ghcr.io/<owner>/mh-relay:<tag>`, `ghcr.io/<owner>/mh-collector:<tag>` — `<owner>` is computed
from `github.repository_owner` (lowercased) at run time, never a literal in any tracked file (this
repo's machine-path/identity lint refuses a literal GitHub username in the tree).

The relay's build context is the **repo root**, not `src/relay` — its `Cargo.toml` reads
`version.workspace = true` from the root workspace manifest. `src/relay/Dockerfile`'s own header
comment has the full reasoning, including why `src/launcher` (the other workspace member) is
copied into the build even though this image never runs it, and how `cargo chef prepare --bin
mh_relay` keeps the launcher's GUI dependency tree (eframe/wgpu/rfd, which need X11/Wayland
headers this musl builder does not carry) out of the cooked dependency layer entirely.

## 2. The supply-chain gate

`images.yml`'s `supply-chain` job runs before `images` (which `needs: supply-chain`) on every
trigger, using the tools' own GitHub Actions (prebuilt binaries, no compile — [`rustsec/audit-check`](https://github.com/rustsec/audit-check),
[`EmbarkStudios/cargo-deny-action`](https://github.com/EmbarkStudios/cargo-deny-action)) against
[`deny.toml`](../deny.toml) at the repo root.

**Run it locally:**

```sh
cargo install --locked cargo-audit cargo-deny   # user-level, no admin; ~2 min to compile each
cargo audit
cargo deny check
```

**Verified at HEAD (2026-09-17, cargo-audit 0.22.2, cargo-deny 0.20.2), against this repo's real
`Cargo.lock`** (364 crate dependencies across `mh_launcher` + `mh_relay` — this machine has no
Docker to build the images with, but `cargo audit`/`cargo deny` need only the lockfile, so this
result *is* real, not a placeholder):

```
cargo audit:   0 advisories (1247 loaded from RustSec) against 364 crate dependencies.
cargo deny:    advisories ok, bans ok, licenses ok, sources ok.
```

`deny.toml`'s own header comment carries the two non-failing `multiple-versions` WARNs
(`windows_x86_64_{gnu,msvc}` at two minor versions — an artifact of `eframe`'s Windows backend
stack, not a security or licensing issue) and the reasoning behind every allow-listed license.
**Re-run after any dependency bump** — this is a point-in-time result, not a standing guarantee,
and the PR gate re-checks it on every change to the workspace.

## 3. One-time: the `vps` Environment

`deploy.yml` cannot run at all until a human does this once, in the repository's Settings — a
workflow file cannot create an Environment, its secrets, or its required reviewers:

1. **Settings → Environments → New environment**, named exactly `vps` (must match
   `environment: vps` in `deploy.yml`).
2. On that Environment, **Required reviewers → add yourself** (or whoever should approve a
   deploy). This is what actually pauses a run for approval — without it the Environment exists
   but approves itself instantly, which is not the gate RP4 asked for.
3. **Environment secrets** (Settings → Environments → `vps` → Add secret):

   | secret | value |
   | --- | --- |
   | `VPS_HOST` | the VPS's bare hostname or IP. `tools/machine_config.py`'s `VPS_HOST` is `"user@host"` for the SSH-tunnel tooling — split it: the host part goes here. |
   | `VPS_USER` | the SSH user — the user part of that same value. |
   | `VPS_SSH_KEY` | the **private key's file contents**, not a path. A GitHub-hosted runner cannot read this machine's filesystem, so `machine_config.py`'s `VPS_SSH_KEY` (a local path) is not what goes here — `cat` the file it names and paste that. Use a key that can run `docker compose`; **not** the read-only `dist:RP3` drain key, which `rrsync -ro` restricts to reading `/srv/reports` and could not run this job's commands even if pointed at it. |

4. **Environment variable** (same page, "Variables" tab — plain text, not a secret, because a
   directory path is not one and whoever approves a run should be able to read it):

   | variable | value |
   | --- | --- |
   | `VPS_DEPLOY_DIR` | the directory on the VPS holding (or that should hold) `docker-compose.yml`, `Caddyfile`, and the collector's `secrets/report_token.txt`. Defaults to `~/mh-deploy` if unset. |

5. On the VPS, **once**, by hand (this workflow never creates it): mint the collector's shared
   report token — `mkdir -p <VPS_DEPLOY_DIR>/secrets && python3 -c "import secrets;
   print(secrets.token_urlsafe(32))" > <VPS_DEPLOY_DIR>/secrets/report_token.txt && chown
   10001:10001 <VPS_DEPLOY_DIR>/secrets/report_token.txt && chmod 0400 <VPS_DEPLOY_DIR>/secrets/report_token.txt`.
   **The `chown` is load-bearing:** compose bind-mounts the file into the container as-is, and
   the collector runs as the fixed uid 10001 (its Dockerfile), so a root-owned `0600` file makes
   every uvicorn worker die on `PermissionError: /run/secrets/report_token` and the container
   sits at `unhealthy` — the first live deploy (2026-09-18) failed exactly there. See
   `src/collector/README.md`'s "Minting the drain key" section for the sibling read-only-key step
   (a different secret, same VPS).

## 4. Running a deploy

**Actions tab → "deploy" → Run workflow.** Optionally set the `tag` input (default `latest`) to
pin a specific image tag rather than whatever `latest` currently points at. The run pauses at the
`vps` Environment gate for the required reviewer's approval (step 2 above) before touching
anything.

Once approved, in order:

1. **Sync** `deploy/docker-compose.yml` + `deploy/Caddyfile` from *this commit* to
   `VPS_DEPLOY_DIR` over `scp` (`appleboy/scp-action`) — the files in this repo are the source of
   truth, never a hand-edit made directly on the box.
2. **Write `.env`** in `VPS_DEPLOY_DIR` with `IMAGE_TAG`, `GHCR_OWNER` and `MH_SITE_ADDR` (this run's own values;
   the last is `VPS_HOST`, the address Caddy serves under -- a bare `:443` site gives `tls internal`
   nothing to issue a certificate for, and a client dialling an IP sends no SNI, so the Caddyfile
   needs both the site address and `default_sni`; the first live deploy failed its /healthz poll
   on exactly that, `alert internal error` on every handshake;
   never committed — `deploy/.gitignore` excludes it).
3. **`docker login ghcr.io`** on the VPS with this run's own `GITHUB_TOKEN` (read-only, expires
   with the job — never a standing credential on the box), then `docker compose pull && docker
   compose up -d`.
4. **Poll `/healthz`** at `https://<VPS_HOST>/healthz` — the site address, not `127.0.0.1`, since
   Caddy routes by Host and an unmatched host gets its empty 200 — through Caddy's self-signed cert (`curl -k` — there is no public CA yet,
   §5) up to six times, 5 s apart, as the one automated signal the stack actually came up rather
   than merely that the ssh commands returned 0.

`docker compose ps` is printed after step 3 for a human glance at the run log.

### 4a. The iteration path: `tools/deploy_relay_direct.py` (added 2026-09-19)

The workflow above is the **release** path and stays what `latest` means. While a relay change is
being measured against live matches, its ~10 min round trip per rebuild is the wall, so the relay
alone has a direct path that bypasses images.yml, GHCR and deploy.yml entirely:

```
python tools/deploy_relay_direct.py [--sync-compose] [--force] [--rollback]
```

It cross-compiles `mh_relay` for `x86_64-unknown-linux-musl` on this box (the crate is pure
Rust, so rustc's bundled `rust-lld` links it with no `cc`, zig or Docker here -- 22 s cold, seconds
incremental, a 1 MB static-pie ELF), scps the binary plus a two-line distroless Dockerfile to
`<deploy dir>/dev-relay/`, builds the image **on the VPS** as `ghcr.io/<owner>/mh-relay:dev-<sha>`
(`-dirty` when `src/relay` had uncommitted changes), writes `RELAY_TAG=dev-<sha>` into `.env` and
`docker compose up -d relay`. The whole loop measured 40 s. The compose file resolves the relay
image as `${RELAY_TAG:-${IMAGE_TAG:-latest}}`; deploy.yml never writes `RELAY_TAG`, so the next CI
deploy puts the GHCR image back by itself, as does `--rollback`. `--sync-compose` scps
`deploy/docker-compose.yml` first (needed once, for that line, and after any compose edit).

Its one guard: the relay's last `counters` line must say `peers=0`, because a restart ends every
match the relay carries (the ship plan's Phase 3 runbook, step 4); `--force` overrides it. Host + key are
`machine_config.VPS_HOST` / `VPS_SSH_KEY`, refused if empty. Note `~` in the deploy dir is
expanded against the VPS's real `$HOME` before any quoting -- the same literal-tilde trap
deploy.yml's first run hit.

The dev image never reaches GHCR and never becomes `latest`: before a release, push and let
images.yml + deploy.yml run so the box carries the reviewed image, not a workstation build.

### 4b. The redeploy rule, the relay healthcheck and the relay key (dist:RP5, 2026-09-19)

**Redeploy rule.** deploy.yml now also runs on `workflow_run` — when images.yml completes green
on master — so a push to master that rebuilds the images creates the deploy run by itself. It
still pauses at the `vps` Environment for the reviewer's click; the rule removes the *dispatch*,
not the approval. A red or cancelled images.yml never deploys (`if:` on the conclusion). The
motive is dead-ends G241 at the VPS: a relay older than the shipped client answered the wave-9
re-key op with `bad_op` and every match stayed on the relay.

**Relay verdict.** After `/healthz`, a fourth ssh step waits for `docker inspect` to say the relay
is `healthy`, prints the new container's `listening` line, then waits up to 75 s for its first
`counters` line and fails unless it carries `peers_rekeyed` and `health_pings` — i.e. the box is
running the current image, read off the log rather than assumed.

**Healthcheck.** distroless has no shell, so the probe is the relay binary itself:
`relay --health 127.0.0.1:7100 --key-file /run/secrets/relay_key` sends one PING from no handle
under the deployment key and exits 0 on a PONG (30 s interval, 3 retries). The relay counts these
as `health_pings`; a deployed relay whose counters show 0 has no healthcheck wired.

**The relay key.** The relay runs `--key-file /run/secrets/relay_key` (compose `secrets:`), 64 hex
digits minted once by hand on the VPS:

```
cd ~/mh-deploy && python3 -c "import secrets; print(secrets.token_hex(32))" > secrets/relay_key.txt && chmod 0444 secrets/relay_key.txt
```

(0444, not 0400: the relay runs as distroless's uid 65532 and the file is a bind mount.) deploy.yml
never syncs `secrets/`, so a redeploy cannot overwrite it. **It is the same value every player's
`mh_key.txt` must hold** — the leg key derives from it — so it is not a secret against players;
it is what keeps random internet scanners off the relay and puts encryption on the wire, which is
why `open` (what the stack ran until 2026-09-19) is LAN/testing only. On this box it lives in
`tools/machine.local.json` as `RELAY_KEY` (`machine_config.RELAY_KEY`, empty by default); players
receive it through LA6's signed manifest, or by hand until then. Rotating it = rewrite the file,
`docker compose up -d relay`, redistribute.

## 5. The compose stack, and why it is not `src/collector`'s file

`deploy/docker-compose.yml` is a **separate file** from `src/collector/docker-compose.yml` (RP2),
on purpose, not an oversight:

| | `src/collector/docker-compose.yml` (RP2) | `deploy/docker-compose.yml` (RP4) |
| --- | --- | --- |
| purpose | local dev loop on a workstation | the VPS production stack |
| images | `build: .` against the collector's own Dockerfile | prebuilt `ghcr.io/.../mh-collector:${IMAGE_TAG}` |
| services | collector + Caddy | **relay** + collector + Caddy |
| needs GHCR credentials | no | yes (to pull) |

Folding the relay into RP2's file would make the local `docker compose up --build` loop depend on
GHCR credentials it has no reason to need, and put a `build:` and an `image:` line in conflict on
the same compose key. The collector *service definition itself* — env vars, secret, healthcheck,
no host port mapping — is kept identical between the two files by hand (both point at
`docker compose exec … healthz`-style checks; there is nothing today that diffs them
automatically, since a Caddyfile and a compose service block are not something `tools/lint_compose.py`
can usefully cross-check — see that file's own header comment).

**The one thing `tools/lint_compose.py` *does* assert, mechanically, on every `lint_repo.py` run,
over every tracked compose file:** the relay service carries `network_mode: host` and the
collector service does not. This is plan D4's own reasoning, not a house style choice: Docker's
default bridge network runs a container's traffic through a userland proxy that rewrites the
packet's apparent source address, which breaks the relay's connection-id demux outright. The lint
also refuses any reference to Watchtower (image or `com.centurylinklabs.watchtower.*` label) —
Watchtower was archived in December 2025 and RP4's scope explicitly does not use it; images are
pulled explicitly by `deploy.yml`, on a human's approval, never auto-updated in place.

```sh
python tools/lint_compose.py            # the two structural checks, over every tracked compose file
python tools/lint_compose.py --selftest # proves each of the 5 arms (3 structural + 2 Watchtower) fires
```

## 6. Assumptions this work made about R1

`dist R1` (the real relay — a tokio UDP forwarder) is being built in parallel with this item, in a
sibling worktree. `src/relay/Dockerfile` and `deploy/docker-compose.yml`'s `relay:` service were
written against `src/relay` **as it stood at base commit `8f9814aa`**: a placeholder binary named
`mh_relay` (`src/relay/Cargo.toml`'s `[[bin]] name = "mh_relay"`) whose only real code is the wire
codec (`src/wire.rs`, mp:T0) against `chacha20`/`hmac`/`sha2` — no `tokio`, no `rustls`, no socket.

**As long as R1 keeps the package/bin name `mh_relay`** and stays a `[[bin]]` target of the same
workspace member, nothing here needs to change on R1's landing. If R1 renames the crate or binary,
two places need the new name: `src/relay/Dockerfile`'s `RELAY_BIN` build arg (one line) and its
`ENTRYPOINT` (which cannot reference an ARG, since distroless has no shell to expand one — the
Dockerfile's own comment marks this line). `deploy/docker-compose.yml`'s `image:` line only ever
names the GHCR repository (`mh-relay`), never the binary inside it, so it needs no change either
way.

If R1 adds `rustls` (RP4's own scope line says the relay ships "distroless/static with musl +
rustls," anticipating this), the `clux/muslrust` builder base was chosen specifically because it
bundles the musl-targeted C toolchain rustls's crypto backend (`ring` or `aws-lc-rs`) needs to
link against under a musl target — a bare `rustup target add … -musl` image would need that added
by hand. `deny.toml`'s license allow-list and `cargo audit`'s clean result (§2) were measured
against the *current* lockfile; re-run both once R1's real dependencies land.

## 7. What is verified vs. not

**Verified on this machine (2026-09-17; Docker is not installed here):**

- `hadolint` (2.15.1, downloaded as a standalone binary — no admin) ran clean against
  `src/relay/Dockerfile`: `hadolint src/relay/Dockerfile` exits 0, no findings.
- `actionlint` (1.7.12, standalone binary) ran clean against all three workflow files:
  `actionlint .github/workflows/images.yml .github/workflows/deploy.yml` exits 0, no findings.
- `cargo audit` / `cargo deny check` — real results against this repo's actual `Cargo.lock`, not a
  synthetic fixture. See §2 for the exact numbers and how to reproduce them.
- `python tools/lint_compose.py` (and `--selftest`) — both pass against the real
  `deploy/docker-compose.yml` and (once merged alongside RP2's work) `src/collector/docker-compose.yml`.
- `python -c "import yaml; yaml.safe_load(open('deploy/docker-compose.yml'))"` — parses; used to
  double-check `relay.network_mode == "host"` and `collector.network_mode is None` directly.

**NOT verified (no Docker, no VPS access, no GitHub Environment on this machine):**

- Neither Dockerfile has actually been **built**. `docker buildx build -f src/relay/Dockerfile .`
  and `docker build src/collector` are the commands that close this gap — RP2's own README states
  the identical caveat for the collector image.
- `docker compose config` has not validated `deploy/docker-compose.yml`'s runtime semantics (only
  its YAML syntax and the two structural facts `lint_compose.py` checks).
- `images.yml` has never run — the first real signal is its first dispatch (a push to `master`, or
  a manual run) once this work is merged.
- `deploy.yml` has never run — it needs the `vps` Environment (§3, a one-time user action) before
  it can run at all; then a real VPS with Docker and `docker compose` installed for the ssh
  commands to succeed against.
- The relay's actual acceptance (a real internet match between two home connections, through the
  deployed relay) is tracker `mp:R4` — a separate, later item; a green `deploy.yml` run proves the
  containers came up and answered `/healthz`, not that the relay's own protocol works end to end.
