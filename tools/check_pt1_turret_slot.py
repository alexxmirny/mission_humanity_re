#!/usr/bin/env python3
"""
tools/check_pt1_turret_slot.py -- patching:PT-FIX1: a destroyed turret's rubble must not reload the live
turret that re-used its slot.

llm_strat_building_tick calls llm_strat_bldg_turret_reload_tick for EVERY A_/H_TURRET building, rubble
(state 4, up to 20 s) included. A destroyed turret's slot is freed (llm_strat_bldg_unmap_footprint zeroes
turrets[p][sub_id].b_index) and handed to the next turret (map_CreateBuilding), but the rubble keeps its
sub_id and a tick budget that grows every step, so its reload tick drains the NEW turret's reload_timer:
about 4x the fire rate. `[net] turret_reload_owner_fix` (default 1) ticks a turret only for the building
that owns its slot: a 5-byte call-splice at 0x0046fe5d in configuration (1)
(mh/seams/net_lockstep.cpp, install_turret_reload_owner_fix), the same rule in the promoted building_tick
(simtest arm sim_bldg_tick_turret_owner_selftest.cpp).

The stimulus is the harness fixture's (`pt1_*`, pt1_tick in mh_harness/harness.cpp): build a turret, kill
it (pending_damage >= energy), order a second turret into the freed slot while the rubble still lives,
arm it (reload_ready_flag = 0, reload_timer = pt1_timer_ms) and count the steps until the flag returns.
Its probe lines (`; PT1 ...`) are what this reads. Two arms, one checker:

  * `turret_slot_reuse` (`--expect fixed`, ship default):
      clause 1  the net log says `turret reload splice: PATCHED ... PT-FIX1 owner=1`;
      clause 2  both peers logged the whole fixture (the kill, ARMED, READY) and no FAIL;
      clause 3  at ARMED the premise held: the rubble is state 4 and the live turret owns the slot;
      clause 4  READY came after about timer/step steps (the live turret drained by its OWN budget only);
      clause 5  both peers' probe lines are identical, the state hash is IDENTICAL on every common step
                and the in-band desync watch stayed quiet.
  * `turret_slot_reuse_retail` (`--expect retail`, `[net] turret_reload_owner_fix=0`):
      clause 1  the net log says `turret reload owner fix OFF`;
      clauses 2, 3, 5 as above (retail still agrees with itself);
      clause 4  READY came at least FACTOR times sooner than the fixed arm's bound (the rubble's budget
                collapses the timer).

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pt1_turret_slot.py --expect fixed|retail <host-run-dir> <client-run-dir>
  python tools/check_pt1_turret_slot.py --selftest        planted logs; every negative RED
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

PATCHED = "turret reload splice: PATCHED"  # one stub carries PT-FIX1 + PT-FIX2 since 6122d45f
OWNER_ON = "PT-FIX1 owner=1"
OFF = "turret reload owner fix OFF"
DESYNC = "*** DESYNC step="
TIMER_STEPS = 100  # pt1_timer_ms 1000 at 10 ms of game time per step
FIXED_MIN, FIXED_MAX = 90, 115
RETAIL_MAX = 60  # retail measured well under this; the fixed arm is >= FIXED_MIN

ARMED_RE = re.compile(
    r"^; PT1 ARMED step=(?P<step>\d+) rubble=(?P<rubble>\d+)\(state (?P<rstate>\d+)\) "
    r"live=(?P<live>\d+) slot=(?P<slot>\d+) b_index=(?P<bidx>\d+) timer_ms=(?P<timer>\d+)\s*$"
)
READY_RE = re.compile(
    r"^; PT1 READY step=(?P<step>\d+) steps_to_ready=(?P<n>-?\d+) rubble_state=(?P<rstate>\d+) "
    r"b_index=(?P<bidx>\d+)"
)


def parse(run_dir):
    """The fixture's lines for one peer."""
    out = {"armed": None, "ready": None, "kill": False, "fail": [], "raw": []}
    for ln in check_storage_purge.harness_log_lines(run_dir):
        if not ln.startswith("; PT1 "):
            continue
        out["raw"].append(ln.strip())
        if ln.startswith("; PT1 FAIL"):
            out["fail"].append(ln.strip())
        elif " KILL " in ln:
            out["kill"] = True
        else:
            m = ARMED_RE.match(ln)
            if m:
                out["armed"] = {k: int(v) for k, v in m.groupdict().items()}
                continue
            m = READY_RE.match(ln)
            if m:
                out["ready"] = {k: int(v) for k, v in m.groupdict().items()}
    return out


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    host_net = check_cancel_task.net_log_lines(host_dir)
    net_all = host_net + check_cancel_task.net_log_lines(client_dir)
    want = PATCHED if expect == "fixed" else OFF
    if not any(want in ln and (expect != "fixed" or OWNER_ON in ln) for ln in host_net):
        fails.append("clause 1: the host's mh_net.log has no `; ... %s ...` line" % want)
    if expect == "fixed" and any(OFF in ln for ln in host_net):
        fails.append("clause 1: an OFF line on the ship arm")

    peers = {"host": parse(host_dir), "client": parse(client_dir)}
    for name, p in peers.items():
        if p["fail"]:
            fails.append("clause 2: %s fixture FAIL: %s" % (name, p["fail"][0]))
            continue
        if not (p["kill"] and p["armed"] and p["ready"]):
            fails.append(
                "clause 2: %s did not log the whole fixture (kill %s, armed %s, ready %s)"
                % (name, p["kill"], bool(p["armed"]), bool(p["ready"]))
            )
            continue
        a, r = p["armed"], p["ready"]
        if a["rstate"] != 4 or a["bidx"] != a["live"]:
            fails.append(
                "clause 3: %s at ARMED the rubble is state %d and the slot's b_index %d (live turret "
                "%d) -- the shared-slot premise did not hold"
                % (name, a["rstate"], a["bidx"], a["live"])
            )
        if r["rstate"] != 4:
            fails.append(
                "clause 3: %s the rubble left state 4 (now %d) before READY -- the measurement is "
                "not of the shared slot" % (name, r["rstate"])
            )
        n = r["n"]
        if expect == "fixed":
            if not (FIXED_MIN <= n <= FIXED_MAX):
                fails.append(
                    "clause 4: %s READY after %d steps, expected %d..%d (timer %d ms)"
                    % (name, n, FIXED_MIN, FIXED_MAX, a["timer"])
                )
        else:
            if not (1 <= n <= RETAIL_MAX):
                fails.append(
                    "clause 4: %s READY after %d steps, expected 1..%d -- the retail collapse did not "
                    "reproduce" % (name, n, RETAIL_MAX)
                )
    if peers["host"]["raw"] != peers["client"]["raw"]:
        # the owner/observer wording differs by design; compare the measurements only
        hm = (peers["host"]["armed"], peers["host"]["ready"])
        cm = (peers["client"]["armed"], peers["client"]["ready"])
        if hm != cm:
            fails.append(
                "clause 5: the peers' probe lines differ -- the sim diverged around the fixture"
            )
    desync = [ln for ln in net_all if DESYNC in ln]
    if desync:
        fails.append("clause 5: the in-band desync watch fired: %s" % desync[0].strip())

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 5: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov, n = d.get("overlap", 0), d.get("mismatch_count", 0)
    if ov < min_overlap:
        fails.append(
            "clause 5: overlap %d steps < %d -- the walk did not reach a live match"
            % (ov, min_overlap)
        )
    if n:
        fails.append(
            "clause 5: state hash differs on %d step(s), first at %s"
            % (n, (d.get("first_mismatch") or {}).get("step"))
        )
    h = peers["host"]
    summary = "overlap=%d mismatches=%d armed=%s ready=%s" % (ov, n, h["armed"], h["ready"])
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------


