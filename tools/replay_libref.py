#!/usr/bin/env python3
# replay_libref.py -- fork F4G: CONFIGURATION (3) AS A GATE UNIT.
#
# WHAT WAS MISSING. The fork plan ships three configurations and F2's done_when says the
# standalone replay fixtures "still verify 5000/5000" -- but nothing ran them. tools/fixture_replay.py
# is invoked by no tool and no gate unit (its `replay` verb drives the GAME, not the standalone
# host), run_gate's units neither built nor ran libref_host, and config (3)'s 5000/5000 was a hand
# check somebody remembered to do at F4D. This is the unit that runs it.
#
# THE SUBJECT IS THE DLL, AND THAT IS ASSERTED RATHER THAN ASSUMED. Since fork F4G the host imports
# `Release\standalone\libmh.dll` (libmh_std.vcxproj -- the STANDALONE arm of the roster, linked
# whole) instead of folding the archive into itself, so "libmh + a host, no game binary" is measured
# against a real dynamic library. A build that quietly went back to static linking would replay just
# as green, so this tool reads libref_host.exe's IMPORT TABLE and fails if libmh.dll is not in it.
#
# WHAT IT RUNS. Every committed fixture under tools/data/fixtures (discovered, never listed), each
# materialised from its compressed, sha-checked committed form into a scratch directory, replayed to
# its full declared step count, and required to report ALL STEPS IDENTICAL with exit 0.
#
# ---- THE ASSET QUESTION, WHICH IS WHY THIS DID NOT SIMPLY WORK ---------------------------------
#
# All three fixtures replayed 5000/5000 ALL STEPS IDENTICAL and then FAILED, on an asset_read trap:
# the landing sequence asks for `init\H_3100.DMP`..`H_3107.DMP` (the AI players' scripted base
# layouts) and a host with no extracted bank tree refuses rather than inventing an answer. MEASURED:
# those files ARE NOT IN THE RETAIL BANK -- mh.nam carries 112 `init\H_*.DMP` entries and the highest
# is H_2507 -- so the game misses them too and the fixtures need NO assets at all. The trap was
# firing on a question the host could not answer from nothing, not on a gap.
#
# So the gate runs the ASSET-FREE arm with a DECLARATION (tools/data/libref_asset_dispositions.json)
# naming exactly the paths measured absent, and this tool RE-VERIFIES that declaration against a real
# .nam index on any box that has the game -- refusing if a declared-absent name turns out to be
# present. No game data is committed: the declaration is eight names and a measurement, and the
# verification reads the bank in place. On a box without the game the declaration is carried and the
# run says so.
#
# ---- MODES --------------------------------------------------------------------------------------
#
#   python tools/replay_libref.py              every committed fixture (the run_gate unit)
#   python tools/replay_libref.py --fixture libref-replay-v1
#   python tools/replay_libref.py --selftest   planted inputs; every negative goes red
#
# Exit 0 = every fixture replayed identically. 1 = a replay failed. 2 = the run cannot be trusted
# (no build, no fixture, a false declaration). NEVER read a 2 as "config (3) is green".

from __future__ import annotations

import argparse
import io
import json
import os
import re
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

FIXTURE_ROOT = os.path.join(REPO, "tools", "data", "fixtures")
DISPOSITIONS = os.path.join(REPO, "tools", "data", "libref_asset_dispositions.json")
STANDALONE = os.path.join(REPO, "src", "mh_dll", "Release", "standalone")
HOST_EXE = os.path.join(STANDALONE, "libref_host.exe")
SPINE_DLL = os.path.join(STANDALONE, "libmh.dll")
WORK = os.path.join(REPO, "tmp", "libref")

# The manifest's artifact keys -> the file name the host reads them back from. The host resolves all
# five beside the --fixture directory (see libref_host/main.cpp), which is why this tool materialises
# a directory rather than handing it the committed compressed form.
LANE_FILES = {
    "orders": "mh_orders.bin",
    "clock": "mh_clock.bin",
    "world": "mh_world.bin",
    "hash_steps": "hash_steps.txt",
    "hash_regions": "hash_regions.txt",
}

