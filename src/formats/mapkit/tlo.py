""".TLO biome/floor tileset codec + Tiled tileset-image (PNG) export.

Container (the TLO format notes): 512 B RGB565 palette[256] header, then N tiles of
1 byte/pixel = palette index. Two geometries: biome 32x32 stride 0x400, tactical
(PODLOGA) 32x24 stride 0x300. Tile count = (size-0x200)//stride (exact divide).

In Phase 1 the .TLO is a READ-ONLY reference for Tiled (to_png renders the atlas
so the map's tile ids are visible/paintable); the map only stores tile *indices*,
so editing the tileset art itself is out of scope (from_png is a future add)."""
from __future__ import annotations

import os
import struct

from .common import rgb565_to_888

GEOM = {"biome": (32, 32, 0x400), "tactical": (32, 24, 0x300)}


class TloModel:
    __slots__ = ("palette", "tiles", "tile_w", "tile_h", "stride", "count", "geom")


def infer_geom(path: str) -> str:
    return "tactical" if os.path.basename(path).lower().startswith("podloga") else "biome"


def parse(path: str, geom: str | None = None) -> TloModel:
    with open(path, "rb") as fh:
        buf = fh.read()
    geom = geom or infer_geom(path)
    tw, th, stride = GEOM[geom]
    body = buf[0x200:]
    if len(body) % stride:
        raise ValueError(f"{path}: tile body {len(body)} not a multiple of stride {stride} "
                         f"(wrong geometry {geom!r}?)")
    m = TloModel()
    m.geom, m.tile_w, m.tile_h, m.stride = geom, tw, th, stride
    m.palette = list(struct.unpack_from("<256H", buf, 0))
    m.tiles = body
    m.count = len(body) // stride
    return m


def emit(m: TloModel) -> bytes:
    return struct.pack("<256H", *m.palette) + m.tiles


def to_png(m: TloModel, columns: int = 32):
    """Render the tileset as a paletted PNG atlas Tiled can use directly."""
    from PIL import Image
    rows = (m.count + columns - 1) // columns
    img = Image.new("P", (columns * m.tile_w, rows * m.tile_h), 0)
    pal = []
    for v in m.palette:
        pal.extend(rgb565_to_888(v))
    img.putpalette(pal)
    for t in range(m.count):
        tile = Image.frombytes("P", (m.tile_w, m.tile_h), m.tiles[t * m.stride:(t + 1) * m.stride])
        img.paste(tile, ((t % columns) * m.tile_w, (t // columns) * m.tile_h))
    return img
