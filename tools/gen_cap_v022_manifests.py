#!/usr/bin/env python3
"""gen_cap_v022_manifests.py -- PT-CAP8 S1 Slice A: the v0.2.2 cap-raise manifests, as GENERATED data.

Two manifests, both applied in memory by mh.dll (the applier is Slice C; nothing references them yet):

  src/patcher/cap_v022_reloc_EN.mh.patch.json   class "caps"         the RELOCATION layer, always applied
  src/patcher/cap_v022_limits_EN.mh.patch.json  class "caps_limits"   the LIMIT layer, `toggle: true`

Arrays are ALWAYS relocated (buildings [8][500], unit_storage/turrets/mines/labs/productions [8][127],
projectiles 4000); the limit layer holds the 51 sites that decide how many slots the game USES. Its
`expect` is the vanilla bytes (OFF) and its `bytes` the raised bytes (ON); the applier writes neither
until a toggle. Units and soldiers are NOT raised (PT-CAP9). The stock .bss extents become the
canonical mirror (spec D1), so the 14 save/load block-base sites stay vanilla and are not in these files.

Inputs, all committed: tools/data/cap_v022_split_plan.json (row classes), the July manifests
grand_all_caphike_storagecap_EN + projectile_pool_caphike_EN (the source of every decoded row and the
CROSS-CHECK: the same re-baking rule under the July layout must reproduce July's bytes), and
tools/data/cap_reloc_scan.json (the baseline gaps that must be covered). The clean EN exe is needed only
by --write and --selftest: its proofs (P1 expect==exe, P3 flags/register liveness) are cached in each
output's `_proofs.exe` block, and --check verifies the cache is complete and current.

  python tools/gen_cap_v022_manifests.py --write [--exe PATH]   # derive, prove against the exe, write both
  python tools/gen_cap_v022_manifests.py --check                # hermetic: re-derive, byte-compare, re-prove
  python tools/gen_cap_v022_manifests.py --selftest             # planted mutations must go red (needs the exe)
  python tools/gen_cap_v022_manifests.py --report

Proofs (each a refusal, each with a --selftest mutation): P1 expect==exe; P2 no overlapping sites and none
over a units/soldiers byte; P3 flags/register liveness at every cave exit and at the 0x4e28ae splice;
P4 in-place limit imm8 <= 127; P5 no limit site in the 8-byte entry window of a claimed body; P6 sites
sorted by VA; plus array extents, July cross-check, scan-gap coverage and gim refs_complete.
"""

import argparse
import copy
import hashlib
import json
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_cap_reloc_sites as scan  # noqa: E402
import gen_cap_split_plan as gsp  # noqa: E402
import gen_inmem_manifest as gim  # noqa: E402

REPO = scan.REPO
PATCHER = scan.PATCHER
RELOC_NAME = "cap_v022_reloc_EN"
LIMITS_NAME = "cap_v022_limits_EN"
RELOC_PATH = os.path.join(PATCHER, RELOC_NAME + ".mh.patch.json")
LIMITS_PATH = os.path.join(PATCHER, LIMITS_NAME + ".mh.patch.json")
GRAND, POOL = gsp.SOURCES

SEC_D = (".mhcapd", 0x010F0000, 0x240000, 0xC0000040)  # zero-init array host
SEC_C = (".mhcapc", 0x01330000, 0x1000, 0x60000020)  # storage-cap caves (reloc)
SEC_L = (
    ".mhcapl",
    0x01340000,
    0x1000,
    0x60000020,
)  # widen + walk caves (limits); 64K-aligned next-free VA after .mhcapc

RAISED_COUNT = {
    "buildings": 500,
    "unit_storage": 127,
    "turrets": 127,
    "mines": 127,
    "labs": 127,
    "productions": 127,
    "projectiles": 4000,
}
ORDER = list(RAISED_COUNT)
SPEC_OFF = {  # spec 1.2, asserted
    "buildings": 0x000000,
    "unit_storage": 0x10A9C0,
    "turrets": 0x147240,
    "mines": 0x154CC0,
    "labs": 0x162B00,
    "productions": 0x164AC0,
    "projectiles": 0x1C9240,
}
JULY_BASE_VA = 0x010F0000  # .mhall and .mhpool both
# The 14 save/load block-base call operands that stay vanilla (they address the canonical mirror).
SAVE_BASES = [
    0x447F00, 0x447F3F, 0x447F54, 0x447F69, 0x447F7E, 0x447F93, 0x447DC5,
    0x4483EC, 0x44842B, 0x448440, 0x448455, 0x44846A, 0x44847F, 0x4482B1,
]  # fmt: skip
GAP_SITES = {  # va -> (expect hex, kind, array, note)
    0x492718: ("a4 6a 00 00", "stride", "buildings", "link_to_network IMUL stride"),
    0x492721: ("a2 d2 c3 00", "base", "buildings", "link_to_network [eax+base+2]"),
    0x492755: ("a4 6a 00 00", "stride", "buildings", "link_to_network IMUL stride"),
    0x49275E: ("a2 d2 c3 00", "base", "buildings", "link_to_network [eax+base+2]"),
    0x4927AF: ("a4 6a 00 00", "stride", "buildings", "link_to_network IMUL stride"),
    0x4927B8: ("a2 d2 c3 00", "base", "buildings", "link_to_network [eax+base+2]"),
    0x4927EC: ("a4 6a 00 00", "stride", "buildings", "link_to_network IMUL stride"),
    0x4927F5: ("a2 d2 c3 00", "base", "buildings", "link_to_network [eax+base+2]"),
    0x4E28AE: (
        "89 da c1 e2 03 29 da c1 e2 02 89 d3 c1 e2 04 29 da 89 d7 c1 e7 06 01 d7",
        "splice",
        "buildings",
        "queue_bldg_repair EDI = EBX*27300 -> imul edi,ebx,136500",
    ),
    0x4560D3: ("fc c4 02 00", "clear", "projectiles", "map_FillDefaults projectile clear size"),
    # PT-CAP8 slice C (2026-10-20): two LIVE shift chains the scan's symbolic evaluator cannot see (a spill through [ebp-0x14];
    # a chain feeding a [eax+base+x] the evaluator drops). Found by the spcamp A/B/C replay (p1_ai_econ ai_intel_flags
    # bit 0x8 = 9 vs 1: register_visible_building read the building x/y of the WRONG row). The July grand manifest has the same hole.
    0x4DB3D7: (
        "c1 e0 03 2b 45 ec c1 e0 02 89 45 ec c1 e0 04 2b 45 ec 89 45 ec c1 e0 06 03 45 ec 89 45 ec",
        "splice",
        "buildings",
        "register_visible_building [ebp-0x14] = EAX*27300 -> imul eax,eax,136500 ; mov [ebp-0x14],eax",
        "69 c0 {rrow} 89 45 ec",
    ),
    0x4D5199: (
        "c1 e0 03 29 d8 c1 e0 02 89 c3 c1 e0 04 29 d8 89 c3 c1 e0 06 01 c3",
        "splice",
        "buildings",
        "engage_candidate_add EBX = EBX*27300 -> imul ebx,ebx,136500 (EAX dead after)",
        "69 db {rrow}",
    ),
    # PT-CAP8 hardening (2026-10-20): the 5 census uses in zero-caller (dead) functions. Closed anyway so the residual check
    # (scan_cap_reloc_sites --check) is clean but for its one allow-listed equality-branch use. Each splice keeps every register
    # the vanilla chain left that anything could read (edx = 420*x via `imul edx,x,420`) and changes only the stride register.
    0x4D393B: (  # llm_bldg_count_owned_matching_ids: EAX=x -> EDX=420x, EAX=26880x; the EA is [edx+eax+base], only the SUM matters
        "c1 e0 03 29 e8 c1 e0 02 89 c2 c1 e0 04 29 d0 89 c2 c1 e0 06",
        "splice",
        "buildings",
        "count_owned_matching_ids head: edx=420x kept, eax = x*(136500-420) so edx+eax = x*136500",
        "69 d0 a4 01 00 00 69 c0 {rrow_m420}",
    ),
    0x4D3959: (  # same function, loop body: EDI = ebp*27300 (EAX, EDX = 420*ebp as side products)
        "89 e8 c1 e0 03 29 e8 c1 e0 02 89 c2 c1 e0 04 29 d0 89 c7 c1 e7 06 01 c7",
        "splice",
        "buildings",
        "count_owned_matching_ids loop: edi = ebp*136500 (edx=420*ebp kept; eax overwritten next insn)",
        "69 d5 a4 01 00 00 69 fd {rrow}",
    ),
    0x4E61A8: (
        "89 c8 c1 e0 03 29 c8 c1 e0 02 89 c2 c1 e0 04 29 d0 89 c2 c1 e0 06 01 d0",
        "splice",
        "buildings",
        "mine_exists_nearby: eax = ecx*136500 (edx = 420*ecx kept)",
        "69 d1 a4 01 00 00 69 c1 {rrow}",
    ),
    0x4ED430: (
        "89 f2 c1 e2 03 29 f2 c1 e2 02 89 d3 c1 e2 04 29 da 89 d6 c1 e6 06 01 d6",
        "splice",
        "buildings",
        "scan_enemy_turrets_in_weapon_range: esi = esi*136500 (ebx, edx side products overwritten at 0x4ed448/0x4ed44c)",
        "69 f6 {rrow}",
    ),
}
# Exit of each bespoke gap splice -> the state that must be dead there (P3): the vanilla chain and the imul leave different flags
# (and, for the second, a different EAX).
GAP_SPLICE_EXITS = {
    0x4DB3D7: ("splice_regvis", 0x4DB3F5, {"eflags"}),
    0x4D5199: ("splice_engage", 0x4D51AF, {"eax", "eflags"}),
    0x4D393B: (
        "splice_count_head",
        0x4D3957,
        {"eax", "eflags"},
    ),  # after the EA-sum insn at 0x4d394f (GAP_EA_SUM)
    0x4D3959: ("splice_count_loop", 0x4D3971, {"eax", "eflags"}),
    0x4E61A8: ("splice_mine_near", 0x4E61C0, {"eflags"}),
    0x4ED430: ("splice_scan_turrets", 0x4ED448, {"ebx", "edx", "eflags"}),
}
# Splices whose exit instruction reads TWO registers that vanilla left as a partial-product pair: only the SUM may matter
# (base+index, scale 1, no other use). va of that instruction -> the two registers.
GAP_EA_SUM = {0x4D394F: {"edx", "eax"}}
REPAIR_SPLICE = 0x4E28AE
REPAIR_EXIT = 0x4E28C6
CLEAN_SHA = None  # taken from the July manifest

