#!/usr/bin/env python3
"""scan_cap_reloc_sites.py -- PT-CAP8 S0: the OFFLINE, whole-.text relocation-site census.

WHY THIS EXISTS. The July cap raise found its relocation sites through Ghidra (a reference list plus
an operand-scalar pass plus an IMUL-stride finder, in the Ghidra-side pool generator), carried the
list to EN through a delta map, and never re-measured it against the EN binary with an independent
reader. The PT-CAP8 research found a live function it missed:
llm_strat_bldg_link_to_network_if_adjacent 0x492716..0x4927f0 -- four `IMUL r,r,0x6aa4` row
strides and four `[EAX + 0xc3d2a2]` bases, in the TAIL of a function whose first 30 sites ARE
patched. A tool that depends on one analysis engine's ref list cannot tell you what that list
omitted; this one reads the bytes.

WHAT IT DOES. Linear-sweeps every function body AND the gaps between them in the clean EN image
(capstone, exact operand fields), and records every 4-byte field that encodes
  disp      a memory displacement inside an array  (`[reg + base+field]`, `[abs]`)
  imm_addr  an immediate address inside an array   (`MOV r,base`, `PUSH base`, `MOV [m],base`)
  row_stride  an imm32 == the array's row stride    (any mnemonic for the big strides, IMUL-only for
              the small ones whose value collides with unrelated constants)
  row_multiple  imm32 == k * row stride (k = 2..7)  (a folded player offset)
  block_size  imm32 == players * row stride         (save / load / clear sizes)
  end       a value exactly at an array's end that is nobody's base (a loop sentinel)
  data_ptr  a dword in an initialised data section pointing into an array
for the arrays PT-CAP8 touches (the v0.2.2 set AND units/soldiers, so PT-CAP9 inherits the list).
Each field is then tested against the BYTE EXTENTS of the cap manifests: covered / partial / none.
Uncovered fields get a liveness verdict from the committed call graph (forward reachability from
the entry point and every address-taken function) with the evidence chain.

Raw cross-check: every in-range dword anywhere in .text is also located by a plain byte search; a
raw hit that no decoded field explains is listed as `raw_only` (a decoder desync, an in-code table,
or a hand-built operand -- review each; none is silently dropped).

DATA. tools/data/cap_reloc_scan.json (summary + uncovered + baseline) and
tools/data/cap_reloc_hits.json (every field, compact -- the input of --check and the mutations).
Re-deriving needs the clean EN exe (md5 b3b389e8...): `--rescan`. The lint arm (`--check`) is
hermetic: it re-tests the committed fields against the CURRENT manifests, so a manifest edit that
drops a site, or a new uncovered live field in the baseline's place, goes red without any binary.

usage:
  python tools/scan_cap_reloc_sites.py --rescan [--exe PATH]   # decode the exe, rewrite both files
  python tools/scan_cap_reloc_sites.py --check                 # lint: committed fields vs manifests
  python tools/scan_cap_reloc_sites.py --check --strict        # S1 gate: ANY uncovered live field fails
  python tools/scan_cap_reloc_sites.py --selftest              # planted mutations must go red
  python tools/scan_cap_reloc_sites.py --report                # human summary of the committed data
"""

import argparse
import bisect
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DATA = os.path.join(REPO, "tools", "data")
PATCHER = os.path.join(REPO, "src", "patcher")
SUMMARY = os.path.join(DATA, "cap_reloc_scan.json")
HITS = os.path.join(DATA, "cap_reloc_hits.json")
EN_FUNCTIONS = os.path.join(DATA, "en_functions.json")
CALL_GRAPH = os.path.join(DATA, "call_graph_no_crt.json")

EN_MD5 = "b3b389e8eb228638de8dbbc5c228118b"

# The manifests whose byte extents count as "covered". Since PT-CAP8 S1 the closure is the v0.2.2 pair
# the globs below pick up (cap_v022_reloc_EN + cap_v022_limits_EN, written by gen_cap_v022_manifests.py; since PT-CAP9-U1
# the cap_v023_units_{reloc,limits}_EN pair too, written by gen_cap_units_manifests.py -- units and soldiers are gated);
# the July grand + pool manifests are the generator's INPUT and no longer count.
CLOSURE_MANIFESTS = []
CLOSURE_GLOBS = ["cap_v022_*.mh.patch.json", "cap_v023_*.mh.patch.json"]

PLAYERS = 8
# name, base, element stride, per-player count, scope. row = elem * count (1-D pool: row == size).
ARRAYS = [
    ("buildings", 0xC3D2A0, 273, 100, "v0.2.2"),
    ("unit_storage", 0xC727C0, 244, 25, "v0.2.2"),
    ("turrets", 0xCC0FE0, 55, 32, "v0.2.2"),
    ("mines", 0xD03480, 56, 32, "v0.2.2"),
    ("labs", 0xD02D80, 8, 25, "v0.2.2"),
    ("productions", 0xDD2648, 405, 8, "v0.2.2"),
    ("units", 0xDD8C48, 233, 100, "PT-CAP9"),
    ("soldiers", 0xE0E5A8, 29, 100, "PT-CAP9"),
]
PROJECTILE = ("projectiles", 0xE1DB98, 121, 1500, "v0.2.2")
# Row strides this small collide with unrelated constants; they only count under IMUL.
SMALL_STRIDE = 10000
# A non-IMUL stride immediate is kept without a same-function array reference only when this big.
BIG_STRIDE = 20000


def arrays():
    out = []
    for name, base, elem, count, scope in ARRAYS:
        row = elem * count
        out.append(
            {
                "name": name,
                "base": base,
                "elem": elem,
                "count": count,
                "row": row,
                "size": PLAYERS * row,
                "scope": scope,
                "players": PLAYERS,
            }
        )
    name, base, elem, count, scope = PROJECTILE
    out.append(
        {
            "name": name,
            "base": base,
            "elem": elem,
            "count": count,
            "row": elem * count,
            "size": elem * count,
            "scope": scope,
            "players": 1,
        }
    )
    return out


# ---- PE image -----------------------------------------------------------------------------------


class Image:
    def __init__(self, raw):
        self.raw = raw
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        n = struct.unpack_from("<H", raw, pe + 6)[0]
        opt = struct.unpack_from("<H", raw, pe + 20)[0]
        self.base = struct.unpack_from("<I", raw, pe + 24 + 28)[0]
        self.entry = self.base + struct.unpack_from("<I", raw, pe + 24 + 16)[0]
        self.secs = []
        for i in range(n):
            o = pe + 24 + opt + i * 40
            name = raw[o : o + 8].rstrip(b"\0").decode("latin1")
            vs, va, rs, ro, chars = (
                struct.unpack_from("<I", raw, o + 8)[0],
                struct.unpack_from("<I", raw, o + 12)[0],
                struct.unpack_from("<I", raw, o + 16)[0],
                struct.unpack_from("<I", raw, o + 20)[0],
                struct.unpack_from("<I", raw, o + 36)[0],
            )
            self.secs.append(
                {
                    "name": name,
                    "va": self.base + va,
                    "vsize": vs,
                    "raw_size": rs,
                    "raw_off": ro,
                    "chars": chars,
                }
            )

    def read(self, va, n):
        for s in self.secs:
            if s["va"] <= va < s["va"] + s["raw_size"]:
                off = s["raw_off"] + (va - s["va"])
                return self.raw[off : off + n]
        return b""

    def exec_sections(self):
        return [s for s in self.secs if s["chars"] & 0x20000000]

    def data_sections(self):
        """Initialised, non-code sections that can hold absolute pointers. .idata/.reloc/.rsrc hold
        RVAs and resource data -- their dwords are not addresses."""
        skip = {".idata", ".reloc", ".rsrc"}
        return [
            s
            for s in self.secs
            if not s["chars"] & 0x20000000
            and s["raw_size"]
            and s["chars"] & 0x40
            and s["name"] not in skip
        ]


# ---- classification of one 32-bit value -----------------------------------------------------------


