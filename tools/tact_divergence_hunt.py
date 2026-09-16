#!/usr/bin/env python3
"""Reproduce the intermittent tactical A/B divergence, and measure it as a RATE.

PERSISTENT (TACT-REC / the tactical-probe work 9, 9k, 9l). Promoted out of tools/oneoff/ on
2026-08-25: it has been the measuring instrument for the tactical determinism blocker across four
sessions, it is what turns "a run went green" into a rate with an N beside it, and every claim about
that blocker in the tracker traces to a number this script printed. A one-off is something you throw
away after the answer; this is the thing that produces the answer.

WHY A LOOP AND NOT ANOTHER HYPOTHESIS. Five explanations have been tested for this divergence and
four were wrong -- process teardown, the self-stop, the event ring, the DirectInput buffer, the wall
clock feeding UI-only readers, and physical mouse input are all excluded. A single run answers
nothing because the fault fires roughly one time in three: a green run is the MAJORITY outcome even
when the fault is present, which is exactly how it survived from TACT-SYNTH (observed once at frame
62, seed lost, never reproduced) to now. So measure a RATE over N runs, and keep the logs of the runs
that fire.

WHAT IT DISCRIMINATES. `--dt` sets [harness] pin_clock_dt_us, the per-frame advance of the pinned
wall clock. Tactical reads time_GetCurrentTime from 57 sites across 20 functions
(llm_tact_unit_update_anim alone has 14), so it IS the tactical timebase -- but the pin makes it
frame-quantised, hence constant within a frame. `--dt 0` FREEZES it outright: every read returns the
same value for the whole run. If the divergence rate is unchanged at dt=0, the clock is exonerated
however heavily it is read; if it drops to zero, it is implicated and the pin is not sufficient.

Run the same N at each dt before comparing -- with a ~1-in-3 rate, 3 runs cannot tell 0% from 33%.

    python tools/tact_divergence_hunt.py --runs 8 --dt 16667
    python tools/tact_divergence_hunt.py --runs 8 --dt 0
"""

import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)  # this file sits directly in the tools tree, one level below the root
sys.path.insert(0, os.path.join(REPO, "tools"))

import test_ui  # noqa: E402


def unit_hashes(run_dir):
    """{frame: [per-record hash]} from the `TU` lines (Sect. 9f), or {} if unit hashing was off."""
    out = {}
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("TU "):
                fld = line.split()
                out[int(fld[1])] = fld[2:]
    return out


def locate_unit(a_dir, b_dir, frame):
    """Which tact_unit_record indices differ at `frame`, and when each first differed.

    This is the whole point of the per-record line: `tact_units` is 129 records, and a divergence
    that names the REGION is not something anyone can act on."""
    au, bu = unit_hashes(a_dir), unit_hashes(b_dir)
    if frame not in au or frame not in bu:
        return None, []
    at, bt = au[frame], bu[frame]
    idx = [i for i in range(min(len(at), len(bt))) if at[i] != bt[i]]
    firsts = []
    for i in idx:
        for f in sorted(set(au) & set(bu)):
            if i < len(au[f]) and i < len(bu[f]) and au[f][i] != bu[f][i]:
                firsts.append((i, f))
                break
    return idx, firsts


def detail(run_dir):
    """{(frame, unit): [chunk hashes]} from the `TD` lines -- the byte-offset level."""
    out = {}
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("TD "):
                fld = line.split()
                out[(int(fld[1]), int(fld[2]))] = fld[3:]
    return out


def locate_bytes(a_dir, b_dir, frame, units, chunk=32):
    """Which BYTE RANGES of which units differ at `frame`. This is the level that names a field."""
    ad, bd = detail(a_dir), detail(b_dir)
    hits = []
    for u in units:
        ac, bc = ad.get((frame, u)), bd.get((frame, u))
        if not ac or not bc:
            continue
        for c in range(min(len(ac), len(bc))):
            if ac[c] != bc[c]:
                hits.append((u, c * chunk, c * chunk + chunk - 1))
    return hits


def gates(run_dir):
    """{frame: {unit: dict}} from the `TG` lines -- the raw gate fields."""
    out = {}
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("TG "):
                continue
            fld = line.split()
            frame = int(fld[1])
            per = {}
            for tok in fld[2:]:
                idx, rest = tok.split(":", 1)
                d = {}
                for kv in rest.split(","):
                    # keys are 1 or 2 letters ("op"), so split on the first digit rather than
                    # assuming a single-character key -- `op0` parsed as key "o", value "p0".
                    i = 0
                    while i < len(kv) and not (kv[i].isdigit() or kv[i] == "-"):
                        i += 1
                    d[kv[:i]] = int(kv[i:], 16 if kv[:i] == "s" else 10)
                per[int(idx)] = d
            out[frame] = per
    return out


