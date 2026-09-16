""".MP strategic planet-map codec (the MP wire-format notes).

Layout (all sizes byte-validated across the 31 shipped maps):
  0x00  magic "ver02"                              |
  0x05  u32 width                                  | 97-byte header, carried
  0x09  u32 height                                 | verbatim (header_raw) so
  0x0d  char[12] tlo_name                          | every reserved byte survives
  0x35  u32 f1 (always 0)   0x39 u32 f2   0x41 i32 f3
  0x61  main plane      w*h * 4  (u16 flags, u16 building)
        passable plane  w*h * 1
        f1 records      f1 * 28  (dead)
        half-res plane  (w/2)*(h/2) * 4
        f2 objects      f2 * 28  (u32 slot, i32 kind, px_x, px_y, next_on_tile, x, y)
        resource plane  (w/4)*(h/4) * 16
        tail            f3 * 12   (landing spots)

The Model stores every plane as literal on-disk values (flags/building/passable
parsed; the rest opaque bytes), so emit(parse(f)) == f is a pure reshape.

Ground-layer fact (Phase-0 RE, 2026-07-05): main-plane `flags & 0x1FFF` is the
visual tile index into the planet's .TLO (blitted by llm_strat_render_ground_tile);
`flags & ~0x1FFF` are per-tile flags (only 0x8000 seen on disk)."""
from __future__ import annotations

import struct

HEADER_LEN = 0x61
GROUND_MASK = 0x1FFF


class MpModel:
    __slots__ = ("header_raw", "width", "height", "tlo_name", "f1", "f2", "f3",
                 "flags", "building", "passable",
                 "f1_raw", "halfres_raw", "objects", "resources_raw", "tail_raw")


def _plane_bytes(w, h):
    return dict(main=w * h * 4, passable=w * h,
                halfres=(w // 2) * (h // 2) * 4, resources=(w // 4) * (h // 4) * 16)


def parse(path: str) -> MpModel:
    with open(path, "rb") as fh:
        buf = fh.read()
    if buf[0:5] != b"ver02":
        raise ValueError(f"{path}: bad magic {buf[0:5]!r} (expected b'ver02')")
    m = MpModel()
    m.header_raw = buf[0:HEADER_LEN]
    m.width = struct.unpack_from("<I", buf, 0x05)[0]
    m.height = struct.unpack_from("<I", buf, 0x09)[0]
    m.tlo_name = buf[0x0d:0x0d + 12]
    m.f1 = struct.unpack_from("<I", buf, 0x35)[0]
    m.f2 = struct.unpack_from("<I", buf, 0x39)[0]
    m.f3 = struct.unpack_from("<i", buf, 0x41)[0]
    w, h, n = m.width, m.height, m.width * m.height
    pb = _plane_bytes(w, h)
    off = HEADER_LEN

    inter = struct.unpack_from("<%dH" % (n * 2), buf, off)
    m.flags = list(inter[0::2])
    m.building = list(inter[1::2])
    off += pb["main"]

    m.passable = list(buf[off:off + n]); off += n
    m.f1_raw = buf[off:off + m.f1 * 28]; off += m.f1 * 28
    m.halfres_raw = buf[off:off + pb["halfres"]]; off += pb["halfres"]

    m.objects = []
    for _ in range(m.f2):
        s, kind, px, py, nxt, x, y = struct.unpack_from("<Iiiiiii", buf, off); off += 28
        m.objects.append(dict(slot=s, kind=kind, px_x=px, px_y=py,
                              next_on_tile=nxt, x=x, y=y))

    m.resources_raw = buf[off:off + pb["resources"]]; off += pb["resources"]
    m.tail_raw = buf[off:off + 12 * m.f3]; off += 12 * m.f3

    if off != len(buf):
        raise ValueError(f"{path}: consumed {off} != file size {len(buf)} (format drift)")
    return m


def assign_slots(objects):
    """The f2 `slot` field is the storage index into _G_LLM_MAP_OBJECTS[] (loader does
    `objects[slot] = record`), so slots must be UNIQUE or records overwrite each other.
    Preserve every already-valid unique slot (byte-identical round-trip + intact
    next_on_tile chains for retail maps); hand added/duplicate objects (slot<=0 or a
    collision — e.g. a Tiled copy-paste) the next free index."""
    used, slots = set(), [None] * len(objects)
    for i, o in enumerate(objects):
        s = o.get("slot", 0)
        if s > 0 and s not in used:
            used.add(s)
            slots[i] = s
    free = 1
    for i in range(len(slots)):
        if slots[i] is None:
            while free in used:
                free += 1
            used.add(free)
            slots[i] = free
    return slots


def colocated_tiles(objects):
    """Objects sharing a tile that are NOT linked into one next_on_tile chain -> the game
    stamps only one chain-head per tile, so extra unchained ones won't render."""
    from collections import defaultdict
    by_tile = defaultdict(list)
    for o in objects:
        by_tile[(o["x"], o["y"])].append(o)
    linked = {o["next_on_tile"] for o in objects if o["next_on_tile"]}
    bad = []
    for tile, objs in by_tile.items():
        if len(objs) > 1:
            heads = [o for o in objs if o.get("slot", 0) not in linked]
            if len(heads) > 1:
                bad.append((tile, len(objs)))
    return bad


def emit(m: MpModel) -> bytes:
    n = m.width * m.height
    slots = assign_slots(m.objects)
    # Header fields are authoritative from the model (so edits to tlo_name / object
    # count flow through); reserved bytes are preserved from header_raw.
    hdr = bytearray(m.header_raw)
    struct.pack_into("<I", hdr, 0x05, m.width)
    struct.pack_into("<I", hdr, 0x09, m.height)
    hdr[0x0d:0x0d + 12] = m.tlo_name
    struct.pack_into("<I", hdr, 0x35, m.f1)
    struct.pack_into("<I", hdr, 0x39, len(m.objects))
    struct.pack_into("<i", hdr, 0x41, m.f3)
    out = bytearray(hdr)
    inter = [0] * (n * 2)
    inter[0::2] = m.flags
    inter[1::2] = m.building
    out += struct.pack("<%dH" % (n * 2), *inter)
    out += bytes(m.passable)
    out += m.f1_raw
    out += m.halfres_raw
    for i, o in enumerate(m.objects):
        out += struct.pack("<Iiiiiii", slots[i], o["kind"], o["px_x"], o["px_y"],
                           o["next_on_tile"], o["x"], o["y"])
    out += m.resources_raw
    out += m.tail_raw
    return bytes(out)


def write_file(m: MpModel, path: str) -> None:
    with open(path, "wb") as fh:
        fh.write(emit(m))
