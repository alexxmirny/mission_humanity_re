#!/usr/bin/env python3
"""gen_cap_path_sites.py -- PT-CAP9-U4b: the path-slot pool sites of the units manifests, derived from the census.

Not a CLI: tools/gen_cap_units_manifests.py imports it and merges the sites into the units RELOCATION and LIMIT manifests
(cap_v023_units_{reloc,limits}_EN). Input is tools/data/cap_path_slot_census.json (every pool / path_slot_id site of the clean
EN exe, classified always_on / raise_switched / unchanged, plus the cave windows) and the units relocation sites (a raise-switched
id access can sit on a relocated displacement). Nothing here reads the exe: the exe-backed proofs (P1 expect==exe, P3 liveness)
stay in the units generator, which owns the exe.

THE POOL (design: tmp/capraise/u4/REPORT.md; user decisions 2026-10-21):
  buffers  byte {heading,run}[8 players][501 slots][300 waypoints]   stride per player 0xea60 -> 0x49638, slot 0x258 unchanged
  flags    byte[8][501]                                              stride per player 100 -> 501
  both live in one appended section `.mhcapp` (buffers at +0x40, flags at +0x24b200); the stock .bss extents become the canonical
  MIRROR (slots 0..99), so map_SavePlanetToDisk / map_LoadPlanetFromDisk keep their stock addresses (policy `unchanged`).
  Slot 500 is the SINK: never handed out, the 16-bit "no path" value, so an unguarded reader lands in the player's own spare slot.

ALWAYS-ON (relocation layer; applied at boot whatever `[caps] raise` says):
  * buffer bases (disp32 / imm32, INCLUDING the 26 `[reg + base-1|-2]` biased ones the first census missed), the buffer player
    stride imm32 0xea60 -> 0x49638, the FillDefaults clear size 0x75300 -> 0x24b1c0, the flags bases, the two data-segment
    pointers 0x5d0ac0 / 0x5d0ac4;
  * CAVES in `.mhcapq` for the sites an imm8 cannot express: the 16 flags-stride `imul r, x, 0x64` (-> 501) and the FillDefaults
    flag-clear bound (`cmp [..], 0x64` -> 501). A cave replaces a WINDOW of whole instructions (census `windows`) with `JMP cave`
    + NOPs; a flags_base displacement inside the window is carried into the cave already re-baked.
RAISE-SWITCHED (limit layer; written only when the raise goes ON):
  * the 54 path_slot_id reads: `movzx r, byte [x+0xaa]` -> `movzx r, word [x+0xda]` (opcode 0xb6 -> 0xb7, disp +0x30; on a
    relocated displacement the same +0x30), one 7-byte site each;
  * the 4 id writes (cave): init_record / map_unit_Add / free_slot store 0xff to the byte AND 500 to the word; attach_slot
    stores min(slot,0xfe) to the byte and the slot to the word;
  * the 6 scanner bounds (cave: `cmp ..,0x64` -> 500) and the free-count start (imm32 100 -> 500).
  Raise OFF never writes the hole +0xda, so records, hashes and fixtures stay byte-identical.
UNCHANGED: the 17 `cmp byte [x+0xaa],0xff` checks (the byte stays 0xff = no path), the count INC/DEC, the Save/Load mirror, the
two row-0 tactical walks.
"""

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scan_cap_path_slots as sp  # noqa: E402

PLAYERS = 8
STOCK_SLOTS = 100
ROW = 501  # slots per player row (500 usable + the sink)
RAISED = 500  # usable slots when raised; also the 16-bit "no path" value (the sink slot)
SLOT_BYTES = 0x258  # 300 waypoints x 2
BUF_STOCK = 0xAEE8C0
FLAGS_STOCK = 0xB74BB0
BUF_STRIDE_OLD = 0xEA60
BUF_STRIDE_NEW = ROW * SLOT_BYTES  # 0x49638
BUF_TOTAL = PLAYERS * BUF_STRIDE_NEW  # 2,404,800 = 0x24b1c0
# The buffers start 64 bytes into the section: instructions address them with a displacement as low as base-2 (the group path-step
# recorder), and the classifier would read a value just BEFORE the section as an offset into the section that ends there.
BUF_OFF = 0x40
FLAGS_OFF = (BUF_OFF + BUF_TOTAL + 63) & ~63
FLAGS_TOTAL = PLAYERS * ROW
HOST_BYTES = FLAGS_OFF + FLAGS_TOTAL
HOST_VSIZE = (HOST_BYTES + 0xFFF) & ~0xFFF
HOLE_DELTA = 0xDA - 0xAA
ID_BYTE_MAX = 0xFE  # byte id = min(id, 0xfe); 0xff = no path
CAVE_SECTION_SIZE = 0x1000
DATA_PTR_NEW = {0x5D0AC0: "flags", 0x5D0AC4: "buf"}


