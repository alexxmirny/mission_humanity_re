#!/usr/bin/env python3
"""udp_rtt_probe -- what the wire actually delivers, measured by something that is not the game.

WHY THIS EXISTS. The transport publishes a per-peer SRTT (tracker mp:T3), and a rig run through
net_shim read it ~19 ms BELOW the delay the shim was configured to inject. Three things could be
lying: the shim (it does not inject what it says), the estimator (the RFC 6298 arithmetic), or the
probe path inside the DLL (where the stopwatch starts and stops). The first two were cleared
separately -- the shim against a LOOPBACK echo on the dev box, the estimator against synthetic
samples in `net_selftest.exe udpstatstest` -- and that is exactly the hole this closes: a loopback
measurement and a two-VM game run are not the same path, so "the shim is fine" and "the DLL is
wrong" were never compared on ONE path.

So: an echo server and a prober that speak plain UDP, are deployed on the SAME two rig peers as the
game, and are pointed THROUGH THE SAME LIVE SHIM. Their round trip is the delivered RTT of the
game's path, measured by code that shares nothing with the transport but the network.

    peer B  --(probe)-->  net_shim (dev box)  -->  peer A (echo)      # the game's client path

Three modes:

    echo      bind a UDP socket and reflect every datagram, stamping its own turnaround into the
              reply. Run on the peer that plays the game HOST slot.
    probe     send N datagrams at a fixed interval, match the replies by sequence, and report the
              round-trip distribution -- plus the RFC 6298 SRTT over the same samples, with the
              same alpha the DLL uses, so the number printed here and the DLL's `srtt0_ms` column
              are the same statistic and can be subtracted.
    selftest  prove the probe itself measures nothing when there is nothing to measure: a loopback
              echo must read under a millisecond, and a sleep injected in the echo must show up.

WHAT THE REPLY CARRIES, AND WHY. The echo writes its OWN turnaround (receive -> send, in
microseconds) into the datagram it reflects. The prober subtracts it and reports both numbers. That
is the difference between "the path costs 83 ms" and "the path costs 83 ms of which the responder
owns 0.1" -- and the game's own echo turnaround is one of the candidates for the missing 19 ms, so
the instrument that is supposed to settle the question must not have the same blind spot.

CLOCKS. `time.perf_counter_ns()` at both stamps, same process, so there is no clock-sync problem:
the prober times its own send and its own receive. The echo's turnaround is likewise measured
entirely inside the echo process. Nothing here compares two machines' clocks, which is the same
discipline the transport's own stopwatch follows (a high-resolution local stamp, with the wire field
used only as a match key).

Usage (the rig path, from the dev box):

    # on peer A (the game's host slot)
    python udp_rtt_probe.py echo --bind 0.0.0.0:6502

    # on the dev box: the same middlebox the game runs through
    python tools/net_shim.py --udp --listen 0.0.0.0:6502 --target <peerA>:6502 --delay 40

    # on peer B (the game's client slot)
    python udp_rtt_probe.py probe --target <devbox>:6502 --count 40 --interval 1000

`--json <path>` writes the summary as a machine-readable record, which is how a run's number gets
into a report without being retyped.
"""

import argparse
import json
import os
import socket
import struct
import sys
import time

MAGIC = b"MHRT"
# magic, seq, prober stamp (echoed verbatim), echo turnaround in microseconds
HDR = struct.Struct("<4sIQQ")
HDR_LEN = HDR.size  # 24

# RFC 6298 section 2, the same constants udp_stats.h uses. Written as fractions so a reader can
# check them against the RFC rather than against this file.
RFC6298_ALPHA = 1.0 / 8.0
RFC6298_BETA = 1.0 / 4.0


def parse_hostport(s, default_port):
    if ":" in s:
        host, port = s.rsplit(":", 1)
        return host, int(port)
    return s, default_port


