#!/usr/bin/env python3
"""
tools/check_host_clicks.py -- one named verdict per segment of the `host_clicks` scenario
(tooling:TL-SUITE-SPLICE-HOSTCLICK).

`mp_host_clicks.txt` runs three host clicks in ONE 2-peer match -- diplomacy (mp:U39), canceltask
(mp:D28), buildclick (mp:D25) -- where each used to boot its own match. Each segment is bracketed in
the host's mh_uidrive.log by `log hc <name> begin|end` lines, and each marker sits directly after
(begin) / before (end) a `gameclock <ms>` fence. The fences give the segment's SIM window
[begin_ms, end_ms), which maps onto the harness step rows by their own clock column.

A segment is RED when:
  * its markers are missing, or not adjacent to a gameclock fence (a walk that never reached the
    segment, or a script edit that moved a fence -- both REFUSE rather than judge the wrong window);
  * the peers' state hash DIVERGES FRESH inside its window: some step in the window differs while the
    step before it agreed. A divergence inherited from an earlier segment is not this segment's
    verdict -- it is printed NOT JUDGED (exit 0) until it heals, and the earlier segment is red;
  * fewer than 95% of the host's window steps have a client row (a peer died mid-segment);
  * (canceltask only) the host's mh_net.log lacks the D28 seam banner, the banner says ROUTING OFF,
    or there is no `routed as order` line -- check_cancel_task.py's clauses 1-2.

The whole-match hash compare stays the row's determinism verdict (ui_registry.hash_plan); this is
the per-segment attribution, so one red segment cannot hide the other two.

  python tools/check_host_clicks.py --segment diplomacy|canceltask|buildclick <host-dir> <client-dir>
  python tools/check_host_clicks.py --selftest     planted logs: each segment red alone
"""

import argparse
import json
import os
import re
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402
import mp_analyze  # noqa: E402

UIDRIVE_LOG = "mh_uidrive.log"
SEGMENTS = ("diplomacy", "canceltask", "buildclick")
MIN_CLIENT_FRAC = 0.95

LOG_RE = re.compile(r"; \[script\] LOG: (?P<msg>.*)")
STEP_OK_RE = re.compile(r"; \[script\] (\d+) ok \(waited [^)]*\): (.*)")
STEP_DO_RE = re.compile(r"; \[script\] (\d+) do: (.*)")
RETRY_RE = re.compile(r"; \[script\] RETRY (\d+)/(\d+) at step (\d+)")
GAMECLOCK_RE = re.compile(r"gameclock (\d+)\s*$")


class Refusal(Exception):
    pass


def process_dir(run_dir):
    """The dir holding mh_uidrive.log: the run dir itself, or the process dir its session.json names."""
    if os.path.isfile(os.path.join(run_dir, UIDRIVE_LOG)):
        return run_dir
    sess = mp_analyze.read_session_json(run_dir) or {}
    pd = sess.get("process_dir")
    if pd:
        cand = os.path.join(os.path.dirname(os.path.abspath(run_dir).rstrip("\\/")), pd)
        if os.path.isfile(os.path.join(cand, UIDRIVE_LOG)):
            return cand
    raise Refusal(
        "no %s in %s (nor in the process dir its session.json names)" % (UIDRIVE_LOG, run_dir)
    )


def parse_uidrive(path):
    """Ordered events: ("ok", step, text) / ("do", step, text) / ("log", None, msg) / ("retry", step, n)."""
    ev = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = LOG_RE.search(line)
            if m:
                ev.append(("log", None, m.group("msg").rstrip()))
                continue
            m = STEP_OK_RE.search(line)
            if m:
                ev.append(("ok", int(m.group(1)), m.group(2).rstrip()))
                continue
            m = STEP_DO_RE.search(line)
            if m:
                ev.append(("do", int(m.group(1)), m.group(2).rstrip()))
                continue
            m = RETRY_RE.search(line)
            if m:
                ev.append(("retry", int(m.group(3)), int(m.group(1))))
    return ev


