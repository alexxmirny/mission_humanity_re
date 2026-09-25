#!/usr/bin/env python3
"""check_gone_peer_guard.py -- mp:U19j: did the gone-peer frame guard's byte-patch carrier FIRE?

THE BRANCH. mp:U19i's carrier call-splices the `call llm_net_send_lockstep_kick` at 0x0049c330 in
the ORIGINAL llm_net_lockstep_dispatch (EN /eng/mh.exe) -- the leader's re-broadcast of a drop for
a sender it has already written off. That call builds its 6-byte CTL_KICK record in
_G_LLM_NET_SEND_BUF, which is also the buffer the datagram being dispatched sits in, so without the
guard the kick overwrites the head of that datagram before it is parsed (dead-ends G238). The call
runs only when, on ONE peer, all three hold:
  (a) a datagram arrives from a sender whose roster slot is flagged AI (0x0049c311),
  (b) llm_net_lockstep_is_local_leader_peer(sender) -- the first ALIVE|HUMAN slot other than the
      sender is this peer's own (the host, slot 0, in the rig's lobby-launched matches),
  (c) the dispatch loop is still running: it is only pumped in session mode 3 and returns once the
      mode leaves 3.
A 2-peer removal never satisfies (c) (dead-ends G300). The `--u19j-gpfg3` shape (tools/test_ui.py)
gets there with THREE peers: one is sim-fenced with its transport up (mp:GS2's shape), the other two
drop it after `[net] data_timeout_ms` and keep playing, and its horizon-heartbeat thread keeps
sending 9-byte type-0x02 adverts to a leader that has already written it off.

WHAT THIS READS. Three peers' pulled mh_net.log files (ui_test --pull-logs: the process directory
plus each session directory, in stamp order): the LEADER (the host), the other SURVIVOR, and the
FENCED peer. Every clause is about the leader, except configuration (1), which all three must be.

  1. CONFIGURATION (1) on every peer: `libmh: NOT BOUND` -- the original dispatch ran, so the carrier
     (and not libmh's own guard in the promoted body) is what stood at 0x0049c330.
  2. THE ARM IS THE ONE NAMED: arm on -> the leader's boot line reads `gone-peer frame guard:
     PATCHED`; arm off -> it does not (the knob really was 0). A mismatch is a REFUSAL.
  3. A DROP happened: a survivor logged `; GS2: peer N data-silent ... -> dropped`. N is the fenced
     peer's side id; every survivor that logged one must name the same N.
  4. THE BRANCH'S INPUT EXISTED: a type-0x02 frame from side N reached the leader AFTER the drop
     (`; GameRecv sender=N type=0x02 count=` rollup, timestamped after the leader's drop). Without it
     the run proves nothing either way -- a FAIL, named "not reached", never a vacuous pass.
  5. THE CARRIER FIRED (arm on): `gone-peer frame guard FIRED #n: kick re-broadcast for side N`.
  6. THE FRAME SURVIVED (arm on): no survivor logged `on_gameover ENTER` at all (both kept playing),
     no `; [rx] garbled:` (libmh's line; configuration (1) cannot print it, checked anyway so a
     promoted run cannot pass as one), no U19d correction.

THE NEGATIVE ARM (`--arm off`, `[net] gone_peer_frame_guard=0`). The same run with the carrier off
must go RED naming the garbled frame. Configuration (1) has no `; [rx] garbled:` line -- that is
libmh's handle_garbled -- so the frame is named from what the original binary leaves behind: the
leader's `on_gameover ENTER sess=3 outcome=7` (the dispatch's own unknown-outer-tag arm: the kick's
record parses harmlessly, then offset 6 is a mantissa byte of the advert read as an outer tag, and
the default arm raises NETWORK_ERROR while still inside session 3), preceded by side N's type-0x02
frames. `--expect-red` makes that the verdict: XFAIL (exit 0) when the arm went red on exactly that
garbled frame, XPASS (exit 1) when it came back clean -- an unguarded arm that does not garble means
the run cannot tell the two binaries apart (G283/G285).

ABSENCE IS A REFUSAL (exit 2): a missing log is never a pass.

  python tools/check_gone_peer_guard.py LEADER_DIR SURVIVOR_DIR FENCED_DIR [--arm on]
  python tools/check_gone_peer_guard.py LEADER_DIR SURVIVOR_DIR FENCED_DIR --arm off --expect-red
  python tools/check_gone_peer_guard.py --selftest
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

# ---- the needles (tools/data/log_formats.json: net.modules_outcome, net.gone_peer_frame_guard_install,
# net.gs2_drop, net.gamerecv_horizon_rollup, net.gone_peer_frame_guard_fired, net.gameover_enter,
# net.rx_garbled, net.u19d_outcome_correction). Keep them textually identical: lint_log_formats arm A.
CONFIG1 = "libmh: NOT BOUND"
PATCHED = "gone-peer frame guard: PATCHED"
GS2_RE = re.compile(r"GS2: peer (\d+) data-silent for (\d+) ms > (\d+) -> dropped")
GAMERECV = "; GameRecv sender="
ROLLUP_RE = re.compile(re.escape(GAMERECV) + r"(-?\d+) type=0x02 count=(\d+)")
FIRED = "gone-peer frame guard FIRED"
FIRED_RE = re.compile(re.escape(FIRED) + r" #(\d+): kick re-broadcast for side (-?\d+)")
GAMEOVER_ENTER = "on_gameover ENTER"
GAMEOVER_RE = re.compile(re.escape(GAMEOVER_ENTER) + r" sess=(\d+) outcome=(\d+)")
GARBLED = "; [rx] garbled:"
U19D_CORRECTION = "U19d: outcome-dialog said"
NETWORK_ERROR_OUTCOME = 7
SESSION_MP_LOCKSTEP = 3

TS_RE = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")


class Refusal(Exception):
    """A run this tool cannot make a statement about. NEVER a pass."""


def ts_ms(line):
    m = TS_RE.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return ((h * 60 + mi) * 60 + s) * 1000 + ms


def after(t, t0):
    """t is at or after t0, tolerating one midnight wrap."""
    if t is None or t0 is None:
        return False
    d = t - t0
    if d < -12 * 3600 * 1000:
        d += 24 * 3600 * 1000
    return d >= 0


def read_log(peer_dir, role):
    path = os.path.join(peer_dir, "mh_net.log")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError as exc:
        raise Refusal("%s: cannot read %s (%s)" % (role, path, exc))


def first(lines, pred):
    for ln in lines:
        if pred(ln):
            return ln
    return None


def check(leader_dir, survivor_dir, fenced_dir, arm="on"):
    """Returns (ok, lines, garbled). `garbled` is the finding that names the garbled frame (the
    unguarded arm's expected red), or None."""
    logs = {
        "leader": read_log(leader_dir, "leader"),
        "survivor": read_log(survivor_dir, "survivor"),
        "fenced": read_log(fenced_dir, "fenced"),
    }
    out, bad, garbled = [], [], None

    # ---- clause 1: configuration (1) everywhere --------------------------------------------------
    not_c1 = [role for role, lines in logs.items() if not any(CONFIG1 in ln for ln in lines)]
    if not_c1:
        raise Refusal(
            "%s did not run CONFIGURATION (1) (no '%s' line) -- a promoted dispatch carries its own "
            "guard, so this run cannot speak for the byte-patch carrier"
            % (", ".join(not_c1), CONFIG1)
        )
    out.append("configuration (1) on all three peers (%s)" % CONFIG1)

    # ---- the survivor's window ends where the leader's process did -------------------------------
    # MEASURED (first rig run, 2026-09-24): ui_test tears the peers down in order, host FIRST, and
    # pulls each later peer's logs after that. The other survivor then sees the host go silent and,
    # data_timeout_ms later, drops IT (`GS2: peer 0 ...` + its own on_gameover outcome 8) -- an
    # artifact of the teardown, not of the match. So the survivor is judged only up to the leader's
    # last log line (the leader writes a [netind] line every 2 s, so that is its kill time to within
    # 2 s, and a teardown drop lands >= data_timeout_ms after it). The rig's machines share a clock
    # to the millisecond (the fenced lane's on_gameover and the host's drop line agree to 1 ms).
    t_end = None
    for ln in reversed(logs["leader"]):
        t_end = ts_ms(ln)
        if t_end is not None:
            break
    if t_end is not None:
        kept = [ln for ln in logs["survivor"] if ts_ms(ln) is None or after(t_end, ts_ms(ln))]
        cut = len(logs["survivor"]) - len(kept)
        if cut:
            out.append(
                "survivor: %d line(s) after the leader's last line ignored (the runner's teardown "
                "kills the host first)" % cut
            )
        logs["survivor"] = kept

    # ---- clause 2: the arm is the one named ------------------------------------------------------
    leader = logs["leader"]
    patched = first(leader, lambda ln: PATCHED in ln)
    if arm == "on" and patched is None:
        raise Refusal(
            "arm on, but the leader never logged '%s' -- the carrier was not armed" % PATCHED
        )
    if arm == "off" and patched is not None:
        raise Refusal(
            "arm off, but the leader logged '%s' -- gone_peer_frame_guard=0 did not reach it"
            % PATCHED
        )
    out.append("leader carrier: %s" % ("PATCHED" if patched else "absent (knob off)"))

    # ---- clause 3: the drop ----------------------------------------------------------------------
    drops = {}
    for role in ("leader", "survivor"):
        for ln in logs[role]:
            m = GS2_RE.search(ln)
            if m:
                drops.setdefault(role, (int(m.group(1)), ts_ms(ln), ln.strip()))
                break
    if not drops:
        bad.append(
            "no survivor logged a GS2 drop -- the fenced peer was never written off, so the "
            "gone-peer branch had nothing to act on"
        )
        for b in bad:
            out.append("FAIL: " + b)
        return False, out, None
    sides = {v[0] for v in drops.values()}
    if len(sides) != 1:
        bad.append("the survivors dropped DIFFERENT sides: %s" % sorted(sides))
    side = sorted(sides)[0]
    t_drop = drops["leader"][1] if "leader" in drops else None
    for role, (_s, _t, ln) in sorted(drops.items()):
        out.append("%s drop: %s" % (role, ln))
    if t_drop is None:
        # The survivor's removal frame reached the leader first, so the leader's own watchdog found
        # the slot already gone and stayed silent (slot_is_active_human). Its first sign is then the
        # survivor's removal record; the ordering below falls back to the leader's FIRST post-freeze
        # line from side N, which is weaker, and is reported as such.
        out.append(
            "note: the leader logged no GS2 line of its own (the survivor's removal came first)"
        )

    # ---- clause 4: frames from side N reached the leader after the drop --------------------------
    late = [
        ln
        for ln in leader
        for m in [ROLLUP_RE.search(ln)]
        if m and int(m.group(1)) == side and (t_drop is None or after(ts_ms(ln), t_drop))
    ]
    # The last rollup AT OR BEFORE the drop: side N was streaming adverts to the leader at that rate.
    # It is what names the garbled frame on the unguarded arm, where the garble ends the leader's
    # match within milliseconds of the drop -- before the next 1 Hz rollup can flush (measured
    # 2026-09-24: drop 36.278, outcome 7 at 36.284, and no later `GameRecv sender=2` line at all).
    streaming = None
    for ln in leader:
        m = ROLLUP_RE.search(ln)
        if m and int(m.group(1)) == side and t_drop is not None and after(t_drop, ts_ms(ln)):
            streaming = ln
    if late:
        out.append("frames from the gone side after the drop: %s" % late[0].strip())
    elif arm == "on":
        bad.append(
            "the gone-peer branch was NOT REACHED: no type-0x02 frame from side %d reached the "
            "leader after its drop, so a carried and an uncarried binary are indistinguishable "
            "here (dead-ends G300)" % side
        )
    else:
        out.append(
            "no rollup from the gone side after the drop (the unguarded leader stops receiving "
            "once its match ends); last one before it: %s" % (streaming or "none").strip()
        )

    # ---- clause 5: the carrier fired -------------------------------------------------------------
    fired = [(ln, int(m.group(2))) for ln in leader for m in [FIRED_RE.search(ln)] if m]
    if arm == "on":
        mine = [ln for ln, s in fired if s == side]
        if not mine:
            bad.append(
                "the leader never logged '%s ... for side %d' -- the kick re-broadcast @0x0049c330 "
                "was not reached through the carrier" % (FIRED, side)
            )
        else:
            out.append("carrier: %s (%d line(s))" % (mine[0].strip(), len(mine)))
    elif fired:
        bad.append("arm off, yet the leader logged %s" % fired[0][0].strip())

    # ---- clause 6: the frame survived -- or, unguarded, the garbled frame ------------------------
    for role in ("leader", "survivor"):
        lines = logs[role]
        g = first(lines, lambda ln: GARBLED in ln)
        if g:
            bad.append("%s hit the garbled-stream arm: %s" % (role, g.strip()))
        c = first(lines, lambda ln: U19D_CORRECTION in ln)
        if c:
            bad.append("%s: U19d's label correction fired (%s)" % (role, c.strip()))
        go = [
            (ln, int(m.group(1)), int(m.group(2)))
            for ln in lines
            for m in [GAMEOVER_RE.search(ln)]
            if m
        ]
        if not go:
            out.append("%s kept playing (no on_gameover ENTER)" % role)
            continue
        ln, sess, outcome = go[0]
        evidence = late[0] if late else streaming
        if (
            role == "leader"
            and outcome == NETWORK_ERROR_OUTCOME
            and sess == SESSION_MP_LOCKSTEP
            and t_drop is not None
            and after(ts_ms(ln), t_drop)
            and evidence
        ):
            # Outcome 7 inside session 3 is the original dispatch's unknown-outer-tag arm (its only
            # NETWORK_ERROR raise in lockstep), and it landed after the drop while side N was still
            # streaming adverts: the first advert dispatched after the drop is the frame the kick
            # re-broadcast overwrote.
            garbled = (
                "the GARBLED FRAME: side %d's next type-0x02 advert after the drop (it was "
                "streaming: %s) was overwritten by the leader's own kick re-broadcast @0x0049c330 "
                "and parsed through the dispatch's unknown-outer-tag arm, which raised "
                "NETWORK_ERROR inside lockstep %d ms after the drop: %s"
                % (side, evidence.strip(), ts_ms(ln) - t_drop, ln.strip())
            )
            bad.append(garbled)
        else:
            bad.append("%s ended its match: %s" % (role, ln.strip()))

    for b in bad:
        out.append("FAIL: " + b)
    return (not bad), out, garbled


# ---- selftest ----------------------------------------------------------------------------------
BOOT = "[20:00:00.000] ; [modules] libmh: NOT BOUND -- LoadLibrary(...) failed"
PATCH_LINE = (
    "[20:00:00.010] ; gone-peer frame guard: PATCHED -- the kick re-broadcast @0049C330 now "
    "snapshots the datagram across the emit (MP U19i)"
)
DROP = "[20:00:10.000] ; GS2: peer 2 data-silent for 3000 ms > 3000 -> dropped"
LATE = "[20:00:11.000] ; GameRecv sender=2 type=0x02 count=20 last_horizon_ms=1030 (1s)"
EARLY = "[20:00:09.000] ; GameRecv sender=2 type=0x02 count=20 last_horizon_ms=1030 (1s)"
FIRE = (
    "[20:00:10.050] ; [net] gone-peer frame guard FIRED #1: kick re-broadcast for side 2, "
    "datagram (9 bytes) restored"
)
GO7 = "[20:00:10.051] ; on_gameover ENTER sess=3 outcome=7 gclk=1030 (downgrade=1)"


def _plant(root, leader, survivor, fenced):
    dirs = []
    for name, lines in (("leader", leader), ("survivor", survivor), ("fenced", fenced)):
        d = os.path.join(root, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        dirs.append(d)
    return dirs


def selftest():
    arms = [
        # (name, leader, survivor, fenced, arm, want) -- want: "pass" | "fail" | "xfail" | "refuse"
        (
            "green: the carrier fired, both kept playing",
            [BOOT, PATCH_LINE, EARLY, DROP, FIRE, LATE],
            [BOOT, DROP],
            [BOOT],
            "on",
            "pass",
        ),
        (
            "red: frames arrived after the drop but no FIRED line",
            [BOOT, PATCH_LINE, DROP, LATE],
            [BOOT, DROP],
            [BOOT],
            "on",
            "fail",
        ),
        (
            "red: NOT REACHED -- no frame from the gone side after the drop",
            [BOOT, PATCH_LINE, EARLY, DROP, FIRE],
            [BOOT, DROP],
            [BOOT],
            "on",
            "fail",
        ),
        (
            "red: the leader ended through outcome 7 even with the carrier",
            [BOOT, PATCH_LINE, DROP, FIRE, LATE, GO7],
            [BOOT, DROP],
            [BOOT],
            "on",
            "fail",
        ),
        (
            "red: the other survivor ended its match",
            [BOOT, PATCH_LINE, DROP, FIRE, LATE],
            [
                BOOT,
                DROP,
                "[20:00:10.500] ; on_gameover ENTER sess=2 outcome=8 gclk=5000 (downgrade=0)",
            ],
            [BOOT],
            "on",
            "fail",
        ),
        (
            "green: the survivor's teardown drop of the host AFTER the leader's end is ignored",
            [BOOT, PATCH_LINE, EARLY, DROP, FIRE, LATE],
            [
                BOOT,
                DROP,
                "[20:00:15.000] ; GS2: peer 0 data-silent for 3000 ms > 3000 -> dropped",
                "[20:00:15.001] ; on_gameover ENTER sess=2 outcome=8 gclk=9000 (downgrade=0)",
            ],
            [BOOT],
            "on",
            "pass",
        ),
        ("red: no drop at all", [BOOT, PATCH_LINE, LATE], [BOOT], [BOOT], "on", "fail"),
        (
            "red: the survivors dropped different sides",
            [BOOT, PATCH_LINE, DROP, FIRE, LATE],
            [BOOT, "[20:00:10.000] ; GS2: peer 1 data-silent for 3000 ms > 3000 -> dropped"],
            [BOOT],
            "on",
            "fail",
        ),
        (
            "refuse: a peer ran promoted",
            [BOOT, PATCH_LINE, DROP, FIRE, LATE],
            [DROP],
            [BOOT],
            "on",
            "refuse",
        ),
        (
            "refuse: arm on without the PATCHED boot line",
            [BOOT, DROP, FIRE, LATE],
            [BOOT, DROP],
            [BOOT],
            "on",
            "refuse",
        ),
        (
            "refuse: arm off but the carrier was armed",
            [BOOT, PATCH_LINE, DROP, LATE, GO7],
            [BOOT, DROP],
            [BOOT],
            "off",
            "refuse",
        ),
        (
            "xfail: the unguarded arm garbled on the gone side's frame",
            [BOOT, DROP, LATE, GO7],
            [BOOT, DROP],
            [BOOT],
            "off",
            "xfail",
        ),
        (
            # the rig's real shape (2026-09-24): the garble ends the leader's match 6 ms after the
            # drop, so no rollup from the gone side is ever flushed after it
            "xfail: garbled within ms of the drop, named from the pre-drop stream",
            [BOOT, EARLY, DROP, GO7],
            [BOOT, DROP],
            [BOOT],
            "off",
            "xfail",
        ),
        (
            "red: outcome 7 BEFORE the drop is not the gone-peer garble",
            [BOOT, EARLY, "[20:00:09.500] ; on_gameover ENTER sess=3 outcome=7 gclk=1000", DROP],
            [BOOT, DROP],
            [BOOT],
            "off",
            "fail",
        ),
        (
            "xpass (red): the unguarded arm came back clean -- binaries indistinguishable",
            [BOOT, DROP, LATE],
            [BOOT, DROP],
            [BOOT],
            "off",
            "fail",
        ),
    ]
    root = tempfile.mkdtemp(prefix="gpfg_selftest_")
    bad = 0
    try:
        for i, (name, leader, survivor, fenced, arm, want) in enumerate(arms):
            dirs = _plant(os.path.join(root, str(i)), leader, survivor, fenced)
            try:
                ok, _lines, garbled = check(*dirs, arm=arm)
                if (
                    arm == "off"
                ):  # the --expect-red verdict: anything but a garbled red is a failure
                    got = "xfail" if (not ok and garbled) else "fail"
                else:
                    got = "pass" if ok else "fail"
            except Refusal:
                got = "refuse"
            flag = "ok " if got == want else "BAD"
            if got != want:
                bad += 1
            print("  [%s] %-72s want=%-6s got=%s" % (flag, name, want, got))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(
        "check_gone_peer_guard --selftest: %s (%d arms)"
        % ("PASS" if not bad else "FAIL", len(arms))
    )
    return 0 if not bad else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dirs", nargs="*", help="LEADER_DIR SURVIVOR_DIR FENCED_DIR (pulled peer logs)")
    ap.add_argument("--arm", choices=("on", "off"), default="on", help="gone_peer_frame_guard knob")
    ap.add_argument(
        "--expect-red",
        action="store_true",
        help="the unguarded twin: exit 0 (XFAIL) only when the run went red on the garbled frame",
    )
    ap.add_argument("--selftest", action="store_true", help="planted logs; every negative red")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if len(args.dirs) != 3:
        ap.error("give LEADER_DIR SURVIVOR_DIR FENCED_DIR, or --selftest")
    try:
        ok, lines, garbled = check(*args.dirs, arm=args.arm)
    except Refusal as exc:
        print("check_gone_peer_guard: REFUSED -- %s" % exc)
        return 2
    for ln in lines:
        print("  " + ln)
    if args.expect_red:
        if not ok and garbled:
            print(
                "check_gone_peer_guard: XFAIL (expected red, mp:U19j) -- the unguarded arm garbled"
            )
            return 0
        print(
            "check_gone_peer_guard: XPASS/UNEXPECTED -- the unguarded arm did not go red on the "
            "garbled frame, so this run cannot tell the guarded binary from the unguarded one"
        )
        return 1
    print("check_gone_peer_guard: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
