#!/usr/bin/env python3
# gen_save_table_header.py -- turn the EXTRACTED block table into the C++ table the reimplemented
# drivers walk.
#
#   python tools/gen_save_table_header.py            # write src/mh_dll/libmh/save/save_table.gen.h
#   python tools/gen_save_table_header.py --check     # verify the committed header is up to date
#
# Input is tools/data/save_block_table.json, which the save-block table extractor derives from the
# exported disassembly. This script adds the one thing that extraction cannot supply: the SHAPE --
# which runs of blocks belong to which driver, where the structural steps (the region graph, the
# progress loop, the embedded members, the media trailer) sit between them, and which reads are
# gated on the detected save version. Everything else, above all the (address, size) pairs and their
# ORDER, comes straight from the binary.
#
# THE SHAPE IS ANCHORED BY CALL ADDRESS, NOT BY INDEX. A run is named by the address of its first and
# last block call, so re-deriving the table after a Ghidra change either resolves those anchors or
# fails here -- a block inserted *inside* a run is picked up silently (that is the point of extracting
# the table), while a boundary that moved is loud.
#
# THE PAIR CHECK IS THE GENERATION. The writer and reader programs are zipped step by step and every
# pair must agree on address and size; a read-only legacy block is marked `discard` and skipped on the
# write side. That is why the JSON's own "19 written vs 20 read" diagnostic is not a defect: applying
# the three version gates makes the two sides line up exactly, and this script fails if they do not.

import argparse
import bisect
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(REPO, "tools", "data", "save_block_table.json")
OUT = os.path.join(REPO, "src", "mh_dll", "libmh", "save", "save_table.gen.h")

# ---------------------------------------------------------------------------------------------
# The shape. Each program is an ordered list of steps:
#   ("run", fn, first_at, last_at)   a contiguous run of block calls, resolved by address
#   ("struct", KIND)                 a structural step, described in the generated header
#
# Version gates live in VER_GATES, keyed by the READER's call address -- the writer always stamps the
# current version, so gating is a read-side concept only. docs/save-format.md "The three compat gates".
VER_GATES = {
    "0x004477cb": (0, 1, True),  # ver < 2: one extra 0x78b4 read into a stack buffer, discarded
    "0x004478a4": (0, 3, True),  # ver < 4: the narrow message queue, converted then discarded
    "0x00447905": (
        4,
        255,
        False,
    ),  # ver >= 4: the modern MESSAGE_QUEUE read the ver<4 path jumps past
    "0x0044795f": (5, 255, False),  # ver >= 5: INVASION_ALERT_TIME
    "0x00447974": (5, 255, False),  # ver >= 5: ADVISOR_NEXT_TIME
}

CONTAINER_W = [
    ("run", "game__SaveGame", "0x0044726e", "0x004473d3"),
    ("run", "FUN_0041c152", "0x0041c181", "0x0041c181"),
    ("struct", "STEP_MEMBERS"),
    ("struct", "STEP_MEDIA"),
]
CONTAINER_R = [
    ("run", "llm_game_load", "0x00447785", "0x00447974"),
    ("run", "llm_game_load_available_projects", "0x0041c1cb", "0x0041c1cb"),
    ("struct", "STEP_MEMBERS"),
    ("struct", "STEP_TAIL"),
]

# SavePlanetToDisk calls llm_map_save_regions BETWEEN the 0x10000 block at 0x0044803f and fog_of_war at
# 0x0044805c (CALL 0x00424773 @ 0x0044804a), and finishes with two helper pairs after the progress
# loop. Both facts were read out of the disassembly; the earlier prose put all three helper pairs in
# game::SaveGame, which is wrong -- only FUN_0041c152 is called there.
PLANET_W = [
    ("run", "SavePlanetToDisk", "0x00447d36", "0x0044803f"),
    ("struct", "STEP_REGIONS"),
    ("run", "SavePlanetToDisk", "0x0044805c", "0x00448086"),
    ("struct", "STEP_PROGRESS"),
    ("run", "llm_game_save_player_data", "0x004dda81", "0x004dda92"),
    ("run", "FUN_0041c1e6", "0x0041c215", "0x0041c22a"),
]
PLANET_R = [
    ("run", "LoadPlanetFromDisk", "0x00448222", "0x0044852b"),
    ("struct", "STEP_REGIONS"),
    ("run", "LoadPlanetFromDisk", "0x0044855f", "0x00448589"),
    ("struct", "STEP_PROGRESS"),
    ("run", "FUN_004ddaa5", "0x004ddac0", "0x004ddad1"),
    ("run", "llm_ui_bldg_panel_load_state", "0x0041c279", "0x0041c28e"),
]