def window_ms(events, seg):
    """(begin_ms, end_ms, retries) for `seg` from its markers and their adjacent gameclock fences."""
    begin, end = "hc %s begin" % seg, "hc %s end" % seg
    idx = {msg: i for i, (k, _, msg) in enumerate(events) if k == "log"}
    for want in (begin, end):
        if want not in idx:
            raise Refusal(
                "no `log %s` line -- the walk never reached this marker (see the script's last step)"
                % want
            )
    bi, ei = idx[begin], idx[end]
    # "N do: log ..." sits right before its LOG line; the fence is the step event just before that.
    steps_before = [e for e in events[:bi] if e[0] in ("ok", "do")]
    if len(steps_before) < 2 or steps_before[-1][0] != "do":
        raise Refusal("`log %s` has no step line before it" % begin)
    fence = steps_before[-2]
    m = GAMECLOCK_RE.search(fence[2]) if fence[0] == "ok" else None
    if not m:
        raise Refusal(
            "`log %s` is not directly preceded by a gameclock fence (got %r)" % (begin, fence[2])
        )
    b_ms = int(m.group(1))
    after = [e for e in events[ei + 1 :] if e[0] in ("ok", "do")]
    m = GAMECLOCK_RE.search(after[0][2]) if after and after[0][0] == "ok" else None
    if not m:
        raise Refusal(
            "`log %s` is not directly followed by a gameclock fence (got %r)"
            % (end, after[0][2] if after else None)
        )
    e_ms = int(m.group(1))
    if e_ms <= b_ms:
        raise Refusal("segment window is empty: %d ms .. %d ms" % (b_ms, e_ms))
    retries = sum(1 for k, _, _ in events[bi:ei] if k == "retry")
    return b_ms, e_ms, retries


def hash_verdict(host, client, b_ms, e_ms):
    """(fails, notes, summary) for the peers' state hash inside [b_ms, e_ms)."""
    sa, sb = host["steps"], client["steps"]
    win = sorted(
        s
        for s in sa
        if (c := mp_analyze.step_clock_seconds(host, s)) is not None and b_ms <= c * 1000.0 < e_ms
    )
    if not win:
        return ["hash: no host harness step inside %d..%d ms" % (b_ms, e_ms)], [], "no steps"
    common = [s for s in win if s in sb]
    fails, notes = [], []
    if len(common) < MIN_CLIENT_FRAC * len(win):
        fails.append(
            "hash: only %d of the host's %d window steps have a client row -- a peer stopped mid-segment"
            % (len(common), len(win))
        )
    all_common = sorted(set(sa) & set(sb))
    prev = {s: p for p, s in zip([None] + all_common, all_common)}

    def differs(s):
        return s is not None and sa[s]["state"] != sb[s]["state"]

    onsets = [s for s in common if differs(s) and not differs(prev.get(s))]
    mism = [s for s in common if differs(s)]
    if onsets:
        s0 = onsets[0]
        ra, rb = host["regions"].get(s0), client["regions"].get(s0)
        regs = ""
        if ra and rb and len(ra) == len(rb):
            regs = " (%s)" % ", ".join(
                mp_analyze.REGION_NAMES[i] if i < len(mp_analyze.REGION_NAMES) else "region[%d]" % i
                for i, (x, y) in enumerate(zip(ra, rb))
                if x != y
            )
        fails.append(
            "hash: state diverges fresh at step %d inside this segment%s -- %d differing step(s) in the window"
            % (s0, regs, len(mism))
        )
    elif mism:
        notes.append(
            "NOT JUDGED by hash: already diverged when the segment began (first at step %d, an "
            "earlier segment's verdict); %d of %d window steps differ"
            % (next(s for s in all_common if differs(s)), len(mism), len(common))
        )
    summary = "window %d..%d ms = steps %d..%d, %d compared, %d differ" % (
        b_ms,
        e_ms,
        win[0],
        win[-1],
        len(common),
        len(mism),
    )
    return fails, notes, summary


def canceltask_clauses(host_dir):
    lines = check_cancel_task.net_log_lines(host_dir)
    banner = [ln for ln in lines if check_cancel_task.SPLICED in ln]
    routed = [ln for ln in lines if check_cancel_task.ROUTED in ln]
    fails = []
    if not banner:
        fails.append("d28: no `; %s` banner -- the seam did not run" % check_cancel_task.SPLICED)
    elif check_cancel_task.ROUTING_OFF in banner[-1]:
        fails.append("d28: the banner says ROUTING OFF -- the reproduction arm, not the ship arm")
    if not routed:
        fails.append(
            "d28: no `%s` line -- the Yes click never reached the order pipeline"
            % check_cancel_task.ROUTED
        )
    return fails


