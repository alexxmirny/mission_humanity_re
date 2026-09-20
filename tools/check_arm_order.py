#!/usr/bin/env python3
"""check_arm_order.py -- fork F3A: the ARM-LOG ORDER gate.

MH_Seam_Init installs the whole spine in ONE 41-step body, and every refactor of it (F3B's
core/net/UI split, F3C's hook API, F3D's desync move) is a change whose ONLY observable is the
order in which those steps report themselves.  F2 ran that diff BY HAND three times -- rebuild the
pre-change DLL in a worktree, run one scenario on each, eyeball two 1,200-line logs side by side --
and a hand diff is exactly the instrument that goes quietly wrong when the person running it is
tired and the change is the fourth of the session.  Measurement constraint 6 of the F3 plan says
the gate lands BEFORE the split, so this tool exists first and the split is measured by it.

---- WHAT IT GATES, AND WHAT IT DELIBERATELY DOES NOT --------------------------------------------

GATED: the ORDER of the structural arm lines in `mh_net.log` (always) and `mh_harness.log` (when
the harness armed).  Structural = what is left after the bulk-enumerated rows are folded out.

ADVISORY ONLY (ruling Q4, CLOSED at fork F4F): the UI channels -- `mh_video.log`, `mh_input.log`,
`mh_capture.log`, `mh_uidrive.log`.  They are reported as present/absent, and NEVER red.

F3 deferred them with a trigger -- "until per-module logs become load-bearing at F4" -- and the
trigger never fired: F4 shipped four DLLs and ruling Q3 kept the 13 oracle seams in mh.dll, so every
writer of these four is still an mh.dll TU.  F4F closed the deferral on two measured grounds rather
than rolling it forward: none of the four has an arm-window END MARKER, and the largest
(`mh_uidrive.log`) is the `[uitest]` script's own trace -- gating it would gate the SCENARIO, which
is the failure the end-marker refusal below exists to prevent.  The condition that would reopen the
question is a gate now, not prose: `check_instrument_wiring.py` assertion 5 pins the writer set of
all 14 instrument channels and reds by name if a satellite starts writing one of these.  Full
argument: the instrument-channel notes "Ruling Q4 is closed".

NOT GATED, ON PURPOSE: the CONTENTS of the bulk rows.  A brokered boot emits ~1,090 enumerated
lines -- 716 `;   [rebind] <sym> -> OURS`, ~340 `; [promote] <domain>: + <sym>`, the per-domain and
per-skip `; [tombstone]` rows.  Those say WHICH rows armed, which is `check_fork_f2_drop`'s and the
rebind census's question, not this one.  Folded out by PATTERN so a row added tomorrow does not
red this gate.

NOT GATED EITHER: everything after the arm window.  The arm ends at the interlock's own summary
line (`; [interlock] <n> detour install(s) ...`), which is MH_Seam_Init's last report; after it the
log is the SCENARIO talking (lobby walk, first dispatches, `call #1` liveness), whose order is a
property of the UI script and the game, not of the arm.  A missing end marker is a REFUSAL, not a
"read to end of file" -- see the absence discipline below.

---- NORMALIZATION ------------------------------------------------------------------------------

Per line, in this order (the order is load-bearing, see the docstrings on the patterns):

  1. `[HH:MM:SS.mmm] ` timestamp prefix stripped
  2. `0x<hex>`                              -> <X>
  3. a hex RUN (addresses, fingerprints, session ids, keys, handles) -> <X>
  4. any digit run, with or without a word boundary (counts, timings, ports, the lane number
     inside "MHMut61", ids like D14) -> <N>

So a gate run is insensitive to how MANY rows armed, how long the cost probe took, which lane it
ran in and what the session id was -- and sensitive to the SEQUENCE of the steps and to the WORDS
each step reports.  That is the whole design: a refactor that reorders arms is caught, a refactor
that arms the same things in the same order with different counts is not.

---- ABSENCE IS A FAILURE (the instrument-channel notes) ------------------------------------------

A run that never reached the arm and a run that armed perfectly produce the same clean grep.  So
every path here REFUSES rather than passes: no run directory, no `mh_net.log`, an empty log, a log
with no `[config mode=...]` line to pick a baseline by, a log with no end marker, a baseline with
no lines in it, a selftest render that produced too few lines.  None of those is a PASS.

---- MODES --------------------------------------------------------------------------------------

  python tools/check_arm_order.py <run-dir>                 gate against the committed baseline
  python tools/check_arm_order.py <run-dir> --mode original force the baseline

The baseline KEY is `[config] mode` plus three further axes, ALL read off the run's own log and none
passed in: fork F4D's flat `config1` (no libmh.dll in the process), fork F3F's `_nomodule` suffix (no
network transport) and fork F4F's `_netoff` suffix (`[net] enable=0`).  Committed keys today:
`brokered`, `original`, `brokered_nomodule`, `config1`, `brokered_netoff` -- but read
`tools/data/arm_order/`, not this sentence, which has been stale once already.

  python tools/check_arm_order.py --compare <old> <new>     NO-GOLDEN diff of two run dirs
  python tools/check_arm_order.py --update <run-dir> [--recipe "<cmd>"]
                                                            (re)derive a baseline from a live run
  python tools/check_arm_order.py --check-baselines         structural check of the committed files
  python tools/check_arm_order.py <run-dir> --spine-armed   fork F3F: the spine armed NON-ZERO work
  python tools/check_arm_order.py --selftest                planted reorder/insert RED, mask NOT red

`--compare` is the mode F2's hand diff really was: two runs, no golden, "did the arm order move,
and if not, exactly which texts changed".  It is what a refactor commit cites when the baseline
itself is being edited in the same commit (the baseline cannot referee its own edit).
"""

import argparse
import json
import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_DIR = os.path.join(REPO, "tools", "data", "arm_order")

# ---- normalization ------------------------------------------------------------------------------

TS_RE = re.compile(r"^\[\d\d:\d\d:\d\d\.\d\d\d\]\s*")

# `0x...` FIRST, whole: otherwise the digit mask below would eat the "0" and leave `<N>x01070414`.
HEX0X_RE = re.compile(r"0[xX][0-9A-Fa-f]+")

# A hex RUN. Two guards, both learned rather than guessed:
#   * it must contain at least one hex LETTER, so a pure-decimal run (a count, a port, a step
#     budget) never lands in the hex bucket -- if it did, `seed=1362723655` would mask as <X> and a
#     seed that happened to be 7 digits would mask as <N>, i.e. the gate would flake on the VALUE of
#     a field it does not care about.
#   * a letters-only run must be 8+ chars, because "added", "decade", "beef", "face" and "deaf" are
#     English words made entirely of hex digits.  8 letters of pure a-f is not a word, it is
#     "deadbeef".
# Delimited by non-alphanumerics rather than \b so `@0049D8EF:` and `fp=539F...,` both match.
HEXRUN_RE = re.compile(
    r"(?<![0-9A-Za-z])"
    r"(?=[0-9A-Fa-f]*[A-Fa-f])"  # at least one hex letter
    r"(?:[0-9A-Fa-f]{8,}|(?=[0-9A-Fa-f]*[0-9])[0-9A-Fa-f]{4,})"
    r"(?![0-9A-Za-z])"
)

