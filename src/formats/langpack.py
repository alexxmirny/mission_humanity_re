#!/usr/bin/env python3
r"""langpack.py -- build a LANGUAGE PACK for the EN exe from a legally obtained install (mods:LANG3).

A language pack is the folder `[lang] pack=<id>` points mh.dll at (mods:LANG1, the lang pack seam
src/mh_dll/mh/seams/lang_pack.cpp):

    <game>\lang\<id>\mh_ex.rsr + mh_ex.nam     the localized resource pack (a matched pair)
    <game>\lang\<id>\Msgs.dat                  the title / CD-prompt strings (when the source has one)
    <game>\lang\<id>\BUILD.txt                 what was built from what (MD5s, no timestamps)

`mh.rsr` is byte-identical across the EN and RU installs and stays shared; everything localized
lives in `mh_ex`. The RU pack this tool writes is the RETAIL RU `mh_ex` with four kinds of member
replaced, and every other member carried over as its original stored bytes:

  1. FONTS (fnt\FontLay.txt + fnt\PFMENU0..4.FNT) -- `fnt.merge_onto(RU, EN, is_latin_accent)`: the
     RU override is the BASE (every RU string renders exactly as retail RU does), the EN override
     donates its 42 Latin accents, and fnt.py's derivation table adds what neither ships (the five
     Cyrillic letters RU never drew -- K, hard sign, YU, IO, io -- the 18 Polish letters and the
     guard's box). Strictly additive over RU: fnt.verify_prefix, per font.
  2. init\initlang.cfg -- the TEXT entries the EN initlang has and the RU one lacks, appended with
     the EN text as fallback (retail: seven -- K_CONS_ENERGY, K_CONS_EDIT_UNITS, BRON_BOMBA_CX2,
     K_PRZYJACIEL_JEDNOSTKA, K_CONS_HOL_MIEJSCE4, K_MENU_Rocks, K_MENU_DiplomacyTip). A TEXT entry
     is bound to its G_TEXT_PTRS slot by NAME (the K_* DEFINE id in the shared INIT.CFG; object
     names like BRON_BOMBA_CX2 by the object's own name), not by file order, so appending at the end
     is exactly as good as inserting in place. The list is computed, never kept here as data.
  3. menu\MenuBck2.gfx + menu\NetGameH.gfx (mods:LANG2) -- retail RU ships MenuBck2 as a byte COPY
     of the 6-button MenuBck1 and NetGameH as a copy of NewGameH, so mh.dll's 7-button menu would
     land every hotspot one row off its painted label. build_menu_art() re-lays the RU background
     into the stock 7-button layout (NETWORK GAME in row 3, the four rows below it moved down one
     40 px slot, a 7th frame) and composes the label "СЕТЕВАЯ ИГРА" from letters ALREADY PAINTED in
     the RU art -- "СЕТЕ" from ВСТУПЛЕНИЕ, "ВАЯ" and "ИГРА" from НОВАЯ ИГРА -- for both the
     background and the hover sprite. No letter is drawn; every label pixel is a retail RU pixel.
  4. Msgs.dat -- copied verbatim (CP1251 bytes: its strings render right only under `[input] codepage=1251` or a 1251 system ANSI codepage).

Plus one file that is NOT retail data: `mh_strings.txt` (mods:LANG4), mh.dll's OWN player-visible
text in the pack's language, copied from the committed translation src/formats/mh_strings/<id>.txt
(built with and without --no-art). Its keys and printf shapes are checked against
src/mh_dll/mh/ui/player_strings.def (strings_problems(); tools/lint_player_strings.py runs the same
check); a key it lacks shows mh.dll's English.

NOTHING THIS PRODUCES IS IN THE REPO, and must not be: 100% of it is retail game data. It is
regenerated per machine from the user's own installs, like fnt.py's merged fonts.

BYTE-IDENTICAL REBUILDS. The pack is written in the source .nam's own member order, untouched
members keep their stored (compressed) bytes, replaced members are stored uncompressed, and every
derivation is a pure function of the inputs -- so the same inputs give the same bytes. --selftest
builds twice and compares.

USAGE
  python src/formats/langpack.py --selftest
  python src/formats/langpack.py build [--ru <RU install|zip>] [--en <EN install|zip>]
                                       [--game <dir>] [--id ru] [--no-art]
      writes <game>\lang\<id>\ (default game = machine_config POLYGON). --no-art keeps the retail
      6-button menu art, which exercises mh.dll's fallback label.
  python src/formats/langpack.py check [--game <dir>] [--id ru]
      prints what the installed pack carries (the UI suite's precondition reads the same facts).
  python src/formats/langpack.py art --out <dir> [--ru ..] [--en ..]
      writes the composed art as PNGs (and the retail RU inputs beside them) for reading.
"""

import argparse
import hashlib
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
os.environ.setdefault("TQDM_DISABLE", "1")

import fnt  # noqa: E402
import machine_config as machine  # noqa: E402

TOOL = "src/formats/langpack.py"
STRINGS_DIR = os.path.join(_HERE, "mh_strings")  # mods:LANG4: our translations of mh.dll's own text
STRINGS_DEF = os.path.join(_REPO, "src", "mh_dll", "mh", "ui", "player_strings.def")
FORMAT_VERSION = 1  # bump when the derivation changes, so BUILD.txt says which recipe made a pack

