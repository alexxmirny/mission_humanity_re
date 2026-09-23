#!/usr/bin/env python3
"""check_el2_mother.py -- mp:EL2: a fresh MP joiner's mother IS placed, by the joiner's own deploy,
in lockstep on both peers; the field's "eliminated with no building at 6 s" line was the joiner's
own ESC-quit. Judged from BOTH peers' logs of the `el2_mother_deploy` scenario (tools/test_ui.py).

WHAT THE FIELD LOG SAID AND WHAT IT MEANT (eb1c9f9d, 2026-09-20). The joiner's process log ends the
match on

    ; presence_lost player=1 mode=1 gate=eliminated self(sf=0x1b ua=1 ba=0 planet=31)
        caller=0x0049ddad sess=3 gclk=5999 | sf0=0x7 ua0=0 ba0=1  sf1=0x1b ua1=1 ba1=0
    ; on_gameover ENTER sess=2 outcome=4 gclk=5999 (downgrade=0)
    ; U17 graceful-leave: broadcast self-removal side=1 before quit-to-menu

and its session.json says `reason=quit`. Three facts pin what that is, none of them about
buildings: `mode=1` is llm_strat_player_presence_lost's FORCED arm, which never reads the unit or
building counts (net_diag.cpp's D20 comment; the retail body at 0x00498089 line 35 gates the
early-out on `mode == 0`); `caller=0x0049ddad` lies inside llm_net_player_remove (0x0049dca3-
0x0049ddc0), the self-removal on_quit_to_menu (U17) issues when a player leaves a running match;
and `sf=0x1b` is exactly that function's rewrite of the removed slot's flags (0x7 & ~0x4 | 0x10 |
0x8). `ua=1 ba=0` on the same line is an UNLANDED mothership: a fresh MP human is given ONE
airborne starting unit and no building (llm_game_land_players_on_planet's HUMAN arm), and the
building exists only once the player lands it (shift+right-click -> order 0x10 move + 0x18 deploy
-> llm_strat_unit_state_deploy_to_building @0x00481a6b -> llm_bldg_construct_finalize, then
llm_strat_unit_teardown -> presence_lost(player, 0) = the `gate=alive ua=0 ba=1` line, caller
0x00487f41). The host had landed by gclk 4159; the joiner had not, and quit at 5999.

THE CLAUSES, each on evidence a pixel cannot carry:

  1. THE SNAPSHOT (host lands first). BOTH peers' session logs carry the host's landing line --
     `presence_lost player=0 mode=0 gate=alive` from the deploy teardown (caller 0x00487f41) with
     `ua0=0 ba0=1` AND `ua1=1 ba1=0`: the eb1c9f9d field state reproduced, and the joiner is
     still `gate=alive` through it (an unlanded ship is presence). The same gclk on both peers:
     it is one sim event, applied in lockstep.

  2. THE JOINER'S PLACEMENT. BOTH peers' session logs then carry the joiner's landing line --
     `presence_lost player=1 mode=0 gate=alive`, same caller, with `ua1=0 ba1=1` -- later than
     clause 1's gclk and again at the same gclk on both peers. This is the clause the row exists
     for: the joiner's mother building is placed by the joiner's own order, through the same sim
     path as the host's, and the host sees it too.

  3. LOCKSTEP AGREEMENT ACROSS BOTH LANDINGS. The D21 in-band desync watch (`[desync] step=N
     peer=P MATCH`, every 50 steps under ini/desync_verbose.ini) agrees on a sample AFTER the
     joiner's landing on both peers, and neither log carries `*** DESYNC`. The harness cannot arm
     without libmh.dll (ruling Q4), so this is the determinism instrument of the shipped
     configuration.

  4. THE QUIT IS THE LINE. The joiner's process log carries the field line's shape, with the
     building counted this time: `presence_lost player=1 mode=1 gate=eliminated
     self(... ua=0 ba=1 ...) caller=0x0049ddad`, then `on_gameover ENTER ... outcome=4`, then
     `; U17 graceful-leave: broadcast self-removal side=1`; its session.json reads `reason: quit`.
     The host's session ends `reason: gameover` with no `fast-drop: transport-dead peer` (it learned
     of the departure from the removal frame, not from the socket).

ABSENCE IS A FAILURE: a missing dir, log, session.json or line is a REFUSAL, never a vacuous pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_el2_mother.py <host-session-dir> <client-session-dir>
  python tools/check_el2_mother.py --selftest        planted logs; every negative RED

The run dirs are the SESSION dirs (test_ui.py `post_check_session: True`); like
check_resync_storm.net_log_lines, each peer's lines are its session dir's mh_net.log PLUS the
process dir its session.json names (the quit-time lines land in the process dir: SES1's
mp_session_close("quit") switches the log folder a few statements before them).
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

# The needles, verbatim from the emitters (tools/data/log_formats.json rows net.presence_lost /
# net.gameover_enter / net.graceful_leave_broadcast / net.desync_watch / net.fast_drop name this
# file as a parser -- lint_log_formats ARM A checks these literals are here).
PRESENCE_NEEDLE = "; presence_lost player="
GAMEOVER_NEEDLE = "on_gameover ENTER"
LEAVE_NEEDLE = "; U17 graceful-leave: broadcast self-removal side="
DESYNC_BAD_NEEDLE = "*** DESYNC step="
FASTDROP_NEEDLE = "fast-drop: transport-dead peer"

PRESENCE_RE = re.compile(
    r"; presence_lost player=(\d+) mode=(\d+) gate=(\w+) self\(sf=0x([0-9a-f]+) ua=(-?\d+) ba=(-?\d+) "
    r"planet=(\d+)[^)]*\) caller=0x([0-9a-f]{8}) sess=(\d+) gclk=(-?\d+) \| "
    r"sf0=0x([0-9a-f]+) ua0=(-?\d+) ba0=(-?\d+)  sf1=0x([0-9a-f]+) ua1=(-?\d+) ba1=(-?\d+)"
)
GAMEOVER_RE = re.compile(r"on_gameover ENTER sess=(\d+) outcome=(\d+) gclk=(-?\d+)")
LEAVE_RE = re.compile(r"; U17 graceful-leave: broadcast self-removal side=(\d+)")
DESYNC_MATCH_RE = re.compile(r"; \[desync\] step=(\d+) peer=(-?\d+) MATCH state=")

# The two callers the mechanism turns on (EN /eng/mh.exe VAs, docs/symbols.md):
#   0x00487f41 -- the return address inside llm_strat_unit_teardown (0x00487ba5) after its
#                 presence_lost(player, 0) call at 0x00487f3c: the deploy consuming the ship.
#   0x0049ddad -- the return address inside llm_net_player_remove (0x0049dca3) after its
#                 presence_lost(idx, 1) call at 0x0049dda8: the quitter's own self-removal.
CALLER_UNIT_TEARDOWN = 0x00487F41
CALLER_PLAYER_REMOVE = 0x0049DDAD
# llm_net_player_remove's flag rewrite on the removed slot: (0x7 & 0xfb) | 0x10 (NET_DROPPED) | 0x8 (AI).
SF_REMOVED = 0x1B
OUTCOME_DEFEAT = 4


class Refusal(Exception):
    pass


def net_log_lines(run_dir):
    """The peer's mh_net.log lines: the session dir's own PLUS the process dir session.json names."""
    if not os.path.isdir(run_dir):
        raise Refusal("run dir missing: %s" % run_dir)
    cands = [os.path.join(run_dir, "mh_net.log")]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            parent = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
            cands.append(os.path.join(parent, pd, "mh_net.log"))
    out = []
    for fp in cands:
        if os.path.isfile(fp):
            try:
                out.extend(open(fp, encoding="utf-8", errors="replace").read().splitlines())
            except OSError:
                pass
    if not out:
        raise Refusal("no mh_net.log content under %s" % run_dir)
    return out


def session_json(run_dir):
    sj = os.path.join(run_dir, "session.json")
    if not os.path.isfile(sj):
        raise Refusal("no session.json under %s" % run_dir)
    try:
        return json.load(open(sj, encoding="utf-8"))
    except Exception as e:
        raise Refusal("unreadable session.json under %s: %s" % (run_dir, e))


def presence_rows(lines):
    """Every presence_lost line, parsed: dict(player, mode, gate, sf, ua, ba, planet, caller,
    sess, gclk, ua0, ba0, ua1, ba1)."""
    rows = []
    for ln in lines:
        if PRESENCE_NEEDLE not in ln:
            continue
        m = PRESENCE_RE.search(ln)
        if not m:
            continue
        g = m.groups()
        rows.append(
            {
                "player": int(g[0]),
                "mode": int(g[1]),
                "gate": g[2],
                "sf": int(g[3], 16),
                "ua": int(g[4]),
                "ba": int(g[5]),
                "planet": int(g[6]),
                "caller": int(g[7], 16),
                "sess": int(g[8]),
                "gclk": int(g[9]),
                "sf0": int(g[10], 16),
                "ua0": int(g[11]),
                "ba0": int(g[12]),
                "sf1": int(g[13], 16),
                "ua1": int(g[14]),
                "ba1": int(g[15]),
                "line": ln,
            }
        )
    return rows


def landing_row(rows, player, ua0, ba0, ua1, ba1):
    """The first DEPLOY presence line for `player` (mode 0, gate alive, unit_teardown caller)
    whose two count columns read exactly the given values; None when absent."""
    for r in rows:
        if (
            r["player"] == player
            and r["mode"] == 0
            and r["gate"] == "alive"
            and r["caller"] == CALLER_UNIT_TEARDOWN
            and (r["ua0"], r["ba0"], r["ua1"], r["ba1"]) == (ua0, ba0, ua1, ba1)
        ):
            return r
    return None


def quit_row(rows):
    """The quitter's own self-removal line: player 1, mode 1, eliminated, player_remove caller."""
    for r in rows:
        if (
            r["player"] == 1
            and r["mode"] == 1
            and r["gate"] == "eliminated"
            and r["caller"] == CALLER_PLAYER_REMOVE
        ):
            return r
    return None


