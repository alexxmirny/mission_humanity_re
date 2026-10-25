#!/usr/bin/env python3
"""
tools/check_cap_ai_units.py -- patching:PT-CAP9-U3: the verdict of the unit-indexed AI-scratch row (cap_ai_units / cap_ai_units_off).

The stimulus is the harness unit-fill fixture pointed at the AI player (capu_player=1, capu_issue=1) with `[caps] raise=1`, libmh.dll
absent: the AI owns 260 units -- past the vanilla 128 attack candidates and 100 group members -- and keeps playing for 10 game minutes
(its opponent's buildings are kept standing by capu_guard, because the retail attack tasks fault on "no hostile player with a standing
building"). Every capu sample logs `; CAPU AI ...`: the AI's unit count and its group sizes. `capu_ai_hammer` then gathers the army into
the holding pen with the game's own group-transfer primitive and drives the game's OWN scratch writers with it
(llm_strat_ai_attack_candidate_add for every unit, llm_strat_ai_group_task_disperse_passable for the whole pen), because the natural AI
groups never exceed ~100 members. The two arrays (src/patcher/cap_v022_aiscratch_EN.mh.patch.json: ai_group_list int[100] and attack_cand
16 B x 128, both unbounded in the game) live in `.mhcapa` behind guard bytes that mh.dll checks every sim step.

Clauses (default arm, all must hold):
  1  the AI layer was STAGED (`[caps] ai-scratch layer STAGED (<n> sites, <n> arrays`) and went ON with the raise; nothing REFUSED.
  2  `; CAPU VERDICT ok=1`, no `; CAPU FAIL`; the START line shows the wide units row (500); the process kept stepping after the hammer.
  3  the AI owned more than 128 units (the per-sample `CAPU AI units=` series is printed); at least --units (default 200).
  4  `; CAPU AIHAMMER`: every unit fed (candidate count == units fed, > 128), the whole pen copied (group scratch count > 100), and NOT ONE byte
     changed after either VANILLA array (the enlarged arrays absorbed it).
  5  no `AI GUARD HIT`, and the last `ai-census` line reads `guard=0x0`; the census saw ai_group_list past its vanilla 100 and attack_cand past 128.
  6  no `*** DESYNC`.
  `--negative` (the pre-fix arm, `[caps] ai_scratch=0`): the layer is OFF (`ai-scratch layer OFF`, no `ai-scratch ON`) and the same stimulus hits the
     VANILLA arrays: the AI owns > 128 units, the hammer's bytes after the vanilla arrays CHANGED (the group list ran into its neighbour) and the
     candidate count is not the units fed (record 128 of the vanilla array overlays the count itself) -- the census of the bad index.

  python tools/check_cap_ai_units.py [--negative] [--units N] <run-dir>
  python tools/check_cap_ai_units.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import check_storage_purge  # noqa: E402

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
AI_RE = re.compile(
    r"; CAPU AI step=(?P<step>\d+) player (?P<p>\d+) units=(?P<units>\d+) ai_enabled=(?P<en>\d+) groups=(?P<g>\d+) members:(?P<rest>.*)"
)
BIG_RE = re.compile(r"biggest=g(?P<g>-?\d+):(?P<n>\d+)")
HAMMER_RE = re.compile(
    r"; CAPU AIHAMMER step=(?P<step>\d+) player (?P<p>\d+): (?P<gathered>\d+) units gathered into the holding pen; (?P<fed>\d+) live units fed to attack_candidate_add -> "
    r"candidate count (?P<cand>-?\d+) \(vanilla 128\); largest AI group g(?P<g>-?\d+) has (?P<bm>\d+) members -> group scratch count (?P<copied>-?\d+) \(vanilla 100\); "
    r"bytes changed after the VANILLA attack array .*?: (?P<ca>\d+), after the VANILLA group list .*?: (?P<cb>\d+)"
)
VERDICT_RE = re.compile(r"; CAPU VERDICT ok=1 step=(?P<step>\d+) units=(?P<units>\d+)")
START_RE = re.compile(r"; CAPU step=\d+ START .* row units=(?P<u>\d+) soldiers=(?P<s>\d+)")
SAMPLE_RE = re.compile(r"; CAPU step=(?P<step>\d+) SAMPLE ")
GUARD_RE = re.compile(
    r"; CAPU step=(?P<step>\d+) GUARD player=(?P<p>\d+) walked=(?P<w>\d+) bldg_row=(?P<row>\d+)"
)
CENSUS_RE = re.compile(
    r"\[caps\] ai-census tick=(?P<tick>\d+) hw:(?P<hw>.*?) guard=0x(?P<guard>[0-9A-Fa-f]+)"
)
PAST_RE = re.compile(
    r"\[caps\] ai-census: (?P<name>\w+) element (?P<el>\d+) is past the vanilla (?P<stock>\d+)"
)
ENTRY_RE = re.compile(r"(\w+)=(\d+)/(\d+)")
REQUIRED_PAST = ("ai_group_list", "attack_cand")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


def ai_series(harness):
    out = []
    for ln in harness:
        m = AI_RE.match(ln)
        if m:
            b = BIG_RE.search(m.group("rest"))
            out.append(
                (
                    int(m.group("step")),
                    int(m.group("units")),
                    int(m.group("en")),
                    int(b.group("n")) if b else 0,
                )
            )
    return out


def hammer(harness):
    last = None
    for ln in harness:
        m = HAMMER_RE.match(ln)
        if m:
            last = {k: int(v) for k, v in m.groupdict().items()}
    return last


def census(net):
    last = None
    past = {}
    for ln in net:
        m = CENSUS_RE.search(ln)
        if m:
            last = (
                int(m.group("tick")),
                {n: (int(h), int(s)) for n, h, s in ENTRY_RE.findall(m.group("hw"))},
                int(m.group("guard"), 16),
            )
        m = PAST_RE.search(ln)
        if m:
            past[m.group("name")] = (int(m.group("el")), int(m.group("stock")))
    return last, past


def common(net, harness, units_min):
    bad = []
    bad += ["clause 2: " + ln for ln in harness if ln.startswith("; CAPU FAIL")]
    start = [m for m in (START_RE.match(ln) for ln in harness) if m]
    if not start:
        bad.append("clause 2: no `; CAPU ... START` line")
    elif int(start[0].group("u")) != 500:
        bad.append("clause 2: START units row %s, expected the wide 500" % start[0].group("u"))
    if not any(VERDICT_RE.match(ln) for ln in harness):
        bad.append(
            "clause 2: no `; CAPU VERDICT ok=1` (the fixture never reached its target, or the process died)"
        )
    ser = ai_series(harness)
    if not ser:
        bad.append(
            "clause 3: no `; CAPU AI` line (the player is not an AI, or the sample never ran)"
        )
    else:
        if any(en == 0 for _s, _u, en, _b in ser):
            bad.append("clause 3: a `CAPU AI` sample reads ai_enabled=0 (capu_player is not an AI)")
        top = max(u for _s, u, _e, _b in ser)
        if top <= 128 or top < units_min:
            bad.append(
                "clause 3: the AI owned at most %d units (need > 128 and >= %d)" % (top, units_min)
            )
    bad += ["clause 6: " + ln for ln in net + harness if "*** DESYNC" in ln]
    # clause 7 (G369): the harness's building guard walks exactly the buildings row (records 1..row-1), never a unit-sized stride past it
    guards = [m for m in (GUARD_RE.match(ln) for ln in harness) if m]
    if not guards:
        bad.append("clause 7: no `; CAPU ... GUARD` line (the building guard never sampled)")
    for g in guards:
        if int(g.group("w")) != int(g.group("row")) - 1:
            bad.append(
                "clause 7: the building guard walked %s records of a %s-record row (step %s)"
                % (g.group("w"), g.group("row"), g.group("step"))
            )
            break
    return bad


def problems(net, harness, negative=False, units_min=200):
    net, harness = strip(net), strip(harness)
    bad = common(net, harness, units_min)
    h = hammer(harness)
    if h is None:
        bad.append("clause 4: no `; CAPU AIHAMMER` line (the stimulus never ran)")
    if negative:
        if not any("[caps] ai-scratch layer OFF" in ln for ln in net):
            bad.append(
                "clause 1: no `ai-scratch layer OFF` banner (the negative arm ran WITH the fix)"
            )
        if any("ai-scratch ON" in ln or "ai-scratch layer STAGED" in ln for ln in net):
            bad.append("clause 1: the AI layer was staged / switched ON in the negative arm")
        if h is not None:
            if h["cb"] <= 0:
                bad.append(
                    "clause 4: no byte after the VANILLA group list changed -- the overflow did not land (the negative arm shows nothing)"
                )
            if h["cand"] == h["fed"] and h["ca"] == 0:
                bad.append(
                    "clause 4: the vanilla attack array took %d candidates without a trace (the negative arm shows nothing)"
                    % h["cand"]
                )
        return bad
    if not any(
        re.search(r"\[caps\] ai-scratch layer STAGED \(\d+ sites, \d+ arrays", ln) for ln in net
    ):
        bad.append("clause 1: no `[caps] ai-scratch layer STAGED (<n> sites, <n> arrays` banner")
    if not any("[caps] ai-scratch ON" in ln for ln in net):
        bad.append(
            "clause 1: no `[caps] ai-scratch ON` (the AI layer never went ON with the raise)"
        )
    bad += ["clause 1: " + ln for ln in net if "[caps]" in ln and "REFUSED" in ln]
    if h is not None:
        if h["fed"] <= 128 or h["cand"] != h["fed"]:
            bad.append(
                "clause 4: %d units fed, candidate count %d (all of them must be kept, past the vanilla 128)"
                % (h["fed"], h["cand"])
            )
        if h["copied"] <= 100:
            bad.append(
                "clause 4: the group scratch took %d ids, not past the vanilla 100" % h["copied"]
            )
        if h["ca"] != 0 or h["cb"] != 0:
            bad.append(
                "clause 4: %d / %d bytes after the vanilla attack array / group list changed with the AI layer ON"
                % (h["ca"], h["cb"])
            )
        later = [int(m.group("step")) for m in (SAMPLE_RE.match(ln) for ln in harness) if m]
        if not later or max(later) <= h["step"]:
            bad.append(
                "clause 2: no SAMPLE after the hammer step %d (the game stopped stepping)"
                % h["step"]
            )
    bad += ["clause 5: " + ln for ln in net if "AI GUARD HIT" in ln]
    last, past = census(net)
    if last is None:
        bad.append(
            "clause 5: no `ai-census` line (the guard check never ran; the match did not step with the layer ON)"
        )
        return bad
    if last[2] != 0:
        bad.append("clause 5: the last ai-census reads guard=0x%X" % last[2])
    for a in REQUIRED_PAST:
        if a not in past:
            bad.append(
                "clause 5: the census never saw %s past the vanilla bound (hw %s)"
                % (a, last[1].get(a))
            )
    return bad


def run(run_dir, negative, units_min):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, negative, units_min)
    ser = ai_series(strip(harness))
    if ser:
        print(
            "  AI units per sample: "
            + " ".join("%d@%d(g%d)" % (u, s, b) for s, u, _e, b in ser[:40])
        )
    h = hammer(strip(harness))
    if h:
        print(
            "  AIHAMMER step=%d: gathered %d, fed %d -> candidates %d, pen %d -> scratch %d, clobbered after vanilla arrays: %d / %d"
            % (
                h["step"],
                h["gathered"],
                h["fed"],
                h["cand"],
                h["bm"],
                h["copied"],
                h["ca"],
                h["cb"],
            )
        )
    last, past = census(strip(net))
    if last and not negative:
        print(
            "  ai-census tick=%d guard=0x%X: %s"
            % (
                last[0],
                last[2],
                " ".join("%s=%d/%d" % (n, hw, s) for n, (hw, s) in last[1].items()),
            )
        )
        print(
            "  past the vanilla bound: %s"
            % (
                ", ".join("%s (%d > %d)" % (n, past[n][0], past[n][1]) for n in sorted(past))
                or "none"
            )
        )
    for b in bad:
        print("  check_cap_ai_units: " + b)
    print(
        "check_cap_ai_units: %s%s"
        % ("FAIL" if bad else "PASS", " (negative arm)" if negative else "")
    )
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] ai-scratch layer STAGED (204 sites, 10 arrays, .mhcapa=0x03170000); written with the raise",
    "; [caps] ai-scratch ON (gameplay settings): 204 sites -> .mhcapa 03170000, 10 arrays enlarged, guards armed",
    "; [caps] ai-census: ai_group_list element 260 is past the vanilla 100 -- a vanilla layout would have overflowed here (tick 700)",
    "; [caps] ai-census: attack_cand element 260 is past the vanilla 128 -- a vanilla layout would have overflowed here (tick 700)",
    "; [caps] ai-census tick=9600 hw: conn_trial=0/100 ai_group_list=260/100 attack_cand=260/128 guard=0x0",
]
GOOD_NEG_NET = [
    "; [caps] ai-scratch layer OFF ([caps] ai_scratch=0) -- the raised AI runs on the vanilla scratch arrays (negative arm)"
]
HAM = (
    "; CAPU AIHAMMER step=668 player 1: 120 units gathered into the holding pen; 260 live units fed to attack_candidate_add -> candidate count %d (vanilla 128); "
    "largest AI group g0 has 260 members -> group scratch count 260 (vanilla 100); bytes changed after the VANILLA attack array (@01022B24, restored): %d, "
    "after the VANILLA group list (@00E6A060, restored): %d"
)
GOOD_HARNESS = [
    "; CAPU step=338 START player 1 proto 34 crewed proto 3 (crew 1) target=260 units / 10 soldiers batch=20 row units=500 soldiers=500",
    "; CAPU AI step=668 player 1 units=260 ai_enabled=1 groups=6 members: g0=12(goal 1) g4=100(goal 6) biggest=g4:100 attack_cand_count=0 group_scratch_count=0 p0:bldg=3,hdr=1,st=7,rel=-1",
    HAM % (260, 0, 0),
    "; CAPU step=2000 SAMPLE units=259(hi 260) soldiers=24(hi 24) issued=259 met=0 ovf_digest=1 row units=500 soldiers=500",
    "; CAPU VERDICT ok=1 step=2368 units=259(hi 260) soldiers=24(hi 24) issued=259 hw_units=260 hw_soldiers=24",
    "; CAPU step=2000 GUARD player=0 walked=499 bldg_row=500",
]
NEG_HARNESS = [
    GOOD_HARNESS[0],
    GOOD_HARNESS[1],
    HAM % (120, 0, 52),
    GOOD_HARNESS[3],
    GOOD_HARNESS[4],
    GOOD_HARNESS[5],
]


def selftest():
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    def has(bad, tag):
        return any(tag in b for b in bad)

    expect("the good logs pass", problems(GOOD_NET, GOOD_HARNESS) == [])
    expect(
        "a GUARD HIT goes red",
        has(
            problems(
                GOOD_NET + ["; [caps] AI GUARD HIT array=attack_cand tick=9: guard byte +0"],
                GOOD_HARNESS,
            ),
            "clause 5",
        ),
    )
    expect(
        "a non-zero guard in the last census goes red",
        has(
            problems([ln.replace("guard=0x0", "guard=0x200") for ln in GOOD_NET], GOOD_HARNESS),
            "guard=0x200",
        ),
    )
    expect(
        "no census line goes red",
        has(
            problems([ln for ln in GOOD_NET if "ai-census tick" not in ln], GOOD_HARNESS),
            "no `ai-census`",
        ),
    )
    expect(
        "a layer that never went ON goes red",
        has(
            problems([ln for ln in GOOD_NET if "ai-scratch ON" not in ln], GOOD_HARNESS),
            "never went ON",
        ),
    )
    for a in REQUIRED_PAST:
        expect(
            "%s never past the vanilla bound goes red" % a,
            has(
                problems([ln for ln in GOOD_NET if "ai-census: %s" % a not in ln], GOOD_HARNESS), a
            ),
        )
    expect(
        "an unstaged layer goes red",
        has(problems([ln for ln in GOOD_NET if "STAGED" not in ln], GOOD_HARNESS), "STAGED"),
    )
    expect(
        "a REFUSED banner goes red",
        has(problems(GOOD_NET + ["; [caps] ai-scratch layer REFUSED: x"], GOOD_HARNESS), "REFUSED"),
    )
    expect(
        "no hammer line goes red",
        has(problems(GOOD_NET, [ln for ln in GOOD_HARNESS if "AIHAMMER" not in ln]), "AIHAMMER"),
    )
    expect(
        "a hammer that lost candidates goes red",
        has(
            problems(GOOD_NET, GOOD_HARNESS[:2] + [HAM % (120, 0, 0)] + GOOD_HARNESS[3:]),
            "candidate count 120",
        ),
    )
    expect(
        "a changed byte after a vanilla array goes red",
        has(
            problems(GOOD_NET, GOOD_HARNESS[:2] + [HAM % (260, 0, 4)] + GOOD_HARNESS[3:]),
            "bytes after",
        ),
    )
    expect(
        "an AI with 100 units goes red",
        has(
            problems(GOOD_NET, [ln.replace("units=260", "units=100") for ln in GOOD_HARNESS]),
            "clause 3",
        ),
    )
    expect(
        "a human capu_player (ai_enabled=0) goes red",
        has(
            problems(GOOD_NET, [ln.replace("ai_enabled=1", "ai_enabled=0") for ln in GOOD_HARNESS]),
            "ai_enabled=0",
        ),
    )
    expect(
        "no verdict goes red",
        has(problems(GOOD_NET, [ln for ln in GOOD_HARNESS if "VERDICT" not in ln]), "VERDICT"),
    )
    expect(
        "a CAPU FAIL goes red",
        has(problems(GOOD_NET, GOOD_HARNESS + ["; CAPU FAIL step=9: x"]), "CAPU FAIL"),
    )
    expect(
        "a building guard that walked past its row goes red (G369)",
        has(
            problems(GOOD_NET, [ln.replace("walked=499", "walked=4705") for ln in GOOD_HARNESS]),
            "clause 7",
        ),
    )
    expect(
        "no GUARD line goes red",
        has(problems(GOOD_NET, [ln for ln in GOOD_HARNESS if "GUARD" not in ln]), "clause 7"),
    )
    expect(
        "a desync goes red",
        has(problems(GOOD_NET + ["*** DESYNC at step 5"], GOOD_HARNESS), "clause 6"),
    )
    expect(
        "no SAMPLE after the hammer goes red",
        has(
            problems(GOOD_NET, [ln for ln in GOOD_HARNESS if "SAMPLE" not in ln]),
            "stopped stepping",
        ),
    )
    expect(
        "the negative arm passes on its banner and the clobbered neighbour",
        problems(GOOD_NEG_NET, NEG_HARNESS, negative=True) == [],
    )
    expect(
        "the negative arm refuses a staged layer",
        has(problems(GOOD_NET, NEG_HARNESS, negative=True), "staged"),
    )
    expect(
        "the negative arm without its OFF banner goes red",
        has(problems([], NEG_HARNESS, negative=True), "OFF"),
    )
    expect(
        "the negative arm with an intact neighbour goes red",
        has(
            problems(
                GOOD_NEG_NET,
                GOOD_HARNESS[:2] + [HAM % (260, 0, 0)] + GOOD_HARNESS[3:],
                negative=True,
            ),
            "shows nothing",
        ),
    )
    print("check_cap_ai_units --selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    negative = "--negative" in argv
    units = 200
    if "--units" in argv:
        units = int(argv[argv.index("--units") + 1])
    dirs = [
        a
        for i, a in enumerate(argv)
        if not a.startswith("--") and (i == 0 or argv[i - 1] != "--units")
    ]
    if not dirs:
        print(__doc__)
        return 2
    rc = 0
    for d in dirs:
        rc |= run(d, negative, units)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
