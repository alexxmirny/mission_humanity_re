#!/usr/bin/env python3
"""
tools/check_pt4_turret_clip.py -- patching:PT-FIX4: a turret fires `ammo` shots at short_time, then waits long_time.

Retail never initialises a turret's magazine: map_CreateBuilding's turret boot (0x004629d6-0x00462ab9) writes
resupply_available but not ammo (+0x2f) / resupply_cycles_remaining (+0x33), so a fresh slot starts at ammo 0,
llm_strat_turret_fire takes it to -1, -2 ... (`ammo != 0` -> reload = short_time every time) and
llm_strat_bldg_turret_reload_tick refills only at ammo == 0 -- the clip and the long reload are dead code.
`[net] turret_clip_fix=1` (default 0) sets ammo = Weapon.ammo and resupply_cycles_remaining = -1 at creation (a byte
splice at 0x00462AB9 in configuration (1), mh/seams/net_lockstep.cpp turret_clip_create_stub) and refills at
ammo <= 0 (a one-byte JNZ->JG at 0x0047C6CF); the promoted bodies carry the same rule (simtest arm
sim_bldg_turret_clip_selftest.cpp).

The stimulus is PT-FIX2's fixture (`pt2_*`, pt2_tick in mh_harness/harness.cpp): an unshared weapon-8 turret
(ammo 3, short 0.2, long 0.4) and weapon-21 turret (ammo 1, short = long = 1.75) shoot an immobile enemy and every
shot is logged `; PT2 FIRE w=<weapon> n=<k> step=<s> clock_us=<t> timer_ms=<ms> ammo=<a>` (ammo AFTER the shot).
The rows pin turret_reload_realtime_fix=1 and turret_fire_rate_mult=2.0, so the expected interval is the PT-FIX2
attack-state model (reload / mult + the attack state's 0.05 s poll) with the magazine added. Two arms:

  * `turret_clip` (`--expect clip`): clause 1 the net log says `turret clip fix ON` with no site MISMATCHED
    (the splices are always installed, runtime-gated); clause 3 weapon 8's ammo runs 2,1,0,2,1,0 and its timer is long_time exactly on the third
    shot, the SHORT-class intervals (after shots 1,2 of a clip) and the LONG-class interval (after shot 3) each
    match the model within TOL, and the long class is clearly longer; weapon 21 (ammo 1) is unchanged.
  * `turret_clip_retail` (`--expect retail`): clause 1 the net log says `turret clip fix OFF` (splices installed, gate 0); clause 3 ammo runs
    -1,-2,-3 ..., every timer is short_time and the mean interval is the steady short one (the PT-FIX2 model).
  Both: clause 2 both peers logged the whole fixture (>= SHOTS shots per turret, a VERDICT, no FAIL);
  clause 4 the peers' shot lines are identical, the state hash is IDENTICAL on every common step and the
  in-band desync watch stayed quiet.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pt4_turret_clip.py --expect clip|retail --step-ms 20 <host-run-dir> <client-run-dir>
  python tools/check_pt4_turret_clip.py --selftest        planted logs; every negative RED
"""

import argparse
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import check_storage_purge  # noqa: E402  (harness_log_lines)
import mp_analyze  # noqa: E402

ON = "turret clip fix ON"
OFF = "turret clip fix OFF"
DESYNC = "*** DESYNC step="
# cfg Weapon[] rows of the two fixture weapons: (clip ammo, short_time, long_time)
WEAPONS = {8: (3, 0.2, 0.4), 21: (1, 1.75, 1.75)}
MULT = 2.0  # [net] turret_fire_rate_mult the rows pin
POLL = 0.05  # per_shot_cost 0.1 * 0.5: the attack state's own budget quantum
TOL_FRAC = 0.07
SHOTS = 12  # shots per turret the fixture must have logged

FIRE_RE = re.compile(
    r"^; PT2 FIRE w=(?P<w>\d+) n=(?P<n>\d+) step=(?P<step>\d+) clock_us=(?P<us>\d+)"
    r" timer_ms=(?P<timer>\d+) ammo=(?P<ammo>-?\d+)"
)


def model_shots(dt, clip, short, long_, magazine, mult=MULT, seconds=300.0):
    """The PT-FIX2 attack-state model with the magazine: returns [(fire_time, ammo_after, timer_set)].
    magazine=False is retail (ammo starts 0 and is never refilled: every reload is short_time)."""
    clock = last = timer = 0.0
    ready = True
    ammo = clip if magazine else 0
    shots = []
    for _ in range(int(seconds / dt)):
        clock += dt
        b = clock - last
        if b <= 0:
            b = 0.0
        else:
            last = clock
        drain = dt * mult if b > 0 else b
        if not ready:
            timer -= drain
            if timer <= 0.0:
                if ammo <= 0 and magazine:
                    ammo = clip
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
                    ammo -= 1
                    timer, ready = (short if ammo != 0 else long_), False
                    shots.append((clock, ammo, timer))
    return shots


