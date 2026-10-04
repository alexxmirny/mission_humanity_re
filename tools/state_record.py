#!/usr/bin/env python3
"""state_record.py -- mp:D42: reader/decoder for the state-recording file format defined by
docs/state-record.md (version 1) -- mh_match_state.bin (whole-match, mp:D40's `net-debug`) and
mh_desync_state.bin (the ship desync ring, mp:D41). THE SPEC FILE IS AUTHORITATIVE; this docstring
restates the parts that matter for using the tool, not the format itself.

WHAT THIS IS FOR. A per-step raw-byte recording of the hash-manifest regions (mh::state::
HASH_REGIONS) is only useful if something can (a) tell you what is in a file without a live game,
(b) rebuild the exact byte state at an arbitrary step, and (c) compare two recordings (two peers, or
a recording vs a replay) and name the FIRST byte that disagrees -- region, offset, and, where a
struct layout is known, the record index and field. That is what `info` / `at` / `diff` do below.
`tools/replay_match_segment.py` is meant to call the module API (`load`/`state_at`/`first_diff`)
directly once the state-file carriage lands there, rather than shelling out to this CLI.

FIELD NAMING. Struct layouts for the record-shaped regions come from the generated header
`src/mh_dll/mh/addr/mh_structs.gen.h` (parsed on demand, cached; see `_get_structs()` below) --
NOT hand-kept here, so a struct edit there is picked up automatically. The generated header lays
fields out BYTE-EXACT (explicit `_pad_0x...` filler fields cover every real gap -- confirmed against
several `// <name> (size 0xNN, ...)` comments), so summing field sizes in declaration order gives the
true offset with no compiler-alignment guessing. `REGION_LAYOUTS` below maps a subset of
`mh::state::HASH_REGIONS` region names (the big fixed-record ones named in mp:D42's scope: buildings,
units, projectile_pool, turrets, mines, soldiers, planets, the player_data slices, tile_objects, plus
a handful of other clean record regions this pass could verify cheaply) to the matching struct type.
A region NOT in that table, or whose struct could not be parsed reliably (an unrecognised field syntax,
or a size that disagrees with the header's own `(size 0xNN` comment), is reported as a bare
region+offset -- this tool never guesses a field name it cannot derive.

Player_data (`p{0..7}_ai_gates` / `_local` / `_ai_econ`) is a special case: those three regions are
WINDOWS into one `mh_game_player_data` record per player (see docs/state-record.md and
mh_regions.gen.h's HASH_REGIONS comment for the byte ranges), not independent record arrays -- handled
by `_PLAYER_DATA_WINDOW_RE` below rather than a `REGION_LAYOUTS` row per player.

THE COMMITTED VERDICT MASKS (mp:D42), used by `diff` BY DEFAULT. `tools/data/state_masks.json` is
GENERATED, not hand-kept -- `net_selftest.exe inchashtest --dump-masks <file>` samples
`mh::state::inc::keep_byte` (src/mh_dll/libmh/state/inc_state.h) for every byte of every region and
asserts the result reproduces it exactly, so this file and the C++ VERDICT mask are ONE decision, not
two hand-kept copies of it (the trap the old byte-range scheme below was built to avoid but could
not, since nothing generated it). `--check-masks <file>` (wired into `tools/run_selftests.py`) fails
the gate if a `keep_byte` edit was not followed by a regenerate. Schema:

    {
      "schema_version": 1, "hash_kind": 2, "region_count": <N>,
      "regions": {
        "<region_name>": {"excluded": <bool>, "len": <int>, "stride": <int>, "keep": [<0-255>, ...]},
        ...
        "order_queue": {"excluded": false, "len": <int>,
                         "dynamic": {"count_region": "order_queue_count", "record_size": <int>, "cap": <int>}}
      }
    }

For a STATIC region, byte `off` (region-relative) is masked with `keep[off % stride]` -- a partial
byte mask (e.g. tile_objects' fog bits) ANDs both sides before they are compared, not just excluded.
`excluded: true` (the peer-local, in-flight state -- `ls_horizon`, `peer_horizon`, `order_pending`,
`frame_ring`, the pacing clocks, ...) means the WHOLE region is skipped: it is not part of the
state-only verdict this tool reproduces, the same way `mh::state::inc::fold_state` skips it. The one
DYNAMIC region, `order_queue`, has no fixed pattern -- bytes at or past the LIVE prefix
(`order_queue_live_bytes(order_queue_count, record_size, cap)`, read from `order_queue_count`'s OWN
value at the same step, independently on each side, exactly as `mh::orders::emit_region` does) are
masked; `diff` reads that count from each side's own rebuilt state, per step.

`load_state_masks(path=None)` loads this file (default `tools/data/state_masks.json`); `first_diff`
auto-loads it (`state_masks=True`, the default) unless `masks` (below) or `state_masks=False`
(`diff --raw`) is given.

THE OLD BYTE-RANGE SCHEME (`diff --masks FILE`, and the `masks` argument of `first_diff()`) still
works, for a mask you want to build by hand rather than derive from `keep_byte` -- it REPLACES the
committed masks above rather than combining with them:

    {
      "<region_name>": [[offset, len], [offset, len], ...],
      ...
    }

Each `[offset, len]` pair excludes that byte range of THAT region's own raw bytes (offsets are
region-relative, matching the region table the file's own header carries) from the comparison. A
region absent from the masks object is compared in full.

USAGE:
    python tools/state_record.py info <file>
    python tools/state_record.py at <file> <step> [--region NAME] [--out DIR]
    python tools/state_record.py diff <a> <b> [--masks masks.json | --raw]
    python tools/state_record.py --selftest
"""

from __future__ import annotations

import argparse
import bisect
import gzip
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STRUCTS_HEADER = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_structs.gen.h")
# mp:D42: the committed VERDICT mask table, generated by `net_selftest.exe inchashtest
# --dump-masks` from src/mh_dll/libmh/state/inc_state.h's keep_byte -- see the module docstring.
STATE_MASKS_PATH = os.path.join(REPO, "tools", "data", "state_masks.json")

MAGIC = 0x5253484D  # "MHSR"
SUPPORTED_VERSIONS = (1,)
FLAG_RING = 1 << 0

TAG_KEYF = 0x4659454B  # "KEYF"
TAG_STEP = 0x50455453  # "STEP"
TAG_END = 0x20444E45  # "END "

HEADER_FIXED_FMT = (
    "<IHHIQII"  # magic,version,flags,header_len,manifest_fp,keyframe_every,region_count
)
HEADER_FIXED_SIZE = struct.calcsize(HEADER_FIXED_FMT)
CHUNK_HDR_FMT = "<III"  # tag, payload_len, crc32
CHUNK_HDR_SIZE = struct.calcsize(CHUNK_HDR_FMT)
RUN_HDR_FMT = "<HIH"  # region, offset, len
RUN_HDR_SIZE = struct.calcsize(RUN_HDR_FMT)


# ==== data types =====================================================================================


class RegionDef:
    __slots__ = ("name", "len")

    def __init__(self, name, length):
        self.name = name
        self.len = length


class Header:
    __slots__ = (
        "version",
        "flags",
        "header_len",
        "manifest_fp",
        "keyframe_every",
        "region_count",
        "regions",
    )

    def __init__(
        self, version, flags, header_len, manifest_fp, keyframe_every, region_count, regions
    ):
        self.version = version
        self.flags = flags
        self.header_len = header_len
        self.manifest_fp = manifest_fp
        self.keyframe_every = keyframe_every
        self.region_count = region_count
        self.regions = regions

    @property
    def is_ring(self):
        return bool(self.flags & FLAG_RING)


class KeyframeChunk:
    __slots__ = ("step", "state")

    def __init__(self, step, state):
        self.step = step
        self.state = state  # bytes, concatenation of every region in table order


class StepChunk:
    __slots__ = ("step", "runs")

    def __init__(self, step, runs):
        self.step = step
        self.runs = runs  # list of (region_idx, offset, len, bytes)


