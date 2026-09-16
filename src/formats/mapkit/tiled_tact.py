"""Tactical ALIEN_*.MAP <-> Tiled JSON (.tmj) + PODLOGA.TLO tileset PNG + sidecar.

Editable Tiled layers:
  - "ground"    tile layer   <- terrain-id & 0x1FFF, painted from PODLOGA.TLO (32x24 tiles).
  - "collision" tile layer   <- impassability bitmap (0/1), red overlay = impassable.
  - "walls"     object layer  <- the height-slot plane rendered as real sprites: each non-zero
                slot is a BANK_120 sprite (walls/consoles/vents), tile-object bottom-anchored by
                its `level` (rle.bin[1]) so stacks build multi-tile-tall walls. slot 0 also = LOS.

The 8 int16 height slots are the wall/object stack: on-disk value = BANK_120 sprite id + 100
(0 = empty). When rendered as objects the plane is rebuilt from them (byte-exact) and is NOT in
the sidecar; if BANK_120 art is unavailable the plane rides verbatim in the sidecar instead and no
wall layer is emitted. Terrain/impassability planes are row-major (idx=row*128+col) - no transpose."""
from __future__ import annotations

import json
import struct

from . import tactical
from .common import b64d, b64e
from .tiled_strat import passable_png

FORMAT = "ALIEN_v1"
GROUND_FIRSTGID = 1
N = tactical.GRID * tactical.GRID
TW, TH = 32, 24


def _prop(props, name, default=None):
    for p in props or []:
        if p.get("name") == name:
            return p.get("value")
    return default


def export(m: tactical.TacticalMapModel, tlo_model, png_name: str, wall_sprites=None) -> tuple[dict, dict]:
    from . import tlo as tlomod
    wall_sprites = wall_sprites or {}
    W = H = tactical.GRID
    pass_firstgid = GROUND_FIRSTGID + tlo_model.count
    walls_firstgid = pass_firstgid + 2

    ground = [GROUND_FIRSTGID + (m.terrain[i] & tactical.GROUND_MASK) for i in range(N)]
    collision = [pass_firstgid + (1 if m.impassable[i] else 0) for i in range(N)]

    sc = {"format": FORMAT,
          "terrain_hi": b64e(struct.pack("<%dH" % N, *[t & ~tactical.GROUND_MASK for t in m.terrain]))}

    img = tlomod.to_png(tlo_model, columns=32)
    iw, ih = img.size
    tilesets = [
        {"firstgid": GROUND_FIRSTGID, "name": "ground", "image": png_name,
         "imagewidth": iw, "imageheight": ih, "tilewidth": TW, "tileheight": TH,
         "columns": 32, "tilecount": tlo_model.count, "margin": 0, "spacing": 0},
        {"firstgid": pass_firstgid, "name": "collision", "image": "collision.png",
         "imagewidth": 2 * TW, "imageheight": TH, "tilewidth": TW, "tileheight": TH,
         "columns": 2, "tilecount": 2, "margin": 0, "spacing": 0},
    ]
    layers = [
        {"type": "tilelayer", "id": 1, "name": "ground", "width": W, "height": H,
         "x": 0, "y": 0, "opacity": 1, "visible": True, "data": ground},
        {"type": "tilelayer", "id": 2, "name": "collision", "width": W, "height": H,
         "x": 0, "y": 0, "opacity": 0.6, "visible": False, "data": collision},
    ]
    images = {png_name: img, "collision.png": passable_png(TW, TH)}
    nextlayerid = 3

    # buf column-major raw height plane, to read the on-disk slot values
    height = m.height_raw
    used = set()
    for off in range(0, tactical.H_LEN, 2):
        v = struct.unpack_from("<h", height, off)[0]
        if v:
            used.add(v - 100)
    have_all = wall_sprites and used.issubset(wall_sprites)

    if have_all:
        objs = []
        oid = 1
        for col in range(W):
            for row in range(H):
                base = col * 0x800 + row * 0x10
                for s in range(8):
                    v = struct.unpack_from("<h", height, base + s * 2)[0]
                    if not v:
                        continue
                    rid = v - 100
                    _pn, _im, w, h, level = wall_sprites[rid]
                    objs.append({"id": oid, "name": "", "gid": walls_firstgid + rid,
                                 "x": col * TW, "y": (row - level) * TH, "width": w, "height": h,
                                 "rotation": 0, "visible": True,
                                 "properties": [{"name": "slot", "type": "int", "value": s}]})
                    oid += 1
        tiles = []
        for rid, (pn, im, w, h, level) in sorted(wall_sprites.items()):
            if rid not in used:
                continue
            tiles.append({"id": rid, "image": pn, "imagewidth": w, "imageheight": h,
                          "properties": [{"name": "level", "type": "int", "value": level}]})
            images[pn] = im
        tilesets.append({
            "firstgid": walls_firstgid, "name": "walls", "tilewidth": 0, "tileheight": 0,
            "tilecount": (max(used) + 1) if used else 1, "columns": 0, "objectalignment": "topleft",
            "grid": {"orientation": "orthogonal", "width": 1, "height": 1}, "tiles": tiles})
        layers.append({"type": "objectgroup", "id": 3, "name": "walls", "x": 0, "y": 0,
                       "opacity": 1, "visible": True, "draworder": "index", "objects": objs})
        nextlayerid = 4
        sc["walls_as_objects"] = True
    else:
        sc["height_raw"] = b64e(m.height_raw)

    tmj = {
        "type": "map", "version": "1.10", "tiledversion": "1.10.2",
        "orientation": "orthogonal", "renderorder": "right-down",
        "width": W, "height": H, "tilewidth": TW, "tileheight": TH,
        "infinite": False, "nextlayerid": nextlayerid, "nextobjectid": N,
        "tilesets": tilesets, "layers": layers,
        "properties": [
            {"name": "mapkit_format", "type": "string", "value": FORMAT},
            {"name": "mapkit_sidecar", "type": "string",
             "value": b64e(json.dumps(sc, separators=(",", ":")).encode("utf-8"))},
        ],
    }
    return tmj, images


