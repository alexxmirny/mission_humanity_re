"""lint_fp_helpers.py -- every helper in mh/fp/ declares its state, and the declaration is CHECKED.

CRT-X87-CPP's done_when asks for something a comment cannot give on its own: that each helper is in
one of two states and that *which one* is READ OFF THE CODE rather than asserted. Prose decays --
this item alone found two helpers whose recorded refusal ("the qword width is PC-dependent above
2^53") was inherited from the shape rather than read off the body, and was wrong for both. So the
two states carry machine-readable markers and this lint holds them to the code:

    FP-CPP-PROVEN: fptest <ID> -- <prose>
        The body must contain NO inline __asm, and <ID> must appear as a word in the fptest source.
        A helper claiming a proof that no test implements is exactly the drift this exists to catch.

    FP-ASM-KEEP: fptest <ID> -- <reason>      (the refusal is PINNED by a case)
    FP-ASM-KEEP: NEVER -- <reason>            (no measurement can ever convert it)
        The body must CONTAIN inline __asm. A pinned refusal's <ID> is checked the same way, so a
        helper cannot record "REFUSED, measured" against a case that has been deleted.

Run standalone or via lint_repo.py. `--selftest` runs the negative cases: an unmarked helper, a
mismatched marker, a dangling case reference and an empty reason must each be caught, because a
checker nobody has seen fail is not evidence.
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FP_DIR = os.path.join(REPO, "src", "mh_dll", "mh", "fp")
FPTEST = os.path.join(REPO, "src", "mh_dll", "mh_nettest", "fp_x87_selftest.cpp")

DEF_RE = re.compile(
    r"^(?:__declspec\(naked\)\s*)?(?:__declspec\(noinline\)\s*)?inline\s+"
    r"[\w:<>*&\s]+?\b(\w+)\s*\([^;{]*\)\s*\{",
    re.M,
)
MIN_REASON = 12


def body_of(src, brace_at):
    depth, i = 0, brace_at
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace_at : i + 1]
        i += 1
    return src[brace_at:]


def leading_comment(src, start):
    """The contiguous // block immediately above a definition."""
    lines = src[:start].rstrip("\n").split("\n")
    out = []
    for ln in reversed(lines):
        if ln.lstrip().startswith("//"):
            out.append(ln)
        else:
            break
    return "\n".join(reversed(out))


def helpers(fp_dir):
    for fname in sorted(os.listdir(fp_dir)):
        if not fname.endswith(".h"):
            continue
        path = os.path.join(fp_dir, fname)
        src = open(path, encoding="utf-8").read()
        for m in DEF_RE.finditer(src):
            name = m.group(1)
            brace = src.index("{", m.end() - 1)
            yield fname, name, leading_comment(src, m.start()), body_of(src, brace)


def check(fp_dir=FP_DIR, fptest=FPTEST):
    problems = []
    test_src = open(fptest, encoding="utf-8").read() if os.path.exists(fptest) else ""
    n_cpp = n_asm = 0
    for fname, name, comment, body in helpers(fp_dir):
        is_asm = "__asm" in body
        cpp = re.search(r"FP-CPP-PROVEN:\s*(.+)", comment)
        keep = re.search(r"FP-ASM-KEEP:\s*(.+)", comment)
        where = "%s:%s" % (fname, name)
        if cpp and keep:
            problems.append("%s carries BOTH markers -- a helper is in one state, not two" % where)
            continue
        if not cpp and not keep:
            problems.append(
                "%s has NO state marker. Add FP-CPP-PROVEN: <fptest case> or "
                "FP-ASM-KEEP: <measured reason>" % where
            )
            continue
        text = (cpp or keep).group(1).strip()
        cid = re.match(r"fptest\s+([A-Za-z][\w-]*)", text)
        if cpp:
            n_cpp += 1
            if is_asm:
                problems.append(
                    "%s is marked FP-CPP-PROVEN but its body still contains inline __asm" % where
                )
            if not cid:
                problems.append(
                    "%s FP-CPP-PROVEN must name its case as 'fptest <ID> -- ...', got %r"
                    % (where, text[:60])
                )
        else:
            n_asm += 1
            if not is_asm:
                problems.append(
                    "%s is marked FP-ASM-KEEP but its body has no inline __asm -- if it became "
                    "C++, it needs a proof and the other marker" % where
                )
            if not cid and not text.startswith("NEVER"):
                problems.append(
                    "%s FP-ASM-KEEP must be 'fptest <ID> -- ...' or 'NEVER -- ...', got %r"
                    % (where, text[:60])
                )
            if len(text) < MIN_REASON:
                problems.append(
                    "%s FP-ASM-KEEP reason is too short to be a reason: %r" % (where, text)
                )
        if cid and test_src and not re.search(r"\b" + re.escape(cid.group(1)) + r"\b", test_src):
            problems.append(
                "%s cites fptest case %r but no such case appears in %s -- a proof nothing implements"
                % (where, cid.group(1), os.path.basename(fptest))
            )
    return problems, n_cpp, n_asm