class Refuse(Exception):
    pass


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


def p32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def hx(b):
    return " ".join("%02x" % x for x in b)


def _md():
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return md


class Layout:
    """Where the pool lives: the `.mhcapp` section's VA (a chain-derived constant owned by the units generator)."""

    def __init__(self, host_va):
        self.host_va = host_va
        self.buf = host_va + BUF_OFF
        self.flags = host_va + FLAGS_OFF

    def buf_new(self, v):
        return self.buf + (v - BUF_STOCK)

    def flags_new(self, v):
        return self.flags + (v - FLAGS_STOCK)


def site(va, exp, new, kind, note, vanilla=None, layer="reloc"):
    return dict(
        va=va,
        exp=exp,
        new=new,
        kind=kind,
        array="path",
        note=note,
        raised=None,
        u4=False,
        p=None,
        q=None,
        jval=None,
        vanilla=vanilla if vanilla is not None else exp,
        layer=layer,
    )


def rows_of(doc):
    rows = sp.unpack(doc["sites"])
    for r in rows:
        r["raw"] = bytes.fromhex(r["hex"])
        r["field"] = r["raw"][r["foff"] : r["foff"] + r["w"]]
        r["fva"] = r["va"] + r["foff"]
    return rows


def windows_of(doc):
    out = {}
    for k, w in doc["windows"].items():
        out[int(k, 16)] = dict(hex=bytes.fromhex(w["hex"]), rows=list(w["rows"]), ins=w["ins"])
    return out


def window_containing(wins, va):
    for s, w in wins.items():
        if s <= va < s + len(w["hex"]):
            return s
    return None


def overlay(vanilla, va, others):
    """The bytes at [va, va+len) after every site of `others` (dicts with va/exp/new) that overlaps them has been applied. A
    site must lie wholly inside the span (it is a field of an instruction of the span)."""
    cur = bytearray(vanilla)
    for o in others:
        lo, hi = o["va"], o["va"] + len(o["exp"])
        if hi <= va or lo >= va + len(vanilla):
            continue
        if lo < va or hi > va + len(vanilla):
            raise Refuse("site %#x straddles the edge of the span at %#x" % (lo, va))
        if bytes(cur[lo - va : hi - va]) != o["exp"]:
            raise Refuse(
                "site %#x expects %s, the span holds %s"
                % (lo, hx(o["exp"]), hx(cur[lo - va : hi - va]))
            )
        cur[lo - va : hi - va] = o["new"]
    return bytes(cur)


# ---- cave bodies -----------------------------------------------------------------------------------------------------


def _decode(win, va):
    out = []
    pos = 0
    md = _md()
    while pos < len(win):
        i = next(md.disasm(win[pos:], va + pos, 1), None)
        if i is None:
            raise Refuse("undecodable window byte at %#x" % (va + pos))
        out.append(i)
        pos += i.size
    return out


def _jmp(at, target):
    return b"\xe9" + struct.pack("<i", target - (at + 5))


def _jcc_rel32(at, cc, target):
    return b"\x0f" + bytes([0x80 + cc]) + struct.pack("<i", target - (at + 6))


def imul_imm32(ins, new_imm):
    """`imul r, r/m, imm8` (6B /r ib) -> `imul r, r/m, imm32` (69 /r id)."""
    b = bytes(ins.bytes)
    if b[0] != 0x6B or ins.mnemonic != "imul":
        raise Refuse("%#x: expected imul r,r/m,imm8, got %s" % (ins.address, b.hex()))
    return b"\x69" + b[1:-1] + p32(new_imm)


def cmp_widen(ins, new_imm):
    """`cmp r/m32, imm8` (83 /7 ib) -> `cmp r/m32, imm32` (81 /7 id); with a 66 prefix the immediate is 16 bits."""
    b = bytes(ins.bytes)
    if ins.mnemonic != "cmp":
        raise Refuse("%#x: expected cmp, got %s" % (ins.address, b.hex()))
    if b[0] == 0x66 and b[1] == 0x83:
        return b"\x66\x81" + b[2:-1] + struct.pack("<H", new_imm)
    if b[0] == 0x83:
        return b"\x81" + b[1:-1] + p32(new_imm)
    raise Refuse("%#x: cmp is not the imm8 form: %s" % (ins.address, b.hex()))


