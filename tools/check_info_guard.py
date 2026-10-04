#!/usr/bin/env python3
"""
tools/check_info_guard.py -- mp:U73: the entity info screen is never opened for a blank/stale entity.

The field report (rc6, 2026-10-04): the info panel of an alien spaceport raised the modal "Error:
Cannot find info text:" with an EMPTY key. A storage-panel info request names a docked row the list no
longer has, and retail indexes the list with it (docked_units[row + scroll - 1] -> a blank unit slot ->
Unit[0].info_txt == ""). mh.dll's seam (src/mh_dll/mh/seams/ui_info_guard.cpp) validates the request at
the consume site and refuses any blank entity at the entry of llm_ui_entity_info_screen_open. The rig
row `info_guard_u73` drives the host's right-clicks on a docked soldier's row four times: valid (the
info screen opens, game mode 4), a stale row injected at the consume site, a blank entity injected at
the entry, valid again. Clauses:

  clause 1  the host's mh_net.log carries the seam's banner with BOTH halves guarded and BOTH test knobs
            armed -- a `NOT guarded` / `KEPT` line is a FAIL: without the knobs nothing was staged;
  clause 2  EXACTLY ONE `storage-panel info request DROPPED` line, TEST-INJECTED, reason
            row-past-docked-list -- the consume site dropped the stale request;
  clause 3  EXACTLY ONE `entity info open REFUSED` line, TEST-INJECTED, kind=0 id=0 reason=key-empty,
            caller=00417971 (the storage panel's call at 0x0041796c) -- the entry refused the blank
            entity and named who called it;
  clause 4  NO other `; [info] guard:` REFUSED/DROPPED line -- the two valid right-clicks were not
            blocked (a guard that refuses a real screen is worse than the modal);
  clause 5  the host's walk logged every marker through `u73 done` -- both valid screens came up and
            took Esc (a modal or a refused valid open stops the `gamemode 4` waits before it), and
            neither injected request opened a screen (the walk's `gamemode 2` waits passed);
  clause 6  both peers wrote harness rows, the overlap covers a live match, and the state hash is
            IDENTICAL on every common step (the guard is UI only: no sim effect).

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_info_guard.py <host-run-dir> <client-run-dir>
  python tools/check_info_guard.py --selftest        planted logs; every negative RED
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

# The registered needles (tools/data/log_formats.json ui.info_guard_*), written as the regexes the
# registry carries.
BANNER_RE = re.compile(r"; \[info\] guard: entry ")
REFUSED_RE = re.compile(r"; \[info\] guard: entity info open REFUSED (?P<rest>.*)")
DROPPED_RE = re.compile(r"; \[info\] guard: storage-panel info request DROPPED (?P<rest>.*)")
SCRIPT_LOG_RE = re.compile(r"; \[script\] LOG: (?P<msg>.*)")
BANNER = "; [info] guard: entry "  # what the selftest plants
ARMED = "entry guarded, storage-request consume guarded"
KNOBS = ("TEST info_guard_test_entry armed", "TEST info_guard_test_row armed")
STORAGE_CALLER = (
    "00417971"  # the instruction after `CALL llm_ui_entity_info_screen_open` at 0x0041796c
)

# The walk's own `log` markers, in order (tools/uiscripts/mp_host_u73_info_guard.txt).
WALK_MARKS = [
    "u73 valid info screen up",
    "u73 valid info screen closed",
    "u73 stale request dropped",
    "u73 blank entity refused",
    "u73 valid again up",
    "u73 done",
]


def banner_of(lines):
    b = [ln for ln in lines if BANNER_RE.search(ln)]
    return b[-1] if b else None


def refused_lines(lines):
    return [ln for ln in lines if REFUSED_RE.search(ln)]


def dropped_lines(lines):
    return [ln for ln in lines if DROPPED_RE.search(ln)]


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


def check(host_dir, client_dir, min_overlap=1000):
    fails = []
    lines = check_cancel_task.net_log_lines(host_dir)

    banner = banner_of(lines)
    if banner is None:
        fails.append("clause 1: no `%s...` banner on the host -- the U73 seam did not run" % BANNER)
    else:
        if ARMED not in banner:
            fails.append("clause 1: the seam is not armed: %s" % banner.strip())
        for k in KNOBS:
            if k not in banner:
                fails.append(
                    "clause 1: the test knob is not armed (`%s`): %s" % (k, banner.strip())
                )

    dropped = dropped_lines(lines)
    refused = refused_lines(lines)
    inj_drop = [ln for ln in dropped if "TEST-INJECTED" in ln]
    inj_ref = [ln for ln in refused if "TEST-INJECTED" in ln]
    if len(inj_drop) != 1:
        fails.append(
            "clause 2: %d injected `storage-panel info request DROPPED` line(s), want exactly 1"
            % len(inj_drop)
        )
    elif "reason=row-past-docked-list" not in inj_drop[0]:
        fails.append(
            "clause 2: the drop's reason is not row-past-docked-list: %s" % inj_drop[0].strip()
        )
    if len(inj_ref) != 1:
        fails.append(
            "clause 3: %d injected `entity info open REFUSED` line(s), want exactly 1"
            % len(inj_ref)
        )
    else:
        r = inj_ref[0]
        for need in ("kind=0 ", "id=0 ", "reason=key-empty", "caller=" + STORAGE_CALLER):
            if need not in r:
                fails.append("clause 3: the refusal lacks `%s`: %s" % (need, r.strip()))
    stray = [ln for ln in dropped + refused if "TEST-INJECTED" not in ln]
    if stray:
        fails.append(
            "clause 4: %d un-injected refusal(s) -- a valid right-click was blocked: %s"
            % (len(stray), stray[0].strip())
        )

    walk = uidrive_lines(host_dir)
    marks = [m.group("msg").strip() for m in (SCRIPT_LOG_RE.search(ln) for ln in walk) if m]
    missing = [m for m in WALK_MARKS if m not in marks]
    if missing:
        fails.append(
            "clause 5: the walk never logged %s -- a screen did not open/close, or one that must "
            "not open did (a modal stops the walk's gamemode waits)"
            % ", ".join("`%s`" % m for m in missing)
        )

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 6: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov = d.get("overlap", 0)
    n = d.get("mismatch_count", 0)
    if ov < min_overlap:
        fails.append("clause 6: overlap %d steps < %d -- no live match" % (ov, min_overlap))
    if n:
        fails.append(
            "clause 6: state hash differs on %d step(s), first at %s"
            % (n, (d.get("first_mismatch") or {}).get("step"))
        )
    summary = "overlap=%d mismatches=%d dropped=%d refused=%d" % (ov, n, len(dropped), len(refused))
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

GOOD_BANNER = "%s; %s; %s (mp:U73)" % (ARMED, KNOBS[0], KNOBS[1])
GOOD_DROP = (
    "[00:00:41.000] ; [info] guard: storage-panel info request DROPPED reason=row-past-docked-list "
    "request=41 scroll=0 docked=1 slot=0 docked_unit=-1 proto=0 side=0 (mp:U73) TEST-INJECTED"
)
GOOD_REFUSE = (
    "[00:00:43.000] ; [info] guard: entity info open REFUSED kind=0 id=0 reason=key-empty key=\"\" "
    "caller=00417971 side=0 cfg_total=40 lists[units=0/0] sel_bldg=1 (mp:U73) TEST-INJECTED"
)


def plant(root, name, steps, banner=None, extra=(), marks=(), diverge_from=None):
    proc = os.path.join(root, name, "logs", "20261004T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261004T000001Z_deadbeef_0_solo")
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
            fh.write("[00:00:00.000] %s%s\n" % (BANNER, banner))
    with open(os.path.join(proc, "mh_uidrive.log"), "w", encoding="utf-8") as fh:
        for i, m in enumerate(marks):
            fh.write("[%6.3f] ; [script] LOG: %s\n" % (41.0 + i, m))
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        for ln in extra:
            fh.write(ln + "\n")
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20261004T000000Z_menu_solo"}, fh)
    return sess


def selftest():
    banner = GOOD_BANNER[len("") :]
    good = dict(banner=banner, extra=[GOOD_DROP, GOOD_REFUSE], marks=list(WALK_MARKS))

    def without(key, val):
        d = dict(good)
        d[key] = val
        return d

    stray = (
        "[00:00:45.000] ; [info] guard: entity info open REFUSED kind=0 id=7 reason=key-not-in-info-txt "
        "key=\"X.INF\" caller=00417971 (mp:U73)"
    )
    cases = [
        ("fixed: armed, one drop, one refusal, walk complete, identical", dict(good), True, None),
        ("NEG: no banner", without("banner", None), False, None),
        (
            "NEG: entry NOT guarded",
            without("banner", banner.replace("entry guarded", "entry NOT guarded")),
            False,
            None,
        ),
        (
            "NEG: consume NOT guarded",
            without(
                "banner",
                banner.replace(
                    "storage-request consume guarded", "storage-request consume NOT guarded"
                ),
            ),
            False,
            None,
        ),
        ("NEG: row knob not armed", without("banner", banner.replace(KNOBS[1], "")), False, None),
        ("NEG: entry knob not armed", without("banner", banner.replace(KNOBS[0], "")), False, None),
        (
            "NEG: no drop line (the consume site did not fire)",
            without("extra", [GOOD_REFUSE]),
            False,
            None,
        ),
        (
            "NEG: no refusal line (the entry did not fire)",
            without("extra", [GOOD_DROP]),
            False,
            None,
        ),
        (
            "NEG: wrong drop reason",
            without(
                "extra",
                [GOOD_DROP.replace("row-past-docked-list", "docked-unit-proto-blank"), GOOD_REFUSE],
            ),
            False,
            None,
        ),
        (
            "NEG: refusal names the wrong caller",
            without(
                "extra", [GOOD_DROP, GOOD_REFUSE.replace("caller=00417971", "caller=00416d61")]
            ),
            False,
            None,
        ),
        (
            "NEG: refusal for the wrong entity",
            without("extra", [GOOD_DROP, GOOD_REFUSE.replace("id=0 ", "id=5 ")]),
            False,
            None,
        ),
        (
            "NEG: a valid open was refused (stray line)",
            without("extra", [GOOD_DROP, GOOD_REFUSE, stray]),
            False,
            None,
        ),
        ("NEG: two drops", without("extra", [GOOD_DROP, GOOD_DROP, GOOD_REFUSE]), False, None),
        (
            "NEG: walk stopped before the 2nd valid screen",
            without("marks", WALK_MARKS[:4]),
            False,
            None,
        ),
        (
            "NEG: valid screen never closed",
            without("marks", [m for m in WALK_MARKS if m != "u73 valid info screen closed"]),
            False,
            None,
        ),
        ("NEG: hash diverges", dict(good), False, 700),
    ]
    bad = 0
    for label, hk, want, div in cases:
        root = tempfile.mkdtemp(prefix="u73chk_")
        try:
            h = plant(root, "host", 1500, diverge_from=div, **hk)
            c = plant(root, "client", 1500)
            fails, _ = check(h, c)
            got = not fails
            ok = got == want
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails)[:160])
            )
            bad += 0 if ok else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print("check_info_guard selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print("usage: check_info_guard.py <host-run-dir> <client-run-dir>")
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1])
    if fails:
        print("check_info_guard: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_info_guard: PASS (%s)" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
