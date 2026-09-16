"""Adopt an external image as a Mission Humanity **V1 sprite** in the mh_tools
extracted-layout-v2 directory format (`sprite.bmp` + `palette.bmp` + `fx_*.bmp`
masks + `meta.bin`), so `mh_tools pack` re-encodes it back into a BNK.

Why this exists
---------------
The engine's V1 sprites are 256-colour palettized. `mh_tools` reads each sprite
dir as **32-bit BGRA BMPs** (a BITMAPV4HEADER + BI_BITFIELDS, alpha 0 =
transparent) and maps every opaque pixel back to a palette index by *exact
colour match* (BNK_FORMAT.md §12.5). PIL cannot write that BMP variant (it drops
alpha to BGRX), so we emit the header ourselves from a captured template and
quantize the source image to a ≤256 colour palette so the exact-match holds.

Effect masks (same size as the sprite, BGRA BMPs, `R==0` = no effect):
- ``fx_darken.bmp`` — 0xA0 shadow: white pixel = darken the ground beneath.
- ``fx_tint.bmp``   — 0x80 tint:  ``R=G=B = level*8`` (level 1..31).
- ``fx_blend.bmp``  — 0x40 blend: ``R`` = palette index, ``G`` = level*8.

Public entry point: :func:`write_sprite_dir`.
"""
from __future__ import annotations

import os
import struct

from PIL import Image

# mh_tools writes 32bpp BMPs with a BITMAPV4HEADER (108 bytes) + BI_BITFIELDS
# so alpha survives. Masks are BGRA: R=0x00ff0000 G=0x0000ff00 B=0x000000ff
# A=0xff000000; colorspace = 'sRGB'. Reproduced field-for-field below.
def _bmp_header(w, h):
    imgsize = w * h * 4
    file_hdr = struct.pack("<2sIHHI", b"BM", 14 + 108 + imgsize, 0, 0, 14 + 108)
    v4 = struct.pack(
        "<IiiHHIIiiII"      # basic BITMAPINFOHEADER fields (40 bytes)
        "IIII"              # R,G,B,A bit masks (16)
        "I"                 # CSType 'sRGB' (4)
        "iiiiiiiii"         # CIEXYZTRIPLE endpoints (36)
        "III",              # Gamma R,G,B (12)
        108, w, h, 1, 32, 3, imgsize, 2835, 2835, 0, 0,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000,
        0x73524742,
        0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0,
    )
    return file_hdr + v4


def _write_bgra_bmp(path, pixels, w, h):
    """Write a 32bpp BGRA BMP. *pixels* is a flat top-to-bottom sequence of
    (r,g,b,a) tuples of length w*h."""
    hdr = bytearray(_bmp_header(w, h))
    imgsize = w * h * 4
    body = bytearray(imgsize)
    # BMP rows are stored bottom-up; each pixel is B,G,R,A.
    for y in range(h):
        srow = (h - 1 - y) * w
        drow = y * w * 4
        for x in range(w):
            r, g, b, a = pixels[srow + x]
            o = drow + x * 4
            body[o] = b
            body[o + 1] = g
            body[o + 2] = r
            body[o + 3] = a
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(body)


def _palette_image(palette):
    p = Image.new("P", (16, 16))
    flat = [c for rgb in palette for c in rgb]
    p.putpalette(flat + [0] * (768 - len(flat)))
    return p


def build_palette(images, maxcolors=256):
    """A single shared 256-colour palette covering the opaque pixels of several
    RGBA images (needed when the sprites share a palette group in the BNK)."""
    cols = [im.convert("RGBA") for im in images]
    total_h = sum(c.height for c in cols)
    montage = Image.new("RGB", (max(c.width for c in cols), total_h), (0, 0, 0))
    y = 0
    for c in cols:
        montage.paste(c.convert("RGB"), (0, y))
        y += c.height
    q = montage.quantize(colors=maxcolors, method=Image.Quantize.MAXCOVERAGE)
    pal = q.getpalette()[: maxcolors * 3]
    pal += [0] * (maxcolors * 3 - len(pal))
    return [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for i in range(maxcolors)]