def _jcc_cc(ins):
    b0 = ins.bytes[0]
    if 0x70 <= b0 <= 0x7F:
        return b0 - 0x70
    if b0 == 0x0F and 0x80 <= ins.bytes[1] <= 0x8F:
        return ins.bytes[1] - 0x80
    raise Refuse("%#x: not a jcc: %s" % (ins.address, bytes(ins.bytes).hex()))


def build_cmp_cave(win_cur, va, new_imm):
    """A window `cmp x, 0x64 [; jcc target]` -> a builder(cave_va) -> (code, exits). The compare is re-encoded with the wide
    immediate, a trailing jcc becomes rel32, and the cave jumps back past the window. The window keeps its flags result."""
    ins = _decode(win_cur, va)
    if len(ins) > 2:
        raise Refuse("%#x: cmp window with %d instructions" % (va, len(ins)))
    first = cmp_widen(ins[0], new_imm)
    after = va + len(win_cur)

    def build(cave_va):
        code = bytearray(first)
        exits = []
        if len(ins) == 2:
            tgt = ins[1].operands[0].imm & 0xFFFFFFFF
            code += _jcc_rel32(cave_va + len(code), _jcc_cc(ins[1]), tgt)
            exits.append(tgt)
        code += _jmp(cave_va + len(code), after)
        exits.append(after)
        return bytes(code), exits

    return build


def build_stride_cave(win_cur, va):
    """A flags-stride window: `imul r, x, 0x64` (+ the following instructions, already re-based) -> imm32 501.
    Nothing but the first instruction changes; no branch is allowed inside (the census guarantees it)."""
    ins = _decode(win_cur, va)
    if ins[0].mnemonic != "imul" or (ins[0].operands[-1].imm & 0xFFFFFFFF) != sp.FLAGS_STRIDE:
        raise Refuse("%#x: flags-stride window does not start with imul ,0x64" % va)
    body = imul_imm32(ins[0], ROW) + b"".join(bytes(i.bytes) for i in ins[1:])
    after = va + len(win_cur)

    def build(cave_va):
        return body + _jmp(cave_va + len(body), after), [after]

    return build


def build_replacement_cave(new_ins, va, wlen):
    """The window's single instruction replaced by `new_ins` (same effect on registers and flags, a different field), then the
    jump back past the window."""
    after = va + wlen

    def build(cave_va):
        return new_ins + _jmp(cave_va + len(new_ins), after), [after]

    return build


def build_hole_store_cave(win_cur, va):
    """init_record / map_unit_Add / free_slot: `mov byte [r+id], 0xff` (C6 /0 ib) keeps the byte and gains
    `mov word [r+id+0x30], 500` (66 C7 /0 iw)."""
    if len(win_cur) != 7 or win_cur[0] != 0xC6 or win_cur[6] != 0xFF or (win_cur[1] & 0xF8) != 0x80:
        raise Refuse("%#x: not `mov byte [r+disp32], 0xff`: %s" % (va, win_cur.hex()))
    modrm = win_cur[1]
    disp = u32(win_cur, 2)
    extra = b"\x66\xc7" + bytes([modrm]) + p32(disp + HOLE_DELTA) + struct.pack("<H", RAISED)
    after = va + len(win_cur)

    def build(cave_va):
        code = win_cur + extra
        return code + _jmp(cave_va + len(code), after), [after]

    return build


def build_attach_cave(win_cur, va):
    """attach_slot: `mov al,[ebp-0x10]; mov byte [edx+id], al` -> the 16-bit store of the slot and the byte as min(slot,0xfe):
         mov eax,[ebp-0x10] ; mov word [edx+id+0x30],ax ; cmp eax,0xfe ; jbe L ; mov al,0xfe ; L: mov byte [edx+id],al
    EAX is dead after the window (the next instruction reloads it) -- the P3 proof checks that against the exe."""
    if (
        len(win_cur) != 9
        or win_cur[:3] != bytes.fromhex("8a45f0")
        or win_cur[3:5] != bytes.fromhex("8882")
    ):
        raise Refuse("%#x: not attach_slot's pair: %s" % (va, win_cur.hex()))
    disp = u32(win_cur, 5)
    body = (
        bytes.fromhex("8b45f0")  # mov eax,[ebp-0x10]
        + bytes.fromhex("668982")
        + p32(disp + HOLE_DELTA)  # mov [edx+id+0x30],ax
        + bytes.fromhex("3dfe000000")  # cmp eax,0xfe
        + bytes.fromhex("7602")  # jbe +2
        + bytes.fromhex("b0fe")  # mov al,0xfe
        + bytes.fromhex("8882")
        + p32(disp)  # mov [edx+id],al
    )
    after = va + len(win_cur)

    def build(cave_va):
        return body + _jmp(cave_va + len(body), after), [after]

    return build


