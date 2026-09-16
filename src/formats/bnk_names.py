#!/usr/bin/env python3
"""Connect BNK sprite contents with INIT.CFG names.

Annotates a tree produced by ``mh_tools unpack`` (BANK_NN/<0000..>/sprite.bmp) with
the sprite names INIT.CFG assigns: its BANK "N" sections list ``SPRITE "name" frames``
entries allocated sequentially, so BNK sprite index <-> name[frame] is a direct
computation (validated: 33/34 BANK sections match the shipped BNK sprite counts
exactly; the 50-family are per-biome variants of the bank-50 tree catalog - every
PLANET loads BANK 0 49 plus exactly one of banks 50-56).

Outputs, written INTO the unpacked tree (mh_tools pack ignores extra files):
  BANK_NN/names.md            sprite dir | global id | name[frame] | used by
  BANK_NN/contact.html        contact sheet (+ BANK_NN/_thumbs/NNNN.png). Thumbnails
                              simulate the in-game look over a neutral background:
                              opaque art + fx_*.bmp effect masks (layout v2) applied
                              (DARKEN exact, TINT/blends approximated by level), and
                              V2 blend alpha via the verified additive formula.
  <root>/names_index.md/.html per-bank summary + planet->tree-bank table + name index

Usage:
  python bnk_names.py --cfg <...>/init/INIT.CFG --tree <unpacked>/BANKI
                      [--lang <...>/initlang.cfg] [--no-html]

--lang defaults to auto-discovery: <cfg dir>/INITLANG.CFG, then
<cfg dir>/../../mh_ex/init/initlang.cfg (the clean-unpack layout).

See /BNK_FORMAT.md (+ section 12 errata) and /docs/cfg-strategic.md.
"""
import argparse
import html
import os
import re
import sys
from collections import defaultdict

SECTION_HEADERS = {
    "BANK", "ANIM", "WEAPON", "UNIT", "BUILDING", "PROGRESS", "PLANET",
    "SYSTEM", "UPGRADE", "PROJECT", "STONE", "TREE",
}
# body keys whose quoted value references a sprite / anim / icon
REF_KEYS = {
    "SPRITE", "SPRITE_SHADOW", "FRAME", "FRAME_2", "ICON", "ANIM", "ANIM_P",
    "ANIM_EXPLO", "COMPONENT", "BULLET_ANIM", "SMOKE_SPRITE", "FIRE_EXPLO",
    "TARGET_EXPLO", "OBJECT",
}

TOKEN_RE = re.compile(r'"([^"]*)"|(\S+)')


def read_utf16(path):
    text = open(path, "rb").read().decode("utf-16-le")
    return text.lstrip("﻿")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = []
    for ln in text.splitlines():
        ln = re.split(r";|//|#", ln)[0]
        if re.match(r"\s*[Rr][Ee][Mm]\b", ln):
            continue
        out.append(ln)
    return out


def tokenize(line):
    """Yield (is_string, value) tokens; quoted strings keep empty values."""
    for m in TOKEN_RE.finditer(line):
        if m.group(1) is not None:
            yield True, m.group(1)
        else:
            yield False, m.group(2)


class Cfg:
    def __init__(self):
        self.def_banks = {}            # bank -> global base id
        self.banks = {}                # bank -> [(sprite name, frames)]
        self.anims = defaultdict(list) # anim name -> [sprite names]
        self.icons = {}                # icon name -> global id
        self.consumers = []            # (kind, object name, key, referenced name)
        self.planet_banks = defaultdict(list)  # planet name -> [(lo, hi)]
        self.warnings = []


