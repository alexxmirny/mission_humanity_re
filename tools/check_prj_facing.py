#!/usr/bin/env python3
"""
tools/check_prj_facing.py -- mods:PRJ1: a flying shot's sprite stays inside its weapon's frame block.

The row prj_facing_rl4 loads 8-ayatus-rl-4.sav (the user's 0.2.2-rc3 playtest save: twin rocket launchers, weapon 20, bullet_anim 1523,
a 24-facing animation) with the shipped configuration (mh.dll without libmh.dll, [caps] raise=1, the user's [net] keys) and has the harness
dump the projectile pool (REGIONS[] index 16, `RX <step> 16 <off> <hex>` rows in mh_match_harness.log) at every sim step.

For a per-facing weapon the frame of a shot is bullet_anim + stride*(facing-1) for the whole flight (stride 1 here). The rc3
`projectile_facing_fix` wrapped the facing delta, so a shot crossing the 24<->1 boundary (due south) went one frame OUTSIDE the
weapon's 24-frame block (1522 = an explosion sprite, or 1547 = weapon 19's bullet) and stayed there. This counts the shots whose
anim_loop_frame is not base + facing - 1 on some step. A shot that is already wrong the first time it is seen, on the FIRST
dumped step, was launched before the load clock (it came out of the save) and is ignored.

  python tools/check_prj_facing.py <session-run-dir>              # green: 0 wrong shots, and a real sample of weapon 20 shots
  python tools/check_prj_facing.py --expect-bad <session-run-dir>  # the expected-red arm ([net] projectile_facing_fix=2, the rc3 rule): >= 10
  python tools/check_prj_facing.py --selftest
"""

import argparse
import glob
import os
import struct
import sys

POOL_STRIDE = 121
POOL_SIZE = 181500  # 1500 slots + slot 0 header, the stock projectile pool
FMT = "<iiHHHHHHHHBdddddddiiiiiiiHiHi"  # llm_strat_projectile (121 bytes); see tmp/playtest/0.2.2-rc3/proj/rx_scan.py
NAMES = (
    "active weapon_id x y src_x src_y_ground src_y_vis dst_x dst_y_ground dst_y_vis owner delta_x delta_y launch_time "
    "duration anim_clock smoke_clock explo_clock scatter_x scatter_y facing anim_frame smoke_sprite explo_flag anim_loop "
    "homing_player homing_unit shooter_ref shooter_unit"
).split()
# per-facing weapons of the save: weapon id -> first frame of the 24-frame block (cfg bullet_anim, stride 1)
BASE = {20: 1523, 19: 1547}
EXPECT_BAD_MIN = 10  # the rc3 rule measured 53 on this save; a fix-off arm cannot be near zero
MIN_SAMPLE = 20  # distinct weapon-19/20 shots a run must have seen, or it measured nothing


def scan(path):
    """-> (steps, {(slot, launch_time): first step seen}, {key: first wrong step}, ignored count)"""
    cur = None
    first_step = None
    buf = bytearray(POOL_SIZE)
    seen = {}
    bad = {}
    pre = set()  # keys wrong at first sight on the first dumped step (launched before the load)
    steps = 0

    def one(step):
        for s in range(1, 1500):
            v = dict(zip(NAMES, struct.unpack_from(FMT, buf, POOL_STRIDE * s)))
            if not v["active"] or v["weapon_id"] not in BASE:
                continue
            key = (s, round(v["launch_time"], 3))
            seen.setdefault(key, step)
            if v["anim_loop"] != BASE[v["weapon_id"]] + v["facing"] - 1:
                if key not in bad:
                    bad[key] = step
                    if seen[key] == step and step == first_step:
                        pre.add(key)

    with open(path, errors="replace") as fh:
        for ln in fh:
            if not ln.startswith("RX "):
                continue
            parts = ln.split()
            if len(parts) != 5 or parts[2] != "16":
                continue
            st, off = int(parts[1]), int(parts[3])
            if cur is not None and st != cur:
                one(cur)
                steps += 1
            if first_step is None:
                first_step = st
            cur = st
            raw = bytes.fromhex(parts[4])
            buf[off : off + len(raw)] = raw
    if cur is not None:
        one(cur)
        steps += 1
    counted = {k: v for k, v in bad.items() if k not in pre}
    return steps, seen, counted, len(pre)


def find_log(run_dir):
    for pat in ("mh_match_harness.log", "*/mh_match_harness.log"):
        g = sorted(glob.glob(os.path.join(run_dir, pat)))
        if g:
            return g[-1]
    return None


def verdict(steps, seen, bad, pre, expect_bad):
    msgs = []
    ok = True
    msgs.append(
        "  %d dumped steps, %d weapon-19/20 shots seen, %d ignored (wrong in the save), %d wrong"
        % (steps, len(seen), pre, len(bad))
    )
    for k, st in sorted(bad.items(), key=lambda kv: kv[1])[:5]:
        msgs.append("    slot %d launched %.3f first wrong at step %d" % (k[0], k[1], st))
    if steps < 100 or len(seen) < MIN_SAMPLE:
        ok = False
        msgs.append(
            "FAIL: sample too small (%d steps, %d shots) -- the pool dump or the save load did not run"
            % (steps, len(seen))
        )
    elif expect_bad:
        if len(bad) < EXPECT_BAD_MIN:
            ok = False
            msgs.append(
                "FAIL: expected-red arm shows only %d wrong shots (< %d): the rc3 rule no longer reproduces, the row guards nothing"
                % (len(bad), EXPECT_BAD_MIN)
            )
    elif bad:
        ok = False
        msgs.append("FAIL: %d shot(s) left their weapon's frame block" % len(bad))
    return ok, msgs


def selftest():
    import tempfile

    def row(step, slot, wid, facing, loop, t0):
        rec = bytearray(POOL_STRIDE)
        vals = dict.fromkeys(NAMES, 0)
        vals.update(active=1, weapon_id=wid, facing=facing, anim_loop=loop, launch_time=t0)
        struct.pack_into(FMT, rec, 0, *[vals[n] for n in NAMES])
        return "RX %d 16 %d %s\n" % (step, POOL_STRIDE * slot, rec.hex())

    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "mh_match_harness.log")
        with open(p, "w") as f:
            f.write(row(1, 3, 20, 24, 1522, 5.0))  # in the save, already wrong -> ignored
            f.write(row(2, 3, 20, 24, 1522, 5.0))
            f.write(row(2, 4, 20, 1, 1523, 9.0))
            f.write(row(3, 4, 20, 24, 1546, 9.0))  # fine: 1523+23
            f.write(row(4, 4, 20, 1, 1523, 9.0))
            f.write(row(4, 5, 20, 24, 1522, 9.5))  # wrong, new shot
            f.write(row(5, 5, 20, 24, 1522, 9.5))
        steps, seen, bad, pre = scan(p)
        assert (steps, len(seen), len(bad), pre) == (5, 3, 1, 1), (steps, len(seen), len(bad), pre)
    print("check_prj_facing selftest: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-bad", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("run_dir", nargs="?")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("usage: check_prj_facing.py [--expect-bad] <session-run-dir>")
    log = find_log(a.run_dir)
    if not log:
        print("FAIL: no mh_match_harness.log under %s" % a.run_dir)
        return 1
    ok, msgs = verdict(*scan(log), a.expect_bad)
    print("\n".join(msgs))
    print(
        "check_prj_facing: %s%s"
        % ("PASS" if ok else "FAIL", " [expected-red arm]" if a.expect_bad else "")
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
