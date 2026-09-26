#!/usr/bin/env python3
"""
tools/check_build_probe.py -- mp:D35: the D25 build-click fix in CONFIGURATION (1) (no libmh.dll).

D25's fix (the HUD build-click's affordability probe charges nothing instead of paying and
re-granting) lived only in libmh's promoted llm_strat_bldg_try_begin_placement, so the player zips
-- which run without libmh.dll -- still desynced at a human's first build click. D35 ports it to an
mh.dll seam (src/mh_dll/mh/seams/ui_bldg_build_probe.cpp). Two arms, one checker (the
check_cancel_task.py shape):

  * `d35_buildclick_c1` (the seam ON, ship default; `--expect identical`):
      clause 1  the host's mh_net.log carries `; D35: build-click probe charge-free at ...` -- the seam
                ARMED (a KEPT / DISPLACED / NOT patched banner is a FAIL: this arm is about the seam);
      clause 2  at least one `; D35: build-click probe player N building M -> verdict ...` line -- the
                click really went through the charge-free probe;
      clause 3  both peers wrote harness rows and the overlap covers a live match;
      clause 4  the state hash is IDENTICAL on every common step;
      clause 6  the in-band desync watch (what the player sees) logged no `*** DESYNC` on either peer.
  * `d35_buildclick_c1_retail` (`[net] build_probe_free=0`, the reproduction arm; `--expect diverge`):
      clause 1  the banner says KEPT ([net] build_probe_free=0);
      clause 2  no probe line (the retail body ran);
      clause 4  the state hash DIVERGES and is still diverged at the last common step (a gains-only
                counter never heals);
      clause 5  a `pK_ai_econ` region is among the regions differing at the first step with
                per-region rows (region_hash_step in the row);
      clause 6  the in-band desync watch logged `*** DESYNC ... pK_ai_econ` -- the rc3 symptom.

Clause names are printed on failure.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_build_probe.py --expect identical|diverge <host-run-dir> <client-run-dir>
  python tools/check_build_probe.py --selftest        planted logs; every negative RED
"""

import argparse
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

ARMED = "D35: build-click probe charge-free at"
KEPT = "D35: build-click probe KEPT ([net] build_probe_free=0)"
BANNER = "D35: build-click probe"
PROBED = "D35: build-click probe player"
DESYNC = "*** DESYNC step="  # the full line (the throttled rollup says "continues")
AI_ECON_RE = re.compile(r"^p\d_ai_econ$")
AI_ECON_IN_LINE_RE = re.compile(r"\bp\d_ai_econ\b")


def banner_of(lines):
    """The seam's boot banner (the last one), or None. Probe lines share the prefix -- excluded."""
    b = [ln for ln in lines if BANNER in ln and PROBED not in ln]
    return b[-1] if b else None