# ---------------------------------------------------------------------------------- pack I/O
NAM_ENTRY = struct.Struct("<47s5s3I")  # name, type, offset, stored size, final size (unpack.py)


class Member:
    """One .nam entry and its STORED payload (possibly LZW) -- round-trips byte-for-byte."""

    def __init__(self, name, type_, stored, final_size):
        self.name = name
        self.type = type_
        self.stored = stored
        self.final_size = final_size

    def key(self):
        return self.name.lower()


def read_pack(src, pack="mh_ex"):
    nam = src._read(pack, ".nam")
    rsr = src._read(pack, ".rsr")
    if not nam or not rsr:
        raise SystemExit(f"{src.path}: no {pack}.nam/.rsr pair")
    out = []
    for i in range(len(nam) // 64):
        name, type_, off, size, final = NAM_ENTRY.unpack(nam[i * 64:(i + 1) * 64])
        out.append(Member(name.split(b"\0")[0].decode("ascii"), type_, rsr[off:off + size], final))
    return out


def write_pack(members, out_dir, pack="mh_ex"):
    nam, rsr, off = bytearray(), bytearray(), 0
    for m in members:
        raw = m.name.encode("ascii")
        if len(raw) > 46:
            raise ValueError(f"member name too long for the .nam: {m.name}")
        nam += NAM_ENTRY.pack(raw, m.type, off, len(m.stored), m.final_size)
        rsr += m.stored
        off += len(m.stored)
    os.makedirs(out_dir, exist_ok=True)
    for ext, blob in ((".nam", nam), (".rsr", rsr)):
        p = os.path.join(out_dir, pack + ext)
        tmp = p + ".new"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, p)  # a lane hardlinked to the old file keeps the old bytes, never half a file


_DEC = None


def payload(m):
    """A member's decompressed bytes (the loader inflates only on the `LZW ` magic)."""
    global _DEC
    if _DEC is None:
        from decompress import Decompressor

        _DEC = Decompressor()
    return _DEC.decompress(m.stored)


def read_loose(src, name):
    """A loose file beside the packs (Msgs.dat) from an install dir or a .zip of one."""
    if src.zip is not None:
        hits = [n for n in src.zip.namelist() if n.replace("\\", "/").split("/")[-1].lower() == name.lower()]
        return src.zip.read(sorted(hits, key=len)[0]) if hits else None
    for n in os.listdir(src.path):
        if n.lower() == name.lower() and os.path.isfile(os.path.join(src.path, n)):
            return open(os.path.join(src.path, n), "rb").read()
    return None


def md5(b):
    return hashlib.md5(b).hexdigest()


# ---------------------------------------------------------------------------------- sources
def _has_cyrillic(src):
    layer = src.font_layer("mh_ex")
    return bool(layer and any(fnt.is_cyrillic(cp) for cp in layer.layout))


def resolve_sources(ru=None, en=None):
    """(ru Source, en Source). Explicit paths win; else the fnt.py candidate list, split by whether
    the mh_ex override carries Cyrillic -- the one property that tells the two builds apart."""
    cands = fnt._candidate_sources()
    ru_src = en_src = None
    if ru:
        ru_src = fnt.open_source(ru, "--ru")
    if en:
        en_src = fnt.open_source(en, "--en")
    for p in cands:
        if ru_src and en_src:
            break
        try:
            s = fnt.Source(p)
            if s.font_layer("mh_ex") is None:
                continue
            cyr = _has_cyrillic(s)
        except (FileNotFoundError, zipfile.BadZipFile, OSError, ValueError):
            continue
        if cyr and ru_src is None:
            ru_src = s
        elif not cyr and en_src is None:
            en_src = s
    if ru_src is None:
        raise SystemExit("no RU install found (an mh_ex whose fonts carry Cyrillic) -- pass --ru <dir|zip>")
    if en_src is None:
        raise SystemExit("no EN install found -- pass --en <dir|zip>")
    if not _has_cyrillic(ru_src):
        raise SystemExit(f"--ru {ru_src.path}: its mh_ex fonts carry no Cyrillic -- not a RU install")
    return ru_src, en_src


# ---------------------------------------------------------------------------------- 1. fonts
LATIN_LIFT_NOTE = "lifted from the EN mh_ex override layer"


def build_fonts(ru_src, en_src):
    ru, en = ru_src.font_layer("mh_ex"), en_src.font_layer("mh_ex")
    merged, report = fnt.merge_onto(ru, en, fnt.is_latin_accent, LATIN_LIFT_NOTE)
    for name in fnt.PFMENU:
        if not fnt.verify_prefix(ru.fonts[name], merged.fonts[name], len(ru.layout)):
            raise SystemExit(f"{name}: the merge moved or changed a retail RU record -- refusing")
    if merged.layout[: len(ru.layout)] != ru.layout:
        raise SystemExit("the merge reordered the RU FONTLAY -- refusing")
    blobs = {"FONTLAY.TXT": fnt.write_fontlay(merged.layout)}
    for name in fnt.PFMENU:
        blobs[name] = merged.fonts[name].to_bytes()
    return blobs, merged, report, ru


# ---------------------------------------------------------------------------------- 2. initlang
def _text_entries(text):
    """[(key, line)] for every `TEXT "KEY" "value"` line, in file order."""
    out = []
    for line in text.split("\r\n"):
        s = line.strip()
        if not s.startswith("TEXT"):
            continue
        rest = s[4:].strip()
        if not rest.startswith('"'):
            continue
        end = rest.find('"', 1)
        if end > 0:
            out.append((rest[1:end], s))
    return out