def desync_max_match_step(lines):
    best = -1
    for ln in lines:
        m = DESYNC_MATCH_RE.search(ln)
        if m:
            best = max(best, int(m.group(1)))
    return best


def check(host_dir, client_dir):
    """Returns (fails, summary)."""
    fails = []
    host_lines = net_log_lines(host_dir)
    cli_lines = net_log_lines(client_dir)
    host_sj = session_json(host_dir)
    cli_sj = session_json(client_dir)
    step_ms = int(cli_sj.get("sim_step_ms") or host_sj.get("sim_step_ms") or 20)
    hp = presence_rows(host_lines)
    cp = presence_rows(cli_lines)

    # ---- clause 1: the host's landing, seen by both, joiner airborne and alive ----------------
    h1 = landing_row(hp, 0, 0, 1, 1, 0)
    c1 = landing_row(cp, 0, 0, 1, 1, 0)
    if h1 is None:
        fails.append(
            "clause 1: HOST log has no deploy presence line for player 0 reading ua0=0 ba0=1 ua1=1 ba1=0 "
            "(caller 0x%08x) -- the host's mother did not land, or the joiner's ship was not airborne then"
            % CALLER_UNIT_TEARDOWN
        )
    if c1 is None:
        fails.append(
            "clause 1: CLIENT log has no matching deploy presence line for player 0 (the host's landing)"
        )
    if h1 and c1 and h1["gclk"] != c1["gclk"]:
        fails.append(
            "clause 1: host landing gclk differs across peers: host %d vs client %d"
            % (h1["gclk"], c1["gclk"])
        )

    # ---- clause 2: the joiner's landing, seen by both, later than clause 1 -------------------
    h2 = landing_row(hp, 1, 0, 1, 0, 1)
    c2 = landing_row(cp, 1, 0, 1, 0, 1)
    if h2 is None:
        fails.append(
            "clause 2: HOST log has no deploy presence line for player 1 reading ua1=0 ba1=1 -- the joiner's "
            "mother was NOT placed on the host (the EL2 report's shape, if real)"
        )
    if c2 is None:
        fails.append(
            "clause 2: CLIENT log has no deploy presence line for player 1 reading ua1=0 ba1=1 -- the joiner's "
            "own deploy did not place its mother"
        )
    if h2 and c2 and h2["gclk"] != c2["gclk"]:
        fails.append(
            "clause 2: joiner landing gclk differs across peers: host %d vs client %d"
            % (h2["gclk"], c2["gclk"])
        )
    if h1 and h2 and not (h2["gclk"] > h1["gclk"]):
        fails.append(
            "clause 2: joiner landing (gclk %d) is not after the host's (gclk %d)"
            % (h2["gclk"], h1["gclk"])
        )

    # ---- clause 3: the in-band desync watch agrees past the joiner's landing -----------------
    hbad = [ln for ln in host_lines if DESYNC_BAD_NEEDLE in ln]
    cbad = [ln for ln in cli_lines if DESYNC_BAD_NEEDLE in ln]
    if hbad or cbad:
        fails.append("clause 3: desync watch flagged a mismatch: %s" % (hbad + cbad)[0].strip())
    hstep = desync_max_match_step(host_lines)
    cstep = desync_max_match_step(cli_lines)
    if hstep < 0 or cstep < 0:
        fails.append(
            "clause 3: no `[desync] step=N peer=P MATCH` sample on %s -- is ini/desync_verbose.ini merged?"
            % ("both peers" if hstep < 0 and cstep < 0 else ("host" if hstep < 0 else "client"))
        )
    elif h2:
        need = h2["gclk"] // step_ms
        if min(hstep, cstep) < need:
            fails.append(
                "clause 3: last agreeing desync sample (host step %d, client step %d) is before the joiner's "
                "landing (gclk %d = step %d at %d ms/step)"
                % (hstep, cstep, h2["gclk"], need, step_ms)
            )

    # ---- clause 4: the quit is the field line -------------------------------------------------
    q = quit_row(cp)
    if q is None:
        fails.append(
            "clause 4: CLIENT log has no `presence_lost player=1 mode=1 gate=eliminated ... caller=0x%08x` "
            "(the self-removal from the ESC quit)" % CALLER_PLAYER_REMOVE
        )
    else:
        if (q["ua"], q["ba"]) != (0, 1):
            fails.append(
                "clause 4: the quit line's own counts read ua=%d ba=%d, expected ua=0 ba=1 (landed)"
                % (q["ua"], q["ba"])
            )
        if q["sf"] != SF_REMOVED:
            fails.append(
                "clause 4: the quit line's sf=0x%x, expected 0x%x (llm_net_player_remove's rewrite)"
                % (q["sf"], SF_REMOVED)
            )
        # ordering: the on_gameover with outcome 4 and the U17 broadcast follow the quit line
        idx = cli_lines.index(q["line"])
        after = cli_lines[idx + 1 :]
        go = None
        for ln in after:
            if GAMEOVER_NEEDLE in ln:
                m = GAMEOVER_RE.search(ln)
                if m:
                    go = int(m.group(2))
                break
        if go is None:
            fails.append("clause 4: no `on_gameover ENTER` after the quit line on the client")
        elif go != OUTCOME_DEFEAT:
            fails.append(
                "clause 4: client's on_gameover outcome=%d after the quit, expected %d (self-removal)"
                % (go, OUTCOME_DEFEAT)
            )
        lv = [LEAVE_RE.search(ln) for ln in after if LEAVE_NEEDLE in ln]
        lv = [m for m in lv if m]
        if not lv:
            fails.append(
                "clause 4: no U17 graceful-leave broadcast after the quit line on the client"
            )
        elif int(lv[0].group(1)) != 1:
            fails.append(
                "clause 4: graceful-leave broadcast side=%s, expected 1 (the joiner)"
                % lv[0].group(1)
            )
    if cli_sj.get("reason") != "quit":
        fails.append(
            "clause 4: client session.json reason=%r, expected 'quit'" % cli_sj.get("reason")
        )
    if host_sj.get("reason") != "gameover":
        fails.append(
            "clause 4: host session.json reason=%r, expected 'gameover' (ended by the removal frame)"
            % host_sj.get("reason")
        )
    if any(FASTDROP_NEEDLE in ln for ln in host_lines):
        fails.append(
            "clause 4: host log carries the transport-death fast-drop -- the removal did not arrive by the wire"
        )

    summary = (
        "host landed gclk=%s, joiner landed gclk=%s, desync MATCH to step %d/%d, quit gclk=%s"
        % (
            h1["gclk"] if h1 else "-",
            h2["gclk"] if h2 else "-",
            hstep,
            cstep,
            q["gclk"] if q else "-",
        )
    )
    return fails, summary


