#!/usr/bin/env python3
"""check_rematch_residue.py -- mp:RM1: A REMATCH IN ONE PROCESS MUST ENTER ITS SECOND MATCH AS CLEAN AS
ITS FIRST, proven from both peers' session_begin_multi entry dumps rather than from the pixels.

THE BUG THIS GATES (2026-09-20 player report, session report §11.2): every 2nd+ match in one host
process entered `llm_strat_session_begin_multi` with the previous match's residue -- a stale game
clock, the P[1..] relation rows of `game::g::Players[]` zeroed (the lobby slot rows never rebuilt)
-- and the joiner's 2nd match per process entered with `p54bc=0` (`_G_LLM_NET_LOBBY_SCAN_HOST_COUNT`,
the lockstep-mode trigger) so it built a mode-2 solo game, free-ran past the host's horizon and sent
no frames. Symptoms: the step-50 desync `9806a28e` and the freezes `05acccbc` / `e758413a` at
committed 6060. Every FIRST match per process was clean. The writer was the pair of process-lifetime
one-shots on the manual-lobby entry prep in mh.dll (launch.cpp `on_begin_map_load`'s `host_done`
and `g_entry_started`), which ran the prep once per process instead of once per lobby.

WHAT IT ASSERTS, off the `; session_begin_multi ENTER ...` + `; PLAYERDUMP[sbm]` block the
`session_begin_multi logger` seam (net_seams.cpp) writes into each match's session directory, and
off the `[desync]` detector's match rollup:

  1. each peer's lane holds EXACTLY two session directories (game 1, game 2), each with exactly one
     session_begin_multi ENTER -- fewer is a refusal (the scenario did not reach the rematch),
     more is a refusal (a re-entry this tool was not written to attribute);
  2. game 2's ENTER on BOTH peers reads gclk=0 and p54bc=2 (game 1's must too -- a dirty game 1 means
     this tool cannot say what "clean" looks like on this run);
  3. game 2's P[0..2] relation rows equal game 1's on each peer, and the two peers' game-2 rows are
     identical to each other (the initial-state divergence that fed the first hashed sample);
  4. no `*** DESYNC` anywhere; game 1's `[desync] match end:` rollup (written at the session
     boundary, mp_session_close, into the match's own directory) reads 0 mismatching / >= 1 compared
     on both peers; and game 2's FIRST SAMPLE (`; [desync] first sample sent: step=50 state=<hash>`)
     exists on BOTH peers with the SAME state hash -- the tracker's "first 50 steps hash-identical"
     clause verbatim. Game 2 does not end inside the scenario (the runner stops the peers at
     `gameclock 12000`), so its rollup is optional -- checked for 0 mismatching / >= 1 compared when
     present. A peer that never entered lockstep takes no sample at all (the detector gates on
     SESSION_MODE 3): the freeze shape reads as a MISSING game-2 sample, never as a vacuous pass.

ABSENCE IS A FAILURE (check_module_bind.py's rule): no lane, no session directories, no ENTER line,
no rollup -- each is a refusal or a fail, never a pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_rematch_residue.py <host-run-dir> <client-run-dir>
  python tools/check_rematch_residue.py --selftest                planted lanes; every negative RED

`<run-dir>` is what test_ui.py's `post_check` machinery (with `post_check_peers`) hands a checker
for every peer: that lane's newest run directory (`sp_newest_run`, which prefers the PROCESS
"<stamp>_menu_<role>" directory). Each is resolved to its lane's `logs/` and the SESSION
directories of THAT PROCESS -- the ones whose session.json `process_dir` names the run directory
-- are read in chronological order. Not every session directory in the lane: `rematch_play` is a
lane SHARER (TL-LANEPOOL), and a shared lane is not re-provisioned between the owner's run and the
sharer's, so its `logs/` holds the owner's two sessions as well (measured on the first suite run:
4 directories, refused). A session directory without a readable `process_dir` is ignored, not
guessed at.
"""

import glob
import json
import os
import re
import shutil
import sys
import tempfile


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


# mh_session_dir.h's directory shape ("<UTC>Z_<mid8>_<slot>_<role>"); textually the same literal
# check_session_rollover.py carries, for the same no-import-chain reason.
SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")

