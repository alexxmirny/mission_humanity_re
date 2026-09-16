#!/usr/bin/env python3
"""report_inbound_liveness.py -- which libmh inbound entries a run actually ENTERED (LIB-SPINE-API).

THE QUESTION. `gen_libmh_inbound.py` answers "does each hosted seam ROUTE through its entry" by
reading source. That is a static property and it is the one that can be gated. It does NOT answer
"did a run reach the entry", and those differ for an ordinary reason: a routed seam sits behind game
code that a given scenario may never execute. So the routing census is the gate and this is the
READING -- the brokered configuration's evidence that the inbound contract was exercised on the real
game rather than only compiled into it.

THIS IS DELIBERATELY NOT A GATE, and the reason is structural rather than caution. The entered set
is a property of WHICH SCENARIOS RAN, not of the tree: `libmh_enter_tactical_mission` is entered by
a run that enters a tactical mission and by no other, so a threshold here would fail on a narrowed
gate and pass on a widened one while the code stayed identical. A number that moves when nothing
moved is not a gate; it is a reading, and readings belong in a report. `--min-entered` exists for a
caller that knows its own scenario set and wants to assert a floor over it.

THE SOURCE is the one-shot line every entry emits on its first call:

    ; [libmh_in] libmh_post_event call #1

state/host_in.cpp's `enter()` writes it for the 45 inbound entries; state/spine.cpp's `in_live()`
writes the identical line for the replay spine (libmh.h's own entries are outside the inbound
ENTRIES[] table, so without that they would be a silent hole in this answer -- and `libmh_sim_step`,
the entry the brokered configuration is built around, is one of them).

The entry ROSTER is derived from the same adjudication the census uses, never typed here.

    python tools/report_inbound_liveness.py                     # every log under tmp/
    python tools/report_inbound_liveness.py tmp/gate tmp/ab     # named roots
    python tools/report_inbound_liveness.py --json out.json
    python tools/report_inbound_liveness.py --since 2026-09-11  # mtime floor, ISO date
"""

from __future__ import annotations

import argparse
import datetime
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import gen_libmh_inbound as G  # noqa: E402  -- the roster's single source

CALL1_RE = re.compile(r"\[libmh_in\] (libmh_[A-Za-z0-9_]+) call #1")
ARM_RE = re.compile(r"\[libmh_in\] inbound surface: open=(\d+), version 0x([0-9A-Fa-f]+)")
TRAP_RE = re.compile(r"\[libmh_in\] TRAP ([^:]+): (\S+)")


def roster():
    """(inbound_entries, spine_entries) -- derived live, exactly as the census derives them."""
    rows, _unc, _f = G.adjudicate(G.candidates())
    spine = set(G.spine_entries(rows))
    # libmh_state_hash is a spine entry that serves no inbound ROW (hosted the lockstep hash is the
    # determinism harness's, not original code's -- so it is not a crossing and has no row). It still
    # carries a liveness line, so name it here or the reading would silently omit an entry that CAN
    # report.
    spine.add("libmh_state_hash")
    return set(G.entry_names(rows)), spine


def scan(roots, since=None):
    seen, traps, arms, files = {}, [], [], 0
    for root in roots:
        pat = os.path.join(root, "**", "*.log") if os.path.isdir(root) else root
        for p in glob.glob(pat, recursive=True):
            if since is not None:
                try:
                    if os.path.getmtime(p) < since:
                        continue
                except OSError:
                    continue
            try:
                text = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if "[libmh_in]" not in text:
                continue
            files += 1
            where = os.path.relpath(p, REPO).replace(os.sep, "/")
            for m in CALL1_RE.finditer(text):
                seen.setdefault(m.group(1), set()).add(where)
            for m in ARM_RE.finditer(text):
                arms.append((where, int(m.group(1)), m.group(2)))
            for m in TRAP_RE.finditer(text):
                traps.append((where, m.group(1), m.group(2)))
    return seen, traps, arms, files


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("roots", nargs="*", default=None, help="log dirs or files (default: tmp/)")
    ap.add_argument("--since", help="ignore logs older than this ISO date (YYYY-MM-DD)")
    ap.add_argument("--json", help="also write the reading to this path")
    ap.add_argument(
        "--min-entered",
        type=int,
        default=None,
        help="exit 1 if fewer than N entries were entered -- for a caller that knows its own "
        "scenario set; there is no default, see the banner",
    )
    args = ap.parse_args()

    roots = args.roots or [os.path.join(REPO, "tmp")]
    since = None
    if args.since:
        since = datetime.datetime.strptime(args.since, "%Y-%m-%d").timestamp()

    entries, spine = roster()
    seen, traps, arms, files = scan(roots, since)

    ent_in = sorted(entries & set(seen))
    ent_sp = sorted(spine & set(seen))
    never_in = sorted(entries - set(seen))
    never_sp = sorted(spine - set(seen))
    stray = sorted(set(seen) - entries - spine)

    print("libmh INBOUND ENTRY LIVENESS -- which entries a run entered (a reading, not a gate)")
    print("  logs with [libmh_in] lines   %d" % files)
    print(
        "  inbound entries              %d  (%d entered, %d never)"
        % (len(entries), len(ent_in), len(never_in))
    )
    print(
        "  spine entries                %d  (%d entered, %d never)"
        % (len(spine), len(ent_sp), len(never_sp))
    )
    if arms:
        vers = sorted({v for _w, _o, v in arms})
        closed = [w for w, o, _v in arms if o != 1]
        print(
            "  arm lines                    %d  versions %s"
            % (len(arms), ", ".join("0x" + v for v in vers))
        )
        if closed:
            print(
                "  *** CLOSED SURFACE in %d arm line(s): %s" % (len(closed), ", ".join(closed[:3]))
            )
    if traps:
        print("  *** TRAPS                    %d  %s" % (len(traps), traps[0][1:]))
    for title, names in (
        ("NEVER ENTERED (inbound)", never_in),
        ("NEVER ENTERED (spine)", never_sp),
    ):
        if names:
            print("  -- %s --" % title)
            for n in names:
                print("     %s" % n)
    if ent_sp:
        print("  -- ENTERED (spine) --")
        for n in ent_sp:
            print("     %-40s %s" % (n, sorted(seen[n])[0]))
    if stray:
        # A line naming something the roster does not know: either a new entry nobody derived, or a
        # renamed one. Both read as fine in a log and as nothing in a summary, so say it.
        print("  -- UNKNOWN NAME in a liveness line (roster drift?) --")
        for n in stray:
            print("     %s" % n)

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(
                {
                    "files": files,
                    "entered_inbound": ent_in,
                    "never_inbound": never_in,
                    "entered_spine": ent_sp,
                    "never_spine": never_sp,
                    "unknown": stray,
                    "traps": traps,
                },
                fh,
                indent=1,
            )
        print("wrote %s" % args.json)

    if args.min_entered is not None and len(ent_in) + len(ent_sp) < args.min_entered:
        print("FAIL: %d entered, floor is %d" % (len(ent_in) + len(ent_sp), args.min_entered))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
