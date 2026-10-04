#!/usr/bin/env python3
"""check_u61_mesh.py -- mp:U61 (HM-M3): did the host-migration MESH come up in a real 3-peer match?

Reads the three pulled peer dirs of a `det_arms.py --determinism --u61-mesh[-relay]` run
(<det_dir>/{host,client1,client2}/mh_net.log) and asserts, per the host-migration plan (mp:U57) sections 3-5:

  1. EVERY PEER LOGGED THE SAME FIRST EPOCH. The host's `net: mesh epoch=E hub=H tier=T rank=R digest=D ...
     role=hub` line and each client's `role=client` line carry the same (epoch, hub, tier, rank, digest) --
     "every peer logs the same epoch and ranking". The host logged it exactly once as `role=hub`, and the
     two clients as `role=client`.
  2. WITHIN 5 s. The host's first epoch is at most 5 s after its last client was admitted (`net: udp accepted
     ... (player N)`), and each client's is at most 5 s after the hub's BROKER reached it. (Timestamps are
     each peer's OWN log clock; no cross-machine subtraction is made.)
  3. THE MATRIX COVERS EVERY PAIR: cov=have/total with have == total and total == 1 for three peers.
  4. THE ACK ROUND COMPLETED: the host logged `acked by all 2 client(s)` for epoch E.
  5. NO DISAGREEMENT: no peer logged `RANKING MISMATCH`, and every `net: mesh counters` line reads bad probe 0
     frame 0.
  6. DIRECT ARM (tier 1): each client logged a `first echo` for the other client within 5 s of brokering and
     sent >= 1 probe; the hub's epoch carries no relay room (rooms=0).
     RELAY ARM (--relay; tier 2): the matrix line shows relay-estimated pairs (`r`) and a leg for BOTH
     clients, the epoch carries a pre-minted room per candidate (rooms=2), and NO client sent a direct probe
     (a relayed client's socket is a loopback tunnel).

Absence of a log or of a mesh line is a REFUSAL (exit 2), never a pass.

  python tools/check_u61_mesh.py <det_dir> [--relay]
"""

import argparse
import os
import re
import sys

PEERS = ("host", "client1", "client2")
TS = r"\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]"
EPOCH_RE = re.compile(
    TS + r" net: mesh epoch=(\d+) hub=(\d+) tier=(\d+) rank=([0-9>]+) digest=([0-9a-f]{8}) "
    r"cov=(\d+)/(\d+) rooms=(\d+) role=(hub|client)"
)
MATRIX_RE = re.compile(
    TS + r" net: mesh epoch=(\d+) matrix \(dms, r = relay-estimated\):(.*?) \| legs(.*?)[ \t\r]*$",
    re.M,
)
ACKED_RE = re.compile(TS + r" net: mesh epoch=(\d+) acked by all (\d+) client\(s\) after (\d+) ms")
ECHO_RE = re.compile(
    TS + r" net: mesh pair (\d+)-(\d+) first echo (\d+) ms after brokering, rtt (\d+) dms"
)
BROKER_RX_RE = re.compile(TS + r" net: mesh BROKER from the hub")
ACCEPT_RE = re.compile(TS + r" net: udp accepted .* -> conn \d+ \(player (\d+)\)")
COUNTERS_RE = re.compile(
    r"net: mesh counters probes tx (\d+) rx (\d+) \| echoes tx (\d+) rx (\d+) \| probe bytes tx (\d+) "
    r"rx (\d+) \| frames tx (\d+) rx (\d+) \(bytes tx (\d+) rx (\d+)\) \| bad probe (\d+) frame (\d+)"
)
FIVE_S = 5000


class Refusal(Exception):
    pass


def read(path):
    if not os.path.isfile(path):
        raise Refusal("missing %s" % path)
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def ms(m, base=1):
    h, mi, s, f = (int(m.group(base + i)) for i in range(4))
    return ((h * 60 + mi) * 60 + s) * 1000 + f