IDENTICAL_RE = re.compile(r"state-hash mismatches: 0 \(ALL STEPS IDENTICAL\)")
REPLAYED_RE = re.compile(r"replayed (\d+) step\(s\) in ([\d.]+) s")
# The host's own refusal line for a blob stamped with another manifest (libref_host/main.cpp). It was
# always there; until TL-GATE-D25FX it sat one line deep in a 5000-line log under a "no ALL STEPS
# IDENTICAL line" headline, and the first gate after D25 read three stale fixtures as three broken
# replays. Now it is the headline, with both fingerprints.
STALE_RE = re.compile(r"captured under a DIFFERENT hash (manifest|_?sink)")


class Refusal(Exception):
    """Exit 2: this run cannot be trusted."""


# ---------------------------------------------------------------------------------------------
# the declaration, and its falsifier
# ---------------------------------------------------------------------------------------------
def load_dispositions():
    d = json.load(io.open(DISPOSITIONS, encoding="utf-8"))
    names = [r["name"] for r in d["absent_from_bank"]]
    if not names:
        raise Refusal(
            "%s declares nothing -- an empty list cannot be what was measured" % DISPOSITIONS
        )
    return d, names


def bank_names(bank_dir):
    """Every filename in the retail packs' .nam index. The index is a table of 64-byte entries
    {filename[47], type[5], offset, size, final_size} (src/formats/unpack.py) -- names only, and
    nothing is decompressed, so this reads the bank without extracting a byte of it."""
    out = set()
    found = False
    for pack in ("mh", "mh_ex"):
        p = os.path.join(bank_dir, pack + ".nam")
        if not os.path.isfile(p):
            continue
        found = True
        with open(p, "rb") as f:
            while True:
                d = f.read(64)
                if len(d) < 64:
                    break
                r = struct.unpack("<47s5s3I", d)
                out.add(r[0].decode("latin1").split("\x00")[0].replace("\\", "/").lower())
    return out if found else None


def verify_dispositions(names):
    """-> a one-line verdict. REFUSES if a declared-absent name is really in the bank: that is the
    one direction in which the declaration can be caught lying, and it is the direction that would
    make the gate answer a MISS for an asset the sim should have read."""
    try:
        import machine_config as machine

        bank_dir = machine.POLYGON
    except Exception:  # noqa: BLE001 -- a box without machine_config still runs the replay
        return "declaration CARRIED (no machine_config: the bank could not be located)"
    have = bank_names(bank_dir)
    if have is None:
        return "declaration CARRIED (no mh.nam under %s: this box has no game copy)" % bank_dir
    bad = [n for n in names if n.replace("\\", "/").lower() in have]
    if bad:
        raise Refusal(
            "THE DECLARATION IS FALSE. %s says the bank does not contain %s, and %s's index does. "
            "Re-derive tools/data/libref_asset_dispositions.json."
            % (os.path.basename(DISPOSITIONS), ", ".join(bad), bank_dir)
        )
    return "declaration RE-VERIFIED against %s (%d bank entries, %d declared absent, 0 present)" % (
        bank_dir,
        len(have),
        len(names),
    )


