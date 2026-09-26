"""Where does a tactical replay's wall time actually go?

WHY DIFFERENTIAL AND NOT A PROFILER. `wpr.exe` is present on this machine but the analyzer half of
the Windows Performance Toolkit is not, so an ETL can be recorded and not read. That is a worse
position than it sounds: a sampling profile of a 32-bit Watcom binary with no symbols would name
addresses in `mh.exe` that still have to be resolved against Ghidra by hand. The rig already has a
better instrument -- it can turn its own features OFF and re-measure. Each row below differs from
the one above it by exactly one knob, so the delta between two rows IS the cost of that knob, with
no attribution step and no symbols required.

WHAT IS BEING MEASURED. The clock is PINNED (`pin_wallclock=1`, `pin_clock_dt_us=16667`), so the
simulation advances a fixed 16.667 ms per frame regardless of how long that frame really took. Frame
rate here is therefore pure throughput -- how fast the machine can grind a deterministic workload --
and not a frame rate anyone experiences. That is the number that decides how long the gate takes.

Run:  python tools/tact_profile.py [--frames N] [--save 11]
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tact_test  # noqa: E402
import ui_suite_common  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JOURNAL = os.path.join(REPO, "tools", "uiscripts", "journals", "poz1-combat.journal")


def frames_in(run_dir):
    """SIMULATED frames, from the last `T` line's frame INDEX -- not the number of T lines.

    The two differ by exactly the sampling rate, which is the variable under test here: at
    tact_hash_step=16 the run simulates 3,000 frames and logs 187 of them. Counting lines made the
    sparser configuration look 16x slower than it is, i.e. it reported the instrument's output rate
    as the machine's throughput. Exactly the metric this tool exists to separate."""
    last, exited = -1, None
    with open(os.path.join(run_dir, "mh_harness.log"), encoding="utf-8", errors="replace") as f:
        for ln in f:
            if ln.startswith("T "):
                try:
                    last = int(ln.split()[1])
                except (IndexError, ValueError):
                    pass
            elif ln.startswith("; TACT EXIT after frame "):
                exited = int(ln.split()[5])
    # PREFER THE EXIT LINE. At a large stride the run logs one `T` and simulates thousands, so the
    # T-line evidence understates the work by the sampling rate -- which is the variable under test.
    # The harness states plainly how far it got; use that when it did.
    return exited if exited is not None else last + 1


def log_bytes(run_dir):
    try:
        return os.path.getsize(os.path.join(run_dir, "mh_harness.log"))
    except OSError:
        return 0