# storage-cap blocks, in cave order: name, site, kind
STORAGE = [
    ("housing_default", 0x457E8E, "hd"),
    ("populate", 0x46286C, "populate"),
    ("find_home1", 0x46352D, "gate"),
    ("find_home2", 0x46368E, "gate"),
    ("can_enter_occ", 0x48ADF5, "gate"),
    ("can_land_occ", 0x48AFB2, "gate"),
    ("ai_route_cnt", 0x4D59A7, "gate"),
    ("can_enter_sol", 0x48AD2F, "sol"),
    ("panel_max", 0x417545, "panel"),
]
CAP_FIELD = 0xEC
JCC = {0x7C: "jl", 0x7D: "jge", 0x7E: "jle"}
INTENDED = {"panel_max": {"eax", "edx"}}  # displaced-instruction / value-substitution writes
FLAGS_EQUAL = {"gate", "sol"}  # the cave's last flag-setter is the vanilla compare itself


def hx(b):
    return " ".join("%02x" % x for x in b)


def unhx(s):
    return bytes.fromhex(re.sub(r"\s", "", s))


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


def p32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


class Refuse(Exception):
    pass


# ---- context ---------------------------------------------------------------------------------------------


class Ctx:
    def __init__(self):
        self.plan = json.load(open(gsp.PLAN, encoding="utf-8"))
        self.july = {}
        self.july_doc = {}
        for m in gsp.SOURCES:
            d = json.load(open(os.path.join(PATCHER, m + ".mh.patch.json"), encoding="utf-8"))
            self.july_doc[m] = d
            for p in d["patches"]:
                self.july[(m, int(p["va"], 16))] = p
        global CLEAN_SHA
        CLEAN_SHA = self.july_doc[GRAND]["sha256"]
        self.scan_summary, self.scan_hits = scan.load_committed()
        self.arrs = arrays()


def arrays():
    stock = {a["name"]: a for a in scan.arrays()}
    out, off = {}, 0
    for n in ORDER:
        a = stock[n]
        rr = a["elem"] * RAISED_COUNT[n]
        size = rr * a["players"]
        out[n] = dict(
            name=n, base=a["base"], elem=a["elem"], players=a["players"], vrow=a["row"],
            vsize=a["size"], rrow=rr, rsize=size, off=off,
        )  # fmt: skip
        if off != SPEC_OFF[n]:
            raise Refuse("array %s offset %#x differs from the spec's %#x" % (n, off, SPEC_OFF[n]))
        off = (off + size + 63) & ~63
    if off > SEC_D[2]:
        raise Refuse("arrays need %#x bytes, section is %#x" % (off, SEC_D[2]))
    return out


def rebake(a, A, off, rrow=None):
    """Stock address A of array a -> (new address, p, q) for a layout whose array starts at `off`."""
    rrow = a["rrow"] if rrow is None else rrow
    r = A - a["base"]
    if not 0 <= r < a["vsize"]:
        raise Refuse("address %#x is outside stock array %s" % (A, a["name"]))
    p, q = (0, r) if a["players"] == 1 else divmod(r, a["vrow"])
    return JULY_BASE_VA + off + p * rrow + q, p, q


# ---- build ---------------------------------------------------------------------------------------------


class Built:
    pass


def site(va, exp, new, kind, array, note, p=None, q=None, jval=None):
    return dict(va=va, exp=exp, new=new, kind=kind, array=array, note=note, p=p, q=q, jval=jval)


