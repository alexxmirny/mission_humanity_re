"""Checkpoint: .MP -> Tiled -> .MP round-trip fidelity + edit-lands-exactly test.

  python -m src.formats.mapkit.roundtrip [--clean DIR]

Gate A: for every shipped .MP, parse -> export -> json dump/load -> import -> emit
        == original bytes (proves the Tiled+sidecar mechanism is lossless).
Gate M: deliberately retile one cell in the exported .tmj, recompile, and assert the
        emitted bytes differ ONLY in that tile's main-plane flags word, by exactly the
        expected delta (guards against reshape/offset bugs a coarse diff would miss)."""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

from . import strategic, tiled_strat, tlo
from .common import CLEAN
from .map_decompile import _find_tlo


def _export_tmj(mp, mh):
    m = strategic.parse(mp)
    tlo_model = tlo.parse(_find_tlo(mh, m.tlo_name))
    tmj, _img = tiled_strat.export(m, tlo_model, "x.png")
    return m, json.loads(json.dumps(tmj))  # force a real JSON round-trip


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clean", default=CLEAN)
    args = ap.parse_args(argv)
    mh = os.path.join(args.clean, "mh")
    maps = sorted(glob.glob(os.path.join(mh, "*.MP")))

    # Gate A -------------------------------------------------------------
    fails = 0
    for mp in maps:
        with open(mp, "rb") as fh:
            original = fh.read()
        _m, tmj = _export_tmj(mp, mh)
        got = strategic.emit(tiled_strat.import_(tmj))
        if got != original:
            fails += 1
            off = next((i for i in range(min(len(got), len(original)))
                        if got[i] != original[i]), min(len(got), len(original)))
            print(f"  FAIL A {os.path.basename(mp)}: first diff at byte 0x{off:x} "
                  f"({len(original)} vs {len(got)} bytes)")
    print(f"Gate A (Tiled round-trip): {len(maps) - fails}/{len(maps)} byte-identical")

    # Gate M: mutation lands exactly ------------------------------------
    mp = maps[0]
    with open(mp, "rb") as fh:
        original = fh.read()
    m, tmj = _export_tmj(mp, mh)
    idx = 10 * m.width + 10                      # cell (x=10, y=10), row-major
    layer = next(l for l in tmj["layers"] if l["name"] == "ground")
    old_gid = layer["data"][idx]
    old_low = (old_gid - tiled_strat.GROUND_FIRSTGID) & strategic.GROUND_MASK
    new_low = (old_low + 1) % 1000               # a different, in-range tile id
    layer["data"][idx] = tiled_strat.GROUND_FIRSTGID + new_low
    mutated = strategic.emit(tiled_strat.import_(tmj))

    flags_off = strategic.HEADER_LEN + idx * 4   # the u16 flags of that tile
    diffs = [i for i in range(len(original)) if original[i] != mutated[i]]
    expected = [flags_off, flags_off + 1]        # low u16 only (may be 1 byte if hi==lo byte)
    orig_flags = struct.unpack_from("<H", original, flags_off)[0]
    new_flags = struct.unpack_from("<H", mutated, flags_off)[0]
    only_here = all(d in (flags_off, flags_off + 1) for d in diffs)
    low_changed = (new_flags & strategic.GROUND_MASK) == new_low
    hi_preserved = (new_flags & ~strategic.GROUND_MASK) == (orig_flags & ~strategic.GROUND_MASK)
    m_ok = only_here and low_changed and hi_preserved and len(diffs) >= 1
    print(f"Gate M (mutation): {'PASS' if m_ok else 'FAIL'} "
          f"(diffs at {[hex(d) for d in diffs]}, expected within {[hex(e) for e in expected]}; "
          f"flags 0x{orig_flags:04x}->0x{new_flags:04x})")

    # Gate E: edits to each editable layer flow through import ----------
    m0 = strategic.parse(maps[0])
    _m, tmj = _export_tmj(maps[0], mh)
    lyr = {l["name"]: l for l in tmj["layers"]}
    # passable toggle at (col=5,row=7)
    pf = tiled_strat._tileset_firstgid(tmj, "passable")
    ti = 7 * m0.width + 5
    orig_p = (lyr["passable"]["data"][ti] - pf) & 1
    lyr["passable"]["data"][ti] = pf + (0 ^ orig_p ^ 1)
    # resource amount0 +7 on first deposit; player_start status -> -2 on first
    if lyr["resources"]["objects"]:
        lyr["resources"]["objects"][0]["properties"][0]["value"] += 7
    if lyr["player_starts"]["objects"]:
        lyr["player_starts"]["objects"][0]["properties"][0]["value"] = -2
    m2 = tiled_strat.import_(tmj)
    f = 5 * m0.height + 7
    e_pass = m2.passable[f] == (orig_p ^ 1)
    e_res = (not m0.resources_raw) or (m2.resources_raw != m0.resources_raw)
    e_start = (m0.f3 == 0) or struct.unpack_from("<3i", m2.tail_raw, 0)[2] == -2
    e_ok = e_pass and e_res and e_start
    print(f"Gate E (layer edits): {'PASS' if e_ok else 'FAIL'} "
          f"(passable={e_pass}, resources={e_res}, player_start={e_start})")

    # Gate T: tactical ALIEN_*.MAP -> Tiled (walls as objects) -> .MAP byte-identical
    from . import tactical, tiled_tact, treebank
    podloga = _find_tlo(mh, "PODLOGA.TLO")
    t_fail = t_n = 0
    walls_mode = "sidecar"
    if podloga:
        tm = tlo.parse(podloga)
        for amap in sorted(glob.glob(os.path.join(mh, "ALIEN_*.MAP"))):
            t_n += 1
            with open(amap, "rb") as fh:
                raw = fh.read()
            m = tactical.parse(amap)
            used = {v - 100 for off in range(0, tactical.H_LEN, 2)
                    for v in [struct.unpack_from("<h", m.height_raw, off)[0]] if v}
            ws = treebank.load_wall_sprites(used)
            if ws:
                walls_mode = "objects"
            tmj, _ = tiled_tact.export(m, tm, "p.png", ws)
            got = tactical.emit(tiled_tact.import_(json.loads(json.dumps(tmj))))
            if got != raw:
                t_fail += 1
                print(f"  FAIL T {os.path.basename(amap)}")
    print(f"Gate T (tactical round-trip, walls={walls_mode}): {t_n - t_fail}/{t_n} byte-identical")

    ok = fails == 0 and m_ok and e_ok and t_fail == 0
    print("RESULT:", "all round-trip gates pass" if ok else "FAILURES above")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
