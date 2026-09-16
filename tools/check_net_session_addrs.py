#!/usr/bin/env python3
"""check_net_session_addrs.py -- NET-SESSION's ADDRESS TABLE, proved offline out of the retail bytes.

WHAT IT REPLACES, AND WHY THE REPLACEMENT IS NOT A DOWNGRADE. `llm_net_session_globals_reset`
@0x0049e4c3 exists to leave 22 session/pacing globals holding specific values, and libmh's
translation of it writes through 22 pointers. `libmh_selftest.exe netsessiontest` proves our body writes
the right values into a FIXTURE; it cannot prove the 22 pointers name the addresses the original
writes. Until F1B that second half was answered by a LIVE read-back (mh/seams/net_session_probe.cpp,
read by tools/check_netprobe.py -- both deleted at fork F2F, which this check made possible):  # CITATION-OK
boot the game twice, once with our body serving the call and once
with the original serving it, and require both runs to report 22/22 at the same instant. The second
run is the arm that proved the address table -- our own body compared against our own pointers would
agree with itself.

That evidence needs a rig, a game install and two boots, and it is deleted at fork F2 along with the
seam that produces it. This script proves the SAME PROPOSITION from the retail image alone: it
disassembles the original function, extracts every absolute store it performs, and asks whether that
store table is exactly the table libmh's `session_init_state` claims -- the same addresses, in the
same widths, holding the same constants. No rig, no boot, no game running, ~0.1 s.

It is strictly narrower in one way and strictly wider in another, and both are worth stating. The
probe watched the process, so it also covered "the region actually lives where the binary put it"
(a relocating host moves several of these). This reads the binary, so it covers only what the binary
says -- but it covers it EVERY lint run rather than whenever someone has a rig, and it reads all 22
addresses out of the instruction stream rather than comparing against a hand table.

FOUR ARMS, each reported on its own line:

  1. EXTRACTION TOTALITY. Every instruction of the body must classify as prologue, the __STK
     stack-probe call, an absolute-displacement immediate store, or epilogue. ANYTHING ELSE REFUSES.
     This is the arm that makes the other three mean something: a partial extraction would produce a
     table that is right about every row it has and silent about the row it dropped, and a silent
     gap reads exactly like a clean pass. Absence is a failure (the same rule the probe's checker
     enforced on a missing log line). The specific trap in this direction is that
     a Watcom [entry..end] extent can carry bytes that were never that function's, so the check also
     requires the decode to land exactly on `end` with no leftover byte.

  2. ADDRESS TABLE. Every folded store target must be a region BASE in tools/data/state_regions.json,
     and the folded field set must EXACTLY equal the expected 22-region id set -- no extra store, no
     missing one. Zero vacuous rows: a row cannot pass by naming an address nothing claims, and the
     table cannot pass while a store went somewhere unlisted.

  3. VALUES. Each folded value must equal the corresponding constant in
     src/mh_dll/libmh/lockstep/net_session.h. The header is parsed TEXTUALLY (the precedent is
     lint_region_mirror.py, which compares two ARTEFACTS rather than two generator inputs) so that
     the constants the ORIGINAL writes and the constants libmh writes cannot drift in one direction
     only: edit SESSION_HORIZON_INIT and this goes red, because the retail bytes did not move.

  4. PROVENANCE. The exe's sha256 and the function body's sha256 must match the stamp committed in
     tools/data/net_session_stores.json. A different build FAILS LOUDLY; it never silently
     re-baselines. Re-baselining is an explicit `--rederive`.

THE EXPECTED FIELD SET is transcribed from the probe's `ROWS[]` (src/mh_dll/mh/seams/
net_session_probe.cpp, 22 rows in store order) -- that is where the 22 ids and their constants were
adjudicated. It is TRANSCRIBED rather than parsed because the probe is deleted at fork F2 and this
check must outlive it; EXPECTED_FIELDS below is the surviving copy.

TWO TIERS:

    --rederive      needs a game exe (default machine.POLYGON_CLEAN/mh.exe, --exe overrides) and
                    REWRITES tools/data/net_session_stores.json. This is the only way the stamp moves.
    --check         (default) offline. Arms 2-4 run against the committed extract; where the game exe
                    exists on this machine it ALSO re-derives and requires identity with the committed
                    extract. Where it does not exist, the output SAYS the live half was not run --
                    it never passes silently on a machine that could not check.
    --selftest      synthetic fixtures in a temp dir: a good one that must pass and four mutants
                    (address shifted, value flipped, row dropped, stamp blanked) that must each go
                    RED. Driven off fixtures, NOT off the tree, so it cannot go inert when the tree
                    is clean -- and it asserts afterwards that the real files were untouched.

Usage:
    python tools/check_net_session_addrs.py              # --check
    python tools/check_net_session_addrs.py --rederive
    python tools/check_net_session_addrs.py --selftest
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_config as machine  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTRACT = os.path.join(REPO, "tools", "data", "net_session_stores.json")
REGIONS = os.path.join(REPO, "tools", "data", "state_regions.json")
FUNCTIONS = os.path.join(REPO, "tools", "data", "en_functions.json")
HEADER = os.path.join(REPO, "src", "mh_dll", "libmh", "lockstep", "net_session.h")

FUNC = "llm_net_session_globals_reset"
# The Watcom stack probe every body in this closure opens with: `PUSH n; CALL utils_assert_stack_
# capacity`. It touches no tracked state, which is why the translation drops it -- and why it has to
# be CLASSIFIED here rather than merely tolerated: an unrecognised CALL must refuse.
STACK_PROBE = "utils_assert_stack_capacity"

# The expected fields, in store order, transcribed from net_session_probe.cpp ROWS[]. The second
# element is the net_session.h constant the store must carry, or "0" for a zeroed counter.
EXPECTED_FIELDS = [
    ("NET_LOCAL_PLAYER_INDEX", "0"),
    ("NET_LOCAL_PLAYER_SLOT", "0"),
    ("NET_IS_HOST", "0"),
    ("NET_LOCKSTEP_PLAYER_COUNT", "0"),
    ("NET_LOBBY_MAP_RECV_DONE", "0"),
    ("NET_SEND_BUF_CURSOR", "0"),
    ("MP_PROBE_SERVER_COUNT", "0"),
    ("NET_SESSION_COUNT", "0"),
    ("NET_LOBBY_SCAN_HOST_COUNT", "0"),
    ("STRAT_LOCKSTEP_COMMITTED_HORIZON", "SESSION_HORIZON_INIT"),
    ("STRAT_LOCKSTEP_HORIZON", "SESSION_HORIZON_INIT"),
    ("STRAT_LOCKSTEP_STEP_SIZE", "SESSION_HORIZON_INIT"),
    ("STRAT_LOCKSTEP_ADAPT_NEXT_TIME", "SESSION_ADAPT_NEXT_TIME_INIT"),
    ("GAME_SESSION_MODE", "SESSION_MODE_MP_LOCAL"),
    ("NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN", "SESSION_SYNC_RETRY_COUNTDOWN_INIT"),
    ("NET_LOCKSTEP_STALL_NAG_COUNT", "0"),
    ("NET_LOCKSTEP_RESYNC_TRIGGER_COUNT", "0"),
    ("STRAT_LOCKSTEP_STALL_COUNT", "0"),
    ("NET_LOCKSTEP_SYNC_WAIT_ELAPSED", "SESSION_SYNC_WAIT_ELAPSED_INIT"),
    ("NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED", "SESSION_PEER_TIMEOUT_ELAPSED_INIT"),
    ("NET_SYNC_WAIT_ACTIVE", "0"),
    ("NET_RESYNC_IN_PROGRESS", "0"),
]

# ------------------------------------------------------------------------------------------------
# Loading the three tracked artefacts (all Ghidra-free, all committed).


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def region_bases(regions_path):
    """base address -> [region id, ...]. A base with several ids is a real thing in this registry
    (aliases/views), so the mapping is one-to-many and the caller picks by expectation."""
    out = {}
    for r in load_json(regions_path)["regions"]:
        out.setdefault(int(r["base"]), []).append(r["id"])
    return out


CONST = re.compile(r"inline\s+constexpr\s+(double|int32_t)\s+(\w+)\s*=\s*([^;]+);")


def session_constants(header_path):
    """Parse net_session.h TEXTUALLY -> {name: (kind, python value)}.

    Textual on purpose: this is the artefact libmh compiles, so a value that changes here without
    the retail bytes changing is exactly the one-directional drift arm 3 exists to catch.
    """
    with open(header_path, encoding="utf-8") as fh:
        src = fh.read()
    out = {}
    for kind, name, raw in CONST.findall(src):
        raw = raw.strip()
        out[name] = (kind, float(raw) if kind == "double" else int(raw, 0))
    return out


def expected_bits(const_name, consts):
    """(width, raw bit pattern) a field must hold. Floats are compared as BIT PATTERNS -- the
    original stores two dword immediates, so "is it 10.0" is really "are those eight bytes
    0x4024000000000000", which is also exact rather than an epsilon question."""
    if const_name == "0":
        return 4, 0
    if const_name not in consts:
        return None, None
    kind, value = consts[const_name]
    if kind == "double":
        return 8, struct.unpack("<Q", struct.pack("<d", value))[0]
    return 4, value & 0xFFFFFFFF


