"""Strategic .MP <-> Tiled JSON (.tmj) + embedded .TLO tileset PNG + sidecar.

Editable Tiled layers:
  - "ground"        tile layer   <- main-plane flags & 0x1FFF, painted from the .TLO atlas.
  - "passable"      tile layer   <- passable plane (0/1), red overlay = impassable.
  - "trees"         object layer <- f2 decoration records (kind/slot/next_on_tile).
  - "resources"     object layer <- resource plane (8x u16 ore amounts per 4x4 block).
  - "player_starts" object layer <- landing spots (x,y,status).

Non-edited planes (flags high bits, building word, half-res plane, header) ride verbatim
in a base64 "mapkit_sidecar" map property, so recompile is byte-identical.

Ordering: the .MP grid planes are COLUMN-major on disk (loader col-outer/row-inner); Tiled
layers are row-major, so tile layers transpose (plane f = col*H + row <-> Tiled t = row*W + col).
Object layers carry explicit tile x,y and need no transpose. Resource blocks are also
column-major: block b = bx*(H/4) + by.
"""
from __future__ import annotations

import json
import struct

from . import strategic
from .common import b64d, b64e

FORMAT = "MP_v2"
GROUND_FIRSTGID = 1
GROUND_COLUMNS = 32
RES_CHANNELS = 8
BLOCK_PX = 128  # a resource block covers 4x4 tiles = 128x128 px


# ---- synthetic tilesets ------------------------------------------------

def passable_png(tw=32, th=32):
    """2-tile RGBA overlay: tile 0 = passable (transparent), tile 1 = impassable (red)."""
    from PIL import Image
    img = Image.new("RGBA", (2 * tw, th), (0, 0, 0, 0))
    img.paste(Image.new("RGBA", (tw, th), (220, 40, 40, 120)), (tw, 0))
    return img


# ---- sidecar -----------------------------------------------------------

def _sidecar_pack(m: strategic.MpModel) -> str:
    n = m.width * m.height
    sc = {
        "format": FORMAT, "width": m.width, "height": m.height, "f1": m.f1,
        "header_raw": b64e(m.header_raw), "tlo_name_raw": b64e(m.tlo_name),
        "flags_hi": b64e(struct.pack("<%dH" % n, *[f & ~strategic.GROUND_MASK for f in m.flags])),
        "building": b64e(struct.pack("<%dH" % n, *m.building)),
        "f1_raw": b64e(m.f1_raw), "halfres_raw": b64e(m.halfres_raw),
    }
    return b64e(json.dumps(sc, separators=(",", ":")).encode("utf-8"))


def _sidecar_unpack(blob: str) -> dict:
    return json.loads(b64d(blob).decode("utf-8"))


def _prop(props, name, default=None):
    for p in props or []:
        if p.get("name") == name:
            return p.get("value")
    return default


def _obj(oid, x, y, name, props):
    return {"id": oid, "name": name, "point": True, "x": x, "y": y,
            "width": 0, "height": 0, "rotation": 0, "visible": True,
            "properties": [{"name": k, "type": "int", "value": v} for k, v in props]}


def _tile_obj(oid, x, y, gid, w, h, props):
    # tile object: renders the tileset image (bottom-left anchor at x,y). x,y stay the
    # authoritative tile anchor (same round-trip as the point object), gid is display-only.
    return {"id": oid, "name": "", "gid": gid, "x": x, "y": y,
            "width": w, "height": h, "rotation": 0, "visible": True,
            "properties": [{"name": k, "type": "int", "value": v} for k, v in props]}


# ---- export ------------------------------------------------------------

