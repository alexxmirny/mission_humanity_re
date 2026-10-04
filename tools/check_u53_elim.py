#!/usr/bin/env python3
"""check_u53_elim.py -- mp:U53 phase 1: what do the SURVIVORS of a 3-peer ELIMINATION do?

Reads the three pulled peer dirs of a `det_arms.py --u53-*` run (<det_dir>/{host,client1,client2}) and
REPORTS: the elimination step (the killer's `; CONQ ... FORCE-KILL` line), every peer's last harness
step and the steps it simulated after the elimination, presence_lost / on_gameover / session-end /
stall-looking lines per peer, and mp_analyze verdicts (survivor vs survivor, victim vs survivor).

It is a REPORT first: only hard conditions turn it red -- a survivor that did not step
>= MIN_POST steps past the elimination, or a survivor pair that is not ALL PAIRS IDENTICAL over
>= MIN_COMMON common steps. Absence of a log is a Refusal, never a pass.

  python tools/check_u53_elim.py <det_dir> <victim: host|client1> [--arm A] [--kill-step N]
"""

import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.abspath(__file__))
MP_ANALYZE = os.path.join(REPO, "mp_analyze.py")
MIN_POST = 1500
MIN_COMMON = 1000
PEERS = ("host", "client1", "client2")
STEP_RE = re.compile(r"^(\d+) ([0-9A-F]{16}) ")
INTEREST = re.compile(
    r"presence_lost player|on_gameover|; GAMEOVER|SESSION_END|PLAYER_LEFT|player_left|mark_player_gone"
    r"|llm_net_player_remove|fast-drop|\[desync\] \*\*\*|DESYNC step|WAITING FOR|\[resync\] (trigger|start|RESUME)"
    r"|LOCALISED|CONQ.*FORCE-KILL|CONQ FAIL|U40:|U17|U19|data[ _]timeout|data-silent|dropping peer|written off|EXIT-PROCESS|timed out|timeout|stall|silent|hub|link (dead|lost|down)|peer .*(lost|gone|closed)",
    re.I,
)
NOISE = re.compile(r"\[promote\]|\[rebind\]|\[interlock\]|\[tombstone\]|armed|logger")


class Refusal(Exception):
    pass


def _read(path):
    if not os.path.isfile(path):
        raise Refusal("missing %s" % path)
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def harness_steps(text):
    """[(step, clock_seconds)] from the per-step hash rows."""
    import struct

    out = []
    for ln in text.splitlines():
        m = STEP_RE.match(ln)
        if m:
            clk = struct.unpack(">d", bytes.fromhex(m.group(2)))[0]
            out.append((int(m.group(1)), clk))
    return out


def lockstep_facts(path, peer):
    """peer0/peer1 horizon freeze, stall column, longest committed-clock freeze, since_rx max."""
    if not os.path.isfile(path):
        return ["    %s: (no mh_lockstep.log)" % peer]
    rows = []
    for ln in open(path, errors="replace"):
        f = ln.split()
        if ln.startswith("#") or len(f) < 14:
            continue
        try:
            rows.append([int(x) for x in f[:14]])
        except ValueError:
            pass
    if not rows:
        return ["    %s: (empty mh_lockstep.log)" % peer]
    # cols: 0 wall_ms 1 clock_ms 2 total_ms 3 local_h 4 committed 5 peer0 6 peer1 7 step 8 stall 9 pcount
    #       10 tx 11 rx 12 since_rx_ms 13 sess
    worst_gap, prev = 0, rows[0]
    for r in rows[1:]:
        if r[4] == prev[4]:
            worst_gap = max(worst_gap, r[0] - prev[0] + 0)
        else:
            prev = r
    # a frozen horizon: the last value + the clock at which it stopped moving
    out = []
    for col, nm in ((5, "peer0_ms"), (6, "peer1_ms")):
        last = rows[-1][col]
        first_at = next((r[1] for r in rows if r[col] == last), None)
        out.append("%s=%d (first at clock %s)" % (nm, last, first_at))
    sess = sorted({r[13] for r in rows})
    out.append("committed longest freeze=%d ms" % worst_gap)
    out.append(
        "stall col max=%d rows>0=%d" % (max(r[8] for r in rows), sum(1 for r in rows if r[8] > 0))
    )
    out.append("since_rx max=%d ms" % max(r[12] for r in rows))
    out.append("sess values seen=%s" % sess)
    return ["    %s lockstep: %s" % (peer, "; ".join(out))]


