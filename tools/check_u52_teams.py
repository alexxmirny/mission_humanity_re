#!/usr/bin/env python3
"""check_u52_teams.py -- mp:U52: do the lobby TEAMS become relations, and what do they change?

Reads the three pulled peer dirs of a `det_arms.py --u52-*` run (<det_dir>/{host,client1,client2}) and checks,
per peer and ACROSS peers:

  1. the step-1 `; RELDUMP` table matches the seed rule: for every ordered pair of enabled players (a != b),
     Players[a].relation[b] == 1 if both carry the same non-zero team else 2, and the AI mirror
     player_data[a].ai_player_relation[b] is '+' / '-' accordingly; the ally-victory flag is 1 in TEAM mode and
     0 in FFA. All three peers' dumps are byte-identical.
  2. the late RELDUMP (after a relation order issued by client1 at step 1100): TEAM mode = the table is
     unchanged (the order is a no-op on every peer); FFA = relation[1][0] became 2 and the mirror '-' on every peer.
  3. the HOSTILITY PROBE (`; HOSTILE` census): the ALLY pair (host, client1) kept every unit at full energy; the
     ENEMY pair (host, client2) is the positive control and must have taken damage -- if it did not engage, the
     ally pair's silence proves nothing and the check is RED. Every peer logs the same census.
  5. the HIT PROBE (`; HITPROBE`, harness hit_probe at step 1000): a synthetic building hit by an ALLY of the victim must
     leave the victim's ai_player_relation[aggressor] at +1 ([net] ally_damage_no_hostility); the ENEMY control hit
     (mirror zeroed first) must flip it to -1. Identical on all peers.
  6. VISION (per-peer VIEW state, not hashed, so each peer is checked against ITS OWN expected value from the synced
     teams): `; VISDUMP` at steps 1 and 1400 -- PLAYER_CONTROL_MASK = my teammates (I share with them), is_human = the
     human view mask = me + my teammates; every enabled player's first building is lit (`seen=1`) on my fog iff it is
     me or a teammate, and dark for an enemy. The 0xf5 order client1 issues at step 1120 (share with player 2): FFA =
     applied (client2's view mask gains client1, client1's control mask gains 2); Team = dropped (client2 unchanged).
  4. the outcome: TEAM mode -- host and client1 reach `on_gameover outcome=5` (victory) after client2's
     elimination, client2 outcome=4; FFA -- nobody but client2 reaches a gameover.

Absence of a log is a Refusal, never a pass.

  python tools/check_u52_teams.py <det_dir> <team|ffa>
"""

import os
import re
import sys

PEERS = ("host", "client1", "client2")
TEAMS = {0: 1, 1: 1, 2: 2}  # the arm's lobby teams: host T1, client1 T1, client2 T2
ENABLED = (0, 1, 2)
TEAMS_AI = {0: 1, 1: 1, 2: 2, 3: 1}  # "team-ai": a computer player in slot 3, on the host's team
ENABLED_AI = (0, 1, 2, 3)
EARLY, LATE = 1, 1400
PROBE_END = 1500  # the force-kill lands at ~1560: judge the probe before it


class Refusal(Exception):
    pass


def _read(path):
    if not os.path.isfile(path):
        raise Refusal("missing %s" % path)
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


DUMP_HEAD = re.compile(r"; RELDUMP step=(\d+) allyflag=(-?\d+)")
DUMP_ROW = re.compile(r"; RELDUMP p(\d) ctrl=([0-9a-f]{2}) team=(\d+) rel=(\d{8}) ai=(\S{8})")


def parse_dumps(har):
    """{step: (flag, {p: (ctrl, team, rel, ai)})}"""
    out, cur = {}, None
    for ln in har.splitlines():
        m = DUMP_HEAD.search(ln)
        if m:
            cur = (int(m.group(2)), {})
            out[int(m.group(1))] = cur
            continue
        m = DUMP_ROW.search(ln)
        if m and cur is not None:
            cur[1][int(m.group(1))] = (int(m.group(2), 16), int(m.group(3)), m.group(4), m.group(5))
    return out


def raw_dump_text(har, step):
    keep, on = [], False
    for ln in har.splitlines():
        m = DUMP_HEAD.search(ln)
        if m:
            on = int(m.group(1)) == step
        if on and "RELDUMP" in ln:
            keep.append(re.sub(r"^\[[^\]]*\]\s*", "", ln.strip()))
    return keep


