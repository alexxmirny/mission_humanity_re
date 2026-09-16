#!/usr/bin/env python3
"""C8-d/C8's direct-call rule, as a lint that FAILS rather than as prose.

THE RULE. Within `libmh/lockstep/` and `mh/net/` -- and nowhere else -- no call from our code to our
code may go through an original entry address. An intra-closure edge is a plain C++ call, or (in the
diagnostic build) it goes through `MH_INTERNAL_CALL`, which is the same thing with a switch on it.

WHY IT NEEDS A LINT AND NOT A PARAGRAPH. The violation is a single token: writing
`mh::call::llm_net_lockstep_dispatch` instead of `MH_INTERNAL_CALL(llm_net_lockstep_dispatch, ...)`
in a `live_*_calls()` table. It compiles, it runs, and it produces IDENTICAL BEHAVIOUR in the
shipping config -- the entry it routes through is our own replace thunk. Nothing observable goes
wrong until someone builds the entry-routed arm, or reads a seam liveness counter, or tries to bisect
a red run. A rule whose breach is invisible for months is a rule that needs a machine.

HOW IT DECIDES WHAT IS "OURS". Not a hand-maintained list: the set of functions we own is derived
from the `MH_EXPORT_REPLACE(<game fn>, <our body>)` macros across the whole DLL tree. Add a seam and
this lint starts guarding its edges the same day, with no second list to forget.

THE EXCEPTIONS ARE NAMED, WITH THEIR REASONS, AND THERE ARE FIVE:
  * `llm_strat_time_tick` -- excepted by the user's decision recorded in C8. It
    is C2 class LIVE (mixed): its clock recompute and FPS ring are unconditional single-player code,
    so it stays promoted but REVERTABLE, and a revertable seam's callers must keep consulting the
    entry or the revert does not reach them.
  * `llm_strat_sim_step` -- the DETERMINISM HARNESS owns sim_step's entry (its per-step golden-hash +
    the SIM-CUT exactly-once probe live in the detour), so sim_step's promotion is harness-mediated
    (C6, RI-SIM/SIM1F) rather than an entry install. The lockstep turn-engine call must stay
    entry-routed: a direct call would bypass on_sim_step and void every determinism run. Same class
    as time_tick, one instrument over.
  * `llm_strat_order_schedule`, `llm_strat_order_release_due`, `llm_strat_order_pending_enqueue` --
    these cross into mh::orders, and C8's rule is deliberately scoped to net + lockstep. Six of
    mh::orders' nine seams are live single-player logic with a real original to compare against, so
    that closure keeps entry routing and its own revertable knob.

Exit 0 if clean, 1 with the offending file:line and the fix if not.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(REPO, "src", "mh_dll", "mh")

# The closure the rule applies to. NOT the whole tree: libmh/orders/ legitimately calls net entries, and
# a lint that flagged it would be arguing with the scope decision rather than enforcing it.
SCOPED_DIRS = ("lockstep", "net")

EXCEPT_ENTRY_ROUTED = {
    "llm_strat_time_tick": "excepted by decision -- C2 LIVE (mixed), stays promoted but REVERTABLE, "
    "so its callers must keep consulting the entry",
    "llm_strat_sim_step": "excepted -- the DETERMINISM HARNESS owns sim_step's entry (its per-step "
    "golden-hash + the SIM-CUT exactly-once probe live in sim_step_detour), so its promotion is "
    "harness-mediated (C6, MH_Harness_RebindSimStep). The lockstep turn-engine's call MUST stay "
    "entry-routed: routing it direct to mh::sim::sim_step would bypass on_sim_step (no per-step hash, "
    "no SIM-CUT probe) and void every determinism run. Same class as time_tick, one instrument over.",
    "time_GetCurrentTime": "excepted at LIB-TRANS-P (2026-09-02) -- its ENTRY @0x00427616 is the "
    "determinism harness's pin_wallclock INSTRUMENT POINT (whole-body replaced by the deterministic "
    "counter at MH_Harness_Init). The wave-2 direct rebind read the REAL clock underneath the pin, "
    "freezing every pinned run's game clock at the pin base (measured on the -P's first SP-oracle "
    "run). Same doctrine as sim_step/time_tick above: the instrument owns the entry, callers reach "
    "the function through it. Promotion still reaches ours in unharnessed runs -- sim_lt_promote "
    "installs our body AT the entry and YIELDS to the pin when the harness declares it.",
    "llm_strat_order_schedule": "crosses into mh::orders, which is outside the rule's scope",
    "llm_strat_order_release_due": "crosses into mh::orders, which is outside the rule's scope",
    "llm_strat_order_pending_enqueue": "crosses into mh::orders, which is outside the rule's scope",
    # The four below cross into mh::sim's RESIDUAL domain (SIM-RESID-P, 2026-09-01): each became
    # "ours" the day resid_promote.cpp's whole-domain MH_EXPORT_REPLACE block landed, but the
    # implementation choice is REVERTABLE and entry routing is what makes the reversal reach these
    # callers (brokered: the E9 lands the call in ours; `[config] mode=original`: the original runs).
    # A direct call would hard-wire our body regardless of the configuration, silently breaking the
    # rollback contract the item's done_when requires stay tested.
    # Same class as the mh::orders trio above: a cross-closure edge into a revertable domain.
    # (It was `[promote] sim_resid` until fork F2E; the knob collapsed into the selector, and the
    # rule is unchanged because it was never about WHICH knob -- only that one exists.)
    "llm_strat_time_resync_and_tick": "crosses into mh::sim's resid domain -- revertable, "
    "the selector must keep governing which body runs (SIM-RESID-P)",
    "llm_strat_advisor_tick": "crosses into mh::sim's resid domain -- revertable, "
    "the selector must keep governing which body runs (SIM-RESID-P)",
    "llm_strat_invasion_due_check": "crosses into mh::sim's resid domain -- revertable, "
    "the selector must keep governing which body runs (SIM-RESID-P)",
    "llm_strat_clock_resync_units_and_buildings": "crosses into mh::sim's resid domain -- "
    "revertable, the selector must keep governing which body runs (SIM-RESID-P)",
}


def strip_comments_and_strings(text):
    """Blank out //, /* */ and "..." so a rule is never broken by PROSE ABOUT the rule.

    Not fussiness: tx_emit.h and tx_emit_ctrl.h both carry long superseded notes that name
    `mh::call::llm_net_lockstep_commit_horizon` in running text, deliberately kept because the
    argument was right and only its premise expired. A naive grep reports those as violations, and a
    lint that cries wolf about its own documentation gets switched off.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def ours():
    """Every function some MH_EXPORT_REPLACE claims, derived from the tree rather than listed."""
    found = set()
    for _tree, root, _dirs, files in _dllsrc.walk():
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                for line in f:
                    # SKIP `#define` LINES. mh_export.gen.h defines the macro itself as
                    # `#define MH_EXPORT_REPLACE(FN, IMPL)`, which otherwise puts the literal token
                    # "FN" into the owned set -- and then the macro DEFINITION in internal_call.h,
                    # `... &::mh::call::FN ...`, reports as a violation of the rule it implements.
                    # That was this lint's first run: a false positive against its own machinery.
                    if line.lstrip().startswith("#define"):
                        continue
                    for m in re.finditer(r"MH_EXPORT_REPLACE\(\s*(\w+)\s*,", line):
                        found.add(m.group(1))
    return found