def _raise_timer_resolution():
    """Windows only: ask for a 1 ms scheduler quantum.

    The prober sleeps between probes and the echo does not sleep at all, so this matters far less
    here than it does in net_shim -- but a 15.6 ms sleep quantum on the SENDING side would pace the
    probes in bursts, and a burst is a different offered load than the game's steady 1 Hz. Best
    effort: a platform that refuses simply keeps its default.
    """
    if sys.platform != "win32":
        return None
    try:
        import ctypes

        ctypes.windll.winmm.timeBeginPeriod(1)
        return ctypes.windll.winmm
    except (AttributeError, OSError):
        return None


# ---- echo ----------------------------------------------------------------------------------------
def do_echo(args):
    host, port = parse_hostport(args.bind, 6502)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.settimeout(0.5)
    print("udp_rtt_probe: echo on %s:%d (ctrl-c to stop)" % (host, port), flush=True)
    n = 0
    deadline = time.monotonic() + args.seconds if args.seconds else None
    while True:
        if deadline is not None and time.monotonic() > deadline:
            break
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            continue
        t_in = time.perf_counter_ns()
        if len(data) < HDR_LEN or data[:4] != MAGIC:
            continue  # not ours; a shim's control traffic or a stray game datagram
        _, seq, stamp, _ = HDR.unpack_from(data, 0)
        # The selftest's second arm: a responder that is deliberately slow, so the instrument is
        # shown to REPORT a delay that is really there rather than always printing something small.
        if args.delay_ms:
            time.sleep(args.delay_ms / 1000.0)
        turn_us = (time.perf_counter_ns() - t_in) // 1000
        out = bytearray(data)
        HDR.pack_into(out, 0, MAGIC, seq, stamp, turn_us)
        try:
            s.sendto(bytes(out), addr)
        except OSError:
            pass
        n += 1
        if args.verbose and n % 100 == 0:
            print("udp_rtt_probe: echoed %d" % n, flush=True)
    print("udp_rtt_probe: echo stopped after %d datagram(s)" % n, flush=True)
    s.close()
    return 0


# ---- probe ---------------------------------------------------------------------------------------
def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    i = int((p / 100.0) * (len(sorted_vals) - 1) + 0.5)
    i = max(0, min(len(sorted_vals) - 1, i))
    return sorted_vals[i]


def srtt_over(samples):
    """RFC 6298 SRTT/RTTVAR over a sample sequence -- the DLL's arithmetic, on the probe's samples.

    Reported so the comparison is like for like. `srtt0_ms` in mh_lockstep.log is a SMOOTHED number
    with an eight-sample time constant; comparing it against a probe's raw MEAN silently charges the
    estimator for whatever the link did before the window, which is how a converging trace reads as
    a constant offset.
    """
    srtt = rttvar = 0.0
    for i, r in enumerate(samples):
        if i == 0:
            srtt, rttvar = r, r / 2.0
        else:
            d = abs(srtt - r)
            rttvar = (1.0 - RFC6298_BETA) * rttvar + RFC6298_BETA * d
            srtt = (1.0 - RFC6298_ALPHA) * srtt + RFC6298_ALPHA * r
    return srtt, rttvar