class Classifier:
    def __init__(self, arrs):
        self.arrs = arrs
        self.bases = sorted((a["base"], a) for a in arrs)
        self.starts = [b for b, _ in self.bases]
        base_set = {a["base"] for a in arrs}
        self.ends = {
            a["base"] + a["size"]: a for a in arrs if a["base"] + a["size"] not in base_set
        }

    def addr(self, v):
        """-> array dict when v is inside [base, base+size), else None."""
        i = bisect.bisect_right(self.starts, v) - 1
        if i >= 0:
            a = self.bases[i][1]
            if a["base"] <= v < a["base"] + a["size"]:
                return a
        return None

    def end(self, v):
        return self.ends.get(v)

    def stride(self, v, mnemonic):
        """-> (array, role) for a row-stride-shaped immediate."""
        hits = []
        for a in self.arrs:
            if a["players"] == 1:
                continue
            row = a["row"]
            if v == row and (row >= SMALL_STRIDE or mnemonic == "imul"):
                hits.append((a, "row_stride"))
            elif v == a["size"]:
                hits.append((a, "block_size"))
            elif row >= SMALL_STRIDE and v % row == 0 and 2 <= v // row <= 7:
                hits.append((a, "row_multiple"))
        for a in self.arrs:
            if a["players"] == 1 and v == a["size"]:
                hits.append((a, "block_size"))
        return hits


# ---- decoding ---------------------------------------------------------------------------------------


def _emit(ins, cls, fn_name, out, X86_OP_MEM, X86_OP_IMM):
    """Append the relocation-relevant 4-byte fields of one decoded instruction to out."""
    for op in ins.operands:
        if op.type == X86_OP_MEM:
            if ins.disp_size != 4:
                continue
            d = op.mem.disp & 0xFFFFFFFF
            a = cls.addr(d)
            if a is not None:
                role = "abs" if (op.mem.base == 0 and op.mem.index == 0) else "disp"
                out.append(_f(ins, ins.disp_offset, 4, a["name"], role, fn_name, d))
            else:
                e = cls.end(d)
                if e is not None:
                    out.append(_f(ins, ins.disp_offset, 4, e["name"], "end", fn_name, d))
        elif op.type == X86_OP_IMM:
            if ins.imm_size != 4:
                continue
            v = op.imm & 0xFFFFFFFF
            a = cls.addr(v)
            if a is not None:
                out.append(_f(ins, ins.imm_offset, 4, a["name"], "imm_addr", fn_name, v))
                continue
            e = cls.end(v)
            if e is not None:
                out.append(_f(ins, ins.imm_offset, 4, e["name"], "end", fn_name, v))
                continue
            if ins.mnemonic in ("and", "or", "xor", "test"):
                continue  # a mask constant is not a size or a stride
            for arr, role in cls.stride(v, ins.mnemonic):
                out.append(_f(ins, ins.imm_offset, 4, arr["name"], role, fn_name, v))


def decode_region(md, code, start_va, cls, fn_name, out, X86_OP_MEM, X86_OP_IMM, mask=None):
    """Linear sweep with resync on undecodable bytes. Appends field dicts to out."""
    off = 0
    n = len(code)
    while off < n:
        progressed = False
        for ins in md.disasm(code[off:], start_va + off):
            progressed = True
            off = ins.address - start_va + ins.size
            if mask is not None:
                mask[ins.address] = 1
                for k in range(1, ins.size):
                    mask[ins.address + k] = 2  # inside an instruction
            _emit(ins, cls, fn_name, out, X86_OP_MEM, X86_OP_IMM)
        if not progressed:
            off += 1  # undecodable byte: resync


def recursive_descent(md, code, entry, end, img, tables=None):
    """-> {addr: instruction} reachable from `entry` by following fall-through and branch edges,
    including the targets of `jmp [reg*4 + table]` (the table is read out of the image). Unlike the
    linear sweep this never decodes the data a function embeds (switch tables), so it cannot be
    desynchronised by it -- the failure the 0x4797d8 stride showed (a linear sweep mis-decodes the
    bytes after an in-code table and swallows the real instruction boundary)."""
    from capstone.x86 import X86_GRP_JUMP, X86_GRP_RET, X86_OP_IMM, X86_OP_MEM

    got = {}
    work = [entry]
    while work:
        a = work.pop()
        while entry <= a <= end and a not in got:
            off = a - entry
            ins = next(md.disasm(code[off : off + 16], a, 1), None)
            if ins is None:
                break
            got[a] = ins
            if ins.group(X86_GRP_RET):
                break
            if ins.group(X86_GRP_JUMP):
                op = ins.operands[0] if ins.operands else None
                if op is not None and op.type == X86_OP_IMM:
                    work.append(op.imm & 0xFFFFFFFF)
                elif op is not None and op.type == X86_OP_MEM and (op.mem.index or op.mem.base):
                    # `jmp [reg*4 + table]` and Watcom's `jmp cs:[reg + table]` (reg pre-shifted): the table
                    # is a run of in-function code addresses
                    t = op.mem.disp & 0xFFFFFFFF
                    found = []
                    for k in range(512):
                        raw = img.read(t + 4 * k, 4)
                        if len(raw) < 4:
                            break
                        v = struct.unpack("<I", raw)[0]
                        if not (entry <= v <= end):
                            break
                        found.append(v)
                        work.append(v)
                    if tables is not None and found:
                        tables[a] = found
                if ins.mnemonic == "jmp":
                    break
            a += ins.size
    return got


def decode_function(md, img, entry, end, cls, name, out, mask, X86_OP_MEM, X86_OP_IMM):
    """Recursive descent first (authoritative), then the linear sweep for whatever it did not reach.
    A linear-sweep field is dropped when recursive descent decoded those bytes differently."""
    code = img.read(entry, end - entry + 1)
    rd = recursive_descent(md, code, entry, end, img)
    cover = {}
    for a, ins in rd.items():
        for k in range(ins.size):
            cover[a + k] = a
    rdf = []
    for ins in rd.values():
        _emit(ins, cls, name, rdf, X86_OP_MEM, X86_OP_IMM)
        mask[ins.address] = 1
        for k in range(1, ins.size):
            mask[ins.address + k] = 2
    lin = []
    decode_region(md, code, entry, cls, name, lin, X86_OP_MEM, X86_OP_IMM, mask)
    keep = [
        f
        for f in lin
        if f["ins_va"] not in rd and not any((f["va"] + j) in cover for j in range(4))
    ]
    out.extend(rdf)
    out.extend(keep)
    return len(rdf), len(keep)


def _f(ins, field_off, w, array, role, fn, value):
    return {
        "va": ins.address + field_off,
        "w": w,
        "array": array,
        "role": role,
        "fn": fn,
        "value": value,
        "ins_va": ins.address,
        "ins": "%s %s" % (ins.mnemonic, ins.op_str),
    }


def scan_image(img, arrs, funcs):
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    cls = Classifier(arrs)
    fields = []
    text = None
    mask = {}
    for s in img.exec_sections():
        text = s
        lo = s["va"]
        hi = s["va"] + s["raw_size"]
        cur = lo
        for entry, end, name in funcs:
            if end < lo or entry >= hi:
                continue
            if entry > cur:
                decode_region(
                    md,
                    img.read(cur, entry - cur),
                    cur,
                    cls,
                    None,
                    fields,
                    X86_OP_MEM,
                    X86_OP_IMM,
                    mask,
                )
            decode_function(md, img, entry, end, cls, name, fields, mask, X86_OP_MEM, X86_OP_IMM)
            cur = max(cur, end + 1)
        if cur < hi:
            decode_region(
                md, img.read(cur, hi - cur), cur, cls, None, fields, X86_OP_MEM, X86_OP_IMM, mask
            )
    # Stride / size immediates are weak evidence on their own (1792, 3240, 1600 are ordinary numbers):
    # keep one only when its function also addresses the same array, or when the value is a big,
    # distinctive stride (>= SMALL_STRIDE handled in Classifier). `noise` is counted, never silent.
    refs = {(f["fn"], f["array"]) for f in fields if f["role"] in ("disp", "abs", "imm_addr")}
    kept, noise = [], 0
    for f in fields:
        if f["role"] in ("row_multiple", "block_size") or (
            f["role"] == "row_stride" and not f["ins"].startswith("imul")
        ):
            big = f["role"] == "row_stride" and f["value"] >= BIG_STRIDE
            if not big and (f["fn"], f["array"]) not in refs:
                noise += 1
                continue
        kept.append(f)
    fields = kept
    # data-section pointers
    data_hits = []
    for s in img.data_sections():
        blob = img.read(s["va"], s["raw_size"])
        for k in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, k)[0]
            a = cls.addr(v)
            if a is not None:
                data_hits.append(
                    {
                        "va": s["va"] + k,
                        "w": 4,
                        "array": a["name"],
                        "role": "data_ptr",
                        "fn": None,
                        "value": v,
                        "ins_va": s["va"] + k,
                        "ins": "dword in " + s["name"],
                    }
                )
    # raw cross-check on the code
    explained = {f["va"] for f in fields}
    raw_only = []
    coincidence = 0
    for s in img.exec_sections():
        blob = img.read(s["va"], s["raw_size"])
        for a in arrs:
            lo, hi = a["base"], a["base"] + a["size"]
            for k in range(len(blob) - 3):
                v = struct.unpack_from("<I", blob, k)[0]
                if lo <= v < hi and (s["va"] + k) not in explained:
                    va = s["va"] + k
                    # A dword that lies wholly inside decoded instructions, at a position that is not one of
                    # their operand fields, is a byte coincidence (e.g. `xx xx c3 00` across two opcodes).
                    # Only a hit touching bytes NO instruction decoded is a lead (data in code, desync).
                    if all(va + j in mask for j in range(4)):
                        coincidence += 1
                        continue
                    raw_only.append(
                        {"va": va, "w": 4, "array": a["name"], "role": "raw_only", "value": v}
                    )
    return fields, data_hits, raw_only, coincidence, noise