def function_extent(functions_path, name):
    for f in load_json(functions_path)["functions"]:
        if f["name"] == name:
            return int(f["entry"], 16), int(f["end"], 16)
    raise KeyError("%s is not in %s" % (name, functions_path))


# ------------------------------------------------------------------------------------------------
# Re-derivation from the retail image.


def read_image(exe):
    """(imagebase, read(va, n)) for the retail exe.

    The section walk is done by hand rather than through lief's virtual-address helper: this PE's
    section headers carry virtual_size == 0 (an old Watcom linker), so the helper resolves nothing
    and the raw size is the span that matters.
    """
    import lief

    # This PE makes lief warn about `.bss` padding on every parse. The warning is correct and
    # irrelevant (the block is uninitialised by definition) and it would land in the middle of a
    # lint line, so it is silenced rather than left to look like a finding.
    lief.logging.disable()
    binary = lief.PE.parse(exe)
    base = binary.optional_header.imagebase
    with open(exe, "rb") as fh:
        raw = fh.read()
    sections = [
        (s.virtual_address, max(s.virtual_size, s.sizeof_raw_data), s.pointerto_raw_data)
        for s in binary.sections
    ]

    def read(va, n):
        rva = va - base
        for start, span, off in sections:
            if start <= rva < start + span:
                return raw[off + (rva - start) : off + (rva - start) + n]
        raise KeyError("VA 0x%08x is in no section of %s" % (va, exe))

    return base, read, hashlib.sha256(raw).hexdigest()