# A WINDOWS ABSOLUTE PATH, masked FIRST and whole (fork F4B). Added because the module bind's
# refusal line names the FULL PATH it tried -- deliberately, and for the reason msvfw32's Rule 2
# gives: exactly one path is tried, so "the module is not here" is a fact rather than a search that
# quietly succeeded elsewhere, and a refusal that does not say WHERE it looked is unactionable. But
# a lane's path contains the lane's NAME, so without this mask the `_nomodule` baseline would be
# pinned to whichever lane folder it happened to be recorded in -- exactly the failure the `MHMut61`
# digit mask exists to prevent, one level up.
#
# `X:\...` is safe to mask greedily because it cannot occur in prose: a drive letter, a colon and a
# backslash together are a path or nothing. Bounded by whitespace and the bracketing characters the
# log lines actually use, so `LoadLibrary(C:\x\y.dll) failed` masks the argument and keeps the rest.
WINPATH_RE = re.compile(r"[A-Za-z]:\\[^\s\"'()]*")

# Digits LAST and with NO word boundary, so the lane number inside `MHMut61` masks too -- that line
# is emitted with the lane in a quoted identifier, and a \b-anchored mask left it literal, which
# would have pinned every baseline to the lane it was recorded in.  Safe to be this greedy only
# because the hex runs are already gone by here.
DEC_RE = re.compile(r"\d+(?:\.\d+)?")


def normalize(line):
    line = TS_RE.sub("", line).rstrip()
    line = WINPATH_RE.sub("<PATH>", line)
    line = HEX0X_RE.sub("<X>", line)
    line = HEXRUN_RE.sub("<X>", line)
    line = DEC_RE.sub("<N>", line)
    return line


# ---- the bulk rows: CONTENTS, not order ---------------------------------------------------------

BULK_RES = [
    # `;   [rebind] cfg_GetAnimTime -> OURS` (716 of them in a brokered boot). The two-token
    # `<sym> -> <target>` shape is what distinguishes a ROW from the `; [rebind] armed N of M
    # row(s) [config mode=...]` HEADLINE, which is structural and stays.
    re.compile(r"^;\s+\[rebind\]\s+\S+\s+->\s+\S+$"),
    # `; [promote] orders_issue: + llm_strat_unit_order_move`
    re.compile(r"^;\s*\[promote\]\s+\w+:\s+\+\s+\S+$"),
    # `; [promote] sim_resid: llm_strat_x call #1 (OURS is live)` -- a per-row liveness row.
    re.compile(r"^;\s*\[promote\]\s+\w+:\s+\S+\s+call #\d+ \(OURS is live\)$"),
    # `; [tombstone] domain orders: 71 armed (...)` / `armed: utils_w_strlen(p)` / `skipped <sym>`
    re.compile(r"^;\s*\[tombstone\]\s+(domain\s+\S+:|armed:|skipped\s)"),
    # `; [libmh_in] libmh_post_event call #1` -- per-entry, and there are 46 entries.
    re.compile(r"^;\s*\[libmh_in\]\s+\S+\s+call #\d+$"),
    # `; [hostevt] first dispatch: channel 1, kind 14`
    re.compile(r"^;\s*\[hostevt\]\s+first dispatch:"),
    # `; [build] mh 0.1.0-rc1+abc12345` -- the release stamp (tracker TL-CI1), written as the very
    # first line of mh_net.log by mh/seams/module_bind.cpp. FOLDED OUT rather than baselined, and
    # this is the one entry here that is folded for a reason other than volume: normalize() exists
    # to make a template independent of the values in a line, and it CANNOT do that to this one.
    # `0.1.0` masks to `<N>.<N>`, `0.1.0-rc1` to `<N>.<N>-rc<N>`, and a short sha that happens to be
    # seven letters survives HEXRUN_RE's eight-character letters-only floor as itself -- so the
    # normalized text of this step would change with the release number and with the commit. A
    # baselined step like that reds the gate on every tagged build, i.e. exactly when the gate most
    # needs to be readable. Its presence is gated where it can be: tools/release_package.py reads
    # the stamp back out of the built DLLs' VERSIONINFO and refuses a mislabelled package.
    re.compile(r"^;\s*\[build\]\s+mh\s"),
]


def is_bulk(line):
    return any(r.match(line) for r in BULK_RES)


# ---- channels -----------------------------------------------------------------------------------

# The two GATED channels.  `end` is the arm window's terminator: the last line MH_Seam_Init (resp.
# the harness arm) emits before the run becomes the scenario's.  It is matched against the
# timestamp-stripped RAW line, not the normalized one, so an end marker stays readable.
CHANNELS = {
    "net": {
        "log": "mh_net.log",
        "required": True,
        "end": re.compile(r"^;\s*\[interlock\]\s+.*detour install\(s\)\s"),
        "end_desc": "the interlock's install summary -- MH_Seam_Init's last report",
    },
    "harness": {
        "log": "mh_harness.log",
        "required": False,  # absent whenever `[harness] enable` is off; that is a SKIP, not a red
        "end": re.compile(r"^;\s*\[libmh_in\]\s+inbound refusals since open:"),
        "end_desc": "the inbound-refusal census -- the harness arm's last report",
    },
}

# Ruling Q4: reported, never gated. CLOSED as a decision at fork F4F (see the docstring) -- these
# four stay advisory because no satellite writes them and none of them has an arm window, not
# because a later item will move them. check_instrument_wiring.py assertion 5 is the reopening
# condition; this list and its CHANNEL_OWNERS `advisory` rows must name the same four.
UI_CHANNELS = ["mh_video.log", "mh_input.log", "mh_capture.log", "mh_uidrive.log"]

MODE_RE = re.compile(r"\[config mode=(\w+)\]")

# fork F3F -- THE SECOND AXIS OF THE BASELINE KEY.
#
# `[config] mode` decides WHICH BODIES run; it says nothing about whether this build has a network
# transport. A no-module run is `mode=brokered` and would therefore select the brokered template and
# fail at the net arm, which is a true statement about an arm order nobody changed -- the run simply
# arms a different (smaller) set of steps, exactly as designed.
#
# THE KEY IS READ OFF THE LOG, not passed in, for the same reason `detect_mode` refuses to read the
# lane's ini: a `--template brokered_nomodule` flag on the TESTS entry would be the CALLER asserting
# what the run was, and the whole point of this tool is that the run says what it did. The net arm
# prints this banner in exactly the configuration that needs the fourth baseline, so that line IS
# the key. A run whose ini asked for `module=none` but which armed the transport anyway therefore
# selects the BROKERED template and reds at the transport step -- which is the right answer, and the
# one a caller-supplied template would have hidden.
#
# FORK F4B RE-POINTED IT, and the move made it MORE derived rather than less. It used to match the
# parenthetical `[net] module=none` inside that banner, i.e. a KEY NAME. From F4B there are two ways
# to have no transport -- the run declined the load (`[net] module=none`) or mh_net.dll is not on
# disk -- and they arm IDENTICALLY, so they must select the same baseline; naming one key in the
# other's run would also have been a log that lies. net_seams.cpp dropped the parenthetical and this
# anchors on the banner itself, which is a statement about what the arm DID. (Anchored on the
# `====` so it cannot be satisfied by install_mp_bootstrap's own `NO NETWORK MODULE` line further
# down, which reports the seven protocol stubs rather than the arm's verdict.)
NOMODULE_RE = re.compile(r"====\s+NO NETWORK MODULE\b")
NOMODULE_SUFFIX = "_nomodule"