class EndChunk:
    __slots__ = ("last_step", "steps_recorded", "raw_bytes_written")

    def __init__(self, last_step, steps_recorded, raw_bytes_written):
        self.last_step = last_step
        self.steps_recorded = steps_recorded
        self.raw_bytes_written = raw_bytes_written


class Recording:
    """A decoded state-record file. Build with `load(path)`, never directly."""

    def __init__(self, path, header):
        self.path = path
        self.header = header
        self.regions = header.regions
        self.region_index = {r.name: i for i, r in enumerate(header.regions)}
        self.region_offset = []
        off = 0
        for r in header.regions:
            self.region_offset.append(off)
            off += r.len
        self.total_len = off
        self.chunks = []
        self.keyframe_index = {}  # step -> index into self.chunks
        self.keyframe_steps = []  # sorted
        self.step_chunks = {}  # step -> StepChunk
        self.end = None
        self.first_step = None
        self.last_step = None
        self.stop_reason = None  # None (clean EOF) | "truncated" | "crc_mismatch" | "malformed"
        self.stop_detail = None


# ==== decoding ========================================================================================


def _read_header(f):
    data = f.read(HEADER_FIXED_SIZE)
    if len(data) < HEADER_FIXED_SIZE:
        raise ValueError("file too short for the fixed header (%d bytes)" % HEADER_FIXED_SIZE)
    magic, version, flags, header_len, manifest_fp, keyframe_every, region_count = struct.unpack(
        HEADER_FIXED_FMT, data
    )
    if magic != MAGIC:
        raise ValueError(
            "bad magic 0x%08x (want 0x%08x 'MHSR') -- not a state-record file" % (magic, MAGIC)
        )
    if version not in SUPPORTED_VERSIONS:
        raise ValueError(
            "unsupported state-record version %d (this reader knows: %s) -- refused by name, "
            "per docs/state-record.md" % (version, ", ".join(str(v) for v in SUPPORTED_VERSIONS))
        )
    regions = []
    for _ in range(region_count):
        head = f.read(5)
        if len(head) < 5:
            raise ValueError("file too short for the region table (region %d)" % len(regions))
        rlen, name_len = struct.unpack("<IB", head)
        name = f.read(name_len)
        if len(name) < name_len:
            raise ValueError("file too short for the region table (region %d name)" % len(regions))
        regions.append(RegionDef(name.decode("utf-8"), rlen))
    return Header(version, flags, header_len, manifest_fp, keyframe_every, region_count, regions)


def _parse_keyf(payload, total_len):
    if len(payload) < 4:
        return None
    (step,) = struct.unpack_from("<I", payload, 0)
    state = payload[4:]
    if len(state) != total_len:
        return None
    return step, state


def _parse_step(payload, regions):
    if len(payload) < 8:
        return None
    step, run_count = struct.unpack_from("<II", payload, 0)
    pos = 8
    runs = []
    for _ in range(run_count):
        if pos + RUN_HDR_SIZE > len(payload):
            return None
        ridx, roff, rlen = struct.unpack_from(RUN_HDR_FMT, payload, pos)
        pos += RUN_HDR_SIZE
        if ridx >= len(regions):
            return None
        if roff + rlen > regions[ridx].len:
            return None  # "runs never cross a region"
        if pos + rlen > len(payload):
            return None
        data = payload[pos : pos + rlen]
        pos += rlen
        runs.append((ridx, roff, rlen, data))
    if pos != len(payload):
        return None
    return step, runs


def _parse_end(payload):
    if len(payload) != 16:
        return None
    return struct.unpack("<IIQ", payload)


GZ_SUFFIX = ".gz"


def resolve_path(path):
    """mp:D46: the file to read for `path`. mh.dll gzips `mh_match_state.bin` after the match and then
    deletes the raw file, so a caller that names the raw path (`<dir>/mh_match_state.bin`) gets the
    compressed sibling when only that exists. A path that exists is returned as it is (the raw file wins
    when BOTH exist -- a kill between the rename and the delete leaves two complete copies); a `.gz` path is
    always read as gzip."""
    if os.path.exists(path):
        return path
    if not path.endswith(GZ_SUFFIX) and os.path.exists(path + GZ_SUFFIX):
        return path + GZ_SUFFIX
    return path


def open_recording(path):
    """Open a state-record file for reading: gzip (by the 1f 8b magic) or raw."""
    f = open(resolve_path(path), "rb")
    magic = f.read(2)
    f.seek(0)
    if magic == b"\x1f\x8b":
        return gzip.GzipFile(fileobj=f, mode="rb")
    return f


def load(path):
    """Decode a state-record file, raw or gzip (docs/state-record.md "The compressed file"). Stops
    cleanly at the first incomplete chunk or CRC mismatch (`rec.stop_reason` names why;
    `rec.last_step` is the last step actually decoded) -- neither is an exception unless the header
    itself cannot be read or nothing decodable follows it. A gzip stream that ends early reads as
    "truncated" at the point it ended."""
    with open_recording(path) as f:
        header = _read_header(f)
        f.seek(header.header_len)
        rec = Recording(path, header)
        try:
            _load_chunks(f, rec)
        except (EOFError, OSError, zlib.error) as e:
            rec.stop_reason = "truncated"
            rec.stop_detail = "compressed stream ended or is damaged: %s" % e
        rec.keyframe_steps.sort()
        return rec


def _load_chunks(f, rec):
    while True:
        chunk_pos = f.tell()
        chdr = f.read(CHUNK_HDR_SIZE)
        if not chdr:
            break  # a clean end of file
        if len(chdr) < CHUNK_HDR_SIZE:
            rec.stop_reason = "truncated"
            rec.stop_detail = "chunk header cut at file offset %d" % chunk_pos
            break
        tag, payload_len, crc = struct.unpack(CHUNK_HDR_FMT, chdr)
        payload = f.read(payload_len)
        if len(payload) < payload_len:
            rec.stop_reason = "truncated"
            rec.stop_detail = (
                "chunk payload cut at file offset %d (tag=0x%08x, wanted %d bytes, got %d)"
                % (
                    chunk_pos,
                    tag,
                    payload_len,
                    len(payload),
                )
            )
            break
        actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if actual_crc != crc:
            rec.stop_reason = "crc_mismatch"
            rec.stop_detail = (
                "tag=0x%08x payload_len=%d at file offset %d (want crc32=0x%08x, got 0x%08x)"
                % (
                    tag,
                    payload_len,
                    chunk_pos,
                    crc,
                    actual_crc,
                )
            )
            break
        if tag == TAG_KEYF:
            parsed = _parse_keyf(payload, rec.total_len)
            if parsed is None:
                rec.stop_reason = "malformed"
                rec.stop_detail = (
                    "KEYF payload does not match the region table at file offset %d" % chunk_pos
                )
                break
            step, state = parsed
            rec.chunks.append(KeyframeChunk(step, state))
            rec.keyframe_index[step] = len(rec.chunks) - 1
            rec.keyframe_steps.append(step)
            if rec.first_step is None:
                rec.first_step = step
            rec.last_step = step
        elif tag == TAG_STEP:
            parsed = _parse_step(payload, rec.regions)
            if parsed is None:
                rec.stop_reason = "malformed"
                rec.stop_detail = (
                    "STEP payload is internally inconsistent at file offset %d" % chunk_pos
                )
                break
            step, runs = parsed
            rec.chunks.append(StepChunk(step, runs))
            rec.step_chunks[step] = rec.chunks[-1]
            if rec.first_step is None:
                rec.first_step = step
            rec.last_step = step
        elif tag == TAG_END:
            parsed = _parse_end(payload)
            if parsed is None:
                rec.stop_reason = "malformed"
                rec.stop_detail = "END payload is not 16 bytes at file offset %d" % chunk_pos
                break
            last_step, steps_recorded, raw_bytes_written = parsed
            rec.end = EndChunk(last_step, steps_recorded, raw_bytes_written)
            rec.chunks.append(rec.end)
        else:
            rec.stop_reason = "malformed"
            rec.stop_detail = "unknown chunk tag 0x%08x at file offset %d" % (tag, chunk_pos)
            break