def stop_index(per):
    """The unit index at which llm_tact_unit_owner_tick(owner=0) RETURNS early, or None.

    Derived HERE rather than in the DLL, so the instrument does not contain a copy of the logic under
    investigation. G1 (owner/type) is a per-unit SKIP; G2 (FIRE bit) and G3 (queue-head busy) are an
    early RET that truncates every higher index -- the tactical-probe work Sect. 9i."""
    for u in sorted(per):
        d = per[u]
        if d["t"] == 0 or d["o"] != 0:
            continue  # G1: skip this unit, keep walking
        if d["s"] & 8:
            return u, "G2-fire"
        if d["op"] != 0 and d["f"] == 0 and d["w"] == 0:
            return u, "G3-queue"
    return None, ""


def compare_gates(a_dir, b_dir):
    """First frame whose derived stop index differs, with both sides' state."""
    ga, gb = gates(a_dir), gates(b_dir)
    for f in sorted(set(ga) & set(gb)):
        sa, ra = stop_index(ga[f])
        sb, rb = stop_index(gb[f])
        if (sa, ra) != (sb, rb):
            return f, (sa, ra, ga[f]), (sb, rb, gb[f])
    return None, None, None


def hashes(run_dir):
    """{frame: combined}, {frame: [per-region]}, {frame: sim} from one arm.

    `sim` is the `TS` line -- the same fold as `combined` with tact_units replaced by its alias
    tact_units_sim, i.e. the roster WITHOUT the render-written animation window. Empty against a DLL
    predating 2026-08-25, which `compare` reports rather than silently treating as agreement."""
    combined, per, sim = {}, {}, {}
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return combined, per, sim
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("T "):
                fld = line.split()
                combined[int(fld[1])] = fld[2]
            elif line.startswith("TS "):
                fld = line.split()
                sim[int(fld[1])] = fld[2]
            elif line.startswith("TR "):
                fld = line.split()
                per[int(fld[1])] = fld[2:]
    return combined, per, sim


def field_names(run_dir):
    """The `; TF FIELDS` header the DLL emits once: the column labels for the TF lines."""
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("; TF FIELDS"):
                return line.split()[3:]
    return []


def field_hashes(run_dir):
    """{(frame, unit): [per-field hash]} from the `TF` lines, or {} if field hashing was off."""
    out = {}
    path = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("TF "):
                fld = line.split()
                out[(int(fld[1]), int(fld[2]))] = fld[3:]
    return out


def locate_fields(a_dir, b_dir, frame):
    """[(unit, [field names])] at `frame` -- the resolution --detail cannot reach.

    A 32-byte chunk straddles cmd_wait_until_time, which the SIM slice keeps, and anim_frame_time,
    which it drops. So "chunk 1 differs" is compatible with both a presentation-only drift and a real
    one, and only a per-field line settles which."""
    names = field_names(a_dir) or field_names(b_dir)
    af, bf = field_hashes(a_dir), field_hashes(b_dir)
    hits = []
    for u in sorted({k[1] for k in af if k[0] == frame}):
        a, b = af.get((frame, u)), bf.get((frame, u))
        if not a or not b:
            continue
        d = [
            names[i] if i < len(names) else "col%d" % i
            for i in range(min(len(a), len(b)))
            if a[i] != b[i]
        ]
        if d:
            hits.append((u, d))
    return hits


# The five fields state/region_view.h emit_tact_units_sim() marks local(). Kept here as NAMES rather
# than offsets so the offline report can say "everything that differs is excluded" in the same
# vocabulary the DLL prints.
PRESENTATION_FIELDS = {
    "anim_frame_t",
    "frame_index",
    "anim_cycle_t",
    "frame_interval",
    "sprite_id",
}


def compare(a_dir, b_dir, which="full"):
    """(first_diverging_frame, [region names], compared, differing) or (None, [], n, 0).

    `which` picks the verdict: "full" = the combined `T` hash, "sim" = the `TS` hash. The region
    names come off the `TR` line either way, since that line carries tact_units and tact_units_sim as
    separate positional columns."""
    ac, ap, asim = hashes(a_dir)
    bc, bp, bsim = hashes(b_dir)
    if which == "sim":
        ac, bc = asim, bsim
        if not ac or not bc:
            # NOT "identical". An absent verdict compares equal over an empty intersection, which is
            # the vacuous-pass shape this whole investigation keeps tripping over.
            return "MISSING", [], 0, 0
    common = sorted(set(ac) & set(bc))
    diff = [f for f in common if ac[f] != bc[f]]
    if not diff:
        return None, [], len(common), 0
    f0 = diff[0]
    names = []
    if f0 in ap and f0 in bp:
        names = [
            test_ui.TACT_REGION_NAMES[i]
            for i in range(min(len(ap[f0]), len(bp[f0]), len(test_ui.TACT_REGION_NAMES)))
            if ap[f0][i] != bp[f0][i]
        ]
    return f0, names, len(common), len(diff)