# fork F4D -- THE THIRD AXIS, and the one that had to come FIRST in the key derivation.
#
# CONFIGURATION (1): libmh.dll is not beside mh.dll, so the 627-TU spine is not in the process at
# all (ruling Q10, ratified). That run's arm log is not a variant of any existing template -- it is
# a different arm: one promotion instead of 396, no [promote] domain reports, no [rebind] block.
#
# AND IT CANNOT BE KEYED THE USUAL WAY, which is the part worth stating. `[config mode=...]` is
# printed by the rebind arm's headline, and mh::rebind::report_arming is a LIBMH row -- so in
# configuration (1) the line that names the mode is one of the lines the missing module took with
# it. detect_mode() raised its "the arm did not get that far" refusal on a boot that in fact ran the
# whole arm and reached the end marker. The fix is the same move F4B made with NOMODULE_RE: anchor
# on a statement about what the arm DID, from a line mh.dll itself emits and therefore cannot lose.
#
# CHECKED BEFORE the mode, not after, because there is nothing to suffix: a config-(1) run has no
# mode line to carry a suffix on. The key is flat.
CONFIG1_RE = re.compile(r"\[modules\]\s+libmh:\s+(?:NOT BOUND|LOADED BUT REFUSED|ABI MISMATCH)\b")
CONFIG1_KEY = "config1"

# fork F4F -- THE FOURTH AXIS. `[net] enable=0`, which is NOT the same question as either transport
# axis above and had been left as "measure it when a lane is first routine" since F3.
#
# It is a DIFFERENT axis from `_nomodule`, and the disjointness is measured rather than assumed: over
# the five shipped config lanes, exactly one carries this banner and exactly one carries
# NOMODULE_RE's, never both (2026-09-13). `module=none`/absent says THERE IS NO TRANSPORT; `enable=0`
# says there is one and this run declines to arm it, which F3B demoted from "disable the DLL" to
# "skip the nine net steps".
#
# IT IS A KEY, NOT AN `alt`, and that follows the precedent rather than taste: every `alt` in these
# files is for an axis the log does not name before the step (the harness owning an entry, the exe's
# static-patch state), while every axis the ARM ITSELF REPORTS got a key -- `_nomodule` at F3F,
# `config1` at F4D. This one reports itself in the same sentence that changes, so it keys.
#
# MEASURED SHAPE: one text change, nothing else. `check_arm_order --compare f4f_bound f4f_netoff` =
# SURVIVOR ORDER IDENTICAL, 1 text change, 71 vs 71 structural lines. The two templates are therefore
# near-identical files, which is expected and is not an argument for merging them: a fifth shape that
# diverges further tomorrow needs a file to diverge in.
#
# SUFFIX ORDER IS FIXED -- `<mode>_nomodule_netoff` -- so a run that is both selects ONE deterministic
# key. No such lane has been recorded, so that file does not exist and such a run REFUSES by name,
# which is the honest answer rather than a silent fall-back to a neighbouring template.
NETOFF_RE = re.compile(r"MP transport NOT armed: \[net\] enable=")
NETOFF_SUFFIX = "_netoff"


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