# ==== state reconstruction ============================================================================


def state_at(rec, step):
    """Rebuild the full state at `step`: the latest KEYF <= step, with every STEP chunk after it up
    to and including `step` applied in order. Returns {region_name: bytes}."""
    if not rec.keyframe_steps:
        raise ValueError("%s: no keyframes decoded, nothing to rebuild from" % rec.path)
    if step < rec.keyframe_steps[0]:
        raise ValueError(
            "%s: step %d is before the first keyframe (%d)"
            % (rec.path, step, rec.keyframe_steps[0])
        )
    if rec.last_step is not None and step > rec.last_step:
        raise ValueError(
            "%s: step %d is past the last decoded step (%d)" % (rec.path, step, rec.last_step)
        )
    ki = bisect.bisect_right(rec.keyframe_steps, step) - 1
    kf_step = rec.keyframe_steps[ki]
    kf_idx = rec.keyframe_index[kf_step]
    buf = bytearray(rec.chunks[kf_idx].state)
    for c in rec.chunks[kf_idx + 1 :]:
        if isinstance(c, EndChunk):
            continue
        if isinstance(c, KeyframeChunk):
            if c.step > step:
                break
            buf = bytearray(c.state)  # a later keyframe at/under `step` re-bases us (defensive)
            continue
        if isinstance(c, StepChunk):
            if c.step > step:
                break
            for ridx, off, rlen, data in c.runs:
                base = rec.region_offset[ridx]
                buf[base + off : base + off + rlen] = data
    out = {}
    for i, r in enumerate(rec.regions):
        base = rec.region_offset[i]
        out[r.name] = bytes(buf[base : base + r.len])
    return out


# ==== struct-layout parsing (mh_structs.gen.h) ========================================================

PRIM_SIZES = {
    "int8_t": 1,
    "uint8_t": 1,
    "char": 1,
    "bool": 1,
    "byte": 1,
    "int16_t": 2,
    "uint16_t": 2,
    "short": 2,
    "int32_t": 4,
    "uint32_t": 4,
    "int": 4,
    "float": 4,
    "int64_t": 8,
    "uint64_t": 8,
    "double": 8,
}

_STRUCT_BLOCK_RE = re.compile(r"^struct\s+(mh_\w+)\s*\{(.*?)^[ \t]*\};", re.M | re.S)
_FIELD_RE = re.compile(
    r"^(?:struct\s+(?P<sname>mh_\w+)|(?P<ptype>[A-Za-z_]\w*))\s+(?P<fname>[A-Za-z_]\w*)"
    r"\s*(?:\[(?P<cnt>\d+)\])?\s*;$"
)
_SIZE_COMMENT_RE = re.compile(r"^//\s*(\S+)\s*\(size\s*(0x[0-9a-fA-F]+)", re.M)


class Field:
    __slots__ = ("name", "offset", "size", "kind", "elem_size", "count", "struct_ref")

    def __init__(self, name, offset, size, kind, elem_size, count, struct_ref):
        self.name = name
        self.offset = offset
        self.size = size
        self.kind = kind  # "prim" | "struct"
        self.elem_size = elem_size
        self.count = count
        self.struct_ref = struct_ref


class StructDef:
    __slots__ = ("name", "fields", "size", "reliable", "declared_size")

    def __init__(self, name, fields, size, reliable, declared_size):
        self.name = name
        self.fields = fields
        self.size = size
        self.reliable = reliable
        self.declared_size = declared_size


def _parse_structs_text(text):
    """Extract raw (unresolved -- nested struct sizes not yet known) field lists per struct, plus the
    `(size 0xNN` comments used only as a cross-check. Pure text processing, no I/O -- kept separate so
    the selftest can feed it synthetic text."""
    raw = {}
    for m in _STRUCT_BLOCK_RE.finditer(text):
        name = m.group(1)
        body = m.group(2)
        fields = []
        ok = True
        for line in body.split("\n"):
            code = line.split("//", 1)[0].strip()
            if not code:
                continue
            fm = _FIELD_RE.match(code)
            if not fm:
                ok = False
                break
            fields.append(fm.groupdict())
        raw[name] = {"fields": fields, "syntax_ok": ok}
    declared_size = {}
    for dm in _SIZE_COMMENT_RE.finditer(text):
        short, hexsize = dm.group(1), dm.group(2)
        full = "mh_" + short
        try:
            declared_size[full] = int(hexsize, 16)
        except ValueError:
            pass
    return raw, declared_size


def _resolve_struct(name, raw, declared_size, cache, in_progress):
    if name in cache:
        return cache[name]
    if name in in_progress or name not in raw:
        sd = StructDef(name, [], 0, False, declared_size.get(name))
        cache[name] = sd
        return sd
    in_progress.add(name)
    entry = raw[name]
    reliable = entry["syntax_ok"]
    fields = []
    offset = 0
    for fr in entry["fields"]:
        fname = fr["fname"]
        count = int(fr["cnt"]) if fr.get("cnt") else 1
        if fr.get("sname"):
            inner = _resolve_struct(fr["sname"], raw, declared_size, cache, in_progress)
            if not inner.reliable or inner.size == 0:
                reliable = False
            elem_size = inner.size
            kind = "struct"
            struct_ref = fr["sname"]
        else:
            ptype = fr["ptype"]
            elem_size = PRIM_SIZES.get(ptype)
            if elem_size is None:
                reliable = False
                elem_size = 1
            kind = "prim"
            struct_ref = None
        size = elem_size * count
        fields.append(Field(fname, offset, size, kind, elem_size, count, struct_ref))
        offset += size
    in_progress.discard(name)
    dsize = declared_size.get(name)
    if dsize is not None and dsize != offset:
        reliable = False  # disagrees with the generator's own recorded size -- don't trust it
    sd = StructDef(name, fields, offset, reliable, dsize)
    cache[name] = sd
    return sd


def parse_structs(text):
    """Pure function: header text -> {struct_name: StructDef}. No file I/O; `_get_structs()` is the
    cached, file-reading wrapper the rest of the tool actually calls."""
    raw, declared_size = _parse_structs_text(text)
    cache = {}
    for name in raw:
        _resolve_struct(name, raw, declared_size, cache, set())
    return cache


_STRUCTS_CACHE = None


def _get_structs():
    global _STRUCTS_CACHE
    if _STRUCTS_CACHE is not None:
        return _STRUCTS_CACHE
    if not os.path.isfile(STRUCTS_HEADER):
        raise FileNotFoundError(STRUCTS_HEADER)
    with open(STRUCTS_HEADER, encoding="utf-8") as f:
        text = f.read()
    _STRUCTS_CACHE = parse_structs(text)
    return _STRUCTS_CACHE


def _field_at(sdef, off):
    for f in sdef.fields:
        if f.offset <= off < f.offset + f.size:
            return f
    return None


def _resolve_field_path(structs, struct_name, off):
    sdef = structs.get(struct_name)
    if sdef is None or not sdef.reliable or off < 0 or off >= sdef.size:
        return None
    f = _field_at(sdef, off)
    if f is None:
        return None
    local = off - f.offset
    if f.kind == "struct":
        if f.count > 1:
            idx = local // f.elem_size
            inner_off = local % f.elem_size
            inner = _resolve_field_path(structs, f.struct_ref, inner_off)
            return "%s[%d]%s" % (f.name, idx, ("." + inner) if inner else "(+0x%x)" % inner_off)
        inner = _resolve_field_path(structs, f.struct_ref, local)
        return f.name + ("." + inner if inner else "(+0x%x)" % local)
    if f.count > 1:
        idx = local // f.elem_size
        return "%s[%d]" % (f.name, idx)
    return f.name


# ==== region -> struct layout table ===================================================================

