#!/usr/bin/env python3
"""
tools/check_sp_recording.py -- mp:Q2 + mp:Q3: did every SINGLE-PLAYER session of a run leave its
recordings, compressed and decodable?

  mp:Q2  mh_match_state.bin.gz   the whole-session state record. Before mp:Q2 the recorder armed only at
                                 llm_strat_session_begin_multi, which no campaign, tutorial or loaded save
                                 passes -- so `[desync] state_record=1` recorded nothing in single
                                 player. (A player setting since mp:Q4; the row sets it explicitly.)
  mp:Q3  mh_rec_orders.bin.gz    mh.dll's own order recording (MHOR v1, the harness format)
         mh_rec_clock.bin.gz     its per-step game clock track

For each solo session folder that belongs to the given process folder (session.json `process_dir`),
all three must exist as .gz (no raw file left: the compression finished), `state_record.py info` must
decode the state file cleanly with an END chunk, the orders file must decode through mp_order_diff's
reader, and the clock track must be strictly increasing with EXACTLY one entry per recorded state step
-- the recorder and the order capture both start at the first sim step with the folder open and both
end at the session close, so a count that differs means one of them missed a step or a boundary.

mp:Q4 -- NO BLOCKING CLOSE. The process folder's mh_net.log must carry one `; [hitch] session_close`
line per solo session (the probe that times each recording path on the game thread is alive), and no
`session_close` / `rec_close` / `ord_close` max may reach CLOSE_BOUND_MS. The close used to wait up to
2 s for the state writer to drain; it now hands over and returns (measured ~0.3 ms). The bound is wide
on purpose: it is there to catch a wait coming back, not to time the disk.

  python tools/check_sp_recording.py [--min-sessions N] <process-run-dir>
  python tools/check_sp_recording.py --selftest
"""

import argparse
import gzip
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

STATE = "mh_match_state.bin"
ORDERS = "mh_rec_orders.bin"
CLOCK = "mh_rec_clock.bin"
CLOSE_BOUND_MS = (
    100.0  # mp:Q4: a quit-to-menu close that takes this long on the game thread is a wait
)
CLOSE_PATHS = ("session_close", "rec_close", "ord_close")
HITCH_RE = re.compile(r"; \[hitch\] (\w+)\s+n=(\d+) .*?max=([\d.]+) us")


def hitch_verdict(proc_dir, sessions):
    """(ok, lines): one session_close sample per solo session, and every close path under the bound."""
    log = os.path.join(proc_dir, "mh_net.log")
    try:
        text = open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return False, ["      FAIL hitch: no %s" % log]
    closes, worst, out, ok = 0, {}, [], True
    for m in HITCH_RE.finditer(text):
        name, n, mx = m.group(1), int(m.group(2)), float(m.group(3))
        if name == "session_close":
            closes += n
        if name in CLOSE_PATHS:
            worst[name] = max(worst.get(name, 0.0), mx)
    if closes < sessions:
        ok = False
        out.append(
            "      FAIL hitch: %d session_close sample(s) for %d solo session(s)"
            % (closes, sessions)
        )
    for name in CLOSE_PATHS:
        if worst.get(name, 0.0) >= CLOSE_BOUND_MS * 1000.0:
            ok = False
            out.append(
                "      FAIL hitch: %s max %.0f us on the game thread (bound %.0f ms)"
                % (name, worst[name], CLOSE_BOUND_MS)
            )
    out.insert(
        0,
        "      %s hitch: %d close(s); worst game-thread close %s"
        % (
            "ok  " if ok else "FAIL",
            closes,
            ", ".join("%s %.0f us" % (k, worst[k]) for k in CLOSE_PATHS if k in worst) or "(none)",
        ),
    )
    return ok, out


