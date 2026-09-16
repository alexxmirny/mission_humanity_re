#!/usr/bin/env python3
"""gen_state_handler_table.py -- extract the two STRATEGIC STATE-MACHINE dispatch tables from the
disassembly, and emit them as the C++ table our own registrar fills.

    1) run-script {programPath: "/eng/mh.exe", scriptName: "mh_export_decomp.py"} with a request
       naming llm_strat_register_state_handlers + its two setters and "disasm": true
    2) python tools/gen_state_handler_table.py --extract --asm-dir tmp/dispatch_asm
    3) python tools/gen_state_handler_table.py            # write the generated header
       python tools/gen_state_handler_table.py --check    # drift gate (lint_repo.py runs this)

SECOND MODE, for the OTHER fn-ptr registry (SIM1-BLDGCB):

    python tools/gen_state_handler_table.py --extract-bldg-type --asm-dir tmp/dispatch_asm

which extracts llm_strat_register_bldg_type_callbacks' 30 callbacks into
tools/data/sim_bldg_type_callback_table.json (for gen_migration.py to seed the sim closure with,
batch H) AND emits src/mh_dll/mh/addr/mh_bldg_type_callbacks.gen.h -- the (building TYPE -> callback)
pairing our own registrar walks. `--check` gates BOTH headers.

WHAT CHANGED 2026-08-23 (SIM1-BLDGCB's closing slice). The first pass extracted only the SET of
callbacks, because the (building ID -> callback) map genuinely is not knowable from the listing: the
setters loop over building IDs and match on cfg data loaded at runtime. That is still true and is
why no header claims an ID map. But the ID map is not the pairing our registrar needs -- the setters
are keyed by building TYPE, and THAT argument is a literal immediate at all 50 explicit call sites.
So the pairing that is extractable, and the one a reimplementation must reproduce exactly, is
(type -> callback) plus the same runtime `Building[id].type == type` scan. Both are emitted here.

WHY THIS IS EXTRACTED AND NOT TRANSCRIBED. `llm_strat_register_state_handlers` (0x0045f26a, boot
stage 4, from llm_strat_mode_init) is 78 consecutive `set_<unit|bldg>_state_handler(state, handler)`
calls plus two default-fill loops. That call list IS the dispatch layer: it decides which of the 68
state handlers every unit and every building runs each tick. SIM1-DISPATCH replaces that registrar
with our own C++ one filling the same two tables with OUR handler pointers -- so the (state ->
handler) pairing has to be reproduced exactly, and a hand-transcribed pairing is 78 places where a
typo silently gives one unit state another state's behaviour. Nothing would fail to compile and no
oracle would notice: the wrong handler is still a handler.

So the pairing is read out of the binary here, committed as data, and the C++ registrar walks it.

WHAT THE EXTRACTION KNOWS AND WHAT IT ASSERTS. Both setters are verified to be the trivial bounded
store this generator assumes -- `if (0 <= id && id < 0xff) table[id] = handler` -- by reading their
own listings for the table base and the bound rather than trusting the name. A setter that grew a
side effect would change that shape and fail here instead of being silently modelled away.

THE DEFAULT FILL IS PART OF THE TABLE. Each table is first filled 0..0xfe with a default handler
(unit: llm_strat_unit_state_default_noop, bldg: llm_strat_bldg_state_default_reset) and only then
overwritten at the assigned states. An unassigned state is therefore NOT "no handler" -- it is a real
call into the default. Dropping the fill would leave 200-odd slots holding whatever was in .bss.

THE X-MACRO IS THE EXHAUSTIVENESS GATE. The header emits MH_STATE_HANDLERS(X) over the DISTINCT
handler set, so the registrar TU binds each original name to its C++ reimplementation through it: a
handler that appears in the binary and has no binding is a COMPILE error, not a silent omission.
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(REPO, "tools", "data", "sim_state_handler_table.json")
OUT = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_state_handlers.gen.h")

REGISTRAR = "llm_strat_register_state_handlers"
SETTERS = {
    "unit": "llm_strat_set_unit_state_handler",
    "bldg": "llm_strat_set_bldg_state_handler",
}

# ---- the SECOND registry (SIM1-BLDGCB, 2026-08-22) ------------------------------------------------
#
# `llm_strat_register_bldg_type_callbacks` @0x0045f76a (boot stage 5, re-run on game load) fills two
# PER-BUILDING-TYPE tables the same way this file's first registry fills the per-STATE ones, and it
# was the second dispatch blind spot: none of its 30 callbacks was in the sim closure, because they
# are reached only through a computed `[TICK2_FUNCS + building_id*4]` call two indirections down
# from the sim root -- a check written to find a blind spot that had its own blind
# spot.
#
# WHY THIS ONE IS EXTRACTED AS A SET AND NOT AS A TABLE, which is a real difference and not laziness.
# The state setters are a flat bounded store: `if (0 <= id && id < 0xff) table[id] = handler`, so the
# (state -> handler) pairing is fully determined by the call arguments and can be reproduced exactly.
# THESE setters are not: each one LOOPS over building ids 1..0x63 and writes the callback into every
# slot whose `Building[id].type` matches the type argument (`llm_bldg_register_done_callback`
# @0x0045f1ae, store at 0x0045f1fb). The resulting (building_id -> callback) map therefore depends on
# cfg data loaded at runtime, and is NOT knowable from the listing alone.
#
# SO THE UNIT OF EXTRACTION HERE IS THE TYPE, NOT THE ID. The `type` argument IS a literal immediate
# at every explicit call site (EAX, exactly as EDX carries the callback), so (type -> callback) is
# fully determined by the listing and is reproduced here with the same fidelity the state tables get.
# What is NOT extracted -- and what no consumer may assume -- is (building_id -> callback): that map
# is the cfg scan `Building[id].type == type` performed at RUNTIME, so our registrar reproduces the
# SCAN rather than a table of ids. The scan's own three constants (the id-loop bound, the cfg record
# stride and the address the type byte is read from) are read out of the setters' listings too, and
# the emitted header hands them to the C++ as static_assert fodder against the region registry and
# the struct layout -- so a cfg base or a struct offset that drifts is a compile error rather than a
# registrar quietly matching on the wrong byte.
CB_DATA = os.path.join(REPO, "tools", "data", "sim_bldg_type_callback_table.json")
CB_OUT = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_bldg_type_callbacks.gen.h")
CB_REGISTRAR = "llm_strat_register_bldg_type_callbacks"
CB_SETTERS = {
    "done": "llm_bldg_register_done_callback",  # -> _G_LLM_STRAT_BLDG_DONE_FUNCS[100]
    "tick2": "llm_bldg_register_tick2_callback",  # -> _G_LLM_STRAT_BLDG_TICK2_FUNCS[100]
}

# "0x0045f299 baace24700       MOV EDX,0x47e2ac                              ; llm_strat_unit_..."
LINE = re.compile(r"^0x([0-9a-f]{8})\s+([0-9a-f]*)\s+(\S.*?)(?:\s{2,};\s*(.*))?$")


def read_asm(asm_dir, fn):
    for name in sorted(os.listdir(asm_dir)):
        if name.startswith(fn + "_") and name.endswith(".asm"):
            with open(os.path.join(asm_dir, name), encoding="utf-8", errors="replace") as fh:
                return [
                    (m.group(1), m.group(3).strip(), (m.group(4) or "").strip())
                    for m in (LINE.match(ln.rstrip()) for ln in fh)
                    if m
                ]
    sys.exit(
        "no %s_*.asm in %s -- export it first (mh_export_decomp.py, disasm: true)" % (fn, asm_dir)
    )


def parse_setter(asm_dir, fn):
    """Return (base, slots) after proving the setter is the bounded store we model."""
    ins = read_asm(asm_dir, fn)
    base = slots = None
    for _, text, _ in ins:
        m = re.match(r"^MOV dword ptr \[E[A-Z]{2} \+ 0x([0-9a-f]+)\],E[A-Z]{2}$", text)
        if m:
            if base is not None:
                sys.exit("%s: more than one table store -- not the setter shape modelled" % fn)
            base = int(m.group(1), 16)
        m = re.match(r"^CMP dword ptr \[EBP \+ -0x[0-9a-f]+\],0x([0-9a-f]+)$", text)
        if m and slots is None:
            slots = int(m.group(1), 16)
    if base is None or slots is None:
        sys.exit("%s: could not read the table base / bound out of the listing" % fn)
    return base, slots


def extract(asm_dir):
    tables = {}
    for key, setter in SETTERS.items():
        base, slots = parse_setter(asm_dir, setter)
        tables[key] = {
            "setter": setter,
            "base": "0x%08x" % base,
            "slots": slots,
            "default": None,
            "assign": [],
        }

    by_setter_addr = {}
    for key, setter in SETTERS.items():
        ins = read_asm(asm_dir, setter)
        by_setter_addr["0x" + ins[0][0]] = key

    eax = edx = edx_sym = None
    for addr, text, sym in read_asm(asm_dir, REGISTRAR):
        m = re.match(r"^MOV EAX,0x([0-9a-f]+)$", text)
        if m:
            eax = int(m.group(1), 16)
            continue
        m = re.match(r"^MOV EDX,0x([0-9a-f]+)$", text)
        if m:
            edx, edx_sym = int(m.group(1), 16), sym
            continue
        m = re.match(r"^CALL (0x[0-9a-f]+)$", text)
        if m:
            key = by_setter_addr.get(m.group(1))
            if key is not None:
                if edx is None:
                    sys.exit("%s: setter call with no handler immediate in EDX" % addr)
                entry = {"name": edx_sym, "addr": "0x%08x" % edx, "at": "0x" + addr}
                if eax is None:
                    # the default-fill loop: EAX is the loop counter, not an immediate
                    if tables[key]["default"] is not None:
                        sys.exit("%s: a second default fill for the %s table" % (addr, key))
                    tables[key]["default"] = entry
                else:
                    tables[key]["assign"].append(dict(entry, state=eax))
            eax = edx = edx_sym = None
            continue
        # any other write to EAX/EDX invalidates the tracked immediate
        if re.match(r"^\w+ EAX\b", text):
            eax = None
        if re.match(r"^\w+ EDX\b", text):
            edx = edx_sym = None

    for key, t in tables.items():
        if t["default"] is None:
            sys.exit("%s table: no default fill found" % key)
        if not t["assign"]:
            sys.exit("%s table: no explicit assignments found" % key)
        for e in t["assign"] + [t["default"]]:
            if not e["name"] or e["name"].startswith("FUN_") or " " in e["name"]:
                sys.exit(
                    "%s table: handler at %s has no resolved symbol (%r)"
                    % (key, e["addr"], e["name"])
                )
        seen = {}
        for e in t["assign"]:
            if e["state"] >= t["slots"]:
                sys.exit(
                    "%s table: state 0x%x is outside the setter bound 0x%x"
                    % (key, e["state"], t["slots"])
                )
            if e["state"] in seen:
                sys.exit("%s table: state 0x%x assigned twice" % (key, e["state"]))
            seen[e["state"]] = e["name"]

    return {
        "_comment": (
            "EXTRACTED by tools/gen_state_handler_table.py from the exported disassembly of "
            + REGISTRAR
            + " -- do not hand-edit. Each table is filled 0..slots-1 with `default` and then "
            "overwritten at each `assign` state, in the order listed. `at` is the setter call site."
        ),
        "registrar": REGISTRAR,
        "tables": tables,
    }


def parse_cb_setter(asm_dir, fn):
    """Prove `fn` is the per-type SCAN this generator models, and return its five constants.

    The shape asserted, from the listing and not from the name:

        for (id = FIRST_ID; id < SLOTS; ++id)
            if (*(uint8_t *)(CFG_SCAN_BASE + id * CFG_STRIDE) == type) table[id] = f;

    A setter that grew a second store, a different bound, or a second cfg read would fail one of the
    exactly-one checks below rather than being silently modelled away -- the same posture
    parse_setter takes for the state setters.
    """
    ins = read_asm(asm_dir, fn)

    stores = [
        int(m.group(1), 16)
        for _, text, _ in ins
        for m in [re.match(r"^MOV dword ptr \[E[A-Z]{2} \+ 0x([0-9a-f]+)\],E[A-Z]{2}$", text)]
        if m
    ]
    if len(stores) != 1:
        sys.exit("%s: expected exactly one table store, found %d" % (fn, len(stores)))

    # The loop variable is the frame slot the bound is compared against, so read the CMP first and
    # then take the init naming that same slot -- the setter has three frame slots and only one of
    # them is the counter.
    bounds = [
        (m.group(1), int(m.group(2), 16))
        for _, text, _ in ins
        for m in [re.match(r"^CMP dword ptr \[EBP \+ -0x([0-9a-f]+)\],0x([0-9a-f]+)$", text)]
        if m
    ]
    if len(bounds) != 1:
        sys.exit("%s: expected exactly one loop bound, found %d" % (fn, len(bounds)))
    slot, slots = bounds[0]

    inits = [
        int(m.group(1), 16)
        for _, text, _ in ins
        for m in [
            re.match(r"^MOV dword ptr \[EBP \+ -0x%s\],0x([0-9a-f]+)$" % re.escape(slot), text)
        ]
        if m
    ]
    if len(inits) != 1:
        sys.exit("%s: expected exactly one init of the loop slot, found %d" % (fn, len(inits)))

    strides = [
        int(m.group(1), 16)
        for _, text, _ in ins
        for m in [
            re.match(
                r"^IMUL E[A-Z]{2},dword ptr \[EBP \+ -0x%s\],0x([0-9a-f]+)$" % re.escape(slot), text
            )
        ]
        if m
    ]
    if len(strides) != 1:
        sys.exit(
            "%s: expected exactly one cfg-record stride multiply, found %d" % (fn, len(strides))
        )

    scans = [
        int(m.group(1), 16)
        for _, text, _ in ins
        for m in [re.match(r"^MOVZX E[A-Z]{2},byte ptr \[E[A-Z]{2} \+ 0x([0-9a-f]+)\]$", text)]
        if m
    ]
    if len(scans) != 1:
        sys.exit("%s: expected exactly one cfg type-byte read, found %d" % (fn, len(scans)))

    return {
        "setter": fn,
        "base": "0x%08x" % stores[0],
        "slots": slots,
        "first_id": inits[0],
        "cfg_stride": strides[0],
        "cfg_scan_base": "0x%08x" % scans[0],
    }


def extract_bldg_type_callbacks(asm_dir):
    """The (building TYPE -> callback) pairing llm_strat_register_bldg_type_callbacks installs into
    the two per-building tables -- see CB_REGISTRAR's comment for what is and is not knowable here.

    Same argument convention as the state registrar (EDX = the callback, EAX = the type), which is
    checked rather than assumed: every EDX immediate reaching a setter call has to resolve to a
    function symbol in the listing's comment column, and every setter constant is read out of the
    setter's own listing, so a setter pointed at a different array, bound or cfg field is noticed
    here.
    """
    tables, setter_addr = {}, {}
    for key, setter in CB_SETTERS.items():
        ins = read_asm(asm_dir, setter)
        setter_addr["0x" + ins[0][0]] = key
        t = parse_cb_setter(asm_dir, setter)
        t.update({"default": None, "assign": []})
        tables[key] = t

    # The two setters perform the SAME cfg scan; if they ever disagreed, one of them reads a
    # different array and the single set of scan constants the header emits would be a fiction.
    scan = {
        (t["cfg_scan_base"], t["cfg_stride"], t["slots"], t["first_id"]) for t in tables.values()
    }
    if len(scan) != 1:
        sys.exit("%s: the two setters do not perform the same cfg scan: %r" % (CB_REGISTRAR, scan))

    eax = edx = edx_sym = eax_slot = None
    init, bound = {}, {}
    calls = []
    for addr, text, sym in read_asm(asm_dir, CB_REGISTRAR):
        m = re.match(r"^MOV dword ptr \[EBP \+ -0x([0-9a-f]+)\],0x([0-9a-f]+)$", text)
        if m:
            init[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.match(r"^CMP dword ptr \[EBP \+ -0x([0-9a-f]+)\],0x([0-9a-f]+)$", text)
        if m:
            bound[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.match(r"^MOV EAX,0x([0-9a-f]+)$", text)
        if m:
            eax, eax_slot = int(m.group(1), 16), None
            continue
        m = re.match(r"^MOV EAX,dword ptr \[EBP \+ -0x([0-9a-f]+)\]$", text)
        if m:
            # the default-fill loop's counter reaching EAX: the TYPE is a range, not an immediate
            eax, eax_slot = None, m.group(1)
            continue
        m = re.match(r"^MOV EDX,0x([0-9a-f]+)$", text)
        if m:
            edx, edx_sym = int(m.group(1), 16), sym
            continue
        m = re.match(r"^CALL (0x[0-9a-f]+)$", text)
        if m:
            key = setter_addr.get(m.group(1))
            if key is not None:
                if edx is None:
                    sys.exit("%s: setter call with no callback immediate in EDX" % addr)
                calls.append((key, edx, edx_sym, eax, eax_slot, addr))
            eax = edx = edx_sym = eax_slot = None
            continue
        if re.match(r"^\w+ EAX\b", text):
            eax = eax_slot = None
        if re.match(r"^\w+ EDX\b", text):
            edx = edx_sym = None

    if not calls:
        sys.exit("%s: no setter calls found" % CB_REGISTRAR)

    by_name, order, unnamed = {}, [], []
    for key, va, sym, type_id, type_slot, at in calls:
        if not sym or " " in sym:
            sys.exit("%s: callback at 0x%08x has no resolved symbol (%r)" % (at, va, sym))
        if sym not in by_name:
            # An UNNAMED callback is recorded, not refused: FUN_00476605 was one of the 30 and
            # SIM1-BLDGCB's done_when requires it be RE'd. Refusing here would only hide it.
            if sym.startswith("FUN_"):
                unnamed.append(sym)
            by_name[sym] = {"name": sym, "addr": "0x%08x" % va, "via": [], "first_at": "0x" + at}
            order.append(sym)
        if key not in by_name[sym]["via"]:
            by_name[sym]["via"].append(key)

        entry = {"name": sym, "addr": "0x%08x" % va, "at": "0x" + at}
        t = tables[key]
        if type_id is None:
            # the default fill: EAX is the loop counter, so the "type" is the counter's whole range
            if type_slot is None or type_slot not in init or type_slot not in bound:
                sys.exit("%s: default-fill call whose type loop could not be read" % at)
            if t["default"] is not None:
                sys.exit("%s: a second default fill for the %s table" % (at, key))
            t["default"] = dict(entry, type_lo=init[type_slot], type_hi=bound[type_slot] - 1)
        else:
            t["assign"].append(dict(entry, type=type_id))

    for key, t in tables.items():
        if t["default"] is None:
            sys.exit("%s table: no default fill found" % key)
        if not t["assign"]:
            sys.exit("%s table: no explicit assignments found" % key)
        seen = {}
        for e in t["assign"]:
            if e["type"] in seen:
                # Not cosmetic: two callbacks claiming one type means the LAST write wins, so a
                # registrar reproducing the pairing as an unordered map would silently differ from
                # the original. Refuse until someone has read the listing again.
                sys.exit(
                    "%s table: building type 0x%x assigned twice (%s then %s)"
                    % (key, e["type"], seen[e["type"]], e["name"])
                )
            seen[e["type"]] = e["name"]
            if not (t["default"]["type_lo"] <= e["type"] <= t["default"]["type_hi"]):
                sys.exit(
                    "%s table: building type 0x%x is outside the default fill range 0x%x..0x%x -- "
                    "that type would carry no default underneath the assignment"
                    % (key, e["type"], t["default"]["type_lo"], t["default"]["type_hi"])
                )

    return {
        "_comment": (
            "EXTRACTED by tools/gen_state_handler_table.py --extract-bldg-type from the exported "
            "disassembly of " + CB_REGISTRAR + " -- do not hand-edit. `tables` is the (building "
            "TYPE -> callback) pairing: each table is filled, for every type in "
            "default.type_lo..default.type_hi, with `default`, then overwritten at each `assign` "
            "type in the order listed. It is NOT a (building_id -> callback) table and cannot be: "
            "each setter LOOPS over building ids first_id..slots-1 and writes the callback into "
            "every slot whose *(uint8)(cfg_scan_base + id*cfg_stride) equals its type argument, so "
            "the id map depends on cfg data loaded at runtime -- a reimplementation reproduces that "
            "SCAN, using the constants recorded here. `callbacks` is the flat distinct set (`via` "
            "naming which table each is installed into), consumed by gen_migration.py as a second "
            "dispatch seed set (batch H / SIM1-BLDGCB) and by the dispatch-closure check."
        ),
        "registrar": CB_REGISTRAR,
        "tables": {k: tables[k] for k in sorted(tables)},
        "unnamed": sorted(set(unnamed)),
        "callbacks": [by_name[n] for n in order],
    }


def distinct(data):
    """Every handler the binary installs, in first-appearance order."""
    out = []
    seen = set()
    for key in ("unit", "bldg"):
        t = data["tables"][key]
        for e in [t["default"]] + t["assign"]:
            if e["name"] not in seen:
                seen.add(e["name"])
                out.append({"name": e["name"], "addr": e["addr"], "table": key})
    return out


def render(data):
    L = []
    A = L.append
    A("// mh_state_handlers.gen.h -- GENERATED by tools/gen_state_handler_table.py. DO NOT EDIT.")
    A("//")
    A("// The two strategic state-machine dispatch tables, read out of the disassembly of")
    A("// %s (see that generator's docstring for why they are extracted" % data["registrar"])
    A(
        "// rather than transcribed). Consumed by libmh/sim/sim_register_state_handlers.cpp, which fills"
    )
    A("// both tables with OUR handler pointers instead of the original's (SIM1-DISPATCH).")
    A("//")
    A(
        "// A table is filled 0..SLOTS-1 with DEFAULT_VA and then overwritten at each ASSIGN entry, in"
    )
    A("// the order the original registrar performs those writes. An unassigned state holds the")
    A("// default, which is a real handler and not a null.")
    A("#pragma once")
    A("#include <cstdint>")
    A("")
    A("namespace mh::addr {")
    A("")
    A("// One (state -> original handler) pair the registrar installs.")
    A("//")
    A("// TWO FORMS, AND THE SECOND ONE IS NOT A CONVENIENCE (LIB-DISPATCH-SA, 2026-09-11). The")
    A("// hosted registrar is keyed by ORIGINAL VA -- it has to be, because it writes entry thunks")
    A("// that patch original addresses. The STANDALONE registrar fills the same tables with plain")
    A("// C++ bodies and is keyed by NAME, and it may not so much as SEE an original address: a")
    A("// constexpr array is emitted as data the moment it is walked at runtime, so keying the")
    A("// standalone fill off the VA-bearing rows would put 68 original-image addresses back into")
    A("// libmh.lib's initialised data and break LIB-REF-SPLIT's measured zero (the ratchet is")
    A("// tools/scan_libmh_vas.py, which reads object BYTES and cannot be talked out of it).")
    A("// So the VA-bearing rows are compiled out of the standalone build entirely, and the")
    A("// name-keyed rows carry no address column at all.")
    A("#ifndef MH_LIBMH_BUILD")
    A("struct state_handler_slot {")
    A("    uint8_t     state;       // table index")
    A("    uintptr_t   original_va; // the ORIGINAL handler the game installs here")
    A("    const char *name;        // its Ghidra symbol")
    A("};")
    A("#endif")
    A("")
    A("// The standalone form: the SAME rows in the SAME order, minus the address column.")
    A("struct state_handler_slot_sa {")
    A("    uint8_t     state; // table index")
    A("    const char *name;  // its Ghidra symbol -- the key the standalone fill binds by")
    A("};")
    A("")
    for key, label in (("unit", "UNIT"), ("bldg", "BLDG")):
        t = data["tables"][key]
        A(
            "// ---- the %s state table: %d slots @ %s, filled by %s"
            % (key, t["slots"], t["base"], t["setter"])
        )
        A("#ifndef MH_LIBMH_BUILD")
        A("inline constexpr uintptr_t %s_STATE_TABLE_BASE  = %su;" % (label, t["base"]))
        A("#endif")
        A("inline constexpr int       %s_STATE_TABLE_SLOTS = %d;" % (label, t["slots"]))
        A("#ifndef MH_LIBMH_BUILD")
        A(
            "inline constexpr uintptr_t %s_STATE_DEFAULT_VA  = %su;  // %s"
            % (label, t["default"]["addr"], t["default"]["name"])
        )
        A("#endif")
        A(
            'inline constexpr const char *%s_STATE_DEFAULT_NAME = "%s";'
            % (label, t["default"]["name"])
        )
        A("#ifndef MH_LIBMH_BUILD")
        A("inline constexpr state_handler_slot %s_STATE_ASSIGN[] = {" % label)
        for e in t["assign"]:
            A(
                '    {0x%02x, %su, "%s"},  // set at %s'
                % (e["state"], e["addr"], e["name"], e["at"])
            )
        A("};")
        A(
            "inline constexpr int %s_STATE_ASSIGN_COUNT ="
            " (int)(sizeof(%s_STATE_ASSIGN) / sizeof(%s_STATE_ASSIGN[0]));" % (label, label, label)
        )
        A("#endif")
        A("// The same rows, name-keyed, for the standalone fill (see state_handler_slot_sa).")
        A("inline constexpr state_handler_slot_sa %s_STATE_ASSIGN_SA[] = {" % label)
        for e in t["assign"]:
            A('    {0x%02x, "%s"},  // set at %s' % (e["state"], e["name"], e["at"]))
        A("};")
        A(
            "inline constexpr int %s_STATE_ASSIGN_SA_COUNT ="
            " (int)(sizeof(%s_STATE_ASSIGN_SA) / sizeof(%s_STATE_ASSIGN_SA[0]));"
            % (label, label, label)
        )
        A("")
    A("// Every DISTINCT handler the two tables between them install, defaults included. The")
    A(
        "// registrar TU expands this to bind each original to its C++ reimplementation, so a handler"
    )
    A("// that appears in the binary with no binding is a compile error rather than a silent gap.")
    A("//   X(original_symbol, cpp_stem)  -- cpp_stem is the original name minus the llm_strat_")
    A(
        "//                                    prefix, which is the mh::sim wrapper's name for all 68."
    )
    A("#define MH_STATE_HANDLERS(X) \\")
    ds = distinct(data)
    for i, e in enumerate(ds):
        stem = e["name"][len("llm_strat_") :]
        A("    X(%s, %s)%s" % (e["name"], stem, " \\" if i + 1 < len(ds) else ""))
    A("")
    A("inline constexpr int STATE_HANDLER_DISTINCT_COUNT = %d;" % len(ds))
    A("")
    A("} // namespace mh::addr")
    return "\n".join(L) + "\n"


def cb_distinct(data):
    """Every callback the registrar installs, in first-appearance order (the JSON's own order)."""
    return data["callbacks"]


def cb_render(data):
    L = []
    A = L.append
    A(
        "// mh_bldg_type_callbacks.gen.h -- GENERATED by tools/gen_state_handler_table.py. DO NOT EDIT."
    )
    A("//")
    A("// The two PER-BUILDING-TYPE callback tables, read out of the disassembly of")
    A("// %s (see that generator's docstring for why they are" % data["registrar"])
    A("// extracted rather than transcribed). Consumed by")
    A(
        "// libmh/sim/sim_register_bldg_type_callbacks.cpp, which fills both tables with OUR callback"
    )
    A("// pointers instead of the original's (SIM1-BLDGCB).")
    A("//")
    A(
        "// THIS IS A (building TYPE -> callback) PAIRING, NOT A (building_id -> callback) TABLE, and"
    )
    A("// the difference is load-bearing. The original's setters do not index by type at all: each")
    A(
        "// one LOOPS over building ids FIRST_ID..SLOTS-1 and writes its callback into every slot whose"
    )
    A("// cfg type byte equals the type argument. That map therefore depends on cfg data loaded at")
    A(
        "// runtime and is not knowable from any listing. Our registrar reproduces the SCAN, with the"
    )
    A("// three constants below read out of the setters' own listings.")
    A("//")
    A("// A table is filled for every type in DEFAULT_TYPE_LO..DEFAULT_TYPE_HI with DEFAULT_VA and")
    A("// then overwritten at each ASSIGN type, in the order the original registrar performs those")
    A("// writes. A building whose type is outside that range is left ALONE -- unlike the state")
    A("// tables, this registrar does not cover every slot, so such a slot keeps whatever the .bss")
    A("// held. That is the original's behaviour and is reproduced, not corrected.")
    A("#pragma once")
    A("#include <cstdint>")
    A("")
    A("namespace mh::addr {")
    A("")
    A("// One (building type -> original callback) pair the registrar installs.")
    A("// TWO FORMS, for the reason mh_state_handlers.gen.h's state_handler_slot records: the")
    A(
        "// standalone registrar is keyed by NAME and may not see an original address at all, because"
    )
    A("// a constexpr array walked at runtime is emitted as data (LIB-DISPATCH-SA, 2026-09-11).")
    A("#ifndef MH_LIBMH_BUILD")
    A("struct bldg_type_callback_slot {")
    A("    uint8_t     type;        // cfg Building[].type this callback is registered for")
    A("    uintptr_t   original_va; // the ORIGINAL callback the game installs for that type")
    A("    const char *name;        // its Ghidra symbol")
    A("};")
    A("#endif")
    A("")
    A("// The standalone form: the SAME rows in the SAME order, minus the address column.")
    A("struct bldg_type_callback_slot_sa {")
    A("    uint8_t     type; // cfg Building[].type this callback is registered for")
    A("    const char *name; // its Ghidra symbol -- the key the standalone fill binds by")
    A("};")
    A("")
    any_t = next(iter(data["tables"].values()))
    A(
        "// ---- the cfg scan BOTH setters perform, from their own listings --------------------------"
    )
    A("// The generator refuses to emit if the two setters disagree on any of these.")
    A(
        "inline constexpr uintptr_t BLDG_TYPE_CFG_SCAN_BASE = %su;  // &Building[0].type"
        % any_t["cfg_scan_base"]
    )
    A(
        "inline constexpr uint32_t  BLDG_TYPE_CFG_STRIDE    = 0x%xu;      // sizeof(cfg Building record)"
        % any_t["cfg_stride"]
    )
    A(
        "inline constexpr int       BLDG_TYPE_FIRST_ID      = %d;          // the setters' id-loop start"
        % any_t["first_id"]
    )
    A(
        "inline constexpr int       BLDG_TYPE_ID_LIMIT      = %d;        // the setters' id-loop bound (id < this)"
        % any_t["slots"]
    )
    A("")
    for key, label in (("done", "BLDG_DONE"), ("tick2", "BLDG_TICK2")):
        t = data["tables"][key]
        d = t["default"]
        A("// ---- the %s table @ %s, filled by %s" % (key, t["base"], t["setter"]))
        A("#ifndef MH_LIBMH_BUILD")
        A("inline constexpr uintptr_t %s_TABLE_BASE = %su;" % (label, t["base"]))
        A(
            "inline constexpr uintptr_t %s_DEFAULT_VA   = %su;  // %s"
            % (label, d["addr"], d["name"])
        )
        A("#endif")
        A('inline constexpr const char *%s_DEFAULT_NAME = "%s";' % (label, d["name"]))
        A(
            "inline constexpr int %s_DEFAULT_TYPE_LO = 0x%02x;  // the default fill's loop, set at %s"
            % (label, d["type_lo"], d["at"])
        )
        A("inline constexpr int %s_DEFAULT_TYPE_HI = 0x%02x;  // inclusive" % (label, d["type_hi"]))
        A("#ifndef MH_LIBMH_BUILD")
        A("inline constexpr bldg_type_callback_slot %s_ASSIGN[] = {" % label)
        for e in t["assign"]:
            A('    {0x%02x, %su, "%s"},  // set at %s' % (e["type"], e["addr"], e["name"], e["at"]))
        A("};")
        A(
            "inline constexpr int %s_ASSIGN_COUNT ="
            " (int)(sizeof(%s_ASSIGN) / sizeof(%s_ASSIGN[0]));" % (label, label, label)
        )
        A("#endif")
        A("// The same rows, name-keyed, for the standalone fill (see bldg_type_callback_slot_sa).")
        A("inline constexpr bldg_type_callback_slot_sa %s_ASSIGN_SA[] = {" % label)
        for e in t["assign"]:
            A('    {0x%02x, "%s"},  // set at %s' % (e["type"], e["name"], e["at"]))
        A("};")
        A(
            "inline constexpr int %s_ASSIGN_SA_COUNT ="
            " (int)(sizeof(%s_ASSIGN_SA) / sizeof(%s_ASSIGN_SA[0]));" % (label, label, label)
        )
        A("")
    A("// Every DISTINCT callback the two tables between them install, defaults included. The")
    A(
        "// registrar TU expands this to bind each original to its C++ reimplementation, so a callback"
    )
    A("// that appears in the binary with no binding is a compile error rather than a silent gap.")
    A("//   X(original_symbol, cpp_stem)  -- cpp_stem is the original name minus the llm_strat_")
    A(
        "//                                    prefix, which is the mh::sim wrapper's name for all 30."
    )
    A("#define MH_BLDG_TYPE_CALLBACKS(X) \\")
    ds = cb_distinct(data)
    for i, e in enumerate(ds):
        stem = e["name"][len("llm_strat_") :]
        A("    X(%s, %s)%s" % (e["name"], stem, " \\" if i + 1 < len(ds) else ""))
    A("")
    A("inline constexpr int BLDG_TYPE_CALLBACK_DISTINCT_COUNT = %d;" % len(ds))
    A("")
    A("} // namespace mh::addr")
    return "\n".join(L) + "\n"


def cb_emit(check):
    """Write (or drift-check) the bldg-type callback header. Returns an exit message or None."""
    rel = os.path.relpath(CB_OUT, REPO).replace("\\", "/")
    if not os.path.exists(CB_DATA):
        return "%s is missing -- run with --extract-bldg-type" % os.path.relpath(CB_DATA, REPO)
    with open(CB_DATA, encoding="utf-8") as fh:
        data = json.load(fh)
    if "assign" not in next(iter(data["tables"].values())):
        return (
            "%s predates the (type -> callback) extraction -- re-run with --extract-bldg-type"
            % os.path.relpath(CB_DATA, REPO)
        )
    text = cb_render(data)
    n = sum(len(t["assign"]) for t in data["tables"].values())
    if check:
        cur = open(CB_OUT, encoding="utf-8").read() if os.path.exists(CB_OUT) else ""
        if cur != text:
            return "DRIFT: %s is stale -- run python tools/gen_state_handler_table.py" % rel
        print(
            "bldg-type callback table: %s up to date (%d distinct callbacks, %d type assignments)"
            % (rel, len(cb_distinct(data)), n)
        )
        return None
    with open(CB_OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(
        "wrote %s (%d distinct callbacks, %d type assignments)" % (rel, len(cb_distinct(data)), n)
    )
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--extract", action="store_true", help="re-extract the table from exported .asm"
    )
    ap.add_argument("--asm-dir", default=os.path.join(REPO, "tmp", "dispatch_asm"))
    ap.add_argument(
        "--extract-bldg-type",
        action="store_true",
        help="re-extract the SECOND registry's callback set (SIM1-BLDGCB)",
    )
    ap.add_argument(
        "--check", action="store_true", help="drift gate: the committed header is current"
    )
    args = ap.parse_args()

    if args.extract_bldg_type:
        cb = extract_bldg_type_callbacks(args.asm_dir)
        with open(CB_DATA, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(cb, fh, indent=2)
            fh.write("\n")
        n = sum(len(t["assign"]) for t in cb["tables"].values())
        print(
            "extracted %d distinct bldg-type callbacks / %d type assignments (%d still FUN_) -> %s"
            % (
                len(cb["callbacks"]),
                n,
                len(cb["unnamed"]),
                os.path.relpath(CB_DATA, REPO).replace("\\", "/"),
            )
        )

    if args.extract:
        data = extract(args.asm_dir)
        with open(DATA, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(data, fh, indent=2)
            fh.write("\n")
        n = sum(len(t["assign"]) for t in data["tables"].values())
        print(
            "extracted %d assignments + 2 default fills -> %s"
            % (n, os.path.relpath(DATA, REPO).replace("\\", "/"))
        )

    if not os.path.exists(DATA):
        sys.exit("%s is missing -- run with --extract" % os.path.relpath(DATA, REPO))
    with open(DATA, encoding="utf-8") as fh:
        data = json.load(fh)
    text = render(data)
    rel = os.path.relpath(OUT, REPO).replace("\\", "/")

    if args.check:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            sys.exit("DRIFT: %s is stale -- run python tools/gen_state_handler_table.py" % rel)
        print(
            "state-handler table: %s up to date (%d distinct handlers)" % (rel, len(distinct(data)))
        )
        # BOTH registries are gated by the one --check, so a lint that runs it cannot cover one
        # dispatch layer and silently skip the other -- which is the exact shape of the blind spot
        # SIM1-BLDGCB exists to close.
        err = cb_emit(check=True)
        if err:
            sys.exit(err)
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote %s (%d distinct handlers)" % (rel, len(distinct(data))))
    err = cb_emit(check=False)
    if err:
        sys.exit(err)


if __name__ == "__main__":
    main()
