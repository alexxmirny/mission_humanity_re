"""Panel icons: cfg ``ICON`` slot -> ``panel\\<file>.GFX`` -> RGBA PNG."""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
from PIL import Image

from .common import ArtSource

ICON_TABLE_VA = 0x00518475  # {char[16] file; u32 slot} x 211 in mh.exe (.data)
ICON_TABLE_COUNT = 211
_ENTRY = 20
GFX_DIRS = ("panel", "panelB")


def va_to_offset(exe: bytes, va: int) -> int:
    """Map a VA to a file offset through the PE section table."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    opt_size = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    rva = va - base
    sec = pe + 24 + opt_size
    for i in range(nsec):
        _n, vsize, vaddr, rsize, rptr = struct.unpack_from("<8sIIII", exe, sec + i * 40)
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return rva - vaddr + rptr
    raise ValueError(f"VA {va:#x} is in no section")


def read_icon_table(exe: bytes) -> list[tuple[str, int]]:
    """[(gfx file name, slot)] in table order."""
    off = va_to_offset(exe, ICON_TABLE_VA)
    out = []
    for i in range(ICON_TABLE_COUNT):
        raw, slot = struct.unpack_from("<16sI", exe, off + i * _ENTRY)
        out.append((raw.split(b"\0")[0].decode("ascii"), slot))
    return out


def decode_gfx(data: bytes) -> Image.Image:
    """u16 w, u16 h, RGB565 LE pixels; pure black is transparent."""
    w, h = struct.unpack_from("<HH", data, 0)
    px = np.frombuffer(data, dtype="<u2", count=w * h, offset=4).reshape(h, w).astype(np.uint32)
    r, g, b = (px >> 11) & 31, (px >> 5) & 63, px & 31
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., 0] = (r * 255 + 15) // 31
    rgba[..., 1] = (g * 255 + 31) // 63
    rgba[..., 2] = (b * 255 + 15) // 31
    rgba[..., 3] = np.where(px == 0, 0, 255)
    return Image.fromarray(rgba, "RGBA")


def used_icon_names(src: ArtSource) -> list[str]:
    """Icon names referenced by units, buildings, projects, upgrades and planets (sorted)."""
    names = set()
    for cls in ("UNIT", "BUILDING", "PROJECT", "UPGRADE", "PLANET", "WEAPON"):
        for rec in src.objects(cls):
            ic = rec.get("icon")
            if ic:
                names.add(ic)
    return sorted(names)


def extract_icons(src: ArtSource, out_dir, names=None, subdir="icons") -> dict[str, str]:
    """{icon name: 'icons/<name>.png'} for every cfg icon that resolves to a GFX file.

    ``names`` defaults to the icons objects actually use. Unresolved ones are listed in
    ``src.stats['icons_unresolved']`` (names) -- e.g. the I_UKLAD_1 system icon has no table row.
    """
    out_dir = Path(out_dir)
    table = read_icon_table(src.mh_exe_bytes())
    slot_file = {}
    for fname, slot in table:
        slot_file.setdefault(slot, fname)
    slots = dict(src.model.icons)
    names = list(names) if names is not None else used_icon_names(src)
    result, missing = {}, []
    for name in names:
        slot = slots.get(name)
        fname = slot_file.get(slot) if slot is not None else None
        data = None
        if fname:
            for d in GFX_DIRS:
                data = src.member(f"{d}\\{fname}")
                if data:
                    break
        if not data:
            missing.append(name)
            continue
        rel = f"{subdir}/{name.lower()}.png"
        dest = out_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        decode_gfx(data).save(dest)
        result[name] = rel
    src.stats["icons_total"] = len(names)
    src.stats["icons_ok"] = len(result)
    src.stats["icons_unresolved"] = missing
    return result
