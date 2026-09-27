#!/usr/bin/env python3
"""check_d36_rematch_seed.py -- mp:D36: A HOST'S REMATCH MUST ENTER FROM THE SAME STATE AS A FRESH
PROCESS, proven by diffing the two peers' match-start seeds rather than by the pixels.

THE BUG THIS GATES (rc4 field match 01a0deb9, 2026-09-26). A second match in one host process
entered `llm_strat_session_begin_multi` with sim state retail never re-initialises between matches,
because a fresh process gets it from zeroed .bss:
  * game::player_data[8] -- `resource_spent[4]` (+0x10054) is a gains-only counter the mother-ship
    landing books +5400/4000/3000/2000 into, and nothing clears it for a HUMAN player
    (llm_strat_init_human_player_data resets a dozen AI scalars; the only wide zeroing is in the AI
    spawn paths). The host carried match 1's grant, the fresh joiner had zeros:
    `DESYNC step=50 first_region=8 p0_ai_econ`.
  * Planets[31], the reserved scenario planet -- its string slots keep the previous (longer) map
    name's tail past the new NUL (`planets` +0x81da).
The fix (mh.dll launch.cpp `mp_clear_match_residue`, both entry preps) zeroes player_data and the
Planets[31] string slots before begin_map_load and logs how many bytes it found non-zero.

WHAT IT ASSERTS (the d36_rematch_seed row: host lands in match 1 on Blue Monday, a FRESH client
process joins its match 2 on the shorter-named Cold War):
  1. the host process holds EXACTLY two session directories and the fresh client's process EXACTLY
     one, each with an `mh_match_seed.bin` (the harness's step-1 dump) of the manifest's size;
  2. the `; D36: match-entry residue ...` line: the host logged two entries, the client one; every
     FIRST-in-process entry measured nonzero=0 for both player_data and Planets[31] (the fix wipes
     only what a fresh process already has as zero), and the host's rematch entry measured
     player_data nonzero>0 (match 1 really landed -- the anti-vacuity half: a run whose match 1 left
     no residue proves nothing about clearing it);
  3. --expect identical (default): the host's match-2 seed equals the client's over every
     player_data slice (p0..p7 gates/local/econ), `planets`, `players`, `player_resources` and
     `strat_players`; no `*** DESYNC` in either peer's match-2 mh_net.log; both took a first
     `[desync]` sample and the two agree;
     --expect diverge (the d36_rematch_seed_retail row, `[net] d36_clear_residue=0` = the rc4
     configuration): the seeds DIFFER inside p0_ai_econ's resource_spent (+0x18..+0x28 of the slice)
     and the host's match-2 log carries a `*** DESYNC` -- the reproduction arm, which is what shows
     the identical arm is not green for a reason unrelated to the fix.

ABSENCE IS A FAILURE: no session directory, no seed, no D36 line -- each is a refusal, never a pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_d36_rematch_seed.py [--expect identical|diverge] <host-run-dir> <client-run-dir>
  python tools/check_d36_rematch_seed.py --selftest
  python tools/check_d36_rematch_seed.py --diff <seedA.bin> <seedB.bin>   print differing runs, all regions

`<run-dir>` is what test_ui.py's post_check (with post_check_peers) hands over: each lane's newest
PROCESS directory; the session directories belonging to that process are the ones whose
session.json names it as `process_dir` (check_rematch_residue.py's rule -- a shared lane also holds
other processes' sessions).
"""

import glob
import json
import os
import re
import shutil
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO, "tools", "data", "hash_manifest.json")


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")
D36_RE = re.compile(
    r"D36: match-entry residue (cleared|KEPT)[^:]*: player_data nonzero=(\d+) of (\d+) B, "
    r"Planets\[31\] strings nonzero=(\d+) of (\d+) B"
)
DESYNC_RE = re.compile(r"\[desync\] \*\*\* DESYNC step=(\d+).*?first_region=(\d+) (\S+)")
FIRST_RE = re.compile(r"\[desync\] first sample sent: step=(\d+) state=([0-9A-F]{16})")

# the regions this fix owns (player_data slices, the scenario planet) plus the per-player identity
# and stock tables a rematch could plausibly carry -- all hashed (state-only) regions
COMPARED_RE = re.compile(
    r"^(p[0-7]_(ai_gates|local|ai_econ)|planets|players|player_resources|strat_players)$"
)
RESOURCE_SPENT = ("p0_ai_econ", 0x18, 0x28)  # record +0x10054..+0x10064 inside the +0x1003c slice


def load_regions():
    with open(MANIFEST, encoding="utf-8") as fh:
        regs = json.load(fh)["regions"]
    out, off = [], 0
    for r in regs:
        out.append((r["name"], int(r["addr"], 16), off, r["size"]))
        off += r["size"]
    return out, off


