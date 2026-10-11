#!/usr/bin/env python3
"""
tools/check_order_record.py -- mp:Q3: does mh.dll's own order recording equal the determinism
harness's recording of the same match?

The row (order_record_mp) runs one 2-peer UI-path match. The HOST arms the harness recorder
(`[harness] order_mode=1`), so its session folder holds the harness's per-match segment
mh_match_orders.bin + mh_match_clock.bin -- and mh.dll's recorder stays off there (one writer per
stream). The CLIENT runs at `[log] level=debug` with the harness hashing only, so ITS session folder
holds mh.dll's mh_rec_orders.bin + mh_rec_clock.bin (raw, or .gz once the close's compression ran).
Both files are on the same axis -- step 1 = the first sim step with the session folder open -- and a
lockstep match runs the same due queue on every peer at every step, so over the steps both recorded:

  * every step's order records are byte-identical (decoded through tools/mp_order_diff.py, the same
    reader the harness's own files use -- which is also the proof the new file is readable by the
    existing replay tooling), and at least MIN_ORDERS records were compared (a match with no orders
    proves nothing);
  * the clock tracks are identical.
The client's final step is excluded only when its harness stop closed the session pre-body (check()).

  python tools/check_order_record.py <host-session-dir> <client-session-dir>
  python tools/check_order_record.py --selftest
"""

import argparse
import gzip
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_order_diff as mod  # noqa: E402

MIN_ORDERS = 1


def _pick(d, name):
    for p in (os.path.join(d, name), os.path.join(d, name + ".gz")):
        if os.path.isfile(p):
            return p
    return None


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def _clock(path):
    blob = open(path, "rb").read()
    if path.endswith(".gz"):
        blob = gzip.decompress(blob)
    return [blob[i : i + 8] for i in range(0, len(blob) - len(blob) % 8, 8)]


def check(host_dir, client_dir):
    out = []
    ho, hc = _pick(host_dir, "mh_match_orders.bin"), _pick(host_dir, "mh_match_clock.bin")
    co, cc = _pick(client_dir, "mh_rec_orders.bin"), _pick(client_dir, "mh_rec_clock.bin")
    missing = [
        n
        for n, p in (
            ("host mh_match_orders.bin", ho),
            ("host mh_match_clock.bin", hc),
            ("client mh_rec_orders.bin", co),
            ("client mh_rec_clock.bin", cc),
        )
        if p is None
    ]
    if missing:
        return False, ["  check_order_record: FAIL -- missing: %s" % ", ".join(missing)]
    hclk, cclk = _clock(hc), _clock(cc)
    # mh.dll hands its batch over at the START of a step, so a client the runner KILLS (this row's
    # normal end) leaves files that end on a whole step. One shape does not: a session the harness stop
    # closes IN PLACE (`[session] HARNESS_STOP`, pre-body in the stop step) ends with that step's clock and
    # none of its orders -- the harness's post-stop latch records exactly that one body. Only then is the
    # client's final step left out.
    stopped = "[session] HARNESS_STOP" in _read(os.path.join(client_dir, "mh_net.log"))
    n = min(len(hclk), len(cclk) - (1 if stopped else 0))
    hs = mod.by_step(o for o in mod.load(ho) if o.step <= n)
    cs = mod.by_step(o for o in mod.load(co) if o.step <= n)
    diff = [
        s
        for s in sorted(set(hs) | set(cs))
        if [o.raw for o in hs.get(s, [])] != [o.raw for o in cs.get(s, [])]
    ]
    compared = sum(len(v) for v in hs.values())
    clk_diff = [i + 1 for i in range(n) if hclk[i] != cclk[i]]
    ok = n >= 1 and not diff and not clk_diff and compared >= MIN_ORDERS
    out.append(
        "  check_order_record: %s -- %d common step(s); orders: %d record(s) over %d step(s) compared, %d step(s) "
        "differ%s; clock: %d step(s) differ%s"
        % (
            "PASS" if ok else "FAIL",
            n,
            compared,
            len(hs),
            len(diff),
            (" (first %d)" % diff[0]) if diff else "",
            len(clk_diff),
            (" (first %d)" % clk_diff[0]) if clk_diff else "",
        )
    )
    out.append("      host   %s (%d clock steps)" % (ho, len(hclk)))
    out.append("      client %s (%d clock steps)" % (co, len(cclk)))
    if compared < MIN_ORDERS:
        out.append(
            "      FAIL: no order was compared -- the match produced none, so equality is vacuous"
        )
    return ok, out