def do_probe(args):
    host, port = parse_hostport(args.target, 6502)
    dst = (host, port)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.001)
    size = max(HDR_LEN, args.size)
    pad = bytes(size - HDR_LEN)

    sent = {}  # seq -> perf_counter_ns at send
    rtts = []  # (seq, rtt_ms, turnaround_ms)
    interval = args.interval / 1000.0
    timeout = args.timeout / 1000.0

    print(
        "udp_rtt_probe: probing %s:%d  count=%d interval=%dms size=%dB"
        % (host, port, args.count, args.interval, size),
        flush=True,
    )

    def drain(until):
        """Read every reply that has arrived, up to a wall-clock deadline."""
        while True:
            left = until - time.perf_counter()
            if left <= 0:
                return
            s.settimeout(max(0.0005, min(left, 0.05)))
            try:
                data, _ = s.recvfrom(4096)
            except (socket.timeout, OSError):
                continue
            now = time.perf_counter_ns()
            if len(data) < HDR_LEN or data[:4] != MAGIC:
                continue
            _, seq, _stamp, turn_us = HDR.unpack_from(data, 0)
            t0 = sent.pop(seq, None)
            if t0 is None:
                continue  # a duplicate, or a reply that arrived after we gave up on it
            rtts.append((seq, (now - t0) / 1e6, turn_us / 1000.0))
            if args.each:
                print("  seq %4d  rtt %8.3f ms  echo turnaround %6.3f ms" % rtts[-1], flush=True)

    t_start = time.perf_counter()
    for seq in range(args.count):
        payload = bytearray(HDR.pack(MAGIC, seq, 0, 0)) + pad
        sent[seq] = time.perf_counter_ns()
        try:
            s.sendto(bytes(payload), dst)
        except OSError as e:
            print("udp_rtt_probe: sendto failed (%s)" % e, flush=True)
            sent.pop(seq, None)
        drain(t_start + (seq + 1) * interval)
    drain(time.perf_counter() + timeout)  # the last probes' replies
    s.close()

    vals = sorted(r for _, r, _ in rtts)
    turns = sorted(t for _, _, t in rtts)
    lost = args.count - len(rtts)
    if not vals:
        print("udp_rtt_probe: NO REPLIES -- the echo is not running, or nothing routes there")
        return 2
    srtt, rttvar = srtt_over([r for _, r, _ in sorted(rtts)])
    mean = sum(vals) / len(vals)
    out = {
        "target": "%s:%d" % (host, port),
        "count": args.count,
        "replies": len(rtts),
        "lost": lost,
        "interval_ms": args.interval,
        "size_b": size,
        "rtt_min_ms": round(vals[0], 3),
        "rtt_p50_ms": round(pct(vals, 50), 3),
        "rtt_mean_ms": round(mean, 3),
        "rtt_p95_ms": round(pct(vals, 95), 3),
        "rtt_max_ms": round(vals[-1], 3),
        "srtt_ms": round(srtt, 3),
        "rttvar_ms": round(rttvar, 3),
        "echo_turnaround_p50_ms": round(pct(turns, 50), 3),
        "echo_turnaround_max_ms": round(turns[-1], 3),
        "label": args.label or "",
    }
    print(
        "udp_rtt_probe: replies %d/%d  rtt min %.2f p50 %.2f mean %.2f p95 %.2f max %.2f ms  "
        "srtt(rfc6298) %.2f rttvar %.2f  echo turnaround p50 %.3f max %.3f ms"
        % (
            out["replies"],
            out["count"],
            out["rtt_min_ms"],
            out["rtt_p50_ms"],
            out["rtt_mean_ms"],
            out["rtt_p95_ms"],
            out["rtt_max_ms"],
            out["srtt_ms"],
            out["rttvar_ms"],
            out["echo_turnaround_p50_ms"],
            out["echo_turnaround_max_ms"],
        ),
        flush=True,
    )
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2)
        print("udp_rtt_probe: wrote %s" % args.json, flush=True)
    return 0


