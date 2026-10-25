#!/usr/bin/env python3
"""gen_cap_orders_manifest.py -- patching:PT-CAP9-U5 (v0.2.3): the ORDER-LAYER enlargement of the cap raise, as GENERATED data.

A raised roster means one click can order hundreds of units, and the three order arrays are small and silent about it
(records are 0x44-byte `llm_strat_order`):

  array    stock extent                  vanilla bound                                   raised to
  queue    0xbb4ed0 x300  (+0x4fb0)      enqueue 0x466094 / release_due 0x46652e         4096   the due-now queue; hashed + saved
  pending  0xbb9e80 x1000 (+0x109a0)     schedule 0x466348 (local) / pending_enqueue     32768  the not-yet-due list; hashed, not saved
                                         0x466790 (remote: overflow RESETS the count)
  staging  0xbca920 x300  (+0x4f80)      stage_scheduled 0x466211 (sender: overflow       2048  the local click's records (move + 0xfb echo)
                                         returns 0 = the order is dropped)

ONE manifest, class `caps_orders` (a TOGGLE layer, flipped with the limit layer by mh::caps::set_limits so raise OFF is
byte-identical to vanilla): src/patcher/cap_v023_orders_EN.mh.patch.json. Its sites:

  base    one 4-byte disp32 / imm32 per operand that names an array: rewritten from the stock address to the same offset into
          `.mhcapo` (record strides are unchanged, so nothing else moves). ~209 sites in 9 functions.
  size    the FillDefaults clear of the queue (0x456138: `mov ebx,0x4fb0` -> the wide queue's bytes).
  limit   the four imm32 bounds `cmp dword [count], 300|1000` (0x4660b5 / 0x4665db queue, 0x466392 / 0x4667a8 pending).
  wipe    U5b: the two overflow WIPES (`mov dword [count],0` at 0x466153 enqueue, 0x4667f2 remote pending_enqueue) become
          `inc dword [overflow counter]` + nops -- the count is no longer reset, the order is simply refused and counted.
  quota   U5c: the sender-side staging bound (`cmp dword [0x5d01a4],300` at 0x466232) becomes `call .mhcapr thunk; test eax,eax`:
          the thunk jumps through a cell the DLL fills with its quota stub (accept = eax -1, refuse = 0), so the per-player
          quota and the player notice live in C++ while the patch stays data.

EXCLUDED on purpose (documented in EXCLUDE): the save/load block sites (0x447d32 / 0x44821e) keep the STOCK queue -- the stock
extent is the canonical mirror, gathered before a save and scattered after a load (D1, like every raised roster array); and the
two disp32 of the O1c local-decode carrier (0x46643d / 0x466451), which live INSIDE that carrier's 27-byte patch and are rebuilt
from the caps base by mh/seams/order_wide_index.cpp whenever the layer toggles.

Two sections: `.mhcapo` (zero-init RW: control cells, the three wide arrays, a guard after each) and `.mhcapr` (the one-page thunk).

  python tools/gen_cap_orders_manifest.py --write [--exe PATH]   # scan the clean EN exe, prove, write cache + manifest + layout header
  python tools/gen_cap_orders_manifest.py --check                # hermetic: re-derive from the cache, byte-compare, re-prove
  python tools/gen_cap_orders_manifest.py --selftest [--exe PATH]  # planted mutations must go red
  python tools/gen_cap_orders_manifest.py --report

Proofs (each a refusal): P1 every site's `expect` == the clean exe (cached); P2 sites sorted, distinct, non-overlapping, none inside
another cap manifest's site; P3 every base site's stock value lies inside exactly one array and its new value inside the same
array's new extent at the same offset; P4 CENSUS: every dword of BEGTEXT/DGROUP whose value falls inside a stock array extent is a
site, an EXCLUDEd site, a straddle of a decoded instruction, or a DGROUP coincidence (never "uncovered"); P5 the layout holds, the
manifest is refs_complete under tools/gen_inmem_manifest.py and its sections decode; P6 the new limits are consistent with the layout.
"""

import argparse
import bisect
import glob
import hashlib
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_inmem_manifest as gim  # noqa: E402
import scan_cap_reloc_sites as scan  # noqa: E402