REGION_LAYOUTS = {
    "buildings": {"struct": "mh_map_object_building"},
    "productions": {"struct": "mh_map_object_production"},
    "mines": {"struct": "mh_map_object_mine"},
    "turrets": {"struct": "mh_map_object_turret"},
    "unit_storage": {"struct": "mh_map_object_unit_storage"},
    "labs": {"struct": "mh_map_object_lab"},
    "strat_players": {"struct": "mh_llm_strat_player_profile"},
    "planets": {"struct": "mh_cfg_final_struct_Planet"},
    "prod_slots": {"struct": "mh_llm_prod_shuttle_slot"},
    "projectile_pool": {"struct": "mh_llm_strat_projectile"},
    "units": {"struct": "mh_map_object_unit"},
    "tile_objects": {"struct": "mh_map_tile_object_data"},
    "soldiers": {"struct": "mh_llm_strat_crew_soldier"},
    "order_queue": {"struct": "mh_llm_strat_order"},
    "order_staging": {"struct": "mh_llm_strat_order"},
    "order_pending_arr": {"struct": "mh_llm_strat_order"},
    "players": {"struct": "mh_llm_strat_player_desc"},
    "progress": {"struct": "mh_game_progress"},
}

# The three player_data slices are WINDOWS into one mh_game_player_data record per player, not
# independent record arrays -- see docs/state-record.md and the HASH_REGIONS comment in
# mh_regions.gen.h (each player's record is split into ai_gates / local / ai_econ slices).
_PLAYER_DATA_WINDOW_RE = re.compile(r"^p\d+_(ai_gates|local|ai_econ)$")
_PLAYER_DATA_BASE = {"ai_gates": 0, "local": 56, "ai_econ": 65596}


def _layout_for(region_name):
    if region_name in REGION_LAYOUTS:
        return REGION_LAYOUTS[region_name]
    m = _PLAYER_DATA_WINDOW_RE.match(region_name)
    if m:
        return {
            "struct": "mh_game_player_data",
            "base": _PLAYER_DATA_BASE[m.group(1)],
            "window": True,
        }
    return None


def describe_offset(region_name, offset, region_len):
    """Best-effort (region, byte offset) -> (record index, field name) using the struct layouts in
    mh_structs.gen.h. Never raises; a layout that cannot be derived reliably comes back as
    {"record": None or int, "field": None, "note": "<why>"} rather than a guess."""
    layout = _layout_for(region_name)
    if layout is None:
        return {
            "record": None,
            "field": None,
            "note": "no known struct layout for region '%s'" % region_name,
        }
    try:
        structs = _get_structs()
    except OSError as e:
        return {"record": None, "field": None, "note": "struct layout unavailable (%s)" % e}
    sname = layout["struct"]
    sdef = structs.get(sname)
    if sdef is None:
        return {
            "record": None,
            "field": None,
            "note": "struct '%s' not found in mh_structs.gen.h" % sname,
        }
    if not sdef.reliable:
        return {
            "record": None,
            "field": None,
            "note": "struct '%s' could not be reliably parsed (best-effort parser gave up)" % sname,
        }
    is_window = bool(layout.get("window"))
    stride = region_len if is_window else sdef.size
    if stride <= 0:
        return {"record": None, "field": None, "note": "degenerate stride for '%s'" % sname}
    record = offset // stride
    sub = offset % stride
    struct_off = layout.get("base", 0) + sub
    path = _resolve_field_path(structs, sname, struct_off)
    if path is None:
        return {
            "record": None if is_window else record,
            "field": None,
            "note": "record +0x%x falls outside the parsed layout of %s (size 0x%x)"
            % (struct_off, sname, sdef.size),
        }
    return {"record": None if is_window else record, "field": path, "note": None}


# ==== mp:D42 committed masks ===========================================================================


def load_state_masks(path=None):
    """The committed VERDICT mask table (default `tools/data/state_masks.json`; see the module
    docstring) -> {region_name: entry}. Raises OSError if the file is missing, ValueError/KeyError
    if it does not parse as this schema -- a caller that wants "no committed masking" passes
    `state_masks=False` to `first_diff` instead of catching this."""
    with open(path or STATE_MASKS_PATH, encoding="utf-8") as f:
        doc = json.load(f)
    return doc["regions"]


# ==== diff =============================================================================================


def _check_compatible_regions(a, b):
    ra = [(r.name, r.len) for r in a.regions]
    rb = [(r.name, r.len) for r in b.regions]
    if ra != rb:
        raise ValueError(
            "%s and %s have different region tables (%d vs %d regions) -- not comparable "
            "(different build/manifest)" % (a.path, b.path, len(ra), len(rb))
        )


def _is_masked(off, mask_ranges):
    for mo, ml in mask_ranges:
        if mo <= off < mo + ml:
            return True
    return False


def _order_queue_live_bytes(count, record_size, cap, region_len):
    """Mirrors mh::state::inc::order_queue_live_bytes (inc_state.h) and emit_order_queue
    (region_view.h): the live byte-prefix of order_queue a given order_queue_count value keeps."""
    if count is None or count < 0:
        count = 0
    if count > cap:
        count = cap
    head = count * record_size
    return head if head <= region_len else region_len


def _region_int32(state, name):
    """The live value of a 4-byte little-endian region (e.g. order_queue_count) in a rebuilt
    state dict (name -> bytes/bytearray), or 0 if the region is missing/short."""
    b = state.get(name)
    if not b or len(b) < 4:
        return 0
    return struct.unpack("<i", bytes(b[:4]))[0]


def _committed_keeper(entry, oq_count=None):
    """A callable off -> keep byte (0..255) for one state_masks.json region entry, bound to one
    side's order_queue_count (meaningful only for the `dynamic` region)."""
    dyn = entry.get("dynamic")
    if dyn:
        live = _order_queue_live_bytes(oq_count, dyn["record_size"], dyn["cap"], entry["len"])
        return lambda off: 0xFF if off < live else 0x00
    stride = entry.get("stride") or 1
    keep = entry.get("keep") or [0xFF]
    return lambda off: keep[off % stride]


def _legacy_keeper(mask_ranges):
    return lambda off: 0x00 if _is_masked(off, mask_ranges) else 0xFF


def _byte_diffs(region_name, base_off, a_bytes, b_bytes, keep_a=None, keep_b=None):
    """Byte-run diffs between two byte strings. `keep_a`/`keep_b` (offset -> keep byte, 0..255, or
    None meaning "compare fully") are applied INDEPENDENTLY to each side before the two are
    compared -- not one shared mask ANDed onto an XOR -- so a per-side-dynamic boundary (order_queue)
    is exact: each side's raw byte is masked by ITS OWN keep byte, exactly as the VERDICT stream
    masks each peer's own bytes before the peers are compared. For every other region keep_a and
    keep_b are the same callable, so this reduces to the equivalent of `(a^b)&keep`."""
    if a_bytes == b_bytes:
        return []
    diffs = []
    run_start = None
    n = len(a_bytes)
    for i in range(n):
        off = base_off + i
        ka = 0xFF if keep_a is None else keep_a(off)
        kb = 0xFF if keep_b is None else keep_b(off)
        differ = (a_bytes[i] & ka) != (b_bytes[i] & kb)
        if differ and run_start is None:
            run_start = i
        elif not differ and run_start is not None:
            diffs.append(
                {"region": region_name, "offset": base_off + run_start, "len": i - run_start}
            )
            run_start = None
    if run_start is not None:
        diffs.append({"region": region_name, "offset": base_off + run_start, "len": n - run_start})
    return diffs


def _merge_spans(spans):
    by_region = {}
    for name, off, length in spans:
        by_region.setdefault(name, []).append((off, off + length))
    merged = {}
    for name, ranges in by_region.items():
        ranges.sort()
        out = []
        for lo, hi in ranges:
            if out and lo <= out[-1][1]:
                out[-1] = (out[-1][0], max(out[-1][1], hi))
            else:
                out.append((lo, hi))
        merged[name] = [(lo, hi - lo) for lo, hi in out]
    return merged


