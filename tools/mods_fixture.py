#!/usr/bin/env python3
"""DATAEXT1 lane fixture: build one mod's <lane>/mods/<id>/ tree for the content-digest UI rows.

WHERE THE PAYLOAD COMES FROM. The lane's own install packs (<src>/<pack>.rsr + .nam), read with
src/formats/unpack.py's .nam entry parser and decompress.py's LZW decoder -- the same library every
other tool uses. Nothing is committed to the repo (no retail art) and nothing is linked: a shared
mutable source dir would make both peers hash the same bytes, which is exactly the disagreement
these scenarios exist to stage. Each lane builds its own copy at provisioning time.

THE MEMBER, AND WHY NOT JUST ANYTHING. An ORDINARY packed text resource under info\\ -- the class
nothing on the menu->lobby path reads before the JOIN: the info screens are operator-opened help/
credits, so a mutated payload cannot confound the admission test with a pre-join render change.
Fonts (fnt\\*.FNT and friends) are EXPLICITLY out: the menu unpacks them before the first screen,
so flipping one would change or crash rendering before admission -- measured pick on the rig's
install had exactly that hazard (the naive picker chose fnt\\PFMENU0.FNT). Preference order:
  1. info\\*.txt, BOM-marked UTF-16LE or ASCII, 512..65536 B. The installed CREDITS.TXT and
     TUTORIAL.TXT are UTF-16LE; the lowest usable name wins (CREDITS.TXT on this install).
  2. nothing usable -> exit loudly. An empty mod dir would stage a scenario where both peers
     quietly hash the stock snapshot, which proves nothing.

MODES
  match    byte-identical payload (the matching-set scenario; the text renders exactly as stock)
  variant  change the final ASCII letter codepoint to the next letter, preserving the encoding,
           BOM, code-unit width, line endings, any terminal NUL padding, name and file size.
A lane provisioned without --mods-fixture has no mods dir at all: its digest is the stock
snapshot's (the missing-active-mod scenario; there is no "no claim" value to fake).

Every provisioning prints what it picked and the sha256 prefix of both sides' payload, so a run's
log names the bytes that were hashed.
"""

import argparse
from collections import Counter
import hashlib
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "src", "formats"))
import decompress as _lzw  # noqa: E402  Decompressor().decompress: LZW 'LZW ' payloads, plain passthrough
import unpack as _rsr  # noqa: E402  the .nam reader (64-byte entries)
from mapkit import strategic as _strategic  # noqa: E402

ENTRY_BYTES = 64
MIN_PAYLOAD = 512
MAX_PAYLOAD = 65536
ID_RE = re.compile(r"^[a-z0-9_-]{1,32}$")

MOD_INI = (
    "[mod]\n"
    "format=1\n"
    "id=%s\n"
    "name=UI fixture %s\n"
    "version=test\n"
    "description=DATAEXT1 lane fixture payload (generated at provisioning, not committed)\n"
)


def _pack_entries(src, pack):
    """Every .nam entry of <src>/<pack>, in stored order."""
    out = []
    with open(os.path.join(src, pack + ".nam"), "rb") as nf:
        rec = nf.read(ENTRY_BYTES)
        while len(rec) == ENTRY_BYTES:
            out.append(_rsr.Entry(rec))
            rec = nf.read(ENTRY_BYTES)
    return out


def _text_payload(data):
    """Decode only known text encodings; allow terminal NULs, never embedded controls."""
    if data.startswith(b"\xff\xfe"):
        encoding, prefix = "utf-16-le", 2
    else:
        encoding, prefix = "ascii", 0
    try:
        text = data[prefix:].decode(encoding, errors="strict")
    except UnicodeError as exc:
        raise ValueError("invalid info text encoding") from exc
    body = text.rstrip("\0")
    if not body or any(not (c.isprintable() or c in "\t\r\n") for c in body):
        raise ValueError("info text has embedded controls")
    if not any("A" <= c <= "Z" or "a" <= c <= "z" for c in body):
        raise ValueError("info text contains no ASCII letter to vary")
    return encoding, prefix, text, body


def pick_payload(src, pack="mh_ex"):
    """(member name, payload bytes) of the deterministic fixture member, or None.

    Ordinary-class packed TEXT only: info\\*.txt first (the menu->lobby path never reads them --
    see the docstring for why fonts and art are excluded)."""

    def band(e):
        return MIN_PAYLOAD <= e.final_size <= MAX_PAYLOAD

    def texty(e):
        n = e.filename.lower()
        return n.endswith(".txt")

    def info_txt(e):
        return texty(e) and e.filename.lower().replace("/", "\\").startswith("info\\")

    with open(os.path.join(src, pack + ".rsr"), "rb") as rf:
        entries = _pack_entries(src, pack)
        for want in (info_txt,):
            for e in sorted(
                (e for e in entries if band(e) and want(e)), key=lambda e: e.filename.lower()
            ):
                rf.seek(e.offset)
                payload = _lzw.Decompressor().decompress(rf.read(e.size))
                if len(payload) != e.final_size:
                    continue
                try:
                    _text_payload(payload)
                except ValueError:
                    continue
                return e.filename, payload
    return None