REPO = scan.REPO
PATCHER = scan.PATCHER
NAME = "cap_v023_orders_EN"
PATH = os.path.join(PATCHER, NAME + ".mh.patch.json")
CACHE = os.path.join(REPO, "tools", "data", "cap_orders_sites.json")
LAYOUT_H = os.path.join(REPO, "src", "mh_dll", "mh", "caps", "orders_layout.gen.h")
# The cap chain this manifest sits behind: the static pipeline (mhpatch.add_bss_section) lays sections at the NEXT FREE VA of
# reloc22 -> limits22 -> aiscratch22 -> reloc23 -> limits23 -> orders23, so ours is derived from the sections of the manifests ahead
# of it rather than chosen. A section added to the chain AHEAD of this manifest moves both ours: re-run `--write`.
AHEAD = (
    "cap_v022_reloc_EN",
    "cap_v022_limits_EN",
    "cap_v022_aiscratch_EN",
    "cap_v023_units_reloc_EN",
    "cap_v023_units_limits_EN",
)
OTHER_CAPS = AHEAD

GUARD = 64
GUARD_BYTE = 0xA5
CTRL_BYTES = 0x1000
REC = 0x44

# name, stock base, elem bytes, stock count, new count, count variable, why
ARRAYS = [
    (
        "queue",
        0xBB4ED0,
        REC,
        300,
        4096,
        0x5D0194,
        "_G_LLM_STRAT_ORDER_QUEUE: the due-now queue (enqueue / release_due / queue_dispatch)",
    ),
    (
        "pending",
        0xBB9E80,
        REC,
        1000,
        32768,
        0x5D01A8,
        "the not-yet-due list (schedule / release_due / pending_enqueue)",
    ),
    (
        "staging",
        0xBCA920,
        REC,
        300,
        2048,
        0x5D01A4,
        "the local click's records (stage_scheduled / schedule)",
    ),
]
NAMES = [a[0] for a in ARRAYS]

# Control cells at the start of .mhcapo (dword each): offset, name, meaning.
CELLS = [
    (
        0x00,
        "thunk_cell",
        "the DLL stores its quota stub's address here before the layer goes ON; the .mhcapr thunk jumps through it",
    ),
    (0x04, "ovf_queue", "enqueue refused a record at the full queue (was: wipe the whole queue)"),
    (
        0x08,
        "ovf_pending",
        "remote pending_enqueue refused a record at the full list (was: reset the count = a desync)",
    ),
    (0x0C, "ovf_staging", "stage_scheduled refused a record (the DLL quota stub increments it)"),
    (0x10, "ovf_quota", "the subset of ovf_staging that was the per-player quota"),
]

# Static (non-scanned) sites: va, kind, expect bytes, what it is.
QUEUE_BYTES = 4096 * REC

# In-extent dwords that are NOT sites. -> reason
EXCLUDE = {
    0x447D32: "save: `mov eax,0xbb4ed0` + size 0x4fb0 in map_SavePlanetToDisk -- the save block keeps the STOCK queue (the canonical mirror, gathered before the save)",
    0x44821E: "load: the same in map_LoadPlanetFromDisk -- the stock queue is scattered into the wide one after the load",
    0x46643D: "O1c local-decode carrier (order_wide_index.cpp SITE_LOCAL, 27 bytes at 0x46643a): its pending disp32 is rebuilt from the caps base by the DLL on every toggle",
    0x466451: "O1c local-decode carrier, second disp32 (the store half of the same 27 bytes)",
}
# Instruction starts the linear sweep MISSES: queue_dispatch's two switch entries (the BLDG arm 0x467330 and the ADMIN arm 0x469691) sit right
# after an INLINE jump table (a byte index table + a dword address table), so capstone decodes the table bytes as code and swallows the first
# real instruction (`mov ax,[eax+queue+0xc]` / `+0xe`). Found 2026-10-21 when the first raise-ON burst of admin orders (0x34) was dequeued and
# never applied: the dispatcher read these two records from the STOCK queue. The sweep restarts at each address below and runs until it re-syncs.
RESYNC = (0x467330, 0x469691)

# DGROUP/BEGTEXT dwords that are data coincidences of a value inside an extent (reason).
COINCIDENCE_OK = {
    0x5110D6: "DGROUP table of 5-byte records `04 nn 00 00 00` (nn counts up from 0xb7): the bytes `00 04 bc 00` span two records",
}

STRADDLE = "straddle"
COINCIDENCE = "data_coincidence"


class Refuse(Exception):
    pass


def hx(b):
    return " ".join("%02x" % x for x in b)


def p32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


# ---- layout --------------------------------------------------------------------------------------


def _chain_end():
    end = 0
    for n in AHEAD:
        doc = json.load(open(os.path.join(PATCHER, n + ".mh.patch.json"), encoding="utf-8"))
        for sec in doc["sections"]:
            end = max(end, int(sec["vaddr"], 16) + int(sec["vsize"], 16))
    return (end + 0xFFFF) & ~0xFFFF  # the image's section alignment (mhpatch next_free_va)