def build_initlang(ru_blob, en_blob):
    if ru_blob[:2] != fnt.BOM or en_blob[:2] != fnt.BOM:
        raise SystemExit("initlang.cfg is expected to be UTF-16LE with a BOM")
    ru = ru_blob[2:].decode("utf-16-le")
    en = en_blob[2:].decode("utf-16-le")
    have = {k for k, _ in _text_entries(ru)}
    missing = [(k, line) for k, line in _text_entries(en) if k not in have]
    if not missing:
        return ru_blob, []
    if not ru.endswith("\r\n"):
        ru += "\r\n"
    block = [
        "",
        "// ---- mods:LANG3 language-pack builder (" + TOOL + "): TEXT entries this pack's retail",
        "// initlang did not carry, appended with the ENGLISH text as a fallback. TEXT is resolved by",
        "// NAME (INIT.CFG's DEFINE ids), so the position of these lines does not matter.",
    ]
    for _k, line in missing:
        block.append("   " + line)
    block.append("")
    text = ru + "\r\n".join(block) + "\r\n"
    return fnt.BOM + text.encode("utf-16-le"), [k for k, _ in missing]


def initlang_coverage(ru_blob, en_blob):
    """EN TEXT keys the given initlang still lacks (empty = full coverage)."""
    have = {k for k, _ in _text_entries(ru_blob[2:].decode("utf-16-le"))}
    return [k for k, _ in _text_entries(en_blob[2:].decode("utf-16-le")) if k not in have]


# ---------------------------------------------------------------------------------- 3. menu art
#
# The main-menu background is 640x480 RGB565 (`u16 w, u16 h` + pixels); a button is a 219x30 frame
# at x=3, rows y = 97 + 40k (k = 0..5 in the retail 6-button art, 0..6 in the 7-button one, where
# NETWORK GAME is k = 2). A hover sprite is one such 219x30 tile, blitted at the widget's x/y.
#
# WHY A TEMPLATE AND A THRESHOLD, NOT PIXEL EQUALITY: the EN and RU tiles were rendered separately
# and their grid textures differ by low-level noise in ~60% of pixels (measured 2026-09-29), so
# "the pixels that differ" is not "the label". The label is what is markedly BRIGHTER than the
# frame's own texture: an empty-frame template is taken per pixel as a low order statistic over
# every retail tile (6 RU + 6 EN 6-button + 7 EN 7-button; labels sit at different columns, so at
# any pixel most samples show bare frame), and ink = luminance above the template by INK_T.
BTN_X, BTN_W, BTN_H, BTN_Y0, BTN_DY = 3, 219, 30, 97, 40
INK_T = 12  # "is a label pixel" -- clean letter segmentation on every retail label at 12
HALO_T = 6  # "belongs to a label's anti-aliased edge" -- what is copied / erased with the ink
LABEL_ROWS = (6, 25)  # label ink lives in these tile rows; the frame border never does
MIN_GLYPH_W = 4  # narrower column runs are texture specks, not letters
RU_LABELS = ["ВСТУПЛЕНИЕ", "НОВАЯ ИГРА", "ОБУЧЕНИЕ", "ЗАГРУЗИТЬ ИГРУ", "ТИТРЫ", "ВЫХОД"]
NET_LABEL = "СЕТЕВАЯ ИГРА"


def gfx_decode(blob):
    import numpy as np

    w, h = struct.unpack("<HH", blob[:4])
    if len(blob) != 4 + 2 * w * h:
        raise ValueError(f"not a raw {w}x{h} gfx ({len(blob)} bytes)")
    return np.frombuffer(blob[4:], dtype="<u2").reshape(h, w).copy()


def gfx_encode(a):
    import numpy as np

    h, w = a.shape
    return struct.pack("<HH", w, h) + a.astype("<u2").tobytes()


def _lum(a):
    import numpy as np

    a = a.astype(np.int32)
    return ((a >> 11) & 31) * 2 + ((a >> 5) & 63) + (a & 31) * 2


def _template(samples, rank):
    import numpy as np

    s = np.stack(samples)
    order = np.argsort(np.stack([_lum(x) for x in samples]), axis=0, kind="stable")
    return np.take_along_axis(s, order[rank][None], 0)[0]


def _ink(tile, tmpl, t):
    import numpy as np

    m = (_lum(tile) - _lum(tmpl)) > t
    band = np.zeros_like(m)
    band[LABEL_ROWS[0]:LABEL_ROWS[1], 2:BTN_W - 2] = True
    return m & band


HOVER_CORE = 78  # a hover label's letter CORE (absolute luminance): the bold hover face carries a glow
# that fills the gaps between letters well above any relative threshold, so the hover sprites are
# segmented on the saturated core instead -- measured 2026-09-29: introh 10 / newgameh 9 / tutorh 8 /
# loadh 13 runs, exactly their letter counts.


def _glyphs(tile, tmpl, core=None):
    """Column runs of label ink wider than MIN_GLYPH_W: one per painted letter (mostly -- a letter
    whose stroke thins below INK_T can split, which is why the composition below only indexes
    letters whose position is verified). `core` = segment on absolute luminance >= core instead."""
    import numpy as np

    if core is None:
        m = _ink(tile, tmpl, INK_T)
    else:
        m = _lum(tile) >= core
        band = np.zeros_like(m)
        band[LABEL_ROWS[0]:LABEL_ROWS[1], 3:BTN_W - 3] = True
        m &= band
    cols = m.any(axis=0)
    runs, x = [], 0
    while x < len(cols):
        if cols[x]:
            s = x
            while x < len(cols) and cols[x]:
                x += 1
            if x - s >= MIN_GLYPH_W:
                runs.append((s, x))
        else:
            x += 1
    return runs