def net_facts(net, peer):
    out = []
    names = []
    for m in re.finditer(r"\[netind\] peer0 name=(\S+)", net):
        if not names or names[-1] != m.group(1):
            names.append(m.group(1))
    if names:
        out.append("    %s netind peer0 name sequence: %s" % (peer, " -> ".join(names)))
    hw = re.findall(r"udp inbound queue depth high-water (\d+) / (\d+)", net)
    if hw:
        out.append(
            "    %s udp inbound queue high-water: last %s / %s" % (peer, hw[-1][0], hw[-1][1])
        )
    uc = re.findall(r"udp counters .*", net)
    if uc:
        out.append("    %s last udp counters: %s" % (peer, re.sub(r"^\S+ ", "", uc[-1])[:200]))
    nm = re.findall(r"; \[freeze\] main thread no present for (\d+) ms", net)
    if nm:
        out.append(
            "    %s main-thread freezes: %d (max %d ms)" % (peer, len(nm), max(map(int, nm)))
        )
    ds = re.findall(r"; \[desync\] STATUS: samples=(\d+).*?compared=(\d+) mismatching=(\d+)", net)
    if ds:
        out.append(
            "    %s last in-band desync STATUS: samples=%s compared=%s mismatching=%s"
            % ((peer,) + ds[-1])
        )
    return out


def analyze(a, b, min_common=1):
    r = subprocess.run(
        [sys.executable, MP_ANALYZE, "--min-common", str(min_common), a, b],
        capture_output=True,
        text=True,
    )
    return (r.stdout or "") + (r.stderr or "")


def step_rows(har):
    """{step: STATE hash column} (mp_analyze compares `state`, not `combined`)."""
    rows = {}
    for ln in har.splitlines():
        f = ln.split()
        if len(f) >= 3 and f[0].isdigit() and re.fullmatch(r"[0-9A-F]{16}", f[1]):
            rows[int(f[0])] = f[3] if len(f) > 3 else f[2]
    return rows


def live_end(net):
    """Last sim step the peer simulated in-session: the final `[desync] STATUS ... step=N`."""
    v = re.findall(r"; \[desync\] STATUS: samples=\d+.*? step=(\d+)", net)
    return int(v[-1]) if v else 0


def live_compare(a, b, logs):
    """Survivor-vs-survivor hash compare over steps 1..min(live end): the 4000-step harness tail
    after the session ended (sim runs on with no lockstep) is NOT comparable, so mp_analyze's
    whole-run verdict is not the evidence in exit mode. Returns (line, ok)."""
    end = min(live_end(logs[a][0]), live_end(logs[b][0]))
    ra, rb = step_rows(logs[a][1]), step_rows(logs[b][1])
    first = next((i for i in range(1, end + 1) if ra.get(i) != rb.get(i)), None)
    if end == 0:
        return "survivor live-window hash compare: no live end found (no STATUS lines)", False
    if first is None:
        return (
            "survivors %s vs %s over the LIVE window steps 1..%d (a ended at %d, b at %d): IDENTICAL"
            % (a, b, end, live_end(logs[a][0]), live_end(logs[b][0])),
            True,
        )
    return (
        "survivors %s vs %s over the LIVE window steps 1..%d: FIRST DIFFERENCE at step %d"
        % (a, b, end, first),
        False,
    )


