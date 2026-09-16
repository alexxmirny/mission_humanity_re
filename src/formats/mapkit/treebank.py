"""Resolve a .MP map to its biome tree bank and load the tree sprite art.

Each strategic PLANET loads one of banks 50-56 as its tree/decoration set; the same
`kind` is different art per biome (the strategic-cfg notes). The mapping .MP -> bank
comes from INIT.CFG's PLANET entries (cfgkit). Sprites are the pre-decoded RGBA
`sprite.bmp` files produced by `mh_tools unpack` (F:\\games\\MH\\bnk_unpack\\BANKI\\
BANK_<n>\\<kind-1:04d>\\sprite.bmp). If either is unavailable the caller falls back
to abstract tree markers."""
from __future__ import annotations

import os

from .common import BANKI as UNPACK_ROOT

_bankmap_cache = {}


def map_to_treebank(clean: str) -> dict:
    """{ map_basename_lower : tree_bank_int } from INIT.CFG PLANET entries (cached)."""
    if clean in _bankmap_cache:
        return _bankmap_cache[clean]
    out = {}
    try:
        from ..cfgkit import parse
        model = parse.parse_files(os.path.join(clean, "mh", "init", "INIT.CFG"), None)
        for p in model.classes.get("PLANET", []):
            mp = (p.get("map") or "").strip().lower()
            banks = sorted(b for b in (p.get("banks") or []) if 50 <= b <= 56)
            if mp and banks:
                out[mp] = banks[0]        # a couple of planets list two; use the first
    except Exception as e:                # cfgkit missing / cfg unreadable -> no tree art
        print(f"  (tree-bank resolution unavailable: {e})")
    _bankmap_cache[clean] = out
    return out


def load_tree_sprites(bank: int, kinds, unpack_root: str = UNPACK_ROOT) -> dict:
    """{ kind : (png_relpath, PIL_RGBA_image, w, h, origin_x, origin_y) } for kinds with
    shipped art. origin (from meta.bin @4,6) is the sprite hotspot: the in-game/editor
    anchor tile pixel is the sprite top-left + origin."""
    if not os.path.isdir(unpack_root):
        return {}
    import struct
    from PIL import Image
    out = {}
    for kind in sorted(set(kinds)):
        d = os.path.join(unpack_root, f"BANK_{bank}", f"{kind - 1:04d}")
        p = os.path.join(d, "sprite.bmp")
        if not os.path.isfile(p):
            continue                       # kinds 43-48 (or beyond a bank's count) have none
        im = Image.open(p).convert("RGBA")
        ox = oy = 0
        mp = os.path.join(d, "meta.bin")
        if os.path.isfile(mp):
            with open(mp, "rb") as fh:
                meta = fh.read()
            if len(meta) >= 8:
                ox, oy = struct.unpack_from("<hh", meta, 4)
        out[kind] = (f"trees/b{bank}_k{kind:02d}.png", im, im.width, im.height, ox, oy)
    return out


WALL_BANK = 120   # tactical base-interior tileset (BANK_120.BNK): walls/consoles/vents


def load_wall_sprites(real_ids, unpack_root: str = UNPACK_ROOT) -> dict:
    """{ real_sprite_id : (png_relpath, PIL_RGBA_image, w, h, level) } for tactical
    ALIEN_*.MAP height-slot sprites. Each slot's value (after the -100 unbias) is a
    BANK_120 sprite index; `level` = the sprite header byte (rle.bin[1]) = how many
    24px tile-rows up it draws (vertical stacking)."""
    if not os.path.isdir(unpack_root):
        return {}
    from PIL import Image
    out = {}
    for rid in sorted(set(real_ids)):
        d = os.path.join(unpack_root, f"BANK_{WALL_BANK}", f"{rid:04d}")
        sp = os.path.join(d, "sprite.bmp")
        if not os.path.isfile(sp):
            continue
        im = Image.open(sp).convert("RGBA")
        level = 0
        rb = os.path.join(d, "rle.bin")
        if os.path.isfile(rb):
            with open(rb, "rb") as fh:
                hdr = fh.read(2)
            if len(hdr) >= 2:
                level = hdr[1]
        out[rid] = (f"walls/b120_{rid:04d}.png", im, im.width, im.height, level)
    return out
