#!/usr/bin/env python3
"""check_a1_mother_free.py -- mods:A1: build without a mothership ([net] mother_free_build).

The a1_mother_free segment of the walks a_walk / a_walk_ship / a_walk_orig (tools/uiscripts/a_walk.txt, a_walk_ship.txt; the rows a1_mother_free /
_shipped / _orig / _off before the fold) runs on 2-almost.sav; the a1_ai_on_shipped segment of a_walk_ship and the row a1_ai_off_shipped run
a1_mother_free_ai.txt; the 2-peer rows a1_mother_free_mp* run the host script a1_mother_free_mp_host.txt. The scripts' `logbldgs`
op writes `; [a1] <label> planet= primary= pmu= alive= gen= con= ... res=` lines (the watched player's primary mother, power, stock)
and `;   [a1] <label> slot= id= type= at=x,y state= flags= online= energy_x10= prog_x100= ...` lines (the buildings in a tile
rectangle) into mh_video.log; the host writes `; mother free build ON|OFF` into mh_net.log.

SOLO (default), the whole walk with the key on:
  * the setting is ON in this process (non-vacuous: absence of the banner is RED);
  * `lifted`: the mother is gone (primary_mother_bldg == 0);
  * A  a lone Reactor P1 at tile (27,47), 20 tiles from every connected building, is CONNECTED at placement (built_flags 3,
       state 100 = under construction), builds, goes online, and the planet generates >= 1000 more power than before;
  * B  a Relay at (31,47) is connected at placement (through the online plant), builds and goes online;
  * C  a second lone Reactor P2 at (19,23) is connected at placement; the Relay is then REMOVED (state DISMANTLE_FINISH ->
       unmap_footprint -> power_network_recompute, which clears every connection bit and re-seeds only ONLINE plants); P2, still under
       construction, is still connected, its progress did not go back, and it finishes online; P1 is connected and online throughout;
  * D  after save / quit / load P1 and P2 are connected and online, power generation is unchanged, and they still are 20 s later.
SOLO with the key OFF (the `_off` twin) fails the first of these on purpose: nothing is placed on a mother-less planet.

  check_a1_mother_free.py <run-dir>              the solo walk (key on)
  check_a1_mother_free.py --ai on|off <run-dir>  B+AI: the AI player (player 1) loses its mother; with the AI option ON it places new
                                                 buildings, with it OFF it places none (the dispatcher refuses its orders as retail)
  check_a1_mother_free.py --mp <run-dir>         the 2-peer row: host + client adopt the host's setting and build on a mother-less planet
  check_a1_mother_free.py --selftest
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_drag_box as cdb  # noqa: E402  session_dirs / STAMP / video_log

HEAD = re.compile(
    r"^; \[a1\] (?P<label>\S+) planet=(?P<planet>-?\d+) primary=(?P<primary>-?\d+) pmu=(?P<pmu>-?\d+) alive=(?P<alive>-?\d+)"
    r" gen=(?P<gen>-?\d+) con=(?P<con>-?\d+) .*$"
)
SLOT = re.compile(
    r"^;\s+\[a1\] (?P<label>\S+) slot=(?P<slot>\d+) id=(?P<id>\d+) type=(?P<type>\d+) at=(?P<x>\d+),(?P<y>\d+) state=(?P<state>-?\d+)"
    r" flags=(?P<flags>\d+) online=(?P<online>-?\d+) energy_x10=(?P<e>-?\d+)/(?P<emax>-?\d+) prog_x100=(?P<prog>-?\d+).*$"
)
PLANT_TYPES = (3, 0x17)
RELAY_TYPES = (15,)
P1, RELAY, P2 = (27, 47), (31, 47), (19, 23)
STATE_CONSTRUCTION = 100


def parse(text):
    """({label: head}, {label: {(x, y): slot}}) -- the last line wins per key."""
    heads, slots = {}, {}
    for ln in text.splitlines():
        ln = cdb.STAMP.sub("", ln.strip())
        m = HEAD.match(ln)
        if m:
            heads[m.group("label")] = {
                k: int(v) if k != "label" else v for k, v in m.groupdict().items()
            }
            continue
        m = SLOT.match(ln)
        if m:
            d = {k: int(v) if k != "label" else v for k, v in m.groupdict().items()}
            slots.setdefault(d["label"], {})[(d["x"], d["y"])] = d
    return heads, slots


def _b(slots, label, tile):
    return slots.get(label, {}).get(tile)


def problems_solo(heads, slots, net_on):
    out = []
    if not net_on:
        out.append(
            "no `; mother free build ON` banner: the setting is not in force in this process"
        )
    for k in (
        "start",
        "lifted",
        "a_placed",
        "a_built",
        "b_placed",
        "b_built",
        "c_placed",
        "c_removed",
        "c_built",
        "d_loaded",
        "d_later",
    ):
        if k not in heads:
            out.append("no `[a1] %s` line" % k)
    if out:
        return out
    if heads["start"]["primary"] == 0:
        out.append("start: the fixture has no primary mother to lift off (primary=0)")
    if heads["lifted"]["primary"] != 0:
        out.append(
            "lifted: primary_mother_bldg=%d after the lift-off, want 0" % heads["lifted"]["primary"]
        )
    # A: the lone plant
    p = _b(slots, "a_placed", P1)
    if not p or p["type"] not in PLANT_TYPES:
        return out + [
            "a_placed: no Reactor at tile %s (the build tab / placement was refused on a mother-less planet)"
            % (P1,)
        ]
    if p["flags"] != 3:
        out.append(
            "a_placed: the lone Reactor has built_flags=%d, want 3 (connected + staffed: it roots the network)"
            % p["flags"]
        )
    if p["state"] != STATE_CONSTRUCTION:
        out.append(
            "a_placed: Reactor state=%d, want %d (under construction)"
            % (p["state"], STATE_CONSTRUCTION)
        )
    p = _b(slots, "a_built", P1)
    if not p or p["online"] != 1 or p["state"] == STATE_CONSTRUCTION or p["flags"] != 3:
        out.append("a_built: the lone Reactor did not finish online and connected (%s)" % (p,))
    gain = heads["a_built"]["gen"] - heads["lifted"]["gen"]
    if gain < 1000:
        out.append(
            "a_built: power generated rose by %d, want >= 1000 (the Reactor's output)" % gain
        )
    # B: the neighbour
    r = _b(slots, "b_placed", RELAY)
    if not r or r["type"] not in RELAY_TYPES:
        return out + ["b_placed: no Relay at tile %s" % (RELAY,)]
    if r["flags"] != 3:
        out.append(
            "b_placed: the Relay has built_flags=%d, want 3 (connected through the plant)"
            % r["flags"]
        )
    r = _b(slots, "b_built", RELAY)
    if not r or r["online"] != 1 or r["state"] == STATE_CONSTRUCTION:
        out.append("b_built: the Relay did not finish online (%s)" % (r,))
    # C: second lone plant + a removal during its construction
    q = _b(slots, "c_placed", P2)
    if not q or q["type"] not in PLANT_TYPES:
        return out + ["c_placed: no second Reactor at tile %s" % (P2,)]
    if q["flags"] != 3 or q["state"] != STATE_CONSTRUCTION:
        out.append(
            "c_placed: P2 flags=%d state=%d, want 3 / %d"
            % (q["flags"], q["state"], STATE_CONSTRUCTION)
        )
    if not _b(slots, "c_placed", RELAY) or _b(slots, "c_placed", RELAY)["state"] == 4:
        out.append("c_placed: the Relay to remove is not alive")
    rr = _b(slots, "c_removed", RELAY)
    if rr and rr["state"] != 4 and rr["e"] > 0:
        out.append("c_removed: the Relay was not removed (state=%d)" % rr["state"])
    q2 = _b(slots, "c_removed", P2)
    if not q2 or q2["flags"] != 3:
        out.append(
            "c_removed: P2 lost its connection after the removal (flags=%s): the recompute must re-seed a plant under construction"
            % (q2 and q2["flags"],)
        )
    elif q2["prog"] < q["prog"]:
        out.append(
            "c_removed: P2's construction progress went back (%d -> %d)" % (q["prog"], q2["prog"])
        )
    q3 = _b(slots, "c_built", P2)
    if not q3 or q3["online"] != 1 or q3["state"] == STATE_CONSTRUCTION or q3["flags"] != 3:
        out.append("c_built: P2 did not finish online and connected (%s)" % (q3,))
    for lab in ("c_placed", "c_removed", "c_built"):
        a = _b(slots, lab, P1)
        if not a or a["flags"] != 3 or a["online"] != 1:
            out.append("%s: P1 is not connected and online (%s)" % (lab, a))
    # D: save / load
    for lab in ("d_loaded", "d_later"):
        for tile, name in ((P1, "P1"), (P2, "P2")):
            a = _b(slots, lab, tile)
            if not a or a["flags"] != 3 or a["online"] != 1:
                out.append(
                    "%s: %s is not connected and online after the load (%s)" % (lab, name, a)
                )
    if heads["d_loaded"]["gen"] != heads["c_built"]["gen"]:
        out.append(
            "d_loaded: power generated %d vs %d before the save"
            % (heads["d_loaded"]["gen"], heads["c_built"]["gen"])
        )
    if heads["d_loaded"]["primary"] != 0:
        out.append(
            "d_loaded: primary_mother_bldg=%d after the load, want 0" % heads["d_loaded"]["primary"]
        )
    return out


def problems_ai(heads, slots, want_on):
    out = []
    for k in ("ai_start", "ai_lifted", "ai_t30", "ai_t60"):
        if k not in heads:
            out.append("no `[a1] %s` line" % k)
    if out:
        return out
    if heads["ai_start"]["primary"] == 0:
        out.append("ai_start: the AI has no primary mother to lose")
    if heads["ai_lifted"]["primary"] != 0:
        out.append(
            "ai_lifted: the AI's primary mother is still set (%d)" % heads["ai_lifted"]["primary"]
        )
    before = set(slots.get("ai_start", {}))
    new = set()
    for lab in ("ai_t30", "ai_t60"):
        for tile, s in slots.get(lab, {}).items():
            if tile not in before and s["state"] != 4 and s["e"] > 0:
                new.add(tile)
    if want_on and not new:
        out.append("AI option ON: the mother-less AI placed no new building in 60 s")
    if not want_on and new:
        out.append(
            "AI option OFF: the mother-less AI placed %d new building(s) %s, want none (retail refuses its orders)"
            % (len(new), sorted(new))
        )
    return out


def problems_mp(peers):
    """peers = [(name, prefix, heads, slots, net_on, net_text)] host first, client second."""
    out = []
    pub = ado = None
    for name, pfx, heads, slots, net_on, net in peers:
        m = re.search(r"; gameplay: PUBLISHED (\S+)", net)
        if name == "host":
            pub = m.group(1) if m else None
        m = re.search(r"; gameplay: ADOPTED from host (\S+)", net)
        if name == "client":
            ado = m.group(1) if m else None
    if not pub or "mother_free_build=1" not in pub:
        out.append("host: no `; gameplay: PUBLISHED` line with mother_free_build=1 (got %r)" % pub)
    if not ado or "mother_free_build=1" not in ado:
        out.append(
            "client: no `; gameplay: ADOPTED from host` line with mother_free_build=1 (got %r): its own ini says 0"
            % ado
        )
    elif pub and ado != pub:
        out.append("client ADOPTED %s != host PUBLISHED %s" % (ado, pub))
    seen = {}
    for name, pfx, heads, slots, net_on, net in peers:
        if not net_on:
            out.append(
                "%s: no `; mother free build ON` banner: the host's value is not in force in this peer's sim"
                % name
            )
        need = ["p0", "lifted", "p1", "later"]
        miss = [k for k in need if "%s_%s" % (pfx, k) not in heads]
        if miss:
            out.append("%s: no `[a1] %s_*` line(s) for %s" % (name, pfx, miss))
            continue
        H = lambda k: heads["%s_%s" % (pfx, k)]  # noqa: E731
        S = lambda k: sorted(slots.get("%s_%s" % (pfx, k), {}).values(), key=lambda d: d["slot"])  # noqa: E731
        plants = lambda k: [d for d in S(k) if d["type"] in PLANT_TYPES]  # noqa: E731
        if H("p0")["primary"] == 0:
            out.append("%s: p0: the mother is not landed when the first Reactor is placed" % name)
        p0 = plants("p0")
        if len(p0) != 1 or p0[0]["flags"] != 3 or p0[0]["state"] != STATE_CONSTRUCTION:
            out.append(
                "%s: p0: want one Reactor under construction and connected, got %s"
                % (name, [(d["state"], d["flags"]) for d in p0])
            )
        if H("lifted")["primary"] != 0:
            out.append(
                "%s: lifted: primary_mother_bldg=%d, want 0 (the fixture's lift-off)"
                % (name, H("lifted")["primary"])
            )
        lp = plants("lifted")
        if (
            len(lp) != 1
            or lp[0]["online"] != 1
            or lp[0]["state"] == STATE_CONSTRUCTION
            or lp[0]["flags"] != 3
        ):
            out.append(
                "%s: lifted: the first Reactor is not finished, online and connected (%s)"
                % (name, lp)
            )
        p1 = plants("p1")
        if len(p1) != 2:
            out.append(
                "%s: p1: %d Reactors, want 2 (the second was placed with no mother: the gate was refused)"
                % (name, len(p1))
            )
        elif any(d["flags"] != 3 for d in p1):
            out.append(
                "%s: p1: a Reactor is not connected (flags %s)" % (name, [d["flags"] for d in p1])
            )
        la = plants("later")
        if len(la) != 2 or any(
            d["online"] != 1 or d["flags"] != 3 or d["state"] == STATE_CONSTRUCTION for d in la
        ):
            out.append(
                "%s: later: the second Reactor did not finish online and connected (%s)"
                % (name, la)
            )
        if H("later")["gen"] - H("lifted")["gen"] < 1000:
            out.append(
                "%s: later: power generated rose by %d, want >= 1000"
                % (name, H("later")["gen"] - H("lifted")["gen"])
            )
        seen[name] = [(k, H(k)["primary"], H(k)["alive"], H(k)["gen"]) for k in need]
    if len(seen) == 2 and seen["host"] != seen["client"]:
        out.append(
            "host and client disagree on the sim summary: %s vs %s" % (seen["host"], seen["client"])
        )
    return out


def snapshot(run_dir):
    text = cdb.video_log(run_dir) or ""  # the TEXT of the run dir's mh_video.log
    net = ""
    for d in cdb.session_dirs(run_dir):
        p = os.path.join(d, "mh_net.log")
        if os.path.isfile(p):
            net += open(p, encoding="utf-8", errors="replace").read()
    return text, net


def peer_snapshot(run_dir):
    """(video text, net text) of one MP peer: run_dir is its SESSION dir; the process's menu dir before it has mh_video.log."""
    run_dir = os.path.normpath(run_dir)
    logs = os.path.dirname(run_dir)
    me = os.path.basename(run_dir)
    vid, net = cdb.video_log(run_dir) or "", ""
    if not vid:
        menus = sorted(n for n in os.listdir(logs) if "_menu_" in n and n < me)
        if menus:
            md = os.path.join(logs, menus[-1])
            vid = cdb.video_log(md) or ""
            p0 = os.path.join(md, "mh_net.log")
            if os.path.isfile(p0):
                net += open(p0, encoding="utf-8", errors="replace").read()
    for d in cdb.session_dirs(run_dir):
        p = os.path.join(d, "mh_net.log")
        if os.path.isfile(p):
            net += open(p, encoding="utf-8", errors="replace").read()
    return vid, net


