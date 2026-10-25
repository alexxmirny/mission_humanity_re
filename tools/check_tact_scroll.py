#!/usr/bin/env python3
"""check_tact_scroll.py -- TSCR1: the tactical camera scroll rate, read from the `[mtrace]` cam= column.

The tact_scroll row holds the right arrow, the down arrow and the cursor at the left screen edge in a
running tactical mission, each between `; [script] LOG: tact_scroll: <name> start` / `... end` markers.
Every sample carries f= (presents so far) and cam=col,row, so a segment's rate is

    tiles = |cam(last sample) - cam(first sample)|        frames = f(last) - f(first)
    tiles per game second = tiles / (frames * dt)         dt = pin_clock_dt_us of the arm

(the pinned game clock advances dt per frame, so dt is the game time of one present).

  --expect cols=15,rows=20 --tol 2.0      the paced rate (480 px/s: 32 px columns, 24 px rows) within +-tol
  --retail                               retail stepping: one tile per held frame (+-5%), the fix-off twin
  --min-frames N                         the least frames a segment must span (default 100; the retail twin uses 40)
  --dt-us N                              the arm's pin_clock_dt_us (16667 = 60 fps stand-in, 33333 = 30 fps)

The segments are `right` (cols), `left` (cols, cursor on the edge) and `down` (rows). ABSENCE IS A
FAILURE: no mtrace lines, a missing marker or fewer than 3 samples / 100 frames in a segment refuse.
The fix-on check must FAIL on a fix-off log (and the reverse); --selftest plants both.

  python tools/check_tact_scroll.py <lane|run-dir> --dt-us 16667 --expect cols=15,rows=20
  python tools/check_tact_scroll.py <lane|run-dir> --dt-us 16667 --retail
  python tools/check_tact_scroll.py --selftest
"""

import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_cam_trace as cct  # noqa: E402

SEGMENTS = (("left", "cols"), ("right", "cols"), ("down", "rows"))
MIN_SAMPLES = 3
MIN_FRAMES = 100  # --min-frames lowers it for the retail twin (one tile per frame leaves room for ~60 frames)


def segment_rate(samples, markers, name, axis, dt, min_frames=MIN_FRAMES):
    """(tiles, frames, tiles_per_s, tiles_per_frame) for one named segment."""
    lo = [ln for ln, m in markers if "tact_scroll: %s start" % name in m]
    hi = [ln for ln, m in markers if "tact_scroll: %s end" % name in m]
    if not lo or not hi:
        raise cct.Refusal(
            "no `tact_scroll: %s start/end` markers -- the script never ran that segment" % name
        )
    seg = [s for s in samples if lo[0] < s["line"] < hi[0]]
    if len(seg) < MIN_SAMPLES:
        raise cct.Refusal(
            "segment %s has %d [mtrace] samples (< %d)" % (name, len(seg), MIN_SAMPLES)
        )
    first, last = seg[0], seg[-1]
    frames = last["f"] - first["f"]
    if frames < min_frames:
        raise cct.Refusal("segment %s spans %d frames (< %d)" % (name, frames, min_frames))
    i = 0 if axis == "cols" else 1
    tiles = abs(last["cam"][i] - first["cam"][i])
    return tiles, frames, tiles / (frames * dt), tiles / frames


def judge(run_dir, dt_us, expect, tol, retail, say=print, min_frames=MIN_FRAMES):
    samples, _q, markers = cct.parse(run_dir)
    dt = dt_us / 1e6
    ok = True
    for name, axis in SEGMENTS:
        tiles, frames, per_s, per_f = segment_rate(samples, markers, name, axis, dt, min_frames)
        if retail:
            good = abs(per_f - 1.0) <= 0.05
            want = "1 tile per held frame (+-5%)"
        else:
            target = expect[axis]
            good = abs(per_s - target) <= tol
            want = "%.2f +- %.2f %s/s" % (target, tol, axis)
        say(
            "  %-5s %3d tiles over %4d frames (dt %d us): %.2f %s/s, %.3f tiles/frame -- want %s: %s"
            % (name, tiles, frames, dt_us, per_s, axis, per_f, want, "ok" if good else "FAIL")
        )
        ok = ok and good
    return ok


