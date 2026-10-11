#!/usr/bin/env python3
"""
tools/check_quiet_session.py -- mp:Q1: does a 2-peer match at `[log] level=quiet` still reach
mh::desync::session_reset()?

THE TRAP THIS GUARDS. Until mp:Q1 the session_begin_multi hook (net_seams.cpp) was installed only
`if (g_ls_log)`, and `[log] level=quiet` sets lockstep_log=0. The hook is not a logger: its observer
carries the manual-menu host player-count fix-up and the desync session reset, so at quiet no MP match
ever started the per-step detector, the order digest, the state recorder or the ring. Nothing said
so -- the sampler logs its "armed but sampling nothing" line only from a session the reset started.

THE EVIDENCE IS NOT THE OBSERVER'S OWN LOG LINE (that line is the part that IS meant to go quiet):
  * the session folder holds mh_match_state.bin (or its .gz) and it DECODES with >= 1 step
    (tools/state_record.py) -- the recorder arms a file only from session_reset;
  * the session mh_net.log holds >= 1 `[desync] step=N peer=P MATCH` line -- a judged sample, which a
    peer only takes while it is in lockstep mode 3 (the host's mode is what the fix-up guards);
  * and the run really was quiet: the process log says `level=quiet effective` and the time_tick
    arm line reads `lockstep_log=0`, and no `session_begin_multi ENTER` line exists.

  --expect fixed    every peer shows all of the above
  --expect broken   every peer is quiet AND shows none of it (the fix-off arm, sbm_hook_log_gated=1)

  python tools/check_quiet_session.py --expect fixed|broken <peer-session-dir> [<peer-session-dir> ...]
  python tools/check_quiet_session.py --selftest
"""

import argparse
import glob
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MATCH_RE = re.compile(r"; \[desync\] step=\d+ peer=\d+ MATCH ")
RESET_RE = re.compile(r"; \[desync\] session reset -- sampling from step 1")
ENTER_RE = re.compile(r"; session_begin_multi ENTER ")
QUIET_RE = re.compile(r"; \[log\] level=quiet effective")
TT_RE = re.compile(r"; time_tick hook armed: .*\blockstep_log=(\d+)")


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def process_dir(run_dir):
    """The session folder's process ("menu") folder, via session.json's process_dir; the folder itself
    when it has none (a run without a session)."""
    sj = os.path.join(run_dir, "session.json")
    try:
        pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
    except (OSError, ValueError):
        pd = None
    if pd:
        cand = os.path.join(os.path.dirname(os.path.abspath(run_dir).rstrip("\\/")), pd)
        if os.path.isdir(cand):
            return cand
    return run_dir


def state_steps(run_dir):
    """(path, steps decoded) for the session's first state file, or (None, 0)."""
    hits = sorted(glob.glob(os.path.join(run_dir, "mh_match_state*.bin")))
    hits += sorted(glob.glob(os.path.join(run_dir, "mh_match_state*.bin.gz")))
    if not hits:
        return None, 0
    import state_record

    try:
        rec = state_record.load(hits[0])
    except (ValueError, OSError):
        return hits[0], 0
    if rec.first_step is None or rec.last_step is None:
        return hits[0], 0
    return hits[0], rec.last_step - rec.first_step + 1


def peer_verdict(run_dir, expect):
    """(ok, [lines]) for one peer."""
    lines = []
    sess = _read(os.path.join(run_dir, "mh_net.log"))
    proc = _read(os.path.join(process_dir(run_dir), "mh_net.log"))
    both = proc + "\n" + sess
    quiet = bool(QUIET_RE.search(both))
    tt = TT_RE.search(both)
    ls0 = tt is not None and tt.group(1) == "0"
    enter = bool(ENTER_RE.search(both))
    if not (quiet and ls0 and not enter):
        lines.append(
            "      FAIL: %s is not a quiet run (level=quiet line %s, time_tick lockstep_log=%s, "
            "session_begin_multi ENTER line %s) -- the row proves nothing about quiet"
            % (
                run_dir,
                "present" if quiet else "ABSENT",
                tt.group(1) if tt else "?",
                "PRESENT" if enter else "absent",
            )
        )
        return False, lines
    path, steps = state_steps(run_dir)
    matches = len(MATCH_RE.findall(sess))
    reset = bool(RESET_RE.search(sess))
    lines.append(
        "      %s: quiet; session reset line %s, %d MATCH sample(s), state file %s (%d step(s) decoded)"
        % (
            os.path.basename(run_dir.rstrip("\\/")),
            "present" if reset else "absent",
            matches,
            os.path.basename(path) if path else "ABSENT",
            steps,
        )
    )
    if expect == "fixed":
        ok = reset and matches >= 1 and steps >= 1
        if not ok:
            lines.append(
                "      FAIL: expected the session reset, >= 1 MATCH sample and a decodable state file"
            )
    else:
        ok = (not reset) and matches == 0 and path is None
        if not ok:
            lines.append(
                "      FAIL: the fix-off arm reached session_reset anyway -- the twin no longer reproduces"
            )
    return ok, lines


