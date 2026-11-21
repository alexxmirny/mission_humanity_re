#!/usr/bin/env python3
"""Independent Python oracle for generated Mission Humanity strategic maps (mods:MAPGEN1).

It shares NO code and NO rules file with the C++ generator (mapgen.exe): the legal tile
pair set is learned here, from the stock maps, through `src.formats.mapkit` only.

  python tools/mapgen_check.py check MAP.mpm... [--wrap] [--islands] [--no-gameplay] [--stock-dir D] [--maps-dir D]
  python tools/mapgen_check.py --selftest
  python tools/mapgen_check.py --sweep --mapgen build/mapgen.exe --rules DIR [--out DIR] [--wrap] [--islands]
  python tools/mapgen_check.py --sheet OUT.png MAP... [--stock-dir D]
  python tools/mapgen_check.py --decals --rules DIR [MAP...]   # report only: rare-ground share / group sizes vs stock
  python tools/mapgen_check.py --lane-maps [--mapgen EXE] [--rules DIR] [--out DIR]   # regenerate the maps of UI row mapgen_seq

check      (a) mapkit emit(parse(f)) == f; header / plane / tree / ore / tail invariants and the
               size envelope (design sec. 1);
           (b) every horizontal / vertical neighbour tile pair must have been seen in the stock maps
               of the same tileset (seam pairs too with --wrap), listed with coordinates;
           (c) 4-neighbour BFS (no wrap unless --bfs-wrap) over passable, tree-free tiles: every landing spot sits on
               such a tile and all landing spots are mutually connected (--islands relaxes the connection, for maps
               generated with --no-paths; the spot-on-free-tile rule stays);
           (d) the AI-facing rules of a generated map (AI_REPORT "Generator constraints"; --no-gameplay skips them,
               e.g. to look at a stock map): even landing-spot count 2..8, status 1, the 5x3 mother footprint
               (x..x+4, y..y+2) free and inside the map, passable tree-free clear radius >= 4 around every spot, an AI
               first-mine ore block (all four channels > 0 and 4a+2b+2c+d >= 500) within 8 tiles of every spot, ore
               values <= 3276 and channels 4-7 zero, tree slots 1..9999, tile ids < 0x1000, description <= 79 bytes.
           Exit 1 on any failure. A stock map is its own learning set, so (b) is 0 on it except
           for seam pairs.
--selftest builds a tiny synthetic map + pair set in memory (no retail data), proves green, then
           plants an illegal pair, a disconnected start and one violation per AI-facing rule and proves each goes red.
--sweep    runs `mapgen gen` for 7 tilesets x {64,128,256} x 2 seeds, checks each map and that the
           same seed twice gives the same SHA-256. Exit 2 if mapgen.exe is missing.
--sheet    preview contact sheet (terrain + landing spots, one caption per map).

Stock map locations: .MP from `--stock-dir` (default: mapkit's CLEAN/mh, falling back to
<POLYGON_CLEAN>/res_unpack/data/uncompressed/mh), .mpm from `--maps-dir` (default <POLYGON>/Maps).
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "tools"))

import machine_config as machine
import tool_paths

from src.formats.mapkit import strategic

GROUND_MASK = 0x1FFF
SIZES = (64, 128, 256)
PLAYERS_BY_SIZE = {64: (2,), 128: (4, 6, 8), 256: (4, 6, 8)}
TREE_HEADROOM = 1.2  # trees <= 1.2 x the shipped maximum for that size
MAX_TREES = 10000
MAX_TREE_SLOT = 9999  # slots index a 10000-entry object table in the loader
MAX_DESC = 254
MAX_DESC_LOADER = 79  # the loader's description buffer
SPOT_DIST_DIV = (
    6  # spots are >= width / this many tiles apart (stock's closest 8-player 128 pair is 21)
)
MIN_SPOT_DIST_OVERRIDE = 0  # --min-spot-dist TILES; 0 = width // SPOT_DIST_DIV
MIN_SPOT_CLEAR = 4  # Chebyshev radius of passable tree-free ground around a landing spot
STOCK_STYLE_ORE = False  # --stock-style: ore on the mother footprint is a note
MAX_FIRST_MINE = 8  # tiles from a spot to the nearest AI-first-mine ore block
RICH_WEIGHT = (
    8000  # 4a+2b+2c+d of a "rich" block (all four channels > 0): the AI's later mines need one
)
MAX_RICH_REACH = 7.5  # tiles, Euclid, footprint centre (x+2, y+1) to the nearest rich block centre (bx*4+2, by*4+2)
MAX_GOOD_REACH = (
    8.0  # ... to the nearest full-channel weight >= 500 block; and >= 2 such blocks within it
)
MIN_BUILD_FIT = 0.20  # share of the 6x6 windows (anchors x-15..x+10, y-15..y+10) around a spot that are fully free
ROOM_FLOOD = 40  # build room: flood depth cap (steps)
ROOM_WIN_R = (
    5  # build room: 4x4 window anchors within this Chebyshev radius of the first-mine block centre
)
MIN_ROOM16 = 150  # build room gate (DESIGN sec. 14): buildable tiles within 16 / 32 flood steps of the footprint
MIN_ROOM32 = 300  # stock (199 spots): room16 min 151 p5 183 median 397; room32 min 159 p1 330 p5 495. windows (4x4 near the
# first mine) are a note only: stock p10 = 0, median 4, so no threshold can be calibrated on them
BUILD_WIN = 6  # the largest footprint (Baza)
MAX_ORE = 3276  # the game reads ore as a signed 16-bit after a x10
MAX_TILE_ID = 0x1000  # the loader replaces the flag high nibble
DESC_LINES, DESC_COLS = 4, 26
SWEEP_SEEDS = (1, 2)
SWEEP_PLAYERS = {64: 2, 128: 4, 256: 4}


# ----------------------------------------------------------------------------------- locations
def default_stock_dir() -> str:
    """Directory holding the stock .MP files (mapkit's CLEAN/mh, else the EN clean unpack)."""
    cands = [
        os.path.join(machine.RES_UNPACK, "mh"),
        os.path.join(machine.POLYGON_CLEAN, "res_unpack", "data", "uncompressed", "mh"),
    ]
    for c in cands:
        if glob.glob(os.path.join(c, "*.MP")):
            return c
    return cands[0]


def default_maps_dir() -> str:
    return os.path.join(machine.POLYGON, "Maps")


def stock_files(stock_dir: str, maps_dir: str) -> list[str]:
    return sorted(glob.glob(os.path.join(stock_dir, "*.MP"))) + sorted(
        glob.glob(os.path.join(maps_dir, "*.mpm"))
    )


# ----------------------------------------------------------------------------- learned rules
def tileset_of(m) -> str:
    return m.tlo_name.split(b"\0")[0].decode("ascii", "replace").lower()


def tile_ids(m) -> list[int]:
    return [f & GROUND_MASK for f in m.flags]


class Rules:
    """What the stock maps taught us (per tileset) plus size-wide statistics."""

    def __init__(self):
        self.h: dict[str, set] = defaultdict(set)
        self.v: dict[str, set] = defaultdict(set)
        self.kinds: dict[str, set] = defaultdict(set)
        self._pass: dict[str, dict[int, list]] = defaultdict(lambda: defaultdict(lambda: [0, 0]))
        self.max_trees: dict[int, int] = {}
        self.maps: Counter = Counter()

    def learn(self, m) -> None:
        ts, w, hh = tileset_of(m), m.width, m.height
        ids = tile_ids(m)
        self.maps[ts] += 1
        h, v = self.h[ts], self.v[ts]
        for c in range(w):
            col = c * hh
            for r in range(hh):
                a = ids[col + r]
                if c + 1 < w:
                    h.add((a, ids[col + hh + r]))
                if r + 1 < hh:
                    v.add((a, ids[col + r + 1]))
        for o in m.objects:
            self.kinds[ts].add(o["kind"])
        treed = {(o["x"], o["y"]) for o in m.objects}
        for c in range(w):
            for r in range(hh):
                if (c, r) not in treed:
                    self._pass[ts][ids[c * hh + r]][1 if m.passable[c * hh + r] else 0] += 1
        self.max_trees[w] = max(self.max_trees.get(w, 0), len(m.objects))

    def seen_pass(self, ts: str, tile: int) -> set:
        """The passability values (0/1) this tile id has carried in stock maps (off-tree)."""
        c = self._pass[ts].get(tile)
        return set() if not c else {i for i in (0, 1) if c[i]}

    def majority(self, ts: str, tile: int):
        """1 / 0 majority passability of a tile id, or None when never seen without a tree."""
        c = self._pass[ts].get(tile)
        if not c:
            return None
        return 1 if c[1] > c[0] else 0

    def knows(self, ts: str) -> bool:
        return self.maps[ts] > 0


def learn_stock(files) -> Rules:
    rules = Rules()
    for f in files:
        rules.learn(strategic.parse(f))
    return rules


# ----------------------------------------------------------------------------------- checking
class Report:
    def __init__(self, name: str):
        self.name = name
        self.errors: list[str] = []
        self.notes: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    @property
    def ok(self) -> bool:
        return not self.errors


def _check_header(m, rep: Report) -> None:
    w, h, hdr = m.width, m.height, m.header_raw
    if hdr[:5] != b"ver02":
        rep.err("magic is not ver02")
    if w != h:
        rep.err(f"not square: {w}x{h}")
    if w % 4 or h % 4:
        rep.err(f"dimension not a multiple of 4: {w}x{h}")
    u = lambda off: struct.unpack_from("<I", hdr, off)[0]
    expect = {0x2D: 2 * w * h, 0x31: w * h, 0x3D: (w // 4) * (h // 4)}
    for off, val in expect.items():
        if u(off) != val:
            rep.err(f"header u32 at 0x{off:02x} = {u(off)}, expected {val}")
    if m.f1 != 0 or m.f1_raw:
        rep.err(f"f1 (0x35) = {m.f1}, expected 0 and no f1 records")
    if u(0x39) != len(m.objects):
        rep.err(f"f2 (0x39) = {u(0x39)} != tree count {len(m.objects)}")
    if m.f3 != len(m.tail_raw) // 12:
        rep.err(f"f3 (0x41) = {m.f3} != landing-spot count {len(m.tail_raw) // 12}")
    name = hdr[0x0D:0x19]
    nul = name.find(b"\0")
    if nul >= 0 and any(name[nul:]):
        rep.err("tileset name has bytes after the NUL")
    if not name[: nul if nul >= 0 else 12].isascii() or not name.strip(b"\0"):
        rep.err(f"tileset name {name!r} is empty or non-ASCII")
    covered = set(range(0x19 + 0)) | set(range(0x2D, 0x45))
    stray = [i for i in range(0x61) if i not in covered and hdr[i]]
    if stray:
        rep.err(
            "non-zero header bytes outside known fields at "
            + ", ".join(f"0x{i:02x}" for i in stray[:8])
        )
    if len(hdr) != 0x61:
        rep.err(f"header length {len(hdr)}")


def _check_planes(m, rules: Rules, rep: Report) -> None:
    w, h = m.width, m.height
    n = w * h
    ts = tileset_of(m)
    bad_flags = [i for i, f in enumerate(m.flags) if not f & 0x8000 or f & ~(GROUND_MASK | 0x8000)]
    if bad_flags:
        i = bad_flags[0]
        rep.err(
            f"{len(bad_flags)} tiles with flags lacking 0x8000 or with stray bits; first ({i // h},{i % h}) = 0x{m.flags[i]:04x}"
        )
    if any(m.halfres_raw):
        rep.err("half-res plane is not all zero")
    if len(m.halfres_raw) != (w // 2) * (h // 2) * 4:
        rep.err("half-res plane has the wrong size")
    if len(m.resources_raw) != (w // 4) * (h // 4) * 16:
        rep.err("ore plane has the wrong size")
    else:
        bad = 0
        for b in range(len(m.resources_raw) // 16):
            vals = struct.unpack_from("<8H", m.resources_raw, b * 16)
            if any(vals[4:]):
                bad += 1
        if bad:
            rep.err(f"{bad} ore blocks with non-zero channels 4-7")
    if len(m.passable) != n or any(p not in (0, 1) for p in m.passable):
        rep.err("passable plane is not n bytes of 0/1")

    # trees
    occupied = {}
    if len(m.objects) > MAX_TREES:
        rep.err(f"{len(m.objects)} trees > {MAX_TREES}")
    for i, o in enumerate(m.objects):
        where = f"tree #{i + 1} at ({o['x']},{o['y']})"
        if not (0 <= o["x"] < w and 0 <= o["y"] < h):
            rep.err(f"{where}: out of range")
            continue
        if not 1 <= o["slot"] <= MAX_TREE_SLOT:
            rep.err(f"{where}: slot {o['slot']} outside 1..{MAX_TREE_SLOT}")
        elif o["slot"] != i + 1:
            rep.err(f"{where}: slot {o['slot']} != {i + 1}")
        if not (0 <= o["px_x"] < 32 and 0 <= o["px_y"] < 32):
            rep.err(f"{where}: pixel offset ({o['px_x']},{o['px_y']}) outside 0..31")
        if o["next_on_tile"] != 0:
            rep.err(f"{where}: next_on_tile = {o['next_on_tile']}")
        if (o["x"], o["y"]) in occupied:
            rep.err(f"{where}: second tree on the same tile")
        occupied[o["x"], o["y"]] = i + 1
        if rules.knows(ts) and o["kind"] not in rules.kinds[ts]:
            rep.err(f"{where}: kind {o['kind']} never seen in stock {ts} maps")
    if len(m.building) == n:
        stray = 0
        for c in range(w):
            for r in range(h):
                b = m.building[c * h + r]
                if b != occupied.get((c, r), 0):
                    stray += 1
                    if stray <= 3:
                        rep.err(
                            f"building field at ({c},{r}) = {b}, tree table says {occupied.get((c, r), 0)}"
                        )
                if (c, r) in occupied and m.passable[c * h + r]:
                    rep.err(f"passable tile under tree at ({c},{r})")
        if stray > 3:
            rep.err(f"... {stray} building/tree mismatches in total")
    # passability: a tile must carry a value stock maps gave that tile id (mixed tiles may be either)
    if rules.knows(ts):
        ids = tile_ids(m)
        wrong, first, off_major = 0, None, 0
        for c in range(w):
            for r in range(h):
                if (c, r) in occupied:
                    continue
                t, p = ids[c * h + r], m.passable[c * h + r]
                if p not in rules.seen_pass(ts, t):
                    wrong += 1
                    first = first or (c, r)
                elif p != rules.majority(ts, t):
                    off_major += 1
        if wrong:
            rep.err(
                f"{wrong} tiles carry a passability never seen for that tile id; first at {first}"
            )
        rep.notes.append(f"{off_major} mixed tiles off their majority passability")


def _check_tail(m, raw_len: int, rep: Report, gameplay: bool = False) -> None:
    if m.mpm_id is None:
        rep.notes.append("no .mpm tail (id/description): .MP style file")
        return
    prefix_len = raw_len - 4 - len(m.mpm_desc)
    if raw_len < 4:
        rep.err("file too short for a tail")
        return
    d = m.mpm_desc
    if len(d) > MAX_DESC:
        rep.err(f"description is {len(d)} bytes > {MAX_DESC}")
    if gameplay and len(d) > MAX_DESC_LOADER:
        rep.err(f"description is {len(d)} bytes > {MAX_DESC_LOADER} (the loader's buffer)")
    if b"\0" in d or not d.isascii():
        rep.err("description has NUL or non-ASCII bytes")
    if d.replace(b"\r\n", b"").count(b"\r") or d.replace(b"\r\n", b"").count(b"\n"):
        rep.err("description has a bare CR or LF (CRLF lines only)")
    lines = d.decode("ascii", "replace").split("\r\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) > DESC_LINES or any(len(x) > DESC_COLS for x in lines):
        rep.err(
            f"description exceeds {DESC_LINES} lines x {DESC_COLS} chars ({len(lines)} lines, longest {max(map(len, lines), default=0)})"
        )
    rep.notes.append(f"id/desc ok-checked against {prefix_len} preceding bytes")


def _check_envelope(m, rules: Rules, rep: Report) -> None:
    w = m.width
    if w not in SIZES:
        rep.err(f"size {w} not in {SIZES}")
        return
    if m.f3 not in PLAYERS_BY_SIZE[w]:
        rep.err(f"{m.f3} landing spots not allowed at size {w} (allowed {PLAYERS_BY_SIZE[w]})")
    cap = rules.max_trees.get(w)
    if cap and len(m.objects) > TREE_HEADROOM * cap:
        rep.err(f"{len(m.objects)} trees > {TREE_HEADROOM} x shipped max {cap} at size {w}")


def _landing(m) -> list[tuple[int, int, int]]:
    return [struct.unpack_from("<3i", m.tail_raw, i * 12) for i in range(len(m.tail_raw) // 12)]


def illegal_pairs(m, rules: Rules, wrap: bool):
    """[(kind, (x,y), (x2,y2), (tileA, tileB))] for pairs absent from the learned set."""
    ts, w, h = tileset_of(m), m.width, m.height
    ids = tile_ids(m)
    hs, vs = rules.h[ts], rules.v[ts]
    out = []
    for c in range(w):
        c2 = c + 1
        if c2 == w:
            if not wrap:
                c2 = None
            else:
                c2 = 0
        for r in range(h):
            a = ids[c * h + r]
            if c2 is not None and (a, ids[c2 * h + r]) not in hs:
                out.append(("h", (c, r), (c2, r), (a, ids[c2 * h + r])))
            r2 = r + 1
            if r2 == h:
                if not wrap:
                    continue
                r2 = 0
            if (a, ids[c * h + r2]) not in vs:
                out.append(("v", (c, r), (c, r2), (a, ids[c * h + r2])))
    return out


def connectivity(m, wrap: bool = False):
    """(landing spots not on passable tree-free tiles, spot indices not connected to spot 0)."""
    w, h = m.width, m.height
    free = bytearray(w * h)
    for i, p in enumerate(m.passable):
        free[i] = 1 if p and not m.building[i] else 0
    spots = _landing(m)
    off = [
        i
        for i, (x, y, _s) in enumerate(spots)
        if not (0 <= x < w and 0 <= y < h and free[x * h + y])
    ]
    seen = bytearray(w * h)
    if (
        spots
        and spots[0][0] in range(w)
        and spots[0][1] in range(h)
        and free[spots[0][0] * h + spots[0][1]]
    ):
        start = spots[0][0] * h + spots[0][1]
        seen[start] = 1
        stack = [start]
        while stack:
            i = stack.pop()
            c, r = divmod(i, h)
            for dc, dr in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                c2, r2 = c + dc, r + dr
                if wrap:
                    c2, r2 = c2 % w, r2 % h
                if 0 <= c2 < w and 0 <= r2 < h:
                    j = c2 * h + r2
                    if free[j] and not seen[j]:
                        seen[j] = 1
                        stack.append(j)
    cut = [i for i, (x, y, _s) in enumerate(spots) if i not in off and not seen[x * h + y]]
    if 0 in off:
        cut = [i for i in range(1, len(spots)) if i not in off]
    return off, cut


def _ring_free(free, w: int, h: int, x: int, y: int, r: int) -> bool:
    """Every tile on the Chebyshev ring of radius r around (x, y) is free (torus)."""
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if max(abs(dx), abs(dy)) == r and not free[((x + dx) % w) * h + (y + dy) % h]:
                return False
    return True


def clear_radius(free, w: int, h: int, x: int, y: int, rmax: int = 12) -> int:
    """Largest r such that every ring 0..r around (x, y) is free; -1 when the spot tile itself is not."""
    for r in range(rmax + 1):
        if not _ring_free(free, w, h, x, y, r):
            return r - 1
    return rmax


def build_fit(free, w: int, h: int, x: int, y: int) -> tuple[int, int]:
    """(fully free 6x6 windows, windows) over the anchors x-15..x+10, y-15..y+10 around a spot (torus)."""
    fit = total = 0
    for ax in range(x - 15, x + 11):
        for ay in range(y - 15, y + 11):
            total += 1
            fit += all(
                free[((ax + dx) % w) * h + (ay + dy) % h]
                for dx in range(BUILD_WIN)
                for dy in range(BUILD_WIN)
            )
    return fit, total


def ore_blocks(m):
    """[(br, bc, (c0, c1, c2, c3, c4, c5, c6, c7))] for every block; block index = bc * (w/4) + br."""
    b_n = m.width // 4
    out = []
    for bc in range(b_n):
        for br in range(b_n):
            out.append((br, bc, struct.unpack_from("<8H", m.resources_raw, (bc * b_n + br) * 16)))
    return out


def buildable_plane(m, blocks_all):
    """bytearray (index x * h + y): passable, tree-free and not under an ore block (a building cannot go there)."""
    w, h = m.width, m.height
    b_n = w // 4
    ore = bytearray(b_n * b_n)
    for br, bc, c in blocks_all:
        ore[bc * b_n + br] = 1 if any(c[:4]) else 0
    out = bytearray(w * h)
    for x in range(w):
        for y in range(h):
            i = x * h + y
            if m.passable[i] and not m.building[i] and not ore[(x >> 2) * b_n + (y >> 2)]:
                out[i] = 1
    return out


def first_mine_block(blocks_all, w: int, h: int, x: int, y: int):
    """(bc, br) of the full-channel weight >= 500 block whose centre is nearest the footprint centre, else None."""
    cx, cy = x + 2, y + 1
    best = None
    for br, bc, c in blocks_all:  # bc-major, strict < : the lowest (bc, br) wins a tie
        if min(c[:4]) <= 0 or 4 * c[0] + 2 * c[1] + 2 * c[2] + c[3] < 500:
            continue
        dx = abs(cx - (bc * 4 + 2))
        dy = abs(cy - (br * 4 + 2))
        d2 = min(dx, w - dx) ** 2 + min(dy, h - dy) ** 2
        if best is None or d2 < best[0]:
            best = (d2, bc, br)
    return None if best is None else (best[1], best[2])


def build_room(buildable, w: int, h: int, x: int, y: int, fm) -> tuple[int, int, int]:
    """(tiles within 16 steps, tiles within 32 steps, free 4x4 windows near the first mine).

    Flood (4-neighbour, torus) from the tiles ringing the 5x3 mother footprint over buildable tiles; the ring is
    step 1. Windows: 4x4 anchors (top-left) within Chebyshev ROOM_WIN_R of the first-mine block centre whose 16
    tiles were all reached (flood capped at ROOM_FLOOD steps) and miss the footprint grown by 1."""
    fp = {((x + dx) % w, (y + dy) % h) for dx in range(5) for dy in range(3)}
    dist: dict[int, int] = {}
    cur = []
    for dx in range(-1, 6):
        for dy in range(-1, 4):
            t = ((x + dx) % w, (y + dy) % h)
            if t in fp:
                continue
            i = t[0] * h + t[1]
            if buildable[i] and i not in dist:
                dist[i] = 1
                cur.append(i)
    step = 1
    while cur and step < ROOM_FLOOD:
        step += 1
        nxt = []
        for i in cur:
            c, r = divmod(i, h)
            for dc, dr in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                c2, r2 = (c + dc) % w, (r + dr) % h
                j = c2 * h + r2
                if buildable[j] and j not in dist and (c2, r2) not in fp:
                    dist[j] = step
                    nxt.append(j)
        cur = nxt
    n16 = sum(1 for d in dist.values() if d <= 16)
    n32 = sum(1 for d in dist.values() if d <= 32)
    win = 0
    if fm is not None:
        cx, cy = fm[0] * 4 + 2, fm[1] * 4 + 2
        keep_out = {((x + dx) % w, (y + dy) % h) for dx in range(-1, 6) for dy in range(-1, 4)}
        for ax in range(cx - ROOM_WIN_R, cx + ROOM_WIN_R + 1):
            for ay in range(cy - ROOM_WIN_R, cy + ROOM_WIN_R + 1):
                ok = True
                for dx in range(4):
                    for dy in range(4):
                        t = ((ax + dx) % w, (ay + dy) % h)
                        if t in keep_out or (t[0] * h + t[1]) not in dist:
                            ok = False
                            break
                    if not ok:
                        break
                win += ok
    return n16, n32, win


def spot_rooms(m):
    """[(x, y, n16, n32, windows)] per landing spot of a map (needs the ore plane)."""
    w, h = m.width, m.height
    if len(m.resources_raw) != (w // 4) * (h // 4) * 16:
        return []
    blocks_all = ore_blocks(m)
    bp = buildable_plane(m, blocks_all)
    out = []
    for x, y, _s in _landing(m):
        if not (0 <= x and x + 4 <= w - 1 and 0 <= y and y + 2 <= h - 1):
            continue
        out.append((x, y, *build_room(bp, w, h, x, y, first_mine_block(blocks_all, w, h, x, y))))
    return out


def check_gameplay(m, rep: Report) -> None:
    """The AI-facing rules a generated map must meet (AI_REPORT "Generator constraints")."""
    w, h = m.width, m.height
    spots = _landing(m)
    n = len(spots)
    if n % 2 or not 2 <= n <= 8:
        rep.err(f"landing-spot count {n} is not even in 2..8")
    free = bytearray(w * h)
    for i, p in enumerate(m.passable):
        free[i] = 1 if p and not m.building[i] else 0
    if any(f & 0x1FFF >= MAX_TILE_ID for f in m.flags):
        rep.err(f"tile id >= 0x{MAX_TILE_ID:x} (the loader replaces the high nibble of the flags)")
    # ore
    if len(m.resources_raw) == (w // 4) * (h // 4) * 16:
        blocks = blocks_all = ore_blocks(m)
        big = [(br, bc, c[:4]) for br, bc, c in blocks if max(c[:4]) > MAX_ORE]
        if big:
            rep.err(f"{len(big)} ore blocks above {MAX_ORE}; first {big[0]}")
        good = [
            (br, bc)
            for br, bc, c in blocks
            if min(c[:4]) > 0 and 4 * c[0] + 2 * c[1] + 2 * c[2] + c[3] >= 500
        ]
    else:
        good = []
        blocks_all = []
    room_plane = buildable_plane(m, blocks_all) if blocks_all else None
    min_dist = MIN_SPOT_DIST_OVERRIDE or max(w, h) // SPOT_DIST_DIV
    for i in range(n):
        for j in range(i + 1, n):
            dx = abs(spots[i][0] - spots[j][0])
            dy = abs(spots[i][1] - spots[j][1])
            dx, dy = min(dx, w - dx), min(dy, h - dy)
            if max(dx, dy) < min_dist:
                rep.err(
                    f"landing spots {i} and {j} are {max(dx, dy)} tiles apart (Chebyshev, torus) < {min_dist}"
                )
            # footprint (x..x+4, y..y+2) grown by the clear radius: the two boxes must not intersect
            sx = (spots[j][0] - spots[i][0]) % w
            sy = (spots[j][1] - spots[i][1]) % h
            gx, gy = 5 + 2 * MIN_SPOT_CLEAR, 3 + 2 * MIN_SPOT_CLEAR
            if (sx < gx or w - sx < gx) and (sy < gy or h - sy < gy):
                rep.err(f"landing spots {i} and {j}: footprints with their clear radii overlap")
    for i, (x, y, status) in enumerate(spots):
        tag = f"landing spot {i} ({x},{y})"
        if status != 1:
            rep.err(f"{tag}: status {status} != 1")
        if not (0 <= x and x + 4 <= w - 1 and 0 <= y and y + 2 <= h - 1):
            rep.err(f"{tag}: the 5x3 mother footprint (x..x+4, y..y+2) leaves the map")
            continue
        if not all(free[(x + dx) * h + y + dy] for dy in range(3) for dx in range(5)):
            rep.err(f"{tag}: the 5x3 mother footprint (x..x+4, y..y+2) is not free")
        r = clear_radius(free, w, h, x, y)
        if r < MIN_SPOT_CLEAR:
            rep.err(f"{tag}: passable tree-free clear radius {r} < {MIN_SPOT_CLEAR}")
        for br, bc, c in blocks_all:
            if (
                any(c[:4])
                and 4 * bc <= x + 4
                and 4 * bc + 3 >= x
                and 4 * br <= y + 2
                and 4 * br + 3 >= y
            ):
                msg = f"{tag}: ore block ({bc}, {br}) overlaps the mother footprint"
                (rep.notes.append if STOCK_STYLE_ORE else rep.err)(msg)
        best = 1 << 30
        for br, bc in good:
            dy = min(min(abs(y - (br * 4 + k)), h - abs(y - (br * 4 + k))) for k in range(4))
            dx = min(min(abs(x - (bc * 4 + k)), w - abs(x - (bc * 4 + k))) for k in range(4))
            best = min(best, max(dx, dy))
        if best > MAX_FIRST_MINE:
            rep.err(
                f"{tag}: no first-mine ore block (all four channels > 0, 4a+2b+2c+d >= 500) within {MAX_FIRST_MINE} tiles"
            )
        # ore reach: distances from the footprint centre to the block centres (torus)
        cx, cy = x + 2, y + 1
        near_rich = near_good = 1e9
        n_good = 0
        for br, bc, c in blocks_all:
            if min(c[:4]) <= 0:
                continue
            wt = 4 * c[0] + 2 * c[1] + 2 * c[2] + c[3]
            if wt < 500:
                continue
            dx = abs(cx - (bc * 4 + 2))
            dy = abs(cy - (br * 4 + 2))
            dist = (min(dx, w - dx) ** 2 + min(dy, h - dy) ** 2) ** 0.5
            near_good = min(near_good, dist)
            n_good += dist <= MAX_GOOD_REACH
            if wt >= RICH_WEIGHT:
                near_rich = min(near_rich, dist)
        sink = rep.notes.append if STOCK_STYLE_ORE else rep.err
        r16, r32, rwin = build_room(
            room_plane, w, h, x, y, first_mine_block(blocks_all, w, h, x, y) if blocks_all else None
        )
        rep.notes.append(
            f"{tag}: build room {r16} / {r32} tiles within 16 / 32 steps, {rwin} free 4x4 windows"
        )
        if r16 < MIN_ROOM16 or r32 < MIN_ROOM32:
            sink(
                f"{tag}: build room {r16} tiles within 16 steps, {r32} within 32 (need {MIN_ROOM16} / {MIN_ROOM32}): the base is sealed in"
            )
        if near_rich > MAX_RICH_REACH:
            sink(
                f"{tag}: nearest rich ore block (weight >= {RICH_WEIGHT}) is {near_rich:.1f} tiles away (max {MAX_RICH_REACH})"
            )
        if near_good > MAX_GOOD_REACH or n_good < 2:
            sink(
                f"{tag}: {n_good} full-channel ore blocks (weight >= 500) within {MAX_GOOD_REACH:g} tiles, nearest {near_good:.1f} (need 2+ and nearest <= {MAX_GOOD_REACH:g})"
            )


def check_bytes(
    buf: bytes,
    rules: Rules,
    name: str,
    wrap: bool,
    envelope: bool = True,
    tmpdir=None,
    bfs_wrap: bool = False,
    gameplay: bool = True,
    islands: bool = False,
) -> Report:
    rep = Report(name)
    with tempfile.NamedTemporaryFile(suffix=".mpm", delete=False, dir=tmpdir) as fh:
        fh.write(buf)
        tmp = fh.name
    try:
        m = strategic.parse(tmp)
    except Exception as exc:  # noqa: BLE001 - any parse failure is a verdict
        rep.err(f"mapkit parse failed: {exc}")
        return rep
    finally:
        os.unlink(tmp)
    # (a)
    if strategic.emit(m) != buf:
        rep.err("mapkit emit(parse(f)) != f (not byte-exact)")
    _check_header(m, rep)
    _check_planes(m, rules, rep)
    if m.mpm_id is not None:
        got = strategic.mpm_id_of(buf[: len(buf) - 4 - len(m.mpm_desc)])
        if got != m.mpm_id:
            rep.err(f"id 0x{m.mpm_id:08x} != CRC-32 0x{got:08x} of preceding bytes")
    _check_tail(m, len(buf), rep, gameplay)
    if envelope:
        _check_envelope(m, rules, rep)
    # (b)
    ts = tileset_of(m)
    if not rules.knows(ts):
        rep.err(f"tileset {ts!r} has no stock maps: no pair set to check against")
    else:
        bad = illegal_pairs(m, rules, wrap)
        for kind, p, q, t in bad[:20]:
            rep.err(f"illegal {kind} pair {t[0]}->{t[1]} at {p}-{q}")
        if len(bad) > 20:
            rep.err(f"... {len(bad)} illegal pairs in total")
        rep.notes.append(f"{len(bad)} illegal pairs ({'with' if wrap else 'no'} seam)")
    # (c)
    off, cut = connectivity(m, bfs_wrap)
    for i in off:
        rep.err(f"landing spot {i} {_landing(m)[i][:2]} is not on a passable tree-free tile")
    if not islands:
        for i in cut:
            rep.err(f"landing spot {i} {_landing(m)[i][:2]} not connected to spot 0")
    if gameplay:
        check_gameplay(m, rep)
    return rep


def check_file(
    path: str,
    rules: Rules,
    wrap: bool,
    envelope: bool = True,
    bfs_wrap: bool = False,
    gameplay: bool = True,
    islands: bool = False,
) -> Report:
    with open(path, "rb") as fh:
        buf = fh.read()
    return check_bytes(
        buf,
        rules,
        os.path.basename(path),
        wrap,
        envelope,
        bfs_wrap=bfs_wrap,
        gameplay=gameplay,
        islands=islands,
    )


def print_report(rep: Report, verbose: bool = True) -> None:
    print(f"{'PASS' if rep.ok else 'FAIL'}  {rep.name}")
    for e in rep.errors:
        print(f"    - {e}")
    if verbose:
        for n in rep.notes:
            print(f"    . {n}")


# ----------------------------------------------------------------------------------- selftest
def synth_map(
    w=16,
    h=16,
    wall_rows=10,
    wall_start=0,
    trees=((4, 12), (10, 12)),
    spots=((2, 2), (12, 2)),
    wall_cols=(7, 8),
    ore=None,
    desc=b"Synthetic\r\nselftest map\r\n",
):
    """A tiny valid map: tile 1 passable, tile 2 blocked wall at columns 7-8 rows < wall_rows."""
    m = strategic.MpModel()
    n = w * h
    hdr = bytearray(0x61)
    hdr[:5] = b"ver02"
    struct.pack_into("<II", hdr, 5, w, h)
    hdr[0x0D : 0x0D + 6] = b"synth\0"[:6]
    struct.pack_into("<III", hdr, 0x2D, 2 * w * h, w * h, 0)
    struct.pack_into("<I", hdr, 0x3D, (w // 4) * (h // 4))
    m.header_raw = bytes(hdr)
    m.width, m.height = w, h
    m.tlo_name = bytes(hdr[0x0D : 0x0D + 12])
    m.f1, m.f2, m.f3 = 0, len(trees), len(spots)
    ids = [1] * n
    for c in wall_cols:
        for r in range(wall_start, wall_rows):
            ids[c * h + r] = 2
    m.flags = [i | 0x8000 for i in ids]
    m.building = [0] * n
    m.passable = [0 if i == 2 else 1 for i in ids]
    m.objects = []
    for k, (x, y) in enumerate(trees):
        m.building[x * h + y] = k + 1
        m.passable[x * h + y] = 0
        m.objects.append(
            {"slot": k + 1, "kind": 5, "px_x": 3, "px_y": 4, "next_on_tile": 0, "x": x, "y": y}
        )
    m.f1_raw = b""
    m.halfres_raw = bytes((w // 2) * (h // 2) * 4)
    res = bytearray((w // 4) * (h // 4) * 16)
    for (br, bc), vals in (ore or {}).items():
        struct.pack_into("<4H", res, (bc * (w // 4) + br) * 16, *vals)
    m.resources_raw = bytes(res)
    m.tail_raw = b"".join(struct.pack("<3i", x, y, 1) for x, y in spots)
    m.mpm_id, m.mpm_desc = 0, desc
    strategic.finalize_mpm(m)
    return m


def synth_ai_map(**kw):
    """A valid 32x32 map for the AI-facing rules: spots (6,14) and (24,14) on open ground, a first-mine block at each."""
    args = {
        "w": 32,
        "h": 32,
        "wall_rows": 10,
        "wall_cols": (15, 16),
        "trees": ((20, 28), (28, 28)),
        "spots": ((6, 14), (24, 14)),
        "ore": {  # per spot: a first-mine block and a rich one (4a+2b+2c+d >= 8000), both within 7 tiles
            (2, 1): (200, 100, 100, 100),
            (2, 2): (2000, 1000, 1000, 1000),
            (2, 6): (200, 100, 100, 100),
            (2, 7): (2000, 1000, 1000, 1000),
        },
    }
    args.update(kw)
    return synth_map(**args)


def selftest() -> int:
    rules = Rules()
    rules.learn(synth_map())  # wall map: 1-1, 1-2, 2-1, 2-2 horizontally; 2-2, 2-1 vertically
    rules.learn(synth_map(wall_start=2))  # adds the vertical 1-2 pair (needed by the seam)
    results = []

    def expect(label, buf, green, wrap=False, needle=None, gameplay=False, islands=False):
        rep = check_bytes(
            buf, rules, label, wrap, envelope=False, gameplay=gameplay, islands=islands
        )
        good = rep.ok == green
        if needle is not None and not green:
            good = good and any(needle in e for e in rep.errors)
        results.append(good)
        want = "green" if green else f"red{f' ({needle})' if needle else ''}"
        print(
            f"{'ok  ' if good else 'BAD '} selftest: {label}: {'green' if rep.ok else 'red'} (wanted {want})"
        )
        if not good:
            print_report(rep)

    base = synth_map()
    expect("valid synthetic map", strategic.emit(base), True, wrap=True)

    bad_pair = synth_map()
    bad_pair.flags[3 * 16 + 3] = 3 | 0x8000  # tile 3 has no learned neighbours
    bad_pair.passable[3 * 16 + 3] = 1
    strategic.finalize_mpm(bad_pair)
    expect("planted illegal pair", strategic.emit(bad_pair), False)

    cut = synth_map(wall_rows=16)  # wall now spans the full height: the two starts are split
    strategic.finalize_mpm(cut)
    expect("planted disconnected start", strategic.emit(cut), False)

    on_tree = synth_map(spots=((4, 12), (12, 2)))
    expect("start on a tree tile", strategic.emit(on_tree), False)

    bad_id = synth_map()
    bad_id.mpm_id ^= 1
    expect("corrupt tail id", strategic.emit(bad_id), False)

    seam = synth_map()
    seam.flags[0] = 2 | 0x8000  # (0,0) blocked tile next to tile 1 across the seam: legal pair
    seam.flags[15 * 16] = 2 | 0x8000
    seam.passable[0] = seam.passable[15 * 16] = 0
    strategic.finalize_mpm(seam)
    expect("legal seam (no pair broken)", strategic.emit(seam), True, wrap=True)

    # ---- the AI-facing rules: one planted violation each, on a map that is green without it
    ai = lambda m: strategic.emit(m)
    expect("AI map: valid", ai(synth_ai_map()), True, wrap=True, gameplay=True)
    expect(
        "AI rule: odd landing-spot count",
        ai(synth_ai_map(spots=((6, 14), (24, 14), (15, 24)))),
        False,
        needle="not even",
        gameplay=True,
    )
    fp = synth_ai_map(
        trees=((20, 28), (28, 28), (10, 16))
    )  # tree on the footprint corner (x+4, y+2)
    expect("AI rule: 5x3 footprint blocked", ai(fp), False, needle="is not free", gameplay=True)
    cr = synth_ai_map()
    cr.flags[2 * 32 + 14] = (
        2 | 0x8000
    )  # blocked tile at (x-4, y): outside the footprint, inside radius 4
    cr.passable[2 * 32 + 14] = 0
    strategic.finalize_mpm(cr)
    expect("AI rule: clear radius 3", ai(cr), False, needle="clear radius 3", gameplay=True)
    expect(
        "AI rule: footprint leaves the map",
        ai(synth_ai_map(spots=((6, 14), (28, 14)))),
        False,
        needle="leaves the map",
        gameplay=True,
    )
    expect(
        "AI rule: ore above 3276",
        ai(synth_ai_map(ore={(2, 1): (3277, 100, 100, 100), (2, 6): (200, 100, 100, 100)})),
        False,
        needle="above 3276",
        gameplay=True,
    )
    expect(
        "AI rule: ore block on the mother footprint",
        ai(
            synth_ai_map(
                ore={
                    (2, 1): (200, 100, 100, 100),
                    (2, 6): (200, 100, 100, 100),
                    (3, 1): (10, 0, 0, 0),
                }
            )
        ),
        False,
        needle="overlaps the mother footprint",
        gameplay=True,
    )
    expect(
        "AI rule: no first-mine ore near a spot",
        ai(synth_ai_map(ore={(2, 1): (200, 100, 100, 100)})),
        False,
        needle="first-mine",
        gameplay=True,
    )
    far = {  # spot 0's only rich block is 10 tiles away (block (br 3, bc 4)); spot 1 is fine
        (2, 1): (200, 100, 100, 100),
        (3, 4): (2000, 1000, 1000, 1000),
        (2, 6): (200, 100, 100, 100),
        (2, 7): (2000, 1000, 1000, 1000),
    }
    expect(
        "AI rule: rich ore block at 10 tiles",
        ai(synth_ai_map(ore=far)),
        False,
        needle="rich ore block",
        gameplay=True,
    )
    expect(
        "AI rule: fewer than 2 full-channel blocks within 8 tiles",
        ai(synth_ai_map(ore={(2, 1): (200, 100, 100, 100), (2, 6): (200, 100, 100, 100)})),
        False,
        needle="full-channel ore blocks",
        gameplay=True,
    )
    expect(
        "AI rule: first-mine record too poor (4a+2b+2c+d < 500)",
        ai(synth_ai_map(ore={(2, 1): (200, 100, 100, 100), (2, 6): (50, 40, 40, 30)})),
        False,
        needle="first-mine",
        gameplay=True,
    )
    # sealed base pocket (DESIGN sec. 14): a wall ring round spot 0 whose only gap (row 9, cols 5-6) lies under the first-mine ore block
    ring = synth_ai_map()
    for c in range(1, 13):
        for r in range(9, 21):
            if (c in (1, 12) or r in (9, 20)) and not (r == 9 and c in (5, 6)):
                ring.flags[c * 32 + r] = 2 | 0x8000
                ring.passable[c * 32 + r] = 0
    strategic.finalize_mpm(ring)
    expect(
        "AI rule: base sealed by a wall ring + ore plug",
        ai(ring),
        False,
        needle="build room",
        gameplay=True,
    )
    expect(
        "AI rule: two spots closer than width/6",
        ai(synth_ai_map(spots=((6, 14), (10, 14)))),
        False,
        needle="tiles apart",
        gameplay=True,
    )
    expect(
        "AI rule: spot footprints and clear radii overlap",
        ai(synth_ai_map(spots=((6, 14), (14, 14)))),
        False,
        needle="overlap",
        gameplay=True,
    )
    expect(
        "AI rule: description over 79 bytes",
        ai(synth_ai_map(desc=b"x" * 80)),
        False,
        needle="loader's buffer",
        gameplay=True,
    )
    slot = synth_ai_map()
    slot.objects[0]["slot"] = 10000
    slot.building[20 * 32 + 28] = 10000
    strategic.finalize_mpm(slot)
    expect(
        "AI rule: tree slot above 9999", ai(slot), False, needle="outside 1..9999", gameplay=True
    )
    st = synth_ai_map()
    st.tail_raw = struct.pack("<3i", 6, 14, 0) + st.tail_raw[12:]
    strategic.finalize_mpm(st)
    expect("AI rule: landing status != 1", ai(st), False, needle="status", gameplay=True)
    big_id = synth_ai_map()
    big_id.flags[0] = 0x1001 | 0x8000
    strategic.finalize_mpm(big_id)
    expect("AI rule: tile id >= 0x1000", ai(big_id), False, needle="tile id >=", gameplay=True)
    cut_ai = synth_ai_map(wall_rows=32)  # wall spans the full height: the spots are split
    expect("AI map: split spots are red", ai(cut_ai), False, needle="not connected", gameplay=True)
    expect(
        "AI map: split spots are green with --islands",
        ai(cut_ai),
        True,
        gameplay=True,
        islands=True,
    )

    ok = all(results)
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


# ----------------------------------------------------------------------------------- sweep
def mapgen_cmd(
    exe: str,
    rules_dir: str,
    tileset: str,
    size: int,
    seed: int,
    out: str,
    wrap: bool,
    players: int | None = None,
    paths: bool = True,
):
    """The ONE place that knows the mapgen command line; adjust here when the CLI settles."""
    cmd = [
        exe,
        "gen",
        "--rules", rules_dir,
        "--tileset", tileset,
        "--size", str(size),
        "--players", str(players or SWEEP_PLAYERS[size]),
        "--seed", str(seed),
        "--out", out,
    ]  # fmt: skip
    if not wrap:
        cmd.append("--no-wrap")  # the generator wraps (seam pairs legal) by default
    cmd.append("--ensure-paths" if paths else "--no-paths")
    return cmd


def sweep(args, rules: Rules) -> int:
    exe = args.mapgen
    if not exe or not os.path.isfile(exe):
        print(
            f"mapgen.exe not found: {exe!r} (build src/mh_dll/mh_mapgen_cli first)", file=sys.stderr
        )
        return 2
    if not args.rules or not os.path.isdir(args.rules):
        print(f"--rules directory not found: {args.rules!r}", file=sys.stderr)
        return 2
    tilesets = sorted(ts for ts in rules.maps)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    fails = total = 0
    for ts in tilesets:
        for size in SIZES:
            for seed in SWEEP_SEEDS:
                total += 1
                stem = f"gen_{ts.split('.')[0]}_{size}_{seed}"
                path = out_dir / f"{stem}.mpm"
                digests = []
                for rerun in range(2):
                    p = out_dir / f"{stem}{'_b' if rerun else ''}.mpm"
                    if p.exists():
                        p.unlink()
                    res = subprocess.run(
                        mapgen_cmd(
                            exe,
                            args.rules,
                            ts,
                            size,
                            seed,
                            str(p),
                            args.wrap,
                            paths=not args.islands,
                        ),
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    if res.returncode != 0 or not p.is_file():
                        digests.append(None)
                        print(
                            f"FAIL  {stem}: mapgen exit {res.returncode}\n    {res.stderr.strip()[:300]}"
                        )
                        break
                    digests.append(hashlib.sha256(p.read_bytes()).hexdigest())
                if len(digests) < 2 or None in digests:
                    fails += 1
                    continue
                if digests[0] != digests[1]:
                    fails += 1
                    print(
                        f"FAIL  {stem}: nondeterministic ({digests[0][:12]} vs {digests[1][:12]})"
                    )
                    continue
                rep = check_file(str(path), rules, args.wrap, islands=args.islands)
                print_report(rep, verbose=False)
                fails += not rep.ok
    print(f"sweep: {total - fails}/{total} ok")
    return 1 if fails else 0


# ----------------------------------------------------------------------------------- lane maps
# Every map the UI row `mapgen_seq` (tools/uiscripts/registry.yaml) plays, from fixed seeds:
#   python tools/mapgen_check.py --lane-maps [--mapgen EXE] [--rules DIR] [--out DIR] (default lane fixtures: input:mapgen/lane)
# (name, tileset, size, players, seed); `_1` = 4 players (solo smoke), `_2` = 8 players (AI soak).
# Names stay <= 24 chars. A generator change moves the maps: re-run this, then the row.
LANE_MAPS = [
    ("gen_diuna256_128_1", "diuna256.tlo", 128, 4, 1),
    ("gen_diuna256_128_2", "diuna256.tlo", 128, 8, 2),
    ("gen_skaly256_128_1", "skaly256.tlo", 128, 4, 1),
    ("gen_skaly256_128_2", "skaly256.tlo", 128, 8, 2),
    ("gen_jungle_128_1", "jungle.tlo", 128, 4, 1),
    ("gen_jungle_128_2", "jungle.tlo", 128, 8, 2),
    ("gen_diuna256_256_1", "diuna256.tlo", 256, 4, 1),
    ("gen_diuna256_256_2", "diuna256.tlo", 256, 8, 2),
    ("gen_skaly256_256_2", "skaly256.tlo", 256, 8, 2),
    ("gen_jungle_256_2", "jungle.tlo", 256, 8, 2),
    ("gen_metal256_256_2", "metal256.tlo", 256, 8, 2),
]


def lane_maps(args) -> int:
    import subprocess

    exe = args.mapgen or str(REPO / "src/mh_dll/mh_mapgen_cli/Release/mapgen.exe")
    rules = args.rules or str(tool_paths.input_path("mapgen/rules", repo=REPO))
    out = Path(
        args.out
        if args.out != str(REPO / "tmp" / "mapgen" / "sweep")
        else tool_paths.input_path("mapgen/lane", repo=REPO)
    )
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    for name, tls, size, players, seed in LANE_MAPS:
        dst = out / f"{name}.mpm"
        r = subprocess.run(
            mapgen_cmd(exe, rules, tls, size, seed, str(dst), True, players),
            capture_output=True,
            check=False,
            text=True,
        )
        print(f"{name}: {'ok' if r.returncode == 0 else 'FAIL'}")
        if r.returncode:
            print(r.stdout + r.stderr)
            bad += 1
    return 1 if bad else 0


# ----------------------------------------------------------------------------------- decals (report only)
RARE_PCT = 1  # a ground tile is "rare" when it is under 1% of the passable cells of its pure corner class (stock-wide)


def _tile_classes(rules_dir: str, tileset: str) -> dict[int, int]:
    """tile id -> pure corner class, from the `c` lines of the C++ learner's rules file. REPORT ONLY: the checks above never read it."""
    stem = tileset.rsplit(".", 1)[0].upper()
    out: dict[int, int] = {}
    with open(os.path.join(rules_dir, stem + ".mgr")) as fh:
        for ln in fh:
            f = ln.split()
            if f and f[0] == "c":
                cs = {int(x) for x in f[2:6]}
                if len(cs) == 1 and min(cs) >= 0:
                    out[int(f[1])] = cs.pop()
    return out


def _decal_cells(m, cls_of: dict[int, int]):
    """[(ids, pass, class_per_cell)] helper: returns (n, ids, pas, cls) with row-major r*n + c."""
    n = m.width
    ids = [0] * (n * n)
    pas = [0] * (n * n)
    for c in range(n):
        for r in range(n):
            ids[r * n + c] = m.flags[c * n + r] & GROUND_MASK
            pas[r * n + c] = 1 if m.passable[c * n + r] else 0
    cls = [cls_of.get(t, -1) if pas[i] else -1 for i, t in enumerate(ids)]
    return n, ids, pas, cls


def decal_report(args, files) -> int:
    """Rare-ground share and rare-group sizes per ground class, for generated maps vs the stock maps (design sec. 12 item 7)."""
    if not args.rules:
        print("--decals needs --rules DIR (the rules directory of `mapgen learn`)", file=sys.stderr)
        return 2
    stock = stock_files(args.stock_dir, args.maps_dir)
    maps = [(f, strategic.parse(f)) for f in stock]
    cls_cache: dict[str, dict[int, int]] = {}

    def classes(ts: str):
        if ts not in cls_cache:
            cls_cache[ts] = _tile_classes(args.rules, ts)
        return cls_cache[ts]

    ref: dict[tuple[str, int], Counter] = defaultdict(Counter)
    cells = {}
    for f, m in maps:
        ts = tileset_of(m)
        n, ids, pas, cls = _decal_cells(m, classes(ts))
        cells[f] = (n, ids, pas, cls)
        for i, k in enumerate(cls):
            if k >= 0:
                ref[(ts, k)][ids[i]] += 1
    rare_sets = {
        key: {t for t, c in cnt.items() if c * 100 < RARE_PCT * sum(cnt.values())}
        for key, cnt in ref.items()
    }

    def measure(m, n, ids, cls, spots):
        ts = tileset_of(m)
        out = []
        near = [0] * (n * n)
        for sx, sy, _ in spots:
            for dy in range(-10, 11):
                for dx in range(-10, 11):
                    near[((sy + dy) % n) * n + (sx + dx) % n] = 1
        for k in (0, 1):
            rare_t = rare_sets.get((ts, k), set())
            area = [i for i in range(n * n) if cls[i] == k]
            if len(area) < 200:
                continue
            rare = [i for i in area if ids[i] in rare_t]
            rset = set(rare)
            seen: set[int] = set()
            sizes = []
            for i in rare:
                if i in seen:
                    continue
                seen.add(i)
                st = [i]
                sz = 0
                while st:
                    j = st.pop()
                    sz += 1
                    r, c = divmod(j, n)
                    for o in (
                        ((r + 1) % n) * n + c,
                        ((r - 1) % n) * n + c,
                        r * n + (c + 1) % n,
                        r * n + (c - 1) % n,
                    ):
                        if o in rset and o not in seen:
                            seen.add(o)
                            st.append(o)
                sizes.append(sz)
            a10 = [i for i in area if near[i]]
            r10 = sum(1 for i in a10 if ids[i] in rare_t)
            out.append(
                (
                    k,
                    len(area),
                    len(rare) / len(area),
                    1000 * len(sizes) / len(area),
                    sum(1 for z in sizes if z >= 4) / len(sizes) if sizes else 0.0,
                    r10 / len(a10) if a10 else 0.0,
                )
            )
        return out

    def show(label, ts, size, rows):
        print(
            f"{label:34} {ts[:6]:6} {size:3} "
            + " | ".join(
                f"c{k} n{na:6} rare {rs:5.3f} groups/1k {g:5.1f} 4+ {q4:4.2f} r10rare {rr:5.3f}"
                for k, na, rs, g, q4, rr in rows
            )
        )

    agg: dict[tuple[str, int, int], list] = defaultdict(list)
    print("stock:")
    for f, m in maps:
        n, ids, pas, cls = cells[f]
        rows = measure(m, n, ids, cls, _landing(m))
        show(os.path.basename(f)[:34], tileset_of(m), n, rows)
        for row in rows:
            agg[("stock", n, row[0])].append((tileset_of(m), row))
    gen_agg: dict[tuple[str, int, int], list] = defaultdict(list)
    if files:
        print("maps:")
    for f in files:
        m = strategic.parse(f)
        ts = tileset_of(m)
        n, ids, pas, cls = _decal_cells(m, classes(ts))
        rows = measure(m, n, ids, cls, _landing(m))
        show(os.path.basename(f)[:34], ts, n, rows)
        for row in rows:
            gen_agg[(ts, n, row[0])].append(row)
    st_agg: dict[tuple[str, int, int], list] = defaultdict(list)
    for (_, n, k), lst in agg.items():
        for ts, row in lst:
            st_agg[(ts, n, k)].append(row)
    print(
        "mean per (tileset, size, class)  [rare share, groups/1k, 4+ group share, rare share r10]:  stock vs maps"
    )
    for key in sorted(set(st_agg) | set(gen_agg)):

        def mean(lst):
            return tuple(sum(r[i] for r in lst) / len(lst) for i in (2, 3, 4, 5)) if lst else None

        a, b = mean(st_agg.get(key, [])), mean(gen_agg.get(key, []))
        fmt = lambda t: (
            "-"
            if t is None
            else f"rare {t[0]:5.3f} g/1k {t[1]:5.1f} 4+ {t[2]:4.2f} r10 {t[3]:5.3f}"
        )
        print(
            f"{key[0][:6]:6} {key[1]:3} c{key[2]}  stock {fmt(a):44} | maps(n={len(gen_agg.get(key, []))}) {fmt(b)}"
        )
    return 0


# ----------------------------------------------------------------------------------- sheet
ORE_COLORS = ((255, 140, 0), (0, 220, 255), (80, 255, 80), (255, 60, 220))  # channels 0..3
ORE_REF = 1500  # block total (c0+c1+c2+c3) that draws the largest marker


def _draw_ore(d, m, block_px: float) -> None:
    """Ore plane over a rendered map: one marker per 4x4-tile block with ore, size ~ sqrt(total)
    (2..half a block), colour = dominant of the 4 channels; a white outline marks AI
    first-mine-capable blocks (all four channels > 0 and 4a+2b+2c+d >= 500)."""
    for br, bc, c in ore_blocks(m):
        ch = c[:4]
        tot = sum(ch)
        if not tot:
            continue
        cx, cy = (bc + 0.5) * block_px * 4, (br + 0.5) * block_px * 4
        hi = max(4.5, 0.5 * block_px * 4)  # a block is block_px*4 cell px wide
        half = 2 + (hi - 2) * min(1.0, (tot / ORE_REF) ** 0.5)
        col = ORE_COLORS[max(range(4), key=lambda i: ch[i])]
        d.rectangle((cx - half, cy - half, cx + half, cy + half), fill=col, outline=(0, 0, 0))
        if min(ch) > 0 and 4 * ch[0] + 2 * ch[1] + 2 * ch[2] + ch[3] >= 500:
            h2 = half + 2
            d.rectangle((cx - h2, cy - h2, cx + h2, cy + h2), outline=(255, 255, 255), width=2)


def _draw_legend(d, y: int) -> None:
    x = 6
    for i, col in enumerate(ORE_COLORS):
        d.rectangle((x, y + 5, x + 10, y + 15), fill=col)
        d.text((x + 14, y + 5), f"ore ch{i}", fill=(230, 230, 230))
        x += 70
    d.text(
        (x + 6, y + 5),
        "size ~ block total | white box = AI first-mine block | yellow ring = landing spot",
        fill=(230, 230, 230),
    )


def sheet(args, files) -> int:
    from PIL import Image, ImageDraw

    from src.formats.mapkit import tlo
    from src.formats.wiki.art import planetmaps

    stock = Path(args.stock_dir)
    tlos = {p.name.lower(): p for p in stock.glob("*.TLO")}
    cell = 384
    thumbs_cache = {}
    tiles = []
    for f in files:
        m = strategic.parse(f)
        tname = m.tlo_name.split(b"\0")[0].decode("ascii").lower()
        caption = (
            f"{os.path.basename(f)}  {m.width}x{m.height}  {tname}  {m.f2} trees  {m.f3} starts"
        )
        if tname not in tlos:
            img = Image.new("RGB", (cell, cell), (60, 20, 20))
            ImageDraw.Draw(img).text((8, 8), f"no tileset {tname}", fill=(255, 255, 255))
        else:
            px = 8 if m.width <= planetmaps.SMALL_MAP else planetmaps.PX
            if (tname, px) not in thumbs_cache:
                thumbs_cache[tname, px] = planetmaps.tile_thumbs(
                    tlo.parse(str(tlos[tname]), "biome"), px
                )
            img = planetmaps.render_ground(m, thumbs_cache[tname, px], px)
            img = img.resize((cell, cell), Image.LANCZOS)
            scale = cell / (m.width * px)  # full-res pixel -> cell pixel
            d = ImageDraw.Draw(img)
            _draw_ore(d, m, px * scale)
            for x, y in planetmaps.markers(m)["land"]:
                cx, cy = (x + 0.5) * px * scale, (y + 0.5) * px * scale
                d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), outline=(255, 255, 0), width=2)
        tiles.append((caption, img))
    cols = min(3, len(tiles)) or 1
    rows = -(-len(tiles) // cols)
    cap_h = 20
    leg_h = 22
    sheet_img = Image.new("RGB", (cols * cell, rows * (cell + cap_h) + leg_h), (24, 24, 24))
    d = ImageDraw.Draw(sheet_img)
    _draw_legend(d, rows * (cell + cap_h))
    for i, (cap, img) in enumerate(tiles):
        x, y = (i % cols) * cell, (i // cols) * (cell + cap_h)
        sheet_img.paste(img, (x, y + cap_h))
        d.text((x + 4, y + 4), cap, fill=(230, 230, 230))
    Path(args.sheet).parent.mkdir(parents=True, exist_ok=True)
    sheet_img.save(args.sheet)
    print(f"sheet: {len(tiles)} maps -> {args.sheet}")
    return 0


# ----------------------------------------------------------------------------------- main
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cmd_or_maps", nargs="*", help="`check` followed by maps, or maps for --sheet")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--sweep", action="store_true")
    ap.add_argument("--sheet", metavar="OUT.png")
    ap.add_argument(
        "--decals",
        action="store_true",
        help="report only: rare-ground share and rare-group sizes per ground class of MAP... vs stock (needs --rules)",
    )
    ap.add_argument(
        "--lane-maps", action="store_true", help="regenerate the maps of UI row mapgen_seq"
    )
    ap.add_argument("--mapgen", help="path to mapgen.exe (--sweep)")
    ap.add_argument("--rules", help="mapgen rules directory (--sweep)")
    ap.add_argument("--out", default=str(REPO / "tmp" / "mapgen" / "sweep"))
    ap.add_argument("--wrap", action="store_true", help="also check the torus seam pairs")
    ap.add_argument(
        "--islands",
        action="store_true",
        help="landing spots need not be connected (maps made with --no-paths; --sweep then passes --no-paths)",
    )
    ap.add_argument(
        "--stock-style",
        action="store_true",
        help="ore blocks on a mother footprint are a note, not an error (34%% of stock spots do it; generator output must not)",
    )
    ap.add_argument(
        "--no-gameplay",
        action="store_true",
        help="skip the AI-facing rules (clearing, footprint, first mine, ore caps, slots, description)",
    )
    ap.add_argument("--stock-dir", default=None, help="stock .MP/.TLO directory")
    ap.add_argument(
        "--maps-dir", default=None, help="stock .mpm directory (default <POLYGON>/Maps)"
    )
    ap.add_argument(
        "--bfs-wrap",
        action="store_true",
        help="(c) BFS over the torus (default no wrap; half the stock maps need wrap)",
    )
    ap.add_argument(
        "--no-envelope", action="store_true", help="skip the size/players/trees envelope"
    )
    ap.add_argument(
        "--min-spot-dist",
        type=int,
        default=0,
        metavar="TILES",
        help="minimum torus Chebyshev distance between landing spots (default width/6)",
    )
    args = ap.parse_intermixed_args(argv)
    global MIN_SPOT_DIST_OVERRIDE
    MIN_SPOT_DIST_OVERRIDE = args.min_spot_dist
    global STOCK_STYLE_ORE
    STOCK_STYLE_ORE = args.stock_style
    args.stock_dir = args.stock_dir or default_stock_dir()
    args.maps_dir = args.maps_dir or default_maps_dir()

    if args.selftest:
        return selftest()
    if args.sweep:
        if not args.mapgen or not os.path.isfile(args.mapgen):
            print(f"mapgen.exe not found: {args.mapgen!r}", file=sys.stderr)
            return 2
        return sweep(args, learn_stock(stock_files(args.stock_dir, args.maps_dir)))
    if args.lane_maps:
        return lane_maps(args)
    if args.decals:
        return decal_report(args, args.cmd_or_maps)
    if args.sheet:
        return sheet(args, args.cmd_or_maps)
    if args.cmd_or_maps and args.cmd_or_maps[0] == "check":
        files = args.cmd_or_maps[1:]
        if not files:
            ap.error("check needs at least one map")
        rules = learn_stock(stock_files(args.stock_dir, args.maps_dir))
        print(f"learned {sum(rules.maps.values())} stock maps over {len(rules.maps)} tilesets")
        bad = 0
        for f in files:
            rep = check_file(
                f,
                rules,
                args.wrap,
                envelope=not args.no_envelope,
                bfs_wrap=args.bfs_wrap,
                gameplay=not args.no_gameplay,
                islands=args.islands,
            )
            print_report(rep)
            bad += not rep.ok
        return 1 if bad else 0
    ap.error("give `check MAP...`, --selftest, --sweep or --sheet")
    return 2


if __name__ == "__main__":
    sys.exit(main())