def mutate_last_printable(payload):
    """Change a letter without altering markup digits, encoding, BOM or terminal padding."""
    encoding, prefix, text, body = _text_payload(payload)
    for i in range(len(body) - 1, -1, -1):
        ch = body[i]
        if "A" <= ch <= "Z" or "a" <= ch <= "z":
            first = "A" if ch.isupper() else "a"
            replacement = chr(ord(first) + (ord(ch) - ord(first) + 1) % 26)
            # Re-encoding the complete decoded text retains UTF-16 code units (including surrogate
            # pairs) and terminal NULs; the chosen ASCII letter occupies one byte/code unit.
            changed = payload[:prefix] + (text[:i] + replacement + text[i + 1 :]).encode(encoding)
            if len(changed) != len(payload):
                raise ValueError("text mutation changed payload size")
            return changed
    raise ValueError("no ASCII letter to vary")


def _rel_parts(member):
    """The payload's game-relative path under data/ (the pack's own name, separators normalised)."""
    return [p for p in member.replace("\\", "/").split("/") if p]


def _write_tree(dst_root, mod_id, member, payload):
    mod_dir = os.path.join(dst_root, "mods", mod_id)
    data_path = os.path.join(mod_dir, "data", *_rel_parts(member))
    os.makedirs(os.path.dirname(data_path), exist_ok=True)
    with open(data_path, "wb") as f:
        f.write(payload)
    with open(os.path.join(mod_dir, "mod.ini"), "w", newline="\r\n") as f:
        f.write(MOD_INI % (mod_id, mod_id))
    return data_path


CFG_MODES = (
    "cfg-a",
    "cfg-b",
    "cfg-invalid-add",
    "cfg-invalid-disable",
    "cfg-invalid-field",
    "cfg-invalid-ref",
    "cfg-pair",
    "cfg-gameplay-fast",
)