# The region graph's own four header dwords. The N x 0x40c records and the two grid buffers that follow
# are structural (N is data), so they are not runs.
REGIONS_W = [("run", "llm_map_save_regions", "0x0042479b", "0x004247d1")]
REGIONS_R = [("run", "llm_map_load_regions", "0x00424ad5", "0x00424b0b")]

PROGRAMS = [
    ("CONTAINER", CONTAINER_W, CONTAINER_R),
    ("PLANET", PLANET_W, PLANET_R),
    ("REGIONS", REGIONS_W, REGIONS_R),
]


def resolve(tables, fn, first_at, last_at):
    """The block records of `fn` from first_at to last_at inclusive, in file order."""
    recs = [
        r for r in tables[fn] if r["kind"] in ("const", "expr") and r["dir"] in ("write", "read")
    ]
    ats = [r["at"] for r in recs]
    if first_at not in ats:
        raise SystemExit("anchor %s not a block call in %s" % (first_at, fn))
    if last_at not in ats:
        raise SystemExit("anchor %s not a block call in %s" % (last_at, fn))
    i, j = ats.index(first_at), ats.index(last_at)
    if i > j:
        raise SystemExit("anchors out of order in %s: %s > %s" % (fn, first_at, last_at))
    return recs[i : j + 1]


def flatten(tables, program):
    """-> [ ('block', rec) | ('struct', KIND) ]"""
    out = []
    for step in program:
        if step[0] == "struct":
            out.append(("struct", step[1]))
            continue
        for rec in resolve(tables, step[1], step[2], step[3]):
            out.append(("block", rec))
    return out


def zip_directions(name, wflat, rflat):
    """Pair the two directions and return the merged step list. Fails on any disagreement."""
    steps, wi, ri = [], 0, 0
    while wi < len(wflat) or ri < len(rflat):
        rk, rv = rflat[ri] if ri < len(rflat) else (None, None)
        # A read-only legacy block has no write partner: emit it and advance only the read side.
        if rk == "block" and VER_GATES.get(rv["at"], (0, 255, False))[2]:
            lo, hi, _ = VER_GATES[rv["at"]]
            steps.append(mkstep(rv, None, rv, lo, hi, True))
            ri += 1
            continue
        if wi >= len(wflat) or ri >= len(rflat):
            raise SystemExit(
                "%s: the two directions have different lengths (w=%d r=%d)"
                % (name, len(wflat) - wi, len(rflat) - ri)
            )
        wk, wv = wflat[wi]
        if wk != rk:
            raise SystemExit(
                "%s: step %d is %s on write and %s on read" % (name, len(steps), wk, rk)
            )
        if wk == "struct":
            # STEP_MEDIA / STEP_TAIL are the two halves of the same trailer, so they may differ.
            steps.append({"kind": wv, "read_kind": rv})
        else:
            if wv.get("size") != rv.get("size"):
                raise SystemExit(
                    "%s: step %d size %s written vs %s read (%s / %s)"
                    % (name, len(steps), wv.get("size"), rv.get("size"), wv["at"], rv["at"])
                )
            if wv.get("addr") != rv.get("addr"):
                raise SystemExit(
                    "%s: step %d addr %s written vs %s read (%s / %s)"
                    % (name, len(steps), wv.get("addr"), rv.get("addr"), wv["at"], rv["at"])
                )
            lo, hi, _ = VER_GATES.get(rv["at"], (0, 255, False))
            steps.append(mkstep(wv, wv, rv, lo, hi, False))
        wi += 1
        ri += 1
    return steps


