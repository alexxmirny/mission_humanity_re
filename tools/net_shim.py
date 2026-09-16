#!/usr/bin/env python3
"""net_shim -- a controllable TCP middlebox for the 2-VM rig.

The rig is a sub-millisecond LAN. Everything that actually goes wrong in multiplayer goes wrong at
internet conditions: the 2026-07-26 session that produced R-live ran at ~200 ms through a VPS tunnel,
the de-sync "icon storm" has only ever been seen there, and P1's last acceptance test ("responds to a
mid-run injected-latency change") has never been run at all because nothing could inject one. This is
that missing piece: a pass-through the peers connect through, whose behaviour can be changed WHILE a
game is running.

    client  --->  net_shim (this box)  --->  host

Because the shim terminates TCP on both sides, it is honest about what it can and cannot model:

  CAN     one-way delay, jitter, a bandwidth ceiling, a mid-run change to any of them, and the four
          ways a link dies (see `The failure shapes` below).
  CANNOT  packet loss or reordering. Deleting bytes from a reliable stream is not loss, it is
          CORRUPTION -- the peer would fail its record MAC, which is a different bug entirely. Real
          loss reaches an application as extra delay (the retransmit), which `delay`/`jitter` already
          model. If you ever want the corruption path itself, add an explicit `corrupt` command and
          call it that.

This is a diagnostic tool, not part of the game: nothing in `src/` knows it exists, and a run through
the shim is byte-identical to a direct run apart from timing.

The failure shapes, and the R-live drop reason each one must produce on the far peer:

    cut           graceful FIN        -> "peer closed the connection"
    cut rst       RST (SO_LINGER 0)   -> "socket error (winsock 10054)"
    blackhole on  read + discard      -> "no data from peer within the link timeout"
    stall on      stop reading        -> nothing, until it outlasts rx_timeout_ms

`stall` under the timeout is the NEGATIVE control: a link that goes quiet but is alive must NOT be
dropped, and that is the half of the watchdog a positive test can never prove.

Usage
-----
Run the middlebox (typically on the dev box, with both game peers on VMs, so the game port stays
6501 everywhere and no peer ini changes):

    python tools/net_shim.py --listen 0.0.0.0:6501 --target <host>:6501 --delay 100

then launch the rig with the CLIENT pointed at this box instead of the host:

    python tools/ui_test.py --host <peerA>:mp_host_start.txt \
                            --client <peerB>:mp_client_start.txt \
                            --connect-ip <this box's LAN ip>

`--delay` is ONE WAY: `--delay 100` is a 200 ms round trip, which is roughly the session that
prompted all of this.

Drive it live from another shell (or from a test):

    python tools/net_shim.py ctl "set delay 250"
    python tools/net_shim.py ctl "blackhole on"
    python tools/net_shim.py ctl status

or make the run reproducible with a timeline instead (ms from the first connection):

    python tools/net_shim.py --listen ... --target ... --timeline tools/uiscripts/shim/p1_latency.txt
"""

import argparse
import asyncio
import ctypes
import os
import random
import socket
import sys
import time

DEFAULT_CONTROL_PORT = 6699


def _now():
    return time.monotonic()


class Params:
    """Live link parameters. Mutated by the control channel; read by every pump on every chunk.

    No locking: asyncio is single-threaded, and a pump only ever reads these between awaits.
    """

    def __init__(self, delay_ms=0.0, jitter_ms=0.0, rate_kbps=0):
        self.delay_ms = float(delay_ms)
        self.jitter_ms = float(jitter_ms)
        self.rate_kbps = int(rate_kbps)
        self.blackhole = set()  # subset of {"c2s", "s2c"}
        self.stall = set()  # subset of {"c2s", "s2c"}
        self.seed = 12345

    def describe(self):
        return "delay=%.1fms (one way; %.1fms rtt) jitter=%.1fms rate=%s blackhole=%s stall=%s" % (
            self.delay_ms,
            self.delay_ms * 2,
            self.jitter_ms,
            ("%dkbps" % self.rate_kbps) if self.rate_kbps else "unlimited",
            ",".join(sorted(self.blackhole)) or "-",
            ",".join(sorted(self.stall)) or "-",
        )