def check(dirs, expect):
    ok_all, out = True, []
    if not dirs:
        return False, ["  FAIL: no peer directories given"]
    for d in dirs:
        ok, lines = peer_verdict(d, expect)
        ok_all = ok_all and ok
        out.extend(lines)
    out.insert(0, "  check_quiet_session --expect %s: %s" % (expect, "PASS" if ok_all else "FAIL"))
    return ok_all, out


# ---- selftest -----------------------------------------------------------------------------------


def selftest():
    fails = 0
    tmp = tempfile.mkdtemp(prefix="chk_quiet_")
    orig = state_steps

    def fake_steps(d):
        p = os.path.join(d, "mh_match_state.bin")
        return (p, 10) if os.path.exists(p) else (None, 0)

    globals()["state_steps"] = fake_steps
    try:

        def mk(name, state=True, **kw):
            sess = _plant_noimport(tmp, name, **kw)
            if state:
                open(os.path.join(sess, "mh_match_state.bin"), "wb").write(b"x")
            return sess

        cases = [
            ("fixed peer passes fixed", "fixed", [mk("a")], True),
            (
                "broken peer passes broken",
                "broken",
                [mk("b", state=False, reset=False, matches=0)],
                True,
            ),
            ("fixed peer fails broken", "broken", [mk("c")], False),
            (
                "broken peer fails fixed",
                "fixed",
                [mk("d", state=False, reset=False, matches=0)],
                False,
            ),
            ("no state file fails fixed", "fixed", [mk("e", state=False)], False),
            ("no MATCH sample fails fixed", "fixed", [mk("f", matches=0)], False),
            ("not quiet is refused", "fixed", [mk("g", quiet=False)], False),
            ("lockstep_log=1 is refused", "fixed", [mk("h", ls=1)], False),
            (
                "an ENTER line is refused",
                "broken",
                [mk("i", enter=True, state=False, reset=False, matches=0)],
                False,
            ),
            ("one bad peer of two fails", "fixed", [mk("j"), mk("k", matches=0)], False),
            ("no dirs fails", "fixed", [], False),
        ]
        for name, expect, dirs, want in cases:
            ok, _ = check(dirs, expect)
            good = ok == want
            fails += 0 if good else 1
            print("  %s %s" % ("ok  " if good else "FAIL", name))
    finally:
        globals()["state_steps"] = orig
        shutil.rmtree(tmp, ignore_errors=True)
    print("check_quiet_session selftest: %s" % ("PASS" if fails == 0 else "%d FAIL" % fails))
    return 0 if fails == 0 else 1


def _plant_noimport(root, name, quiet=True, ls=0, enter=False, reset=True, matches=2):
    proc = os.path.join(root, name + "_menu")
    sess = os.path.join(root, name + "_sess")
    os.makedirs(proc)
    os.makedirs(sess)
    with open(os.path.join(sess, "session.json"), "w") as fh:
        json.dump({"process_dir": os.path.basename(proc)}, fh)
    p = []
    if quiet:
        p.append("; [log] level=quiet effective: net.lockstep_log=0 net.frametime_log=0")
    p.append("; time_tick hook armed: lockstep_step=30 ms, lockstep_log=%d, sp_clock_log=0" % ls)
    with open(os.path.join(proc, "mh_net.log"), "w") as fh:
        fh.write("\n".join(p) + "\n")
    s = []
    if enter:
        s.append("; session_begin_multi ENTER p54bc=2 pcount=0 sess(before)=2 gclk=0")
    if reset:
        s.append("; [desync] session reset -- sampling from step 1")
    s += ["; [desync] step=%d peer=1 MATCH state=00" % (50 * (i + 1)) for i in range(matches)]
    with open(os.path.join(sess, "mh_net.log"), "w") as fh:
        fh.write("\n".join(s) + "\n")
    return sess


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--expect", choices=("fixed", "broken"), default="fixed")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    ok, lines = check(a.dirs, a.expect)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
