#!/usr/bin/env python3
"""
tools/check_netcrit_pure.py -- mp:D37: the HUD network panel's is_network_critical probe leaves no
trace in the sim of a lockstep match.

With a power-network building selected, llm_ui_hud_bldg_network_status_panel_draw calls
llm_strat_bldg_is_network_critical every frame on the selecting peer alone; its three
power_network_recomputes re-flood built_flags bit 0 over that player's buildings row. When the sim's
flags are stale (a relay mid-dismantle) that peer's `buildings` diverges -- the rc4 Nortus desync.
The mh.dll seam (src/mh_dll/mh/seams/ui_bldg_netcrit_pure.cpp) restores the player's buildings row +
strat_players record around the call. Two arms, one checker (the check_build_probe.py shape):

  * `d37_relay_chain` (seam ON, ship default; `--expect identical`):
      clause 1  the host's mh_net.log carries `; D37: network-panel probe sim-pure at ...` (ARMED);
      clause 2  a `; D37: network-panel probe player ... restored N sim byte(s)` line with N > 0 -- the
                panel really probed on stale flags and the seam undid it (N = 0 everywhere means the
                walk never reached the stale window, so IDENTICAL would prove nothing);
      clause 3  both peers wrote harness rows and the overlap covers a live match;
      clause 4  the state hash is IDENTICAL on every common step;
      clause 6  the in-band desync watch logged no `*** DESYNC` on either peer.
  * `d37_relay_chain_retail` (`[net] netcrit_ui_pure=0`, the reproduction arm; `--expect diverge`):
      clause 1  the banner says KEPT ([net] netcrit_ui_pure=0);
      clause 2  no probe line (the retail call ran);
      clause 4  the state hash DIVERGES and is still diverged at the last common step;
      clause 5  `buildings` is among the regions differing at the first step with per-region rows;
      clause 6  the in-band desync watch logged `*** DESYNC ... buildings`.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_netcrit_pure.py --expect identical|diverge <host-run-dir> <client-run-dir>
  python tools/check_netcrit_pure.py --selftest        planted logs; every negative RED
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
import check_build_probe  # noqa: E402  (first_region_diff)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import mp_analyze  # noqa: E402

ARMED = "D37: network-panel probe sim-pure at"
KEPT = "D37: network-panel probe KEPT ([net] netcrit_ui_pure=0)"
BANNER = "D37: network-panel probe"
PROBED = "D37: network-panel probe player"
PROBED_RE = re.compile(r"restored (\d+) sim byte")
DESYNC = "*** DESYNC step="
REGION = "buildings"
REGION_IN_LINE_RE = re.compile(r"\bbuildings\b")


def banner_of(lines):
    b = [ln for ln in lines if BANNER in ln and PROBED not in ln]
    return b[-1] if b else None


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    host_lines = check_cancel_task.net_log_lines(host_dir)
    client_lines = check_cancel_task.net_log_lines(client_dir)
    banner = banner_of(host_lines)
    probed = [ln for ln in host_lines if PROBED in ln]
    restored = [int(m.group(1)) for m in (PROBED_RE.search(ln) for ln in probed) if m]
    desync = [ln for ln in host_lines + client_lines if DESYNC in ln]

    if banner is None:
        fails.append(
            "clause 1: the host's mh_net.log has no `; %s ...` banner -- the D37 seam did not run"
            % BANNER
        )
    elif expect == "identical" and ARMED not in banner:
        fails.append("clause 1: the banner is not ARMED: %s" % banner.strip())
    elif expect == "diverge" and KEPT not in banner:
        fails.append(
            "clause 1: the banner does not say KEPT ([net] netcrit_ui_pure=0): %s" % banner.strip()
        )
    if expect == "identical" and not any(n > 0 for n in restored):
        fails.append(
            "clause 2: no `; %s ... restored N sim byte(s)` line with N > 0 (%d probe line(s)) -- the walk never "
            "selected a network building while the sim's flags were stale" % (PROBED, len(probed))
        )
    if expect == "diverge" and probed:
        fails.append(
            "clause 2: a probe line on the reproduction arm -- the seam was armed after all"
        )

    if expect == "identical" and desync:
        fails.append("clause 6: the in-band desync watch fired: %s" % desync[0].strip())
    if expect == "diverge" and not any(REGION_IN_LINE_RE.search(ln) for ln in desync):
        fails.append(
            "clause 6: no `%s ... buildings` line from the in-band watch (%d DESYNC line(s)) -- the symptom did not reproduce"
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
    rstep, regs = check_build_probe.first_region_diff(a["harness"], b["harness"])
    if expect == "identical":
        if n:
            fails.append(
                "clause 4: state hash differs on %d step(s), first at %s -- the panel still writes hashed state on one peer"
                % (n, fm.get("step"))
            )
    else:
        if n < 1:
            fails.append(
                "clause 4: state hash IDENTICAL on every step -- the retail probe no longer diverges"
            )
        else:
            sa, sb = a["harness"]["steps"], b["harness"]["steps"]
            common = sorted(set(sa) & set(sb))
            if common and sa[common[-1]]["state"] == sb[common[-1]]["state"]:
                fails.append(
                    "clause 4: the last common step %d is identical again -- the divergence healed"
                    % common[-1]
                )
            if regs is None:
                fails.append(
                    "clause 5: no differing step carries per-region rows on both peers -- the row needs region_hash_step"
                )
            elif REGION not in regs:
                fails.append(
                    "clause 5: `buildings` not among the regions differing at step %s: %s"
                    % (rstep, ", ".join(regs))
                )
    summary = (
        "overlap=%d mismatches=%d first=%s region_step=%s regions=%s probe_lines=%d restored_max=%d desync_lines=%d"
        % (
            ov,
            n,
            fm.get("step"),
            rstep,
            ",".join(regs or []),
            len(probed),
            max(restored or [0]),
            len(desync),
        )
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
BLDG_IDX = mp_analyze.REGION_NAMES.index(REGION) if REGION in mp_analyze.REGION_NAMES else 0


def plant(root, name, steps, diverge_from, banner, restored, desync, region_idx=None, heal_at=None):
    proc = os.path.join(root, name, "logs", "20260927T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260927T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    ridx = BLDG_IDX if region_idx is None else region_idx
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
                "; %s 00418967 -- in a lockstep match the caller's buildings row + strat_players record are restored around is_network_critical\n"
                % ARMED
            )
        elif banner == "kept":
            fh.write(
                "; %s: selecting a network building recomputes this peer's power network -- the reproduction arm\n"
                % KEPT
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        for r in restored or []:
            fh.write("; %s 0 building 3 -> verdict 0, restored %d sim byte(s)\n" % (PROBED, r))
        if desync:
            fh.write(
                "; [desync] %s%d peer=1 mine=0000000000000001 theirs=0000000000000002 first_region=0 %s (mismatch #1)\n"
                % (DESYNC, diverge_from or 1, desync)
            )
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260927T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    ship = dict(diverge_from=None, banner="armed", restored=[0, 14], desync=None)
    repro = dict(diverge_from=520, banner="kept", restored=None, desync="buildings")
    cases = [
        ("ship arm: armed, restored bytes, identical, watch clean", "identical", ship, True),
        (
            "ship arm NEG: probes restored nothing (never stale)",
            "identical",
            dict(ship, restored=[0]),
            False,
        ),
        ("ship arm NEG: no probe line", "identical", dict(ship, restored=None), False),
        ("ship arm NEG: still diverges", "identical", dict(ship, diverge_from=520), False),
        (
            "ship arm NEG: the in-band watch fired",
            "identical",
            dict(ship, desync="buildings"),
            False,
        ),
        ("ship arm NEG: banner says KEPT", "identical", dict(ship, banner="kept"), False),
        ("ship arm NEG: no banner at all", "identical", dict(ship, banner=None), False),
        ("repro arm: kept, diverges in buildings and stays, watch fired", "diverge", repro, True),
        (
            "repro arm NEG: identical (premise moved)",
            "diverge",
            dict(repro, diverge_from=None),
            False,
        ),
        (
            "repro arm NEG: wrong region",
            "diverge",
            dict(repro, region_idx=(BLDG_IDX + 1) % REGION_COUNT),
            False,
        ),
        ("repro arm NEG: healed", "diverge", dict(repro, heal_at=900), False),
        (
            "repro arm NEG: seam armed (banner + probe line)",
            "diverge",
            dict(repro, banner="armed", restored=[14]),
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
        root = tempfile.mkdtemp(prefix="d37chk_")
        try:
            h = plant(root, "host", 1500, **kw)
            c = plant(
                root, "client", 1500, diverge_from=None, banner=None, restored=None, desync=None
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
    print("check_netcrit_pure selftest: %d/%d" % (len(cases) - bad, len(cases)))
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
            "usage: check_netcrit_pure.py --expect identical|diverge <host-run-dir> <client-run-dir>"
        )
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.expect)
    if fails:
        print("check_netcrit_pure: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_netcrit_pure: PASS -- %s arm (%s)" % (args.expect, summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