def class_means(dt, clip, short, long_, magazine):
    """(mean interval after a short-armed shot, mean interval after a long-armed shot or None)."""
    sh = model_shots(dt, clip, short, long_, magazine)
    s, l = [], []
    for i in range(1, len(sh)):
        (l if sh[i - 1][2] == long_ and long_ != short else s).append(sh[i][0] - sh[i - 1][0])
    return sum(s) / len(s), (sum(l) / len(l) if l else None)


def parse(run_dir):
    out = {"fires": {8: [], 21: []}, "verdict": None, "fail": [], "target": False}
    for ln in check_storage_purge.harness_log_lines(run_dir):
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
                out["fires"].setdefault(int(m["w"]), []).append(
                    (int(m["step"]), int(m["us"]), int(m["timer"]), int(m["ammo"]))
                )
    return out


def mean(xs):
    return sum(xs) / len(xs)


def check(host_dir, client_dir, expect, step_ms, min_overlap=1000):
    fails = []
    dt = step_ms / 1000.0
    host_net = check_cancel_task.net_log_lines(host_dir)
    net_all = host_net + check_cancel_task.net_log_lines(client_dir)
    clip = expect == "clip"
    want = ON if clip else OFF
    if not any(want in ln for ln in host_net):
        fails.append("clause 1: the host's mh_net.log has no `; ... %s ...` line" % want)
    other = OFF if clip else ON
    if any(other in ln for ln in host_net):
        fails.append("clause 1: a `%s` line on the %s arm" % (other, expect))
    if any("turret clip fix" in ln and "MISMATCHED" in ln for ln in host_net):
        fails.append("clause 1: a splice site MISMATCHED (the splices are installed on both arms)")

    peers = {"host": parse(host_dir), "client": parse(client_dir)}
    summary = {}
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
        for w, (magazine, short, long_) in WEAPONS.items():
            f = p["fires"].get(w, [])
            if len(f) < SHOTS:
                fails.append(
                    "clause 2: %s weapon %d logged %d shots (< %d)" % (name, w, len(f), SHOTS)
                )
                continue
            # ---- per-shot magazine bookkeeping: ammo after each shot, and which reload it armed ----
            for i, (_, _, timer, ammo) in enumerate(f):
                n = i + 1
                if clip:
                    want_ammo = magazine - ((n - 1) % magazine) - 1
                    want_timer = long_ if want_ammo == 0 else short
                else:
                    want_ammo = -n
                    want_timer = short
                if ammo != want_ammo:
                    fails.append(
                        "clause 3: %s weapon %d shot %d left ammo %d, want %d"
                        % (name, w, n, ammo, want_ammo)
                    )
                    break
                if abs(timer - want_timer * 1000) > 70:
                    fails.append(
                        "clause 3: %s weapon %d shot %d armed a %d ms reload, want ~%d ms"
                        % (name, w, n, timer, want_timer * 1000)
                    )
                    break
            # ---- interval classes vs the model ----
            iv_s, iv_l = [], []
            for i in range(1, len(f)):
                iv = (f[i][1] - f[i - 1][1]) / 1e6
                armed_long = clip and f[i - 1][3] == 0 and long_ != short
                (iv_l if armed_long else iv_s).append(iv)
            ms, ml = class_means(dt, magazine, short, long_, clip)
            got_s = mean(iv_s)
            summary[(name, w)] = (got_s, mean(iv_l) if iv_l else None, len(f))
            if abs(got_s - ms) > max(TOL_FRAC * ms, dt):
                fails.append(
                    "clause 3: %s weapon %d short-class mean %.4f s, the model says %.4f s"
                    % (name, w, got_s, ms)
                )
            if clip and ml is not None:
                if not iv_l:
                    fails.append("clause 3: %s weapon %d has no long-class interval" % (name, w))
                else:
                    got_l = mean(iv_l)
                    if abs(got_l - ml) > max(TOL_FRAC * ml, dt):
                        fails.append(
                            "clause 3: %s weapon %d long-class mean %.4f s, the model says %.4f s"
                            % (name, w, got_l, ml)
                        )
                    if got_l < got_s + (long_ - short) / MULT * 0.5:
                        fails.append(
                            "clause 3: %s weapon %d long wait %.4f s is not longer than the short %.4f s"
                            % (name, w, got_l, got_s)
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
        "w%d short-class %.4f long-class %s (%d shots)"
        % (
            w,
            summary[("host", w)][0],
            "%.4f" % summary[("host", w)][1] if summary[("host", w)][1] else "-",
            summary[("host", w)][2],
        )
        for w in WEAPONS
        if ("host", w) in summary
    )
    return fails, "overlap=%d mismatches=%d %s" % (ov, n, table)


# ---- selftest ------------------------------------------------------------------------------------


