#!/usr/bin/env python3
"""
tools/check_cap_path.py -- patching:PT-CAP9-U4: the 500-paths smoke of the v0.2.3 path-slot rework.

The stimulus is the harness unit-fill fixture (capu_*) followed by the 500-paths fixture (`path_*` keys, pathf_tick in
mh_harness/harness.cpp): player 0 owns ~490 units; they are ordered to ONE tile >= path_dist BFS tiles away in batches of path_batch
every path_gap steps; the pool is sampled every step through MH_Caps_PathCensus. libmh.dll is ABSENT.

Clauses, ON arm (`[caps] raise=1`, `path_check=1`):
  1  mh_net.log: `[caps] path pool relocated`, `[caps] path_check ARMED`-free of `path_check FAIL`, no `[caps] ... REFUSED`.
  2  mh_harness.log: `; PATH VERDICT`, no `; PATH FAIL`, limit=500.
  3  peak units holding a path >= --peak (default 400), highest slot id held >= 300, exhausted_steps == 0 (the free count never hit 0),
     violations == 0.
  4  >= 30% of the ordered units MOVED 8+ tiles from where they started (arrival is reported, not judged: 490 units leaving one base gridlock, about half do so with the raise ON or OFF); no `*** DESYNC`.
Clauses, OFF arm (--negative, `[caps] raise=0`): limit=100, peak <= 100, maxid < 100, violations == 0 (the vanilla 100-slot pool, relocated).

Save arms (`--save`, PT-CAP9-U7b / U4d): the same walk with a game_SaveGame + llm_game_load round trip IN ONE STEP once the pool is at its
peak (harness path_save=1: the first step with >= path_save_hold units holding and a held id >= path_save_id). The harness logs a `; PATH SAVE
<phase> ...` line before the save and after the load. Clauses:
  S1  both phases logged, `; PATH SAVE rc save=1 load=1`;
  S2  limit, the stock path window digest (`win`: slots 0..99 of every player), the per-player census (holding / max id / busy flags / free count) are
      EQUAL before and after, and somebody held a path at the save (mid-move);
  S3  ON arm only: the slots-past-the-window digest (`tails`), the whole-pool digest (`pool`) and the overflow digest (`ovf`) are EQUAL, the peak
      precondition held (holding >= --save-hold, max id >= --save-id), the save wrote a trailer that names the path section and the load applied it;
      (both arms: the pool invariants found 0 violations right after the load, `viol_after`);
  S4  OFF arm (--negative): the save wrote NO trailer (nothing past the window) and the window digest still survived the round trip -- the arm that
      pins the load-order bug (the pool was scattered before the path blocks were read, raise ON or OFF);
  S5  `--arrived-min N` (the no-save arm's `arrived`): the run after the round trip arrives at least that many units.

Tactical round trip (`--save --tact`, PT-CAP9-U4f; harness path_tact=1): the same `; PATH SAVE before|after` pair, but the "round trip" is a
strategic -> tactical mission -> strategic excursion (planet saved by map_SavePlanetToDisk, mission entered by mh::tact::mission_start, the mission
overwrites row 0 slots 1..99 and resets their flags, the game's own early-exit gate returns through llm_tact_mission_end_return_to_strategic =
map_LoadPlanetFromDisk). All S-clauses apply (the slots >= 100 and every digest/census survive), plus
  T1  `; PATH TACT ... ENTER ... planet_save_rc=1`, `mission_start -> mode=6`, `EXIT frames=N mode=2` all logged, N >= --tact-frames (default 10), no `PATH TACT FAIL`;
  T2  a `; PATH SAVE mid` line shows the mission DID disturb the shared structures (window digest or census differs from `before`) -- otherwise the
      excursion proved nothing about the restore;
  T3  the slots past the window were NOT disturbed by the mission (tails digest of `mid` == `before`; the tactical walks touch row 0 slots 1..99 only).

  python tools/check_cap_path.py [--negative] [--save [--tact]] [--peak N] [--save-hold N] [--save-id N] [--arrived-min N] <run-dir> [...]
  python tools/check_cap_path.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import check_storage_purge  # noqa: E402

STAMP = re.compile(r"^\[[\d:.]+\]\s*")
VERDICT_RE = re.compile(
    r"; PATH VERDICT step=(?P<step>\d+) ordered=(?P<ordered>\d+) issued=(?P<issued>\d+) arrived=(?P<arrived>\d+) moved=(?P<moved>\d+) still_pathing=(?P<still>\d+) "
    r"peak=(?P<peak>\d+) maxid=(?P<maxid>\d+) free_min=(?P<free_min>-?\d+) exhausted_steps=(?P<exh>\d+) violations=(?P<viol>\d+) limit=(?P<limit>\d+)"
)


SAVE_RE = re.compile(
    r"; PATH SAVE (?P<phase>before|after) step=(?P<step>\d+) limit=(?P<limit>\d+) win=(?P<win>[0-9A-Fa-f]{16}) tails=(?P<tails>[0-9A-Fa-f]{16}) "
    r"pool=(?P<pool>[0-9A-Fa-f]{16}) ovf=(?P<ovf>[0-9A-Fa-f]{16}) census=(?P<census>\S+)"
)
MID_RE = re.compile(
    r"; PATH SAVE mid step=(?P<step>\d+) limit=(?P<limit>\d+) win=(?P<win>[0-9A-Fa-f]{16}) tails=(?P<tails>[0-9A-Fa-f]{16}) "
    r"pool=(?P<pool>[0-9A-Fa-f]{16}) ovf=(?P<ovf>[0-9A-Fa-f]{16}) census=(?P<census>\S+)"
)
TACT_ENTER_RE = re.compile(
    r"; PATH TACT step=\d+ ENTER planet=(?P<p>-?\d+) system=(?P<s>-?\d+) squad=(?P<q>\d+) planet_save_rc=(?P<rc>\d+)"
)
TACT_START_RE = re.compile(r"; PATH TACT step=\d+ mission_start -> mode=(?P<m>\d+) ")
TACT_EXIT_RE = re.compile(r"; PATH TACT step=\d+ EXIT frames=(?P<n>\d+) mode=(?P<m>\d+)")
SAVE_RC_RE = re.compile(
    r"; PATH SAVE rc step=(?P<step>\d+) save=(?P<save>\d+) load=(?P<load>\d+) viol_after=(?P<viol>\d+)"
)
TRAILER_RE = re.compile(
    r"; \[caps\] save: planet (?P<p>-?\d+) trailer (?P<n>\d+) records \((?P<b>\d+) bytes\)(?P<rest>.*)"
)
APPLIED_RE = re.compile(r"; \[caps\] load: planet file overflow (?P<n>\d+) records applied")


def strip(lines):
    return [STAMP.sub("", ln.strip()) for ln in lines]


def census_of(text):
    """`h/m/f/c,h/m/f/c,...` (8 players) -> list of (holding, max id, flags, free)."""
    return [tuple(int(x) for x in c.split("/")) for c in text.split(",")]


def tact_problems(harness, before, frames_min):
    bad = []
    enter = [m for m in map(TACT_ENTER_RE.match, harness) if m]
    start = [m for m in map(TACT_START_RE.match, harness) if m]
    leave = [m for m in map(TACT_EXIT_RE.match, harness) if m]
    bad += ["clause T1: " + ln for ln in harness if ln.startswith("; PATH TACT FAIL")]
    if not enter or enter[-1].group("rc") != "1":
        bad.append("clause T1: no `; PATH TACT ... ENTER ... planet_save_rc=1` line")
    if not start or start[-1].group("m") != "6":
        bad.append("clause T1: the mission never started (no `mission_start -> mode=6`)")
    if not leave or leave[-1].group("m") != "2":
        bad.append(
            "clause T1: the mission never returned to the strategic mode (no `EXIT ... mode=2`)"
        )
    elif int(leave[-1].group("n")) < frames_min:
        bad.append(
            "clause T1: only %s tactical frames ran (< %d)" % (leave[-1].group("n"), frames_min)
        )
    mid = [m for m in map(MID_RE.match, harness) if m]
    if not mid:
        bad.append(
            "clause T2: no `; PATH SAVE mid` line (the mission never reached the exit frame)"
        )
        return bad
    mid = mid[-1].groupdict()
    if mid["win"] == before["win"] and mid["census"] == before["census"]:
        bad.append(
            "clause T2: the mission left the window digest AND the census untouched (the excursion proved nothing about the restore)"
        )
    if mid["tails"] != before["tails"]:
        bad.append(
            "clause T3: the mission changed the slots past the window (%s -> %s)"
            % (before["tails"], mid["tails"])
        )
    return bad


def save_problems(
    net,
    harness,
    negative=False,
    hold_min=400,
    id_min=300,
    arrived_min=0,
    tact=False,
    tact_frames=10,
):
    net, harness = strip(net), strip(harness)
    bad = []
    ph = {}
    for ln in harness:
        m = SAVE_RE.match(ln)
        if m:
            ph[m.group("phase")] = m.groupdict()
    rc = None
    for ln in harness:
        m = SAVE_RC_RE.match(ln)
        if m:
            rc = {k: int(x) for k, x in m.groupdict().items()}
    if "before" not in ph or "after" not in ph or rc is None:
        return [
            "clause S1: the `; PATH SAVE` before / after / rc lines are not all there (the fixture never fired)"
        ]
    if rc["save"] != 1 or rc["load"] != 1:
        bad.append("clause S1: save rc=%d load rc=%d (want 1 / 1)" % (rc["save"], rc["load"]))
    b, a = ph["before"], ph["after"]
    if tact:
        bad += tact_problems(harness, b, tact_frames)
    for key in ("limit", "win"):
        if b[key] != a[key]:
            bad.append(
                "clause S2: %s differs across the round trip (%s -> %s)" % (key, b[key], a[key])
            )
    cb, ca = census_of(b["census"]), census_of(a["census"])
    if cb != ca:
        bad.append(
            "clause S2: per-player census (holding/maxid/flags/free) differs across the round trip: %s -> %s"
            % (b["census"], a["census"])
        )
    if not any(c[0] for c in cb):
        bad.append("clause S2: nobody held a path at the save (not mid-move)")
    trailers = [TRAILER_RE.search(ln) for ln in net]
    trailers = [t for t in trailers if t]
    applied = [APPLIED_RE.search(ln) for ln in net]
    applied = [t for t in applied if t]
    if rc["viol"]:
        bad.append("clause S2: %d pool invariant violation(s) right after the load" % rc["viol"])
    v = None
    for ln in harness:
        m = VERDICT_RE.match(ln)
        if m:
            v = {k: int(x) for k, x in m.groupdict().items()}
    if arrived_min and v is not None and v["arrived"] < arrived_min:
        bad.append("clause S5: %d arrived < the no-save arm's %d" % (v["arrived"], arrived_min))
    if negative:
        if trailers:
            bad.append("clause S4: the OFF arm wrote a trailer: " + trailers[-1].group(0))
        return bad
    if b["tails"] != a["tails"]:
        bad.append("clause S3: slots past the window differ (%s -> %s)" % (b["tails"], a["tails"]))
    if b["pool"] != a["pool"]:
        bad.append("clause S3: the whole pool differs (%s -> %s)" % (b["pool"], a["pool"]))
    if b["ovf"] != a["ovf"]:
        bad.append("clause S3: the overflow digest differs (%s -> %s)" % (b["ovf"], a["ovf"]))
    hold = max(c[0] for c in cb)
    mid = max(c[1] for c in cb)
    if hold < hold_min:
        bad.append(
            "clause S3: the save was taken with only %d paths held (< %d)" % (hold, hold_min)
        )
    if mid < id_min:
        bad.append("clause S3: the highest id held at the save was %d (< %d)" % (mid, id_min))
    if not trailers:
        bad.append("clause S3: no `[caps] save: planet P trailer N records` line")
    elif "path" not in trailers[-1].group("rest"):
        bad.append(
            "clause S3: the trailer line does not name a path section: " + trailers[-1].group(0)
        )
    if not applied:
        bad.append("clause S3: no `[caps] load: planet file overflow N records applied` line")
    return bad


def problems(net, harness, negative=False, peak_min=400):
    net, harness = strip(net), strip(harness)
    bad = []
    if not any("[caps] path pool relocated" in ln for ln in net):
        bad.append("clause 1: no `[caps] path pool relocated` in mh_net.log")
    bad += ["clause 1: " + ln for ln in net if "[caps]" in ln and "REFUSED" in ln]
    bad += ["clause 1: " + ln for ln in net if "path_check FAIL" in ln]
    bad += ["clause 2: " + ln for ln in harness if ln.startswith("; PATH FAIL")]
    bad += ["clause 4: " + ln for ln in net + harness if "*** DESYNC" in ln]
    v = None
    for ln in harness:
        m = VERDICT_RE.match(ln)
        if m:
            v = {k: int(x) for k, x in m.groupdict().items()}
    if v is None:
        bad.append(
            "clause 2: no `; PATH VERDICT` (the fixture never finished, or the process died)"
        )
        return bad
    if v["viol"]:
        bad.append("clause 3: %d path-pool invariant violation(s)" % v["viol"])
    if negative:
        if v["limit"] != 100:
            bad.append("clause 2: limit=%d, the OFF arm expects the vanilla 100" % v["limit"])
        if v["peak"] > 100:
            bad.append("clause 3: peak %d paths held with the raise OFF (> 100)" % v["peak"])
        if v["maxid"] >= 100:
            bad.append("clause 3: slot id %d held with the raise OFF" % v["maxid"])
        if v["ordered"] == 0 or v["moved"] * 10 < v["ordered"] * 3:
            bad.append(
                "clause 4: only %d of %d ordered units moved 8+ tiles from where they started"
                % (v["moved"], v["ordered"])
            )
        return bad
    if v["limit"] != 500:
        bad.append("clause 2: limit=%d, expected 500" % v["limit"])
    if v["peak"] < peak_min:
        bad.append("clause 3: peak %d units holding a path < %d" % (v["peak"], peak_min))
    if v["maxid"] < 300:
        bad.append(
            "clause 3: highest slot id %d < 300 (the raised slots were not used)" % v["maxid"]
        )
    if v["exh"]:
        bad.append("clause 3: the free-slot count hit 0 in %d step(s) (a scanner fail)" % v["exh"])
    if v["ordered"] == 0 or v["moved"] * 10 < v["ordered"] * 3:
        bad.append(
            "clause 4: only %d of %d ordered units moved 8+ tiles from where they started"
            % (v["moved"], v["ordered"])
        )
    return bad


def run(run_dir, negative, peak_min, save=None):
    net = check_cancel_task.net_log_lines(run_dir)
    harness = check_storage_purge.harness_log_lines(run_dir)
    bad = problems(net, harness, negative, peak_min)
    if save is not None:
        bad += save_problems(net, harness, negative, **save)
    for ln in strip(harness):
        if (
            ln.startswith("; PATH VERDICT")
            or ln.startswith("; PATH SAVE")
            or ln.startswith("; PATH TACT")
            or ln.startswith("; PATH step")
            and "START" in ln
        ):
            print("  " + ln[:260])
    for ln in strip(net):
        if "trailer" in ln and "[caps] save" in ln:
            print("  " + ln[:260])
    for b in bad:
        print("  check_cap_path: " + b)
    print("check_cap_path: %s%s" % ("FAIL" if bad else "PASS", " (OFF arm)" if negative else ""))
    return 1 if bad else 0


GOOD_NET = [
    "; [caps] path pool relocated (8 players x 501 slots, .mhcapp=0x014D0000, caves .mhcapq=0x01720000)",
    "; [caps] limits ON (gameplay settings): buildings 500",
]
GOOD_HARNESS = [
    "; PATH step=400 START player 0 units=490 dest=(10,20) bfs=62 batch=100 gap=10 limit=500",
    "; PATH VERDICT step=2000 ordered=490 issued=490 arrived=480 moved=488 still_pathing=3 peak=486 maxid=499 free_min=4 exhausted_steps=0 violations=0 limit=500",
]
GOOD_OFF = [
    GOOD_HARNESS[0].replace("limit=500", "limit=100"),
    "; PATH VERDICT step=2000 ordered=99 issued=99 arrived=99 moved=99 still_pathing=0 peak=97 maxid=98 free_min=2 exhausted_steps=0 violations=0 limit=100",
]


def selftest():
    fails = []

    def expect(name, ok, detail=""):
        print("  [%s] %s" % ("ok" if ok else "FAIL", name))
        if not ok:
            fails.append(name)
            if detail:
                print("      " + str(detail))

    def has(lst, needle):
        return any(needle in x for x in lst)

    def rep(h, a, b):
        return [x.replace(a, b) for x in h]

    expect(
        "a good ON run is green",
        problems(GOOD_NET, GOOD_HARNESS) == [],
        problems(GOOD_NET, GOOD_HARNESS),
    )
    expect(
        "a good OFF run is green",
        problems(GOOD_NET, GOOD_OFF, True) == [],
        problems(GOOD_NET, GOOD_OFF, True),
    )
    expect(
        "no relocation line is RED",
        has(problems(GOOD_NET[1:], GOOD_HARNESS), "path pool relocated"),
    )
    expect(
        "a refusal is RED",
        has(problems(GOOD_NET + ["; [caps] raise REFUSED (x)"], GOOD_HARNESS), "REFUSED"),
    )
    expect(
        "a path_check FAIL is RED",
        has(problems(GOOD_NET + ["; [caps] path_check FAIL (x): y"], GOOD_HARNESS), "path_check"),
    )
    expect("no verdict is RED", has(problems(GOOD_NET, GOOD_HARNESS[:1]), "no `; PATH VERDICT"))
    expect(
        "a PATH FAIL is RED",
        has(problems(GOOD_NET, ["; PATH FAIL step=1: x"] + GOOD_HARNESS), "PATH FAIL"),
    )
    expect(
        "a low peak is RED",
        has(problems(GOOD_NET, rep(GOOD_HARNESS, "peak=486", "peak=99")), "peak 99"),
    )
    expect(
        "a low max id is RED",
        has(problems(GOOD_NET, rep(GOOD_HARNESS, "maxid=499", "maxid=99")), "highest slot id"),
    )
    expect(
        "an exhausted pool is RED",
        has(
            problems(GOOD_NET, rep(GOOD_HARNESS, "exhausted_steps=0", "exhausted_steps=5")),
            "scanner fail",
        ),
    )
    expect(
        "violations are RED",
        has(problems(GOOD_NET, rep(GOOD_HARNESS, "violations=0", "violations=2")), "violation"),
    )
    expect(
        "few arrivals are RED",
        has(problems(GOOD_NET, rep(GOOD_HARNESS, "moved=488", "moved=100")), "moved 8+"),
    )
    expect(
        "a desync is RED",
        has(problems(GOOD_NET + ["; [desync] *** DESYNC step=9"], GOOD_HARNESS), "DESYNC"),
    )
    expect(
        "OFF arm with a raised limit is RED", has(problems(GOOD_NET, GOOD_HARNESS, True), "OFF arm")
    )
    expect(
        "OFF arm with peak > 100 is RED",
        has(problems(GOOD_NET, rep(GOOD_OFF, "peak=97", "peak=150"), True), "peak 150"),
    )

    # ---- the save arms
    def census(first):
        return ",".join([first] + ["0/0/0/500"] * 7)

    def sv(phase, **kw):
        d = dict(
            step=1000,
            limit=500,
            win="1111111111111111",
            tails="2222222222222222",
            pool="3333333333333333",
            ovf="4444444444444444",
            census=census("450/450/450/50"),
        )
        d.update(kw)
        return "; PATH SAVE %s step=%d limit=%d win=%s tails=%s pool=%s ovf=%s census=%s" % (
            phase,
            d["step"],
            d["limit"],
            d["win"],
            d["tails"],
            d["pool"],
            d["ovf"],
            d["census"],
        )

    good_save_net = GOOD_NET + [
        "; [caps] save: planet 3 trailer 410 records (252000 bytes) v2 path 330",
        "; [caps] load: planet file overflow 410 records applied",
    ]
    good_save = GOOD_HARNESS + [
        sv("before"),
        sv("after"),
        "; PATH SAVE rc step=1000 save=1 load=1 viol_after=0",
    ]

    def after_with(**kw):
        return GOOD_HARNESS + [
            sv("before"),
            sv("after", **kw),
            "; PATH SAVE rc step=1000 save=1 load=1 viol_after=0",
        ]

    def sp(net=good_save_net, h=good_save, **kw):
        return save_problems(net, h, **kw)

    expect("a good save arm is green", sp() == [], sp())
    expect("no PATH SAVE lines is RED", has(sp(h=GOOD_HARNESS), "never fired"))
    expect("a failed load rc is RED", has(sp(h=rep(good_save, "load=1", "load=0")), "load rc=0"))
    expect(
        "a changed window digest is RED",
        has(sp(h=after_with(win="9999999999999999")), "win differs"),
    )
    expect(
        "a changed census is RED", has(sp(h=after_with(census=census("449/450/450/51"))), "census")
    )
    expect(
        "a changed tails digest is RED",
        has(sp(h=after_with(tails="5555555555555555")), "past the window"),
    )
    expect(
        "a changed pool digest is RED", has(sp(h=after_with(pool="5555555555555555")), "whole pool")
    )
    expect(
        "a changed ovf digest is RED",
        has(sp(h=after_with(ovf="5555555555555555")), "overflow digest"),
    )
    expect("a low-peak save is RED", has(sp(hold_min=500), "only 450 paths"))
    expect("a low max id at the save is RED", has(sp(id_min=460), "highest id held"))
    expect(
        "a trailer without a path section is RED",
        has(sp(net=[x.replace(" v2 path 330", "") for x in good_save_net]), "does not name a path"),
    )
    expect("no trailer is RED (ON arm)", has(sp(net=GOOD_NET), "no `[caps] save"))
    expect("no applied line is RED", has(sp(net=good_save_net[:-1]), "overflow N records applied"))
    expect(
        "violations after the load are RED",
        has(sp(h=rep(good_save, "viol_after=0", "viol_after=3")), "violation(s) right after"),
    )
    expect(
        "violations after the load are RED (OFF arm)",
        has(
            sp(negative=True, net=GOOD_NET, h=rep(good_save, "viol_after=0", "viol_after=3")),
            "violation(s) right after",
        ),
    )
    expect(
        "too few arrivals are RED (OFF arm)",
        has(sp(negative=True, net=GOOD_NET, arrived_min=490), "no-save arm"),
    )
    expect("too few arrivals are RED", has(sp(arrived_min=490), "no-save arm"))
    expect(
        "a good OFF save arm is green",
        sp(negative=True, net=GOOD_NET) == [],
        sp(negative=True, net=GOOD_NET),
    )
    expect("an OFF arm trailer is RED", has(sp(negative=True), "OFF arm wrote a trailer"))
    expect(
        "an OFF arm that lost the window is RED",
        has(sp(negative=True, net=GOOD_NET, h=after_with(win="0000000000000001")), "win differs"),
    )

    # ---- the tactical round trip (--save --tact)
    tact_lines = [
        "; PATH TACT step=1000 ENTER planet=3 system=1 squad=8 planet_save_rc=1",
        "; PATH TACT step=1000 mission_start -> mode=6 (mission running) active_units=12",
        sv("mid", win="7777777777777777", census=census("450/450/380/50")),
        "; PATH TACT step=1000 EXIT frames=30 mode=2",
    ]
    tact_h = (
        GOOD_HARNESS
        + [sv("before"), sv("after")]
        + tact_lines
        + ["; PATH SAVE rc step=1000 save=1 load=1 viol_after=0"]
    )

    def tp(h=tact_h, **kw):
        return sp(h=h, tact=True, **kw)

    expect("a good tactical round trip is green", tp() == [], tp())
    expect(
        "no ENTER line is RED",
        has(tp(h=[x for x in tact_h if "ENTER" not in x]), "planet_save_rc=1"),
    )
    expect(
        "a failed planet save is RED",
        has(tp(h=rep(tact_h, "planet_save_rc=1", "planet_save_rc=0")), "planet_save_rc=1"),
    )
    expect(
        "a bounced mission is RED",
        has(tp(h=rep(tact_h, "-> mode=6", "-> mode=2")), "never started"),
    )
    expect("no EXIT is RED", has(tp(h=[x for x in tact_h if "EXIT" not in x]), "never returned"))
    expect(
        "a short mission is RED",
        has(tp(h=rep(tact_h, "frames=30", "frames=3")), "tactical frames ran"),
    )
    expect(
        "a TACT FAIL is RED", has(tp(h=tact_h + ["; PATH TACT FAIL step=1: x"]), "PATH TACT FAIL")
    )
    expect(
        "a mission that touched nothing is RED",
        has(tp(h=[sv("mid") if "SAVE mid" in x else x for x in tact_h]), "proved nothing"),
    )
    expect("no mid line is RED", has(tp(h=[x for x in tact_h if "SAVE mid" not in x]), "SAVE mid"))
    expect(
        "a mission that changed the tails is RED",
        has(
            tp(
                h=[
                    sv("mid", win="7777777777777777", tails="5555555555555555")
                    if "SAVE mid" in x
                    else x
                    for x in tact_h
                ]
            ),
            "past the window",
        ),
    )
    expect(
        "a lost slot after the excursion is RED",
        has(
            tp(
                h=[sv("after", pool="5555555555555555") if "SAVE after" in x else x for x in tact_h]
            ),
            "whole pool",
        ),
    )
    print("check_cap_path --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    negative = "--negative" in argv
    save = {} if "--save" in argv else None
    ints = {}
    tact = "--tact" in argv
    argv = [a for a in argv if a != "--tact"]
    for flag in ("--peak", "--save-hold", "--save-id", "--arrived-min", "--tact-frames"):
        if flag in argv:
            i = argv.index(flag)
            ints[flag] = int(argv[i + 1])
            argv = argv[:i] + argv[i + 2 :]
    peak = ints.get("--peak", 400)
    if tact:
        if save is None:
            print("--tact needs --save")
            return 2
        save["tact"] = True
    if save is not None:
        for flag, key in (
            ("--save-hold", "hold_min"),
            ("--save-id", "id_min"),
            ("--arrived-min", "arrived_min"),
            ("--tact-frames", "tact_frames"),
        ):
            if flag in ints:
                save[key] = ints[flag]
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    return max(run(d, negative, peak, save) for d in args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
