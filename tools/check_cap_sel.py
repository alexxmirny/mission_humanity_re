#!/usr/bin/env python3
"""
tools/check_cap_sel.py -- patching:PT-CAP9-U6: the verdict of the selection-cap row (cap_sel / cap_sel_off).

The game keeps the selection (control group 0) and the nine control groups in `llm_strat_ctrl_group[10]`: a record is `int count` + `ushort unit_ids[200]`
(404 bytes), saved with the world. `llm_strat_unit_ctrlgroup_add_member` (0x00449401) appends with NO bound, so with the units raise (500 per player) a
box select over more than 200 own units writes past the record into the next group's bytes. The user's ruling (2026-10-21): the 200 cap stays; the append
is clamped (the code splice in src/patcher/cap_v022_aiscratch_EN.mh.patch.json, with libmh absent). The stimulus: the harness unit-fill fixture gives the
human ~440 units and plants a pattern in control groups 1 and 2 (`; CAPSEL plant`); tools/uiscripts/cap_sel_solo.txt drags a box over all of them and
right-clicks the ground once; the harness saves and reloads the world (cap_save_at).

Clauses (default arm, all must hold):
  1  `[caps] ai-scratch layer STAGED` and `ai-scratch ON` (the layer that carries the clamp), nothing REFUSED; `; CAPU VERDICT ok=1` with more than
     --units (default 300) live units, no `; CAPU FAIL`.
  2  the box select: the `; [sel] box` line shows exactly 200 selected ids (and no id twice), and every `; CAPSEL sample` after it has g0 <= 200.
  3  GUARD: the planted records after group 0 are byte-identical in EVERY `; CAPSEL` line (broken=0, g1/g2 counts unchanged).
  4  the 200-unit order: `; [goal] order` lists 200 units, every one with a goal tile, and they are not the `before_order` goals.
  5  save/load: `; CAP SAVE` / `; CAP LOAD` rc=1 and `; CAPSEL SAVEGROUPS before` == `after` (the ten records round-trip).
  6  no `*** DESYNC`, the game kept stepping after the verdict.
  `--negative` (`[caps] ai_scratch=0`: the clamp is out, the retail append): the layer is OFF; the selection count passed 200 and the records after
     group 0 were overwritten (broken > 0) -- the census of the bad write.

  python tools/check_cap_sel.py [--negative] [--units N] <run-dir>
  python tools/check_cap_sel.py --selftest
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import check_storage_purge  # noqa: E402

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
SEL_RE = re.compile(
    r"; \[sel\] (?P<label>\S+) cam=.*? n=(?P<n>-?\d+) bldg=(?P<b>-?\d+) ids=(?P<ids>[\d,]*)"
)
GOAL_RE = re.compile(r"; \[goal\] (?P<label>\S+) n=(?P<n>\d+) goals=(?P<g>[\d:,]*)")
CAPSEL_RE = re.compile(
    r"; CAPSEL (?P<tag>plant|sample) step=(?P<step>\d+) g0=(?P<g0>-?\d+) g1=(?P<g1>-?\d+) g2=(?P<g2>-?\d+) planted=(?P<pl>\d) broken=(?P<br>\d+) digest=(?P<d>[0-9A-F]+)"
)
GROUPS_RE = re.compile(
    r"; CAPSEL SAVEGROUPS (?P<when>before|after) step=\d+ digest=(?P<d>[0-9A-F]+)"
)
SAVE_RE = re.compile(r"; CAP (?P<what>SAVE|LOAD) step=\d+ .*?rc=(?P<rc>-?\d+)")
VERDICT_RE = re.compile(r"; CAPU VERDICT ok=1 step=(?P<step>\d+) units=(?P<units>\d+)")
SAMPLE_RE = re.compile(r"; CAPU step=(?P<step>\d+) SAMPLE ")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


def video_lines(run_dir):
    """mh_video.log of the run (session) dir and of the process dir its session.json names."""
    dirs = [run_dir]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            dirs.append(os.path.join(os.path.dirname(os.path.abspath(run_dir).rstrip("\\/")), pd))
    out = []
    for d in dirs:
        p = os.path.join(d, "mh_video.log")
        if os.path.isfile(p):
            out += open(p, encoding="utf-8", errors="replace").read().splitlines()
    return out


def sels(video):
    out = {}
    for ln in video:
        m = SEL_RE.match(ln)
        if m:
            ids = [int(x) for x in m.group("ids").split(",") if x]
            out[m.group("label")] = (int(m.group("n")), ids)
    return out


def goals(video):
    out = {}
    for ln in video:
        m = GOAL_RE.match(ln)
        if m:
            g = [tuple(int(v) for v in x.split(":")) for x in m.group("g").split(",") if x]
            out[m.group("label")] = (int(m.group("n")), g)
    return out


def capsel(harness):
    return [
        {k: (v if k in ("tag", "d") else int(v)) for k, v in m.groupdict().items()}
        for m in (CAPSEL_RE.match(ln) for ln in harness)
        if m
    ]


def problems(net, harness, video, negative=False, units_min=300):
    net, harness, video = strip(net), strip(harness), strip(video)
    bad = []
    bad += ["clause 1: " + ln for ln in harness if ln.startswith("; CAPU FAIL")]
    v = None
    for ln in harness:
        m = VERDICT_RE.match(ln)
        if m:
            v = {k: int(x) for k, x in m.groupdict().items()}
    if v is None:
        bad.append(
            "clause 1: no `; CAPU VERDICT ok=1` (the fixture never reached its target, or the process died)"
        )
    elif v["units"] < units_min:
        bad.append("clause 1: %d live units < the asked %d" % (v["units"], units_min))
    cs = capsel(harness)
    if not any(c["tag"] == "plant" for c in cs):
        bad.append("clause 3: no `; CAPSEL plant` line (capu_sel never planted the guard records)")
    sel = sels(video)
    box = sel.get("box")
    if box is None:
        bad.append("clause 2: no `; [sel] box` line (the script's box select never logged)")
    bad += ["clause 6: " + ln for ln in net + harness if "*** DESYNC" in ln]
    if negative:
        if not any("[caps] ai-scratch layer OFF" in ln for ln in net):
            bad.append(
                "clause 1: no `ai-scratch layer OFF` banner (the negative arm ran WITH the clamp)"
            )
        if any("ai-scratch ON" in ln or "ai-scratch layer STAGED" in ln for ln in net):
            bad.append(
                "clause 1: the AI layer (which carries the clamp) was staged / switched ON in the negative arm"
            )
        sam = [c for c in cs if c["tag"] == "sample"]
        if not sam or max(c["g0"] for c in sam) <= 200:
            bad.append(
                "clause 2: the selection count never passed 200 (%s) -- the box select did not select past the record"
                % (max((c["g0"] for c in sam), default=None),)
            )
        if not sam or max(c["br"] for c in sam) == 0:
            bad.append(
                "clause 3: the records after group 0 were never overwritten -- the retail append showed nothing (the negative arm shows nothing)"
            )
        return bad
    if not any(
        re.search(r"\[caps\] ai-scratch layer STAGED \(\d+ sites, \d+ arrays", ln) for ln in net
    ):
        bad.append(
            "clause 1: no `[caps] ai-scratch layer STAGED` banner (the layer that carries the clamp)"
        )
    if not any("[caps] ai-scratch ON" in ln for ln in net):
        bad.append("clause 1: no `[caps] ai-scratch ON` (the clamp never went ON with the raise)")
    bad += ["clause 1: " + ln for ln in net if "[caps]" in ln and "REFUSED" in ln]
    if box is not None:
        n, ids = box
        if n != 200 or len(ids) != 200:
            bad.append(
                "clause 2: the box select shows n=%d with %d ids, expected exactly 200"
                % (n, len(ids))
            )
        if len(set(ids)) != len(ids):
            bad.append("clause 2: the selection holds an id twice")
    sam = [c for c in cs if c["tag"] == "sample"]
    if not sam:
        bad.append("clause 3: no `; CAPSEL sample` line")
    for c in sam + [c for c in cs if c["tag"] == "plant"]:
        if c["br"] != 0 or c["g1"] != 5 or c["g2"] != 4:
            bad.append(
                "clause 3: step %d: the planted records after group 0 changed (broken=%d g1=%d g2=%d)"
                % (c["step"], c["br"], c["g1"], c["g2"])
            )
            break
    if sam and max(c["g0"] for c in sam) > 200:
        bad.append("clause 2: a CAPSEL sample read g0=%d > 200" % max(c["g0"] for c in sam))
    gl = goals(video)
    od, bo = gl.get("order"), gl.get("before_order")
    if od is None:
        bad.append("clause 4: no `; [goal] order` line")
    else:
        n, g = od
        if n != 200 or len(g) != 200:
            bad.append(
                "clause 4: the order line lists n=%d with %d goals, expected 200" % (n, len(g))
            )
        elif bo is not None and {x[0]: x[1:] for x in g} == {x[0]: x[1:] for x in bo[1]}:
            bad.append("clause 4: no selected unit's goal tile changed with the order")
        elif sum(1 for x in g if x[1:] != (0, 0)) < 200:
            bad.append(
                "clause 4: %d of the 200 selected units have no goal tile after the order"
                % (200 - sum(1 for x in g if x[1:] != (0, 0)))
            )
    sv = {m.group("what"): m for m in (SAVE_RE.search(ln) for ln in harness) if m}
    if "SAVE" not in sv or "LOAD" not in sv:
        bad.append("clause 5: no `; CAP SAVE` / `; CAP LOAD` pair (cap_save_at never fired)")
    elif sv["SAVE"].group("rc") != "1" or sv["LOAD"].group("rc") != "1":
        bad.append(
            "clause 5: save rc=%s load rc=%s" % (sv["SAVE"].group("rc"), sv["LOAD"].group("rc"))
        )
    grp = {m.group("when"): m.group("d") for m in (GROUPS_RE.match(ln) for ln in harness) if m}
    if "before" not in grp or "after" not in grp:
        bad.append("clause 5: no `; CAPSEL SAVEGROUPS before/after` pair")
    elif grp["before"] != grp["after"]:
        bad.append(
            "clause 5: the control-group records changed across the save/load round trip (%s -> %s)"
            % (grp["before"], grp["after"])
        )
    if v is not None:
        later = [int(m.group("step")) for m in (SAMPLE_RE.match(ln) for ln in harness) if m]
        if not later or max(later) <= v["step"]:
            bad.append(
                "clause 6: no SAMPLE after the verdict step %d (the game stopped stepping)"
                % v["step"]
            )
    return bad


def run(run_dir, negative, units_min):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    video = video_lines(run_dir)
    bad = problems(net, harness, video, negative, units_min)
    sel = sels(strip(video))
    for k in ("box", "after_order"):
        if k in sel:
            print("  [sel] %s: n=%d" % (k, sel[k][0]))
    gl = goals(strip(video))
    if "order" in gl:
        g = gl["order"][1]
        print(
            "  [goal] order: n=%d, distinct goal tiles %d"
            % (gl["order"][0], len({x[1:] for x in g}))
        )
    cs = capsel(strip(harness))
    if cs:
        print(
            "  CAPSEL: %d lines, max g0=%d, max broken=%d"
            % (len(cs), max(c["g0"] for c in cs), max(c["br"] for c in cs))
        )
    for b in bad:
        print("  check_cap_sel: " + b)
    print(
        "check_cap_sel: %s%s" % ("FAIL" if bad else "PASS", " (negative arm)" if negative else "")
    )
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] ai-scratch layer STAGED (204 sites, 10 arrays, .mhcapa=0x03170000); written with the raise",
    "; [caps] ai-scratch ON (gameplay settings): 204 sites -> .mhcapa 03170000, 10 arrays enlarged, guards armed",
]
NEG_NET = [
    "; [caps] ai-scratch layer OFF ([caps] ai_scratch=0) -- the raised AI runs on the vanilla scratch arrays (negative arm)"
]
IDS = ",".join(str(i) for i in range(1, 201))


def _goals(prefix, x):
    return ",".join("%d:%d:%d" % (i, x, x) for i in range(1, 201))


def harness_lines(g0=200, broken=0):
    return [
        "; CAPU VERDICT ok=1 step=900 units=440(hi 441) soldiers=24(hi 24) issued=439 hw_units=440 hw_soldiers=24",
        "; CAPSEL plant step=500 g0=0 g1=5 g2=4 planted=1 broken=0 digest=00000000AABBCCDD",
        "; CAPSEL sample step=1000 g0=%d g1=5 g2=4 planted=1 broken=%d digest=00000000AABBCCDD"
        % (g0, broken),
        "; CAPSEL SAVEGROUPS before step=4000 digest=00000000AABBCCDD",
        "; CAP SAVE step=4000 rc=1 digest=1",
        "; CAP LOAD step=4000 rc=1 digest=1",
        "; CAPSEL SAVEGROUPS after step=4000 digest=00000000AABBCCDD",
        "; CAPU step=5000 SAMPLE units=440(hi 441) soldiers=24(hi 24) issued=439 met=1 ovf_digest=1 row units=500 soldiers=500",
    ]


def video(n=200, goal_n=200, goal_x=7):
    return [
        "; [sel] box cam=0,0 gest=0 btn=0 n=%d bldg=0 ids=%s"
        % (n, ",".join(str(i) for i in range(1, n + 1))),
        "; [goal] before_order n=200 goals=%s" % _goals("b", 0),
        "; [goal] order n=%d goals=%s"
        % (goal_n, ",".join("%d:%d:%d" % (i, goal_x, goal_x) for i in range(1, goal_n + 1))),
    ]


def selftest():
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    def has(bad, tag):
        return any(tag in b for b in bad)

    expect("the good logs pass", problems(GOOD_NET, harness_lines(), video()) == [])
    expect(
        "a selection of 201 goes red",
        has(problems(GOOD_NET, harness_lines(), video(n=201)), "exactly 200"),
    )
    expect(
        "a selection of 150 goes red",
        has(problems(GOOD_NET, harness_lines(), video(n=150)), "exactly 200"),
    )
    expect(
        "a sample with g0 > 200 goes red",
        has(problems(GOOD_NET, harness_lines(g0=440), video()), "g0=440"),
    )
    expect(
        "a broken guard goes red",
        has(problems(GOOD_NET, harness_lines(broken=12), video()), "planted records"),
    )
    expect(
        "no plant line goes red",
        has(
            problems(GOOD_NET, [ln for ln in harness_lines() if "plant step" not in ln], video()),
            "plant",
        ),
    )
    expect(
        "an order that moved 199 goes red",
        has(problems(GOOD_NET, harness_lines(), video(goal_n=199)), "clause 4"),
    )
    expect(
        "an order that changed no goal goes red",
        has(problems(GOOD_NET, harness_lines(), video(goal_x=0)), "no selected unit's goal"),
    )
    expect(
        "no order line goes red",
        has(problems(GOOD_NET, harness_lines(), video()[:2]), "no `; [goal] order`"),
    )
    expect(
        "a changed group digest across save/load goes red",
        has(
            problems(
                GOOD_NET,
                [
                    ln.replace(
                        "after step=4000 digest=00000000AABBCCDD",
                        "after step=4000 digest=0000000011111111",
                    )
                    for ln in harness_lines()
                ],
                video(),
            ),
            "round trip",
        ),
    )
    expect(
        "a failed load goes red",
        has(
            problems(
                GOOD_NET,
                [
                    ln.replace("CAP LOAD step=4000 rc=1", "CAP LOAD step=4000 rc=0")
                    for ln in harness_lines()
                ],
                video(),
            ),
            "load rc=0",
        ),
    )
    expect(
        "no save pair goes red",
        has(
            problems(GOOD_NET, [ln for ln in harness_lines() if "CAP SAVE" not in ln], video()),
            "SAVE",
        ),
    )
    expect(
        "a layer that never went ON goes red",
        has(
            problems(
                [ln for ln in GOOD_NET if "ai-scratch ON" not in ln], harness_lines(), video()
            ),
            "never went ON",
        ),
    )
    expect(
        "a REFUSED banner goes red",
        has(
            problems(GOOD_NET + ["; [caps] ai-scratch layer REFUSED: x"], harness_lines(), video()),
            "REFUSED",
        ),
    )
    expect(
        "too few units goes red",
        has(
            problems(
                GOOD_NET, [ln.replace("units=440", "units=250") for ln in harness_lines()], video()
            ),
            "live units",
        ),
    )
    expect(
        "a CAPU FAIL goes red",
        has(problems(GOOD_NET, harness_lines() + ["; CAPU FAIL step=9: x"], video()), "CAPU FAIL"),
    )
    expect(
        "a desync goes red",
        has(problems(GOOD_NET + ["*** DESYNC at step 5"], harness_lines(), video()), "clause 6"),
    )
    neg_h = harness_lines(g0=440, broken=808)
    neg_v = video(n=200)
    expect(
        "the negative arm passes on its banner, g0 > 200 and the overwritten records",
        problems(NEG_NET, neg_h, neg_v, negative=True) == [],
    )
    expect(
        "the negative arm refuses a staged layer",
        has(problems(GOOD_NET, neg_h, neg_v, negative=True), "staged"),
    )
    expect(
        "the negative arm without an overwrite goes red",
        has(
            problems(NEG_NET, harness_lines(g0=440, broken=0), neg_v, negative=True),
            "shows nothing",
        ),
    )
    expect(
        "the negative arm with a clamped count goes red",
        has(
            problems(NEG_NET, harness_lines(g0=200, broken=808), neg_v, negative=True),
            "never passed 200",
        ),
    )
    print("check_cap_sel --selftest: %s" % ("FAIL (%d)" % len(fails) if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    negative = "--negative" in argv
    units = 300
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