def first_region_diff(a, b):
    """(step, [region names]) at the first common step whose state differs AND both peers carry
    per-region rows, or (None, None)."""
    sa, sb = a["steps"], b["steps"]
    for s in sorted(set(sa) & set(sb)):
        if sa[s]["state"] == sb[s]["state"]:
            continue
        ra, rb = a["regions"].get(s), b["regions"].get(s)
        if ra and rb and len(ra) == len(rb):
            return s, [
                mp_analyze.REGION_NAMES[i] if i < len(mp_analyze.REGION_NAMES) else "region[%d]" % i
                for i, (x, y) in enumerate(zip(ra, rb))
                if x != y
            ]
    return None, None


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    host_lines = check_cancel_task.net_log_lines(host_dir)
    client_lines = check_cancel_task.net_log_lines(client_dir)
    banner = banner_of(host_lines)
    probed = [ln for ln in host_lines if PROBED in ln]
    desync = [ln for ln in host_lines + client_lines if DESYNC in ln]

    if banner is None:
        fails.append(
            "clause 1: the host's mh_net.log has no `; %s ...` banner -- the D35 seam did not run"
            % BANNER
        )
    elif expect == "identical" and ARMED not in banner:
        fails.append("clause 1: the banner is not ARMED: %s" % banner.strip())
    elif expect == "diverge" and KEPT not in banner:
        fails.append(
            "clause 1: the banner does not say KEPT ([net] build_probe_free=0) -- the retail probe was NOT the arm under test: %s"
            % banner.strip()
        )
    if expect == "identical" and not probed:
        fails.append(
            "clause 2: no `; %s ...` line -- the Academy click never reached the charge-free probe (tab not opened, or the click was lost)"
            % PROBED
        )
    if expect == "diverge" and probed:
        fails.append(
            "clause 2: a probe line on the reproduction arm -- the seam was armed after all"
        )

    if expect == "identical" and desync:
        fails.append("clause 6: the in-band desync watch fired: %s" % desync[0].strip())
    if expect == "diverge" and not any(AI_ECON_IN_LINE_RE.search(ln) for ln in desync):
        fails.append(
            "clause 6: no `%s ... pK_ai_econ` line from the in-band watch (%d DESYNC line(s)) -- the player-visible symptom did not reproduce"
            % (DESYNC, len(desync))
        )

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 3: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov = d.get("overlap", 0)
    if ov < min_overlap:
        fails.append(
            "clause 3: overlap %d steps < %d -- the walk did not reach a live match"
            % (ov, min_overlap)
        )
    n = d.get("mismatch_count", 0)
    fm = d.get("first_mismatch") or {}
    rstep, regs = first_region_diff(a["harness"], b["harness"])
    if expect == "identical":
        if n:
            fails.append(
                "clause 4: state hash differs on %d step(s), first at %s -- the click still writes hashed state on one peer"
                % (n, fm.get("step"))
            )
    else:
        if n < 1:
            fails.append(
                "clause 4: state hash IDENTICAL on every step -- the retail probe no longer diverges (the premise moved, or the click never landed)"
            )
        else:
            sa, sb = a["harness"]["steps"], b["harness"]["steps"]
            common = sorted(set(sa) & set(sb))
            if common and sa[common[-1]]["state"] == sb[common[-1]]["state"]:
                fails.append(
                    "clause 4: the last common step %d is identical again -- a gains-only counter cannot heal"
                    % common[-1]
                )
            if regs is None:
                fails.append(
                    "clause 5: no differing step carries per-region rows on both peers -- the row needs region_hash_step"
                )
            elif not any(AI_ECON_RE.match(r) for r in regs):
                fails.append(
                    "clause 5: no pK_ai_econ among the regions differing at step %s: %s"
                    % (rstep, ", ".join(regs))
                )
    summary = (
        "overlap=%d mismatches=%d first=%s region_step=%s regions=%s probe_lines=%d desync_lines=%d"
        % (
            ov,
            n,
            fm.get("step"),
            rstep,
            ",".join(regs or []),
            len(probed),
            len(desync),
        )
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
ECON_IDX = (
    mp_analyze.REGION_NAMES.index("p0_ai_econ") if "p0_ai_econ" in mp_analyze.REGION_NAMES else 0
)


def plant(root, name, steps, diverge_from, banner, probed, desync, region_idx=None, heal_at=None):
    proc = os.path.join(root, name, "logs", "20260926T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260926T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    ridx = ECON_IDX if region_idx is None else region_idx
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 region_hash_step=50 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = diverge_from is not None and s >= diverge_from and (heal_at is None or s < heal_at)
            fh.write(
                "%d %016X %016X %016X\n"
                % (s, 0x3F80 + s, 0xB000 + s, 0xA000 + s + (1 if dv else 0))
            )
            if s % 50 == 0:
                regs = [
                    "%016X" % (0xC000 + s * 7 + i + (1 if (dv and i == ridx) else 0))
                    for i in range(REGION_COUNT)
                ]
                fh.write("R %d %s\n" % (s, " ".join(regs)))
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        if banner == "armed":
            fh.write(
                "; %s 00448C56 (pay -> can_afford) + 00448C90 (re-grant NOPed) -- the HUD click books nothing into resource_spent\n"
                % ARMED
            )
        elif banner == "kept":
            fh.write(
                "; %s: the click pays and re-grants, booking +cost into resource_spent on this peer only -- the reproduction arm\n"
                % KEPT
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if probed:
            fh.write("; %s 0 building 5 -> verdict 0x0, nothing charged\n" % PROBED)
        if desync:
            fh.write(
                "; [desync] %s%d peer=1 mine=0000000000000001 theirs=0000000000000002 first_region=11 %s (mismatch #1)\n"
                % (DESYNC, diverge_from or 1, desync)
            )
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260926T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    ship = dict(diverge_from=None, banner="armed", probed=True, desync=None)
    repro = dict(diverge_from=520, banner="kept", probed=False, desync="p0_ai_econ")
    cases = [
        ("ship arm: armed, probe line, identical, watch clean", "identical", ship, True),
        ("ship arm NEG: no probe line (click lost)", "identical", dict(ship, probed=False), False),
        ("ship arm NEG: still diverges", "identical", dict(ship, diverge_from=520), False),
        (
            "ship arm NEG: the in-band watch fired",
            "identical",
            dict(ship, desync="p0_ai_econ"),
            False,
        ),
        ("ship arm NEG: banner says KEPT", "identical", dict(ship, banner="kept"), False),
        ("ship arm NEG: no banner at all", "identical", dict(ship, banner=None), False),
        ("repro arm: kept, diverges in p0_ai_econ and stays, watch fired", "diverge", repro, True),
        (
            "repro arm NEG: identical (premise moved)",
            "diverge",
            dict(repro, diverge_from=None),
            False,
        ),
        (
            "repro arm NEG: wrong region",
            "diverge",
            dict(repro, region_idx=(ECON_IDX + 1) % REGION_COUNT),
            False,
        ),
        ("repro arm NEG: healed", "diverge", dict(repro, heal_at=900), False),
        (
            "repro arm NEG: seam armed (banner + probe line)",
            "diverge",
            dict(repro, banner="armed", probed=True),
            False,
        ),
        (
            "repro arm NEG: the in-band watch never fired",
            "diverge",
            dict(repro, desync=None),
            False,
        ),
    ]
    bad = 0
    for label, expect, kw, want in cases:
        root = tempfile.mkdtemp(prefix="d35chk_")
        try:
            h = plant(root, "host", 1500, **kw)
            c = plant(
                root, "client", 1500, diverge_from=None, banner=None, probed=False, desync=None
            )
            fails, _ = check(h, c, expect)
            got = not fails
            ok = got == want
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails))
            )
            bad += 0 if ok else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print("check_build_probe selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", choices=("identical", "diverge"), default="identical")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print(
            "usage: check_build_probe.py --expect identical|diverge <host-run-dir> <client-run-dir>"
        )
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.expect)
    if fails:
        print("check_build_probe: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_build_probe: PASS -- %s arm (%s)" % (args.expect, summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