# ---- selftest: planted logs, every negative RED --------------------------------------------------

_PL = (
    "[00:00:%02d.000] ; presence_lost player=%d mode=%d gate=%s self(sf=0x%x ua=%d ba=%d planet=31) "
    "caller=0x%08x sess=%d gclk=%d | sf0=0x%x ua0=%d ba0=%d  sf1=0x%x ua1=%d ba1=%d"
)


def _host_land(g=4159):
    return _PL % (10, 0, 0, "alive", 0x7, 0, 1, CALLER_UNIT_TEARDOWN, 3, g, 0x7, 0, 1, 0x7, 1, 0)


def _joiner_land(g=7239):
    return _PL % (14, 1, 0, "alive", 0x7, 0, 1, CALLER_UNIT_TEARDOWN, 3, g, 0x7, 0, 1, 0x7, 0, 1)


def _quit(ua=0, ba=1, sf=SF_REMOVED, caller=CALLER_PLAYER_REMOVE, g=10019):
    return _PL % (20, 1, 1, "eliminated", sf, ua, ba, caller, 3, g, 0x7, 0, 1, sf, ua, ba)


def _desync(step, peer):
    return "[00:00:%02d.000] ; [desync] step=%d peer=%d MATCH state=DEADBEEF" % (
        step // 10,
        step,
        peer,
    )