def diff_runs(a, b, regions, names=None):
    """{region: [(start, end), ...]} of differing byte runs, restricted to `names` if given."""
    res = {}
    for name, _base, off, size in regions:
        if names is not None and not names(name):
            continue
        ra, rb = a[off : off + size], b[off : off + size]
        if ra == rb:
            continue
        runs, j = [], 0
        while j < size:
            if ra[j] != rb[j]:
                k = j
                while k < size and ra[k] != rb[k]:
                    k += 1
                runs.append((j, k))
                j = k
            else:
                j += 1
        res[name] = runs
    return res


def session_process_dir(folder):
    try:
        with open(os.path.join(folder, "session.json"), encoding="utf-8", errors="replace") as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return None
    return d.get("process_dir") if isinstance(d, dict) else None


def lane_sessions(run_dir):
    if not os.path.isdir(run_dir):
        raise Refusal("no such run directory: %s" % run_dir)
    logs = os.path.dirname(run_dir.rstrip("\\/"))
    if os.path.basename(logs) != "logs":
        raise Refusal("%s is not a run directory under a lane's logs/" % run_dir)
    leaf = os.path.basename(run_dir.rstrip("\\/"))
    sess = [
        d
        for d in glob.glob(os.path.join(logs, "*"))
        if os.path.isdir(d)
        and SESSION_DIR_RE.match(os.path.basename(d))
        and session_process_dir(d) == leaf
    ]
    return sorted(sess, key=os.path.basename)