# ---- selftest ------------------------------------------------------------------------------------
def do_selftest():
    """Prove the instrument reads zero on a zero path and sees a delay that is really there.

    Two arms, and the second is the one that matters: an instrument that always prints a plausible
    small number would have "cleared" the shim exactly as convincingly as a correct one.
    """
    import subprocess
    import threading

    port = 39720
    fails = []
    me = os.path.abspath(__file__)

    def run_echo(delay_ms, seconds):
        return subprocess.Popen(
            [
                sys.executable,
                me,
                "echo",
                "--bind",
                "127.0.0.1:%d" % port,
                "--delay-ms",
                str(delay_ms),
                "--seconds",
                str(seconds),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )

    def probe(count, interval):
        r = subprocess.run(
            [
                sys.executable,
                me,
                "probe",
                "--target",
                "127.0.0.1:%d" % port,
                "--count",
                str(count),
                "--interval",
                str(interval),
                "--json",
                os.path.join(os.path.dirname(me) or ".", "_udp_rtt_probe_selftest.json"),
            ],
            capture_output=True,
            text=True,
        )
        path = os.path.join(os.path.dirname(me) or ".", "_udp_rtt_probe_selftest.json")
        try:
            with open(path, encoding="utf-8") as fh:
                d = json.load(fh)
        except OSError:
            d = None
        finally:
            try:
                os.remove(path)
            except OSError:
                pass
        return r, d

    p = run_echo(0, 8)
    time.sleep(1.0)
    try:
        _, d = probe(40, 10)
        if d is None:
            fails.append("[1] no summary -- the loopback echo did not answer")
        else:
            print("[1] loopback, no delay      p50 %.3f ms  (expect < 1)" % d["rtt_p50_ms"])
            if d["rtt_p50_ms"] > 1.0:
                fails.append(
                    "[1] loopback p50 %.3f ms -- the probe itself is too slow to trust"
                    % d["rtt_p50_ms"]
                )
            if d["lost"] > 2:
                fails.append("[1] lost %d/40 on loopback" % d["lost"])
    finally:
        p.terminate()

    p = run_echo(25, 8)
    time.sleep(1.0)
    try:
        _, d = probe(20, 40)
        if d is None:
            fails.append("[2] no summary -- the slow echo did not answer")
        else:
            print(
                "[2] echo sleeping 25 ms     p50 %.3f ms, turnaround p50 %.3f ms  (expect ~25 both)"
                % (d["rtt_p50_ms"], d["echo_turnaround_p50_ms"])
            )
            if not 20.0 <= d["rtt_p50_ms"] <= 45.0:
                fails.append("[2] injected 25 ms read as %.3f ms" % d["rtt_p50_ms"])
            if not 20.0 <= d["echo_turnaround_p50_ms"] <= 45.0:
                fails.append(
                    "[2] the responder's own 25 ms did not show in the turnaround field (%.3f ms)"
                    % d["echo_turnaround_p50_ms"]
                )
    finally:
        p.terminate()

    print(
        "\n=== %s ==="
        % ("PASS: the probe measures what is there" if not fails else "FAIL: " + "; ".join(fails))
    )
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description="UDP echo + round-trip prober: the delivered RTT of the rig path, measured "
        "by something that is not the game.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = ap.add_subparsers(dest="mode", required=True)

    e = sub.add_parser("echo", help="reflect datagrams (run on the peer in the host slot)")
    e.add_argument("--bind", default="0.0.0.0:6502")
    e.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = forever)")
    e.add_argument("--delay-ms", type=float, default=0, help="selftest arm: answer this slowly")
    e.add_argument("--verbose", action="store_true")

    p = sub.add_parser("probe", help="measure the round trip (run on the peer in the client slot)")
    p.add_argument(
        "--target", required=True, help="the SHIM's listen address, to walk the game's path"
    )
    p.add_argument("--count", type=int, default=40)
    p.add_argument(
        "--interval",
        type=int,
        default=1000,
        help="ms between probes (1000 = the game's ping cadence)",
    )
    p.add_argument("--timeout", type=int, default=2000, help="ms to wait for the last replies")
    p.add_argument(
        "--size", type=int, default=80, help="datagram bytes (the game's ping packet is ~80)"
    )
    p.add_argument("--each", action="store_true", help="print every sample")
    p.add_argument("--json", default="", help="write the summary record here")
    p.add_argument("--label", default="", help="free-text tag carried into the JSON record")

    sub.add_parser("selftest", help="prove the probe measures what is there")

    args = ap.parse_args()
    winmm = _raise_timer_resolution()
    try:
        if args.mode == "echo":
            return do_echo(args)
        if args.mode == "probe":
            return do_probe(args)
        return do_selftest()
    finally:
        if winmm is not None:
            try:
                winmm.timeEndPeriod(1)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