def _plant(
    root, host_sess, host_menu, cli_sess, cli_menu, host_reason="gameover", cli_reason="quit"
):
    hs = os.path.join(root, "h", "20260922T000100Z_deadbeef_0_solo")
    hm = os.path.join(root, "h", "20260922T000000Z_menu_solo")
    cs = os.path.join(root, "c", "20260922T000100Z_deadbeef_1_solo")
    cm = os.path.join(root, "c", "20260922T000000Z_menu_solo")
    for d in (hs, hm, cs, cm):
        os.makedirs(d, exist_ok=True)
    open(os.path.join(hs, "mh_net.log"), "w").write("\n".join(host_sess) + "\n")
    open(os.path.join(hm, "mh_net.log"), "w").write("\n".join(host_menu) + "\n")
    open(os.path.join(cs, "mh_net.log"), "w").write("\n".join(cli_sess) + "\n")
    open(os.path.join(cm, "mh_net.log"), "w").write("\n".join(cli_menu) + "\n")
    json.dump(
        {
            "match_id": "deadbeef",
            "reason": host_reason,
            "sim_step_ms": 20,
            "process_dir": os.path.basename(hm),
        },
        open(os.path.join(hs, "session.json"), "w"),
    )
    json.dump(
        {
            "match_id": "deadbeef",
            "reason": cli_reason,
            "sim_step_ms": 20,
            "process_dir": os.path.basename(cm),
        },
        open(os.path.join(cs, "session.json"), "w"),
    )
    return hs, cs


