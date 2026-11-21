r"""Planet map previews: the planet's ``.MP`` terrain rendered at 4 px per tile.

``PLANET MAP`` names a strategic map (``101_diun.mp``) in the ``mh`` pack; its ``.MP`` header names the
``.TLO`` tileset.  The parsing is :mod:`mapkit` (``strategic``, ``tlo``); this module only turns the
ground plane into a picture (each 32x32 tile box-averaged down to 4x4) and lists the landing spots
and ore deposits as tile coordinates, so the page can draw them as crisp HTML markers over a JPEG.
Rendered previews are cached under ``<cache>/planetmaps`` (keyed by map name + render version).
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

from .common import ArtSource

GROUND_MASK = 0x1FFF  # mapkit.strategic.GROUND_MASK: main-plane flags & mask = tile index
PX = 4  # output pixels per map tile (8 for the 64x64 maps, so every preview is >= 512 px)
SMALL_MAP = 64
THUMB = 160  # thumbnail width for the planets index
VERSION = 2  # bump to invalidate the cache
JPEG_QUALITY = 82


def _rgb(v):
    r5, g6, b5 = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r5 * 255 + 15) // 31, (g6 * 255 + 31) // 63, (b5 * 255 + 15) // 31)


def tile_thumbs(t, px=PX) -> np.ndarray:
    """(count, px, px, 3) uint8: every tile box-averaged down to ``px`` x ``px``."""
    pal = np.array([_rgb(v) for v in t.palette], dtype=np.float32)
    idx = np.frombuffer(t.tiles, dtype=np.uint8).reshape(t.count, t.stride)[
        :, : t.tile_w * t.tile_h
    ]
    rgb = pal[idx].reshape(t.count, px, t.tile_h // px, px, t.tile_w // px, 3)
    return rgb.mean(axis=(2, 4)).round().astype(np.uint8)


def render_ground(m, thumbs: np.ndarray, px=PX) -> Image.Image:
    """Terrain picture of the whole map (the ``.MP`` main plane is column-major)."""
    w, h = m.width, m.height
    ids = np.array(m.flags, dtype=np.int64).reshape(w, h) & GROUND_MASK
    ids = np.minimum(ids, len(thumbs) - 1)
    cells = thumbs[ids.T]  # (h, w, px, px, 3): rows first
    img = cells.transpose(0, 2, 1, 3, 4).reshape(h * px, w * px, 3)
    return Image.fromarray(img, "RGB")


def markers(m) -> dict:
    """Landing spots and ore deposits as tile coordinates (``status`` -1 = unused spot)."""
    land = []
    for i in range(m.f3):
        x, y, status = struct.unpack_from("<3i", m.tail_raw, i * 12)
        if status != -1:
            land.append([x, y])
    ore = []
    hq = m.height // 4
    for b in range(len(m.resources_raw) // 16):
        if any(struct.unpack_from("<8H", m.resources_raw, b * 16)):
            bx, by = divmod(b, hq)
            ore.append([bx * 4 + 2, by * 4 + 2])  # block centre
    return {"land": land, "ore": ore}


def extract_planet_maps(src: ArtSource, out_dir, subdir="planetmaps"):
    """({planet name: 'planetmaps/<name>.jpg'}, {name: 'planetmaps/<name>_t.jpg'}, {name: info}).

    ``info`` = ``{"w", "h", "map", "land": [[x, y]], "ore": [[x, y]]}`` (tile coordinates).
    A planet whose map or tileset is missing is skipped (counted in ``src.stats``).
    """
    from ...mapkit import strategic as mp  # lazy: mapkit pulls in tools/machine_config
    from ...mapkit import tlo

    big, thumb, info = {}, {}, {}
    cache = src.cache / "planetmaps"
    cache.mkdir(parents=True, exist_ok=True)
    tlo_cache = {}
    skipped = []
    for p in src.objects("PLANET"):
        name, mapname = p["__name__"], p.get("map", "")
        mpath = src.member_to_file(mapname) if mapname else None
        if mpath is None:
            skipped.append(name)
            continue
        stem = f"{Path(mapname).stem.lower()}_v{VERSION}"
        jpg, jthumb, meta = cache / f"{stem}.jpg", cache / f"{stem}_t.jpg", cache / f"{stem}.json"
        if not (jpg.is_file() and jthumb.is_file() and meta.is_file()):
            m = mp.parse(str(mpath))
            tname = m.tlo_name.split(b"\0")[0].decode("ascii")
            px = PX * 2 if m.width <= SMALL_MAP else PX
            if (tname, px) not in tlo_cache:
                tpath = src.member_to_file(tname)
                tlo_cache[tname, px] = (
                    tile_thumbs(tlo.parse(str(tpath), "biome"), px) if tpath else None
                )
            if tlo_cache[tname, px] is None:
                skipped.append(name)
                continue
            im = render_ground(m, tlo_cache[tname, px], px)
            im.save(jpg, quality=JPEG_QUALITY)
            tw = THUMB
            im.resize((tw, round(tw * im.height / im.width)), Image.LANCZOS).save(
                jthumb, quality=JPEG_QUALITY
            )
            meta.write_text(
                json.dumps({"w": m.width, "h": m.height, "map": mapname, **markers(m)}),
                encoding="utf-8",
            )
        for sub, key, d in ((jpg, name, big), (jthumb, name, thumb)):
            rel = f"{subdir}/{name.lower()}{'_t' if sub is jthumb else ''}.jpg"
            dest = Path(out_dir) / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(sub.read_bytes())
            d[key] = rel
        info[name] = json.loads(meta.read_text(encoding="utf-8"))
    src.stats["planet_maps_ok"] = len(big)
    src.stats["planet_maps_skipped"] = skipped
    return big, thumb, info