def main():
    owned = ours()
    if not owned:
        print("lint_internal_edges: FAIL -- found no MH_EXPORT_REPLACE at all, so this lint would")
        print("  pass vacuously. That is a broken scan, not a clean tree.")
        return 1

    bad = []
    scanned = 0
    for tree, d in [(t, d) for t in _dllsrc.ROOTS for d in SCOPED_DIRS]:
        base = os.path.join(tree, d)
        if not os.path.isdir(base):
            continue
        for root, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(root, fn)
                scanned += 1
                with open(path, encoding="utf-8", errors="replace") as f:
                    code = strip_comments_and_strings(f.read())
                for ln, line in enumerate(code.splitlines(), 1):
                    # A macro BODY is a template, not a call site -- the substitution decides where
                    # it lands, and MH_INTERNAL_CALL's own definition necessarily names both arms.
                    if line.lstrip().startswith("#define") or line.rstrip().endswith("\\"):
                        continue
                    for m in re.finditer(r"mh::call::(\w+)", line):
                        callee = m.group(1)
                        if callee not in owned or callee in EXCEPT_ENTRY_ROUTED:
                            continue
                        bad.append((os.path.relpath(path, REPO), ln, callee))

    if bad:
        print("lint_internal_edges: FAIL -- %d entry-routed intra-closure call(s)" % len(bad))
        print()
        for path, ln, callee in bad:
            print("  %s:%d" % (path, ln))
            print(
                "      mh::call::%s is a function WE OWN (some MH_EXPORT_REPLACE claims it), and"
                % callee
            )
            print("      this file is inside the net/lockstep closure, so the edge must be direct.")
            print("      Fix: MH_INTERNAL_CALL(%s, mh::lockstep::<our production entry>)" % callee)
            print(
                "      -- see libmh/lockstep/internal_call.h. If the edge genuinely must stay routed"
            )
            print(
                "      through the original entry, it needs a NAMED exception in this file with a"
            )
            print("      reason, not a silent one.")
        return 1

    print(
        "lint_internal_edges: PASS (%d file(s) in %s; %d owned functions; %d named exception(s))"
        % (scanned, "/".join(SCOPED_DIRS), len(owned), len(EXCEPT_ENTRY_ROUTED))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