def parse_cfg(path):
    cfg = Cfg()
    section = None  # (kind, name)
    for line in strip_comments(read_utf16(path)):
        toks = list(tokenize(line))
        i = 0
        while i < len(toks):
            is_str, val = toks[i]

            if section is not None:
                kind, name = section
                if not is_str and val.upper() == "END":
                    section = None
                    i += 1
                    continue
                if kind == "BANK":
                    if not is_str and val.upper() == "SPRITE" and i + 2 < len(toks) \
                            and toks[i + 1][0] and not toks[i + 2][0]:
                        cfg.banks[int(name)].append((toks[i + 1][1], int(toks[i + 2][1])))
                        i += 3
                        continue
                elif kind == "ANIM":
                    if not is_str and val.upper() == "SPRITE" and i + 1 < len(toks) and toks[i + 1][0]:
                        cfg.anims[name].append(toks[i + 1][1])
                        i += 2
                        continue
                else:
                    if not is_str and val.upper() in REF_KEYS:
                        # KEY ["ref"] or KEY n "ref" (ANIM 0 "A_x", COMPONENT 1 "S_x")
                        j = i + 1
                        if j < len(toks) and not toks[j][0] and re.fullmatch(r"\d+", toks[j][1]):
                            j += 1
                        if j < len(toks) and toks[j][0]:
                            cfg.consumers.append((kind, name, val.upper(), toks[j][1]))
                            i = j + 1
                            continue
                    if kind == "PLANET" and not is_str and val.upper() == "BANK" \
                            and i + 2 < len(toks) and not toks[i + 1][0] and not toks[i + 2][0]:
                        cfg.planet_banks[name].append((int(toks[i + 1][1]), int(toks[i + 2][1])))
                        i += 3
                        continue
                i += 1
                continue

            # top level
            if not is_str:
                word = val.upper()
                if word == "DEF_BANK" and i + 2 < len(toks):
                    cfg.def_banks[int(toks[i + 1][1])] = int(toks[i + 2][1])
                    i += 3
                    continue
                if word == "ICON" and i + 2 < len(toks) and toks[i + 1][0] \
                        and not toks[i + 2][0] and re.fullmatch(r"\d+", toks[i + 2][1]):
                    cfg.icons[toks[i + 1][1]] = int(toks[i + 2][1])
                    i += 3
                    continue
                if word in SECTION_HEADERS and i + 1 < len(toks) and toks[i + 1][0]:
                    section = (word, toks[i + 1][1])
                    if word == "BANK":
                        cfg.banks.setdefault(int(toks[i + 1][1]), [])
                    i += 2
                    continue
            i += 1
    if section is not None:
        cfg.warnings.append(f"unterminated section {section}")
    return cfg


def parse_lang(path):
    display = {}
    for line in strip_comments(read_utf16(path)):
        m = re.match(r'\s*TEXT\s+"([^"]+)"\s+"([^"]*)"', line)
        if m:
            display[m.group(1)] = m.group(2)
    return display


TREE_BANKS = range(50, 57)  # per-biome variants of the bank-50 tree catalog


def build_sprite_table(cfg):
    """bank -> [(name, frames, first_local_index)]; propagate bank 50 -> 51..56."""
    table = {}
    for bank, entries in cfg.banks.items():
        rows, off = [], 0
        for name, frames in entries:
            rows.append((name, frames, off))
            off += frames
        table[bank] = rows
    if 50 in table:
        for b in TREE_BANKS:
            table.setdefault(b, table[50])
    return table


def build_name_lookup(table, cfg):
    """sprite name -> (bank, first_local_index, frames); collisions -> warn + drop."""
    lookup, dupes = {}, set()
    for bank, rows in table.items():
        if bank in TREE_BANKS and bank != 50:
            continue  # shared bank-50 names
        for name, frames, off in rows:
            if name in lookup:
                dupes.add(name)
            else:
                lookup[name] = (bank, off, frames)
    for name in dupes:
        cfg.warnings.append(f"duplicate sprite name across banks: {name}")
        lookup.pop(name, None)
    return lookup


def bank_of_global_id(cfg, gid):
    best = None
    for bank, base in cfg.def_banks.items():
        if gid >= base and (best is None or base > cfg.def_banks[best]):
            best = bank
    return best


# Owner ranking: primary art references beat shared UI references when deciding
# which object a sprite is grouped under in the contact sheets.
KEY_RANK = {"SPRITE": 0, "SPRITE_SHADOW": 1, "ANIM": 2, "ANIM_P": 3, "COMPONENT": 3,
            "ANIM_EXPLO": 4}


