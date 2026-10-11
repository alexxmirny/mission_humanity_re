#!/usr/bin/env python3
"""
tools/check_pt5_turret_poll.py -- patching:PT-FIX5: an aimed, ready turret fires on the step its reload completes.

Retail's attack state (llm_strat_bldg_state_turret_attack) charges every aim pass per_shot_cost * 0.5 = 0.05 s of the
step's tick budget (budget below that is banked), breaks once aimed within tolerance 2, and its final gate fires only
with budget LEFT and reload_ready_flag == 1 -- so a finished reload waits for the next affordable pass: fire interval =
reload + 0..~0.06 s. `[net] turret_fire_poll_fix=1` (default 0) lets a READY turret already within the aim tolerance
skip the aim loop (a byte splice at 0x004721D0 in configuration (1), mh/seams/net_lockstep.cpp turret_fire_poll_stub;
the promoted attack state carries the same rule, simtest arm sim_bldg_turret_poll_selftest.cpp); a turret that has to
turn still pays 0.05 s per heading.

The stimulus is PT-FIX2's fixture (`pt2_*`, pt2_tick in mh_harness/harness.cpp): an unshared weapon-8 turret (short 0.2)
and weapon-21 turret (short 1.75) shoot an immobile enemy and every shot is logged `; PT2 FIRE w=.. n=.. step=..
clock_us=..`. The rows pin turret_reload_realtime_fix=1 and turret_fire_rate_mult=2.0. Two arms:

  * `turret_poll` (`--expect fixed`): clause 1 the net log says `turret fire poll fix ON` with no MISMATCHED; clause 3
    EVERY fire interval (the first shot is the target-acquisition shot and is skipped) lies in
    [short/mult, short/mult + one step] -- the reload is the only thing between two shots.
  * `turret_poll_retail` (`--expect retail`): clause 1 the net log says `turret fire poll fix OFF` (splice installed,
    gate 0); clause 3 the mean interval follows the PT-FIX2 table (reload / mult + the attack state's 0.05 s poll,
    check_pt2_turret_rate.model_interval).
  Both: clause 2 both peers logged the whole fixture (>= SHOTS shots per turret, a VERDICT, no FAIL); clause 4 the
  peers' shot lines are identical, the state hash is IDENTICAL on every common step and the in-band desync watch
  stayed quiet.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pt5_turret_poll.py --expect fixed|retail --step-ms 10|20 <host-run-dir> <client-run-dir>
  python tools/check_pt5_turret_poll.py --selftest        planted logs; every negative RED
"""

import argparse
import json
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import check_pt2_turret_rate as pt2  # noqa: E402  (parse, intervals, model_interval, plant)
import mp_analyze  # noqa: E402

ON = "turret fire poll fix ON"
OFF = "turret fire poll fix OFF"
DESYNC = "*** DESYNC step="
SHORT = pt2.SHORT  # cfg Weapon[].short_time of the two fixture weapons
MULT = pt2.MULT  # [net] turret_fire_rate_mult the rows pin
SHOTS = pt2.SHOTS
EPS = 1e-4  # clock_us rounding + float noise, far below one step


def ideal_interval(dt, short, mult=MULT):
    """The fix-on interval of a ready, aimed turret: the whole steps the reload timer needs."""
    return math.ceil(short / mult / dt - 1e-9) * dt