def layout():
    """-> (arrays, o_vsize, o_va, q_va). arrays: dicts with stock/new geometry and the offset inside `.mhcapo`."""
    out, off = [], CTRL_BYTES
    for name, base, elem, scount, count, cvar, why in ARRAYS:
        size = elem * count
        out.append(dict(name=name, stock_base=base, elem=elem, stock_count=scount, count=count, count_var=cvar,
                        off=off, size=size, stock_size=elem * scount, why=why))  # fmt: skip
        off = (off + size + GUARD + 63) & ~63
    o_vsize = (off + 0xFFF) & ~0xFFF
    o_va = _chain_end()
    q_va = (o_va + o_vsize + 0xFFFF) & ~0xFFFF
    return out, o_vsize, o_va, q_va


def array_of(value, lay):
    for a in lay:
        if a["stock_base"] <= value < a["stock_base"] + a["stock_size"]:
            return a
    return None


def new_value(value, a, o_va):
    return o_va + a["off"] + (value - a["stock_base"])


def cell_va(o_va, name):
    for off, n, _w in CELLS:
        if n == name:
            return o_va + off
    raise KeyError(name)


# ---- the static sites ------------------------------------------------------------------------------


def static_sites(lay, o_va, q_va):
    """-> [dict(va, kind, exp, new, note)] -- the non-scanned sites, with the bytes the clean exe must hold."""
    A = {a["name"]: a for a in lay}
    S = []

    def add(va, kind, exp, new, note):
        assert len(exp) == len(new), (hex(va), len(exp), len(new))
        S.append(dict(va=va, kind=kind, exp=bytes(exp), new=bytes(new), note=note))

    # the FillDefaults clear of the queue: `bb b0 4f 00 00` mov ebx, 0x4fb0 (the imm is the size of the memset)
    add(
        0x456139,
        "size",
        p32(A["queue"]["stock_size"]),
        p32(A["queue"]["size"]),
        "map_FillDefaults: clear size of the queue",
    )
    # the four `cmp dword [count], imm32` bounds: the imm32 is the last 4 bytes of the 10-byte instruction
    add(
        0x4660BB,
        "limit",
        p32(300),
        p32(A["queue"]["count"]),
        "llm_strat_order_enqueue: queue bound",
    )
    add(
        0x4665E1,
        "limit",
        p32(300),
        p32(A["queue"]["count"]),
        "llm_strat_order_release_due: queue bound (carry over past it)",
    )
    add(
        0x466398,
        "limit",
        p32(1000),
        p32(A["pending"]["count"]),
        "llm_strat_order_schedule: pending bound (keep in staging past it)",
    )
    add(
        0x4667AE,
        "limit",
        p32(1000),
        p32(A["pending"]["count"]),
        "llm_strat_order_pending_enqueue: pending bound",
    )
    # U5b: the two wipes -> `inc dword [counter]` + 4 nops
    wq = b"\xc7\x05" + p32(0x5D0194) + p32(0)
    wp = b"\xc7\x05" + p32(0x5D01A8) + p32(0)
    add(
        0x466153,
        "wipe",
        wq,
        b"\xff\x05" + p32(cell_va(o_va, "ovf_queue")) + b"\x90" * 4,
        "llm_strat_order_enqueue: the full-queue WIPE (count = 0) becomes a counted refusal",
    )
    add(
        0x4667F2,
        "wipe",
        wp,
        b"\xff\x05" + p32(cell_va(o_va, "ovf_pending")) + b"\x90" * 4,
        "llm_strat_order_pending_enqueue: the remote-overflow count reset becomes a counted refusal",
    )
    # U5c: the sender-side staging bound -> call the thunk; the following `jge` (taken = refuse) keeps its meaning
    qx = b"\x81\x3d" + p32(0x5D01A4) + p32(300)
    rel = (q_va - (0x466232 + 5)) & 0xFFFFFFFF
    add(
        0x466232,
        "quota",
        qx,
        b"\xe8" + p32(rel) + b"\x85\xc0" + b"\x90" * 3,
        "llm_strat_order_stage_scheduled: staging bound -> the DLL quota stub through the .mhcapr thunk (eax -1 = accept, else refuse)",
    )
    return S


def thunk_code(o_va, q_va):
    """The one-page .mhcapr blob: `jmp dword ptr [thunk_cell]`, padded."""
    code = b"\xff\x25" + p32(cell_va(o_va, "thunk_cell"))
    return code + b"\xcc" * (16 - len(code))


# ---- exe scan --------------------------------------------------------------------------------------


def _sweep(md, d, base, ranges):
    covered = {}
    for lo, hi in ranges:
        o = lo - base
        while base + o < hi:
            ins = list(md.disasm(d[o : o + 15], base + o, 1))
            if not ins:
                o += 1
                continue
            covered[ins[0].address] = ins[0]
            o += ins[0].size
    return covered