class Stats:
    def __init__(self):
        self.conns = 0
        self.live = 0
        self.bytes = {"c2s": 0, "s2c": 0}
        self.dropped = {"c2s": 0, "s2c": 0}


class Shim:
    def __init__(self, params, stats, log):
        self.p = params
        self.s = stats
        self.log = log
        self.conns = []  # live [(client_writer, server_writer)] for `cut`
        self.rng = random.Random(params.seed)
        self.t0 = None

    # ---- logging ---------------------------------------------------------------------------
    def say(self, msg):
        line = "[%8.3f] %s" % (_now() - (self.t0 or _now()), msg)
        print(line, flush=True)
        if self.log:
            with open(self.log, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")

    # ---- one direction of one connection ----------------------------------------------------
    async def pump(self, reader, writer, tag):
        """Forward reader->writer, applying the current link parameters.

        Release times are forced MONOTONIC (`max(last, ...)`). Without that, jitter would let chunk
        N+1 come due before chunk N and the proxy would reorder a TCP stream -- which the real
        network cannot do, and which downstream would look like corruption rather than jitter.
        """
        last_release = 0.0
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break

                while tag in self.p.stall:  # backpressure: we simply stop draining
                    await asyncio.sleep(0.05)

                if tag in self.p.blackhole:  # read it, forget it -- the socket stays open
                    self.s.dropped[tag] += len(data)
                    continue

                delay = self.p.delay_ms
                if self.p.jitter_ms:
                    delay += self.rng.uniform(-self.p.jitter_ms, self.p.jitter_ms)
                if delay < 0:
                    delay = 0.0
                serialize = (
                    (len(data) * 8.0 / (self.p.rate_kbps * 1000.0)) if self.p.rate_kbps else 0.0
                )

                release = max(last_release, _now() + delay / 1000.0) + serialize
                last_release = release
                sleep_for = release - _now()
                if sleep_for > 0:
                    await asyncio.sleep(sleep_for)

                writer.write(data)
                await writer.drain()
                self.s.bytes[tag] += len(data)
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            pass
        except asyncio.CancelledError:
            raise
        finally:
            try:
                writer.close()
            except OSError:
                pass

    # ---- one accepted connection ------------------------------------------------------------
    async def handle(self, creader, cwriter, target):
        if self.t0 is None:
            self.t0 = _now()
        peer = cwriter.get_extra_info("peername")
        self.s.conns += 1
        n = self.s.conns
        try:
            sreader, swriter = await asyncio.open_connection(target[0], target[1])
        except OSError as e:
            self.say(
                "conn %d from %s -- target %s:%d unreachable (%s)"
                % (n, peer, target[0], target[1], e)
            )
            cwriter.close()
            return
        for w in (cwriter, swriter):
            try:
                w.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass

        self.conns.append((cwriter, swriter))
        self.s.live += 1
        self.say(
            "conn %d OPEN  %s -> %s:%d   (%s)" % (n, peer, target[0], target[1], self.p.describe())
        )
        try:
            await asyncio.gather(
                self.pump(creader, swriter, "c2s"),
                self.pump(sreader, cwriter, "s2c"),
            )
        finally:
            self.s.live -= 1
            if (cwriter, swriter) in self.conns:
                self.conns.remove((cwriter, swriter))
            self.say(
                "conn %d CLOSE (c2s=%dB s2c=%dB dropped c2s=%dB s2c=%dB)"
                % (
                    n,
                    self.s.bytes["c2s"],
                    self.s.bytes["s2c"],
                    self.s.dropped["c2s"],
                    self.s.dropped["s2c"],
                )
            )

    # ---- cut every live connection ----------------------------------------------------------
    def cut(self, rst):
        k = 0
        for cwriter, swriter in list(self.conns):
            for w in (cwriter, swriter):
                sock = w.get_extra_info("socket")
                if rst and sock is not None:
                    try:  # SO_LINGER with a zero timeout makes close() send RST, not FIN
                        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, LINGER_ZERO)
                    except OSError:
                        pass
                try:
                    w.close()
                except OSError:
                    pass
            k += 1
        return k