def write_gfx(path, art_rgba, size=(44, 44), bg=(0, 0, 0)):
    """Write a panel ``.GFX`` icon: ``[u16 w][u16 h][w*h RGB565]`` (row-major,
    top-to-bottom). Used for build-menu icons (``panel\\ILB_*.GFX``, indexed by
    the cfg ICON id). Transparent source pixels fall back to *bg* (icons render
    on a black panel, so bg defaults to black)."""
    w, h = size
    im = art_rgba.convert("RGBA")
    im.thumbnail((w, h), Image.LANCZOS)
    canvas = Image.new("RGB", (w, h), bg)
    canvas.paste(im, ((w - im.width) // 2, (h - im.height) // 2), im)
    px = canvas.load()
    out = bytearray(struct.pack("<HH", w, h))
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += struct.pack("<H", v)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)


def quantize(img, maxcolors=256, palette=None):
    """Quantize an RGBA image's opaque pixels to <=maxcolors. Returns
    ``(flat_pixels, palette)`` where flat_pixels is a top-to-bottom list of
    (r,g,b,a) using only palette colours (transparent pixels -> (0,0,0,0)).
    Pass *palette* (a list of (r,g,b)) to map onto a fixed shared palette instead
    of building a per-image one."""
    img = img.convert("RGBA")
    w, h = img.size
    alpha = img.getchannel("A")
    if palette is not None:
        q = img.convert("RGB").quantize(palette=_palette_image(palette),
                                        dither=Image.Dither.NONE)
    else:
        q = img.convert("RGB").quantize(colors=maxcolors, method=Image.Quantize.MAXCOVERAGE)
        pal = q.getpalette()[: maxcolors * 3]
        pal += [0] * (maxcolors * 3 - len(pal))
        palette = [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for i in range(maxcolors)]
    qpx = q.load()
    apx = alpha.load()
    out = []
    for y in range(h):
        for x in range(w):
            if apx[x, y] < 128:
                out.append((0, 0, 0, 0))
            else:
                c = palette[qpx[x, y]]
                out.append((c[0], c[1], c[2], 255))
    return out, palette


def _write_palette_bmp(path, palette):
    """16x16 swatch of the 256 palette colours (opaque)."""
    px = [(r, g, b, 255) for (r, g, b) in palette]
    _write_bgra_bmp(path, px, 16, 16)


def _blank_mask(w, h):
    return [(0, 0, 0, 255)] * (w * h)   # R==0 everywhere -> no effect


def write_sprite_dir(dst_dir, art_rgba, *, palette=None, shadow_alpha=None, glow_rgb=None,
                     glow_level=0, meta_src=None, keep_from=None, meta_wh=None):
    """Author one V1 sprite directory from *art_rgba* (a PIL RGBA image, already
    at the target canvas size).

    - ``shadow_alpha``: optional PIL 'L' image (same size) — non-zero -> a
      ``fx_darken`` shadow pixel (thresholded).
    - ``glow_rgb`` + ``glow_level`` (0..31): optional PIL RGB glow image; where it
      is bright, emit an ``fx_tint`` pixel at ``glow_level`` (a soft additive
      glow). Pass glow_level via the frame loop for a pulse.
    - ``meta_src``: path to a 24-byte meta.bin to copy (anchor/hotspot). If None
      and ``keep_from`` is given, copies that dir's meta.bin.
    - ``keep_from``: a source sprite dir to copy non-authored sidecars from
      (``palette.bin``/``rle.bin`` are regenerated by mh_tools on re-encode, but
      copying keeps the dir complete). Optional.
    """
    os.makedirs(dst_dir, exist_ok=True)
    w, h = art_rgba.size
    pixels, palette = quantize(art_rgba, palette=palette)
    _write_bgra_bmp(os.path.join(dst_dir, "sprite.bmp"), pixels, w, h)
    _write_palette_bmp(os.path.join(dst_dir, "palette.bmp"), palette)

    # fx_darken from the shadow alpha
    if shadow_alpha is not None:
        sa = shadow_alpha.resize((w, h)).load() if shadow_alpha.size != (w, h) else shadow_alpha.load()
        dk = [(255, 255, 255, 255) if sa[x, y] >= 96 else (0, 0, 0, 255)
              for y in range(h) for x in range(w)]
    else:
        dk = _blank_mask(w, h)
    _write_bgra_bmp(os.path.join(dst_dir, "fx_darken.bmp"), dk, w, h)

    # fx_tint from the glow (bright glow pixels -> tint at glow_level)
    lvl = max(0, min(31, int(glow_level)))
    if glow_rgb is not None and lvl > 0:
        g = glow_rgb.convert("RGB")
        g = g.resize((w, h)) if g.size != (w, h) else g
        gp = g.load()
        v = lvl * 8
        tint = []
        for y in range(h):
            for x in range(w):
                r, gg, b = gp[x, y]
                tint.append((v, v, v, 255) if (r + gg + b) >= 120 else (0, 0, 0, 255))
    else:
        tint = _blank_mask(w, h)
    _write_bgra_bmp(os.path.join(dst_dir, "fx_tint.bmp"), tint, w, h)

    # blend masks unused (keep the dir shape mh_tools expects)
    for m in ("fx_blend.bmp", "fx_blend2.bmp"):
        _write_bgra_bmp(os.path.join(dst_dir, m), _blank_mask(w, h), w, h)

    # meta.bin (hotspot/anchor): copy an existing one unless told otherwise
    meta = meta_src or (os.path.join(keep_from, "meta.bin") if keep_from else None)
    if meta and os.path.exists(meta):
        with open(meta, "rb") as f:
            mb = bytearray(f.read())
        # meta[0:4] are the content-extent int16s (w,h) the game uses for draw
        # bounds; stale small values would clip a larger sprite, so override.
        if meta_wh is not None:
            struct.pack_into("<hh", mb, 0, int(meta_wh[0]), int(meta_wh[1]))
        with open(os.path.join(dst_dir, "meta.bin"), "wb") as f:
            f.write(mb)