# ---------------------------------------------------------------------------------------------
# the fixtures
# ---------------------------------------------------------------------------------------------
def fixtures():
    """DISCOVERED, never listed: every directory under tools/data/fixtures with a manifest. A hand
    list is how a fixture gets added and silently not gated."""
    # The directory MISSING ENTIRELY is a real case, not a broken checkout: fork F5C measured it by
    # materialising the publish set without the Q5-pending fixtures, and os.listdir raised
    # FileNotFoundError -- a traceback and exit 1, which reads like a tool bug rather than the
    # declared absence it is. Refusal (exit 2, "the run cannot be trusted") is the verdict this tool
    # already has for "no fixtures"; a missing root is the same verdict, one directory earlier.
    if not os.path.isdir(FIXTURE_ROOT):
        raise Refusal("no fixture directory at %s" % FIXTURE_ROOT)
    out = []
    for name in sorted(os.listdir(FIXTURE_ROOT)):
        d = os.path.join(FIXTURE_ROOT, name)
        if os.path.isfile(os.path.join(d, "manifest.json")):
            out.append(name)
    if not out:
        raise Refusal("no fixtures under %s" % FIXTURE_ROOT)
    return out


def materialise(name, out_dir):
    """Unpack ONE committed fixture into a directory the host can read, checking every artifact
    against the sha the manifest records. Returns (manifest, is_live).

    THE READERS ARE fixture_replay's, NOT COPIES OF THEM. That tool owns the committed form -- it
    is what PACKED these fixtures, and its `_read_z`/`sha256`/`load_manifest` are the same three
    functions its own `unpack` and `integrity` use. Re-implementing fifteen lines of zlib and sha
    here would have made a second reader of a committed format, which is the drift this repo keeps
    finding; and it would have left R4's actual complaint standing, which is that fixture_replay.py
    is reached by no tool and no gate unit. It is reached by this one now."""
    import fixture_replay as fr

    src = os.path.join(FIXTURE_ROOT, name)
    man = fr.load_manifest(src)
    if ("replay_contract" in man) == ("live_contract" in man):
        raise Refusal("%s declares both contracts or neither -- it describes no arrangement" % name)
    os.makedirs(out_dir, exist_ok=True)
    for key, a in man["artifacts"].items():
        if key not in LANE_FILES:
            raise Refusal("%s: artifact %r has no file name in this tool's table" % (name, key))
        raw = fr._read_z(os.path.join(src, a["file"]))
        if fr.sha256(raw) != a["sha256_raw"]:
            raise Refusal("%s: %s decompresses to the wrong bytes (sha mismatch)" % (name, key))
        with open(os.path.join(out_dir, LANE_FILES[key]), "wb") as fh:
            fh.write(raw)
    return man, "live_contract" in man


# ---------------------------------------------------------------------------------------------
# the build the unit measures
# ---------------------------------------------------------------------------------------------
def imports_of(pe):
    from check_module_bind import find_dumpbin

    db = find_dumpbin()
    if not db:
        raise Refusal("no dumpbin.exe under machine_config.VS_INSTALL_ROOT")
    r = subprocess.run([db, "/imports", pe], capture_output=True, text=True)
    return {m.group(1).lower() for m in re.finditer(r"^\s{4}(\S+\.dll)\s*$", r.stdout, re.M)}


def check_build():
    """The two facts that make this unit's subject what it claims to be."""
    for p, what in ((HOST_EXE, "libref_host.exe"), (SPINE_DLL, "the standalone libmh.dll")):
        if not os.path.isfile(p):
            raise Refusal(
                "%s is missing (%s). Build Release|x86 first -- a config (3) gate with no config (3) "
                "deployment measures nothing." % (p, what)
            )
    imp = imports_of(HOST_EXE)
    if "libmh.dll" not in imp:
        raise Refusal(
            "libref_host.exe does NOT import libmh.dll (imports: %s). The spine is folded into the "
            "host, so this unit would be replaying an ARCHIVE and calling it a DLL -- which is the "
            "one thing F4G's done_when asks it to distinguish." % (", ".join(sorted(imp)) or "none")
        )
    return "host imports %s" % ", ".join(sorted(imp))


# ---------------------------------------------------------------------------------------------
# the manifest stamp (TL-GATE-D25FX)
# ---------------------------------------------------------------------------------------------
def current_manifest_fp():
    """The hash-manifest fingerprint the DLL is built with, from the generated header -- None if
    the analyzer cannot compute it (then the host's own refusal line still decides)."""
    try:
        import mp_analyze as _m

        return _m.hash_manifest_fingerprint()
    except Exception:  # noqa: BLE001 -- a missing header must not turn into a crash here
        return None


