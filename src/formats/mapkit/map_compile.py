"""CLI: edited Tiled projects (.tmj) -> compiled map overlay for build_mod.

  python -m src.formats.mapkit.map_compile [--src mod_src/map] [--out mod_src/overlay]

Reads every <src>/*.tmj, dispatches by its mapkit_format (strategic .MP or tactical
ALIEN_*.MAP), recompiles byte-faithfully, and writes <out>/mh/<stem>.<MP|MAP> in
build_mod.py's --overlay shape. Full loop:

  map_decompile -> edit mod_src/map/*.tmj in Tiled -> map_compile
                -> build_mod --map-src mod_src/map -> launch
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

from . import strategic, tactical, tiled_strat, tiled_tact


def _format(tmj):
    for p in tmj.get("properties", []):
        if p.get("name") == "mapkit_format":
            return p.get("value")
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", default="mod_src/map")
    ap.add_argument("--out", default="mod_src/overlay")
    args = ap.parse_args(argv)

    tmjs = sorted(glob.glob(os.path.join(args.src, "*.tmj")))
    if not tmjs:
        print(f"no .tmj files in {args.src}")
        return 1
    out_mh = os.path.join(args.out, "mh")
    os.makedirs(out_mh, exist_ok=True)

    n = 0
    for tmj_path in tmjs:
        with open(tmj_path, "r", encoding="utf-8") as fh:
            tmj = json.load(fh)
        fmt = _format(tmj)
        stem = os.path.splitext(os.path.basename(tmj_path))[0]
        try:
            if fmt == tiled_tact.FORMAT:
                model = tiled_tact.import_(tmj)
                out_path = os.path.join(out_mh, stem + ".MAP")
                tactical.write_file(model, out_path)
                note = f"128x128 tactical, {sum(model.impassable)} impassable"
            else:
                model = tiled_strat.import_(tmj)
                for tile, cnt in strategic.colocated_tiles(model.objects):
                    print(f"  WARNING {os.path.basename(tmj_path)}: tile {tile} holds {cnt} "
                          f"unchained trees; the game renders one per tile - spread them out")
                out_path = os.path.join(out_mh, stem + ".MP")
                strategic.write_file(model, out_path)
                note = f"{model.width}x{model.height}, {len(model.objects)} trees"
        except ValueError as e:
            print(f"  {os.path.basename(tmj_path)}: {e}")
            return 1
        n += 1
        print(f"  {os.path.basename(tmj_path):16} -> mh/{os.path.basename(out_path)}  ({note})")

    print(f"compiled {n} map(s) -> {args.out}")
    print(f"next: python src/formats/build_mod.py --overlay {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
