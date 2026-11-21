r"""Planet portraits: frame 0 of ``menu\<INFO_FLC>`` (Pillow reads FLI/FLC)."""

from __future__ import annotations

import io
from pathlib import Path

from PIL import Image

from .common import ArtSource

FALLBACK_FLC = "BLUE1.FLC"


def flc_frame(data: bytes, frame=0) -> Image.Image:
    im = Image.open(io.BytesIO(data))
    im.seek(frame)
    return im.convert("RGBA")


def extract_planets(src: ArtSource, out_dir, subdir="planets") -> dict[str, str]:
    """{planet name: 'planets/<name>.png'}; missing FLC falls back to BLUE1.FLC."""
    planets = src.objects("PLANET")
    out, fallback = {}, []
    for p in planets:
        data = src.member(rf"menu\{p.get('info_flc', '')}")
        if not data:
            data = src.member(rf"menu\{FALLBACK_FLC}")
            fallback.append(p["__name__"])
        if not data:
            continue
        rel = f"{subdir}/{p['__name__'].lower()}.png"
        dest = Path(out_dir) / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        flc_frame(data).save(dest)
        out[p["__name__"]] = rel
    src.stats["planets_total"] = len(planets)
    src.stats["planets_ok"] = len(out)
    src.stats["planets_fallback"] = fallback
    return out