def fmt_use(u, display):
    kind, obj, key, via = u
    loc = display.get(obj)
    loc = f" “{loc}”" if loc else ""
    return f"{kind} {obj}{loc} ({key})" + (f" via {via}" if via else "")


def owner_of(uses):
    """(kind, obj) an index/name is grouped under, or None. TREE collapses to one group."""
    if not uses:
        return None
    kind, obj, _key, _via = min(uses, key=lambda u: KEY_RANK.get(u[2], 9))
    if kind == "TREE":
        return ("TREE", "catalog 01–48")
    return (kind, obj)


def build_usage(cfg, lookup, display):
    """(bank, sprite name) / ("gid", id) -> [(kind, obj, key, via)]. Unresolved -> warnings."""
    usage = defaultdict(list)

    def attach_sprite(sname, kind, obj, key, via):
        if sname in lookup:
            usage[(lookup[sname][0], sname)].append((kind, obj, key, via))
            return True
        return False

    for kind, obj, key, ref in cfg.consumers:
        if attach_sprite(ref, kind, obj, key, None):
            continue
        if ref in cfg.anims:
            ok = False
            for sname in cfg.anims[ref]:
                ok = attach_sprite(sname, kind, obj, key, f"ANIM {ref}") or ok
            if ok:
                continue
        if ref in cfg.icons:
            gid = cfg.icons[ref]
            bank = bank_of_global_id(cfg, gid)
            if bank is not None:
                usage[("gid", gid)].append((kind, obj, key, f"ICON {ref}"))
                continue
        # Only sprite/anim/icon-looking names are worth a warning; other refs are
        # legitimate non-sprite objects (e.g. UPGRADE OBJECT -> unit/weapon names).
        if ref.startswith(("S_", "A_", "I_")):
            cfg.warnings.append(f"unresolved reference: {kind} {obj} {key} \"{ref}\"")
    return usage


# ---------------------------------------------------------------- outputs

def sprite_dirs(bank_dir):
    return sorted(d for d in os.listdir(bank_dir)
                  if re.fullmatch(r"\d{4}", d) and os.path.isdir(os.path.join(bank_dir, d)))


def rows_for_bank(bank, table, cfg, usage, nsprites):
    """Yield (local_index, global_id, name, frame, use_list) for each sprite dir index."""
    base = cfg.def_banks.get(bank if bank not in TREE_BANKS else 50)
    named = []
    for name, frames, off in table.get(bank, []):
        for f in range(frames):
            named.append((name, f))
    for idx in range(nsprites):
        gid = base + idx if base is not None else None
        if idx < len(named):
            name, frame = named[idx]
            uses = list(usage.get((50 if bank in TREE_BANKS else bank, name), []))
        else:
            name, frame, uses = None, None, []
        if gid is not None:
            uses += usage.get(("gid", gid), [])
        yield idx, gid, name, frame, uses