def check(host_dir, client_dir, expect, step_ms, min_overlap=1000):
    fails = []
    dt = step_ms / 1000.0
    host_net = check_cancel_task.net_log_lines(host_dir)
    net_all = host_net + check_cancel_task.net_log_lines(client_dir)
    want = ON if expect == "fixed" else OFF
    other = OFF if expect == "fixed" else ON
    if not any(want in ln for ln in host_net):
        fails.append("clause 1: the host's mh_net.log has no `; ... %s ...` line" % want)
    if any(other in ln for ln in host_net):
        fails.append("clause 1: a `%s` line on the %s arm" % (other, expect))
    if any(want in ln and "MISMATCHED" in ln for ln in host_net):
        fails.append("clause 1: the splice site MISMATCHED (not carried)")

    peers = {"host": pt2.parse(host_dir), "client": pt2.parse(client_dir)}
    means = {}
    for name, p in peers.items():
        if p["fail"]:
            fails.append("clause 2: %s fixture FAIL: %s" % (name, p["fail"][0]))
            continue
        if not (p["target"] and p["verdict"]):
            fails.append(
                "clause 2: %s did not log the whole fixture (target %s, verdict %s)"
                % (name, p["target"], bool(p["verdict"]))
            )
            continue
        for w, short in SHORT.items():
            f = p["fires"].get(w, [])
            if len(f) < SHOTS:
                fails.append(
                    "clause 2: %s weapon %d logged %d shots (< %d)" % (name, w, len(f), SHOTS)
                )
                continue
            iv = pt2.intervals(f)
            mean = sum(iv) / len(iv)
            means[(name, w)] = (mean, min(iv), max(iv), len(f))
            if expect == "fixed":
                lo, hi = short / MULT - EPS, short / MULT + dt + EPS
                bad = [x for x in iv if not (lo <= x <= hi)]
                if bad:
                    fails.append(
                        "clause 3: %s weapon %d: %d of %d intervals outside [%.4f, %.4f] s (first %.4f)"
                        % (name, w, len(bad), len(iv), lo, hi, bad[0])
                    )
            else:
                model = pt2.model_interval(dt, short, True)
                if abs(mean - model) > max(pt2.TOL_FRAC * model, dt):
                    fails.append(
                        "clause 3: %s weapon %d mean interval %.4f s, the PT-FIX2 table says %.4f s"
                        % (name, w, mean, model)
                    )
    if peers["host"]["fires"] != peers["client"]["fires"]:
        fails.append(
            "clause 4: the peers' shot lines differ -- the sim diverged around the fixture"
        )
    desync = [ln for ln in net_all if DESYNC in ln]
    if desync:
        fails.append("clause 4: the in-band desync watch fired: %s" % desync[0].strip())

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 4: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov, n = d.get("overlap", 0), d.get("mismatch_count", 0)
    if ov < min_overlap:
        fails.append(
            "clause 4: overlap %d steps < %d -- the walk did not reach a live match"
            % (ov, min_overlap)
        )
    if n:
        fails.append(
            "clause 4: state hash differs on %d step(s), first at %s"
            % (n, (d.get("first_mismatch") or {}).get("step"))
        )
    table = "; ".join(
        "w%d mean %.4f (min %.4f max %.4f, %d shots)" % (w, *means[("host", w)])
        for w in SHORT
        if ("host", w) in means
    )
    return fails, "overlap=%d mismatches=%d %s" % (ov, n, table)


# ---- selftest ------------------------------------------------------------------------------------


