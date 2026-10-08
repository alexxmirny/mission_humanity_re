#!/usr/bin/env python3
r"""langpack_pl_art.py -- the Polish main-menu art of the `pl` language pack (RL18).

The EN mh_ex paints the seven main-menu labels into MENUBCK1/2 (the background) and into one hover
sprite per button (INTROH, NEWGAMEH, NETGAMEH, TUTORH, LOADH, CREDITSH, EXITH). A button is a 219x30
frame at x=3, rows y = 97 + 40k. This module re-letters those frames
in Polish with the same rule the RU build follows: no letter is drawn that a retail label already
paints.

  INTRO and TUTORIAL are Polish words too (Extermination's own wording): the EN tiles stay untouched.
  NOWA GRA / GRA W SIECI / WCZYTAJ GRE / AUTORZY / WYJSCIE are composed. Where a letter exists in a
  retail label (N O W A G R S I E C T U from the EN art, Z from the DE art) its pixels are cut out and
  pasted. Three letters no retail label of this typeface paints are SYNTHESISED, and only those:
    * Y, J      -- stroked from the font's own measurements (cap height, stem width, ink colour);
    * S-acute   -- S + the acute of the FR art's E-acute (a retail pixel move);
    * E-ogonek  -- E + a small hook under it.
  The report says which letter came from which label.

Letters are cut by a column dynamic programme that knows each label's text: n letters, n-1 cuts, each
cut in a low-ink column, each width near the typeface's (I narrow, M/W wide). Column-run segmentation
(the RU path's method) fails on this typeface: the letters sit 1-2 px apart and the halo bridges them.

The same cuts serve the hover sprites (a hover sprite is the same 219x30 frame with the label in the
same place, bright with a glow): its letters are cut at the normal tile's columns and glued with a
max-luminance union, which adds the glows up the way the painted glow does.

Also here: POWROTH (the back button's hover), TXTRACE / TXTWAIT ("Wybierz rase:", "Prosze czekac ...",
Extermination's, padded onto MH's canvas sizes) and the "powrot" plate on every BACK*_A/B/C background.
"""


import numpy as np

EN_ROWS = ["INTRO", "NEW GAME", "NETWORK GAME", "TUTORIAL", "LOAD GAME", "CREDITS", "QUIT"]
DE_ROWS = [
    "INTRO",
    "NEUES SPIEL",
    "NETZWERK-SPIEL",
    "TUTORIAL",
    "SPIEL LADEN",
    "MITWIRKENDE",
    "BEENDEN",
]
FR_ROWS = [
    "INTRO",
    "NOUVELLE PARTIE",
    "PARTIE EN RÉSEAU",
    "DIDACTICIEL",
    "CHARGER UNE PARTIE",
    "REMERCIEMENTS",
    "QUITTER",
]
HOVERS = (
    "introh",
    "newgameh",
    "netgameh",
    "tutorh",
    "loadh",
    "creditsh",
    "exith",
)  # row order of the 7-button art
# the Polish rows of the 7-button background, and the hover sprite each one owns
PL_ROWS7 = ["INTRO", "NOWA GRA", "GRA W SIECI", "TUTORIAL", "WCZYTAJ GRĘ", "AUTORZY", "WYJŚCIE"]
PL_ROWS6 = ["INTRO", "NOWA GRA", "TUTORIAL", "WCZYTAJ GRĘ", "AUTORZY", "WYJŚCIE"]
# which EN label supplies a letter first (the DP cuts of these are the cleanest), then the rest
PREFERRED = (1, 3, 5, 2, 4, 6, 0)
EXPECTED_W = {"I": 3, "M": 14, "W": 14, "L": 9, "Q": 11, "A": 11, "G": 11, "R": 10, "U": 10}
MIN_W = 2


def _lum(a):
    a = a.astype(np.int32)
    return ((a >> 11) & 31) * 2 + ((a >> 5) & 63) + (a & 31) * 2


def _rgb(a):
    a = a.astype(np.int32)
    return np.stack(
        [((a >> 11) & 31) * 255 / 31, ((a >> 5) & 63) * 255 / 63, (a & 31) * 255 / 31], axis=-1
    )