def derive_reloc_sites(ctx):
    """-> (sites, old_off). Re-bake every July relocation row; the July bytes are the cross-check."""
    rows = ctx.plan["packed"]["rows"]
    out, base_rows = [], []
    save = set(SAVE_BASES)
    seen_save = set()
    for va, ln, mi, layer, cls, arr_s, _rule, _fn in rows:
        if layer != "a" or cls == "walk_backstop":
            continue
        if cls == "base" and va in save:
            seen_save.add(va)
            continue
        jp = ctx.july[(gsp.SOURCES[mi], va)]
        exp, jb = unhx(jp["expect"]), unhx(jp["bytes"])
        a = ctx.arrs[arr_s]
        if cls == "shift_splice":
            out.append(site(va, exp, jb, "splice", arr_s, "shift row-stride splice (July bytes)"))
        elif cls in ("stride", "clear_size"):
            v = u32(exp)
            if a["players"] == 1:
                if v != a["vsize"]:
                    raise Refuse("%#x: projectile size %d" % (va, v))
                new = a["rsize"]
            else:
                k, rem = divmod(v, a["vrow"])
                if rem:
                    raise Refuse("%#x: %d is not a multiple of %s's row" % (va, v, arr_s))
                new = k * a["rrow"]
            out.append(
                site(va, exp, p32(new), "stride" if cls == "stride" else "clear", arr_s,
                     "%s %s %d->%d" % (cls, arr_s, v, new), jval=u32(jb))
            )  # fmt: skip
        elif cls == "base":
            out.append(None)
            base_rows.append((len(out) - 1, va, exp, jb, a))
        else:
            raise Refuse("unhandled plan row %s/%s at %#x" % (layer, cls, va))
    if seen_save != save:
        raise Refuse("save-base sites not all in the plan: %s" % sorted(save - seen_save))
    # old offsets from the p == 0 rows of the July layout
    cand = {}
    for _i, va, exp, jb, a in base_rows:
        r = u32(exp) - a["base"]
        p, q = (0, r) if a["players"] == 1 else divmod(r, a["vrow"])
        if p == 0:
            cand.setdefault(a["name"], set()).add(u32(jb) - JULY_BASE_VA - q)
    old_off = {}
    for n, s in cand.items():
        if len(s) != 1:
            raise Refuse("July layout for %s is not one offset: %s" % (n, sorted(s)))
        old_off[n] = next(iter(s))
    for i, va, exp, jb, a in base_rows:
        A = u32(exp)
        new, p, q = rebake(a, A, a["off"])
        out[i] = site(va, exp, p32(new), "base", a["name"], "base %s [%d]+%d" % (a["name"], p, q),
                      p=p, q=q, jval=u32(jb))  # fmt: skip
    # the gaps
    planned = {s["va"] for s in out}
    for va, ent in sorted(GAP_SITES.items()):
        ehx, kind, arr_s, note = ent[:4]
        if va in planned:
            raise Refuse("gap site %#x is already a planned row" % va)
        exp, a = unhx(ehx), ctx.arrs[arr_s]
        if kind == "base":
            new, p, q = rebake(a, u32(exp), a["off"])
            out.append(site(va, exp, p32(new), "base", arr_s, "gap: " + note, p=p, q=q))
        elif kind == "stride":
            k, rem = divmod(u32(exp), a["vrow"])
            if rem:
                raise Refuse("gap %#x stride" % va)
            out.append(site(va, exp, p32(k * a["rrow"]), "stride", arr_s, "gap: " + note))
        elif kind == "clear":
            out.append(site(va, exp, p32(a["rsize"]), "clear", arr_s, "gap: " + note))
        else:  # splice: imul edi, ebx, rrow(buildings)
            if len(ent) > 4:
                pre = unhx(
                    ent[4]
                    .replace("{rrow_m420}", hx(p32(a["rrow"] - 420)))
                    .replace("{rrow}", hx(p32(a["rrow"])))
                )
            else:
                pre = b"\x69\xfb" + p32(a["rrow"])
            if len(pre) > len(exp):
                raise Refuse("gap splice %#x: replacement longer than the chain" % va)
            new = pre + b"\x90" * (len(exp) - len(pre))
            out.append(
                site(va, exp, new, "splice", arr_s, "shift row-stride splice (gap): " + note)
            )
    return out, old_off


def july_check(sites, old_off, arrs):
    """The July cross-check: the SAME rule under the July layout must reproduce July's bytes."""
    fails = []
    for s in sites:
        if s["jval"] is None and s["kind"] != "base":
            continue
        if s["kind"] == "base" and s.get("jval") is not None:
            a = arrs[s["array"]]
            want = JULY_BASE_VA + old_off[s["array"]] + s["p"] * a["rrow"] + s["q"]
            if want != s["jval"]:
                fails.append(
                    "July cross-check: %#x %s [%d]+%d would bake %#x, July has %#x"
                    % (s["va"], s["array"], s["p"], s["q"], want, s["jval"])
                )
        elif s["kind"] in ("stride", "clear") and s.get("jval") is not None:
            if u32(s["new"]) != s["jval"]:
                fails.append(
                    "July cross-check: %#x %s %d vs July %d"
                    % (s["va"], s["kind"], u32(s["new"]), s["jval"])
                )
    return fails


# ---- caves ---------------------------------------------------------------------------------------------


def _ks():
    from keystone import Ks, KS_ARCH_X86, KS_MODE_32

    return Ks(KS_ARCH_X86, KS_MODE_32)


def _asm(ks, code, va):
    b, _n = ks.asm(code, va)
    if b is None:
        raise Refuse("keystone failed: %r" % code)
    return bytes(b)


def storage_blocks(ctx, pushfd):
    """-> [dict(name, kind, site, exp, code, off, exits)] with the caves laid out in .mhcapc."""
    ks = _ks()
    us = ctx.arrs["unit_storage"]
    usb = JULY_BASE_VA + us["off"]
    cap = usb + CAP_FIELD
    blocks = []
    cur = SEC_C[1]
    for name, va, kind in STORAGE:
        jp = ctx.july[(GRAND, va)]
        exp = unhx(jp["expect"])
        ln = len(exp)
        after = va + ln
        pf = name in pushfd
        pre, post = ("pushfd\n", "popfd\n") if pf else ("", "")
        exits = [after]
        if kind == "hd":
            house = u32(exp, 2)
            if exp[:2] != b"\xc7\x80" or exp[6:] != b"\x32\x00\x00\x00":
                raise Refuse("housing_default expect shape")
            t = (
                pre + "cmp dword ptr [eax + %#x], 0\njnz ok\nmov dword ptr [eax + %#x], 0x32\nok:\n" % (house, house)
                + post + "jmp %#x\n" % after
            )  # fmt: skip
        elif kind == "populate":
            if exp[:2] != b"\x89\x82" or u32(exp, 2) != us["base"]:
                raise Refuse("populate expect shape")
            t = (
                "mov dword ptr [edx + %#x], eax\npush eax\npush ecx\n" % usb
                + pre
                + "mov eax, [ebp - 0x1c]\nimul eax, eax, 0x842\nmov ecx, [eax + 0xd9f291]\n"
                "cmp ecx, 0x32\njne store\nxor ecx, ecx\nstore:\n"
                "mov dword ptr [edx + %#x], ecx\n" % cap
                + post
                + "pop ecx\npop eax\njmp %#x\n" % after
            )  # fmt: skip
        elif kind == "gate":
            if exp[:2] != b"\x83\xb8" or exp[6] != 0x32:
                raise Refuse("%s expect shape" % name)
            f = u32(exp, 2) - us["base"]
            if exp[7] in JCC:
                jcc, tgt = JCC[exp[7]], after - ln + 9 + struct.unpack_from("<b", exp, 8)[0]
            elif exp[7:9] == b"\x0f\x8d":
                jcc, tgt = "jge", va + 13 + struct.unpack_from("<i", exp, 9)[0]
            else:
                raise Refuse("%s jcc" % name)
            exits = [tgt, after]
            t = (
                "push ecx\nmov ecx, [eax + %#x]\ntest ecx, ecx\njnz ok\nmov ecx, 0x32\nok:\n" % cap
                + "cmp dword ptr [eax + %#x], ecx\npop ecx\n%s %#x\njmp %#x\n" % (usb + f, jcc, tgt, after)
            )  # fmt: skip
        elif kind == "sol":
            if exp[:3] != b"\x83\xf8\x32" or exp[3] not in JCC:
                raise Refuse("sol expect shape")
            tgt = va + 5 + struct.unpack_from("<b", exp, 4)[0]
            exits = [tgt, after]
            t = (
                "push ecx\npush edx\nmovzx edx, word ptr [ebp - 0x10]\nimul edx, edx, 0xf4\nadd edx, ebx\n"
                "mov ecx, [edx + %#x]\ntest ecx, ecx\njnz ok\nmov ecx, 0x32\nok:\n" % cap
                + "cmp eax, ecx\npop edx\npop ecx\n%s %#x\njmp %#x\n" % (JCC[exp[3]], tgt, after)
            )  # fmt: skip
        elif kind == "panel":
            if exp[:2] != b"\x8b\x80":
                raise Refuse("panel expect shape")
            f = u32(exp, 2) - us["base"]
            t = (
                pre + "mov edx, [eax + %#x]\ntest edx, edx\njnz ok\nmov edx, 0x32\nok:\n" % cap
                + "mov eax, [eax + %#x]\n" % (usb + f) + post + "jmp %#x\n" % after
            )  # fmt: skip
        else:
            raise Refuse(kind)
        code = _asm(ks, t, cur)
        if len(_asm(ks, t, 0)) != len(code):
            raise Refuse("%s: encoding length depends on the VA" % name)
        blocks.append(dict(name=name, kind=kind, site=va, exp=exp, code=code, va=cur,
                           exits=exits, flags="equal" if kind in FLAGS_EQUAL else "dead",
                           intended=sorted(INTENDED.get(name, ()))))  # fmt: skip
        cur += len(code)
    if cur > SEC_C[1] + SEC_C[2]:
        raise Refuse("storage caves overflow .mhcapc")
    return blocks


