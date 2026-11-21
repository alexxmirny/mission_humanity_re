"""Galaxy-screen backgrounds for the star-system pages.

The game draws ``menu/BACK0<mode>_<A|B|C>.GFX`` (letter = star system 1..3, mode 2 = 1024x768) as the
planet-select screen background.  The bottom ~96 px of the art is the baked-in info panel and Back
button, so the page crops it away; the planets are placed over the picture by the page itself
(``model.Wiki.starmap_layout``).
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

NATIVE = 2  # BACK02_*: the 1024x768 art the game runs the galaxy screen at
CROP_H = 672  # rows above the baked-in info panel
SYSTEM_LETTERS = "ABC"


def decode_opaque(data: bytes) -> Image.Image:
    """u16 w, u16 h, RGB565 LE pixels -> opaque RGB (the background has no transparency)."""
    w = int.from_bytes(data[0:2], "little")
    h = int.from_bytes(data[2:4], "little")
    px = np.frombuffer(data, dtype="<u2", count=w * h, offset=4).reshape(h, w).astype(np.uint32)
    rgb = np.zeros((h, w, 3), dtype=np.uint8)
    rgb[..., 0] = (((px >> 11) & 31) * 255 + 15) // 31
    rgb[..., 1] = (((px >> 5) & 63) * 255 + 31) // 63
    rgb[..., 2] = ((px & 31) * 255 + 15) // 31
    return Image.fromarray(rgb, "RGB")


def extract_starmaps(src, wiki, out_dir, subdir="starmaps", quality=85) -> dict[str, str]:
    """{system cfg name: 'starmaps/<name>.jpg'} for the first three systems (cfg order)."""
    out = {}
    for k, s in enumerate(wiki.by_kind["system"][: len(SYSTEM_LETTERS)]):
        data = src.member(rf"menu\BACK0{NATIVE}_{SYSTEM_LETTERS[k]}.GFX")
        if not data:
            continue
        im = decode_opaque(data)
        im = im.crop((0, 0, im.width, min(CROP_H, im.height)))
        rel = f"{subdir}/{s.cfg_name.lower()}.jpg"
        dest = Path(out_dir) / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        im.save(dest, quality=quality)
        out[s.cfg_name] = rel
    return out
