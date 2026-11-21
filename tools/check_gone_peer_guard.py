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
  2. THE ARM IS THE ONE NAMED: the mandatory leader carrier is PATCHED in both arms.
     Its fresh-datagram snapshot description identifies on; explicit `fresh-datagram guard OFF`
     identifies off. Missing, unknown, contradictory or mismatched arm evidence is a REFUSAL.
  3. A DROP happened: a survivor logged `; GS2: peer N data-silent ... -> dropped`. N is the fenced
     peer's side id; every survivor that logged one must name the same N.
  4. THE BRANCH'S INPUT: a direct native carrier trace contains the gone side's complete nine-byte
     horizon, the emitter's six-byte CTL_KICK overwrite, and the final bytes. Rollups and another
     machine's timestamp do not establish that a particular datagram reached this branch.
  5. THE CARRIER FIRED (arm on): its FIRED marker and direct trace prove the original bytes survived.
  6. THE FRAME SURVIVED (arm on): no judged survivor logged an outcome or U19d correction.

THE NEGATIVE ARM (`--arm off`, `[net] gone_peer_frame_guard=0`) must independently witness the
actual overwrite, an invalid outer tag at offset six, and the native unknown-tag outcome call
in the same dispatch frame, thread, length and unchanged buffer. Its sequence and game clock
must match the leader's actual lockstep outcome7. This handles either survivor's watchdog
winning the removal race without accepting arbitrary outcomes or inventing unlogged bytes.
`--expect-red` accepts only that causal XFAIL; missing proof is a failure, never a retry success.

ABSENCE IS A REFUSAL (exit 2): a missing log is never a pass.

  python tools/check_gone_peer_guard.py LEADER_DIR SURVIVOR_DIR FENCED_DIR [--arm on]
  python tools/check_gone_peer_guard.py LEADER_DIR SURVIVOR_DIR FENCED_DIR --arm off --expect-red
  python tools/check_gone_peer_guard.py --selftest