def check(seg, host_dir, client_dir):
    """(fails, notes, summary). Raises Refusal when the segment cannot be isolated at all."""
    events = parse_uidrive(os.path.join(process_dir(host_dir), UIDRIVE_LOG))
    b_ms, e_ms, retries = window_ms(events, seg)
    a, b = mp_analyze.load_peer(host_dir), mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        raise Refusal(
            "a peer has no harness log (host=%s client=%s)" % ("harness" in a, "harness" in b)
        )
    fails, notes, summary = hash_verdict(a["harness"], b["harness"], b_ms, e_ms)
    if seg == "canceltask":
        fails += canceltask_clauses(host_dir)
    if retries:
        notes.append(
            "%d click retr%s inside the segment" % (retries, "y" if retries == 1 else "ies")
        )
    return fails, notes, summary


def report(seg, label, host_dir, client_dir):
    name = label or seg
    try:
        fails, notes, summary = check(seg, host_dir, client_dir)
    except Refusal as e:
        print("check_host_clicks [%s]: FAIL (refused) -- %s" % (name, e))
        return 1
    status = "FAIL" if fails else ("NOT JUDGED" if notes and "NOT JUDGED" in notes[0] else "PASS")
    print("check_host_clicks [%s]: %s -- %s" % (name, status, summary))
    for f in fails:
        print("  " + f)
    for n in notes:
        print("  note: " + n)
    return 1 if fails else 0


# ---- selftest ------------------------------------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
FENCES = {"diplomacy": (3200, 20000), "canceltask": (20000, 29000), "buildclick": (29000, 39000)}
LAST_STEP = 3950  # 10 ms per step: past the last fence


def _uidrive_lines(stop_before=None, bad_fence=None):
    """A host mh_uidrive.log shaped like mp_host_clicks.txt's: shared gameclock fences between the
    segments' markers. `stop_before` ends the walk before that segment; `bad_fence` puts a click
    between that segment's begin fence and its begin marker."""
    out, n = [], 0

    def step(kind, text):
        nonlocal n
        head = "ok (waited 3 fr, 0.050s)" if kind == "ok" else "do"
        out.append("[ 1.000] ; [script] %d %s: %s\n" % (n, head, text))
        n += 1

    step("ok", "gameclock 3000")
    step("do", "rclick 480 330 shift")
    step("ok", "gameclock %d" % FENCES["diplomacy"][0])
    for seg in SEGMENTS:
        if seg == stop_before:
            return out
        if seg == bad_fence:
            step("do", "press 1 1")
        step("do", "log hc %s begin" % seg)
        out.append("[ 1.000] ; [script] LOG: hc %s begin\n" % seg)
        step("do", "press 1 1")
        step("do", "release 1 1")
        if seg == "diplomacy":
            out.append(
                "[ 1.000] ; [script] RETRY 1/3 at step %d after 1500 game-ms: rewinding 4\n" % n
            )
        step("ok", "retry 1500 4 3 settled value:100")
        step("do", "log hc %s end" % seg)
        out.append("[ 1.000] ; [script] LOG: hc %s end\n" % seg)
        step("ok", "gameclock %d" % FENCES[seg][1])
    out.append("[ 1.000] ; [script] COMPLETE (end)\n")
    return out