def main(argv):
    if "--selftest" in argv:
        return selftest()
    args = [a for a in argv if not a.startswith("--")]
    ai = None
    if "--ai" in argv:
        i = argv.index("--ai")
        ai = argv[i + 1]
        args = [a for a in args if a != ai]
    if "--mp" in argv:
        if len(args) != 2:
            print(__doc__)
            return 2
        peers = []
        for name, pfx, d in (("host", "h", args[0]), ("client", "c", args[1])):
            vid, net = peer_snapshot(d)
            heads, slots = parse(vid)
            peers.append(
                (name, pfx, heads, slots, bool(re.search(r"; mother free build ON \(", net)), net)
            )
        bad = problems_mp(peers)
        for b in bad:
            print("  check_a1_mother_free: " + b)
        print("check_a1_mother_free: %s" % ("FAIL" if bad else "PASS (host + client)"))
        return 1 if bad else 0
    if len(args) != 1:
        print(__doc__)
        return 2
    text, net = snapshot(args[0])
    heads, slots = parse(text)
    net_on = bool(re.search(r"; mother free build ON \(", net))
    if ai is not None:
        bad = problems_ai(heads, slots, ai == "on")
    else:
        bad = problems_solo(heads, slots, net_on)
    for b in bad:
        print("  check_a1_mother_free: " + b)
    print("check_a1_mother_free: %s" % ("FAIL" if bad else "PASS (%d labels)" % len(heads)))
    return 1 if bad else 0


