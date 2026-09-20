"""deploy_relay_direct.py -- put THIS TREE's relay on the VPS without images.yml / deploy.yml.

The CI path (images.yml -> GHCR -> deploy.yml, ~10 min plus a reviewer click) is the RELEASE path
and stays the source of truth for what `latest` means. This is the iteration path: while a relay
change is being measured against a live match (mp:R4b, RP5, R6 ...) a ten-minute round trip per
rebuild is the wall, and the relay itself is a 1 MB static binary that this box can cross-compile
in seconds -- the crate is pure Rust (chacha20/hmac/sha2/tokio, no C dependency), so
`x86_64-unknown-linux-musl` links with rustc's own bundled `rust-lld` and needs no `cc`, no zig,
no Docker here. What the script does, in order:

  1. `cargo build --release --target x86_64-unknown-linux-musl -p mh_relay` (incremental: seconds).
  2. scp the binary + a two-line Dockerfile to `<deploy dir>/dev-relay/` on the VPS.
  3. On the VPS: `docker build` a distroless image tagged `ghcr.io/<owner>/mh-relay:dev-<sha>`,
     set `RELAY_TAG=dev-<sha>` in the compose `.env`, `docker compose up -d relay`.
  4. Print the new container's first log lines (the `listening` line is the proof it came up).

`deploy/docker-compose.yml` resolves the relay image as `${RELAY_TAG:-${IMAGE_TAG:-latest}}`, so
a later deploy.yml run -- which rewrites `.env` without RELAY_TAG -- puts the GHCR image back by
itself; `--rollback` does the same by hand. The dev image is never pushed anywhere: it exists only
in the VPS's local image store, and the tag names the commit it came from (`-dirty` when the tree
had uncommitted changes) so `docker compose ps` says what is running.

THE ONE GUARD: a relay restart ends every match it carries unless BOTH the relay and the clients
are post-R4b (the 2026-09-19 VPS smoke lost its match to exactly that), so the script reads the relay's last
`counters` line and REFUSES when `peers` is not 0. `--force` overrides it, knowingly.

Host + key come from machine_config (VPS_HOST "user@host", VPS_SSH_KEY a private-key path), both
refused-if-empty like every other tool that dials the VPS. The deploy dir defaults to `~/mh-deploy`
(deploy.yml's own default); `--deploy-dir` overrides.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import machine_config as machine  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MUSL = "x86_64-unknown-linux-musl"
BIN = ROOT / "target" / MUSL / "release" / "mh_relay"
DEV_DOCKERFILE = (
    "FROM gcr.io/distroless/static-debian12:nonroot\n"
    "COPY --chmod=0755 mh_relay /usr/local/bin/relay\n"
    'ENTRYPOINT ["/usr/local/bin/relay"]\n'
)


def die(msg):
    print(f"deploy_relay_direct: {msg}", file=sys.stderr)
    sys.exit(2)


def run(cmd, **kw):
    print("+", " ".join(cmd if isinstance(cmd, list) else [cmd]), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def ssh_base():
    if not machine.VPS_HOST or not machine.VPS_SSH_KEY:
        die("VPS_HOST / VPS_SSH_KEY unset -- put them in tools/machine.local.json (see bootstrap)")
    return ["-i", machine.VPS_SSH_KEY, "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"]


def ssh(script, capture=False):
    cmd = ["ssh", *ssh_base(), machine.VPS_HOST, script]
    print("+ ssh", machine.VPS_HOST, "<script>", flush=True)
    r = subprocess.run(cmd, check=False, capture_output=capture, text=True)
    if r.returncode != 0:
        if capture:
            sys.stderr.write(r.stderr)
        die(f"remote step failed (exit {r.returncode})")
    return r.stdout if capture else ""


def build():
    env = dict(os.environ)
    # rustc ships rust-lld + musl's crt objects for this target; pointing the linker at them is
    # what turns "linker `cc` not found" (the default on a Windows host) into a static-pie ELF.
    env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER"] = "rust-lld"
    env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_RUSTFLAGS"] = (
        "-C link-self-contained=yes -C linker-flavor=ld.lld"
    )
    run(["rustup", "target", "add", MUSL], cwd=ROOT)
    run(
        ["cargo", "build", "--release", "--target", MUSL, "-p", "mh_relay", "--bin", "mh_relay"],
        cwd=ROOT,
        env=env,
    )
    if not BIN.is_file():
        die(f"build produced no {BIN}")
    print(f"built {BIN} ({BIN.stat().st_size} bytes)")


def git_tag():
    sha = subprocess.run(
        ["git", "rev-parse", "--short=10", "HEAD"], cwd=ROOT, capture_output=True, text=True
    ).stdout.strip()
    dirty = subprocess.run(
        ["git", "status", "--porcelain", "--", "src/relay", "Cargo.lock", "Cargo.toml"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return f"dev-{sha}{'-dirty' if dirty else ''}"


def resolve_deploy_dir(deploy_dir):
    """A quoted `~` is a literal on the remote (deploy.yml's own first-run failure) -- expand it
    against the VPS's real $HOME once, here, so every later shlex.quote is safe."""
    if deploy_dir == "~" or deploy_dir.startswith("~/"):
        home = ssh("echo $HOME", capture=True).strip()
        deploy_dir = home + deploy_dir[1:]
    return deploy_dir


def relay_peers(deploy_dir):
    out = ssh(
        f"cd {shlex.quote(deploy_dir)} && docker logs --tail 400 mh-relay 2>&1 "
        "| { grep '\"event\":\"counters\"' || true; } | tail -1",
        capture=True,
    )
    m = re.search(r'"peers":(\d+)', out)
    return None if not m else int(m.group(1))


def deploy(tag, deploy_dir):
    q = shlex.quote
    dev_dir = f"{deploy_dir}/dev-relay"
    ssh(f"mkdir -p {q(dev_dir)}")
    scp_target = f"{machine.VPS_HOST}:{dev_dir}/"
    dockerfile = ROOT / "target" / MUSL / "release" / "Dockerfile.dev-relay"
    dockerfile.write_text(DEV_DOCKERFILE)
    run(["scp", *ssh_base(), str(BIN), scp_target])
    run(["scp", *ssh_base(), str(dockerfile), f"{scp_target}Dockerfile"])
    script = f"""set -euo pipefail
cd {q(deploy_dir)}
OWNER=$(grep '^GHCR_OWNER=' .env | cut -d= -f2)
[ -n "$OWNER" ] || {{ echo 'GHCR_OWNER missing from .env (deploy.yml writes it)' >&2; exit 3; }}
IMG="ghcr.io/$OWNER/mh-relay:{tag}"
docker build -q -t "$IMG" dev-relay/
grep -v '^RELAY_TAG=' .env > .env.new || true
echo "RELAY_TAG={tag}" >> .env.new
mv .env.new .env
docker compose config --images | grep -F "$IMG" >/dev/null || {{ echo "compose did not resolve $IMG -- is deploy/docker-compose.yml synced?" >&2; exit 4; }}
docker compose up -d relay
sleep 2
docker compose ps relay --format '{{{{.Name}}}} {{{{.Image}}}} {{{{.Status}}}}'
docker logs --tail 5 mh-relay 2>&1 | cut -c1-240
"""
    ssh(script)


def rollback(deploy_dir):
    q = shlex.quote
    ssh(
        f"""set -euo pipefail
cd {q(deploy_dir)}
grep -v '^RELAY_TAG=' .env > .env.new || true
mv .env.new .env
docker compose up -d relay
sleep 2
docker compose ps relay --format '{{{{.Name}}}} {{{{.Image}}}} {{{{.Status}}}}'
"""
    )


def sync_compose(deploy_dir):
    run(
        [
            "scp",
            *ssh_base(),
            str(ROOT / "deploy" / "docker-compose.yml"),
            f"{machine.VPS_HOST}:{deploy_dir}/docker-compose.yml",
        ]
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--deploy-dir", default="~/mh-deploy", help="compose dir on the VPS")
    ap.add_argument("--force", action="store_true", help="restart even with peers connected")
    ap.add_argument("--build-only", action="store_true", help="cross-build, touch nothing remote")
    ap.add_argument("--no-build", action="store_true", help="ship the binary already in target/")
    ap.add_argument(
        "--sync-compose",
        action="store_true",
        help="also scp deploy/docker-compose.yml first (needed once, for the RELAY_TAG line)",
    )
    ap.add_argument("--rollback", action="store_true", help="drop RELAY_TAG, back to IMAGE_TAG")
    a = ap.parse_args()
    a.deploy_dir = resolve_deploy_dir(a.deploy_dir)

    if a.rollback:
        peers = relay_peers(a.deploy_dir)
        if peers and not a.force:
            die(f"relay reports peers={peers}; a restart kills their match (--force to override)")
        rollback(a.deploy_dir)
        return

    if not a.no_build:
        build()
    if a.build_only:
        return

    tag = git_tag()
    peers = relay_peers(a.deploy_dir)
    if peers is None:
        print("no counters line found in the relay log (fresh container?) -- continuing")
    elif peers and not a.force:
        die(f"relay reports peers={peers}; a restart kills their match (--force to override)")
    if a.sync_compose:
        sync_compose(a.deploy_dir)
    deploy(tag, a.deploy_dir)
    print(f"deployed relay {tag}; `--rollback` returns to the GHCR image")


if __name__ == "__main__":
    main()