LINGER_ZERO = None  # built in main() once struct packing is known


# ---- control channel -------------------------------------------------------------------------
HELP = """commands:
  set delay <ms>        one-way delay (rtt is twice this)
  set jitter <ms>       +/- uniform, never reordering
  set rate <kbps>       bandwidth ceiling (0 = unlimited)
  blackhole on|off|c2s|s2c    read and discard; sockets stay OPEN (-> rx watchdog)
  stall on|off          stop draining; sender feels backpressure (alive, just quiet)
  cut [rst]             close live connections (FIN, or RST with 'rst')
  status                current parameters + counters
"""


def apply_command(shim, line):
    t = line.split()
    if not t:
        return "ok"
    c = t[0].lower()
    p = shim.p
    try:
        if c == "set" and len(t) == 3:
            k, v = t[1].lower(), t[2]
            if k == "delay":
                p.delay_ms = float(v)
            elif k == "jitter":
                p.jitter_ms = float(v)
            elif k == "rate":
                p.rate_kbps = int(v)
            else:
                return "err: unknown parameter %r" % k
            shim.say("SET %s" % p.describe())
            return "ok " + p.describe()
        if c == "blackhole" and len(t) == 2:
            a = t[1].lower()
            p.blackhole = set() if a == "off" else ({"c2s", "s2c"} if a == "on" else {a})
            shim.say("BLACKHOLE %s" % (",".join(sorted(p.blackhole)) or "off"))
            return "ok " + p.describe()
        if c == "stall" and len(t) == 2:
            a = t[1].lower()
            p.stall = set() if a == "off" else ({"c2s", "s2c"} if a == "on" else {a})
            shim.say("STALL %s" % (",".join(sorted(p.stall)) or "off"))
            return "ok " + p.describe()
        if c == "cut":
            rst = len(t) > 1 and t[1].lower() == "rst"
            k = shim.cut(rst)
            shim.say("CUT%s -- %d connection(s)" % (" rst" if rst else "", k))
            return "ok cut %d" % k
        if c == "status":
            return "ok %s | conns=%d live=%d c2s=%dB s2c=%dB dropped=%dB/%dB" % (
                p.describe(),
                shim.s.conns,
                shim.s.live,
                shim.s.bytes["c2s"],
                shim.s.bytes["s2c"],
                shim.s.dropped["c2s"],
                shim.s.dropped["s2c"],
            )
        if c in ("help", "?"):
            return HELP
    except ValueError as e:
        return "err: %s" % e
    return "err: bad command (try 'help')"


async def control_server(shim, port):
    async def handle(reader, writer):
        try:
            data = await reader.readline()
            reply = apply_command(shim, data.decode("utf-8", "replace").strip())
            writer.write((reply + "\n").encode("utf-8"))
            await writer.drain()
        finally:
            writer.close()

    srv = await asyncio.start_server(handle, "127.0.0.1", port)
    shim.say("control on 127.0.0.1:%d  (python tools/net_shim.py ctl \"status\")" % port)
    return srv


# ---- timeline --------------------------------------------------------------------------------
async def run_timeline(shim, path):
    """Replay `<ms> <command>` lines, timed from the FIRST connection.

    Timing from the first connection rather than from process start is what makes a run repeatable:
    the operator's launch latency stops being part of the experiment.
    """
    steps = []
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            s = raw.split("#", 1)[0].strip()
            if not s:
                continue
            ms, _, cmd = s.partition(" ")
            steps.append((float(ms), cmd.strip()))
    steps.sort(key=lambda x: x[0])
    shim.say(
        "timeline %s: %d step(s), armed -- starts at the first connection"
        % (os.path.basename(path), len(steps))
    )
    while shim.t0 is None:
        await asyncio.sleep(0.05)
    for ms, cmd in steps:
        wait = (shim.t0 + ms / 1000.0) - _now()
        if wait > 0:
            await asyncio.sleep(wait)
        shim.say("timeline @%.0fms: %s -> %s" % (ms, cmd, apply_command(shim, cmd)))
    shim.say("timeline done")