def stale_fp(man):
    """True when the fixture's stamped manifest fingerprint differs from the current build's."""
    cur = current_manifest_fp()
    fp = (man.get("step0") or {}).get("hash_manifest_fp")
    return bool(cur and fp and fp.upper() != cur)


# ---------------------------------------------------------------------------------------------
# the run
# ---------------------------------------------------------------------------------------------
def run_one(name, absent_file, timeout):
    lane = os.path.join(WORK, name)
    man, live = materialise(name, lane)
    steps = int(man["steps"])
    argv = [HOST_EXE, "--fixture", lane, "--assets-absent", absent_file]
    if live:
        argv.append("--live")
    log = os.path.join(WORK, name + ".log")
    t0 = time.time()
    # KILLED AT THE BOUND, NEVER WAITED OUT PAST IT -- run_gate's own rule one level up. A replay
    # that hung would otherwise sit until the UNIT's timeout with nothing said about which fixture,
    # and the log is unbuffered so the last line before the hang is already on disk.
    timed_out = False
    with open(log, "w", encoding="utf-8") as fh:
        proc = subprocess.Popen(argv, stdout=fh, stderr=subprocess.STDOUT)
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            rc, timed_out = -1, True
    secs = time.time() - t0
    text = io.open(log, encoding="utf-8", errors="replace").read()

    problems = []
    if timed_out:
        problems.append("TIMEOUT after %ds" % timeout)
    elif rc != 0:
        problems.append("exit %d" % rc)
    if STALE_RE.search(text) or stale_fp(man):
        # STALE, NOT WRONG -- said first, because every number below it is incomparable rather than
        # a divergence. The fixture's own stamp and the current build's fingerprint are both named
        # so the reader sees which side moved.
        problems.append(
            "STALE FIXTURE: captured under hash manifest %s, current %s -- re-capture it (%s/README.md) "
            "in the session that changed the manifest"
            % (
                (man.get("step0") or {}).get("hash_manifest_fp", "?"),
                current_manifest_fp() or "?",
                os.path.relpath(os.path.join(FIXTURE_ROOT, "libref-replay-v1"), REPO).replace(
                    "\\", "/"
                ),
            )
        )
    if not IDENTICAL_RE.search(text):
        problems.append("no ALL STEPS IDENTICAL line")
    m = REPLAYED_RE.search(text)
    if not m:
        problems.append("no `replayed N step(s)` line")
    elif int(m.group(1)) != steps:
        # A SHORT RUN THAT AGREES WITH ITSELF IS NOT A PASS. The fixture declares its step count and
        # the whole claim is over that count; a replay that stopped early would report ALL STEPS
        # IDENTICAL about however few steps it managed.
        problems.append("replayed %s of %d declared step(s)" % (m.group(1), steps))
    return {
        "name": name,
        "live": live,
        "steps": steps,
        "secs": secs,
        "log": log,
        "problems": problems,
        "tail": [ln for ln in text.splitlines()[-14:]],
    }