def storage_sites(blocks):
    out = []
    for b in blocks:
        ln = len(b["exp"])
        if ln < 5:
            raise Refuse("site too short")
        rel = b["va"] - (b["site"] + 5)
        new = b"\xe9" + struct.pack("<i", rel) + b"\x90" * (ln - 5)
        out.append(
            site(
                b["site"],
                b["exp"],
                new,
                "cave",
                "unit_storage",
                "%s -> cave %#x" % (b["name"], b["va"]),
            )
        )
    return out


def limit_sites(ctx):
    """-> (sites, caves) for the limit layer; caves are laid out in .mhcapl sorted by site VA."""
    rows = ctx.plan["packed"]["rows"]
    todo = []
    for va, ln, mi, layer, cls, arr_s, _rule, _fn in rows:
        if layer == "b" or (layer == "a" and cls == "walk_backstop"):
            todo.append((va, mi, cls, arr_s))
    todo.sort()
    # pass 1: cave sizes
    caves, cur = [], SEC_L[1]
    for va, mi, cls, arr_s in todo:
        jp = ctx.july[(gsp.SOURCES[mi], va)]
        exp = unhx(jp["expect"])
        if cls == "widen_bound":
            c = widen_cave(va, exp, cur)
        elif cls == "walk_backstop":
            c = walk_cave(va, exp, cur)
        else:
            continue
        caves.append(c)
        cur += len(c["code"])
    byva = {c["site"]: c for c in caves}
    out = []
    for va, mi, cls, arr_s in todo:
        jp = ctx.july[(gsp.SOURCES[mi], va)]
        exp, jb = unhx(jp["expect"]), unhx(jp["bytes"])
        if va in byva:
            c = byva[va]
            new = b"\xe9" + struct.pack("<i", c["va"] - (va + 5)) + b"\x90" * (len(exp) - 5)
            out.append(site(va, exp, new, "cave", arr_s, "%s -> cave %#x" % (cls, c["va"])))
        else:
            out.append(site(va, exp, jb, "limit", arr_s, "%s (July bytes)" % cls))
    return out, caves


def widen_cave(va, exp, cave_va):
    if len(exp) != 6 or exp[:2] != b"\x83\x7d" or not 0x70 <= exp[4] <= 0x7F:
        raise Refuse("widen site %#x shape %s" % (va, hx(exp)))
    imm = {0x64: 500, 0x5B: 491}.get(exp[3])
    if imm is None:
        raise Refuse("widen site %#x immediate %#x" % (va, exp[3]))
    tgt = va + 6 + struct.unpack_from("<b", exp, 5)[0]
    cc = exp[4] - 0x70
    code = b"\x81\x7d" + bytes([exp[2]]) + p32(imm)
    code += b"\x0f" + bytes([0x80 + cc]) + struct.pack("<i", tgt - (cave_va + 7 + 6))
    code += b"\xe9" + struct.pack("<i", (va + 6) - (cave_va + 13 + 5))
    return dict(name="widen@%#x" % va, kind="widen", site=va, exp=exp, code=code, va=cave_va,
                exits=[tgt, va + 6], flags="dead", intended=[])  # fmt: skip


def walk_cave(va, exp, cave_va):
    if exp[:2] != b"\x83\x7d" or exp[3] != 0 or exp[4] != 0x75:
        raise Refuse("walk site %#x shape %s" % (va, hx(exp)))
    body = va + 6 + exp[5]
    if len(exp) == 11 and exp[6] == 0xE9:
        exit_ = va + 11 + struct.unpack_from("<i", exp, 7)[0]
    elif len(exp) == 8 and exp[6] == 0xEB:
        exit_ = va + 8 + struct.unpack_from("<b", exp, 7)[0]
    else:
        raise Refuse("walk site %#x tail" % va)
    code = b"\x83\x7d" + bytes([exp[2], 0])
    code += b"\x0f\x84" + struct.pack("<i", exit_ - (cave_va + 4 + 6))
    code += b"\x66\x81\x3d" + p32(0xE58142) + struct.pack("<H", 500)
    code += b"\x0f\x83" + struct.pack("<i", exit_ - (cave_va + 19 + 6))
    code += b"\xe9" + struct.pack("<i", body - (cave_va + 25 + 5))
    return dict(name="walk@%#x" % va, kind="walk", site=va, exp=exp, code=code, va=cave_va,
                exits=[exit_, body], flags="dead", intended=[])  # fmt: skip


# ---- decode helpers -------------------------------------------------------------------------------


def _md():
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return md


def decode_blob(code, va):
    md = _md()
    out, pos = [], 0
    while pos < len(code):
        i = next(md.disasm(code[pos:], va + pos, 1), None)
        if i is None:
            raise Refuse("undecodable cave byte at %#x" % (va + pos))
        out.append(i)
        pos += i.size
    return out


FULL = {}
for _n in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
    FULL[_n] = _n
for _n, _f in (("ax", "eax"), ("bx", "ebx"), ("cx", "ecx"), ("dx", "edx"), ("si", "esi"), ("di", "edi"),
               ("al", "eax"), ("ah", "eax"), ("bl", "ebx"), ("bh", "ebx"), ("cl", "ecx"), ("ch", "ecx"),
               ("dl", "edx"), ("dh", "edx")):  # fmt: skip
    FULL[_n] = _f


def clobbered(code, va):
    """Full registers a cave writes and does not restore (push/pop pairs), excluding esp."""
    ins = decode_blob(code, va)
    wr, push, pop = set(), {}, {}
    for i in ins:
        _r, w = i.regs_access()
        if i.mnemonic == "push" and i.operands[0].type == 1:
            push[i.reg_name(i.operands[0].reg)] = push.get(i.reg_name(i.operands[0].reg), 0) + 1
        elif i.mnemonic == "pop" and i.operands[0].type == 1:
            pop[i.reg_name(i.operands[0].reg)] = pop.get(i.reg_name(i.operands[0].reg), 0) + 1
        for x in w:
            n = i.reg_name(x)
            if n in FULL and FULL[n] != "esp":
                wr.add(FULL[n])
    return {r for r in wr if not (push.get(r, 0) and push.get(r) == pop.get(r))}


