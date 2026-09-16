"""ALIEN_*.MAP tactical-mission map codec (the mission-format notes).

Headerless, exactly 296960 B (0x48800), a fixed 128x128 tile grid, three planes:
  0x00000  0x40000  height-slot  int16[8]/tile, COLUMN-major (col*0x800+row*0x10+slot*2)
  0x40000  0x00800  impassability bitmap  1 bit/tile, LSB-first, 16 B/row, idx=row*128+col (ROW-major)
  0x40800  0x08000  terrain-id  u16/tile, ROW-major idx=row*128+col; &0x1FFF = tile into GROUND .TLO

The tileset is out-of-band: the mission POZ*.DAT names GROUND "PODLOGA.TLO" (32x24 tiles).

Model stores literal on-disk values (terrain full u16, impassable bit, height raw), so
emit(parse(f)) == f is a pure reshape. terrain & height are what edits target; the height
plane is carried verbatim (only slot-0 semantics are confirmed - see docs)."""
from __future__ import annotations

import struct

SIZE = 0x48800
GRID = 128
H_OFF, H_LEN = 0x00000, 0x40000
BM_OFF, BM_LEN = 0x40000, 0x00800
T_OFF = 0x40800
GROUND_MASK = 0x1FFF


class TacticalMapModel:
    __slots__ = ("height_raw", "impassable", "terrain")   # impassable/terrain: [row*128+col]


def parse(path: str) -> TacticalMapModel:
    with open(path, "rb") as fh:
        buf = fh.read()
    if len(buf) != SIZE:
        raise ValueError(f"{path}: size {len(buf)} != {SIZE} (not an ALIEN_*.MAP)")
    m = TacticalMapModel()
    m.height_raw = buf[H_OFF:H_OFF + H_LEN]
    bm = buf[BM_OFF:BM_OFF + BM_LEN]
    m.impassable = [(bm[idx >> 3] >> (idx & 7)) & 1 for idx in range(GRID * GRID)]
    m.terrain = list(struct.unpack_from("<%dH" % (GRID * GRID), buf, T_OFF))
    return m


def emit(m: TacticalMapModel) -> bytes:
    out = bytearray(m.height_raw)
    bm = bytearray(BM_LEN)
    for idx, v in enumerate(m.impassable):
        if v:
            bm[idx >> 3] |= 1 << (idx & 7)
    out += bm
    out += struct.pack("<%dH" % (GRID * GRID), *m.terrain)
    return bytes(out)


def write_file(m: TacticalMapModel, path: str) -> None:
    with open(path, "wb") as fh:
        fh.write(emit(m))