def mkstep(src, w, r, lo, hi, discard):
    return {
        "kind": "STEP_BLOCK",
        "addr": src.get("addr") or "0x00000000",
        "size": src.get("size") or 0,
        "ver_min": lo,
        "ver_max": hi,
        "discard": discard,
        "write_at": (w or {}).get("at", "0x00000000"),
        "read_at": (r or {}).get("at", "0x00000000"),
        "name": src.get("addr_sym") or ("expr@" + src["at"]),
    }


HEADER = '''//
// save/save_table.gen.h -- GENERATED by tools/gen_save_table_header.py from
// tools/data/save_block_table.json. DO NOT EDIT: regenerate.
//
// The save format IS an ordered list of (address, size) block calls, so it is carried as data rather
// than as ninety hand-written calls in two directions. The pairing between the two directions is
// checked by the generator, not asserted here -- see its header comment, and docs/save-format.md.
//
// KIND vocabulary:
//   STEP_BLOCK     one block, fixed address, fixed size.
//   STEP_REGIONS   the region graph -- four 4-byte header blocks, then N x REGION_RECORD_BYTES where
//                  N is the value of the FOURTH header block (G_LAST_MAP_INDEX), then one
//                  REGION_GRID_IDS_BYTES block and one REGION_TERRAIN_BYTES block. All unconditional.
//   STEP_PROGRESS  PROGRESS_COUNT blocks at PROGRESS_STRIDE, each 3 * (uint16)[PROGRESS_SLICE_WORD].
//                  The only data-derived SIZE in the whole format.
//   STEP_MEMBERS   per planet satisfying the embedded-member predicate: a plain uint32 length then
//                  that planet file's bytes VERBATIM, streamed in MEMBER_CHUNK-byte pieces. NOT
//                  blocks. The member count is implicit -- recomputed from state read earlier in the
//                  same file.
//   STEP_MEDIA     the write-only 0x400 media/CD-fingerprint block, the last thing the writer emits.
//   STEP_TAIL      its read-side counterpart: nothing in the original reads it, so a round-tripping
//                  reader captures the remaining bytes opaquely and re-emits them.
//
#pragma once
#include <cstdint>

// SB-HOSTFREE: BLOCK_SLICES names regions by `mh::state::region_id`, so the decomposition and
// the registry cannot drift into two different opinions about which region a run belongs to.
#include "addr/mh_regions.gen.h"

namespace mh::save::table {

enum step_kind : uint8_t {
    STEP_BLOCK = 0,
    STEP_REGIONS,
    STEP_PROGRESS,
    STEP_MEMBERS,
    STEP_MEDIA,
    STEP_TAIL,
};

struct step {
    uint32_t    addr;      // 0 when the step is not one fixed region
    uint32_t    size;      // 0 when derived at run time
    uint8_t     kind;
    uint8_t     ver_min;   // inclusive: applies when detected version >= this
    uint8_t     ver_max;   // inclusive
    bool        discard;   // read-side only: consumed and thrown away, never written
    uint32_t    write_at;  // the original's call address, writer side
    uint32_t    read_at;   // and reader side
    const char *name;
};

inline constexpr uint8_t VER_MIN_ANY = 0;
inline constexpr uint8_t VER_MAX_ANY = 255;

// The progress[] loop: 8 blocks at stride 0x384 from `progress`, each 3 * a word in .data
// (MOVZX EDX,word ptr [0x00e16305] / LEA EDX,[EDX+EDX*2] @ 0x004480a8).
inline constexpr uint32_t PROGRESS_BASE = 0x00c38750;
inline constexpr uint32_t PROGRESS_STRIDE = 0x384;
inline constexpr uint32_t PROGRESS_COUNT = 8;
inline constexpr uint32_t PROGRESS_SLICE_WORD = 0x00e16305;

// The region graph. N is G_LAST_MAP_INDEX, i.e. the fourth header block -- read from the FILE on load
// and from live state on save, which is why a mismatch between it and the linked list's length is a
// real (and unchecked) hazard in the original.
inline constexpr uint32_t REGION_RECORD_BYTES = 0x40c;
inline constexpr uint32_t REGION_COUNT_ADDR = 0x00708b18;
inline constexpr uint32_t REGION_GRID_IDS_BYTES = 0x40000;
inline constexpr uint32_t REGION_TERRAIN_BYTES = 0x10000;
inline constexpr uint32_t REGION_LIST_HEAD_ADDR = 0x0051de7c;
inline constexpr uint32_t REGION_NODE_BYTES = 0x420;   // malloc 0x420 @ 0x00424b76
inline constexpr uint32_t REGION_NODE_NEXT_OFF = 0x40c;
inline constexpr uint32_t REGION_NEIGHBOUR_PTRS_OFF = 0x0c;
inline constexpr uint32_t REGION_NEIGHBOUR_DATA_OFF = 0x20c;
inline constexpr uint32_t REGION_NEIGHBOUR_COUNT_OFF = 0x08;
inline constexpr uint32_t REGION_BY_INDEX_ADDR = 0x00684ac4;

// The embedded-member predicate, byte for byte the same test at 0x0044741a (save) and 0x004479c2
// (load):  Planets[i].system_index == CurrentSystem && (PlanetStatus[i] != 0 || PlanetIndex == i)
inline constexpr uint32_t PLANETS_BASE = 0x00be6da0;
inline constexpr uint32_t PLANET_STRIDE = 0x427;
inline constexpr uint32_t PLANET_SYSTEM_OFF = 0x08;
inline constexpr uint32_t CURRENT_SYSTEM_ADDR = 0x00e58781;
inline constexpr uint32_t PLANET_STATUS_ADDR = 0x00e5836b;   // + i * 4
inline constexpr uint32_t PLANET_INDEX_ADDR = 0x00e58366;
inline constexpr int32_t  MEMBER_FIRST = 1;                  // MOV [EBP-0x28],1 -- NOT 0
inline constexpr int32_t  MEMBER_LIMIT = 0x20;               // CMP [EBP-0x28],0x20 / JL
inline constexpr uint32_t MEMBER_CHUNK = 0x927c0;            // the streaming chunk, = BLOCK_READ_CAP
inline constexpr uint32_t MEDIA_BLOCK_BYTES = 0x400;

'''


