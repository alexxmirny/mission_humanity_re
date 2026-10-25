#!/usr/bin/env python3
"""scan_cap_path_slots.py -- PT-CAP9-U4a: the path-slot site census, every site classified.

WHY. U4 raises the per-player path pool 100 -> 500 slots (design: tmp/capraise/u4/REPORT.md, user decisions
2026-10-21). The pool is three arrays plus a byte in the unit record, and every access to them is an
instruction the patcher must either MOVE, SWITCH on the raise, or consciously leave alone:

  path_slot_id   unit record +0xaa (a byte; 0xff = no path), read by 54 MOVZX, checked by 17 CMP ,0xff, written by 4
  buffer         0xaee8c0  byte {heading,run}[8 players][100 slots][300 waypoints]; player stride 0xea60, slot 0x258
  flags          0xb74bb0  byte[8][100], player stride IMUL 0x64
  free count     0xb63bc0  int[8]
  data pointers  0x5d0ac0 -> flags, 0x5d0ac4 -> buffer (tactical row 0)
  hole           unit record +0xda..+0xdb, the 16-bit id's new home: NO instruction may touch it

Each site gets one POLICY (a column, derived from the class by `policy_of`, so a hand-edited verdict goes red):

  always_on        relocation: base / stride / size / data-pointer value. Applied whether or not the raise is ON.
  raise_switched   only behind the raise: the 6 scanner bounds, the free-count start value, the id reads (MOVZX
                   byte -> word at the new +0xda) and the id writes. Raise OFF never writes the hole.
  unchanged        stays as shipped: map_SavePlanetToDisk / map_LoadPlanetFromDisk keep the STOCK addresses as the
                   canonical mirror (the PT-CAP8 pattern), the CMP byte,0xff checks (the byte stays 0xff = no
                   path), the free-count INC/DEC/save/load, the two row-0 tactical loops, the pointer reads.

This tool applies NOTHING. U4b reads the table. The scanner takes an IMAGE PATH, so the same code answers
"does any stock pool site survive in this (patched) image": `--residual --exe PATCHED` (the U4b gate; the
clean exe lists every always_on site, a correctly patched image lists none).

DATA. tools/data/cap_path_slot_census.json. `--check` validates every invariant hermetically, re-derives the
whole table from the clean EN exe (md5 b3b389e8...) when it is on this machine (otherwise says so), and runs the
hook-overlap check (no mh.dll hook may overlap a U4 site). `--selftest` plants mutations that must go red.

usage:
  python tools/scan_cap_path_slots.py --check [--exe PATH]
  python tools/scan_cap_path_slots.py --selftest
  python tools/scan_cap_path_slots.py --gen [--exe PATH]
  python tools/scan_cap_path_slots.py --residual --exe IMAGE
  python tools/scan_cap_path_slots.py --report
"""

import argparse
import glob
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_cap_reloc_sites as scan  # noqa: E402

REPO = scan.REPO
CENSUS = os.path.join(scan.DATA, "cap_path_slot_census.json")
CLEAN_MD5 = "b3b389e8eb228638de8dbbc5c228118b"

PLAYERS = 8
UNITS_BASE = 0xDD8C48
UNITS_END = UNITS_BASE + PLAYERS * 23300
ID_OFF = 0xAA
HOLE = (0xDA, 0xDB)  # the new 16-bit id; must stay untouched
# Bytes the design lists as free in the unit record. 0xda-0xdb is the contract; the rest are reported.
FREE_BYTES = (0x83, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xDA, 0xDB, 0xE0, 0xE1)
BUF = (0xAEE8C0, 480000)
FLAGS = (0xB74BB0, 800)
COUNT = (0xB63BC0, 32)
DATA_PTRS = {
    0x5D0AC0: FLAGS[0],
    0x5D0AC4: BUF[0],
}  # address of the pointer dword -> what it points at
BUF_STRIDE = 0xEA60
BUF_SIZE = 0x75300
# Instructions address the buffer with a small NEGATIVE bias on the displacement (`[reg + base-2]` / `[reg + base-1]`: the
# group path-step recorder indexes entry*2 from a +2 origin). The first U4a census missed all 26 of them (its range test started
# at the base); relocating the buffer without them would read stale memory even with the raise OFF. 8 bytes is the window.
BUF_BIAS = 8
BUF_LO = BUF[0] - BUF_BIAS
BUF_SPAN = (
    BUF[1] + BUF_BIAS
)  # the biased window [BUF_LO, BUF_LO + BUF_SPAN) -- low side only (the high side is the free-count array)
FLAGS_STRIDE = 0x64
FLAGS_SIZE = 0x320
SAVE_LOAD = ("map_SavePlanetToDisk", "map_LoadPlanetFromDisk")
FILL = "map_FillDefaults"
# the six scanners (bound = the `cmp slot,0x64` of the 0..99 walk) -- raise-switched
SCANNERS = (
    "llm_strat_path_find_free_slot",
    "llm_strat_path_alloc_slot",
    "llm_strat_path_make_single_step",
    "llm_strat_unit_assign_path_from_job_result",
    "llm_strat_unit_assign_shared_path",
    "llm_strat_unit_state_plot_turn_path",
)
# two tactical row-0 walks over slots 1..99 (NOT in the design's list of six; found by this census)
TACT_BOUNDS = ("llm_tact_move_step_attempt", "llm_tact_map_reset")
# the two stack/register-passed slot values the design left open (see args_verdict in the data)
ARG_SITES = {0x48B0F3: "llm_strat_storage_exit_air", 0x48BA48: "llm_strat_storage_accept_landing"}