# ---- strength-reduced row strides (shift/add chains) ---------------------------------------------------------
#
# The immediate scan above sees `IMUL r,r,27300`. It cannot see a stride the compiler built from shifts:
#     SHL EAX,2 ; MOV EDX,EAX ; SHL EAX,4 ; SUB EAX,EDX ; MOV EDX,EAX ; SHL EAX,6 ; ... [EDX+EAX+0xc3d2a0]
# (= 975 * 28 * player = 27300 * player). July's Ghidra-side shift-stride finder
# matched ONE register layout of that core. This pass is layout-independent: for every memory operand that
# lands in an array it runs a tiny symbolic evaluator over the preceding instructions and reads the
# effective address as a linear form over the registers' unknown starting values. A coefficient that is a
# multiple of the array's row stride (or of 975 for the buildings family) IS a row-stride term, however it
# was spelled. Uses whose chain is spelled with an IMUL are covered by that IMUL's field; uses spelled with
# shifts need a shift-splice patch inside the chain window.

CHAIN_WINDOW = 30  # (legacy name; see CHAIN_VARIANTS)
_PARENT = {
    "ax": "eax",
    "al": "eax",
    "ah": "eax",
    "bx": "ebx",
    "bl": "ebx",
    "bh": "ebx",
    "cx": "ecx",
    "cl": "ecx",
    "ch": "ecx",
    "dx": "edx",
    "dl": "edx",
    "dh": "edx",
    "si": "esi",
    "sil": "esi",
    "di": "edi",
    "dil": "edi",
    "bp": "ebp",
    "sp": "esp",
}
_GPR = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")


class _Win:
    def __init__(self):
        self.regs = {}
        self.n = 0

    def fresh(self, addr):
        self.n += 1
        return ({"s%d" % self.n: 1}, addr)

    def get(self, name):
        name = _PARENT.get(name, name)
        if name not in self.regs:
            self.regs[name] = ({"r0_" + name: 1}, None)
        return self.regs[name]


def _lin(a, b, sign=1):
    out = dict(a)
    for k, v in b.items():
        out[k] = out.get(k, 0) + sign * v
        if out[k] == 0:
            del out[k]
    return out


def _scale(a, m):
    return {k: v * m for k, v in a.items() if v * m}


def _omin(*o):
    o = [x for x in o if x is not None]
    return min(o) if o else None


def _ea(win, ins, op):
    """-> (form, origin) of a memory operand's address (displacement included as const key '')."""
    f, o = {}, None
    m = op.mem
    if m.base:
        bf, bo = win.get(ins.reg_name(m.base))
        f, o = _lin(f, bf), _omin(o, bo)
    if m.index:
        xf, xo = win.get(ins.reg_name(m.index))
        f, o = _lin(f, _scale(xf, m.scale)), _omin(o, xo)
    d = m.disp & 0xFFFFFFFF
    if d >= 0x80000000:
        d -= 1 << 32
    if d:
        f = _lin(f, {"": d})
    return f, o


def _slot(ins, op):
    """Key of a dword EBP-frame stack slot operand (`[ebp +/- d]`, no index), else None."""
    from capstone.x86 import X86_OP_MEM

    if op.type != X86_OP_MEM or op.size != 4:
        return None
    m = op.mem
    if m.index or not m.base or ins.reg_name(m.base) != "ebp" or m.segment:
        return None
    return "m%+d" % m.disp


def _is_ebp_mem(ins, op):
    from capstone.x86 import X86_OP_MEM

    return (
        op.type == X86_OP_MEM
        and op.mem.base
        and ins.reg_name(op.mem.base) == "ebp"
        and not op.mem.index
    )


def step(win, ins):
    """Apply one instruction to the window state (unknown effects become fresh symbols).

    State keys are the full GPRs AND dword EBP-frame stack slots ("m-20" = [ebp-0x14]): the compiler spills
    the player index (or a half-built stride) to a local and reloads it into the shift/add chain, which a
    registers-only evaluator loses (PT-CAP8 gap site 0x4db3d7). A slot is invalidated by any sub-dword or
    unmodelled write to it; stores through other pointers are assumed not to alias the frame."""
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

    mn = ins.mnemonic
    ops = ins.operands
    a = ins.address
    d = None
    if ops and ops[0].type == X86_OP_REG:
        n0 = ins.reg_name(ops[0].reg)
        d = n0 if n0 in _GPR else None
    elif ops:
        d = _slot(ins, ops[0])
    full = d is not None
    dreg = full and d in _GPR
    src = ops[1] if len(ops) > 1 else None

    def srcform(op):
        if op.type == X86_OP_IMM:
            return ({"": op.imm}, None)
        if op.type == X86_OP_REG:
            n = ins.reg_name(op.reg)
            if n not in _GPR:
                return win.fresh(a)
            return win.get(n)
        k = _slot(ins, op)
        if k is not None:
            return win.get(k)
        return win.fresh(a)

    def srcok(op):
        return op.type != X86_OP_MEM or _slot(ins, op) is not None

    if mn == "mov" and full and src is not None:
        if src.type == X86_OP_MEM and _slot(ins, src) is None:
            win.regs[d] = win.fresh(a)
        else:
            f, o = srcform(src)
            win.regs[d] = (dict(f), _omin(o, a) if src.type != X86_OP_IMM else o)
        return
    if mn in ("add", "sub") and full and src is not None and srcok(src):
        sf, so = srcform(src)
        df, do = win.get(d)
        win.regs[d] = (_lin(df, sf, 1 if mn == "add" else -1), _omin(do, so, a))
        return
    if (
        mn in ("shl", "sal")
        and full
        and src is not None
        and src.type == X86_OP_IMM
        and 0 < src.imm < 31
    ):
        df, do = win.get(d)
        win.regs[d] = (_scale(df, 1 << src.imm), _omin(do, a))
        return
    if mn == "imul" and dreg and len(ops) == 3 and ops[2].type == X86_OP_IMM:
        sf, so = srcform(ops[1]) if srcok(ops[1]) and ops[1].type != X86_OP_IMM else win.fresh(a)
        win.regs[d] = (_scale(sf, ops[2].imm), _omin(so, a))
        return
    if mn == "lea" and dreg and src is not None and src.type == X86_OP_MEM:
        f, o = _ea(win, ins, src)
        win.regs[d] = (f, _omin(o, a))
        return
    if (
        mn == "xor"
        and dreg
        and src is not None
        and src.type == X86_OP_REG
        and ins.reg_name(src.reg) == d
    ):
        win.regs[d] = ({}, a)
        return
    if mn == "neg" and full:
        df, do = win.get(d)
        win.regs[d] = (_scale(df, -1), _omin(do, a))
        return
    if mn in ("inc", "dec") and full:
        df, do = win.get(d)
        win.regs[d] = (_lin(df, {"": 1}, 1 if mn == "inc" else -1), _omin(do, a))
        return
    if mn in ("cmp", "test", "push", "call", "jmp") or mn.startswith("j"):
        pass
    elif ops and _is_ebp_mem(ins, ops[0]):
        # an unmodelled (or sub-dword) write to a frame slot: forget every slot it could touch
        m0 = ops[0].mem
        for k in [k for k in win.regs if k.startswith("m")]:
            if abs(int(k[1:]) - m0.disp) < 4:
                win.regs[k] = win.fresh(a)
    try:
        _r, w = ins.regs_access()
    except Exception:
        w = ()
    for rid in w:
        n = _PARENT.get(ins.reg_name(rid), ins.reg_name(rid))
        if n in _GPR:
            win.regs[n] = win.fresh(a)


