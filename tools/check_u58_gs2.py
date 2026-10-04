#!/usr/bin/env python3
"""check_u58_gs2.py -- mp:U58: IN A 3-PEER STAR, BOTH SURVIVORS MUST WATCH THE DATA-SILENT PEER.

mp:GS2's data-timeout watchdog (net_lockstep.cpp `data_timeout_tick`) once bounded its slot scan with
`1 + MH_Net_PeerCount()`. PeerCount is a peer's own active transport connections: the host holds one per
client, a client holds ONE (the host). So the bound was only right for 2 peers; in a 3-peer match client 1
scanned slots 0..1 and nobody watched slot 2 from a client. The scan now covers every slot and lets
slot_is_active_human() + the horizon observer decide which are real.

Reads <det_dir>/{host,client1,client2} (a `det_arms.py --determinism --u58-gs2-3peer` run; client 2 is the
sim-FENCED peer: transport up, horizon frozen). Asserts:
  1. client 1 logs exactly ONE `; GS2: peer 2 data-silent ... -> dropped` (its own watchdog -- the clients'
     timeout is shorter than the host's), the host logs none or that one line for slot 2 (it follows
     client 1's removal), the fenced peer logs no GS2 line at all.
  2. both survivors PARKED at the same sim clock while waiting for the fenced peer (the longest constant
     clock_ms run in mh_lockstep.log, which is the stall on slot 2's frozen horizon).
  3. the survivors' per-step state hashes are IDENTICAL over the live window (check_u53_elim.live_compare).
Absence of a log is a REFUSAL (exit 2), never a pass.

  python tools/check_u58_gs2.py <det_dir>
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_u53_elim as u53  # noqa: E402

GS2_RE = re.compile(r"GS2: peer (\d+) data-silent for (\d+) ms > (\d+) -> dropped")
FENCED_SLOT = 2


def read(path):
    if not os.path.isfile(path):
        raise u53.Refusal("missing %s" % path)
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def parked_clocks(lockstep_text, min_rows=4):
    """{clock_ms: rows} for every clock the peer sat on for >= min_rows consecutive frame rows."""
    runs, prev, n = {}, None, 0
    for ln in lockstep_text.splitlines():
        f = ln.split()
        if not f or f[0].startswith("#") or len(f) < 2 or not f[1].lstrip("-").isdigit():
            continue
        c = int(f[1])
        if c == prev:
            n += 1
        else:
            prev, n = c, 1
        if n >= min_rows:
            runs[c] = n
    return runs


def check(det_dir):
    logs, lock = {}, {}
    for p in u53.PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (read(os.path.join(d, "mh_net.log")), read(os.path.join(d, "mh_harness.log")))
        lock[p] = read(os.path.join(d, "mh_lockstep.log"))
    bad, out = [], []
    drops = {p: [m.groups() for m in GS2_RE.finditer(logs[p][0])] for p in u53.PEERS}
    # The CLIENTS' data_timeout_ms is shorter than the host's (det_arms.run_u58), so the first watchdog to
    # fire is CLIENT 1's -- which only happens if its scan covers slot 2 (mp:U58). Its removal goes to the
    # host as a lockstep control record and the host applies it (and re-broadcasts the kick), so the host
    # needs no GS2 line of its own: it logs none, or at most one for slot 2. What it MUST do is play on.
    d = drops["client1"]
    if len(d) != 1 or int(d[0][0]) != FENCED_SLOT:
        bad.append("client1: expected exactly one GS2 drop of slot %d, got %s" % (FENCED_SLOT, d))
    else:
        out.append("client1: GS2 dropped slot %s after %s ms (T=%s)" % (d[0][0], d[0][1], d[0][2]))
    d = drops["host"]
    if len(d) > 1 or any(int(x[0]) != FENCED_SLOT for x in d):
        bad.append("host: GS2 drops other than a single slot-%d one: %s" % (FENCED_SLOT, d))
    else:
        out.append(
            "host: %s"
            % ("GS2 dropped slot 2 as well" if d else "no GS2 line (followed client1's removal)")
        )
    if drops["client2"]:
        bad.append("client2 (the fenced peer) logged a GS2 drop: %s" % drops["client2"])
    parked = {p: parked_clocks(lock[p]) for p in ("host", "client1")}
    for p, runs in parked.items():
        out.append("%s: parked clocks (ms: rows) %s" % (p, dict(sorted(runs.items()))))
    # The FINAL park (the highest clock either survivor sat on) is the one waiting on slot 2's frozen
    # horizon; the promoted build also parks briefly on the earlier 30 ms grid point, so compare the max.
    pk = {p: (max(r) if r else None) for p, r in parked.items()}
    if pk["host"] is None or pk["host"] != pk["client1"]:
        bad.append("survivors parked at different clocks: %s" % pk)
    park_clock = pk["host"]
    peak = {
        p: max(
            [
                int(ln.split()[1])
                for ln in lock[p].splitlines()
                if ln[:1].isdigit() and len(ln.split()) > 1
            ]
            or [0]
        )
        for p in ("host", "client1")
    }
    for p, c in peak.items():
        out.append("%s: furthest game clock %d ms" % (p, c))
        if park_clock is not None and c < park_clock + 2000:
            bad.append("%s never played on past the park (clock %d ms)" % (p, c))
    line, ok = u53.live_compare("host", "client1", logs)
    out.append(line)
    if not ok:
        bad.append(line)
    for ln in out:
        print("  " + ln)
    for ln in bad:
        print("  [FAIL] " + ln)
    print("check_u58_gs2: %s" % ("FAIL (%d)" % len(bad) if bad else "PASS"))
    return 1 if bad else 0


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    try:
        return check(argv[1])
    except u53.Refusal as exc:
        print("check_u58_gs2: REFUSED: %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