SELFTEST_CASES = [
    (
        "unmarked helper",
        "namespace mh::fp {\ninline int f(double x) { return (int)x; }\n}\n",
        "NO state marker",
    ),
    (
        "C++ marker on an asm body",
        "namespace mh::fp {\n// FP-CPP-PROVEN: fptest Q -- proven\ninline int f(double x) { __asm { fld x }\nreturn 0; }\n}\n",
        "still contains inline __asm",
    ),
    (
        "asm marker on a C++ body",
        "namespace mh::fp {\n// FP-ASM-KEEP: fptest W -- measured, the exponent range differs\n"
        "inline int f(double x) { return (int)x; }\n}\n",
        "no inline __asm",
    ),
    (
        "dangling case reference",
        "namespace mh::fp {\n// FP-CPP-PROVEN: fptest ZZQQNOTACASE -- x\ninline int f(double x) { return (int)x; }\n}\n",
        "no such case appears",
    ),
    (
        "empty reason",
        "namespace mh::fp {\n// FP-ASM-KEEP: n/a\ninline int f(double x) { __asm { fld x }\nreturn 0; }\n}\n",
        "too short to be a reason",
    ),
    (
        "unstructured asm reason",
        "namespace mh::fp {\n// FP-ASM-KEEP: because it felt safer that way, honestly\n"
        "inline int f(double x) { __asm { fld x }\nreturn 0; }\n}\n",
        "must be 'fptest <ID> -- ...' or 'NEVER -- ...'",
    ),
]


def selftest():
    import shutil
    import tempfile

    ok = True
    tmp = tempfile.mkdtemp(prefix="fphelper_lint_")
    try:
        for label, src, expect in SELFTEST_CASES:
            d = os.path.join(tmp, re.sub(r"\W+", "_", label))
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "probe.h"), "w", encoding="utf-8") as fh:
                fh.write(src)
            problems, _, _ = check(d, FPTEST)
            hit = any(expect in p for p in problems)
            print("  [%s] negative case: %s" % ("ok" if hit else "FAIL", label))
            if not hit:
                ok = False
                print("       expected a problem containing %r, got %r" % (expect, problems))
        # and the positive control: the real tree must pass
        problems, n_cpp, n_asm = check()
        print("  [%s] positive control: the real mh/fp/ tree" % ("ok" if not problems else "FAIL"))
        if problems:
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="run the negative cases")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    problems, n_cpp, n_asm = check()
    for p in problems:
        print("  [FAIL] %s" % p)
    if problems:
        print("lint_fp_helpers: FAIL (%d problem(s))" % len(problems))
        return 1
    print(
        "lint_fp_helpers: %d helper(s) -- %d C++ with a named fptest case, %d assembly with a "
        "recorded reason" % (n_cpp + n_asm, n_cpp, n_asm)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