def read_lines(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def extract(run_dir, chan, normalized=True):
    """-> (structural lines, stats) for one channel of one run dir.

    `normalized=False` returns the SAME window with the masking left off -- the arm window is the
    unit every reader here wants, and fork F3F's --spine-armed needs the counts the mask removes.

    Raises Refusal for every shape of "there is nothing here to check", because the alternative is
    a clean exit over a run that never armed."""
    spec = CHANNELS[chan]
    path = os.path.join(run_dir, spec["log"])
    if not os.path.isfile(path):
        return None, {"present": False}
    raw = read_lines(path)
    if not raw:
        raise Refusal(
            "%s is EMPTY -- a run that logged nothing is not a run that armed cleanly" % path
        )
    body = [TS_RE.sub("", ln) for ln in raw]
    end = None
    for i, ln in enumerate(body):
        if spec["end"].search(ln):
            end = i
    if end is None:
        raise Refusal(
            "%s has NO END MARKER (%s). Either the arm never finished, or the line that ends it "
            "was renamed -- both are things to look at, neither is a pass. Reading to EOF instead "
            "would silently start gating the scenario's own output." % (path, spec["end_desc"])
        )
    window = body[: end + 1]
    kept = [ln for ln in window if ln.strip() and not is_bulk(ln)]
    stats = {
        "present": True,
        "raw": len(raw),
        "window": len(window),
        "folded": len(window) - len(kept),
        "structural": len(kept),
    }
    return ([normalize(ln) for ln in kept] if normalized else kept), stats


def detect_mode(run_dir):
    """The BASELINE KEY the run actually ran, read off its own log.

    `[config] mode`, plus three axes read off the same log: fork F4D's `config1` (flat, checked
    first -- see CONFIG1_RE), fork F3F's `_nomodule` and fork F4F's `_netoff`, applied as suffixes IN
    THAT ORDER. All of them are read from the log and none from the lane's mh_net.ini -- an archived
    or copied run directory has no ini beside it, and the question is what the DLL DID, not what the
    file asked for."""
    path = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(path):
        raise Refusal("%s: no mh_net.log -- not a run directory" % run_dir)
    lines = read_lines(path)
    if any(CONFIG1_RE.search(ln) for ln in lines):
        return CONFIG1_KEY
    nomodule = any(NOMODULE_RE.search(ln) for ln in lines)
    netoff = any(NETOFF_RE.search(ln) for ln in lines)
    for ln in lines:
        m = MODE_RE.search(ln)
        if m:
            return (
                m.group(1)
                + (NOMODULE_SUFFIX if nomodule else "")
                + (NETOFF_SUFFIX if netoff else "")
            )
    raise Refusal(
        "%s: no `[config mode=...]` line in mh_net.log, so the baseline cannot be chosen. The "
        "rebind arm headline carries it in every mode -- EXCEPT configuration (1), where the "
        "headline's own emitter is inside the missing libmh.dll and CONFIG1_RE catches the run one "
        "step earlier. So this refusal now means what it always said it meant: the arm did not get "
        "that far." % run_dir
    )


# ---- baselines ----------------------------------------------------------------------------------


def baseline_path(mode):
    return os.path.join(BASELINE_DIR, "%s.json" % mode)


def load_baseline(mode):
    p = baseline_path(mode)
    if not os.path.isfile(p):
        raise Refusal(
            "no committed baseline for [config] mode=%s (%s). Record one with --update <run-dir>."
            % (mode, p)
        )
    # A MALFORMED baseline is a REFUSAL, not a traceback (fork F4F). These files are hand-edited --
    # that is the discipline, every edit carries a --compare verdict in a `why` -- and a `why` is
    # prose, so it is where a stray `C:\games` lands an invalid JSON escape. Before this, such a file
    # aborted --check-baselines with a raw json.JSONDecodeError naming a character offset, which is
    # both unactionable and outside the tool's own "every path REFUSES with a reason" rule.
    try:
        with open(p, encoding="utf-8") as fh:
            data = json.load(fh)
    except ValueError as e:
        raise Refusal(
            "%s is not valid JSON (%s). A baseline is hand-edited, and a `why` is prose -- the usual "
            "cause is an unescaped backslash in a Windows path (write `\\\\`). Nothing can be gated "
            "against an unreadable template." % (p, e)
        )
    for chan in ("net", "harness"):
        ent = data.get("channels", {}).get(chan)
        if ent is not None and not ent.get("lines"):
            raise Refusal(
                "%s: channel %r has NO lines -- an empty template passes everything" % (p, chan)
            )
    if not data.get("channels", {}).get("net", {}).get("lines"):
        raise Refusal("%s: no `net` template -- that channel is mandatory" % p)
    return data


def dump_baseline(data, path):
    """One JSON record per line for the entries, so `git diff` of a reorder is readable."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out = ["{"]
    out.append('  "mode": %s,' % json.dumps(data["mode"]))
    out.append('  "note": %s,' % json.dumps(data["note"]))
    out.append('  "recipe": %s,' % json.dumps(data["recipe"]))
    out.append('  "channels": {')
    chans = [c for c in ("net", "harness") if c in data["channels"]]
    for ci, chan in enumerate(chans):
        ent = data["channels"][chan]
        out.append('    %s: {' % json.dumps(chan))
        out.append('      "log": %s,' % json.dumps(ent["log"]))
        out.append('      "required": %s,' % json.dumps(bool(ent.get("required"))))
        out.append('      "end_marker": %s,' % json.dumps(ent["end_marker"]))
        out.append('      "lines": [')
        for li, rec in enumerate(ent["lines"]):
            comma = "" if li == len(ent["lines"]) - 1 else ","
            out.append("        %s%s" % (json.dumps(rec, sort_keys=True), comma))
        out.append("      ]")
        out.append("    }%s" % ("" if ci == len(chans) - 1 else ","))
    out.append("  }")
    out.append("}")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")


def entry_texts(rec):
    return [rec["t"]] + list(rec.get("alt") or [])


# ---- the match ----------------------------------------------------------------------------------


def match_channel(template, observed, chan):
    """Strict ordered match of `observed` against `template`.

    Strict, not subsequence: a subsequence match would pass a log that had grown an extra arm step
    in the middle, which is the single most likely thing a refactor does wrong.  `optional` entries
    may be skipped; `alt` texts are alternative spellings of the SAME step (the four the harness
    drives).  Returns a list of failure strings."""
    fails = []
    ti = oi = 0
    while ti < len(template) and oi < len(observed):
        rec = template[ti]
        if observed[oi] in entry_texts(rec):
            ti += 1
            oi += 1
            continue
        if rec.get("optional"):
            ti += 1
            continue
        fails.append(_explain(template, observed, ti, oi, chan))
        return fails
    while ti < len(template):
        rec = template[ti]
        if not rec.get("optional"):
            fails.append(
                "[%s] MISSING arm step #%d -- the log ended before it:\n        expected: %s"
                % (chan, ti + 1, rec["t"])
            )
            return fails
        ti += 1
    if oi < len(observed):
        fails.append(
            "[%s] %d UNEXPECTED structural line(s) after the last template step; first is #%d:\n"
            "        got: %s" % (chan, len(observed) - oi, oi + 1, observed[oi])
        )
    return fails


def _explain(template, observed, ti, oi, chan):
    """Say WHICH failure this is -- reorder, insertion, deletion or reworded -- not just 'differs'.

    The classification is the difference between a red someone can act on and a red someone
    re-runs.  It is derived, not guessed: where else does each line appear?"""
    want = template[ti]["t"]
    got = observed[oi]
    tpl_texts = [entry_texts(r) for r in template]
    got_at = next((k for k in range(len(template)) if got in tpl_texts[k]), None)
    want_at = next((k for k in range(oi, len(observed)) if observed[k] == want), None)
    if got_at is not None and got_at > ti and want_at is not None:
        kind = (
            "REORDERED -- template step #%d arrived early (it belongs at #%d) and step #%d is "
            "delayed to observed line #%d" % (got_at + 1, got_at + 1, ti + 1, want_at + 1)
        )
    elif got_at is not None and got_at < ti:
        kind = "REPEATED -- this is template step #%d, already matched" % (got_at + 1)
    elif got_at is None and want_at is not None:
        kind = "INSERTED -- this line is in no template step, and step #%d is still ahead" % (
            ti + 1
        )
    elif got_at is None and want_at is None:
        kind = "REWORDED or INSERTED -- neither line is found anywhere else"
    else:
        kind = "MISSING -- template step #%d never appears in the log" % (ti + 1)
    return (
        "[%s] arm order diverges at template step #%d / log line #%d: %s\n"
        "        expected: %s\n"
        "        got     : %s" % (chan, ti + 1, oi + 1, kind, want, got)
    )


# ---- the gate -----------------------------------------------------------------------------------


# ---- --spine-armed: the counts the ORDER gate structurally cannot see (fork F3F) ----------------
#
# WHY THIS IS A SEPARATE MODE AND NOT PART OF check_run. The order gate masks every digit run on
# purpose -- "how MANY rows armed" is exactly what it refuses to be sensitive to, so a run that
# armed 0 tombstones over 0 regions matches the same template as a healthy one, line for line.
# That masking is right for an ORDER gate and wrong for F3F's central claim, which is a claim about
# QUANTITY: with the transport gone the spine still arms, and "still arms" means non-zero work.
#
# Opt-in, because the claim is `[config] mode=brokered`'s. Under `original` there are no promotions
# by design and a zero would be the correct answer, so making this always-on would turn a mode into
# a failure. Not in lint either: lint has no run directory. It is a CLI an item's proof invokes,
# the same arrangement F3A chose for check_run itself.
SPINE_CHECKS = [
    (
        "promote",
        re.compile(r"^; \[promote\] .* is LIVE"),
        None,
        "no `[promote] ... is LIVE` line -- the reimpl spine did not arm at all",
    ),
    (
        "tombstone",
        re.compile(r"^; \[tombstone\] (\d+) bodies armed"),
        1,
        "`[tombstone] 0 bodies armed` -- the tombstones did not arm",
    ),
    (
        "statebind",
        re.compile(r"^; \[statebind\] regions bound: (\d+)/(\d+),"),
        1,
        "`[statebind] regions bound: 0/...` -- the state ABI bound nothing",
    ),
]


def check_spine_armed(run_dir, say=print):
    """Fork F3F: assert the SPINE armed with real work, whatever the transport did.

    The four families the F3F scope names -- [promote], [tombstone], [hostapi], [statebind] -- plus
    the one thing that makes all four legible at once: no `<-- FAILED` anywhere in the arm window.
    Every one of those lines already carries its own verdict suffix, so the DLL is the judge and
    this is the reader."""
    fails = []
    lines, _stats = extract(run_dir, "net", normalized=False)
    if lines is None:
        raise Refusal("%s: no mh_net.log -- nothing to assert about" % run_dir)
    say("check_arm_order --spine-armed: %s" % run_dir)
    for name, rx, grp, why in SPINE_CHECKS:
        hit = next((m for m in (rx.match(ln) for ln in lines) if m), None)
        if hit is None:
            fails.append("[%s] %s" % (name, why))
            continue
        n = int(hit.group(grp)) if grp else 1
        if n <= 0:
            fails.append("[%s] %s" % (name, why))
        say("  %-10s ok  %s" % (name, hit.group(0)[:110]))
    # statebind's OTHER postcondition: a host answers for EVERY region (the shared clause, the one
    # that holds on both the stock and the relocating bind -- see the line's own comment in
    # net_seams.cpp). Checked here rather than trusted to the FAILED suffix, because that suffix is
    # the DLL agreeing with itself and this is a reader that can disagree.
    for ln in lines:
        m = SPINE_CHECKS[2][1].match(ln)
        if m and m.group(1) != m.group(2):
            fails.append("[statebind] %s of %s regions bound -- the walk is holey" % m.groups())
    for family in ("hostapi", "libmh_in", "hostevt"):
        pat = "; [%s] " % family
        if not any(ln.startswith(pat) for ln in lines):
            fails.append("[%s] no arm-time report line at all" % family)
    bad = [ln for ln in lines if "<-- FAILED" in ln]
    for ln in bad:
        fails.append("a line reports its own FAILURE: %s" % ln)
    if not bad:
        say("  %-10s ok  no `<-- FAILED` in the arm window" % "verdicts")
    for f in fails:
        say("[FAIL] %s" % f)
    if not fails:
        say("check_arm_order --spine-armed: PASS -- the spine armed with real work")
    return 1 if fails else 0


def check_run(run_dir, mode=None, say=print):
    if not os.path.isdir(run_dir):
        raise Refusal("no such run directory: %s" % run_dir)
    mode = mode or detect_mode(run_dir)
    base = load_baseline(mode)
    fails = []
    say("check_arm_order: %s" % run_dir)
    say("  baseline key=%s -> %s" % (mode, os.path.relpath(baseline_path(mode), REPO)))
    checked_any = False
    for chan in ("net", "harness"):
        spec = base.get("channels", {}).get(chan)
        observed, stats = extract(run_dir, chan)
        if observed is None:
            if spec is not None and spec.get("required"):
                fails.append(
                    "[%s] %s is ABSENT but the baseline requires it" % (chan, CHANNELS[chan]["log"])
                )
            else:
                say(
                    "  %-8s SKIP  (%s absent -- not armed in this run)"
                    % (chan, CHANNELS[chan]["log"])
                )
            continue
        if spec is None:
            say(
                "  %-8s SKIP  (%s present but baseline %s has no template for it)"
                % (chan, CHANNELS[chan]["log"], mode)
            )
            continue
        checked_any = True
        f = match_channel(spec["lines"], observed, chan)
        say(
            "  %-8s %s  %d structural line(s) vs %d template step(s)  "
            "[%d raw, %d in arm window, %d bulk rows folded]"
            % (
                chan,
                "FAIL" if f else "ok  ",
                stats["structural"],
                len(spec["lines"]),
                stats["raw"],
                stats["window"],
                stats["folded"],
            )
        )
        fails += f
    if not checked_any:
        fails.append(
            "NO channel was actually gated -- this run proves nothing. A green with zero compared "
            "channels is the failure the instrument-channel notes names by hand."
        )
    # Q4: advisory, never red.
    seen = [n for n in UI_CHANNELS if os.path.isfile(os.path.join(run_dir, n))]
    say(
        "  ui       ADVISORY (Q4, CLOSED at F4F -- no satellite writes these): %d/%d channel(s) "
        "present: %s" % (len(seen), len(UI_CHANNELS), ", ".join(seen) or "none")
    )
    for f in fails:
        say("[FAIL] %s" % f)
    if not fails:
        say("check_arm_order: PASS -- arm order matches the %s baseline" % mode)
    return 1 if fails else 0


# ---- --compare: the no-golden diff F2 ran by hand ------------------------------------------------


def compare_runs(old_dir, new_dir, say=print):
    import difflib

    rc = 0
    say("check_arm_order --compare")
    say("  A (old): %s" % old_dir)
    say("  B (new): %s" % new_dir)
    for chan in ("net", "harness"):
        a, _ = extract(old_dir, chan)
        b, _ = extract(new_dir, chan)
        if a is None and b is None:
            continue
        if a is None or b is None:
            say("  %-8s CHANNEL PRESENCE CHANGED: A=%s B=%s" % (chan, a is not None, b is not None))
            rc = 1
            continue
        sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
        ops = [op for op in sm.get_opcodes() if op[0] != "equal"]
        text_changes = []
        structural = []
        for tag, i1, i2, j1, j2 in ops:
            if tag == "replace" and (i2 - i1) == (j2 - j1):
                for k in range(i2 - i1):
                    text_changes.append((i1 + k, a[i1 + k], b[j1 + k]))
            else:
                structural.append((tag, i1, i2, j1, j2))
        if not ops:
            say(
                "  %-8s SURVIVOR ORDER IDENTICAL, 0 text changes (%d structural lines)"
                % (chan, len(a))
            )
            continue
        if not structural:
            say(
                "  %-8s SURVIVOR ORDER IDENTICAL, %d text change(s) (%d vs %d structural lines)"
                % (chan, len(text_changes), len(a), len(b))
            )
            for idx, ta, tb in text_changes:
                say("      #%d  - %s" % (idx + 1, ta))
                say("          + %s" % tb)
            continue
        rc = 1
        say(
            "  %-8s ORDER CHANGED: %d structural edit(s), %d text change(s) (%d vs %d lines)"
            % (chan, len(structural), len(text_changes), len(a), len(b))
        )
        for tag, i1, i2, j1, j2 in structural:
            if tag in ("delete", "replace"):
                for ln in a[i1:i2]:
                    say("      - #%d %s" % (i1 + 1, ln))
            if tag in ("insert", "replace"):
                for ln in b[j1:j2]:
                    say("      + #%d %s" % (j1 + 1, ln))
        for idx, ta, tb in text_changes:
            say("      #%d  - %s" % (idx + 1, ta))
            say("          + %s" % tb)
    say("check_arm_order --compare: %s" % ("ORDER MOVED" if rc else "ORDER HELD"))
    return rc


# ---- --update: derive a baseline from a live run -------------------------------------------------


# Annotations that must SURVIVE a re-record, keyed by the normalized text they belong to. A
# re-record that silently dropped them would turn every optional step into a mandatory one and the
# alternative texts into reds, which is how a regenerated golden quietly becomes a stricter one.
def merge_annotations(old_lines, new_texts):
    by_text = {}
    for rec in old_lines or []:
        for t in entry_texts(rec):
            by_text[t] = rec
    out = []
    for t in new_texts:
        old = by_text.get(t)
        rec = {"t": t}
        if old:
            # keep the ORIGINAL primary text, so an `alt` spelling recorded from this run does not
            # promote itself to the canonical one
            rec["t"] = old["t"]
            if old.get("alt"):
                rec["alt"] = old["alt"]
            if old.get("optional"):
                rec["optional"] = True
            if old.get("why"):
                rec["why"] = old["why"]
            if t not in entry_texts(rec):
                rec.setdefault("alt", []).append(t)
        out.append(rec)
    return out


def update_baseline(run_dir, mode=None, note=None, recipe=None, say=print):
    """Re-record a baseline from a live run.

    `recipe` is the COMMAND that produced the run, not the path it landed in -- a run directory is
    under tmp/ or a lane and will not exist for the next reader, while the command reproduces it.
    Kept from the previous file when not given, so a re-record never silently loses it."""
    mode = mode or detect_mode(run_dir)
    p = baseline_path(mode)
    old = {}
    if os.path.isfile(p):
        with open(p, encoding="utf-8") as fh:
            old = json.load(fh)
    data = {
        "mode": mode,
        "note": note
        or old.get("note")
        or "fork F3A -- the arm-log order gate. Edit ONLY with a --compare verdict in the same "
        "commit saying which steps moved and why.",
        "recipe": recipe
        or old.get("recipe")
        or "(unrecorded -- pass --recipe on the next re-record)",
        "channels": {},
    }
    for chan in ("net", "harness"):
        observed, stats = extract(run_dir, chan)
        if observed is None:
            keep = old.get("channels", {}).get(chan)
            if keep:
                data["channels"][chan] = keep
                say("  %-8s kept from the previous baseline (not armed in this run)" % chan)
            continue
        oldlines = old.get("channels", {}).get(chan, {}).get("lines")
        # A RE-RECORD MAY NOT SILENTLY DROP AN `optional` STEP (fork F4B). merge_annotations exists
        # so a regenerated golden does not quietly become a STRICTER one; this is the same hazard
        # from the other side, and it was live -- F4B's own first re-record of brokered.json deleted
        # F3G's two `[inmem]` optional entries, because the run it recorded from did not have
        # `[patch] inmem=1` and nothing noticed. The result looks harmless and is not: the next
        # inmem-armed lane reds with two INSERTED lines nobody can explain, and the obvious fix is to
        # re-record again, which deletes something else. An `optional` line is by definition one that
        # this run need not contain, so absence is never evidence to remove it -- only a human
        # deleting the mechanism is. REFUSE and hand the operator the list.
        observed_set = set(observed)
        dropped = [
            rec
            for rec in (oldlines or [])
            if rec.get("optional") and not (set(entry_texts(rec)) & observed_set)
        ]
        if dropped:
            raise Refusal(
                "re-recording %s/%s would DROP %d `optional` step(s) the old baseline carries, "
                "because this run did not emit them: %s. An optional step is one a run need not "
                "contain, so its absence here is not evidence that it should go -- re-recording "
                "anyway would make the baseline stricter than the mechanism it describes, and the "
                "next run that DOES emit them would red as an unexplained INSERT. Hand-merge the "
                "new step into the file instead, or record from a run that arms them."
                % (mode, chan, len(dropped), "; ".join(entry_texts(r)[0][:70] for r in dropped))
            )
        data["channels"][chan] = {
            "log": CHANNELS[chan]["log"],
            "required": CHANNELS[chan]["required"],
            "end_marker": CHANNELS[chan]["end"].pattern,
            "lines": merge_annotations(oldlines, observed),
        }
        say(
            "  %-8s %d structural step(s) (%d bulk rows folded)"
            % (chan, len(observed), stats["folded"])
        )
    dump_baseline(data, p)
    say("wrote %s" % os.path.relpath(p, REPO))
    return 0


def check_baselines(say=print):
    """Structural check of the committed files -- runs with no rig and no run directory."""
    fails = []
    if not os.path.isdir(BASELINE_DIR):
        fails.append("no baseline directory at %s" % BASELINE_DIR)
        for f in fails:
            say("[FAIL] %s" % f)
        return 1
    names = sorted(n[:-5] for n in os.listdir(BASELINE_DIR) if n.endswith(".json"))
    if not names:
        fails.append("%s holds NO baselines -- nothing is being gated" % BASELINE_DIR)
    for mode in names:
        try:
            data = load_baseline(mode)
        except Refusal as e:
            fails.append(str(e))
            continue
        for chan, ent in data["channels"].items():
            texts = []
            for rec in ent["lines"]:
                if not rec.get("t"):
                    fails.append("%s/%s: an entry has no `t`" % (mode, chan))
                    continue
                for t in entry_texts(rec):
                    if t in texts:
                        fails.append(
                            "%s/%s: text appears in two steps, so the matcher cannot tell them "
                            "apart: %s" % (mode, chan, t)
                        )
                    texts.append(t)
                if normalize(rec["t"]) != rec["t"]:
                    fails.append(
                        "%s/%s: a template line is NOT in normalized form (a raw address or count "
                        "was recorded literally and would pin the baseline to one run): %s"
                        % (mode, chan, rec["t"])
                    )
        say(
            "  %-10s %s"
            % (
                mode,
                ", ".join(
                    "%s=%d step(s)%s"
                    % (
                        c,
                        len(e["lines"]),
                        " optional=%d" % sum(1 for r in e["lines"] if r.get("optional")),
                    )
                    for c, e in sorted(data["channels"].items())
                ),
            )
        )
    for f in fails:
        say("[FAIL] %s" % f)
    if not fails:
        say("check_arm_order --check-baselines: PASS -- %d baseline(s)" % len(names))
    return 1 if fails else 0


# ---- selftest ------------------------------------------------------------------------------------

# Rendering values for the two masks. They are deliberately NOT the values any real run produced:
# the point is that normalization must map an ARBITRARY address/count back onto the template, so a
# render that reused the recorded literals would prove nothing about the masker.
#
# THE COUNT FILL IS ONE DIGIT ON PURPOSE, and it has now been narrowed TWICE by a failing control --
# which is the round-trip arm in selftest() doing its job both times, naming the line instead of
# leaving the control mysteriously red.
#
#   * too LONG breaks the ids that SUFFIX a letter. Several arm lines carry a short alphanumeric id
#     -- `(C8)`, `MP D14`, `U18`, `LT1C` -- which normalizes to `C<N>` because 2-3 characters is
#     below the hex run's 4-character floor. Render `<N>` as five digits and `C<N>` comes back as
#     `C97531`: six hex characters, which masks to `<X>`. (The fill was 5, then 2, for this.)
#   * TWO is still too long for an id with a letter on BOTH sides. fork F4F's fifth baseline carries
#     `(F3B: this key now skips the nine net steps ...)` -> `F<N>B`, and a two-digit fill renders
#     `F31B`: four hex characters with a letter in them, i.e. exactly the hex run's floor, so it
#     masks to `<X>` and the control reds over the RENDER. One digit gives `F7B` -- three characters,
#     under the floor -- and every template line in all five baselines then round-trips under both
#     fills (658 checks). Keep both fills single-digit and distinct.
FILL_N = "7"
FILL_X = "A1B2C3D4E5F60789"
FILL_N2 = "8"
FILL_X2 = "0FEDCBA987654321"


def render(text, fill_n=FILL_N, fill_x=FILL_X):
    """A template line -> a plausible RAW log line. The inverse of normalize(), value-wise."""
    return "[04:05:06.789] " + text.replace("<X>", fill_x).replace("<N>", fill_n)


def synth_run(tmp, mode, mutate=None):
    """Write a synthetic run directory from the committed baseline for `mode`.

    Hermetic on purpose -- lint has no rig and no run directories, and a gate that can only be
    exercised by a 40-second game launch is a gate nobody re-runs.  The three live lanes are the
    acceptance evidence; this is the instrument's own control."""
    base = load_baseline(mode)
    d = os.path.join(tmp, "run")
    os.makedirs(d, exist_ok=True)
    for chan, ent in base["channels"].items():
        lines = [render(rec["t"]) for rec in ent["lines"] if not rec.get("optional")]
        # a bulk row of each kind, so the fold is exercised rather than assumed
        lines[1:1] = [
            "[04:05:06.789] ;   [rebind] cfg_GetAnimTime -> OURS",
            "[04:05:06.789] ; [promote] orders_issue: + llm_strat_unit_order_move",
            "[04:05:06.790] ; [tombstone] domain orders: 71 armed (71 promoted-fill, 0 ledger-dead, 0 force-armed)",
        ]
        if mutate:
            lines = mutate(chan, lines)
        # trailing scenario chatter: everything past the end marker must be ignored
        lines.append("[04:05:07.000] ; S2 host session: uitest#DEADBEEF01 map=blue monday.mpm")
        with open(os.path.join(d, ent["log"]), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
    return d


def selftest():
    ok = True
    quiet = lambda *_: None  # noqa: E731

    def expect(name, cond, detail=""):
        nonlocal ok
        print(
            "  [%s] %s%s"
            % ("ok" if cond else "FAIL", name, (" -- " + detail) if detail and not cond else "")
        )
        ok = ok and cond

    # --- the committed files themselves ------------------------------------------------------
    sink = []
    expect("committed baselines are structurally sound", check_baselines(say=sink.append) == 0)
    if sink and any(s.startswith("[FAIL]") for s in sink):
        for s in sink:
            print("    %s" % s)

    modes = sorted(n[:-5] for n in os.listdir(BASELINE_DIR) if n.endswith(".json"))
    expect(
        "at least two baselines are committed (one per [config] mode)", len(modes) >= 2, str(modes)
    )

    # --- the normalizer, in isolation ---------------------------------------------------------
    expect(
        "an address masks",
        normalize("; [interlock] fix 'x' at 0043F2A5 is inside")
        == "; [interlock] fix 'x' at <X> is inside",
        normalize("; [interlock] fix 'x' at 0043F2A5 is inside"),
    )
    expect(
        "a fingerprint masks",
        normalize("fp=539FD2F36C23C89E,") == "fp=<X>,",
        normalize("fp=539FD2F36C23C89E,"),
    )
    expect(
        "an 0x id masks",
        normalize("version 0x8777535E,") == "version <X>,",
        normalize("version 0x8777535E,"),
    )
    expect("a count masks", normalize("armed 719 of 719 row(s)") == "armed <N> of <N> row(s)")
    expect("a timing masks", normalize("mean 4382.63 us each") == "mean <N> us each")
    expect(
        "the lane number inside an identifier masks (no \\b to lean on)",
        normalize('renamed to "MHMut61"') == 'renamed to "MHMut<N>"',
        normalize('renamed to "MHMut61"'),
    )
    expect(
        "a pure-decimal run does NOT become a hex mask",
        normalize("seed=1362723655 at=60") == "seed=<N> at=<N>",
        normalize("seed=1362723655 at=60"),
    )
    # The words that are entirely hex digits. This is the arm that stops the hex mask from eating
    # prose, and it is not hypothetical -- "added", "decade" and "face" all appear in log English.
    for word in ("added", "decade", "face", "beef", "dead", "deaf", "cafe", "faded"):
        expect(
            "the English word %r survives the hex mask" % word,
            normalize("x %s y" % word) == "x %s y" % word,
        )
    expect(
        "a run of 8 pure hex letters IS masked (that is deadbeef, not a word)",
        normalize("k deadbeef z") == "k <X> z",
    )

    # --- the fold ------------------------------------------------------------------------------
    expect("a rebind ROW folds out", is_bulk(";   [rebind] cfg_GetAnimTime -> OURS"))
    expect(
        "a promote ROW folds out", is_bulk("; [promote] orders_issue: + llm_strat_unit_order_move")
    )
    expect("a tombstone domain ROW folds out", is_bulk("; [tombstone] domain orders: 71 armed (x)"))
    expect(
        "the rebind HEADLINE does NOT fold out (it is structural)",
        not is_bulk("; [rebind] armed 719 of 719 row(s) [config mode=brokered]:"),
    )
    expect(
        "the tombstone HEADLINE does NOT fold out",
        not is_bulk(
            "; [tombstone] 393 bodies armed (393 promoted-fill, 0 ledger-dead, 0 force-armed), 3 skipped"
        ),
    )

    # --- the render round-trips, so a red control means the MASKER, never the fixture ----------
    rt_ok, rt_n, rt_bad = True, 0, ""
    for mode in modes:
        base = load_baseline(mode)
        for chan, ent in base["channels"].items():
            for rec in ent["lines"]:
                for t in entry_texts(rec):
                    for fn, fx in ((FILL_N, FILL_X), (FILL_N2, FILL_X2)):
                        rt_n += 1
                        got = normalize(render(t, fn, fx))
                        if got != t and rt_ok:
                            rt_ok, rt_bad = False, "%r -> %r" % (t, got)
    expect(
        "every template line round-trips render()->normalize() under BOTH fills (%d checks)" % rt_n,
        rt_ok and rt_n > 100,
        rt_bad or "only %d checks ran" % rt_n,
    )

    # --- planted mutations, per mode, in a temp tree --------------------------------------------
    for mode in modes:
        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode)
            n_struct = len(extract(d, "net")[0])
            expect(
                "[%s] the synthetic run is not vacuous (%d structural lines)" % (mode, n_struct),
                n_struct >= 30,
                str(n_struct),
            )
            expect(
                "[%s] CONTROL: an unmutated render PASSES" % mode,
                check_run(d, mode, say=quiet) == 0,
            )
            # fork F3F: THE KEY IS DERIVED, NOT PASSED. Every arm above hands check_run the mode, so
            # none of them exercises the one step that decides WHICH baseline a real run is judged
            # against -- and that step grew a second axis. Render the baseline back into a log and
            # assert detect_mode names this very file.
            got = None
            try:
                got = detect_mode(d)
            except Refusal as e:
                got = "REFUSED: %s" % e
            expect(
                "[%s] detect_mode reads the key back off the rendered log" % mode,
                got == mode,
                str(got),
            )
            # fork F3F: --spine-armed on a baseline that HAS a promoted spine. FILL_N is non-zero, so
            # the rendered control is the positive arm; the negative is one count set to zero.
            spine = any(
                "is LIVE" in t
                for rec in load_baseline(mode)["channels"]["net"]["lines"]
                for t in entry_texts(rec)
            )
            if spine:
                expect(
                    "[%s] --spine-armed PASSES the unmutated render" % mode,
                    check_spine_armed(d, say=quiet) == 0,
                )

        def swap(chan, lines):
            # swap two ADJACENT structural lines deep in the arm -- the smallest real reorder
            i = len(lines) // 2
            j = i + 1
            while j < len(lines) and is_bulk(TS_RE.sub("", lines[j])):
                j += 1
            if j < len(lines):
                lines[i], lines[j] = lines[j], lines[i]
            return lines

        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode, mutate=swap)
            expect("[%s] planted REORDER -> RED" % mode, check_run(d, mode, say=quiet) == 1)

        def insert(chan, lines):
            lines.insert(
                len(lines) // 2, "[04:05:06.789] ; [promote] rogue: a step nobody registered"
            )
            return lines

        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode, mutate=insert)
            expect("[%s] planted INSERTED LINE -> RED" % mode, check_run(d, mode, say=quiet) == 1)

        def delete(chan, lines):
            for k in range(len(lines) - 2, 1, -1):
                if not is_bulk(TS_RE.sub("", lines[k])):
                    del lines[k]
                    break
            return lines

        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode, mutate=delete)
            expect("[%s] planted DELETED LINE -> RED" % mode, check_run(d, mode, say=quiet) == 1)

        def remask(chan, lines):
            # every masked FIELD changes value: addresses, counts, timings, the lane number.
            return [ln.replace(FILL_X, FILL_X2).replace(FILL_N, FILL_N2) for ln in lines]

        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode, mutate=remask)
            expect(
                "[%s] a MASKED-FIELD change is NOT red (different addresses/counts/timings)" % mode,
                check_run(d, mode, say=quiet) == 0,
            )

        def reword(chan, lines):
            for k in range(len(lines) - 2, 1, -1):
                raw = TS_RE.sub("", lines[k])
                if not is_bulk(raw):
                    lines[k] = lines[k] + " AND SOMETHING ELSE"
                    break
            return lines

        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode, mutate=reword)
            expect("[%s] a REWORDED step -> RED" % mode, check_run(d, mode, say=quiet) == 1)

        # fork F3F's negative arms: the two shapes --spine-armed exists to catch, both of which the
        # ORDER gate passes by construction (a masked count and a suffix on a line it already has).
        def zero_tombstones(chan, lines):
            return [
                re.sub(r"(\[tombstone\] )\d+( bodies armed)", r"\g<1>0\g<2>", ln) for ln in lines
            ]

        def plant_failed(chan, lines):
            out = []
            for ln in lines:
                out.append(ln + "  <-- FAILED" if "; [statebind] " in ln else ln)
            return out

        if spine:
            with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
                d = synth_run(tmp, mode, mutate=zero_tombstones)
                expect(
                    "[%s] --spine-armed REDS on a ZERO tombstone count, and the ORDER gate passes "
                    "it (the whole reason the mode exists)" % mode,
                    check_spine_armed(d, say=quiet) == 1 and check_run(d, mode, say=quiet) == 0,
                )
            with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
                d = synth_run(tmp, mode, mutate=plant_failed)
                expect(
                    "[%s] --spine-armed REDS on a line reporting `<-- FAILED`" % mode,
                    check_spine_armed(d, say=quiet) == 1,
                )

        def kill_marker(chan, lines):
            return [ln for ln in lines if not CHANNELS[chan]["end"].search(TS_RE.sub("", ln))]

        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            d = synth_run(tmp, mode, mutate=kill_marker)
            refused = False
            try:
                check_run(d, mode, say=quiet)
            except Refusal:
                refused = True
            expect("[%s] a log with NO END MARKER is REFUSED, not passed" % mode, refused)

        # --compare against itself, and against a one-text-change copy
        with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
            a = synth_run(os.path.join(tmp, "a"), mode)
            b = synth_run(os.path.join(tmp, "b"), mode, mutate=remask)
            expect(
                "[%s] --compare of two masked-equal runs: ORDER HELD" % mode,
                compare_runs(a, b, say=quiet) == 0,
            )
            c = synth_run(os.path.join(tmp, "c"), mode, mutate=swap)
            expect(
                "[%s] --compare of a reordered run: ORDER MOVED" % mode,
                compare_runs(a, c, say=quiet) == 1,
            )
            out = []
            d2 = synth_run(os.path.join(tmp, "d"), mode, mutate=reword)
            rc = compare_runs(a, d2, say=out.append)
            joined = "\n".join(out)
            expect(
                "[%s] --compare of ONE reworded step reports 'SURVIVOR ORDER IDENTICAL, 1 text change'"
                % mode,
                rc == 0 and "SURVIVOR ORDER IDENTICAL, 1 text change(s)" in joined,
                joined[-300:],
            )

    # --- the refusals -----------------------------------------------------------------------------
    with tempfile.TemporaryDirectory(prefix="armorder_") as tmp:
        empty = os.path.join(tmp, "empty")
        os.makedirs(empty)
        refused = False
        try:
            check_run(empty, say=quiet)
        except Refusal:
            refused = True
        expect("an EMPTY run directory is REFUSED, not passed", refused)

        nomode = os.path.join(tmp, "nomode")
        os.makedirs(nomode)
        with open(os.path.join(nomode, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write("; [interlock] 0 detour install(s) refused -- everything got its entry\n")
        refused = False
        try:
            check_run(nomode, say=quiet)
        except Refusal:
            refused = True
        expect("a log with no `[config mode=...]` line is REFUSED", refused)

        refused = False
        try:
            check_run(os.path.join(tmp, "does_not_exist"), say=quiet)
        except Refusal:
            refused = True
        expect("a non-existent run directory is REFUSED", refused)

    print("check_arm_order --selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---- main -----------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dir", nargs="?", help="a run directory (…/logs/<stamp>_solo/)")
    ap.add_argument(
        "--mode", help="force the [config] mode baseline instead of reading it off the log"
    )
    ap.add_argument(
        "--compare", nargs=2, metavar=("OLD", "NEW"), help="no-golden diff of two run dirs"
    )
    ap.add_argument(
        "--update", metavar="RUN_DIR", help="(re)derive the baseline for that run's mode"
    )
    ap.add_argument(
        "--recipe", help="--update: the COMMAND that produced the run, recorded into the baseline"
    )
    ap.add_argument(
        "--check-baselines", action="store_true", help="structural check of the committed files"
    )
    ap.add_argument(
        "--spine-armed",
        action="store_true",
        help="fork F3F: assert the spine armed with NON-ZERO work ([promote]/[tombstone]/"
        "[statebind]/[hostapi], no `<-- FAILED`) -- the counts the order gate masks away. "
        "Composes with the order check on the same run directory.",
    )
    ap.add_argument("--selftest", action="store_true", help="planted-violation reds + the refusals")
    args = ap.parse_args(argv)
    try:
        if args.selftest:
            return selftest()
        if args.check_baselines:
            return check_baselines()
        if args.compare:
            return compare_runs(args.compare[0], args.compare[1])
        if args.update:
            return update_baseline(args.update, args.mode, recipe=args.recipe)
        if not args.run_dir:
            ap.error("give a run directory, --compare, --update, --check-baselines or --selftest")
        rc = check_run(args.run_dir, args.mode)
        if args.spine_armed:
            rc |= check_spine_armed(args.run_dir)
        return rc
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