def _slot(label, tile, typ, state, flags, online, prog=0, e=1000):
    return (
        ";   [a1] %s slot=1 id=4 type=%d at=%d,%d state=%d flags=%d online=%d energy_x10=%d/20000 prog_x100=%d eff_x1000=1000 workers=1 epow=1"
        % (label, typ, tile[0], tile[1], state, flags, online, e, prog)
    )


def _head(label, primary, gen, alive=28):
    return (
        "; [a1] %s planet=2 primary=%d pmu=0 alive=%d gen=%d con=6500 pgen=0 pcon=0 ratio_x1000=1000 pop=1 emp=1 res=0"
        % (label, primary, alive, gen)
    )


def _good_text():
    L = []
    L += [_head("start", 1, 6800), _head("lifted", 0, 6000)]
    L += [_head("a_placed", 0, 6000), _slot("a_placed", P1, 3, 100, 3, 0, 36)]
    L += [_head("a_built", 0, 7200), _slot("a_built", P1, 3, 134, 3, 1, 1200)]
    L += [
        _head("b_placed", 0, 7200),
        _slot("b_placed", P1, 3, 134, 3, 1),
        _slot("b_placed", RELAY, 15, 100, 3, 0, 36),
    ]
    L += [
        _head("b_built", 0, 7200),
        _slot("b_built", P1, 3, 134, 3, 1),
        _slot("b_built", RELAY, 15, 136, 3, 1, 1000),
    ]
    L += [
        _head("c_placed", 0, 7200),
        _slot("c_placed", P1, 3, 134, 3, 1),
        _slot("c_placed", RELAY, 15, 136, 3, 1),
        _slot("c_placed", P2, 3, 100, 3, 0, 36),
    ]
    L += [
        _head("c_removed", 0, 7200),
        _slot("c_removed", P1, 3, 134, 3, 1),
        _slot("c_removed", RELAY, 15, 4, 2, 1, 0, 0),
        _slot("c_removed", P2, 3, 100, 3, 0, 78),
    ]
    L += [
        _head("c_built", 0, 8400),
        _slot("c_built", P1, 3, 134, 3, 1),
        _slot("c_built", P2, 3, 134, 3, 1, 1200),
    ]
    L += [
        _head("d_loaded", 0, 8400),
        _slot("d_loaded", P1, 3, 134, 3, 1),
        _slot("d_loaded", P2, 3, 134, 3, 1),
    ]
    L += [
        _head("d_later", 0, 8400),
        _slot("d_later", P1, 3, 134, 3, 1),
        _slot("d_later", P2, 3, 134, 3, 1),
    ]
    return "\n".join(L)