def classify_body(body, entry, stk_va):
    """Disassemble the body and classify EVERY instruction. -> (stores, kinds, refusals).

    A store is `mov <width> ptr [imm32], imm` with no base and no index register -- the only shape
    that names an absolute address. The width is read off the operand rather than assumed dword:
    this table would be wrong in a way nothing else could see if a `mov byte ptr` were folded as 4.
    """
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    stores, kinds, refusals = [], {}, []
    covered = 0
    saw_probe_call = False
    for ins in md.disasm(body, entry):
        covered += ins.size
        kind = None
        ops = ins.operands
        m, o = ins.mnemonic, ins.op_str
        if m == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM:
            mem = ops[0].mem
            if (
                mem.base == 0
                and mem.index == 0
                and ops[1].type == capstone.x86.X86_OP_IMM
                and mem.disp > 0
            ):
                kind = "store"
                stores.append(
                    {
                        "at": "0x%08x" % ins.address,
                        "addr": "0x%08x" % (mem.disp & 0xFFFFFFFF),
                        "width": ops[0].size,
                        "value": "0x%0*x" % (ops[0].size * 2, ops[1].imm & ((1 << 32) - 1)),
                    }
                )
        if kind is None and m == "call" and len(ops) == 1:
            if ops[0].type == capstone.x86.X86_OP_IMM and ops[0].imm == stk_va:
                kind, saw_probe_call = "stack_probe_call", True
        if kind is None and not stores:
            # the prologue: frame setup, the stack-probe argument, the callee-saved pushes, and the
            # local-frame reservation. Anything else before the first store is NOT prologue.
            if (
                (m == "push" and ops[0].type in (capstone.x86.X86_OP_REG, capstone.x86.X86_OP_IMM))
                or (m == "mov" and o == "ebp, esp")
                or (m == "sub" and o.startswith("esp,"))
            ):
                kind = "prologue"
        if kind is None and stores:
            if (
                m in ("pop", "ret", "leave")
                or (m == "lea" and o.startswith("esp,"))
                or (m == "mov" and o == "esp, ebp")
            ):
                kind = "epilogue"
        if kind is None:
            refusals.append("0x%08x  %s %s" % (ins.address, m, o))
        else:
            kinds[kind] = kinds.get(kind, 0) + 1
    if covered != len(body):
        refusals.append(
            "the decode covered %d of %d bytes -- the extent does not end on an instruction "
            "boundary -- a Watcom extent can carry foreign bytes, because the compiler "
            "shares epilogue tails and lays out disjoint bodies" % (covered, len(body))
        )
    if not saw_probe_call:
        refusals.append(
            "no %s call -- the prologue is not the one this table was read from" % STACK_PROBE
        )
    return stores, kinds, refusals