# ---- derivation ------------------------------------------------------------------------------------------------------


def derive(doc, others, lay):
    """-> (reloc_sites, limit_sites, cave_defs). `others` = the units relocation sites (for the overlay of a raised id
    access that sits on a relocated displacement). cave_defs: [(site, builder, layer, name, flags, intended)] -- the caller
    allocates each cave's VA in its section, calls builder(cave_va) -> (code, exits) and rewrites the site's `new`."""
    rows = rows_of(doc)
    wins = windows_of(doc)
    reloc, limits, caves = [], [], []

    # rows that ride inside a cave window are carried by the cave, not by a site of their own
    in_window = {}
    for s, w in wins.items():
        for rv in w["rows"]:
            in_window[rv] = s

    by_va = {}
    for r in rows:
        by_va.setdefault(r["va"], []).append(r)

    # ---- always-on, in place
    for r in rows:
        if (
            r["policy"] != "always_on"
            or r["va"] in in_window
            and r["cls"] in ("flags_base", "flags_stride", "flag_clear_bound")
        ):
            continue
        cls, v = r["cls"], u32(r["field"]) if r["w"] == 4 else None
        if cls == "buf_base":
            if not sp.BUF_LO <= v < BUF_STOCK + sp.BUF[1]:
                raise Refuse("buf_base %#x holds %#x" % (r["fva"], v))
            reloc.append(
                site(
                    r["fva"],
                    p32(v),
                    p32(lay.buf_new(v)),
                    "pbase",
                    "path buffer base %s %+d" % (r["role"], v - BUF_STOCK),
                )
            )
        elif cls == "flags_base":
            if not FLAGS_STOCK <= v < FLAGS_STOCK + sp.FLAGS[1]:
                raise Refuse("flags_base %#x holds %#x" % (r["fva"], v))
            reloc.append(
                site(
                    r["fva"],
                    p32(v),
                    p32(lay.flags_new(v)),
                    "pbase",
                    "path flags base +%d" % (v - FLAGS_STOCK),
                )
            )
        elif cls == "buf_stride":
            if v != BUF_STRIDE_OLD:
                raise Refuse("buf_stride %#x holds %#x" % (r["fva"], v))
            reloc.append(
                site(
                    r["fva"],
                    p32(v),
                    p32(BUF_STRIDE_NEW),
                    "pstride",
                    "path buffer player stride %d->%d" % (v, BUF_STRIDE_NEW),
                )
            )
        elif cls == "buf_size":
            if v != sp.BUF_SIZE:
                raise Refuse("buf_size %#x holds %#x" % (r["fva"], v))
            reloc.append(
                site(
                    r["fva"],
                    p32(v),
                    p32(BUF_TOTAL),
                    "pclear",
                    "path buffer clear size %d->%d" % (v, BUF_TOTAL),
                )
            )
        elif cls == "data_ptr":
            which = DATA_PTR_NEW[r["va"]]
            want_old = FLAGS_STOCK if which == "flags" else BUF_STOCK
            if v != want_old:
                raise Refuse("data pointer %#x holds %#x" % (r["va"], v))
            new = lay.flags if which == "flags" else lay.buf
            reloc.append(site(r["va"], p32(v), p32(new), "pptr", "data pointer -> path %s" % which))
        elif cls in ("flags_stride", "flag_clear_bound"):
            pass  # a cave (below)
        else:
            raise Refuse("always_on row %#x of class %s has no rule" % (r["va"], cls))

    # ---- always-on caves (windows holding a flags_stride or the flag_clear_bound row)
    for s, w in sorted(wins.items()):
        classes = {r["cls"] for rv in w["rows"] for r in by_va[rv]}
        if not classes & {"flags_stride", "flag_clear_bound"}:
            continue
        # re-base the flags_base displacements riding inside the window
        inner = []
        for rv in w["rows"]:
            for r in by_va[rv]:
                if r["cls"] == "flags_base":
                    v = u32(r["field"])
                    inner.append(dict(va=r["fva"], exp=p32(v), new=p32(lay.flags_new(v))))
        cur = overlay(w["hex"], s, inner + others)
        if "flag_clear_bound" in classes:
            build = build_cmp_cave(cur, s, ROW)
            name = "clear@%#x" % s
            what = "FillDefaults flag-clear bound %d->%d" % (STOCK_SLOTS, ROW)
        else:
            build = build_stride_cave(cur, s)
            name = "fstride@%#x" % s
            what = "flags row stride %d->%d" % (sp.FLAGS_STRIDE, ROW)
        st = site(
            s,
            w["hex"],
            b"",
            "pcave",
            "%s: %s (%d-byte window) -> cave" % (name, what, len(w["hex"])),
        )
        caves.append((st, build, "reloc", name, _cmp_flags(cur, s), _intended(cur, s)))
        reloc.append(st)

    # ---- raise-switched
    for r in rows:
        if r["policy"] != "raise_switched":
            continue
        cls = r["cls"]
        if cls == "id_read":
            vanilla = r["raw"]
            if len(vanilla) != 7 or vanilla[:2] != b"\x0f\xb6":
                raise Refuse(
                    "id_read %#x is not a 7-byte movzx r32, byte [x+disp32]: %s"
                    % (r["va"], vanilla.hex())
                )
            cur = overlay(vanilla, r["va"], others + reloc)
            disp = u32(cur, 3)
            new = b"\x0f\xb7" + cur[2:3] + p32(disp + HOLE_DELTA)
            if cur == vanilla:
                # [reg + 0xaa]: a small displacement -- rewritten in place (opcode 0xb6 -> 0xb7, disp 0xaa -> 0xda)
                limits.append(
                    site(
                        r["va"],
                        cur,
                        new,
                        "praise",
                        "path_slot_id read %s -> word +0xda" % r["role"],
                        vanilla=vanilla,
                        layer="limits",
                    )
                )
            else:
                # [reg + units+0xaa], the displacement is an ABSOLUTE address into the relocated units array. A limit-manifest site
                # may not hold one (it declares no section for it), so the instruction moves into a cave of the relocation
                # manifest's code section and the site is the JMP.
                name = "idread@%#x" % r["va"]
                st = site(
                    r["va"],
                    cur,
                    b"",
                    "pcave",
                    "%s: path_slot_id read %s -> word +0xda (cave)" % (name, r["role"]),
                    vanilla=vanilla,
                    layer="limits",
                )
                caves.append(
                    (
                        st,
                        build_replacement_cave(new, r["va"], len(cur)),
                        "limits",
                        name,
                        "equal",
                        _intended(cur, r["va"]),
                    )
                )
                limits.append(st)
        elif cls == "count_init":
            v = u32(r["field"])
            if v != STOCK_SLOTS:
                raise Refuse("count_init %#x holds %d" % (r["fva"], v))
            limits.append(
                site(
                    r["fva"],
                    p32(v),
                    p32(RAISED),
                    "plimit",
                    "free-count start %d->%d" % (v, RAISED),
                    layer="limits",
                )
            )
        elif cls == "scanner_bound":
            s = window_containing(wins, r["va"])
            w = wins[s]
            cur = overlay(w["hex"], s, others + reloc)
            build = build_cmp_cave(cur, s, RAISED)
            name = "scan@%#x" % s
            st = site(
                s,
                cur,
                b"",
                "pcave",
                "%s: %s -> cave (%d->%d)" % (name, r["fn"], STOCK_SLOTS, RAISED),
                vanilla=w["hex"],
                layer="limits",
            )
            caves.append((st, build, "limits", name, _cmp_flags(cur, s), _intended(cur, s)))
            limits.append(st)
        elif cls == "id_write":
            s = window_containing(wins, r["va"])
            w = wins[s]
            cur = overlay(w["hex"], s, others + reloc)
            if s == sp.ATTACH_WINDOW[0]:
                build, name, flags, intended = (
                    build_attach_cave(cur, s),
                    "attach@%#x" % s,
                    "dead",
                    [],
                )
            else:
                build, name, flags, intended = (
                    build_hole_store_cave(cur, s),
                    "idinit@%#x" % s,
                    "equal",
                    [],
                )
            st = site(
                s,
                cur,
                b"",
                "pcave",
                "%s: %s id store -> cave (+16-bit hole)" % (name, r["fn"]),
                vanilla=w["hex"],
                layer="limits",
            )
            caves.append((st, build, "limits", name, flags, intended))
            limits.append(st)
        else:
            raise Refuse("raise_switched row %#x of class %s has no rule" % (r["va"], cls))

    reloc.sort(key=lambda x: x["va"])
    limits.sort(key=lambda x: x["va"])
    return reloc, limits, caves


