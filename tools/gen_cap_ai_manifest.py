#!/usr/bin/env python3
"""gen_cap_ai_manifest.py -- patching:PT-CAP11: the AI scratch-array relocation layer of the v0.2.2 cap raise, as GENERATED data.

With `[caps] raise=1` an AI player can own more buildings / mines than vanilla allowed, and eight fixed AI scratch arrays then
overflow into whatever .bss global sits after them:

  array              stock extent                  bounded by                                  raised to
  conn_trial/base    byte[100] x2 @0x1013044/a8    NOTHING (indexed by buildings[player][i])   512 each (slot index < 500)
  mine_roster        int[32]                       NOTHING (collect walk, count local)         128   (<= 127 mines per player)
  mine_qual          12 B x 32                     NOTHING                                      128
  mine_yield         32 B x 32                     NOTHING                                      128
  engage_scratch     12 B x 256 @0xfb4db0          NOTHING (dedup + append, count global)       8192  (every object that can be listed)
  scan_targets_a/b   22 B x 1024 each @0x1017314   NOTHING (llm_strat_ai_scan_target_list_add)  8192 each
  ai_group_list      int[100] @0xe69ed0            NOTHING (group member chain copy; U3)        512   (<= 499 units per player)
  attack_cand        16 B x 128 @0x1022314         NOTHING (llm_strat_ai_attack_candidate_add)  512   (<= 499 idle units per group; U3)

(patching:PT-CAP9-U3 added the last two: the UNIT-indexed AI scratch. A 1-based-address site, `lea esi,[eax+0x1022304]` at 0x4ed02b, is declared in
BIASED: its stock value is attack_cand's base - 16.)

(ENGAGE_CANDIDATE_SOURCE_LIST @0xfb26b0 is NOT here: its only appender, the unnamed function at 0x4ed95f, CMPs the count with
0x100 at 0x4ed97f and stops -- bounded, no overflow.)

ONE manifest, class `caps_ai` (a TOGGLE layer, like the limit layer): src/patcher/cap_v022_aiscratch_EN.mh.patch.json. Each site is a
single 4-byte disp32 / imm32 whose value is rewritten from the stock array address to the same offset into the `.mhcapa` section
(element strides are unchanged, so nothing else moves). `expect` is the vanilla bytes (raise OFF: the manifest is never written, the
game is byte-identical to vanilla), `bytes` the relocated ones; mh.dll stages them at boot and flips them with the limit layer
(mh::caps::set_limits) at a quiescent point, when every scratch array is idle. The release ships WITHOUT libmh.dll: this is a retail
splice.

`.mhcapa` is laid out array after array, each followed by GUARD bytes (0xA5 fill, written by mh/caps/ai_scratch.cpp); the per-step
guard check in the harness turns an overflow past the NEW size into a red verdict.

  python tools/gen_cap_ai_manifest.py --write [--exe PATH]   # scan the clean EN exe, prove, write cache + manifest + layout header
  python tools/gen_cap_ai_manifest.py --check                # hermetic: re-derive from the cache, byte-compare, re-prove
  python tools/gen_cap_ai_manifest.py --selftest [--exe PATH]  # planted mutations must go red
  python tools/gen_cap_ai_manifest.py --report

Proofs (each a refusal): P1 every site's `expect` == the clean exe (cached); P2 sites sorted, distinct, non-overlapping, none inside a
cap_v022 reloc/limit site; P3 every site's stock value lies inside exactly one array and its new value inside the same array's new
extent at the same offset; P4 CENSUS: every dword in BEGTEXT/DGROUP whose value falls inside a stock array extent is a site, a
straddle of a decoded instruction, or a DGROUP coincidence that is NOT aligned to a field of the array (never "uncovered"); P5 the
section holds the layout with guards and the manifest is refs_complete under tools/gen_inmem_manifest.py.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_cap_reloc_sites as scan  # noqa: E402
import gen_inmem_manifest as gim  # noqa: E402

REPO = scan.REPO
PATCHER = scan.PATCHER
NAME = "cap_v022_aiscratch_EN"
PATH = os.path.join(PATCHER, NAME + ".mh.patch.json")
CACHE = os.path.join(REPO, "tools", "data", "cap_ai_scratch_sites.json")
LAYOUT_H = os.path.join(REPO, "src", "mh_dll", "mh", "caps", "ai_scratch_layout.gen.h")
LIMITS_PATH = os.path.join(PATCHER, "cap_v022_limits_EN.mh.patch.json")
RELOC_PATH = os.path.join(PATCHER, "cap_v022_reloc_EN.mh.patch.json")

SEC = (
    ".mhcapa",
    0x01350000,
    0xC0000040,
)  # zero-init, RW; 64K-aligned next-free VA after .mhcapl (0x1340000+0x1000)
GUARD = 64
GUARD_BYTE = 0xA5

# name, stock base, elem bytes, stock count, new count, why
ARRAYS = [
    (
        "conn_trial",
        0x1013044,
        1,
        100,
        512,
        "llm_strat_ai_plan_turret_upgrade: byte flag per buildings[player] slot (trial flood fill)",
    ),
    (
        "conn_base",
        0x10130A8,
        1,
        100,
        512,
        "llm_strat_ai_plan_turret_upgrade: byte flag per buildings[player] slot (base flood fill)",
    ),
    (
        "mine_roster",
        0x101310C,
        4,
        32,
        128,
        "llm_strat_ai_mine_portfolio_rebalance: roster slot per collected mine",
    ),
    (
        "mine_qual",
        0x101318C,
        12,
        32,
        128,
        "llm_strat_ai_mine_portfolio_rebalance: near/mid/far histogram per collected mine",
    ),
    (
        "mine_yield",
        0x101330C,
        32,
        32,
        128,
        "llm_strat_ai_mine_portfolio_rebalance: per-resource yield per collected mine",
    ),
    (
        "engage_scratch",
        0xFB4DB0,
        12,
        256,
        8192,
        "llm_strat_ai_engage_candidate_add: (ref,index,dist) per listed target",
    ),
    (
        "scan_targets_a",
        0x1017314,
        22,
        1024,
        8192,
        "llm_strat_ai_scan_target_list_add: first list (the global AI target scratch)",
    ),
    (
        "scan_targets_b",
        0x101CB14,
        22,
        1024,
        8192,
        "llm_strat_ai_build_target_list & co: the second list (stock 0x1017314 + 1024*22)",
    ),
    # patching:PT-CAP9-U3 (units raise): the two UNIT-indexed AI scratch arrays. Both are filled from an AI group's member chain
    # (units[p][i].ai_group_next) with NO bound, and a group can hold every unit the player owns (the holding pen, group 0, holds all
    # fresh units), so 500 units per player overflow them.
    (
        "ai_group_list",
        0xE69ED0,
        4,
        100,
        512,
        "llm_strat_ai_group_task_advance_to_anchor / _disperse_passable / _reposition_members: the group's member unit ids (int[100], count 0xfe5b48)",
    ),
    (
        "attack_cand",
        0x1022314,
        16,
        128,
        512,
        "llm_strat_ai_attack_candidate_add: one 16 B record per idle unit of the ticked group (count 0x1022b1c), scored in llm_strat_ai_active_unit_tick",
    ),
]
# Stock extents are [base, base + elem*count); the neighbours are other globals that stay where they are.
NAMES = [a[0] for a in ARRAYS]

# Raw-dword census classes that are accepted for a dword inside a stock extent that is NOT a site.
STRADDLE = "straddle"  # inside a decoded instruction, not at an operand dword (offset coincidence)
COINCIDENCE = (
    "data_coincidence"  # DGROUP dword whose offset into the array is not aligned to any field of it
)


# In-extent dwords that are NOT references to the array (the value is a neighbour's displacement that happens to fall inside).
EXCLUDE = {}

# Sites whose stock value is an array base plus a constant BIAS (the compiler folded `count*16 - 16` into the displacement): the value lies OUTSIDE the
# array's own extent (here in scan_targets_b's last 16 bytes by address only), so array_of() cannot find it. va -> (array, bias). patching:PT-CAP9-U3.
BIASED = {
    # llm_strat_ai_active_unit_tick: `lea esi,[eax+0x1022304]` with eax = count*16, the source of the memcpy that moves the LAST record (count-1) into the
    # slot just committed: base - 16 + count*16. (PT-CAP11 listed it in EXCLUDE: it is not scan_targets_b's.)
    0x4ED02B: ("attack_cand", -16),
}


# patching:PT-CAP9-U6 -- ONE code splice rides in this toggle layer (it is a bound repair behind the raise, like the scratch arrays): the control-group /
# selection append `llm_strat_unit_ctrlgroup_add_member` (0x00449401) writes unit_ids[*count] with NO bound; a group record is count + ushort[200]
# (404 B), so with more than 200 own units (raised units) a box select writes past the record into the next group's count. The clamp: when *count >= 200 the
# function does nothing (the unit is not selected). The new body is the SAME LENGTH as the old (52 bytes from the `sub esp`), so every address after it
# stays put: `sub esp,0xc` is re-encoded 83 ec 0c (3 B instead of 6) and the saved bytes buy the check; 5 bytes of padding keep the length exact.
#   83 ec 0c            sub  esp,0xc
#   89 45 e8 / 89 55 ec / 89 5d f0   spill eax (unit id) / edx (&count) / ebx (group index)
#   8b 0a               mov  ecx,[edx]                      ; count
#   81 f9 c8 00 00 00   cmp  ecx,0xc8
#   73 4f               jae  0x449476                       ; the original epilogue (lea esp,[ebp-0xc]; pop edi/esi/ecx/ebp; ret)
#   69 75 f0 94 01 00 00  imul esi,[ebp-0x10],0x194          ; group * 404
#   8d 34 4e            lea  esi,[esi+ecx*2]
#   66 89 86 e4 3b b6 00  mov [esi+0xb63be4],ax              ; unit_ids[count] = unit id (ax still holds it)
#   ff 02               inc  dword ptr [edx]
#   0f 1f 44 00 00      nop  (5 B padding)
#   85 db / 66 90       test ebx,ebx (ebx still holds the group index) / 2-byte nop
#   74 1f               je   +0x1f                          ; UNCHANGED from the original (same address, same target 0x449464)
# (the original `cmp [ebp-0x10],0` is a dword whose bytes 83 7d f0 00 happen to read as a rel32 landing inside .mhcapa, which the in-memory
# manifest classifier's conservative value scan refuses; `test ebx,ebx` is the same flags and clean.)
TOGGLE_CHUNK = 26  # a toggle site is at most inmem_patch.h MAX_TOGGLE_LEN (32) bytes: the 52-byte body is emitted as two adjacent 26-byte sites
CODE_PATCHES = [
    dict(
        va=0x449411,
        fn="llm_strat_unit_ctrlgroup_add_member",
        epilogue=0x449476,
        keep_tail=2,
        new=bytes.fromhex(
            "83ec0c"
            "8945e8"
            "8955ec"
            "895df0"
            "8b0a"
            "81f9c8000000"
            "734f"
            "6975f094010000"
            "8d344e"
            "668986e43bb600"
            "ff02"
            "0f1f440000"
            "85db"
            "6690"
            "741f"
        ),  # fmt: skip
    ),
]


class Refuse(Exception):
    pass


def hx(b):
    return " ".join("%02x" % x for x in b)


def p32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


# ---- layout --------------------------------------------------------------------------------------


def layout():
    """-> ([dict(name, stock_base, elem, stock_count, count, off, size, stock_size)], vsize)"""
    out, off = [], 0
    for name, base, elem, scount, count, why in ARRAYS:
        size = elem * count
        out.append(dict(name=name, stock_base=base, elem=elem, stock_count=scount, count=count, off=off,
                        size=size, stock_size=elem * scount, why=why))  # fmt: skip
        off = (off + size + GUARD + 63) & ~63
    vsize = (off + 0xFFF) & ~0xFFF
    return out, vsize


def array_of(value, lay):
    for a in lay:
        if a["stock_base"] <= value < a["stock_base"] + a["stock_size"]:
            return a
    return None


def new_value(value, a, sec_va=None):
    sec_va = SEC[1] if sec_va is None else sec_va
    return sec_va + a["off"] + (value - a["stock_base"])


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
    import bisect
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
            va = addr + off
            bias = 0
            if va in BIASED:
                a = next(x for x in lay if x["name"] == BIASED[va][0])
                bias = BIASED[va][1]
                if v != a["stock_base"] + bias:
                    raise Refuse("biased site %#x holds %#x, not %s%+d" % (va, v, a["name"], bias))
            else:
                a = array_of(v, lay)
            if a is None:
                continue
            if va in seen or va in EXCLUDE:
                continue
            seen.add(va)
            site = dict(
                va=va,
                array=a["name"],
                old=v,
                kind=kind,
                fn=fn(addr),
                insn="%s %s" % (i.mnemonic, i.op_str),
            )
            if bias:
                site["bias"] = bias
            sites.append(site)
    # the census: a raw dword scan of the initialised non-code sections that can hold such a value
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
            continue  # .bss reads file offset 0.. (a mirror of BEGTEXT), .rsrc/.idata/.reloc hold no game addresses
        data = raw[sec["raw_off"] : sec["raw_off"] + sec["raw_size"]]
        for o in range(len(data) - 3):
            v = u32(data, o)
            a = array_of(v, lay)
            if sec["va"] + o in BIASED:
                a = next(x for x in lay if x["name"] == BIASED[sec["va"] + o][0])
            if a is None:
                continue
            va = sec["va"] + o
            if va in seen:
                cls = "site"
            elif va in EXCLUDE:
                cls = "excluded"
            elif sec["name"] == "BEGTEXT" and covering(va) is not None:
                cls = STRADDLE
            elif sec["name"] == "DGROUP" and (v - a["stock_base"]) % a["elem"] not in fields.get(
                a["name"], ()
            ):
                cls = COINCIDENCE
            else:
                cls = "UNCOVERED"
            census.append(dict(va=va, array=a["name"], value=v, sec=sec["name"], cls=cls))
    return sites, census


# ---- manifest ----------------------------------------------------------------------------------


def clean_sha():
    return json.load(open(LIMITS_PATH, encoding="utf-8"))["sha256"]


def site_rows(cache, lay):
    rows = []
    for s in cache["sites"]:
        a = next(x for x in lay if x["name"] == s["array"])
        old, new = s["old"], new_value(s["old"], a)
        rows.append(dict(va=s["va"], exp=p32(old), new=p32(new), array=s["array"], off=s["old"] - a["stock_base"],
                         fn=s["fn"], kind=s["kind"], insn=s["insn"], bias=s.get("bias", 0)))  # fmt: skip
    rows.sort(key=lambda r: r["va"])
    return rows


def render_doc(cache, lay, vsize):
    rows = site_rows(cache, lay)
    per = {}
    for r in rows:
        per[r["array"]] = per.get(r["array"], 0) + 1
    sha = hashlib.sha256()
    for r in rows:
        sha.update(p32(r["va"]) + r["exp"] + b"|" + r["new"] + b"\n")
    doc = {
        "class": "caps_ai",
        "_class_why": (
            "v0.2.2 cap raise, AI SCRATCH layer (PT-CAP11): `expect` is the vanilla bytes (raise OFF), `bytes` the same dword pointing into "
            "`.mhcapa`. Staged at boot, written with the limit layer at a quiescent point; never written when the raise is OFF."
        ),
        "toggle": True,
        "target": "mh.exe (English, clean retail)",
        "sha256": clean_sha(),
        "_purpose": (
            "GENERATED by tools/gen_cap_ai_manifest.py -- do not hand-edit. %d sites moving %d AI scratch arrays into .mhcapa "
            "(elements and strides unchanged, so only the base dword moves)."
            % (len(rows), len(lay))
        ),
        "_proofs": {
            "sites": len(rows),
            "sites_sha256": sha.hexdigest(),
            "per_array": per,
            "guard_bytes": GUARD,
            "guard_fill": "0x%02X" % GUARD_BYTE,
            "arrays": {
                a["name"]: dict(
                    stock_base="0x%08x" % a["stock_base"],
                    stock_count=a["stock_count"],
                    elem=a["elem"],
                    count=a["count"],
                    off="0x%06x" % a["off"],
                    bytes=a["size"],
                )
                for a in lay
            },  # fmt: skip
            "exe": cache["exe"],
        },
        "sections": [
            {
                "name": SEC[0],
                "vaddr": "0x%x" % SEC[1],
                "vsize": "0x%x" % vsize,
                "chars": "0x%X" % SEC[2],
            }
        ],
        "patches": sorted(
            [
                {
                    "va": "0x%08x" % r["va"],
                    "expect": hx(r["exp"]),
                    "bytes": hx(r["new"]),
                    "_note": "%s +0x%x (%s)" % (r["array"], r["off"], r["fn"] or "undefined fn"),
                }
                for r in rows
            ]
            + [
                {
                    "va": "0x%08x" % (c["va"] + o),
                    "expect": hx(bytes.fromhex(c["exp"])[o : o + TOGGLE_CHUNK]),
                    "bytes": hx(bytes.fromhex(c["new"])[o : o + TOGGLE_CHUNK]),
                    "_note": "CODE (PT-CAP9-U6): %s +0x%x -- clamp the append at 200 ids (see CODE_PATCHES)"
                    % (c["fn"], o),
                }
                for c in cache.get("code", [])
                for o in range(0, len(c["new"]) // 2, TOGGLE_CHUNK)
            ],
            key=lambda p: int(p["va"], 16),
        ),
    }
    doc["_proofs"]["code_patches"] = len(cache.get("code", []))
    return json.dumps(doc, indent=1) + "\n"


def render_layout_h(lay, vsize):
    L = [
        "// GENERATED by tools/gen_cap_ai_manifest.py from tools/data/cap_ai_scratch_sites.json -- do not hand-edit.",
        "//",
        "// The `.mhcapa` section of src/patcher/cap_v022_aiscratch_EN.mh.patch.json: where each relocated AI scratch array lives",
        "// (offset from the section base), how big it is now, and the guard that follows it (patching:PT-CAP11).",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace mh::caps::ai_scratch_layout {",
        "",
        "struct entry {",
        "    const char *name;",
        "    uint32_t    stock_base;  // the vanilla .bss address",
        "    uint32_t    elem;        // bytes per element",
        "    uint32_t    stock_count; // vanilla elements",
        "    uint32_t    count;       // elements now",
        "    uint32_t    off;         // from the section base",
        "    uint32_t    bytes;       // elem * count (the guard follows)",
        "};",
        "",
        "inline constexpr uint32_t SECTION_VSIZE = 0x%x;" % vsize,
        "inline constexpr uint32_t GUARD_BYTES   = %d;" % GUARD,
        "inline constexpr uint8_t  GUARD_FILL    = 0x%02X;" % GUARD_BYTE,
        "inline constexpr unsigned COUNT         = %d;" % len(lay),
        "",
        "inline constexpr entry ARRAYS[COUNT] = {",
    ]
    for a in lay:
        L.append(
            '    {"%s", 0x%08xu, %du, %du, %du, 0x%06xu, %du},'
            % (
                a["name"],
                a["stock_base"],
                a["elem"],
                a["stock_count"],
                a["count"],
                a["off"],
                a["size"],
            )
        )
    L += ["};", "", "} // namespace mh::caps::ai_scratch_layout", ""]
    return "\n".join(L)


# ---- proofs ----------------------------------------------------------------------------------------


def _cap_extents():
    out = []
    for p in (RELOC_PATH, LIMITS_PATH):
        for s in json.load(open(p, encoding="utf-8"))["patches"]:
            va = int(s["va"], 16)
            out.append((va, va + len(s["expect"].split())))
    return sorted(out)


def _units_extents():
    out = []
    for n in ("cap_v023_units_reloc_EN", "cap_v023_units_limits_EN"):
        p = os.path.join(PATCHER, n + ".mh.patch.json")
        if not os.path.isfile(p):
            continue
        for s in json.load(open(p, encoding="utf-8"))["patches"]:
            va = int(s["va"], 16)
            out.append((va, va + len(s["expect"].split())))
    return out


def prove_code(cache, rows):
    """P6: the code splices are exactly CODE_PATCHES, same length as what they replace, decode cleanly, and keep the unchanged tail."""
    import capstone

    f = []
    code = cache.get("code", [])
    if [c["va"] for c in code] != [c["va"] for c in CODE_PATCHES]:
        return ["P6: the cached code splices differ from CODE_PATCHES (run --write)"]
    if cache["exe"].get("p1_code") != len(code):
        f.append(
            "P6: the cached exe proof covers %s code splices, the cache has %d"
            % (cache["exe"].get("p1_code"), len(code))
        )
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for c, spec in zip(code, CODE_PATCHES):
        exp, new = bytes.fromhex(c["exp"]), bytes.fromhex(c["new"])
        va = c["va"]
        if new != spec["new"]:
            f.append("P6: code %#x bytes differ from CODE_PATCHES" % va)
        if len(exp) != len(new):
            f.append("P6: code %#x is %d bytes, replaces %d" % (va, len(new), len(exp)))
            continue
        if exp == new:
            f.append("P6: code %#x changes nothing" % va)
        k = spec["keep_tail"]
        if exp[-k:] != new[-k:]:
            f.append(
                "P6: code %#x does not keep the last %d bytes (the addresses after it move)"
                % (va, k)
            )
        ins = list(md.disasm(new, va))
        if sum(i.size for i in ins) != len(new):
            f.append("P6: code %#x does not decode cleanly to its last byte" % va)
        txt = [("%s %s" % (i.mnemonic, i.op_str)) for i in ins]
        if not any(t == "cmp ecx, 0xc8" for t in txt):
            f.append("P6: code %#x has no `cmp ecx, 0xc8`" % va)
        jae = [i for i in ins if i.mnemonic == "jae"]
        if len(jae) != 1 or int(jae[0].op_str, 16) != spec["epilogue"]:
            f.append(
                "P6: code %#x: the clamp does not jump to the epilogue %#x" % (va, spec["epilogue"])
            )
        for lo, hi in _cap_extents() + _units_extents():
            if lo < va + len(new) and va < hi:
                f.append("P2: code %#x overlaps a cap_v022/v023 site at %#x" % (va, lo))
        for r in rows:
            if va < r["va"] + 4 and r["va"] < va + len(new):
                f.append("P2: code %#x overlaps site %#x" % (va, r["va"]))
    return f


def prove(cache, lay, vsize, text=None):
    f = []
    rows = site_rows(cache, lay)
    if not rows:
        f.append("no sites")
    for x, y in zip(rows, rows[1:]):
        if y["va"] < x["va"] + 4:
            f.append("P2: sites overlap/duplicate at %#x / %#x" % (x["va"], y["va"]))
    caps = _cap_extents()
    import bisect

    starts = [c[0] for c in caps]
    for r in rows:
        j = bisect.bisect_right(starts, r["va"] + 3) - 1
        for k in range(max(0, j - 2), min(len(caps), j + 3)):
            if caps[k][0] < r["va"] + 4 and r["va"] < caps[k][1]:
                f.append(
                    "P2: site %#x overlaps a cap_v022 reloc/limit site at %#x"
                    % (r["va"], caps[k][0])
                )
    for r in rows:
        a = next(x for x in lay if x["name"] == r["array"])
        old, new = u32(r["exp"]), u32(r["new"])
        bias = r.get("bias", 0)
        if bias:
            if BIASED.get(r["va"]) != (a["name"], bias) or old != a["stock_base"] + bias:
                f.append("P3: biased site %#x is not %s%+d" % (r["va"], a["name"], bias))
        elif r["va"] in BIASED:
            f.append("P3: site %#x is declared biased but carries no bias" % r["va"])
        elif array_of(old, lay) is not a:
            f.append("P3: site %#x stock value %#x is not inside %s" % (r["va"], old, a["name"]))
        lo = SEC[1] + a["off"]
        if not lo + bias <= new < lo + a["size"]:
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
            "P1: the cached exe proof covers %s sites, the cache has %d"
            % (cache["exe"].get("p1_sites"), len(rows))
        )
    for c in cache["census"]:
        if c["cls"] not in ("site", STRADDLE, COINCIDENCE, "excluded") or (
            c["cls"] == "excluded"
        ) != (c["va"] in EXCLUDE):
            f.append("P4: census %#x (%s %s) is %s" % (c["va"], c["array"], c["sec"], c["cls"]))
    site_vas = {r["va"] for r in rows}
    for c in cache["census"]:
        if (c["cls"] == "site") != (c["va"] in site_vas):
            f.append("P4: census class of %#x disagrees with the site list" % c["va"])
    f += prove_code(cache, rows)
    # ordering of the layout: no overlap, section holds it
    prev = 0
    for a in lay:
        if a["off"] < prev:
            f.append("layout: %s overlaps the previous array+guard" % a["name"])
        prev = a["off"] + a["size"] + GUARD
    if prev > vsize:
        f.append("layout: section is %#x, needs %#x" % (vsize, prev))
    # stock neighbour check: arrays that were adjacent in .bss must not make a site ambiguous (end == next base)
    for a, b in zip(
        sorted(lay, key=lambda x: x["stock_base"]), sorted(lay, key=lambda x: x["stock_base"])[1:]
    ):
        if a["stock_base"] + a["stock_size"] > b["stock_base"]:
            f.append("layout: stock extents of %s and %s overlap" % (a["name"], b["name"]))
    if text is not None:
        tmp = tempfile.mkdtemp(prefix="capai_")
        path = os.path.join(tmp, NAME + ".mh.patch.json")
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        try:
            c = gim._classify(path)
        except gim.Refused as e:
            f.append("P5: REFUSED by gen_inmem_manifest: %s" % e)
        else:
            if not c["complete"]:
                f.append("P5: not refs_complete: %s" % c["why"])
            if len(c["refs"]) != len(rows):
                f.append("P5: %d refs for %d sites" % (len(c["refs"]), len(rows)))
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
    lay, _v = layout()
    sites, census = scan_exe(raw, img, lay)
    for s in sites:
        if img.read(s["va"], 4) != p32(s["old"]):
            raise Refuse("P1: site %#x does not hold %#x in the exe" % (s["va"], s["old"]))
    code = []
    for c in CODE_PATCHES:
        exp = img.read(c["va"], len(c["new"]))
        if len(exp) != len(c["new"]):
            raise Refuse("P1: code patch %#x runs off the image" % c["va"])
        code.append(dict(va=c["va"], fn=c["fn"], exp=exp.hex(), new=c["new"].hex()))
    return {
        "_why": "GENERATED by tools/gen_cap_ai_manifest.py --write (needs the clean EN exe). The hermetic --check re-derives the manifest from this.",
        "exe": {"exe_md5": md5, "p1_sites": len(sites), "p1_code": len(code)},
        "code": code,
        "sites": [
            dict(
                va=s["va"],
                array=s["array"],
                old=s["old"],
                kind=s["kind"],
                fn=s["fn"],
                insn=s["insn"],
                **({"bias": s["bias"]} if s.get("bias") else {}),
            )
            for s in sites
        ],
        "census": census,
    }


def dump_cache(cache):
    # compact, stable, one site per line
    L = [
        "{",
        '"_why": %s,' % json.dumps(cache["_why"]),
        '"exe": %s,' % json.dumps(cache["exe"]),
        '"code": %s,' % json.dumps(cache["code"]),
        '"sites": [',
    ]
    L.append(",\n".join(json.dumps(s) for s in cache["sites"]))
    L += ["],", '"census": [']
    L.append(",\n".join(json.dumps(c) for c in cache["census"]))
    L += ["]", "}", ""]
    return "\n".join(L)


def derive(cache):
    lay, vsize = layout()
    text = render_doc(cache, lay, vsize)
    return lay, vsize, text, render_layout_h(lay, vsize)


def cmd_write(exe):
    raw, img, md5 = load_exe(exe)
    cache = build_cache(raw, img, md5)
    lay, vsize, text, hdr = derive(cache)
    f = prove(cache, lay, vsize, text)
    if f:
        for x in f[:40]:
            print("  FAIL", x)
        print("gen_cap_ai_manifest --write: REFUSED (%d)" % len(f))
        return 1
    for path, body in ((CACHE, dump_cache(cache)), (PATH, text), (LAYOUT_H, hdr)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8", newline="\n").write(body)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, REPO), len(body)))
    per = {}
    for s in cache["sites"]:
        per[s["array"]] = per.get(s["array"], 0) + 1
    print("sites %d %s; census %d dwords (%d straddle, %d coincidence)" % (
        len(cache["sites"]), per, len(cache["census"]),
        sum(c["cls"] == STRADDLE for c in cache["census"]), sum(c["cls"] == COINCIDENCE for c in cache["census"])))  # fmt: skip
    return 0


def cmd_check():
    if not os.path.isfile(CACHE):
        print(
            "gen_cap_ai_manifest --check: MISSING %s -- run --write" % os.path.relpath(CACHE, REPO)
        )
        return 1
    cache = json.load(open(CACHE, encoding="utf-8"))
    lay, vsize, text, hdr = derive(cache)
    f = prove(cache, lay, vsize, text)
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
        print("gen_cap_ai_manifest --check: FAIL (%d)" % len(f))
        return 1
    print(
        "gen_cap_ai_manifest --check: ok (%d sites, %d arrays, %d census dwords, section %#x)"
        % (len(cache["sites"]), len(lay), len(cache["census"]), vsize)
    )
    return 0


def cmd_report():
    cache = json.load(open(CACHE, encoding="utf-8"))
    lay, vsize = layout()
    per = {}
    for s in cache["sites"]:
        per[s["array"]] = per.get(s["array"], 0) + 1
    for a in lay:
        print(
            "  %-15s stock %#010x x%-5d %2dB -> %#07x +%6d B (x%d)  sites %d"
            % (
                a["name"],
                a["stock_base"],
                a["stock_count"],
                a["elem"],
                a["off"],
                a["size"],
                a["count"],
                per.get(a["name"], 0),
            )
        )
    print("section %s @%#x vsize %#x; %d sites" % (SEC[0], SEC[1], vsize, len(cache["sites"])))
    return 0


def selftest(exe):
    cache = json.load(open(CACHE, encoding="utf-8"))
    lay, vsize = layout()
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    def mut(fn):
        c = json.loads(json.dumps(cache))
        fn(c)
        return prove(c, lay, vsize)

    expect(
        "the committed cache proves clean",
        prove(cache, lay, vsize, render_doc(cache, lay, vsize)) == [],
    )
    expect(
        "a site moved outside its array goes red",
        any("P3" in x for x in mut(lambda c: c["sites"][0].__setitem__("old", 0x1))),
    )
    expect(
        "a duplicated site goes red",
        any("P2" in x for x in mut(lambda c: c["sites"].append(dict(c["sites"][0])))),
    )
    expect(
        "an array with no site goes red",
        any(
            "has no site" in x
            for x in mut(
                lambda c: c.__setitem__(
                    "sites", [s for s in c["sites"] if s["array"] != "mine_roster"]
                )
            )
        ),
    )
    expect(
        "an uncovered census dword goes red",
        any(
            "P4" in x
            for x in mut(
                lambda c: c["census"].append(
                    dict(
                        va=0x500000,
                        array="conn_base",
                        value=0x10130A8,
                        sec="DGROUP",
                        cls="UNCOVERED",
                    )
                )
            )
        ),
    )
    expect(
        "a biased site that lost its bias goes red",
        any(
            "P3" in x
            for x in mut(lambda c: [s.pop("bias") for s in c["sites"] if s["va"] in BIASED])
        ),
    )
    expect(
        "a code splice whose clamp jumps elsewhere goes red",
        any(
            "P6" in x
            for x in mut(
                lambda c: c["code"][0].__setitem__(
                    "new", c["code"][0]["new"].replace("734f", "734e")
                )
            )
        ),
    )
    expect(
        "a code splice that changes length goes red",
        any(
            "P6" in x
            for x in mut(lambda c: c["code"][0].__setitem__("new", c["code"][0]["new"] + "90"))
        ),
    )
    expect(
        "a code splice with no change goes red",
        any("P6" in x for x in mut(lambda c: c["code"][0].__setitem__("new", c["code"][0]["exp"]))),
    )
    expect(
        "a stale exe proof goes red",
        any("P1" in x for x in mut(lambda c: c["exe"].__setitem__("p1_sites", 1))),
    )
    expect(
        "a site over a reloc site goes red",
        any(
            "overlaps a cap_v022" in x
            for x in mut(
                lambda c: c["sites"].append(
                    dict(
                        c["sites"][0],
                        va=int(
                            json.load(open(LIMITS_PATH, encoding="utf-8"))["patches"][0]["va"], 16
                        ),
                    )
                )
            )
        ),
    )
    if exe or scan.default_exe():
        raw, img, md5 = load_exe(exe)
        fresh = build_cache(raw, img, md5)
        expect("a fresh scan of the exe reproduces the committed cache", fresh == cache)
        bad = bytearray(raw)
        s0 = cache["sites"][0]
        for sec in img.secs:
            if sec["va"] <= s0["va"] < sec["va"] + sec["raw_size"]:
                bad[sec["raw_off"] + s0["va"] - sec["va"]] ^= 0xFF
        try:
            other = build_cache(bytes(bad), scan.Image(bytes(bad)), md5)
            expect(
                "a site over wrong exe bytes is refused (or no longer reproduces the cache)",
                other != cache,
            )
        except Refuse:
            expect(
                "a site over wrong exe bytes is refused (or no longer reproduces the cache)", True
            )
        except Exception:
            expect(
                "a site over wrong exe bytes is refused (or no longer reproduces the cache)", True
            )
    print("gen_cap_ai_manifest --selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "ok"))
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
        print("gen_cap_ai_manifest: REFUSED:", e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
