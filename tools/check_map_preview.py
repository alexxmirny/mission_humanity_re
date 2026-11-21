#!/usr/bin/env python3
"""check_map_preview.py -- mods:MAPGEN2: did the map picker / lobby PREVIEW read the right data?

A capture proves a thumbnail was drawn; it does not prove the numbers behind it. mh.dll logs one line per preview build
(ui/map_preview.cpp):

    ; [mappreview] <file> <w>x<h> tlo=<tileset> spots=N ore=M trees=T build_us=U (tlo cached|loaded <us> us)

This reads ONE run directory's `mh_net.log` and, for every such line, re-parses the SAME file with an independent
implementation (struct-based, no shared code with mh_mapgen) and requires width, height, tileset, landing spots, ore
blocks and tree tiles to agree. The file is looked up beside the run (the rig lane's `Maps\\`), then in --maps-dir.
Relative --maps-dir paths are tried from the current directory first, then from the lane containing the run.

Modes
  check_map_preview.py <run-dir>                       armed: the `armed` line, >= --min-previews build lines, every one agrees
  check_map_preview.py --expect off <run-dir>          `[ui] map_preview=0`: the `off` line, and NO preview line at all
  check_map_preview.py --selftest                      planted logs: every negative goes RED

Absence is a failure: no log, no arm line, or too few build lines is never a pass.
"""

import argparse
import os
import re
import struct
import sys
import tempfile

LINE = re.compile(
    r"; \[mappreview\] (?P<file>.+?) (?P<w>\d+)x(?P<h>\d+) tlo=(?P<tlo>\S+) spots=(?P<spots>\d+) ore=(?P<ore>\d+) "
    r"trees=(?P<trees>\d+) build_us=(?P<us>\d+) \(tlo (?P<tl>cached|loaded)(?: (?P<tlus>\d+) us)?\)"
)
ARMED = re.compile(r"; \[mappreview\] armed \(")
OFF = re.compile(r"; \[mappreview\] off \(")