def selftest():
    fails = 0

    def check(name, cond):
        nonlocal fails
        if not cond:
            fails += 1
            print("SELFTEST FAIL: " + name)

    heads, slots = parse(_good_text())
    check("good walk passes", problems_solo(heads, slots, True) == [])
    check("key not in force is red", any("banner" in p for p in problems_solo(heads, slots, False)))
    h2, s2 = parse(
        _good_text().replace(
            _slot("a_placed", P1, 3, 100, 3, 0, 36), _slot("a_placed", P1, 3, 100, 2, 0, 36)
        )
    )
    check(
        "a disconnected lone plant is red",
        any("built_flags=2" in p for p in problems_solo(h2, s2, True)),
    )
    h3, s3 = parse(_good_text().replace(_slot("a_placed", P1, 3, 100, 3, 0, 36), ""))
    check("a refused placement is red", any("no Reactor" in p for p in problems_solo(h3, s3, True)))
    h4, s4 = parse(
        _good_text().replace(
            _slot("c_removed", P2, 3, 100, 3, 0, 78), _slot("c_removed", P2, 3, 100, 2, 0, 78)
        )
    )
    check(
        "a plant disconnected by the recompute is red",
        any("recompute must re-seed" in p for p in problems_solo(h4, s4, True)),
    )
    ai = "\n".join(
        [
            _head("ai_start", 1, 593),
            _slot("ai_start", (57, 6), 26, 139, 3, 1),
            _head("ai_lifted", 0, 0),
            _head("ai_t30", 0, 0),
            _head("ai_t60", 0, 0),
        ]
    )
    ai_new = ai + "\n" + _slot("ai_t30", (48, 2), 22, 100, 2, 0, 0)
    ha, sa = parse(ai)
    hn, sn = parse(ai_new)
    check("AI on without a new building is red", bool(problems_ai(ha, sa, True)))
    check("AI on with a new building passes", problems_ai(hn, sn, True) == [])
    check("AI off with a new building is red", bool(problems_ai(hn, sn, False)))
    check("AI off without one passes", problems_ai(ha, sa, False) == [])

    def peer_text(pfx, second=True):
        L = [_head(pfx + "_p0", 1, 800, 2), _slot(pfx + "_p0", (1, 1), 23, 100, 3, 0, 91)]
        L += [
            _head(pfx + "_lifted", 0, 1200, 1),
            _slot(pfx + "_lifted", (1, 1), 23, 134, 3, 1, 1201),
        ]
        L += [_head(pfx + "_p1", 0, 1200, 2), _slot(pfx + "_p1", (1, 1), 23, 134, 3, 1, 1201)]
        if second:
            L += [_slot(pfx + "_p1", (5, 1), 23, 100, 3, 0, 93)]
        L += [_head(pfx + "_later", 0, 2400, 2), _slot(pfx + "_later", (1, 1), 23, 134, 3, 1, 1201)]
        if second:
            L += [_slot(pfx + "_later", (5, 1), 23, 134, 3, 1, 1200)]
        return "\n".join(L)

    def peers(second=True, client_adopts=True):
        sm = "turret_clip_fix=0;mother_free_build=1;mother_free_build_ai=0"
        hn = "; gameplay: PUBLISHED %s [6 key(s)]\n" % sm
        cn = ("; gameplay: ADOPTED from host %s -- START trailer\n" % sm) if client_adopts else ""
        hh, hs = parse(peer_text("h", second))
        ch, cs = parse(peer_text("c", second))
        return [("host", "h", hh, hs, True, hn), ("client", "c", ch, cs, client_adopts, cn)]

    check("mp good passes", problems_mp(peers()) == [])
    check(
        "mp: a refused second Reactor is red",
        any("gate was refused" in p for p in problems_mp(peers(second=False))),
    )
    check(
        "mp: a client that did not adopt is red",
        any("ADOPTED" in p for p in problems_mp(peers(client_adopts=False))),
    )
    print("check_a1_mother_free selftest: %s" % ("FAIL (%d)" % fails if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