def live(img, start, regs, budget=120):
    """Forward liveness from `start`: is every register/flag in `regs` written before it is read on
    EVERY path? -> (ok, reason). Conservative: partial writes, calls, rets and indirect jumps that
    could observe the value refuse."""
    from capstone.x86 import X86_OP_IMM, X86_GRP_JUMP

    md = _md()
    seen, stack, n = set(), [(start, frozenset(regs))], 0
    while stack:
        va, t = stack.pop()
        while t:
            if (va, t) in seen:
                break
            seen.add((va, t))
            n += 1
            if n > budget * 8:
                return False, "budget exhausted"
            i = next(md.disasm(img.read(va, 16), va, 1), None)
            if i is None:
                return False, "undecodable at %#x" % va
            r, w = i.regs_access()
            for x in [i.reg_name(x) for x in r]:
                if x == "eflags":
                    if "eflags" in t:
                        return False, "flags read at %#x (%s %s)" % (va, i.mnemonic, i.op_str)
                elif FULL.get(x) in t:
                    return False, "%s read at %#x (%s %s)" % (FULL[x], va, i.mnemonic, i.op_str)
            if i.mnemonic == "call":
                for x in ("eax", "ebx", "ecx", "edx"):
                    if x in t:
                        return False, "call at %#x may read %s" % (va, x)
                t = t - {"eflags"}
                va += i.size
                continue
            if i.mnemonic.startswith("ret"):
                for x in ("eax", "ebx", "ecx", "edx", "esi", "edi"):
                    if x in t:
                        return False, "ret at %#x may read %s" % (va, x)
                break
            kill = set()
            for x in [i.reg_name(x) for x in w]:
                if x == "eflags":
                    m = i.mnemonic
                    partial = (
                        m in ("inc", "dec")
                        or (m in ("shl", "shr", "sar", "sal", "rol", "ror", "rcl", "rcr")
                            and i.operands[-1].type != X86_OP_IMM)
                        or i.prefix[0] in (0xF2, 0xF3)
                    )  # fmt: skip
                    if not partial:
                        kill.add("eflags")
                else:
                    f = FULL.get(x)
                    if f and x == f:
                        kill.add(f)
                    elif f and f in t:
                        return False, "partial write of %s at %#x" % (f, va)
            t = frozenset(t - kill)
            if not t:
                break
            if i.mnemonic == "jmp":
                op = i.operands[0]
                if op.type != X86_OP_IMM:
                    return False, "indirect jmp at %#x" % va
                va = op.imm
                continue
            if X86_GRP_JUMP in i.groups:
                op = i.operands[0]
                if op.type != X86_OP_IMM:
                    return False, "indirect jcc at %#x" % va
                stack.append((op.imm, t))
            va += i.size
    return True, "dead"


def p3_checks(b):
    """-> [(id, exit_va, regs_set)] every liveness question the built manifests owe."""
    out = []
    for c in b.caves_reloc + b.caves_limits:
        regs = clobbered(c["code"], c["va"]) - set(c["intended"])
        if c["flags"] != "equal":
            regs = regs | {"eflags"}
        if not regs:
            continue
        for k, ex in enumerate(c["exits"]):
            out.append(("%s/exit%d" % (c["name"], k), ex, regs))
    out.append(("repair_splice/exit0", REPAIR_EXIT, {"ebx", "edx", "eflags"}))
    for _va, (nm, ex, regs) in sorted(GAP_SPLICE_EXITS.items()):
        out.append((nm + "/exit0", ex, regs))
    return out


# ---- build / render ---------------------------------------------------------------------------------


def build(ctx, exe_proofs):
    """Derive both manifests. exe_proofs: {'pushfd': [...]} chosen by the cached P3 run."""
    pushfd = set((exe_proofs or {}).get("pushfd", ()))
    b = Built()
    b.ctx = ctx
    b.arrs = ctx.arrs
    b.reloc, b.old_off = derive_reloc_sites(ctx)
    b.caves_reloc = storage_blocks(ctx, pushfd)
    cs = storage_sites(b.caves_reloc)
    plan_c = sorted(r[0] for r in ctx.plan["packed"]["rows"] if r[3] == "c")
    if plan_c != sorted(s["va"] for s in cs):
        raise Refuse("storage cave sites differ from the plan's c-rows")
    b.reloc += cs
    b.reloc.sort(key=lambda s: s["va"])
    b.limits, b.caves_limits = limit_sites(ctx)
    b.cave_data_reloc = b"".join(c["code"] for c in b.caves_reloc)
    b.cave_data_limits = b"".join(c["code"] for c in b.caves_limits)
    b.exe_proofs = exe_proofs or {}
    return b


def _sites_sha(sites):
    h = hashlib.sha256()
    for s in sites:
        h.update(p32(s["va"]) + s["exp"] + b"|" + s["new"] + b"\n")
    return h.hexdigest()


def render_doc(b, which, exe_part):
    reloc = which == "reloc"
    sites = b.reloc if reloc else b.limits
    data = b.cave_data_reloc if reloc else b.cave_data_limits
    if reloc:
        secs = [
            {"name": SEC_D[0], "vaddr": "0x%x" % SEC_D[1], "vsize": "0x%x" % SEC_D[2], "chars": "0x%X" % SEC_D[3]},
            {"name": SEC_C[0], "vaddr": "0x%x" % SEC_C[1], "vsize": "0x%x" % SEC_C[2], "data": hx(data), "chars": "0x%X" % SEC_C[3]},
        ]  # fmt: skip
    else:
        secs = [
            {"name": SEC_L[0], "vaddr": "0x%x" % SEC_L[1], "vsize": "0x%x" % SEC_L[2], "data": hx(data), "chars": "0x%X" % SEC_L[3]}
        ]  # fmt: skip
    kinds = {}
    for s in sites:
        kinds[s["kind"]] = kinds.get(s["kind"], 0) + 1
    proofs = {
        "sites": len(sites),
        "sites_sha256": _sites_sha(sites),
        "by_kind": dict(sorted(kinds.items())),
        "caves": [{"name": c["name"], "va": "0x%08x" % c["va"], "len": len(c["code"])}
                  for c in (b.caves_reloc if reloc else b.caves_limits)],
        "exe": exe_part,
    }  # fmt: skip
    if reloc:
        proofs["arrays"] = {
            n: {"off": "0x%06x" % a["off"], "bytes": a["rsize"], "row_bytes": a["rrow"]}
            for n, a in b.arrs.items()
        }
        proofs["july_old_off"] = {n: "0x%06x" % v for n, v in sorted(b.old_off.items())}
        proofs["gap_sites"] = ["0x%08x" % v for v in sorted(GAP_SITES)]
    doc = {
        "class": "caps" if reloc else "caps_limits",
        "_class_why": (
            "v0.2.2 cap raise, RELOCATION layer: every array is ALWAYS relocated to its raised size "
            "(applied at boot; the setting never moves an array). Compiled into mh.dll, not in COMPILED[]."
            if reloc
            else "v0.2.2 cap raise, LIMIT layer: `expect` is the vanilla bytes (OFF), `bytes` the raised bytes (ON). "
            "The applier verifies and stages them and writes only on a toggle at a quiescent point."
        ),
    }
    if not reloc:
        doc["toggle"] = True
    doc.update(
        {
            "target": "mh.exe (English, clean retail)",
            "sha256": CLEAN_SHA,
            "_purpose": (
                "GENERATED by tools/gen_cap_v022_manifests.py -- do not hand-edit. "
                + (
                    "3 arrays-host sections; %d sites: base/stride/shift/clear re-baked from the July grand+pool "
                    "manifests into .mhcapd, the 10 scan gaps, and the 9 storage-cap redirects (populate caches 0 "
                    "for the stock cap of 50, so a stock cfg is vanilla-identical)." % len(sites)
                    if reloc
                    else "%d sites: 24 loop bounds, 1 round-robin budget, 4 allocator bounds, 17 widened bounds, "
                    "3 walk backstops, 2 projectile sites." % len(sites)
                )
            ),
            "_proofs": proofs,
            "sections": secs,
            "patches": [
                {
                    "va": "0x%08x" % s["va"],
                    "expect": hx(s["exp"]),
                    "bytes": hx(s["new"]),
                    "_note": s["note"],
                }
                for s in sites
            ],
        }
    )
    return json.dumps(doc, indent=1) + "\n"