def _regions_with_gaps(img, funcs):
    """funcs plus the byte ranges BETWEEN them (named None). The 2026-10 miss: 0x4d516c..0x4d5244 is live
    code Ghidra never made a function (a prune loop after engage_candidate_add); the chain pass walked
    only the function table, so a shift chain there was invisible while the immediate scan (which does
    decode gaps) saw its [eax+base] displacement and called it covered."""
    out = list(funcs)
    try:
        secs = img.exec_sections()
    except AttributeError:
        return out
    for s in secs:
        lo, hi = s["va"], s["va"] + s["raw_size"]
        cur = lo
        for entry, end, _n in funcs:
            if end < lo or entry >= hi:
                continue
            if entry > cur:
                out.append((cur, entry - 1, None))
            cur = max(cur, end + 1)
        if cur < hi:
            out.append((cur, hi - 1, None))
    return sorted(out, key=lambda r: r[0])


# (window, reset_at_unconditional_jump). Linear sweep ignores control flow, so a long window drags in a
# previous basic block's constants (`xor esi,esi` before a `jmp` makes the next block's player index look
# like 0 and the stride term vanishes); the reset variant cuts the window at `jmp`/`ret`. The short
# no-reset variant is the pre-2026-10 behaviour. Their UNION is the census (a hit any variant sees counts).
CHAIN_VARIANTS = ((30, False), (60, True))


def _touches_array(cls, x, X86_OP_IMM, X86_OP_MEM):
    for op in x.operands:
        if op.type == X86_OP_IMM and cls.addr(op.imm & 0xFFFFFFFF) is not None:
            return True
        if op.type == X86_OP_MEM and cls.addr(op.mem.disp & 0xFFFFFFFF) is not None:
            return True
    return False


