"""Contact sheets for human review of the guessed art rules (buildings, unit facings)."""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

from .common import ArtSource
from .sprites import FACING_COUNT, SpriteStore, load_facing, object_keys, pick_facing

BG = (70, 90, 70, 255)


def _fit(img, size):
    if img is None:
        return None
    s = min(size / img.width, size / img.height, 1.0)
    if s < 1.0:
        img = img.resize((max(1, int(img.width * s)), max(1, int(img.height * s))), Image.LANCZOS)
    return img


def _paste_center(sheet, img, cx, cy):
    if img is not None:
        sheet.alpha_composite(img, (int(cx - img.width / 2), int(cy - img.height / 2)))


def building_sheet(
    src: ArtSource, review_dir, store: SpriteStore | None = None, cols=8, cell=170
) -> list[str]:
    """All buildings' completed renders on a labelled grid; one PNG per faction half."""
    store = store or SpriteStore(src)
    blds = src.objects("BUILDING")
    names = []
    for b in blds:
        names += store.building_sprite_names(b)
    store.ensure_banks(store.banks_for(names))
    paths = []
    per = cols * 5
    for part, i in enumerate(range(0, len(blds), per)):
        chunk = blds[i : i + per]
        rows = (len(chunk) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cell, rows * (cell + 14)), BG)
        d = ImageDraw.Draw(sheet)
        keys = object_keys(blds)[i : i + per]
        for n, b in enumerate(chunk):
            x, y = (n % cols) * cell, (n // cols) * (cell + 14)
            img = _fit(store.render_building(b), cell - 10)
            _paste_center(sheet, img, x + cell / 2, y + cell / 2)
            d.text((x + 3, y + cell), keys[n][2:], fill=(255, 255, 255, 255))
            if img is None:
                d.text((x + 10, y + 10), "NO SPRITE", fill=(255, 120, 120, 255))
        dest = Path(review_dir) / f"buildings_{part + 1}.png"
        dest.parent.mkdir(parents=True, exist_ok=True)
        sheet.convert("RGB").save(dest)
        paths.append(str(dest))
    return paths


def unit_sheets(
    src: ArtSource, review_dir, store: SpriteStore | None = None, facing=None, per_sheet=8, cell=72
) -> list[str]:
    """One row per unit with all 24 facings; the chosen facing has a red frame."""
    store = store or SpriteStore(src)
    facing = facing or load_facing()
    units = src.objects("UNIT")
    names = []
    for u in units:
        names += [u.get("sprite", ""), u.get("sprite_shadow", "")]
    store.ensure_banks(store.banks_for(names))
    label_w = 150
    paths = []
    for part, i in enumerate(range(0, len(units), per_sheet)):
        chunk = units[i : i + per_sheet]
        sheet = Image.new("RGBA", (label_w + FACING_COUNT * cell, len(chunk) * (cell + 12)), BG)
        d = ImageDraw.Draw(sheet)
        for r, u in enumerate(chunk):
            y = r * (cell + 12)
            chosen = pick_facing(facing, u)
            frames = store.lookup.get(u.get("sprite", ""), (0, 0, 0))[2]
            d.text((3, y + 4), u["__name__"][2:], fill=(255, 255, 255, 255))
            d.text((3, y + 18), f"frames={frames} pick={chosen}", fill=(200, 255, 200, 255))
            for f in range(1, FACING_COUNT + 1):
                x = label_w + (f - 1) * cell
                img = _fit(store.render_unit(u, f), cell - 6)
                _paste_center(sheet, img, x + cell / 2, y + cell / 2)
                d.text((x + 2, y + cell - 2), str(f), fill=(255, 255, 160, 255))
                if f == chosen:
                    d.rectangle(
                        (x, y, x + cell - 1, y + cell + 10), outline=(255, 40, 40, 255), width=2
                    )
        dest = Path(review_dir) / f"units_{part + 1}.png"
        dest.parent.mkdir(parents=True, exist_ok=True)
        sheet.convert("RGB").save(dest)
        paths.append(str(dest))
    return paths