# Design numbers (tmp/capraise/u4/REPORT.md). They are FLOORS: a lower count means the census lost a site.
DESIGN = {
    "id_instructions": 75,
    "id_read": 54,
    "id_check": 17,
    "id_write": 4,
    "buf_base": 132,
    "buf_stride": 140,
    "flags_base": 19,
    "flags_stride": 16,
    "count_base": 5,
}
# The design also quoted "34 functions" for the 75 id instructions; the exe holds them in 26 (the 34 is not
# reproducible by any instruction-level reading -- see the report). Measured, and pinned as a floor of its own:
MEASURED_ID_FUNCTIONS = 26
# data dwords whose VALUE falls in a pool array but that are not pointers (byte-pattern coincidences)
NOT_POINTERS = {
    0x5074D4: "inside a table of 16-bit pairs (0x14e0130 0x16d 0x3c001f 0x79005b [0xb60098] 0xf400d5 ...) between "
    "the strtok/strtod ctype tables; no instruction addresses it",
    0x5110A4: "id-byte record table (0xb103 [0xb20400] 0xb3040000 0x4000000 0xb4 0xb504 ...): 0xb2 0xb3 0xb4 are "
    "the incrementing id of unaligned records, the same table as the units census' 0x511180; no references",
    0x5110B8: "same incrementing-id record table as 0x5110a4 ([0xb60400] 0xb7040000 0x4000000 0xb8 0xb904); "
    "no references",
    0x66BDEC: "x86 code bytes embedded in DGROUP (8b 1d fc .. 47 37 83 ... a copy of a code fragment, with "
    "0xb64bb0 beside it); an UNALIGNED 0x00b3820f, not a pointer; no instruction addresses it",
}
# mh.dll hook literals that sit in a U4 site's reach window but do not touch the site (value -> reason)
HOOK_OK = {
    0x41EC1D: "SITE_ATK_HEADING: MOV AL,[EBP-0x20] (3 bytes 0x41ec1d-0x41ec1f) ends where the buf_base site "
    "0x41ec20 begins; the hook flips ONE operand byte at 0x41ec1f, no detour",
    0x41EC1F: "the operand byte of that same MOV AL,[EBP-0x20] (E0 -> E4); one byte, inside the MOV",
}
HOOK_SRC_GLOBS = ("src/mh_dll/mh/**/*.cpp", "src/mh_dll/mh/**/*.h")
HOOK_REACH = 4  # a 5-byte detour at v clobbers v..v+4

# U4b: the sites that cannot be patched in place get a CAVE (a 5-byte JMP over a window of whole instructions, the window's tail
# NOP-padded). The census records the window of each, so the generator and its hermetic --check never need the exe for it.
WINDOW_CLASSES = ("flags_stride", "flag_clear_bound", "scanner_bound", "id_write")
WINDOW_MIN = 5
# attach_slot's id store is the second half of a pair: `mov al,[ebp-0x10]` (the slot, a dword on the stack) then the byte store. The
# raised cave replaces BOTH (it must store the 16-bit id, and the byte as min(id,0xfe)); the pair starts 3 bytes before the census row.
ATTACH_WINDOW = (0x496AB8, "8a45f08882f28cdd00")
COLS = ["va", "len", "foff", "w", "fn", "cls", "role", "policy", "ins", "hex"]
POLICIES = ("always_on", "raise_switched", "unchanged")


# ---- classification ------------------------------------------------------------------------------------------


def policy_of(row):
    """The ONE place a class becomes a policy. `check` recomputes it for every committed row."""
    cls, fn, role = row["cls"], row["fn"], row["role"]
    if cls in ("id_read", "id_write"):
        return "raise_switched"
    if cls in ("id_check", "tact_bound", "data_ptr_read", "count_base"):
        return "unchanged"
    if cls in ("scanner_bound", "count_init"):
        return "raise_switched"
    if cls == "data_ptr":
        return "always_on"
    if cls in ("buf_base", "buf_size", "flags_base", "flags_size", "flags_stride", "buf_stride"):
        return "unchanged" if fn in SAVE_LOAD else "always_on"
    if cls == "flag_clear_bound":
        return "always_on"
    raise ValueError("unclassified row %r" % (row,))


# ---- decoding ------------------------------------------------------------------------------------------------


