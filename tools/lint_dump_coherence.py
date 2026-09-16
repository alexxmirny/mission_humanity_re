# lint_dump_coherence.py -- the DB dumps of /eng/mh.exe must agree with each other.
#
# THE GAP THIS CLOSES. lint_repo already runs five drift gates (addr / struct / calls / exports /
# shadow) but every one of them checks a GENERATED HEADER against tools/data/dll_call_protos.json.
# Nothing checked the json itself. So a stale dump is not drift to any of them -- it is the input
# they faithfully agree with, and the whole chain goes green while the DB has moved on.
#
# Measured, 2026-08-30: dll_call_protos.json was stale against the DB and regenerating it pulled in
# three renames committed sessions earlier (llm_gfx_sprite_rle_row_skip_hittest,
# llm_mp_netsetup_enter_game_name_screen, llm_mp_create_game_action). It surfaced because a human
# regenerated the file for an unrelated reason, which is not a gate.
#
# WHY THIS IS OFFLINE AND WHY THAT IS ENOUGH. The authoritative answer lives in Ghidra, and
# the Ghidra-side call-proto dump can only run inside it -- so a true DB comparison would put a Ghidra launch in
# the middle of a gate that is meant to be rig-free and sub-second. It is not needed: the DB is
# dumped by SEVERAL scripts (dump_call_protos, dump_functions, dump_addr_index), each refreshed on
# its own schedule, and a rename lands in whichever ran most recently. Disagreement BETWEEN dumps is
# therefore the same evidence as disagreement with the DB, and it is free. The 2026-08-30 case is
# exactly this shape: en_functions.json was refreshed in one pass, dll_call_protos.json was not.
# What it CANNOT see is all three dumps being stale together -- that is what the per-session
# regenerate rule is for; this gate catches the far commoner partial refresh.
#
# Run: python tools/lint_dump_coherence.py [--selftest]

import argparse
import collections
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(REPO, "tools", "data")

# An IMPORT THUNK is a 5- or 6-byte JMP through the IAT that Ghidra names after the imported symbol
# (GetLastError, AVIFileInit, ...) rather than thunk_*. the Ghidra-side call-proto dump skips thunks, so these
# are legitimately absent from the protos dump and the completeness arm must not demand them.
# Bounding by SIZE rather than by an import allowlist keeps the rule mechanical and offline; it errs
# toward a false NEGATIVE (a genuine <=6-byte function would be excused), never a false positive.
THUNK_MAX_BYTES = 6


def load(name):
    with open(os.path.join(DATA, name), encoding="utf-8-sig") as fh:
        return json.load(fh)


def protos_index(protos):
    """{va: [row, ...]} -- a list because a variadic callee gets one row per fixed-arity shape."""
    out = collections.defaultdict(list)
    for row in protos["functions"]:
        out[row["va"].lower()].append(row)
    return out


def reference_index(functions, addr_index):
    """{va: (name, size)} from the two dumps carrying function extents, merged and cross-checked."""
    out, findings = {}, []
    for f in functions["functions"]:
        out[f["entry"].lower()] = (f["name"], f["size"])
    for start, end, name in addr_index["function_extents"]:
        va = "0x" + start.lower()
        size = int(end, 16) - int(start, 16) + 1
        prev = out.get(va)
        if prev is None:
            out[va] = (name, size)
        elif prev[0] != name:
            findings.append(
                "%s: en_functions.json says %r, en_addr_index.json says %r. Two dumps of the same "
                "DB disagree, so at least one is stale -- re-dump both." % (va, prev[0], name)
            )
    return out, findings


def audit(protos=None, functions=None, addr_index=None, varargs=None):
    protos = protos if protos is not None else load("dll_call_protos.json")
    functions = functions if functions is not None else load("en_functions.json")
    addr_index = addr_index if addr_index is not None else load("en_addr_index.json")
    varargs = varargs if varargs is not None else load("varargs_shapes.json")

    ref, findings = reference_index(functions, addr_index)
    idx = protos_index(protos)
    shapes = set(varargs["functions"])

    for va in sorted(idx):
        rows = idx[va]
        got = ref.get(va)
        if got is None:
            findings.append(
                "%s (%s) is in dll_call_protos.json but is not a function in en_functions.json / "
                "en_addr_index.json. A proto for a function that no longer exists at that address "
                "generates a call into nothing." % (va, rows[0]["name"])
            )
            continue
        name, size = got
        for row in rows:
            if row["name"] != name:
                findings.append(
                    "%s: dll_call_protos.json says %r, the function dumps say %r. THIS IS THE "
                    "2026-08-30 CASE -- the protos dump predates a rename; re-run "
                    "mh_dump_call_protos.py." % (va, row["name"], name)
                )
            if row["size"] != size:
                findings.append(
                    "%s (%s): dll_call_protos.json says %d bytes, the function dumps say %d. The "
                    "body moved or was re-bounded, and the committed steal length and entry8 guard "
                    "are derived from it." % (va, name, row["size"], size)
                )
        if len(rows) > 1:
            # The one legal shape of a repeated address: the variadic callee's own row
            # (status "varargs", the uncallable base) plus one row per fixed-arity shape, each
            # carrying `from_varargs_shape` and its own distinct cident (w_sprintf__vs, __vis, ...).
            # The cidents are what the generator emits, so a collision there is a real duplicate
            # definition rather than a shape family.
            base = [r for r in rows if r.get("status") == "varargs"]
            derived = [r for r in rows if r.get("from_varargs_shape")]
            cidents = set(r["cident"] for r in rows)
            if (
                rows[0]["name"] not in shapes
                or len(base) != 1
                or len(derived) != len(rows) - 1
                or len(cidents) != len(rows)
            ):
                findings.append(
                    "%s (%s) has %d rows in dll_call_protos.json. A repeated address is only legal "
                    "for a variadic callee in varargs_shapes.json: exactly one status-varargs base "
                    "row plus one from_varargs_shape row per fixed-arity shape, all with distinct "
                    "cidents. This one is not that." % (va, rows[0]["name"], len(rows))
                )

    for va in sorted(ref):
        name, size = ref[va]
        if va in idx or size <= THUNK_MAX_BYTES:
            continue
        if name.startswith("FUN_") or name.startswith("thunk_"):
            continue  # unnamed, or an explicit thunk -- the Ghidra-side call-proto dump skips both by design
        findings.append(
            "%s (%s, %d B) is a NAMED function with no row in dll_call_protos.json. Either it was "
            "named after the last protos dump -- re-run mh_dump_call_protos.py -- or it is a thunk "
            "shape this gate does not recognise." % (va, name, size)
        )
    return findings