def scan_stride_chains(img, funcs, arrs):
    """-> list of hit dicts (role shift_chain / imul_chain) for every memory use whose effective address
    carries a row-stride term toward an array. value = the chain's first contributing instruction.
    Three things beyond the original per-function register evaluator (PT-CAP8 follow-up, 2026-10-20):
      * stack spills: the player index / a half-built stride stored to `[ebp-d]` and reloaded (0x4db3d7);
      * the gaps BETWEEN Ghidra's functions (0x4d516c..0x4d5244, never made a function);
      * an array base arriving in a register (`mov r,base` / `add r,base`), not only as the operand's
        displacement, so the final dereference carries no array displacement at all."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    cls = Classifier(arrs)
    out = {}
    for entry, end, name in _regions_with_gaps(img, funcs):
        code = img.read(entry, end - entry + 1)
        ins = list(md.disasm(code, entry))
        touch = [_touches_array(cls, x, X86_OP_IMM, X86_OP_MEM) for x in ins]
        for i, x in enumerate(ins):
            for op in x.operands:
                if op.type != X86_OP_MEM or not (op.mem.base or op.mem.index):
                    continue
                for window, reset in CHAIN_VARIANTS:
                    lo = max(0, i - window)
                    if not any(touch[lo : i + 1]):
                        continue
                    win = _Win()
                    for y in ins[lo:i]:
                        if reset and y.mnemonic in ("jmp", "ret", "retn"):
                            win = _Win()
                            continue
                        step(win, y)
                    f, o = _ea(win, x, op)
                    base = f.get("", 0) & 0xFFFFFFFF
                    arr = cls.addr(base)
                    if arr is None or arr["players"] == 1:
                        continue
                    row = arr["row"]
                    stride = False
                    for k, c in f.items():
                        if k == "":
                            continue
                        if c % row == 0 or (
                            arr["name"] == "buildings" and c % 975 == 0 and abs(c) >= 975
                        ):
                            stride = True
                    if not stride:
                        continue
                    key = (x.address, arr["name"])
                    if key in out:
                        continue
                    has_imul = any(
                        y.mnemonic == "imul"
                        and any(o2.type == X86_OP_IMM and o2.imm == row for o2 in y.operands)
                        for y in ins[lo : i + 1]
                    )
                    out[key] = {
                        "va": x.address,
                        "w": x.size,
                        "array": arr["name"],
                        "role": "imul_chain" if has_imul else "shift_chain",
                        "fn": name,
                        "value": o if o is not None else x.address,
                        "ins_va": x.address,
                        "ins": "%s %s" % (x.mnemonic, x.op_str),
                    }
    return [out[k] for k in sorted(out)]


# ---- addresses taken (for liveness) -------------------------------------------------------------------


def address_taken(img, entries):
    """Entries that appear as an absolute dword anywhere in the image (callbacks, tables)."""
    ent = set(entries)
    taken = {}
    for s in img.secs:
        if not s["raw_size"]:
            continue
        blob = img.read(s["va"], s["raw_size"])
        for k in range(len(blob) - 3):
            v = struct.unpack_from("<I", blob, k)[0]
            if v in ent:
                taken.setdefault(v, []).append(s["va"] + k)
    return taken


# Roles a manifest must cover. `end` (a value equal to an array's end, which is usually the NEXT
# object's base) and `data_ptr` (an initialised-data dword that happens to fall in a range) are listed
# for review but never gate: neither has an instruction that dereferences the array.
GATING = {"disp", "abs", "imm_addr", "row_stride", "row_multiple", "block_size", "shift_chain"}


def hit_status(cov, f):
    if f["role"] == "shift_chain":
        return "covered" if cov.test_chain(f["value"], f["va"] + f["w"]) else "none"
    return cov.test(f["va"], f["w"])


def load_funcs():
    doc = json.load(open(EN_FUNCTIONS, encoding="utf-8"))
    return sorted((int(r["entry"], 16), int(r["end"], 16), r["name"]) for r in doc["functions"])


def reachability(funcs, taken, entry_point):
    """Forward reachability over direct-call + tail-jump edges from the entry point and every
    address-taken function. -> {entry: ('root', why) | ('via', caller_entry)}"""
    g = json.load(open(CALL_GRAPH, encoding="utf-8"))
    adj = {}
    callers = {}
    for e in g["links"] + g.get("tail_jumps", []):
        try:
            s, t = int(e["source"], 16), int(e["target"], 16)
        except ValueError:
            continue  # EXTERNAL:... import targets
        adj.setdefault(s, set()).add(t)
        callers.setdefault(t, set()).add(s)
    entries = {f[0] for f in funcs}
    find = body_finder(funcs)
    roots = {}
    ep = find(entry_point)
    if ep:
        roots[ep[0]] = "pe-entry-point"
    for v, where in taken.items():
        if v in entries:
            roots.setdefault(v, "address-taken at " + ", ".join("0x%08x" % w for w in where[:3]))
    seen = dict((r, ("root", why)) for r, why in roots.items())
    todo = list(roots)
    while todo:
        cur = todo.pop()
        for t in adj.get(cur, ()):
            if t not in seen:
                seen[t] = ("via", cur)
                todo.append(t)
    return seen, callers


def body_finder(funcs):
    starts = [f[0] for f in funcs]

    def find(va):
        i = bisect.bisect_right(starts, va) - 1
        if i < 0 or va > funcs[i][1]:
            return None
        return funcs[i]

    return find


def liveness(fn_entry, seen, callers, names):
    """-> dict(verdict, chain, callers) for one function entry."""
    c = sorted(callers.get(fn_entry, ()))
    cn = [names.get(x, "0x%08x" % x) for x in c][:6]
    if fn_entry in seen:
        chain, cur = [], fn_entry
        while len(chain) < 12:
            kind, ref = seen[cur]
            chain.append(names.get(cur, "0x%08x" % cur))
            if kind == "root":
                chain.append("[%s]" % ref)
                break
            cur = ref
        return {"verdict": "live", "chain": chain, "callers": cn}
    return {"verdict": "dead", "chain": [], "callers": cn}


# ---- manifest extents ---------------------------------------------------------------------------------


def manifest_extents(patcher=PATCHER, names=None, globs=None):
    """-> sorted [(lo, hi, manifest)] of every patch in the closure manifests."""
    import glob as _g

    paths = []
    for n in names if names is not None else CLOSURE_MANIFESTS:
        paths.append(os.path.join(patcher, n + ".mh.patch.json"))
    for pat in globs if globs is not None else CLOSURE_GLOBS:
        paths.extend(sorted(_g.glob(os.path.join(patcher, pat))))
    out = []
    for p in paths:
        if not os.path.isfile(p):
            continue
        mname = os.path.basename(p)[: -len(".mh.patch.json")]
        for pt in json.load(open(p, encoding="utf-8")).get("patches", []):
            if "va" not in pt:
                continue
            ln = len(bytes.fromhex(pt.get("bytes", "").replace(" ", "")))
            if ln:
                va = int(str(pt["va"]), 16)
                kind = (
                    "splice"
                    if str(pt.get("_note", "")).startswith("shift row-stride splice")
                    else "field"
                )
                out.append((va, va + ln, mname, kind))
    out.sort()
    return out


class Coverage:
    def __init__(self, extents):
        self.ext = extents
        self.starts = [e[0] for e in extents]
        # running max of hi so a long splice that starts early is still found
        self.maxhi = []
        m = 0
        for e in extents:
            m = max(m, e[1])
            self.maxhi.append(m)

    def test_chain(self, start, end):
        """True when a shift-splice patch lies wholly inside [start, end) -- the chain was rewritten."""
        for e in self.ext:
            if (e[3] if len(e) > 3 else "field") == "splice" and start <= e[0] and e[1] <= end:
                return True
        return False

    def test(self, va, w):
        """-> 'covered' | 'partial' | 'none' for the field [va, va+w)."""
        hi = va + w
        i = bisect.bisect_left(self.starts, hi) - 1
        partial = False
        while i >= 0 and self.maxhi[i] > va:
            lo2, hi2 = self.ext[i][0], self.ext[i][1]
            if lo2 <= va and hi <= hi2:
                return "covered"
            if lo2 < hi and va < hi2:
                partial = True
            i -= 1
        return "partial" if partial else "none"


# ---- the committed data -----------------------------------------------------------------------------------


def pack_hits(fields, fn_names):
    idx = {n: i for i, n in enumerate(sorted({f["fn"] for f in fields if f["fn"]}))}
    names = sorted(idx, key=idx.get)
    rows = []
    for f in sorted(fields, key=lambda f: (f["va"], f["array"], f["role"])):
        rows.append([f["va"], f["w"], f["array"], f["role"], idx.get(f["fn"], -1), f["value"]])
    return {"fns": names, "cols": ["va", "w", "array", "role", "fn", "value"], "rows": rows}


def unpack_hits(doc):
    names = doc["fns"]
    out = []
    for r in doc["rows"]:
        out.append(
            {
                "va": r[0],
                "w": r[1],
                "array": r[2],
                "role": r[3],
                "fn": names[r[4]] if r[4] >= 0 else None,
                "value": r[5],
            }
        )
    return out


def rescan(exe_path):
    raw = open(exe_path, "rb").read()
    md5 = hashlib.md5(raw).hexdigest()
    if md5 != EN_MD5:
        print(
            "scan_cap_reloc_sites: %s is md5 %s, not the clean EN exe %s" % (exe_path, md5, EN_MD5)
        )
        return 1
    img = Image(raw)
    funcs = load_funcs()
    arrs = arrays()
    fields, data_hits, raw_only, coincidence, noise = scan_image(img, arrs, funcs)
    chains = [c for c in scan_stride_chains(img, funcs, arrs) if c["role"] == "shift_chain"]
    all_fields = fields + data_hits + chains
    taken = address_taken(img, [f[0] for f in funcs])
    seen, callers = reachability(funcs, taken, img.entry)
    names = {f[0]: f[2] for f in funcs}
    find = body_finder(funcs)
    cov = Coverage(manifest_extents())

    rows = pack_hits(all_fields, names)
    prior = []
    if os.path.isfile(SUMMARY):
        prior = json.load(open(SUMMARY, encoding="utf-8")).get("baseline_v022", [])
    summary = build_summary(
        arrs, all_fields, raw_only, coincidence, noise, cov, find, seen, callers, names, md5, prior
    )
    with open(HITS, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(rows, fh, separators=(",", ":"))
        fh.write("\n")
    with open(SUMMARY, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(summary, indent=2) + "\n")
    print_summary(summary)
    return 0


SAVE_FNS = ("map_SavePlanetToDisk", "map_LoadPlanetFromDisk")


def _save_layer(u):
    """A serialized-block size OR base operand in the save writer/reader. The v0.2.2 design (user,
    2026-10-19, D1) keeps the stock .bss as the canonical mirror: the save/load block calls address it
    with the VANILLA base and size, so no manifest is owed either -- the gather/scatter work lives in
    PT-CAP8 S1 Slice C and the trailer in S3."""
    return u["role"] in ("block_size", "disp", "abs", "imm_addr") and u["fn"] in SAVE_FNS


def build_summary(
    arrs, fields, raw_only, coincidence, noise, cov, find, seen, callers, names, md5, prior=()
):
    per = {}
    uncovered = []
    for f in fields:
        st = hit_status(cov, f)
        p = per.setdefault(f["array"], {})
        k = p.setdefault(f["role"], {"covered": 0, "partial": 0, "none": 0})
        k[st] += 1
        if st != "covered":
            fn = find(f["va"])
            lv = (
                liveness(fn[0], seen, callers, names)
                if fn
                else {"verdict": "orphan", "chain": [], "callers": []}
            )
            uncovered.append(
                {
                    "va": "0x%08x" % f["va"],
                    "w": f["w"],
                    "array": f["array"],
                    "role": f["role"],
                    "status": st,
                    "ins_va": "0x%08x" % f["ins_va"],
                    "ins": f["ins"],
                    "fn": fn[2] if fn else None,
                    "fn_entry": ("0x%08x" % fn[0]) if fn else None,
                    "liveness": lv["verdict"],
                    "chain": lv["chain"],
                    "callers": lv["callers"],
                }
            )
    uncovered.sort(key=lambda u: (u["array"], u["va"]))
    baseline = [
        {
            "va": u["va"],
            "array": u["array"],
            "role": u["role"],
            "liveness": u["liveness"],
            "state": "by_design" if _save_layer(u) else "open",
        }
        for u in uncovered
        if u["liveness"] != "dead" and u["role"] in GATING
    ]
    # An entry the previous baseline listed as OPEN that a manifest now covers is CLOSED, not dropped:
    # `closed` is what turns a later loss of the patch into a REGRESSION instead of silence.
    have = {(b["va"], b["array"], b["role"]) for b in baseline}
    for b in prior:
        k = (b["va"], b["array"], b["role"])
        if k not in have and b["state"] in ("open", "closed"):
            baseline.append(dict(b, state="closed"))
    baseline.sort(key=lambda b: (b["va"], b["array"], b["role"]))
    return {
        "_generated_by": "tools/scan_cap_reloc_sites.py --rescan",
        "_what": (
            "PT-CAP8 S0 whole-.text relocation-field census of the clean EN exe against the cap manifests' "
            "byte extents. Every field of every cataloged array, covered or not; the uncovered ones carry a "
            "liveness verdict. Fields themselves: tools/data/cap_reloc_hits.json."
        ),
        "exe_md5": md5,
        "closure_manifests": CLOSURE_MANIFESTS,
        "closure_globs": CLOSURE_GLOBS,
        "arrays": arrs,
        "coverage_by_array_role": per,
        "raw_coincidence_count": coincidence,
        "weak_stride_noise_dropped": noise,
        "raw_only_count": len(raw_only),
        "raw_only": [
            {
                "va": "0x%08x" % r["va"],
                "array": r["array"],
                "value": "0x%08x" % r["value"],
                "fn": (find(r["va"]) or [0, 0, None])[2],
            }
            for r in raw_only
        ],
        "uncovered_count": len(uncovered),
        "uncovered": uncovered,
        "baseline_v022": baseline,
        "_baseline_why": (
            "Uncovered NON-dead fields of the v0.2.2 arrays. state=open means PT-CAP8 S1 still owes the patch; "
            "--check goes red on any uncovered live field NOT listed here, on a listed field that is now "
            "covered (flip it to closed with --rescan), and, under --strict, on any open one. by_design = a "
            "save/load block size that stays vanilla under the trailer design (PT-CAP8 S3), never owed a patch."
        ),
    }


def print_summary(s):
    print("exe md5 %s" % s["exe_md5"])
    for a in s["arrays"]:
        per = s["coverage_by_array_role"].get(a["name"], {})
        tot = sum(sum(v.values()) for v in per.values())
        unc = sum(v["partial"] + v["none"] for v in per.values())
        print(
            "  %-12s scope %-8s fields %5d  uncovered %4d  %s"
            % (
                a["name"],
                a["scope"],
                tot,
                unc,
                " ".join(
                    "%s=%d/%d" % (r, v["covered"], sum(v.values())) for r, v in sorted(per.items())
                ),
            )
        )
    live = [u for u in s["uncovered"] if u["liveness"] == "live"]
    dead = [u for u in s["uncovered"] if u["liveness"] == "dead"]
    orph = [u for u in s["uncovered"] if u["liveness"] == "orphan"]
    print(
        "uncovered %d: live %d, dead %d, in no function %d; raw_only %d"
        % (s["uncovered_count"], len(live), len(dead), len(orph), s["raw_only_count"])
    )


# ---- the lint arm ---------------------------------------------------------------------------------------


def evaluate(summary, hits, cov, strict=False):
    """-> list of failure strings. Hermetic: committed fields vs the CURRENT manifests."""
    fails = []
    base = {(b["va"], b["array"], b["role"]): b for b in summary["baseline_v022"]}
    dead_ok = {(u["va"]) for u in summary["uncovered"] if u["liveness"] == "dead"}
    scope = {a["name"]: a["scope"] for a in summary["arrays"]}
    seen_unc = set()
    for f in hits:
        if f["role"] not in GATING:
            continue
        st = hit_status(cov, f)
        key = ("0x%08x" % f["va"], f["array"], f["role"])
        if st == "covered":
            b = base.get(key)
            if b is not None and b["state"] == "open":
                fails.append(
                    "baseline field %s (%s %s) is now COVERED but still listed open -- run --rescan to close it"
                    % (key[0], f["array"], f["role"])
                )
            continue
        seen_unc.add(key)
        if scope.get(f["array"]) not in ("v0.2.2", "PT-CAP9"):
            continue  # (every cataloged array is gated since PT-CAP9-U1 closed units and soldiers)
        if key in base:
            if base[key]["state"] == "closed":
                fails.append(
                    "REGRESSION: %s (%s %s, %s) was closed and is uncovered again"
                    % (key[0], f["array"], f["role"], f["fn"])
                )
            elif strict and base[key]["state"] == "open":
                fails.append(
                    "OPEN: %s (%s %s in %s) is a live relocation field no manifest covers"
                    % (key[0], f["array"], f["role"], f["fn"])
                )
        elif key[0] in dead_ok:
            continue
        else:
            fails.append(
                "NEW MISS: %s (%s %s in %s) is an uncovered relocation field not in the baseline"
                % (key[0], f["array"], f["role"], f["fn"])
            )
    return fails


def load_committed():
    summary = json.load(open(SUMMARY, encoding="utf-8"))
    hits = unpack_hits(json.load(open(HITS, encoding="utf-8")))
    return summary, hits


def cmd_check(strict):
    if not (os.path.isfile(SUMMARY) and os.path.isfile(HITS)):
        print("scan_cap_reloc_sites: MISSING committed data -- run --rescan")
        return 1
    summary, hits = load_committed()
    cov = Coverage(manifest_extents())
    fails = evaluate(summary, hits, cov, strict) + cmd_residual()
    openn = sum(1 for b in summary["baseline_v022"] if b["state"] == "open")
    if fails:
        for f in fails:
            print("  FAIL %s" % f)
        print("scan_cap_reloc_sites --check: FAIL (%d)" % len(fails))
        return 1
    print(
        "scan_cap_reloc_sites --check: ok (%d fields; %d known-open live gap(s) tracked for PT-CAP8 S1%s)"
        % (len(hits), openn, "; STRICT" if strict else "")
    )
    return 0


# ---- residual check (PT-CAP8 hardening) ----------------------------------------------------------------------
# --check above is hermetic (committed fields vs manifest BYTE EXTENTS). It cannot see a patch that covers the
# bytes but leaves the chain computing the OLD stride. This arm applies every stride/splice patch of the reloc
# manifest to a copy of the clean exe and re-evaluates each buildings shift-chain use with the same symbolic
# evaluator: every use must carry the NEW row stride and no OLD-only term. Needs the clean exe.
RESIDUAL_ALLOW = {
    # llm_strat_ai_notify_object_removed: the chain is compared by an equality branch (`cmp ecx,eax`) against
    # a value computed with the SAME (old) arithmetic on both sides, so the symbolic residual is not an indexing error.
    0x4DB64A: "notify_object_removed: correct by equality branch cmp ecx,eax",
}


def residual(img, patches, hits, funcs, allow=None):
    """-> (n_ok, [(va_hex, fn, terms)]) for the buildings shift-chain uses under the patched image."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_MEM

    allow = RESIDUAL_ALLOW if allow is None else allow
    old = 273 * 100
    new = 273 * 500
    raw = bytearray(img.raw)
    for p in patches:
        n = p.get("_note", "")
        if "splice" not in n and "stride" not in n:
            continue
        va = int(p["va"], 16)
        sec = next(x for x in img.secs if x["va"] <= va < x["va"] + x["raw_size"])
        off = sec["raw_off"] + va - sec["va"]
        e = bytes.fromhex(p["expect"].replace(" ", ""))
        if bytes(raw[off : off + len(e)]) != e:
            raise ValueError("residual: patch %#x expect does not match the exe" % va)
        b = bytes.fromhex(p["bytes"].replace(" ", ""))
        raw[off : off + len(b)] = b
    pimg = Image(bytes(raw))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    regions = _regions_with_gaps(img, funcs)
    starts = [r[0] for r in regions]
    bad, ok = [], 0
    for h in hits:
        if h["role"] != "shift_chain" or h["array"] != "buildings":
            continue
        entry, end, _name = regions[bisect.bisect_right(starts, h["va"]) - 1]
        ins = list(md.disasm(pimg.read(entry, end - entry + 1), entry))
        idx = next((k for k, x in enumerate(ins) if x.address == h["va"]), None)
        if idx is None:
            bad.append(("0x%x" % h["va"], h["fn"], "use no longer decodes"))
            continue
        x = ins[idx]
        op = next(o for o in x.operands if o.type == X86_OP_MEM)
        best = None
        for window, reset in CHAIN_VARIANTS:
            win = _Win()
            for y in ins[max(0, idx - window) : idx]:
                if reset and y.mnemonic in ("jmp", "ret", "retn"):
                    win = _Win()
                    continue
                step(win, y)
            f, _o = _ea(win, x, op)
            cs = [c for k, c in f.items() if k != ""]
            best = cs if best is None or any(c % new == 0 for c in cs) else best
            if any(c and c % new == 0 for c in cs):
                break
        has_old = [c for c in best if c % old == 0 and c % new != 0]
        has_new = [c for c in best if c and c % new == 0]
        if has_old or not has_new:
            if h["va"] in allow:
                continue
            bad.append(("0x%x" % h["va"], h["fn"], best))
        else:
            ok += 1
    return ok, bad


