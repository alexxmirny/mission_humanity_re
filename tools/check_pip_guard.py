#!/usr/bin/env python3
"""
tools/check_pip_guard.py -- hotfix rc9.4 `[net] bldg_pip_guard`: a building with a garbage pip_level must
not crash the game.

Field crashes 0x0047908b (llm_strat_bldg_update_charge_pips) and 0x004793e1 (llm_strat_bldg_tick_pip_anim)
indexed the 4-entry fire-animation table A_OGIEN with a building's pip_level[slot] holding garbage
(tmp/report_triage_2026-10-15/crash_7908b.md). The guard (mh/seams/bldg_pip_guard.cpp splices 0x0046fe67 +
0x00478ecb; the libmh twin in sim_building_tick.cpp) zeroes an illegal pip state and logs `; [pip] GUARD`.

The stimulus is the harness fixture's (`pip_*`, pip_tick in mh_harness/harness.cpp): at one step on every peer,
plant pip_level[0] = 0xC384DE82, pip_active_count 0 and half-energy pending damage into player 0's mother
building, then `pip_tail` steps later log `; PIP DONE`. Two arms, one checker:

  * `bldg_pip_guard` (`--expect fixed`, ship default; a segment of the registry row `ship_fix_a`):
      clause 1  the host's net log says `bldg pip guard ON` with both sites PATCHED (or DISPLACED);
      clause 2  both peers logged POKE and DONE, and at DONE every pip_level is 0..4 and active 0..16;
      clause 3  both peers logged one `[pip] GUARD (building_tick)` line carrying the planted value;
      clause 4  the state hash is IDENTICAL on every common step and the in-band desync watch stayed quiet.
  * the reproduction arm (`--expect retail`, `[net] bldg_pip_guard=0`) -- NOT a registry row: both peers die
    at 0x0047908b and a walk whose process dies is red before any post_check runs. Run it by hand:
      python tools/ui_test.py --host lane=ui_pip_retail_host:mp_host_ship_fix_a.txt --connect-ip 127.0.0.1
        --port 10000 --extra-ini tools/uiscripts/ini/video_1024.ini --client lane=ui_pip_retail_c1:mp_client_idle.txt
        --net-extra "transport=udp;bldg_pip_guard=0" --headless
        --harness-extra "region_hash_step=50;synth_move=0;pip_at=300;pip_player=0;pip_tail=300"
    (mp_host_ship_fix_a.txt is the folded walk that carries this check as its bldg_pip_guard segment; with only the pip
    knobs set the other fixtures stay off and the walk dies at the poke.) then this checker on the two lanes' newest
    session dirs. Measured 2026-10-15 on the standalone walk the row had before it was folded: PASS, crash markers
    address=0x0047908b on both peers.
      clause 1  the host's net log says `bldg pip guard OFF`;
      clause 2  the host logged POKE and NO peer logged DONE (the game did not survive the poke);
      clause 3  no `[pip] GUARD` line anywhere; a crash marker, when one was written, names 0x0047908B or
                0x004793E1.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pip_guard.py --expect fixed|retail <host-run-dir> <client-run-dir>
  python tools/check_pip_guard.py --selftest        planted logs; every negative RED
"""

import argparse
import glob
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import check_storage_purge  # noqa: E402  (harness_log_lines)
import mp_analyze  # noqa: E402

ON = "bldg pip guard ON"
OFF = "bldg pip guard OFF"
GUARD = "; [pip] GUARD (building_tick)"
DESYNC = "*** DESYNC step="
CRASH_SITES = ("0047908B", "004793E1")
POKE_RE = re.compile(
    r"^; PIP POKE step=(\d+) player (\d+) building (\d+) type (\d+): pip_level\[0\] = (-?\d+)"
)
DONE_RE = re.compile(
    r"^; PIP DONE step=(\d+) building (\d+) state (\d+) pip_level \{(-?\d+),(-?\d+),(-?\d+),(-?\d+)\} active (-?\d+)"
)


def parse(run_dir):
    out = {"poke": None, "done": None}
    for ln in check_storage_purge.harness_log_lines(run_dir):
        m = POKE_RE.match(ln)
        if m:
            out["poke"] = [int(x) for x in m.groups()]
            continue
        m = DONE_RE.match(ln)
        if m:
            out["done"] = [int(x) for x in m.groups()]
    return out


def crash_addrs(run_dir):
    """`address=` of every mh_crash_*.marker in the run dir or its parent (the logs root)."""
    out = []
    base = os.path.abspath(run_dir).rstrip("\\/")
    for d in (base, os.path.dirname(base)):
        for fp in glob.glob(os.path.join(d, "mh_crash_*.marker")):
            try:
                for ln in open(fp, encoding="utf-8", errors="replace").read().splitlines():
                    if ln.startswith("address="):
                        out.append(ln.split("=", 1)[1].strip().upper())
            except OSError:
                pass
    return out