ENTER_RE = re.compile(
    r"session_begin_multi ENTER p54bc=(-?\d+) pcount=(-?\d+) sess\(before\)=(-?\d+) gclk=(-?\d+)"
)
PREL_RE = re.compile(r";\s+P\[(\d)\] ctrl=0x[0-9a-f]{2} .*? rel=((?:[0-9a-f]{2} ?){8})")
DESYNC_RE = re.compile(r"\[desync\] \*\*\* DESYNC step=(\d+)")
FIRST_RE = re.compile(r"\[desync\] first sample sent: step=(\d+) state=([0-9A-F]{16})")
ROLLUP_RE = re.compile(r"\[desync\] match end: (\d+) mismatching / (\d+) compared sample")

EXPECT_P54BC = 2  # two humans; the scenario seats exactly host + one client
N_PLAYERS_DUMPED = 3  # the PLAYERDUMP seam prints P[0..2]


def session_process_dir(folder):
    """The `process_dir` a session directory's session.json names (mh_session_dir.h writes it at
    SESSION_BEGIN), or None when there is no readable record."""
    fp = os.path.join(folder, "session.json")
    try:
        with open(fp, encoding="utf-8", errors="replace") as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return None
    return d.get("process_dir") if isinstance(d, dict) else None


def session_runs(logs_dir, process_leaf):
    """The SESSION directories under `logs_dir` that belong to the process `process_leaf`
    (the "<stamp>_menu_<role>" directory the runner handed over), oldest first."""
    c = [
        d
        for d in glob.glob(os.path.join(logs_dir, "*"))
        if os.path.isdir(d)
        and SESSION_DIR_RE.match(os.path.basename(d))
        and session_process_dir(d) == process_leaf
    ]
    return sorted(c, key=lambda d: os.path.basename(d))