def one(runner, lane_dir, label, frames, save, **cfg):
    """Run one configuration and return its measured throughput."""
    tact_test.tact_write_config(
        lane_dir,
        cfg.pop("stop", frames),
        0,
        -1,
        8,
        100,
        1,
        None,
        0,
        **cfg,
    )
    t0 = time.time()
    run_dir = tact_test.tact_run_arm(lane_dir, save, 900, runner)
    wall = time.time() - t0
    if not run_dir:
        return {"label": label, "err": "no run folder"}
    n = frames_in(run_dir)
    return {
        "label": label,
        "frames": n,
        "wall": wall,
        "fps": (n / wall) if wall > 0 else 0,
        "us_per_frame": (wall * 1e6 / n) if n else 0,
        "log_mb": log_bytes(run_dir) / 1e6,
        "run_dir": run_dir,
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--frames",
        type=int,
        default=3000,
        help="frames per configuration (default 3000 -- enough to swamp the ~9 s of "
        "launch+menu+mission-load that every row pays equally)",
    )
    # NAMED --tact-save so the Namespace this builds is shape-compatible with test_ui's own args:
    # tact_provision_lane reads args.tact_save / args.tact_system / args.visible directly.
    ap.add_argument("--tact-save", default="11")
    ap.add_argument("--tact-system", default=None)
    ap.add_argument(
        "--sample",
        type=int,
        default=0,
        metavar="HZ",
        help="also take a flat EIP profile of the game thread at HZ samples/s and print where the "
        "time is by module and address (harness.cpp prof_start). 1000 is a good starting point.",
    )
    ap.add_argument("--visible", action="store_true")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--desktop", default="mh_rig")
    ap.add_argument("--no-desktop", action="store_true")
    a = ap.parse_args()

    runner = ui_suite_common.RunnerConfig(desktop="" if a.no_desktop else a.desktop)
    if runner.desktop:
        print("[rig] isolated desktop: %s" % runner.desktop)

    lane_dir = tact_test.tact_provision_lane(a)
    if lane_dir is None:
        return 1

    # EACH ROW DIFFERS FROM ITS NEIGHBOUR BY ONE KNOB. Read the table by SUBTRACTING adjacent rows;
    # the absolute numbers include a fixed launch+menu+mission-load cost that every row pays.
    rows = []
    # NOT hash_step=0: that uninstalls the tactical frame hook entirely, which also removes the
    # step counter that tact_exit_at reads -- so the run never ends and the row measures the wall
    # clock instead of the game (it burned the full 900 s the first time this was run). A very large
    # stride keeps the hook, the counter and the exit condition while logging one line in 100k.
    if a.sample:
        # ONE configuration, sampled -- the differential table prices a KNOB, this prices an
        # ADDRESS. Running the sampler across all four rows would just repeat the same answer at
        # four different amplitudes.
        r = one(
            runner,
            lane_dir,
            "sampled: hashing EVERY frame",
            a.frames,
            a.tact_save,
            hash_step=1,
            profile_hz=a.sample,
        )
        print("")
        print(
            "%-38s %8d frames in %.1f s (%.1f frames/s)"
            % (r["label"], r.get("frames", 0), r.get("wall", 0), r.get("fps", 0))
        )
        run = r.get("run_dir")
        if run:
            for ln in open(os.path.join(run, "mh_harness.log"), encoding="utf-8", errors="replace"):
                if ln.startswith("; PROF"):
                    print("  " + ln.strip().lstrip("; "))
        return 0

    rows.append(
        one(
            runner,
            lane_dir,
            "sim + frame hook, no logging",
            a.frames,
            a.tact_save,
            hash_step=100000,
        )
    )
    rows.append(
        one(
            runner,
            lane_dir,
            "+ hashing, logged every 16th frame",
            a.frames,
            a.tact_save,
            hash_step=16,
        )
    )
    rows.append(
        one(runner, lane_dir, "+ hashing, logged EVERY frame", a.frames, a.tact_save, hash_step=1)
    )
    if os.path.isfile(JOURNAL):
        rows.append(
            one(
                runner,
                lane_dir,
                "+ journal replay (input injection)",
                a.frames,
                a.tact_save,
                hash_step=1,
                journal=JOURNAL,
            )
        )

    print("")
    print(
        "%-38s %8s %8s %9s %10s %8s"
        % ("configuration", "frames", "wall s", "frames/s", "us/frame", "log MB")
    )
    print("-" * 88)
    prev = None
    for r in rows:
        if "err" in r:
            print("%-38s  FAILED: %s" % (r["label"], r["err"]))
            continue
        print(
            "%-38s %8d %8.1f %9.1f %10.0f %8.1f"
            % (r["label"], r["frames"], r["wall"], r["fps"], r["us_per_frame"], r["log_mb"])
        )
        if prev and prev.get("us_per_frame"):
            d = r["us_per_frame"] - prev["us_per_frame"]
            print("%-38s %8s %8s %9s %10s" % ("  ^ cost of this knob", "", "", "", "%+.0f us" % d))
        prev = r
    print("")
    print("The clock is PINNED, so these are THROUGHPUT numbers, not a playable frame rate:")
    print("the sim advances a fixed 16.667 ms per frame no matter how long the frame really took.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