def scan_exe(raw, img, lay):
    """-> (sites, census). sites: sorted dicts; census: every in-extent dword of BEGTEXT/DGROUP with its class."""
    import capstone

    funcs = scan.load_funcs()
    ents = [f[0] for f in funcs]

    def fn(va):
        i = bisect.bisect_right(ents, va) - 1
        return funcs[i][2] if i >= 0 and funcs[i][0] <= va < funcs[i][1] else None

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    text = next(s for s in img.secs if s["name"] == "BEGTEXT")
    d = raw[text["raw_off"] : text["raw_off"] + text["raw_size"]]
    base = text["va"]
    ranges, prev = [], base
    for lo, hi, _nm in funcs:
        if lo > prev and lo >= base:
            ranges.append((prev, lo))
        if lo >= base:
            ranges.append((lo, hi))
        prev = max(prev, hi)
    ranges.append((prev, base + len(d)))
    covered = _sweep(md, d, base, ranges)
    for start in (
        RESYNC
    ):  # re-decode from a known instruction start, dropping the misdecoded instructions it overlaps
        a, o = start, start - base
        while True:
            ins = list(md.disasm(d[o : o + 15], a, 1))
            if not ins:
                break
            i = ins[0]
            if a in covered and covered[a].size == i.size:
                break  # re-synced with the sweep
            for k in [k for k, v in covered.items() if k < a + i.size and a < k + v.size]:
                del covered[k]
            covered[a] = i
            a += i.size
            o += i.size

    sites, seen = [], set()
    for addr in sorted(covered):
        i = covered[addr]
        b = bytes(i.bytes)
        for kind, off, sz in (
            ("disp", i.disp_offset, i.disp_size),
            ("imm", i.imm_offset, i.imm_size),
        ):
            if sz != 4:
                continue
            v = u32(b, off)
            a = array_of(v, lay)
            if a is None:
                continue
            va = addr + off
            if va in seen:
                continue
            seen.add(va)
            sites.append(
                dict(
                    va=va,
                    array=a["name"],
                    old=v,
                    kind=kind,
                    fn=fn(addr),
                    insn="%s %s" % (i.mnemonic, i.op_str),
                )
            )
    starts = sorted(covered)

    def covering(va):
        j = bisect.bisect_right(starts, va) - 1
        if j >= 0:
            i = covered[starts[j]]
            if starts[j] <= va < starts[j] + i.size:
                return i
        return None

    fields = {}
    for s in sites:
        a = next(x for x in lay if x["name"] == s["array"])
        fields.setdefault(a["name"], set()).add((s["old"] - a["stock_base"]) % a["elem"])
    census = []
    for sec in img.secs:
        if sec["name"] not in ("BEGTEXT", "DGROUP"):
            continue
        data = raw[sec["raw_off"] : sec["raw_off"] + sec["raw_size"]]
        for o in range(len(data) - 3):
            v = u32(data, o)
            a = array_of(v, lay)
            if a is None:
                continue
            va = sec["va"] + o
            if va in seen:
                cls = "excluded" if va in EXCLUDE else "site"
            elif va in COINCIDENCE_OK:
                cls = COINCIDENCE
            elif sec["name"] == "BEGTEXT" and covering(va) is not None:
                cls = STRADDLE
            elif sec["name"] == "DGROUP" and (v - a["stock_base"]) % a["elem"] not in fields.get(
                a["name"], ()
            ):
                cls = COINCIDENCE
            else:
                cls = "UNCOVERED"
            census.append(dict(va=va, array=a["name"], value=v, sec=sec["name"], cls=cls))
    # excluded sites stay in `sites` (the cache records what the scan saw); derive() drops them
    return sites, census


# ---- manifest ----------------------------------------------------------------------------------


def clean_sha():
    return json.load(
        open(os.path.join(PATCHER, "cap_v022_limits_EN.mh.patch.json"), encoding="utf-8")
    )["sha256"]


def all_rows(cache, lay, o_va, q_va):
    """-> sorted rows: dict(va, exp, new, kind, note). The scanned base sites (minus EXCLUDE) + the static ones."""
    rows = []
    for s in cache["sites"]:
        if s["va"] in EXCLUDE:
            continue
        a = next(x for x in lay if x["name"] == s["array"])
        old = s["old"]
        rows.append(dict(va=s["va"], exp=p32(old), new=p32(new_value(old, a, o_va)), kind="base",
                         note="%s +0x%x (%s)" % (s["array"], old - a["stock_base"], s["fn"] or "undefined fn"),
                         array=s["array"], off=old - a["stock_base"], fn=s["fn"]))  # fmt: skip
    for s in static_sites(lay, o_va, q_va):
        rows.append(dict(s, array=None, off=0, fn=None))
    rows.sort(key=lambda r: r["va"])
    return rows