def read_log(folder):
    fp = os.path.join(folder, "mh_net.log")
    if not os.path.isfile(fp):
        raise Refusal("%s has no mh_net.log" % folder)
    with open(fp, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def parse_session(folder):
    """One session directory -> {enter: (p54bc, pcount, sess, gclk), rel: {i: 'xx xx ..'},
    desyncs: [step, ...], rollups: [(mismatching, compared), ...]}."""
    lines = read_log(folder)
    enters, rel, desyncs, rollups, first = [], {}, [], [], None
    for ln in lines:
        m = FIRST_RE.search(ln)
        if m:
            first = (int(m.group(1)), m.group(2))
            continue
        m = ENTER_RE.search(ln)
        if m:
            enters.append(tuple(int(x) for x in m.groups()))
            rel = {}  # the PLAYERDUMP that follows THIS entry
            continue
        m = PREL_RE.search(ln)
        if m and enters:
            rel[int(m.group(1))] = m.group(2).strip()
            continue
        m = DESYNC_RE.search(ln)
        if m:
            desyncs.append(int(m.group(1)))
            continue
        m = ROLLUP_RE.search(ln)
        if m:
            rollups.append((int(m.group(1)), int(m.group(2))))
    return {
        "dir": folder,
        "enters": enters,
        "rel": rel,
        "desyncs": desyncs,
        "rollups": rollups,
        "first": first,
    }


def lane_logs_dir(run_dir):
    if not os.path.isdir(run_dir):
        raise Refusal("no such run directory: %s" % run_dir)
    logs = os.path.dirname(run_dir.rstrip("\\/"))
    if os.path.basename(logs) != "logs":
        raise Refusal("%s is not a run directory under a lane's logs/" % run_dir)
    return logs


def check(run_dirs):
    if len(run_dirs) != 2:
        raise Refusal(
            "want exactly two peer run directories (host, client), got %d -- the residue is a "
            "cross-peer claim (register the row with post_check_peers)" % len(run_dirs)
        )
    peers = []
    for role, rd in zip(("host", "client"), run_dirs):
        logs = lane_logs_dir(rd)
        leaf = os.path.basename(rd.rstrip("\\/"))
        sess = session_runs(logs, leaf)
        print(
            "check_rematch_residue: %s lane=%s process=%s (%d session dir(s) of this process)"
            % (role, os.path.dirname(logs), leaf, len(sess))
        )
        if len(sess) != 2:
            raise Refusal(
                "%s process %s holds %d session directory(ies), want EXACTLY 2 (game 1, game 2) -- %s"
                % (role, leaf, len(sess), ", ".join(os.path.basename(d) for d in sess) or "(none)")
            )
        games = [parse_session(d) for d in sess]
        for k, g in enumerate(games):
            if len(g["enters"]) != 1:
                raise Refusal(
                    "%s game %d (%s): %d session_begin_multi ENTER line(s), want exactly 1 -- "
                    "the `session_begin_multi logger` seam must be armed ([net] lockstep_log=1) "
                    "and the session must have entered the game exactly once"
                    % (role, k + 1, os.path.basename(g["dir"]), len(g["enters"]))
                )
            if sorted(g["rel"]) != list(range(N_PLAYERS_DUMPED)):
                raise Refusal(
                    "%s game %d: PLAYERDUMP[sbm] P[] rows incomplete (have %s)"
                    % (role, k + 1, sorted(g["rel"]))
                )
        peers.append((role, games))

    fails = []
    for role, games in peers:
        for k, g in enumerate(games):
            p54bc, pcount, sess, gclk = g["enters"][0]
            print(
                "  %-6s game %d  %s  ENTER p54bc=%d pcount=%d sess(before)=%d gclk=%d"
                % (role, k + 1, os.path.basename(g["dir"]), p54bc, pcount, sess, gclk)
            )
            for i in range(N_PLAYERS_DUMPED):
                print("           P[%d] rel=%s" % (i, g["rel"][i]))
            if g["desyncs"]:
                fails.append(
                    "%s game %d: %d `*** DESYNC` line(s), first at step %d"
                    % (role, k + 1, len(g["desyncs"]), g["desyncs"][0])
                )
            if g["first"]:
                print("           [desync] first sample: step=%d state=%s" % g["first"])
            if g["rollups"]:
                for mm, cmp_ in g["rollups"]:
                    print("           [desync] rollup: %d mismatching / %d compared" % (mm, cmp_))
        g1, g2 = games
        # game 1 is the reference: this tool measures game 2 AGAINST it, so a dirty game 1 is a
        # refusal rather than a second failure with the same cause.
        if g1["enters"][0][3] != 0 or g1["enters"][0][0] != EXPECT_P54BC:
            raise Refusal(
                "%s game 1 is not the clean first-in-process shape (gclk=%d p54bc=%d) -- nothing "
                "here can be measured against it" % (role, g1["enters"][0][3], g1["enters"][0][0])
            )
        p54bc, _, _, gclk = g2["enters"][0]
        if gclk != 0:
            fails.append(
                "%s game 2 entered session_begin_multi with a STALE game clock (gclk=%d, game 1 "
                "had 0) -- the previous match's residue" % (role, gclk)
            )
        if p54bc != EXPECT_P54BC:
            fails.append(
                "%s game 2 entered session_begin_multi with p54bc=%d, want %d -- the lockstep-mode "
                "trigger was not re-armed for the rematch (a peer with p54bc<2 builds a mode-2 solo "
                "game and sends no frames)" % (role, p54bc, EXPECT_P54BC)
            )
        for i in range(N_PLAYERS_DUMPED):
            if g2["rel"][i] != g1["rel"][i]:
                fails.append(
                    "%s game 2 P[%d] rel=%s differs from game 1's %s -- the relation row was not "
                    "rebuilt from the lobby slots for the rematch"
                    % (role, i, g2["rel"][i], g1["rel"][i])
                )
        # game 1 ENDS inside the scenario, so its rollup must exist (written at the match boundary
        # into game 1's own directory) and must have compared something: the detector reports a
        # clean match too ("0 mismatching / N compared"), so no line means the boundary never wrote
        # one or nothing was ever sampled -- the armed-and-sampling-nothing shape.
        if not g1["rollups"]:
            fails.append(
                "%s game 1 has no `[desync] match end:` rollup -- the match boundary never wrote one "
                "(nothing sampled: the detector was armed and sampling nothing, or the session never "
                "closed)" % role
            )
        else:
            mm, cmp_ = g1["rollups"][-1]
            if mm != 0:
                fails.append("%s game 1 rollup: %d mismatching sample(s)" % (role, mm))
            if cmp_ < 1:
                fails.append(
                    "%s game 1 rollup: 0 compared samples -- the peers never exchanged one" % role
                )
        # game 2 runs until the runner stops it, so its rollup is optional; its FIRST SAMPLE is not.
        if g2["first"] is None:
            fails.append(
                "%s game 2 took no [desync] sample -- a peer that never entered lockstep (p54bc<2 -> "
                "SESSION_MODE 2) samples nothing; this is the freeze shape" % role
            )
        if g2["rollups"]:
            mm, cmp_ = g2["rollups"][-1]
            if mm != 0:
                fails.append("%s game 2 rollup: %d mismatching sample(s)" % (role, mm))
            if cmp_ < 1:
                fails.append(
                    "%s game 2 rollup: 0 compared samples -- the peers never exchanged a hashed "
                    "sample (the freeze shape: one side is not in lockstep)" % role
                )
    # cross-peer: the two game-2 relation tables must be byte-identical (host vs joiner), and so
    # must the first hashed sample of game 2 -- "the 2nd match's first 50 steps hash-identical".
    (hr, hg), (cr, cg) = peers
    hf, cf = hg[1]["first"], cg[1]["first"]
    if hf and cf:
        if hf != cf:
            fails.append(
                "game 2 first sample differs between the peers: host step=%d state=%s client "
                "step=%d state=%s -- the rematch diverged at its first hashed sample" % (hf + cf)
            )
        elif hg[0]["first"] and hg[0]["first"] != hf:
            # informational: same map, same seed byte, same slots -> a rematch normally reproduces
            # the first match's own first sample. Printed, not asserted (the map's seed is its own).
            print(
                "  note: game 2's first sample %s differs from game 1's %s (peers agree; a different "
                "map or seed, not a divergence)" % (hf[1], hg[0]["first"][1])
            )
    for i in range(N_PLAYERS_DUMPED):
        if hg[1]["rel"][i] != cg[1]["rel"][i]:
            fails.append(
                "game 2 P[%d] rel differs between the peers: host=%s client=%s"
                % (i, hg[1]["rel"][i], cg[1]["rel"][i])
            )
    for f in fails:
        print("[FAIL] %s" % f)
    if fails:
        return 1
    print(
        "[PASS] the rematch entered session_begin_multi as clean as the first match on both peers"
    )
    return 0


# ---- selftest: planted lanes ----------------------------------------------------------------------

_CLEAN = """[00:00:00.000] ; session_begin_multi ENTER p54bc=2 pcount=0 sess(before)=2 gclk=0
[00:00:00.000] ; PLAYERDUMP[sbm] host=1 PlayerSide=0 localidx=0 map_pcount=2 rng_seed=0x00 p54bc=2
[00:00:00.000] ;   P[0] ctrl=0x07 race=1 color=1 sprite=17728 side=0 rel=02 02 00 00 00 00 00 00
[00:00:00.000] ;   P[1] ctrl=0x07 race=1 color=0 sprite=17727 side=1 rel=02 02 00 00 00 00 00 00
[00:00:00.000] ;   P[2] ctrl=0x00 race=0 color=0 sprite=0 side=0 rel=00 00 00 00 00 00 00 00
[00:00:01.000] ; [desync] first sample sent: step=50 state=59135D2D75636E7C (hash source: our own walk)
[00:00:09.000] ; [desync] match end: 0 mismatching / 12 compared sample(s), 0 dropped as too old, 0 bad frame(s), 600 steps seen
"""
# game 2 as the scenario leaves it: sampled, never ended (no rollup)
_CLEAN2 = _CLEAN.rsplit("\n", 2)[0] + "\n"
_RESIDUE = """[00:00:00.000] ; session_begin_multi ENTER p54bc=2 pcount=0 sess(before)=2 gclk=56460
[00:00:00.000] ; PLAYERDUMP[sbm] host=1 PlayerSide=0 localidx=0 map_pcount=2 rng_seed=0x00 p54bc=2
[00:00:00.000] ;   P[0] ctrl=0x07 race=1 color=1 sprite=17728 side=0 rel=02 02 00 00 00 00 00 00
[00:00:00.000] ;   P[1] ctrl=0x07 race=1 color=0 sprite=17727 side=1 rel=00 00 00 00 00 00 00 00
[00:00:00.000] ;   P[2] ctrl=0x00 race=0 color=0 sprite=0 side=0 rel=00 00 00 00 00 00 00 00
[00:00:01.000] ; [desync] *** DESYNC step=50 peer=1 mine=0000000000000001 theirs=0000000000000002 first_region=8 p0_ai_econ (mismatch #1)
[00:00:09.000] ; [desync] match end: 1 mismatching / 12 compared sample(s), 0 dropped as too old, 0 bad frame(s), 600 steps seen
"""
_NOFRAMES = (
    _CLEAN.replace(
        "p54bc=2 pcount=0 sess(before)=2 gclk=0", "p54bc=0 pcount=0 sess(before)=2 gclk=5999"
    )
    .replace("0 mismatching / 12 compared", "0 mismatching / 0 compared")
    .replace(
        "[00:00:01.000] ; [desync] first sample sent: step=50 state=59135D2D75636E7C (hash source: our own walk)\n",
        "",
    )
)
_OTHERHASH = _CLEAN2.replace("state=59135D2D75636E7C", "state=0000000000000001")


def _plant(root, name, games):
    lane = os.path.join(root, name)
    logs = os.path.join(lane, "logs")
    menu_leaf = "20260921T000000Z_menu_solo"
    menu = os.path.join(logs, menu_leaf)
    os.makedirs(menu)
    # a FOREIGN session (the lane owner's, from before this process) that must be ignored
    foreign = os.path.join(logs, "20260920T235900Z_deadbeef_0_solo")
    os.makedirs(foreign)
    with open(os.path.join(foreign, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write(_RESIDUE)
    with open(os.path.join(foreign, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260920T235800Z_menu_solo"}, fh)
    for k, text in enumerate(games):
        d = os.path.join(logs, "20260921T00000%dZ_0000000%d_0_solo" % (k + 1, k + 1))
        os.makedirs(d)
        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write(text)
        with open(os.path.join(d, "session.json"), "w", encoding="utf-8") as fh:
            json.dump({"match_id": "0000000%d" % (k + 1), "process_dir": menu_leaf}, fh)
    return menu


def selftest():
    cases = [
        ("clean rematch on both peers (game 2 ended)", [_CLEAN, _CLEAN], [_CLEAN, _CLEAN], 0),
        (
            "clean rematch, game 2 still running (no rollup)",
            [_CLEAN, _CLEAN2],
            [_CLEAN, _CLEAN2],
            0,
        ),
        (
            "host game 2 carries the residue + a step-50 desync",
            [_CLEAN, _RESIDUE],
            [_CLEAN, _CLEAN],
            1,
        ),
        (
            "joiner game 2 never entered lockstep (p54bc=0, no sample)",
            [_CLEAN, _CLEAN],
            [_CLEAN, _NOFRAMES],
            1,
        ),
        (
            "game 2 first samples disagree across the peers",
            [_CLEAN, _CLEAN2],
            [_CLEAN, _OTHERHASH],
            1,
        ),
        (
            "game 1 rollup missing (detector sampled nothing)",
            [_CLEAN2, _CLEAN2],
            [_CLEAN, _CLEAN2],
            1,
        ),
        ("only one session per lane (rematch never reached)", [_CLEAN], [_CLEAN], "refuse"),
    ]
    bad = 0
    for title, host, client, want in cases:
        root = tempfile.mkdtemp(prefix="rm1_selftest_")
        try:
            h = _plant(root, "ui_x_host", host)
            c = _plant(root, "ui_x_c1", client)
            import io
            import contextlib

            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    got = check([h, c])
            except Refusal:
                got = "refuse"
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-62s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
    print("selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    dirs = [a for a in argv if not a.startswith("--")]
    try:
        return check(dirs)
    except Refusal as e:
        print("[REFUSED] %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