def decode_all(img, funcs):
    """-> [(capstone instruction, function name | None)] over the whole executable image, recursive descent first
    and the linear sweep for what it did not reach (scan_cap_reloc_sites.decode_function's rule)."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out = []
    for sec in img.exec_sections():
        lo, hi = sec["va"], sec["va"] + sec["raw_size"]
        cur = lo
        for entry, end, name in funcs:
            if end < lo or entry >= hi:
                continue
            if entry > cur:
                _linear(md, img.read(cur, entry - cur), cur, None, out)
            code = img.read(entry, end - entry + 1)
            rd = scan.recursive_descent(md, code, entry, end, img)
            cover = {}
            for a, ins in rd.items():
                for k in range(ins.size):
                    cover[a + k] = a
            out.extend((ins, name) for ins in rd.values())
            lin = []
            _linear(md, code, entry, name, lin)
            out.extend(
                (i, n)
                for i, n in lin
                if i.address not in rd and not any((i.address + j) in cover for j in range(i.size))
            )
            cur = max(cur, end + 1)
        if cur < hi:
            _linear(md, img.read(cur, hi - cur), cur, None, out)
    out.sort(key=lambda x: x[0].address)
    return out


def _linear(md, code, start, name, out):
    off = 0
    while off < len(code):
        progressed = False
        for ins in md.disasm(code[off:], start + off):
            progressed = True
            off = ins.address - start + ins.size
            out.append((ins, name))
        if not progressed:
            off += 1


def _stack_regs():
    from capstone.x86 import X86_REG_EBP, X86_REG_ESP

    return (X86_REG_EBP, X86_REG_ESP)


STACK_REGS = _stack_regs()


def _in(v, base, size):
    return base <= v < base + size


def _row(ins, fn, foff, w, cls, role):
    r = {
        "va": ins.address,
        "len": ins.size,
        "foff": foff,
        "w": w,
        "fn": fn,
        "cls": cls,
        "role": role,
        "ins": "%s %s" % (ins.mnemonic, ins.op_str),
        "hex": bytes(ins.bytes).hex(),
    }
    r["policy"] = policy_of(r)
    return r


def _id_cls(ins, op):
    """-> (cls, role-suffix) for one memory operand that addresses unit +0xaa."""
    from capstone import CS_AC_WRITE

    m = ins.mnemonic
    if m == "cmp" and len(ins.operands) == 2 and ins.operands[1].type == 2:  # X86_OP_IMM
        return ("id_check" if (ins.operands[1].imm & 0xFF) == 0xFF else "id_other"), ""
    if m == "lea":
        return "id_other", ""
    if op.access & CS_AC_WRITE and m in ("mov",):
        return "id_write", ""
    if m in ("movzx", "mov", "movsx") and not op.access & CS_AC_WRITE:
        return "id_read", ""
    return "id_other", ""


def find_sites(img, decoded=None, funcs=None):
    """Every pool / path_slot_id site in the image at its STOCK address. -> (rows, extra) where extra carries the
    things that are not sites: hole candidates, id-overlap candidates, unmatched data dwords, unpaired imuls."""
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM

    funcs = funcs or scan.load_funcs()
    dec = decoded if decoded is not None else decode_all(img, funcs)
    rows, seen = [], set()

    def add(ins, fn, foff, w, cls, role):
        k = (ins.address, foff)
        if k in seen:
            return None
        seen.add(k)
        r = _row(ins, fn, foff, w, cls, role)
        rows.append(r)
        return r

    unit_fns = set()
    for ins, fn in dec:
        for op in ins.operands:
            if op.type == X86_OP_MEM:
                d = op.mem.disp & 0xFFFFFFFF
                if _in(d, UNITS_BASE, UNITS_END - UNITS_BASE) or (
                    d == ID_OFF and op.mem.base and ins.disp_size == 4
                ):
                    if fn:
                        unit_fns.add(fn)

    hole, overlap, imuls_flags, probe = [], [], [], []
    flag_mem = []  # (ins, fn) for IMUL pairing
    for ins, fn in dec:
        for op in ins.operands:
            if op.type == X86_OP_MEM:
                d = op.mem.disp & 0xFFFFFFFF
                dsz = ins.disp_size
                # ---- path_slot_id: unit +0xaa, pointer form [reg+0xaa] or direct form [units+0xaa (+ index regs)]
                if d == UNITS_BASE + ID_OFF or (
                    d == ID_OFF and op.mem.base and op.mem.base not in STACK_REGS
                ):
                    cls, _ = _id_cls(ins, op)
                    role = "direct" if d == UNITS_BASE + ID_OFF else "ptr"
                    add(ins, fn, ins.disp_offset, dsz, cls, role)
                # field offset inside a unit record: direct form = disp - units base; pointer form = disp itself,
                # only trusted in functions that touch the units array / the id (other structs share small offsets)
                if _in(d, UNITS_BASE, 0xE9):
                    off = d - UNITS_BASE
                elif (
                    op.mem.base
                    and op.mem.base not in STACK_REGS
                    and fn in unit_fns
                    and 0 <= d < 0x100
                ):
                    off = d
                else:
                    off = None
                # unrestricted probe (informational): any non-stack [reg+d] covering the hole, in any function
                if (
                    op.mem.base
                    and op.mem.base not in STACK_REGS
                    and d <= HOLE[1]
                    and d + max(op.size, 1) > HOLE[0]
                ):
                    probe.append(_cand(ins, fn, d, op.size))
                if off is not None:
                    # a WIDER neighbour access that also covers the id byte (dword at +0xa7..+0xaa, word at +0xa9)
                    if op.size > 1 and off < ID_OFF < off + op.size:
                        overlap.append(_cand(ins, fn, off, op.size))
                    # the hole: any access whose extent covers +0xda or +0xdb
                    if off <= HOLE[1] and off + max(op.size, 1) > HOLE[0]:
                        hole.append(_cand(ins, fn, off, op.size))
                # ---- buffer / flags / count (memory forms)
                if _in(d, BUF_LO, BUF_SPAN):
                    off = d - BUF[0]
                    role = {0: "heading", 1: "run"}.get(off) or (
                        "off%d" % off if off < 0 else "off+0x%x" % off
                    )
                    add(ins, fn, ins.disp_offset, dsz, "buf_base", role)
                if _in(d, *FLAGS):
                    r = add(ins, fn, ins.disp_offset, dsz, "flags_base", "mem")
                    if r:
                        flag_mem.append((ins, fn))
                if _in(d, *COUNT):
                    cnt_role = "mem"
                    add(ins, fn, ins.disp_offset, dsz, "count_base", cnt_role)
                if d in DATA_PTRS:
                    add(ins, fn, ins.disp_offset, dsz, "data_ptr_read", "0x%x" % d)
            elif op.type == X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
                if ins.imm_size == 4:
                    if _in(v, *BUF):
                        add(ins, fn, ins.imm_offset, 4, "buf_base", "imm")
                    if _in(v, *FLAGS):
                        add(ins, fn, ins.imm_offset, 4, "flags_base", "imm")
                    if _in(v, *COUNT):
                        add(ins, fn, ins.imm_offset, 4, "count_base", "imm")
                    if v == BUF_STRIDE and ins.mnemonic == "imul":
                        add(ins, fn, ins.imm_offset, 4, "buf_stride", "player")
                    if v == BUF_SIZE:
                        add(ins, fn, ins.imm_offset, 4, "buf_size", "bytes")
                if fn in SAVE_LOAD and v == FLAGS_SIZE and ins.imm_size == 4:
                    add(ins, fn, ins.imm_offset, 4, "flags_size", "bytes")
                if ins.mnemonic == "imul" and v == FLAGS_STRIDE:
                    imuls_flags.append((ins, fn))
        # the free-count start value: `mov dword ptr [reg+0xb63bc0], 100`
        if ins.mnemonic == "mov" and len(ins.operands) == 2 and ins.operands[0].type == X86_OP_MEM:
            d = ins.operands[0].mem.disp & 0xFFFFFFFF
            if _in(d, *COUNT) and ins.operands[1].type == X86_OP_IMM and ins.imm_size == 4:
                add(
                    ins,
                    fn,
                    ins.imm_offset,
                    4,
                    "count_init",
                    "start=%d" % (ins.operands[1].imm & 0xFFFFFFFF),
                )

    # flags stride: the IMUL ,0x64 that builds the row index within 24 bytes before a flags access, same function
    by_addr = {i.address: (i, f) for i, f in dec}
    unpaired_flag_sites = []
    paired = set()
    for ins, fn in flag_mem:
        got = None
        for im, f in imuls_flags:
            if f == fn and 0 < ins.address - im.address <= 24 and im.address not in paired:
                got = im
        if got is None:
            for im, f in imuls_flags:
                if f == fn and 0 < ins.address - im.address <= 24:
                    got = im
        if got is None:
            unpaired_flag_sites.append(ins.address)
            continue
        paired.add(got.address)
        foff = got.imm_offset
        add(got, fn, foff, got.imm_size, "flags_stride", "player")
    unpaired_imuls = [
        (im.address, f)
        for im, f in imuls_flags
        if im.address not in paired and f in {x[1] for x in flag_mem}
    ]

    # loop bounds
    flag_addrs = {}
    for fi, ff in flag_mem:
        flag_addrs.setdefault(ff, []).append(fi.address)
    fnidx = {}
    for ins, fn in dec:
        if fn:
            fnidx.setdefault(fn, []).append(ins)
    for fn in SCANNERS + (FILL,) + TACT_BOUNDS:
        for ins in fnidx.get(fn, []):
            if (
                ins.mnemonic == "cmp"
                and len(ins.operands) == 2
                and ins.operands[1].type == X86_OP_IMM
            ):
                if (ins.operands[1].imm & 0xFFFFFFFF) != 100:
                    continue
                if ins.operands[0].type == X86_OP_MEM:
                    # tie the compare to the walk: a flags access follows within 0x40 bytes
                    if not any(0 < a - ins.address <= 0x40 for a in flag_addrs.get(fn, ())):
                        continue
                    if fn in SCANNERS:
                        add(ins, fn, ins.imm_offset, ins.imm_size, "scanner_bound", "slots<100")
                    elif fn == FILL:
                        add(ins, fn, ins.imm_offset, ins.imm_size, "flag_clear_bound", "slots<100")
                    else:
                        add(
                            ins,
                            fn,
                            ins.imm_offset,
                            ins.imm_size,
                            "tact_bound",
                            "slots 1..99, row 0",
                        )

    # data dwords: the two pointers, and every other dword whose value lands in a pool array
    data_rows, data_other = [], []
    for s in img.data_sections():
        blob = img.read(s["va"], s["raw_size"])
        for k in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, k)[0]
            va = s["va"] + k
            if va in DATA_PTRS and v == DATA_PTRS[va]:
                data_rows.append(
                    {
                        "va": va,
                        "len": 4,
                        "foff": 0,
                        "w": 4,
                        "fn": None,
                        "cls": "data_ptr",
                        "role": "-> 0x%x" % v,
                        "ins": "dword in " + s["name"],
                        "hex": struct.pack("<I", v).hex(),
                    }
                )
            elif _in(v, *BUF) or _in(v, *FLAGS) or _in(v, *COUNT):
                data_other.append((va, v))
    for r in data_rows:
        r["policy"] = policy_of(r)
    rows.extend(data_rows)
    for r in rows:
        r["va"], r["len"] = int(r["va"]), int(r["len"])
    rows.sort(key=lambda r: (r["va"], r["foff"]))
    extra = {
        "hole_candidates": sorted({c["va"]: c for c in hole}.values(), key=lambda c: c["va"]),
        "hole_probe": sorted({c["va"]: c for c in probe}.values(), key=lambda c: c["va"]),
        "id_overlap_candidates": sorted(overlap, key=lambda c: c["va"]),
        "data_other": sorted(data_other),
        "unpaired_flag_sites": sorted(unpaired_flag_sites),
        "unpaired_imuls": sorted(unpaired_imuls),
        "unit_functions": sorted(unit_fns),
        "windows": build_windows(img, rows, dec),
    }
    return rows, extra


def _cand(ins, fn, off, size):
    return {
        "va": ins.address,
        "fn": fn,
        "off": off,
        "size": size,
        "ins": "%s %s" % (ins.mnemonic, ins.op_str),
    }


def branch_targets(dec):
    """Every direct branch / call target in the decoded image (a cave window must not be jumped into)."""
    from capstone.x86 import X86_GRP_CALL, X86_GRP_JUMP, X86_OP_IMM

    out = set()
    for ins, _fn in dec:
        if (X86_GRP_JUMP in ins.groups or X86_GRP_CALL in ins.groups) and ins.operands:
            if ins.operands[0].type == X86_OP_IMM:
                out.add(ins.operands[0].imm & 0xFFFFFFFF)
    return out


def cave_window(img, va, targets):
    """-> (start, bytes, [instruction texts]) the window of whole instructions from `va` that is at least WINDOW_MIN bytes
    long. Refuses (ValueError) on a call / ret / indirect transfer, on a branch that is not the last instruction, and on a
    branch target inside the window (a cave would swallow a join point)."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_GRP_CALL, X86_GRP_JUMP, X86_GRP_RET, X86_OP_IMM

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = img.read(va, 40)
    ins_l, pos = [], 0
    for i in md.disasm(code, va):
        ins_l.append(i)
        pos += i.size
        if pos >= WINDOW_MIN:
            break
    if pos < WINDOW_MIN:
        raise ValueError("0x%x: window shorter than %d bytes" % (va, WINDOW_MIN))
    for k, i in enumerate(ins_l):
        if X86_GRP_CALL in i.groups or X86_GRP_RET in i.groups:
            raise ValueError("0x%x: a call/ret in the window" % va)
        if X86_GRP_JUMP in i.groups:
            if k != len(ins_l) - 1:
                raise ValueError("0x%x: a branch inside the window (not last)" % va)
            if not i.operands or i.operands[0].type != X86_OP_IMM:
                raise ValueError("0x%x: an indirect branch in the window" % va)
        if k and i.address in targets:
            raise ValueError("0x%x: a branch target (0x%x) inside the window" % (va, i.address))
    return va, bytes(code[:pos]), ["%s %s" % (i.mnemonic, i.op_str) for i in ins_l]