def render_doc(cache, lay, o_vsize, o_va, q_va):
    rows = all_rows(cache, lay, o_va, q_va)
    by_kind = {}
    for r in rows:
        by_kind[r["kind"]] = by_kind.get(r["kind"], 0) + 1
    sha = hashlib.sha256()
    for r in rows:
        sha.update(p32(r["va"]) + r["exp"] + b"|" + r["new"] + b"\n")
    doc = {
        "class": "caps_orders",
        "_class_why": (
            "v0.2.3 cap raise, ORDER layer (PT-CAP9-U5): `expect` is the vanilla bytes (raise OFF), `bytes` the raised ones (the three order "
            "arrays in `.mhcapo`, the wider bounds, the counted refusals, the sender quota thunk). Staged at boot, written with the limit "
            "layer at a quiescent point; never written when the raise is OFF."
        ),
        "toggle": True,
        "target": "mh.exe (English, clean retail)",
        "sha256": clean_sha(),
        "_purpose": (
            "GENERATED by tools/gen_cap_orders_manifest.py -- do not hand-edit. %d sites: %s. Arrays: queue 300->%d, pending 1000->%d, staging 300->%d."
            % (
                len(rows),
                ", ".join("%d %s" % (v, k) for k, v in sorted(by_kind.items())),
                lay[0]["count"],
                lay[1]["count"],
                lay[2]["count"],
            )
        ),
        "_proofs": {
            "sites": len(rows),
            "sites_sha256": sha.hexdigest(),
            "by_kind": by_kind,
            "guard_bytes": GUARD,
            "guard_fill": "0x%02X" % GUARD_BYTE,
            "ctrl_bytes": CTRL_BYTES,
            "cells": {n: "0x%02x" % off for off, n, _w in CELLS},
            "arrays": {
                a["name"]: dict(
                    stock_base="0x%08x" % a["stock_base"],
                    stock_count=a["stock_count"],
                    elem=a["elem"],
                    count=a["count"],
                    off="0x%06x" % a["off"],
                    bytes=a["size"],
                    count_var="0x%08x" % a["count_var"],
                )
                for a in lay
            },  # fmt: skip
            "exe": cache["exe"],
        },
        "sections": [
            {
                "name": ".mhcapo",
                "vaddr": "0x%x" % o_va,
                "vsize": "0x%x" % o_vsize,
                "chars": "0xC0000040",
            },
            {
                "name": ".mhcapr",
                "vaddr": "0x%x" % q_va,
                "vsize": "0x1000",
                "data": hx(thunk_code(o_va, q_va)),
                "chars": "0x60000020",
            },
        ],
        "patches": [
            {
                "va": "0x%08x" % r["va"],
                "expect": hx(r["exp"]),
                "bytes": hx(r["new"]),
                "_note": r["note"],
            }
            for r in rows
        ],
    }
    return json.dumps(doc, indent=1) + "\n"