def solo_sessions(proc_dir):
    """The session folders whose session.json names `proc_dir` as their process folder and whose mode is
    not a lobby's (host / client)."""
    parent = os.path.dirname(os.path.abspath(proc_dir).rstrip("\\/"))
    leaf = os.path.basename(os.path.abspath(proc_dir).rstrip("\\/"))
    out = []
    for d in sorted(os.listdir(parent)):
        sj = os.path.join(parent, d, "session.json")
        if not os.path.isfile(sj):
            continue
        try:
            rec = json.load(open(sj, encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if rec.get("process_dir") != leaf or rec.get("mode") in ("host", "client"):
            continue
        out.append((os.path.join(parent, d), rec.get("mode", "?")))
    return out


def state_steps(path):
    """(ok, steps_recorded, line) via the real CLI and the module API."""
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, "state_record.py"), "info", path],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        return False, 0, "state_record.py info rc=%d" % r.returncode
    import state_record

    rec = state_record.load(path)
    if rec.stop_reason or not rec.end:
        return (
            False,
            0,
            "decode stopped (%s) / END %s" % (rec.stop_reason, "present" if rec.end else "ABSENT"),
        )
    return (
        True,
        rec.end.steps_recorded,
        "info rc=0, steps %d..%d, END present" % (rec.first_step, rec.last_step),
    )


def session_verdict(d, mode):
    lines, ok = [], True
    name = os.path.basename(d)
    for f in (STATE, ORDERS, CLOCK):
        if os.path.exists(os.path.join(d, f)):
            lines.append("      FAIL %s: raw %s left (compression did not finish)" % (name, f))
            ok = False
        if not os.path.exists(os.path.join(d, f + ".gz")):
            lines.append("      FAIL %s: %s.gz ABSENT" % (name, f))
            ok = False
    if not ok:
        return False, lines
    sok, steps, sline = state_steps(os.path.join(d, STATE + ".gz"))
    import mp_order_diff

    try:
        orders = mp_order_diff.load(os.path.join(d, ORDERS + ".gz"))
        ook = True
    except SystemExit as e:
        orders, ook = [], False
        sline += "; orders: %s" % e
    blob = gzip.decompress(open(os.path.join(d, CLOCK + ".gz"), "rb").read())
    clocks = [struct.unpack_from("<d", blob, i)[0] for i in range(0, len(blob) - len(blob) % 8, 8)]
    mono = all(b > a for a, b in zip(clocks, clocks[1:]))
    osteps = {o.step for o in orders}
    in_range = all(1 <= s <= len(clocks) for s in osteps)
    good = (
        sok
        and ook
        and len(blob) % 8 == 0
        and mono
        and in_range
        and len(clocks) == steps
        and steps >= 1
    )
    lines.append(
        "      %s %s (%s): state %s; orders %d record(s) over %d step(s); clock %d step(s)%s%s"
        % (
            "ok  " if good else "FAIL",
            name,
            mode,
            sline,
            len(orders),
            len(osteps),
            len(clocks),
            "" if mono else " NOT INCREASING",
            "" if len(clocks) == steps else " != the state file's %d steps" % steps,
        )
    )
    return good, lines


def check(proc_dir, min_sessions):
    sess = solo_sessions(proc_dir)
    out, ok_all = [], len(sess) >= min_sessions
    if not ok_all:
        out.append(
            "      FAIL: %d solo session folder(s) for %s, want >= %d"
            % (len(sess), proc_dir, min_sessions)
        )
    for d, mode in sess:
        ok, lines = session_verdict(d, mode)
        ok_all = ok_all and ok
        out.extend(lines)
    ok, lines = hitch_verdict(proc_dir, len(sess))
    ok_all = ok_all and ok
    out.extend(lines)
    out.insert(
        0,
        "  check_sp_recording: %s (%d solo session(s))" % ("PASS" if ok_all else "FAIL", len(sess)),
    )
    return ok_all, out


# ---- selftest: the session discovery and the per-file clauses, on planted folders ---------------