def _label_layer(tile, tmpl, x0, x1):
    """(mask, pixels) of the label ink in columns [x0, x1) -- the halo included."""
    import numpy as np

    m = _ink(tile, tmpl, HALO_T)
    m[:, :x0] = False
    m[:, x1:] = False
    return m, tile


def _erase(tile, tmpl):
    out = tile.copy()
    m = _ink(tile, tmpl, HALO_T)
    out[m] = tmpl[m]
    return out


def _paste(dst, src_tile, mask, dx):
    import numpy as np

    ys, xs = np.nonzero(mask)
    xd = xs + dx
    ok = (xd >= 0) & (xd < dst.shape[1])
    dst[ys[ok], xd[ok]] = src_tile[ys[ok], xs[ok]]


SLAB_EDGE = 10  # glow columns a hover slab keeps on a side with no neighbouring letter in its source


def _paste_slab(dst, src_tile, s, e, pl, pr, dx):
    """Max-luminance union of a column slab (the letter + its glow) into `dst`, label rows only
    (the frame border stays the template's). A slab reaches `pl`/`pr` columns past the letter's
    core -- half the gap to a neighbouring letter in the SOURCE (so no slab carries a piece of its
    neighbour), SLAB_EDGE where there is none (so the glow at a word's ends is not cut off). Where
    two slabs meet, the union adds their glows up the way the painted glow does."""
    r0, r1 = LABEL_ROWS[0] - 3, LABEL_ROWS[1] + 2
    for x in range(max(0, s - pl), min(BTN_W, e + pr)):
        xd = x + dx
        if not (3 <= xd < BTN_W - 3):
            continue
        col_s = src_tile[r0:r1, x]
        col_d = dst[r0:r1, xd]
        take = _lum(col_s) > _lum(col_d)
        col_d[take] = col_s[take]