def cmd_residual(exe=None):
    exe = exe or default_exe()
    if not exe or hashlib.md5(open(exe, "rb").read()).hexdigest() != EN_MD5:
        print("  note: clean EN exe not found -- residual check SKIPPED (set MH_EN_CLEAN_EXE)")
        return []
    man = os.path.join(PATCHER, "cap_v022_reloc_EN.mh.patch.json")
    patches = json.load(open(man, encoding="utf-8"))["patches"]
    hits = unpack_hits(json.load(open(HITS, encoding="utf-8")))
    ok, bad = residual(Image(open(exe, "rb").read()), patches, hits, load_funcs())
    print("  residual: %d shift-chain use(s) carry only the new stride, %d bad" % (ok, len(bad)))
    return ["RESIDUAL old stride at %s (%s): %r" % b for b in bad]


def cmd_report():
    s, _h = load_committed()
    print_summary(s)
    for u in s["uncovered"]:
        if u["liveness"] != "dead":
            print(
                "  %s %-10s %-12s %-7s %s | %s"
                % (u["va"], u["array"], u["role"], u["liveness"], u["fn"], u["ins"])
            )
    return 0


# ---- selftest ---------------------------------------------------------------------------------------------


def selftest():
    fails = []

    def expect(what, ok, detail=None):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)
            if detail is not None:
                print("       got: %r" % (detail,))

    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM

    arrs = arrays()
    cls = Classifier(arrs)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    def dec(code):
        out = []
        decode_region(md, code, 0x500000, cls, "fn", out, X86_OP_MEM, X86_OP_IMM)
        return out

    le = lambda v: struct.pack("<I", v)  # noqa: E731
    # 1. the exact shapes the research found missed (llm_strat_bldg_link_to_network_if_adjacent)
    imul = b"\x69\xd2" + le(27300)  # imul edx, edx, 0x6aa4
    movzx = b"\x0f\xb7\x80" + le(0xC3D2A2)  # movzx eax, word ptr [eax + 0xc3d2a2]
    got = dec(imul + movzx)
    expect(
        "IMUL edx,edx,27300 is a buildings row_stride",
        any(f["role"] == "row_stride" and f["array"] == "buildings" for f in got),
        got,
    )
    expect(
        "[eax+0xc3d2a2] is a buildings disp",
        any(
            f["role"] == "disp" and f["array"] == "buildings" and f["value"] == 0xC3D2A2
            for f in got
        ),
        got,
    )
    expect(
        "...the field offsets are the dword inside the instruction",
        sorted(f["va"] for f in got) == [0x500002, 0x500009],
        [hex(f["va"]) for f in got],
    )
    # 2. other shapes: absolute operand, mov reg,imm base, push imm, cmp reg,end sentinel, block size
    expect(
        "mov [0xd03418],eax is an abs labs field",
        any(f["role"] == "abs" and f["array"] == "labs" for f in dec(b"\x89\x05" + le(0xD02E00))),
    )
    expect(
        "mov eax,imm32 base is imm_addr",
        any(
            f["role"] == "imm_addr" and f["array"] == "turrets" for f in dec(b"\xb8" + le(0xCC0FE0))
        ),
    )
    expect(
        "push imm32 inside an array is imm_addr",
        any(f["role"] == "imm_addr" for f in dec(b"\x68" + le(0xC3D2A2))),
    )
    expect(
        "mov edx,218400 is a buildings block_size",
        any(
            f["role"] == "block_size" and f["array"] == "buildings"
            for f in dec(b"\xba" + le(218400))
        ),
    )
    expect(
        "add ebx,27300 (non-IMUL, big stride) is a row_stride",
        any(f["role"] == "row_stride" for f in dec(b"\x81\xc3" + le(27300))),
    )
    expect(
        "mov eax,200 (labs row, small stride) is NOT a stride -- only IMUL counts for small ones",
        not any(f["array"] == "labs" for f in dec(b"\xb8" + le(200))),
    )
    expect(
        "imul eax,eax,200 IS a labs row_stride",
        any(f["array"] == "labs" and f["role"] == "row_stride" for f in dec(b"\x69\xc0" + le(200))),
    )
    expect("an unrelated imm32 yields nothing", dec(b"\xb8" + le(0x12345678)) == [])
    # 3. coverage is byte-extent: a patch that starts BEFORE the field and runs into it covers/partials it
    cov = Coverage([(0x500000, 0x500006, "m")])
    expect("a field fully inside a longer patch is covered", cov.test(0x500002, 4) == "covered")
    expect(
        "a field only half inside is PARTIAL (the start-address-only test missed this)",
        cov.test(0x500004, 4) == "partial",
    )
    expect("a field past the patch is none", cov.test(0x500010, 4) == "none")
    expect(
        "a long early patch is still found past a short later one",
        Coverage([(0x10, 0x100, "a"), (0x20, 0x24, "b")]).test(0x80, 4) == "covered",
    )

    # 3b. strength-reduced strides: the layout July's finder missed (llm_bldg_count_owned_matching_ids),
    # a split IMUL spelling, and an unrelated index that must NOT be flagged
    class FakeImg:
        def __init__(self, code):
            self.code = code

        def read(self, va, n):
            return self.code[va - 0x500000 : va - 0x500000 + n]

    shift = (
        b"\xc1\xe0\x03\x29\xe8\xc1\xe0\x02\x89\xc2\xc1\xe0\x04\x29\xd0\x89\xc2\xc1\xe0\x06"
        + b"\x0f\xb7\x8c\x02"
        + le(0xC3D2A0)
    )
    got = scan_stride_chains(FakeImg(shift), [(0x500000, 0x500000 + len(shift) - 1, "f")], arrs)
    expect(
        "a shifted 975*28*player chain feeding [edx+eax+0xc3d2a0] is a buildings shift_chain",
        [g["role"] for g in got] == ["shift_chain"]
        and got[0]["array"] == "buildings"
        and got[0]["value"] == 0x500000,
        got,
    )
    imulc = (
        b"\x69\xc0" + le(27300) + b"\x0f\xb7\x88" + le(0xC3D2A0)
    )  # imul eax,eax,27300 ; movzx ecx,[eax+base]
    got = scan_stride_chains(FakeImg(imulc), [(0x500000, 0x500000 + len(imulc) - 1, "f")], arrs)
    expect(
        "the IMUL spelling is an imul_chain (its field is checked separately)",
        [g["role"] for g in got] == ["imul_chain"],
        got,
    )
    plain = (
        b"\x8b\x45\xe8" + b"\x0f\xb7\x88" + le(0xC3D2A0)
    )  # mov eax,[ebp-0x18] ; movzx ecx,[eax+base]
    expect(
        "an index with no stride term (element 0 of player 0) is not flagged",
        scan_stride_chains(FakeImg(plain), [(0x500000, 0x500000 + len(plain) - 1, "f")], arrs)
        == [],
    )
    # 3c. PT-CAP8 follow-up classes: a stack spill, a stride chain in a gap between functions, a base that
    # arrives in a register. Each is a synthetic copy of a REAL missed site (see the live controls below).
    spill = (
        b"\x89\x45\xec\xc1\xe0\x03\x2b\x45\xec\xc1\xe0\x02\x89\x45\xec\xc1\xe0\x04\x2b\x45\xec"
        + b"\x89\x45\xec\xc1\xe0\x06\x03\x45\xec"
    )  # mov [ebp-14],eax; shl 3; sub eax,[ebp-14]; shl 2; mov [..],eax; shl 4; sub; mov; shl 6; add
    spill_use = b"\x0f\xb6\x88" + le(0xC3D2A0)  # movzx ecx, byte [eax+base]
    sp = spill + spill_use
    got = scan_stride_chains(FakeImg(sp), [(0x500000, 0x500000 + len(sp) - 1, "f")], arrs)
    expect(
        "a row stride built THROUGH [ebp-0x14] spills (gap site 0x4db3d7 shape) is a shift_chain",
        [g["role"] for g in got] == ["shift_chain"] and got[0]["array"] == "buildings",
        got,
    )
    # the same bytes without the reloads (every `sub/add eax,[ebp-14]` replaced by a clobbered slot) must
    # NOT be flagged -- the slot tracking is what finds the first one, not a loosened threshold.
    broken = spill.replace(b"\x2b\x45\xec", b"\x2b\x45\xe0").replace(
        b"\x03\x45\xec", b"\x03\x45\xe0"
    )
    bs = broken + spill_use
    expect(
        "the spill chain with its reloads pointing at an unrelated slot is not flagged",
        scan_stride_chains(FakeImg(bs), [(0x500000, 0x500000 + len(bs) - 1, "f")], arrs) == [],
    )
    rb = (
        spill + b"\xba" + le(0xC3D2A0) + b"\x01\xc2" + b"\x0f\xb6\x0a"
    )  # mov edx,base; add edx,eax; movzx ecx,[edx]
    got = scan_stride_chains(FakeImg(rb), [(0x500000, 0x500000 + len(rb) - 1, "f")], arrs)
    expect(
        "a stride chain feeding a register-held array base (no displacement on the use) is found",
        any(
            g["role"] == "shift_chain" and g["ins"].startswith("movzx ecx, byte ptr [edx]")
            for g in got
        ),
        got,
    )

    class GapImg(FakeImg):
        def exec_sections(self):
            return [{"va": 0x500000, "raw_size": len(self.code)}]

    gap = b"\x90" + sp
    got = scan_stride_chains(GapImg(gap), [(0x500000, 0x500000, "f")], arrs)
    expect(
        "a chain in a gap BETWEEN functions (gap site 0x4d5199 shape) is found with fn None",
        [(g["role"], g["fn"]) for g in got] == [("shift_chain", None)],
        got,
    )
    expect(
        "a splice wholly inside the chain window covers it; one outside does not",
        Coverage([(0x500004, 0x50000A, "m", "splice")]).test_chain(0x500000, 0x500020)
        and not Coverage([(0x500100, 0x500106, "m", "splice")]).test_chain(0x500000, 0x500020)
        and not Coverage([(0x500004, 0x50000A, "m", "field")]).test_chain(0x500000, 0x500020),
    )

    # 4. the committed data against the CURRENT manifests, then mutated
    if os.path.isfile(SUMMARY) and os.path.isfile(HITS):
        summary, hits = load_committed()
        real = manifest_extents()
        expect(
            "committed fields: no failure against the current manifests",
            evaluate(summary, hits, Coverage(real)) == [],
            evaluate(summary, hits, Coverage(real))[:3],
        )
        gr = [e for e in real if e[2] == "cap_v022_reloc_EN"]
        # mutation A: drop one covered buildings stride site -> must be a NEW MISS
        victim = next(
            h
            for h in hits
            if h["array"] == "buildings"
            and h["role"] == "row_stride"
            and Coverage(real).test(h["va"], 4) == "covered"
        )
        mut = [e for e in real if not (e[0] <= victim["va"] and victim["va"] + 4 <= e[1])]
        f = evaluate(summary, hits, Coverage(mut))
        expect(
            "mutation A: removing a covered buildings stride site goes RED",
            any("NEW MISS" in x and ("0x%08x" % victim["va"]) in x for x in f),
            f[:2],
        )
        # mutation B: shrink a patch by one byte so the field is only partial
        v2 = next(
            h
            for h in hits
            if h["array"] == "turrets"
            and h["role"] == "disp"
            and Coverage(real).test(h["va"], 4) == "covered"
        )
        mut2 = [
            (
                lo,
                hi - 1 if (lo <= v2["va"] and v2["va"] + 4 <= hi and hi == v2["va"] + 4) else hi,
                m,
                k,
            )
            for lo, hi, m, k in real
        ]
        f2 = evaluate(summary, hits, Coverage(mut2))
        expect(
            "mutation B: a patch one byte too short (partial overlap) goes RED",
            any(("0x%08x" % v2["va"]) in x for x in f2) or hi_is_wide(real, v2),
            f2[:2],
        )
        # mutation E: drop the splices -> every covered shift_chain use goes uncovered
        nosp = [e for e in real if e[3] != "splice"]
        f6 = evaluate(summary, hits, Coverage(nosp))
        nshift = sum(1 for h in hits if h["role"] == "shift_chain")
        expect(
            "mutation E: removing the shift-splices turns the shift_chain uses RED (%d chains)"
            % nshift,
            sum(1 for x in f6 if "shift_chain" in x) >= 100,
            len([x for x in f6 if "shift_chain" in x]),
        )
        # mutation F (PT-CAP8 follow-up positive controls): the two gap sites slice C found by replay must be
        # PRESENT in the committed census (the scan found them itself) and go RED when their splice goes.
        for gva, gfn in (
            (0x4DB404, "llm_strat_ai_bldg_register_visible_building"),
            (0x4D51C3, "llm_strat_ai_engage_prune_unseen_candidates"),
        ):
            gh = [h for h in hits if h["va"] == gva and h["role"] == "shift_chain"]
            expect(
                "positive control: the census holds the shift_chain use at %#x (fn %s)"
                % (gva, gfn),
                len(gh) == 1 and gh[0]["fn"] == gfn,
                gh,
            )
            if not gh:
                continue
            gsp = [
                e
                for e in real
                if not (e[3] == "splice" and gh[0]["value"] <= e[0] and e[1] <= gva + gh[0]["w"])
            ]
            fg = evaluate(summary, hits, Coverage(gsp))
            expect(
                "mutation F: dropping the splice for %#x goes RED" % gva,
                any("NEW MISS" in x and ("0x%08x" % gva) in x for x in fg),
                fg[:2],
            )
        # live controls: decode the REAL functions with the detector (skipped, loudly, without the clean exe)
        exe = default_exe()
        if exe and hashlib.md5(open(exe, "rb").read()).hexdigest() == EN_MD5:
            rimg = Image(open(exe, "rb").read())

            class Sub:
                def __init__(self, va, size):
                    self.v, self.n = va, size

                def read(self, va, n):
                    return rimg.read(va, n)

                def exec_sections(self):
                    return [{"va": self.v, "raw_size": self.n}]

            cases = (
                (0x4DB22F, 0x26A, [(0x4DB22F, 0x4DB498, "rvb")], 0x4DB404, "rvb"),
                (0x4D50B3, 0x200, [(0x4D50B3, 0x4D516B, "ecand")], 0x4D51C3, None),
            )
            for v0, n0, fl, want, wfn in cases:
                r = scan_stride_chains(Sub(v0, n0), fl, arrs)
                expect(
                    "live control: the detector itself finds %#x in the real binary" % want,
                    any(
                        h["va"] == want and h["role"] == "shift_chain" and h["fn"] == wfn for h in r
                    ),
                    [(hex(h["va"]), h["fn"]) for h in r][:6],
                )
            # mutation G: the residual check itself -- drop one splice and the old stride must reappear
            patches = json.load(
                open(os.path.join(PATCHER, "cap_v022_reloc_EN.mh.patch.json"), encoding="utf-8")
            )["patches"]
            funcs_ = load_funcs()
            ok0, bad0 = residual(rimg, patches, hits, funcs_)
            drop = 0x4D5199
            ok1, bad1 = residual(
                rimg, [p for p in patches if int(p["va"], 16) != drop], hits, funcs_
            )
            expect(
                "mutation G: dropping the %#x splice makes the residual check go RED" % drop,
                len(bad1) > len(bad0) and ok1 < ok0,
                (len(bad0), len(bad1)),
            )
            expect(
                "residual allow-list entries are all reasoned and only the reasoned use is exempt",
                all(RESIDUAL_ALLOW.values()) and len(bad0) == 0,
                bad0[:3],
            )
        else:
            print(
                "  note: clean EN exe not found -- live positive controls skipped (committed-data ones ran)"
            )
        # mutation C: a baseline field becoming covered but still listed open
        # With every gap closed there is no `open` entry to mutate, so the arms flip a CLOSED one back to
        # `open` in a copy of the summary: that is exactly the state a gap is in before its patch lands.
        so = json.loads(json.dumps(summary))
        ob = next((b for b in so["baseline_v022"] if b["state"] == "open"), None)
        if ob is None:
            ob = next(b for b in so["baseline_v022"] if b["state"] == "closed")
            ob["state"] = "open"
        va = int(ob["va"], 16)
        f3 = evaluate(so, hits, Coverage(real))
        expect(
            "mutation C: a gap that is patched but stays listed open goes RED (stale baseline)",
            any("now COVERED" in x for x in f3),
            f3[:2],
        )
        f4 = evaluate(so, hits, Coverage([e for e in real if not (e[0] <= va < e[1])]), strict=True)
        expect(
            "--strict turns an open live gap RED",
            any("OPEN" in x for x in f4),
            f4[:2],
        )
        # mutation D: a closed baseline entry whose coverage vanishes = REGRESSION
        sm = json.loads(json.dumps(summary))
        gone = [
            h
            for h in hits
            if h["array"] == "buildings"
            and h["role"] == "disp"
            and Coverage(real).test(h["va"], 4) == "covered"
        ][0]
        sm["baseline_v022"].append(
            {
                "va": "0x%08x" % gone["va"],
                "array": "buildings",
                "role": "disp",
                "liveness": "live",
                "state": "closed",
            }
        )
        mut3 = [e for e in real if not (e[0] <= gone["va"] and gone["va"] + 4 <= e[1])]
        f5 = evaluate(sm, hits, Coverage(mut3))
        expect(
            "mutation D: a closed gap losing its patch is a REGRESSION",
            any("REGRESSION" in x for x in f5),
            f5[:2],
        )
        known = [
            u
            for u in summary["uncovered"]
            if u["fn"] == "llm_strat_bldg_link_to_network_if_adjacent"
        ]
        closed = [
            b
            for b in summary["baseline_v022"]
            if b["state"] == "closed"
            and b["array"] == "buildings"
            and b["role"] in ("disp", "row_stride")
        ]
        expect(
            "the research's known-missed function (8 fields) is CLOSED in the baseline, not dropped",
            len(closed) >= 8 and not known,
            (len(closed), len(known)),
        )
        expect(
            "...and every uncovered live gap of a v0.2.2 array is baselined",
            all(
                any(b["va"] == u["va"] for b in summary["baseline_v022"])
                for u in summary["uncovered"]
                if u["liveness"] != "dead" and u["role"] in GATING
            ),
        )
        expect("cap_v022_reloc extents are non-empty (walker liveness)", len(gr) > 3000, len(gr))
    else:
        expect("committed data present", False)
    print("scan_cap_reloc_sites --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def hi_is_wide(real, h):
    """mutation B is vacuous when the covering patch is wider than the field (nothing to shrink)."""
    return not any(hi == h["va"] + 4 and lo <= h["va"] for lo, hi, _m in real)


def default_exe():
    cands = [os.environ.get("MH_EN_CLEAN_EXE")]
    try:
        sys.path.insert(0, HERE)
        import machine_config as mc

        cands.append(os.path.join(mc.get("POLYGON_CLEAN") if hasattr(mc, "get") else "", "mh.exe"))
    except Exception:
        pass
    cands += [
        "F:/games/mh_en_clean/mh.exe",
        os.path.join(REPO, "workdir", "mh_en_clean", "mh.exe"),
        os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(REPO))),
            "workdir",
            "mh_en_clean",
            "mh.exe",
        ),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rescan", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--exe")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.rescan:
        exe = a.exe or default_exe()
        if not exe:
            print("scan_cap_reloc_sites: no clean EN exe found; pass --exe or set MH_EN_CLEAN_EXE")
            return 1
        return rescan(exe)
    if a.report:
        return cmd_report()
    if a.check:
        return cmd_check(a.strict)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