def selftest():
    fails = 0
    tmp = tempfile.mkdtemp(prefix="chk_ordrec_")
    try:

        def rec(step, b):
            return struct.pack("<i", step) + bytes([b]) * 0x44

        def plant(name, files):
            d = os.path.join(tmp, name)
            os.makedirs(d)
            for f, (data, gz) in files.items():
                with open(os.path.join(d, f + (".gz" if gz else "")), "wb") as fh:
                    fh.write(gzip.compress(data) if gz else data)
            return d

        hdr = struct.pack("<II", 0x524F484D, 1)
        clk = b"".join(struct.pack("<d", 1.0 + i) for i in range(10))
        good_orders = hdr + rec(3, 1) + rec(4, 1) + rec(4, 2)
        host = plant(
            "h", {"mh_match_orders.bin": (good_orders, False), "mh_match_clock.bin": (clk, False)}
        )

        def client(name, orders=good_orders, clock=clk, gz=True):
            return plant(name, {"mh_rec_orders.bin": (orders, gz), "mh_rec_clock.bin": (clock, gz)})

        def stopped(d):
            with open(os.path.join(d, "mh_net.log"), "w") as fh:
                fh.write("; [session] HARNESS_STOP step=900 close=closed" + chr(10))
            return d

        cases = [
            ("identical (client gz) passes", host, client("c1"), True),
            ("identical (client raw) passes", host, client("c2", gz=False), True),
            (
                "a differing order fails",
                host,
                client("c3", orders=hdr + rec(3, 1) + rec(4, 9) + rec(4, 2)),
                False,
            ),
            (
                "a missing order fails",
                host,
                client("c4", orders=hdr + rec(3, 1) + rec(4, 1)),
                False,
            ),
            (
                "an order on the wrong step fails",
                host,
                client("c5", orders=hdr + rec(3, 1) + rec(5, 1) + rec(4, 2)),
                False,
            ),
            (
                "a clock difference fails",
                host,
                client("c6", clock=clk[:16] + struct.pack("<d", 99.0) + clk[24:]),
                False,
            ),
            (
                "no orders at all fails (vacuous)",
                plant(
                    "h0", {"mh_match_orders.bin": (hdr, False), "mh_match_clock.bin": (clk, False)}
                ),
                client("c7", orders=hdr),
                False,
            ),
            (
                "a missing client file fails",
                host,
                plant("c8", {"mh_rec_clock.bin": (clk, True)}),
                False,
            ),
            (
                "orders past the common steps are ignored",
                host,
                client("c9", orders=good_orders + rec(11, 7), clock=clk),
                True,
            ),
            (
                "a killed client's final step IS compared",
                host,
                client("c10", orders=hdr + rec(3, 1) + rec(4, 1), clock=clk[:32]),
                False,
            ),
            (
                "after a HARNESS_STOP the client's final step is not",
                host,
                stopped(
                    client("c11", orders=hdr + rec(3, 1) + rec(4, 1) + rec(4, 2), clock=clk[:32])
                ),
                True,
            ),
            (
                "...but the one before it is",
                host,
                stopped(client("c12", orders=hdr + rec(3, 1), clock=clk[:40])),
                False,
            ),
        ]
        for name, h, c, want in cases:
            ok, _ = check(h, c)
            good = ok == want
            fails += 0 if good else 1
            print("  %s %s" % ("ok  " if good else "BAD ", name))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("check_order_record selftest: %s" % ("PASS" if fails == 0 else "%d FAIL" % fails))
    return 0 if fails == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if len(a.dirs) != 2:
        ap.error("two session directories: host, client")
    ok, lines = check(a.dirs[0], a.dirs[1])
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