# ---------------------------------------------------------------------------------------------
# SB-HOSTFREE: THE BLOCK DECOMPOSITION.
#
# A save block is a contiguous (address, size) window, and TEN of them run past the symbol they
# start at (docs/save-format.md "Blocks do not align to symbols"; docs/state-boundary.md D6.6). In
# the in-process configuration that is harmless -- every region is at its .bss address, so one
# `translate()` hands back one pointer and the bytes are exactly where the format expects.
#
# Under a RELOCATING host it stops being harmless, and quietly. `covering()` resolves a block
# against `reach`, and `reach` was widened precisely so these blocks resolve (D6.5) -- so a block
# that spans four regions still gets a single valid pointer, to whichever region it STARTED in, and
# reads the other three from the copy that region abandoned. Nothing fails; the save is simply
# wrong, in bytes only the save format reads (G135).
#
# So a block is decomposed into the runs a single region OWNS -- by its MEASURED size, never by
# `reach`, which is the number that caused the problem -- plus GAP runs that no region describes.
# A region run follows its region; a gap run stays at its stock address, which is correct because
# nothing relocates memory no region claims.
#
# ONLY BLOCKS WITH TWO OR MORE REGION RUNS ARE EMITTED. A block with one region run plus a trailing
# gap already resolves correctly: the gap lies inside the host's `reach`, and the host bind copies
# `reach` bytes, so the gap travels with it. Emitting those too would cost a 1.3 MB staging buffer
# (player_data) to change nothing.
SLICE_GAP = 0xFFFF


def _regions():
    """(base, size, RID) for every SIZED registry region, sorted. The registry is the same file
    gen_state_registry.py emits mh_regions.gen.h from, so the two cannot disagree."""
    path = os.path.join(REPO, "tools", "data", "state_regions.json")
    regs = json.load(open(path, encoding="utf-8-sig"))["regions"]
    return sorted((r["base"], r["size"], r["id"]) for r in regs if r["size"])


