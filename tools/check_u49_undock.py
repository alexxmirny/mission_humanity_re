#!/usr/bin/env python3
"""
tools/check_u49_undock.py -- mp:U49: a second undock order must not wedge a unit that is already walking
out of its storage.

Retail llm_strat_order_queue_dispatch (0x00466892) applies an undock order (code 0x20) with no state
re-check. A second 0x20 applied after the first has moved the unit out (state 0x21, door_mutex_unit =
the unit) re-enters llm_strat_unit_state_exit_storage_begin, whose llm_strat_storage_can_exit refuses
the door's own holder: the unit sits in EXIT_WAIT (0x22) on a door only it can release, and every later
order for it is kept. The mh.dll byte patch at 0x00466cb6 (mh/seams/net_lockstep.cpp,
install_undock_reentry_fix; `[net] undock_reentry_fix`, default 1) drops a 0x20 whose unit is neither
PARKED nor already in EXIT_STORAGE_BEGIN. The promoted dispatcher carries the same rule (simtest arm).

The stimulus is the harness fixture's (`d38_*` builds a barracks and docks a soldier; `u49_*` issues two
0x20 orders `u49_gap` = 7 steps apart on the owner peer through the replicated lane, then a MOVE for the
same unit `u49_settle` steps later) and its probe lines (`; U49 ...`) are what this reads. Two arms, one
checker:

  * `u49_undock_double` (`--expect fixed`, ship default):
      clause 1  the net log says `undock re-entry fix: PATCHED`;
      clause 2  both peers logged the whole fixture (TARGET, both UNDOCK orders `gap` steps apart, MOVE,
                FINAL) and no FAIL;
      clause 3  when the second order was issued the unit was ALREADY walking out holding the door
                (state 0x21, door_mutex_unit = the unit) -- without it the premise (the second order
                lands in a later pass) did not hold and the row proves nothing;
      clause 4  SETTLED: the unit is not in EXIT_WAIT, the door mutex is 0 and no unit waits on it;
      clause 5  FINAL: the follow-up move was consumed (goal = the move target, state not 0x22);
      clause 6  both peers' probe lines are identical, the state hash is IDENTICAL on every common step
                and the in-band desync watch stayed quiet.
  * `u49_undock_double_retail` (`--expect retail`, `[net] undock_reentry_fix=0`):
      clause 1  the net log says `undock re-entry fix OFF`;
      clauses 2, 3, 6 as above (retail still agrees with itself: the wedge is deterministic);
      clause 4  SETTLED: state 0x22 and door_mutex_unit = the unit;
      clause 5  FINAL: still 0x22, the door still held, and the move NOT consumed (goal != target).

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_u49_undock.py --expect fixed|retail <host-run-dir> <client-run-dir>
  python tools/check_u49_undock.py --selftest        planted logs; every negative RED
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

PATCHED = "undock re-entry fix: PATCHED"
OFF = "undock re-entry fix OFF"
DESYNC = "*** DESYNC step="
GAP = 7
EXIT_WAIT = 0x22
WALK_OUT = 0x21

PROBE_RE = re.compile(
    r"^; U49 (?P<tag>[A-Z_]+) step=(?P<step>\d+) unit=(?P<unit>\d+) state=0x(?P<state>[0-9a-f]+) "
    r"door=(?P<door>-?\d+) waiters=(?P<waiters>-?\d+) docked=(?P<docked>-?\d+) "
    r"xy=(?P<x>\d+),(?P<y>\d+) goal=(?P<gx>\d+),(?P<gy>\d+)\s*$"
)
UNDOCK_RE = re.compile(
    r"^; U49 UNDOCK (?P<which>first|second) step=(?P<step>\d+) unit=(?P<unit>\d+) "
)
MOVE_RE = re.compile(
    r"^; U49 MOVE step=(?P<step>\d+) unit=(?P<unit>\d+) -> \((?P<tx>\d+),(?P<ty>\d+)\)"
)


def parse(run_dir):
    """The fixture's lines for one peer: probes by tag, undock steps, the move, FAIL lines."""
    out = {"probes": {}, "undock": {}, "move": None, "fail": [], "raw": []}
    for ln in check_storage_purge.harness_log_lines(run_dir):
        if not ln.startswith("; U49 "):
            continue
        out["raw"].append(ln.strip())
        if ln.startswith("; U49 FAIL"):
            out["fail"].append(ln.strip())
            continue
        m = PROBE_RE.match(ln)
        if m:
            d = {
                k: (int(v, 16) if k == "state" else int(v) if k != "tag" else v)
                for k, v in m.groupdict().items()
            }
            out["probes"].setdefault(d["tag"], []).append(d)
            continue
        m = UNDOCK_RE.match(ln)
        if m:
            out["undock"][m.group("which")] = int(m.group("step"))
            continue
        m = MOVE_RE.match(ln)
        if m:
            out["move"] = (int(m.group("tx")), int(m.group("ty")))
    return out