TL_EVENTS = (
    ("first stall waiting on host", re.compile(r"\[netind\] stall waiting_for=host")),
    ("link silent/dropped", re.compile(r"udp conn 0 (silent|dropped)")),
    ("resync trigger (first)", re.compile(r"\[resync\] trigger_count=")),
    (
        "peer removed (player_remove)",
        re.compile(r"promote] wire/player_remove: call"),
    ),
    ("on_gameover", re.compile(r"on_gameover ENTER")),
    ("SESSION_END", re.compile(r"\[session\] SESSION_END")),
)


def _secs(ts):
    h, m, sec = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(sec)


def timeline(net, peer):
    """Wall-clock timeline of what a survivor did after the hub died, seconds relative to its first
    stall on the host (the earliest visible symptom of the hub's death)."""
    ev = []
    for ln in net.splitlines():
        m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\] (.*)", ln)
        if not m:
            continue
        for name, rx in TL_EVENTS:
            if rx.search(m.group(2)):
                ev.append((_secs(m.group(1)), name, m.group(2)))
    ev.sort()
    seen, out, t0 = set(), [], None
    for t, name, txt in ev:
        if name in seen:
            continue
        seen.add(name)
        if t0 is None:
            t0 = next((e[0] for e in ev if e[1].startswith("first stall")), t)
        extra = ""
        if name == "on_gameover":
            mo = re.search(r"outcome=(\d+) gclk=(\d+)", txt)
            extra = " outcome=%s gclk=%s" % mo.groups() if mo else ""
        if name == "link silent/dropped":
            extra = " (%s)" % txt[:90]
        out.append("    %s timeline +%6.1fs  %s%s" % (peer, t - t0, name, extra))
    if not out:
        return ["    %s timeline: no hub-death events in the net log" % peer]
    return out


def exit_facts(det_dir, victim, exit_step, last_steps):
    """mp:U55: the victim's `; EXIT-PROCESS` line + pid, and (best effort) a pid-gone probe over ssh.
    Returns (lines, bad)."""
    har = _read(os.path.join(det_dir, victim, "mh_harness.log"))
    m = re.search(r"; EXIT-PROCESS step=(\d+) mode=(\w+) pid=(\d+)", har)
    if not m:
        return (
            ["host exit line: MISSING (no `; EXIT-PROCESS` in %s's harness log)" % victim],
            ["the exit knob never fired"],
        )
    step, mode, pid = int(m.group(1)), m.group(2), int(m.group(3))
    out = [
        "host exit line: step=%d mode=%s pid=%d (requested step %d); last hashed step after it=%d"
        % (step, mode, pid, exit_step, last_steps.get(victim, 0))
    ]
    bad = []
    if last_steps.get(victim, 0) > step:
        bad.append("host kept hashing after its exit line (%d > %d)" % (last_steps[victim], step))
    try:
        import machine_config as machine
        import mp_run

        ip = machine.RIG_PEERS[0]
        r = mp_run.ssh(machine.SSH_KEY, machine.VM_USER, ip, 'tasklist /FI "PID eq %d" /NH' % pid)
        txt = (r.stdout or "").strip()
        gone = str(pid) not in txt or "No tasks" in txt
        out.append(
            "host pid %d on %s after the run: %s (%s)"
            % (pid, ip, "GONE" if gone else "STILL RUNNING", txt[:80])
        )
        if not gone:
            out.append(
                "(note: the runner may have restarted/left it; the harness log's last step is the authority)"
            )
    except Exception as exc:  # best effort: the VM may already be torn down
        out.append("host pid probe unavailable: %s" % str(exc)[:100])
    return out, bad


