#!/usr/bin/env python3
"""check_cheat_gate.py -- mp:CH1: A SINGLE-PLAYER CHEAT TYPED IN A NETWORK GAME MUST BE REFUSED, proven
from both peers' mh_net.log rather than from the pixels.

THE BUG THIS GATES (2026-09-20 player report, session report §10.1): the strategic view's Shift+Enter
opens the hidden developer console on the SAME text line the chat uses (`_G_LLM_CHAT_MODE = 3`), and
that arm of llm_strat_input_update is not gated on the session mode -- so in a lockstep match a
player can type any entry of the 47-string cheat table and llm_debug_console_dispatch runs
it. `_NUCLEAR BOMB` (catalog idx 0x25) issues order 0xfa on the REPLICATED lane: every peer executes
llm_combat_credit_planet_conquest_kills, which kills every unit and building of the first other
player alive on the planet -- an unearned, unanswerable order. The local-write cheats (`_SPY`,
`_NETDELAY UP/DOWN/SYNC`) diverge the typing peer alone. mh.dll's CH1 seam (seams/ui_cheat_gate.cpp)
blanks every console-mode line matching the catalog except `_NETDELAY` in `SESSION_MODE == 3`, at
llm_chat_history_push -- upstream of the dispatcher and of every chat send.

WHAT IT ASSERTS, off the CH1 seam's own lines and the D20 / D21 diagnostics:

  1. EXACTLY ONE peer submitted a console line matching idx 0x25 -- the `; [chat] submit mode=3 ...
     cheat_idx=0x25` line (the redacted submit log). None = the scenario never typed it (a refusal:
     this tool cannot say what the gate did with a line nobody submitted); two = a topology this
     tool was not written for.
  2. That peer carries `; CH1: cheat '_NUCLEAR BOMB' (idx 0x25) refused in a network game`. Absent
     = FAIL. The gate-off reproduction arm ([input] cheat_gate=0) writes `... RUNS in a network game`
     instead, and the failure names that line when it sees it.
  3. NEITHER peer carries a `presence_lost player=<p> mode=0 gate=eliminated` line before its
     SESSION_END -- the sim-path elimination (D20: mode 0 + no units + no buildings) that an
     unrefused idx-0x25 produces on BOTH peers (the order is replicated). The quit at the end of the
     walk produces `mode=1` lines (a FORCED removal) and those are expected.
  4. NEITHER peer carries a `[desync] *** DESYNC` sample. (An unrefused order cheat does NOT desync
     -- both peers execute it -- so this clause catches the LOCAL-write class, `_SPY` and the
     `_NETDELAY` writes, not idx 0x25; it is here because the row's done_when names it and because
     a gate that refused the order but let a local write through would otherwise read green.)
  5. If the `[netind]` sampler ran (`[hud] net_indicator=1`, off by default in the suite), the
     `peer0 name=` column never changes across the run -- the field's morph.
  6. NEITHER peer's mh_harness.log (the row arms the harness with order_log=1) carries an
     `;ord ... code=FA` row -- order 0xfa in ORDER_PENDING or ORDER_QUEUE. THIS IS THE CLAUSE THAT
     SEES THE CHEAT: one blast (2000 to every unit, 5000 to every building of the victim) does not
     kill a 6-second-old mothership, so clause 3 never fires on the rig, and the order is replicated
     so clause 4 never does either. The gate-off arm shows `code=FA` staged on BOTH peers
     (`;ord   P0 own=0001 unit=0 code=FA p0=250 exec=6400ms`, then `Q0` when released); the gate
     must leave neither with one. A lane without mh_harness.log is a refusal, not a pass.

ABSENCE IS A FAILURE (check_module_bind.py's rule): no lane, no session directory, no mh_net.log --
each is a refusal, never a pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_cheat_gate.py <host-run-dir> <client-run-dir>
  python tools/check_cheat_gate.py --selftest                planted lanes; every negative RED

`<run-dir>` is what test_ui.py's `post_check` machinery (with `post_check_peers`) hands a checker
for every peer: that lane's newest run directory (the PROCESS "<stamp>_menu_<role>" directory). Each
is resolved to its lane's `logs/` and the SESSION directories of THAT PROCESS (session.json
`process_dir`), because ch1_cheat is a lane SHARER (TL-LANEPOOL) and a shared lane's `logs/` holds
the owner's sessions as well. The process directory's own mh_net.log is read too: the CH1 arm
banner lands there before any session opens.
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


SESSION_DIR_RE = re.compile(r"^\d{8}T\d{6}Z_[0-9a-f]{8}_\d+_[A-Za-z0-9]+$")

SUBMIT_RE = re.compile(
    r"; \[chat\] submit mode=(\d+) sess=(\d+) len=(\d+) first='(.?)' cheat_idx=(0x[0-9a-f]{2}|-)"
)
REFUSED_RE = re.compile(r"; CH1: cheat '([^']*)' \(idx (0x[0-9a-f]{2})\) refused in a network game")
RUNS_RE = re.compile(r"; CH1: cheat '([^']*)' \(idx (0x[0-9a-f]{2})\) RUNS in a network game")
ELIM_RE = re.compile(r"; presence_lost player=(\d+) mode=(\d+) gate=eliminated")
DESYNC_RE = re.compile(r"\[desync\] \*\*\* DESYNC step=(\d+)")
NETIND_RE = re.compile(r"; \[netind\] peer0 name=(\S+)")
ORD_FA_RE = re.compile(r"^;ord\s+(\S+) own=([0-9A-F]{4}) unit=(\d+) code=FA ")
END_RE = re.compile(r"; \[session\] SESSION_END ")

CHEAT_IDX = "0x25"
CHEAT_CMD = "_NUCLEAR BOMB"


def session_process_dir(folder):
    fp = os.path.join(folder, "session.json")
    try:
        with open(fp, encoding="utf-8", errors="replace") as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return None
    return d.get("process_dir") if isinstance(d, dict) else None


def session_runs(logs_dir, process_leaf):
    c = [
        d
        for d in glob.glob(os.path.join(logs_dir, "*"))
        if os.path.isdir(d)
        and SESSION_DIR_RE.match(os.path.basename(d))
        and session_process_dir(d) == process_leaf
    ]
    return sorted(c, key=lambda d: os.path.basename(d))


def read_lines(folder):
    fp = os.path.join(folder, "mh_net.log")
    if not os.path.isfile(fp):
        return None
    with open(fp, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def harness_fa_rows(run_dir):
    """The `;ord ... code=FA` rows of the process directory's mh_harness.log (the harness writes
    into the process directory, not the session's). Refuses when the log is absent: the row arms
    the harness, so a missing log means the arm never happened."""
    fp = os.path.join(os.path.abspath(run_dir), "mh_harness.log")
    if not os.path.isfile(fp):
        raise Refusal(
            "%s has no mh_harness.log (order_log=1 is the row's arm; nothing to read)" % run_dir
        )
    out = []
    with open(fp, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = ORD_FA_RE.match(ln)
            if m:
                out.append(ln.strip())
    return out


def peer_lines(run_dir):
    """All mh_net.log lines of one peer: the process directory's, then each of its sessions', in
    order. Refuses when nothing is readable."""
    run_dir = os.path.abspath(run_dir)
    if not os.path.isdir(run_dir):
        raise Refusal("%s is not a directory" % run_dir)
    logs_dir = os.path.dirname(run_dir)
    leaf = os.path.basename(run_dir)
    out = []
    proc = read_lines(run_dir)
    if proc is not None:
        out.extend(proc)
    sessions = session_runs(logs_dir, leaf)
    for s in sessions:
        ln = read_lines(s)
        if ln is not None:
            out.extend(ln)
    if not out:
        raise Refusal("%s: no mh_net.log in the process directory or its sessions" % run_dir)
    if not sessions:
        raise Refusal(
            "%s: no session directory names it as process_dir (the walk never opened a lobby)"
            % run_dir
        )
    return out


def analyse(lines):
    r = {
        "submits": [],
        "refused": [],
        "runs": [],
        "elim_pre_end": [],
        "desync": [],
        "netind": [],
    }
    ended = False
    for ln in lines:
        m = SUBMIT_RE.search(ln)
        if m:
            r["submits"].append(m.groups())
            continue
        m = REFUSED_RE.search(ln)
        if m:
            r["refused"].append(m.groups())
            continue
        m = RUNS_RE.search(ln)
        if m:
            r["runs"].append(m.groups())
            continue
        m = ELIM_RE.search(ln)
        if m and m.group(2) == "0" and not ended:
            r["elim_pre_end"].append(ln.strip())
            continue
        m = DESYNC_RE.search(ln)
        if m:
            r["desync"].append(int(m.group(1)))
            continue
        m = NETIND_RE.search(ln)
        if m:
            r["netind"].append(m.group(1))
            continue
        if END_RE.search(ln):
            ended = True
    return r


def check(dirs):
    if len(dirs) != 2:
        raise Refusal("expected exactly two run directories (host, client); got %d" % len(dirs))
    peers = [analyse(peer_lines(d)) for d in dirs]
    fa = [harness_fa_rows(d) for d in dirs]
    fails = []
    typed = [
        i
        for i, p in enumerate(peers)
        if any(s[0] == "3" and s[4] == CHEAT_IDX for s in p["submits"])
    ]
    if not typed:
        raise Refusal(
            "no peer submitted a mode-3 line matching cheat_idx=%s (`; [chat] submit`)" % CHEAT_IDX
        )
    if len(typed) > 1:
        raise Refusal(
            "both peers submitted a mode-3 line matching cheat_idx=%s; this tool attributes one typist"
            % CHEAT_IDX
        )
    t = peers[typed[0]]
    print(
        "  typing peer: %s (submits: %s)"
        % (dirs[typed[0]], ["mode=%s len=%s idx=%s" % (s[0], s[2], s[4]) for s in t["submits"]])
    )
    if not any(c == CHEAT_CMD and i == CHEAT_IDX for c, i in t["refused"]):
        why = "no `; CH1: cheat '%s' (idx %s) refused in a network game` on the typing peer" % (
            CHEAT_CMD,
            CHEAT_IDX,
        )
        if t["runs"]:
            why += " -- it RAN instead: %s" % ["%s (idx %s)" % x for x in t["runs"]]
        fails.append(why)
    else:
        print("  refused: %s" % ["%s (idx %s)" % x for x in t["refused"]])
    for i, p in enumerate(peers):
        if p["elim_pre_end"]:
            fails.append(
                "%s: a sim-path elimination before SESSION_END: %s"
                % (dirs[i], p["elim_pre_end"][0])
            )
        if p["desync"]:
            fails.append("%s: [desync] *** DESYNC at steps %s" % (dirs[i], p["desync"][:5]))
        if p["netind"] and len(set(p["netind"])) > 1:
            fails.append(
                "%s: [netind] peer0 name= morphed: %s"
                % (dirs[i], " -> ".join(dict.fromkeys(p["netind"])))
            )
        if fa[i]:
            fails.append(
                "%s: order 0xfa reached the order buffers (%d row(s)): %s"
                % (dirs[i], len(fa[i]), fa[i][0])
            )
    for f in fails:
        print("  [FAIL] %s" % f)
    if fails:
        print("check_cheat_gate: FAIL (%d)" % len(fails))
        return 1
    print(
        "check_cheat_gate: PASS -- '%s' (idx %s) refused on the typing peer; no 0xfa in either peer's order "
        "buffers, no elimination, no desync%s"
        % (CHEAT_CMD, CHEAT_IDX, ", netind name stable" if any(p["netind"] for p in peers) else "")
    )
    return 0


# ---- selftest ---------------------------------------------------------------------------------

BANNER = "[00:00:00.000] ; CH1: cheat console gate armed (2/2 hooks) -- in a network game only _NETDELAY (idx 0x0b) passes"
SUBMIT = "[00:00:10.000] ; [chat] submit mode=3 sess=3 len=13 first='_' cheat_idx=0x25"
REFUSED = "[00:00:10.001] ; CH1: cheat '_NUCLEAR BOMB' (idx 0x25) refused in a network game"
RUNS = "[00:00:10.001] ; CH1: cheat '_NUCLEAR BOMB' (idx 0x25) RUNS in a network game ([input] cheat_gate=0)"
ELIM0 = "[00:00:11.000] ; presence_lost player=0 mode=0 gate=eliminated self(sf=0xb ua=0 ba=0 planet=31) caller=0x00487f41 sess=3 gclk=7979 | sf0=0x7 ua0=0 ba0=0  sf1=0x7 ua1=1 ba1=1"
ELIM1_FORCED = "[00:00:20.000] ; presence_lost player=1 mode=1 gate=eliminated self(sf=0xb ua=1 ba=1 planet=31) caller=0x00487f41 sess=3 gclk=12000 | sf0=0x7 ua0=1 ba0=1  sf1=0x7 ua1=1 ba1=1"
DESYNC = "[00:00:12.000] ; [desync] *** DESYNC step=300 peer=0 mine=3703D9A3FCA342DD theirs=A5D3D43D3AC7DB29 first_region=20 units (mismatch #1)"
END = "[00:00:21.000] ; [session] SESSION_END match_id=01a0bf84220771c9bed4486da952c570 reason=quit final_clock_ms=12000"
NETIND = "[00:00:%02d.000] ; [netind] peer0 name=%s srtt_ms=1 ipdv_ms=0 loss_pm=0 bar=4 cmd_ms=80 look_ms=60 step_ms=20"


ORD_FA = ";ord   P0 own=0001 unit=0 code=FA p0=250 exec=6400ms"
ORD_OK = ";ord   P0 own=0080 unit=3 code=10 p0=0 exec=6400ms"


def plant(root, role, proc_lines, sess_lines, with_session=True, harness=(ORD_OK,)):
    lane = os.path.join(root, role)
    logs = os.path.join(lane, "logs")
    proc_leaf = "20260921T050000Z_menu_" + role
    proc = os.path.join(logs, proc_leaf)
    os.makedirs(proc)
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(proc_lines) + "\n")
    if harness is not None:
        with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(harness) + "\n")
    if with_session:
        sess = os.path.join(
            logs, "20260921T050010Z_01a0bf84_%d_%s" % (0 if role == "host" else 1, role)
        )
        os.makedirs(sess)
        with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
            json.dump({"match_id": "01a0bf84", "process_dir": proc_leaf}, fh)
        with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(sess_lines) + "\n")
    return proc


def selftest():
    cases = [
        (
            "green: refused on the client, quit eliminations forced only",
            [BANNER],
            [ELIM1_FORCED, END],
            [BANNER],
            [SUBMIT, REFUSED, END],
            0,
        ),
        (
            "RED: refused line present but 0xfa staged on the host anyway",
            [BANNER],
            [END],
            [BANNER],
            [SUBMIT, REFUSED, END],
            1,
            (ORD_FA,),
            (ORD_OK,),
        ),
        (
            "RED: gate off -- RUNS + 0xfa on both peers, nobody died",
            [BANNER],
            [END],
            [BANNER],
            [SUBMIT, RUNS, END],
            1,
            (ORD_OK, ORD_FA),
            (ORD_OK, ORD_FA),
        ),
        (
            "REFUSED: no mh_harness.log on the client",
            [BANNER],
            [END],
            [BANNER],
            [SUBMIT, REFUSED, END],
            2,
            (ORD_OK,),
            None,
        ),
        (
            "green: netind stable across the run",
            [BANNER],
            [NETIND % (1, "client"), NETIND % (3, "client"), END],
            [BANNER],
            [SUBMIT, REFUSED, END],
            0,
        ),
        (
            "RED: gate off -- the cheat RAN, host eliminated on both peers",
            [BANNER],
            [ELIM0, END],
            [BANNER],
            [SUBMIT, RUNS, ELIM0, END],
            1,
        ),
        (
            "RED: refused line missing (no CH1 line at all)",
            [BANNER],
            [END],
            [BANNER],
            [SUBMIT, END],
            1,
        ),
        (
            "RED: refused, but a desync sample on the host",
            [BANNER],
            [DESYNC, END],
            [BANNER],
            [SUBMIT, REFUSED, END],
            1,
        ),
        (
            "RED: refused, but the netind name morphed",
            [BANNER],
            [NETIND % (1, "Fef"), NETIND % (3, "Rizzen"), NETIND % (5, "Computer"), END],
            [BANNER],
            [SUBMIT, REFUSED, END],
            1,
        ),
        (
            "RED: a sim-path elimination before the end on the typing peer",
            [BANNER],
            [END],
            [BANNER],
            [SUBMIT, REFUSED, ELIM0, END],
            1,
        ),
        ("REFUSED: nobody submitted the line", [BANNER], [END], [BANNER], [END], 2),
        (
            "REFUSED: no session directory for the process",
            [BANNER],
            None,
            [BANNER],
            [SUBMIT, REFUSED, END],
            2,
        ),
    ]
    bad = 0
    for case in cases:
        title, hp, hs, cp, cs, want = case[:6]
        hh = case[6] if len(case) > 6 else (ORD_OK,)
        ch = case[7] if len(case) > 7 else (ORD_OK,)
        root = tempfile.mkdtemp(prefix="ch1_selftest_")
        try:
            h = plant(root, "host", hp, hs or [], with_session=hs is not None, harness=hh)
            c = plant(root, "client", cp, cs, with_session=True, harness=ch)
            try:
                got = check([h, c])
            except Refusal as e:
                print("  [REFUSED] %s" % e)
                got = 2
        finally:
            shutil.rmtree(root, ignore_errors=True)
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %-66s want=%s got=%s" % ("ok" if ok else "BAD", title, want, got))
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