# ---- selftest: every arm must fire on a mutated copy, and none on the clean one -----------------


def _fixture():
    protos = {
        "functions": [
            {"va": "0x00400100", "name": "llm_a", "cident": "llm_a", "size": 40, "status": "ok"},
            {
                "va": "0x00400200",
                "name": "w_sprintf",
                "cident": "w_sprintf",
                "size": 60,
                "status": "varargs",
            },
            {
                "va": "0x00400200",
                "name": "w_sprintf",
                "cident": "w_sprintf__vs",
                "size": 60,
                "status": "ok",
                "from_varargs_shape": "vs",
            },
        ]
    }
    functions = {
        "functions": [
            {"name": "llm_a", "entry": "0x00400100", "end": "0x00400127", "size": 40},
            {"name": "w_sprintf", "entry": "0x00400200", "end": "0x0040023b", "size": 60},
            {"name": "GetLastError", "entry": "0x00400300", "end": "0x00400305", "size": 6},
            {"name": "FUN_00400400", "entry": "0x00400400", "end": "0x00400427", "size": 40},
        ]
    }
    addr_index = {
        "function_extents": [
            ["00400100", "00400127", "llm_a"],
            ["00400200", "0040023b", "w_sprintf"],
            ["00400300", "00400305", "GetLastError"],
            ["00400400", "00400427", "FUN_00400400"],
        ]
    }
    return protos, functions, addr_index, {"functions": {"w_sprintf": {}}}


def _arm_stale_rename(p, f, a, v):
    p["functions"][0]["name"] = "llm_a_old"


def _arm_extent_moved(p, f, a, v):
    p["functions"][0]["size"] = 41


def _arm_dead_address(p, f, a, v):
    p["functions"].append(
        {"va": "0x00400900", "name": "llm_ghost", "cident": "llm_ghost", "size": 12, "status": "ok"}
    )


def _arm_illegal_duplicate(p, f, a, v):
    # a second row at the same address that is NOT a varargs shape -- a real duplicate definition
    p["functions"][2].pop("from_varargs_shape")


def _arm_named_without_proto(p, f, a, v):
    f["functions"].append(
        {"name": "llm_new", "entry": "0x00400500", "end": "0x00400527", "size": 40}
    )


def _arm_dumps_disagree(p, f, a, v):
    a["function_extents"][0][2] = "llm_a_renamed"


ARMS = (
    ("stale rename", _arm_stale_rename),
    ("extent moved", _arm_extent_moved),
    ("proto for a dead address", _arm_dead_address),
    ("illegal duplicate va", _arm_illegal_duplicate),
    ("named function with no proto", _arm_named_without_proto),
    ("the two function dumps disagree", _arm_dumps_disagree),
)


def selftest():
    import copy

    base = _fixture()
    ok = True
    clean = audit(*base)
    if clean:
        print("SELFTEST: the clean fixture reports findings -- the gate over-refuses.")
        for f in clean:
            print("    " + f)
        ok = False
    for label, mutate in ARMS:
        p, f, a, v = copy.deepcopy(base)
        mutate(p, f, a, v)
        if not audit(p, f, a, v):
            print("SELFTEST: the %r arm did NOT fire -- that failure would be invisible." % label)
            ok = False
    print("dump coherence selftest: %s (%d arms)" % ("ok" if ok else "FAILED", len(ARMS)))
    return ok


def main():
    ap = argparse.ArgumentParser(
        description="cross-check the /eng/mh.exe DB dumps against each other"
    )
    ap.add_argument("--selftest", action="store_true", help="prove every arm still fires")
    args = ap.parse_args()
    if args.selftest:
        return 0 if selftest() else 1
    findings = audit()
    if findings:
        print("dump coherence: %d problem(s)" % len(findings))
        for f in findings[:40]:
            print("  " + f)
        if len(findings) > 40:
            print("  ... and %d more" % (len(findings) - 40))
        return 1
    n = len(load("dll_call_protos.json")["functions"])
    print("dump coherence: %d protos rows agree with en_functions.json + en_addr_index.json" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