def check(det_dir, victim, arm="", kill_step=300, exit_step=None):
    out, bad = [], []
    logs = {}
    for p in PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (_read(os.path.join(d, "mh_net.log")), _read(os.path.join(d, "mh_harness.log")))
    survivors = [p for p in PEERS if p != victim]
    last_steps = {}
    # elimination step: the killer's FORCE-KILL line
    ks = None
    for p in PEERS:
        m = re.search(r"; CONQ step=(\d+) FORCE-KILL: order 0xf8 x(\d+)", logs[p][1])
        if m:
            ks = int(m.group(1))
            out.append("force-kill issued by %s at step %d (x%s objects)" % (p, ks, m.group(2)))
    if ks is None:
        out.append("NOTE: no FORCE-KILL line in any harness log -- the kill never issued")
        ks = kill_step
    for p in PEERS:
        net, har = logs[p]
        st = harness_steps(har)
        last = st[-1] if st else (0, 0.0)
        post = last[0] - ks
        role = "VICTIM  " if p == victim else "survivor"
        out.append(
            "%-7s %s last hashed step=%d (gclk %.1fs), %d steps past the elimination"
            % (p, role, last[0], last[1], post)
        )
        last_steps[p] = last[0]
        if exit_step is None and p != victim and post < MIN_POST:
            bad.append("%s stopped %d steps after the elimination (< %d)" % (p, post, MIN_POST))
        out += lockstep_facts(os.path.join(det_dir, p, "mh_lockstep.log"), p)
        out += net_facts(net, p)
        lines = [
            ln
            for ln in (net + "\n" + har).splitlines()
            if INTEREST.search(ln) and not NOISE.search(ln)
        ]
        seen = set()
        shown = 0
        for ln in lines:
            key = re.sub(r"\d+", "#", ln[12:90])
            if key in seen:
                continue
            seen.add(key)
            out.append("    %s: %s" % (p, ln.strip()[:210]))
            shown += 1
            if shown >= 14:
                out.append("    %s: ... (%d interesting lines total)" % (p, len(lines)))
                break
    # stalls from frametime logs are not step-keyed; just report size of the gap if any
    a, b = survivors
    txt = analyze(os.path.join(det_dir, a), os.path.join(det_dir, b), MIN_COMMON)
    m = re.search(r"combined-hash=(\d+)", txt)
    common = int(m.group(1)) if m else 0
    ident = "ALL PAIRS IDENTICAL" in txt
    out.append(
        "survivors %s vs %s: %s, %d common hashed steps"
        % (a, b, "IDENTICAL" if ident else "NOT IDENTICAL", common)
    )
    if not ident and "ALL PAIRS" not in txt and common == 0:
        out.append("(mp_analyze found no common steps)")
    if not ident and exit_step is not None:
        out.append(
            "(mp_analyze whole-run NOT IDENTICAL is expected here: it spans the post-session tail)"
        )
    elif not ident:
        bad.append("survivors are not ALL PAIRS IDENTICAL")
        out += ["    " + ln for ln in txt.strip().splitlines()[-8:]]
    if exit_step is not None:
        lc, lok = live_compare(a, b, logs)
        out.append(lc)
        if not lok:
            bad.append("survivors differ inside their live window")
        xl, xbad = exit_facts(det_dir, victim, exit_step, last_steps)
        out += xl
        bad += xbad
        for sv in survivors:
            out += timeline(logs[sv][0], sv)
            out.append(
                "%s survived %d steps past the host's exit step %d (last step %d)"
                % (sv, last_steps[sv] - exit_step, exit_step, last_steps[sv])
            )
        out.append(
            "(exit mode is a MEASUREMENT: survivor progress / common-step floors are not verdicts)"
        )
    elif common < MIN_COMMON:
        bad.append("only %d common steps between survivors (< %d)" % (common, MIN_COMMON))
    for s in survivors:
        txt = analyze(os.path.join(det_dir, victim), os.path.join(det_dir, s), 1)
        m = re.search(r"first (?:divergent|mismatch)[^\n]*", txt, re.I)
        v = [
            ln
            for ln in txt.splitlines()
            if "VERDICT" in ln or "state-mismatch" in ln or "DESYNC" in ln
        ]
        out.append("victim %s vs %s: %s" % (victim, s, " | ".join(x.strip() for x in v[:3])[:300]))
    return (not bad), out + ["FAIL: " + b for b in bad]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("det_dir")
    ap.add_argument("victim", choices=["host", "client1"])
    ap.add_argument("--kill-step", type=int, default=300)
    a = ap.parse_args()
    try:
        ok, lines = check(a.det_dir, a.victim, "", a.kill_step)
    except Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