def selftest():
    good_host_sess = [
        _host_land(),
        _desync(200, 1),
        _joiner_land(),
        _desync(400, 1),
        _desync(500, 1),
    ]
    good_host_menu = [
        "[00:00:20.100] ; on_gameover ENTER sess=3 outcome=8 gclk=10019 (downgrade=1)"
    ]
    good_cli_sess = [
        _host_land(),
        _desync(200, 0),
        _joiner_land(),
        _desync(400, 0),
        _desync(500, 0),
    ]
    good_cli_menu = [
        _quit(),
        "[00:00:20.001] ; on_gameover ENTER sess=2 outcome=4 gclk=10019 (downgrade=0)",
        "[00:00:20.002] ; U17 graceful-leave: broadcast self-removal side=1 before quit-to-menu",
    ]
    cases = []

    def arm(name, hs, hm, cs, cm, want_ok, hr="gameover", cr="quit"):
        cases.append((name, hs, hm, cs, cm, want_ok, hr, cr))

    arm("good", good_host_sess, good_host_menu, good_cli_sess, good_cli_menu, True)
    arm(
        "no host landing on host",
        good_host_sess[1:],
        good_host_menu,
        good_cli_sess,
        good_cli_menu,
        False,
    )
    arm(
        "no host landing on client",
        good_host_sess,
        good_host_menu,
        good_cli_sess[1:],
        good_cli_menu,
        False,
    )
    arm(
        "host landing gclk differs",
        [_host_land(4159)] + good_host_sess[1:],
        good_host_menu,
        [_host_land(4259)] + good_cli_sess[1:],
        good_cli_menu,
        False,
    )
    arm(
        "joiner never lands (the EL2 shape)",
        [_host_land(), _desync(200, 1), _desync(400, 1)],
        good_host_menu,
        [_host_land(), _desync(200, 0), _desync(400, 0)],
        [_quit(ua=1, ba=0)] + good_cli_menu[1:],
        False,
    )
    arm(
        "joiner lands on client only",
        [_host_land(), _desync(200, 1), _desync(400, 1)],
        good_host_menu,
        good_cli_sess,
        good_cli_menu,
        False,
    )
    arm(
        "joiner lands before the host",
        [_host_land(7239), _joiner_land(4159), _desync(500, 1)],
        good_host_menu,
        [_host_land(7239), _joiner_land(4159), _desync(500, 0)],
        good_cli_menu,
        False,
    )
    arm(
        "desync flagged",
        good_host_sess + ["[00:00:19.000] ; [desync] *** DESYNC step=450 peer=1 ours=1 theirs=2"],
        good_host_menu,
        good_cli_sess,
        good_cli_menu,
        False,
    )
    arm(
        "no desync samples",
        [_host_land(), _joiner_land()],
        good_host_menu,
        [_host_land(), _joiner_land()],
        good_cli_menu,
        False,
    )
    arm(
        "desync agreement ends before the joiner's landing",
        [_host_land(), _desync(200, 1), _joiner_land()],
        good_host_menu,
        [_host_land(), _desync(200, 0), _joiner_land()],
        good_cli_menu,
        False,
    )
    arm("no quit line", good_host_sess, good_host_menu, good_cli_sess, good_cli_menu[1:], False)
    arm(
        "quit line with the wrong caller",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        [_quit(caller=0x00487F41)] + good_cli_menu[1:],
        False,
    )
    arm(
        "quit line with the wrong flags",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        [_quit(sf=0x7)] + good_cli_menu[1:],
        False,
    )
    arm(
        "no on_gameover after the quit",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        [good_cli_menu[0], good_cli_menu[2]],
        False,
    )
    arm(
        "on_gameover outcome 7 after the quit",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        [good_cli_menu[0], good_cli_menu[1].replace("outcome=4", "outcome=7"), good_cli_menu[2]],
        False,
    )
    arm(
        "no graceful-leave broadcast",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        good_cli_menu[:2],
        False,
    )
    arm(
        "client reason not quit",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        good_cli_menu,
        False,
        cr="gameover",
    )
    arm(
        "host reason not gameover",
        good_host_sess,
        good_host_menu,
        good_cli_sess,
        good_cli_menu,
        False,
        hr="quit",
    )
    arm(
        "host ended by fast-drop",
        good_host_sess,
        good_host_menu + ["[00:00:20.050] ; U17 fast-drop: transport-dead peer side=1"],
        good_cli_sess,
        good_cli_menu,
        False,
    )
    bad = 0
    for name, hs, hm, cs, cm, want_ok, hr, cr in cases:
        root = tempfile.mkdtemp(prefix="el2_selftest_")
        try:
            hd, cd = _plant(root, hs, hm, cs, cm, hr, cr)
            fails, summary = check(hd, cd)
            ok = not fails
            verdict = "ok" if ok == want_ok else "WRONG"
            if ok != want_ok:
                bad += 1
            print(
                "  [%s] %-45s -> %s%s"
                % (
                    verdict,
                    name,
                    "PASS" if ok else "FAIL",
                    "" if ok else " (" + fails[0][:70] + ")",
                )
            )
        finally:
            shutil.rmtree(root, ignore_errors=True)
    # the refusal case: a missing dir must raise, never pass
    try:
        check(
            os.path.join(tempfile.gettempdir(), "el2_nonexistent_x"),
            os.path.join(tempfile.gettempdir(), "el2_nonexistent_y"),
        )
        print("  [WRONG] missing dirs -> returned instead of refusing")
        bad += 1
    except Refusal:
        print("  [ok] missing dirs -> REFUSAL")
    n = len(cases) + 1
    print("check_el2_mother selftest: %d/%d" % (n - bad, n))
    return 0 if bad == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print("usage: check_el2_mother.py <host-session-dir> <client-session-dir>")
        return 2
    try:
        fails, summary = check(args.dirs[0], args.dirs[1])
    except Refusal as e:
        print("check_el2_mother: REFUSED -- %s" % e)
        return 1
    if fails:
        print("check_el2_mother: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_el2_mother: PASS -- %s" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