# ---- the proofs ---------------------------------------------------------------------------------------


def _extents(b):
    return {
        n: (JULY_BASE_VA + a["off"], JULY_BASE_VA + a["off"] + a["rsize"])
        for n, a in b.arrs.items()
    }


def prove_hermetic(b, ctx, tmpdir=None):
    """Everything provable without the exe. -> [failure strings]."""
    f = []
    # counts
    if len(b.reloc) != 3945:
        f.append("reloc has %d sites, expected 3945" % len(b.reloc))
    if len(b.limits) != 51:
        f.append("limits has %d sites, expected 51" % len(b.limits))
    # P6 sorted
    for nm, ss in (("reloc", b.reloc), ("limits", b.limits)):
        for x, y in zip(ss, ss[1:]):
            if y["va"] <= x["va"]:
                f.append("P6: %s sites not strictly sorted at %#x" % (nm, y["va"]))
                break
    # P2 overlap (both manifests) + x rows
    ext = sorted((s["va"], s["va"] + len(s["exp"]), "reloc") for s in b.reloc)
    ext += [(s["va"], s["va"] + len(s["exp"]), "limits") for s in b.limits]
    ext.sort()
    for x, y in zip(ext, ext[1:]):
        if y[0] < x[1]:
            f.append("P2: sites overlap at %#x (%s) and %#x (%s)" % (x[0], x[2], y[0], y[2]))
    xrows = sorted((r[0], r[0] + r[1]) for r in ctx.plan["packed"]["rows"] if r[3] in "x?")
    for lo, hi, nm in ext:
        for xl, xh in xrows:
            if xl < hi and lo < xh:
                f.append("P2: %s site %#x overlaps a units/soldiers row byte at %#x" % (nm, lo, xl))
                break
    # P4 imm range
    for s in b.limits:
        if s["kind"] == "limit" and len(s["new"]) == 1 and s["new"][0] > 127:
            f.append("P4: limit imm8 at %#x is %d > 127" % (s["va"], s["new"][0]))
    # extents
    E = _extents(b)
    for s in b.reloc:
        if s["kind"] == "base":
            t = u32(s["new"])
            lo, hi = E[s["array"]]
            if not lo <= t < hi:
                f.append(
                    "extent: %#x bakes %#x outside %s [%#x,%#x)" % (s["va"], t, s["array"], lo, hi)
                )
            for n, (l2, h2) in E.items():
                if n != s["array"] and l2 <= t < h2:
                    f.append("extent: %#x bakes %#x inside the WRONG array %s" % (s["va"], t, n))
    for c in b.caves_reloc:
        for i in decode_blob(c["code"], c["va"]):
            if i.disp_size == 4:
                d = u32(c["code"], i.address - c["va"] + i.disp_offset)
                if SEC_D[1] <= d < SEC_D[1] + SEC_D[2]:
                    lo, hi = E["unit_storage"]
                    if not lo <= d < hi:
                        f.append("extent: cave %s names %#x outside unit_storage" % (c["name"], d))
    # July cross-check
    f += july_check(b.reloc, b.old_off, b.arrs)
    f += cave_crosscheck(b, ctx)
    # scan gaps
    hits = {(h["va"], h["array"], h["role"]): h for h in ctx.scan_hits}
    exts = sorted(
        (
            s["va"],
            s["va"] + len(s["exp"]),
            RELOC_NAME,
            "splice" if s["kind"] == "splice" else "field",
        )
        for s in b.reloc
    )
    cov = scan.Coverage(exts)
    for bl in ctx.scan_summary["baseline_v022"]:
        if bl["state"] not in ("open", "closed"):
            continue
        h = hits.get((int(bl["va"], 16), bl["array"], bl["role"]))
        if h is None:
            f.append("scan baseline %s has no committed field" % bl["va"])
        elif scan.hit_status(cov, h) != "covered":
            f.append(
                "scan gap %s (%s %s) is NOT covered by the reloc manifest"
                % (bl["va"], bl["array"], bl["role"])
            )
    for va in GAP_SITES:
        if not any(s["va"] == va for s in b.reloc):
            f.append("gap site %#x missing from the reloc manifest" % va)
    # P5
    f += prove_p5(b)
    # refs complete
    f += prove_refs(b, tmpdir)
    return f


_P5 = {}


def _p5_ctx():
    if not _P5:
        import check_inmem_composition as comp

        extents = comp.function_extents()
        find = comp.body_finder(extents)
        P = comp.named_table(comp.PATCHES_HEADER, "promotable_ranges")
        targets, _u = comp.detour_targets()
        D = {find(v)[2] for v in targets if find(v)}
        _P5.update(find=find, claimed=P | D, window=comp.promoted_entry_window_bytes())
    return _P5


def prove_p5(b):
    c = _p5_ctx()
    f = []
    for s in b.limits:
        body = c["find"](s["va"])
        if body and body[2] in c["claimed"]:
            if s["va"] + len(s["exp"]) > body[0] and s["va"] < body[0] + c["window"]:
                f.append(
                    "P5: limit site %#x is inside the %d-byte entry window of claimed body %s"
                    % (s["va"], c["window"], body[2])
                )
    return f


def claimed_entry():
    c = _p5_ctx()
    return next(
        (e[0], e[2]) for e in sorted(_all_bodies()) if e[2] in c["claimed"]
    )  # lowest claimed entry


def _all_bodies():
    import check_inmem_composition as comp

    return comp.function_extents()


def prove_refs(b, tmpdir):
    f = []
    root = tmpdir or tempfile.mkdtemp(prefix="capgen_")
    for which in ("reloc", "limits"):
        path = os.path.join(
            root, (RELOC_NAME if which == "reloc" else LIMITS_NAME) + ".mh.patch.json"
        )
        open(path, "w", encoding="utf-8", newline="\n").write(render_doc(b, which, {}))
        try:
            c = gim._classify(path)
        except gim.Refused as e:
            f.append("refs: %s REFUSED: %s" % (which, e))
            continue
        if not c["complete"]:
            f.append("refs: %s is not refs_complete: %s" % (which, c["why"]))
    return f


def sig(code, va, norm=None):
    """Normalised instruction signature of a blob: branches by absolute target, array disps by
    (array, offset), everything else by bytes."""
    out = []
    for i in decode_blob(code, va):
        if i.group(1) or i.group(7):  # jump / call
            if i.operands[0].type == 2:
                t = i.operands[0].imm
                inside = va <= t < va + len(code)
                out.append((i.mnemonic, "->blk+%d" % (t - va) if inside else "->%#x" % t))
            else:
                out.append((i.mnemonic, i.op_str))
            continue
        raw = bytes(i.bytes)
        if norm and i.disp_size == 4:
            d = u32(raw, i.disp_offset)
            tag = norm(d)
            if tag:
                raw = raw[: i.disp_offset] + tag + raw[i.disp_offset + 4 :]
        out.append((i.mnemonic, raw.hex()))
    return out


def july_blob(ctx, va):
    """July section bytes starting at `va`: -> (bytes_from_va, section va)."""
    for s in ctx.july_doc[GRAND]["sections"]:
        sv = int(s["vaddr"], 16)
        d = unhx(s["data"])
        if sv <= va < sv + len(d):
            return d[va - sv :], va
    raise Refuse("July cave address %#x not in a July section" % va)