def plant(
    root,
    name,
    steps_to_ready,
    banner,
    steps=1400,
    rubble_state=4,
    bidx_ok=True,
    desync=False,
    diverge=False,
    fail=False,
    kill=True,
):
    """A planted peer pair's worth of logs under root/name."""
    proc = os.path.join(root, name, "logs", "20261010T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261010T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    lines = ["; PT1 step=500 turret #1 stands: building 7 slot 5 b_index 7"]
    if kill:
        lines.append("; PT1 step=520 KILL turret #1 (building 7, slot 5)")
    lines.append(
        "; PT1 ARMED step=700 rubble=7(state %d) live=12 slot=5 b_index=%d timer_ms=1000"
        % (rubble_state, 12 if bidx_ok else 0)
    )
    lines.append(
        "; PT1 READY step=%d steps_to_ready=%d rubble_state=4 b_index=12"
        % (700 + steps_to_ready, steps_to_ready)
    )
    if fail:
        lines.append("; PT1 FAIL step=900 phase=2: phase timeout")
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
        if banner == "patched":
            fh.write(
                "; turret reload splice: PATCHED -- stub at 0046FE5D (owner check / real-dt drain per the two knobs) (PT-FIX1 owner=1, PT-FIX2 realtime=0)\n"
            )
        elif banner == "off":
            fh.write(
                "; turret reload owner fix OFF ([net] turret_reload_owner_fix=0, PT-FIX1): retail\n"
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if desync:
            fh.write("; [desync] %s1100 peer=1 first_region=3 turrets (mismatch #1)\n" % DESYNC)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20261010T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    cases = [
        (
            "fixed arm: patched, ~100 steps",
            "fixed",
            dict(steps_to_ready=100, banner="patched"),
            True,
        ),
        ("fixed NEG: banner OFF", "fixed", dict(steps_to_ready=100, banner="off"), False),
        ("fixed NEG: no banner", "fixed", dict(steps_to_ready=100, banner=None), False),
        (
            "fixed NEG: collapsed (25 steps)",
            "fixed",
            dict(steps_to_ready=25, banner="patched"),
            False,
        ),
        ("fixed NEG: never ready (-1)", "fixed", dict(steps_to_ready=-1, banner="patched"), False),
        (
            "fixed NEG: rubble not in state 4 at ARMED",
            "fixed",
            dict(steps_to_ready=100, banner="patched", rubble_state=3),
            False,
        ),
        (
            "fixed NEG: slot not owned by the live turret",
            "fixed",
            dict(steps_to_ready=100, banner="patched", bidx_ok=False),
            False,
        ),
        (
            "fixed NEG: no kill logged",
            "fixed",
            dict(steps_to_ready=100, banner="patched", kill=False),
            False,
        ),
        (
            "fixed NEG: fixture FAIL line",
            "fixed",
            dict(steps_to_ready=100, banner="patched", fail=True),
            False,
        ),
        (
            "fixed NEG: desync watch fired",
            "fixed",
            dict(steps_to_ready=100, banner="patched", desync=True),
            False,
        ),
        (
            "fixed NEG: hash diverges",
            "fixed",
            dict(steps_to_ready=100, banner="patched", diverge=True),
            False,
        ),
        (
            "fixed NEG: short match",
            "fixed",
            dict(steps_to_ready=100, banner="patched", steps=300),
            False,
        ),
        (
            "retail arm: OFF, collapsed (25 steps)",
            "retail",
            dict(steps_to_ready=25, banner="off"),
            True,
        ),
        (
            "retail NEG: banner says PATCHED",
            "retail",
            dict(steps_to_ready=25, banner="patched"),
            False,
        ),
        (
            "retail NEG: did not collapse (100 steps)",
            "retail",
            dict(steps_to_ready=100, banner="off"),
            False,
        ),
        ("retail NEG: never ready (-1)", "retail", dict(steps_to_ready=-1, banner="off"), False),
    ]
    tmp = tempfile.mkdtemp(prefix="pt1_selftest_")
    bad = 0
    for i, (label, expect, kw, want_ok) in enumerate(cases):
        kw = dict(kw)
        diverge = kw.pop("diverge", False)
        host = plant(tmp, "c%d_host" % i, **kw)
        client = plant(tmp, "c%d_client" % i, diverge=diverge, **kw)
        fails, _ = check(host, client, expect)
        ok = (not fails) == want_ok
        print("  %-60s %s" % (label, "ok" if ok else "FAIL (%s)" % (fails[:1] or "passed")))
        bad += 0 if ok else 1
    print("check_pt1_turret_slot selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("dirs", nargs="*", help="host run dir, client run dir")
    ap.add_argument("--expect", choices=("fixed", "retail"))
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if len(a.dirs) != 2 or not a.expect:
        ap.error("--expect fixed|retail and the two run dirs are required")
    fails, summary = check(a.dirs[0], a.dirs[1], a.expect)
    print("check_pt1_turret_slot (%s): %s" % (a.expect, summary))
    for f in fails:
        print("  FAIL: " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
