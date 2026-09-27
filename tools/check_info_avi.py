#!/usr/bin/env python3
"""
tools/check_info_avi.py -- mp:X2g: an info screen whose AVI will not open.

The rc4 client crash: right-click a research -> "insert the CD" -> "can't find .avi" -> IDIV by zero
at 0x004cab52 (llm_ui_info_media_frame_tick wrapping by the zero-filled stream length). mh.dll's
seam (src/mh_dll/mh/seams/ui_info_avi.cpp) guards the tick and replaces the failure branch with a
no-video screen. The rig row `info_avi_absent` reaches the failure with `[net] info_avi_test_absent=*`
(the clip opens are redirected to res\\NOT.AVI -- nothing on disk moves). Clauses:

  clause 1  the host's mh_net.log carries the seam's banner with the tick GUARDED and the failure
            branch spliced (`-> no-video screen`), and the test knob ARMED -- a RETAIL / NOT line is a
            FAIL: this row is about the seam, and without the knob nothing was staged;
  clause 2  at least one `; [ui] info screen clip missing: ...` line -- the open really failed and
            the no-video path ran (no insert-CD dialog and no error box exist on that path);
  clause 3  the host's walk reached `LOG: X2G info screen closed` -- the screen came up (game mode 4),
            took Esc and returned to the map; a modal would have stopped the walk before it;
  clause 4  both peers wrote harness rows, the overlap covers a live match, and the state hash is
            IDENTICAL on every common step;
  clause 5  neither peer's lockstep clock stood still for more than --max-stall-ms (1000) of wall
            time after the match started -- the rc4 field's CD prompt froze the pair for ~4 s.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_info_avi.py <host-run-dir> <client-run-dir> [--max-stall-ms N]
  python tools/check_info_avi.py --selftest        planted logs; every negative RED
"""

import argparse
import glob
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import mp_analyze  # noqa: E402

# The registered needles (tools/data/log_formats.json ui.info_avi_guard / ui.info_avi_missing /
# uidrive.script_log_marker), written as the regexes the registry carries.
BANNER_RE = re.compile(r"; \[ui\] info screen AVI guard:")
MISSING_RE = re.compile(r"; \[ui\] info screen clip missing: ")
SCRIPT_LOG_RE = re.compile(r"; \[script\] LOG: (?P<msg>.*)")
BANNER = "; [ui] info screen AVI guard:"  # what the selftest plants
ARMED = "tick guarded, open failure -> no-video screen"
KNOB_ARMED = "info_avi_test_absent="
MISSING = "; [ui] info screen clip missing:"  # ditto
CLOSED = "X2G info screen closed"  # the walk's own `log` marker


def banner_of(lines):
    b = [ln for ln in lines if BANNER_RE.search(ln)]
    return b[-1] if b else None


def uidrive_lines(run_dir):
    """The walk's own trace: mh_uidrive.log beside the run dir's process dir (the script runs from
    DllMain's process, whose log dir the session.json names) or in the run dir itself."""
    cands = glob.glob(os.path.join(run_dir, "mh_uidrive.log"))
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            parent = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
            cands.append(os.path.join(parent, pd, "mh_uidrive.log"))
    out = []
    for fp in cands:
        if os.path.isfile(fp):
            out.extend(open(fp, encoding="utf-8", errors="replace").read().splitlines())
    return out


def longest_clock_freeze(rows):
    """Longest wall-clock span (ms) over which clock_ms did not advance, counted from the first row
    whose clock is past 0 (the lobby and the start barrier are not the question)."""
    started = [r for r in rows if r.get("clock_ms", 0) > 0]
    worst, t0, prev = 0, None, None
    for r in started:
        if prev is not None and r["clock_ms"] == prev:
            worst = max(worst, r["wall_ms"] - t0)
        else:
            t0 = r["wall_ms"]
        prev = r["clock_ms"]
    return worst, len(started)


