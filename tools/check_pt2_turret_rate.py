#!/usr/bin/env python3
"""
tools/check_pt2_turret_rate.py -- patching:PT-FIX2: a turret's fire interval must not depend on the sim step.

Retail hands llm_strat_bldg_turret_reload_tick the re-counted budget GAME_CLOCK - last_tick_time, and the
attack state (llm_strat_bldg_state_turret_attack) gives an unspent budget back, so that budget runs dt, 2dt ..
K*dt (K = ceil(0.05 / dt)) and the reload timer drains ~(K+1)/2 x real time: ~2-3x too fast at our 10/20 ms
steps, cfg timing at the retail MP step (0.1 s). `[net] turret_reload_realtime_fix` (default 1) drains by the
step's real dt times `[net] turret_fire_rate_mult` (default 2.0) instead: a byte splice at 0x0046FE5D in
configuration (1) (mh/seams/net_lockstep.cpp, turret_reload_stub), the same rule in the promoted building_tick
(simtest arm sim_bldg_tick_turret_rate_selftest.cpp).

The stimulus is the harness fixture's (`pt2_*`, pt2_tick in mh_harness/harness.cpp): an unshared weapon-8 turret
(short_time 0.2) and weapon-21 turret (short_time 1.75) shoot an immobile, immortal enemy helicopter and every
shot is logged `; PT2 FIRE w=<weapon> n=<k> step=<s> clock_us=<t>`. The expected interval comes from the
attack-state MODEL (the same one the simtest arm and the analysis use): fire interval = the reload time plus the
attack state's own 0.05 s poll. Two arms, one checker, run at --step-ms 20 and 10:

  * `turret_rate*` (`--expect fixed`): clause 1 the net log says `turret reload realtime fix ON`;
    clause 3 each weapon's mean interval is within TOL of the model with the fix on, and at least
    short/mult - one step (the reload itself is exact; the poll only adds).
  * `turret_rate*_retail` (`--expect retail`): clause 1 the net log says `turret reload realtime fix OFF`;
    clause 3 the mean interval is within TOL of the re-count model.
  Both: clause 2 both peers logged the whole fixture (>= SHOTS shots per turret, a VERDICT, no FAIL);
  clause 4 the peers' shot lines are identical, the state hash is IDENTICAL on every common step and the
  in-band desync watch stayed quiet.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pt2_turret_rate.py --expect fixed|retail --step-ms 20|10 <host-run-dir> <client-run-dir>
  python tools/check_pt2_turret_rate.py --selftest        planted logs; every negative RED

PT-SET1 (`gameplay_host_wins`): `--mult M` is the turret_fire_rate_mult the HOST ran (default 2.0; the model and the
lower bound use it), and `--expect-adopted` adds clause 5: the host's net log has a `; gameplay: PUBLISHED <summary>`
line, the client's has `; gameplay: ADOPTED from host <summary>` with the SAME summary (and that summary says
turret_fire_rate_mult=M), and the client's last turret-reload-realtime banner is the arm's (the client's own ini
said otherwise, so the host's value reached it). Clause 3 then measures BOTH peers at the host's rate.
"""

import argparse
import json
import math
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import check_storage_purge  # noqa: E402  (harness_log_lines)
import mp_analyze  # noqa: E402

ON = "turret reload realtime fix ON"
OFF = "turret reload realtime fix OFF"
DESYNC = "*** DESYNC step="
SHORT = {8: 0.2, 21: 1.75}  # cfg Weapon[].short_time of the two fixture weapons
MULT = 2.0  # [net] turret_fire_rate_mult shipped default
POLL = 0.05  # per_shot_cost 0.1 * 0.5: the attack state's own budget quantum
TOL_FRAC = (
    0.07  # the model is exact for the aligned-target shape; a real turret adds target/aim noise
)
SHOTS = 12  # shots per turret the fixture must have logged (it is asked for pt2_shots)

FIRE_RE = re.compile(
    r"^; PT2 FIRE w=(?P<w>\d+) n=(?P<n>\d+) step=(?P<step>\d+) clock_us=(?P<us>\d+)"
)


PUBLISHED_RE = re.compile(r"; gameplay: PUBLISHED (\S+)")
ADOPTED_RE = re.compile(r"; gameplay: ADOPTED from host (\S+)")