def selftest():
    """Planted inputs for the two judgments this tool makes that are not a subprocess's exit code:
    the declaration's falsifier and the short-run rule."""
    bad = 0

    def case(label, ok):
        nonlocal bad
        print("  %-4s %s" % ("ok" if ok else "FAIL", label))
        bad += not ok

    d, names = load_dispositions()
    case("the committed declaration parses and is non-empty", len(names) >= 1)
    case(
        "every declared name is a bank-shaped relative path",
        all(("\\" in n or "/" in n) and not os.path.isabs(n) for n in names),
    )

    real = globals()["bank_names"]
    globals()["bank_names"] = lambda _d: {names[0].replace("\\", "/").lower()}
    try:
        verify_dispositions(names)
        caught = False
    except Refusal:
        caught = True
    globals()["bank_names"] = real
    case("a declared-absent name that IS in the bank REFUSES", caught)

    globals()["bank_names"] = lambda _d: {"fnt/fonty08.fnt"}
    try:
        verify_dispositions(names)
        ok = True
    except Refusal:
        ok = False
    globals()["bank_names"] = real
    case("a bank that has none of them verifies", ok)

    globals()["bank_names"] = lambda _d: None
    carried = "CARRIED" in verify_dispositions(names)
    globals()["bank_names"] = real
    case("a box with no game copy CARRIES the declaration rather than passing silently", carried)

    # The short-run rule, checked on the parse rather than by running a 20 s replay.
    text = "replayed 12 step(s) in 0.10 s\n  state-hash mismatches: 0 (ALL STEPS IDENTICAL)\n"
    m = REPLAYED_RE.search(text)
    case(
        "a short run is detected by the declared-step-count compare",
        bool(m) and int(m.group(1)) != 5000,
    )
    case("the identity line is recognised", bool(IDENTICAL_RE.search(text)))
    case(
        "the host's stale-manifest refusal is recognised as STALE",
        bool(
            STALE_RE.search(
                "  FAIL: the blob was captured under a DIFFERENT hash manifest -- stale fixture"
            )
        ),
    )
    case(
        "a fixture stamped with another fingerprint reads as stale",
        stale_fp({"step0": {"hash_manifest_fp": "DEADBEEF"}}) is True,
    )
    case(
        "a fixture stamped with the current fingerprint does not",
        stale_fp({"step0": {"hash_manifest_fp": current_manifest_fp() or "DEADBEEF"}}) is False,
    )

    case("every committed fixture is discoverable", len(fixtures()) >= 1)
    print("[replay_libref] selftest: %s" % ("PASS" if not bad else "%d FAILED" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description="fork F4G: replay the committed standalone fixtures against libmh.dll"
    )
    ap.add_argument("--fixture", action="append", help="run only this fixture (repeatable)")
    ap.add_argument("--timeout", type=int, default=900, help="per-fixture kill bound (s)")
    ap.add_argument("--selftest", action="store_true", help="planted inputs")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    try:
        os.makedirs(WORK, exist_ok=True)
        print("[libref] %s" % check_build())
        d, names = load_dispositions()
        print("[libref] %s" % verify_dispositions(names))
        absent_file = os.path.join(WORK, "assets_absent.txt")
        io.open(absent_file, "w", encoding="utf-8", newline="\n").write(
            "# GENERATED by tools/replay_libref.py from tools/data/libref_asset_dispositions.json\n"
            + "".join(n + "\n" for n in names)
        )
        want = args.fixture or fixtures()
        for f in want:
            if not os.path.isdir(os.path.join(FIXTURE_ROOT, f)):
                raise Refusal("no fixture named %r under %s" % (f, FIXTURE_ROOT))
        results = [run_one(f, absent_file, args.timeout) for f in want]
    except Refusal as e:
        print("[libref] REFUSED (exit 2): %s" % e)
        return 2

    print("\n" + "=" * 78)
    print("CONFIGURATION (3): the standalone fixtures against libmh.dll")
    print("-" * 78)
    bad = 0
    for r in results:
        verdict = "PASS" if not r["problems"] else "FAIL"
        bad += verdict != "PASS"
        print(
            "  %-28s %-5s %-4s %5d steps  %6.1fs   %s"
            % (
                r["name"],
                "live" if r["live"] else "replay",
                verdict,
                r["steps"],
                r["secs"],
                os.path.relpath(r["log"], REPO),
            )
        )
        if r["problems"]:
            print("      %s" % "; ".join(r["problems"]))
            for ln in r["tail"]:
                print("      | " + ln)
    print("-" * 78)
    print("  %s (%d fixture(s))" % ("PASS" if not bad else "%d FAILED" % bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