def export(m: strategic.MpModel, tlo_model, png_name: str, tree_sprites=None) -> tuple[dict, dict]:
    """tree_sprites: optional {kind: (png_name, PIL_image, w, h)} to render trees as their
    real sprite art (image-collection tileset); trees without art stay abstract points."""
    from . import tlo as tlomod
    tree_sprites = tree_sprites or {}
    W, H = m.width, m.height
    pass_firstgid = GROUND_FIRSTGID + tlo_model.count
    trees_firstgid = pass_firstgid + 2
    tree_gid = {k: (trees_firstgid + (k - 1), w, h, ox, oy)
                for k, (_pn, _im, w, h, ox, oy) in tree_sprites.items()}

    ground = [0] * (W * H)
    passable = [0] * (W * H)
    for f in range(W * H):
        col, row = divmod(f, H)
        t = row * W + col
        ground[t] = GROUND_FIRSTGID + (m.flags[f] & strategic.GROUND_MASK)
        passable[t] = pass_firstgid + (1 if m.passable[f] else 0)

    oid = 1
    trees = []
    for o in m.objects:
        ax, ay = o["x"] * 32 + o["px_x"], o["y"] * 32 + o["px_y"]
        props = [("kind", o["kind"]), ("slot", o["slot"]), ("next_on_tile", o["next_on_tile"])]
        if o["kind"] in tree_gid:
            gid, w, h, ox, oy = tree_gid[o["kind"]]
            # objectalignment=topleft: object (x,y) is the sprite top-left; place it at
            # anchor - origin so the hotspot lands on the tile anchor (ax, ay).
            trees.append(_tile_obj(oid, ax - ox, ay - oy, gid, w, h, props))
        else:
            trees.append(_obj(oid, ax, ay, "", props))
        oid += 1

    resources = []
    hq = H // 4
    for b in range(len(m.resources_raw) // 16):
        amts = struct.unpack_from("<8H", m.resources_raw, b * 16)
        if not any(amts):
            continue
        bx, by = divmod(b, hq)
        resources.append(_obj(oid, bx * BLOCK_PX, by * BLOCK_PX, "",
                              [("amount%d" % k, amts[k]) for k in range(RES_CHANNELS)])); oid += 1

    starts = []
    for i in range(m.f3):
        x, y, status = struct.unpack_from("<3i", m.tail_raw, i * 12)
        starts.append(_obj(oid, x * 32, y * 32, "", [("status", status)])); oid += 1

    img = tlomod.to_png(tlo_model, columns=GROUND_COLUMNS)
    iw, ih = img.size

    tilesets = [
        {"firstgid": GROUND_FIRSTGID, "name": "ground", "image": png_name,
         "imagewidth": iw, "imageheight": ih,
         "tilewidth": tlo_model.tile_w, "tileheight": tlo_model.tile_h,
         "columns": GROUND_COLUMNS, "tilecount": tlo_model.count, "margin": 0, "spacing": 0},
        {"firstgid": pass_firstgid, "name": "passable", "image": "passable.png",
         "imagewidth": 64, "imageheight": 32, "tilewidth": 32, "tileheight": 32,
         "columns": 2, "tilecount": 2, "margin": 0, "spacing": 0},
    ]
    images = {png_name: img, "passable.png": passable_png()}
    if tree_sprites:
        tiles = []
        for kind, (pn, im, w, h, ox, oy) in sorted(tree_sprites.items()):
            tiles.append({"id": kind - 1, "image": pn, "imagewidth": w, "imageheight": h,
                          "properties": [{"name": "ox", "type": "int", "value": ox},
                                         {"name": "oy", "type": "int", "value": oy}]})
            images[pn] = im
        maxid = max(k - 1 for k in tree_sprites)
        tilesets.append({
            "firstgid": trees_firstgid, "name": "trees", "tilewidth": 0, "tileheight": 0,
            "tilecount": maxid + 1, "columns": 0, "objectalignment": "topleft",
            "grid": {"orientation": "orthogonal", "width": 1, "height": 1},
            "tiles": tiles,
        })

    tmj = {
        "type": "map", "version": "1.10", "tiledversion": "1.10.2",
        "orientation": "orthogonal", "renderorder": "right-down",
        "width": W, "height": H, "tilewidth": 32, "tileheight": 32,
        "infinite": False, "nextlayerid": 6, "nextobjectid": oid,
        "tilesets": tilesets,
        "layers": [
            {"type": "tilelayer", "id": 1, "name": "ground", "width": W, "height": H,
             "x": 0, "y": 0, "opacity": 1, "visible": True, "data": ground},
            {"type": "tilelayer", "id": 2, "name": "passable", "width": W, "height": H,
             "x": 0, "y": 0, "opacity": 0.6, "visible": False, "data": passable},
            {"type": "objectgroup", "id": 3, "name": "trees", "x": 0, "y": 0,
             "opacity": 1, "visible": True, "draworder": "index", "objects": trees},
            {"type": "objectgroup", "id": 4, "name": "resources", "x": 0, "y": 0,
             "opacity": 1, "visible": True, "draworder": "index", "objects": resources},
            {"type": "objectgroup", "id": 5, "name": "player_starts", "x": 0, "y": 0,
             "opacity": 1, "visible": True, "draworder": "index", "objects": starts},
        ],
        "properties": [
            {"name": "mapkit_format", "type": "string", "value": FORMAT},
            {"name": "mapkit_tlo", "type": "string",
             "value": m.tlo_name.split(b"\x00")[0].decode("latin1")},
            {"name": "mapkit_sidecar", "type": "string", "value": _sidecar_pack(m)},
        ],
    }
    return tmj, images


# ---- import ------------------------------------------------------------

def _tileset_firstgid(tmj, name):
    for ts in tmj.get("tilesets", []):
        if ts.get("name") == name:
            return ts["firstgid"]
    raise ValueError(f"ABORT: .tmj has no '{name}' tileset")


def import_(tmj: dict) -> strategic.MpModel:
    blob = _prop(tmj.get("properties"), "mapkit_sidecar")
    if blob is None:
        raise ValueError("ABORT: .tmj is missing the 'mapkit_sidecar' property "
                         "(regenerate with map_decompile; never hand-delete it).")
    sc = _sidecar_unpack(blob)
    if sc.get("format") != FORMAT:
        raise ValueError(f"ABORT: sidecar format {sc.get('format')!r} != {FORMAT!r}")

    m = strategic.MpModel()
    m.width, m.height, m.f1 = sc["width"], sc["height"], sc["f1"]
    m.header_raw = b64d(sc["header_raw"])
    m.tlo_name = b64d(sc["tlo_name_raw"])
    W, H, n = m.width, m.height, m.width * m.height
    flags_hi = struct.unpack("<%dH" % n, b64d(sc["flags_hi"]))
    m.building = list(struct.unpack("<%dH" % n, b64d(sc["building"])))
    m.f1_raw = b64d(sc["f1_raw"])
    m.halfres_raw = b64d(sc["halfres_raw"])

    layers = {l.get("name"): l for l in tmj.get("layers", [])}

    def transpose_in(layer_name, firstgid):
        layer = layers.get(layer_name)
        if layer is None or "data" not in layer:
            raise ValueError(f"ABORT: .tmj has no '{layer_name}' tile layer")
        if len(layer["data"]) != n:
            raise ValueError(f"ABORT: '{layer_name}' has {len(layer['data'])} cells != {n} "
                             "(map resize is not supported)")
        out = [0] * n
        for t, gid in enumerate(layer["data"]):
            col, row = t % W, t // W
            out[col * H + row] = (gid & 0x1FFFFFFF) - firstgid  # strip Tiled flip flags
        return out

    ground_local = transpose_in("ground", _tileset_firstgid(tmj, "ground"))
    m.flags = [(ground_local[f] & strategic.GROUND_MASK) | flags_hi[f] for f in range(n)]
    passable_local = transpose_in("passable", _tileset_firstgid(tmj, "passable"))
    m.passable = [1 if v else 0 for v in passable_local]

    # tree hotspot lookup from the trees tileset (tile-local id -> origin), if present
    tree_ts = next((ts for ts in tmj.get("tilesets", []) if ts.get("name") == "trees"), None)
    tree_first = tree_ts["firstgid"] if tree_ts else None
    tile_origin = {t["id"]: (_prop(t.get("properties"), "ox", 0), _prop(t.get("properties"), "oy", 0))
                   for t in (tree_ts.get("tiles", []) if tree_ts else [])}

    m.objects = []
    for o in (layers.get("trees") or {}).get("objects", []):
        gid = o.get("gid")
        if gid and tree_first is not None:
            local = (gid & 0x1FFFFFFF) - tree_first     # gid (tile drawn in Tiled) is authoritative for kind
            kind = local + 1
            ox, oy = tile_origin.get(local, (0, 0))
            ax, ay = int(round(o["x"])) + ox, int(round(o["y"])) + oy
        else:
            kind = _prop(o.get("properties"), "kind", 0)
            ax, ay = int(round(o["x"])), int(round(o["y"]))
        m.objects.append({"slot": _prop(o.get("properties"), "slot", 0), "kind": kind,
                          "px_x": ax % 32, "px_y": ay % 32,
                          "next_on_tile": _prop(o.get("properties"), "next_on_tile", 0),
                          "x": ax // 32, "y": ay // 32})

    hq = H // 4
    res = bytearray((W // 4) * (H // 4) * 16)
    for o in (layers.get("resources") or {}).get("objects", []):
        bx = int(round(o["x"])) // BLOCK_PX
        by = int(round(o["y"])) // BLOCK_PX
        b = bx * hq + by
        amts = [int(_prop(o.get("properties"), "amount%d" % k, 0)) for k in range(RES_CHANNELS)]
        struct.pack_into("<8H", res, b * 16, *amts)
    m.resources_raw = bytes(res)

    tail = bytearray()
    for o in (layers.get("player_starts") or {}).get("objects", []):
        x = int(round(o["x"])) // 32
        y = int(round(o["y"])) // 32
        tail += struct.pack("<3i", x, y, int(_prop(o.get("properties"), "status", 0)))
    m.tail_raw = bytes(tail)
    m.f3 = len(tail) // 12
    m.f2 = len(m.objects)
    return m