def fold_stores(stores, bases):
    """Fold contiguous stores into FIELDS. A store whose target is a region base opens a field; the
    following store continues it only while it is contiguous AND its own address is NOT a base --
    which is how the six 8-byte doubles (written as two dword immediates, low half then high half)
    come back together without a hand list of which fields are doubles."""
    fields, i = [], 0
    while i < len(stores):
        group = [stores[i]]
        addr = int(stores[i]["addr"], 16)
        end = addr + stores[i]["width"]
        j = i + 1
        while j < len(stores) and int(stores[j]["addr"], 16) == end and end not in bases:
            group.append(stores[j])
            end += stores[j]["width"]
            j += 1
        width = end - addr
        bits = 0
        off = 0
        for s in group:
            bits |= (int(s["value"], 16) & ((1 << (8 * s["width"])) - 1)) << (8 * off)
            off += s["width"]
        fields.append(
            {
                "region": (bases.get(addr) or ["<unlisted>"])[0],
                "addr": "0x%08x" % addr,
                "width": width,
                "bits": "0x%0*x" % (width * 2, bits),
                "stores": [s["at"] for s in group],
            }
        )
        i = j
    return fields


def resolve_regions(fields, bases, expected_ids):
    """Prefer the EXPECTED id when a base carries several -- an alias must not decide the name."""
    for f in fields:
        ids = bases.get(int(f["addr"], 16), [])
        hit = [i for i in ids if i in expected_ids]
        f["region"] = (hit or ids or ["<unlisted>"])[0]
    return fields