def _format_diff(step, diffs, rec):
    resolved = []
    for d in diffs:
        region_len = rec.regions[rec.region_index[d["region"]]].len
        desc = describe_offset(d["region"], d["offset"], region_len)
        entry = dict(d)
        entry.update(desc)
        resolved.append(entry)
    return {"identical": False, "step": step, "diffs": resolved}


def first_diff(a, b, masks=None, state_masks=True):
    """The first step (within the common decoded step range) where `a` and `b` disagree. Returns
    {"identical": True, "first_step": s0, "last_step": s1} or
    {"identical": False, "step": s, "diffs": [{"region","offset","len","record","field","note"}, ...]}.

    `masks`: the OLD byte-range schema {region: [[offset,len],...]} (see the module docstring). When
    given, it is used ALONE -- `state_masks` is forced off, the two schemes do not combine.
    `state_masks`: the committed mp:D42 mask table (a dict from `load_state_masks()`), True (the
    default) to auto-load `tools/data/state_masks.json`, or False/None to compare raw bytes (what
    the CLI's `diff --raw` passes). Ignored when `masks` is given. A region marked `excluded` in the
    table is skipped entirely, matching `mh::state::inc::fold_state`.
    """
    if masks:
        state_masks = None
    elif state_masks is True:
        state_masks = load_state_masks()
    _check_compatible_regions(a, b)
    if a.first_step is None or b.first_step is None:
        raise ValueError("one of the recordings has no decodable steps")
    lo = max(a.first_step, b.first_step)
    hi = min(a.last_step, b.last_step)
    if lo > hi:
        raise ValueError("no overlapping step range between %s and %s" % (a.path, b.path))

    def keepers_for(name, sa, sb):
        """(keep_a, keep_b) for `_byte_diffs`, or ("SKIP", "SKIP") for an excluded region."""
        if state_masks:
            entry = state_masks.get(name)
            if entry is None:
                return None, None
            if entry.get("excluded"):
                return "SKIP", "SKIP"
            dyn = entry.get("dynamic")
            if dyn:
                return (
                    _committed_keeper(entry, _region_int32(sa, dyn["count_region"])),
                    _committed_keeper(entry, _region_int32(sb, dyn["count_region"])),
                )
            k = _committed_keeper(entry)
            return k, k
        if masks:
            k = _legacy_keeper(masks.get(name, []))
            return k, k
        return None, None

    state_a = state_at(a, lo)
    state_b = state_at(b, lo)
    diffs = []
    for r in a.regions:
        ka, kb = keepers_for(r.name, state_a, state_b)
        if ka == "SKIP":
            continue
        diffs.extend(_byte_diffs(r.name, 0, state_a[r.name], state_b[r.name], ka, kb))
    if diffs:
        return _format_diff(lo, diffs, a)

    buf_a = {r.name: bytearray(state_a[r.name]) for r in a.regions}
    buf_b = {r.name: bytearray(state_b[r.name]) for r in b.regions}
    for step in range(lo + 1, hi + 1):
        sa = a.step_chunks.get(step)
        sb = b.step_chunks.get(step)
        touched = []
        if sa:
            for ridx, off, rlen, data in sa.runs:
                name = a.regions[ridx].name
                buf_a[name][off : off + rlen] = data
                touched.append((name, off, rlen))
        if sb:
            for ridx, off, rlen, data in sb.runs:
                name = b.regions[ridx].name
                buf_b[name][off : off + rlen] = data
                touched.append((name, off, rlen))
        if not touched:
            continue
        diffs = []
        for name, spans in _merge_spans(touched).items():
            ka, kb = keepers_for(name, buf_a, buf_b)
            if ka == "SKIP":
                continue
            for off, length in spans:
                da = bytes(buf_a[name][off : off + length])
                db = bytes(buf_b[name][off : off + length])
                diffs.extend(_byte_diffs(name, off, da, db, ka, kb))
        if diffs:
            return _format_diff(step, diffs, a)
    return {"identical": True, "first_step": lo, "last_step": hi}


# ==== CLI ==============================================================================================


def cmd_info(a):
    try:
        rec = load(a.file)
    except (ValueError, OSError) as e:
        print("state_record: ERROR -- %s" % e)
        return 2
    h = rec.header
    print("file: %s" % a.file)
    print("version: %d" % h.version)
    print("flags: 0x%04x%s" % (h.flags, "  (RING)" if h.is_ring else ""))
    print("header_len: %d" % h.header_len)
    print("manifest_fp: 0x%016x" % h.manifest_fp)
    print("keyframe_every: %d" % h.keyframe_every)
    print("region_count: %d  (%d bytes/state)" % (h.region_count, rec.total_len))
    for i, r in enumerate(h.regions):
        print("  [%3d] %-28s %8d bytes  @flat+0x%x" % (i, r.name, r.len, rec.region_offset[i]))
    n_keyf = sum(1 for c in rec.chunks if isinstance(c, KeyframeChunk))
    n_step = sum(1 for c in rec.chunks if isinstance(c, StepChunk))
    n_end = 1 if rec.end else 0
    print("chunks: %d KEYF, %d STEP, %d END" % (n_keyf, n_step, n_end))
    print("first step: %s" % rec.first_step)
    print("last step (decoded): %s" % rec.last_step)
    ks = rec.keyframe_steps
    if len(ks) <= 20:
        print("keyframe steps: %s" % ks)
    else:
        print("keyframe steps: %s ... %s  (%d total)" % (ks[:5], ks[-5:], len(ks)))
    print("END chunk present: %s" % bool(rec.end))
    if rec.end:
        print(
            "  last_step=%d steps_recorded=%d raw_bytes_written=%d"
            % (rec.end.last_step, rec.end.steps_recorded, rec.end.raw_bytes_written)
        )
    if rec.stop_reason:
        print("decode stopped: %s -- %s" % (rec.stop_reason, rec.stop_detail))
    else:
        print("decode stopped: clean EOF")
    return 0


def cmd_at(a):
    try:
        rec = load(a.file)
    except (ValueError, OSError) as e:
        print("state_record: ERROR -- %s" % e)
        return 2
    names = [a.region] if a.region else [r.name for r in rec.regions]
    for n in names:
        if n not in rec.region_index:
            print("state_record: unknown region '%s' (file has %d regions)" % (n, len(rec.regions)))
            return 2
    try:
        state = state_at(rec, a.step)
    except ValueError as e:
        print("state_record: ERROR -- %s" % e)
        return 2
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        for n in names:
            path = os.path.join(a.out, n + ".bin")
            with open(path, "wb") as f:
                f.write(state[n])
            print("%s -> %s (%d bytes)" % (n, path, len(state[n])))
    else:
        for n in names:
            b = state[n]
            print("%-28s %8d bytes  sha1=%s" % (n, len(b), hashlib.sha1(b).hexdigest()))
    return 0


def cmd_diff(a):
    try:
        rec_a = load(a.a)
        rec_b = load(a.b)
    except (ValueError, OSError) as e:
        print("state_record: ERROR -- %s" % e)
        return 2
    masks = {}
    if a.masks:
        with open(a.masks, encoding="utf-8") as f:
            masks = json.load(f)
    # mp:D42: the committed mask table is the default (masks/keep bits + excluded regions + the
    # dynamic order_queue boundary) unless --masks (the old byte-range scheme) or --raw overrides it.
    state_masks_arg = False if a.raw else True
    try:
        result = first_diff(rec_a, rec_b, masks=masks, state_masks=state_masks_arg)
    except (ValueError, OSError, KeyError) as e:
        print("state_record: ERROR -- %s" % e)
        return 2
    if result["identical"]:
        print("IDENTICAL over steps %d..%d" % (result["first_step"], result["last_step"]))
        return 0
    print("FIRST DIFF at step %d" % result["step"])
    for d in result["diffs"]:
        loc = ""
        if d.get("field"):
            rec_str = "record %d, " % d["record"] if d.get("record") is not None else ""
            loc = "  (%sfield=%s)" % (rec_str, d["field"])
        elif d.get("note"):
            loc = "  (%s)" % d["note"]
        print("  %-20s offset 0x%x len %d%s" % (d["region"], d["offset"], d["len"], loc))
    return 1


