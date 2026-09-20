#!/usr/bin/env python3
r"""fnt.py -- read, write, dump and MERGE the game's bitmap fonts (`fnt\*` in mh.rsr / mh_ex.rsr).

Tracker item mp:F2. The research that opened it is the fonts page in the maintainer docs (mp:F1); this module is the
executable half of it, and it CORRECTS F1 on the one thing F1 guessed: the `.FNT` header is ONE
byte, not two. The ground truth is the loader, not a parse that happens to exhaust the file --
`llm_gfx_font_load` (EN 0x004a2580) reads it as:

    H          = font_data[0]                    -- fixed pixel row count for every glyph
    p          = 1                               -- the FIRST RECORD STARTS AT OFFSET 1
    while p < end:  w = font_data[p]; record = font_data[p+1 : p+1 + w*H]; p += 1 + w*H

so what F1 read as "second header byte" (6 for FONTY08, 3/2/5 for the PFMENU family) is simply the
first glyph's width. With the 1-byte header every one of the 18 shipped `.FNT` files parses to the
exact last byte, and the record count lines up with `FONTLAY.TXT` the way the loader requires
(below) instead of by a scatter of +1/-1/+67 fudges.

CODE UNIT -> PIXELS, the whole chain (all three steps verified in the EN decompile):

  1. `llm_ui_main_menu_screen_load` (0x004b64c8) reads `fnt\FontLay.txt`, skips the BOM, and fills
     the shared char-index table:  `charmap[code_unit] = ordinal`, ordinal counting from 1.
  2. `llm_gfx_font_load` builds `glyph_ptr[1..N]` -- one entry per record, in file order -- and then
     overwrites `glyph_ptr[0]` with the font's scratch buffer. Index 0 is therefore NOT a glyph.
  3. `llm_gfx_font_layout_text` (0x004a2400) does `glyph_ptr[charmap[code_unit]]` and adds the
     record's first byte (the width) to the running string width.

  => FONTLAY ordinal i (0-based, after the BOM) is served by RECORD i (0-based, from file offset 1).
     An UNLISTED code unit indexes `charmap` with a raw UTF-16 value and nothing bounds-checks it;
     inside the table it lands on ordinal 0, i.e. the scratch buffer rendered as a glyph. That is
     what the mp:F2 DLL seam (src/mh_dll/mh/seams/gfx_font_guard.cpp) guards.

WHAT THIS TOOL DOES NOT DO: ship glyphs. The Cyrillic bitmaps belong to the retail RU build and the
Latin ones to the retail EN build; neither is redistributable and neither is in this repo. `merge`
REGENERATES the merged fonts on the user's machine from the user's own two installs, and the repo
carries only this tool + the derivation table below + the docs.

USAGE
  python src/formats/fnt.py --selftest
      Round-trips every shipped `fnt\*` member of every retail source it can find, byte-identically,
      and re-parses a merge. This is mp:F2's acceptance command.

  python src/formats/fnt.py dump --src <install-or-zip> --font PFMENU2 --chars "AB0" --cp 0410
      ASCII-art a few glyphs (the way F1's proof-of-decode was produced).

  python src/formats/fnt.py merge --en <EN install-or-zip> --ru <RU install-or-zip> --out <dir>
      Writes an overlay tree  <dir>/mh_ex/fnt/{FONTLAY.TXT,PFMENU0..4.FNT}  -- exactly the shape
      `src/formats/build_mod.py --overlay` consumes.

  python src/formats/fnt.py install --game <game dir> [--en ... --ru ...]
      merge + repack mh_ex.rsr/.nam into the game directory, with one-time *.vanilla backups and a
      re-extract check that every non-`fnt\` member survived byte-for-byte.

A retail SOURCE is either an install directory (holding mh.rsr/mh.nam/mh_ex.rsr/mh_ex.nam) or a
.zip of one. With no --en/--ru the tool looks at machine_config's POLYGON_CLEAN / RU_CLEAN and then
at any mh_*_clean.zip in the repo's own game_data/ directory.
"""

import argparse
import io
import os
import shutil
import struct
import sys
import tempfile
import zipfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", ".."))
sys.path.insert(0, os.path.join(_REPO, "tools"))
sys.path.insert(0, _HERE)

import machine_config as machine  # noqa: E402

# decompress.py draws a tqdm bar per member; a 26-member selftest is then 26 bars of carriage
# returns interleaved with the result lines. Honoured by tqdm >= 4.66; harmless on older ones.
os.environ.setdefault("TQDM_DISABLE", "1")
from decompress import Decompressor  # noqa: E402,E401

BOM = b"\xff\xfe"
PFMENU = ("PFMENU0.FNT", "PFMENU1.FNT", "PFMENU2.FNT", "PFMENU3.FNT", "PFMENU4.FNT")
MAX_PIXEL = 0x1F  # the fonts' intensity ceiling; llm_gfx_font_draw_* indexes a 32x32 LUT with it