def expected(a, b, teams=TEAMS):
    ta, tb = teams.get(a, 0), teams.get(b, 0)
    return 1 if (ta != 0 and ta == tb) else 2


HOST_RE = re.compile(
    r"; HOSTILE step=(\d+) pair(\d) a=(\d) unit=(-?\d+) energy=(-?\d+) b=(\d) unit=(-?\d+) energy=(-?\d+) phase=(\d)"
)


def parse_hostile(har):
    """{pair: [(step, phase, a_energy, b_energy)]} -- only rows where BOTH exact units are latched."""
    out = {1: [], 2: []}
    for ln in har.splitlines():
        m = HOST_RE.search(ln)
        if m and int(m.group(4)) >= 0 and int(m.group(7)) >= 0:
            out[int(m.group(2))].append(
                (int(m.group(1)), int(m.group(9)), int(m.group(5)), int(m.group(8)))
            )
    return out


def outcomes(net):
    return [int(x) for x in re.findall(r"on_gameover ENTER sess=\d+ outcome=(\d+)", net)]


def check(det_dir, mode):
    if mode not in ("team", "ffa", "team-ai"):
        raise Refusal("mode must be team, ffa or team-ai")
    teams, enabled = (TEAMS_AI, ENABLED_AI) if mode == "team-ai" else (TEAMS, ENABLED)
    mode = "team" if mode.startswith("team") else "ffa"
    out, bad = [], []
    har, net = {}, {}
    for p in PEERS:
        d = os.path.join(det_dir, p)
        har[p] = _read(os.path.join(d, "mh_harness.log"))
        net[p] = _read(os.path.join(d, "mh_net.log"))
    dumps = {p: parse_dumps(har[p]) for p in PEERS}
    for p in PEERS:
        for st in (EARLY, LATE):
            if st not in dumps[p] or len(dumps[p][st][1]) != 8:
                raise Refusal(
                    "%s has no complete RELDUMP at step %d (relation_dump_step[2] not armed?)"
                    % (p, st)
                )
    want_flag = 1 if mode == "team" else 0

    # ---- 1. the seed rule at step 1, per peer + across peers
    for p in PEERS:
        flag, rows = dumps[p][EARLY]
        errs = []
        if flag != want_flag:
            errs.append("allyflag=%d (want %d)" % (flag, want_flag))
        for a in enabled:
            ctrl, team, rel, ai = rows[a]
            if ctrl == 0:
                errs.append("p%d not enabled" % a)
            if team != teams[a]:
                errs.append("p%d team byte %d (want %d)" % (a, team, teams[a]))
            for b in enabled:
                if a == b:
                    continue
                want = expected(a, b, teams)
                if int(rel[b]) != want:
                    errs.append("rel[%d][%d]=%s (want %d)" % (a, b, rel[b], want))
                wai = "+" if want == 1 else "-"
                if ai[b] != wai:
                    errs.append("ai[%d][%d]=%s (want %s)" % (a, b, ai[b], wai))
        out.append(
            "step-%d seed table on %-7s: %s"
            % (EARLY, p, "matches the rule (flag=%d)" % flag if not errs else "; ".join(errs[:6]))
        )
        if errs:
            bad.append("%s seed table wrong" % p)
    texts = {p: raw_dump_text(har[p], EARLY) for p in PEERS}
    same = texts["host"] == texts["client1"] == texts["client2"]
    out.append(
        "step-%d dumps byte-identical across the three peers: %s" % (EARLY, "yes" if same else "NO")
    )
    if not same:
        bad.append("step-%d dumps differ between peers" % EARLY)
        for p in PEERS:
            out += ["    %s: %s" % (p, ln) for ln in texts[p][:9]]
    out += ["    " + ln for ln in texts["host"]]

    # ---- 2. the relation order (client1: relation toward the host -> enemy), judged at LATE
    for p in PEERS:
        flag, rows = dumps[p][LATE]
        rel10, ai10 = rows[1][2][0], rows[1][3][0]
        if mode == "team":
            ok = rel10 == "1" and ai10 == "+" and flag == 1
            what = "relation[1][0]=%s ai=%s flag=%d -> %s" % (
                rel10,
                ai10,
                flag,
                "UNCHANGED (order was a no-op)" if ok else "CHANGED",
            )
        else:
            ok = rel10 == "2" and ai10 == "-" and flag == 0
            what = "relation[1][0]=%s ai=%s flag=%d -> %s" % (
                rel10,
                ai10,
                flag,
                "APPLIED" if ok else "NOT applied",
            )
        out.append("step-%d on %-7s: %s" % (LATE, p, what))
        if not ok:
            bad.append(
                "%s: relation order %s"
                % (p, "applied in Team mode" if mode == "team" else "not applied in FFA")
            )
    late = {p: raw_dump_text(har[p], LATE) for p in PEERS}
    if not (late["host"] == late["client1"] == late["client2"]):
        bad.append("step-%d dumps differ between peers" % LATE)
        out.append("step-%d dumps differ between peers" % LATE)
    else:
        out.append("step-%d dumps byte-identical across the three peers" % LATE)

    # ---- 3. the hostility probe: two phases, an ALLY pair and an ENEMY pair (the positive control)
    census = {p: parse_hostile(har[p]) for p in PEERS}
    verdict = {}
    for pair, label in ((1, "ALLY  pair (host,client1)"), (2, "ENEMY pair (host,client2)")):
        rows = [r for r in census["host"][pair] if r[0] <= PROBE_END]
        p1 = [r for r in rows if r[1] == 1]
        p2 = [r for r in rows if r[1] == 2]
        if len(p1) < 2 or len(p2) < 2:
            bad.append(
                "probe pair %d: too few census rows (P1 %d, P2 %d) -- units never latched?"
                % (pair, len(p1), len(p2))
            )
            out.append("%s: too few census rows (P1 %d, P2 %d)" % (label, len(p1), len(p2)))
            continue
        a0, b0 = p1[0][2], p1[0][3]
        a1, b1 = p1[-1][2], p1[-1][3]
        a2, b2 = p2[-1][2], p2[-1][3]
        d1 = a1 < a0 or b1 < b0
        d2 = a2 < a1 or b2 < b1
        verdict[pair] = (d1, d2)
        out.append(
            "%s: start a=%d b=%d | end of P1 (no orders, step %d) a=%d b=%d -> %s | end of P2 (a ordered to attack b, step %d) a=%d b=%d -> %s"
            % (
                label,
                a0,
                b0,
                p1[-1][0],
                a1,
                b1,
                "AUTO-ENGAGED" if d1 else "no damage",
                p2[-1][0],
                a2,
                b2,
                "DAMAGE" if d2 else "no damage",
            )
        )
    if 1 in verdict and 2 in verdict:
        if verdict[2][1] is False:
            bad.append(
                "ENEMY pair: the explicit attack order did no damage -- the positive control is dead, nothing below means anything"
            )
        if verdict[1][0]:
            bad.append("ALLY pair auto-engaged -- allies fought each other with no orders")
        if not verdict[2][0]:
            out.append(
                "NOTE: the ENEMY pair never auto-engaged in P1, so 'allies do not auto-engage' is UNPROVEN by this probe"
            )
        if verdict[1][1]:
            out.append(
                "NOTE: an explicit attack order DID damage an ALLY (attack orders are relation-blind) -- the open risk, measured"
            )
        else:
            out.append("an explicit attack order against an ALLY did no damage")
    c_ident = all(census[p] == census["host"] for p in PEERS)
    out.append("the census is identical on all three peers: %s" % ("yes" if c_ident else "NO"))
    if not c_ident:
        bad.append("hostility census differs between peers")

    # ---- 5. the hit probe: an ally's building hit must not flip the victim hostile; an enemy's must
    hit_re = re.compile(
        r"; HITPROBE step=(\d+) kind=(\w+) victim=(\d) aggressor=(\d) before=(-?\d+) after=(-?\d+)"
    )
    hits = {p: [m.groups() for m in map(hit_re.search, har[p].splitlines()) if m] for p in PEERS}
    for p in PEERS:
        got = {h[1]: h for h in hits[p]}
        if "ally" not in got or "enemy" not in got:
            bad.append("%s has no HITPROBE lines (hit_probe_at not armed?)" % p)
            continue
        a, e = got["ally"], got["enemy"]
        out.append(
            "hit probe on %-7s: ALLY hit (victim %s, aggressor %s) mirror %s -> %s | ENEMY hit (victim %s, aggressor %s) mirror %s -> %s"
            % (p, a[2], a[3], a[4], a[5], e[2], e[3], e[4], e[5])
        )
        if a[5] != "1":
            bad.append(
                "%s: an ALLY's building hit flipped the victim to %s (want it to stay +1)"
                % (p, a[5])
            )
        if e[5] != "-1":
            bad.append(
                "%s: the ENEMY control hit left the mirror at %s (want -1: the probe is dead)"
                % (p, e[5])
            )
    if len({tuple(hits[p]) for p in PEERS}) != 1:
        bad.append("hit probe differs between peers")

    # ---- 6. vision: the per-peer view masks + the fog readout
    vh = re.compile(
        r"; VISDUMP step=(\d+) side=(\d+) is_human=([0-9a-f]+) hmask=([0-9a-f]+) ctl=([0-9a-f]+)"
    )
    vr = re.compile(r"; VISDUMP p(\d) (bldg|unit)=(\d+) at=\d+,\d+ vis=([0-9a-f]+) seen=(\d)")
    vdump = {}
    for p in PEERS:
        cur = None
        for ln in har[p].splitlines():
            m = vh.search(ln)
            if m:
                cur = (
                    int(m.group(2)),
                    int(m.group(3), 16),
                    int(m.group(4), 16),
                    int(m.group(5), 16),
                    {},
                )
                vdump[(p, int(m.group(1)))] = cur
                continue
            m = vr.search(ln)
            if m and cur is not None:
                cur[4][int(m.group(1))] = (int(m.group(5)), m.group(2))
    for p in PEERS:
        for st in (EARLY, LATE):
            if (p, st) not in vdump:
                bad.append("%s has no VISDUMP at step %d" % (p, st))
    sides = {"host": 0, "client1": 1, "client2": 2}
    if all((p, st) in vdump for p in PEERS for st in (EARLY, LATE)):
        for p in PEERS:
            me = sides[p]
            mates = sum(
                1 << b for b in enabled if b != me and teams[b] == teams[me] and teams[me] != 0
            )
            for st in (EARLY, LATE):
                side, ih, hm, ctl, seen = vdump[(p, st)]
                want_hm = (1 << me) | mates
                want_ctl = mates
                if st == LATE:
                    if p == "client1":
                        want_ctl |= (
                            1 << 2
                        )  # the dialog call's own local write (the UI lock is the widget, not the call)
                    if p == "client2" and mode == "ffa":
                        want_hm |= 1 << 1  # client1's 0xf5 grant applied on its target
                errs = []
                if side != me:
                    errs.append("side %d (want %d)" % (side, me))
                if ih != want_hm or hm != want_hm:
                    errs.append("is_human=%02x hmask=%02x (want %02x)" % (ih, hm, want_hm))
                if ctl != want_ctl:
                    errs.append("ctl=%02x (want %02x)" % (ctl, want_ctl))
                if st == LATE:
                    for q, (sn, kind) in sorted(seen.items()):
                        friend = q == me or (mates >> q) & 1
                        if friend and sn != 1:
                            errs.append("p%d's %s is DARK on my fog (want lit)" % (q, kind))
                        if not friend and kind == "bldg" and sn != 0:
                            errs.append("enemy p%d's building is LIT on my fog (want dark)" % q)
                out.append(
                    "vision step-%d on %-7s (side %d): is_human=%02x ctl=%02x seen=%s -> %s"
                    % (
                        st,
                        p,
                        side,
                        ih,
                        ctl,
                        dict(sorted(seen.items())),
                        "OK" if not errs else "; ".join(errs),
                    )
                )
                if errs:
                    bad.append("%s step-%d vision wrong" % (p, st))
    # ---- 4. the outcome after client2's elimination
    oc = {p: outcomes(net[p]) for p in PEERS}
    out.append("on_gameover outcomes: " + ", ".join("%s=%s" % (p, oc[p] or "none") for p in PEERS))
    if mode == "team":
        for p in ("host", "client1"):
            if 5 not in oc[p]:
                bad.append("%s did not reach outcome 5 (alliance victory)" % p)
    else:
        for p in ("host", "client1"):
            if 5 in oc[p]:
                bad.append("%s reached a victory in FFA with two players left" % p)
    if 4 not in oc["client2"]:
        bad.append("client2 (eliminated) did not reach outcome 4")
    return (not bad), out + ["    BAD: " + b for b in bad]


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        ok, lines = check(argv[1], argv[2])
    except Refusal as exc:
        print("REFUSED: %s" % exc)
        return 2
    print("\n".join(lines))
    print("check_u52_teams: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