# ==== naming the in-band watch's LOCALISED lines ====================================================
# mp:D44: the in-band desync watch (desync_watch.cpp, WIRE_VERSION 2) localises a divergence live and
# logs `; [desync] LOCALISED step=S peer=P region=R <name> +0xOFF ...` in mh_net.log. mh.dll carries no
# struct layouts beyond record sizes, so the FIELD is named here, from the same mh_structs.gen.h.
_LOCALISED_RE = re.compile(
    r"LOCALISED step=(\d+) peer=(-?\d+) region=(\d+) (\S+) \+0x([0-9A-Fa-f]+)"
)


def name_localised_lines(lines):
    """mh_net.log lines -> [{step, peer, region, offset, record, field, note}] for every LOCALISED line."""
    out = []
    for ln in lines:
        m = _LOCALISED_RE.search(ln)
        if not m:
            continue
        off = int(m.group(5), 16)
        d = describe_offset(m.group(4), off, off + 1)
        out.append(
            {
                "step": int(m.group(1)),
                "peer": int(m.group(2)),
                "region": m.group(4),
                "offset": off,
                "record": d.get("record"),
                "field": d.get("field"),
                "note": d.get("note"),
            }
        )
    return out


def cmd_name(a):
    if a.log:
        with open(a.log, encoding="utf-8", errors="replace") as fh:
            rows = name_localised_lines(fh.read().splitlines())
        if not rows:
            print("no LOCALISED lines in %s" % a.log)
            return 1
        for r in rows:
            where = "record %s, field %s" % (r["record"], r["field"]) if r["field"] else r["note"]
            print(
                "step %d peer %d: %s +0x%x -> %s"
                % (r["step"], r["peer"], r["region"], r["offset"], where)
            )
        return 0
    if a.region is None or a.offset is None:
        print("name: give REGION OFFSET, or --log mh_net.log")
        return 2
    off = int(a.offset, 0)
    d = describe_offset(a.region, off, off + 1)
    print(
        "%s +0x%x -> record %s, field %s%s"
        % (
            a.region,
            off,
            d.get("record"),
            d.get("field"),
            ("  (%s)" % d["note"]) if d.get("note") else "",
        )
    )
    return 0 if d.get("field") else 1


def build_argparser():
    ap = argparse.ArgumentParser(prog="state_record.py", description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="run the hermetic selftest and exit")
    sub = ap.add_subparsers(dest="cmd")

    p_info = sub.add_parser("info", help="header fields, region table, chunk counts, decode status")
    p_info.add_argument("file")

    p_at = sub.add_parser("at", help="rebuild the state at STEP")
    p_at.add_argument("file")
    p_at.add_argument("step", type=int)
    p_at.add_argument("--region", help="only this region (default: all)")
    p_at.add_argument(
        "--out", help="write raw region bytes into this directory instead of printing sha1"
    )

    p_diff = sub.add_parser("diff", help="first differing step between two recordings")
    p_diff.add_argument("a")
    p_diff.add_argument("b")
    p_diff.add_argument(
        "--masks",
        help="old byte-range JSON file: {region_name: [[offset,len],...]} -- REPLACES the "
        "committed mp:D42 masks (tools/data/state_masks.json) rather than combining with them",
    )
    p_diff.add_argument(
        "--raw",
        action="store_true",
        help="compare raw bytes, ignoring the committed mp:D42 masks (tools/data/state_masks.json)",
    )

    p_name = sub.add_parser(
        "name",
        help="record + field of a region offset, or of every LOCALISED line in an mh_net.log",
    )
    p_name.add_argument("region", nargs="?")
    p_name.add_argument("offset", nargs="?", help="byte offset within the slice (0x.. or decimal)")
    p_name.add_argument("--log", help="an mh_net.log: name every `[desync] LOCALISED` line in it")

    return ap


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--selftest" in argv:
        return selftest()
    ap = build_argparser()
    a = ap.parse_args(argv)
    if not a.cmd:
        ap.error("a subcommand is required (info | at | diff | name), or --selftest")
    if a.cmd == "info":
        return cmd_info(a)
    if a.cmd == "at":
        return cmd_at(a)
    if a.cmd == "diff":
        return cmd_diff(a)
    if a.cmd == "name":
        return cmd_name(a)
    ap.error("unknown command %r" % a.cmd)
    return 2


# ==== selftest =========================================================================================
# Hand-assembled bytes, deliberately NOT built through any shared "encode" helper the reader also uses --
# `_pack_*` below hardcode the tag/format literals independently of the module-level TAG_*/*_FMT
# constants, so a bug that canceled itself out between a shared writer and the reader cannot happen.


def _pack_header(region_defs, version=1, flags=0, manifest_fp=0xDEADBEEF, keyframe_every=4):
    region_table = b""
    for name, ln in region_defs:
        nb = name.encode("utf-8")
        region_table += struct.pack("<IB", ln, len(nb)) + nb
    header_len = 4 + 2 + 2 + 4 + 8 + 4 + 4 + len(region_table)
    fixed = struct.pack(
        "<IHHIQII",
        0x5253484D,
        version,
        flags,
        header_len,
        manifest_fp,
        keyframe_every,
        len(region_defs),
    )
    return fixed + region_table


def _pack_chunk(tag, payload):
    return struct.pack("<III", tag, len(payload), zlib.crc32(payload) & 0xFFFFFFFF) + payload


def _pack_keyf(step, state):
    return _pack_chunk(0x4659454B, struct.pack("<I", step) + state)


def _pack_step(step, runs):
    body = struct.pack("<II", step, len(runs))
    for ridx, off, rlen, data in runs:
        body += struct.pack("<HIH", ridx, off, rlen) + data
    return _pack_chunk(0x50455453, body)


def _pack_end(last_step, steps_recorded, raw_bytes_written):
    return _pack_chunk(
        0x20444E45, struct.pack("<IIQ", last_step, steps_recorded, raw_bytes_written)
    )


def _make_runs(prev, cur, region_defs):
    runs = []
    off = 0
    for ridx, (_name, ln) in enumerate(region_defs):
        pv, cv = prev[off : off + ln], cur[off : off + ln]
        i = 0
        while i < ln:
            if pv[i] != cv[i]:
                j = i
                while j < ln and pv[j] != cv[j]:
                    j += 1
                runs.append((ridx, i, j - i, cv[i:j]))
                i = j
            else:
                i += 1
        off += ln
    return runs


def _build_match(region_defs, states_by_step, keyframe_every, with_end=True, version=1, flags=0):
    """Standalone synthetic-file builder for the selftest. `states_by_step`: {step: full_state_bytes}
    for consecutive steps starting at the lowest key. Returns (bytes, offsets) where `offsets[step]` is
    the file offset where that step's FIRST chunk (its STEP chunk if present, else its KEYF) begins --
    used by the tail-cut test to slice a valid mid-file KEYF boundary without decoding anything."""
    steps = sorted(states_by_step)
    out = bytearray(
        _pack_header(region_defs, version=version, flags=flags, keyframe_every=keyframe_every)
    )
    offsets = {}
    prev = None
    for s in steps:
        offsets[s] = len(out)
        cur = states_by_step[s]
        is_kf = (s == steps[0]) or ((s - steps[0]) % keyframe_every == 0)
        if prev is not None:
            out += _pack_step(s, _make_runs(prev, cur, region_defs))
        if is_kf:
            out += _pack_keyf(s, cur)
        prev = cur
    if with_end:
        out += _pack_end(steps[-1], len(steps), len(out))
    return bytes(out), offsets


def _synthetic_state(step, total_len):
    return bytes((step + i) % 256 for i in range(total_len))


