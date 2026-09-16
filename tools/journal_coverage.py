#!/usr/bin/env python3
"""journal_coverage.py -- what a recorded tactical session CAN and CANNOT test.

WHY THIS EXISTS. On 2026-09-04 a differential gate built on `poz1-combat.journal` was described as
mutation-proven, then measured to PASS with a real, live defect present (an
inverted branch in llm_tact_unit_stand_tick, which left a kneeling unit unable to stand). The reason
was not subtle and was sitting in the file the whole time: that recording contains five KNEEL orders
and ZERO STAND orders. There was no stand in the session to break.

Nothing in the tree could say that. The journal's coverage was a property nobody had ever computed,
so a gate's teeth were argued from its wall-clock cost and its pass/fail history instead of from
what its input actually exercises. A recording is a TEST FIXTURE; a fixture that cannot reach a
behaviour cannot defend it, and that is a mechanical fact about the file, not a judgement call.

WHAT IT MEASURES, and the limits are the point:

  * ORDER-OP coverage -- which of the command opcodes the game's tick dispatch handles appear in the
    recording, and which do not. This is the level that answers "can this journal test STAND".
    Reported through BOTH instruments, `seam N / watch N`, and the split is worth reading. `seam` is
    the E/G trampolines on the original order entries, which go blind the moment a promoted body
    calls its own sibling (zero E and zero G under `[promote] tact_frame=1`, measured on both
    recorded sessions). `watch` is the hook-free `Q` record: the command queue polled per frame, so
    it sees an order whoever issued it, and it also sees ops 2 and 6, which short-circuit past the
    queue into the immediate records and which the E recorder drops on purpose. A journal recorded
    before the watch existed has an empty `watch` column and says so.
  * DIRECT-WRITE coverage -- the four fields no opcode exists for (active_gun, squad_group_id,
    def_stat, SELECTED), which the recorder journals separately.
  * INPUT shape -- mouse/key/cursor record counts and the frame span, so a journal that drives
    nothing is distinguishable from one that drives a lot.

WHAT IT DOES NOT MEASURE, and this was MEASURED rather than guessed: this is COMMAND coverage, not
EXECUTION coverage, and the gap between them is not small. A journal recorded 2026-09-04 that DOES
issue op 5 (STAND) still does not let `--tact-equiv` catch the G115 stand_tick inversion -- because
in ship config OUR stand_tick body never runs at all (entry probe: zero calls). With tact_frame
unpromoted the tick chain is the ORIGINAL unit_weapons_tick/unit_move_tick calling the ORIGINAL
stand_tick, and a rebind only redirects OUR call sites. The command was covered; the code was not
reached. That is G113 at row level.

So op coverage is NECESSARY and NOT SUFFICIENT, and it is also not the last word on what a fixture
reaches. poz1_combat issues ZERO stand orders and still exercises llm_tact_unit_stand_tick to 89% of
its lines, because llm_tact_unit_move_tick drives that body and the journal has 128 moves -- once the
promotion puts our code on the path, a body is reachable through callers the command list never
names. Use this tool to REJECT a fixture that provably cannot issue a command, never to conclude that
the code behind that command is untested; only execution coverage (tools/coverage.py) answers that. The missing instrument is per-body EXECUTION
coverage -- which of the 98 verified tact rows a given scenario+configuration actually enters --
which needs a counting variant of the tombstone arming (it already knows the 936 verified ranges and
their domains; it fails fast instead of counting). Branch-level coverage on top of that needs a real
native coverage tool over a non-optimised build. See TACT1-P.

The op vocabulary is DERIVED from harness.cpp's TACT_OPS rather than copied, so an op added to the
workload cannot silently drop out of the coverage report.
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(REPO, "src", "mh_dll", "mh_harness", "harness.cpp")

# Names for the ops the workload can put on the wire. Kept beside the derivation rather than in it:
# the LIST comes from the source, these are only labels for the report.
OP_NAMES = {
    0x01: "MOVE (group)",
    0x02: "ATTACK/AIM",
    0x04: "KNEEL",
    0x05: "STAND",
    0x06: "FACE/TURN",
    0x09: "MINE-ARM",
    0x0A: "TELEPORT",
    # THE MARKERS ARE THE OBSERVABLE FORM, and 0x1d/0x1f effectively are not (measured
    # 2026-09-05). A queue never carries 0x1d/0x1f: those are the enqueue's own short-circuit
    # (@0x42b805 -> llm_tact_unit_cmd_stance_on/off) and nothing in the game issues them -- all 35
    # order-seam call sites were enumerated. What a stance change actually looks like in a journal
    # is the WALK/RUN marker sitting in the queue, dispatched by llm_tact_unit_weapons_tick
    # @0x42f836. A full POZ3 replay records 98 x 0x1c and 423 x 0x1e and ZERO of 0x1d/0x1f.
    0x1C: "STANCE walk-marker",
    0x1D: "stance-off short-circuit (never queued)",
    0x1E: "STANCE run-marker",
    0x1F: "stance-on short-circuit (never queued)",
    0x40: "RUN-MODE set",
    0x46: "RESET (queue wipe)",
    0x47: "RUN-MODE scan",
    0x7F: "STOP",
}
FIELD_NAMES = {0: "active_gun", 1: "squad_group_id", 2: "def_stat", 3: "SELECTED"}


def harness_ops():
    """The op vocabulary, read out of harness.cpp's TACT_OPS initialiser."""
    try:
        with open(HARNESS, encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError:
        return None
    m = re.search(r"TACT_OPS\[TACT_OP_COUNT\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        return None
    ops = []
    for tok in m.group(1).replace("\n", " ").split(","):
        tok = tok.strip()
        if tok:
            ops.append(int(tok, 0))
    return sorted(set(ops)) or None


def read_journal(path):
    """(records, span) -- records is a list of (kind, fields[]) for record lines only."""
    recs = []
    span = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            t = ln.strip()
            if not t or t.startswith(";"):
                continue
            parts = t.split()
            if len(parts) < 2 or parts[0] not in ("E", "G", "D", "M", "K", "C", "Q"):
                continue
            try:
                frame = int(parts[1])
            except ValueError:
                continue
            span = max(span, frame)
            recs.append((parts[0], parts))
    return recs, span


def coverage(path):
    recs, span = read_journal(path)
    ops_seen = {}
    q_ops_seen = {}
    fields_seen = {}
    kinds = {}
    for kind, p in recs:
        kinds[kind] = kinds.get(kind, 0) + 1
        try:
            if kind == "E" and len(p) >= 4:
                op = int(p[3])
                ops_seen[op] = ops_seen.get(op, 0) + 1
            elif kind == "G" and len(p) >= 3:
                op = int(p[2])
                ops_seen[op] = ops_seen.get(op, 0) + 1
            elif kind == "Q" and len(p) >= 6:
                # Q <frame> <unit> <slot> <flag> <op> <a0..a3> -- the hook-free queue watch. Counted
                # SEPARATELY from E/G rather than merged, because the two instruments disagree by
                # construction and merging them would hide which one saw what. Q sees ops 2 and 6,
                # which short-circuit past the queue into the immediate records and which the E
                # recorder drops on purpose; E sees the player/engine split, which Q cannot recover.
                op = int(p[5])
                q_ops_seen[op] = q_ops_seen.get(op, 0) + 1
            elif kind == "D" and len(p) >= 4:
                fld = int(p[3])
                fields_seen[fld] = fields_seen.get(fld, 0) + 1
        except ValueError:
            continue
    return {
        "span": span,
        "kinds": kinds,
        "ops": ops_seen,
        "q_ops": q_ops_seen,
        "fields": fields_seen,
    }


def report(path, cov, vocab, verbose=True):
    name = os.path.basename(path)
    # THE UNION IS THE FIXTURE'S REACH, and taking it is not a fudge. The two instruments have
    # complementary blind spots: the E recorder deliberately drops op 6, and both E and G stand on
    # trampolines that a promoted body walks straight past, while the Q watch cannot see an order
    # enqueued and dequeued inside one frame. An op either instrument observed IS in the recording.
    seen = dict(cov["ops"])
    for o, n in cov.get("q_ops", {}).items():
        seen[o] = seen.get(o, 0) + n
    covered = [o for o in vocab if o in seen]
    missing = [o for o in vocab if o not in seen]
    pct = (100.0 * len(covered) / len(vocab)) if vocab else 0.0
    if verbose:
        print("=" * 78)
        print("journal coverage: %s" % name)
        print("=" * 78)
        print(
            "  span      %d frames; %s"
            % (
                cov["span"],
                ", ".join("%s=%d" % (k, v) for k, v in sorted(cov["kinds"].items())),
            )
        )
        print("  ops       %d of %d (%.0f%%)" % (len(covered), len(vocab), pct))
        for op in vocab:
            n, q = cov["ops"].get(op, 0), cov.get("q_ops", {}).get(op, 0)
            print(
                "     %-4s %-22s %s"
                % (
                    "0x%02X" % op,
                    OP_NAMES.get(op, "?"),
                    ("seam %d / watch %d" % (n, q)) if (n or q) else "-- NOT EXERCISED",
                )
            )
        if not cov.get("q_ops"):
            print(
                "  note      no `Q` records -- recorded before the hook-free queue watch existed,"
                " so the `watch` column is empty and ops 2/6 may be under-reported."
            )
        else:
            print(
                "  note      a journal's own `Q` records are what the watch saw AT RECORDING TIME."
                " Files recorded before 2026-09-05 under-report: the watch was owner-0 only and"
                " primed on frame 1, so scripted (alien) orders and the mission's whole initial"
                " command load were invisible. --tact-equiv reads the REPLAY's run log, not this"
                " file, so the gate is unaffected -- but to judge what a fixture REACHES, replay it"
                " and read the run log's `TJ Q` stream."
            )
        extra = sorted(o for o in seen if o not in vocab)
        if extra:
            print(
                "  also      ops outside the workload vocabulary: %s"
                % ", ".join("0x%02X(%d)" % (o, seen[o]) for o in extra)
            )
        print(
            "  direct    %s"
            % (
                ", ".join(
                    "%s=%d" % (FIELD_NAMES.get(f, str(f)), n)
                    for f, n in sorted(cov["fields"].items())
                )
                or "(none)"
            )
        )
        gaps = [OP_NAMES.get(o, "0x%02X" % o) for o in missing if o not in UNREACHABLE_OPS]
        if gaps:
            print("  CANNOT TEST: %s" % ", ".join(gaps))
    return {"covered": covered, "missing": missing, "pct": pct, "seen": seen}


# Ops NO recording can ever contain, so they are excluded from every gap list rather than sitting in
# one forever as unfinished business. 0x1d/0x1f are the enqueue's own short-circuit
# (@0x42b805 -> llm_tact_unit_cmd_stance_on/off) and nothing in the game issues them: all 35
# order-seam call sites were enumerated 2026-09-05 and none passes those constants. The observable
# form of a stance change is the WALK/RUN marker (0x1c/0x1e) sitting in the queue.
UNREACHABLE_OPS = {0x1D: "never queued -- see OP_NAMES", 0x1F: "never queued -- see OP_NAMES"}


def registered_journals():
    """The journals the tactical suite actually gates on, DERIVED from test_ui.py's registry.

    Derived rather than globbed: `tools/uiscripts/journals/` also holds trimmed variants and
    experiments, and a coverage claim about "the fixture set" has to be about the set the gate
    runs."""
    path = os.path.join(REPO, "tools", "test_ui.py")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
    except OSError:
        return []
    m = re.search(r"TACT_SCENARIOS\s*=\s*\[(.*?)\n\]", src, re.S)
    if not m:
        return []
    return [os.path.join(REPO, p) for p in re.findall(r'"journal":\s*"([^"]+)"', m.group(1))]


def report_union(paths, vocab):
    """What the FIXTURE SET as a whole reaches -- the number that answers "what can the suite
    defend", and the one this tool never used to compute.

    WHY IT WAS ADDED (2026-09-05). Asked whether STAND and MINE-ARM had fixtures, I read the
    per-journal `CANNOT TEST` lines and said they had none. Both were covered -- STAND by
    poz-stand-exit, MINE-ARM by poz1-combat -- and the per-journal view had said so all along, on
    two different pages. A per-file report answers "can THIS journal test X"; nobody was asking
    that. Printing the union makes the misread impossible rather than warning against it."""
    who = {}
    for p in paths:
        if not os.path.isfile(p):
            continue
        cov = coverage(p)
        seen = set(cov["ops"]) | set(cov.get("q_ops", {}))
        for op in seen:
            who.setdefault(op, []).append(os.path.basename(p).replace(".journal", ""))
    print("=" * 78)
    print("FIXTURE-SET UNION -- %d registered journal(s)" % len(paths))
    print("=" * 78)
    for op in vocab:
        names = who.get(op)
        if names:
            mark = ", ".join(names)
        elif op in UNREACHABLE_OPS:
            mark = "-- n/a: %s" % UNREACHABLE_OPS[op]
        else:
            mark = "-- NO FIXTURE REACHES IT"
        print("  0x%02X %-22s %s" % (op, OP_NAMES.get(op, "?"), mark))
    real = [o for o in vocab if o not in who and o not in UNREACHABLE_OPS]
    reachable = [o for o in vocab if o not in UNREACHABLE_OPS]
    print(
        "  covered   %d of %d reachable ops (%d excluded as never-queued)"
        % (len(reachable) - len(real), len(reachable), len(UNREACHABLE_OPS))
    )
    if real:
        print(
            "  GAP       %s -- no registered fixture records it"
            % ", ".join("0x%02X %s" % (o, OP_NAMES.get(o, "?")) for o in real)
        )
    return real


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("journals", nargs="*", help="journal files (default: every registered one)")
    ap.add_argument(
        "--require-op",
        action="append",
        default=[],
        help="fail unless this op appears (hex or decimal); repeatable",
    )
    args = ap.parse_args()

    vocab = harness_ops()
    if not vocab:
        print(
            "FAIL: could not derive TACT_OPS from %s -- the vocabulary is not a hand list."
            % HARNESS
        )
        return 2

    # DEFAULT TO THE REGISTERED SET, not to a glob of the directory. The directory also holds
    # trimmed variants and experiments, and counting those toward "the fixture set" inflates a
    # coverage claim with files the gate never runs.
    paths = args.journals or registered_journals()
    if not paths:
        jdir = os.path.join(REPO, "tools", "uiscripts", "journals")
        paths = sorted(os.path.join(jdir, f) for f in os.listdir(jdir) if f.endswith(".journal"))
    rc = 0
    for p in paths:
        if not os.path.isfile(p):
            print("FAIL: no such journal: %s" % p)
            rc = 1
            continue
        cov = coverage(p)
        res = report(p, cov, vocab)
        for want in args.require_op:
            op = int(want, 0)
            if op not in cov["ops"]:
                print(
                    "  FAIL: %s does not exercise op 0x%02X (%s)"
                    % (os.path.basename(p), op, OP_NAMES.get(op, "?"))
                )
                rc = 1
    # ALWAYS LAST, so the number that answers "what can the suite defend" is the one left on screen.
    report_union(paths, vocab)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