ARCHIVE = os.path.join(REPO, "tmp", "tact_divergence")


def archive(run_dirs, tag):
    """COPY a diverging pair out of the lane. The lane is reprovisioned per invocation, which DELETES
    logs/ -- so a pair left in place survives only until the next sweep, and the first captured pair
    was lost exactly that way (2026-08-24), the same shape of loss as TACT-SYNTH's lost seed. A
    warning printed to a human is not a mechanism; copying is."""
    dst = os.path.join(ARCHIVE, tag)
    os.makedirs(dst, exist_ok=True)
    for name, src in zip(("a", "b"), run_dirs):
        shutil.copytree(src, os.path.join(dst, name), dirs_exist_ok=True)
    return dst


def main():
    ap_ = argparse.ArgumentParser()
    ap_.add_argument("--runs", type=int, default=8)
    ap_.add_argument("--frames", type=int, default=3000)
    ap_.add_argument("--wall", type=int, default=60)
    ap_.add_argument(
        "--dt", type=int, default=16667, help="[harness] pin_clock_dt_us; 0 FREEZES it"
    )
    ap_.add_argument("--save", default="11")
    ap_.add_argument(
        "--unit-hash",
        action="store_true",
        help="emit per-tact_unit_record hashes so a divergence names the unit INDEX, not just the "
        "region. ~1.2 KB of log per frame per arm.",
    )
    ap_.add_argument(
        "--gates",
        default="",
        metavar="LO-HI",
        help="log llm_tact_unit_owner_tick's gate fields for units LO..HI and report the first frame "
        "whose derived early-RETURN index differs between the arms. e.g. --gates 1-8.",
    )
    ap_.add_argument(
        "--detail",
        default="",
        metavar="LO-HI",
        help="also chunk-hash units LO..HI (32-byte chunks) so a divergence names a BYTE OFFSET and "
        "therefore a field. e.g. --detail 1-8 for the player squad. ~3.5 KB/frame for eight units.",
    )
    ap_.add_argument(
        "--fields",
        default="",
        metavar="LO-HI",
        help="also FIELD-hash units LO..HI, so a divergence names a field by NAME. This is the "
        "resolution --detail cannot reach: a 32-byte chunk straddles cmd_wait_until_time (which the "
        "SIM slice keeps) and anim_frame_time (which it drops), so a chunk verdict cannot say which "
        "side of the sim/presentation line a divergence fell on. e.g. --fields 1-8.",
    )
    args = ap_.parse_args()

    class A:
        tact_save = args.save
        visible = False

    dlo, dhi = 0, 0
    if args.detail:
        dlo, dhi = (int(x) for x in args.detail.split("-", 1))
    glo, ghi = 0, 0
    if args.gates:
        glo, ghi = (int(x) for x in args.gates.split("-", 1))
    flo, fhi = 0, 0
    if args.fields:
        flo, fhi = (int(x) for x in args.fields.split("-", 1))
    lane_dir = test_ui.tact_provision_lane(A, visible=False)
    if lane_dir is None:
        return 1
    print("dt=%d  frames=%d  runs=%d\n" % (args.dt, args.frames, args.runs))

    fired = []
    for i in range(1, args.runs + 1):
        dirs = []
        for _arm in ("a", "b"):
            test_ui.tact_write_config(
                lane_dir,
                args.frames,
                0,
                -1,
                8,
                100,
                1,
                None,
                0,
                pin_dt_us=args.dt,
                unit_hash=1 if args.unit_hash else 0,
                detail_lo=dlo,
                detail_hi=dhi,
                gate_lo=glo,
                gate_hi=ghi,
                field_lo=flo,
                field_hi=fhi,
            )
            d = test_ui.tact_run_arm(lane_dir, args.save, args.wall)
            if not d:
                print("run %d: arm produced no run folder" % i)
                break
            dirs.append(d)
        if len(dirs) != 2:
            continue
        f0, names, compared, differing = compare(dirs[0], dirs[1], "full")
        s0, snames, _sc, sdiffering = compare(dirs[0], dirs[1], "sim")
        if s0 == "MISSING":
            print(
                "run %2d/%d  NO `TS` LINE -- this DLL predates the sim verdict (2026-08-25). "
                "Rebuild before reading anything below as a sim result." % (i, args.runs)
            )
            s0 = None
        if f0 is None:
            print("run %2d/%d  IDENTICAL  (%d frames compared)" % (i, args.runs, compared))
        else:
            kept = archive(dirs, "dt%d_run%02d_f%d" % (args.dt, i, f0))
            fired.append((i, f0, names, kept, s0))
            # THE VERDICT THAT MATTERS IS THE SIM ONE, and it is printed as its own line rather than
            # folded into the headline, because the two answer different questions: FULL asks "did
            # anything in the tactical arena differ", SIM asks "did anything a reimplementation or a
            # journal replay depends on differ". A run that is FULL-red and SIM-green is presentation
            # drift -- real, reported, and not a simulation fault.
            verdict = (
                "SIM-CLEAN (presentation drift)"
                if s0 is None
                else "SIM DIVERGED at frame %d (%d frames)" % (s0, sdiffering)
            )
            print(
                "run %2d/%d  DIVERGED at frame %d  regions=%s  (%d of %d frames differ)  -> %s"
                % (i, args.runs, f0, names or ["?"], differing, compared, verdict)
            )
            if s0 is not None and snames:
                print("           sim regions at f=%d: %s" % (s0, snames))
            print("           archived -> %s" % kept)
            if fhi:
                hits = locate_fields(dirs[0], dirs[1], f0)
                if not hits:
                    print("           (no TF coverage at that frame)")
                else:
                    every = sorted({n for _u, ns in hits for n in ns})
                    print("           FIELDS differing at f=%d:" % f0)
                    for u, ns in hits:
                        print("              unit %-3d %s" % (u, ", ".join(ns)))
                    outside = [n for n in every if n not in PRESENTATION_FIELDS]
                    if outside:
                        print(
                            "           >> %d field(s) OUTSIDE the presentation set: %s"
                            % (len(outside), ", ".join(outside))
                        )
                    else:
                        print(
                            "           >> every differing field is in the presentation set "
                            "(%s) -- consistent with render-driven drift, not sim state."
                            % ", ".join(every)
                        )
            if ghi:
                gf, ga_, gb_ = compare_gates(dirs[0], dirs[1])
                if gf is None:
                    print("           GATES: derived stop index never differs (!)")
                else:
                    print("           GATES: first differing stop at frame %d" % gf)
                    print("              A stops at unit %s (%s)" % (ga_[0], ga_[1] or "-"))
                    print("              B stops at unit %s (%s)" % (gb_[0], gb_[1] or "-"))
                    who = ga_[0] if ga_[0] is not None else gb_[0]
                    if who is not None:
                        print("              A unit %d: %s" % (who, ga_[2].get(who)))
                        print("              B unit %d: %s" % (who, gb_[2].get(who)))
            if args.unit_hash:
                idx, firsts = locate_unit(dirs[0], dirs[1], f0)
                if idx is None:
                    print("           (no TU lines -- unit hashing produced nothing)")
                else:
                    print("           units differing AT f=%d: %s" % (f0, idx))
                    print("           earliest divergence per unit: %s" % firsts)
                    if dhi:
                        hits = locate_bytes(dirs[0], dirs[1], f0, idx)
                        if hits:
                            print("           BYTE RANGES differing at f=%d:" % f0)
                            for u, lo, hi in hits:
                                print(
                                    "              unit %-3d record[0x%03X..0x%03X]" % (u, lo, hi)
                                )
                        else:
                            print("           (no TD coverage for those units at that frame)")

    sim_red = [x for x in fired if x[4] is not None]
    print(
        "\nRATE: %d of %d runs diverged on the FULL hash (dt=%d)" % (len(fired), args.runs, args.dt)
    )
    print(
        "      %d of %d diverged on the SIM hash -- the verdict a shadow arm or a journal replay "
        "depends on" % (len(sim_red), args.runs)
    )
    if fired:
        print("first-divergence frames (full):", [f for _i, f, _n, _d, _s in fired])
    if sim_red:
        print("first-divergence frames (sim): ", [x[4] for x in sim_red])
    if fired:
        print("archived pairs (safe from the next reprovision):")
        for _i, _f, _n, kept, _s in fired:
            print("   " + kept)
    if fired and not sim_red:
        print(
            "\nEVERY divergence this sweep was FULL-only: the render-written animation window moved\n"
            "and nothing the sim slice hashes did. That is the presentation-drift reading -- state it\n"
            "with this N, not as a general claim."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