def _tileset(tmj, name):
    for ts in tmj.get("tilesets", []):
        if ts.get("name") == name:
            return ts
    return None


def import_(tmj: dict) -> tactical.TacticalMapModel:
    blob = _prop(tmj.get("properties"), "mapkit_sidecar")
    if blob is None:
        raise ValueError("ABORT: .tmj missing 'mapkit_sidecar' (regenerate with map_decompile).")
    sc = json.loads(b64d(blob).decode("utf-8"))
    if sc.get("format") != FORMAT:
        raise ValueError(f"ABORT: sidecar format {sc.get('format')!r} != {FORMAT!r}")

    m = tactical.TacticalMapModel()
    terrain_hi = struct.unpack("<%dH" % N, b64d(sc["terrain_hi"]))
    layers = {l.get("name"): l for l in tmj.get("layers", [])}
    for name in ("ground", "collision"):
        if name not in layers or "data" not in layers[name] or len(layers[name]["data"]) != N:
            raise ValueError(f"ABORT: '{name}' tile layer missing or wrong size (no resize).")

    gf = _tileset(tmj, "ground")["firstgid"]
    pf = _tileset(tmj, "collision")["firstgid"]
    gd, cd = layers["ground"]["data"], layers["collision"]["data"]
    m.terrain = [(((gd[i] & 0x1FFFFFFF) - gf) & tactical.GROUND_MASK) | terrain_hi[i] for i in range(N)]
    m.impassable = [1 if ((cd[i] & 0x1FFFFFFF) - pf) else 0 for i in range(N)]

    if "height_raw" in sc:
        m.height_raw = b64d(sc["height_raw"])
    else:
        wts = _tileset(tmj, "walls")
        if wts is None:
            raise ValueError("ABORT: no 'walls' tileset but sidecar has no height_raw.")
        wf = wts["firstgid"]
        level = {t["id"]: _prop(t.get("properties"), "level", 0) for t in wts.get("tiles", [])}
        height = bytearray(tactical.H_LEN)
        for o in (layers.get("walls") or {}).get("objects", []):
            gid = o.get("gid")
            if not gid:
                continue
            rid = (gid & 0x1FFFFFFF) - wf
            col = int(round(o["x"])) // TW
            row = int(round(o["y"])) // TH + level.get(rid, 0)
            s = _prop(o.get("properties"), "slot", 0)
            struct.pack_into("<h", height, col * 0x800 + row * 0x10 + s * 2, rid + 100)
        m.height_raw = bytes(height)
    return m
