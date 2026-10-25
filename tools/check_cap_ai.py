#!/usr/bin/env python3
"""
tools/check_cap_ai.py -- patching:PT-CAP11: the verdict of the AI-scratch row (cap_ai_scratch / cap_ai_scratch_off).

The stimulus is the harness cap-fill fixture pointed at the AI player (cap_player=1, cap_issue=1) with `[caps] raise=1`, libmh.dll absent:
the AI owns 250 buildings / 60 mines (the vanilla caps are 100 / 32) while the human's buildings stand around its mother, and the AI
keeps playing for 10 game minutes. The eight AI scratch arrays (src/patcher/cap_v022_aiscratch_EN.mh.patch.json) have no bound check in the
game; with the layer ON they live in `.mhcapa` behind guard bytes that mh.dll checks every sim step.

Clauses (all must hold):
  1-5  tools/check_cap_fill.py's clauses on the fixture: relocation APPLIED, limits ON, the counts past the vanilla caps, wide geometry,
       the game kept stepping, no desync, the mirror watch clean.
  6  (default arm) the AI layer was STAGED (`[caps] ai-scratch layer STAGED (165 sites, 8 arrays`) and went ON with the raise
     (`[caps] ai-scratch ON`), and nothing is REFUSED.
  7  (default arm) no `AI GUARD HIT` line, and the last `ai-census` line reads `guard=0x0`: nothing wrote past an enlarged array.
  8  (default arm) the census saw the AI index its scratch PAST the vanilla bound: at least the mine arrays (mine_roster, mine_qual,
     mine_yield: > 32 collected mines) -- i.e. a vanilla layout would have overflowed in this very run. The other arrays that crossed
     their vanilla bound are listed; an array that did not is reported as `not reached` (never as a pass for that array).
  `--negative` (the pre-fix arm, `[caps] ai_scratch=0`): the AI layer is OFF (`ai-scratch layer OFF`, no `ai-scratch ON`), and the fixture's own
     numbers are the census of the bad index: the AI owns buildings at slot indices past 100 and more than 32 mines, i.e.
     llm_strat_bldg_connectivity_flood_fill / llm_strat_ai_mine_portfolio_rebalance index their vanilla byte[100] / [32] scratch past its size.

  python tools/check_cap_ai.py [--negative] <run-dir>
  python tools/check_cap_ai.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import check_cap_fill  # noqa: E402
import check_storage_purge  # noqa: E402

strip = check_cap_fill.strip
CENSUS_RE = re.compile(
    r"\[caps\] ai-census tick=(?P<tick>\d+) hw:(?P<hw>.*?) guard=0x(?P<guard>[0-9A-Fa-f]+)"
)
PAST_RE = re.compile(
    r"\[caps\] ai-census: (?P<name>\w+) element (?P<el>\d+) is past the vanilla (?P<stock>\d+)"
)
ENTRY_RE = re.compile(r"(\w+)=(\d+)/(\d+)")
REQUIRED_PAST = (
    "conn_trial",
    "conn_base",
    "mine_roster",
    "engage_scratch",
)  # mine_qual / mine_yield share the roster index but hold zero rows for mines without a yield; scan_targets cannot pass 1024 in a 1v1
HAMMER_RE = re.compile(
    r"; CAP AIHAMMER step=(?P<step>\d+) player (?P<p>\d+): (?P<live>\d+) live buildings of player \d+ fed to the game's adders -> engage_scratch count (?P<eng>\d+) "
    r"\(vanilla 256\), scan_targets count (?P<scn>\d+) \(vanilla 1024\); bytes changed after the VANILLA engage scratch .*?: (?P<clob>\d+)"
)


def hammer(harness):
    last = None
    for ln in harness:
        m = HAMMER_RE.match(ln)
        if m:
            last = {k: int(v) for k, v in m.groupdict().items()}
    return last


def census(net):
    last = None
    for ln in net:
        m = CENSUS_RE.search(ln)
        if m:
            last = (
                int(m.group("tick")),
                {n: (int(h), int(s)) for n, h, s in ENTRY_RE.findall(m.group("hw"))},
                int(m.group("guard"), 16),
            )
    past = {}
    for ln in net:
        m = PAST_RE.search(ln)
        if m:
            past[m.group("name")] = (int(m.group("el")), int(m.group("stock")))
    return last, past


def problems(net, harness, negative=False):
    net, harness = strip(net), strip(harness)
    bad = check_cap_fill.problems(net, harness)
    if negative:
        if not any("[caps] ai-scratch layer OFF" in ln for ln in net):
            bad.append(
                "clause 6: no `ai-scratch layer OFF` banner (the negative arm ran WITH the fix)"
            )
        if any("ai-scratch ON" in ln or "ai-scratch layer STAGED" in ln for ln in net):
            bad.append("clause 6: the AI layer was staged / switched ON in the negative arm")
        v = None
        for ln in harness:
            m = check_cap_fill.VERDICT_RE.match(ln)
            if m:
                v = {k: int(x) for k, x in m.groupdict().items()}
        h = hammer(harness)
        if h is None:
            bad.append("clause 9: no `; CAP AIHAMMER` line (the engage-list stimulus never ran)")
        else:
            if h["eng"] <= 256:
                bad.append(
                    "clause 9: the engage scratch reached %d entries, not past the vanilla 256"
                    % h["eng"]
                )
            if h["clob"] <= 0:
                bad.append(
                    "clause 9: no byte after the vanilla engage scratch changed -- the overflow did not land (the negative arm shows nothing)"
                )
        if v:
            if v["bldg_hi"] <= 100:
                bad.append(
                    "clause 8: the AI's highest building slot %d is not past the vanilla byte[100] scratch"
                    % v["bldg_hi"]
                )
            if v["mine"] <= 32:
                bad.append(
                    "clause 8: the AI owns %d mines, not past the vanilla [32] scratch" % v["mine"]
                )
        return bad
    if not any(
        re.search(r"\[caps\] ai-scratch layer STAGED \(\d+ sites, \d+ arrays", ln) for ln in net
    ):
        bad.append("clause 6: no `[caps] ai-scratch layer STAGED (<n> sites, <n> arrays` banner")
    if not any("[caps] ai-scratch ON" in ln for ln in net):
        bad.append(
            "clause 6: no `[caps] ai-scratch ON` (the AI layer never went ON with the raise)"
        )
    for ln in net:
        if "AI GUARD HIT" in ln:
            bad.append("clause 7: " + ln)
    h = hammer(harness)
    if h is None:
        bad.append("clause 9: no `; CAP AIHAMMER` line (the engage-list stimulus never ran)")
    else:
        if h["eng"] <= 256:
            bad.append(
                "clause 9: the engage scratch reached %d entries, not past the vanilla 256"
                % h["eng"]
            )
        if h["clob"] != 0:
            bad.append(
                "clause 9: %d bytes after the vanilla engage scratch changed with the AI layer ON (the enlarged array should have absorbed the list)"
                % h["clob"]
            )
    last, past = census(net)
    if last is None:
        bad.append(
            "clause 7: no `ai-census` line (the guard check never ran; the match did not step with the layer ON)"
        )
        return bad
    if last[2] != 0:
        bad.append("clause 7: the last ai-census reads guard=0x%X" % last[2])
    for a in REQUIRED_PAST:
        if a not in past:
            bad.append(
                "clause 8: the census never saw %s past the vanilla bound (hw %s)"
                % (a, last[1].get(a))
            )
    return bad


def run(run_dir, negative):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, negative)
    v = [ln for ln in strip(harness) if ln.startswith("; CAP VERDICT")]
    if v:
        print("  " + v[-1])
    last, past = census(strip(net))
    if last and not negative:
        print(
            "  ai-census tick=%d guard=0x%X: %s"
            % (last[0], last[2], " ".join("%s=%d/%d" % (n, h, s) for n, (h, s) in last[1].items()))
        )
        reached = sorted(past)
        missing = sorted(set(last[1]) - set(past))
        print(
            "  past the vanilla bound: %s"
            % (", ".join("%s (%d > %d)" % (n, past[n][0], past[n][1]) for n in reached) or "none")
        )
        print("  NOT reached in this match: %s" % (", ".join(missing) or "none"))
    for b in bad:
        print("  check_cap_ai: " + b)
    print("check_cap_ai: %s%s" % ("FAIL" if bad else "PASS", " (negative arm)" if negative else ""))
    return 1 if bad else 0


GOOD_NET = check_cap_fill.GOOD_NET + [
    "; [caps] ai-scratch layer STAGED (165 sites, 8 arrays, .mhcapa=0x04200000); written with the raise",
    "; [caps] ai-scratch ON (gameplay settings): 165 sites -> .mhcapa 04200000, 8 arrays enlarged, guards armed",
    "; [caps] ai-census: conn_base element 185 is past the vanilla 100 -- a vanilla layout would have overflowed here (tick 700)",
    "; [caps] ai-census: conn_trial element 251 is past the vanilla 100 -- a vanilla layout would have overflowed here (tick 700)",
    "; [caps] ai-census: mine_roster element 60 is past the vanilla 32 -- a vanilla layout would have overflowed here (tick 700)",
    "; [caps] ai-census: engage_scratch element 370 is past the vanilla 256 -- a vanilla layout would have overflowed here (tick 9000)",
    "; [caps] ai-census tick=9600 hw: conn_trial=251/100 conn_base=185/100 mine_roster=60/32 mine_qual=8/32 mine_yield=8/32 engage_scratch=370/256 scan_targets_a=370/1024 scan_targets_b=0/1024 guard=0x0",
]
GOOD_NEG_NET = list(check_cap_fill.GOOD_NET) + [
    "; [caps] ai-scratch layer OFF ([caps] ai_scratch=0) -- the raised AI runs on the vanilla scratch arrays (negative arm)"
]
HAM = "; CAP AIHAMMER step=4400 player 1: 370 live buildings of player 0 fed to the game's adders -> engage_scratch count 370 (vanilla 256), scan_targets count 370 (vanilla 1024); bytes changed after the VANILLA engage scratch (site candidates @00FB59B0, restored): %d"


def selftest():
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    def has(bad, tag):
        return any(tag in b for b in bad)

    H = check_cap_fill.GOOD_HARNESS + [HAM % 0]
    expect("the good logs pass", problems(GOOD_NET, H) == [])
    expect(
        "a GUARD HIT goes red",
        has(
            problems(
                GOOD_NET + ["; [caps] AI GUARD HIT array=mine_roster tick=9: guard byte +0"], H
            ),
            "clause 7",
        ),
    )
    expect(
        "a non-zero guard in the last census goes red",
        has(problems([ln.replace("guard=0x0", "guard=0x4") for ln in GOOD_NET], H), "guard=0x4"),
    )
    expect(
        "no census line goes red",
        has(problems([ln for ln in GOOD_NET if "ai-census tick" not in ln], H), "no `ai-census`"),
    )
    expect(
        "a layer that never went ON goes red",
        has(problems([ln for ln in GOOD_NET if "ai-scratch ON" not in ln], H), "never went ON"),
    )
    for a in ("conn_base", "conn_trial", "mine_roster", "engage_scratch"):
        expect(
            "%s never past the vanilla bound goes red" % a,
            has(problems([ln for ln in GOOD_NET if "ai-census: %s" % a not in ln], H), a),
        )
    expect(
        "an unstaged layer goes red",
        has(problems([ln for ln in GOOD_NET if "STAGED" not in ln], H), "STAGED"),
    )
    expect(
        "a REFUSED banner goes red",
        has(problems(GOOD_NET + ["; [caps] ai-scratch layer REFUSED: x"], H), "REFUSED"),
    )
    expect(
        "no hammer line goes red", has(problems(GOOD_NET, check_cap_fill.GOOD_HARNESS), "AIHAMMER")
    )
    expect(
        "an engage list that did not pass 256 goes red",
        has(
            problems(
                GOOD_NET,
                check_cap_fill.GOOD_HARNESS
                + [(HAM % 0).replace("engage_scratch count 370", "engage_scratch count 200")],
            ),
            "not past the vanilla 256",
        ),
    )
    expect(
        "bytes changed behind the enlarged array go red",
        has(problems(GOOD_NET, check_cap_fill.GOOD_HARNESS + [HAM % 12]), "absorbed"),
    )
    neg_v = [
        ln.replace("bldg=140(hi 140)", "bldg=250(hi 250)").replace(
            "mine=40(hi 40)", "mine=60(hi 60)"
        )
        for ln in check_cap_fill.GOOD_HARNESS
    ]
    neg_h = neg_v + [HAM % 1840]
    expect(
        "the negative arm passes on its banner, the bad-index census and the clobbered neighbour",
        problems(GOOD_NEG_NET, neg_h, True) == [],
    )
    expect(
        "the negative arm refuses a staged layer",
        has(problems(GOOD_NEG_NET + ["; [caps] ai-scratch ON (x)"], neg_h, True), "negative arm"),
    )
    expect(
        "the negative arm without its OFF banner goes red",
        has(problems(check_cap_fill.GOOD_NET, neg_h, True), "OFF"),
    )
    expect(
        "the negative arm with an intact neighbour goes red (the overflow showed nothing)",
        has(problems(GOOD_NEG_NET, neg_v + [HAM % 0], True), "did not land"),
    )
    print("check_cap_ai --selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    neg = "--negative" in argv
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    return run(args[0], neg)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