def parse_expect(text):
    out = {}
    for part in text.split(","):
        k, _, v = part.partition("=")
        out[k.strip()] = float(v)
    for k in ("cols", "rows"):
        if k not in out:
            raise SystemExit("--expect needs cols= and rows=")
    return out


def _plant(path, dt, cols_per_s, rows_per_s, retail, frames=360):
    """A synthetic mh_uidrive.log: three segments of `frames` presents at 60 samples apart."""
    lines = []
    f = 0
    cam = [10, 10]
    for name, axis in SEGMENTS:
        lines.append("; [script] LOG: tact_scroll: %s start" % name)
        rate = 1.0 / dt if retail else (cols_per_s if axis == "cols" else rows_per_s)
        per_frame = 1.0 if retail else rate * dt
        acc = 0.0
        step = -1 if name == "left" else 1
        i = 0 if axis == "cols" else 1
        for n in range(frames):
            f += 1
            acc += per_frame
            while acc >= 1.0:
                acc -= 1.0
                cam[i] += step
            if n % 60 == 0:
                lines.append(
                    "; [mtrace] f=%d produced=0 depth=0 max=0 di=on last=0,0 cur=320,240 vis=1 edge=---- "
                    "held=---- cam=%d,%d camd=1 gest=00" % (f, cam[0], cam[1])
                )
        lines.append("; [script] LOG: tact_scroll: %s end" % name)
    with open(os.path.join(path, cct.UIDRIVE_LOG), "w") as fh:
        fh.write("\n".join(lines) + "\n")


def selftest():
    bad = 0
    sink = []
    with tempfile.TemporaryDirectory() as d:
        exp = {"cols": 15.0, "rows": 20.0}
        for dt_us in (16667, 33333):
            dt = dt_us / 1e6
            _plant(d, dt, 15.0, 20.0, False, frames=int(5 / dt))
            if not judge(d, dt_us, exp, 2.0, False, say=sink.append):
                print("SELFTEST FAIL: paced log red at dt %d" % dt_us)
                bad += 1
            if judge(d, dt_us, exp, 2.0, True, say=sink.append):
                print("SELFTEST FAIL: paced log passed the retail check at dt %d" % dt_us)
                bad += 1
            _plant(d, dt, 0, 0, True, frames=int(5 / dt))
            if judge(d, dt_us, exp, 2.0, False, say=sink.append):
                print("SELFTEST FAIL: retail log passed the paced check at dt %d" % dt_us)
                bad += 1
            if not judge(d, dt_us, exp, 2.0, True, say=sink.append):
                print("SELFTEST FAIL: retail log red on the retail check at dt %d" % dt_us)
                bad += 1
        # absence refuses
        open(os.path.join(d, cct.UIDRIVE_LOG), "w").write("nothing here\n")
        try:
            judge(d, 16667, exp, 2.0, False, say=sink.append)
            print("SELFTEST FAIL: an empty log did not refuse")
            bad += 1
        except cct.Refusal:
            pass
    print("check_tact_scroll selftest: %s" % ("PASS" if not bad else "FAIL (%d)" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dir", nargs="*")
    ap.add_argument("--dt-us", type=int, default=16667)
    ap.add_argument("--expect", default="cols=15,rows=20")
    ap.add_argument("--tol", type=float, default=2.0)
    ap.add_argument("--retail", action="store_true")
    ap.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("a run or lane folder is required")
    try:
        run = cct.newest_run(a.run_dir[0])
        ok = judge(run, a.dt_us, parse_expect(a.expect), a.tol, a.retail, min_frames=a.min_frames)
    except cct.Refusal as e:
        print("check_tact_scroll: REFUSED -- %s" % e)
        return 1
    print("check_tact_scroll: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