def decompose(addr, size, regions):
    """[(RID or None, region_offset, stock_addr, length)] covering [addr, addr+size) exactly."""
    starts = [b for b, _s, _i in regions]
    out, cur, end = [], addr, addr + size
    while cur < end:
        i = bisect.bisect_right(starts, cur) - 1
        if i >= 0 and cur < regions[i][0] + regions[i][1]:
            base, sz, rid = regions[i]
            stop = min(end, base + sz)
            out.append((rid, cur - base, cur, stop - cur))
        else:
            j = bisect.bisect_right(starts, cur)
            stop = min(end, starts[j]) if j < len(starts) else end
            out.append((None, 0, cur, stop - cur))
        cur = stop
    assert sum(p[3] for p in out) == size, "decomposition lost bytes at 0x%08x" % addr
    return out


def sliced_blocks(steps_by_name):
    """The blocks that need slicing, their runs, and EVERY block's owning region.

    Returns (blocks, slices, report, owners). `owners` is the LIB-REF-SPLIT half: one
    (addr, size, rid_or_None, off) per distinct STEP_BLOCK -- the same decomposition, read for the
    question "which region does this block start in" rather than "does it span several".

    WHY IT IS DERIVED HERE AND NOT LOOKED UP AT RUNTIME. The standalone build has no stock base
    column, so mh::state::covering() -- the runtime answer to that question -- is not declared there
    (addr/mh_regions.gen.h). The answer is a static property of the block table and the registry, and
    both are inputs to THIS generator, so it is computed once, at generation, from the same
    decompose() the slicing uses. One derivation, not a second opinion.
    """
    regions = _regions()
    seen, blocks, slices, report, owners = set(), [], [], [], []
    for _name, steps in steps_by_name:
        for s in steps:
            if s["kind"] != "STEP_BLOCK" or not s["size"]:
                continue
            addr = int(s["addr"].rstrip(","), 16) if isinstance(s["addr"], str) else s["addr"]
            key = (addr, s["size"])
            if key in seen:
                continue
            seen.add(key)
            parts = decompose(addr, s["size"], regions)
            nreg = sum(1 for p in parts if p[0])
            report.append((addr, s["size"], nreg, len(parts) - nreg))
            # The owning run is the FIRST run that a region claims. For the 10 multi-run blocks that
            # is the region the block starts in, which is exactly what covering() returns hosted --
            # so the two arms agree by construction rather than by coincidence. A block no region
            # claims at all records None and becomes RID_COUNT, which the standalone registration
            # check treats the same way the hosted one treats a null from covering().
            own = next(((rid, off) for rid, off, _st, _ln in parts if rid is not None), (None, 0))
            owners.append((addr, s["size"], own[0], own[1]))
            if nreg < 2:
                continue
            blocks.append((addr, s["size"], len(slices), len(parts), s["name"]))
            slices.extend(parts)
    return blocks, slices, report, owners


SLICE_HEADER = """
// ---- SB-HOSTFREE: the block DECOMPOSITION ----------------------------------------------------
//
// Ten save blocks run past the symbol they start at. In the in-process configuration that costs
// nothing -- every region is at its .bss address, so one `translate()` covers the whole window.
// Under a RELOCATING host it is silently wrong: `covering()` resolves a block against `reach`, and
// `reach` was widened so exactly these blocks would resolve (D6.5), so a block spanning four
// regions still gets ONE valid pointer -- to the region it STARTED in -- and reads the rest out of
// the copy that region abandoned. Nothing fails. The save is simply wrong, in bytes only the save
// format ever reads, which is why no determinism run can see it -- the fault class an oracle is
// STRUCTURALLY blind to: corrupt saves from a relocation, with every step green.
//
// A block is therefore decomposed into the runs a single region OWNS -- by its MEASURED size, never
// by `reach`, which is the number that caused this -- plus GAP runs no region describes. A region
// run follows its region through live_base(); a gap run stays at its stock address, which is
// correct because nothing relocates memory that no region claims.
//
// ONLY BLOCKS WITH TWO OR MORE REGION RUNS ARE HERE. One region run plus a trailing gap already
// resolves correctly: the gap lies inside the host's `reach`, and the host bind copies `reach`
// bytes, so the gap travels with it. Emitting those as well would buy a 1.3 MB staging buffer
// (player_data's block) and change nothing.
struct block_slice {
    uint16_t rid;   // mh::state::region_id, or SLICE_GAP
    uint32_t off;   // offset within that region; unused for a gap
    uint32_t stock; // this run's stock address -- where a GAP is served from
    uint32_t len;
};
inline constexpr uint16_t SLICE_GAP = 0xffffu;

struct sliced_block {
    uint32_t    addr;
    uint32_t    size;
    uint16_t    first; // index into BLOCK_SLICES
    uint16_t    count;
    const char *name;
};

inline constexpr block_slice BLOCK_SLICES[] = {
"""