def _intended(win_cur, va):
    """Registers the VANILLA window writes (the cave re-executes the same instructions, so those writes are not extra clobbers)."""
    names = set()
    for ins in _decode(win_cur, va):
        _r, w = ins.regs_access()
        names |= {ins.reg_name(x) for x in w}
    full = {
        "al": "eax",
        "ax": "eax",
        "ah": "eax",
        "bl": "ebx",
        "bx": "ebx",
        "cl": "ecx",
        "cx": "ecx",
        "dl": "edx",
        "dx": "edx",
    }
    out = {full.get(n, n) for n in names}
    return sorted(n for n in out if n in ("eax", "ebx", "ecx", "edx", "esi", "edi"))


def _cmp_flags(win_cur, va):
    """A window that ENDS in the compare hands its flags to the instruction after it (a jcc outside the window): the cave ends
    in the same compare, so the flags are the same by construction. Otherwise the cave's flags must be dead at every exit."""
    return "equal" if _decode(win_cur, va)[-1].mnemonic == "cmp" else "dead"


def nop_fill(n):
    return b"\x90" * n


def cave_site_bytes(site_va, win_len, cave_va):
    return _jmp(site_va, cave_va) + nop_fill(win_len - 5)


# ---- hermetic proofs (no exe) ----------------------------------------------------------------------------------------