def _plant(root, name, diverge=None, net=None, uidrive=None, steps=LAST_STEP):
    """A process dir + a session dir naming it, like a real post_check_session target."""
    proc = os.path.join(root, name, "logs", "20260926T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260926T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 "
            "region_hash_step=50 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = diverge is not None and diverge[0] <= s < diverge[1]
            clk = struct.pack(">d", s * 0.01).hex().upper()
            fh.write("%d %s %016X %016X\n" % (s, clk, 0xB000 + s, 0xA000 + s + (1 if dv else 0)))
            if s % 50 == 0 or dv:
                fh.write(
                    "R %d %s\n"
                    % (
                        s,
                        " ".join(
                            "%016X" % (0xC000 + s + i + (1 if dv and i == 0 else 0))
                            for i in range(REGION_COUNT)
                        ),
                    )
                )
    if uidrive is not None:
        with open(os.path.join(proc, UIDRIVE_LOG), "w", encoding="utf-8") as fh:
            fh.write("".join(uidrive))
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write("".join(net or []))
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": os.path.basename(proc)}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


NET_OK = [
    "; %s 004C7060 -- in a lockstep match the cancel is a replicated building order\n"
    % check_cancel_task.SPLICED,
    "; D28: %s (bldg 3 state 0x6D -> idle 0x0067) instead of the local call\n"
    % check_cancel_task.ROUTED,
]
NET_OFF = [
    "; %s 004C7060 -- %s: retail local call\n"
    % (check_cancel_task.SPLICED, check_cancel_task.ROUTING_OFF),
]


def selftest():
    # (label, host-kwargs, expected exit per segment (diplomacy, canceltask, buildclick))
    cases = [
        ("clean run: three PASS", {}, (0, 0, 0)),
        (
            "u39 echo shape: 4 steps diverge in seg 1 and heal -> seg 1 ONLY",
            {"diverge": (400, 404)},
            (1, 0, 0),
        ),
        (
            "d28 local shape: seg 2 diverges, never heals -> seg 2 ONLY",
            {"diverge": (2100, 10**6)},
            (0, 1, 0),
        ),
        ("d28: no routed line -> seg 2 ONLY", {"net": NET_OK[:1]}, (0, 1, 0)),
        ("d28: banner says ROUTING OFF -> seg 2 ONLY", {"net": NET_OFF + NET_OK[1:]}, (0, 1, 0)),
        (
            "d25 original shape: seg 3 diverges, never heals -> seg 3 ONLY",
            {"diverge": (3050, 10**6)},
            (0, 0, 1),
        ),
        (
            "walk stopped before seg 3 -> seg 3 refuses ONLY",
            {"uidrive": _uidrive_lines(stop_before="buildclick")},
            (0, 0, 1),
        ),
        (
            "seg 2 begin not on a gameclock fence -> seg 2 refuses ONLY",
            {"uidrive": _uidrive_lines(bad_fence="canceltask")},
            (0, 1, 0),
        ),
        ("client died in seg 3 -> seg 3 ONLY", {"client_steps": 3300}, (0, 0, 1)),
    ]
    bad = 0
    for label, kw, want in cases:
        root = tempfile.mkdtemp(prefix="hcchk_")
        try:
            kw = dict(kw)
            csteps = kw.pop("client_steps", LAST_STEP)
            kw.setdefault("net", NET_OK)
            kw.setdefault("uidrive", _uidrive_lines())
            h = _plant(root, "host", **kw)
            c = _plant(root, "client", net=[], steps=csteps)
            got = []
            for seg in SEGMENTS:
                try:
                    fails, _, _ = check(seg, h, c)
                    got.append(1 if fails else 0)
                except Refusal:
                    got.append(1)
            ok = tuple(got) == want
            bad += 0 if ok else 1
            print("  %s  %-66s want=%s got=%s" % ("ok " if ok else "BAD", label, want, tuple(got)))
        finally:
            shutil.rmtree(root, ignore_errors=True)
    # an inherited divergence is reported NOT JUDGED, never PASS
    root = tempfile.mkdtemp(prefix="hcchk_")
    try:
        h = _plant(root, "host", diverge=(2100, 10**6), net=NET_OK, uidrive=_uidrive_lines())
        c = _plant(root, "client", net=[])
        _, notes, _ = check("buildclick", h, c)
        ok = bool(notes) and "NOT JUDGED" in notes[0]
        bad += 0 if ok else 1
        print(
            "  %s  %-66s"
            % ("ok " if ok else "BAD", "inherited divergence reads NOT JUDGED, not PASS")
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)
    total = len(cases) + 1
    print("check_host_clicks selftest: %d/%d" % (total - bad, total))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--segment", choices=SEGMENTS)
    ap.add_argument("--label", default=None, help="name printed on the verdict line")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*", help="<host-run-dir> <client-run-dir>")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.segment or len(args.dirs) != 2:
        ap.error("--segment and exactly two run dirs (host, client) are required")
    return report(args.segment, args.label, args.dirs[0], args.dirs[1])


if __name__ == "__main__":
    sys.exit(main())