def read_lines(fp):
    if not os.path.isfile(fp):
        return []
    with open(fp, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def d36_lines(run_dir, sessions):
    """Every D36 line this process wrote. lg() writes mh_launch.log into the session directory
    while one is open and into the process directory otherwise, so read both."""
    out = []
    for d in [run_dir] + sessions:
        for ln in read_lines(os.path.join(d, "mh_launch.log")):
            m = D36_RE.search(ln)
            if m:
                out.append((m.group(1), int(m.group(2)), int(m.group(4))))
    return out


def net_facts(folder):
    lines = read_lines(os.path.join(folder, "mh_net.log"))
    if not lines:
        raise Refusal("%s has no mh_net.log" % folder)
    desyncs, first = [], None
    for ln in lines:
        m = DESYNC_RE.search(ln)
        if m:
            desyncs.append((int(m.group(1)), m.group(3)))
            continue
        m = FIRST_RE.search(ln)
        if m and first is None:
            first = (int(m.group(1)), m.group(2))
    return desyncs, first


def read_seed(folder, total):
    fp = os.path.join(folder, "mh_match_seed.bin")
    if not os.path.isfile(fp):
        raise Refusal("%s has no mh_match_seed.bin (the harness's step-1 dump)" % folder)
    with open(fp, "rb") as fh:
        blob = fh.read()
    if len(blob) != total:
        raise Refusal(
            "%s: seed is %d B, the manifest sums to %d B -- a different region table"
            % (fp, len(blob), total)
        )
    return blob


def check(run_dirs, expect="identical"):
    if len(run_dirs) != 2:
        raise Refusal(
            "want exactly two peer run directories (host, client), got %d" % len(run_dirs)
        )
    regions, total = load_regions()
    host_rd, cli_rd = run_dirs
    hs, cs = lane_sessions(host_rd), lane_sessions(cli_rd)
    print("check_d36_rematch_seed: host %d session(s), client %d session(s)" % (len(hs), len(cs)))
    if len(hs) != 2:
        raise Refusal(
            "host process holds %d session dir(s), want EXACTLY 2 (match 1, the rematch)" % len(hs)
        )
    if len(cs) != 1:
        raise Refusal(
            "client process holds %d session dir(s), want EXACTLY 1 (a fresh process)" % len(cs)
        )

    fails = []
    # sorted by the player_data count, not by file order: lg() lands in whichever of the process /
    # session directories is current at the entry prep, so file order is not match order. The
    # first-in-process entry is the smaller one (a fresh process has zero), the rematch the larger.
    hl, cl = sorted(d36_lines(host_rd, hs), key=lambda t: t[1]), d36_lines(cli_rd, cs)
    for role, got in (("host", hl), ("client", cl)):
        for mode, pd, pl in got:
            print(
                "  %-6s D36 entry: %s player_data nonzero=%d Planets[31] strings nonzero=%d"
                % (role, mode, pd, pl)
            )
    if len(hl) != 2 or len(cl) != 1:
        raise Refusal(
            "want 2 host + 1 client `D36: match-entry residue` lines, got %d + %d -- an mh.dll without "
            "the D36 entry clear, or the entry prep never ran" % (len(hl), len(cl))
        )
    fresh = [hl[0], cl[0]]
    for who, (_m, pd, pl) in zip(("host match 1", "client (fresh)"), fresh):
        if pd != 0 or pl != 0:
            fails.append(
                "%s entered with player_data nonzero=%d / Planets[31] strings nonzero=%d -- a FIRST-in-"
                "process entry is not all-zero, so the clear wipes state a fresh process has (retail "
                "writes it before the lobby entry)" % (who, pd, pl)
            )
    if hl[1][1] == 0:
        raise Refusal(
            "the host's rematch entry measured player_data nonzero=0 -- match 1 left no residue (did the "
            "host land?), so this run cannot say anything about clearing it"
        )

    hseed, cseed = read_seed(hs[1], total), read_seed(cs[0], total)
    diffs = diff_runs(hseed, cseed, regions, lambda n: bool(COMPARED_RE.match(n)))
    for name, runs in diffs.items():
        print(
            "  seed diff %-16s %d run(s): %s"
            % (name, len(runs), ", ".join("+%#x..+%#x" % r for r in runs[:6]))
        )
    hdes, hfirst = net_facts(hs[1])
    cdes, cfirst = net_facts(cs[0])
    for role, des, first in (("host", hdes, hfirst), ("client", cdes, cfirst)):
        print(
            "  %-6s match-2 [desync]: first sample %s, %d DESYNC line(s)%s"
            % (role, first, len(des), (" first step=%d %s" % des[0]) if des else "")
        )

    if expect == "identical":
        for name, runs in diffs.items():
            fails.append(
                "rematch seed differs from the fresh client's in %s (%d run(s))" % (name, len(runs))
            )
        for role, des in (("host", hdes), ("client", cdes)):
            if des:
                fails.append("%s match 2: `*** DESYNC` at step %d (%s)" % ((role,) + des[0]))
        if hfirst is None or cfirst is None:
            fails.append(
                "match 2 took no [desync] sample on %s"
                % ("the host" if hfirst is None else "the client")
            )
        elif hfirst != cfirst:
            fails.append("match 2 first samples differ: host %s client %s" % (hfirst, cfirst))
    else:
        name, lo, hi = RESOURCE_SPENT
        if not any(s < hi and e > lo for s, e in diffs.get(name, [])):
            fails.append(
                "reproduction arm: the seeds do NOT differ in %s resource_spent -- no residue reached the rematch"
                % name
            )
        if not hdes:
            fails.append("reproduction arm: the host's match 2 logged no `*** DESYNC`")
    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print(
        "[PASS] %s"
        % (
            "the rematch entered from a fresh process's state: seeds equal, [desync] clean"
            if expect == "identical"
            else "reproduction: residue in resource_spent reached the rematch and the watch said DESYNC"
        )
    )
    return 0


# ---- selftest: planted lanes -----------------------------------------------------------------------


def _plant(root, lane, sessions, launch_proc=""):
    logs = os.path.join(root, lane, "logs")
    leaf = "20260926T000000Z_menu_solo"
    menu = os.path.join(logs, leaf)
    os.makedirs(menu)
    with open(os.path.join(menu, "mh_launch.log"), "w", encoding="utf-8") as fh:
        fh.write(launch_proc)
    # a FOREIGN session (another process in a shared lane) -- must be ignored
    foreign = os.path.join(logs, "20260925T235900Z_deadbeef_0_solo")
    os.makedirs(foreign)
    with open(os.path.join(foreign, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"process_dir": "20260925T235800Z_menu_solo"}, fh)
    for k, (seed, net, launch) in enumerate(sessions):
        d = os.path.join(logs, "20260926T00000%dZ_0000000%d_0_solo" % (k + 1, k + 1))
        os.makedirs(d)
        with open(os.path.join(d, "session.json"), "w", encoding="utf-8") as fh:
            json.dump({"process_dir": leaf}, fh)
        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write(net)
        with open(os.path.join(d, "mh_launch.log"), "w", encoding="utf-8") as fh:
            fh.write(launch)
        if seed is not None:
            with open(os.path.join(d, "mh_match_seed.bin"), "wb") as fh:
                fh.write(seed)
    return menu


def selftest():
    regions, total = load_regions()
    off = {n: o for n, _b, o, _s in regions}
    clean = bytes(total)

    def poke(blob, region, rel, data):
        b = bytearray(blob)
        b[off[region] + rel : off[region] + rel + len(data)] = data
        return bytes(b)

    grant = bytes.fromhex("18150000a00f0000b80b0000d0070000")
    resid = poke(poke(clean, "p0_ai_econ", 0x18, grant), "p1_ai_econ", 0x18, grant)
    tail = poke(clean, "planets", 0x81DA, b"m")
    net_ok = "; [desync] first sample sent: step=50 state=59135D2D75636E7C (x)\n"
    net_bad = (
        net_ok
        + "; [desync] *** DESYNC step=50 peer=1 mine=1 theirs=2 first_region=8 p0_ai_econ (mismatch #1)\n"
    )
    net_other = "; [desync] first sample sent: step=50 state=0000000000000001 (x)\n"

    def l36(pd, pl, mode="cleared"):
        return (
            "; D36: match-entry residue %s: player_data nonzero=%d of 1329136 B, Planets[31] strings nonzero=%d of 765 B\n"
            % (mode, pd, pl)
        )

    fixed_host = [(clean, net_ok, l36(0, 0)), (clean, net_ok, l36(412, 18))]
    fixed_cli = [(clean, net_ok, l36(0, 0))]
    kept = "KEPT ([net] d36_clear_residue=0 -- the rc4 configuration)"
    cases = [
        ("fixed build: rematch == fresh, clean watch", fixed_host, fixed_cli, "identical", 0),
        (
            "D36 line in the PROCESS dir, not the session dir",
            [(clean, net_ok, ""), (clean, net_ok, l36(412, 18))],
            fixed_cli,
            "identical",
            0,
            l36(0, 0),
        ),
        (
            "rc4: resource_spent residue + DESYNC",
            [(clean, net_ok, l36(0, 0, kept)), (resid, net_bad, l36(412, 18, kept))],
            [(clean, net_ok, l36(0, 0, kept))],
            "identical",
            1,
        ),
        (
            "planets tail only (watch silent)",
            [fixed_host[0], (tail, net_ok, l36(412, 18))],
            fixed_cli,
            "identical",
            1,
        ),
        ("first samples disagree", fixed_host, [(clean, net_other, l36(0, 0))], "identical", 1),
        (
            "fresh entry not zero (clear would wipe retail state)",
            fixed_host,
            [(clean, net_ok, l36(7, 0))],
            "identical",
            1,
        ),
        (
            "host never landed (rematch nonzero=0)",
            [fixed_host[0], (clean, net_ok, l36(0, 0))],
            fixed_cli,
            "identical",
            "refuse",
        ),
        (
            "no D36 line (old build)",
            [(clean, net_ok, ""), (clean, net_ok, "")],
            [(clean, net_ok, "")],
            "identical",
            "refuse",
        ),
        (
            "seed missing",
            [fixed_host[0], (None, net_ok, l36(412, 18))],
            fixed_cli,
            "identical",
            "refuse",
        ),
        ("only one host session", fixed_host[:1], fixed_cli, "identical", "refuse"),
        (
            "reproduction arm reproduces",
            [(clean, net_ok, l36(0, 0, kept)), (resid, net_bad, l36(412, 18, kept))],
            [(clean, net_ok, l36(0, 0, kept))],
            "diverge",
            0,
        ),
        ("reproduction arm on a fixed run -> red", fixed_host, fixed_cli, "diverge", 1),
    ]
    import contextlib
    import io

    bad = 0
    for case in cases:
        title, host, cli, expect, want = case[:5]
        proc_launch = case[5] if len(case) > 5 else ""
        root = tempfile.mkdtemp(prefix="d36_selftest_")
        try:
            h = _plant(root, "ui_x_host", host, proc_launch)
            c = _plant(root, "ui_x_c1", cli)
            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    got = check([h, c], expect)
            except Refusal:
                got = "refuse"
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-58s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
    print("selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if argv and argv[0] == "--diff":
        regions, total = load_regions()
        a, b = (open(p, "rb").read() for p in argv[1:3])
        for name, runs in diff_runs(a, b, regions).items():
            base, off = next((r[1], r[2]) for r in regions if r[0] == name)
            print("== %s runs=%d" % (name, len(runs)))
            for s, e in runs[:12]:
                print(
                    "   +%#x..+%#x VA %#x  A:%s B:%s"
                    % (
                        s,
                        e,
                        base + s,
                        a[off + s : off + e][:20].hex(),
                        b[off + s : off + e][:20].hex(),
                    )
                )
        return 0
    expect = "identical"
    if "--expect" in argv:
        expect = argv[argv.index("--expect") + 1]
        if expect not in ("identical", "diverge"):
            print("--expect identical|diverge")
            return 2
    dirs = [a for a in argv if not a.startswith("--") and a not in ("identical", "diverge")]
    try:
        return check(dirs, expect)
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