def check(host_dir, client_dir, max_stall_ms=1000, min_overlap=1000):
    fails = []
    lines = check_cancel_task.net_log_lines(host_dir)
    banner = banner_of(lines)
    missing = [ln for ln in lines if MISSING_RE.search(ln)]
    if banner is None:
        fails.append(
            "clause 1: no `%s ...` banner on the host -- the X2g seam did not run" % BANNER
        )
    else:
        if ARMED not in banner:
            fails.append("clause 1: the seam is not armed: %s" % banner.strip())
        if KNOB_ARMED not in banner or "NOT armed" in banner:
            fails.append(
                "clause 1: the test knob is not armed -- nothing was staged: %s" % banner.strip()
            )
    if not missing:
        fails.append(
            "clause 2: no `%s` line -- the open never failed, or the no-video path never ran"
            % MISSING
        )
    walk = uidrive_lines(host_dir)
    marks = [m.group("msg").strip() for m in (SCRIPT_LOG_RE.search(ln) for ln in walk) if m]
    if CLOSED not in marks:
        fails.append(
            "clause 3: the walk never logged `LOG: %s` -- the screen did not come up and close (a modal "
            "stops the walk before it)" % CLOSED
        )

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 4: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov = d.get("overlap", 0)
    n = d.get("mismatch_count", 0)
    if ov < min_overlap:
        fails.append("clause 4: overlap %d steps < %d -- no live match" % (ov, min_overlap))
    if n:
        fails.append(
            "clause 4: state hash differs on %d step(s), first at %s"
            % (n, (d.get("first_mismatch") or {}).get("step"))
        )
    stalls = {}
    for role, peer in (("host", a), ("client", b)):
        rows = peer.get("lockstep_rows") or []
        worst, nrows = longest_clock_freeze(rows)
        stalls[role] = worst
        if nrows < 10:
            fails.append(
                "clause 5: %s has %d started lockstep rows -- nothing to measure" % (role, nrows)
            )
        elif worst > max_stall_ms:
            fails.append(
                "clause 5: %s's lockstep clock stood still for %d ms (> %d)"
                % (role, worst, max_stall_ms)
            )
    summary = "overlap=%d mismatches=%d missing_lines=%d freeze_ms host=%s client=%s" % (
        ov,
        n,
        len(missing),
        stalls.get("host"),
        stalls.get("client"),
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------


def plant(root, name, steps, banner, missing, closed, diverge_from=None, freeze_ms=0):
    proc = os.path.join(root, name, "logs", "20260926T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260926T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 region_hash_step=50 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = diverge_from is not None and s >= diverge_from
            fh.write(
                "%d %016X %016X %016X\n"
                % (s, 0x3F80 + s, 0xB000 + s, 0xA000 + s + (1 if dv else 0))
            )
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        if banner:
            fh.write("[00:00:00.000] %s %s (mp:X2g)\n" % (BANNER, banner))
    with open(os.path.join(proc, "mh_uidrive.log"), "w", encoding="utf-8") as fh:
        fh.write("[ 41.000] ; [script] LOG: X2G open info\n")
        if closed:
            fh.write("[ 45.000] ; [script] LOG: %s\n" % CLOSED)
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if missing:
            fh.write(
                "[00:00:41.000] %s res\\x.AVI and C:\\g\\res\\x.AVI did not open -- shown WITHOUT video (mp:X2g)\n"
                % MISSING
            )
    with open(os.path.join(sess, "mh_lockstep.log"), "w", encoding="utf-8") as fh:
        fh.write("# " + " ".join(mp_analyze.LOCKSTEP_COLS) + "\n")
        wall, clk = 1000000, 0
        for i in range(80):
            wall += 250
            frozen = freeze_ms and 40 <= i < 40 + freeze_ms // 250
            if not frozen:
                clk += 250
            vals = {c: 0 for c in mp_analyze.LOCKSTEP_COLS}
            vals["wall_ms"], vals["clock_ms"] = wall, clk
            fh.write(" ".join(str(vals[c]) for c in mp_analyze.LOCKSTEP_COLS) + "\n")
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260926T000000Z_menu_solo"}, fh)
    return sess


def selftest():
    good = dict(banner="%s; TEST info_avi_test_absent=* armed" % ARMED, missing=True, closed=True)
    cases = [
        ("fixed: armed, missing line, walk closed, identical, no freeze", dict(good), {}, True),
        (
            "NEG: seam RETAIL",
            dict(
                good, banner="tick RETAIL, open failure RETAIL; TEST info_avi_test_absent=* armed"
            ),
            {},
            False,
        ),
        ("NEG: knob not armed", dict(good, banner=ARMED), {}, False),
        ("NEG: no banner", dict(good, banner=None), {}, False),
        ("NEG: open never failed", dict(good, missing=False), {}, False),
        ("NEG: walk stopped by a modal", dict(good, closed=False), {}, False),
        ("NEG: hash diverges", dict(good, diverge_from=700), {}, False),
        (
            "NEG: host clock froze 4 s (the field's CD prompt)",
            dict(good, freeze_ms=4000),
            {},
            False,
        ),
        ("NEG: client clock froze 1.5 s", dict(good), dict(freeze_ms=1500), False),
        ("under the bar: a 750 ms freeze passes", dict(good, freeze_ms=750), {}, True),
    ]
    bad = 0
    for label, hk, ck, want in cases:
        root = tempfile.mkdtemp(prefix="x2gchk_")
        try:
            h = plant(root, "host", 1500, **hk)
            c = plant(root, "client", 1500, banner=None, missing=False, closed=False, **ck)
            fails, _ = check(h, c)
            got = not fails
            ok = got == want
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails))
            )
            bad += 0 if ok else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print("check_info_avi selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--max-stall-ms", type=int, default=1000)
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print("usage: check_info_avi.py <host-run-dir> <client-run-dir> [--max-stall-ms N]")
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.max_stall_ms)
    if fails:
        print("check_info_avi: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_info_avi: PASS (%s)" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
