"""CLI: retail maps -> editable Tiled projects (.tmj + tileset PNG).

  python -m src.formats.mapkit.map_decompile [--clean DIR] [--out mod_src/map] [maps...]

Handles both strategic .MP planet maps (ground painted from the planet .TLO, trees as
real sprites, passable/resource/player-start layers) and tactical ALIEN_*.MAP mission
maps (ground from PODLOGA.TLO + collision layer). `maps` are basenames/globs; default =
all *.MP + ALIEN_*.MAP under <clean>/mh. Edit in Tiled, then map_compile."""
from __future__ import annotations

import argparse
import fnmatch
import glob
import json
import os
import struct
import sys

from . import strategic, tactical, tiled_strat, tiled_tact, tlo, treebank
from .common import CLEAN


def _find_tlo(mh_dir, tlo_name):
    want = (tlo_name if isinstance(tlo_name, str) else
            tlo_name.split(b"\x00")[0].decode("latin1")).lower()
    for f in os.listdir(mh_dir):
        if f.lower() == want:
            return os.path.join(mh_dir, f)
    return None


def _write_images(out, images, written):
    for name, im in images.items():
        if name not in written:
            dst = os.path.join(out, name)
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            im.save(dst)
            written.add(name)


def _decompile_strategic(mp, mh, args, written):
    m = strategic.parse(mp)
    tlo_path = _find_tlo(mh, m.tlo_name)
    if tlo_path is None:
        return f"SKIP: tileset {m.tlo_name!r} not found"
    tlo_model = tlo.parse(tlo_path)
    png_name = os.path.splitext(os.path.basename(tlo_path))[0].lower() + ".png"
    bank = args._bankmap.get(os.path.basename(mp).lower())
    tree_sprites = {}
    if bank is not None and m.objects:
        tree_sprites = treebank.load_tree_sprites(
            bank, [o["kind"] for o in m.objects], args.unpack_root)
    tmj, images = tiled_strat.export(m, tlo_model, png_name, tree_sprites)
    _write_images(args.out, images, written)
    art = f"bank{bank}" if tree_sprites else "no art"
    return tmj, f"{m.width}x{m.height}, {len(m.objects)} trees [{art}]"


def _decompile_tactical(mp, mh, args, written):
    m = tactical.parse(mp)
    tlo_path = _find_tlo(mh, "PODLOGA.TLO")
    if tlo_path is None:
        return "SKIP: PODLOGA.TLO not found"
    tlo_model = tlo.parse(tlo_path)
    used = {v - 100 for off in range(0, tactical.H_LEN, 2)
            for v in [struct.unpack_from("<h", m.height_raw, off)[0]] if v}
    wall_sprites = {} if args.no_trees else treebank.load_wall_sprites(used, args.unpack_root)
    tmj, images = tiled_tact.export(m, tlo_model, "podloga.png", wall_sprites)
    _write_images(args.out, images, written)
    walls = next((l for l in tmj["layers"] if l["name"] == "walls"), None)
    note = f", {len(walls['objects'])} wall objects" if walls else " (walls in sidecar - no BANK_120 art)"
    return tmj, f"128x128 tactical, {sum(m.impassable)} impassable{note}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clean", default=CLEAN)
    ap.add_argument("--out", default="mod_src/map")
    ap.add_argument("--unpack-root", default=treebank.UNPACK_ROOT,
                    help="mh_tools BANKI unpack tree for real tree-sprite art")
    ap.add_argument("--no-trees", action="store_true", help="skip rendering tree sprites")
    ap.add_argument("maps", nargs="*", help="basenames/globs; default = all *.MP + ALIEN_*.MAP")
    args = ap.parse_args(argv)
    mh = os.path.join(args.clean, "mh")
    os.makedirs(args.out, exist_ok=True)
    args._bankmap = {} if args.no_trees else treebank.map_to_treebank(args.clean)

    all_maps = sorted(glob.glob(os.path.join(mh, "*.MP")) +
                      glob.glob(os.path.join(mh, "ALIEN_*.MAP")))
    if args.maps:
        sel = []
        for pat in args.maps:
            sel += [p for p in all_maps if fnmatch.fnmatch(os.path.basename(p), pat)
                    or os.path.basename(p).lower() == pat.lower()]
        all_maps = sorted(set(sel))
    if not all_maps:
        print("no matching maps")
        return 1

    written = set()
    for mp in all_maps:
        tactical_map = os.path.splitext(mp)[1].lower() == ".map"
        res = (_decompile_tactical if tactical_map else _decompile_strategic)(mp, mh, args, written)
        if isinstance(res, str):
            print(f"  {os.path.basename(mp):16} {res}")
            continue
        tmj, note = res
        stem = os.path.splitext(os.path.basename(mp))[0]
        with open(os.path.join(args.out, stem + ".tmj"), "w", encoding="utf-8") as fh:
            json.dump(tmj, fh)
        print(f"  {os.path.basename(mp):16} -> {stem}.tmj  ({note})")

    print(f"decompiled -> {args.out}")
    print(f"next: edit in Tiled, then  python -m src.formats.mapkit.map_compile --src {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