def write_names_md(bank, bank_dir, rows, header_note, display):
    lines = [f"# {os.path.basename(bank_dir)} — sprite names (from INIT.CFG)", ""]
    if header_note:
        lines += [header_note, ""]
    lines += ["| dir | global id | sprite | used by |", "| --- | --- | --- | --- |"]
    for idx, gid, name, frame, uses in rows:
        nm = f"`{name}`[{frame}]" if name else "*(no INIT.CFG name)*"
        gd = str(gid) if gid is not None else "—"
        us = "; ".join(fmt_use(u, display) for u in uses)
        lines.append(f"| {idx:04} | {gd} | {nm} | {us} |")
    with open(os.path.join(bank_dir, "names.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


CSS = """body{font-family:sans-serif;background:#222;color:#ddd;margin:16px}
.grid{display:flex;flex-wrap:wrap;gap:8px}
.cell{background:#333;border:1px solid #555;padding:6px;width:180px;font-size:11px}
.cell img{image-rendering:pixelated;background:
repeating-conic-gradient(#444 0 25%,#3a3a3a 0 50%) 0 0/16px 16px;max-width:170px}
.name{color:#8fc7ff;word-break:break-all}.uses{color:#aaa;word-break:break-word}
h2{margin:4px 0 12px}a{color:#8fc7ff}
h3.obj{margin:20px 0 6px;color:#ffd27f;font-size:15px}
h3.obj .count{color:#888;font-weight:normal;font-size:12px}"""


def object_heading(owner, display):
    if owner is None:
        return "Not referenced by any INIT.CFG object"
    kind, obj = owner
    loc = display.get(obj)
    return (f"{loc} — {kind} {obj}" if loc else f"{kind} {obj}")


def write_contact_html(bank, bank_dir, rows, header_note, thumbs, display):
    parts = [f"<!doctype html><meta charset='utf-8'><title>{os.path.basename(bank_dir)}</title>",
             f"<style>{CSS}</style>",
             f"<h2>{os.path.basename(bank_dir)}</h2>",
             "<p style='color:#888'>Thumbnails simulate background effects (shadows, tints, "
             "blends) over neutral gray — exact for DARKEN, approximate for the LUT types. "
             "Sprites are grouped by the object that uses them.</p>"]
    if header_note:
        parts.append(f"<p>{html.escape(header_note)}</p>")

    # Group consecutive sprites by owning object (keeps physical bank order).
    sections = []
    for row in rows:
        own = owner_of(row[4])
        if not sections or sections[-1][0] != own:
            sections.append((own, []))
        sections[-1][1].append(row)

    for owner, srows in sections:
        parts.append(f"<h3 class='obj'>{html.escape(object_heading(owner, display))} "
                     f"<span class='count'>({len(srows)} sprites)</span></h3>")
        parts.append("<div class='grid'>")
        for idx, gid, name, frame, uses in srows:
            nm = f"{html.escape(name)}[{frame}]" if name else "(unnamed)"
            img = f"<img src='_thumbs/{idx:04}.png' loading='lazy'>" if idx in thumbs else "<i>absent</i>"
            gd = f" · id {gid}" if gid is not None else ""
            us_txt = "; ".join(fmt_use(u, display) for u in uses)
            us = f"<div class='uses'>{html.escape(us_txt)}</div>" if us_txt else ""
            parts.append(f"<div class='cell'>{img}<div>{idx:04}{gd}</div>"
                         f"<div class='name'>{nm}</div>{us}</div>")
        parts.append("</div>")
    with open(os.path.join(bank_dir, "contact.html"), "w", encoding="utf-8") as f:
        f.write("\n".join(parts))


PREVIEW_BG = (100, 100, 100)  # neutral background the effect simulation composites over

# (mask file, handler kind) — layout-v2 effect masks written by mh_tools unpack
FX_MASKS = (("fx_darken.bmp", "darken"), ("fx_tint.bmp", "tint"),
            ("fx_blend.bmp", "blend"), ("fx_blend2.bmp", "blend"))


def rgb565_to_888(v):
    r5, g6, b5 = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r5 * 255 + 15) // 31, (g6 * 255 + 31) // 63, (b5 * 255 + 15) // 31)


def load_palette888(sdir):
    p = os.path.join(sdir, "palette.bin")
    if not os.path.exists(p):
        return None
    raw = open(p, "rb").read()
    return [rgb565_to_888(int.from_bytes(raw[i:i + 2], "little"))
            for i in range(0, len(raw) - 1, 2)]


def make_preview(sdir):
    """Simulated in-game look: sprite over a neutral background with background
    effects applied. DARKEN = bg/2 (exact); TINT/blends approximate the runtime
    LUTs by level; V2 blend alpha uses the verified additive formula."""
    from PIL import Image
    src = os.path.join(sdir, "sprite.bmp")
    if not os.path.exists(src):
        return None
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    spx = im.load()
    out = Image.new("RGB", (w, h), PREVIEW_BG)
    opx = out.load()
    masks = []
    for fname, kind in FX_MASKS:
        mp = os.path.join(sdir, fname)
        if os.path.exists(mp):
            masks.append((Image.open(mp).convert("RGB").load(), kind))
    pal = load_palette888(sdir) if any(k == "blend" for _, k in masks) else None
    bg = PREVIEW_BG
    for y in range(h):
        for x in range(w):
            r, g, b, a = spx[x, y]
            if a == 255:
                opx[x, y] = (r, g, b)
                continue
            if a != 0:  # V2 blend pixel: dst = sprite + bg*(31-p)/31, A = 8p+4
                f = 255 - a
                opx[x, y] = (min(255, r + bg[0] * f // 255),
                             min(255, g + bg[1] * f // 255),
                             min(255, b + bg[2] * f // 255))
                continue
            for mpx, kind in masks:
                mr, mg, _ = mpx[x, y]
                if mr == 0:
                    continue
                if kind == "darken":
                    opx[x, y] = (bg[0] // 2, bg[1] // 2, bg[2] // 2)
                elif kind == "tint":
                    lvl = mr // 8
                    opx[x, y] = tuple(c * (31 - lvl) // 31 for c in bg)
                else:  # blend: R = palette index, G = level*8
                    lvl = min(31, mg // 8)
                    c = pal[mr] if pal and mr < len(pal) else (mr, mr, mr)
                    opx[x, y] = tuple((c[i] * lvl + bg[i] * (31 - lvl)) // 31
                                      for i in range(3))
                break
    return out


def make_thumbs(bank_dir, dirs):
    from PIL import Image
    tdir = os.path.join(bank_dir, "_thumbs")
    os.makedirs(tdir, exist_ok=True)
    done = set()
    for d in dirs:
        im = make_preview(os.path.join(bank_dir, d))
        if im is None:
            continue
        if max(im.size) < 48:  # tiles/icons: scale up for visibility
            im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
        im.save(os.path.join(tdir, d + ".png"))
        done.add(int(d))
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cfg", required=True, help="path to init/INIT.CFG")
    ap.add_argument("--tree", required=True, help="unpacked root (folder containing info.txt)")
    ap.add_argument("--lang", help="path to initlang.cfg (default: auto-discover)")
    ap.add_argument("--no-html", action="store_true", help="skip contact sheets/thumbnails")
    args = ap.parse_args()

    cfg = parse_cfg(args.cfg)
    lang_path = args.lang
    if not lang_path:
        cfg_dir = os.path.dirname(os.path.abspath(args.cfg))
        for cand in (os.path.join(cfg_dir, "INITLANG.CFG"),
                     os.path.normpath(os.path.join(cfg_dir, "..", "..", "mh_ex", "init", "initlang.cfg"))):
            if os.path.exists(cand):
                lang_path = cand
                break
    display = parse_lang(lang_path) if lang_path else {}
    if not display:
        print("WARNING: no initlang.cfg found - localized display names omitted")

    table = build_sprite_table(cfg)
    lookup = build_name_lookup(table, cfg)
    usage = build_usage(cfg, lookup, display)

    tree = args.tree
    bank_dirs = sorted(d for d in os.listdir(tree)
                       if re.fullmatch(r"BANK_\d+", d) and os.path.isdir(os.path.join(tree, d)))
    print(f"{'bank':>4} {'cfg frames':>10} {'sprites':>8}  status")
    summary = []
    for bd in bank_dirs:
        bank = int(bd.split("_")[1])
        bank_dir = os.path.join(tree, bd)
        dirs = sprite_dirs(bank_dir)
        nspr = len(dirs)
        frames = sum(fr for _, fr, _ in table.get(bank, []))

        note = None
        if bank in TREE_BANKS and 50 in cfg.banks:
            planets = sorted(p for p, ranges in cfg.planet_banks.items()
                             if any(lo <= bank <= hi for lo, hi in ranges if lo >= 50))
            ploc = ", ".join(f"{p} “{display[p]}”" if p in display else p for p in planets)
            note = (f"Biome variant of the bank-50 tree catalog (TREE “NN” sprites; "
                    f"first {nspr} of 48). Loaded by: {ploc or 'no planet (unused?)'}.")
        elif bank not in cfg.banks:
            note = "No INIT.CFG BANK section covers this bank - indices only."

        # Allocation is sequential, so any count difference is at the tail: shipped
        # indices keep correct names either way (verified: banks 0/4 over-declare
        # 2/1 unshipped tail frames in the retail cfg).
        if bank in TREE_BANKS:
            status = f"tree catalog subset ({nspr}/48)"
        elif frames == 0:
            status = "no cfg names"
        elif frames == nspr:
            status = "OK"
        elif frames > nspr:
            status = f"OK ({frames - nspr} declared tail frames not shipped)"
        else:
            status = f"{nspr - frames} tail sprites unnamed"
        print(f"{bank:>4} {frames:>10} {nspr:>8}  {status}")

        rows = list(rows_for_bank(bank, table, cfg, usage, nspr))
        # Unique owning objects, in order of first appearance in the bank.
        objects = []
        for row in rows:
            own = owner_of(row[4])
            if own is not None and own not in objects:
                objects.append(own)
        summary.append((bd, bank, frames, nspr, status, objects))

        write_names_md(bank, bank_dir, rows, note, display)
        if not args.no_html:
            thumbs = make_thumbs(bank_dir, dirs)
            write_contact_html(bank, bank_dir, rows, note, thumbs, display)

    # root index
    def objects_label(objects):
        return ", ".join(display.get(obj, obj) for _kind, obj in objects)

    idx_md = ["# BNK ↔ INIT.CFG name index", "",
              f"Source cfg: `{args.cfg}`" + (f" + `{lang_path}`" if lang_path else ""), "",
              "| bank | cfg frames | sprites | status | objects |",
              "| --- | --- | --- | --- | --- |"]
    for bd, bank, frames, nspr, status, objects in summary:
        idx_md.append(f"| [{bd}]({bd}/names.md) | {frames} | {nspr} | {status} | "
                      f"{objects_label(objects)} |")
    idx_md += ["", "## Planet → tree bank", "",
               "| planet | banks |", "| --- | --- |"]
    for p in sorted(cfg.planet_banks):
        loc = f" “{display[p]}”" if p in display else ""
        ranges = ", ".join(f"{lo}–{hi}" if lo != hi else str(lo)
                           for lo, hi in cfg.planet_banks[p])
        idx_md.append(f"| {p}{loc} | {ranges} |")
    idx_md += ["", "## Sprite name → location", "",
               "| name | bank | dirs | frames |", "| --- | --- | --- | --- |"]
    for name in sorted(lookup):
        bank, off, frames = lookup[name]
        idx_md.append(f"| `{name}` | {bank} | {off:04}–{off + frames - 1:04} | {frames} |")
    with open(os.path.join(tree, "names_index.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(idx_md) + "\n")
    if not args.no_html:
        rows_html = "".join(
            f"<tr><td><a href='{bd}/contact.html'>{bd}</a></td><td>{frames}</td>"
            f"<td>{nspr}</td><td>{html.escape(status)}</td>"
            f"<td class='objs'>{html.escape(objects_label(objects))}</td></tr>"
            for bd, bank, frames, nspr, status, objects in summary)
        with open(os.path.join(tree, "names_index.html"), "w", encoding="utf-8") as f:
            f.write(f"<!doctype html><meta charset='utf-8'><title>BNK name index</title>"
                    f"<style>{CSS}table{{border-collapse:collapse}}td,th{{border:1px solid #555;"
                    f"padding:4px 8px;vertical-align:top}}td.objs{{max-width:640px;color:#bbb;"
                    f"font-size:12px}}</style><h2>BNK ↔ INIT.CFG name index</h2>"
                    f"<table><tr><th>bank</th><th>cfg frames</th><th>sprites</th><th>status</th>"
                    f"<th>objects</th></tr>{rows_html}</table>")

    for w in cfg.warnings:
        print("WARNING:", w)
    print(f"done: {len(bank_dirs)} banks annotated under {tree}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
