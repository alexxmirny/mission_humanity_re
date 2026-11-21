"""python -m src.formats.wiki.art --out <dir> [--game <dir|zip>] [--sheets] [--stills]"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from . import (
    DEFAULT_CACHE,
    ArtSource,
    SpriteStore,
    building_sheet,
    extract_building_sprites,
    extract_icons,
    extract_planets,
    extract_stills,
    extract_unit_sprites,
    stills_review_sheets,
    unit_sheets,
)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="asset output dir")
    ap.add_argument("--game", help="EN install dir or zip (default: machine_config POLYGON_CLEAN)")
    ap.add_argument("--mh-exe", help="mh.exe path (default: <game>/mh.exe)")
    ap.add_argument("--cache", default=str(DEFAULT_CACHE))
    ap.add_argument("--mh-tools", help="mh_tools.exe path")
    ap.add_argument("--sheets", action="store_true", help="write review contact sheets")
    ap.add_argument("--review-dir", help="default <cache>/review")
    ap.add_argument("--stills", action="store_true", help="extract video stills (needs ffmpeg)")
    a = ap.parse_args(argv)

    src = ArtSource(a.game, a.mh_exe, a.cache, a.mh_tools)
    out = Path(a.out)
    assets, timing = {}, {}

    def step(name, fn):
        t = time.time()
        assets[name] = fn()
        timing[name] = round(time.time() - t, 1)

    step("icons", lambda: extract_icons(src, out))
    store = SpriteStore(src)
    step("buildings", lambda: extract_building_sprites(src, out, store))
    step("units", lambda: extract_unit_sprites(src, out, store))
    step("planets", lambda: extract_planets(src, out))
    if a.stills:
        step("stills", lambda: extract_stills(src, out))
    if a.sheets:
        rd = a.review_dir or Path(a.cache) / "review"
        step("sheets", lambda: building_sheet(src, rd, store) + unit_sheets(src, rd, store))
        step("stills_sheets", lambda: stills_review_sheets(src, rd))
    (out / "assets.json").write_text(json.dumps(assets, indent=1, sort_keys=True), encoding="utf-8")
    print(json.dumps({"stats": src.stats, "notes": src.notes, "seconds": timing}, indent=1))


if __name__ == "__main__":
    main()