def plant(
    root,
    name,
    banner,
    dt,
    magazine,
    steps=1400,
    desync=False,
    diverge=False,
    fail=False,
    shots=20,
    verdict=True,
    ammo_shift=0,
    flat_timer=False,
    stretch_long=1.0,
):
    """A planted peer's logs under root/name; shots follow the model (magazine = the clip arm)."""
    proc = os.path.join(root, name, "logs", "20261011T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261011T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    lines = ["; PT2 step=300 target unit 4 at (40,50) state=0x13 -- measuring"]
    for w, (clip, short, long_) in WEAPONS.items():
        sh = model_shots(dt, clip, short, long_, magazine, seconds=shots * 3.0 * long_ + 10)
        t0 = 30.0 - sh[0][0]
        extra = 0.0
        for k, (t, ammo, timer) in enumerate(sh[:shots], 1):
            if k > 1 and sh[k - 2][2] == long_ and long_ != short:
                extra += (stretch_long - 1.0) * (t - sh[k - 2][0])
            tm = int((short if flat_timer else timer) * 1000)
            lines.append(
                "; PT2 FIRE w=%d n=%d step=%d clock_us=%d timer_ms=%d ammo=%d"
                % (w, k, int((t + t0) / dt), int((t + t0 + extra) * 1e6), tm, ammo + ammo_shift)
            )
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
                "; turret clip fix ON ([net] turret_clip_fix=1, PT-FIX4): create: PATCHED -- x; refill: PATCHED -- y\n"
            )
        elif banner == "mismatch":
            fh.write(
                "; turret clip fix ON ([net] turret_clip_fix=1, PT-FIX4): create: MISMATCHED; refill: PATCHED -- y\n"
            )
        elif banner == "off":
            fh.write(
                "; turret clip fix OFF ([net] turret_clip_fix=0, PT-FIX4): create: PATCHED -- x; refill: PATCHED -- y\n"
            )
        elif banner == "off_mismatch":
            fh.write(
                "; turret clip fix OFF ([net] turret_clip_fix=0, PT-FIX4): create: PATCHED -- x; refill: MISMATCHED\n"
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if desync:
            fh.write("; [desync] %s1100 peer=1 first_region=3 units (mismatch #1)\n" % DESYNC)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20261011T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    dt = 0.02
    clipk = dict(banner="on", dt=dt, magazine=True)
    retk = dict(banner="off", dt=dt, magazine=False)
    cases = [
        ("clip: model-conformant", "clip", dict(clipk), True),
        ("clip NEG: banner OFF", "clip", dict(clipk, banner="off"), False),
        ("clip NEG: no banner", "clip", dict(clipk, banner=None), False),
        ("clip NEG: a splice site MISMATCHED", "clip", dict(clipk, banner="mismatch"), False),
        (
            "clip NEG: retail shots (ammo -1,-2 ..)",
            "clip",
            dict(banner="on", dt=dt, magazine=False),
            False,
        ),
        ("clip NEG: ammo off by one", "clip", dict(clipk, ammo_shift=1), False),
        (
            "clip NEG: every timer short (no long reload)",
            "clip",
            dict(clipk, flat_timer=True),
            False,
        ),
        (
            "clip NEG: long wait collapsed to the short one",
            "clip",
            dict(clipk, stretch_long=0.5),
            False,
        ),
        ("clip NEG: long wait 60% too long", "clip", dict(clipk, stretch_long=1.6), False),
        ("clip NEG: too few shots", "clip", dict(clipk, shots=5), False),
        ("clip NEG: no verdict", "clip", dict(clipk, verdict=False), False),
        ("clip NEG: fixture FAIL", "clip", dict(clipk, fail=True), False),
        ("clip NEG: desync watch fired", "clip", dict(clipk, desync=True), False),
        ("clip NEG: hash diverges", "clip", dict(clipk, diverge=True), False),
        ("clip NEG: short match", "clip", dict(clipk, steps=300), False),
        ("retail: steady short interval", "retail", dict(retk), True),
        ("retail NEG: a splice MISMATCHED", "retail", dict(retk, banner="off_mismatch"), False),
        ("retail NEG: banner ON", "retail", dict(retk, banner="on"), False),
        (
            "retail NEG: clip-shaped shots on the retail arm",
            "retail",
            dict(banner="off", dt=dt, magazine=True),
            False,
        ),
    ]
    tmp = tempfile.mkdtemp(prefix="pt4_selftest_")
    bad = 0
    for i, (label, expect, kw, want_ok) in enumerate(cases):
        kw = dict(kw)
        diverge = kw.pop("diverge", False)
        host = plant(tmp, "c%d_host" % i, **kw)
        client = plant(tmp, "c%d_client" % i, diverge=diverge, **kw)
        fails, _ = check(host, client, expect, 20)
        ok = (not fails) == want_ok
        print("  %-60s %s" % (label, "ok" if ok else "FAIL (%s)" % (fails[:1] or "passed")))
        bad += 0 if ok else 1
    print("check_pt4_turret_clip selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("dirs", nargs="*", help="host run dir, client run dir")
    ap.add_argument("--expect", choices=("clip", "retail"))
    ap.add_argument("--step-ms", type=int, choices=(10, 20), default=20)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if len(a.dirs) != 2 or not a.expect:
        ap.error("--expect clip|retail and the two run dirs are required")
    fails, summary = check(a.dirs[0], a.dirs[1], a.expect, a.step_ms)
    print("check_pt4_turret_clip (%s, %d ms): %s" % (a.expect, a.step_ms, summary))
    for f in fails:
        print("  FAIL: " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