def check(det_dir, relay):
    logs = {p: read(os.path.join(det_dir, p, "mh_net.log")) for p in PEERS}
    bad, out = [], []

    epochs = {}
    for p in PEERS:
        rows = [m for m in EPOCH_RE.finditer(logs[p])]
        if not rows:
            raise Refusal(
                "%s: no `net: mesh epoch=` line at all -- the mesh never published/held an epoch"
                % p
            )
        epochs[p] = rows
    first = {}
    for p in PEERS:
        m = epochs[p][0]
        first[p] = (
            int(m.group(5)),
            int(m.group(6)),
            int(m.group(7)),
            m.group(8),
            m.group(9),
            int(m.group(10)),
            int(m.group(11)),
            int(m.group(12)),
            m.group(13),
        )
        out.append(
            "%-8s first epoch=%d hub=%d tier=%d rank=%s digest=%s cov=%d/%d rooms=%d role=%s at %s"
            % (
                (p,)
                + first[p]
                + ("%02d:%02d:%02d.%03d" % tuple(int(m.group(i)) for i in range(1, 5)),)
            )
        )
    key = {p: first[p][:5] for p in PEERS}
    if len({k for k in key.values()}) != 1:
        bad.append(
            "the three peers' first epochs DIFFER (epoch, hub, tier, rank, digest): %s" % key
        )
    else:
        out.append(
            "same epoch, hub, tier, ranking and digest on all three peers: %s" % (key["host"],)
        )
    if first["host"][8] != "hub" or any(first[p][8] != "client" for p in PEERS[1:]):
        bad.append("roles wrong: %s" % {p: first[p][8] for p in PEERS})
    # every LATER epoch must also agree across peers, epoch by epoch
    seen = {p: {int(m.group(5)): m.group(5, 6, 7, 8, 9) for m in epochs[p]} for p in PEERS}
    for e in sorted(set().union(*[set(v) for v in seen.values()])):
        got = {p: seen[p].get(e) for p in PEERS}
        if len(set(got.values())) != 1:
            bad.append("epoch %d is not held identically by every peer: %s" % (e, got))
    if len(first["host"]) and first["host"][0] < 1:
        bad.append("epoch numbering did not start at 1")

    # 2. within 5 s
    acc = [ms(m) for m in ACCEPT_RE.finditer(logs["host"])]
    h_first = ms(epochs["host"][0])
    if not acc:
        bad.append("host: no `udp accepted` line to measure from")
    else:
        d = h_first - max(acc)
        out.append("host: first epoch published %d ms after the last client was admitted" % d)
        if d > FIVE_S:
            bad.append("host: first epoch took %d ms after the last admission (> 5 s)" % d)
    for p in PEERS[1:]:
        br = BROKER_RX_RE.search(logs[p])
        if not br:
            bad.append("%s: never received the hub's BROKER" % p)
            continue
        d = ms(epochs[p][0]) - ms(br)
        out.append("%s: first epoch held %d ms after the BROKER arrived" % (p, d))
        if d > FIVE_S:
            bad.append("%s: first epoch %d ms after the BROKER (> 5 s)" % (p, d))

    # 3. coverage
    have, total = first["host"][5], first["host"][6]
    if total != 1 or have != total:
        bad.append(
            "matrix coverage in the first epoch is %d/%d (want 1/1 for three peers)" % (have, total)
        )

    # 4. ack round
    acked = {int(m.group(5)) for m in ACKED_RE.finditer(logs["host"])}
    if first["host"][0] not in acked:
        bad.append("host never logged `acked by all` for epoch %d" % first["host"][0])
    else:
        a = [m for m in ACKED_RE.finditer(logs["host"]) if int(m.group(5)) == first["host"][0]][0]
        if int(a.group(6)) != 2:
            bad.append("the ack round covered %s clients, not 2" % a.group(6))
        out.append(
            "host: epoch %d acked by all %s client(s) after %s ms"
            % (first["host"][0], a.group(6), a.group(7))
        )

    # 5. no disagreement
    for p in PEERS:
        if "RANKING MISMATCH" in logs[p]:
            bad.append("%s logged a RANKING MISMATCH" % p)
        for c in COUNTERS_RE.finditer(logs[p]):
            if int(c.group(11)) != 0 or int(c.group(12)) != 0:
                bad.append("%s: bad probe/frame counters non-zero: %s" % (p, c.group(0)))
    totals = {}
    for p in PEERS:
        rows = [tuple(int(x) for x in c.groups()) for c in COUNTERS_RE.finditer(logs[p])]
        totals[p] = rows[-1] if rows else None
        if rows:
            r = rows[-1]
            out.append(
                "%-8s last mesh counters: probes tx %d rx %d, echoes tx %d rx %d, probe bytes tx %d rx %d, "
                "frames tx %d rx %d (bytes tx %d rx %d)" % ((p,) + r[:10])
            )

    # 6. the arm-specific clauses
    if not relay:
        if first["host"][2] != 1:
            bad.append("direct arm: tier %d, expected 1 (measured direct edges)" % first["host"][2])
        if first["host"][7] != 0:
            bad.append(
                "direct arm: the epoch carries %d relay room(s); a direct match has none"
                % first["host"][7]
            )
        for p, other in (("client1", None), ("client2", None)):
            echoes = [
                (int(m.group(5)), int(m.group(6)), int(m.group(7)))
                for m in ECHO_RE.finditer(logs[p])
            ]
            if not echoes:
                bad.append("%s: no `first echo` line -- its probes were never answered" % p)
                continue
            for a, b, after in echoes:
                out.append("%s: pair %d-%d first echo %d ms after brokering" % (p, a, b, after))
                if after > FIVE_S:
                    bad.append("%s: first echo of pair %d-%d took %d ms (> 5 s)" % (p, a, b, after))
            t = totals[p]
            if t is None or t[0] + t[2] < 1:
                bad.append("%s: sent no probe or echo" % p)
    else:
        if first["host"][2] != 2:
            bad.append("relay arm: tier %d, expected 2 (the relay-leg tier)" % first["host"][2])
        if first["host"][7] != 2:
            bad.append(
                "relay arm: the epoch carries %d pre-minted room(s), expected 2" % first["host"][7]
            )
        mx = [m for m in MATRIX_RE.finditer(logs["host"]) if int(m.group(5)) == first["host"][0]]
        if not mx:
            bad.append("relay arm: no matrix line for epoch %d" % first["host"][0])
        else:
            pairs, legs = mx[0].group(6), mx[0].group(7).split()
            out.append("host: epoch matrix %s | legs %s" % (pairs.strip(), " ".join(legs)))
            if not re.search(r"\d+r\b", pairs):
                bad.append("relay arm: the matrix has no relay-estimated pair (`r`): %r" % pairs)
            if len(legs) != 2:
                bad.append("relay arm: expected a relay leg for both clients, got %r" % legs)
        for p in PEERS[1:]:
            t = totals[p]
            if t is not None and (t[0] != 0 or t[2] != 0):
                bad.append("%s: a relayed client sent a direct probe/echo (%s)" % (p, t[:4]))
    for ln in out:
        print("  " + ln)
    for ln in bad:
        print("  [FAIL] " + ln)
    print(
        "check_u61_mesh (%s): %s"
        % ("relay" if relay else "direct", "FAIL (%d)" % len(bad) if bad else "PASS")
    )
    return 1 if bad else 0


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("det_dir")
    ap.add_argument("--relay", action="store_true")
    a = ap.parse_args(argv[1:])
    try:
        return check(a.det_dir, a.relay)
    except Refusal as exc:
        print("check_u61_mesh: REFUSED: %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