def render_layout_h(lay, o_vsize):
    L = [
        "// GENERATED by tools/gen_cap_orders_manifest.py from tools/data/cap_orders_sites.json -- do not hand-edit.",
        "//",
        "// The `.mhcapo` section of src/patcher/cap_v023_orders_EN.mh.patch.json: the control cells, and where each enlarged order array",
        "// lives (offset from the section base), how big it is now, and the guard that follows it (patching:PT-CAP9-U5).",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace mh::caps::orders_layout {",
        "",
        "struct entry {",
        "    const char *name;",
        "    uint32_t    stock_base;  // the vanilla .bss address (the canonical mirror)",
        "    uint32_t    elem;        // bytes per record (0x44)",
        "    uint32_t    stock_count; // vanilla records",
        "    uint32_t    count;       // records now",
        "    uint32_t    off;         // from the section base",
        "    uint32_t    bytes;       // elem * count (the guard follows)",
        "    uint32_t    count_var;   // the game's live-count dword",
        "};",
        "",
        "inline constexpr uint32_t SECTION_VSIZE = 0x%x;" % o_vsize,
        "inline constexpr uint32_t CTRL_BYTES    = 0x%x;" % CTRL_BYTES,
        "inline constexpr uint32_t GUARD_BYTES   = %d;" % GUARD,
        "inline constexpr uint8_t  GUARD_FILL    = 0x%02X;" % GUARD_BYTE,
        "inline constexpr unsigned COUNT         = %d;" % len(lay),
        "",
        "// control cells (dword offsets from the section base)",
    ]
    for off, n, why in CELLS:
        L.append("inline constexpr uint32_t CELL_%-11s = 0x%02x; // %s" % (n.upper(), off, why))
    L += [
        "",
        "enum : unsigned { QUEUE   = 0,",
        "                  PENDING = 1,",
        "                  STAGING = 2 };",
        "",
        "inline constexpr entry ARRAYS[COUNT] = {",
    ]
    for a in lay:
        L.append(
            '    {"%s", 0x%08xu, %du, %du, %du, 0x%06xu, %du, 0x%08xu},'
            % (
                a["name"],
                a["stock_base"],
                a["elem"],
                a["stock_count"],
                a["count"],
                a["off"],
                a["size"],
                a["count_var"],
            )
        )
    L += ["};", "", "} // namespace mh::caps::orders_layout", ""]
    return "\n".join(L)


# ---- proofs ----------------------------------------------------------------------------------------


def _other_extents():
    out = []
    for n in OTHER_CAPS:
        for s in json.load(open(os.path.join(PATCHER, n + ".mh.patch.json"), encoding="utf-8"))[
            "patches"
        ]:
            va = int(s["va"], 16)
            out.append((va, va + len(s["expect"].split())))
    return sorted(out)


def prove(cache, lay, o_vsize, o_va, q_va, text=None):
    f = []
    rows = all_rows(cache, lay, o_va, q_va)
    if not rows:
        f.append("no sites")
    for x, y in zip(rows, rows[1:]):
        if y["va"] < x["va"] + len(x["exp"]):
            f.append("P2: sites overlap/duplicate at %#x / %#x" % (x["va"], y["va"]))
    caps = _other_extents()
    starts = [c[0] for c in caps]
    for r in rows:
        j = bisect.bisect_right(starts, r["va"] + len(r["exp"]) - 1) - 1
        for k in range(max(0, j - 2), min(len(caps), j + 3)):
            if caps[k][0] < r["va"] + len(r["exp"]) and r["va"] < caps[k][1]:
                f.append(
                    "P2: site %#x overlaps another cap manifest's site at %#x"
                    % (r["va"], caps[k][0])
                )
    for r in rows:
        if r["kind"] != "base":
            continue
        a = next(x for x in lay if x["name"] == r["array"])
        old, new = u32(r["exp"]), u32(r["new"])
        if array_of(old, lay) is not a:
            f.append("P3: site %#x stock value %#x is not inside %s" % (r["va"], old, a["name"]))
        lo = o_va + a["off"]
        if not lo <= new < lo + a["size"]:
            f.append(
                "P3: site %#x new value %#x outside %s's new extent" % (r["va"], new, a["name"])
            )
        if new - lo != old - a["stock_base"]:
            f.append("P3: site %#x offset into %s changed" % (r["va"], a["name"]))
    for a in lay:
        if not any(r["array"] == a["name"] for r in rows):
            f.append("P3: array %s has no site" % a["name"])
    if cache["exe"].get("exe_md5") != scan.EN_MD5:
        f.append("P1: the cached exe proof was not taken against the clean EN exe")
    if cache["exe"].get("p1_sites") != len(rows):
        f.append(
            "P1: the cached exe proof covers %s sites, the manifest has %d"
            % (cache["exe"].get("p1_sites"), len(rows))
        )
    for c in cache["census"]:
        if c["cls"] not in ("site", "excluded", STRADDLE, COINCIDENCE) or (
            c["cls"] == "excluded"
        ) != (c["va"] in EXCLUDE):
            f.append("P4: census %#x (%s %s) is %s" % (c["va"], c["array"], c["sec"], c["cls"]))
    site_vas = {r["va"] for r in rows}
    for c in cache["census"]:
        if (c["cls"] == "site") != (c["va"] in site_vas):
            f.append("P4: census class of %#x disagrees with the site list" % c["va"])
    for va in EXCLUDE:
        if not any(c["va"] == va and c["cls"] == "excluded" for c in cache["census"]):
            f.append("P4: excluded dword %#x is not in the census" % va)
    prev = CTRL_BYTES
    for a in lay:
        if a["off"] < prev:
            f.append("layout: %s overlaps the previous array+guard" % a["name"])
        prev = a["off"] + a["size"] + GUARD
    if prev > o_vsize:
        f.append("layout: .mhcapo is %#x, needs %#x" % (o_vsize, prev))
    for off, n, _w in CELLS:
        if off + 4 > CTRL_BYTES:
            f.append("layout: cell %s past the control block" % n)
    if q_va < o_va + o_vsize:
        f.append("layout: .mhcapr overlaps .mhcapo")
    # P6: the stock extents are what the rewrite assumes, the new bounds are consistent
    for a in lay:
        if a["stock_size"] != a["elem"] * a["stock_count"] or a["count"] <= a["stock_count"]:
            f.append("P6: %s geometry is inconsistent" % a["name"])
    if text is not None:
        tmp = tempfile.mkdtemp(prefix="capord_")
        path = os.path.join(tmp, NAME + ".mh.patch.json")
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        try:
            c = gim._classify(path)
        except gim.Refused as e:
            f.append("P5: REFUSED by gen_inmem_manifest: %s" % e)
        else:
            if not c["complete"]:
                f.append("P5: not refs_complete: %s" % c["why"])
    return f


# ---- modes -----------------------------------------------------------------------------------------


def load_exe(path=None):
    path = path or scan.default_exe()
    if not path or not os.path.isfile(path):
        raise Refuse("no clean EN exe (md5 %s); pass --exe" % scan.EN_MD5)
    raw = open(path, "rb").read()
    md5 = hashlib.md5(raw).hexdigest()
    if md5 != scan.EN_MD5:
        raise Refuse("%s is md5 %s, not the clean EN exe" % (path, md5))
    return raw, scan.Image(raw), md5


def build_cache(raw, img, md5):
    lay, _v, o_va, q_va = layout()
    sites, census = scan_exe(raw, img, lay)
    for s in sites:
        if img.read(s["va"], 4) != p32(s["old"]):
            raise Refuse("P1: site %#x does not hold %#x in the exe" % (s["va"], s["old"]))
    for s in static_sites(lay, o_va, q_va):
        if img.read(s["va"], len(s["exp"])) != s["exp"]:
            raise Refuse(
                "P1: static site %#x (%s) does not hold the expected %s in the exe"
                % (s["va"], s["kind"], hx(s["exp"]))
            )
    n = len([s for s in sites if s["va"] not in EXCLUDE]) + len(static_sites(lay, o_va, q_va))
    return {
        "_why": "GENERATED by tools/gen_cap_orders_manifest.py --write (needs the clean EN exe). The hermetic --check re-derives the manifest from this.",
        "exe": {"exe_md5": md5, "p1_sites": n},
        "sites": [
            dict(
                va=s["va"],
                array=s["array"],
                old=s["old"],
                kind=s["kind"],
                fn=s["fn"],
                insn=s["insn"],
            )
            for s in sites
        ],
        "census": census,
    }


def dump_cache(cache):
    L = [
        "{",
        '"_why": %s,' % json.dumps(cache["_why"]),
        '"exe": %s,' % json.dumps(cache["exe"]),
        '"sites": [',
    ]
    L.append(",\n".join(json.dumps(s) for s in cache["sites"]))
    L += ["],", '"census": [']
    L.append(",\n".join(json.dumps(c) for c in cache["census"]))
    L += ["]", "}", ""]
    return "\n".join(L)


def derive(cache):
    lay, o_vsize, o_va, q_va = layout()
    return (
        lay,
        o_vsize,
        o_va,
        q_va,
        render_doc(cache, lay, o_vsize, o_va, q_va),
        render_layout_h(lay, o_vsize),
    )


def cmd_write(exe):
    raw, img, md5 = load_exe(exe)
    cache = build_cache(raw, img, md5)
    lay, o_vsize, o_va, q_va, text, hdr = derive(cache)
    f = prove(cache, lay, o_vsize, o_va, q_va, text)
    if f:
        for x in f[:40]:
            print("  FAIL", x)
        print("gen_cap_orders_manifest --write: REFUSED (%d)" % len(f))
        return 1
    for path, body in ((CACHE, dump_cache(cache)), (PATH, text), (LAYOUT_H, hdr)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8", newline="\n").write(body)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, REPO), len(body)))
    per = {}
    for s in cache["sites"]:
        per[s["array"]] = per.get(s["array"], 0) + 1
    print("scanned %d base dwords %s (%d excluded); census %d dwords (%d straddle, %d coincidence)" % (
        len(cache["sites"]), per, len(EXCLUDE), len(cache["census"]),
        sum(c["cls"] == STRADDLE for c in cache["census"]), sum(c["cls"] == COINCIDENCE for c in cache["census"])))  # fmt: skip
    return 0


def cmd_check():
    if not os.path.isfile(CACHE):
        print(
            "gen_cap_orders_manifest --check: MISSING %s -- run --write"
            % os.path.relpath(CACHE, REPO)
        )
        return 1
    cache = json.load(open(CACHE, encoding="utf-8"))
    lay, o_vsize, o_va, q_va, text, hdr = derive(cache)
    f = prove(cache, lay, o_vsize, o_va, q_va, text)
    for path, body in ((PATH, text), (LAYOUT_H, hdr)):
        cur = open(path, encoding="utf-8").read() if os.path.isfile(path) else None
        if cur != body:
            f.append(
                "STALE: %s differs from the re-derivation (run --write)"
                % os.path.relpath(path, REPO)
            )
    if f:
        for x in f[:40]:
            print("  FAIL", x)
        print("gen_cap_orders_manifest --check: FAIL (%d)" % len(f))
        return 1
    rows = all_rows(cache, lay, o_va, q_va)
    print(
        "gen_cap_orders_manifest --check: ok (%d sites, %d arrays, %d census dwords, .mhcapo %#x @%#x)"
        % (len(rows), len(lay), len(cache["census"]), o_vsize, o_va)
    )
    return 0


def cmd_report():
    cache = json.load(open(CACHE, encoding="utf-8"))
    lay, o_vsize, o_va, q_va = layout()
    rows = all_rows(cache, lay, o_va, q_va)
    per = {}
    for r in rows:
        per[r["array"] or r["kind"]] = per.get(r["array"] or r["kind"], 0) + 1
    for a in lay:
        print(
            "  %-8s stock %#010x x%-5d -> %#07x +%8d B (x%d)  sites %d"
            % (
                a["name"],
                a["stock_base"],
                a["stock_count"],
                a["off"],
                a["size"],
                a["count"],
                per.get(a["name"], 0),
            )
        )
    print(
        "sections .mhcapo @%#x vsize %#x, .mhcapr @%#x; %d sites %s"
        % (o_va, o_vsize, q_va, len(rows), {k: v for k, v in per.items() if k not in NAMES})
    )
    return 0


def selftest(exe):
    cache = json.load(open(CACHE, encoding="utf-8"))
    lay, o_vsize, o_va, q_va = layout()
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    def mut(fn):
        c = json.loads(json.dumps(cache))
        fn(c)
        return prove(c, lay, o_vsize, o_va, q_va)

    expect(
        "the committed cache proves clean",
        prove(cache, lay, o_vsize, o_va, q_va, render_doc(cache, lay, o_vsize, o_va, q_va)) == [],
    )
    expect(
        "a site moved outside its array goes red",
        any("P3" in x for x in mut(lambda c: c["sites"][5].__setitem__("old", 0x1))),
    )
    expect(
        "a duplicated site goes red",
        any("P2" in x for x in mut(lambda c: c["sites"].append(dict(c["sites"][5])))),
    )
    expect(
        "an array with no site goes red",
        any(
            "has no site" in x
            for x in mut(
                lambda c: c.__setitem__("sites", [s for s in c["sites"] if s["array"] != "staging"])
            )
        ),
    )
    expect(
        "an uncovered census dword goes red",
        any(
            "P4" in x
            for x in mut(
                lambda c: c["census"].append(
                    dict(va=0x500000, array="queue", value=0xBB4ED0, sec="DGROUP", cls="UNCOVERED")
                )
            )
        ),
    )
    expect(
        "a stale exe proof goes red",
        any("P1" in x for x in mut(lambda c: c["exe"].__setitem__("p1_sites", 1))),
    )
    expect(
        "an excluded site missing from the census goes red",
        any(
            "excluded dword" in x
            for x in mut(
                lambda c: c.__setitem__("census", [x for x in c["census"] if x["va"] != 0x447D32])
            )
        ),
    )
    other = _other_extents()
    expect(
        "a site over another cap manifest's site goes red",
        any(
            "overlaps another cap" in x
            for x in mut(lambda c: c["sites"].append(dict(c["sites"][5], va=other[0][0])))
        ),
    )
    if exe or scan.default_exe():
        raw, img, md5 = load_exe(exe)
        fresh = build_cache(raw, img, md5)
        expect("a fresh scan of the exe reproduces the committed cache", fresh == cache)
        bad = bytearray(raw)
        s0 = cache["sites"][5]
        for sec in img.secs:
            if sec["va"] <= s0["va"] < sec["va"] + sec["raw_size"]:
                bad[sec["raw_off"] + s0["va"] - sec["va"]] ^= 0xFF
        try:
            other_c = build_cache(bytes(bad), scan.Image(bytes(bad)), md5)
            expect(
                "a site over wrong exe bytes is refused (or no longer reproduces the cache)",
                other_c != cache,
            )
        except Exception:
            expect(
                "a site over wrong exe bytes is refused (or no longer reproduces the cache)", True
            )
    print("gen_cap_orders_manifest --selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "ok"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--exe")
    a = ap.parse_args(argv)
    try:
        if a.write:
            return cmd_write(a.exe)
        if a.selftest:
            return selftest(a.exe)
        if a.report:
            return cmd_report()
        return cmd_check()
    except Refuse as e:
        print("gen_cap_orders_manifest: REFUSED:", e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