BLOCK_OWNER_HEADER = """
// ---- LIB-REF-SPLIT: every block's owning region, resolved at GENERATION -----------------------
//
// The driver needs two things from a block's address that only the registry can answer: is this
// block covered by a state region at all (the registration check), and which region + offset is it
// (the owned-block delegation). Hosted, mh::state::covering() answers both at compile time from the
// stock base column. The STANDALONE build does not carry that column -- there is no image, so an
// original .bss address is not a thing it can resolve -- and covering() is not declared there.
//
// So the answer is precomputed here, from the same decompose() that produces BLOCK_SLICES and the
// same tools/data/state_regions.json the registry is generated from. It is not a second derivation:
// it is the SAME derivation, read at generation time instead of at compile time.
//
// THE FILE FORMAT IS UNTOUCHED BY THIS, and that is the load-bearing property. `addr` remains the
// step's key and its stock value; nothing here is written to or read from a save. This table only
// answers "where do those bytes live now", which is a question about memory, never about the file.
struct block_owner {
    uint32_t addr; // the step's stock address -- the key, unchanged
    uint32_t size;
    uint16_t rid;  // mh::state::region_id, or RID_COUNT when no region covers the block
    uint32_t off;  // offset of `addr` within that region
};

inline constexpr block_owner BLOCK_OWNERS[] = {
"""

BLOCK_OWNER_LOOKUP = """
// The owning region of the block at [addr, addr+size), or RID_COUNT. constexpr, so the standalone
// registration check below is a compile-time assert exactly as the hosted covering() one is --
// neither configuration gets a weaker check than the other, only a different route to it.
constexpr int block_owner_index(uint32_t addr, uint32_t size) {
    for (int i = 0; i < BLOCK_OWNER_COUNT; ++i)
        if (BLOCK_OWNERS[i].addr == addr && BLOCK_OWNERS[i].size == size) return i;
    return -1;
}

"""