def summary_mult(summary):
    """turret_fire_rate_mult out of a `k=v;k=v` gameplay summary (None = absent / unparsable)."""
    for kv in summary.split(";"):
        if kv.startswith("turret_fire_rate_mult="):
            try:
                return float(kv.split("=", 1)[1])
            except ValueError:
                return None
    return None


def adoption_fails(host_net, client_net, mult, expect):
    """Clause 5 -- the client adopted the host's published gameplay set."""
    fails = []
    pub = [m[1] for m in map(PUBLISHED_RE.search, host_net) if m]
    ado = [m[1] for m in map(ADOPTED_RE.search, client_net) if m]
    if not pub:
        fails.append("clause 5: the host's net log has no `; gameplay: PUBLISHED <summary>` line")
    if not ado:
        fails.append(
            "clause 5: the client's net log has no `; gameplay: ADOPTED from host <summary>` line"
        )
    if pub and ado:
        if ado[-1] != pub[-1]:
            fails.append(
                "clause 5: the client's ADOPTED summary differs from the host's PUBLISHED one: %s != %s"
                % (ado[-1], pub[-1])
            )
        m = summary_mult(ado[-1])
        if m is None or abs(m - mult) > 1e-9:
            fails.append(
                "clause 5: the adopted summary says turret_fire_rate_mult=%s, expected %g"
                % (m, mult)
            )
    want = ON if expect == "fixed" else OFF
    # the boot banner is in the process log, the adoption's in the session log: order by the [hh:mm:ss.mmm] prefix
    banners = sorted(
        (ln for ln in client_net if ON in ln or OFF in ln),
        key=lambda ln: ln[:14] if ln.startswith("[") else "",
    )
    if not banners or want not in banners[-1]:
        fails.append("clause 5: the client's last turret reload realtime banner is not `%s`" % want)
    return fails


def model_interval(dt, short, fix, mult=MULT, seconds=300.0):
    """Mean fire interval of the attack-state model: drain = dt*mult (fix) or the re-counted budget (retail)."""
    clock = last = timer = 0.0
    ready = True
    fires = []
    for _ in range(int(seconds / dt)):
        clock += dt
        b = clock - last
        if b <= 0:
            b = 0.0
        else:
            last = clock
        drain = dt * mult if (fix and b > 0) else b
        if not ready:
            timer -= drain
            if timer <= 0.0:
                timer, ready = 0.0, True
        while b > 0.0:
            if b < POLL:
                last -= b
                b = 0.0
            else:
                b -= POLL
            if b > 0.0:
                if not ready:
                    b = 0.0
                else:
                    timer, ready = short, False
                    fires.append(clock)
    iv = [fires[i] - fires[i - 1] for i in range(1, len(fires))]
    return sum(iv) / len(iv)


def parse(run_dir):
    out = {"fires": {8: [], 21: []}, "verdict": None, "fail": [], "target": False}
    # dict.fromkeys: a SESSION run dir (post_check_session) reads the session and the process harness log, which
    # hold the same fixture lines twice
    for ln in dict.fromkeys(check_storage_purge.harness_log_lines(run_dir)):
        if not ln.startswith("; PT2 "):
            continue
        if ln.startswith("; PT2 FAIL"):
            out["fail"].append(ln.strip())
        elif ln.startswith("; PT2 VERDICT"):
            out["verdict"] = ln.strip()
        elif "measuring" in ln:
            out["target"] = True
        else:
            m = FIRE_RE.match(ln)
            if m:
                out["fires"].setdefault(int(m["w"]), []).append((int(m["step"]), int(m["us"])))
    return out


def intervals(fires):
    return [(fires[i][1] - fires[i - 1][1]) / 1e6 for i in range(1, len(fires))]


