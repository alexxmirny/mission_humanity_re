#!/usr/bin/env python3
"""check_u54_spectate.py -- mp:U54 (spectate after defeat): the verdict of a `det_arms.py --u54-*` run.

Reads the three pulled peer dirs (<det_dir>/{host,client1,client2}) of a 3-peer match in which ONE peer (the
victim) is eliminated by the harness force-kill at ~step 304 and, with `[net] spectate_after_defeat` on, stays in
the session as a SPECTATOR. What each arm must show:

  *-spec     the victim keeps stepping with the survivors: ALL THREE peers hash IDENTICAL for every common step
             to the end (the spectator is NOT excluded), the victim's defeat dialog left the session lockstep
             (`on_gameover ENTER sess=3`), nobody logged a CTL_PLAYER_LEFT for it and no survivor ended its match.
  client-exit / client-leave / host-exit / host-leave
             the victim leaves (Ok at the dialog, or ESC -> Quit later): the two SURVIVORS stay IDENTICAL to the
             end (their match is not disturbed: no on_gameover, no drop of the victim by GS2 / fast-drop), the
             victim reaches the main menu; a host victim hands the hub over (U62 lines on every peer).
  neg        `[net] spectate_after_defeat=0` reproduces today's flow: the victim downgrades at its dialog
             (`on_gameover ENTER sess=3 ... downgrade=1`) and the survivors see its CTL_PLAYER_LEFT.

  python tools/check_u54_spectate.py <det_dir> <victim: host|client1> <arm>
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_u53_elim as u53  # noqa: E402

Refusal = u53.Refusal
PEERS = u53.PEERS
MIN_POST = 1500
MIN_COMMON = 1000

SPEC_LINE = re.compile(r"; U54 spectate: defeat dialog opened by a SPECTATOR")
SPEC_RETAIL = re.compile(r"; U54 spectate: local human eliminated -> SPECTATOR")
GAMEOVER = re.compile(r"on_gameover ENTER sess=(\d+) outcome=(\d+) gclk=(\d+) \(downgrade=(\d+)\)")
LEFT = re.compile(r"; \[rx\] CTL_PLAYER_LEFT sender=(\d+)")
DROPPED = re.compile(r"; U54 spectate: dropped order owner=(\d+)")
LEFT_MATCH = re.compile(r"; U54 spectate: the spectator left the match")
MASK = re.compile(r"; U71 spectator mask ([0-9a-f]{2}) -> ([0-9a-f]{2})")
SESSION_END = re.compile(r"\[session\] SESSION_END")
CONT = re.compile(r"; U54 spectate: dialog answered CONTINUE")
WIDEN = re.compile(r"; U54 spectate: defeat dialog widened")
QUIT_END = re.compile(r"\[session\] SESSION_END .*reason=quit")
HUB_NOTICE = re.compile(r"; U62 notice: hub (\d+) -> (\d+)")
SURV_BAD = re.compile(
    r"on_gameover ENTER|U17 fast-drop|; U19b|GS2 drop|data[ _]timeout|dropping peer|\[resync\] (trigger|start)|DESYNC"
)


def _victim_slot(victim):
    return 0 if victim == "host" else 1


def check_team(det_dir, victim):
    """The TEAM-VICTORY arm: slot 1 (the spectator, host's ally) falls at step ~300, slot 2 (the enemy) at ~600; the host
    is then alone with the spectator's ally side -> the host wins and the spectator's dialog must say VICTORY (5)."""
    out, bad = [], []
    logs = {}
    for p in PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (
            u53._read(os.path.join(d, "mh_net.log")),
            u53._read(os.path.join(d, "mh_harness.log")),
        )
    vnet = logs[victim][0]
    k1 = re.search(r"; CONQ step=(\d+) FORCE-KILL: order 0xf8", logs["host"][1])
    k2 = re.search(r"; CONQ step=(\d+) FORCE-KILL2: order 0xf8 x(\d+)", logs["host"][1])
    out.append(
        "force-kill 1 at step %s, force-kill 2 at step %s"
        % (k1 and k1.group(1), k2 and k2.group(1))
    )
    if not k1 or not k2:
        bad.append(
            "the two eliminations did not both happen (FORCE-KILL / FORCE-KILL2 missing in the host harness log)"
        )
    out.append(
        "victim spectator lines: %s"
        % ("yes" if (SPEC_LINE.search(vnet) or SPEC_RETAIL.search(vnet)) else "NO")
    )
    if not (SPEC_LINE.search(vnet) or SPEC_RETAIL.search(vnet)):
        bad.append("the victim never became a spectator")
    if not WIDEN.search(vnet):
        bad.append("the victim's defeat dialog was not widened")
    if not CONT.search(vnet):
        bad.append("the victim never answered CONTINUE")
    go = GAMEOVER.findall(vnet)
    out.append("victim on_gameover lines (sess, outcome, gclk, downgrade): %s" % go[:4])
    if not any(g[1] == "5" for g in go):
        bad.append(
            "the spectator's end-of-match dialog never said VICTORY (no `on_gameover ... outcome=5`)"
        )
    hgo = GAMEOVER.findall(logs["host"][0])
    out.append("host on_gameover lines: %s" % hgo[:3])
    if not any(g[1] in ("5", "8") for g in hgo):
        # 5 = last man standing / allied victory; 8 = the forced-removal flavour when CTL_PLAYER_LEFT beat the host's own flip
        bad.append("the surviving ally (host) never ended its match (no outcome=5/8)")
    left = LEFT.findall(logs["host"][0]) + LEFT.findall(logs["client2"][0])
    if any(x == "1" for x in left):
        bad.append("a CTL_PLAYER_LEFT for the spectator (slot 1) was received")
    tx = u53.analyze(os.path.join(det_dir, victim), os.path.join(det_dir, "host"), 100)
    m = re.search(r"state-hash first differs at step (\d+)", tx)
    k2s = int(k2.group(1)) if k2 else 600
    if m:
        out.append(
            "spectator vs host: first hash difference at step %s (kill 2 at %d)" % (m.group(1), k2s)
        )
        if int(m.group(1)) < k2s:
            bad.append(
                "the spectator's hashes diverged from the host's BEFORE the match was decided (step %s)"
                % m.group(1)
            )
    else:
        out.append("spectator vs host: no hash difference over the compared steps")
    return (not bad), out + ["FAIL: " + x for x in bad]


LASTWIN_RACE = "RACE: forced removal first"
FW_LINE = re.compile(r"; U54-FW outcome=(\d+) won_text_queued=(\d) enemies_alive=(\d+)")
FW_RETAIL = re.compile(
    r"; U54-FW last human: outcome=(\d+) .* -> (an ENEMY is alive|no enemy alive)"
)
FW_PATCHED = re.compile(r"; U52 retail carrier last-human (text|fanfare) @[0-9A-F]+: (\w+)")


def check_lastwin(det_dir, arm):
    """mp:U54-FW, the false "You have won": the host force-kills client2 (it spectates) and then client1, so the host is
    the last human. It must get outcome 6 (the "last player" dialog). `lastwin`: an ENEMY AI is alive -> no win text
    queued; `lastwin-neg` (fix off): the text IS queued (retail's false win); `lastwin-ally`: only an allied AI -> the
    text stays. The retail carrier's own verdict line, when it ran, must agree (the fanfare follows the same decision)."""
    out, bad = [], []
    neg = "lastwin-neg" in arm
    ally = "lastwin-ally" in arm
    hnet = u53._read(os.path.join(det_dir, "host", "mh_net.log"))
    hh = u53._read(os.path.join(det_dir, "host", "mh_harness.log"))
    k1 = re.search(r"; CONQ step=(\d+) FORCE-KILL: order 0xf8 x\d+ against player 2", hh)
    k2 = re.search(r"; CONQ step=(\d+) FORCE-KILL2: order 0xf8 x\d+ against player 1", hh)
    out.append(
        "host force-kills: client2 at %s, client1 at %s" % (k1 and k1.group(1), k2 and k2.group(1))
    )
    if not (k1 and k2):
        bad.append("the host did not force-kill client2 and then client1")
    c2net = u53._read(os.path.join(det_dir, "client2", "mh_net.log"))
    if not (SPEC_LINE.search(c2net) or SPEC_RETAIL.search(c2net)):
        bad.append(
            "client2 never became a spectator (the shape needs it out of the humans but in the match)"
        )
    go = GAMEOVER.findall(hnet)
    out.append("host on_gameover (sess, outcome, gclk, downgrade): %s" % go[:3])
    if not any(g[1] == "6" for g in go):
        if any(g[1] == "8" for g in go):
            # client1's CTL_PLAYER_LEFT reached the host before the host's own sim stepped the elimination: the FORCED
            # flavour (outcome 8), which never queues the win text in retail either -- the shape did not happen, not a
            # verdict on the fix. Measured ~1 run in 4. det_arms.py re-runs the arm on this marker.
            bad.append(
                LASTWIN_RACE
                + " -- client1's CTL_PLAYER_LEFT beat the host's own elimination step (outcome 8)"
            )
        else:
            bad.append("the host (the last human) never got outcome 6 (the 'last player' dialog)")
    fw = FW_LINE.findall(hnet)
    out.append("host U54-FW evidence (outcome, won_text_queued, enemies_alive): %s" % fw[:3])
    f6 = [x for x in fw if x[0] == "6"]
    if not f6:
        bad.append("no `; U54-FW outcome=6 ...` evidence line on the host")
    else:
        _, won, enemies = f6[0]
        want_won = "1" if (neg or ally) else "0"
        if won != want_won:
            bad.append(
                "won_text_queued=%s, expected %s (%s)"
                % (
                    won,
                    want_won,
                    "retail false win"
                    if neg
                    else "allied AI only"
                    if ally
                    else "an enemy AI is alive",
                )
            )
        if ally and enemies != "0":
            bad.append("positive arm: enemies_alive=%s, expected 0" % enemies)
        if not ally and enemies == "0":
            bad.append(
                "the enemy AI was not alive when the last human's dialog opened (enemies_alive=0)"
            )
    out.append("host retail carriers: %s" % FW_PATCHED.findall(hnet))
    rv = FW_RETAIL.findall(hnet)
    if rv:
        out.append("host retail carrier verdict: %s" % rv[:2])
        if neg:
            bad.append("the fix-off arm still ran the retail carrier")
        elif (rv[0][1] == "no enemy alive") != ally:
            bad.append("the retail carrier's verdict disagrees with the roster")
    return (not bad), out + ["FAIL: " + x for x in bad]


def check(det_dir, victim, arm="u54-client-spec", kill_step=300):
    if "lastwin" in arm:
        return check_lastwin(det_dir, arm)
    if "team" in arm:
        return check_team(det_dir, victim)
    out, bad = [], []
    logs = {}
    for p in PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (
            u53._read(os.path.join(d, "mh_net.log")),
            u53._read(os.path.join(d, "mh_harness.log")),
        )
    survivors = [p for p in PEERS if p != victim]
    spec_arm = arm.endswith(("-spec", "-spec-relay", "-probe", "-probe-relay"))
    neg = "neg" in arm
    leave = any(k in arm for k in ("exit", "leave"))
    ks = None
    for p in PEERS:
        m = re.search(r"; CONQ step=(\d+) FORCE-KILL: order 0xf8 x(\d+)", logs[p][1])
        if m:
            ks = int(m.group(1))
            out.append("force-kill issued by %s at step %d (x%s objects)" % (p, ks, m.group(2)))
    if ks is None:
        bad.append("no FORCE-KILL line in any harness log -- the elimination never happened")
        ks = kill_step
    last = {}
    for p in PEERS:
        st = u53.harness_steps(logs[p][1])
        last[p] = st[-1][0] if st else 0
        out.append(
            "%-7s %s last hashed step=%d (%d past the elimination)"
            % (p, "VICTIM  " if p == victim else "survivor", last[p], last[p] - ks)
        )
    for sv in survivors:
        if last[sv] - ks < MIN_POST:
            bad.append(
                "%s stopped %d steps after the elimination (< %d)" % (sv, last[sv] - ks, MIN_POST)
            )

    vnet = logs[victim][0]
    go = GAMEOVER.findall(vnet)
    if go:
        out.append("victim on_gameover lines (sess, outcome, gclk, downgrade): %s" % go[:3])
    spec_seen = bool(SPEC_LINE.search(vnet) or SPEC_RETAIL.search(vnet))
    left_seen = [(p, LEFT.findall(logs[p][0])) for p in PEERS if p != victim]
    out.append("victim spectator lines: %s" % ("yes" if spec_seen else "NO"))
    out.append(
        "CTL_PLAYER_LEFT receipts: %s" % ", ".join("%s:%d" % (p, len(v)) for p, v in left_seen)
    )
    for p in PEERS:
        mk = MASK.findall(logs[p][0])
        if mk:
            out.append("%s spectator mask transitions: %s" % (p, mk[:4]))
    drops = DROPPED.findall(vnet)
    out.append("victim dropped-order lines: %d" % len(drops))
    if "probe" in arm:
        # mutation arm: the host issued an order OWNED BY the spectator; whichever peer dispatched it must have dropped it
        all_drops = {p: len(DROPPED.findall(logs[p][0])) for p in PEERS}
        probe_line = bool(re.search(r"SPECTATOR-PROBE", " ".join(logs[p][1] for p in PEERS)))
        out.append(
            "spectator-owned order issued=%s, dropped-order lines per peer: %s"
            % (probe_line, all_drops)
        )
        if not probe_line:
            bad.append("the harness never issued the spectator-owned probe order")
        if not any(all_drops.values()):
            bad.append(
                "the spectator-owned order was NOT dropped by the gate (no `dropped order` line on any peer)"
            )

    if neg:
        if spec_seen:
            bad.append(
                "negative arm: the victim still spectated (key=0 must reproduce today's defeat flow)"
            )
        if not any(g[1] == "4" for g in go):
            bad.append(
                "negative arm: no `on_gameover ENTER ... outcome=4` (the defeat dialog) on the victim"
            )
        if any(g[0] == "3" and g[3] == "0" for g in go):
            bad.append("negative arm: the victim's session stayed lockstep at its defeat dialog")
        if not any(v for _p, v in left_seen):
            bad.append("negative arm: no survivor saw the victim's CTL_PLAYER_LEFT")
    else:
        if not spec_seen:
            bad.append("the victim never became a spectator (no U54 spectate line)")
        if not any(g[0] == "3" for g in go):
            bad.append("the victim's defeat dialog did not keep the session lockstep")
        if any(v for _p, v in left_seen):
            bad.append("a survivor received a CTL_PLAYER_LEFT for a spectator")
        for sv in survivors:
            hits = [ln for ln in logs[sv][0].splitlines() if SURV_BAD.search(ln)]
            for ln in hits[:6]:
                out.append("    %s: %s" % (sv, ln.strip()[:200]))
            if any("on_gameover ENTER" in ln for ln in hits):
                bad.append(
                    "%s ended its own match (on_gameover) -- the survivors should play on" % sv
                )

    # --- hashes ---
    a, b = survivors
    txt = u53.analyze(os.path.join(det_dir, a), os.path.join(det_dir, b), MIN_COMMON)
    m = re.search(r"combined-hash=(\d+)", txt)
    common = int(m.group(1)) if m else 0
    sv_ident = "ALL PAIRS IDENTICAL" in txt
    out.append(
        "survivors %s vs %s: %s, %d common steps"
        % (a, b, "IDENTICAL" if sv_ident else "NOT IDENTICAL", common)
    )
    if not sv_ident:
        bad.append("survivors are not ALL PAIRS IDENTICAL")
        out += ["    " + ln for ln in txt.strip().splitlines()[-8:]]
    elif common < MIN_COMMON:
        bad.append("only %d common steps between survivors (< %d)" % (common, MIN_COMMON))
    if spec_arm:
        for s in survivors:
            tx = u53.analyze(os.path.join(det_dir, victim), os.path.join(det_dir, s), MIN_COMMON)
            m = re.search(r"combined-hash=(\d+)", tx)
            c = int(m.group(1)) if m else 0
            ident = "ALL PAIRS IDENTICAL" in tx
            out.append(
                "SPECTATOR %s vs survivor %s: %s, %d common steps (last: victim %d, survivor %d)"
                % (victim, s, "IDENTICAL" if ident else "NOT IDENTICAL", c, last[victim], last[s])
            )
            if not ident:
                bad.append("the spectator's hashes differ from %s" % s)
                out += ["    " + ln for ln in tx.strip().splitlines()[-8:]]
            elif c < MIN_COMMON:
                bad.append("only %d common steps between the spectator and %s" % (c, s))
        if last[victim] - ks < MIN_POST:
            bad.append(
                "the spectator stopped %d steps after its defeat (< %d)"
                % (last[victim] - ks, MIN_POST)
            )
    if not neg:
        if not WIDEN.search(vnet):
            bad.append(
                "the victim's defeat dialog was not widened to [Continue spectating] [Exit match]"
            )
        exits = "exit" in arm
        left_line = bool(LEFT_MATCH.search(vnet))
        cont_line = bool(CONT.search(vnet))
        quit_line = bool(QUIT_END.search(vnet))
        out.append(
            "victim: dialog widened=%s, answered CONTINUE=%s, left-match(menu)=%s, quit-end=%s, last step %d"
            % (bool(WIDEN.search(vnet)), cont_line, left_line, quit_line, last[victim])
        )
        if exits:
            if not left_line:
                bad.append(
                    "the victim chose Exit match but never reached the main menu as a spectator"
                )
            if cont_line:
                bad.append("the victim logged a CONTINUE answer on an Exit arm")
            # (a host victim waits for the clients' mid-match signals before it clicks Exit, so the bound is "left before the end")
            if last[victim] > 3900:
                bad.append(
                    "the victim kept stepping after choosing Exit match (last step %d)"
                    % last[victim]
                )
        else:
            if not cont_line:
                bad.append(
                    "the victim never answered CONTINUE (the dialog's Continue spectating button did not work)"
                )
        if "leave" in arm:
            if not (
                quit_line or left_line
            ):  # the quit seam or the main-menu tick closes the session first (a race), either is a leave
                bad.append(
                    "the victim never ended its session with reason=quit (ESC -> Quit game -> Yes did not run)"
                )
            if last[victim] > 3900:
                bad.append("the victim never left: it stepped to %d" % last[victim])
    if victim == "host" and ("exit" in arm or "leave" in arm) and not neg:
        for sv in survivors:
            hn = HUB_NOTICE.findall(logs[sv][0])
            out.append("%s hub-change notices: %s" % (sv, hn[:2]))
            if not hn:
                bad.append("%s saw no hub handover notice after the host left" % sv)
    return (not bad), out + ["FAIL: " + x for x in bad]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    try:
        ok, lines = check(
            sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "u54-client-spec"
        )
    except Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