def rederive(exe, regions_path=REGIONS, functions_path=FUNCTIONS):
    """Build the extract from the retail image. Raises on a refusal from arm 1."""
    entry, end = function_extent(functions_path, FUNC)
    _base, read, exe_sha = read_image(exe)
    body = read(entry, end - entry + 1)
    stk_entry, _stk_end = function_extent(functions_path, STACK_PROBE)
    stores, kinds, refusals = classify_body(body, entry, stk_entry)
    bases = region_bases(regions_path)
    fields = resolve_regions(fold_stores(stores, bases), bases, {i for i, _ in EXPECTED_FIELDS})
    return {
        "_generated_by": "tools/check_net_session_addrs.py --rederive",
        "_do_not_hand_edit": True,
        "_comment": [
            "NET-SESSION's address table, extracted from the RETAIL bytes of",
            "llm_net_session_globals_reset -- every absolute store the original performs, folded",
            "into the 22 fields libmh's session_init_state claims.",
            "",
            "WHAT IT IS EVIDENCE FOR. The offline oracle netsessiontest proves our body writes the",
            "right values into a fixture; it cannot prove our 22 pointers name the addresses the",
            "ORIGINAL writes. This table is that second half, read out of the instruction stream",
            "rather than out of a hand list -- see the header of the generator for the four arms",
            "and for what it replaces (the live read-back probe, deleted at fork F2).",
            "",
            "VALUES ARE RAW BIT PATTERNS. The doubles are stored by the original as two dword",
            "immediates, so the comparison is over the eight bytes (0x4024000000000000 == 10.0),",
            "which is exact rather than an epsilon question.",
            "",
            "REBASELINING IS EXPLICIT: --rederive, never a silent re-stamp on a changed binary.",
        ],
        "_measured": datetime.date.today().isoformat(),
        "function": {
            "name": FUNC,
            "entry": "0x%08x" % entry,
            "end": "0x%08x" % end,
            "size": len(body),
        },
        "provenance": {
            "exe": os.path.basename(exe),
            "exe_sha256": exe_sha,
            "body_sha256": hashlib.sha256(body).hexdigest(),
        },
        "classification": {
            "instructions": sum(kinds.values()) + len(refusals),
            "bytes": len(body),
            "by_kind": {k: kinds[k] for k in sorted(kinds)},
            "unclassified": refusals,
        },
        "stores": stores,
        "fields": fields,
    }


# ------------------------------------------------------------------------------------------------
# The four arms, over an extract that is already in hand.


def _identity(extract):
    """Serialisation used for the committed-vs-re-derived comparison. `_measured` is EXCLUDED: it
    is the date of the measurement, not part of it, and including it would turn the gate red on the
    first run of the next day after a --rederive."""
    return json.dumps({k: v for k, v in extract.items() if k != "_measured"}, sort_keys=True)