def check(host_dir, client_dir, expect, min_overlap=500):
    fails = []
    host_net = check_cancel_task.net_log_lines(host_dir)
    client_net = check_cancel_task.net_log_lines(client_dir)
    if expect == "fixed":
        ban = [ln for ln in host_net if ON in ln]
        if not ban:
            fails.append("clause 1: the host's mh_net.log has no `; %s` banner" % ON)
        elif "MISMATCHED" in ban[-1]:
            fails.append("clause 1: a guard site is MISMATCHED: %s" % ban[-1].strip())
    else:
        if not any(OFF in ln for ln in host_net):
            fails.append("clause 1: the host's mh_net.log has no `; %s` line" % OFF)

    peers = {"host": parse(host_dir), "client": parse(client_dir)}
    if expect == "fixed":
        for name, p in peers.items():
            if not (p["poke"] and p["done"]):
                fails.append(
                    "clause 2: %s did not log the whole fixture (poke %s, done %s)"
                    % (name, bool(p["poke"]), bool(p["done"]))
                )
                continue
            lv, act = p["done"][3:7], p["done"][7]
            if any(v < 0 or v > 4 for v in lv) or not (0 <= act <= 16):
                fails.append(
                    "clause 2: %s at DONE the pip state is still illegal: %s active %d"
                    % (name, lv, act)
                )
        for name, net in (("host", host_net), ("client", client_net)):
            g = [ln for ln in net if GUARD in ln]
            poke = peers[name]["poke"]
            if len(g) != 1:
                fails.append(
                    "clause 3: %s logged %d `[pip] GUARD (building_tick)` line(s), expected 1"
                    % (name, len(g))
                )
            elif poke and ("{%d," % poke[4]) not in g[0]:
                fails.append(
                    "clause 3: %s GUARD line does not carry the planted value %d: %s"
                    % (name, poke[4], g[0])
                )
        desync = [ln for ln in host_net + client_net if DESYNC in ln]
        if desync:
            fails.append("clause 4: the in-band desync watch fired: %s" % desync[0].strip())
        a = mp_analyze.load_peer(host_dir)
        b = mp_analyze.load_peer(client_dir)
        if "harness" not in a or "harness" not in b:
            fails.append("clause 4: a peer has no harness log")
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
        return fails, "overlap=%d mismatches=%d poke=%s done=%s" % (
            ov,
            n,
            peers["host"]["poke"],
            peers["host"]["done"],
        )
    # retail
    if not peers["host"]["poke"]:
        fails.append("clause 2: the host never logged `; PIP POKE` -- the stimulus did not run")
    for name, p in peers.items():
        if p["done"]:
            fails.append(
                "clause 2: %s logged DONE -- retail survived the poke, the crash did not reproduce"
                % name
            )
    if any("[pip] GUARD" in ln for ln in host_net + client_net):
        fails.append("clause 3: a `[pip] GUARD` line on the retail arm")
    addrs = crash_addrs(host_dir) + crash_addrs(client_dir)
    bad = [a for a in addrs if not any(s in a for s in CRASH_SITES)]
    if bad:
        fails.append("clause 3: a crash marker names %s, not 0x0047908B / 0x004793E1" % bad[0])
    return fails, "poke=%s crash_markers=%s" % (peers["host"]["poke"], addrs or "none found")


# ---- selftest ------------------------------------------------------------------------------------


def plant(root, name, net, harness):
    d = os.path.join(root, name)
    os.makedirs(d)
    open(os.path.join(d, "mh_net.log"), "w").write("\n".join(net) + "\n")
    open(os.path.join(d, "mh_harness.log"), "w").write("\n".join(harness) + "\n")
    return d


def selftest():
    poke = "; PIP POKE step=520 player 0 building 1 type 9: pip_level[0] = -1014702462 (0xC384DE82), active 0"
    done = "; PIP DONE step=820 building 1 state 7 pip_level {2,1,0,0} active 3 energy 500"
    guard = "; [pip] GUARD (building_tick): player 0 building 1 type 9 state 7 built_flags 0x03: pip_level {-1014702462,0,0,0}"
    on = "; bldg pip guard ON ([net] bldg_pip_guard=1): building_tick @0046FE67 PATCHED; update_charge_pips @00478ECB PATCHED"
    off = "; bldg pip guard OFF ([net] bldg_pip_guard=0): retail"
    ok = True
    with tempfile.TemporaryDirectory() as t:
        h = plant(t, "h_ret", [off], [poke])
        c = plant(t, "c_ret", [off], [poke])
        f, _ = check(h, c, "retail")
        ok &= not f
        print("  retail planted crash: %s" % ("GREEN" if not f else "RED %s" % f))
        h = plant(t, "h_ret2", [off], [poke, done])
        f, _ = check(h, c, "retail")
        ok &= bool(f)
        print("  retail that survived: %s" % ("RED (expected)" if f else "GREEN -- WRONG"))
        h = plant(t, "h_fix_noguard", [on], [poke, done])
        c = plant(t, "c_fix_noguard", [on], [poke, done])
        f, _ = check(h, c, "fixed")
        ok &= any("clause 3" in x for x in f)
        print("  fixed without a GUARD line: %s" % ("RED (expected)" if f else "GREEN -- WRONG"))
        bad_done = done.replace("{2,1,0,0}", "{-5,1,0,0}")
        h = plant(t, "h_fix_bad", [on, guard], [poke, bad_done])
        c = plant(t, "c_fix_bad", [on, guard], [poke, bad_done])
        f, _ = check(h, c, "fixed")
        ok &= any("clause 2" in x for x in f)
        print(
            "  fixed with an illegal DONE state: %s" % ("RED (expected)" if f else "GREEN -- WRONG")
        )
        h = plant(t, "h_fix_mm", [on.replace("PATCHED;", "MISMATCHED;"), guard], [poke, done])
        f, _ = check(h, c, "fixed")
        ok &= any("clause 1" in x for x in f)
        print("  fixed with a MISMATCHED site: %s" % ("RED (expected)" if f else "GREEN -- WRONG"))
    print("check_pip_guard selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--expect", choices=("fixed", "retail"))
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.expect or len(a.dirs) != 2:
        ap.error("--expect fixed|retail <host-run-dir> <client-run-dir>")
    fails, summary = check(a.dirs[0], a.dirs[1], a.expect)
    print("check_pip_guard (%s): %s" % (a.expect, summary))
    for f in fails:
        print("  FAIL " + f)
    print("check_pip_guard: %s" % ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
