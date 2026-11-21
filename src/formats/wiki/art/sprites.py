"""BNK sprites -> RGBA PNGs (building completed-render, unit facing).

Pipeline: ``mh_tools unpack`` writes ``BANK_NN/<idx>/sprite.bmp`` (+ ``fx_*.bmp`` effect masks,
``palette.bin``, ``meta.bin``); this module resolves cfg sprite names to (bank, index) with the
same table logic as ``bnk_names`` and composites each frame to RGBA.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import types
from pathlib import Path

import bnk_names
import numpy as np
import yaml
from PIL import Image

from .common import ArtSource

FACING_COUNT = 24
DEFAULT_FACING = 22
FACING_YAML = Path(__file__).resolve().parent.parent / "facing.yaml"


class Layer:
    __slots__ = ("hx", "hy", "img")

    def __init__(self, img, hx, hy):
        self.img, self.hx, self.hy = img, hx, hy


def load_facing(path=None) -> dict:
    """{'default', 'categories': [{'facing', <cfg field>: [values]}], 'units'} from facing.yaml."""
    p = Path(path) if path else FACING_YAML
    data = {}
    if p.is_file():
        data = yaml.safe_load(p.read_text(encoding="utf-8")) or {}
    return {
        "default": int(data.get("default", DEFAULT_FACING)),
        "categories": [
            {k: v for k, v in (c or {}).items()} for c in (data.get("categories") or {}).values()
        ],
        "units": {str(k): int(v) for k, v in (data.get("units") or {}).items()},
    }


def pick_facing(facing: dict, u) -> int:
    """Facing 1..24 for a cfg UNIT: per-unit override, else first matching category, else default."""
    if u["__name__"] in facing["units"]:
        return facing["units"][u["__name__"]]
    for cat in facing.get("categories", []):
        rules = {k: v for k, v in cat.items() if k != "facing"}
        if rules and all(str(u.get(k)) in [str(x) for x in vs] for k, vs in rules.items()):
            return int(cat["facing"])
    return facing["default"]


def effect_layer_rgba(sdir: Path) -> Image.Image | None:
    """sprite.bmp as straight-alpha RGBA with the background-dependent effects baked in.

    Opaque stays opaque; V2 partial alpha A = 8p+4 is the premultiplied-over form
    (dst = S + bg*(1-A)), so un-premultiply; effect masks on fully transparent pixels become
    translucent black (DARKEN 50%, TINT by level) or the palette colour at the blend level.
    """
    bmp = sdir / "sprite.bmp"
    if not bmp.is_file():
        return None
    arr = np.array(Image.open(bmp).convert("RGBA"), dtype=np.int32)
    a = arr[..., 3]
    part = (a > 0) & (a < 255)
    if part.any():
        safe = np.maximum(a, 1)
        for c in range(3):
            arr[..., c] = np.where(part, np.minimum(255, arr[..., c] * 255 // safe), arr[..., c])
    free = a == 0
    pal = bnk_names.load_palette888(str(sdir))
    for fname, kind in bnk_names.FX_MASKS:
        mp = sdir / fname
        if not mp.is_file():
            continue
        m = np.array(Image.open(mp).convert("RGB"), dtype=np.int32)
        if m.shape[:2] != a.shape:
            continue
        hit = free & (m[..., 0] > 0)
        if not hit.any():
            continue
        if kind == "darken":
            rgb = np.zeros_like(arr[..., :3])
            alpha = np.full(a.shape, 128)
        elif kind == "tint":
            rgb = np.zeros_like(arr[..., :3])
            alpha = (m[..., 0] // 8) * 255 // 31
        else:  # blend: R = palette index, G = level * 8
            idx = np.minimum(m[..., 0], 255)
            table = np.array(pal if pal else [(i, i, i) for i in range(256)], dtype=np.int32)
            rgb = table[np.minimum(idx, len(table) - 1)]
            alpha = np.minimum(31, m[..., 1] // 8) * 255 // 31
        for c in range(3):
            arr[..., c] = np.where(hit, rgb[..., c], arr[..., c])
        arr[..., 3] = np.where(hit, alpha, arr[..., 3])
        free = free & ~hit
    return Image.fromarray(arr.astype(np.uint8), "RGBA")


def compose(layers: list[Layer], sign: int = -1) -> Image.Image | None:
    """Alpha-composite layers by hotspot; trim to content.

    Unit parts (hull, turret, shadow): the anchor lies at canvas pixel (hx, hy), so the layer
    top-left is -(hx, hy) (sign=-1; verified by eye on tank/rocket-launcher turrets, where the
    old +(hx, hy) put turrets off the hull by twice the hotspot difference). Building parts
    (sign=+1) mostly share hotspot (-1, -1); the few with a real one (human helipad platform)
    sit correctly only with +(hx, hy), also verified by eye.
    """
    layers = [layer for layer in layers if layer is not None]
    if not layers:
        return None
    pos = [(sign * layer.hx, sign * layer.hy) for layer in layers]
    x0 = min(p[0] for p in pos)
    y0 = min(p[1] for p in pos)
    x1 = max(p[0] + layer.img.width for p, layer in zip(pos, layers, strict=True))
    y1 = max(p[1] + layer.img.height for p, layer in zip(pos, layers, strict=True))
    canvas = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    for p, layer in zip(pos, layers, strict=True):
        canvas.alpha_composite(layer.img, (p[0] - x0, p[1] - y0))
    bbox = canvas.getchannel("A").getbbox()
    return canvas.crop(bbox) if bbox else None


class SpriteStore:
    """Resolves cfg sprite names and lazily unpacks the banks they live in."""

    def __init__(self, src: ArtSource):
        self.src = src
        banks = {}
        for b in src.model.banks:
            try:
                n = int(str(b["name"]).strip())
            except ValueError:
                continue
            banks[n] = [(name, frames) for name, frames in b["sprites"]]
        shim = types.SimpleNamespace(banks=banks, warnings=[])
        self.table = bnk_names.build_sprite_table(shim)
        self.lookup = bnk_names.build_name_lookup(self.table, shim)
        self.warnings = shim.warnings
        self.anims = {a["__name__"]: a for a in src.objects("ANIM")}
        self._layers: dict[tuple[int, int], Layer | None] = {}
        self._dirs: dict[int, Path] = {}
        self._seq = 0

    # ------------------------------------------------------------------ unpacking
    def _bank_dir(self, bank):
        if bank in self._dirs:
            return self._dirs[bank]
        for p in sorted((self.src.cache / "unpacked").glob(f"*/BANKI/BANK_{bank:02d}")):
            self._dirs[bank] = p
            return p
        return None

    def ensure_banks(self, banks):
        """Unpack every missing bank in as few mh_tools calls as possible."""
        need = sorted(b for b in set(banks) if self._bank_dir(b) is None)
        if not need:
            return
        bnk_dir = self.src.cache / "bnk"
        bnk_dir.mkdir(parents=True, exist_ok=True)
        dat = self.src.member_to_file("BANKI.DAT")
        if dat is None:
            raise FileNotFoundError("BANKI.DAT not in the packs")
        shutil.copyfile(dat, bnk_dir / "BANKI.DAT")
        for b in need:
            name = f"BANK_{b:02d}.BNK"
            p = self.src.member_to_file(name)
            if p is None:
                self.src.note(f"{name} missing from packs")
                continue
            shutil.copyfile(p, bnk_dir / name)
        base = self.src.cache / "unpacked"
        n = 0
        while (base / f"b{n}").exists():
            n += 1
        out = base / f"b{n}"
        cmd = [self.src.mh_tools, "unpack", "-d", str(bnk_dir / "BANKI.DAT"), "-b", str(bnk_dir)]
        cmd += ["-o", str(out), "--bank", *map(str, need)]
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if r.returncode != 0:
            raise RuntimeError(f"mh_tools unpack failed: {r.stderr.strip() or r.stdout.strip()}")
        for b in need:
            self._dirs.pop(b, None)

    # ------------------------------------------------------------------ frames
    def layer(self, bank, idx) -> Layer | None:
        key = (bank, idx)
        if key in self._layers:
            return self._layers[key]
        res = None
        bdir = self._bank_dir(bank)
        sdir = bdir / f"{idx:04d}" if bdir else None
        if sdir and (sdir / "sprite.bmp").is_file():
            img = effect_layer_rgba(sdir)
            meta = (sdir / "meta.bin").read_bytes()
            hx, hy = struct.unpack_from("<hh", meta, 4)
            res = Layer(img, hx, hy)
        self._layers[key] = res
        return res

    def frame_ref(self, sprite_name, k=0):
        """(bank, local index) of frame ``k`` of a named sprite, or None."""
        hit = self.lookup.get(sprite_name)
        if hit is None:
            return None
        bank, off, _frames = hit
        return bank, off + k

    def banks_for(self, names):
        return {self.lookup[n][0] for n in names if n in self.lookup}

    def named_layer(self, sprite_name, k=0):
        ref = self.frame_ref(sprite_name, k)
        return self.layer(*ref) if ref else None

    def anim_layer(self, anim_name, k=0):
        a = self.anims.get(anim_name)
        return self.named_layer(a["sprite"], k) if a else None

    # ------------------------------------------------------------------ renders
    @staticmethod
    def building_components(b) -> list[str]:
        """Sprite names of a finished idle building, in draw order.

        cfg ``COMPONENT n`` (``COMPONENT_QUANT`` of them) lists exactly the sprites the game
        stacks for the standing building: body, door/overlay, light, radar... in index order
        (slot 0 of the anims is the construction scaffold, the explosion anims in slots 2/7 are
        effects and are NOT part of it). Every component is drawn at frame 0 (closed doors,
        lights in their first phase). The sprites of one building share a canvas origin
        (meta hotspot is the same for all parts), so layers are stacked at their hotspot.
        """
        comps = b.get("components", {}) or {}
        return [comps[i] for i in sorted(comps)]

    def building_sprite_names(self, b):
        return [n for n in self.building_components(b) if n in self.lookup]

    def render_building(self, b) -> Image.Image | None:
        """Completed building: every COMPONENT sprite at frame 0, stacked in cfg order."""
        layers = []
        for name in self.building_components(b):
            hit = self.lookup.get(name)
            if hit is None or hit[2] < 1:
                continue
            layers.append(self.named_layer(name, 0))
        return compose(layers, sign=1)

    def unit_frames(self, u):
        """(frame count, body first sprite name, shadow name) for a unit."""
        hit = self.lookup.get(u.get("sprite", ""))
        return (hit[2] if hit else 0), u.get("sprite", ""), u.get("sprite_shadow", "")

    @staticmethod
    def facing_indices(frames, facing) -> list[int]:
        """Local frame indices (drawn in order) for facing 1..24 given a sprite's frame count.

        Layouts seen in retail (compass sense of the index is the game's own; picks are reviewed on
        tmp/wiki_art/review/units_*.png):
        24 = one frame per facing (jets, helicopters, most vehicles);
        48 = 24 hull frames then 24 turret frames (both drawn, hull first);
        64 = walkers (Centaur family): 24 rest frames, one per facing (index = facing - 1), then
        8 directions x 5 stride (leg-position) frames at 24 + 5*dir + n. The old 8x8 reading was
        wrong. Source: the game's own walker frame table (mh/seams/walker_frames.h), whose rest
        entries are facing - 1; so a rest frame exists for every facing, SE included;
        192 = soldiers: three 64-frame sets of 8 dirs x 8 walk frames. The sets are the PLANET MAP
        SKINS (camouflage per map, chosen from the current planet's cfg at draw time), not unit
        data -- the wiki always shows set 0;
        anything else (1, 10 ...) = frame 0.
        """
        k = (facing - 1) % FACING_COUNT
        if frames == FACING_COUNT:
            return [k]
        if frames == 2 * FACING_COUNT:
            return [k, FACING_COUNT + k]
        if frames == 64:
            return [k]
        if frames == 192:
            return [8 * (((k - 12 + 1) // 3) % 8)]
        return [0]

    def render_unit(self, u, facing) -> Image.Image | None:
        """One facing (1..24) of a unit: shadow layers under body layers."""
        body = u.get("sprite", "")
        if body not in self.lookup:
            return None
        layers = []
        for name in (u.get("sprite_shadow", ""), body):
            if name in self.lookup:
                for i in self.facing_indices(self.lookup[name][2], facing):
                    layers.append(self.named_layer(name, i))
        return compose(layers)


def object_keys(records) -> list[str]:
    """Asset key per record: the cfg name, or ``name#k`` (k = 0-based occurrence) when the name repeats."""
    names = [r["__name__"] for r in records]
    seen: dict[str, int] = {}
    keys = []
    for n in names:
        k = seen.get(n, 0)
        seen[n] = k + 1
        keys.append(f"{n}#{k}" if names.count(n) > 1 else n)
    return keys


def _save(img, out_dir, rel):
    dest = Path(out_dir) / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    img.save(dest)


def extract_building_sprites(
    src: ArtSource, out_dir, store: SpriteStore | None = None, subdir="sprites"
) -> dict[str, str]:
    """{building name: 'sprites/<name>.png'} completed-building renders."""
    store = store or SpriteStore(src)
    blds = src.objects("BUILDING")
    names = []
    for b in blds:
        names += store.building_sprite_names(b)
    store.ensure_banks(store.banks_for(names))
    out, miss = {}, []
    for key, b in zip(object_keys(blds), blds, strict=True):
        img = store.render_building(b)
        if img is None:
            miss.append(key)
            continue
        rel = f"{subdir}/{key.lower().replace('#', '_')}.png"
        _save(img, out_dir, rel)
        out[key] = rel
    src.stats["buildings_total"] = len(blds)
    src.stats["buildings_ok"] = len(out)
    src.stats["buildings_missing"] = miss
    return out


def extract_unit_sprites(
    src: ArtSource, out_dir, store: SpriteStore | None = None, facing=None, subdir="sprites"
) -> dict[str, str]:
    """{unit name: 'sprites/<name>.png'}: the chosen facing (facing.yaml override or default)."""
    store = store or SpriteStore(src)
    facing = facing or load_facing()
    units = src.objects("UNIT")
    names = []
    for u in units:
        names += [u.get("sprite", ""), u.get("sprite_shadow", "")]
    store.ensure_banks(store.banks_for(names))
    out, miss = {}, []
    for key, u in zip(object_keys(units), units, strict=True):
        f = pick_facing(facing, u)
        img = store.render_unit(u, f)
        if img is None:
            miss.append(key)
            continue
        rel = f"{subdir}/{key.lower().replace('#', '_')}.png"
        _save(img, out_dir, rel)
        out[key] = rel
    src.stats["units_total"] = len(units)
    src.stats["units_ok"] = len(out)
    src.stats["units_missing"] = miss
    return out
