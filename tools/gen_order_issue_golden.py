#!/usr/bin/env python3
"""gen_order_issue_golden.py -- derive the order-issue GOLDEN from the ORIGINAL disassembly.

    python tools/gen_order_issue_golden.py            # regenerate the JSON + the C++ header
    python tools/gen_order_issue_golden.py --check     # verify the committed outputs are current

WHAT THIS ANSWERS. The order-issue migration domain (tracker item O4-0) is 112 "order wrapper"
functions in the 32-bit Watcom binary. Each one's entire job is to pack a handful of arguments and
call the order container (`llm_strat_order_dispatch` for the REPLICATED lane, `llm_strat_order_enqueue`
for the IMMEDIATE lane), optionally staging extra "hidden" arguments first via
`llm_strat_order_scratch_set_field` (index/value pairs) after an optional `llm_strat_order_scratch_reset`.
So the oracle that proves a C++ reimplementation of one of these wrappers correct has to compare WHAT
WAS EMITTED against what the ORIGINAL emits for the same inputs -- and this tool is what derives that
"what the original emits" answer, mechanically, from the original listing (never from the C++ we wrote).

METHOD. A small forward abstract interpreter walks each wrapper's exported disassembly
(`tmp/decomp_orders_issue/*.asm`) in address order, tracking an abstract VALUE (immediate / parameter /
an operator tree over a fixed, EXPLICIT vocabulary / unknown-with-a-reason) per general register
(EAX/EBX/ECX/EDX, sub-registers folded into their parent) and per EBP-relative stack slot. At every CALL
to one of the four order-container primitives it snapshots the four argument registers (or the two
scratch registers) into a site record. Anything outside the supported instruction vocabulary sets its
destination to UNKNOWN with a reason rather than guessing -- see the module-level `unsupported()` /
`handle_unknown_mnemonic()` and the "never guess" rule in the task brief. Two things make the sites more
conservative (and more honest) than a from-scratch dataflow analysis would be:

  1. BRANCHES ARE NOT ENUMERATED. The walk is a single linear pass over one execution order; whenever an
     instruction's address is the target of ANY jump found anywhere in the function, the ENTIRE
     register+stack map is invalidated to unknown ("join point") before that instruction executes, and
     only what gets explicitly re-written after the join is trusted again. This is deliberately coarser
     than a real dominance-based merge (a slot that is provably invariant across both incoming paths
     still gets invalidated) -- see docs note in `Interp.maybe_join`. `after_branch` on a site just means
     "a conditional jump was processed somewhere before this call", independent of whether the join logic
     actually tainted anything at this particular site.
  2. THE INSTRUCTION VOCABULARY IS FIXED, NOT "WHATEVER THE COMPILER IDIOM MEANS". Notably, the Watcom
     signed-divide-by-power-of-two idiom (SAR/SHL/SBB/SAR) that recurs throughout this domain is NOT
     special-cased even though its arithmetic meaning is well known: SBB is not in the supported
     vocabulary (nor in the consuming C++ header's `g_op` enum), so it resolves to UNKNOWN. Recognizing
     it would be exactly the kind of guess the brief prohibits, and the header enum has no slot for it
     anyway. This produces real, reportable unknowns in several scratch values -- see `unresolved`.

OUTPUTS.
  tools/data/order_issue_golden.json                  -- the JSON golden + the crosscheck + every
                                                          unresolved leaf, grouped by wrapper/site/field.
  src/mh_dll/mh_nettest/order_issue_golden.gen.h       -- the same data as a self-contained, constexpr
                                                          C++ table for the (separately written) shadow
                                                          evaluator to walk without touching JSON at
                                                          runtime.

CROSSCHECK. `order_matrix_raw.json` is an INDEPENDENT reading of the same call sites (Ghidra pcode +
a short backward immediate scan). For every site this tool also extracted, `crosscheck_site()` compares
param0/order_code (must match when the raw reading is a non-null int and ours resolves to a lone
constant) and the owner-byte "kind" (the raw file's OR-constant reading must line up with whether/what
this tool's `owner_and_kind` tree ORs in). A disagreement means one of the two independent readings is
wrong, so the run prints every one and exits non-zero -- see the brief's "do not skip or soften it".
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ASM_DIR = REPO / "tmp" / "decomp_orders_issue"
LEDGER = REPO / "tools" / "data" / "orders_issue_migration.json"
RAW = REPO / "tools" / "data" / "order_matrix_raw.json"
OUT_JSON = REPO / "tools" / "data" / "order_issue_golden.json"
OUT_HDR = REPO / "src" / "mh_dll" / "mh_nettest" / "order_issue_golden.gen.h"

DISPATCH = "llm_strat_order_dispatch"
ENQUEUE = "llm_strat_order_enqueue"
SCRATCH_SET = "llm_strat_order_scratch_set_field"
SCRATCH_RESET = "llm_strat_order_scratch_reset"
TRANSPARENT_CALL = "utils_assert_stack_capacity"  # the one callee the listings show preserving regs

# ---------------------------------------------------------------------------------------------
# register tables -- sub-registers fold into their EAX/EBX/ECX/EDX parent; anything else (ESI,
# EDI, ESP, EBP and their sub-forms) is a real x86 register but NOT one this domain's order/scratch
# arguments ever legitimately flow through, so it is tracked only well enough to report "read of an
# untracked register" rather than silently losing information.

REG_PARENT = {
    "EAX": "EAX", "AX": "EAX", "AL": "EAX", "AH": "EAX",
    "EBX": "EBX", "BX": "EBX", "BL": "EBX", "BH": "EBX",
    "ECX": "ECX", "CX": "ECX", "CL": "ECX", "CH": "ECX",
    "EDX": "EDX", "DX": "EDX", "DL": "EDX", "DH": "EDX",
}  # fmt: skip
REG_WIDTH = {
    "EAX": 32, "EBX": 32, "ECX": 32, "EDX": 32, "ESI": 32, "EDI": 32, "ESP": 32, "EBP": 32,
    "AX": 16, "BX": 16, "CX": 16, "DX": 16, "SI": 16, "DI": 16,
    "AL": 8, "BL": 8, "CL": 8, "DL": 8, "AH": 8, "BH": 8,
}  # fmt: skip
KNOWN_REGS = set(REG_WIDTH.keys())

# ---------------------------------------------------------------------------------------------
# VALUE constructors -- see the module docstring's schema. Never mutate a value dict after
# creation; register/stack writes always install a brand-new value, so sharing references between
# a stack slot and a register (a plain MOV) is always safe.


def v_imm(n: int) -> dict:
    return {"k": "imm", "v": n}


def v_param(i: int) -> dict:
    return {"k": "param", "i": i}


def v_op(op: str, a: dict, b: dict | None, imm: int | None) -> dict:
    return {"k": "op", "op": op, "a": a, "b": b, "imm": imm}


def v_unknown(why: str) -> dict:
    return {"k": "unknown", "why": why}


def collect_unknowns(value: dict | None):
    """Yield the `why` of every UNKNOWN leaf reachable in a value tree (there can be more than one,
    e.g. `ADD reg,reg` with both operands independently unresolved)."""
    if value is None:
        return
    if value["k"] == "unknown":
        yield value["why"]
    elif value["k"] == "op":
        yield from collect_unknowns(value.get("a"))
        yield from collect_unknowns(value.get("b"))


def owner_kind_like(value: dict | None):
    """Mirror the Ghidra-side order-matrix dump's `owner_kind()` pcode walk on OUR value tree, so the
    crosscheck compares like with like.

    The raw reader checks `vn.isConstant()` BEFORE it ever looks for an OR: a `owner_and_kind` that
    collapses to a whole compile-time constant (e.g. `XOR EDX,EDX` -> 0, no OR involved at all)
    still reads as `kind_source="or-const"` with that constant's low byte as the kind -- it is not
    limited to the `player | kind_const` shape. Strips through the same "transparent" layers raw's
    CAST/COPY/ZEXT/SEXT/SUBPIECE branch does (here: our movzx/movsx wrapper ops)."""
    if value is None:
        return None, "unknown"
    k = value["k"]
    if k == "imm":
        return value["v"] & 0xFF, "or-const"
    if k == "param":
        return None, "no-or"
    if k == "unknown":
        return None, "unknown"
    op = value["op"]
    if op in ("or", "or8"):
        if value.get("imm") is not None:
            return value["imm"] & 0xFF, "or-const"
        return None, "or-computed"
    if op in ("movzx16", "movzx8", "movsx16", "movsx8"):
        return owner_kind_like(value.get("a"))
    if op == "and":
        return None, "no-or"
    return None, "unknown"


def fmt_off(off: int) -> str:
    return "-0x%x" % (-off) if off < 0 else "0x%x" % off


# ---------------------------------------------------------------------------------------------
# instruction-line parsing
#
# Listing format (see tmp/decomp_orders_issue/*.asm):
#   0x0046dae0 55               PUSH EBP
#   0x0046db21 e8378dffff       CALL 0x0046685d                          ; llm_strat_order_scratch_set_field
# Label-only lines ("LAB_0046db50:") carry no information we need -- jump targets are read off the
# Jcc/JMP operands directly, so those lines are simply skipped (they don't start with "0x").

INSTR_RE = re.compile(r"^(0x[0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(.*)$")
HEADER_FIRST_RE = re.compile(r"^;\s*(\S+)\s+@\s+(0x[0-9a-fA-F]+)\s+\(size\s+0x[0-9a-fA-F]+\)")
PARAM_RE = re.compile(r"^;\s*param\s+(\S+)\s+(.+?)\s+storage=(\S+)\s*$")


def parse_line(line: str) -> dict | None:
    m = INSTR_RE.match(line)
    if not m:
        return None
    addr_s, rest = m.groups()
    comment = None
    code = rest
    if ";" in rest:
        code, _, comment = rest.partition(";")
        comment = comment.strip()
    code = code.strip()
    parts = code.split(None, 1)
    mnem = parts[0].upper() if parts else ""
    operand_str = parts[1].strip() if len(parts) > 1 else ""
    return {
        "addr": int(addr_s, 16),
        "addr_s": addr_s,
        "mnem": mnem,
        "operands": operand_str,
        "comment": comment,
    }


def split_operands(s: str) -> list[str]:
    s = s.strip()
    if not s:
        return []
    return [p.strip() for p in s.split(",")]


def parse_header(lines: list[str], expect_name: str, expect_addr: int) -> list[dict]:
    m = HEADER_FIRST_RE.match(lines[0])
    if not m or m.group(1) != expect_name or int(m.group(2), 16) != expect_addr:
        raise SystemExit(
            "%s: header line %r does not match ledger name/addr (%s/0x%08x)"
            % (expect_name, lines[0], expect_name, expect_addr)
        )
    params = []
    for line in lines:
        pm = PARAM_RE.match(line)
        if pm:
            params.append(
                {"name": pm.group(1), "type": pm.group(2).strip(), "storage": pm.group(3)}
            )
        elif line.startswith("0x"):
            break  # header block is over
    return params


def storage_to_parent(storage: str):
    """'EAX:4' -> ('reg','EAX'); 'DX:2' -> ('reg','EDX'); 'Stack[0x4]:4' -> ('stack', 8).

    The Stack[N] -> EBP+(N+4) mapping is measured, not assumed: llm_strat_unit_order_move's
    param_5 (storage=Stack[0x4]:4) is read back via `MOV EDX,dword ptr [EBP + 0x8]`, and
    llm_strat_bldg_footprint_random_point's param_6 (storage=Stack[0x8]:4) via `[EBP + 0xc]` --
    both +4 past the declared stack offset, which is exactly the return-address slot's width.
    """
    reg_part = storage.split(":")[0]
    if reg_part.startswith("Stack["):
        n = int(reg_part[len("Stack[") : -1], 16)
        return ("stack", n + 4)
    parent = REG_PARENT.get(reg_part.upper())
    if parent:
        return ("reg", parent)
    return (None, None)


# ---------------------------------------------------------------------------------------------
# operand classification / read / write

IMM_RE = re.compile(r"^-?0x[0-9a-fA-F]+$")
MEM_SIZE_RE = re.compile(r"^(byte|word|dword|qword|double|float)\s+ptr\s+(\[.*\])$")
EBP_OFF_RE = re.compile(r"^\[EBP\s*\+\s*(-?0x[0-9a-fA-F]+)\]$")
EBP_BARE_RE = re.compile(r"^\[EBP\]$")
LEA_REG_OFF_RE = re.compile(r"^\[\s*([A-Za-z]+)\s*\+\s*(-?0x[0-9a-fA-F]+)\s*\]$")


def classify_operand(tok: str):
    """-> (kind, value, size) where kind in {'imm','reg','mem','other'}.

    size is only meaningful for 'mem' (byte/word/dword/... or None if no size prefix was written,
    e.g. a bare LEA source)."""
    tok = tok.strip()
    if IMM_RE.match(tok):
        return ("imm", int(tok, 16), None)
    up = tok.upper()
    if up in KNOWN_REGS:
        return ("reg", up, None)
    mm = MEM_SIZE_RE.match(tok)
    if mm:
        return ("mem", mm.group(2), mm.group(1).lower())
    if tok.startswith("["):
        return ("mem", tok, None)
    return ("other", tok, None)


def ebp_offset(memexpr: str) -> int | None:
    memexpr = memexpr.strip()
    m = EBP_OFF_RE.match(memexpr)
    if m:
        return int(m.group(1), 16)
    if EBP_BARE_RE.match(memexpr):
        return 0
    return None


def mem_addr_text(memexpr: str) -> str:
    nums = re.findall(r"0x[0-9a-fA-F]+", memexpr)
    return nums[-1] if nums else memexpr


class Interp:
    def __init__(self, jump_targets: set[int], protected_stack_slots: frozenset[int] = frozenset()):
        self.regs: dict[str, dict] = {
            r: v_unknown("register never written before use") for r in ("EAX", "EBX", "ECX", "EDX")
        }
        self.stack: dict[int, dict] = {}
        self.jump_targets = jump_targets
        self.protected_stack_slots = protected_stack_slots
        self.seen_cond_jump = False
        self.sites: list[dict] = []
        self.cur_scratch: list[dict] = []
        self.cur_reset = False
        self.cur_addr: int | None = None  # set by the driver loop before each execute()
        # offset -> [addr, addr, ...] of every instruction that wrote that EBP slot, in this
        # function. Populated on every pass (cheap) by write_dest(); a preliminary pass over the
        # instructions uses it to compute `protected_stack_slots` for the real pass -- see
        # `compute_protected_slots()`.
        self.writes: dict[int, list[int]] = {}

    def maybe_join(self, addr: int) -> None:
        """Conservative merge-point handling -- see the module docstring point (1).

        Registers are ALWAYS fully invalidated at a join -- a register's value at a merge point
        genuinely can differ per incoming path, and there is no cheap way to tell. EBP stack slots
        are different: the Watcom prologue spills each incoming register parameter to its OWN slot
        exactly once, at function entry, and nothing else in these wrapper bodies ever writes it
        again -- so that slot's value is valid at every later point, joins included. A slot only
        keeps its value across this invalidation when BOTH are true: it was written exactly once in
        the whole function, and that write's address precedes every jump target recorded for this
        function (`protected_stack_slots`, computed once up front in `process_wrapper`). Any slot
        that doesn't clear that bar -- multiple writes, or a write that itself sits inside/after a
        branch -- is invalidated exactly as before. When in doubt this stays conservative: a slot
        not proven single-write-before-every-branch is never treated as protected.
        """
        if addr not in self.jump_targets:
            return
        for r in self.regs:
            self.regs[r] = v_unknown("join point")
        for k in list(self.stack.keys()):
            if k in self.protected_stack_slots:
                continue
            self.stack[k] = v_unknown("join point")


def read_value(interp: Interp, tok: str) -> dict:
    kind, val, _size = classify_operand(tok)
    if kind == "imm":
        return v_imm(val)
    if kind == "reg":
        parent = REG_PARENT.get(val)
        if parent:
            return interp.regs[parent]
        return v_unknown("read of untracked register %s" % val)
    if kind == "mem":
        off = ebp_offset(val)
        if off is not None:
            found = interp.stack.get(off)
            return (
                found
                if found is not None
                else v_unknown("stack slot %s never written" % fmt_off(off))
            )
        return v_unknown("MEM read %s" % mem_addr_text(val))
    return v_unknown("unrecognized operand '%s'" % tok)


def write_dest(interp: Interp, tok: str, value: dict) -> None:
    kind, val, _size = classify_operand(tok)
    if kind == "reg":
        parent = REG_PARENT.get(val)
        if parent:
            interp.regs[parent] = value
        return  # untracked register destination (ESI/EDI/ESP/EBP...) -- nothing to do
    if kind == "mem":
        off = ebp_offset(val)
        if off is not None:
            interp.stack[off] = value
            interp.writes.setdefault(off, []).append(interp.cur_addr)
        return  # global/non-EBP memory destination -- untracked, ignore


def unsupported(interp: Interp, mnem: str, operand_str: str, dest_tok: str | None) -> None:
    reason = ("unsupported instruction %s %s" % (mnem, operand_str)).strip()
    if dest_tok is not None:
        write_dest(interp, dest_tok, v_unknown(reason))


# ---------------------------------------------------------------------------------------------
# per-mnemonic handlers -- exactly the vocabulary the brief specifies (plus the uniform reg/imm/
# EBP-slot operand reader shared by AND/ADD/SUB/IMUL, matching how IMUL's stack-operand form is
# explicitly asked for). Everything not modeled here invalidates its destination via `unsupported`.

NO_EFFECT = {"PUSH", "POP", "CMP", "TEST", "RET", "NOP", "SAHF"}
FPU_MEM_ONLY = {
    "FLD",
    "FCOMP",
    "FADD",
    "FSTP",
}  # FPU-stack/float-memory traffic; never a tracked GPR
ARITH_OPNAME = {"AND": "and", "ADD": "add", "SUB": "sub"}
SHIFT_OPNAME = {"SHL": "shl", "SHR": "shr", "SAR": "sar"}


def handle_movx(interp: Interp, mnem: str, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 2:
        return
    dest, src = operands
    dk, dv, _ = classify_operand(dest)
    if dk != "reg" or dv not in REG_PARENT:
        return
    sk, sv, ssize = classify_operand(src)
    if sk == "reg":
        width = REG_WIDTH.get(sv)
    elif sk == "mem":
        width = {"byte": 8, "word": 16, "dword": 32}.get(ssize)
    else:
        width = None
    ext = "zx" if mnem == "MOVZX" else "sx"
    if width == 8:
        opname = "mov%s8" % ext
    elif width == 16:
        opname = "mov%s16" % ext
    else:
        unsupported(interp, mnem, ops_raw, dest)
        return
    write_dest(interp, dest, v_op(opname, read_value(interp, src), None, None))


def handle_xor(interp: Interp, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 2:
        return
    dest, src = operands
    if dest.strip().upper() == src.strip().upper():
        write_dest(interp, dest, v_imm(0))
    else:
        unsupported(interp, "XOR", ops_raw, dest)


def handle_or(interp: Interp, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 2:
        return
    dest, src = operands
    dk, dv, _ = classify_operand(dest)
    if dk != "reg" or dv not in REG_PARENT:
        unsupported(interp, "OR", ops_raw, dest)
        return
    sk, sv, _ = classify_operand(src)
    if sk != "imm":
        unsupported(interp, "OR", ops_raw, dest)
        return
    width = REG_WIDTH.get(dv, 32)
    opname = "or8" if width == 8 else "or"
    write_dest(interp, dest, v_op(opname, interp.regs[REG_PARENT[dv]], None, sv))


def handle_arith(interp: Interp, mnem: str, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 2:
        return
    dest, src = operands
    dk, dv, _ = classify_operand(dest)
    if dk != "reg" or dv not in REG_PARENT:
        unsupported(interp, mnem, ops_raw, dest)
        return
    a_val = interp.regs[REG_PARENT[dv]]
    sk, sv, _ = classify_operand(src)
    opname = ARITH_OPNAME[mnem]
    if sk == "imm":
        write_dest(interp, dest, v_op(opname, a_val, None, sv))
    else:
        write_dest(interp, dest, v_op(opname, a_val, read_value(interp, src), None))


def handle_imul(interp: Interp, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 3:
        unsupported(interp, "IMUL", ops_raw, operands[0] if operands else None)
        return
    dest, src, imm_tok = operands
    dk, dv, _ = classify_operand(dest)
    if dk != "reg" or dv not in REG_PARENT:
        unsupported(interp, "IMUL", ops_raw, dest)
        return
    ik, iv, _ = classify_operand(imm_tok)
    if ik != "imm":
        unsupported(interp, "IMUL", ops_raw, dest)
        return
    write_dest(interp, dest, v_op("imul", read_value(interp, src), None, iv))


def handle_shift(interp: Interp, mnem: str, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 2:
        return
    dest, amt = operands
    dk, dv, _ = classify_operand(dest)
    if dk != "reg" or dv not in REG_PARENT:
        unsupported(interp, mnem, ops_raw, dest)
        return
    ak, av, _ = classify_operand(amt)
    if ak != "imm":  # register-controlled shift amount (e.g. SHL AL,CL) -- not modeled
        unsupported(interp, mnem, ops_raw, dest)
        return
    write_dest(interp, dest, v_op(SHIFT_OPNAME[mnem], interp.regs[REG_PARENT[dv]], None, av))


def handle_lea(interp: Interp, operands: list[str], ops_raw: str) -> None:
    if len(operands) != 2:
        return
    dest, src = operands
    dk, dv, _ = classify_operand(dest)
    if dk != "reg" or dv not in REG_PARENT:
        return  # dest untracked (ESP, EDI, ...) -- irrelevant to order args, ignore
    src_s = src.strip()
    if EBP_OFF_RE.match(src_s) or EBP_BARE_RE.match(src_s):
        # LEA of a stack-slot ADDRESS is categorically not one of our int values -- never guess it.
        write_dest(interp, dest, v_unknown("LEA of a stack-slot address, not a value"))
        return
    m = LEA_REG_OFF_RE.match(src_s)
    if m and m.group(1).upper() in REG_PARENT:
        base_val = interp.regs[REG_PARENT[m.group(1).upper()]]
        write_dest(interp, dest, v_op("add", base_val, None, int(m.group(2), 16)))
        return
    unsupported(interp, "LEA", ops_raw, dest)


def handle_unknown_mnemonic(interp: Interp, mnem: str, operands: list[str], ops_raw: str) -> None:
    if mnem == "CDQ":  # sign-extends EAX into EDX:EAX; EAX itself is unaffected
        interp.regs["EDX"] = v_unknown("unsupported instruction CDQ")
        return
    if mnem in NO_EFFECT or mnem in FPU_MEM_ONLY:
        return
    if not operands:
        return
    unsupported(interp, mnem, ops_raw, operands[0])


def execute(interp: Interp, instr: dict) -> None:
    mnem = instr["mnem"]
    ops_raw = instr["operands"]
    operands = split_operands(ops_raw)
    if mnem in NO_EFFECT:
        return
    if mnem == "JMP":
        return
    if mnem.startswith("J"):  # any conditional jump
        interp.seen_cond_jump = True
        return
    if mnem == "CALL":
        handle_call(interp, instr)
        return
    if mnem == "MOV":
        if len(operands) == 2:
            write_dest(interp, operands[0], read_value(interp, operands[1]))
        return
    if mnem in ("MOVZX", "MOVSX"):
        handle_movx(interp, mnem, operands, ops_raw)
        return
    if mnem == "XOR":
        handle_xor(interp, operands, ops_raw)
        return
    if mnem == "OR":
        handle_or(interp, operands, ops_raw)
        return
    if mnem in ARITH_OPNAME:
        handle_arith(interp, mnem, operands, ops_raw)
        return
    if mnem == "IMUL":
        handle_imul(interp, operands, ops_raw)
        return
    if mnem in SHIFT_OPNAME:
        handle_shift(interp, mnem, operands, ops_raw)
        return
    if mnem == "LEA":
        handle_lea(interp, operands, ops_raw)
        return
    handle_unknown_mnemonic(interp, mnem, operands, ops_raw)


def handle_call(interp: Interp, instr: dict) -> None:
    callee = instr["comment"].split()[0] if instr["comment"] else instr["operands"]
    if callee == TRANSPARENT_CALL:
        return
    if callee == SCRATCH_SET:
        interp.cur_scratch.append(
            {"index": interp.regs["EAX"], "value": interp.regs["EDX"], "at": instr["addr_s"]}
        )
    elif callee == SCRATCH_RESET:
        interp.cur_reset = True
    elif callee in (DISPATCH, ENQUEUE):
        interp.sites.append(
            {
                "site": instr["addr_s"],
                "root": callee,
                "lane": "replicated" if callee == DISPATCH else "immediate",
                "after_branch": interp.seen_cond_jump,
                "unit_index": interp.regs["EAX"],
                "owner_and_kind": interp.regs["EDX"],
                "param0": interp.regs["EBX"],
                "order_code": interp.regs["ECX"],
                "scratch_reset_before": interp.cur_reset,
                "scratch": interp.cur_scratch,
            }
        )
        interp.cur_scratch = []
        interp.cur_reset = False
    # every non-transparent call clobbers all four tracked registers, including calls to the
    # order-container primitives themselves -- the listings always reload fresh values afterward.
    for r in ("EAX", "EBX", "ECX", "EDX"):
        interp.regs[r] = v_unknown("clobbered by call to %s" % callee)


def collect_jump_targets(instrs: list[dict]) -> set[int]:
    targets = set()
    for i in instrs:
        if i["mnem"] == "JMP" or (i["mnem"].startswith("J") and i["mnem"] != "JMP"):
            ops = i["operands"].strip()
            if re.fullmatch(r"0x[0-9a-fA-F]+", ops):
                targets.add(int(ops, 16))
    return targets


def compute_protected_slots(instrs: list[dict], jump_targets: set[int]) -> frozenset[int]:
    """Which EBP slots survive a join-point invalidation -- see `Interp.maybe_join`.

    Runs the SAME `execute()` the real pass uses, on a throwaway Interp, purely to observe which
    instruction writes which EBP offset -- `write_dest()` records that from the operand text alone,
    independent of the (possibly still-unseeded/unknown) values flowing through this dry run. Using
    the real dispatch here (rather than a second hand-written classifier) is deliberate: the set of
    mnemonics that write memory can only ever drift out of sync with itself.
    """
    scratch = Interp(set())
    for instr in instrs:
        scratch.cur_addr = instr["addr"]
        execute(scratch, instr)
    min_target = min(jump_targets) if jump_targets else None
    return frozenset(
        off
        for off, addrs in scratch.writes.items()
        if len(addrs) == 1 and (min_target is None or addrs[0] < min_target)
    )


def process_wrapper(name: str, addr: int, asm_path: pathlib.Path):
    lines = asm_path.read_text(encoding="utf-8").splitlines()
    params = parse_header(lines, name, addr)
    instrs = [i for i in (parse_line(l) for l in lines) if i is not None]
    jump_targets = collect_jump_targets(instrs)
    protected = compute_protected_slots(instrs, jump_targets)
    interp = Interp(jump_targets, protected)
    for idx, p in enumerate(params):
        kind, target = storage_to_parent(p["storage"])
        if kind == "reg":
            interp.regs[target] = v_param(idx)
        elif kind == "stack":
            interp.stack[target] = v_param(idx)
    for instr in instrs:
        interp.cur_addr = instr["addr"]
        interp.maybe_join(instr["addr"])
        execute(interp, instr)
    return params, interp.sites


# ---------------------------------------------------------------------------------------------
# crosscheck against order_matrix_raw.json (an independent pcode-based reading) -- see the module
# docstring. `compared`/`agreed` are counted per INDIVIDUAL CHECK (param0, order_code, kind), not
# per site, since each is only applicable when the raw reading actually offers an opinion.


def crosscheck_site(name: str, s: dict, raw_site: dict):
    compared = agreed = 0
    disagreed = []
    for field in ("param0", "order_code"):
        raw_v = raw_site.get(field)
        if raw_v is None:
            continue
        our_v = s[field]
        if our_v["k"] != "imm":
            continue  # ours doesn't resolve to a constant -- nothing to hold raw's reading against
        compared += 1
        if our_v["v"] == raw_v:
            agreed += 1
        else:
            disagreed.append(
                {
                    "wrapper": name,
                    "site": s["site"],
                    "field": field,
                    "raw": raw_v,
                    "ours": our_v["v"],
                    "detail": "raw %s=%d but our extraction resolves to imm %d"
                    % (field, raw_v, our_v["v"]),
                }
            )
    ks = raw_site.get("kind_source")
    k = raw_site.get("kind")
    if ks in ("or-const", "no-or"):
        # The bar (task brief): does raw say there's an OR-derived (or whole-constant) kind, and do
        # we agree. Raw's own "no-or" bucket already folds together several distinct pcode shapes
        # (a bare register leaf, a LOAD, a MULTIEQUAL/INDIRECT merge) that all mean the same thing
        # -- "no resolvable kind constant here" -- so the honest match on OUR side is symmetric:
        # anything that ISN'T our own "or-const" (a lone imm or an or/or8 node) counts as agreeing
        # with raw's "no-or", regardless of which reason our own read carries for it.
        compared += 1
        our_k, our_ks = owner_kind_like(s["owner_and_kind"])
        if ks == "or-const" and our_ks == "or-const" and our_k == k:
            agreed += 1
        elif ks == "no-or" and our_ks != "or-const":
            agreed += 1
        else:
            disagreed.append(
                {
                    "wrapper": name,
                    "site": s["site"],
                    "field": "kind",
                    "raw": {"kind": k, "kind_source": ks},
                    "ours": {"kind": our_k, "kind_source": our_ks},
                    "detail": "raw kind_source=%s kind=%s but our owner_and_kind reads kind_source=%s kind=%s"
                    % (ks, k, our_ks, our_k),
                }
            )
    return compared, agreed, disagreed


# ---------------------------------------------------------------------------------------------
# build


def load_ledger() -> list[dict]:
    return json.loads(LEDGER.read_text(encoding="utf-8"))["functions"]


def build() -> dict:
    functions = load_ledger()
    raw = json.loads(RAW.read_text(encoding="utf-8"))
    raw_by_name = {w["name"]: w for w in raw["wrappers"]}

    wrappers_out = []
    unresolved = []
    disagreed_all = []
    compared_total = agreed_total = 0
    c = collections.Counter()

    for f in sorted(functions, key=lambda r: r["name"]):
        name, addr_s = f["name"], f["addr"]
        dead = "todo:dead" in f.get("tags", [])
        asm_path = ASM_DIR / ("%s_%s.asm" % (name, addr_s[2:]))
        if not asm_path.exists():
            raise SystemExit("missing asm export for %s: %s" % (name, asm_path))
        params, sites = process_wrapper(name, int(addr_s, 16), asm_path)
        wrappers_out.append(
            {
                "name": name,
                "addr": addr_s,
                "batch": f.get("batch"),
                "dead": dead,
                "params": params,
                "sites": sites,
            }
        )

        c["wrappers"] += 1
        c["dead_wrappers"] += 1 if dead else 0
        raw_w = raw_by_name.get(name)

        for s in sites:
            c["sites"] += 1
            c["guarded_sites"] += 1 if s["after_branch"] else 0
            site_unknown = False
            for field in ("unit_index", "owner_and_kind", "param0", "order_code"):
                for why in collect_unknowns(s[field]):
                    unresolved.append(
                        {"wrapper": name, "site": s["site"], "field": field, "why": why}
                    )
                    site_unknown = True
            c["sites_fully_resolved" if not site_unknown else "sites_with_unknown"] += 1

            for k, sc in enumerate(s["scratch"]):
                c["scratch_writes"] += 1
                sc_unknown = False
                for subfield in ("index", "value"):
                    for why in collect_unknowns(sc[subfield]):
                        unresolved.append(
                            {
                                "wrapper": name,
                                "site": s["site"],
                                "field": "scratch[%d].%s" % (k, subfield),
                                "why": why,
                            }
                        )
                        sc_unknown = True
                c["scratch_writes_unknown"] += 1 if sc_unknown else 0

            if raw_w is not None:
                raw_site = next((rs for rs in raw_w["sites"] if rs["site"] == s["site"]), None)
                if raw_site is not None:
                    cmp_, agr_, dis_ = crosscheck_site(name, s, raw_site)
                    compared_total += cmp_
                    agreed_total += agr_
                    disagreed_all.extend(dis_)

    return {
        "_generated_by": "tools/gen_order_issue_golden.py",
        "_do_not_hand_edit": "Edit the generator; inputs are tmp/decomp_orders_issue/*.asm, "
        "tools/data/orders_issue_migration.json and tools/data/order_matrix_raw.json.",
        "_what": (
            "The order-issue GOLDEN: for each of the 112 order-wrapper functions in "
            "tools/data/orders_issue_migration.json, the exact register values (unit_index, "
            "owner_and_kind, param0, order_code) and scratch-field writes each call site into "
            "llm_strat_order_dispatch/llm_strat_order_enqueue/llm_strat_order_scratch_set_field "
            "emits, derived by a small forward abstract interpreter walking the function's ORIGINAL "
            "disassembly (tmp/decomp_orders_issue/*.asm) -- never from any C++ reimplementation. "
            "Values that cannot be pinned to a constant/parameter through the supported instruction "
            "vocabulary are UNKNOWN with a reason rather than guessed; every one is listed in "
            "`unresolved`. `crosscheck` compares param0/order_code/kind against an independent "
            "pcode-based reading (order_matrix_raw.json) and the run fails if they disagree."
        ),
        "counts": {
            "wrappers": c["wrappers"],
            "sites": c["sites"],
            "sites_fully_resolved": c["sites_fully_resolved"],
            "sites_with_unknown": c["sites_with_unknown"],
            "scratch_writes": c["scratch_writes"],
            "scratch_writes_unknown": c["scratch_writes_unknown"],
            "guarded_sites": c["guarded_sites"],
            "dead_wrappers": c["dead_wrappers"],
        },
        "crosscheck": {
            "compared": compared_total,
            "agreed": agreed_total,
            "disagreed": disagreed_all,
        },
        "unresolved": unresolved,
        "wrappers": wrappers_out,
    }


# ---------------------------------------------------------------------------------------------
# C++ header emission -- exact shape pinned by the task brief (the consuming evaluator is written
# separately against these names). Each g_val gets its own small flattened node array (post-order,
# last node is the result); node arrays and g_scratch arrays are pooled with generated names.

OP_ENUM = {
    "or": "G_OR",
    "or8": "G_OR8",
    "and": "G_AND",
    "add": "G_ADD",
    "sub": "G_SUB",
    "imul": "G_IMUL",
    "shl": "G_SHL",
    "shr": "G_SHR",
    "sar": "G_SAR",
    "movzx16": "G_MOVZX16",
    "movzx8": "G_MOVZX8",
    "movsx16": "G_MOVSX16",
    "movsx8": "G_MOVSX8",
}

HEADER_PREAMBLE = '''\
// GENERATED by tools/gen_order_issue_golden.py -- DO NOT HAND-EDIT.
#pragma once
#include <cstdint>
namespace mh::orders::issue::golden {

enum g_op : uint8_t {
    G_IMM = 0, G_PARAM, G_OR, G_OR8, G_AND, G_ADD, G_SUB, G_IMUL,
    G_SHL, G_SHR, G_SAR, G_MOVZX16, G_MOVZX8, G_MOVSX16, G_MOVSX8, G_UNKNOWN
};

// A value is a flat node array; the LAST node is the result. `a`/`b` are indices into the
// same array, or -1. For G_IMM the constant is in `imm`; for G_PARAM the parameter index is
// in `imm`; for binary ops with a constant operand, `b` is -1 and the constant is in `imm`.
struct g_node { uint8_t op; int8_t a; int8_t b; int32_t imm; };
struct g_val  { const g_node *nodes; uint8_t n; };          // n == 0 means "not present"
struct g_scratch { g_val index; g_val value; uint32_t at; };
struct g_site {
    const char *wrapper;  uint32_t wrapper_va;  uint32_t site_va;
    uint8_t lane;                      // 0 = replicated (dispatch), 1 = immediate (enqueue)
    bool after_branch;  bool scratch_reset_before;  bool dead;
    g_val unit_index, owner_and_kind, param0, order_code;
    const g_scratch *scratch;  uint8_t scratch_n;
};

'''

HEADER_TAIL = '''\

inline bool g_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// Look one up by wrapper name + site VA. Returns nullptr if absent.
inline const g_site *find(const char *wrapper, uint32_t site_va) {
    for (int i = 0; i < SITE_COUNT; ++i) {
        if (SITES[i].site_va == site_va && g_streq(SITES[i].wrapper, wrapper)) return &SITES[i];
    }
    return nullptr;
}

// All sites for one wrapper, in program order. Returns the count; fills `out` up to `max`.
inline int find_all(const char *wrapper, const g_site **out, int max) {
    int n = 0;
    for (int i = 0; i < SITE_COUNT; ++i) {
        if (g_streq(SITES[i].wrapper, wrapper)) {
            if (n < max) out[n] = &SITES[i];
            ++n;
        }
    }
    return n;
}

}  // namespace mh::orders::issue::golden
'''


class HeaderBuilder:
    def __init__(self):
        self.node_arrays: list[tuple[str, list[tuple[str, int, int, int]]]] = []
        self.scratch_arrays: list[tuple[str, list[str]]] = []
        self._n = 0

    def _new_name(self, prefix: str) -> str:
        self._n += 1
        return "%s%d" % (prefix, self._n)

    def flatten(self, value: dict) -> list[tuple[str, int, int, int]]:
        nodes: list[tuple[str, int, int, int]] = []

        def visit(v: dict) -> int:
            if v["k"] == "imm":
                nodes.append(("G_IMM", -1, -1, v["v"]))
            elif v["k"] == "param":
                nodes.append(("G_PARAM", -1, -1, v["i"]))
            elif v["k"] == "unknown":
                nodes.append(("G_UNKNOWN", -1, -1, 0))
            elif v["k"] == "op":
                a_idx = visit(v["a"]) if v.get("a") is not None else -1
                b_idx = visit(v["b"]) if v.get("b") is not None else -1
                nodes.append((OP_ENUM[v["op"]], a_idx, b_idx, v.get("imm") or 0))
            else:
                raise SystemExit("bad value kind %r" % v["k"])
            return len(nodes) - 1

        visit(value)
        if len(nodes) > 128:  # a/b are int8_t indices into this same array (0..127, or -1)
            raise SystemExit("value tree too deep for int8_t node index (>128 nodes)")
        return nodes

    def emit_value(self, value: dict) -> str:
        nodes = self.flatten(value)
        name = self._new_name("N")
        self.node_arrays.append((name, nodes))
        return "{%s, %d}" % (name, len(nodes))

    def emit_scratch(self, scratch_list: list[dict]):
        if not scratch_list:
            return "nullptr", 0
        entries = []
        for sc in scratch_list:
            idx_code = self.emit_value(sc["index"])
            val_code = self.emit_value(sc["value"])
            entries.append("{%s, %s, %du}" % (idx_code, val_code, int(sc["at"], 16)))
        name = self._new_name("SC")
        self.scratch_arrays.append((name, entries))
        return name, len(entries)

    def emit_site(self, wrapper_name: str, wrapper_va: int, dead: bool, s: dict) -> str:
        uidx = self.emit_value(s["unit_index"])
        okind = self.emit_value(s["owner_and_kind"])
        p0 = self.emit_value(s["param0"])
        ocode = self.emit_value(s["order_code"])
        scratch_name, scratch_n = self.emit_scratch(s["scratch"])
        lane = 0 if s["lane"] == "replicated" else 1
        return "{%s, %du, %du, %d, %s, %s, %s, %s, %s, %s, %s, %s, %d}" % (
            json.dumps(wrapper_name),
            wrapper_va,
            int(s["site"], 16),
            lane,
            _cbool(s["after_branch"]),
            _cbool(s["scratch_reset_before"]),
            _cbool(dead),
            uidx,
            okind,
            p0,
            ocode,
            scratch_name,
            scratch_n,
        )


def _cbool(b: bool) -> str:
    return "true" if b else "false"


def format_nodes(nodes: list[tuple[str, int, int, int]]) -> str:
    return ", ".join("{%s, %d, %d, %d}" % n for n in nodes)


def build_header(doc: dict) -> str:
    hb = HeaderBuilder()
    all_sites = []
    for w in doc["wrappers"]:
        wva = int(w["addr"], 16)
        for s in w["sites"]:
            all_sites.append((w["name"], wva, w["dead"], s))
    all_sites.sort(key=lambda t: (t[0], int(t[3]["site"], 16)))

    site_codes = [hb.emit_site(name, wva, dead, s) for name, wva, dead, s in all_sites]

    out = [HEADER_PREAMBLE]
    for name, nodes in hb.node_arrays:
        out.append("static constexpr g_node %s[] = {%s};\n" % (name, format_nodes(nodes)))
    for name, entries in hb.scratch_arrays:
        out.append("static constexpr g_scratch %s[] = {%s};\n" % (name, ", ".join(entries)))
    out.append("inline constexpr g_site SITES[] = {\n")
    for code in site_codes:
        out.append("    %s,\n" % code)
    out.append("};\n")
    out.append("inline constexpr int SITE_COUNT = %d;\n" % len(site_codes))
    out.append(HEADER_TAIL)
    return "".join(out)


# ---------------------------------------------------------------------------------------------
# entry point


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--check", action="store_true", help="fail if the committed outputs are stale")
    ns = ap.parse_args(argv)

    doc = build()
    json_text = json.dumps(doc, indent=1) + "\n"
    hdr_text = build_header(doc)

    disagreed = doc["crosscheck"]["disagreed"]
    for d in disagreed:
        print("CROSSCHECK DISAGREEMENT: %s" % json.dumps(d), file=sys.stderr)

    if ns.check:
        stale = []
        for path, want in ((OUT_JSON, json_text), (OUT_HDR, hdr_text)):
            if not path.exists() or path.read_text(encoding="utf-8") != want:
                stale.append(path.relative_to(REPO).as_posix())
        if stale:
            print(
                "STALE (re-run tools/gen_order_issue_golden.py): %s" % ", ".join(stale),
                file=sys.stderr,
            )
            return 1
        if disagreed:
            print("%d crosscheck disagreement(s)" % len(disagreed), file=sys.stderr)
            return 1
        print("order issue golden outputs current, crosscheck clean")
        return 0

    OUT_JSON.write_text(json_text, encoding="utf-8")
    OUT_HDR.write_text(hdr_text, encoding="utf-8")
    c = doc["counts"]
    print(
        "wrappers=%d sites=%d resolved=%d unknown=%d scratch=%d scratch_unknown=%d guarded=%d dead=%d"
        % (
            c["wrappers"],
            c["sites"],
            c["sites_fully_resolved"],
            c["sites_with_unknown"],
            c["scratch_writes"],
            c["scratch_writes_unknown"],
            c["guarded_sites"],
            c["dead_wrappers"],
        )
    )
    cc = doc["crosscheck"]
    print(
        "crosscheck: compared=%d agreed=%d disagreed=%d"
        % (cc["compared"], cc["agreed"], len(disagreed))
    )
    print("wrote %s and %s" % (OUT_JSON.relative_to(REPO), OUT_HDR.relative_to(REPO)))
    return 1 if disagreed else 0


if __name__ == "__main__":
    raise SystemExit(main())