def cave_crosscheck(b, ctx):
    f = []
    us_new = JULY_BASE_VA + b.arrs["unit_storage"]["off"]
    us_old = JULY_BASE_VA + b.old_off["unit_storage"]
    rr = b.arrs["unit_storage"]["rsize"]

    def norm_new(d):
        return b"US" + p32(d - us_new)[:2] if us_new <= d < us_new + rr else None

    def norm_old(d):
        return b"US" + p32(d - us_old)[:2] if us_old <= d < us_old + rr else None

    # limits: widen / walk caves vs the cave the July site pointed at
    byva = {c["site"]: c for c in b.caves_limits}
    for s in b.limits:
        c = byva.get(s["va"])
        if c is None:
            continue
        jp = ctx.july[(GRAND, s["va"])]
        jb = unhx(jp["bytes"])
        if jb[0] != 0xE9:
            f.append("July site %#x is not a JMP" % s["va"])
            continue
        jcave = s["va"] + 5 + struct.unpack_from("<i", jb, 1)[0]
        blob, _v = july_blob(ctx, jcave)
        n = len(c["code"])
        if sig(blob[:n], jcave) != sig(c["code"], c["va"]):
            f.append("cave cross-check: %s differs from July's cave at %#x" % (c["name"], jcave))
    # storage: vs July .mhcap blocks
    cap = [s for s in ctx.july_doc[GRAND]["sections"] if s["name"] == ".mhcap"][0]
    cv, cd = int(cap["vaddr"], 16), unhx(cap["data"])
    starts = []
    for c in b.caves_reloc:
        note = ctx.july[(GRAND, c["site"])]["_note"]
        starts.append(int(note.split("cave ")[1], 16))
    ends = starts[1:] + [cv + len(cd)]
    for c, s0, e0 in zip(b.caves_reloc, starts, ends):
        jb = cd[s0 - cv : e0 - cv]
        if c["kind"] == "populate":
            txt = [(i.mnemonic, i.op_str) for i in decode_blob(c["code"], c["va"])]
            joined = " | ".join("%s %s" % t for t in txt)
            if "cmp ecx, 0x32 | jne" not in joined or "| xor ecx, ecx |" not in joined:
                f.append(
                    "populate cave lost the 'cmp ecx,0x32 / jne / xor ecx,ecx' stock-cap shape"
                )
            continue
        if sig(jb, s0, norm_old) != sig(c["code"], c["va"], norm_new):
            f.append("cave cross-check: storage block %s differs from July's" % c["name"])
    return f


def prove_exe(b, img):
    """P1 + P3 against the clean exe -> (failures, exe_part-for-each-manifest)."""
    f = []
    for nm, ss in (("reloc", b.reloc), ("limits", b.limits)):
        for s in ss:
            if img.read(s["va"], len(s["exp"])) != s["exp"]:
                f.append(
                    "P1: %s site %#x expect %s != exe %s"
                    % (nm, s["va"], hx(s["exp"]), hx(img.read(s["va"], len(s["exp"]))))
                )
    from capstone.x86 import X86_OP_MEM

    for va, regs in sorted(GAP_EA_SUM.items()):
        i = next(_md().disasm(img.read(va, 16), va, 1), None)
        m = next((o.mem for o in (i.operands if i else ()) if o.type == X86_OP_MEM), None)
        if not (m and m.scale == 1 and {i.reg_name(m.base), i.reg_name(m.index)} == regs):
            f.append("P3: EA-sum insn %#x is not a scale-1 [%s] sum" % (va, "+".join(sorted(regs))))
    res = []
    for cid, ex, regs in p3_checks(b):
        ok, why = live(img, ex, regs)
        res.append(
            dict(
                id=cid,
                exit="0x%08x" % ex,
                need=sorted(regs),
                verdict="dead" if ok else "REFUSED",
                why=why,
            )
        )
        if not ok:
            f.append("P3: %s exit %#x: %s" % (cid, ex, why))
    return f, res


def exe_parts(b, p3, md5):
    names = {c["name"] for c in b.caves_limits}
    lim = [r for r in p3 if r["id"].split("/")[0] in names]
    rel = [r for r in p3 if r["id"].split("/")[0] not in names]
    return (
        {"exe_md5": md5, "p1_sites": len(b.reloc), "pushfd": sorted(b.pushfd), "p3": rel},
        {"exe_md5": md5, "p1_sites": len(b.limits), "p3": lim},
    )


# ---- exe loading --------------------------------------------------------------------------------------


def load_exe(path=None):
    path = path or scan.default_exe()
    if not path or not os.path.isfile(path):
        raise Refuse("no clean EN exe (md5 %s); pass --exe" % scan.EN_MD5)
    raw = open(path, "rb").read()
    md5 = hashlib.md5(raw).hexdigest()
    if md5 != scan.EN_MD5:
        raise Refuse("%s is md5 %s, not the clean EN exe" % (path, md5))
    return scan.Image(raw), md5, raw


def derive_with_proofs(ctx, img, md5):
    """Run P3 to pick the pushfd set, rebuild, run P1/P3 for real. -> (built, failures)."""
    b0 = build(ctx, {})
    pushfd = set()
    for cid, ex, regs in p3_checks(b0):
        name = cid.split("/")[0]
        if name in ("housing_default", "populate", "panel_max"):
            ok, _w = live(img, ex, {"eflags"})
            if not ok:
                pushfd.add(name)
    b = build(ctx, {"pushfd": sorted(pushfd)})
    b.pushfd = pushfd
    f, p3 = prove_exe(b, img)
    b.p3 = p3
    b.exe_rel, b.exe_lim = exe_parts(b, p3, md5)
    b.exe_proofs = b.exe_rel
    return b, f


def render_both(b):
    return render_doc(b, "reloc", b.exe_rel), render_doc(b, "limits", b.exe_lim)


# ---- modes ----------------------------------------------------------------------------------------------


def cmd_write(exe):
    ctx = Ctx()
    img, md5, _raw = load_exe(exe)
    b, f = derive_with_proofs(ctx, img, md5)
    f += prove_hermetic(b, ctx)
    if f:
        for x in f[:30]:
            print("  FAIL", x)
        print("gen_cap_v022_manifests --write: REFUSED (%d)" % len(f))
        return 1
    r, l = render_both(b)
    for path, text in ((RELOC_PATH, r), (LIMITS_PATH, l)):
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, REPO), len(text)))
    print("P3 verdicts: %d, pushfd: %s" % (len(b.p3), sorted(b.pushfd) or "none needed"))
    return 0


def committed_proofs():
    out = []
    for path in (RELOC_PATH, LIMITS_PATH):
        if not os.path.isfile(path):
            raise Refuse("MISSING %s -- run --write" % os.path.relpath(path, REPO))
        out.append(json.load(open(path, encoding="utf-8"))["_proofs"]["exe"])
    return out