def selftest():
    import state_record  # noqa: F401  (the real decoder; the planted state file is checked by stub)

    fails = 0
    tmp = tempfile.mkdtemp(prefix="chk_sprec_")
    orig = globals()["state_steps"]
    try:
        proc = os.path.join(tmp, "2026_menu_solo")
        os.makedirs(proc)
        netlog = os.path.join(proc, "mh_net.log")

        def hitch_log(close_us, n=1):
            with open(netlog, "w", encoding="utf-8") as fh:
                for name in CLOSE_PATHS:
                    fh.write(
                        "[00:00:00.000] ; [hitch] %-13s n=%d mean=1.0 us p99=1 us max=%.0f us (game thread, quit)\n"
                        % (name, n, close_us)
                    )

        hitch_log(250.0)

        def plant(
            name,
            mode="campaign",
            steps=5,
            raw=None,
            missing=None,
            clock_steps=None,
            decreasing=False,
        ):
            d = os.path.join(tmp, name)
            os.makedirs(d)
            json.dump(
                {"process_dir": "2026_menu_solo", "mode": mode},
                open(os.path.join(d, "session.json"), "w"),
            )
            n = steps if clock_steps is None else clock_steps
            vals = [float(n - i) if decreasing else float(i + 1) for i in range(n)]
            files = {
                STATE: b"S%d" % steps,
                ORDERS: struct.pack("<II", 0x524F484D, 1) + struct.pack("<i", 1) + bytes(0x44),
                CLOCK: b"".join(struct.pack("<d", v) for v in vals),
            }
            for f, data in files.items():
                if f == missing:
                    continue
                with open(os.path.join(d, f + ".gz"), "wb") as fh:
                    fh.write(gzip.compress(data))
                if f == raw:
                    open(os.path.join(d, f), "wb").write(data)
            return d

        def fake_state(path):
            n = int(gzip.decompress(open(path, "rb").read())[1:])
            return True, n, "stub"

        globals()["state_steps"] = fake_state
        cases = []
        plant("s_ok", steps=5)
        cases.append(("one good session passes", 1, True))
        plant("s_host", mode="host", steps=5, missing=STATE)
        cases.append(("a lobby session is not a solo session (ignored)", 1, True))
        cases.append(("min-sessions not met fails", 2, False))
        for name, kw in (
            ("raw file left", dict(raw=ORDERS)),
            ("missing state .gz", dict(missing=STATE)),
            ("missing clock .gz", dict(missing=CLOCK)),
            ("clock count != state steps", dict(clock_steps=4)),
            ("clock not increasing", dict(decreasing=True)),
        ):
            d = plant("s_bad_" + name.replace(" ", "_").replace(".", ""), **kw)
            ok, _ = check(proc, 1)
            good = not ok
            fails += 0 if good else 1
            print("  %s %s -> FAIL" % ("ok  " if good else "BAD ", name))
            shutil.rmtree(d)
        for name, mn, want in cases:
            ok, _ = check(proc, mn)
            good = ok == want
            fails += 0 if good else 1
            print("  %s %s" % ("ok  " if good else "BAD ", name))
        for name, setup in (
            ("a close that blocked 2 s (the pre-mp:Q4 wait)", lambda: hitch_log(2000000.0)),
            ("no [hitch] lines (the probe is gone)", lambda: open(netlog, "w").close()),
        ):
            setup()
            ok, _ = check(proc, 1)
            good = not ok
            fails += 0 if good else 1
            print("  %s %s -> FAIL" % ("ok  " if good else "BAD ", name))
        hitch_log(250.0)
    finally:
        globals()["state_steps"] = orig
        shutil.rmtree(tmp, ignore_errors=True)
    print("check_sp_recording selftest: %s" % ("PASS" if fails == 0 else "%d FAIL" % fails))
    return 0 if fails == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--min-sessions", type=int, default=1)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if len(a.dirs) != 1:
        ap.error("one process run directory")
    ok, lines = check(a.dirs[0], a.min_sessions)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
