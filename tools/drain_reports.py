#!/usr/bin/env python3
r"""drain_reports.py -- pull new crash/bug report directories off the VPS collector (tracker
`dist:RP2`) over a READ-ONLY rrsync key, into a local gitignored store, and print one summary
line per report this run actually drained (tracker `dist:RP3`, plan D14 "Drain").

WHY A DEDICATED READ-ONLY KEY. The collector (`src/collector`) writes reports server-side under
`/srv/reports/<YYYY-MM>/<ulid>/{report.zip,meta.json,description.txt}` (RP2's storage layout).
Getting them onto a dev machine should not need the VPS's real admin key: the drain key is a
SEPARATE ed25519 keypair, and the VPS's `~/.ssh/authorized_keys` line for it is

    restrict,command="rrsync -ro /srv/reports" ssh-ed25519 AAAA... mh-drain-readonly

`restrict` turns off every ssh feature the key does not need (port/agent/X11 forwarding, ptys);
`command="rrsync -ro /srv/reports"` FORCES every session on this key to run `rrsync -ro
/srv/reports` regardless of what the client asked for -- so the key can only read that one
directory tree and can never run an arbitrary remote command. `rrsync` ships with rsync itself
(`man rrsync`; `dpkg -L rsync | grep rrsync` finds it if it is not already on PATH as
`/usr/bin/rrsync`). A client push through this key (a write-direction rsync) is refused by
`rrsync` itself with `rrsync error: sending to read-only server is not allowed` -- proven live
against the VPS 2026-09-17, not merely asserted; see "Draining reports" in
src/collector/README.md for the exact authorized_keys recipe and how the key was minted.

RRSYNC'S CLIENT-PATH CONVENTION (easy to get wrong once, so it is written down here). rrsync
`chdir()`s into its configured root (`/srv/reports`) and then applies the CLIENT's requested path
*relative to that root*, after stripping one leading slash. So the client must NOT repeat the
remote root in its own path -- `host:/srv/reports` doubles it into `/srv/reports/srv/reports` (a
`change_dir ... No such file or directory` from the far end). The whole tree is `host:/` (a bare
slash IS the configured root once the leading slash is stripped); a subtree would be
`host:/2026-09/` etc. This module always drains the whole tree.

KEY / HOST RESOLUTION. The host is `machine_config.VPS_HOST` (an ssh destination, `user@host`) --
same source `tools/mh_tunnel.ps1` and the collector's SPKI-pinning docs use, refused (not
guessed) when unset, for the same reason `mh_tunnel.ps1` refuses: a missing value must not
silently mean "use whatever was last configured". The KEY is different from
`machine_config.VPS_SSH_KEY` on purpose -- that is the ADMIN key (full shell), and this tool
should never need it. `machine_config.py` is not in this tool's write set (see the RP3 tracker
row), so the drain key's path is read from the environment instead: `MH_DRAIN_KEY` first, falling
back to `machine_config.VPS_SSH_KEY` if unset (useful for a quick local smoke test with the admin
key before the dedicated read-only key exists; the real drain key should be set via `MH_DRAIN_KEY`
so an operator is never one `machine.local.json` edit away from draining with the wrong key).

A WINDOWS GOTCHA THIS FILE WORKS AROUND (measured 2026-09-17, cost about an hour to pin down):
Windows has no native `rsync`; a common way to get one (this box: `choco install rsync`, which
installs cwrsync -- a `cygwin1.dll`-linked rsync.exe) creates TWO traps for a naive `-e ssh ...`:

  1. An absolute Windows path (a drive-letter path such as the key's) given as rsync's OWN local
     source/destination argument is misparsed as a REMOTE spec -- cygwin rsync's positional-arg
     parser sees a single letter before `:` as a hostname, not a drive letter, and dies with
     "The source and destination cannot both be remote." The fix used everywhere below: never pass
     an absolute path as rsync's positional source/dest argument -- `cwd` into the parent
     directory first and pass a plain RELATIVE path (`./<leaf>/`). This works identically on
     Linux/macOS, so it is not a Windows-only code path -- just always-safe.
  2. `-e "ssh -i <key> ..."` where `ssh` resolves to a DIFFERENT POSIX runtime than the rsync
     binary itself (Git for Windows' bundled ssh is MSYS2-linked; cwrsync's rsync is
     cygwin1.dll-linked) fails with `dup() in/out/err failed` the moment rsync tries to fork the
     transport -- the two runtimes' handle/dup emulation do not agree. The fix: prefer an `ssh`
     that lives NEXT TO the resolved `rsync` binary (same package, same runtime -- cwrsync ships
     one, at `<rsync_dir>/ssh.exe`, though chocolatey disables ITS OWN shim for it by renaming it
     `ssh.exe.ignore` -- the binary itself is untouched and runs fine invoked by path). Only fall
     back to a bare `ssh` off PATH if no colocated one exists (the normal case on Linux/macOS,
     where this whole class of bug does not exist because there is exactly one relevant runtime).
     The `-i <key>` VALUE itself does not need this treatment -- a native Windows path there works
     with either runtime, verified directly.

Usage:
  python tools/drain_reports.py                       # drain the VPS into tmp/drained_reports
  python tools/drain_reports.py --out tmp/drained_reports --json
  python tools/drain_reports.py --dry-run              # rsync -n; no local writes, no summaries
  python tools/drain_reports.py --local-source DIR     # drain from a local dir instead of the VPS
                                                        # (no ssh at all -- ops testing / offline)
  python tools/drain_reports.py --selftest             # hermetic; a lint_repo row
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import machine_config  # noqa: E402

DEFAULT_OUT = os.path.join(REPO, "tmp", "drained_reports")
MANIFEST_NAME = ".drain_manifest.json"
REMOTE_ROOT_SPEC = "/"  # bare slash = the rrsync-configured root itself (see module docstring)
# With the ADMIN-key fallback there is no rrsync forced command, so "/" is the VPS's filesystem
# root -- a 2026-09-20 drain pulled the whole VPS (etc/shadow, the swapfile, 814 MB) that way.
ADMIN_ROOT_SPEC = "/srv/reports/"
# Only `<YYYY-MM>/...` trees are reports; everything else at the source root (the collector's
# index.db, or a whole filesystem when the root is wrong) is never pulled, whatever the key.
REPORT_FILTER = ("--include=/[0-9][0-9][0-9][0-9]-[0-9][0-9]/***", "--exclude=*")


class Refusal(Exception):
    """A clean, operator-readable stop -- never a stack trace for a missing key/host/binary."""


# ─────────────────────────────── resolving the transport ───────────────────────────────


def find_rsync():
    p = shutil.which("rsync")
    if not p:
        raise Refusal(
            "no `rsync` on PATH. Windows has none built in -- install one (this box used "
            "`choco install rsync`, which gets cwrsync) and re-run. Linux/macOS: your package "
            "manager's `rsync`."
        )
    return p


def find_compatible_ssh(rsync_path):
    """An `ssh` that shares rsync's own POSIX runtime -- see the module docstring, gotcha 2.

    An explicit `MH_DRAIN_SSH` always wins (the escape hatch when none of the below guesses this
    machine's layout). Then: same directory as `rsync_path` -- true when rsync itself is the real
    binary. On Windows, `shutil.which("rsync")` more often resolves to a PACKAGE-MANAGER SHIM
    (measured 2026-09-17: chocolatey's `rsync.exe` on PATH is a ~380 KB generic shimgen redirector
    with no `ssh.exe`/`cygwin1.dll` anywhere near it) -- the REAL cygwin-linked rsync.exe (and its
    matching ssh.exe) lives under the package's own install tree, e.g. chocolatey's
    `%ChocolateyInstall%\\lib\\<pkg>\\tools\\bin\\`. So when the colocated check finds nothing, and
    `ChocolateyInstall` is set, glob that tree for a `rsync.exe` next to an `ssh.exe` and use THAT
    ssh (the arm this box's real drain needed). Bare `ssh` off PATH is the last resort -- correct
    on Linux/macOS (no runtime-mismatch class exists there), a gamble on Windows.
    """
    override = os.environ.get("MH_DRAIN_SSH")
    if override:
        return override

    def _colocated(dirpath):
        for name in ("ssh.exe", "ssh.exe.ignore", "ssh"):
            cand = os.path.join(dirpath, name)
            if os.path.isfile(cand) and os.path.getsize(cand) > 0:
                return cand
        return None

    d = os.path.dirname(rsync_path)
    cand = _colocated(d)
    if cand:
        return cand

    choco = os.environ.get("ChocolateyInstall")
    if choco:
        import glob

        for real_rsync in glob.glob(os.path.join(choco, "lib", "*", "tools", "bin", "rsync.exe")):
            cand = _colocated(os.path.dirname(real_rsync))
            if cand:
                return cand

    found = shutil.which("ssh")
    if not found:
        raise Refusal("no `ssh` on PATH (and none colocated with rsync at %s)." % d)
    return found


def drain_key_path():
    """Returns (key_path, is_admin_fallback)."""
    key = os.environ.get("MH_DRAIN_KEY")
    is_admin = not key
    key = key or machine_config.VPS_SSH_KEY
    if not key:
        raise Refusal(
            "no drain key configured. Set MH_DRAIN_KEY to the dedicated read-only key's path "
            "(restrict,command=\"rrsync -ro /srv/reports\" on the VPS -- see src/collector/"
            "README.md 'Draining reports'), or VPS_SSH_KEY in tools/machine.local.json as a "
            "fallback for a quick smoke test with the admin key."
        )
    if not os.path.isfile(key):
        raise Refusal("drain key does not exist on disk: %s" % key)
    return key, is_admin


def vps_host():
    host = machine_config.VPS_HOST
    if not host:
        raise Refusal(
            "no VPS configured (machine_config.VPS_HOST is empty). Put VPS_HOST in the gitignored "
            "tools/machine.local.json, or set MH_VPS_HOST -- refusing rather than guessing, same "
            "as tools/mh_tunnel.ps1."
        )
    return host


def _rsh_command(ssh_exe, key_path):
    # rsync's --rsh value gets a small shell-word split on its own side, so simple double-quoting
    # is enough to survive a space in either path (a VS-style install root, say).
    return '"%s" -i "%s" -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=15' % (
        ssh_exe,
        key_path,
    )


# ─────────────────────────────── the pull itself ───────────────────────────────


def rsync_pull(rsync_exe, source_spec, local_dir, rsh=None, dry_run=False, extra_args=()):
    """One rsync invocation. `local_dir` is ALWAYS passed to rsync as a RELATIVE path (gotcha 1 in
    the module docstring). When `rsh` is None, `source_spec` is a LOCAL directory too (the
    `--local-source` / selftest path) and gets the exact same relative-path treatment -- the
    drive-letter misparse does not care which side of the transfer the absolute path is on. A
    remote spec (`rsh` given, `source_spec` is `user@host:/...`) is passed through untouched; it
    has no Windows drive letter to misparse.

    Returns (returncode, stdout, stderr, files_transferred). `files_transferred` is parsed out of
    rsync's own --stats block, which is the ground truth for "did anything actually move" --
    independent of whatever our own manifest bookkeeping concludes.
    """
    local_dir = os.path.abspath(local_dir)
    os.makedirs(local_dir, exist_ok=True)
    parent = os.path.dirname(local_dir.rstrip("\\/")) or "."
    leaf = os.path.basename(local_dir.rstrip("\\/"))

    if rsh is None:
        # Local-to-local: run from a cwd that can reach BOTH sides by a relative path. `parent`
        # (local_dir's own parent) qualifies as long as source_spec doesn't escape onto another
        # drive -- true for the selftest (both live under one tempfile.mkdtemp() root) and for any
        # sane `--local-source`. A cross-drive local source is not a real deployment shape (the
        # VPS path never hits this branch), so it is left to fail loudly rather than special-cased.
        src_rel = os.path.relpath(os.path.abspath(source_spec.rstrip("\\/")), parent)
        source_arg = "./%s/" % src_rel.replace("\\", "/")
    else:
        source_arg = source_spec

    argv = [rsync_exe, "-a", "--itemize-changes", "--stats", "--timeout=60"]
    if dry_run:
        argv.append("-n")
    argv.extend(REPORT_FILTER)
    argv.extend(extra_args)
    if rsh is not None:
        argv.extend(["-e", rsh])
    argv.append(source_arg)
    argv.append("./%s/" % leaf)
    proc = subprocess.run(argv, cwd=parent, capture_output=True, text=True)
    files_transferred = None
    for line in (proc.stdout or "").splitlines():
        if line.strip().startswith("Number of regular files transferred:"):
            try:
                files_transferred = int(line.split(":", 1)[1].strip().replace(",", ""))
            except ValueError:
                pass
    return proc.returncode, proc.stdout, proc.stderr, files_transferred


# ─────────────────────────────── the local store + manifest ───────────────────────────────


def load_manifest(out_dir):
    p = os.path.join(out_dir, MANIFEST_NAME)
    if not os.path.isfile(p):
        return {}
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_manifest(out_dir, manifest):
    p = os.path.join(out_dir, MANIFEST_NAME)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    os.replace(tmp, p)


def find_report_dirs(out_dir):
    """Every `<out_dir>/<YYYY-MM>/<ulid>/` that looks like a complete report -- has meta.json.

    A directory with report.zip but no meta.json YET is a report still mid-transfer (the collector
    writes report.zip and description.txt before meta.json -- see src/collector/collector/
    app.py's `_store_report`) or, on our side, a drain that landed only part of a report because
    the source directory itself was still being written server-side. Either way it is NOT ready to
    summarize; the NEXT drain will pick up the rest once it exists, and this function will find it
    then. Silently skipping an incomplete directory is deliberate, not a bug: printing a summary
    for a report that turns out to have no meta.json is worse than a one-run delay.
    """
    out = []
    if not os.path.isdir(out_dir):
        return out
    for month in sorted(os.listdir(out_dir)):
        mdir = os.path.join(out_dir, month)
        if not os.path.isdir(mdir) or not _looks_like_month(month):
            continue
        for ulid in sorted(os.listdir(mdir)):
            rdir = os.path.join(mdir, ulid)
            if os.path.isfile(os.path.join(rdir, "meta.json")):
                out.append((ulid, rdir))
    return out


def _looks_like_month(name):
    return len(name) == 7 and name[4] == "-" and name[:4].isdigit() and name[5:].isdigit()


def read_meta(rdir):
    with open(os.path.join(rdir, "meta.json"), encoding="utf-8") as f:
        return json.load(f)


def read_description_head(rdir, n=60):
    p = os.path.join(rdir, "description.txt")
    if not os.path.isfile(p):
        return ""
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            head = f.read(n + 1).replace("\n", " ").strip()
    except OSError:
        return ""
    return (head[:n] + "...") if len(head) > n else head


def summary_line(ulid, meta, desc_head):
    """RP2's README contract: match_id, version, exit code, description head."""
    crash = meta.get("crash") or {}
    fault = ""
    if crash.get("module"):
        fault = "  fault=%s+%s" % (crash.get("module"), crash.get("offset", "?"))
    return "[report] ulid=%s match_id=%s version=%s exit_code=%s%s  desc: %s" % (
        ulid,
        meta.get("match_id", "?"),
        meta.get("version", "?"),
        meta.get("exit_code", "?"),
        fault,
        desc_head or "(no description)",
    )


def summarize_new(out_dir, manifest, say=print):
    """Print + record one summary line per report NOT already in the manifest. Returns the list of
    ulids newly summarized this call -- the thing "pulls each report exactly once" is proven by."""
    new_ulids = []
    for ulid, rdir in find_report_dirs(out_dir):
        if ulid in manifest:
            continue
        try:
            meta = read_meta(rdir)
        except (OSError, ValueError):
            continue  # meta.json exists but is not valid JSON yet (a torn write); try next drain
        say(summary_line(ulid, meta, read_description_head(rdir)))
        manifest[ulid] = {"drained_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        new_ulids.append(ulid)
    return new_ulids


# ─────────────────────────────── orchestration ───────────────────────────────


def drain(
    out_dir,
    local_source=None,
    host=None,
    key=None,
    dry_run=False,
    say=print,
):
    """One full drain: resolve the transport, rsync-pull, summarize what's new. Returns a dict
    (JSON-serializable) describing what happened -- used by both --json and the selftest."""
    rsync_exe = find_rsync()
    if local_source is not None:
        source_spec = local_source.rstrip("\\/") + os.sep
        rsh = None
    else:
        host = host or vps_host()
        is_admin = False
        if not key:
            key, is_admin = drain_key_path()
        if is_admin:
            print(
                "drain_reports: WARNING -- MH_DRAIN_KEY unset, using the ADMIN key; draining %s"
                % ADMIN_ROOT_SPEC,
                file=sys.stderr,
            )
        ssh_exe = find_compatible_ssh(rsync_exe)
        rsh = _rsh_command(ssh_exe, key)
        source_spec = "%s:%s" % (host, ADMIN_ROOT_SPEC if is_admin else REMOTE_ROOT_SPEC)

    rc, out, err, files_transferred = rsync_pull(
        rsync_exe, source_spec, out_dir, rsh=rsh, dry_run=dry_run
    )
    if rc != 0:
        raise Refusal(
            "rsync exited %d pulling from %s\n--- stdout ---\n%s\n--- stderr ---\n%s"
            % (rc, source_spec, out.strip(), err.strip())
        )

    manifest = load_manifest(out_dir)
    new_ulids = [] if dry_run else summarize_new(out_dir, manifest, say=say)
    if not dry_run:
        save_manifest(out_dir, manifest)

    if not new_ulids and not dry_run:
        say(
            "drain_reports: nothing new (%s file(s) transferred by rsync)"
            % (files_transferred or 0)
        )

    return {
        "source": source_spec,
        "out_dir": out_dir,
        "rsync_files_transferred": files_transferred,
        "new_reports": new_ulids,
        "dry_run": dry_run,
    }


# ─────────────────────────────── selftest ───────────────────────────────


def _write_fixture_report(
    root, month, ulid, meta, description, zip_bytes=b"PK\x05\x06" + b"\x00" * 18
):
    d = os.path.join(root, month, ulid)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "report.zip"), "wb") as f:
        f.write(zip_bytes)
    with open(os.path.join(d, "description.txt"), "w", encoding="utf-8") as f:
        f.write(description)
    with open(os.path.join(d, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)


def selftest():
    import tempfile

    fails = []
    total = [0]

    def ck(name, ok):
        total[0] += 1
        print("  %-64s %s" % (name, "ok" if ok else "FAIL"))
        if not ok:
            fails.append(name)

    # ---- pure functions, no transport at all --------------------------------------------
    ck(
        "summary_line carries match_id/version/exit_code/description head",
        summary_line(
            "01ULID",
            {"match_id": "abc123", "version": "1.2.3", "exit_code": -1073741819},
            "it crashed",
        )
        == "[report] ulid=01ULID match_id=abc123 version=1.2.3 exit_code=-1073741819  "
        "desc: it crashed",
    )
    ck(
        "summary_line surfaces a crash field when present",
        "fault=mh.dll+0x175b0"
        in summary_line(
            "01ULID",
            {"match_id": "x", "crash": {"module": "mh.dll", "offset": "0x175b0"}},
            "",
        ),
    )
    ck("_looks_like_month accepts YYYY-MM", _looks_like_month("2026-09"))
    ck("_looks_like_month rejects the manifest file", not _looks_like_month(MANIFEST_NAME))

    # ---- refusals: no stack trace, a clear message ---------------------------------------
    old_host, old_key = machine_config.VPS_HOST, machine_config.VPS_SSH_KEY
    old_env = os.environ.pop("MH_DRAIN_KEY", None)
    try:
        machine_config.VPS_HOST = ""
        try:
            vps_host()
            ck("empty VPS_HOST refuses", False)
        except Refusal:
            ck("empty VPS_HOST refuses", True)
        machine_config.VPS_HOST = old_host
        machine_config.VPS_SSH_KEY = ""
        try:
            drain_key_path()
            ck("no drain key configured refuses", False)
        except Refusal:
            ck("no drain key configured refuses", True)
    finally:
        machine_config.VPS_HOST, machine_config.VPS_SSH_KEY = old_host, old_key
        if old_env is not None:
            os.environ["MH_DRAIN_KEY"] = old_env

    ssh_probe_dir = tempfile.mkdtemp(prefix="drainssh_")
    try:
        fake_rsync = os.path.join(ssh_probe_dir, "rsync")
        fake_ssh = os.path.join(ssh_probe_dir, "ssh.exe.ignore")  # chocolatey-disabled-shim shape
        with open(fake_rsync, "w") as f:
            f.write("x")
        with open(fake_ssh, "w") as f:
            f.write("not empty")  # find_compatible_ssh skips zero-byte (choco ignore marker) files
        ck(
            "find_compatible_ssh prefers a colocated ssh over PATH",
            find_compatible_ssh(fake_rsync) == fake_ssh,
        )
        with open(fake_ssh, "w"):
            pass  # truncate to zero bytes -- the choco .ignore-marker shape must be skipped
        found_or_none = None
        try:
            found_or_none = find_compatible_ssh(fake_rsync)
        except Refusal:
            pass
        ck(
            "find_compatible_ssh skips a zero-byte colocated file",
            found_or_none != fake_ssh,
        )
    finally:
        shutil.rmtree(ssh_probe_dir, ignore_errors=True)

    # ---- the offline drain: local directory standing in for the VPS ----------------------
    # No network, no ssh, no VPS -- `local_source=` skips the rsh entirely (see drain()), so this
    # exercises exactly the rsync-pull + manifest + summarize machinery the real drain shares.
    rsync_exe = None
    try:
        rsync_exe = find_rsync()
    except Refusal as exc:
        print("  (skipping the rsync-backed half: %s)" % exc)

    if rsync_exe:
        tmp = tempfile.mkdtemp(prefix="drainsel_")
        try:
            remote = os.path.join(tmp, "remote_reports")
            local = os.path.join(tmp, "local_store")
            _write_fixture_report(
                remote,
                "2026-09",
                "01AAAAAAAAAAAAAAAAAAAAAAAA",
                {"match_id": "m1", "version": "1.0", "exit_code": 0},
                "clean run",
            )
            _write_fixture_report(
                remote,
                "2026-09",
                "01BBBBBBBBBBBBBBBBBBBBBBBB",
                {
                    "match_id": "m2",
                    "version": "1.0",
                    "exit_code": -1073741819,
                    "crash": {"module": "mh.dll", "offset": "0x175b0"},
                },
                "deliberate fault",
            )
            # An INCOMPLETE report -- report.zip written, meta.json not yet (mid atomic-write on
            # the source side). Must never be summarized.
            incomplete = os.path.join(remote, "2026-09", "01CCCCCCCCCCCCCCCCCCCCCCCC")
            os.makedirs(incomplete, exist_ok=True)
            with open(os.path.join(incomplete, "report.zip"), "wb") as f:
                f.write(b"not-a-real-zip-yet")
            # Non-report entries at the source root -- the collector's index.db, or a whole
            # filesystem when the root is wrong (the 2026-09-20 admin-key drain). Never pulled.
            os.makedirs(os.path.join(remote, "etc"), exist_ok=True)
            with open(os.path.join(remote, "etc", "shadow"), "w") as f:
                f.write("root:x")
            with open(os.path.join(remote, "index.db"), "wb") as f:
                f.write(b"db")

            printed1 = []
            result1 = drain(local, local_source=remote, say=printed1.append)
            ck(
                "run 1: both complete reports are new",
                sorted(result1["new_reports"])
                == [
                    "01AAAAAAAAAAAAAAAAAAAAAAAA",
                    "01BBBBBBBBBBBBBBBBBBBBBBBB",
                ],
            )
            ck(
                "run 1: incomplete report NOT summarized",
                "01CCCCCCCCCCCCCCCCCCCCCCCC" not in result1["new_reports"],
            )
            ck(
                "run 1: rsync itself transferred files",
                (result1["rsync_files_transferred"] or 0) > 0,
            )
            ck("run 1: printed one summary line per new report", len(printed1) == 2)
            ck(
                "run 1: nothing outside <YYYY-MM>/ is pulled",
                sorted(os.listdir(local)) == [MANIFEST_NAME, "2026-09"],
            )
            ck(
                "run 1: the crash report's summary carries match_id and the fault field",
                any("match_id=m2" in ln and "fault=mh.dll+0x175b0" in ln for ln in printed1),
            )

            printed2 = []
            result2 = drain(local, local_source=remote, say=printed2.append)
            ck(
                "run 2 (consecutive, nothing changed): pulls NO new report a second time",
                result2["new_reports"] == [],
            )
            ck(
                "run 2: rsync itself transferred zero new files -- pulled exactly once",
                result2["rsync_files_transferred"] in (0, None),
            )

            # Now the source gains ONE more report -- run 3 must see exactly that one, not the
            # two it already summarized.
            _write_fixture_report(
                remote,
                "2026-09",
                "01DDDDDDDDDDDDDDDDDDDDDDDD",
                {"match_id": "m3", "version": "1.0", "exit_code": 0},
                "later report",
            )
            result3 = drain(local, local_source=remote, say=lambda *_: None)
            ck(
                "run 3: exactly the one genuinely-new report",
                result3["new_reports"] == ["01DDDDDDDDDDDDDDDDDDDDDDDD"],
            )

            ck(
                "the manifest persisted to disk between runs",
                os.path.isfile(os.path.join(local, MANIFEST_NAME)),
            )

            # --dry-run must not write the manifest or print summaries, even on a fresh store.
            dry_local = os.path.join(tmp, "dry_store")
            printed_dry = []
            result_dry = drain(dry_local, local_source=remote, dry_run=True, say=printed_dry.append)
            ck("--dry-run reports no new_reports", result_dry["new_reports"] == [])
            ck("--dry-run prints nothing", printed_dry == [])
            ck(
                "--dry-run never writes a manifest",
                not os.path.isfile(os.path.join(dry_local, MANIFEST_NAME)),
            )
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print("drain_reports selftest: %d check(s), %d failure(s)" % (total[0], len(fails)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument(
        "--out", default=DEFAULT_OUT, help="local gitignored store (default: tmp/drained_reports)"
    )
    ap.add_argument(
        "--local-source", help="drain from this local directory instead of the VPS (no ssh)"
    )
    ap.add_argument(
        "--dry-run", action="store_true", help="rsync -n; no local writes, no summaries"
    )
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="hermetic; no VPS, no network")
    a = ap.parse_args()
    # A player's description is arbitrary text (Cyrillic, emoji); on a cp1252 console a strict
    # print of it raised mid-summarize, before save_manifest -- so every later drain died too.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass

    if a.selftest:
        return selftest()

    try:
        result = drain(a.out, local_source=a.local_source, dry_run=a.dry_run)
    except Refusal as exc:
        print("drain_reports: REFUSED -- %s" % exc, file=sys.stderr)
        return 2

    if a.json:
        print(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