def selftest():
    fails = []

    def check(label, cond):
        print("  %-72s %s" % (label, "ok" if cond else "FAIL"))
        if not cond:
            fails.append(label)

    region_defs = [("alpha", 8), ("beta", 4)]
    total_len = 12
    n_steps = 20
    states = {s: _synthetic_state(s, total_len) for s in range(1, n_steps + 1)}
    data, offsets = _build_match(region_defs, states, keyframe_every=5)

    tmp = tempfile.mkdtemp(prefix="state_record_selftest_")
    try:
        path = os.path.join(tmp, "a.bin")
        with open(path, "wb") as f:
            f.write(data)
        rec = load(path)

        check("clean file decodes with no stop reason", rec.stop_reason is None)
        check("first/last step decoded", rec.first_step == 1 and rec.last_step == n_steps)
        check("keyframe steps at every keyframe_every", rec.keyframe_steps == [1, 6, 11, 16])
        check("END chunk present", rec.end is not None and rec.end.last_step == n_steps)

        ok = True
        for s in range(1, n_steps + 1):
            st = state_at(rec, s)
            rebuilt = b"".join(st[name] for name, _ in region_defs)
            if rebuilt != states[s]:
                ok = False
        check("round-trip rebuild matches the known state at every step", ok)

        # mp:D46: the gzip form mh.dll leaves after a match decodes identically to the raw one.
        gz_path = os.path.join(tmp, "a.bin.gz")
        with open(gz_path, "wb") as f:
            f.write(gzip.compress(data, 6))
        rec_gz = load(gz_path)
        check(
            "gzip file: same steps, keyframes, END and stop reason as the raw file",
            rec_gz.stop_reason is None
            and rec_gz.first_step == rec.first_step
            and rec_gz.last_step == rec.last_step
            and rec_gz.keyframe_steps == rec.keyframe_steps
            and rec_gz.end is not None
            and rec_gz.end.last_step == n_steps,
        )
        check(
            "gzip file: every step rebuilds to the same bytes as the raw file",
            all(
                b"".join(state_at(rec_gz, s)[n] for n, _ in region_defs) == states[s]
                for s in range(1, n_steps + 1)
            ),
        )
        gz_only = os.path.join(tmp, "gzonly")
        os.makedirs(gz_only)
        with open(os.path.join(gz_only, "m.bin.gz"), "wb") as f:
            f.write(gzip.compress(data, 6))
        check(
            "naming the raw path reads the .gz sibling when the raw file is gone",
            load(os.path.join(gz_only, "m.bin")).last_step == n_steps,
        )
        both = os.path.join(tmp, "both")
        os.makedirs(both)
        with open(os.path.join(both, "m.bin"), "wb") as f:
            f.write(data)
        with open(os.path.join(both, "m.bin.gz"), "wb") as f:
            f.write(b"not a gzip stream")  # proves the raw file wins: this would not load
        check(
            "raw and .gz both present (a kill after the rename): the raw file is read",
            load(os.path.join(both, "m.bin")).last_step == n_steps,
        )
        gz_cut = os.path.join(tmp, "cut.bin.gz")
        gz_whole = gzip.compress(data, 6)
        with open(gz_cut, "wb") as f:
            f.write(gz_whole[: len(gz_whole) * 3 // 4])
        rec_cut = load(gz_cut)
        check(
            "a gzip stream cut short stops as 'truncated' and keeps the steps before the cut",
            rec_cut.stop_reason == "truncated"
            and rec_cut.last_step is not None
            and 1 <= rec_cut.last_step < n_steps,
        )

        # identical files -> IDENTICAL
        path2 = os.path.join(tmp, "a_copy.bin")
        with open(path2, "wb") as f:
            f.write(data)
        rec2 = load(path2)
        res = first_diff(rec, rec2, {})
        check("identical files -> IDENTICAL", res["identical"] is True and res["first_step"] == 1)

        # one poked byte -> exactly that step/region/offset
        poke_step, poke_region_off = 13, 2  # inside "alpha" (region 0, offset 2)
        states_b = dict(states)
        poked = bytearray(states_b[poke_step])
        poked[poke_region_off] ^= 0xFF
        states_b[poke_step] = bytes(poked)
        data_b, _ = _build_match(region_defs, states_b, keyframe_every=5)
        path_b = os.path.join(tmp, "b_poked.bin")
        with open(path_b, "wb") as f:
            f.write(data_b)
        rec_b = load(path_b)
        res = first_diff(rec, rec_b, {})
        check(
            "one poked byte -> exactly that step/region/offset",
            res["identical"] is False
            and res["step"] == poke_step
            and len(res["diffs"]) == 1
            and res["diffs"][0]["region"] == "alpha"
            and res["diffs"][0]["offset"] == poke_region_off
            and res["diffs"][0]["len"] == 1,
        )

        # masked-only difference -> IDENTICAL
        res_masked = first_diff(rec, rec_b, {"alpha": [[poke_region_off, 1]]})
        check("masked-only difference -> IDENTICAL", res_masked["identical"] is True)

        # field naming on a real region + real struct: "progress" = mh_game_progress {available,
        # acquired, f3}, 3 bytes/record. Region len 9 = 3 records.
        prog_defs = [("progress", 9)]
        prog_states = {s: _synthetic_state(s, 9) for s in range(1, 9)}
        prog_a, _ = _build_match(prog_defs, prog_states, keyframe_every=4)
        prog_states_b = dict(prog_states)
        pb = bytearray(prog_states_b[7])
        pb[4] ^= 0x7  # record 1 (offset 3..5), local offset 1 -> "acquired"
        prog_states_b[7] = bytes(pb)
        prog_b, _ = _build_match(prog_defs, prog_states_b, keyframe_every=4)
        pa_path, pb_path = os.path.join(tmp, "prog_a.bin"), os.path.join(tmp, "prog_b.bin")
        with open(pa_path, "wb") as f:
            f.write(prog_a)
        with open(pb_path, "wb") as f:
            f.write(prog_b)
        res_field = first_diff(load(pa_path), load(pb_path), {})
        check(
            "poked byte in a known struct region names record + field",
            res_field["identical"] is False
            and res_field["step"] == 7
            and res_field["diffs"][0]["record"] == 1
            and res_field["diffs"][0]["field"] == "acquired",
        )
        # mp:D44: the watch's live LOCALISED line names the same record + field offline.
        named = name_localised_lines(
            [
                "[00:01:02.000] ; [desync] LOCALISED step=1200 peer=1 region=62 progress +0x4 = record 1 +0x1 "
                "of game_progress: 1 byte(s) differ in its 64-byte block 0, first mine=01 theirs=06",
                "[00:01:02.000] ; [desync] STATUS: compared=5",
            ]
        )
        check(
            "name: a LOCALISED log line resolves to record + field",
            len(named) == 1
            and named[0]["step"] == 1200
            and named[0]["record"] == 1
            and named[0]["field"] == "acquired",
        )

        # truncated file: cut off mid-payload of a later chunk
        cut_at = offsets[13] + 3  # inside step 13's chunk header/payload
        trunc_path = os.path.join(tmp, "trunc.bin")
        with open(trunc_path, "wb") as f:
            f.write(data[:cut_at])
        rec_t = load(trunc_path)
        check(
            "truncated file decodes up to its last complete step and says so",
            rec_t.stop_reason == "truncated" and rec_t.last_step == 12,
        )

        # CRC-corrupted chunk: flip a payload byte of step 13's STEP chunk without fixing the CRC
        corrupt = bytearray(data)
        corrupt[offsets[13] + CHUNK_HDR_SIZE] ^= 0xFF
        corrupt_path = os.path.join(tmp, "corrupt.bin")
        with open(corrupt_path, "wb") as f:
            f.write(bytes(corrupt))
        rec_c = load(corrupt_path)
        check(
            "CRC-corrupted chunk stops decoding there",
            rec_c.stop_reason == "crc_mismatch" and rec_c.last_step == 12,
        )

        # tail-cut file: header + chunks from a later KEYF (step 11)
        header_bytes = _pack_header(region_defs, keyframe_every=5)
        tail_path = os.path.join(tmp, "tail.bin")
        with open(tail_path, "wb") as f:
            f.write(header_bytes + data[offsets[11] :])
        rec_tail = load(tail_path)
        tail_state_11 = state_at(rec_tail, 11)
        tail_state_18 = state_at(rec_tail, 18)
        check(
            "tail-cut file decodes, starting at the kept keyframe",
            rec_tail.first_step == 11
            and rec_tail.last_step == n_steps
            and b"".join(tail_state_11[n] for n, _ in region_defs) == states[11]
            and b"".join(tail_state_18[n] for n, _ in region_defs) == states[18],
        )

        # unknown version refused by name
        bad_header = _pack_header(region_defs, version=99, keyframe_every=5)
        bad_path = os.path.join(tmp, "bad_version.bin")
        with open(bad_path, "wb") as f:
            f.write(bad_header)
        try:
            load(bad_path)
            refused = False
            msg = ""
        except ValueError as e:
            refused = True
            msg = str(e)
        check("unknown version refused by name", refused and "version" in msg and "99" in msg)

        # struct-layout parser sanity, independent of any real region: a tiny synthetic header text
        synth_text = (
            "struct mh_x_inner {\n"
            "    uint8_t a;\n"
            "    int32_t b;\n"
            "};\n"
            "\n"
            "// x_outer (size 0xc, Ghidra category /test)\n"
            "struct mh_x_outer {\n"
            "    struct mh_x_inner items[2];\n"
            "    uint8_t _pad_0xa[2];\n"
            "};\n"
        )
        structs = parse_structs(synth_text)
        outer = structs["mh_x_outer"]
        check(
            "synthetic struct parser: size matches its own (size 0xNN) comment",
            outer.reliable and outer.size == 12,
        )
        check(
            "synthetic struct parser: nested array field resolves by index + inner field",
            _resolve_field_path(structs, "mh_x_outer", 6) == "items[1].b",
        )
        check(
            "synthetic struct parser: pad field still names (it is an explicit field, not a gap)",
            _resolve_field_path(structs, "mh_x_outer", 10) == "_pad_0xa[0]",
        )

        # CLI diff with a masks FILE (not the in-memory dict path)
        masks_path = os.path.join(tmp, "masks.json")
        with open(masks_path, "w", encoding="utf-8") as f:
            json.dump({"alpha": [[poke_region_off, 1]]}, f)
        ns = argparse.Namespace(a=path, b=path_b, masks=masks_path, raw=False)
        rc = cmd_diff(ns)
        check("CLI diff --masks (file path) also reports IDENTICAL", rc == 0)

        # mp:D42: the COMMITTED masks (tools/data/state_masks.json), the default `diff` now uses --
        # real region names, so a masked-only difference in each of the mask table's four special
        # shapes (a static bit mask, a fully-masked byte, an excluded region, the dynamic
        # order_queue boundary) must read IDENTICAL, and a real byte in the same regions must not.
        oq_defs = [
            ("tile_objects", 16),  # 2 records, stride 8 (real region: TILE_FLAGS_KEEP fog bits)
            ("rng_state", 16),  # 4 x 4-byte slots (real region: slot 1 is the masked fx channel)
            ("ls_horizon", 8),  # excluded: true in the real table
            ("order_queue", 136),  # 2 records @ 68 bytes (real record_size)
            ("order_queue_count", 4),
        ]

        _oq_seq = [0]

        def _one_step(tag, parts):
            _oq_seq[0] += 1
            state = b"".join(parts[n] for n, _ln in oq_defs)
            data_, _ = _build_match(oq_defs, {1: state}, keyframe_every=1)
            p = os.path.join(tmp, "cm_%s_%d.bin" % (tag, _oq_seq[0]))
            with open(p, "wb") as f:
                f.write(data_)
            return p, load(p)

        base = {
            "tile_objects": bytes([0x11, 0x22, 3, 4, 5, 6, 7, 8, 0x11, 0x22, 3, 4, 5, 6, 7, 8]),
            "rng_state": bytes(range(1, 17)),
            "ls_horizon": bytes(range(1, 9)),
            "order_queue": bytes([0xAA] * 68 + [0xBB] * 68),
            "order_queue_count": struct.pack("<i", 1),  # only record 0 (bytes [0,68)) is live
        }
        path_x, rec_x = _one_step("x", base)

        def _mutated(field, mutate):
            parts = dict(base)
            b = bytearray(parts[field])
            mutate(b)
            parts[field] = bytes(b)
            return _one_step(field, parts)

        _, y_fog = _mutated(
            "tile_objects", lambda b: b.__setitem__(1, b[1] ^ 0xC0)
        )  # bits 6-7 only
        check(
            "committed masks: a fog-bit-only tile_objects change -> IDENTICAL",
            first_diff(rec_x, y_fog)["identical"] is True,
        )
        _, y_vis = _mutated(
            "tile_objects", lambda b: b.__setitem__(7, b[7] ^ 0xFF)
        )  # +7, fully masked
        check(
            "committed masks: a tile_objects visibility (+7) change -> IDENTICAL",
            first_diff(rec_x, y_vis)["identical"] is True,
        )
        _, y_rng = _mutated("rng_state", lambda b: b.__setitem__(5, b[5] ^ 0xFF))  # slot 1, +4..+7
        check(
            "committed masks: an rng_state slot-1 (fx channel) change -> IDENTICAL",
            first_diff(rec_x, y_rng)["identical"] is True,
        )
        _, y_excl = _mutated("ls_horizon", lambda b: b.__setitem__(0, b[0] ^ 0xFF))
        check(
            "committed masks: an excluded region (ls_horizon) change -> IDENTICAL",
            first_diff(rec_x, y_excl)["identical"] is True,
        )
        _, y_oq_dead = _mutated(
            "order_queue", lambda b: b.__setitem__(100, b[100] ^ 0xFF)
        )  # record 1
        check(
            "committed masks: an order_queue record past order_queue_count -> IDENTICAL",
            first_diff(rec_x, y_oq_dead)["identical"] is True,
        )

        _, y_real_tile = _mutated(
            "tile_objects", lambda b: b.__setitem__(0, b[0] ^ 0xFF)
        )  # +0, kept
        res_ct = first_diff(rec_x, y_real_tile)
        check(
            "committed masks: a REAL tile_objects byte (+0) is named, not masked away",
            res_ct["identical"] is False
            and res_ct["diffs"][0]["region"] == "tile_objects"
            and res_ct["diffs"][0]["offset"] == 0,
        )
        _, y_real_oq = _mutated(
            "order_queue", lambda b: b.__setitem__(5, b[5] ^ 0xFF)
        )  # record 0, live
        res_cq = first_diff(rec_x, y_real_oq)
        check(
            "committed masks: a REAL order_queue byte (in the live prefix) is named, not masked away",
            res_cq["identical"] is False
            and res_cq["diffs"][0]["region"] == "order_queue"
            and res_cq["diffs"][0]["offset"] == 5,
        )

        # --raw disables the committed masks: the same fog-bit-only change now reads as a real diff --
        # exercised both through the module API and through the CLI (cmd_diff's --raw forwarding).
        res_raw = first_diff(rec_x, y_fog, state_masks=False)
        check(
            "--raw (state_masks=False): the same fog-bit-only change is now a real diff",
            res_raw["identical"] is False and res_raw["diffs"][0]["region"] == "tile_objects",
        )
        path_fog, _ = _mutated("tile_objects", lambda b: b.__setitem__(1, b[1] ^ 0xC0))
        rc_masked = cmd_diff(argparse.Namespace(a=path_x, b=path_fog, masks=None, raw=False))
        rc_raw = cmd_diff(argparse.Namespace(a=path_x, b=path_fog, masks=None, raw=True))
        check(
            "CLI diff: default masked IDENTICAL, --raw reports the real diff",
            rc_masked == 0 and rc_raw == 1,
        )
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)

    print("state_record selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