def cfg_map_bytes(src):
    """Make a normal, flat .mpm from the fixed stock Cold War codec template.

    The live placement check requires nonzero passability and no class_owner.
    Remove all on-disk object occupancy and reuse an observed legal clear ground
    tile, the template's flat all-zero half-resolution plane and its real starts.
    Visibility and any live entities are still decided by the game at runtime.
    """
    template = os.path.join(src, "Maps", "Cold War.mpm")
    m = _strategic.parse(template)
    n = m.width * m.height
    if (m.width, m.height) != (128, 128) or m.f1 != 0 or m.mpm_id is None:
        raise ValueError("cfg map template has unexpected dimensions or record format")
    if len(m.halfres_raw) != (m.width // 2) * (m.height // 2) * 4 or any(m.halfres_raw):
        raise ValueError("cfg map template does not have the known flat half-resolution plane")
    if not 2 <= m.f3 <= 8 or len(m.tail_raw) != m.f3 * 12:
        raise ValueError("cfg map template has invalid player starts")
    starts = list(struct.iter_unpack("<iii", m.tail_raw))
    if any(not (0 <= x < m.width and 0 <= y < m.height) for x, y, _ in starts):
        raise ValueError("cfg map template has an out-of-range player start")
    if len({(x, y) for x, y, _ in starts}) != len(starts):
        raise ValueError("cfg map template has duplicate player starts")
    tiles = Counter(
        flag
        for flag, passable, occupant in zip(m.flags, m.passable, m.building)
        if passable == 1 and occupant == 0
    )
    if not tiles:
        raise ValueError("cfg map template has no observed passable empty ground tile")
    ground = min(tiles, key=lambda flag: (-tiles[flag], flag))
    m.flags = [ground] * n
    m.building = [0] * n
    m.passable = [1] * n
    m.objects = []
    _strategic.finalize_mpm(m, b"DATAEXT2 configuration fixture\r\n")
    return _strategic.emit(m)


TILESET_MODES = (
    "tileset-preview33",
    "tileset-latch",
    "tileset-generated",
    "tileset-generated-client",
    "tileset-kam",
    "tileset-generated-art",
    "tileset-generated-metadata",
    "tileset-generated-rules",
)


def build_tileset_kam(dst_root, src, *, child_env=None):
    """Generate one ordinary map from unmodified stock Kam art; mount no mod."""
    import subprocess
    import tempfile

    expected = "2e4b68f3d5cd7861080a50ca6c524161a1cffc6400f3b96279447f83acd37860"
    art = unpack_named(src, "Kam256.tlo")
    actual = hashlib.sha256(art).hexdigest()
    if actual != expected:
        raise ValueError(
            "tileset-kam: unsupported Kam256.tlo SHA-256 %s (expected %s)" % (actual, expected)
        )
    maps = os.path.join(dst_root, "Maps")
    if not os.path.isdir(maps) or os.listdir(maps):
        raise ValueError("tileset-kam requires --mods-empty-maps in an isolated lane")
    semantic = os.path.join(
        REPO, "src", "formats", "mapkit", "tileset_examples", "kam256.semantic.mgr"
    )
    mapgen = os.path.join(REPO, "src", "mh_dll", "mh_mapgen_cli", "Release", "mapgen.exe")
    if not os.path.isfile(semantic) or not os.path.isfile(mapgen):
        raise ValueError("tileset-kam: semantic template or Release mapgen.exe missing")
    with tempfile.TemporaryDirectory(prefix="mh_kam_fixture_") as temp:
        tlo = os.path.join(temp, "KAM256.tlo")
        rules = os.path.join(temp, "KAM256.mgr")
        generated = os.path.join(temp, "kamproof.mpm")
        with open(tlo, "wb") as f:
            f.write(art)
        # Scores cover every internal edge of the complete five-by-two rock.
        subprocess.run(
            [
                mapgen,
                "infer",
                "--tlo",
                tlo,
                "--semantic",
                semantic,
                "--h-limit",
                "3431",
                "--v-limit",
                "3813",
                "--out",
                rules,
            ],
            check=True,
            env=child_env,
        )
        subprocess.run(
            [
                mapgen,
                "gen",
                "--rules",
                temp,
                "--tileset",
                "kam256.tlo",
                "--size",
                "64",
                "--players",
                "2",
                "--seed",
                "41001",
                "--out",
                generated,
            ],
            check=True,
            env=child_env,
        )
        with open(generated, "rb") as f:
            contents = f.read()
    path = os.path.join(maps, "kamproof.mpm")
    with open(path, "wb") as f:
        f.write(contents)
    print("  KAM FIXTURE: ordinary map %s sha256=%s" % (path, hashlib.sha256(contents).hexdigest()))
    return path


def build_tileset_generated(dst_root, mod_id, src, include_map, variant="", *, child_env=None):
    """Reproduce the calibrated MARSH_01 PNG -> inference -> map fixture per lane.

    The client mode omits only the ordinary loose map. Its mod payload is
    identical to the host's, so a missing ordinary map tests download rather
    than the mod-content admission check.
    """
    import subprocess
    import tempfile

    from PIL import Image
    from mapkit import tlo as tlo_codec
    from mapkit import tlo_cli

    stock_sha = "2e4b68f3d5cd7861080a50ca6c524161a1cffc6400f3b96279447f83acd37860"
    recolor_sha = "b961b06b2c262be2ee126ee11647bb44c198d74a95296f30e732233386e9aafc"
    stock = unpack_named(src, "Kam256.tlo")
    actual_sha = hashlib.sha256(stock).hexdigest()
    if actual_sha != stock_sha:
        raise ValueError(
            "tileset-generated: unsupported Kam256.tlo SHA-256 %s (expected %s)"
            % (actual_sha, stock_sha)
        )
    maps = os.path.join(dst_root, "Maps")
    if not os.path.isdir(maps) or os.listdir(maps):
        raise ValueError("tileset-generated requires --mods-empty-maps in an isolated lane")
    semantic = os.path.join(
        REPO, "src", "formats", "mapkit", "tileset_examples", "marsh_01.semantic.mgr"
    )
    mapgen = os.path.join(REPO, "src", "mh_dll", "mh_mapgen_cli", "Release", "mapgen.exe")
    if not os.path.isfile(semantic) or not os.path.isfile(mapgen):
        raise ValueError("tileset-generated: semantic template or Release mapgen.exe missing")
    with tempfile.TemporaryDirectory(prefix="mh_tileset_fixture_") as temp:
        source = os.path.join(temp, "KAM256.tlo")
        atlas = os.path.join(temp, "kam.png")
        recolored_png = os.path.join(temp, "marsh.png")
        recolored_tlo = os.path.join(temp, "MARSH_01.tlo")
        rules = os.path.join(temp, "MARSH_01.mgr")
        generated = os.path.join(temp, "marsh_01.mpm")
        with open(source, "wb") as f:
            f.write(stock)
        model = tlo_codec.parse(source, "biome")
        if model.count != 1200:
            raise ValueError("tileset-generated: calibrated Kam tile count differs")
        if tlo_cli.main(["export", source, atlas, "--columns", "32"]):
            raise ValueError("tileset-generated: indexed PNG export failed")
        with Image.open(atlas) as image:
            image.load()
            if image.mode != "P":
                raise ValueError("tileset-generated: exported atlas is not indexed")
            palette = image.getpalette("RGB")
            if palette is None or len(palette) != 768:
                raise ValueError("tileset-generated: exported palette is not 256 RGB entries")
            rotated = []
            for i in range(0, len(palette), 3):
                rotated.extend((palette[i + 2], palette[i], palette[i + 1]))
            image.putpalette(rotated)
            image.save(recolored_png, format="PNG")
        if tlo_cli.main(["import", recolored_png, recolored_tlo, "--count", str(model.count)]):
            raise ValueError("tileset-generated: PNG import failed")
        with open(recolored_tlo, "rb") as f:
            art = f.read()
        if hashlib.sha256(art).hexdigest() != recolor_sha:
            raise ValueError("tileset-generated: recolored atlas differs from calibrated SHA-256")
        # 100%-of-authored-case scores measured on the exact guarded TLO:
        # Complete five-by-two rock: horizontal 3431, vertical 3817.
        subprocess.run(
            [
                mapgen,
                "infer",
                "--tlo",
                recolored_tlo,
                "--semantic",
                semantic,
                "--h-limit",
                "3431",
                "--v-limit",
                "3817",
                "--out",
                rules,
            ],
            check=True,
            env=child_env,
        )
        with open(rules, "rb") as f:
            rules_bytes = f.read()
        map_bytes = None
        if include_map:
            subprocess.run(
                [
                    mapgen,
                    "gen",
                    "--rules",
                    temp,
                    "--tileset",
                    "marsh_01.tlo",
                    "--size",
                    "64",
                    "--players",
                    "2",
                    "--seed",
                    "41001",
                    "--name",
                    "Scarlet Marsh",
                    "--out",
                    generated,
                ],
                check=True,
                env=child_env,
            )
            with open(generated, "rb") as f:
                map_bytes = f.read()
    base = os.path.join(dst_root, "mods", mod_id)
    if os.path.exists(base):
        raise ValueError("tileset-generated: fixture mod directory already exists")
    os.makedirs(os.path.join(base, "data", "tilesets"))
    with open(os.path.join(base, "mod.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write(MOD_INI % (mod_id, mod_id))
    descriptor = (
        "[tileset]\nformat=1\nname=MARSH_01.tlo\nlabel=Scarlet Marsh\n"
        "trees=jungle\nsoldiers=2\nrules=tilesets/marsh_01.mgr\n"
    ).encode("ascii")
    # Each negative keeps the fixture structurally valid and changes exactly
    # one existing payload; neither mod.ini nor any other bytes change.
    if variant == "art":
        art = bytes([art[0] ^ 1]) + art[1:]  # RGB565 palette low bit, same geometry
    elif variant == "metadata":
        before = b"label=Scarlet Marsh\n"
        if descriptor.count(before) != 1:
            raise ValueError("tileset-generated: unexpected descriptor label")
        descriptor = descriptor.replace(before, b"label=Scarlet Mire\n", 1)
    elif variant == "rules":
        before = b"\nmaps 0\n"
        if rules_bytes.count(before) != 1:
            raise ValueError("tileset-generated: unexpected inferred rules map count")
        rules_bytes = rules_bytes.replace(before, b"\nmaps 1\n", 1)
    elif variant:
        raise ValueError("tileset-generated: unknown fixture variant " + variant)
    for relative, data in (
        ("MARSH_01.tlo", art),
        ("tilesets/marsh_01.ini", descriptor),
        ("tilesets/marsh_01.mgr", rules_bytes),
    ):
        path = os.path.join(base, "data", *relative.split("/"))
        with open(path, "wb") as f:
            f.write(data)
        print("  TILESET FIXTURE: %s sha256=%s" % (relative, hashlib.sha256(data).hexdigest()))
    if map_bytes is not None:
        path = os.path.join(maps, "marsh_01.mpm")
        with open(path, "wb") as f:
            f.write(map_bytes)
        print(
            "  TILESET FIXTURE: ordinary map %s sha256=%s"
            % (path, hashlib.sha256(map_bytes).hexdigest())
        )
    return base


def build_tileset_preview(dst_root, mod_id, src):
    """33 separate synthetic terrain identities plus playable ordinary map files.

    No retail art is copied: one deterministic RGB565 palette and one indexed
    tile per terrain. The real stock map supplies starts and ordinary framing.
    The minimal rules only satisfy descriptor validation, not generation or
    inferred-adjacency claims.
    """
    template = os.path.join(src, "Maps", "Cold War.mpm")
    model = _strategic.parse(template)
    if (model.width, model.height) != (128, 128) or model.f1 != 0 or model.mpm_id is None:
        raise ValueError("tileset fixture: unexpected stock map template")
    if not 2 <= model.f3 <= 8 or len(model.tail_raw) != 12 * model.f3:
        raise ValueError("tileset fixture: invalid player starts")
    starts = list(struct.iter_unpack("<iii", model.tail_raw))
    if any(not (0 <= x < model.width and 0 <= y < model.height) for x, y, _ in starts):
        raise ValueError("tileset fixture: start outside map")
    if len({(x, y) for x, y, _ in starts}) != len(starts):
        raise ValueError("tileset fixture: duplicate starts")
    n = model.width * model.height
    model.flags = [0] * n
    model.building = [0] * n
    model.passable = [1] * n
    model.objects = []
    model.header_raw = bytearray(model.header_raw)
    model.header_raw[0x0D + 12] = 0
    base = os.path.join(dst_root, "mods", mod_id)
    os.makedirs(base, exist_ok=True)
    with open(os.path.join(base, "mod.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write(MOD_INI % (mod_id, mod_id))

    def put(rel, data):
        path = os.path.join(base, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    for i in range(33):
        stem = "m%07d" % i
        token = stem + ".tlo"
        # RGB565 entry 1 is distinct in every payload; the tile contains index 1.
        palette = bytearray(512)
        struct.pack_into("<H", palette, 2, (i + 1) * 127)
        put("data/" + token, bytes(palette) + bytes([1]) * 1024)
        label = "Scarlet Marsh" if i == 0 else "Fixture %02d" % i
        desc = (
            "[tileset]\nformat=1\nname=%s\nlabel=%s\ntrees=jungle\n"
            "soldiers=%d\nrules=tilesets/%s.mgr\n" % (token, label, (2 if i == 0 else i % 3), stem)
        )
        rules = (
            "mapgen-rules 3\ntileset %s\nmaps 1\ninputs_sha256 %s\n"
            "profile diuna256 jungle\nt 0 1 1 0 1 2\nh 0 0 1\nv 0 0 1\n"
            "class 0 -1 0 1000000 1\ncase 0 0 0 0 x0 0 0 0 0 1\n" % (token, "0" * 64)
        )
        put("data/tilesets/" + stem + ".ini", desc.encode("ascii"))
        put("data/tilesets/" + stem + ".mgr", rules.encode("ascii"))
        model.tlo_name = token.encode("ascii")  # raw 12 bytes; header byte 13 was cleared above
        _strategic.finalize_mpm(model, ("TILESET1 preview fixture %02d\r\n" % i).encode("ascii"))
        put("data/maps/ts%02d.mpm" % i, _strategic.emit(model))
    return base


def build_tileset_latch(dst_root, mod_id, src):
    """Same filename/TLO/players in two real folders, with distinct terrain and descriptions."""
    maps = os.path.join(dst_root, "Maps")
    if not os.path.isdir(maps) or os.listdir(maps):
        raise ValueError("tileset-latch requires --mods-empty-maps in an isolated lane")
    base = build_tileset_preview(dst_root, mod_id, src)
    # The second terrain tile is fixture-generated, not retail art; the first
    # remains the one used by the preview fixture's one-tile rules.
    terrain = os.path.join(base, "data", "m0000000.tlo")
    with open(terrain, "rb") as f:
        art = bytearray(f.read())
    if len(art) != 512 + 1024:
        raise ValueError("tileset-latch unexpected fixture TLO")
    struct.pack_into("<H", art, 4, 0xF800)
    art.extend(bytes([2]) * 1024)
    with open(terrain, "wb") as f:
        f.write(art)
    model = _strategic.parse(os.path.join(base, "data", "maps", "ts00.mpm"))
    starts = {(x, y) for x, y, _ in struct.iter_unpack("<iii", model.tail_raw)}
    cell = next(
        (
            i
            for i in range(model.width * model.height)
            if (i % model.width, i // model.width) not in starts
        ),
        None,
    )
    if cell is None:
        raise ValueError("tileset-latch map has no non-start tile")
    for folder, text, tile in (
        ("Alpha", b"ALPHA MAP DESCRIPTION\r\n", 0),
        ("Beta", b"BETA MAP DESCRIPTION\r\n", 1),
    ):
        model.flags[cell] = tile
        _strategic.finalize_mpm(model, text)
        outdir = os.path.join(maps, folder)
        os.makedirs(outdir)
        with open(os.path.join(outdir, "same.mpm"), "wb") as f:
            f.write(_strategic.emit(model))
    return base


def build_cfg(dst_root, mod_id, mode, src, *, fast_gameplay=False):
    """Write fixed, locale-independent patch fixtures; invalid modes add a later mod.

    Enable `mod_id` for A/B. For an invalid mode enable `mod_id,mod_id_bad` in that
    order. Translation bytes are identical in every mode and on every language base.
    """
    if not ID_RE.fullmatch(mod_id or ""):
        raise ValueError("invalid mod id: " + repr(mod_id))
    if mode not in CFG_MODES or mode in ("cfg-pair", "cfg-gameplay-fast"):
        raise ValueError("invalid single config fixture mode: " + repr(mode))
    later = mod_id + "_bad"
    if mode.startswith("cfg-invalid-") and not ID_RE.fullmatch(later):
        raise ValueError("mod id too long for invalid-later fixture")
    variant_a = mode != "cfg-b"
    costs = (137, 61, 43, 0) if variant_a else (211, 67, 47, 0)
    planets = (
        ("N_planeta_102", "N_planeta_101") if variant_a else ("N_planeta_101", "N_planeta_102")
    )
    patch = (
        '@patch UNIT "N_human_Soldier1" INDEX 26\n'
        + "".join('RESOURCE %d "zloze_%s"\n' % (value, key) for key, value in zip("abcd", costs))
        + 'END\n'
        + '@patch PROGRESS "V_human_Elektrownia"\n'
        + 'DEPEND "V_human_Matka"\nDEPEND "V_human_Akademia"\nEND\n'
        + '@patch SYSTEM "N_uklad1"\n'
        + 'PLANET 1 "%s"\nPLANET 2 "%s"\nEND\n' % planets
    )
    if fast_gameplay:
        # Only the isolated gameplay lanes opt in. Retain native completion, costs,
        # full HP, POWER, animation retiming, receipt and movement; never speed the
        # A-save/B-load forensic control or the Soldier's displayed 11-second stat.
        patch += (
            '@patch BUILDING "N_human_Akademia"\nBUILD_TIME 10.0\nPRODUCTION_TIME 0 4.0\nEND\n'
            '@patch BUILDING "N_human_Koszary_Zolnierzy"\nBUILD_TIME 10.0\nEND\n'
            '@patch BUILDING "N_human_Elektrownia"\nBUILD_TIME 10.0\nEND\n'
        )
    base = os.path.join(dst_root, "mods", mod_id)
    os.makedirs(os.path.join(base, "cfg"), exist_ok=True)
    os.makedirs(os.path.join(base, "lang"), exist_ok=True)
    with open(os.path.join(base, "mod.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write(MOD_INI % (mod_id, mod_id))
    files = {
        "cfg/patch.cfg": patch.encode("utf-8"),
        "lang/en.cfg": (
            'TEXT "N_human_Soldier1" "Fixture Soldier"\nTEXT "N_human_Akademia" "Fixture Academy"\n'
        ).encode("utf-8"),
        "data/info/cfgproof.txt": b"DATAEXT2 fixture resource\n",
        "data/maps/cfgfixture.mpm": cfg_map_bytes(src),
        "lang/ru.cfg": 'TEXT "N_human_Soldier1" "Солдат фикстуры"\n'.encode("utf-8"),
    }
    for rel, data in files.items():
        os.makedirs(os.path.dirname(os.path.join(base, *rel.split("/"))), exist_ok=True)
        with open(os.path.join(base, *rel.split("/")), "wb") as f:
            f.write(data)
        print("  CFG FIXTURE: %s/%s sha256=%s" % (mod_id, rel, hashlib.sha256(data).hexdigest()))
    map_path = os.path.join(base, "data", "maps", "cfgfixture.mpm")
    if _strategic.emit(_strategic.parse(map_path)) != files["data/maps/cfgfixture.mpm"]:
        raise ValueError("cfg map codec roundtrip differs from emitted fixture")
    if mode.startswith("cfg-invalid-"):
        bad = {
            "cfg-invalid-add": '@add UNIT "FixtureAdded"\nEND\n',
            "cfg-invalid-disable": '@disable UNIT "N_human_Soldier1"\nEND\n',
            "cfg-invalid-field": '@patch UNIT "N_human_Soldier1" INDEX 26\nNO_SUCH_FIELD 1\nEND\n',
            "cfg-invalid-ref": '@patch UNIT "N_human_Soldier1" INDEX 26\nINVENTION "FixtureNoSuchProgress"\nEND\n',
        }[mode]
        bad_root = os.path.join(dst_root, "mods", later)
        os.makedirs(os.path.join(bad_root, "cfg"), exist_ok=True)
        with open(os.path.join(bad_root, "mod.ini"), "w", encoding="utf-8", newline="\n") as f:
            f.write(MOD_INI % (later, later))
        with open(os.path.join(bad_root, "cfg", "patch.cfg"), "wb") as f:
            f.write(bad.encode("utf-8"))
    return os.path.join(base, "cfg", "patch.cfg")


def build(dst_root, mod_id, mode, src, pack="mh_ex", *, child_env=None):
    """Build <dst_root>/mods/<mod_id>/ in `mode`. Returns the written payload path."""
    if not ID_RE.match(mod_id or ""):
        sys.exit("mods_fixture: bad mod id %r (ASCII [a-z0-9_-], 1..32)" % (mod_id,))
    if mode in ("cfg-pair", "cfg-gameplay-fast"):
        # One lane survives every solo step: A then B, plus four independently
        # invalid later mods. Only explicitly enabled ids are mounted.
        reserved = ("other", "add", "disable", "field", "ref", "cfgcampaign")
        if mod_id in reserved or mod_id.endswith("_bad"):
            raise ValueError("cfg-pair fixture id collides with a reserved companion")
        result = build_cfg(
            dst_root, mod_id, "cfg-a", src, fast_gameplay=mode == "cfg-gameplay-fast"
        )
        if mode == "cfg-pair":
            build_cfg(dst_root, "other", "cfg-b", src)
            for name in ("add", "disable", "field", "ref"):
                build_cfg(dst_root, name, "cfg-invalid-" + name, src)
        # The campaign A SYSTEM starts N_planeta_102. Verify its real base MAP
        # before supplying that exact pack-resource name in a separate mod.
        init = unpack_named(src, r"init\init.cfg").decode("utf-16")
        planets = re.findall(r'\bPLANET\s+"N_planeta_102"\s+(.*?)\bEND\b', init, re.I | re.S)
        maps = re.findall(r'\bMAP\s+"([^"]+)"', planets[0], re.I) if len(planets) == 1 else []
        if len(maps) != 1 or maps[0].lower() != "107_ksie.mp":
            raise ValueError("base N_planeta_102 MAP is not the expected 107_ksie.mp")
        mpm = os.path.join(dst_root, "mods", mod_id, "data", "maps", "cfgfixture.mpm")
        model = _strategic.parse(mpm)
        model.mpm_id = None  # campaign .mp has no .mpm CRC/description trailer
        campaign = _strategic.emit(model)
        minimum = (
            0x61
            + 7 * model.width * model.height
            + 28 * (model.f1 + len(model.objects))
            + 12 * model.f3
        )
        if len(campaign) != minimum:
            raise ValueError("cfg campaign map does not match the native plane/footer formula")
        path = _write_tree(dst_root, "cfgcampaign", "107_ksie.mp", campaign)
        check = _strategic.parse(path)
        if (
            check.mpm_id is not None
            or (check.width, check.height) != (128, 128)
            or check.f3 < 2
            or len(check.tail_raw) != 12 * check.f3
            or _strategic.emit(check) != campaign
        ):
            raise ValueError("cfg campaign map did not roundtrip as a valid plain .mp")
        print(
            "  CFG CAMPAIGN FIXTURE: %s bytes=%d sha256=%s"
            % (path, len(campaign), hashlib.sha256(campaign).hexdigest())
        )
        return result
    if mode == "tileset-kam":
        return build_tileset_kam(dst_root, src, child_env=child_env)
    variants = {
        "tileset-generated-art": "art",
        "tileset-generated-metadata": "metadata",
        "tileset-generated-rules": "rules",
    }
    if mode in ("tileset-generated", "tileset-generated-client") or mode in variants:
        return build_tileset_generated(
            dst_root,
            mod_id,
            src,
            mode == "tileset-generated",
            variants.get(mode, ""),
            child_env=child_env,
        )
    if mode == "tileset-latch":
        return build_tileset_latch(dst_root, mod_id, src)
    if mode in TILESET_MODES:
        return build_tileset_preview(dst_root, mod_id, src)
    if mode in CFG_MODES:
        return build_cfg(dst_root, mod_id, mode, src)
    if mode not in ("match", "variant", "full-match", "full-variant"):
        sys.exit("mods_fixture: invalid fixture mode %r" % (mode,))
    picked = pick_payload(src, pack)
    if picked is None:
        sys.exit(
            "mods_fixture: no usable ASCII or BOM-marked UTF-16LE info text member "
            "(%d..%d B) in %s/%s" % (MIN_PAYLOAD, MAX_PAYLOAD, src, pack)
        )
    member, payload = picked
    variant = mutate_last_printable(payload) if mode in ("variant", "full-variant") else payload
    data_path = _write_tree(dst_root, mod_id, member, variant)
    print(
        "  MOD FIXTURE: mods/%s mode=%s member=%s bytes=%d sha256=%s%s"
        % (
            mod_id,
            mode,
            member,
            len(variant),
            hashlib.sha256(variant).hexdigest()[:16],
            " (stock %s)" % hashlib.sha256(payload).hexdigest()[:16]
            if mode in ("variant", "full-variant")
            else "",
        )
    )
    if mode.startswith("full-"):
        build_game_assets(dst_root, mod_id, src)
    return data_path


def unpack_named(src, name):
    # The game opens mh_ex first, then mh. Decode with the established format reader.
    for pack in ("mh_ex", "mh"):
        for entry in _pack_entries(src, pack):
            if entry.filename.lower() == name.lower():
                with open(os.path.join(src, pack + ".rsr"), "rb") as rf:
                    rf.seek(entry.offset)
                    data = _lzw.Decompressor().decompress(rf.read(entry.size))
                if len(data) != entry.final_size:
                    raise ValueError("bad decoded size for " + name)
                return data
    raise ValueError("resource missing from installed packs: " + name)


def build_game_assets(dst_root, mod_id, src):
    # Cold War is a stock playable two-player map. A new leaf exercises picker injection without
    # changing gameplay bytes; its own header names the TLO, so the ground fixture is not guessed.
    source = os.path.join(src, "Maps", "Cold War.mpm")
    model = _strategic.parse(source)
    tileset = model.tlo_name.split(b"\0", 1)[0].decode("ascii")
    if not tileset or os.path.basename(tileset) != tileset:
        raise ValueError("invalid TLO name in stock map")
    with open(source, "rb") as fh:
        map_bytes = fh.read()
    assets = {
        "maps\\fixture.mpm": map_bytes,
        "banki.dat": unpack_named(src, "banki.dat"),
        "bank_00.bnk": unpack_named(src, "bank_00.bnk"),
        tileset: unpack_named(src, tileset),
    }
    for name, data in assets.items():
        _write_tree(dst_root, mod_id, name, data)
        print(
            "  MOD FIXTURE: %s bytes=%d sha256=%s"
            % (name, len(data), hashlib.sha256(data).hexdigest())
        )


def _selftest():
    fails = 0

    def expect(cond, what):
        nonlocal fails
        print(("ok   " if cond else "FAIL ") + what)
        fails += 0 if cond else 1

    expect(ID_RE.match("fixture") is not None, "the default id is a valid mod id")
    expect(
        not ID_RE.match("Bad ID") and not ID_RE.match("") and not ID_RE.match("x" * 33),
        "bad ids are rejected",
    )
    for encoding, bom in (("ascii", b""), ("utf-16-le", b"\xff\xfe")):
        payload = bom + "\\6text Z\\7\r\n\0\0".encode(encoding)
        variant = mutate_last_printable(payload)
        expect(
            variant == bom + "\\6text A\\7\r\n\0\0".encode(encoding),
            encoding + ": one letter changed; BOM, markup, newline and terminal NULs preserved",
        )
        expect(len(payload) == len(variant), encoding + ": size remains unchanged")
    payload = b"\xff\xfe" + "emoji \U0001f642 Z\0".encode("utf-16-le")
    expect(
        mutate_last_printable(payload) == b"\xff\xfe" + "emoji \U0001f642 A\0".encode("utf-16-le"),
        "UTF-16LE: supplementary character survives before the changed letter",
    )
    for bad in (b"\xff\xfeA", b"\xff\xfeA\0\0\0B\0", b"text\x01letter"):
        try:
            _text_payload(bad)
        except ValueError:
            rejected = True
        else:
            rejected = False
        expect(rejected, "malformed encoding or embedded control is rejected")
    print("mods_fixture --selftest: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dst", default=None, help="the lane folder to build mods/<id>/ into")
    ap.add_argument("--src", default=None, help="the install the packs are unpacked from")
    ap.add_argument(
        "--mode",
        default=None,
        choices=("match", "variant", "full-match", "full-variant") + CFG_MODES + TILESET_MODES,
    )
    ap.add_argument("--mods-id", default="fixture")
    ap.add_argument("--pack", default="mh_ex")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return _selftest()
    for missing, what in ((a.dst, "--dst"), (a.src, "--src"), (a.mode, "--mode")):
        if missing is None:
            ap.error(what + " is required (or --selftest)")
    build(a.dst, a.mods_id, a.mode, a.src, a.pack)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