# ---- modes -----------------------------------------------------------------------------------
def parse_hostport(s, default_port=None):
    if ":" in s:
        h, _, p = s.rpartition(":")
        return h, int(p)
    if default_port is None:
        raise ValueError("expected host:port, got %r" % s)
    return s, default_port


def do_selftest():
    """Prove the shim does what it claims, against a loopback echo server.

    Worth having as a mode rather than a scratch script: every experiment built on this tool reads
    its numbers as ground truth, so "the shim injected 200 ms" needs to be a measured fact. In
    particular [5] guards the property that is easy to get wrong and impossible to notice later --
    jitter must never reorder a TCP stream (the far peer would see a MAC failure, not jitter).
    """
    import subprocess
    import threading

    ECHO, LISTEN, CTL = 39610, 39611, 39699
    stop = threading.Event()

    def echo_server():
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", ECHO))
        srv.listen(4)
        srv.settimeout(0.3)
        while not stop.is_set():
            try:
                c, _ = srv.accept()
            except socket.timeout:
                continue
            threading.Thread(
                target=lambda s: [s.sendall(d) for d in iter(lambda: s.recv(4096), b"")],
                args=(c,),
                daemon=True,
            ).start()
        srv.close()

    threading.Thread(target=echo_server, daemon=True).start()
    proc = subprocess.Popen(
        [
            sys.executable,
            os.path.abspath(__file__),
            "--listen",
            "127.0.0.1:%d" % LISTEN,
            "--target",
            "127.0.0.1:%d" % ECHO,
            "--delay",
            "0",
            "--control",
            str(CTL),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )
    time.sleep(1.5)

    def ctl(cmd):
        with socket.create_connection(("127.0.0.1", CTL), timeout=5) as s:
            s.sendall((cmd + "\n").encode())
            return s.recv(4096).decode().strip()

    def rtt(sock, payload=b"x" * 64):
        t = time.perf_counter()
        sock.sendall(payload)
        got = b""
        while len(got) < len(payload):
            got += sock.recv(4096)
        return (time.perf_counter() - t) * 1000.0

    fails = []
    try:
        c = socket.create_connection(("127.0.0.1", LISTEN), timeout=5)
        c.settimeout(10)

        base = min(rtt(c) for _ in range(5))
        print("[1] baseline rtt              %6.1f ms" % base)
        if base > 15:
            fails.append("baseline rtt %.1fms -- the shim itself is too slow to trust" % base)

        ctl("set delay 100")
        med = sorted(rtt(c) for _ in range(5))[2]
        print("[2] rtt @ delay=100 one-way   %6.1f ms  (expect ~200)" % med)
        if not 185 <= med <= 235:
            fails.append("injected delay measured %.1fms, expected ~200" % med)

        ctl("set delay 0")
        ctl("blackhole on")
        c.sendall(b"vanish")
        c.settimeout(1.5)
        try:
            c.recv(10)
            fails.append("blackhole forwarded data")
            print("[3] blackhole drops data      FAIL")
        except socket.timeout:
            print("[3] blackhole drops data      ok")
        try:  # the whole point: gone, but NOT closed -- that is what the rx watchdog must catch
            c.sendall(b"still-open")
            print("[4] ...socket stays OPEN      ok")
        except OSError as e:
            fails.append("socket died under blackhole (%s) -- that is a cut, not a blackhole" % e)
            print("[4] ...socket stays OPEN      FAIL")

        ctl("blackhole off")
        ctl("set delay 20")
        ctl("set jitter 15")
        c.settimeout(20)
        payload = b"".join(b"%06d" % i for i in range(4000))
        c.sendall(payload)
        got = b""
        while len(got) < len(payload):
            got += c.recv(65536)
        ok = got == payload
        print(
            "[5] 24 kB under jitter        %s" % ("ok (in order, byte-identical)" if ok else "FAIL")
        )
        if not ok:
            fails.append("jitter reordered or corrupted the stream")

        ctl("set jitter 0")
        ctl("cut rst")
        time.sleep(0.5)
        try:
            c.settimeout(3)
            c.sendall(b"after-cut")
            if c.recv(10) != b"":
                fails.append("cut left the connection alive")
                print("[6] cut rst                   FAIL")
            else:
                raise ConnectionResetError
        except OSError:
            print("[6] cut rst                   ok")
    finally:
        proc.terminate()
        stop.set()

    print(
        "\n=== %s ==="
        % ("PASS: the shim injects what it says" if not fails else "FAIL: " + "; ".join(fails))
    )
    return 1 if fails else 0


def do_ctl(args):
    cmd = " ".join(args.command)
    with socket.create_connection(("127.0.0.1", args.control), timeout=5) as s:
        s.sendall((cmd + "\n").encode("utf-8"))
        s.settimeout(5)
        out = b""
        while not out.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            out += chunk
    text = out.decode("utf-8", "replace").rstrip()
    print(text)
    return 0 if text.startswith("ok") or text.startswith("commands:") else 1


async def do_run(args):
    lhost, lport = parse_hostport(args.listen, 6501)
    thost, tport = parse_hostport(args.target, 6501)
    params = Params(args.delay, args.jitter, args.rate)
    shim = Shim(params, Stats(), args.log)
    shim.t0 = None

    server = await asyncio.start_server(
        lambda r, w: shim.handle(r, w, (thost, tport)), lhost, lport
    )
    print("net_shim: %s:%d -> %s:%d" % (lhost, lport, thost, tport), flush=True)
    print("net_shim: %s" % params.describe(), flush=True)
    ctl = await control_server(shim, args.control)
    tasks = [
        asyncio.ensure_future(server.serve_forever()),
        asyncio.ensure_future(ctl.serve_forever()),
    ]
    if args.timeline:
        tasks.append(asyncio.ensure_future(run_timeline(shim, args.timeline)))
    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass


def main():
    global LINGER_ZERO
    import struct

    LINGER_ZERO = struct.pack("hh", 1, 0)  # l_onoff=1, l_linger=0 -> RST on close

    ap = argparse.ArgumentParser(
        description="Controllable TCP middlebox: inject delay/jitter/rate and the four ways a link dies.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=HELP,
    )
    ap.add_argument(
        "--listen", default="0.0.0.0:6501", help="where peers connect (default 0.0.0.0:6501)"
    )
    ap.add_argument("--target", help="the real host, e.g. <host>:6501")
    ap.add_argument("--delay", type=float, default=0.0, help="ONE-WAY delay in ms (rtt = 2x this)")
    ap.add_argument("--jitter", type=float, default=0.0, help="+/- uniform ms around --delay")
    ap.add_argument("--rate", type=int, default=0, help="bandwidth ceiling in kbps (0 = unlimited)")
    ap.add_argument(
        "--control", type=int, default=DEFAULT_CONTROL_PORT, help="control port (localhost only)"
    )
    ap.add_argument(
        "--timeline", help="file of '<ms> <command>' lines, timed from the first connection"
    )
    ap.add_argument("--log", help="also append the event log to this file")
    ap.add_argument(
        "command",
        nargs="*",
        help="'ctl <command>' to drive a running shim, or 'selftest' to verify the shim itself",
    )

    args = ap.parse_args()

    if args.command and args.command[0] == "selftest":
        return do_selftest()

    if args.command and args.command[0] == "ctl":
        args.command = args.command[1:]
        if not args.command:
            ap.error("ctl needs a command, e.g. ctl \"set delay 200\"")
        return do_ctl(args)

    if not args.target:
        ap.error("--target is required (the real host, e.g. <host>:6501)")

    if sys.platform == "win32":
        # Default Windows timer granularity is ~15 ms, which would quantise every delay we inject
        # (and add up to 15 ms of noise to a 100 ms setting). Same call the DLL makes for the clock.
        try:
            ctypes.windll.winmm.timeBeginPeriod(1)
        except (AttributeError, OSError):
            pass

    try:
        asyncio.run(do_run(args))
    except KeyboardInterrupt:
        print("net_shim: stopped", flush=True)
    finally:
        if sys.platform == "win32":
            try:
                ctypes.windll.winmm.timeEndPeriod(1)
            except (AttributeError, OSError):
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
