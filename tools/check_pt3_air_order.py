#!/usr/bin/env python3
"""
tools/check_pt3_air_order.py -- patching:PT-FIX3: a helicopter stuck at microstep 31 with a stale facing
must apply its next order.

llm_strat_order_queue_dispatch (0x00466892) defers an air unit's order while facing_target (+0x2c) differs
from MOVE_MICROSTEPS[move_heading][move_microstep].facing, and sets order_queued. llm_strat_unit_state_move_path
(0x00480b53) returns early at move_microstep == 31 && order_queued without reconciling the two, and
llm_strat_unit_update_rotation never rotates a non-independent unit in state 0x12: the order is deferred
forever and the helicopter freezes for the rest of the match (RE-MISC10, report 01M4GXVSZYS90E275896C1VDGZ).
`[net] air_order_facing_fix` (default 1) snaps both facings to the table value in that early return: a block
splice at 0x00480B86 in configuration (1) (mh/seams/net_lockstep.cpp, install_air_order_facing_fix), the same
rule in the promoted move_path (simtest arm sim_unit_move_path_air_facing_selftest.cpp).

The stimulus is the harness fixture's (`pt3_*`, pt3_tick in mh_harness/harness.cpp): create a helicopter, send
it east (MOVE1), and once it is in the path state (0x12) send it north-west (MOVE2) while holding state
0x12 / microstep 31 / a stale facing until the dispatcher defers MOVE2 (order_queued = 1). Then release and
watch pt3_tail steps. Its probe lines (`; PT3 ...`) are what this reads. Two arms, one checker:

  * `air_order_deadlock` (`--expect fixed`, ship default):
      clause 1  the net log says `air order facing fix: PATCHED`;
      clause 2  both peers logged the whole fixture (FOUND, IN_PATH, MOVE2, QUEUED, VERDICT), no FAIL;
      clause 3  at QUEUED the premise held: state 0x12, microstep 31, order_queued 1 (the stuck shape)
      clause 4  the order was APPLIED within BOUND steps of being queued and the helicopter then moved
                (displacement >= MIN_MOVED tiles) with the goal set to MOVE2's target;
      clause 5  both peers' probe lines agree, the state hash is IDENTICAL on every common step and the
                in-band desync watch stayed quiet.
  * `air_order_deadlock_retail` (`--expect retail`, `[net] air_order_facing_fix=0`):
      clause 1  the net log says `air order facing fix OFF`;
      clauses 2, 3, 5 as above (retail still agrees with itself);
      clause 4  the order was NEVER applied: after the whole tail the helicopter is still in state 0x12 at
                microstep 31 with order_queued 1, has not moved, and its goal is not MOVE2's target.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pt3_air_order.py --expect fixed|retail <host-run-dir> <client-run-dir>
  python tools/check_pt3_air_order.py --selftest        planted logs; every negative RED
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

PATCHED = "air order facing fix: PATCHED"
OFF = "air order facing fix OFF"
DESYNC = "*** DESYNC step="
BOUND = 30  # steps from QUEUED to the order being applied (fixed arm); measured well under this
MIN_MOVED = 3  # tiles the helicopter must have travelled by the end of the tail (fixed arm)

QUEUED_RE = re.compile(
    r"^; PT3 QUEUED step=(?P<step>\d+) unit=(?P<unit>\d+) state=0x(?P<state>[0-9a-f]+) ms=(?P<ms>-?\d+) "
    r"oq=(?P<oq>\d+) facing=(?P<ft>\d+)/(?P<fc>\d+) heading=(?P<hd>\d+)"
)
VERDICT_RE = re.compile(
    r"^; PT3 VERDICT step=(?P<step>\d+) applied_after=(?P<applied>-?\d+) state=0x(?P<state>[0-9a-f]+) "
    r"oq=(?P<oq>\d+) ms=(?P<ms>-?\d+) moved_tiles=(?P<moved>\d+) goal=(?P<gx>\d+),(?P<gy>\d+) "
    r"want_goal=(?P<wx>\d+),(?P<wy>\d+)"
)


def parse(run_dir):
    """The fixture's lines for one peer."""
    out = {
        "found": False,
        "in_path": False,
        "move2": False,
        "queued": None,
        "verdict": None,
        "fail": [],
        "raw": [],
    }
    for ln in check_storage_purge.harness_log_lines(run_dir):
        if not ln.startswith("; PT3 "):
            continue
        out["raw"].append(ln.strip())
        if ln.startswith("; PT3 FAIL"):
            out["fail"].append(ln.strip())
        elif ln.startswith("; PT3 FOUND"):
            out["found"] = True
        elif ln.startswith("; PT3 IN_PATH"):
            out["in_path"] = True
        elif ln.startswith("; PT3 MOVE2"):
            out["move2"] = True
        else:
            m = QUEUED_RE.match(ln)
            if m:
                d = m.groupdict()
                out["queued"] = {k: (int(v, 16) if k == "state" else int(v)) for k, v in d.items()}
                continue
            m = VERDICT_RE.match(ln)
            if m:
                d = m.groupdict()
                out["verdict"] = {k: (int(v, 16) if k == "state" else int(v)) for k, v in d.items()}
    return out


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    host_net = check_cancel_task.net_log_lines(host_dir)
    net_all = host_net + check_cancel_task.net_log_lines(client_dir)
    want = PATCHED if expect == "fixed" else OFF
    if not any(want in ln for ln in host_net):
        fails.append("clause 1: the host's mh_net.log has no `; ... %s ...` line" % want)
    if expect == "fixed" and any(OFF in ln for ln in host_net):
        fails.append("clause 1: an OFF line on the ship arm")

    peers = {"host": parse(host_dir), "client": parse(client_dir)}
    for name, p in peers.items():
        if p["fail"]:
            fails.append("clause 2: %s fixture FAIL: %s" % (name, p["fail"][0]))
            continue
        if not (p["found"] and p["in_path"] and p["move2"] and p["queued"] and p["verdict"]):
            fails.append(
                "clause 2: %s did not log the whole fixture (found %s, in_path %s, move2 %s, queued %s, verdict %s)"
                % (
                    name,
                    p["found"],
                    p["in_path"],
                    p["move2"],
                    bool(p["queued"]),
                    bool(p["verdict"]),
                )
            )
            continue
        q, v = p["queued"], p["verdict"]
        if q["state"] != 0x12 or q["ms"] != 31 or q["oq"] != 1:
            fails.append(
                "clause 3: %s at QUEUED the unit is state 0x%02x ms %d oq %d -- the stuck-shape premise did not hold"
                % (name, q["state"], q["ms"], q["oq"])
            )
        goal_ok = (v["gx"], v["gy"]) == (v["wx"], v["wy"])
        if expect == "fixed":
            if not (1 <= v["applied"] <= BOUND):
                fails.append(
                    "clause 4: %s the order was applied after %d steps, expected 1..%d"
                    % (name, v["applied"], BOUND)
                )
            if v["moved"] < MIN_MOVED:
                fails.append(
                    "clause 4: %s the helicopter moved only %d tiles after the order (expected >= %d)"
                    % (name, v["moved"], MIN_MOVED)
                )
            if not goal_ok:
                fails.append(
                    "clause 4: %s the goal %d,%d is not MOVE2's target %d,%d"
                    % (name, v["gx"], v["gy"], v["wx"], v["wy"])
                )
        else:
            if v["applied"] != -1 or v["state"] != 0x12 or v["oq"] != 1 or v["ms"] != 31:
                fails.append(
                    "clause 4: %s the retail deadlock did not reproduce (applied_after %d, state 0x%02x, oq %d, ms %d)"
                    % (name, v["applied"], v["state"], v["oq"], v["ms"])
                )
            if v["moved"] != 0:
                fails.append(
                    "clause 4: %s the stuck helicopter moved %d tiles" % (name, v["moved"])
                )
            if goal_ok:
                fails.append(
                    "clause 4: %s the order was applied (goal == MOVE2's target) on the retail arm"
                    % name
                )
    hv, cv = peers["host"]["verdict"], peers["client"]["verdict"]
    if hv != cv or peers["host"]["queued"] != peers["client"]["queued"]:
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
    summary = "overlap=%d mismatches=%d queued=%s verdict=%s" % (
        ov,
        n,
        peers["host"]["queued"],
        peers["host"]["verdict"],
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------


def plant(
    root,
    name,
    banner,
    applied=5,
    state=0x12,
    oq=1,
    ms=31,
    moved=12,
    goal_ok=True,
    q_state=0x12,
    q_ms=31,
    q_oq=1,
    steps=1400,
    desync=False,
    diverge=False,
    fail=False,
    move2=True,
):
    """A planted peer's logs under root/name."""
    proc = os.path.join(root, name, "logs", "20261010T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261010T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    lines = [
        "; PT3 FOUND step=600 unit=4 state=0x13 ms=0 oq=0 facing=3/3 heading=0 xy=40,50 goal=0,0",
        "; PT3 IN_PATH step=700 unit=4 state=0x12 ms=12 oq=0 facing=6/6 heading=2 xy=44,50 goal=65,50",
    ]
    if move2:
        lines.append("; PT3 MOVE2 step=700 unit=4 -> (31,38) -- issued here (owner)")
    lines.append(
        "; PT3 QUEUED step=708 unit=4 state=0x%02x ms=%d oq=%d facing=16/16 heading=2 xy=52,50 goal=65,50"
        % (q_state, q_ms, q_oq)
    )
    gx, gy = (31, 38) if goal_ok else (65, 50)
    lines.append(
        "; PT3 VERDICT step=1008 applied_after=%d state=0x%02x oq=%d ms=%d moved_tiles=%d goal=%d,%d want_goal=31,38"
        % (applied, state, oq, ms, moved, gx, gy)
    )
    if fail:
        lines.append("; PT3 FAIL step=900 phase=5: phase timeout")
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
                "; air order facing fix: PATCHED -- an air unit stuck at microstep 31 with a stale facing is snapped to the table facing at 00480B86 (PT-FIX3)\n"
            )
        elif banner == "off":
            fh.write("; air order facing fix OFF ([net] air_order_facing_fix=0, PT-FIX3): retail\n")
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if desync:
            fh.write("; [desync] %s1100 peer=1 first_region=3 units (mismatch #1)\n" % DESYNC)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20261010T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    fixed = dict(banner="patched")
    retail = dict(banner="off", applied=-1, moved=0, goal_ok=False)
    cases = [
        ("fixed arm: applied in 5 steps, moved 12", "fixed", dict(fixed), True),
        ("fixed NEG: banner OFF", "fixed", dict(fixed, banner="off"), False),
        ("fixed NEG: no banner", "fixed", dict(fixed, banner=None), False),
        ("fixed NEG: never applied (-1)", "fixed", dict(fixed, applied=-1), False),
        ("fixed NEG: applied too late (80)", "fixed", dict(fixed, applied=80), False),
        ("fixed NEG: applied but did not move", "fixed", dict(fixed, moved=0), False),
        ("fixed NEG: goal is not MOVE2's target", "fixed", dict(fixed, goal_ok=False), False),
        ("fixed NEG: not stuck at QUEUED (ms 12)", "fixed", dict(fixed, q_ms=12), False),
        ("fixed NEG: not stuck at QUEUED (state 0x0b)", "fixed", dict(fixed, q_state=0x0B), False),
        ("fixed NEG: MOVE2 never logged", "fixed", dict(fixed, move2=False), False),
        ("fixed NEG: fixture FAIL line", "fixed", dict(fixed, fail=True), False),
        ("fixed NEG: desync watch fired", "fixed", dict(fixed, desync=True), False),
        ("fixed NEG: hash diverges", "fixed", dict(fixed, diverge=True), False),
        ("fixed NEG: short match", "fixed", dict(fixed, steps=300), False),
        ("retail arm: OFF, stuck", "retail", dict(retail), True),
        ("retail NEG: banner says PATCHED", "retail", dict(retail, banner="patched"), False),
        (
            "retail NEG: applied (not stuck)",
            "retail",
            dict(retail, applied=5, goal_ok=True, moved=12, state=0x0B, oq=0, ms=3),
            False,
        ),
        ("retail NEG: moved while 'stuck'", "retail", dict(retail, moved=4), False),
        ("retail NEG: oq cleared", "retail", dict(retail, oq=0), False),
    ]
    tmp = tempfile.mkdtemp(prefix="pt3_selftest_")
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
    print("check_pt3_air_order selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
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
    print("check_pt3_air_order (%s): %s" % (a.expect, summary))
    for f in fails:
        print("  FAIL: " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
