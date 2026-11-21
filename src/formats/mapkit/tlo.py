"""TLO tileset codec and strategic indexed-atlas authoring.

A TLO contains 256 little-endian RGB565 colours followed by indexed tiles.
Biome tiles are 32x32; tactical floor tiles are 32x24 (export only).
"""
from __future__ import annotations

import os
import struct

from .common import rgb565_to_888, rgb888_to_565

GEOM = {"biome": (32, 32, 0x400), "tactical": (32, 24, 0x300)}
MAX_BIOME_TILES = 8192


class TloModel:
    __slots__ = ("palette", "tiles", "tile_w", "tile_h", "stride", "count", "geom")


def infer_geom(path: str) -> str:
    return "tactical" if os.path.basename(path).lower().startswith("podloga") else "biome"


def _validate(m: TloModel) -> None:
    if m.geom not in GEOM or (m.tile_w, m.tile_h, m.stride) != GEOM[m.geom]:
        raise ValueError("invalid TLO geometry or stride")
    if not isinstance(m.count, int) or m.count < 1:
        raise ValueError("TLO must contain at least one tile")
    if m.geom == "biome" and m.count > MAX_BIOME_TILES:
        raise ValueError("biome TLO exceeds 8192 tiles")
    if len(m.palette) != 256 or any(not isinstance(v, int) or not 0 <= v <= 65535 for v in m.palette):
        raise ValueError("TLO palette must contain 256 RGB565 entries")
    if not isinstance(m.tiles, bytes) or len(m.tiles) != m.count * m.stride:
        raise ValueError("TLO tile body does not match count and stride")


def parse(path: str, geom: str | None = None) -> TloModel:
    with open(path, "rb") as fh:
        buf = fh.read()
    geom = geom or infer_geom(path)
    if geom not in GEOM:
        raise ValueError(f"unknown TLO geometry: {geom!r}")
    tw, th, stride = GEOM[geom]
    if len(buf) < 0x200:
        raise ValueError(f"{path}: truncated TLO palette")
    body = buf[0x200:]
    if len(body) % stride:
        raise ValueError(f"{path}: tile body {len(body)} not a multiple of stride {stride} "
                         f"(wrong geometry {geom!r}?)")
    m = TloModel()
    m.geom, m.tile_w, m.tile_h, m.stride = geom, tw, th, stride
    m.palette = list(struct.unpack_from("<256H", buf, 0))
    m.tiles = body
    m.count = len(body) // stride
    _validate(m)
    return m


def emit(m: TloModel) -> bytes:
    _validate(m)
    return struct.pack("<256H", *m.palette) + m.tiles


def to_png(m: TloModel, columns: int = 32):
    """Render an indexed atlas. The caller must retain the exact tile count."""
    from PIL import Image

    _validate(m)
    if not isinstance(columns, int) or columns < 1 or columns > MAX_BIOME_TILES:
        raise ValueError("columns must be between 1 and 8192")
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


def from_png(img, count: int, *, quantize: bool = False) -> TloModel:
    """Build a biome TLO from an atlas and its explicit, unpadded tile count.

    Atlas padding must contain index 0 (or black for RGB). RGB/RGBA input is
    accepted only with quantize=True, using fixed integer RGB332, no dithering.
    Indexed palette entries are converted independently; indices never change.
    """
    if not isinstance(count, int) or not 1 <= count <= MAX_BIOME_TILES:
        raise ValueError("tile count must be between 1 and 8192")
    w, h = img.size
    if w < 32 or h < 32 or w % 32 or h % 32:
        raise ValueError("atlas dimensions must be positive multiples of 32")
    columns, rows = w // 32, h // 32
    if columns > MAX_BIOME_TILES or rows != (count + columns - 1) // columns:
        raise ValueError("atlas dimensions do not match tile count")
    if img.mode == "P":
        transparency = img.info.get("transparency")
        if isinstance(transparency, int) or (isinstance(transparency, bytes) and any(a != 255 for a in transparency)):
            raise ValueError("transparent indexed PNG is not supported")
        palette = img.getpalette("RGB")
        if palette is None or len(palette) % 3 or len(palette) > 768:
            raise ValueError("invalid indexed PNG palette")
        pixels = img.tobytes()
        if pixels and max(pixels) >= len(palette) // 3:
            raise ValueError("pixel index outside PNG palette")
        palette = palette + [0] * (768 - len(palette))
        colors = [rgb888_to_565(*palette[i:i + 3]) for i in range(0, 768, 3)]
    elif img.mode in ("RGB", "RGBA"):
        if not quantize:
            raise ValueError("truecolor PNG requires --quantize")
        if img.mode == "RGBA" and img.getchannel("A").getextrema() != (255, 255):
            raise ValueError("nonopaque alpha is not supported")
        if "transparency" in img.info:
            raise ValueError("transparent PNG is not supported")
        rgb = img.convert("RGB").tobytes()
        pixels = bytes((rgb[i] & 0xe0) | ((rgb[i + 1] >> 3) & 0x1c) | (rgb[i + 2] >> 6)
                       for i in range(0, len(rgb), 3))
        colors = [rgb888_to_565(((i >> 5) * 255 + 3) // 7,
                               (((i >> 2) & 7) * 255 + 3) // 7,
                               ((i & 3) * 255 + 1) // 3) for i in range(256)]
        # A nearly-black RGB padding pixel must not disappear through quantization.
        for t in range(count, columns * rows):
            x, y = (t % columns) * 32, (t // columns) * 32
            for dy in range(32):
                start = ((y + dy) * w + x) * 3
                if any(rgb[start:start + 96]):
                    raise ValueError("unused atlas tiles must be black")
    else:
        raise ValueError(f"unsupported PNG mode {img.mode!r}; use indexed or RGB/RGBA")
    for t in range(count, columns * rows):
        x, y = (t % columns) * 32, (t // columns) * 32
        for dy in range(32):
            start = (y + dy) * w + x
            if any(pixels[start:start + 32]):
                raise ValueError("unused atlas tiles must contain index 0")
    tiles = bytearray(count * 1024)
    for t in range(count):
        x, y = (t % columns) * 32, (t // columns) * 32
        for dy in range(32):
            src = (y + dy) * w + x
            dst = t * 1024 + dy * 32
            tiles[dst:dst + 32] = pixels[src:src + 32]
    m = TloModel()
    m.geom, m.tile_w, m.tile_h, m.stride = "biome", 32, 32, 1024
    m.count, m.palette, m.tiles = count, colors, bytes(tiles)
    _validate(m)
    return m
