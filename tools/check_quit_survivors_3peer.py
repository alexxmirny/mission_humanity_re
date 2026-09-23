#!/usr/bin/env python3
"""check_quit_survivors_3peer.py -- mp:U19b: a 3-PEER clean quit, judged from all three peers' logs.

WHAT WAS UNTESTED. `graceful_quit` (U19) is 2-peer, and an AI seat does not count toward the quorum
`llm_net_player_remove` tests (measured 2026-09-18: with `occ 3` and an AI visibly seated, the
client's removal still ended the host's match). So U19's own third clause -- survivors that keep
PLAYING past a departure, rather than the match ending -- had no exerciser. The `u19b_quit3`
determinism shape (`tools/test_ui.py`'s `run_u19b_quit3`, the `--det-3peer` VM+VM+local-lane
topology) drives that: a host, one survivor client on the second rig VM, and a quitter on a local
lane. This is that shape's post_check, over the run directories `run_u19b_quit3` pulls back
(`tmp/ui_test/determinism/{host,client1,client2}`, det3_barrier_report's own layout) -- `host` and
`client1` are the two SURVIVORS, `client2` is the QUITTER.

THE TWO KINDS OF EVIDENCE:

  1. LOG NEEDLES (the same clauses `check_graceful_quit.py` reads for the 2-peer case, adapted to
     "did the drop stay a below-quorum roster removal instead of ending the match"): the quitter's
     log carries the U17 self-removal BROADCAST, and NEITHER survivor's log carries an
     `on_gameover ENTER` at all -- the whole point of a 3-peer drop is that nobody's match ends -- nor
     a `; U17 fast-drop` (the B2 socket-dead catch; if this fired instead of the ordered lockstep
     dispatch, the run proved graceful_drop, not graceful_leave, same as U19's clause 3).

  2. mp_analyze.py OVER BOTH SURVIVORS' logs must read the exact verdict string "ALL PAIRS IDENTICAL
     (determinism-clean)". mp_analyze.py's own exit code is NOT a usable signal here (it returns 0 on
     a DESYNC too -- confirmed by reading analyse(): the only non-zero return is the cross-match-id
     refusal), so this tool runs it as a subprocess and reads its PRINTED verdict line and its
     per-pair "combined-hash=N" rollup instead. `--min-common` (default 100) is a floor on that N: a
     pair that compared too few overlapping steps could read ALL PAIRS IDENTICAL having barely
     compared the pre-drop portion, which would not be evidence the survivors stayed identical PAST
     the drop. There is no separate "steps before the drop" vs "steps after the drop" comparison to
     make here: the departure is itself an ordered lockstep event both survivors process identically,
     so one continuous ALL-PAIRS-IDENTICAL verdict over a window that clears --min-common already
     covers both sides of it.

ABSENCE IS A FAILURE (check_module_bind.py's rule, kept here): a directory that cannot be read, or
carries no evidence at all, is a REFUSAL, never a vacuous pass.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_quit_survivors_3peer.py <host-dir> <client1-dir> <client2-quitter-dir> \\
      [--min-common 100]
  python tools/check_quit_survivors_3peer.py --selftest        planted logs; every negative RED
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.abspath(__file__))

# ---- the needles -------------------------------------------------------------------------------
# Registered in tools/data/log_formats.json (net.graceful_leave_broadcast, net.fast_drop,
# net.gameover_enter). Keep these literals textually identical to the registry: lint_log_formats arm
# A compares them.
BROADCAST = "; U17 graceful-leave: broadcast self-removal side="
FAST_DROP = "; U17 fast-drop: transport-dead peer"
# mp:U19b -- the mechanism's own line (net.graceful_leave_park): the quitter froze its horizon and
# waited for the survivors to PARK on it before stamping the removal record. Only PARKED is the
# proven shape; VALVE-UNPARKED / UNWAITED / SESSION-ENDED / NO-SURVIVORS each name a way the wait did
# not do its job, and a green determinism verdict on top of one of those is luck, not the mechanism.
PARK = "; U19b graceful-leave: survivors "
PARK_RE = re.compile(
    r"; U19b graceful-leave: survivors (\S+) after (\d+) ms -- H_d=(\d+) ms P=(\d+) ms"
)
GAMEOVER_ENTER = "on_gameover ENTER"
# tooling:TL-QUIT3-FLAKE -- the discriminator between "the quitter quit and a seam did not fire"
# and "the quitter never quit at all". The second is a HARNESS fault -- the script's ESC does not
# open the in-game menu, so the quitter simply plays on until the runner tears it down -- and it
# presents with the SAME two missing needles as a genuine seam regression, so without this the red
# blames the DLL for something the DLL never did. A quitter
# that really left always writes `; [session] SESSION_END ... reason=quit`; a flaked one writes
# SESSION_BEGIN and nothing else, because it is still playing when the harness tears it down.
# Registered as net.session_end in tools/data/log_formats.json.
SESSION_END_QUIT = "SESSION_END"
QUIT_REASON = "reason=quit"

MP_ANALYZE = os.path.join(REPO, "mp_analyze.py")
VERDICT_OK = "ALL PAIRS IDENTICAL (determinism-clean)"
PAIR_ROLLUP_RE = re.compile(r"\[.*? vs .*?\]\s+state-mismatch steps=(\d+)\s+combined-hash=(\d+)")

DEFAULT_MIN_COMMON = 100


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


def read_net_log(run_dir):
    fp = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(fp):
        raise Refusal("%s: no mh_net.log" % run_dir)
    with open(fp, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def check_needles(host_dir, client1_dir, client2_dir):
    """(ok, [lines]) -- clause 1. Raises Refusal when a directory is unreadable."""
    quitter = read_net_log(client2_dir)
    survivor_a = read_net_log(host_dir)
    survivor_b = read_net_log(client1_dir)

    out, bad = [], []

    # Did the quitter actually leave? Everything below that is missing means something different
    # depending on the answer, so establish it FIRST rather than guessing in each message.
    quitter_left = any(SESSION_END_QUIT in ln and QUIT_REASON in ln for ln in quitter.splitlines())
    if not quitter_left:
        out.append(
            "NOTE: the quitter never logged a `%s ... %s` -- it did not leave the match at all, so "
            "every missing needle below is a consequence of that, NOT of a seam failing to arm "
            "(tooling:TL-QUIT3-FLAKE: the script's ESC does not open the in-game menu ~1 run in 6)"
            % (SESSION_END_QUIT, QUIT_REASON)
        )

    if BROADCAST not in quitter:
        bad.append(
            "the quitter (%s) never logged %r -- %s"
            % (
                client2_dir,
                BROADCAST,
                "IT NEVER QUIT (no SESSION_END reason=quit): this is the harness flake "
                "tooling:TL-QUIT3-FLAKE, not a DLL regression -- re-run rather than bisect"
                if not quitter_left
                else "the knob was off, or the quit-to-menu detour did not arm",
            )
        )
    else:
        line = next(ln for ln in quitter.splitlines() if BROADCAST in ln)
        out.append("quitter broadcast: %s" % line.strip())

    # U19b: the freeze + park wait ran, and it PARKED. Without this the record still carries a
    # horizon the survivors never advertised-to (the 10 s sentinel) and every survivor's match ends;
    # with the stamp but no park the flip lands at different sim clocks (D5).
    pm = PARK_RE.search(quitter)
    if not pm:
        bad.append(
            "the quitter (%s) never logged %r -- %s"
            % (
                client2_dir,
                PARK,
                "IT NEVER QUIT (no SESSION_END reason=quit): tooling:TL-QUIT3-FLAKE, a harness "
                "fault -- graceful_leave_park is never reached because the quit never happened"
                if not quitter_left
                else "graceful_leave_park did not run (knob off, or an older mh.dll), so the "
                "removal record carried the unwritten local PEER_HORIZON slot",
            )
        )
    else:
        out.append("quitter park: %s" % pm.group(0))
        if pm.group(1) != "PARKED":
            bad.append(
                "the quitter's park wait ended %s after %s ms -- the survivors were not proven parked "
                "on H_d=%s ms before the removal went out" % (pm.group(1), pm.group(2), pm.group(3))
            )

    for name, dir_, text in (("host", host_dir, survivor_a), ("client1", client1_dir, survivor_b)):
        if GAMEOVER_ENTER in text:
            bad.append(
                "survivor %s (%s) logged %r -- its match ended, which is exactly what the "
                "3-peer clause says must NOT happen (an AI-seat run already showed a 2-peer-shaped "
                "removal ends the match even at occ 3)" % (name, dir_, GAMEOVER_ENTER)
            )
        else:
            out.append("survivor %s: no on_gameover -- match never ended" % name)
        if FAST_DROP in text:
            bad.append(
                "survivor %s (%s) dropped the quitter through the B2 socket-close catch -- that "
                "is graceful_drop, not graceful_leave (U19's own clause 3, restated for 3 peers)"
                % (name, dir_)
            )
        else:
            out.append(
                "survivor %s: no fast-drop line -- learned it from the lockstep dispatch" % name
            )

    return (not bad), out, bad


def check_determinism(host_dir, client1_dir, min_common):
    """(ok, [lines]) -- clause 2: mp_analyze.py's PRINTED verdict over the two survivors, since its
    own exit code is not a usable pass/fail signal (see module docstring)."""
    if not os.path.isfile(MP_ANALYZE):
        raise Refusal("cannot find %s" % MP_ANALYZE)
    r = subprocess.run(
        [sys.executable, MP_ANALYZE, host_dir, client1_dir],
        capture_output=True,
        text=True,
    )
    text = (r.stdout or "") + (r.stderr or "")
    out, bad = [], []

    m = PAIR_ROLLUP_RE.search(text)
    if not m:
        raise Refusal(
            "mp_analyze produced no pairwise rollup line for [%s vs %s] -- cannot read a verdict "
            "at all (output: %r)" % (host_dir, client1_dir, text[-500:])
        )
    mismatch_steps, combined_hash = int(m.group(1)), int(m.group(2))
    out.append(
        "mp_analyze pairwise: state-mismatch steps=%d combined-hash=%d"
        % (mismatch_steps, combined_hash)
    )

    if VERDICT_OK not in text:
        bad.append(
            "mp_analyze did not print %r -- the two survivors were not determinism-clean over the "
            "compared window" % VERDICT_OK
        )
    if combined_hash < min_common:
        bad.append(
            "only %d overlapping hashed step(s) compared, below --min-common %d -- too little of "
            "the run (and possibly none of the post-drop portion) was actually measured"
            % (combined_hash, min_common)
        )

    return (not bad), out, bad


def check(host_dir, client1_dir, client2_dir, min_common=DEFAULT_MIN_COMMON):
    out = []
    ok1, lines1, bad1 = check_needles(host_dir, client1_dir, client2_dir)
    out.extend(lines1)
    out.extend("FAIL: " + b for b in bad1)
    ok2, lines2, bad2 = check_determinism(host_dir, client1_dir, min_common)
    out.extend(lines2)
    out.extend("FAIL: " + b for b in bad2)
    return (ok1 and ok2), out


# ---- selftest ----------------------------------------------------------------------------------
# The determinism half is faked by monkeypatching subprocess.run (mp_analyze needs a real rig run's
# harness logs to say anything at all; this tool's own logic -- reading its printed verdict and
# rollup line rather than trusting its exit code -- is what is under test here, not mp_analyze
# itself, which has its own --selftest).
# tooling:TL-QUIT3-FLAKE: a quitter that really quit ALWAYS writes this first (mp_session_close runs
# at the head of on_quit_to_menu, before the graceful-leave half). Its presence is what separates
# "the quit happened and a seam did not fire" -- a DLL regression -- from "the quit never happened",
# which is the harness flake. Both fixtures below carry it; QUIT_LINE is omitted only by the arm
# that exists to test the flake wording.
QUIT_LINE = (
    "[03:00:00.010] ; [session] SESSION_END match_id=deadbeef reason=quit final_clock_ms=3290 "
    "stall=0 icon_calls=1 icon_shown=1 ended=20260101T000000Z"
)
GREEN_QUITTER = [
    QUIT_LINE,
    "[03:00:00.050] "
    + PARK
    + "PARKED after 212 ms -- H_d=3320 ms P=3320 ms lookahead=30 ms survivors=2",
    "[03:00:00.100] " + BROADCAST + "2 before quit-to-menu",
]
GREEN_SURVIVOR = ["[03:00:00.200] ; GameRecv sender=2 len=14 type=0x04"]


def _plant(root, name, lines):
    d = os.path.join(root, name)
    os.makedirs(d)
    open(os.path.join(d, "mh_net.log"), "w").write("\n".join(lines) + "\n")
    return d


class _FakeProc:
    def __init__(self, stdout):
        self.stdout = stdout
        self.stderr = ""


def selftest():
    arms = []

    def arm(name, quitter, survivor_a, survivor_b, analyze_stdout, want_ok, want_text=None):
        # want_text: a substring the REPORT must contain. An arm that only checks ok/not-ok cannot
        # tell a red that names the right cause from one that names the wrong one.
        arms.append((name, quitter, survivor_a, survivor_b, analyze_stdout, want_ok, want_text))

    good_analyze = (
        "   [host vs client1]  state-mismatch steps=0  combined-hash=400  OK\n"
        "   VERDICT: " + VERDICT_OK + "\n"
    )
    arm(
        "green: broadcast seen, neither survivor ended, determinism-clean over 400 steps",
        GREEN_QUITTER,
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        good_analyze,
        True,
    )
    arm(
        "red: quitter never broadcast (knob off / detour unarmed)",
        [QUIT_LINE],
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        good_analyze,
        False,
        "the knob was off",
    )
    arm(
        "red: the quitter never logged the U19b park line (older DLL / knob off)",
        [QUIT_LINE, GREEN_QUITTER[2]],
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        good_analyze,
        False,
        "graceful_leave_park did not run",
    )
    # tooling:TL-QUIT3-FLAKE. The SAME two needles are missing as in the two arms above, and the
    # cause is entirely different -- this run says nothing about the DLL at all. The arm asserts the
    # WORDING, not just the red, because a red that names the wrong cause is what this row exists to
    # stop: the flake's message used to read "the knob was off, or the quit-to-menu detour did not
    # arm", and it cost a build cycle in the mp:U19h hunt before the quitter's own log settled it.
    arm(
        "red: the quitter never quit at all -- named as the harness flake, not as a DLL regression",
        [],
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        good_analyze,
        False,
        # Assert the FAIL line's own wording, not the NOTE's: the NOTE names the flake
        # unconditionally, so matching it would pass even with the discrimination removed -- which a
        # mutation run proved, before this string was narrowed.
        "IT NEVER QUIT",
    )
    arm(
        "red: the park wait hit its safety valve (survivors never proven parked)",
        [
            "[03:00:01.600] " + PARK + "VALVE-UNPARKED after 1502 ms -- H_d=3320 ms P=3320 ms "
            "lookahead=30 ms survivors=2",
            GREEN_QUITTER[1],
        ],
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        good_analyze,
        False,
    )
    arm(
        "red: a survivor's match ended (the 3-peer clause's own failure mode)",
        GREEN_QUITTER,
        GREEN_SURVIVOR + ["[03:00:00.300] ; " + GAMEOVER_ENTER + " sess=2 outcome=8 gclk=6030"],
        GREEN_SURVIVOR,
        good_analyze,
        False,
    )
    arm(
        "red: a survivor dropped it via the B2 fast-drop catch instead of the lockstep dispatch",
        GREEN_QUITTER,
        GREEN_SURVIVOR + ["[03:00:00.250] " + FAST_DROP + " side=2 -> broadcast removal"],
        GREEN_SURVIVOR,
        good_analyze,
        False,
    )
    arm(
        "red: mp_analyze did not read ALL PAIRS IDENTICAL",
        GREEN_QUITTER,
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        "   [host vs client1]  state-mismatch steps=5  combined-hash=400  DESYNC\n"
        "   VERDICT: DESYNC in >=1 pair -- NOT clean\n",
        False,
    )
    arm(
        "red: too few overlapping steps compared (a vacuous-looking green)",
        GREEN_QUITTER,
        GREEN_SURVIVOR,
        GREEN_SURVIVOR,
        "   [host vs client1]  state-mismatch steps=0  combined-hash=3  OK\n"
        "   VERDICT: " + VERDICT_OK + "\n",
        False,
    )

    failures = 0
    for name, quitter, survivor_a, survivor_b, analyze_stdout, want_ok, want_text in arms:
        root = tempfile.mkdtemp(prefix="quit3_selftest_")
        try:
            host_dir = _plant(root, "host", survivor_a)
            client1_dir = _plant(root, "client1", survivor_b)
            client2_dir = _plant(root, "client2", quitter)
            orig_run = subprocess.run
            subprocess.run = lambda *a, **k: _FakeProc(analyze_stdout)  # noqa: E731
            try:
                try:
                    ok, _report = check(host_dir, client1_dir, client2_dir)
                except Refusal as exc:
                    ok = False
                    _report = ["REFUSAL: %s" % exc]
            finally:
                subprocess.run = orig_run
            text_ok = want_text is None or any(want_text in ln for ln in _report)
            good = ok == want_ok and text_ok
            verdict = "PASS" if good else "SELFTEST FAILURE"
            if not good:
                failures += 1
            extra = ""
            if not text_ok:
                extra = " -- report never said %r: %s" % (want_text, " | ".join(_report))
            print("  [%s] %s (got ok=%s, wanted ok=%s)%s" % (verdict, name, ok, want_ok, extra))
        finally:
            shutil.rmtree(root, ignore_errors=True)

    print(
        "check_quit_survivors_3peer --selftest: %s -- %d arm(s)"
        % ("FAIL" if failures else "PASS", len(arms))
    )
    return 1 if failures else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("host_dir", nargs="?")
    ap.add_argument("client1_dir", nargs="?")
    ap.add_argument("client2_dir", nargs="?")
    ap.add_argument("--min-common", type=int, default=DEFAULT_MIN_COMMON)
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="planted logs + a faked mp_analyze; every negative RED",
    )
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not (args.host_dir and args.client1_dir and args.client2_dir):
        ap.error("give host_dir, client1_dir (survivors) and client2_dir (quitter), or --selftest")
    try:
        ok, report = check(args.host_dir, args.client1_dir, args.client2_dir, args.min_common)
    except Refusal as exc:
        print("check_quit_survivors_3peer: REFUSED -- %s" % exc)
        return 2
    for line in report:
        print("  " + line)
    print("check_quit_survivors_3peer: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