def check_extract(extract, regions_path, header_path, expected, live=None, exe_note=""):
    """Run the four arms. -> (ok, [line, ...]). Pure: it reads the two artefacts and judges."""
    lines, ok = [], True

    def arm(n, title, good, detail, problems):
        nonlocal ok
        lines.append("[%s] arm %d %s -- %s" % ("ok" if good else "FAIL", n, title, detail))
        for p in problems:
            lines.append("      " + p)
        if not good:
            ok = False

    bases = region_bases(regions_path)
    consts = session_constants(header_path)
    fields = extract.get("fields", [])
    stores = extract.get("stores", [])
    cls = extract.get("classification", {})

    # ---- arm 1: extraction totality --------------------------------------------------------------
    bad = list(cls.get("unclassified", []))
    kinds = cls.get("by_kind", {})
    if kinds.get("store", -1) != len(stores):
        bad.append(
            "the classification counts %s stores, the table holds %d"
            % (kinds.get("store"), len(stores))
        )
    if sum(kinds.values()) != cls.get("instructions"):
        bad.append("the per-kind counts do not sum to the instruction count")
    if cls.get("bytes") != extract.get("function", {}).get("size"):
        bad.append("the classified byte count is not the function's size")
    seen = {s["at"] for s in stores}
    for f in fields:
        if any(a not in seen for a in f["stores"]):
            bad.append("field %s folds a store that is not in the store table" % f["region"])
        if sum(s["width"] for s in stores if s["at"] in f["stores"]) != f["width"]:
            bad.append(
                "field %s: the folded widths do not add up to %d" % (f["region"], f["width"])
            )
    if len({a for f in fields for a in f["stores"]}) != len(stores):
        bad.append("the folded fields do not account for every store exactly once")
    if live is not None and _identity(live) != _identity(extract):
        bad.append(
            "the re-derivation from %s DIFFERS from the committed extract -- run --rederive and "
            "read the diff before committing it" % live["provenance"]["exe"]
        )
    arm(
        1,
        "EXTRACTION TOTALITY",
        not bad,
        "%s instructions, %d store(s) -> %d field(s), %d unclassified%s"
        % (
            cls.get("instructions"),
            len(stores),
            len(fields),
            len(cls.get("unclassified", [])),
            exe_note,
        ),
        bad,
    )

    # ---- arm 2: the address table ----------------------------------------------------------------
    bad = []
    got_ids = []
    for f in fields:
        addr = int(f["addr"], 16)
        ids = bases.get(addr, [])
        if not ids:
            bad.append("store target 0x%08x is NOT a region base in state_regions.json" % addr)
            continue
        if f["region"] not in ids:
            bad.append("field %s: 0x%08x is the base of %s" % (f["region"], addr, "/".join(ids)))
        got_ids.append(f["region"])
    want_ids = [i for i, _ in expected]
    for miss in sorted(set(want_ids) - set(got_ids)):
        bad.append("EXPECTED region %s has no store -- the extraction is missing a field" % miss)
    for extra in sorted(set(got_ids) - set(want_ids)):
        bad.append("region %s is stored to but is not in the expected set" % extra)
    if len(got_ids) != len(set(got_ids)):
        bad.append("a region is stored to more than once -- the fold did not close")
    arm(
        2,
        "ADDRESS TABLE",
        not bad,
        "%d/%d folded fields are region bases and the id set is exact"
        % (len(got_ids), len(want_ids)),
        bad,
    )

    # ---- arm 3: the values ------------------------------------------------------------------------
    bad = []
    by_id = {f["region"]: f for f in fields}
    doubles = ints = matched = 0
    for rid, const in expected:
        f = by_id.get(rid)
        if f is None:
            continue  # arm 2 already named it
        width, bits = expected_bits(const, consts)
        if width is None:
            bad.append("%s: net_session.h declares no constant %s" % (rid, const))
            continue
        if f["width"] != width:
            bad.append("%s: stored as %d bytes, %s is %d" % (rid, f["width"], const, width))
            continue
        if int(f["bits"], 16) != bits:
            bad.append(
                "%s: the original stores %s, %s is 0x%0*x"
                % (rid, f["bits"], const, width * 2, bits)
            )
            continue
        matched += 1
        doubles, ints = (doubles + 1, ints) if width == 8 else (doubles, ints + 1)
    arm(
        3,
        "VALUES",
        not bad,
        "%d/%d match net_session.h (%d double, %d int32)" % (matched, len(expected), doubles, ints),
        bad,
    )

    # ---- arm 4: provenance -------------------------------------------------------------------------
    bad = []
    prov = extract.get("provenance", {})
    for key in ("exe_sha256", "body_sha256"):
        if not prov.get(key):
            bad.append("the committed extract carries no %s" % key)
    if live is not None:
        for key in ("exe_sha256", "body_sha256"):
            if prov.get(key) != live["provenance"].get(key):
                bad.append(
                    "%s: committed %s, this machine's %s %s"
                    % (key, prov.get(key), live["provenance"]["exe"], live["provenance"].get(key))
                )
    arm(
        4,
        "PROVENANCE",
        not bad,
        "exe %s body %s%s"
        % (
            str(prov.get("exe_sha256"))[:16],
            str(prov.get("body_sha256"))[:16],
            exe_note if live is not None else " -- NOT re-measured" + exe_note,
        ),
        bad,
    )
    return ok, lines