def build_windows(img, rows, dec):
    targets = branch_targets(dec)
    out = {}
    for r in rows:
        if r["cls"] not in WINDOW_CLASSES:
            continue
        if r["cls"] == "id_write" and r["va"] == ATTACH_WINDOW[0] + 3:
            start = ATTACH_WINDOW[0]
            win = img.read(start, len(ATTACH_WINDOW[1]) // 2)
            if win.hex() != ATTACH_WINDOW[1]:
                raise ValueError("attach_slot window bytes changed: %s" % win.hex())
            if any(a in targets for a in range(start, start + len(win))):
                raise ValueError("attach_slot window is a branch target")
            out[start] = {
                "hex": win.hex(),
                "rows": [r["va"]],
                "ins": ["mov al, byte ptr [ebp - 0x10]", r["ins"]],
            }
            continue
        start, win, texts = cave_window(img, r["va"], targets)
        w = out.setdefault(start, {"hex": win.hex(), "rows": [], "ins": texts})
        w["rows"].append(r["va"])
    # every other census row that falls inside a window rides with it (a flags_base disp inside a flags-stride window)
    for start, w in out.items():
        end = start + len(w["hex"]) // 2
        w["rows"] = sorted({r["va"] for r in rows if start <= r["va"] < end} | set(w["rows"]))
    return out


# ---- derived facts -------------------------------------------------------------------------------------------


def counts(rows):
    c = {}
    for r in rows:
        c[r["cls"]] = c.get(r["cls"], 0) + 1
    ids = [r for r in rows if r["cls"].startswith("id_")]
    c["id_instructions"] = len({r["va"] for r in ids})
    c["id_functions"] = len({r["fn"] for r in ids if r["fn"]})
    c["buf_base_instructions"] = len({r["va"] for r in rows if r["cls"] == "buf_base"})
    pol = {}
    for r in rows:
        pol[r["policy"]] = pol.get(r["policy"], 0) + 1
    c["by_policy"] = dict(sorted(pol.items()))
    return dict(sorted(c.items()))


def args_verdict():
    """The two slot values the design left unverified. Derived by reading the disassembly (EN clean exe), recorded
    as a verdict + evidence; --gen re-confirms the structural facts it rests on (see `_verify_args`)."""
    return {
        "0x48b0f3": {
            "caller": "llm_strat_storage_exit_air",
            "callee": "llm_strat_storage_setup_exit_path (0x495cd3, __watcall + 2 stack dwords, RET 8)",
            "verdict": "int (32-bit dword end to end)",
            "evidence": [
                "slot = exit_air's 4th register param (ECX), spilled `MOV [EBP-0x20],ECX` (dword, 0x48b03b) and pushed "
                "`PUSH dword [EBP-0x20]` (0x48b0e0/0x48b0e3); the callee reads it as the dword [EBP+8]",
                "callee uses it as IMUL [EBP+8],0x258 (slot offset in the buffer, 0x495ea1/0x495ecf) and "
                "`MOV EBX,[EBP+8]` for attach_slot (0x495ee6, a dword in EBX)",
                "the value originates in exit_storage_begin: `CALL find_free_slot; MOV [EBP-0x1c],EAX; CMP [EBP-0x1c],-1`"
                " (0x47f04b..0x47f053), a signed dword",
            ],
            "u4_impact": "none: no byte/word truncation anywhere on the path; a slot > 255 flows through unchanged. "
            "The callee's own buffer writes (0x495eb7, 0x495edf) and attach call are already census rows.",
        },
        "0x48ba48": {
            "caller": "llm_strat_storage_accept_landing",
            "callee": "llm_strat_storage_setup_approach_path (0x495b6f, __watcall, 4 register params)",
            "verdict": "int (32-bit dword end to end)",
            "evidence": [
                "slot = accept_landing's 4th register param (ECX), spilled `MOV [EBP-0x10],ECX` (dword, 0x48ba38), "
                "passed in EBX (`MOV EBX,[EBP-0x10]`, 0x48ba3e) and stored `MOV [EBP-0x20],EBX` in the callee "
                "(0x495b8a)",
                "callee hands it on as `MOV EBX,[EBP-0x20]; CALL attach_slot` (0x495ca0/0x495caa) and indexes the "
                "buffer with the stack walk, never with the slot value",
                "the value originates in landing_request: `CALL find_free_slot; MOV [EBP-0x18],EAX; CMP [EBP-0x18],-1`"
                " (0x4801a2..0x4801aa), a signed dword",
            ],
            "u4_impact": "none, as above.",
        },
    }


# ---- the hook-overlap check (ported from tmp/capraise/u4/overlap.py) -----------------------------------------


def hook_overlaps(rows, repo=REPO, windows=None):
    """-> (overlaps, allowed): mh.dll source literals 0x4xxxxx that fall inside a U4 site's reach window
    [va - HOOK_REACH, va + len). A literal in HOOK_OK is reported as allowed with its reason. `windows` (the census'
    cave windows) widen the question: a cave overwrites the WHOLE window, not just the field."""
    sv = sorted((r["va"], r["len"], r["cls"], r["fn"]) for r in rows if r["va"] >= 0x410000)
    for k, w in (windows or {}).items():
        sv.append((int(k, 16), len(w["hex"]) // 2, "cave_window", None))
    sv.sort(key=lambda x: (x[0], x[1], x[2], x[3] or ""))
    bad, ok = set(), set()
    files = []
    for g in HOOK_SRC_GLOBS:
        files += glob.glob(os.path.join(repo, g), recursive=True)
    for f in sorted(files):
        if ".gen." in f:
            continue
        rel = os.path.relpath(f, repo).replace(os.sep, "/")
        try:
            t = open(f, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for m in re.finditer(r"0x(?:00)?(4[0-9a-fA-F]{5})(?![0-9a-fA-F])", t):
            v = int(m.group(1), 16)
            for a, ln, cls, fn in sv:
                if a - HOOK_REACH <= v < a + ln:
                    k = (rel, "0x%x" % v, "0x%x" % a, cls, fn)
                    (ok if v in HOOK_OK else bad).add(k)
    return sorted(bad), sorted(ok)


def hooked_functions(rows, repo=REPO):
    """Info: functions that hold U4 sites AND are named by an mh.dll literal at their ENTRY (whole-body thunks)."""
    funcs = {n: e for e, _, n in scan.load_funcs()}
    entries = {funcs[r["fn"]]: r["fn"] for r in rows if r["fn"] in funcs}
    hit = set()
    for g in HOOK_SRC_GLOBS:
        for f in glob.glob(os.path.join(repo, g), recursive=True):
            t = open(f, encoding="utf-8", errors="ignore").read()
            for m in re.finditer(r"0x(?:00)?(4[0-9a-fA-F]{5})(?![0-9a-fA-F])", t):
                v = int(m.group(1), 16)
                if v in entries:
                    hit.add((entries[v], "0x%x" % v))
    return sorted(hit)


# ---- build / persist -----------------------------------------------------------------------------------------


def build(exe_path):
    raw = open(exe_path, "rb").read()
    img = scan.Image(raw)
    funcs = scan.load_funcs()
    rows, extra = find_sites(img, None, funcs)
    return raw, rows, extra


def pack(rows):
    return [[r.get(c) for c in COLS] for r in rows]


def unpack(table):
    return [dict(zip(COLS, row)) for row in table]


def document(raw, rows, extra):
    allow_data = {a: NOT_POINTERS[a] for a in sorted(NOT_POINTERS)}
    doc = {
        "_generated_by": "tools/scan_cap_path_slots.py --gen",
        "_what": __doc__,
        "exe_md5": hashlib.md5(raw).hexdigest(),
        "design_floors": DESIGN,
        "counts": counts(rows),
        "columns": COLS,
        "windows": {hex(k): v for k, v in sorted(extra["windows"].items())},
        "sites": pack(rows),
        "hole": {
            "bytes": list(HOLE),
            "free_bytes_design": list(FREE_BYTES),
            "candidates": [
                {k: (hex(c[k]) if k == "va" else c[k]) for k in ("va", "fn", "off", "size", "ins")}
                for c in extra["hole_candidates"]
            ],
            "verdict": "no instruction touches +0xda..+0xdb"
            if not extra["hole_candidates"]
            else "TOUCHED",
            "probe_other_structs": [
                {k: (hex(c[k]) if k == "va" else c[k]) for k in ("va", "fn", "off", "size", "ins")}
                for c in extra["hole_probe"]
            ],
            "probe_note": "unrestricted probe: every non-stack [reg+d] covering +0xda/+0xdb in ANY function. These "
            "are in functions that never address the units array or the id byte (other structs); the contract "
            "check above is restricted to unit-touching functions plus the direct form [units+0xda].",
        },
        "id_overlap_candidates": [
            {k: (hex(c[k]) if k == "va" else c[k]) for k in ("va", "fn", "off", "size", "ins")}
            for c in extra["id_overlap_candidates"]
        ],
        "data_not_pointers": {hex(a): allow_data[a] for a in allow_data},
        "no_stride_pair": [hex(a) for a in extra["unpaired_flag_sites"]],
        "unpaired_imul64": [[hex(a), f] for a, f in extra["unpaired_imuls"]],
        "args_verdict": args_verdict(),
        "hook_ok": {hex(k): v for k, v in sorted(HOOK_OK.items())},
        "hooked_function_entries": hooked_functions(rows),
        "pool_functions_without_id": sorted(
            {r["fn"] for r in rows if r["fn"] and r["cls"] in ("buf_base", "flags_base")}
            - {r["fn"] for r in rows if r["cls"].startswith("id_")}
        ),
        "notes": [
            "design floors vs measured: see counts; ids are counted per instruction (an instruction has one id operand).",
            "tact_bound rows: llm_tact_move_step_attempt (the tactical scanner, slots 1..99) and llm_tact_map_reset "
            "(its reset) walk row 0 only; they are not in the design's list of six scanners. Policy unchanged "
            "(tactical never exceeds 100 slots); U4c decides whether a strategic->tactical->strategic round trip "
            "needs them to clear slots 100..500.",
            "flag_clear_bound (map_FillDefaults 0x45623c) is always_on: with the flags stride 501 the clear must "
            "cover the whole row; clearing flags that raise OFF never sets is a no-op.",
        ],
    }
    return doc


def dump_doc(doc):
    """indent=1 JSON with one site row per line (a 400-row table as 400 lines, not 4000)."""
    rest = {k: v for k, v in doc.items() if k != "sites"}
    head = json.dumps(rest, indent=1)
    body = ",\n".join("  " + json.dumps(r, separators=(",", ":")) for r in doc["sites"])
    return head[:-2] + ',\n "sites": [\n' + body + "\n ]\n}\n"


def load():
    return json.load(open(CENSUS, encoding="utf-8"))


def default_exe():
    return scan.default_exe()


def cmd_gen(exe):
    exe = exe or default_exe()
    if not exe or not os.path.isfile(exe):
        print("scan_cap_path_slots --gen: no clean EN exe (pass --exe or set MH_EN_CLEAN_EXE)")
        return 1
    raw, rows, extra = build(exe)
    if hashlib.md5(raw).hexdigest() != CLEAN_MD5:
        print(
            "scan_cap_path_slots --gen: %s is not the clean EN exe (md5 %s)"
            % (exe, hashlib.md5(raw).hexdigest())
        )
        return 1
    doc = document(raw, rows, extra)
    bad = validate(doc)
    if bad:
        print("scan_cap_path_slots --gen: refusing to write, invariants broken:")
        for b in bad:
            print("  " + b)
        return 1
    with open(CENSUS, "w", encoding="utf-8", newline="\n") as f:
        f.write(dump_doc(doc))
    print("wrote %s" % os.path.relpath(CENSUS, REPO))
    print_counts(doc)
    return 0


def print_counts(doc):
    c = doc["counts"]
    print(
        "path-slot census: %d id instructions in %d functions (read %d, check %d, write %d); buf_base %d, buf_stride %d, "
        "flags_base %d, flags_stride %d, count_base %d; policies %s"
        % (
            c["id_instructions"],
            c["id_functions"],
            c.get("id_read", 0),
            c.get("id_check", 0),
            c.get("id_write", 0),
            c.get("buf_base", 0),
            c.get("buf_stride", 0),
            c.get("flags_base", 0),
            c.get("flags_stride", 0),
            c.get("count_base", 0),
            c["by_policy"],
        )
    )


# ---- validation ----------------------------------------------------------------------------------------------


def validate(doc):
    """Hermetic invariants over a census document. -> list of problems (empty = green)."""
    bad = []
    rows = unpack(doc["sites"])
    keys = [(r["va"], r["foff"]) for r in rows]
    if len(set(keys)) != len(keys):
        bad.append("duplicate (va, field offset) rows")
    if keys != sorted(keys):
        bad.append("rows are not sorted by (va, foff)")
    for r in rows:
        try:
            want = policy_of(r)
        except ValueError as e:
            bad.append(str(e))
            continue
        if r["policy"] != want:
            bad.append(
                "0x%x %s: policy %s but the class rule says %s"
                % (r["va"], r["cls"], r["policy"], want)
            )
    c = counts(rows)
    if c != doc["counts"]:
        bad.append("counts block does not match the rows")
    floors = doc["design_floors"]
    for k, want in DESIGN.items():
        if floors.get(k) != want:
            bad.append("design_floors[%s] edited" % k)
        got = c.get(k, 0)
        if got < want:
            bad.append("%s: %d < the design floor %d (a site was lost)" % (k, got, want))
    if c.get("id_functions", 0) < MEASURED_ID_FUNCTIONS:
        bad.append(
            "id_functions: %d < the measured %d" % (c.get("id_functions", 0), MEASURED_ID_FUNCTIONS)
        )
    if c.get("id_other", 0):
        bad.append("%d id access(es) of an unclassified form (id_other)" % c["id_other"])
    # the structural sets
    have = {(r["fn"], r["cls"]) for r in rows}
    for fn in SCANNERS:
        if (fn, "scanner_bound") not in have:
            bad.append("scanner bound of %s missing" % fn)
    if (FILL, "flag_clear_bound") not in have:
        bad.append("map_FillDefaults flag-clear loop bound missing")
    if (FILL, "count_init") not in have:
        bad.append("map_FillDefaults free-count start value missing")
    for fn in SAVE_LOAD:
        for cls in ("buf_base", "flags_base", "count_base"):
            if (fn, cls) not in have:
                bad.append("%s %s missing" % (fn, cls))
    if sum(1 for r in rows if r["cls"] == "data_ptr") != len(DATA_PTRS):
        bad.append("the data-pointer dwords 0x5d0ac0 / 0x5d0ac4 are not both present")
    # scanner bounds are all raise_switched, the save/load mirror all unchanged
    for r in rows:
        if r["cls"] == "scanner_bound" and r["policy"] != "raise_switched":
            bad.append("scanner bound 0x%x not raise_switched" % r["va"])
        if r["fn"] in SAVE_LOAD and r["policy"] != "unchanged":
            bad.append("save/load site 0x%x not unchanged" % r["va"])
    # the hole
    if doc["hole"]["candidates"]:
        bad.append(
            "an instruction touches the hole +0xda..+0xdb: %s" % doc["hole"]["candidates"][:3]
        )
    if doc["id_overlap_candidates"]:
        bad.append("a wider access overlaps path_slot_id: %s" % doc["id_overlap_candidates"][:3])
    for k in ("no_stride_pair",):
        for a in doc[k]:
            if a not in ALLOWED_NO_STRIDE:
                bad.append("flags access %s has no IMUL ,0x64 row stride in front of it" % a)
    if doc["unpaired_imul64"]:
        bad.append(
            "IMUL ,0x64 in a flags function with no flags access: %s" % doc["unpaired_imul64"][:3]
        )
    # data dwords: every pool-range dword is a pointer row or an explained coincidence
    explained = {int(a, 16) for a in doc["data_not_pointers"]}
    if explained != set(NOT_POINTERS):
        bad.append("data_not_pointers does not match the tool's NOT_POINTERS table")
    # the args verdict
    for k in ARG_SITES:
        v = doc["args_verdict"].get("0x%x" % k)
        if not v or not v.get("verdict", "").startswith("int"):
            bad.append("args verdict for 0x%x missing or not int" % k)
    # the cave windows: one per window-class row, >= WINDOW_MIN bytes, the rows they list exist
    wins = doc.get("windows")
    if wins is None:
        bad.append("the census has no `windows` block (run --gen)")
    else:
        starts = sorted(int(k, 16) for k in wins)
        for r in rows:
            if r["cls"] in WINDOW_CLASSES and not any(
                int(k, 16) <= r["va"] < int(k, 16) + len(w["hex"]) // 2 for k, w in wins.items()
            ):
                bad.append("0x%x (%s) has no cave window" % (r["va"], r["cls"]))
        for k, w in wins.items():
            if len(w["hex"]) // 2 < WINDOW_MIN:
                bad.append("window %s is shorter than %d bytes" % (k, WINDOW_MIN))
        for a, b2 in zip(starts, starts[1:]):
            if b2 < a + len(wins[hex(a)]["hex"]) // 2:
                bad.append("windows 0x%x and 0x%x overlap" % (a, b2))
    # hook overlap
    over, _ok = hook_overlaps(rows, windows=doc.get("windows"))
    if over:
        bad.append("mh.dll hook literal(s) overlap a U4 site: %s" % over[:3])
    return bad


# tact_move_step_attempt reads flags with the row folded to 0 (no IMUL): the one stride-less flags access
ALLOWED_NO_STRIDE = ("0x494bbd",)


def cmd_check(exe):
    if not os.path.isfile(CENSUS):
        print("scan_cap_path_slots --check: %s missing (run --gen)" % os.path.relpath(CENSUS, REPO))
        return 1
    doc = load()
    bad = validate(doc)
    exe = exe or default_exe()
    rederived = "skipped (no clean EN exe on this machine)"
    if exe and os.path.isfile(exe):
        raw, rows, extra = build(exe)
        md5 = hashlib.md5(raw).hexdigest()
        if exe == default_exe() and md5 != CLEAN_MD5:
            bad.append("the clean EN exe %s has md5 %s, expected %s" % (exe, md5, CLEAN_MD5))
        elif md5 == CLEAN_MD5:
            fresh = document(raw, rows, extra)
            for key in (
                "sites",
                "windows",
                "counts",
                "hole",
                "id_overlap_candidates",
                "no_stride_pair",
                "unpaired_imul64",
            ):
                if json.dumps(fresh[key], sort_keys=True) != json.dumps(doc[key], sort_keys=True):
                    bad.append("committed %s differs from the exe re-scan (run --gen)" % key)
            rederived = "re-derived from %s, identical" % os.path.basename(os.path.dirname(exe))
    if bad:
        print("cap_path_slot_census FAILED:")
        for b in bad:
            print("  " + b)
        return 1
    print("cap_path_slot_census OK (%s)" % rederived)
    print_counts(doc)
    return 0


# ---- the U4b gate ----------------------------------------------------------------------------------------------


def residual(exe_path):
    """Scan an IMAGE for pool sites still at their STOCK value and report every one whose policy is always_on.
    Clean exe: all of them. A correctly U4b-patched image: none."""
    raw = open(exe_path, "rb").read()
    rows, _extra = find_sites(scan.Image(raw))
    left = [r for r in rows if r["policy"] == "always_on"]
    return left


def cmd_residual(exe):
    if not exe:
        print("scan_cap_path_slots --residual: pass --exe IMAGE")
        return 2
    left = residual(exe)
    for r in left:
        print("0x%08x %-16s %-9s %s  %s" % (r["va"], r["cls"], r["role"], r["fn"], r["ins"]))
    print("%d unpatched always_on pool site(s) in %s" % (len(left), exe))
    return 1 if left else 0


def cmd_report():
    doc = load()
    print_counts(doc)
    rows = unpack(doc["sites"])
    for p in POLICIES:
        print("\n== %s" % p)
        by = {}
        for r in rows:
            if r["policy"] == p:
                by[r["cls"]] = by.get(r["cls"], 0) + 1
        for k, v in sorted(by.items()):
            print("  %-18s %d" % (k, v))
    print("\nargs verdicts:")
    for k, v in doc["args_verdict"].items():
        print("  %s %s -> %s" % (k, v["caller"], v["verdict"]))
    print("hole: %s" % doc["hole"]["verdict"])
    return 0


# ---- selftest ------------------------------------------------------------------------------------------------


def selftest():
    import copy

    doc = load()
    fails = 0

    def expect_red(name, mutate, recount=False):
        nonlocal fails
        d = copy.deepcopy(doc)
        mutate(d)
        if (
            recount
        ):  # a careful editor fixes the counts block too: the design floors must still catch it
            d["counts"] = counts(unpack(d["sites"]))
        bad = validate(d)
        if not bad:
            print("SELFTEST FAIL: %s stayed green" % name)
            fails += 1
        else:
            print("selftest ok: %s -> red (%s)" % (name, bad[0][:90]))

    if validate(doc):
        print("SELFTEST FAIL: the committed census is not green: %s" % validate(doc)[:2])
        return 1

    def drop(d):
        t = d["sites"]
        i = next(k for k, r in enumerate(t) if r[COLS.index("cls")] == "buf_stride")
        del t[i]

    def mis(d):
        t = d["sites"]
        i = next(k for k, r in enumerate(t) if r[COLS.index("cls")] == "buf_stride")
        t[i][COLS.index("policy")] = "unchanged"

    def drop_scanner(d):
        t = d["sites"]
        i = next(k for k, r in enumerate(t) if r[COLS.index("cls")] == "scanner_bound")
        del t[i]

    def hole(d):
        d["hole"]["candidates"].append(
            {"va": "0x47cb54", "fn": "x", "off": 0xDA, "size": 2, "ins": "planted"}
        )

    def edit_floor(d):
        d["design_floors"]["buf_stride"] = 1

    def drop_ptr(d):
        t = d["sites"]
        i = next(k for k, r in enumerate(t) if r[COLS.index("cls")] == "data_ptr")
        del t[i]

    expect_red("a dropped buf_stride site", drop)
    expect_red("a dropped buf_stride site, counts block repaired", drop, recount=True)
    expect_red("a misclassified (policy-edited) site", mis)
    expect_red("a dropped scanner bound", drop_scanner)
    expect_red("a dropped scanner bound, counts block repaired", drop_scanner, recount=True)
    expect_red("a planted access to the hole", hole)
    expect_red("an edited design floor", edit_floor)
    expect_red("a dropped data-pointer row", drop_ptr)
    expect_red("a dropped data-pointer row, counts block repaired", drop_ptr, recount=True)

    # hook overlap: a source literal planted inside a site's window must be found
    import tempfile

    rows = unpack(doc["sites"])
    victim = next(r for r in rows if r["cls"] == "buf_stride")
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "src", "mh_dll", "mh"))
        open(os.path.join(td, "src", "mh_dll", "mh", "plant.cpp"), "w").write(
            "constexpr unsigned long long X = 0x%08x;\n" % (victim["va"] + 1)
        )
        over, _ = hook_overlaps(rows, repo=td)
        if not over:
            print("SELFTEST FAIL: a planted hook literal inside a site was not found")
            fails += 1
        else:
            print("selftest ok: a planted hook literal inside a site -> found")

    # exe-backed arms (skipped without the clean exe)
    exe = default_exe()
    if exe and os.path.isfile(exe) and hashlib.md5(open(exe, "rb").read()).hexdigest() == CLEAN_MD5:
        raw = open(exe, "rb").read()
        img = scan.Image(raw)
        funcs = scan.load_funcs()
        dec = decode_all(img, funcs)
        rows0, extra0 = find_sites(img, dec, funcs)
        if pack(rows0) != doc["sites"]:
            print("SELFTEST FAIL: re-scan differs from the committed table")
            fails += 1
        else:
            print("selftest ok: re-scan equals the committed table (%d rows)" % len(rows0))

        # plant an access to the hole into a copy of the image: id read disp 0xaa -> 0xda
        site = next(r for r in rows0 if r["cls"] == "id_read" and r["role"] == "ptr")
        buf = bytearray(raw)
        off = _va_to_off(img, site["va"] + site["foff"])
        assert struct.unpack_from("<I", buf, off)[0] == ID_OFF
        struct.pack_into("<I", buf, off, HOLE[0])
        _r, ex = find_sites(scan.Image(bytes(buf)))
        if not ex["hole_candidates"]:
            print("SELFTEST FAIL: a planted [reg+0xda] access in a unit function was not flagged")
            fails += 1
        else:
            print(
                "selftest ok: a planted [reg+0xda] access -> hole candidate at 0x%x"
                % ex["hole_candidates"][0]["va"]
            )

        # the residual gate: patch every always_on field (shift it clear of every pool array), then un-patch one
        always = [r for r in rows0 if r["policy"] == "always_on"]
        buf = bytearray(raw)
        for r in always:
            _shift_field(img, buf, r)
        left = residual_bytes(bytes(buf))
        if left:
            print("SELFTEST FAIL: a fully shifted image still shows %d stock sites" % len(left))
            fails += 1
        else:
            print(
                "selftest ok: an image with all %d always_on fields shifted -> residual 0"
                % len(always)
            )
        keep = always[len(always) // 2]
        buf2 = bytearray(buf)
        _restore_field(img, buf2, raw, keep)
        left = residual_bytes(bytes(buf2))
        if [x["va"] for x in left] != [keep["va"]]:
            print(
                "SELFTEST FAIL: one unpatched site was reported as %s"
                % [hex(x["va"]) for x in left]
            )
            fails += 1
        else:
            print(
                "selftest ok: one left-behind stock site (0x%x) -> the residual names exactly it"
                % keep["va"]
            )
        full = [r for r in residual(exe)]
        if len(full) != len(always):
            print(
                "SELFTEST FAIL: clean-exe residual %d != always_on rows %d"
                % (len(full), len(always))
            )
            fails += 1
        else:
            print("selftest ok: the clean exe's residual is every always_on row (%d)" % len(full))
    else:
        print("selftest: exe-backed arms skipped (no clean EN exe on this machine)")
    if fails:
        print("scan_cap_path_slots --selftest: %d FAILED" % fails)
        return 1
    print("scan_cap_path_slots --selftest OK")
    return 0


def residual_bytes(raw):
    rows, _ = find_sites(scan.Image(raw))
    return [r for r in rows if r["policy"] == "always_on"]


def _va_to_off(img, va):
    for s in img.secs:
        if s["va"] <= va < s["va"] + s["raw_size"]:
            return s["raw_off"] + (va - s["va"])
    raise ValueError("va 0x%x not in the image" % va)


SHIFT = 0x10000000  # moves a field far outside every pool array; imm strides/sizes move by +1 (never equal stock)


def _shift_field(img, buf, r):
    off = _va_to_off(img, r["va"] + r["foff"])
    w = r["w"]
    fmt = {1: "<B", 2: "<H", 4: "<I"}[w]
    v = struct.unpack_from(fmt, buf, off)[0]
    if r["cls"] in ("buf_stride", "buf_size", "flags_stride", "flags_size", "flag_clear_bound"):
        nv = (v + 1) & ((1 << (8 * w)) - 1)
    else:
        nv = (v + SHIFT) & 0xFFFFFFFF
    struct.pack_into(fmt, buf, off, nv)


def _restore_field(img, buf, raw, r):
    off = _va_to_off(img, r["va"] + r["foff"])
    buf[off : off + r["w"]] = raw[off : off + r["w"]]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gen", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--residual", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--exe")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.gen:
        return cmd_gen(a.exe)
    if a.check:
        return cmd_check(a.exe)
    if a.residual:
        return cmd_residual(a.exe)
    if a.report:
        return cmd_report()
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
