#!/usr/bin/env python3
"""net_shim -- a controllable TCP **or UDP** middlebox for the 2-VM rig.

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

`--udp` (tracker mp:T1) is the other mode, for `[net] transport=udp`, and it lifts exactly that
restriction: a datagram is a unit, so destroying one IS loss, and `set loss <percent>` does it. It
also reorders under jitter, deliberately -- a real UDP path does, and a transport that only works on
an in-order path has not been tested. `stall` is REFUSED in udp mode rather than emulated: there is
no receive window to stop draining, so it would be a blackhole under a second name, and two names
for one behaviour is two pieces of evidence for one fact. See the UDP section at the bottom.

    python tools/net_shim.py --udp --listen 0.0.0.0:6501 --target <host>:6501 --loss 5
    python tools/net_shim.py udpselftest      # prove the loss dial is real before believing a run

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

`--manual-arm` (mp:TL-SHIMUDP) is for an owning runner (ui_test.py, not a human) that starts the shim
before the real session exists and wants the timeline's zero point to be the moment IT decides, not
whatever packet happens to arrive first -- especially on UDP, where there is no connection state to
tell a real peer from a stray one:

    python tools/net_shim.py --udp --listen ... --target ... --timeline ... --manual-arm
    python tools/net_shim.py ctl arm       # starts the clock now
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
    # perf_counter, not monotonic, and that is the whole of mp:T3b -- see sharpen_loop_clock below.
    # `time.monotonic()` on Windows is GetTickCount64: it advances in ~15.6 ms STEPS (measured on the
    # dev box 2026-09-18: min 15.0, median 16.0 ms, and timeBeginPeriod(1) does not change it). Every
    # delay this file schedules and every elapsed time it prints was quantised by that.
    return time.perf_counter()


# ---- the 31 ms the shim used to give away (tracker mp:T3b) ---------------------------------------
#
# THE BUG. Under load this shim delivered up to ~31 ms LESS round-trip delay than it printed, and it
# did so silently. Two Windows-specific facts compose:
#
#   1. `loop.time()` is `time.monotonic()` = GetTickCount64, which advances in ~15.6 ms steps. So
#      `call_later(0.040)` computes its deadline from a clock reading that is already up to 15.6 ms
#      STALE, and the timer comes due that much early in real time.
#   2. `BaseEventLoop._run_once` then runs every handle due before `self.time() + _clock_resolution`,
#      and `_clock_resolution` is that same 15.625 ms. A second tick, given away on top of the first.
#
# WHY IT WAS INVISIBLE. Fact 2 only bites when the loop has other work: with `self._ready` non-empty
# the select timeout is 0, `_run_once` returns instantly, and the within-resolution timers go out
# with it. An idle loop instead waits out the remaining time. So the shim injected what it claimed
# when a human probed it by hand at a few datagrams a second, and under-delivered once a real game
# put ~190 datagrams a second through it. Measured on the rig, same shim, same two-VM path,
# `--delay 40` (an 80 ms nominal round trip):
#
#       offered load        delivered RTT
#       5 probes/s              84.2 ms
#       500 probes/s            57.7 ms
#
# That is the whole of mp:T3b. The transport's SRTT had been read as ~19 ms "below the injected RTT"
# against a reference that was not injecting the RTT it printed, and the loopback probe that
# "cleared" the shim cleared it at a load the bug does not appear at. The lesson generalises past
# this file: a delay injector must be verified AT THE LOAD IT WILL BE USED AT, by something outside
# it (tools/udp_rtt_probe.py is that something).
#
# THE FIX. Give the loop a real clock. `time.perf_counter()` is QPC-backed, sub-microsecond, and
# monotonic, and asyncio only ever uses `loop.time()` for differences -- the epoch does not matter,
# so swapping it before anything is scheduled is safe. `_clock_resolution` then comes down with it,
# which closes fact 2 as well.
def sharpen_loop_clock(loop):
    """Point the event loop at QPC instead of the 15.6 ms system tick.

    Instance-attribute override of a public method, plus one private attribute. Both are guarded:
    a runtime that does not take them keeps its own behaviour, and `_schedule` re-checks the real
    deadline anyway, which is the belt to this braces.
    """
    try:
        loop.time = time.perf_counter
        res = time.get_clock_info("perf_counter").resolution
        if getattr(loop, "_clock_resolution", 0.0) > res:
            loop._clock_resolution = res
    except (AttributeError, TypeError, ValueError):
        pass


class Params:
    """Live link parameters. Mutated by the control channel; read by every pump on every chunk.

    No locking: asyncio is single-threaded, and a pump only ever reads these between awaits.
    """

    def __init__(self, delay_ms=0.0, jitter_ms=0.0, rate_kbps=0):
        self.delay_ms = float(delay_ms)
        self.jitter_ms = float(jitter_ms)
        self.rate_kbps = int(rate_kbps)
        # UDP ONLY, and refused in tcp mode rather than silently ignored -- a knob that reports a
        # loss rate it is not injecting is worse than no knob, and the whole mp:T1 loss claim is
        # read off this number.
        self.loss_pct = 0.0
        self.blackhole = set()  # subset of {"c2s", "s2c"}
        self.stall = set()  # subset of {"c2s", "s2c"}
        self.seed = 12345
        # UDP ONLY (mp:R1d). A PATH MTU: a datagram longer than this is destroyed, in both
        # directions, silently -- which is what a real path with a smaller MTU and the DF bit does
        # to a UDP datagram that will not fit. 0 = unlimited.
        #
        # It models the ONE failure the 1200-byte ceiling exists to prevent, and it is a separate
        # knob from --loss for the reason `blackhole` is separate from `loss`: this loss is
        # SIZE-DEPENDENT, so it destroys exactly the bulk-transfer datagrams and leaves every
        # keepalive and lockstep packet untouched. That asymmetry is the diagnosis -- a link that
        # plays fine and cannot finish a map download is this, and a uniform loss rate never
        # reproduces it.
        self.mtu = 0

    def describe(self):
        return (
            "delay=%.1fms (one way; %.1fms rtt) jitter=%.1fms rate=%s loss=%.1f%% mtu=%s "
            "blackhole=%s stall=%s"
            % (
                self.delay_ms,
                self.delay_ms * 2,
                self.jitter_ms,
                ("%dkbps" % self.rate_kbps) if self.rate_kbps else "unlimited",
                self.loss_pct,
                ("%dB" % self.mtu) if self.mtu else "unlimited",
                ",".join(sorted(self.blackhole)) or "-",
                ",".join(sorted(self.stall)) or "-",
            )
        )


class Stats:
    def __init__(self):
        self.conns = 0
        self.live = 0
        self.bytes = {"c2s": 0, "s2c": 0}
        self.dropped = {"c2s": 0, "s2c": 0}
        # Kept apart from `dropped` on purpose: a blackholed byte and a lost datagram are different
        # experiments, and a run that conflated them could not say which one it had performed.
        self.lost = {"c2s": 0, "s2c": 0}
        # mp:R1d -- destroyed for being over --mtu, and the high-water mark of what was seen. Kept
        # apart from `lost` for the same reason `lost` is kept apart from `dropped`: a run must be
        # able to say WHICH experiment it performed, and "0 oversize" over a 12,000-datagram
        # transfer is the positive result R1d is after, indistinguishable from "the knob was off"
        # unless the largest datagram is reported beside it.
        self.oversize = {"c2s": 0, "s2c": 0}
        self.max_seen = 0


class Shim:
    def __init__(self, params, stats, log, manual_arm=False):
        self.p = params
        self.s = stats
        self.log = log
        self.conns = []  # live [(client_writer, server_writer)] for `cut`
        self.rng = random.Random(params.seed)
        self.t0 = None
        self.manual_arm = manual_arm  # mp:TL-SHIMUDP -- see `arm` in apply_command

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
        if self.t0 is None and not self.manual_arm:
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
  set loss <percent>    UDP ONLY: destroy this fraction of datagrams, each direction independently
  set mtu <bytes>       UDP ONLY: destroy any datagram LONGER than this (0 = unlimited). A path
                        MTU, so the loss is size-dependent -- it takes the bulk-transfer datagrams
                        and leaves the lockstep ones alone (mp:R1d)
  blackhole on|off|c2s|s2c    read and discard; sockets stay OPEN (-> rx watchdog)
  stall on|off          stop draining; sender feels backpressure (alive, just quiet)
  cut [rst]             close live connections (FIN, or RST with 'rst')
  arm                    start the timeline clock NOW (only meaningful with --manual-arm; see below)
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
            elif k == "loss":
                if not isinstance(shim, UdpShim):
                    return (
                        "err: `set loss` is UDP ONLY. Deleting bytes from a TCP stream is "
                        "corruption, not loss -- the peer fails its record MAC. Run with --udp."
                    )
                p.loss_pct = float(v)
            elif k == "mtu":
                # mp:R1d. UDP only for the same reason `loss` is: a TCP stream has no datagram to
                # be too big, and a byte limit on a stream is a bandwidth ceiling, which is `rate`.
                if not isinstance(shim, UdpShim):
                    return (
                        "err: `set mtu` is UDP ONLY. A TCP stream has no datagram boundary to "
                        "exceed; the ceiling you want on a stream is `set rate`. Run with --udp."
                    )
                p.mtu = int(v)
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
            if isinstance(shim, UdpShim):
                return (
                    "err: `stall` has no meaning on UDP -- there is no receive window to stop "
                    "draining, so it would be `blackhole` under a second name. Use blackhole."
                )
            a = t[1].lower()
            p.stall = set() if a == "off" else ({"c2s", "s2c"} if a == "on" else {a})
            shim.say("STALL %s" % (",".join(sorted(p.stall)) or "off"))
            return "ok " + p.describe()
        if c == "cut":
            rst = len(t) > 1 and t[1].lower() == "rst"
            k = shim.cut(rst)
            shim.say("CUT%s -- %d connection(s)" % (" rst" if rst else "", k))
            return "ok cut %d" % k
        if c == "arm":
            # mp:TL-SHIMUDP. Only meaningful on a shim started --manual-arm: normally `t0` (the
            # timeline's zero point) is set by the first connection/datagram, which for UDP is any
            # stray packet -- there is no handshake to distinguish "a real peer" from "something else
            # hit this port". A shim the runner owns can instead tell it exactly when the real session
            # began (host confirmed LISTENING) and arm explicitly; --manual-arm suppresses the
            # datagram-triggered arm so this is the only thing that can start the clock.
            if shim.t0 is None:
                shim.t0 = _now()
                shim.say("ARMED (explicit)")
                return "ok armed"
            return "ok already-armed"
        if c == "status":
            return (
                "ok %s | conns=%d live=%d c2s=%dB s2c=%dB dropped=%d/%d lost=%d/%d "
                "oversize=%d/%d max_seen=%dB"
                % (
                    p.describe(),
                    shim.s.conns,
                    shim.s.live,
                    shim.s.bytes["c2s"],
                    shim.s.bytes["s2c"],
                    shim.s.dropped["c2s"],
                    shim.s.dropped["s2c"],
                    shim.s.lost["c2s"],
                    shim.s.lost["s2c"],
                    getattr(shim.s, "oversize", {}).get("c2s", 0),
                    getattr(shim.s, "oversize", {}).get("s2c", 0),
                    getattr(shim.s, "max_seen", 0),
                )
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
    shim = Shim(params, Stats(), args.log, manual_arm=args.manual_arm)
    sharpen_loop_clock(asyncio.get_event_loop())

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


# ---- UDP mode (tracker mp:T1) ------------------------------------------------------------------
#
# The shim above terminates TCP on both sides, and the module docstring is blunt about what that
# costs: it CANNOT model loss, because deleting bytes from a reliable stream is corruption, not
# loss. `[net] transport=udp` removes that constraint -- a datagram is a unit, and destroying one is
# exactly what a network does. So this half is a UDP forwarder with the same control channel, the
# same timeline file and the same live `ctl` commands, plus the one command TCP mode refuses:
#
#     set loss <percent>        destroy this fraction of datagrams, per direction, independently
#
# WHAT IT MODELS, AND WHAT IT DOES NOT. It can drop, delay, jitter and (unlike the TCP half)
# REORDER -- jitter here is deliberately not clamped monotonic, because a real UDP path reorders and
# a transport that only works on an in-order path has not been tested. `rate` is honoured as a
# per-datagram serialisation delay. `stall` is REFUSED rather than emulated: there is no receive
# window to stop draining, so a "stall" on UDP would be indistinguishable from a blackhole and
# giving it a second name would make the two look like different evidence.
#
# THE MAPPING. One socket faces the peers; each distinct source address gets its own upstream socket
# towards the target, so replies come back to the right peer. That is the same star the game plays:
# the clients point at this box's `--listen` and the box points at the host. `cut` forgets every
# mapping, so the far end goes silent and each peer's rx watchdog fires -- the UDP analogue of the
# TCP `cut`, and the reason it keeps the name.


class UdpShim:
    """One socket facing the peers, one upstream socket per peer address."""

    def __init__(self, params, stats, log, target, manual_arm=False):
        self.p = params
        self.s = stats
        self.log = log
        self.target = target
        self.rng = random.Random(params.seed ^ 0x5DEECE66)
        self.t0 = None
        self.manual_arm = manual_arm  # mp:TL-SHIMUDP -- see `arm` in apply_command
        self.down = None  # the transport peers send to
        self.ups = {}  # peer addr -> upstream transport
        self.conns = []  # for `cut`, and so `status` can report a peer count

    def say(self, msg):
        line = "[%8.3f] %s" % (_now() - (self.t0 or _now()), msg)
        print(line, flush=True)
        if self.log:
            with open(self.log, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")

    # ---- the one decision every datagram passes through ---------------------------------------
    def _schedule(self, tag, send, size=0):
        """Apply mtu / loss / delay / jitter / rate to one datagram, then hand it to `send`."""
        if size > self.s.max_seen:
            self.s.max_seen = size
        if tag in self.p.blackhole:
            self.s.dropped[tag] += 1
            return
        # mp:R1d -- THE MTU DROP, and it is tested BEFORE the loss dice on purpose. A datagram that
        # will not fit is destroyed by the path every single time, not `loss_pct` of the time, and a
        # size limit sampled through a random gate would turn a deterministic failure into a flaky
        # one -- which is the hardest kind of bug to read off a run.
        if self.p.mtu and size > self.p.mtu:
            self.s.oversize[tag] += 1
            return
        if self.p.loss_pct > 0 and self.rng.random() * 100.0 < self.p.loss_pct:
            self.s.lost[tag] += 1
            return
        delay = self.p.delay_ms
        if self.p.jitter_ms:
            delay += self.rng.uniform(-self.p.jitter_ms, self.p.jitter_ms)
        if delay < 0:
            delay = 0.0
        if delay <= 0:
            send()
            return
        # No monotonic clamp, unlike the TCP pump: a real UDP path reorders, and a transport that
        # only works in order has not been tested.
        #
        # RE-ARMED ON THE REAL DEADLINE (mp:T3b). `call_later` is allowed to fire up to one
        # `_clock_resolution` early -- see sharpen_loop_clock, which is the primary fix. This is the
        # second one, and it is here because the first edits a private attribute: if a future runtime
        # renames it, an early wakeup still cannot shorten the link, it can only cost one extra pass
        # through the loop. `left > 0.0005` rather than `> 0` so the last half-millisecond does not
        # buy a wakeup nobody can measure.
        loop = asyncio.get_event_loop()
        deadline = _now() + delay / 1000.0

        def fire():
            left = deadline - _now()
            if left > 0.0005:
                loop.call_later(left, fire)
                return
            send()

        loop.call_later(delay / 1000.0, fire)

    def on_from_peer(self, data, addr):
        if self.t0 is None and not self.manual_arm:
            self.t0 = _now()
            self.say("first datagram from %s:%d  (%s)" % (addr[0], addr[1], self.p.describe()))
        up = self.ups.get(addr)
        if up is None:
            return  # the mapping is created by the caller; a race here means it was just cut
        self.s.bytes["c2s"] += len(data)

        def send():
            try:
                up.sendto(data, self.target)
            except OSError:
                pass

        self._schedule("c2s", send, len(data))

    def on_from_target(self, data, addr):
        self.s.bytes["s2c"] += len(data)

        def send():
            try:
                if self.down is not None:
                    self.down.sendto(data, addr)
            except OSError:
                pass

        self._schedule("s2c", send, len(data))

    def cut(self, rst):
        # RST has no meaning on UDP; the flag is accepted so one timeline file can drive both modes.
        n = len(self.ups)
        for tr in list(self.ups.values()):
            try:
                tr.close()
            except OSError:
                pass
        self.ups.clear()
        return n


class _DownProto(asyncio.DatagramProtocol):
    def __init__(self, shim, loop):
        self.shim = shim
        self.loop = loop

    def connection_made(self, transport):
        self.shim.down = transport

    def datagram_received(self, data, addr):
        sh = self.shim
        if addr not in sh.ups:
            # A new peer. Its upstream socket is created lazily, and the datagram that created it is
            # delivered once the socket exists -- dropping that first datagram would make every join
            # cost one handshake retransmit and would look exactly like injected loss.
            self.loop.create_task(_open_upstream(sh, addr, first=data))
            return
        sh.on_from_peer(data, addr)


class _UpProto(asyncio.DatagramProtocol):
    def __init__(self, shim, peer):
        self.shim = shim
        self.peer = peer

    def datagram_received(self, data, _addr):
        self.shim.on_from_target(data, self.peer)


async def _open_upstream(shim, addr, first=None):
    loop = asyncio.get_event_loop()
    try:
        transport, _ = await loop.create_datagram_endpoint(
            lambda: _UpProto(shim, addr), local_addr=("0.0.0.0", 0)
        )
    except OSError as e:
        shim.say("cannot open an upstream socket for %s:%d (%s)" % (addr[0], addr[1], e))
        return
    if addr in shim.ups:  # lost a race with another datagram from the same peer
        transport.close()
    else:
        shim.ups[addr] = transport
        shim.s.conns += 1
        shim.s.live = len(shim.ups)
        shim.say(
            "peer %d: %s:%d -> %s:%d (udp)"
            % (shim.s.conns, addr[0], addr[1], shim.target[0], shim.target[1])
        )
    if first is not None:
        shim.on_from_peer(first, addr)


async def do_run_udp(args):
    lhost, lport = parse_hostport(args.listen, 6501)
    thost, tport = parse_hostport(args.target, 6501)
    params = Params(args.delay, args.jitter, args.rate)
    params.loss_pct = float(args.loss)
    params.mtu = int(getattr(args, "mtu", 0) or 0)  # mp:R1d
    shim = UdpShim(params, Stats(), args.log, (thost, tport), manual_arm=args.manual_arm)

    loop = asyncio.get_event_loop()
    sharpen_loop_clock(loop)
    await loop.create_datagram_endpoint(lambda: _DownProto(shim, loop), local_addr=(lhost, lport))
    print("net_shim(udp): %s:%d -> %s:%d" % (lhost, lport, thost, tport), flush=True)
    print("net_shim(udp): %s" % params.describe(), flush=True)
    ctl = await control_server(shim, args.control)
    tasks = [asyncio.ensure_future(ctl.serve_forever())]
    if args.timeline:
        tasks.append(asyncio.ensure_future(run_timeline(shim, args.timeline)))
    try:
        await asyncio.gather(*tasks)
    except asyncio.CancelledError:
        pass


def do_selftest_udp():
    """Prove the UDP half injects what it says, against a loopback echo server.

    The same argument as the TCP selftest, and one arm more: [3] is the only measurement anywhere
    that says the LOSS DIAL IS REAL. Every claim mp:T1 makes about redundancy covering injected loss
    is downstream of that number, so it is measured rather than assumed.
    """
    import subprocess
    import threading

    ECHO, LISTEN, CTL = 39710, 39711, 39799
    stop = threading.Event()

    def echo_server():
        srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        srv.bind(("127.0.0.1", ECHO))
        srv.settimeout(0.3)
        while not stop.is_set():
            try:
                d, a = srv.recvfrom(4096)
            except socket.timeout:
                continue
            srv.sendto(d, a)
        srv.close()

    threading.Thread(target=echo_server, daemon=True).start()
    proc = subprocess.Popen(
        [
            sys.executable,
            os.path.abspath(__file__),
            "--udp",
            "--listen",
            "127.0.0.1:%d" % LISTEN,
            "--target",
            "127.0.0.1:%d" % ECHO,
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

    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c.settimeout(1.0)
    dst = ("127.0.0.1", LISTEN)

    def round_trip(payload=b"x" * 64, timeout=1.0):
        c.settimeout(timeout)
        t = time.perf_counter()
        c.sendto(payload, dst)
        try:
            c.recv(4096)
        except socket.timeout:
            return None
        return (time.perf_counter() - t) * 1000.0

    fails = []
    try:
        for _ in range(3):
            round_trip()  # warm the mapping
        base = min(x for x in (round_trip() for _ in range(5)) if x is not None)
        print("[1] baseline rtt              %6.1f ms" % base)
        if base > 15:
            fails.append("baseline rtt %.1fms -- the shim itself is too slow to trust" % base)

        ctl("set delay 100")
        got = sorted(x for x in (round_trip(timeout=3.0) for _ in range(5)) if x is not None)
        med = got[len(got) // 2] if got else 1e9
        print("[2] rtt @ delay=100 one-way   %6.1f ms  (expect ~200)" % med)
        if not 185 <= med <= 245:
            fails.append("injected delay measured %.1fms, expected ~200" % med)

        ctl("set delay 0")
        ctl("set loss 50")
        # 50% each way -> ~25% of round trips survive. 200 attempts makes the binomial tail
        # negligible: the arm fails only if the dial is doing nothing at all, or everything.
        seen = sum(1 for _ in range(200) if round_trip(timeout=0.4) is not None)
        print("[3] 50%% loss both ways        %d/200 round trips survived (expect ~50)" % seen)
        if not 10 <= seen <= 110:
            fails.append("loss dial measured %d/200 survivors, expected ~50" % seen)

        ctl("set loss 0")
        seen = sum(1 for _ in range(30) if round_trip(timeout=1.0) is not None)
        print("[4] loss 0 restores the link  %d/30" % seen)
        if seen < 28:
            fails.append("loss 0 did not restore the link (%d/30)" % seen)

        r = ctl("stall on")
        print("[5] stall is refused on udp   %s" % r)
        if not r.startswith("err"):
            fails.append("stall was accepted in udp mode; it has no meaning there")
    finally:
        c.close()
        proc.terminate()
        stop.set()

    print(
        "\n=== %s ==="
        % ("PASS: the udp shim injects what it says" if not fails else "FAIL: " + "; ".join(fails))
    )
    return 1 if fails else 0


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
        "--udp",
        action="store_true",
        help="forward UDP instead of TCP (for `[net] transport=udp`); enables --loss",
    )
    ap.add_argument(
        "--loss",
        type=float,
        default=0.0,
        help="UDP only: percent of datagrams destroyed in each direction",
    )
    ap.add_argument(
        "--mtu",
        type=int,
        default=0,
        help="UDP only (mp:R1d): a PATH MTU in bytes. A datagram longer than this is destroyed "
        "silently, in both directions -- what a real path with a smaller MTU does to a UDP "
        "datagram that will not fit. 0 = unlimited. `--mtu 1200` is the figure T0's own ceiling "
        "is chosen for, so a relayed run over it proves the leg envelope still fits; `--mtu 1233` "
        "is the sharper test, since it drops a pre-R1d 1234-byte relayed datagram and nothing "
        "else. `ctl status` reports `oversize=` and `max_seen=`.",
    )
    ap.add_argument(
        "--control", type=int, default=DEFAULT_CONTROL_PORT, help="control port (localhost only)"
    )
    ap.add_argument(
        "--timeline", help="file of '<ms> <command>' lines, timed from the first connection"
    )
    ap.add_argument(
        "--manual-arm",
        action="store_true",
        help="mp:TL-SHIMUDP -- do NOT start the timeline clock on the first connection/datagram; "
        "wait for an explicit `ctl arm` instead. UDP has no connection state, so 'first datagram' "
        "is any stray packet that happens to hit this port, not necessarily the real session -- an "
        "owning runner that knows exactly when the real peer is ready (e.g. the host confirmed "
        "LISTENING) should arm explicitly instead of trusting whatever arrives first.",
    )
    ap.add_argument("--log", help="also append the event log to this file")
    ap.add_argument(
        "command",
        nargs="*",
        help="'ctl <command>' to drive a running shim, or 'selftest' / 'udpselftest' to "
        "verify the shim itself",
    )

    args = ap.parse_args()

    if args.command and args.command[0] == "selftest":
        return do_selftest()

    if args.command and args.command[0] == "udpselftest":
        return do_selftest_udp()

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
        asyncio.run(do_run_udp(args) if args.udp else do_run(args))
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
