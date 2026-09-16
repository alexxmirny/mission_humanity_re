"""scan_pointer_slots.py -- every region libmh binds with a POINTER element type (SB-BIND T3).

THE HAZARD. Most state regions hold values; a few hold POINTERS. `_G_LLM_STRAT_CUR_UNIT` holds a
live `unit*` INTO the `units` region, not a unit. While the host binds the stock bases that is
harmless -- the stored address is the address the pointer means. The moment a host binds a RELOCATED
copy it stops being harmless: the stored pointer still names the abandoned `.bss`, so a read through
it sees stale bytes and a write through it lands in memory nothing else reads.

An earlier HAND COUNT of these slots is what this costs when it goes wrong, and it is also why the list has to be
generated rather than remembered: that count said `_G_LLM_STRAT_CUR_UNIT` is "the only sim-view member that
reads memory during binding", which was true on 2026-08-11 and is wrong by 4x today. A hand list of
this shape rots silently, because nothing about adding a fifth one looks like touching the list.

TWO FORMS, and only the first is a correctness problem:

  *ptr<T *>(RID_X)   DEREFERENCED AT BIND -- the stored pointer is read and handed to a view. These
                     must be TRANSLATED through mh::state::translate(), or carry a recorded reason.
  ptr<T *>(RID_X)    THE SLOT'S ADDRESS -- a `T**` the store writes through. The slot itself follows
                     the registry like any other region; what it points AT is the first form's
                     problem, at the point something dereferences it.

Adjudication lives in tools/data/pointer_slots.json: one entry per (region, form) with a disposition
of `translated` or `safe` plus a reason. `--check` refuses an unadjudicated one, which is what makes
a NEW pointer-valued slot a build failure rather than a surprise under SB-HOSTFREE.

Usage:
  python tools/scan_pointer_slots.py            # the report
  python tools/scan_pointer_slots.py --check    # the gate
  python tools/scan_pointer_slots.py --selftest # prove the scanner still fires
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_ROOT = os.path.join(REPO, "src", "mh_dll", "mh")
ADJUDICATION = os.path.join(REPO, "tools", "data", "pointer_slots.json")

# `ptr<SOMETHING *>(RID_X)` -- the element type ends in a pointer. The leading `*` (dereference at
# the bind site) is captured so the two forms can be told apart.
RE_PTR_SLOT = re.compile(r"(\*?)\s*ptr<\s*([^>]*?\*)\s*>\s*\(\s*(RID_[A-Z0-9_]+)\s*\)")


def strip_comments(text):
    out, in_block = [], False
    for line in text.splitlines():
        buf, i = [], 0
        while i < len(line):
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = len(line)
                    break
                in_block, i = False, j + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block, i = True, i + 2
                continue
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def scan_tree(root=None):
    """[(rid, form, elem_type, repo_rel, line)] -- form is 'deref' or 'slot'."""
    roots = _dllsrc.ROOTS if root is None else (root,)
    found = []
    for dirpath, _dirs, files in _dllsrc.walk_flat(roots):
        for name in sorted(files):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            for i, code in enumerate(strip_comments(text), 1):
                for m in RE_PTR_SLOT.finditer(code):
                    star, elem, rid = m.groups()
                    try:
                        rel = os.path.relpath(path, REPO).replace(os.sep, "/")
                    except ValueError:
                        rel = path.replace(os.sep, "/")
                    found.append((rid, "deref" if star else "slot", elem.strip(), rel, i))
    return found


def load_adjudication():
    try:
        with open(ADJUDICATION, encoding="utf-8") as fh:
            doc = json.load(fh)
        return {k: v for k, v in doc.items() if not k.startswith("_")}
    except (OSError, ValueError):
        return {}


def check(found=None, adj=None):
    """(problems, ok). A (region, form) pair with no recorded disposition is a failure."""
    found = scan_tree() if found is None else found
    adj = load_adjudication() if adj is None else adj
    probs = []
    seen = set()
    for rid, form, elem, rel, ln in found:
        key = "%s:%s" % (rid, form)
        if key in seen:
            continue
        seen.add(key)
        entry = adj.get(key)
        if entry is None:
            probs.append(
                "%s (%s, %s) at %s:%d has NO recorded disposition -- add it to "
                "tools/data/pointer_slots.json as `translated` or `safe` with a reason"
                % (rid, form, elem, rel, ln)
            )
        elif entry.get("disposition") not in ("translated", "safe"):
            probs.append(
                "%s (%s) has disposition %r -- must be 'translated' or 'safe'"
                % (rid, form, entry.get("disposition"))
            )
        elif not entry.get("why"):
            probs.append("%s (%s) has a disposition but no `why`" % (rid, form))
    # A disposition for something that no longer exists is drift in the other direction.
    for key in sorted(set(adj) - seen):
        probs.append(
            "%s is adjudicated but no longer appears in the module tree -- remove it" % key
        )
    return probs, not probs


def report():
    found = scan_tree()
    adj = load_adjudication()
    by_key = {}
    for rid, form, elem, rel, ln in found:
        by_key.setdefault("%s:%s" % (rid, form), []).append((elem, rel, ln))
    deref = sorted(k for k in by_key if k.endswith(":deref"))
    slot = sorted(k for k in by_key if k.endswith(":slot"))
    print("=== pointer-valued region slots (SB-BIND T3) ===")
    print("  DEREFERENCED AT BIND -- these read a stored pointer: %d" % len(deref))
    for k in deref:
        d = adj.get(k, {})
        print("    %-46s %-11s %s" % (k, d.get("disposition", "UNADJUDICATED"), by_key[k][0][1]))
    print("  SLOT ADDRESS ONLY -- a T** the store writes through: %d" % len(slot))
    for k in slot:
        d = adj.get(k, {})
        print("    %-46s %-11s %s" % (k, d.get("disposition", "UNADJUDICATED"), by_key[k][0][1]))
    print("  TOTAL %d distinct (region, form) pair(s) over %d site(s)" % (len(by_key), len(found)))
    return 0


def selftest():
    fails = 0
    fake_deref = [("RID_X", "deref", "unit *", "a.cpp", 1)]
    probs, ok = check(fake_deref, adj={})
    if ok or "NO recorded disposition" not in probs[0]:
        print("  FAIL: an unadjudicated pointer slot was not refused")
        fails += 1
    else:
        print("  ok: an unadjudicated pointer slot is refused")

    probs, ok = check(fake_deref, adj={"RID_X:deref": {"disposition": "translated", "why": "r"}})
    if not ok:
        print("  FAIL: an adjudicated slot was refused (over-refusal arm)")
        fails += 1
    else:
        print("  ok: an adjudicated slot passes")

    probs, ok = check(fake_deref, adj={"RID_X:deref": {"disposition": "translated"}})
    if ok:
        print("  FAIL: a disposition with no `why` was accepted")
        fails += 1
    else:
        print("  ok: a disposition with no reason is refused")

    probs, ok = check([], adj={"RID_GONE:deref": {"disposition": "safe", "why": "r"}})
    if ok:
        print("  FAIL: a stale adjudication for a vanished slot was accepted")
        fails += 1
    else:
        print("  ok: a stale adjudication is refused (drift in the other direction)")

    # The regex must tell the two forms apart, which is the whole classification.
    hits = [
        m.groups() for m in RE_PTR_SLOT.finditer("a = *ptr<unit *>(RID_A); b = ptr<unit *>(RID_B);")
    ]
    if [h[0] for h in hits] != ["*", ""] or [h[2] for h in hits] != ["RID_A", "RID_B"]:
        print("  FAIL: the deref/slot forms are no longer distinguished")
        fails += 1
    else:
        print("  ok: deref and slot forms are distinguished")

    # A non-pointer element type must NOT match, or every region in the tree lands in the report.
    if RE_PTR_SLOT.search("x = ptr<unit>(RID_UNITS);"):
        print("  FAIL: a value-typed bind was reported as a pointer slot")
        fails += 1
    else:
        print("  ok: a value-typed bind is not a pointer slot")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        print("=== scan_pointer_slots --selftest ===")
        fails = selftest()
        print("%d failure(s)" % fails)
        return 1 if fails else 0
    if args.check:
        probs, ok = check()
        if not ok:
            print("=== scan_pointer_slots: %d unadjudicated pointer slot(s) ===" % len(probs))
            for p in probs:
                print("  %s" % p)
            return 1
        n = len({"%s:%s" % (r, f) for r, f, _e, _p, _l in scan_tree()})
        print("scan_pointer_slots: ok -- all %d pointer-valued region slot(s) adjudicated" % n)
        return 0
    return report()


if __name__ == "__main__":
    sys.exit(main())