def plant(
    root,
    name,
    banner,
    dt,
    fix,
    steps=1400,
    desync=False,
    diverge=False,
    fail=False,
    shots=20,
    scale=1.0,
    verdict=True,
    jitter=0.0,
    mismatched=False,
):
    """A planted peer's logs under root/name. fix: shots follow the fix-on rule (whole reload steps);
    else the PT-FIX2 table. jitter adds a late shot every 5th interval (the retail poll on the fix arm)."""
    proc = os.path.join(root, name, "logs", "20261011T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261011T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    lines = ["; PT2 step=300 target unit 4 at (40,50) state=0x13 -- measuring"]
    for w, short in SHORT.items():
        iv = (ideal_interval(dt, short) if fix else pt2.model_interval(dt, short, True)) * scale
        t = 30.0
        for k in range(1, shots + 1):
            lines.append(
                "; PT2 FIRE w=%d n=%d step=%d clock_us=%d timer_ms=%d"
                % (w, k, int(t / dt), int(round(t * 1e6)), int(short * 1000))
            )
            t += iv + (0.04 if jitter and k % 5 == 0 else 0.0)
    if verdict:
        lines.append("; PT2 VERDICT step=1300 shots=%d,%d steps=1000 done=1" % (shots, shots))
    if fail:
        lines.append("; PT2 FAIL step=900 phase=1: phase timeout")
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 region_hash_step=50 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = diverge and s >= 1000
            fh.write(
                "%d %016X %016X %016X\n"
                % (s, 0x3F80 + s, 0xB000 + s, 0xA000 + s + (1 if dv else 0))
            )
        fh.write("\n".join(lines) + "\n")
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        site = "MISMATCHED" if mismatched else "PATCHED -- stub at 004721D0"
        if banner == "on":
            fh.write(
                "; turret fire poll fix ON ([net] turret_fire_poll_fix=1, PT-FIX5): %s\n" % site
            )
        elif banner == "off":
            fh.write(
                "; turret fire poll fix OFF ([net] turret_fire_poll_fix=0, PT-FIX5): %s\n" % site
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if desync:
            fh.write("; [desync] %s1100 peer=1 first_region=3 units (mismatch #1)\n" % DESYNC)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20261011T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    cases = []
    for ms in (20, 10):
        dt = ms / 1000.0
        fixed = dict(banner="on", dt=dt, fix=True)
        retail = dict(banner="off", dt=dt, fix=False)
        cases += [
            ("fixed %d ms: whole reload steps" % ms, "fixed", ms, dict(fixed), True),
            ("fixed %d NEG: banner OFF" % ms, "fixed", ms, dict(fixed, banner="off"), False),
            ("fixed %d NEG: no banner" % ms, "fixed", ms, dict(fixed, banner=None), False),
            (
                "fixed %d NEG: site MISMATCHED" % ms,
                "fixed",
                ms,
                dict(fixed, mismatched=True),
                False,
            ),
            (
                "fixed %d NEG: a late shot (the poll is back)" % ms,
                "fixed",
                ms,
                dict(fixed, jitter=1.0),
                False,
            ),
            ("fixed %d NEG: shots 25%% fast" % ms, "fixed", ms, dict(fixed, scale=0.75), False),
            ("fixed %d NEG: too few shots" % ms, "fixed", ms, dict(fixed, shots=5), False),
            ("fixed %d NEG: no verdict" % ms, "fixed", ms, dict(fixed, verdict=False), False),
            ("fixed %d NEG: fixture FAIL" % ms, "fixed", ms, dict(fixed, fail=True), False),
            ("fixed %d NEG: desync watch fired" % ms, "fixed", ms, dict(fixed, desync=True), False),
            ("fixed %d NEG: hash diverges" % ms, "fixed", ms, dict(fixed, diverge=True), False),
            ("fixed %d NEG: short match" % ms, "fixed", ms, dict(fixed, steps=300), False),
            ("retail %d ms: the PT-FIX2 table" % ms, "retail", ms, dict(retail), True),
            ("retail %d NEG: banner ON" % ms, "retail", ms, dict(retail, banner="on"), False),
            ("retail %d NEG: shots 25%% slow" % ms, "retail", ms, dict(retail, scale=1.25), False),
        ]
    cases += [
        (
            "retail 10 NEG: fix-timed shots on the retail arm",
            "retail",
            10,
            dict(banner="off", dt=0.01, fix=True),
            False,
        ),
    ]
    tmp = tempfile.mkdtemp(prefix="pt5_selftest_")
    bad = 0
    for i, (label, expect, ms, kw, want_ok) in enumerate(cases):
        kw = dict(kw)
        diverge = kw.pop("diverge", False)
        host = plant(tmp, "c%d_host" % i, **kw)
        client = plant(tmp, "c%d_client" % i, diverge=diverge, **kw)
        fails, _ = check(host, client, expect, ms)
        ok = (not fails) == want_ok
        print("  %-60s %s" % (label, "ok" if ok else "FAIL (%s)" % (fails[:1] or "passed")))
        bad += 0 if ok else 1
    print("check_pt5_turret_poll selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("dirs", nargs="*", help="host run dir, client run dir")
    ap.add_argument("--expect", choices=("fixed", "retail"))
    ap.add_argument("--step-ms", type=int, choices=(10, 20))
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if len(a.dirs) != 2 or not a.expect or not a.step_ms:
        ap.error("--expect fixed|retail, --step-ms and the two run dirs are required")
    fails, summary = check(a.dirs[0], a.dirs[1], a.expect, a.step_ms)
    print("check_pt5_turret_poll (%s, %d ms): %s" % (a.expect, a.step_ms, summary))
    for f in fails:
        print("  FAIL: " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