# ------------------------------------------------------------------------------------------------


def default_exe():
    return os.path.join(machine.POLYGON_CLEAN, "mh.exe").replace("\\", "/")


def run_check(exe, extract_path=EXTRACT, regions_path=REGIONS, header_path=HEADER, expected=None):
    expected = EXPECTED_FIELDS if expected is None else expected
    extract = load_json(extract_path)
    live, note = None, ""
    if exe and os.path.exists(exe):
        live = rederive(exe, regions_path=regions_path)
        note = " (re-derived from %s)" % os.path.basename(exe)
    else:
        # Not a pass-by-default: the line has to SAY that the live half did not run, or a machine
        # with no game install reports the same green as one that re-derived and agreed.
        note = " (NO GAME EXE at %s -- nothing was re-derived this run)" % exe
    ok, lines = check_extract(
        extract, regions_path, header_path, expected, live=live, exe_note=note
    )
    print("\n".join(lines))
    print("check_net_session_addrs: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ------------------------------------------------------------------------------------------------
# The selftest. Fixtures only -- it must stay able to go red on a tree where everything is right.

SYN_HEADER = """#pragma once
namespace mh::lockstep {
inline constexpr double  SYN_D_INIT = 2.5;
inline constexpr int32_t SYN_I_INIT = 0x7;
}
"""


def _syn_fixture(tmp):
    regions = {
        "_comment": ["synthetic"],
        "regions": [
            {"id": "SYN_A", "name": "_G_SYN_A", "base": 0x1000},
            {"id": "SYN_B", "name": "_G_SYN_B", "base": 0x2000},
            {"id": "SYN_C", "name": "_G_SYN_C", "base": 0x3000},
        ],
    }
    stores = [
        {"at": "0x00400010", "addr": "0x00001000", "width": 4, "value": "0x00000000"},
        {"at": "0x00400020", "addr": "0x00002000", "width": 4, "value": "0x00000000"},
        {"at": "0x00400030", "addr": "0x00002004", "width": 4, "value": "0x40040000"},
        {"at": "0x00400040", "addr": "0x00003000", "width": 4, "value": "0x00000007"},
    ]
    fields = [
        {
            "region": "SYN_A",
            "addr": "0x00001000",
            "width": 4,
            "bits": "0x00000000",
            "stores": ["0x00400010"],
        },
        {
            "region": "SYN_B",
            "addr": "0x00002000",
            "width": 8,
            "bits": "0x4004000000000000",
            "stores": ["0x00400020", "0x00400030"],
        },
        {
            "region": "SYN_C",
            "addr": "0x00003000",
            "width": 4,
            "bits": "0x00000007",
            "stores": ["0x00400040"],
        },
    ]
    extract = {
        "_measured": "2026-01-01",
        "function": {"name": "syn", "entry": "0x00400000", "end": "0x0040004f", "size": 80},
        "provenance": {"exe": "syn.exe", "exe_sha256": "a" * 64, "body_sha256": "b" * 64},
        "classification": {
            "instructions": 6,
            "bytes": 80,
            "by_kind": {"epilogue": 1, "prologue": 1, "store": 4},
            "unclassified": [],
        },
        "stores": stores,
        "fields": fields,
    }
    paths = {}
    for name, obj in (("regions.json", regions), ("extract.json", extract)):
        paths[name] = os.path.join(tmp, name)
        with open(paths[name], "w", encoding="utf-8") as fh:
            json.dump(obj, fh, indent=1)
    paths["header.h"] = os.path.join(tmp, "header.h")
    with open(paths["header.h"], "w", encoding="utf-8") as fh:
        fh.write(SYN_HEADER)
    return paths, extract


SYN_EXPECTED = [("SYN_A", "0"), ("SYN_B", "SYN_D_INIT"), ("SYN_C", "SYN_I_INIT")]


def selftest():
    """Every mutation must go RED, and the unmutated fixture must go GREEN -- a checker that always
    fails passes a mutation suite that only tests for red."""
    real_before = [
        (p, hashlib.sha256(open(p, "rb").read()).hexdigest())
        for p in (EXTRACT, REGIONS, HEADER)
        if os.path.exists(p)
    ]
    ok = True
    with tempfile.TemporaryDirectory(prefix="net_session_addrs_") as tmp:
        paths, good = _syn_fixture(tmp)

        def judge(label, extract, want_pass, want_arm=None):
            """`want_arm` is the arm that must be the one to go red -- a mutation that fails the
            suite through some OTHER arm is a checker that is right by accident."""
            nonlocal ok
            passed, lines = check_extract(
                extract, paths["regions.json"], paths["header.h"], SYN_EXPECTED
            )
            hit = passed == want_pass
            if want_arm is not None:
                hit = hit and any(line.startswith("[FAIL] arm %d " % want_arm) for line in lines)
            print(
                "  [%s] %-28s %s (wanted %s%s)"
                % (
                    "ok" if hit else "FAIL",
                    label,
                    "PASS" if passed else "RED",
                    "PASS" if want_pass else "RED",
                    "" if want_arm is None else " on arm %d" % want_arm,
                )
            )
            if not hit:
                print("\n".join("        " + line for line in lines))
                ok = False

        judge("unmutated fixture", json.loads(json.dumps(good)), True)

        m = json.loads(json.dumps(good))
        m["fields"][1]["addr"] = "0x00002004"  # one address shifted by 4
        judge("address shifted by 4", m, False, want_arm=2)

        m = json.loads(json.dumps(good))
        m["fields"][2]["bits"] = "0x00000008"  # one value flipped
        judge("value flipped", m, False, want_arm=3)

        m = json.loads(json.dumps(good))
        del m["fields"][0]  # one row dropped
        judge("row dropped", m, False, want_arm=2)

        m = json.loads(json.dumps(good))
        m["provenance"]["body_sha256"] = ""  # the stamp blanked
        judge("provenance stamp blanked", m, False, want_arm=4)

    for path, digest in real_before:
        if hashlib.sha256(open(path, "rb").read()).hexdigest() != digest:
            print("FAIL: the selftest modified %s -- it must only touch its temp dir" % path)
            ok = False
    print("check_net_session_addrs --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--rederive", action="store_true", help="rewrite the committed extract")
    ap.add_argument("--check", action="store_true", help="the default: judge the committed extract")
    ap.add_argument("--selftest", action="store_true", help="mutation-test the four arms")
    ap.add_argument("--exe", default=None, help="the retail EN exe (default: POLYGON_CLEAN/mh.exe)")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    exe = args.exe or default_exe()
    if args.rederive:
        if not os.path.exists(exe):
            print("FAIL: --rederive needs the retail exe; nothing at %s" % exe)
            return 1
        extract = rederive(exe)
        bad = extract["classification"]["unclassified"]
        if bad:
            print("REFUSED: the body does not classify, so the extraction may be PARTIAL:")
            for line in bad:
                print("    " + line)
            print("A partial table is right about every row it has and silent about the one it")
            print("dropped -- that is a wrong answer, not a smaller one. Nothing was written.")
            return 1
        with open(EXTRACT, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(extract, fh, indent=1)
            fh.write("\n")
        print(
            "wrote %s: %d store(s) -> %d field(s) from %s"
            % (
                os.path.relpath(EXTRACT, REPO),
                len(extract["stores"]),
                len(extract["fields"]),
                os.path.basename(exe),
            )
        )
        return 0
    return run_check(exe)


if __name__ == "__main__":
    sys.exit(main())