def prove(doc, reloc, limits, n_reloc_caves, n_limit_caves, lay):
    """-> [failures]. Census coverage: every always_on row is a site or inside an always-on cave window; every raise_switched
    row is a limit site or inside a limit cave window; no `unchanged` row's field is overwritten."""
    f = []
    rows = rows_of(doc)
    wins = windows_of(doc)
    rl = [(s["va"], s["va"] + len(s["exp"]), s["kind"]) for s in reloc]
    ll = [(s["va"], s["va"] + len(s["exp"]), s["kind"]) for s in limits]

    def covered(lst, lo, hi):
        return any(a <= lo and hi <= b for a, b, _k in lst)

    for r in rows:
        lo, hi = r["fva"], r["fva"] + r["w"]
        if r["policy"] == "always_on" and not covered(rl, lo, hi):
            f.append("path: always_on row %#x (%s) is not patched" % (r["va"], r["cls"]))
        if r["policy"] == "raise_switched" and not covered(ll, lo, hi):
            f.append("path: raise_switched row %#x (%s) has no limit site" % (r["va"], r["cls"]))
        if r["policy"] == "unchanged":
            for a, b, k in rl + ll:
                if a < hi and lo < b:
                    f.append(
                        "path: unchanged row %#x (%s) is overlapped by a %s site"
                        % (r["va"], r["cls"], k)
                    )
    # new values stay inside the new arrays
    for s in reloc:
        if s["kind"] == "pbase" and "buffer" in s["note"]:
            t = u32(s["new"])
            if not lay.buf - sp.BUF_BIAS <= t < lay.buf + BUF_TOTAL:
                f.append("path: buffer base %#x bakes %#x outside the new buffer" % (s["va"], t))
        if s["kind"] == "pbase" and "flags" in s["note"]:
            t = u32(s["new"])
            if not lay.flags <= t < lay.flags + FLAGS_TOTAL:
                f.append("path: flags base %#x bakes %#x outside the new flags" % (s["va"], t))
    if lay.buf + BUF_TOTAL > lay.flags:
        f.append("path: buffers overlap flags")
    if FLAGS_OFF + FLAGS_TOTAL > HOST_VSIZE:
        f.append("path: pool does not fit the host section")
    if n_reloc_caves != 17:
        f.append(
            "path: expected 17 always-on caves (16 flags strides + the clear bound), got %d"
            % n_reloc_caves
        )
    n_direct = sum(1 for s in limits if s["kind"] == "pcave" and s["note"].startswith("idread@"))
    if n_limit_caves != 10 + n_direct:
        f.append(
            "path: expected %d raise caves (6 scanner bounds, 3 id initialisers, attach, %d direct id reads), got %d"
            % (10 + n_direct, n_direct, n_limit_caves)
        )
    n_read = sum(1 for s in limits if s["kind"] == "praise") + n_direct
    if n_read != 54:
        f.append("path: expected 54 id reads, got %d" % n_read)
    return f