def _compose_net(label_tiles, tmpl, report, what, core=None):
    """Lay "СЕТЕВАЯ ИГРА" out of letters painted in ВСТУПЛЕНИЕ (tile 0) and НОВАЯ ИГРА (tile 1).
    Returns [(source tile index, s, e, pad_left, pad_right, dx)] -- source columns [s, e) are the
    piece's letter cores; the caller pastes each piece (the pads matter only to the hover slabs)."""
    g = [_glyphs(label_tiles[0], tmpl, core), _glyphs(label_tiles[1], tmpl, core)]
    g0, g1 = g
    # НОВАЯ ИГРА must segment into exactly its 9 letters with the one word gap the widest; ВСТУПЛЕНИЕ
    # into at least its 10 letters (a thin stroke may split one) with its first three and its last
    # in place. Anything else refuses: a wrong guess here paints the wrong letter.
    if len(g1) != 9:
        raise SystemExit(f"{what}: НОВАЯ ИГРА segmented into {len(g1)} runs, not 9 -- refusing to guess")
    gaps1 = [g1[i + 1][0] - g1[i][1] for i in range(8)]
    if gaps1.index(max(gaps1)) != 4:
        raise SystemExit(f"{what}: НОВАЯ ИГРА's word gap is not after its 5th letter ({gaps1})")
    if len(g0) < 10:
        raise SystemExit(f"{what}: ВСТУПЛЕНИЕ segmented into {len(g0)} runs, fewer than its 10 letters")
    gaps0 = [g0[i + 1][0] - g0[i][1] for i in range(len(g0) - 1)]
    letter_gap = sorted(gaps0)[len(gaps0) // 2]
    word_gap = max(gaps1)
    last0 = len(g0) - 1
    # (source, first run, last run): С Е Т Е from ВСТУПЛЕНИЕ, ВАЯ and ИГРА as whole blocks
    pieces = [(0, 1, 1), (0, last0, last0), (0, 2, 2), (0, last0, last0), (1, 2, 4), None, (1, 5, 8)]

    def side(gap):  # a tight letter gap: stop at its middle; a word gap: keep the glow, not the neighbour
        if gap is None:
            return SLAB_EDGE
        return gap // 2 if gap <= 2 * letter_gap + 1 else min(SLAB_EDGE, gap - 1)

    def pads(src, a, b):
        runs = g[src]
        pl = side(None if a == 0 else runs[a][0] - runs[a - 1][1])
        pr = side(None if b == len(runs) - 1 else runs[b + 1][0] - runs[b][1])
        return pl, pr

    width, placed = 0, []
    for i, p in enumerate(pieces):
        if p is None:
            continue
        src, a, b = p
        s, e = g[src][a][0], g[src][b][1]
        gap = 0 if not placed else (word_gap if pieces[i - 1] is None else letter_gap)
        placed.append((src, s, e) + pads(src, a, b) + (width + gap,))
        width += gap + (e - s)
    centre = ((g0[0][0] + g0[-1][1]) / 2 + (g1[0][0] + g1[-1][1]) / 2) / 2
    left = int(round(centre - width / 2))
    report.append(f"  {what}: {NET_LABEL} = С Е Т Е from ВСТУПЛЕНИЕ runs {g0[1]} {g0[-1]} {g0[2]}, ВАЯ "
                  f"{(g1[2][0], g1[4][1])} + ИГРА {(g1[5][0], g1[8][1])} from НОВАЯ ИГРА; letter gap "
                  f"{letter_gap}px, word gap {word_gap}px, {width}px wide centred at x={centre:.1f}")
    return [(src, s, e, pl, pr, left + off - s) for src, s, e, pl, pr, off in placed]


def build_menu_art(ru_menu, en_menu):
    """(MenuBck2 bytes, NetGameH bytes, report lines) -- see the section comment above."""
    import numpy as np

    def pick(members, base):
        for k, v in members.items():
            if k.split("\\")[-1].lower() == base:
                return gfx_decode(v)
        raise SystemExit(f"menu\\{base} missing from the source mh_ex")

    R1 = pick(ru_menu, "menubck1.gfx")
    E1 = pick(en_menu, "menubck1.gfx")
    E2 = pick(en_menu, "menubck2.gfx")
    if (R1 == E2).all() or (E1 == E2).all():
        raise SystemExit("the EN pack's MENUBCK2 is not a 7-button background -- need a stock EN install")

    def tile(img, k):
        y = BTN_Y0 + BTN_DY * k
        return img[y:y + BTN_H, BTN_X:BTN_X + BTN_W]

    report = []
    samples = [tile(R1, k) for k in range(6)] + [tile(E1, k) for k in range(6)] + [tile(E2, k) for k in range(7)]
    tmpl = _template(samples, 4)
    ru_tiles = [tile(R1, k) for k in range(6)]

    # the background: R1 everywhere except the button column rows 2..6
    out = R1.copy()
    net = _erase(ru_tiles[2], tmpl)
    for src, s, e, _pl, _pr, dx in _compose_net(ru_tiles, tmpl, report, "MenuBck2"):
        m, px = _label_layer(ru_tiles[src], tmpl, s, e)
        _paste(net, px, m, dx)
    rows = {2: net}
    for k in (3, 4, 5, 6):  # ОБУЧЕНИЕ .. ВЫХОД move down one slot
        dst = _erase(ru_tiles[k], tmpl) if k < 6 else tmpl.copy()  # row 6 is a new, empty frame
        m, px = _label_layer(ru_tiles[k - 1], tmpl, 0, BTN_W)
        _paste(dst, px, m, 0)
        rows[k] = dst
    for k, t in rows.items():
        y = BTN_Y0 + BTN_DY * k
        out[y:y + BTN_H, BTN_X:BTN_X + BTN_W] = t
    report.append("  MenuBck2: RU MenuBck1 re-laid to the 7-button layout (rows 3-6 = the RU labels of "
                  "rows 2-5, row 6 a template frame); outside the button column every pixel is RU MenuBck1's")

    # the hover sprite: an empty RU hover frame + the same composition from the RU hover labels
    hov = {n: pick(ru_menu, n) for n in ("introh.gfx", "newgameh.gfx", "tutorh.gfx", "loadh.gfx",
                                         "creditsh.gfx", "exith.gfx")}
    en_hov = [pick(en_menu, n) for n in ("introh.gfx", "newgameh.gfx", "netgameh.gfx", "tutorh.gfx",
                                         "loadh.gfx", "creditsh.gfx", "exith.gfx")]
    for a in list(hov.values()) + en_hov:
        if a.shape != (BTN_H, BTN_W):
            raise SystemExit(f"hover sprite is {a.shape}, expected {(BTN_H, BTN_W)}")
    htmpl = _template(list(hov.values()) + en_hov, 0)
    hlabels = [hov["introh.gfx"], hov["newgameh.gfx"]]
    netg = htmpl.copy()
    for src, s, e, pl, pr, dx in _compose_net(hlabels, htmpl, report, "NetGameH", core=HOVER_CORE):
        _paste_slab(netg, hlabels[src], s, e, pl, pr, dx)
    return gfx_encode(out), gfx_encode(netg), report


def art_png(blob, path, scale=1):
    import numpy as np
    from PIL import Image

    a = gfx_decode(blob).astype(np.uint32)
    rgb = np.dstack([((a >> 11) & 31) * 255 // 31, ((a >> 5) & 63) * 255 // 63, (a & 31) * 255 // 31])
    im = Image.fromarray(rgb.astype(np.uint8))
    if scale > 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    im.save(path)


# ---------------------------------------------------------------------------------- the build
def _replace(members, base, blob, report, why):
    hits = [m for m in members if m.name.split("\\")[-1].lower() == base.lower()]
    if len(hits) != 1:
        raise SystemExit(f"expected exactly one {base} in the source pack, found {len(hits)}")
    m = hits[0]
    m.stored = blob  # stored uncompressed: the loader only inflates on the `LZW ` magic
    m.final_size = len(blob)
    report.append(f"  {m.name:24} {len(blob):8} bytes  {why}")


def build(ru_src, en_src, out_dir, art=True, quiet=False, lang_id="ru"):
    report = []
    members = read_pack(ru_src)
    en_members = read_pack(en_src)
    by_key = {m.key(): m for m in members}
    en_by_key = {m.key(): m for m in en_members}

    # 1. fonts
    blobs, merged, frep, ru_layer = build_fonts(ru_src, en_src)
    for base, blob in blobs.items():
        _replace(members, base, blob, report,
                 "fonts: RU base %d -> %d FONTLAY entries (%d lifted from EN, %d derived)"
                 % (len(ru_layer.layout), len(merged.layout), len(frep.lifted), len(frep.derived)))
    # 2. initlang
    ru_il = payload(by_key["init\\initlang.cfg"])
    en_il = payload(en_by_key["init\\initlang.cfg"])
    il, added = build_initlang(ru_il, en_il)
    if added:
        _replace(members, "initlang.cfg", il, report, "initlang: + %d TEXT entries in English: %s"
                 % (len(added), ", ".join(added)))
    # 3. menu art
    if art:
        ru_menu = {m.name: payload(m) for m in members if m.key().startswith("menu\\")}
        en_menu = {m.name: payload(m) for m in en_members if m.key().startswith("menu\\")}
        bck2, netg, arep = build_menu_art(ru_menu, en_menu)
        report += arep
        _replace(members, "menubck2.gfx", bck2, report, "menu: 7-button background (LANG2)")
        _replace(members, "netgameh.gfx", netg, report, "menu: NETWORK GAME hover sprite (LANG2)")
    write_pack(members, out_dir)
    msgs = read_loose(ru_src, "Msgs.dat")
    if msgs is not None:
        with open(os.path.join(out_dir, "Msgs.dat"), "wb") as f:
            f.write(msgs)
    lines = [
        f"language pack built by {TOOL} (format {FORMAT_VERSION}); retail game data -- never commit",
        f"ru source: {os.path.basename(ru_src.path)}  mh_ex.rsr md5 {md5(ru_src._read('mh_ex', '.rsr'))}",
        f"en source: {os.path.basename(en_src.path)}  mh_ex.rsr md5 {md5(en_src._read('mh_ex', '.rsr'))}",
        f"menu art: {'built (7-button)' if art else 'retail (6-button; mh.dll draws the fallback label)'}",
        f"Msgs.dat: {'copied' if msgs is not None else 'absent in the source'}",
        install_strings(lang_id, out_dir),
    ]
    for ext in (".nam", ".rsr"):
        lines.append(f"mh_ex{ext} md5 {md5(open(os.path.join(out_dir, 'mh_ex' + ext), 'rb').read())}")
    lines += [r.strip() for r in report]
    with open(os.path.join(out_dir, "BUILD.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if not quiet:
        for line in lines:
            print(line)
    return report


# ---------------------------------------------------------------------------------- mh.dll strings (LANG4)
def read_string_def(path=STRINGS_DEF):
    """{key: English} from player_strings.def's MH_STR rows (a row may wrap onto the next line)."""
    import re

    lines = open(path, encoding="utf-8").read().splitlines()
    text = "\n".join(ln for ln in lines if not ln.lstrip().startswith("//"))
    rows = re.findall(r'MH_STR\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*((?:L"(?:[^"\\]|\\.)*"\s*)+)\)', text)
    return {k: "".join(re.findall(r'L"((?:[^"\\]|\\.)*)"', en)) for k, en in rows}


def format_shape(s):
    """The conversion sequence mh.dll's str_format_compatible compares: %% skipped, %lu is one kind."""
    out, i = [], 0
    while i < len(s):
        if s[i] != "%":
            i += 1
            continue
        c = s[i + 1] if i + 1 < len(s) else ""
        if c == "%":
            i += 2
        elif s[i + 1 : i + 3] == "lu":
            out.append("lu")
            i += 3
        else:
            out.append(c or "?")
            i += 2
    return out


def strings_problems(path, rows=None):
    """Everything mh.dll's loader would refuse or fall back on in a mh_strings.txt: [] = clean."""
    rows = rows if rows is not None else read_string_def()
    probs, seen = [], set()
    raw = open(path, "rb").read()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        return [f"{path}: not UTF-8 ({exc})"]
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, eq, val = line.partition("=")
        key, val = key.strip(), val.strip()
        where = f"{os.path.basename(path)}:{n}"
        if not eq or not key:
            probs.append(f"{where}: no `key = text`")
        elif key not in rows:
            probs.append(f"{where}: unknown key {key!r}")
        elif key in seen:
            probs.append(f"{where}: {key} given twice (the first wins)")
        elif not val:
            probs.append(f"{where}: {key} is empty")
        elif format_shape(val) != format_shape(rows[key]):
            probs.append(f"{where}: {key} takes {format_shape(val)}, the English takes {format_shape(rows[key])}")
        elif len(val) >= 256:
            probs.append(f"{where}: {key} is {len(val)} characters (mh.dll keeps 255)")
        seen.add(key)
    for key in rows:
        if key not in seen:
            probs.append(f"{os.path.basename(path)}: no translation for {key} (shows English)")
    return probs


def install_strings(lang_id, out_dir):
    """Copy src/formats/mh_strings/<id>.txt to <out_dir>/mh_strings.txt. Returns a report line."""
    lang_id = lang_id.split("_")[0]  # a variant pack (`--id ru_noart`) carries its language's text
    src = os.path.join(STRINGS_DIR, f"{lang_id}.txt")
    if not os.path.isfile(src):
        return f"mh_strings.txt: none for '{lang_id}' -- mh.dll's own text stays English"
    probs = strings_problems(src)
    if probs:
        raise SystemExit("mh_strings: " + "; ".join(probs))
    data = open(src, "rb").read()
    with open(os.path.join(out_dir, "mh_strings.txt"), "wb") as f:
        f.write(data)
    return f"mh_strings.txt: {len(read_string_def())} keys from {os.path.relpath(src, _REPO)} (md5 {md5(data)})".replace(os.sep, "/")


# ---------------------------------------------------------------------------------- inspection
PROBE_CPS = (0x041F, 0x041A, 0x0141, 0xFFFD)  # lifted Cyrillic, drawn Cyrillic, drawn Polish, box


def pack_status(pack_dir):
    """What an installed pack carries, read from the pack itself (no marker file): a dict with
    `ok` (a readable pair), `cyrillic`/`merged` (the fonts), `art7` (MenuBck2 != MenuBck1),
    `strings` (a mh_strings.txt the LANG4 check finds clean) and `reason` (why not ok). The UI suite's
    lang preconditions read exactly this."""
    st = {"ok": False, "merged": False, "art7": False, "cyrillic": False, "strings": False, "reason": ""}
    sp = os.path.join(pack_dir, "mh_strings.txt")
    st["strings"] = os.path.isfile(sp) and not strings_problems(sp)
    if not (os.path.isfile(os.path.join(pack_dir, "mh_ex.nam")) and os.path.isfile(os.path.join(pack_dir, "mh_ex.rsr"))):
        st["reason"] = f"no mh_ex.nam/.rsr pair in {pack_dir}"
        return st
    try:
        src = fnt.Source(pack_dir)
        layer = src.font_layer("mh_ex")
        members = read_pack(src)
    except (OSError, ValueError, SystemExit) as exc:
        st["reason"] = f"{pack_dir}: unreadable pack ({exc})"
        return st
    st["ok"] = True
    st["cyrillic"] = bool(layer and any(fnt.is_cyrillic(cp) for cp in layer.layout))
    st["merged"] = bool(layer and all(cp in layer.index for cp in PROBE_CPS))
    b = {m.name.split("\\")[-1].lower(): m for m in members}
    if "menubck1.gfx" in b and "menubck2.gfx" in b:
        st["art7"] = b["menubck1.gfx"].stored != b["menubck2.gfx"].stored
    return st


# ---------------------------------------------------------------------------------- commands
def cmd_build(args):
    ru_src, en_src = resolve_sources(args.ru, args.en)
    game = args.game or machine.POLYGON
    out = os.path.join(game, "lang", args.id)
    print(f"RU source: {ru_src.path}\nEN source: {en_src.path}\nwriting  : {out}")
    build(ru_src, en_src, out, art=not args.no_art, lang_id=args.id)
    print(f"done -- enable with `[lang] pack={args.id}` in {os.path.join(game, 'mh_net.ini')}")
    return 0


def cmd_check(args):
    game = args.game or machine.POLYGON
    d = os.path.join(game, "lang", args.id)
    st = pack_status(d)
    print(f"{d}: " + (", ".join(f"{k}={v}" for k, v in st.items() if k != "reason") if st["ok"] else st["reason"]))
    return 0 if st["ok"] else 1


def cmd_art(args):
    ru_src, en_src = resolve_sources(args.ru, args.en)
    os.makedirs(args.out, exist_ok=True)
    ru_menu = {m.name: payload(m) for m in read_pack(ru_src) if m.key().startswith("menu\\")}
    en_menu = {m.name: payload(m) for m in read_pack(en_src) if m.key().startswith("menu\\")}
    bck2, netg, rep = build_menu_art(ru_menu, en_menu)
    for line in rep:
        print(line)
    art_png(bck2, os.path.join(args.out, "menubck2_built.png"))
    art_png(netg, os.path.join(args.out, "netgameh_built.png"), 4)
    for k, v in ru_menu.items():
        base = k.split("\\")[-1].lower()
        if base in ("menubck1.gfx", "newgameh.gfx"):
            art_png(v, os.path.join(args.out, base.replace(".gfx", "_ru.png")), 1 if "bck" in base else 4)
    print(f"wrote PNGs to {args.out}")
    return 0


def cmd_selftest(args):
    fails = []
    # mh.dll's own strings (LANG4): offline, needs no retail data.
    rows = read_string_def()
    for f in sorted(os.listdir(STRINGS_DIR)):
        probs = strings_problems(os.path.join(STRINGS_DIR, f), rows)
        fails += probs
        print(f"  {'OK ' if not probs else 'FAIL'} mh_strings/{f}: {len(rows)} keys, shapes match player_strings.def")
    tmpf = os.path.join(tempfile.mkdtemp(prefix="mh_strings_"), "bad.txt")
    with open(tmpf, "w", encoding="utf-8") as f:
        f.write("lobby.refused = %u\nlobby.refused = x %s\nno.such = y\n")
    planted = strings_problems(tmpf, rows)
    shutil.rmtree(os.path.dirname(tmpf), ignore_errors=True)
    if not (any("takes" in p for p in planted) and any("twice" in p for p in planted) and any("unknown" in p for p in planted)):
        fails.append(f"a planted bad strings file was not caught: {planted[:3]}")
    else:
        print("  OK  a planted bad strings file (wrong shape, duplicate, unknown key) is caught")
    try:
        ru_src, en_src = resolve_sources(getattr(args, "ru", None), getattr(args, "en", None))
    except SystemExit as exc:
        print(f"NO RETAIL SOURCES: {exc}")
        return 2
    tmp = tempfile.mkdtemp(prefix="mh_langpack_")
    try:
        a, b = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        build(ru_src, en_src, a, quiet=True)
        build(ru_src, en_src, b, quiet=True)
        for n in sorted(os.listdir(a)):
            if open(os.path.join(a, n), "rb").read() != open(os.path.join(b, n), "rb").read():
                fails.append(f"rebuild is not byte-identical: {n}")
        print(f"  OK  two builds from the same inputs: {len(os.listdir(a))} files, byte-identical"
              if not fails else "  FAIL rebuild differs")

        built = fnt.Source(a)
        orig = {m.key(): m for m in read_pack(ru_src)}
        now = {m.key(): m for m in read_pack(built)}
        if [m.key() for m in read_pack(built)] != [m.key() for m in read_pack(ru_src)]:
            fails.append("the built pack's member list/order differs from the RU source's")
        replaced = {"fnt\\fontlay.txt", "init\\initlang.cfg", "menu\\menubck2.gfx", "menu\\netgameh.gfx"} | {
            "fnt\\" + n.lower() for n in fnt.PFMENU}
        kept = 0
        for k, m in orig.items():
            if k in replaced:
                continue
            kept += 1
            if now[k].stored != m.stored:
                fails.append(f"{k} did not survive byte-for-byte")
        print(f"  OK  {kept} untouched members carried over as their original stored bytes")

        # fonts: additive over RU, every probe family present and non-blank
        ru_layer, layer = ru_src.font_layer("mh_ex"), built.font_layer("mh_ex")
        for name in fnt.PFMENU:
            if not fnt.verify_prefix(ru_layer.fonts[name], layer.fonts[name], len(ru_layer.layout)):
                fails.append(f"{name}: a retail RU record moved or changed")
            for cp in PROBE_CPS + tuple(c for c in en_src.font_layer("mh_ex").layout if fnt.is_latin_accent(c)):
                g = layer.glyph(name, cp)
                if g is None or g.ink_bbox() is None:
                    fails.append(f"{name}: U+{cp:04X} missing or blank")
        print(f"  OK  fonts: RU {len(ru_layer.layout)} -> {len(layer.layout)} FONTLAY entries, RU prefix "
              f"byte-identical, Cyrillic + Latin accents + Polish + box all non-blank in {len(fnt.PFMENU)} fonts")

        # initlang covers every EN entry
        il = payload(now["init\\initlang.cfg"])
        en_il = payload({m.key(): m for m in read_pack(en_src)}["init\\initlang.cfg"])
        gap = initlang_coverage(il, en_il)
        if gap:
            fails.append(f"initlang still lacks {gap}")
        print(f"  OK  initlang covers every EN TEXT entry ({len(_text_entries(en_il[2:].decode('utf-16-le')))})")

        # menu art: a real 7-button layout, the six retail labels untouched in rows 0-1, NetGameH new
        st = pack_status(a)
        if not (st["ok"] and st["art7"] and st["merged"] and st["cyrillic"]):
            fails.append(f"pack_status says {st}")
        bck1 = gfx_decode(payload(orig["menu\\menubck1.gfx"]))
        bck2 = gfx_decode(payload(now["menu\\menubck2.gfx"]))
        y_top = BTN_Y0 + BTN_DY * 2
        if not (bck1[:y_top] == bck2[:y_top]).all():
            fails.append("MenuBck2 changed a pixel above the network row")
        if not (bck1[:, BTN_X + BTN_W:] == bck2[:, BTN_X + BTN_W:]).all():
            fails.append("MenuBck2 changed a pixel right of the button column")
        netg = payload(now["menu\\netgameh.gfx"])
        if netg == payload(orig["menu\\newgameh.gfx"]):
            fails.append("NetGameH is still a copy of NewGameH")
        print("  OK  menu art: MenuBck2 != MenuBck1, rows 0-1 and everything right of the buttons are "
              "RU MenuBck1's own pixels, NetGameH is a new sprite")
        if open(os.path.join(a, "mh_strings.txt"), "rb").read() != open(os.path.join(STRINGS_DIR, "ru.txt"), "rb").read():
            fails.append("mh_strings.txt is not the committed translation")
        if "Msgs.dat" in os.listdir(a) and read_loose(ru_src, "Msgs.dat") != open(os.path.join(a, "Msgs.dat"), "rb").read():
            fails.append("Msgs.dat is not a verbatim copy")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print()
    if fails:
        for f in fails:
            print("FAIL " + f)
        return 1
    print("langpack selftest PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="build twice from the retail sources and check")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("build", "art", "selftest"):
        p = sub.add_parser(name)
        p.add_argument("--ru", help="RU install directory or .zip (default: search)")
        p.add_argument("--en", help="EN install directory or .zip (default: search)")
        if name == "build":
            p.add_argument("--game", help="game directory to write lang/<id>/ into (default: machine.POLYGON)")
            p.add_argument("--id", default="ru")
            p.add_argument("--no-art", action="store_true", help="keep the retail 6-button menu art")
        if name == "art":
            p.add_argument("--out", required=True)
    p = sub.add_parser("check")
    p.add_argument("--game")
    p.add_argument("--id", default="ru")
    args = ap.parse_args(argv)
    if args.selftest or args.cmd == "selftest":
        return cmd_selftest(args)
    if args.cmd == "build":
        return cmd_build(args)
    if args.cmd == "check":
        return cmd_check(args)
    if args.cmd == "art":
        return cmd_art(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