def _pack(rgb):
    rgb = np.clip(np.rint(rgb), 0, 255).astype(np.int32)
    return (
        ((rgb[..., 0] * 31 + 127) // 255) << 11
        | ((rgb[..., 1] * 63 + 127) // 255) << 5
        | ((rgb[..., 2] * 31 + 127) // 255)
    ).astype(np.uint16)


def _template(samples, rank):
    s = np.stack(samples)
    order = np.argsort(np.stack([_lum(x) for x in samples]), axis=0, kind="stable")
    return np.take_along_axis(s, order[rank][None], 0)[0]


# ---------------------------------------------------------------------------------- cutting letters
def split_label(L, tile, tmpl, text):
    """[(char, s, e)] -- one cut segment per letter of `text` (spaces and hyphens skipped)."""
    d = (_lum(tile) - _lum(tmpl)).astype(int)
    prof = np.clip(d[6:25] - 6, 0, None).sum(axis=0).astype(float)
    prof[:8] = 0
    prof[-8:] = 0
    xs = np.nonzero(prof > 60)[0]
    x0, x1 = int(xs[0]), int(xs[-1]) + 1
    letters = [c for c in text if c not in " -"]
    n = len(letters)
    memo = {}
    INF = 1e18

    def f(i, start):
        if i == n - 1:
            return 0.5 * (x1 - start - EXPECTED_W.get(letters[i], 10)) ** 2, ()
        key = (i, start)
        if key in memo:
            return memo[key]
        best = (INF, ())
        for c in range(start + MIN_W, x1 - MIN_W * (n - 1 - i)):
            sub = f(i + 1, c + 1)
            cost = prof[c] + 0.5 * (c - start - EXPECTED_W.get(letters[i], 10)) ** 2 + sub[0]
            if cost < best[0]:
                best = (cost, (c,) + sub[1])
        memo[key] = best
        return best

    cost, cuts = f(0, x0)
    edges = [x0 - 1] + list(cuts) + [x1]
    return [(letters[i], edges[i] + 1, edges[i + 1]) for i in range(n)], x0, x1


FLOOR_N = (
    8  # luminance a normal-tile pixel must exceed the bare-frame template by to count as label ink
)
ALPHA_FLOOR = 0.12  # coverage below this is tile noise, not a letter
INK_ROWS = (3, 28)  # rows a letter may touch: the accent above the caps and the ogonek below them
# hover sprite from the ink coverage: (core blur, core gain, glow blur, glow gain). Fitted by eye against
# the seven EN hover sprites rendered from their own normal tiles (the glow of a painted hover label
# overlaps its neighbours, so hover letters cannot be cut out one by one -- they are derived instead).
HOVER_FIT = (0.9, 1.6, 2.4, 1.2)


class Glyph:
    """One letter: its ink as RGB deltas over the bare frame, covering tile columns [seg[0], seg[1]);
    `tight` = the ink's column range relative to seg[0]."""

    def __init__(self, ch, delta, tight, seg, origin):
        self.ch, self.delta, self.tight, self.seg, self.origin = ch, delta, tight, seg, origin

    @property
    def width(self):
        return self.tight[1] - self.tight[0]


def _delta(tile, bare, floor):
    d = _rgb(tile) - _rgb(bare)
    d[(_lum(tile) - _lum(bare)) <= floor] = 0
    d[: INK_ROWS[0]] = 0
    d[INK_ROWS[1] :] = 0
    return np.clip(d, 0, None)


def _tight_cols(delta, s, e, min_px=2, floor=0):
    """Columns of [s, e) carrying at least `min_px` ink pixels (summed RGB delta above `floor`)."""
    lum = delta.sum(axis=2)
    cols = np.nonzero((lum[:, s:e] > floor).sum(axis=0) >= min_px)[0]
    if len(cols) == 0:
        return s, e
    return s + int(cols[0]), s + int(cols[-1]) + 1


INK_PX = 60  # summed-RGB delta of a pixel that is label ink, not texture


def _row_edges(delta):
    """{row: (first ink column, one past the last)} of a letter's delta window."""
    lum = delta.sum(axis=2)
    out = {}
    for r in range(lum.shape[0]):
        cols = np.nonzero(lum[r] > INK_PX)[0]
        if len(cols):
            out[r] = (int(cols[0]), int(cols[-1]) + 1)
    return out


def _pair_offset(left_edges, right_edges, target):
    """Where the right letter's window starts, relative to the left one's, so that the closest rows of
    the two letters are `target` px apart (optical kerning: W|A tucks in, I|I does not)."""
    rows = set(left_edges) & set(right_edges)
    if not rows:
        return None
    return max(left_edges[r][1] - right_edges[r][0] for r in rows) + target


def _tight_rows(delta, s, e, min_px=2, floor=0):
    lum = delta.sum(axis=2)
    rows = np.nonzero((lum[:, s:e] > floor).sum(axis=1) >= min_px)[0]
    return int(rows[0]), int(rows[-1]) + 1


# ---------------------------------------------------------------------------------- the build
def build_art(L, members, donors, ext_menu):
    """({lower-case member key: bytes}, report lines)."""
    from PIL import Image, ImageDraw, ImageFilter

    rep = []

    def lazy(pack_members):
        return {m.key(): m for m in pack_members if m.key().startswith("menu\\")}

    en_menu, de_menu, fr_menu = (
        lazy(members),
        lazy(L.read_pack(donors["de"])),
        lazy(L.read_pack(donors["fr"])),
    )

    def img(menu, base):
        return L.gfx_decode(L.payload(menu["menu\\" + base + ".gfx"]))

    def tile(a, k):
        y = L.BTN_Y0 + L.BTN_DY * k
        return a[y : y + L.BTN_H, L.BTN_X : L.BTN_X + L.BTN_W]

    en1, en2 = img(en_menu, "menubck1"), img(en_menu, "menubck2")
    de2, fr2 = img(de_menu, "menubck2"), img(fr_menu, "menubck2")
    for name, a in (("EN MenuBck2", en2), ("DE MenuBck2", de2), ("FR MenuBck2", fr2)):
        if (a == en1).all():
            raise SystemExit(f"{name} is not a 7-button background")
    samples = [tile(en1, k) for k in range(6)] + [
        tile(a, k) for a in (en2, de2, fr2) for k in range(7)
    ]
    tmpl = _template(samples, 6)
    tiles = {
        "en": [tile(en2, k) for k in range(7)],
        "de": [tile(de2, k) for k in range(7)],
        "fr": [tile(fr2, k) for k in range(7)],
    }
    hov = [img(m, h) for m in (en_menu, de_menu, fr_menu) for h in HOVERS]
    for a in hov:
        if a.shape != (L.BTN_H, L.BTN_W):
            raise SystemExit(f"hover sprite is {a.shape}")
    htmpl = _template(hov, 2)  # the bare hover frame
    t_rgb, h_rgb = _rgb(tmpl), _rgb(htmpl)

    # ---- 1. cut every source label into letters; harvest spacing
    lib = {}
    spacing = {"letter": [], "word": [], "word_tight": [], "centre": []}

    def harvest(lang, rows, rows_idx):
        for k in rows_idx:
            text = rows[k]
            segs, _x0, _x1 = split_label(L, tiles[lang][k], tmpl, text)
            nd = _delta(tiles[lang][k], tmpl, FLOOR_N)
            ti, prev, spaced, firsts, lasts = 0, None, False, None, None
            for ch in text:
                if ch in " -":
                    spaced = True
                    continue
                c, s, e = segs[ti]
                ti += 1
                ts, te = _tight_cols(nd, s, e, min_px=1, floor=60)
                edges = _row_edges(nd[:, s:e])
                if prev is not None and lang == "en":
                    off = _pair_offset(prev[0], edges, 0)  # window-to-window offset at zero row gap
                    if off is not None:
                        spacing["word" if spaced else "letter"].append((s - prev[1]) - off)
                    if spaced:
                        spacing["word_tight"].append(s + ts - s - prev[2])
                prev, spaced = (edges, s, s + te - s), False
                firsts = ts if firsts is None else firsts
                lasts = te
                if (lang, c) not in lib:
                    lib[(lang, c)] = Glyph(
                        c, nd[:, s:e].copy(), (ts - s, te - s), (s, e), (lang, k, text)
                    )
            if lang == "en":
                spacing["centre"].append((firsts + lasts) / 2)

    harvest("en", EN_ROWS, PREFERRED)
    harvest("de", DE_ROWS, (2,))
    harvest("fr", FR_ROWS, (2,))
    letter_gap = int(round(np.median(spacing["letter"])))
    word_gap = int(round(np.median(spacing["word"])))
    word_gap_tight = int(round(np.median(spacing["word_tight"])))
    centre = float(np.median(spacing["centre"]))
    rep.append(
        f"  menu art: closest-row gap {letter_gap}px within a word, {word_gap}px across a space (bounding boxes "
        f"{word_gap_tight}px), label centre "
        f"x={centre:.1f} (medians over the EN labels)"
    )

    # ---- 2. the letters no retail label paints
    def ink_colour(tile_, delta):
        px = _rgb(tile_)[delta.sum(axis=2) > 0]
        lum = px.sum(axis=1)
        return px[lum >= np.percentile(lum, 85)].mean(axis=0)

    col_n = ink_colour(tiles["en"][1], _delta(tiles["en"][1], tmpl, FLOOR_N))
    col_h = np.concatenate([_rgb(a)[_lum(a) >= L.HOVER_CORE] for a in hov[:7]]).mean(axis=0)
    full_ink = float((col_n - t_rgb).sum(axis=2).mean())  # summed-RGB delta of a fully inked pixel
    gN = lib[("en", "N")]
    cap_t, cap_b = _tight_rows(gN.delta, 0, gN.delta.shape[1], min_px=2, floor=90)
    for (lang, _c), g in list(lib.items()):  # texture specks above/below the caps are not letter
        g.delta[: max(0, cap_t - 1)] = 0
        g.delta[cap_b + 1 :] = 0
        g.tight = _tight_cols(g.delta, 0, g.delta.shape[1], min_px=1, floor=60)
    stem = float(max(2, min(3, lib[("en", "I")].width)))
    rep.append(
        f"  menu art: cap rows {cap_t}..{cap_b - 1}, stem {stem:.0f}px, ink colour {tuple(int(v) for v in col_n)}"
    )
    SS = 8

    def alpha_of(paths, lw):
        im = Image.new("L", (L.BTN_W * SS, L.BTN_H * SS), 0)
        dr = ImageDraw.Draw(im)
        for pts in paths:
            dr.line(
                [(x * SS, y * SS) for x, y in pts],
                fill=255,
                width=int(round(lw * SS)),
                joint="curve",
            )
        return np.asarray(im.resize((L.BTN_W, L.BTN_H), Image.BOX), dtype=np.float64) / 255.0

    def stroke_delta(paths):
        d = np.clip(col_n - t_rgb, 0, None) * alpha_of(paths, stem)[..., None]
        d[: INK_ROWS[0]] = 0
        d[INK_ROWS[1] :] = 0
        return d

    X0, top, bot, hh = 20.0, float(cap_t), float(cap_b), stem / 2.0

    def synth(ch, width, paths):
        s, e = int(X0) - 3, int(X0 + width) + 3
        d = stroke_delta(paths)
        ts, te = _tight_cols(d, s, e, min_px=1, floor=60)
        return Glyph(ch, d[:, s:e].copy(), (ts - s, te - s), (s, e), ("synth", 0, ch))

    wY, wJ = 11.0, 9.0
    xc, mid = X0 + wY / 2, top + (bot - top) * 0.55
    lib[("synth", "Y")] = synth(
        "Y",
        wY,
        [[(X0 + hh, top), (xc, mid)], [(X0 + wY - hh, top), (xc, mid)], [(xc, mid), (xc, bot)]],
    )
    xr = X0 + wJ - hh
    lib[("synth", "J")] = synth(
        "J",
        wJ,
        [
            [(xr, top), (xr, bot - hh)],
            [(xr, bot - hh), (X0 + hh, bot - hh)],
            [(X0 + hh, bot - hh), (X0 + hh, bot - 4.0)],
        ],
    )

    # S-acute: S + the acute of the FR art's E-acute, lifted as a delta and re-centred on the S
    fr_k = 2
    fr_segs, _a, _b = split_label(L, tiles["fr"][fr_k], tmpl, FR_ROWS[fr_k])
    fr_letters = [c for c in FR_ROWS[fr_k] if c not in " -"]
    _c, es, ee = fr_segs[fr_letters.index("É")]
    fr_nd = _delta(tiles["fr"][fr_k], tmpl, FLOOR_N)
    rows_e = np.nonzero((fr_nd[:cap_t, es:ee].sum(axis=2) > 30).any(axis=1))[0]
    if len(rows_e) == 0 or int(rows_e[0]) >= cap_t - 1:
        raise SystemExit(
            f"the FR E-acute has no accent above the cap rows ({cap_t}..{cap_b}) -- cannot lift it"
        )
    acc_top = int(rows_e[0])
    e_cols = _tight_cols(fr_nd[cap_t:], es, ee, min_px=1, floor=60)

    def full(g):
        d = np.zeros((L.BTN_H, L.BTN_W, 3))
        d[:, g.seg[0] : g.seg[1]] = g.delta
        return d

    def repack(ch, d, seg, origin):
        ts, te = _tight_cols(d, seg[0], seg[1], min_px=1, floor=60)
        return Glyph(ch, d[:, seg[0] : seg[1]].copy(), (ts - seg[0], te - seg[0]), seg, origin)

    def with_accent(base_ch, ch):
        g = lib[("en", base_ch)]
        d = full(g)
        shift = int(round(g.seg[0] + (g.tight[0] + g.tight[1]) / 2 - (e_cols[0] + e_cols[1]) / 2))
        for y in range(acc_top, cap_t):
            for x in range(es - 1, ee + 1):
                xd = x + shift
                if 0 <= xd < L.BTN_W:
                    d[y, xd] = np.maximum(d[y, xd], fr_nd[y, x])
        return repack(ch, d, (g.seg[0] - 3, g.seg[1] + 3), ("accent", base_ch, ch))

    def with_ogonek(base_ch, ch):
        g = lib[("en", base_ch)]
        d = full(g)
        xr_ = g.seg[0] + g.tight[1] - 2.2
        d = np.maximum(
            d, stroke_delta([[(xr_, bot - 0.5), (xr_, bot + 1.6), (xr_ + 1.8, bot + 2.6)]])
        )
        return repack(ch, d, (g.seg[0] - 1, g.seg[1] + 4), ("ogonek", base_ch, ch))

    lib[("synth", "Ś")] = with_accent("S", "Ś")
    lib[("synth", "Ę")] = with_ogonek("E", "Ę")
    rep.append(
        "  menu art: Y, J stroked; S-acute = S + the acute of the FR art's E-acute; E-ogonek = E + a drawn hook"
    )

    def glyph_for(ch):
        if ch in ("Y", "J", "Ś", "Ę"):
            return lib[("synth", ch)], "synth"
        if ch == "Z":
            return lib[("de", "Z")], "de"
        g = lib.get(("en", ch))
        if g is None:
            raise SystemExit(f"no source for the letter {ch!r}")
        return g, "en"

    # ---- 3. lay a label out on a frame; the hover sprite is derived from the same ink coverage
    def compose(text, base_rgb):
        ink = np.zeros((L.BTN_H, L.BTN_W, 3))
        items, spaced = [], False
        for ch in text:
            if ch == " ":
                spaced = True
                continue
            g, src = glyph_for(ch)
            items.append((g, src, spaced))
            spaced = False
        # window origins relative to the first letter's, by optical kerning
        xs = [0]
        edges = [_row_edges(g.delta) for g, _s, _sp in items]
        for i in range(1, len(items)):
            g, _s, spaced = items[i]
            pg = items[i - 1][0]
            target = word_gap if spaced else letter_gap
            off = _pair_offset(edges[i - 1], edges[i], target)
            box_off = pg.tight[1] - g.tight[0] + (word_gap_tight if spaced else target)
            if off is None:  # no common rows: fall back to the tight boxes
                off = box_off
            elif spaced:  # a word gap is never tucked tighter than the EN word gaps' bounding boxes
                off = max(off, box_off)
            xs.append(xs[-1] + off)
        first, last = items[0][0], items[-1][0]
        left_ink, right_ink = xs[0] + first.tight[0], xs[-1] + last.tight[1]
        start = int(round(centre - (left_ink + right_ink) / 2))
        for (g, _s, _sp), x in zip(items, xs):
            for i in range(g.delta.shape[1]):
                xd = start + x + i
                if 3 <= xd < L.BTN_W - 3:
                    ink[:, xd] = np.maximum(ink[:, xd], g.delta[:, i])
        return _pack(base_rgb + ink), ink, [f"{g.ch}:{src}" for g, src, _sp in items]

    def hover_of(ink):
        cov = np.clip(ink.sum(axis=2) / full_ink, 0, 1)
        cov[cov < ALPHA_FLOOR] = 0
        sc, gc, sg, gg = HOVER_FIT

        def blur(a, s):
            im = Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8))
            return np.asarray(im.filter(ImageFilter.GaussianBlur(s)), dtype=np.float64) / 255.0

        core = np.clip(blur(cov, sc) * gc, 0, 1)
        glow = np.clip(blur(cov, sg) * gg, 0, 1)
        a = np.clip(core + glow * (1 - core), 0, 1)
        return _pack(h_rgb * (1 - a[..., None]) + col_h * a[..., None])

    erased = lambda t: np.where((_lum(t) - _lum(tmpl))[..., None] > L.HALO_T, t_rgb, _rgb(t))  # noqa: E731
    bck2, bck1 = en2.copy(), en1.copy()
    en1_tiles = [tile(en1, k) for k in range(6)]
    used_log, hover_out = {}, {}
    for k, text in enumerate(PL_ROWS7):
        if text in ("INTRO", "TUTORIAL"):
            continue
        t, ink, used = compose(text, erased(tiles["en"][k]))
        y = L.BTN_Y0 + L.BTN_DY * k
        bck2[y : y + L.BTN_H, L.BTN_X : L.BTN_X + L.BTN_W] = t
        hover_out[HOVERS[k]] = hover_of(ink)
        used_log[text] = used
    for k, text in enumerate(PL_ROWS6):
        if text in ("INTRO", "TUTORIAL"):
            continue
        t, _ink, _used = compose(text, erased(en1_tiles[k]))
        y = L.BTN_Y0 + L.BTN_DY * k
        bck1[y : y + L.BTN_H, L.BTN_X : L.BTN_X + L.BTN_W] = t
    out = {"menu\\menubck2.gfx": L.gfx_encode(bck2), "menu\\menubck1.gfx": L.gfx_encode(bck1)}
    for hname, t in hover_out.items():
        out["menu\\" + hname + ".gfx"] = L.gfx_encode(t)
    for text, used in used_log.items():
        rep.append(f"  menu art: {text}: " + " ".join(used))
    rep.append(
        "  menu art: hover sprites derived from the composed label (HOVER_FIT); INTRO and TUTORIAL keep the EN art"
    )

    # ---- 4. POWROTH, TXTRACE, TXTWAIT, BACK*
    def ext(base):
        for k, v in ext_menu.items():
            if k.split("\\")[-1].lower() == base:
                return L.gfx_decode(v)
        raise SystemExit(f"Extermination has no menu\\{base}")

    en_pow = img(en_menu, "powroth")
    ex_pow = ext("powroth.gfx")
    if ex_pow.shape != en_pow.shape:
        raise SystemExit(f"POWROTH {ex_pow.shape} != EN {en_pow.shape}")
    out["menu\\powroth.gfx"] = L.gfx_encode(ex_pow)
    for base in ("txtrace", "txtwait"):
        out["menu\\" + base + ".gfx"] = L.gfx_encode(
            _pad_text(img(en_menu, base), ext(base + ".gfx"), base, rep)
        )
    out.update(
        _lift_plates(L, lambda key: L.gfx_decode(L.payload(en_menu["menu\\" + key])), ext, rep)
    )
    rep.append("  menu art: POWROTH from Extermination; 'powrot' plate on the 9 BACK backgrounds")
    return out, rep


def _ink_bbox(a):
    ys, xs = np.nonzero(a != 0)
    return int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1


def _pad_text(en_img, ex_img, name, rep):
    """Extermination's title text onto MH's canvas: the ink box centred vertically, left edge where MH's ink starts."""
    h, w = en_img.shape
    y0, y1, x0, x1 = _ink_bbox(ex_img)
    ey0, ey1, ex0, ex1 = _ink_bbox(en_img)
    crop = ex_img[y0:y1, x0:x1]
    if crop.shape[0] > h:
        raise SystemExit(
            f"{name}: Extermination's text is {crop.shape[0]} rows tall, MH's canvas {h}"
        )
    squeezed = 0
    while (
        crop.shape[1] > w
    ):  # too wide: narrow the widest blank gap (a word space) by one column, never below 3
        blank = ~(crop != 0).any(axis=0)
        runs, i = [], 0
        while i < len(blank):
            if blank[i]:
                j = i
                while j < len(blank) and blank[j]:
                    j += 1
                runs.append((j - i, i))
                i = j
            else:
                i += 1
        n, at = max(runs) if runs else (0, 0)
        if n <= 3:
            raise SystemExit(
                f"{name}: Extermination's text ({crop.shape[1]}px) does not fit MH's {w}px canvas"
            )
        crop = np.delete(crop, at, axis=1)
        squeezed += 1
    out = np.zeros_like(en_img)
    oy = (h - crop.shape[0]) // 2
    ox = min(ex0, w - crop.shape[1])
    out[oy : oy + crop.shape[0], ox : ox + crop.shape[1]] = crop
    rep.append(
        f"  menu art: {name} {crop.shape[1]}x{crop.shape[0]} text onto MH's {w}x{h} canvas at ({ox},{oy})"
        + (f" (word gaps narrowed by {squeezed}px)" if squeezed else "")
    )
    return out


def _yellow(a):
    a = a.astype(np.uint32)
    r, g, b = (a >> 11) & 31, (a >> 5) & 63, a & 31
    return (r >= 22) & (g >= 44) & (b <= 10)


PLATE = (4, 53, 9, 30)  # x0, x1, y0, y1 of the label pill, relative to the stripes' bbox corner


def _lift_plates(L, get_mh, ext, rep):
    out = {}
    for res, ex_name in ((0, "back0.gfx"), (1, "back1.gfx"), (2, "back2.gfx")):
        exb = ext(ex_name)
        y = _yellow(exb)
        y[: int(exb.shape[0] * 0.8)] = False
        ys, xs = np.nonzero(y)
        yx, yy = int(xs.min()), int(ys.min())
        for v in "abc":
            key = f"back{res:02d}_{v}.gfx"
            mh = get_mh(key)
            if mh.shape != exb.shape:
                raise SystemExit(f"{key} {mh.shape} != Extermination's {ex_name} {exb.shape}")
            my = _yellow(mh)
            my[: int(mh.shape[0] * 0.8)] = False
            mys, mxs = np.nonzero(my)
            if (int(mxs.min()), int(mys.min())) != (yx, yy):
                raise SystemExit(
                    f"{key}: the plate sits at a different place than in Extermination's {ex_name}"
                )
            x0, x1, y0, y1 = PLATE
            mh[yy + y0 : yy + y1, yx + x0 : yx + x1] = exb[yy + y0 : yy + y1, yx + x0 : yx + x1]
            out["menu\\" + key] = L.gfx_encode(mh)
    return out
