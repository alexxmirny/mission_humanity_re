#!/usr/bin/env python3
"""check_port_unlock.py -- mods:UNL1: the campaign unlock recheck on load / planet arrival ([net] project_gate_fix).

The port_unlock_ship / port_unlock_lib rows run two scripts. The script's `logport <label>` op writes, into mh_video.log,

  ; [port] <label> side= planet= avail= acq= list_has18= nlist= sess= pst=<32 planet statuses> [drop_row= drop_bld= drop_avail= drop_in_list=]
  ;   [port] <label> p=<player> en= rows_avail= rows_acq= rows_f3= bld=<offer list ids> rowsav=<available-not-acquired row ids>

`avail` is progress[side][21].available (unlock row 21 = V_alien_Port) and `list_has18` is "building 18 (N_alien_Port, the spaceport) is in the
side's offer list"; the drop_* fields describe the row `unlockdrop` made un-offered. Retail runs llm_progress_propagate_unlocks only on an acquire
/ New Game / MP start, so a LOAD or a planet switch leaves an unlock whose last prerequisite was met elsewhere unoffered; the recheck offers it.

port_unlock_user.txt -- the user's save 8-ayatus-rl-2 (planet 8, a RAISED-limits save), labels loaded, traveled_N / arrived_N:
  loaded         ON: avail=1 and list_has18=1 (the spaceport is offered); OFF: 0 / 0 (stock). sess=1, planet 8, player 0, a computer player enabled
  traveled_N     right after SwitchToPlanet(N), before the arrival tail
  arrived_N      after llm_strat_planet_transition_finalize: ON: no enabled player's available-row count fell and, over the pairs, the local
                 player's rose at least once (the tail offered something); OFF: every count equals its traveled_N value (stock)
port_unlock_gap.txt -- a vanilla save with the gap manufactured (`unlockdrop`), saved, quit to the menu and loaded back:
  dropped        the gap is open: drop_avail=0 drop_in_list=0 (non-vacuous: a row was found)
  reloaded       ON: drop_avail=1 drop_in_list=1 (the load put it back); OFF: 0 / 0
  gap_traveled / gap_arrived   the same on the visited planet 1 after SwitchToPlanet + a fresh `unlockdrop` for the local player AND for
                 computer player 1 (`watch 1`): open before the arrival tail (0 / 0), closed after it with the switch ON (1 / 1), still open
                 OFF (0 / 0); a computer player is judged on its unlock row alone (its offer list is refilled by its own tick)

  check_port_unlock.py --expect on|off <run-dir>
  check_port_unlock.py --selftest

Absence of the log, of every recognised probe, or of a non-vacuous precondition is RED.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_drag_box as cdb  # noqa: E402  session_dirs / STAMP

HEAD = re.compile(
    r"^; \[port\] (?P<label>\S+) side=(?P<side>\d+) planet=(?P<planet>\d+) (?P<rest>.*)$"
)
PLAYER = re.compile(r"^;\s+\[port\] (?P<label>\S+) p=(?P<p>\d+) (?P<rest>.*)$")


def kv(rest):
    return dict(re.findall(r"(\w+)=(\S*)", rest))


def parse(text):
    """({label: head}, {label: {player: rec}}, [labels in first-seen order]) -- the last line wins per key."""
    heads, players, order = {}, {}, []
    for ln in text.splitlines():
        ln = cdb.STAMP.sub("", ln.strip())
        m = HEAD.match(ln)
        if m:
            d = kv(m.group("rest"))
            d["side"], d["planet"] = int(m.group("side")), int(m.group("planet"))
            heads[m.group("label")] = d
            if m.group("label") not in order:
                order.append(m.group("label"))
            continue
        m = PLAYER.match(ln)
        if m:
            players.setdefault(m.group("label"), {})[int(m.group("p"))] = kv(m.group("rest"))
    return heads, players, order


def ids(s):
    return [int(x) for x in s.split(",") if x]


def rows_avail(players, label, p):
    try:
        return int(players[label][p]["rows_avail"])
    except (KeyError, ValueError):
        return None


def problems(heads, players, order, expect):
    out = []
    judged = 0
    on = expect == "on"
    want = "1" if on else "0"
    for lab in order:
        if heads[lab].get("sess") != "1":
            out.append(
                "%s: sess=%s -- not a single-player session, the scenario is vacuous"
                % (lab, heads[lab].get("sess"))
            )

    h = heads.get("loaded")
    if h:
        judged += 1
        if h["planet"] != 8 or h["side"] != 0:
            out.append(
                "loaded: planet=%d side=%d -- not the fixture (planet 8, player 0)"
                % (h["planet"], h["side"])
            )
        if h.get("avail") != want:
            out.append(
                "loaded: avail=%s want %s (progress[0][21].available)" % (h.get("avail"), want)
            )
        if h.get("list_has18") != want:
            out.append(
                "loaded: list_has18=%s want %s (building 18 in the offer list)"
                % (h.get("list_has18"), want)
            )
        others = [p for p in players.get("loaded", {}) if p != h["side"]]
        if not others:
            out.append(
                "loaded: no enabled computer player row -- 'computer players too' is untested"
            )
        gain = 0
        for lab in order:
            if not lab.startswith("arrived_"):
                continue
            trav = "traveled_" + lab[len("arrived_") :]
            if trav not in heads:
                out.append("%s: no matching %s probe" % (lab, trav))
                continue
            for p in sorted(set(players.get(lab, {})) | set(players.get(trav, {}))):
                a, t = rows_avail(players, lab, p), rows_avail(players, trav, p)
                if a is None or t is None:
                    out.append("%s: player %d has no row in one of the pair" % (lab, p))
                    continue
                if on and a < t:
                    out.append("%s: player %d lost available rows (%d -> %d)" % (lab, p, t, a))
                if not on and a != t:
                    out.append(
                        "%s: switch OFF but player %d's rows moved (%d -> %d) -- not stock"
                        % (lab, p, t, a)
                    )
                if on and p == h["side"]:
                    gain += a - t
            if on and heads[lab]["planet"] != heads[trav]["planet"]:
                out.append(
                    "%s: the arrival changed the planet (%d -> %d)"
                    % (lab, heads[trav]["planet"], heads[lab]["planet"])
                )
        if not any(l.startswith("arrived_") for l in order):
            out.append("no arrived_* probe -- the transition arm never ran")
        elif on and gain <= 0:
            out.append(
                "switch ON but no arrival offered the local player anything (sum of row gains %d) -- the arrival hook is untested"
                % gain
            )

    d = heads.get("dropped")
    if d:
        judged += 1
        if not d.get("drop_row") or d.get("drop_avail") != "0" or d.get("drop_in_list") != "0":
            out.append(
                "dropped: drop_row=%s avail=%s in_list=%s -- the gap was not manufactured (non-vacuous precondition)"
                % (d.get("drop_row"), d.get("drop_avail"), d.get("drop_in_list"))
            )
        r = heads.get("reloaded")
        if not r:
            out.append(
                "dropped but no `reloaded` probe (the save / quit / load leg never completed)"
            )
        else:
            if r.get("drop_row") != d.get("drop_row"):
                out.append(
                    "reloaded: drop_row=%s != dropped's %s" % (r.get("drop_row"), d.get("drop_row"))
                )
            if r.get("drop_avail") != want or r.get("drop_in_list") != want:
                out.append(
                    "reloaded: drop_avail=%s drop_in_list=%s want %s/%s (row %s, building %s)"
                    % (
                        r.get("drop_avail"),
                        r.get("drop_in_list"),
                        want,
                        want,
                        r.get("drop_row"),
                        r.get("drop_bld"),
                    )
                )
    gt, ga = heads.get("gap_traveled"), heads.get("gap_arrived")
    if gt or ga:
        judged += 1
        if not gt or not ga:
            out.append(
                "gap_traveled / gap_arrived: one of the pair is missing (the transition leg never completed)"
            )
        else:
            if gt["planet"] != 1 or ga["planet"] != 1:
                out.append(
                    "gap_traveled / gap_arrived: planets %d / %d, want the visited planet 1"
                    % (gt["planet"], ga["planet"])
                )
            if (
                not gt.get("drop_row")
                or gt.get("drop_avail") != "0"
                or gt.get("drop_in_list") != "0"
            ):
                out.append(
                    "gap_traveled: drop_row=%s avail=%s in_list=%s -- the gap was not manufactured on the arrival planet (non-vacuous precondition)"
                    % (gt.get("drop_row"), gt.get("drop_avail"), gt.get("drop_in_list"))
                )
            if ga.get("drop_row") != gt.get("drop_row"):
                out.append(
                    "gap_arrived: drop_row=%s != gap_traveled's %s"
                    % (ga.get("drop_row"), gt.get("drop_row"))
                )
            if ga.get("drop_avail") != want or ga.get("drop_in_list") != want:
                out.append(
                    "gap_arrived: drop_avail=%s drop_in_list=%s want %s/%s (row %s, building %s)"
                    % (
                        ga.get("drop_avail"),
                        ga.get("drop_in_list"),
                        want,
                        want,
                        ga.get("drop_row"),
                        ga.get("drop_bld"),
                    )
                )
            # the computer players' manufactured gaps (a `drop_*` on their `p=` line): the unlock ROW is open before the tail, closed after it only
            # with the switch ON. Judged on the row alone: a computer player's offer list is also refilled by its own tick between the two probes
            # (measured: building 27 back in player 1's list with the row still unavailable), so `drop_in_list` is informational for it.
            ai_t = {p: r for p, r in players.get("gap_traveled", {}).items() if r.get("drop_row")}
            if not ai_t:
                out.append(
                    "gap_traveled: no computer player has a manufactured gap -- 'computer players too' is untested"
                )
            for p, r in sorted(ai_t.items()):
                a = players.get("gap_arrived", {}).get(p, {})
                if r.get("drop_avail") != "0":
                    out.append(
                        "gap_traveled: player %d's unlock row %s is not open (avail=%s)"
                        % (p, r.get("drop_row"), r.get("drop_avail"))
                    )
                if a.get("drop_avail") != want:
                    out.append(
                        "gap_arrived: computer player %d: unlock row %s avail=%s want %s (building %s)"
                        % (p, a.get("drop_row"), a.get("drop_avail"), want, a.get("drop_bld"))
                    )
    if not judged:
        out.append(
            "no recognised probe (`loaded` or `dropped`) in mh_video.log -- the script never reached its first logport"
        )
    return out


def selftest():
    def lines(label, planet, avail, has18, extra="", rows=(40, 10, 5), ai=""):
        t = (
            "; [port] %s side=0 planet=%d avail=%s acq=0 list_has18=%s nlist=12 sess=1 pst=0,2,2%s\n"
            % (label, planet, avail, has18, extra)
        )
        for p, r in enumerate(rows):
            t += ";   [port] %s p=%d en=1 rows_avail=%d rows_acq=5 rows_f3=2 bld=4, rowsav=%s\n" % (
                label,
                p,
                r,
                ai if p == 1 else "",
            )
        return t

    def user(on, gain=2):
        a = "1" if on else "0"
        base = gain if on else 0
        return (
            lines("loaded", 8, a, a)
            + lines("traveled_1", 1, "0", "0")
            + lines("arrived_1", 1, "0", "0", rows=(40 + base, 10, 5))
            + lines("traveled_8", 8, a, a)
            + lines("arrived_8", 8, a, a)
        )

    def gap(on):
        a = "1" if on else "0"
        return lines(
            "dropped", 5, "0", "0", " drop_row=7 drop_bld=9 drop_avail=0 drop_in_list=0"
        ) + lines(
            "reloaded", 5, "0", "0", " drop_row=7 drop_bld=9 drop_avail=%s drop_in_list=%s" % (a, a)
        )

    def trans(on):
        a = "1" if on else "0"
        return lines(
            "gap_traveled",
            1,
            "0",
            "0",
            " drop_row=7 drop_bld=9 drop_avail=0 drop_in_list=0",
            ai=" drop_row=8 drop_bld=3 drop_avail=0 drop_in_list=0",
        ) + lines(
            "gap_arrived",
            1,
            "0",
            "0",
            " drop_row=7 drop_bld=9 drop_avail=%s drop_in_list=%s" % (a, a),
            ai=" drop_row=8 drop_bld=3 drop_avail=%s drop_in_list=%s" % (a, a),
        )

    def check(text, exp):
        h, p, o = parse(text)
        return problems(h, p, o, exp)

    assert check(user(True), "on") == [], check(user(True), "on")
    assert check(user(False), "off") == [], check(user(False), "off")
    assert check(user(False), "on"), "OFF output judged as ON must be red"
    assert check(user(True), "off"), "ON output judged as OFF must be red"
    assert check(user(True, gain=0), "on"), "ON with no arrival gain is untested -> red"
    assert check(user(True).replace("sess=1", "sess=3"), "on"), "MP session -> red"
    assert check(user(True).replace("planet=8 avail=1", "planet=7 avail=1", 1), "on"), (
        "wrong fixture planet -> red"
    )
    lost = user(True).replace("rows_avail=42 rows_acq", "rows_avail=39 rows_acq")
    assert check(lost, "on"), "a recheck that LOSES rows -> red"
    assert check(gap(True), "on") == [] and check(gap(False), "off") == []
    assert check(gap(False), "on") and check(gap(True), "off")
    nogap = lines(
        "dropped", 5, "0", "0", " drop_row=0 drop_bld=0 drop_avail=0 drop_in_list=0"
    ) + lines("reloaded", 5, "0", "0")
    assert check(nogap, "off"), "no manufactured gap -> vacuous -> red"
    assert check(gap(True).split("; [port] reloaded")[0], "on"), "no reloaded probe -> red"
    assert (
        check(gap(True) + trans(True), "on") == [] and check(gap(False) + trans(False), "off") == []
    )
    assert check(gap(True) + trans(False), "on"), "arrival tail did not close the gap -> red"
    aiopen = trans(True).replace(
        "drop_row=8 drop_bld=3 drop_avail=1 drop_in_list=1",
        "drop_row=8 drop_bld=3 drop_avail=0 drop_in_list=0",
    )
    assert check(gap(True) + aiopen, "on"), (
        "the computer player's gap left open with the switch ON -> red"
    )
    noai = (
        trans(True)
        .replace(" drop_row=8 drop_bld=3 drop_avail=0 drop_in_list=0", "")
        .replace(" drop_row=8 drop_bld=3 drop_avail=1 drop_in_list=1", "")
    )
    assert check(gap(True) + noai, "on"), "no computer-player gap manufactured -> untested -> red"
    assert check(gap(False) + trans(True), "off"), (
        "arrival closed the gap with the switch off -> red"
    )
    assert check(gap(True) + trans(True).split("; [port] gap_arrived")[0], "on"), (
        "no gap_arrived probe -> red"
    )
    assert check(gap(True) + trans(True).replace("planet=1 ", "planet=4 "), "on"), (
        "wrong arrival planet -> red"
    )
    assert check("", "on"), "empty log -> red"
    print("check_port_unlock: selftest PASS")
    return 0


def video_text(run_dir):
    text = ""
    for d in cdb.session_dirs(run_dir):
        t = cdb.video_log(d)
        if t:
            text += t
    return text


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if "--expect" not in argv:
        print(__doc__)
        return 2
    expect = argv[argv.index("--expect") + 1]
    args = [a for a in argv if not a.startswith("--") and a != expect]
    if expect not in ("on", "off") or len(args) != 1:
        print(__doc__)
        return 2
    heads, players, order = parse(video_text(args[0]))
    bad = problems(heads, players, order, expect)
    for lab in order:
        d = heads[lab]
        print(
            "  [port] %s planet=%d avail=%s list_has18=%s nlist=%s%s  players: %s"
            % (
                lab,
                d["planet"],
                d.get("avail"),
                d.get("list_has18"),
                d.get("nlist"),
                " drop(row=%s avail=%s in_list=%s)"
                % (d.get("drop_row"), d.get("drop_avail"), d.get("drop_in_list"))
                if "drop_row" in d
                else "",
                " ".join(
                    "p%d(avail=%s acq=%s bld=%d)"
                    % (p, r.get("rows_avail"), r.get("rows_acq"), len(ids(r.get("bld", ""))))
                    for p, r in sorted(players.get(lab, {}).items())
                ),
            )
        )
    for b in bad:
        print("  check_port_unlock: " + b)
    print("check_port_unlock: %s (--expect %s)" % ("RED" if bad else "PASS", expect))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