def cmd_check():
    ctx = Ctx()
    try:
        exe_rel, exe_lim = committed_proofs()
    except Refuse as e:
        print("gen_cap_v022_manifests --check:", e)
        return 1
    b = build(ctx, exe_rel)
    b.pushfd = set(exe_rel.get("pushfd", ()))
    b.exe_rel, b.exe_lim = exe_rel, exe_lim
    f = []
    # the cached exe proofs must cover exactly the questions the current sites ask, all green
    need = {c[0]: c for c in p3_checks(b)}
    have = {r["id"]: r for r in exe_rel["p3"] + exe_lim["p3"]}
    for k in sorted(set(need) - set(have)):
        f.append("exe proof missing for %s -- run --write with the exe" % k)
    for k in sorted(set(have) - set(need)):
        f.append("exe proof for %s is stale (no such question any more)" % k)
    for k, r in have.items():
        if r["verdict"] != "dead":
            f.append("exe proof %s is %s" % (k, r["verdict"]))
        elif k in need and (r["exit"] != "0x%08x" % need[k][1] or r["need"] != sorted(need[k][2])):
            f.append("exe proof %s was run for a different exit/register set" % k)
    for e in (exe_rel, exe_lim):
        if e.get("exe_md5") != scan.EN_MD5:
            f.append("exe proofs were not run against the clean EN exe")
    r, l = render_both(b)
    for path, text in ((RELOC_PATH, r), (LIMITS_PATH, l)):
        cur = open(path, encoding="utf-8").read()
        if cur != text:
            f.append(
                "STALE: %s differs from the re-derivation (run --write)"
                % os.path.relpath(path, REPO)
            )
    f += prove_hermetic(b, ctx)
    if f:
        for x in f[:30]:
            print("  FAIL", x)
        print("gen_cap_v022_manifests --check: FAIL (%d)" % len(f))
        return 1
    print(
        "gen_cap_v022_manifests --check: ok (reloc %d sites, limits %d sites, %d cached exe proofs)"
        % (len(b.reloc), len(b.limits), len(have))
    )
    return 0


def cmd_report():
    ctx = Ctx()
    exe_rel, exe_lim = committed_proofs()
    b = build(ctx, exe_rel)
    for n, a in b.arrs.items():
        print("  %-13s off %#08x  row %7d  size %8d" % (n, a["off"], a["rrow"], a["rsize"]))
    k = {}
    for s in b.reloc:
        k[s["kind"]] = k.get(s["kind"], 0) + 1
    print("reloc %d %s; limits %d; storage caves %d bytes; limit caves %d bytes" % (
        len(b.reloc), k, len(b.limits), len(b.cave_data_reloc), len(b.cave_data_limits)))  # fmt: skip
    return 0


# ---- selftest -------------------------------------------------------------------------------------------


def patched_image(raw, img, va, data):
    r = bytearray(raw)
    for s in img.secs:
        if s["va"] <= va < s["va"] + s["raw_size"]:
            o = s["raw_off"] + va - s["va"]
            r[o : o + len(data)] = data
            return scan.Image(bytes(r))
    raise Refuse("va not in image")


def selftest(exe):
    ctx = Ctx()
    img, md5, raw = load_exe(exe)
    fails = []

    def expect(what, ok, detail=None):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)
            if detail is not None:
                print("       got: %r" % (detail,))

    b, f = derive_with_proofs(ctx, img, md5)
    f += prove_hermetic(b, ctx)
    expect("the unmutated derivation is green against the exe", not f, f[:3])
    expect("3945 reloc sites and 51 limit sites", len(b.reloc) == 3945 and len(b.limits) == 51)
    expect("the populate cave carries the stock-cap shape (cmp ecx,0x32 / jne / xor ecx,ecx)",
           not [x for x in cave_crosscheck(b, ctx) if "populate" in x])  # fmt: skip
    expect(
        "no pushfd was needed (all three flag questions are dead)", not b.pushfd, sorted(b.pushfd)
    )

    def mut():
        return copy.deepcopy(b)

    # 1. a site whose expect != exe
    m = mut()
    m.reloc[100]["exp"] = bytes([m.reloc[100]["exp"][0] ^ 0xFF]) + m.reloc[100]["exp"][1:]
    ff, _ = prove_exe(m, img)
    expect(
        "mutation 1: a site whose expect != exe goes RED (P1)",
        any(x.startswith("P1") for x in ff),
        ff[:2],
    )
    # 2. a re-baked target outside its array extent
    m = mut()
    s = next(x for x in m.reloc if x["kind"] == "base" and x["array"] == "turrets")
    s["new"] = p32(JULY_BASE_VA + 0x23F000)
    ff = prove_hermetic(m, ctx)
    expect(
        "mutation 2: a baked target outside its array extent goes RED",
        any(x.startswith("extent") for x in ff),
        ff[:2],
    )
    # 3. a reloc site overlapping an x-row
    m = mut()
    xr = next(r for r in ctx.plan["packed"]["rows"] if r[3] == "x" and r[4] == "base")
    m.reloc.append(site(xr[0], b"\0" * 4, b"\0" * 4, "base", "turrets", "planted", p=0, q=0))
    m.reloc.sort(key=lambda x: x["va"])
    ff = prove_hermetic(m, ctx)
    expect(
        "mutation 3: a reloc site over a units/soldiers byte goes RED (P2)",
        any("units/soldiers" in x for x in ff),
        ff[:2],
    )
    # 4. a widen cave whose exit lands on a flags read
    m = mut()
    wc = next(c for c in m.caves_limits if c["kind"] == "widen")
    bad = patched_image(raw, img, wc["exits"][1], b"\x0f\x84\x00\x00\x00\x00")
    ff, _ = prove_exe(m, bad)
    expect(
        "mutation 4: a widen cave exit that reads flags goes RED (P3)",
        any(x.startswith("P3") and "widen" in x for x in ff),
        ff[:2],
    )
    # 5. a limit site inside an entry window
    m = mut()
    ent, nm = claimed_entry()
    m.limits.append(site(ent + 2, b"\x90", b"\x90", "limit", "buildings", "planted"))
    m.limits.sort(key=lambda x: x["va"])
    ff = prove_p5(m)
    expect(
        "mutation 5: a limit site in a claimed body's entry window goes RED (P5)",
        any(x.startswith("P5") for x in ff),
        ff[:2],
    )
    # 6. an unsorted site list
    m = mut()
    m.limits[3], m.limits[4] = m.limits[4], m.limits[3]
    ff = prove_hermetic(m, ctx)
    expect(
        "mutation 6: an unsorted site list goes RED (P6)",
        any(x.startswith("P6") for x in ff),
        ff[:2],
    )
    # 7. a July cross-check mismatch
    m = mut()
    m.old_off["unit_storage"] += 64
    ff = july_check(m.reloc, m.old_off, m.arrs)
    expect("mutation 7: a perturbed July offset goes RED (cross-check)", len(ff) > 100, len(ff))
    # 8. a missing scan gap
    m = mut()
    m.reloc = [x for x in m.reloc if x["va"] != 0x4927AF]
    ff = prove_hermetic(m, ctx)
    expect(
        "mutation 8: a missing scan gap goes RED",
        any("scan gap" in x or "gap site" in x for x in ff),
        ff[:2],
    )
    # 9. P4
    m = mut()
    lim = next(x for x in m.limits if x["kind"] == "limit" and len(x["new"]) == 1)
    lim["new"] = b"\x80"
    ff = prove_hermetic(m, ctx)
    expect(
        "mutation 9: a limit imm8 above 127 goes RED (P4)",
        any(x.startswith("P4") for x in ff),
        ff[:2],
    )
    # 10. the repair splice flag proof is real (a flags read at the exit)
    bad = patched_image(raw, img, REPAIR_EXIT, b"\x0f\x84\x00\x00\x00\x00")
    ff, _ = prove_exe(b, bad)
    expect(
        "mutation 10: the 0x4e28ae splice exit reading flags goes RED (P3)",
        any("repair_splice" in x for x in ff),
        ff[:2],
    )
    print("gen_cap_v022_manifests --selftest: %s" % ("FAIL" if fails else "PASS"))
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
        if a.check:
            return cmd_check()
        if a.selftest:
            return selftest(a.exe)
        if a.report:
            return cmd_report()
    except Refuse as e:
        print("gen_cap_v022_manifests: REFUSED -- %s" % e)
        return 1
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