def last(p, tag):
    v = p["probes"].get(tag)
    return v[-1] if v else None


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
        need = [last(p, t) for t in ("BEFORE", "BEFORE_SECOND", "SETTLED", "FINAL")]
        if None in need or set(p["undock"]) != {"first", "second"} or p["move"] is None:
            fails.append(
                "clause 2: %s did not log the whole fixture (probes %s, undock %s, move %s)"
                % (name, sorted(p["probes"]), sorted(p["undock"]), p["move"])
            )
            continue
        gap = p["undock"]["second"] - p["undock"]["first"]
        if gap != GAP:
            fails.append(
                "clause 2: %s undock orders %d steps apart, expected %d" % (name, gap, GAP)
            )
        bs = last(p, "BEFORE_SECOND")
        if not (bs["state"] == WALK_OUT and bs["door"] == bs["unit"]):
            fails.append(
                "clause 3: %s at the second order the unit was state 0x%02x door=%d -- not walking out holding "
                "the door, so the second order did not land in a later pass"
                % (name, bs["state"], bs["door"])
            )
        st, fin = last(p, "SETTLED"), last(p, "FINAL")
        tx, ty = p["move"]
        consumed = (fin["gx"], fin["gy"]) == (tx, ty) and fin["state"] != EXIT_WAIT
        if expect == "fixed":
            if st["state"] == EXIT_WAIT or st["door"] != 0 or st["waiters"] != 0:
                fails.append(
                    "clause 4: %s SETTLED state 0x%02x door=%d waiters=%d -- the unit is wedged"
                    % (name, st["state"], st["door"], st["waiters"])
                )
            if not consumed:
                fails.append(
                    "clause 5: %s FINAL state 0x%02x goal=%d,%d != move target %d,%d -- the move was not consumed"
                    % (name, fin["state"], fin["gx"], fin["gy"], tx, ty)
                )
        else:
            if not (st["state"] == EXIT_WAIT and st["door"] == st["unit"]):
                fails.append(
                    "clause 4: %s SETTLED state 0x%02x door=%d -- the retail wedge did not reproduce"
                    % (name, st["state"], st["door"])
                )
            if not (fin["state"] == EXIT_WAIT and fin["door"] == fin["unit"] and not consumed):
                fails.append(
                    "clause 5: %s FINAL state 0x%02x door=%d goal=%d,%d (move target %d,%d) -- the retail wedge "
                    "did not hold" % (name, fin["state"], fin["door"], fin["gx"], fin["gy"], tx, ty)
                )
    if peers["host"]["raw"] != peers["client"]["raw"]:
        # the owner/observer wording differs by design; compare the probes only
        hp = {t: v for t, v in peers["host"]["probes"].items()}
        cp = {t: v for t, v in peers["client"]["probes"].items()}
        if hp != cp:
            fails.append(
                "clause 6: the peers' probe lines differ -- the sim diverged around the fixture"
            )
    desync = [ln for ln in net_all if DESYNC in ln]
    if desync:
        fails.append("clause 6: the in-band desync watch fired: %s" % desync[0].strip())

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 6: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov, n = d.get("overlap", 0), d.get("mismatch_count", 0)
    if ov < min_overlap:
        fails.append(
            "clause 6: overlap %d steps < %d -- the walk did not reach a live match"
            % (ov, min_overlap)
        )
    if n:
        fails.append(
            "clause 6: state hash differs on %d step(s), first at %s"
            % (n, (d.get("first_mismatch") or {}).get("step"))
        )
    h = peers["host"]
    summary = "overlap=%d mismatches=%d settled=%s final=%s" % (
        ov,
        n,
        last(h, "SETTLED"),
        last(h, "FINAL"),
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------


def _probe(tag, step, state, door, waiters=0, goal=(0, 0), docked=0, unit=2):
    return (
        "; U49 %s step=%d unit=%d state=0x%02x door=%d waiters=%d docked=%d xy=150,38 goal=%d,%d"
        % (tag, step, unit, state, door, waiters, docked, goal[0], goal[1])
    )


def plant(
    root,
    name,
    kind,
    banner,
    steps=1400,
    gap=GAP,
    bs_state=WALK_OUT,
    desync=False,
    diverge=False,
    fail=False,
):
    """kind: 'fixed' (unit walks out, move consumed) or 'retail' (wedged)."""
    proc = os.path.join(root, name, "logs", "20261001T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20261001T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    tgt = (154, 38)
    if kind == "fixed":
        settled = _probe("SETTLED", 1231, 0x01, 0)
        final = _probe("FINAL", 1381, 0x0F, 0, goal=tgt)
    else:
        settled = _probe("SETTLED", 1231, EXIT_WAIT, 2, waiters=1)
        final = _probe("FINAL", 1381, EXIT_WAIT, 2, waiters=1)
    lines = [
        "; U49 TARGET step=824 unit=2 slot=1 building=2",
        _probe("BEFORE", 824, 0x1F, 0, docked=1),
        "; U49 UNDOCK first step=824 unit=2 state=0x1f -- issued here (owner)",
        _probe("BEFORE_SECOND", 824 + gap, bs_state, 2),
        "; U49 UNDOCK second step=%d unit=2 state=0x21 -- issued here (owner)" % (824 + gap),
        settled,
        "; U49 MOVE step=1231 unit=2 -> (%d,%d) -- issued here (owner)" % tgt,
        final,
    ]
    if fail:
        lines.append("; U49 FAIL step=900: no PARKED unit in an operational storage")
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
                "; undock re-entry fix: PATCHED -- a 0x20 for a unit that is not PARKED is dropped at 00466CB6 (MP U49)\n"
            )
        elif banner == "off":
            fh.write(
                "; undock re-entry fix OFF ([net] undock_reentry_fix=0, MP U49): retail dispatch\n"
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        if desync:
            fh.write("; [desync] %s1100 peer=1 first_region=4 units (mismatch #1)\n" % DESYNC)
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20261001T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    cases = [
        (
            "fixed arm: patched, walks out, move consumed",
            "fixed",
            dict(kind="fixed", banner="patched"),
            True,
        ),
        ("fixed NEG: banner OFF", "fixed", dict(kind="fixed", banner="off"), False),
        ("fixed NEG: no banner", "fixed", dict(kind="fixed", banner=None), False),
        ("fixed NEG: still wedged", "fixed", dict(kind="retail", banner="patched"), False),
        (
            "fixed NEG: second order not in a later pass (unit still parked)",
            "fixed",
            dict(kind="fixed", banner="patched", bs_state=0x1F),
            False,
        ),
        (
            "fixed NEG: orders not 7 steps apart",
            "fixed",
            dict(kind="fixed", banner="patched", gap=3),
            False,
        ),
        (
            "fixed NEG: fixture FAIL line",
            "fixed",
            dict(kind="fixed", banner="patched", fail=True),
            False,
        ),
        (
            "fixed NEG: desync watch fired",
            "fixed",
            dict(kind="fixed", banner="patched", desync=True),
            False,
        ),
        (
            "fixed NEG: hash diverges",
            "fixed",
            dict(kind="fixed", banner="patched", diverge_client=True),
            False,
        ),
        ("fixed NEG: short match", "fixed", dict(kind="fixed", banner="patched", steps=300), False),
        ("retail arm: OFF, wedged, move kept", "retail", dict(kind="retail", banner="off"), True),
        ("retail NEG: banner says PATCHED", "retail", dict(kind="retail", banner="patched"), False),
        (
            "retail NEG: the unit walked out (premise moved)",
            "retail",
            dict(kind="fixed", banner="off"),
            False,
        ),
        (
            "retail NEG: second order not in a later pass",
            "retail",
            dict(kind="retail", banner="off", bs_state=0x1F),
            False,
        ),
    ]
    bad = 0
    with tempfile.TemporaryDirectory() as root:
        for i, (label, expect, kw, should_pass) in enumerate(cases):
            kw = dict(kw)
            dc = kw.pop("diverge_client", False)
            h = plant(root, "h%d" % i, **kw)
            c = plant(root, "c%d" % i, diverge=dc, **kw)
            fails, _ = check(h, c, expect)
            ok = (not fails) == should_pass
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if ok else "  -> " + "; ".join(fails))
            )
            bad += 0 if ok else 1
    print(
        "check_u49_undock selftest: %s"
        % ("FAIL (%d)" % bad if bad else "PASS (%d cases)" % len(cases))
    )
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--expect", choices=("fixed", "retail"))
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.expect or len(a.dirs) != 2:
        ap.error("--expect and <host-run-dir> <client-run-dir> are required")
    fails, summary = check(a.dirs[0], a.dirs[1], a.expect)
    print("check_u49_undock (%s): %s" % (a.expect, summary))
    for f in fails:
        print("  FAIL " + f)
    print("check_u49_undock: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