def emit(steps_by_name, counts=None):
    out = [HEADER]
    for name, steps in steps_by_name:
        before = len(out)
        out.append("inline constexpr step %s[] = {\n" % name)
        for s in steps:
            if s["kind"] != "STEP_BLOCK":
                out.append(
                    "    { 0, 0, %-14s VER_MIN_ANY, VER_MAX_ANY, false, 0, 0, \"%s\" },\n"
                    % (s["kind"] + ",", s["kind"])
                )
                if s.get("read_kind") and s["read_kind"] != s["kind"]:
                    out.append(
                        "    { 0, 0, %-14s VER_MIN_ANY, VER_MAX_ANY, true,  0, 0, \"%s\" },\n"
                        % (s["read_kind"] + ",", s["read_kind"])
                    )
                continue
            out.append(
                "    { %-12s %8d, STEP_BLOCK,    %3d, %3d, %-6s %s, %s, \"%s\" },\n"
                % (
                    s["addr"] + ",",
                    s["size"],
                    s["ver_min"],
                    s["ver_max"],
                    ("true," if s["discard"] else "false,"),
                    s["write_at"],
                    s["read_at"],
                    s["name"],
                )
            )
        # A structural step whose two directions differ (STEP_MEDIA / STEP_TAIL) emits TWO array
        # entries from one paired step, so the array length is not len(steps). Report the emitted
        # count, not the paired count -- a number that does not match the header would be exactly the
        # kind of quietly-wrong instrument this file exists to avoid.
        if counts is not None:
            counts[name] = len(out) - before - 1
        out.append("};\n")
        out.append(
            "inline constexpr int %s_STEPS = (int)(sizeof %s / sizeof *%s);\n\n"
            % (name, name, name)
        )
    blocks, slices, report, owners = sliced_blocks(steps_by_name)
    if counts is not None:
        counts["_slices"] = (len(blocks), len(slices), report)
    out.append(SLICE_HEADER)
    for rid, off, stock, ln in slices:
        out.append(
            "    { %-44s %8d, 0x%08xu, %8d },\n"
            % (
                ("SLICE_GAP," if rid is None else "(uint16_t)mh::state::RID_%s," % rid),
                off,
                stock,
                ln,
            )
        )
    out.append("};\n")
    out.append(
        "inline constexpr int BLOCK_SLICE_COUNT = (int)(sizeof BLOCK_SLICES / sizeof *BLOCK_SLICES);\n\n"
    )
    out.append("inline constexpr sliced_block SLICED_BLOCKS[] = {\n")
    for addr, size, first, count, name in blocks:
        out.append('    { 0x%08xu, %8d, %4d, %3d, "%s" },\n' % (addr, size, first, count, name))
    out.append("};\n")
    out.append(
        "inline constexpr int SLICED_BLOCK_COUNT = "
        "(int)(sizeof SLICED_BLOCKS / sizeof *SLICED_BLOCKS);\n\n"
    )
    out.append(
        "// The staging buffer a sliced block is gathered into / scattered out of. Derived from the\n"
        "// table above, so it cannot be outgrown silently.\n"
        "inline constexpr uint32_t MAX_SLICED_BLOCK = %du;\n\n"
        % (max([b[1] for b in blocks]) if blocks else 0)
    )
    out.append(BLOCK_OWNER_HEADER)
    for addr, size, rid, off in owners:
        out.append(
            "    { 0x%08xu, %8d, %-44s %8d },\n"
            % (
                addr,
                size,
                (
                    "(uint16_t)mh::state::RID_COUNT,"
                    if rid is None
                    else "(uint16_t)mh::state::RID_%s," % rid
                ),
                off,
            )
        )
    out.append("};\n")
    out.append(
        "inline constexpr int BLOCK_OWNER_COUNT = "
        "(int)(sizeof BLOCK_OWNERS / sizeof *BLOCK_OWNERS);\n"
    )
    out.append(BLOCK_OWNER_LOOKUP)
    out.append(
        "// Each block's runs really do TILE it -- first run starts at the block, last run ends at\n"
        "// its end -- so a decomposition that lost or doubled a byte is a compile error rather than\n"
        "// a save written from the wrong memory.\n"
    )
    for addr, size, first, count, _n in blocks:
        out.append("static_assert(BLOCK_SLICES[%d].stock == 0x%08xu);\n" % (first, addr))
        out.append(
            "static_assert(BLOCK_SLICES[%d].stock + BLOCK_SLICES[%d].len == 0x%08xu);\n"
            % (first + count - 1, first + count - 1, addr + size)
        )
    out.append("\n")
    out.append("} // namespace mh::save::table\n")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="fail if the committed header is stale")
    a = ap.parse_args()

    tables = json.load(open(TABLE, encoding="utf-8"))["tables"]
    built = []
    for name, w, r in PROGRAMS:
        built.append((name, zip_directions(name, flatten(tables, w), flatten(tables, r))))
    counts = {}
    text = emit(built, counts)

    if a.check:
        have = open(OUT, encoding="utf-8", newline="").read() if os.path.exists(OUT) else ""
        if have.replace("\r\n", "\n") != text:
            raise SystemExit("STALE: %s does not match the table; re-run without --check" % OUT)
        for name, _ in built:
            print("  %-10s %3d steps" % (name, counts[name]))
        print("save_table.gen.h is up to date, and the writer/reader pair-check passes")
        return

    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    for name, _ in built:
        print("  %-10s %3d steps" % (name, counts[name]))
    print("wrote %s" % OUT)


if __name__ == "__main__":
    main()