def parse_map(path):
    """(w, h, tileset, spots, ore_blocks, tree_tiles) of a .MP/.mpm, parsed straight off the documented layout."""
    b = open(path, "rb").read()
    assert b[:5] == b"ver02", "not a map: %s" % path
    w, h = struct.unpack_from("<II", b, 5)
    tlo = b[0x0D:0x2D].split(b"\0")[0].decode("ascii")
    f1, f2 = struct.unpack_from("<II", b, 0x35)
    f3 = struct.unpack_from("<i", b, 0x41)[0]
    off = 0x61 + w * h * 4 + w * h + f1 * 28 + (w // 2) * (h // 2) * 4
    trees = set()
    for i in range(f2):
        x, y = struct.unpack_from("<ii", b, off + i * 28 + 20)
        if 0 <= x < w and 0 <= y < h:
            trees.add((x, y))
    off += f2 * 28
    nb = (w // 4) * (h // 4)
    ore = struct.unpack_from("<%dH" % (nb * 8), b, off)
    ore_blocks = sum(1 for i in range(nb) if any(ore[i * 8 : i * 8 + 4]))
    off += nb * 16
    spots = 0
    for i in range(f3):
        x, y, _ = struct.unpack_from("<iii", b, off + 12 * i)
        if 0 <= x < w and 0 <= y < h:
            spots += 1
    return w, h, tlo, spots, ore_blocks, len(trees)


def find_map(name, run_dir, extra):
    # Process and session directories are both direct children of the lane's logs directory.
    lane = os.path.join(run_dir, "..", "..")
    cands = [os.path.join(lane, "Maps", name)]
    cands += [os.path.join(d, name) for d in extra]
    cands += [os.path.join(lane, d, name) for d in extra if not os.path.isabs(d)]
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


def check(run_dir, expect, min_previews, extra_dirs):
    log = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(log):
        return ["no mh_net.log in %s" % run_dir], []
    text = open(log, encoding="utf-8", errors="replace").read().splitlines()
    bad, info = [], []
    builds = [m for m in (LINE.search(l) for l in text) if m]
    if expect == "off":
        if not any(OFF.search(l) for l in text):
            bad.append("no `[mappreview] off` line: the knob was never read")
        if builds:
            bad.append("%d preview build line(s) with [ui] map_preview=0" % len(builds))
        return bad, info
    if not any(ARMED.search(l) for l in text):
        bad.append("no `[mappreview] armed` line (not armed, or the log is not this process's)")
    if len(builds) < min_previews:
        bad.append("%d preview build line(s), need >= %d" % (len(builds), min_previews))
    for m in builds:
        name = m.group("file")
        p = find_map(name, run_dir, extra_dirs)
        if not p:
            bad.append("%s: the file is not beside the run or in --maps-dir" % name)
            continue
        want = parse_map(p)
        got = (
            int(m.group("w")),
            int(m.group("h")),
            m.group("tlo"),
            int(m.group("spots")),
            int(m.group("ore")),
            int(m.group("trees")),
        )
        if want != got:
            bad.append("%s: log says %s, the file says %s" % (name, got, want))
        info.append(
            "%s %dx%d %s spots=%d ore=%d trees=%d build %d us (tlo %s%s)"
            % (
                name,
                got[0],
                got[1],
                got[2],
                got[3],
                got[4],
                got[5],
                int(m.group("us")),
                m.group("tl"),
                " %s us" % m.group("tlus") if m.group("tlus") else "",
            )
        )
    return bad, info


def selftest():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import struct as st

    d = tempfile.mkdtemp(prefix="cmp_")
    run = os.path.join(d, "lane", "logs", "run1")
    os.makedirs(run)
    os.makedirs(os.path.join(d, "lane", "Maps"))
    # a 8x8 map: one landing spot inside, one outside; one ore block; two tree records on ONE tile
    w = h = 8
    hdr = bytearray(0x61)
    hdr[0:5] = b"ver02"
    st.pack_into("<II", hdr, 5, w, h)
    hdr[0x0D : 0x0D + 9] = b"synth.tlo"
    st.pack_into("<I", hdr, 0x35, 0)
    st.pack_into("<I", hdr, 0x39, 2)
    st.pack_into("<i", hdr, 0x41, 2)
    body = bytes(hdr) + bytes(w * h * 4) + bytes(w * h) + bytes((w // 2) * (h // 2) * 4)
    for _ in range(2):
        body += st.pack("<Iiiiiii", 0, 0, 0, 0, 0, 3, 3)
    ore = [0] * ((w // 4) * (h // 4) * 8)
    ore[8 * 3 + 1] = 5
    body += st.pack("<%dH" % len(ore), *ore)
    body += st.pack("<iii", 2, 2, 0) + st.pack("<iii", 99, 1, 0)
    open(os.path.join(d, "lane", "Maps", "t.mpm"), "wb").write(body)
    assert parse_map(os.path.join(d, "lane", "Maps", "t.mpm")) == (8, 8, "synth.tlo", 1, 1, 1)

    def write(lines):
        open(os.path.join(run, "mh_net.log"), "w").write("\n".join(lines) + "\n")

    good = "[1] ; [mappreview] t.mpm 8x8 tlo=synth.tlo spots=1 ore=1 trees=1 build_us=10 (tlo loaded 5 us)"
    arm = "[0] ; [mappreview] armed ([ui] map_preview=1): description box 120 -> 130 px"
    failures = 0

    def expect_red(label, lines, **kw):
        nonlocal failures
        write(lines)
        bad, _ = check(run, kw.get("expect", "armed"), kw.get("min", 1), [])
        if not bad:
            print("selftest: %s was NOT red" % label)
            failures += 1

    write([arm, good])
    bad, _ = check(run, "armed", 1, [])
    if bad:
        print("selftest: the good log was red: %s" % bad)
        failures += 1
    expect_red("wrong spots", [arm, good.replace("spots=1", "spots=2")])
    expect_red("wrong ore", [arm, good.replace("ore=1", "ore=0")])
    expect_red("wrong tileset", [arm, good.replace("synth.tlo", "other.tlo")])
    expect_red("wrong trees", [arm, good.replace("trees=1", "trees=2")])
    expect_red("no build line", [arm])
    expect_red("not armed", [good])
    expect_red("missing file", [arm, good.replace("t.mpm", "gone.mpm")])

    # The same explicit relative directory works for both process and session logs.
    mod_dir = os.path.join(d, "lane", "mods", "fixture", "data", "maps")
    os.makedirs(mod_dir)
    mod_body = bytearray(body)
    mod_body[0x0D : 0x0D + 9] = b"other.tlo"
    with open(os.path.join(mod_dir, "mod.mpm"), "wb") as f:
        f.write(mod_body)
    # A different mod copy of t.mpm must not override the ordinary Maps copy.
    with open(os.path.join(mod_dir, "t.mpm"), "wb") as f:
        f.write(mod_body)
    mod_arg = os.path.join("mods", "fixture", "data", "maps")
    mod_good = good.replace("t.mpm", "mod.mpm").replace("synth.tlo", "other.tlo")
    session = os.path.join(d, "lane", "logs", "2026-11-02T12-00-00Z_ab12cd34_map_skirmish")
    os.makedirs(session)
    for target in (run, session):
        with open(os.path.join(target, "mh_net.log"), "w") as f:
            f.write("\n".join([arm, good, mod_good]) + "\n")
        bad, _ = check(target, "armed", 2, [mod_arg])
        if bad:
            print("selftest: lane-relative mod or ordinary priority was red: %s" % bad)
            failures += 1
        with open(os.path.join(target, "mh_net.log"), "w") as f:
            f.write("\n".join([arm, mod_good.replace("spots=1", "spots=2")]) + "\n")
        if not check(target, "armed", 1, [mod_arg])[0]:
            print("selftest: wrong lane-relative mod count was not red")
            failures += 1
        with open(os.path.join(target, "mh_net.log"), "w") as f:
            f.write("\n".join([arm, mod_good.replace("mod.mpm", "missing.mpm")]) + "\n")
        if not check(target, "armed", 1, [mod_arg])[0]:
            print("selftest: missing lane-relative mod map was not red")
            failures += 1
    write(["[0] ; [mappreview] off ([ui] map_preview=0): picker and lobby stay stock"])
    if check(run, "off", 0, [])[0]:
        print("selftest: the off log was red")
        failures += 1
    expect_red(
        "off with a build line",
        ["[0] ; [mappreview] off ([ui] map_preview=0): x", good],
        expect="off",
    )
    expect_red("off with no off line", [arm], expect="off")
    os.remove(os.path.join(run, "mh_net.log"))
    if not check(run, "armed", 1, [])[0]:
        print("selftest: a missing log was not red")
        failures += 1
    print("check_map_preview selftest: %s" % ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--expect", choices=("armed", "off"), default="armed")
    ap.add_argument("--min-previews", type=int, default=1)
    ap.add_argument(
        "--maps-dir",
        action="append",
        default=[],
        help="extra map directory (repeatable; relative to cwd, then the run's lane)",
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("a run directory is required")
    bad, info = check(a.run_dir, a.expect, a.min_previews, a.maps_dir)
    for i in info:
        print("  check_map_preview: " + i)
    for b in bad:
        print("  check_map_preview: BAD " + b)
    print("check_map_preview: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