def check(host_dir, client_dir, expect, step_ms, min_overlap=1000, mult=MULT, expect_adopted=False):
    fails = []
    dt = step_ms / 1000.0
    host_net = check_cancel_task.net_log_lines(host_dir)
    net_all = host_net + check_cancel_task.net_log_lines(client_dir)
    want = ON if expect == "fixed" else OFF
    if not any(want in ln for ln in host_net):
        fails.append("clause 1: the host's mh_net.log has no `; ... %s ...` line" % want)
    other = OFF if expect == "fixed" else ON
    if any(other in ln for ln in host_net):
        fails.append("clause 1: a `%s` line on the %s arm" % (other, expect))

    peers = {"host": parse(host_dir), "client": parse(client_dir)}
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
            iv = intervals(f)
            mean = sum(iv) / len(iv)
            means[(name, w)] = (mean, min(iv), max(iv), len(f))
            model = model_interval(dt, short, expect == "fixed", mult)
            if abs(mean - model) > max(TOL_FRAC * model, dt):
                fails.append(
                    "clause 3: %s weapon %d mean interval %.4f s, the %s model says %.4f s"
                    % (name, w, mean, "fix-on" if expect == "fixed" else "re-count", model)
                )
            if expect == "fixed" and min(iv) < short / mult - dt - 1e-6:
                fails.append(
                    "clause 3: %s weapon %d fired after %.4f s < short_time/mult %.4f"
                    % (name, w, min(iv), short / mult)
                )
    if peers["host"]["fires"] != peers["client"]["fires"]:
        fails.append(
            "clause 4: the peers' shot lines differ -- the sim diverged around the fixture"
        )
    if expect_adopted:
        fails += adoption_fails(host_net, check_cancel_task.net_log_lines(client_dir), mult, expect)
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
    gp="",
    mult=MULT,
):
    """A planted peer's logs under root/name; shots follow the model (scaled by `scale`)."""
    proc = os.path.join(root, name, "logs", "20261011T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261011T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    lines = ["; PT2 step=300 target unit 4 at (40,50) state=0x13 -- measuring"]
    for w, short in SHORT.items():
        iv = model_interval(dt, short, fix, mult) * scale
        t = 30.0
        for k in range(1, shots + 1):
            lines.append(
                "; PT2 FIRE w=%d n=%d step=%d clock_us=%d timer_ms=%d"
                % (w, k, int(t / dt), int(t * 1e6), int(short * 1000))
            )
            t += iv
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
        if banner == "on":
            fh.write(
                "; turret reload realtime fix ON ([net] turret_reload_realtime_fix=1, PT-FIX2): the reload timer drains by real step dt * mult\n"
            )
        elif banner == "off":
            fh.write(
                "; turret reload realtime fix OFF ([net] turret_reload_realtime_fix=0, PT-FIX2): retail\n"
            )
        elif banner == "off_then_on":  # the client: its own ini said 0, the host's set turned it on
            fh.write(
                "; turret reload realtime fix OFF ([net] turret_reload_realtime_fix=0, PT-FIX2): retail\n"
                "; turret reload realtime fix ON ([net] turret_reload_realtime_fix=1, PT-FIX2): the reload timer drains by real step dt * mult\n"
            )
        if gp:
            fh.write(gp)
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
            ("fixed %d ms: model-conformant" % ms, "fixed", ms, dict(fixed), True),
            ("fixed %d NEG: banner OFF" % ms, "fixed", ms, dict(fixed, banner="off"), False),
            ("fixed %d NEG: no banner" % ms, "fixed", ms, dict(fixed, banner=None), False),
            ("fixed %d NEG: shots 25%% fast" % ms, "fixed", ms, dict(fixed, scale=0.75), False),
            ("fixed %d NEG: too few shots" % ms, "fixed", ms, dict(fixed, shots=5), False),
            ("fixed %d NEG: no verdict" % ms, "fixed", ms, dict(fixed, verdict=False), False),
            ("fixed %d NEG: fixture FAIL" % ms, "fixed", ms, dict(fixed, fail=True), False),
            ("fixed %d NEG: desync watch fired" % ms, "fixed", ms, dict(fixed, desync=True), False),
            ("fixed %d NEG: hash diverges" % ms, "fixed", ms, dict(fixed, diverge=True), False),
            ("fixed %d NEG: short match" % ms, "fixed", ms, dict(fixed, steps=300), False),
            ("retail %d ms: re-count model" % ms, "retail", ms, dict(retail), True),
            ("retail %d NEG: banner ON" % ms, "retail", ms, dict(retail, banner="on"), False),
        ]
    # At 20 ms the fix-on and re-count models coincide BY DESIGN (mult 2.0 is calibrated to the 20 ms feel), so
    # a wrong-arm timing is only separable at 10 ms; at 20 ms the banner clause carries the arm.
    cases += [
        (
            "fixed 10 NEG: retail-timed shots (fix not live)",
            "fixed",
            10,
            dict(banner="on", dt=0.01, fix=False),
            False,
        ),
        (
            "retail 10 NEG: fixed-timed shots on the retail arm",
            "retail",
            10,
            dict(banner="off", dt=0.01, fix=True),
            False,
        ),
    ]
    # PT-SET1: the host's gameplay set reaches the client (mult 3.0 is separable from retail and from 2.0 at both steps)
    sm = "turret_reload_realtime_fix=1;turret_fire_rate_mult=3;turret_clip_fix=0;turret_fire_poll_fix=0"
    pub = "; gameplay: PUBLISHED %s [4 key(s), 4 hook(s)]\n" % sm
    ado = "; gameplay: ADOPTED from host %s -- START trailer [4 key(s), 4 hook(s)]\n" % sm
    ad = dict(
        banner="on",
        dt=0.02,
        fix=True,
        mult=3.0,
        gp_host=pub,
        gp_client=ado,
        cbanner="off_then_on",
        adopted=True,
        M=3.0,
    )
    cases += [
        ("adopted 20: host 3.0 on both peers", "fixed", 20, dict(ad), True),
        ("adopted 10: host 3.0 on both peers", "fixed", 10, dict(ad, dt=0.01), True),
        ("adopted NEG: no ADOPTED line", "fixed", 20, dict(ad, gp_client=""), False),
        ("adopted NEG: no PUBLISHED line", "fixed", 20, dict(ad, gp_host=""), False),
        (
            "adopted NEG: summaries differ",
            "fixed",
            20,
            dict(ad, gp_client=ado.replace("turret_clip_fix=0", "turret_clip_fix=1")),
            False,
        ),
        (
            "adopted NEG: both summaries carry the wrong mult",
            "fixed",
            20,
            dict(
                ad,
                gp_host=pub.replace("mult=3", "mult=2"),
                gp_client=ado.replace("mult=3", "mult=2"),
            ),
            False,
        ),
        ("adopted NEG: client banner stayed OFF", "fixed", 20, dict(ad, cbanner="off"), False),
        ("adopted NEG: shots at mult 2.0 timing", "fixed", 10, dict(ad, dt=0.01, mult=2.0), False),
    ]
    tmp = tempfile.mkdtemp(prefix="pt2_selftest_")
    bad = 0
    for i, (label, expect, ms, kw, want_ok) in enumerate(cases):
        kw = dict(kw)
        diverge = kw.pop("diverge", False)
        adopted = kw.pop("adopted", False)
        want_mult = kw.pop("M", MULT)
        gp_host, gp_client = kw.pop("gp_host", ""), kw.pop("gp_client", "")
        cbanner = kw.pop("cbanner", kw.get("banner"))
        host = plant(tmp, "c%d_host" % i, gp=gp_host, **kw)
        client = plant(
            tmp, "c%d_client" % i, diverge=diverge, gp=gp_client, **dict(kw, banner=cbanner)
        )
        fails, _ = check(host, client, expect, ms, mult=want_mult, expect_adopted=adopted)
        ok = (not fails) == want_ok
        print("  %-60s %s" % (label, "ok" if ok else "FAIL (%s)" % (fails[:1] or "passed")))
        bad += 0 if ok else 1
    print("check_pt2_turret_rate selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("dirs", nargs="*", help="host run dir, client run dir")
    ap.add_argument("--expect", choices=("fixed", "retail"))
    ap.add_argument("--step-ms", type=int, choices=(10, 20))
    ap.add_argument(
        "--mult", type=float, default=MULT, help="turret_fire_rate_mult the host ran (default 2.0)"
    )
    ap.add_argument(
        "--expect-adopted",
        action="store_true",
        help="PT-SET1: the client's net log ADOPTED the host's PUBLISHED gameplay summary",
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if len(a.dirs) != 2 or not a.expect or not a.step_ms:
        ap.error("--expect fixed|retail, --step-ms and the two run dirs are required")
    fails, summary = check(
        a.dirs[0], a.dirs[1], a.expect, a.step_ms, mult=a.mult, expect_adopted=a.expect_adopted
    )
    print("check_pt2_turret_rate (%s, %d ms): %s" % (a.expect, a.step_ms, summary))
    for f in fails:
        print("  FAIL: " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