# --------------------------------------------------------------------------- the two file formats
class Glyph:
    """One `.FNT` record: a width byte plus width*H row-major intensity bytes."""

    __slots__ = ("width", "px", "height")

    def __init__(self, width, px, height):
        if len(px) != width * height:
            raise ValueError(f"glyph {width}x{height} needs {width * height} bytes, got {len(px)}")
        self.width = width
        self.px = bytes(px)
        self.height = height

    @classmethod
    def blank(cls, width, height):
        return cls(width, bytes(width * height), height)

    def get(self, row, col):
        if row < 0 or col < 0 or row >= self.height or col >= self.width:
            return 0
        return self.px[row * self.width + col]

    def rows(self):
        return [self.px[r * self.width:(r + 1) * self.width] for r in range(self.height)]

    def ink_bbox(self):
        """(top, left, bottom, right) of the non-zero pixels, or None for a blank glyph."""
        top = left = None
        bottom = right = -1
        for r in range(self.height):
            for c in range(self.width):
                if self.px[r * self.width + c]:
                    if top is None:
                        top = r
                    bottom = r
                    left = c if left is None else min(left, c)
                    right = max(right, c)
        return None if top is None else (top, left, bottom, right)

    def to_bytes(self):
        return bytes([self.width]) + self.px

    def art(self, ramp=" .:-=+*#%@"):
        out = []
        for r in range(self.height):
            out.append("".join(ramp[min(len(ramp) - 1, self.px[r * self.width + c] * len(ramp) // (MAX_PIXEL + 1))]
                               for c in range(self.width)))
        return out


class Font:
    """A whole `.FNT` file: the height header byte plus the record list, in file order."""

    def __init__(self, height, glyphs):
        self.height = height
        self.glyphs = list(glyphs)

    @classmethod
    def parse(cls, data, name="<bytes>"):
        if len(data) < 1:
            raise ValueError(f"{name}: empty")
        height = data[0]
        if height == 0:
            raise ValueError(f"{name}: header row count is 0")
        glyphs, p = [], 1
        while p < len(data):
            width = data[p]
            end = p + 1 + width * height
            if end > len(data):
                raise ValueError(f"{name}: record {len(glyphs)} at 0x{p:x} (w={width}, H={height}) "
                                 f"runs {end - len(data)} bytes past EOF -- not a .FNT, or not H={height}")
            glyphs.append(Glyph(width, data[p + 1:end], height))
            p = end
        return cls(height, glyphs)

    def to_bytes(self):
        out = bytearray([self.height])
        for g in self.glyphs:
            out += g.to_bytes()
        return bytes(out)

    def glyph_for_ordinal(self, ordinal):
        """FONTLAY ordinal (0-based) -> the record the loader would hand `llm_gfx_font_layout_text`."""
        return self.glyphs[ordinal] if 0 <= ordinal < len(self.glyphs) else None


def read_fontlay(data, name="FONTLAY.TXT"):
    """UTF-16LE + BOM, no separators: one u16 code point per glyph slot, in slot order."""
    if data[:2] != BOM:
        raise ValueError(f"{name}: expected a UTF-16LE BOM, got {data[:2]!r}")
    if (len(data) - 2) % 2:
        raise ValueError(f"{name}: {len(data)} bytes is not BOM + whole u16s")
    return [struct.unpack_from("<H", data, 2 + 2 * i)[0] for i in range((len(data) - 2) // 2)]


def write_fontlay(codepoints):
    out = bytearray(BOM)
    for cp in codepoints:
        if not 0 <= cp <= 0xFFFF:
            raise ValueError(f"FONTLAY holds u16 code units; U+{cp:04X} is not one")
        out += struct.pack("<H", cp)
    return bytes(out)


class FontSet:
    """One layer's `fnt\\` members: the FONTLAY list plus the fonts that index through it."""

    def __init__(self, layout, fonts, lay_name="FONTLAY.TXT"):
        self.layout = list(layout)
        self.fonts = dict(fonts)  # UPPER-CASED basename -> Font
        self.lay_name = lay_name
        self.index = {cp: i for i, cp in enumerate(self.layout)}

    def glyph(self, font_name, codepoint):
        i = self.index.get(codepoint)
        return None if i is None else self.fonts[font_name].glyph_for_ordinal(i)

    def require(self, font_name, codepoint, why):
        g = self.glyph(font_name, codepoint)
        if g is None:
            raise KeyError(f"{font_name}: U+{codepoint:04X} is not in this layer, needed for {why}")
        return g


# --------------------------------------------------------------------------- retail sources
class Source:
    """A retail install (directory or .zip of one), read through the `.nam`/`.rsr` pair."""

    def __init__(self, path):
        self.path = str(path)
        self.zip = None
        if os.path.isfile(self.path) and self.path.lower().endswith(".zip"):
            self.zip = zipfile.ZipFile(self.path)
        elif not os.path.isdir(self.path):
            raise FileNotFoundError(f"not an install directory or .zip: {self.path}")
        self._dec = Decompressor()

    def label(self):
        return os.path.basename(self.path.rstrip("/\\")) or self.path

    def _read(self, pack, ext):
        if self.zip is not None:
            hits = [n for n in self.zip.namelist() if n.lower().endswith(pack + ext)
                    and n.lower().rstrip().split("/")[-1] == pack + ext]
            if not hits:
                return None
            return self.zip.read(sorted(hits, key=len)[0])
        f = os.path.join(self.path, pack + ext)
        return open(f, "rb").read() if os.path.isfile(f) else None

    def members(self, pack, prefix="fnt\\"):
        """{entry name: decompressed bytes} for every pack member under `prefix` (case-insensitive)."""
        nam = self._read(pack, ".nam")
        rsr = self._read(pack, ".rsr")
        if not nam or not rsr:
            return {}
        out = {}
        for i in range(len(nam) // 64):
            name, _type, off, size, _final = struct.unpack("<47s5s3I", nam[i * 64:(i + 1) * 64])
            name = name.split(b"\0")[0].decode("ascii")
            if not name.lower().startswith(prefix.lower()):
                continue
            out[name] = self._dec.decompress(rsr[off:off + size])
        return out

    def all_members(self, pack):
        return self.members(pack, prefix="")

    def font_layer(self, pack="mh_ex"):
        """The FontSet this pack layer contributes, or None if it carries no `fnt\\` members."""
        raw = self.members(pack)
        if not raw:
            return None
        lay_key = next((k for k in raw if k.upper().endswith("FONTLAY.TXT")), None)
        if lay_key is None:
            return None
        fonts = {}
        for k, v in raw.items():
            base = k.split("\\")[-1].upper()
            if base.endswith(".FNT"):
                fonts[base] = Font.parse(v, name=f"{self.label()}:{k}")
        return FontSet(read_fontlay(raw[lay_key], lay_key), fonts, lay_name=lay_key)


def _candidate_sources(explicit=None):
    """Retail sources to try, most specific first. Never invents a path outside these three."""
    out, seen = [], set()

    def add(p):
        if p and str(p) not in seen and (os.path.isdir(str(p)) or os.path.isfile(str(p))):
            seen.add(str(p))
            out.append(str(p))

    for p in (explicit or []):
        add(p)
    if not explicit:
        for p in (os.environ.get("MH_FNT_RETAIL") or "").split(os.pathsep):
            add(p.strip())
        for key in ("POLYGON_CLEAN", "RU_CLEAN", "POLYGON"):
            add(getattr(machine, key, None))
        # The retail-artifact drop: `game_data/` beside the repo (gitignored) and beside the game
        # installs, so a worktree with no game_data of its own still finds the machine's copy.
        roots = [_REPO, os.path.dirname(str(getattr(machine, "GAMES_ROOT", "") or "").rstrip("/\\"))]
        for root in roots:
            gd = os.path.join(root, "game_data") if root else None
            if gd and os.path.isdir(gd):
                for n in sorted(os.listdir(gd)):
                    if n.lower().endswith(".zip") and "clean" in n.lower():
                        add(os.path.join(gd, n))
    return out


def open_source(path, what):
    try:
        return Source(path)
    except (FileNotFoundError, zipfile.BadZipFile) as exc:
        raise SystemExit(f"{what}: {exc}")


def resolve_layers(paths, what):
    """First candidate that actually yields a `fnt\\` override layer. (path, FontSet) or (None, None)."""
    for p in paths:
        try:
            src = Source(p)
        except (FileNotFoundError, zipfile.BadZipFile):
            continue
        layer = src.font_layer("mh_ex")
        if layer is not None and layer.fonts:
            return src, layer
    return None, None


# --------------------------------------------------------------------------- glyph derivation
#
# The 23 glyphs no retail layer ships are DERIVED from glyphs that a layer does ship, in the SAME
# font -- never drawn as literal bitmaps. Two reasons, and the second is the load-bearing one:
#   * 23 glyphs x 5 fonts x 5 different cell heights = 115 bitmaps to author and re-author whenever
#     a metric changes. A derivation table is 23 rows.
#   * A lifted mark carries the font's own stroke weight, dot size and vertical placement. Measured:
#     in every one of PFMENU0-4, U+00C1 A-acute differs from U+0041 A in exactly the top TWO rows
#     and has the IDENTICAL width, and U+00C4 A-diaeresis differs in exactly one row. So "the acute"
#     and "the diaeresis" can be lifted as pixel-exact diffs and re-centred over another letter.
#
# Only the ogonek, the L-stroke and the box are synthesised, because no layer ships a glyph carrying
# them. They are drawn from the font's own measurements (stem column, ink baseline, cap height), not
# from constants.

CYRILLIC_LIFT_NOTE = "lifted from the RU mh_ex override layer"

# (target, base, mark source pair) -- mark = (accented - base), re-centred over the target's ink.
ACUTE_U = (0x00C1, 0x0041)  # A-acute  - A
ACUTE_L = (0x00E1, 0x0061)  # a-acute  - a
DIAER_U = (0x00C4, 0x0041)  # A-diaeresis - A
DIAER_L = (0x00E4, 0x0061)  # a-diaeresis - a

DERIVED = [
    # --- the 5 Cyrillic letters NO layer ships (the fonts page's "Verdict") -----------------------
    ("copy", 0x041A, 0x004B, None, "CYRILLIC CAPITAL KA is the Latin K letterform"),
    ("mark", 0x0401, 0x0415, DIAER_U, "CYRILLIC CAPITAL IO = IE + diaeresis"),
    ("mark", 0x0451, 0x0435, DIAER_L, "CYRILLIC SMALL IO = ie + diaeresis"),
    ("hardsign", 0x042A, 0x042C, None, "CYRILLIC CAPITAL HARD SIGN = SOFT SIGN + a top-left bar"),
    ("yu", 0x042E, 0x041E, None, "CYRILLIC CAPITAL YU = stem + connector + O"),
    # --- the 18 Polish letters no layer ships anywhere ----------------------------------------
    ("mark", 0x0106, 0x0043, ACUTE_U, "C WITH ACUTE"),
    ("mark", 0x0107, 0x0063, ACUTE_L, "c with acute"),
    ("mark", 0x0143, 0x004E, ACUTE_U, "N WITH ACUTE"),
    ("mark", 0x0144, 0x006E, ACUTE_L, "n with acute"),
    ("mark", 0x00D3, 0x004F, ACUTE_U, "O WITH ACUTE"),
    ("mark", 0x00F3, 0x006F, ACUTE_L, "o with acute"),
    ("mark", 0x015A, 0x0053, ACUTE_U, "S WITH ACUTE"),
    ("mark", 0x015B, 0x0073, ACUTE_L, "s with acute"),
    ("mark", 0x0179, 0x005A, ACUTE_U, "Z WITH ACUTE"),
    ("mark", 0x017A, 0x007A, ACUTE_L, "z with acute"),
    ("dot", 0x017B, 0x005A, DIAER_U, "Z WITH DOT ABOVE -- one half of the diaeresis, re-centred"),
    ("dot", 0x017C, 0x007A, DIAER_L, "z with dot above"),
    ("ogonek", 0x0104, 0x0041, None, "A WITH OGONEK"),
    ("ogonek", 0x0105, 0x0061, None, "a with ogonek"),
    ("ogonek", 0x0118, 0x0045, None, "E WITH OGONEK"),
    ("ogonek", 0x0119, 0x0065, None, "e with ogonek"),
    ("stroke", 0x0141, 0x004C, None, "L WITH STROKE"),
    ("stroke", 0x0142, 0x006C, None, "l with stroke"),
    # --- the substitute the DLL guard points an unmapped code unit at -------------------------
    ("box", 0xFFFD, 0x0041, None, "REPLACEMENT CHARACTER -- the guard's box glyph"),
]

POLISH = tuple(cp for kind, cp, _b, _m, _w in DERIVED if 0x0100 <= cp <= 0x017F or cp in (0x00D3, 0x00F3))
DRAWN_CYRILLIC = (0x041A, 0x0401, 0x0451, 0x042A, 0x042E)
BOX_CODEPOINT = 0xFFFD


def _paint(width, height, pixels):
    buf = bytearray(width * height)
    for (r, c), v in pixels.items():
        if 0 <= r < height and 0 <= c < width:
            buf[r * width + c] = v
    return Glyph(width, buf, height)


def _shifted(g, dx, new_width):
    px = bytearray(new_width * g.height)
    for r in range(g.height):
        for c in range(g.width):
            if 0 <= c + dx < new_width:
                px[r * new_width + c + dx] = g.px[r * g.width + c]
    return Glyph(new_width, px, g.height)


def extract_mark(font_set, font_name, pair, why):
    """The pixels an accented glyph adds to its base: {(row, col): value}, plus the base's ink box.

    Both glyphs must have the same advance width -- measured true for every accent pair in all five
    PFMENU fonts. If a future font breaks that, this raises rather than silently mis-placing a mark.
    """
    acc_cp, base_cp = pair
    acc = font_set.require(font_name, acc_cp, why)
    base = font_set.require(font_name, base_cp, why)
    if acc.width != base.width:
        raise ValueError(f"{font_name}: U+{acc_cp:04X} is {acc.width}px wide but its base "
                         f"U+{base_cp:04X} is {base.width}px -- cannot lift the mark by difference")
    mark = {}
    for r in range(acc.height):
        for c in range(acc.width):
            a = acc.get(r, c)
            if a and a != base.get(r, c):
                mark[(r, c)] = a
    if not mark:
        raise ValueError(f"{font_name}: U+{acc_cp:04X} and U+{base_cp:04X} are identical -- no mark")
    return mark, base.ink_bbox()


def _mark_centre(mark):
    cols = [c for (_r, c) in mark]
    return (min(cols) + max(cols)) / 2.0


def apply_mark(base, mark, src_box, half=None):
    """Overlay a lifted mark on `base`, re-centred from the mark's source letter onto this one.

    `half` = "left"/"right" keeps only that side of the mark (how a dot-above is made from a
    diaeresis). Widens the glyph only if the mark would otherwise fall outside it.
    """
    if half:
        centre = _mark_centre(mark)
        mark = {k: v for k, v in mark.items() if (k[1] < centre if half == "left" else k[1] > centre)}
        if not mark:
            raise ValueError("splitting the mark left an empty half")
    box = base.ink_bbox()
    if box is None:
        raise ValueError("cannot place a mark over a blank base glyph")
    # The mark was drawn centred over ITS source letter; move it by the difference between the two
    # letters' ink centres so it lands centred over this one.
    src_c = (src_box[1] + src_box[3]) / 2.0
    dst_c = (box[1] + box[3]) / 2.0
    shift = int(round(dst_c - src_c))
    placed = {(r, c + shift): v for (r, c), v in mark.items()}
    top = min(r for (r, _c) in placed)
    if top > box[0] - 1 and top >= box[0]:
        raise ValueError("the mark would land on the letter, not above it")
    lo = min(c for (_r, c) in placed)
    hi = max(c for (_r, c) in placed)
    left_pad = max(0, -lo)
    width = max(base.width + left_pad, hi + 1 + left_pad)
    out = _shifted(base, left_pad, width)
    px = bytearray(out.px)
    for (r, c), v in placed.items():
        cc = c + left_pad
        if 0 <= r < out.height and 0 <= cc < width:
            px[r * width + cc] = v
    return Glyph(width, px, out.height)


def apply_ogonek(base, ink=MAX_PIXEL):
    """A tail hooked under the base's bottom-right ink, in the rows the cell already leaves blank."""
    box = base.ink_bbox()
    if box is None:
        raise ValueError("cannot hook an ogonek under a blank glyph")
    bottom = box[2]
    free = base.height - 1 - bottom
    if free < 2:
        raise ValueError(f"only {free} blank row(s) under the baseline -- no room for an ogonek")
    last = [c for c in range(base.width) if base.get(bottom, c)]
    anchor = last[-1] if last else box[3]
    px = bytearray(base.px)
    for (r, c) in ((bottom + 1, anchor), (bottom + 2, anchor), (bottom + 2, anchor - 1)):
        if 0 <= c < base.width and r < base.height:
            px[r * base.width + c] = ink
    return Glyph(base.width, px, base.height)


def apply_stroke(base, ink=MAX_PIXEL):
    """The Polish L-bar: a 3px cross-stroke at mid ink height, with a column added on the left so
    the bar sticks out the way it does in a real L-stroke instead of reading as a serif."""
    box = base.ink_bbox()
    if box is None:
        raise ValueError("cannot stroke a blank glyph")
    top, left, bottom, _right = box
    row = (top + bottom) // 2
    out = _shifted(base, 1, base.width + 1)
    px = bytearray(out.px)
    for c in (left, left + 1, left + 2):
        if 0 <= c < out.width:
            px[row * out.width + c] = ink
    return Glyph(out.width, px, out.height)


def make_hard_sign(soft_sign, ink=MAX_PIXEL):
    """CYRILLIC CAPITAL HARD SIGN from the SOFT SIGN: the same body, plus the top-left bar."""
    box = soft_sign.ink_bbox()
    if box is None:
        raise ValueError("SOFT SIGN is blank")
    top, left, _bottom, _right = box
    pad = 2
    out = _shifted(soft_sign, pad, soft_sign.width + pad)
    px = bytearray(out.px)
    for c in range(0, left + pad + 1):
        px[top * out.width + c] = ink
    return Glyph(out.width, px, out.height)


def make_yu(o_glyph, ink=MAX_PIXEL):
    """CYRILLIC CAPITAL YU: a full-height stem, a connector at mid height, then the O bowl."""
    box = o_glyph.ink_bbox()
    if box is None:
        raise ValueError("O is blank")
    top, left, bottom, _right = box
    gap = 2
    pad = 1 + gap
    out = _shifted(o_glyph, pad, o_glyph.width + pad)
    px = bytearray(out.px)
    for r in range(top, bottom + 1):
        px[r * out.width] = ink
    mid = (top + bottom) // 2
    for c in range(0, left + pad):
        px[mid * out.width + c] = ink
    return Glyph(out.width, px, out.height)


def make_box(cap, ink=MAX_PIXEL):
    """The substitute glyph: a hollow rectangle spanning the reference letter's cap height."""
    box = cap.ink_bbox()
    if box is None:
        raise ValueError("the reference capital is blank")
    top, _left, bottom, _right = box
    height = bottom - top + 1
    width = max(3, (height * 2) // 3)
    total = width + 1  # one blank column of advance, like every other glyph in these fonts
    px = bytearray(total * cap.height)
    for c in range(width):
        px[top * total + c] = ink
        px[bottom * total + c] = ink
    for r in range(top, bottom + 1):
        px[r * total] = ink
        px[r * total + width - 1] = ink
    return Glyph(total, px, cap.height)


def derive_glyph(kind, base_cp, mark_pair, font_set, font_name, why):
    base = font_set.require(font_name, base_cp, why)
    if kind == "copy":
        return base
    if kind in ("mark", "dot"):
        mark, src_box = extract_mark(font_set, font_name, mark_pair, why)
        return apply_mark(base, mark, src_box, half="left" if kind == "dot" else None)
    if kind == "ogonek":
        return apply_ogonek(base)
    if kind == "stroke":
        return apply_stroke(base)
    if kind == "hardsign":
        return make_hard_sign(base)
    if kind == "yu":
        return make_yu(base)
    if kind == "box":
        return make_box(base)
    raise ValueError(f"unknown derivation kind {kind!r}")


# --------------------------------------------------------------------------- the merge
class MergeReport:
    def __init__(self):
        self.lifted = []     # code points copied from RU
        self.derived = []    # code points synthesised
        self.skipped = []    # code points already present in EN
        self.lines = []

    def say(self, line):
        self.lines.append(line)


def merge(en_layer, ru_layer, fonts=PFMENU, report=None):
    """EN override + RU override + the derivation table -> a merged FontSet.

    Strictly ADDITIVE: every existing FONTLAY entry keeps its ordinal and every existing record
    keeps its bytes, so the merged files are a byte-prefix of nothing less than the originals and
    every already-rendered ASCII string is bit-identical. `verify_prefix()` asserts it.
    """
    report = report or MergeReport()
    layout = list(en_layer.layout)
    index = {cp: i for i, cp in enumerate(layout)}
    merged = {name: Font(en_layer.fonts[name].height, en_layer.fonts[name].glyphs) for name in fonts}

    # The record list must be at least as long as the FONTLAY list before we start appending: the
    # shipped files carry 2-3 spare records past the last listed ordinal, and a new ordinal has to
    # OVERWRITE the spare at its index, not land after it.
    spare = {}
    for name in fonts:
        f = merged[name]
        if len(f.glyphs) < len(layout):
            raise ValueError(f"{name}: {len(f.glyphs)} records for {len(layout)} FONTLAY entries")
        spare[name] = f.glyphs[len(layout):]
        f.glyphs = f.glyphs[:len(layout)]

    def append(cp, per_font, note):
        if cp in index:
            report.skipped.append(cp)
            return False
        layout.append(cp)
        index[cp] = len(layout) - 1
        for name in fonts:
            merged[name].glyphs.append(per_font[name])
        report.say(f"  U+{cp:04X}  {note}")
        return True

    # 1. lift every Cyrillic code point the RU override actually ships, in ITS FONTLAY order so the
    #    alphabet lands in a stable, reproducible sequence.
    for cp in ru_layer.layout:
        if not (0x0400 <= cp <= 0x04FF):
            continue
        per_font = {}
        for name in fonts:
            g = ru_layer.glyph(name, cp)
            if g is None:
                per_font = None
                break
            if g.height != merged[name].height:
                raise ValueError(f"{name}: RU cell height {g.height} != EN {merged[name].height} "
                                 f"-- the lift is only valid while the metrics match")
            per_font[name] = g
        if per_font is None:
            report.say(f"  U+{cp:04X}  SKIPPED: the RU layer does not carry it in every font")
            continue
        if append(cp, per_font, CYRILLIC_LIFT_NOTE):
            report.lifted.append(cp)

    # 2. derive what neither layer ships. Order matters: the Cyrillic IO letters are built on the
    #    IE letters lifted in step 1, so derivation reads the MERGED set, not the EN one.
    merged_set = FontSet(layout, merged, lay_name=en_layer.lay_name)
    for kind, cp, base_cp, mark_pair, why in DERIVED:
        if cp in index:
            report.skipped.append(cp)
            continue
        per_font = {}
        for name in fonts:
            merged_set.layout = layout
            merged_set.index = index
            per_font[name] = derive_glyph(kind, base_cp, mark_pair, merged_set, name, why)
        if append(cp, per_font, f"derived ({kind}) from U+{base_cp:04X} -- {why}"):
            report.derived.append(cp)
        merged_set.layout = layout
        merged_set.index = index

    for name in fonts:
        merged[name].glyphs.extend(spare[name])  # keep the shipped spares, now past the new tail

    return FontSet(layout, merged, lay_name=en_layer.lay_name), report


def verify_prefix(original, merged_font, n_original_records):
    """The merged file must begin with the original file's bytes, minus its trailing spares.

    This is the mechanical form of "the stock EN ASCII baselines are unchanged": if every original
    record is byte-identical AND at its original index, no already-mapped code point can render
    differently, whatever we appended.
    """
    head = Font(original.height, original.glyphs[:n_original_records]).to_bytes()
    got = Font(merged_font.height, merged_font.glyphs[:n_original_records]).to_bytes()
    return head == got


# --------------------------------------------------------------------------- overlay / repack
def write_overlay(merged, out_dir, lay_name=None, fnt_names=None):
    """Write the merged layer as `<out>/mh_ex/fnt/*` -- the shape build_mod.py --overlay expects."""
    fnt_dir = os.path.join(out_dir, "mh_ex", "fnt")
    os.makedirs(fnt_dir, exist_ok=True)
    written = []
    lay = (lay_name or merged.lay_name).split("\\")[-1]
    p = os.path.join(fnt_dir, lay)
    with open(p, "wb") as f:
        f.write(write_fontlay(merged.layout))
    written.append(p)
    for name, font in sorted(merged.fonts.items()):
        real = (fnt_names or {}).get(name, name)
        p = os.path.join(fnt_dir, real.split("\\")[-1])
        with open(p, "wb") as f:
            f.write(font.to_bytes())
        written.append(p)
    return written


def repack_mh_ex(source, overlay_dir, out_dir):
    """Stage the source's whole mh_ex layer, drop the overlay over it, rebuild mh_ex.rsr/.nam.

    Returns (staged_member_count, replaced_names). The staged tree is the DECOMPRESSED members --
    pack.py stores entries uncompressed and the loader only inflates on the `LZW ` magic, which is
    the same thing build_mod.py has done since 2026-07-05.
    """
    from pack import pack as pack_tree  # local: pack.py imports machine_config at module scope

    staging = os.path.join(out_dir, "packsrc")
    ex_dir = os.path.join(staging, "mh_ex")
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    members = source.all_members("mh_ex")
    if not members:
        raise SystemExit(f"{source.path}: no mh_ex pack members -- is mh_ex.rsr/.nam present?")
    for name, data in members.items():
        p = os.path.join(ex_dir, *name.split("\\"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    replaced = []
    ov = os.path.join(overlay_dir, "mh_ex")
    for path, _dirs, files in os.walk(ov):
        rel = os.path.relpath(path, ov)
        for n in files:
            dst_dir = ex_dir if rel == "." else os.path.join(ex_dir, rel)
            os.makedirs(dst_dir, exist_ok=True)
            shutil.copy2(os.path.join(path, n), os.path.join(dst_dir, n))
            replaced.append(os.path.normpath(os.path.join(rel, n)))
    pack_tree(staging, out_dir, ["mh_ex"])
    return len(members), replaced


def verify_repack(original_members, packed_dir, replaced):
    """Every member we did NOT replace must come back out of the new pack byte-for-byte."""
    src = Source(packed_dir)
    now = src.all_members("mh_ex")
    repl = {r.replace("/", "\\").lower() for r in replaced}
    problems = []
    if set(k.lower() for k in now) != set(k.lower() for k in original_members):
        missing = set(original_members) - set(now)
        added = set(now) - set(original_members)
        problems.append(f"member set changed: -{sorted(missing)[:4]} +{sorted(added)[:4]}")
    for name, data in original_members.items():
        if name.lower() in repl:
            continue
        got = now.get(name)
        if got is None or got != data:
            problems.append(f"{name} did not survive the repack byte-for-byte")
    return problems


# --------------------------------------------------------------------------- commands
def cmd_dump(args):
    src = open_source(args.src or (_candidate_sources() or [""])[0], "--src")
    layer = src.font_layer(args.pack)
    if layer is None:
        raise SystemExit(f"{src.path}: pack {args.pack} carries no fnt\\ members")
    name = args.font.upper()
    if not name.endswith(".FNT"):
        name += ".FNT"
    if name not in layer.fonts:
        raise SystemExit(f"{name} is not in this layer; have {sorted(layer.fonts)}")
    font = layer.fonts[name]
    print(f"{src.label()}:{args.pack}:{layer.lay_name}  {len(layer.layout)} FONTLAY entries")
    print(f"{name}  H={font.height}  records={len(font.glyphs)}  "
          f"(ordinal i -> record i; {len(font.glyphs) - len(layer.layout)} spare)")
    wanted = [ord(c) for c in (args.chars or "")] + [int(x, 16) for x in (args.cp or [])]
    for cp in wanted:
        g = layer.glyph(name, cp)
        if g is None:
            print(f"U+{cp:04X}: not in FONTLAY")
            continue
        print(f"U+{cp:04X} ordinal={layer.index[cp]} w={g.width}")
        for line in g.art():
            print("   |" + line + "|")
    return 0


def _load_two_layers(args):
    en_src, en = resolve_layers(_candidate_sources([args.en] if args.en else None), "EN")
    if en is None:
        raise SystemExit("no EN override layer found -- pass --en <install dir or .zip>")
    ru_src, ru = (None, None)
    ru_paths = [args.ru] if args.ru else [p for p in _candidate_sources() if p != en_src.path]
    for p in ru_paths:
        try:
            cand = Source(p)
        except (FileNotFoundError, zipfile.BadZipFile):
            continue
        layer = cand.font_layer("mh_ex")
        if layer and any(0x0400 <= cp <= 0x04FF for cp in layer.layout):
            ru_src, ru = cand, layer
            break
    if ru is None:
        raise SystemExit("no Cyrillic-carrying override layer found -- pass --ru <RU install or .zip>")
    return en_src, en, ru_src, ru


def cmd_merge(args, quiet=False):
    en_src, en, ru_src, ru = _load_two_layers(args)
    merged, report = merge(en, ru)
    if not quiet:
        print(f"EN base   : {en_src.path}  ({len(en.layout)} FONTLAY entries)")
        print(f"RU source : {ru_src.path}  ({len(ru.layout)} FONTLAY entries)")
        for line in report.lines:
            print(line)
    # the additivity proof, per font
    for name in PFMENU:
        if not verify_prefix(en.fonts[name], merged.fonts[name], len(en.layout)):
            raise SystemExit(f"{name}: the merge moved or changed an existing record -- refusing")
    if merged.layout[:len(en.layout)] != en.layout:
        raise SystemExit("the merge reordered FONTLAY -- refusing")
    names = {n: n for n in PFMENU}
    for k in en.fonts:
        names.setdefault(k, k)
    out = write_overlay(merged, args.out, lay_name=en.lay_name, fnt_names=names)
    if not quiet:
        print(f"lifted {len(report.lifted)} Cyrillic, derived {len(report.derived)} "
              f"({len(DRAWN_CYRILLIC)} Cyrillic + {len(POLISH)} Polish + 1 box), "
              f"skipped {len(report.skipped)} already present")
        print(f"FONTLAY {len(en.layout)} -> {len(merged.layout)} entries")
        for p in out:
            print(f"  wrote {p} ({os.path.getsize(p)} bytes)")
    return merged, report, en_src


def cmd_install(args):
    build = args.build_dir or os.path.join(tempfile.gettempdir(), "mh_fnt_build")
    os.makedirs(build, exist_ok=True)
    overlay = os.path.join(build, "overlay")
    ns = argparse.Namespace(en=args.en, ru=args.ru, out=overlay)
    merged, report, en_src = cmd_merge(ns)
    game = args.game or machine.POLYGON
    src = Source(args.pack_source) if args.pack_source else Source(game)
    original = src.all_members("mh_ex")
    out_dir = os.path.join(build, "out")
    os.makedirs(out_dir, exist_ok=True)
    n, replaced = repack_mh_ex(src, overlay, out_dir)
    problems = verify_repack(original, out_dir, replaced)
    if problems:
        for p in problems[:10]:
            print("  REFUSED: " + p)
        raise SystemExit("the repack is not byte-faithful on the members it did not replace")
    print(f"repacked mh_ex: {n} members, {len(replaced)} replaced, every other member verified identical")
    if args.no_install:
        print(f"--no-install: outputs left in {out_dir}")
        return 0
    for ext in (".rsr", ".nam"):
        live = os.path.join(game, "mh_ex" + ext)
        backup = live + ".vanilla"
        if os.path.exists(live) and not os.path.exists(backup):
            shutil.copy2(live, backup)
            print(f"  backed up mh_ex{ext} -> mh_ex{ext}.vanilla")
        tmp = live + ".new"
        shutil.copy2(os.path.join(out_dir, "mh_ex" + ext), tmp)
        os.replace(tmp, live)  # atomic: a lane symlinked to this pack never sees a half-written file
        print(f"  installed mh_ex{ext} into {game}")
    print("revert with: copy <game>\\mh_ex.rsr.vanilla mh_ex.rsr  (and .nam)")
    return 0


def cmd_selftest(args):
    fails, checked, sources = [], 0, []
    for path in _candidate_sources([args.src] if getattr(args, "src", None) else None):
        try:
            src = Source(path)
        except (FileNotFoundError, zipfile.BadZipFile) as exc:
            print(f"  skip {path}: {exc}")
            continue
        got_any = False
        for pack in ("mh", "mh_ex"):
            for name, data in sorted(src.members(pack).items()):
                got_any = True
                checked += 1
                base = name.split("\\")[-1].upper()
                try:
                    if base.endswith(".FNT"):
                        obj = Font.parse(data, name=f"{src.label()}:{name}")
                        back = obj.to_bytes()
                        extra = (f"H={obj.height} records={len(obj.glyphs)}")
                    else:
                        cps = read_fontlay(data, name)
                        back = write_fontlay(cps)
                        extra = f"{len(cps)} code points"
                    if back != data:
                        fails.append(f"{src.label()}:{pack}:{name} round-trip differs "
                                     f"({len(data)} -> {len(back)} bytes)")
                    else:
                        print(f"  OK  {src.label():18} {pack:6} {name:22} {len(data):7} bytes  {extra}")
                except (ValueError, KeyError) as exc:
                    fails.append(f"{src.label()}:{pack}:{name}: {exc}")
        if got_any:
            sources.append(src)

    if not checked:
        print("NO RETAIL SOURCE FOUND. --selftest needs an install (or a .zip of one) to round-trip.")
        print("Looked at: " + ", ".join(_candidate_sources()) or "(nothing)")
        print("Pass --src <install dir or .zip>, or set POLYGON_CLEAN / RU_CLEAN in machine.local.json.")
        return 2

    # The merge has to survive its own re-parse, and every appended code point has to resolve to a
    # glyph with ink -- a silently blank derivation is the failure mode this catches.
    try:
        en_src, en, ru_src, ru = _load_two_layers(argparse.Namespace(en=None, ru=None))
    except SystemExit as exc:
        print(f"  merge check SKIPPED: {exc}")
        en = None
    if en is not None:
        merged, report = merge(en, ru)
        for name in PFMENU:
            blob = merged.fonts[name].to_bytes()
            again = Font.parse(blob, name=name)
            if again.to_bytes() != blob:
                fails.append(f"merged {name} does not re-parse to itself")
            if not verify_prefix(en.fonts[name], merged.fonts[name], len(en.layout)):
                fails.append(f"merged {name} changed an already-shipped record")
            for cp in list(report.lifted) + list(report.derived):
                g = FontSet(merged.layout, merged.fonts).glyph(name, cp)
                if g is None:
                    fails.append(f"merged {name}: U+{cp:04X} has no record")
                elif g.ink_bbox() is None:
                    fails.append(f"merged {name}: U+{cp:04X} is blank")
        lay = write_fontlay(merged.layout)
        if read_fontlay(lay) != merged.layout:
            fails.append("merged FONTLAY does not round-trip")
        print(f"  OK  merge  {len(en.layout)} -> {len(merged.layout)} FONTLAY entries; "
              f"{len(report.lifted)} lifted + {len(report.derived)} derived, all non-blank in all "
              f"{len(PFMENU)} fonts; every shipped record byte-identical at its original index")

    print()
    if fails:
        for f in fails:
            print("FAIL " + f)
        print(f"{len(fails)} failure(s), {checked} member(s) checked")
        return 1
    print(f"selftest PASS -- {checked} shipped fnt\\ member(s) round-tripped byte-identically "
          f"from {len(sources)} retail source(s)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true",
                    help="round-trip every shipped fnt\\ member byte-identically, then check a merge")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("selftest", help="same as --selftest")
    p.add_argument("--src", help="one retail install directory or .zip (default: search)")
    p.set_defaults(fn=cmd_selftest)

    p = sub.add_parser("dump", help="ASCII-art glyphs out of a font")
    p.add_argument("--src", help="retail install directory or .zip")
    p.add_argument("--pack", default="mh_ex", choices=("mh", "mh_ex"))
    p.add_argument("--font", default="PFMENU2")
    p.add_argument("--chars", help="literal characters to render, e.g. \"A0z\"")
    p.add_argument("--cp", nargs="*", help="code points in hex, e.g. 0410 0104")
    p.set_defaults(fn=cmd_dump)

    p = sub.add_parser("merge", help="write the merged overlay tree")
    p.add_argument("--en", help="EN retail install or .zip (default: search)")
    p.add_argument("--ru", help="RU retail install or .zip (default: search)")
    p.add_argument("--out", required=True, help="overlay root; files land in <out>/mh_ex/fnt/")
    p.set_defaults(fn=lambda a: (cmd_merge(a), 0)[1])

    p = sub.add_parser("install", help="merge + repack mh_ex into a game directory")
    p.add_argument("--en")
    p.add_argument("--ru")
    p.add_argument("--game", help="game directory to install into (default: machine.POLYGON)")
    p.add_argument("--pack-source", help="where to take the mh_ex members from (default: --game)")
    p.add_argument("--build-dir")
    p.add_argument("--no-install", action="store_true")
    p.set_defaults(fn=cmd_install)

    args = ap.parse_args(argv)
    if args.selftest and not args.cmd:
        args.src = None
        return cmd_selftest(args)
    if not args.cmd:
        ap.print_help()
        return 2
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