"""

import argparse
import contextlib
import io
import os
import re
import shutil
import sys
import tempfile

# ---- registered needles: net.modules_outcome, net.gone_peer_frame_guard_install, net.gs2_drop,
# net.gone_peer_frame_guard_fired, net.gone_peer_frame_trace, net.gone_peer_frame_outcome,
# net.gameover_enter, net.rx_garbled, net.u19d_outcome_correction.
CONFIG1 = "libmh: NOT BOUND"
PATCHED = "gone-peer frame guard: PATCHED"
INSTALL = "gone-peer frame guard:"
GUARD_ON = "now snapshots the datagram across the emit"
GUARD_OFF = "fresh-datagram guard OFF"
GS2_RE = re.compile(r"GS2: peer (\d+) data-silent for (\d+) ms > (\d+) -> dropped")
FIRED = "gone-peer frame guard FIRED"
FIRED_RE = re.compile(re.escape(FIRED) + r" #(\d+): kick re-broadcast for side (-?\d+)")
GAMEOVER_ENTER = "on_gameover ENTER"
GAMEOVER_RE = re.compile(re.escape(GAMEOVER_ENTER) + r" sess=(\d+) outcome=(\d+)")
GARBLED = "; [rx] garbled:"
U19D_CORRECTION = "U19d: outcome-dialog said"
NETWORK_ERROR_OUTCOME = 7
SESSION_MP_LOCKSTEP = 3
TRACE = "gone-peer frame trace"
TRACE_RE = re.compile(
    r"gone-peer frame trace #(?P<serial>\d+): mode=(?P<mode>on|off) side=(?P<side>-?\d+) "
    r"len=(?P<length>\d+) before=(?P<before>[0-9a-fA-F]*) emitted=(?P<emitted>[0-9a-fA-F]*) "
    r"after=(?P<after>[0-9a-fA-F]*)\s*$"
)
OUTCOME_TRACE = "gone-peer frame outcome"
OUTCOME_TRACE_RE = re.compile(
    r"gone-peer frame outcome #(?P<serial>\d+): side=(?P<side>-?\d+) caller=[0-9a-fA-F]{8} "
    r"native_unknown_tag=(?P<native_unknown_tag>\d+) same_frame=(?P<same_frame>\d+) "
    r"same_len=(?P<same_len>\d+) same_buffer=(?P<same_buffer>\d+) "
    r"same_thread=(?P<same_thread>\d+) sess=(?P<session>\d+) outcome=(?P<outcome>\d+) "
    r"gclk=(?P<clock>-?\d+)\s*$"
)

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
    """Returns (ok, lines, expected_garble). Only an otherwise valid off-arm causal
    failure supplies expected_garble; unrelated failures cannot be accepted as XFAIL."""
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
    installs = [ln for ln in leader if INSTALL in ln]
    if (
        len(installs) != 1
        or PATCHED not in installs[0]
        or (GUARD_ON in installs[0], GUARD_OFF in installs[0]) != (arm == "on", arm == "off")
    ):
        raise Refusal(
            "arm %s requires one PATCHED carrier with unambiguous matching fresh-datagram mode"
            % arm
        )
    out.append("leader carrier: PATCHED; fresh-datagram guard %s" % arm)

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
    for role, (_s, _t, ln) in sorted(drops.items()):
        out.append("%s drop: %s" % (role, ln))
    if "leader" not in drops:
        out.append("leader has no local GS2 line; direct carrier evidence is required")

    # ---- clauses 4-5: actual native datagram, emit damage and restoration ------------------------
    try:
        kick = b"\x04\x0a" + side.to_bytes(4, "little", signed=True)
    except OverflowError as exc:
        raise Refusal("dropped side does not fit the native signed side ID") from exc
    packets, seen = {}, set()
    for ln in leader:
        if TRACE not in ln:
            continue
        m = TRACE_RE.search(ln)
        if not m:
            raise Refusal("malformed native carrier trace")
        serial = int(m["serial"])
        if serial in seen:
            raise Refusal("duplicate native carrier trace sequence %d" % serial)
        seen.add(serial)
        if int(m["side"]) != side:
            continue
        if m["mode"] != arm:
            bad.append("carrier trace contradicts the installed arm")
            continue
        try:
            before, emitted, final = (
                bytes.fromhex(m[key]) for key in ("before", "emitted", "after")
            )
        except ValueError as exc:
            raise Refusal("incomplete native carrier hex bytes") from exc
        if int(m["length"]) != 9 or len(before) != 9 or before[0] != 2:
            continue
        expected = kick + before[6:]
        if emitted != expected or final != (before if arm == "on" else emitted):
            bad.append("carrier #%d violates the observed emit/restoration byte contract" % serial)
            continue
        packets[serial] = (ln, emitted)
    if not packets:
        bad.append("no complete native gone-peer horizon/overwrite witness for side %d" % side)
    else:
        out.append("native packet witness: %s" % next(iter(packets.values()))[0].strip())

    fired = [(ln, int(m.group(2))) for ln in leader for m in [FIRED_RE.search(ln)] if m]
    if arm == "on":
        mine = [ln for ln, s in fired if s == side]
        if not mine:
            bad.append("the mandatory restoration carrier never logged FIRED for side %d" % side)
        else:
            out.append("carrier: %s (%d line(s))" % (mine[0].strip(), len(mine)))
    elif fired:
        bad.append("arm off, yet the leader logged %s" % fired[0][0].strip())

    outcomes = []
    for ln in leader:
        if OUTCOME_TRACE not in ln:
            continue
        m = OUTCOME_TRACE_RE.search(ln)
        if not m:
            raise Refusal("malformed native outcome witness")
        serial = int(m["serial"])
        packet = packets.get(serial)
        bound = all(
            int(m[field]) == 1
            for field in (
                "native_unknown_tag",
                "same_frame",
                "same_len",
                "same_buffer",
                "same_thread",
            )
        )
        if (
            arm == "off"
            and packet
            and int(m["side"]) == side
            and not 1 <= packet[1][6] <= 5
            and bound
            and int(m["session"]) == SESSION_MP_LOCKSTEP
            and int(m["outcome"]) == NETWORK_ERROR_OUTCOME
        ):
            outcomes.append((int(m["clock"]), serial, ln.strip()))

    # ---- clause 6: the frame survived -- or, unguarded, the garbled frame ------------------------
    for role in ("leader", "survivor"):
        lines = logs[role]
        g = first(lines, lambda ln: GARBLED in ln)
        if g:
            bad.append("%s hit the garbled-stream arm: %s" % (role, g.strip()))
        go = [
            (ln, int(m.group(1)), int(m.group(2)))
            for ln in lines
            for m in [GAMEOVER_RE.search(ln)]
            if m
        ]
        clock = re.search(r"\bgclk=(-?\d+)", go[0][0]) if go else None
        evidence = next(
            (record for record in outcomes if clock and record[0] == int(clock.group(1))), None
        )
        causal_garble = (
            bool(go)
            and role == "leader"
            and go[0][1] == SESSION_MP_LOCKSTEP
            and go[0][2] == NETWORK_ERROR_OUTCOME
            and evidence is not None
        )
        for correction in (ln for ln in lines if U19D_CORRECTION in ln):
            if arm == "off" and causal_garble and after(ts_ms(correction), ts_ms(go[0][0])):
                out.append("%s: expected post-garble dialog correction" % role)
            else:
                bad.append("%s: U19d's label correction fired (%s)" % (role, correction.strip()))
        if not go:
            out.append("%s kept playing (no on_gameover ENTER)" % role)
            continue
        ln, _sess, _outcome = go[0]
        if causal_garble:
            garbled = "direct native corruption witness #%d: %s; actual outcome: %s" % (
                evidence[1],
                evidence[2],
                ln.strip(),
            )
        else:
            bad.append("%s ended its match: %s" % (role, ln.strip()))

    for b in bad:
        out.append("FAIL: " + b)
    if garbled:
        out.append("FAIL: " + garbled)
    return (not bad and not garbled), out, garbled if arm == "off" and not bad else None


# ---- selftest ----------------------------------------------------------------------------------
BOOT = "[20:00:00.000] ; [modules] libmh: NOT BOUND -- LoadLibrary(...) failed"
PATCH_LINE = (
    "[20:00:00.010] ; gone-peer frame guard: PATCHED -- the kick re-broadcast @0049C330 now "
    "snapshots the datagram across the emit (MP U19i)"
)
OFF_LINE = (
    "[20:00:00.010] ; gone-peer frame guard: PATCHED -- saved continuations bypass the preamble; "
    "fresh-datagram guard OFF (MP U19i)"
)
CORRECTION = (
    "[20:00:10.052] ; U19d: outcome-dialog said 'Connection to server lost' with no real transport "
    "failure -- corrected to 'you are the last player'"
)
DROP = "[20:00:10.000] ; GS2: peer 2 data-silent for 3000 ms > 3000 -> dropped"
FIRE = (
    "[20:00:10.050] ; [net] gone-peer frame guard FIRED #1: kick re-broadcast for side 2, "
    "datagram (9 bytes) restored"
)
TRACE_ON = (
    "[20:00:10.050] ; [net] gone-peer frame trace #1: mode=on side=2 len=9 "
    "before=021122334455ff7788 emitted=040a02000000ff7788 after=021122334455ff7788"
)
TRACE_OFF = TRACE_ON.replace("mode=on", "mode=off").replace(
    "after=021122334455ff7788", "after=040a02000000ff7788"
)
DIRECT_OUTCOME = (
    "[20:00:10.051] ; [net] gone-peer frame outcome #1: side=2 caller=0049D319 "
    "native_unknown_tag=1 same_frame=1 same_len=1 same_buffer=1 same_thread=1 "
    "sess=3 outcome=7 gclk=1030"
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
    on = [BOOT, PATCH_LINE, DROP, FIRE, TRACE_ON]
    off = [BOOT, OFF_LINE, DROP, TRACE_OFF, DIRECT_OUTCOME, GO7]
    survivor, fenced = [BOOT, DROP], [BOOT]
    arms = [
        ("guarded datagram survives its actual emit", on, survivor, fenced, "on", "pass"),
        (
            "guarded survivor-first removal",
            [ln for ln in on if ln != DROP],
            survivor,
            fenced,
            "on",
            "pass",
        ),
        ("unguarded direct corruption and native outcome", off, survivor, fenced, "off", "xfail"),
        (
            "unguarded survivor-first removal",
            [ln for ln in off if ln != DROP],
            survivor,
            fenced,
            "off",
            "xfail",
        ),
        (
            "missing native packet observation",
            [ln for ln in on if ln != TRACE_ON],
            survivor,
            fenced,
            "on",
            "fail",
        ),
        (
            "outcome7 alone is not corruption proof",
            [BOOT, OFF_LINE, DROP, GO7],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        ("no restoration carrier", [ln for ln in on if ln != FIRE], survivor, fenced, "on", "fail"),
        (
            "damaged guarded datagram",
            [ln.replace("after=021122334455ff7788", "after=040a02000000ff7788") for ln in on],
            survivor,
            fenced,
            "on",
            "fail",
        ),
        (
            "off arm unexpectedly restores bytes",
            [ln.replace("after=040a02000000ff7788", "after=021122334455ff7788") for ln in off],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "emitter did not perform the kick overwrite",
            [ln.replace("emitted=040a02000000ff7788", "emitted=021122334455ff7788") for ln in off],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "different packet sender",
            [ln.replace("mode=off side=2", "mode=off side=1") for ln in off],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "different survivor removals",
            off,
            [BOOT, DROP.replace("peer 2", "peer 1")],
            fenced,
            "off",
            "fail",
        ),
        ("guarded leader ends", on + [GO7], survivor, fenced, "on", "fail"),
        (
            "other survivor ends during observation",
            off,
            survivor + [GO7.replace("10.051", "10.040")],
            fenced,
            "off",
            "fail",
        ),
        ("unexpected restoration in off arm", off + [FIRE], survivor, fenced, "off", "fail"),
        (
            "unguarded clean run does not distinguish the binary",
            [ln for ln in off if ln not in (GO7, DIRECT_OUTCOME)],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "missing direct native outcome witness",
            [ln for ln in off if ln != DIRECT_OUTCOME],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "missing actual outcome",
            [ln for ln in off if ln != GO7],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "outcome belongs to another carrier invocation",
            [ln.replace("frame outcome #1", "frame outcome #2") for ln in off],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "outcome clock does not match",
            [
                ln.replace(
                    "same_thread=1 sess=3 outcome=7 gclk=1030",
                    "same_thread=1 sess=3 outcome=7 gclk=1040",
                )
                for ln in off
            ],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "wrong native session",
            [ln.replace("sess=3", "sess=2") for ln in off],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "unrelated native outcome",
            [ln.replace("outcome=7", "outcome=8") for ln in off],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        (
            "earlier independent dialog correction",
            off + [CORRECTION.replace("10.052", "10.001")],
            survivor,
            fenced,
            "off",
            "fail",
        ),
        ("post-corruption dialog correction", off + [CORRECTION], survivor, fenced, "off", "xfail"),
        ("no watchdog removal", [ln for ln in off if ln != DROP], [BOOT], fenced, "off", "fail"),
        ("promoted peer cannot certify the native carrier", on, [DROP], fenced, "on", "refuse"),
        (
            "missing guarded carrier installation",
            [ln for ln in on if ln != PATCH_LINE],
            survivor,
            fenced,
            "on",
            "refuse",
        ),
        (
            "missing off carrier installation",
            [ln for ln in off if ln != OFF_LINE],
            survivor,
            fenced,
            "off",
            "refuse",
        ),
        (
            "wrong installed arm",
            [BOOT, PATCH_LINE, DROP, TRACE_OFF, DIRECT_OUTCOME, GO7],
            survivor,
            fenced,
            "off",
            "refuse",
        ),
        ("conflicting installations", off + [PATCH_LINE], survivor, fenced, "off", "refuse"),
        (
            "duplicate packet invocation identity",
            off + [TRACE_OFF],
            survivor,
            fenced,
            "off",
            "refuse",
        ),
        (
            "partial hex observation",
            [ln.replace("before=021122334455ff7788", "before=021122334455ff778") for ln in off],
            survivor,
            fenced,
            "off",
            "refuse",
        ),
    ]
    for field in ("native_unknown_tag", "same_frame", "same_len", "same_buffer", "same_thread"):
        arms.append(
            (
                "unbound native outcome: " + field,
                [ln.replace(field + "=1", field + "=0") for ln in off],
                survivor,
                fenced,
                "off",
                "fail",
            )
        )
    root = tempfile.mkdtemp(prefix="gpfg_selftest_")
    bad = 0
    try:
        for i, (name, leader, survivor, fenced, arm, want) in enumerate(arms):
            dirs = _plant(os.path.join(root, str(i)), leader, survivor, fenced)
            argv = [*dirs, "--arm", arm]
            if arm == "off":
                argv.append("--expect-red")
            with contextlib.redirect_stdout(io.StringIO()):
                code = main(argv)
            got = {0: "xfail" if arm == "off" else "pass", 1: "fail", 2: "refuse"}[code]
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
