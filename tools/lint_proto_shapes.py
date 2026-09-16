#!/usr/bin/env python3
"""lint_proto_shapes.py -- one callee, ONE committed shape: mh_calls.gen.h vs mh_export.gen.h.

THE FAILURE THIS EXISTS TO PREVENT. Two generated headers describe the same original function from
different inputs -- `inline <ret> <name>(...)` in mh_calls.gen.h (from dll_call_protos.json via
gen_dll_calls.py) and `using sig_<name> = <ret>(__cdecl *)(...)` in mh_export.gen.h (via
gen_dll_exports.py). Nothing made them agree, so they drifted, and the drift is INVISIBLE until some
consumer has to satisfy both at once:

  * a calls-struct MEMBER's type comes from mh_calls.gen.h;
  * a promotion/shadow arm is pinned to sig_<fn> by its MH_*_REPLACE macro.

For a row where they disagree, no single symbol can be bound to the member -- the wrapper fits or
the arm fits, never both -- and C++ will not convert between the two pointer types silently. The
const class surfaced as two compile errors in rx_dispatch.cpp (LIB-REBIND slice 2); the out-pointer
WIDTH class surfaced a day later as four unresolved externals at link when tact left the rebind's
deferred set. Both were found by a third consumer tripping over them, which is exactly the accident
this gate removes: it compares the two headers DIRECTLY and needs no consumer to exist.

ONE DIFFERENCE IS EXPECTED AND IS NOT DRIFT -- measured 2026-09-04, and it is the whole reason
LIB-CONSTSIG's premise ("the disagreement is in the generators or their input") does not survive
contact. For a BY-VALUE BLOB parameter the two headers describe two DIRECTIONS of the same argument
and correctly disagree:

  * CALL direction (mh_calls.gen.h): the caller hands a pointer to its own buffer and the thunk
    copies it, so the callee cannot affect the caller's copy -- `const` is right.
  * ENTRY direction (sig_<fn>): our exported body receives the pushed copy, which it OWNS and may
    legitimately mutate, and several originals do -- non-const is right.

gen_dll_exports.callee_ptype() strips the const for exactly these params, deliberately and with that
reasoning in its docstring. So this gate encodes the rule instead of reporting it: a const-only
difference on a `slot.kind == "blob"` param is EXPECTED. Anything else is drift. All 11 differences
in the tree when this gate was written were of the expected kind.

WHAT IT DOES NOT DO. It does not decide whether a given body may be bound to the const member --
that is a claim about what the ORIGINAL does with its copy, evidenced from the disassembly and
recorded per row (LIB-CONSTSIG's done_when). This gate only refuses to let a description drift.

    python tools/lint_proto_shapes.py            # report
    python tools/lint_proto_shapes.py --check    # exit 1 on any unwaived disagreement
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MH = os.path.join(REPO, "src", "mh_dll", "mh")
CALLS_H = os.path.join(MH, "addr", "mh_calls.gen.h")
EXPORT_H = os.path.join(MH, "addr", "mh_export.gen.h")
WAIVERS = os.path.join(REPO, "tools", "data", "proto_shape_waivers.json")

SIG_DECL = re.compile(r"^using\s+sig_(\w+)\s*=\s*(.+?)\s*\(__cdecl \*\)\((.*?)\);", re.M)
# ret may end in `*`, which a naive `(.+?)\s+(\w+)\(` drops -- same trap gen_libmh_rebind documents.
CALL_DECL = re.compile(r"^inline\s+(.+?)\s*\b([A-Za-z_]\w*)\s*\((.*?)\)\s*\{", re.M)


def norm_type(s):
    """Normalise for comparison. CONST IS SIGNIFICANT and deliberately kept -- stripping it is what
    hid the first four rows until a compile error found them."""
    s = s.replace("::", "")
    s = re.sub(r"\s*\*\s*", "*", s)
    return re.sub(r"\s+", " ", s).strip()


def param_types(params):
    params = params.strip()
    if not params or params == "void":
        return []
    out = []
    for p in params.split(","):
        p = p.strip()
        m = re.search(r"([A-Za-z_]\w*)\s*$", p)
        ty = p[: m.start()].rstrip() if m and p[: m.start()].strip() else p
        out.append(norm_type(ty))
    return out


def classify(a, b):
    """Name the disagreement. `const` is a claim about who owns the buffer; width is a claim about
    what is in it. They need different dispositions, so they are never merged."""
    if a.replace("const ", "") == b.replace("const ", ""):
        return "const"
    if a.rstrip("*") in ("void", "const void") or b.rstrip("*") in ("void", "const void"):
        return "void-vs-typed"
    return "other"


def blob_params(protos_path):
    """{cident: {param index that is a by-value blob}} -- the set where a const difference is the
    documented rule rather than drift."""
    out = {}
    d = json.load(open(protos_path, encoding="utf-8"))
    for f in d["functions"]:
        idx = {
            i for i, p in enumerate(f.get("params", [])) if p.get("slot", {}).get("kind") == "blob"
        }
        if idx:
            out[f.get("cident")] = idx
    return out


def compare(calls, sigs, blobs):
    """The whole rule, over dicts, so --selftest can drive it without the tree."""
    findings, expected = [], 0
    for name in sorted(set(calls) & set(sigs)):
        cret, cps = calls[name]
        sret, sps = sigs[name]
        diffs = []
        if cret != sret:
            diffs.append(("return", cret, sret))
        if len(cps) == len(sps):
            for i, (a, b) in enumerate(zip(cps, sps)):
                if a != b:
                    diffs.append((f"param{i}", a, b))
        else:
            diffs.append(("arity", str(len(cps)), str(len(sps))))
        # Drop the EXPECTED call-vs-entry const difference on by-value blob params (see the banner).
        real = []
        for where, a, b in diffs:
            m = re.fullmatch(r"param(\d+)", where)
            if (
                m
                and classify(a, b) == "const"
                and int(m.group(1)) in blobs.get(name, set())
                and a.startswith("const ")
                and not b.startswith("const ")
            ):
                continue
            real.append((where, a, b))
        if real:
            findings.append({"name": name, "diffs": real})
        elif diffs:
            expected += 1
    return findings, expected


def selftest():
    """THE NEGATIVE CASES. A gate whose only evidence is that it passes has not been shown to be
    able to fail -- and this one was BORN passing, because its first version flagged the documented
    blob rule as drift and its second stopped flagging anything at all. Both directions are pinned
    here."""
    T = lambda *p: ("int32_t", list(p))  # noqa: E731
    cases = [
        # (label, calls, sigs, blobs, expect_findings, expect_expected)
        (
            "blob const difference is the RULE, not drift",
            {"f": T("const void*")},
            {"f": T("void*")},
            {"f": {0}},
            0,
            1,
        ),
        (
            "the same difference on a NON-blob param IS drift",
            {"f": T("const void*")},
            {"f": T("void*")},
            {},
            1,
            0,
        ),
        (
            "width difference is drift even on a blob",
            {"f": T("const int32_t*")},
            {"f": T("void*")},
            {"f": {0}},
            1,
            0,
        ),
        (
            "const on the ENTRY side only is drift (the rule is one-directional)",
            {"f": T("void*")},
            {"f": T("const void*")},
            {"f": {0}},
            1,
            0,
        ),
        (
            "return-type difference is drift",
            {"f": ("int32_t", [])},
            {"f": ("void", [])},
            {"f": {0}},
            1,
            0,
        ),
        (
            "arity difference is drift",
            {"f": T("int32_t")},
            {"f": ("int32_t", [])},
            {"f": {0}},
            1,
            0,
        ),
        ("agreement is silence", {"f": T("void*")}, {"f": T("void*")}, {"f": {0}}, 0, 0),
    ]
    bad = 0
    for label, c, s, b, want_f, want_e in cases:
        f, e = compare(c, s, b)
        ok = len(f) == want_f and e == want_e
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")
        if not ok:
            bad += 1
            print(f"        got findings={len(f)} expected={e}, wanted {want_f}/{want_e}")
    print(f"[proto-shapes] selftest: {len(cases) - bad}/{len(cases)} passed")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="drive the rule with synthetic drift")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    calls = {}
    for ret, name, params in CALL_DECL.findall(open(CALLS_H, encoding="utf-8").read()):
        calls[name] = (norm_type(ret), param_types(params))
    sigs = {}
    for name, ret, params in SIG_DECL.findall(open(EXPORT_H, encoding="utf-8").read()):
        sigs[name] = (norm_type(ret), param_types(params))

    waived = {}
    if os.path.exists(WAIVERS):
        waived = json.load(open(WAIVERS, encoding="utf-8")).get("waivers", {})
    blobs = blob_params(os.path.join(REPO, "tools", "data", "dll_call_protos.json"))

    both = sorted(set(calls) & set(sigs))
    findings, expected = compare(calls, sigs, blobs)
    for f in findings:
        f["waived"] = f["name"] in waived

    unwaived = [f for f in findings if not f["waived"]]
    by_class = collections.Counter(
        classify(d[1], d[2]) for f in findings for d in f["diffs"] if d[0] != "arity"
    )

    print(f"[proto-shapes] {len(both)} callees described by BOTH headers")
    print(f"[proto-shapes] {expected} expected call-vs-entry const difference(s) on by-value blobs")
    print(f"[proto-shapes] disagreements: {len(findings)} ({len(unwaived)} unwaived)")
    for cls, n in sorted(by_class.items()):
        print(f"    {cls:<14} {n}")
    for f in findings:
        tag = " (waived)" if f["waived"] else ""
        print(f"  {f['name']}{tag}")
        for where, a, b in f["diffs"]:
            print(f"      {where:<8} calls={a!r:<34} export={b!r}")

    if args.check and unwaived:
        print(
            f"[proto-shapes] FAIL: {len(unwaived)} callee(s) carry two committed shapes. Fix the "
            "generators, or record a per-row waiver with the disassembly evidence in "
            f"{os.path.relpath(WAIVERS, REPO)}.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
