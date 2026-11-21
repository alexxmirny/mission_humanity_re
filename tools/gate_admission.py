"""Gate-owned CPU/VM admission. Standalone tools retain their ordinary execution.

One core is three credits. Root commands are contained before their startup barrier
opens; a lost active client cancels its whole root, never silently refunds capacity.
The gate calls complete_root only AFTER proving the contained process tree empty.
"""

from __future__ import annotations

import contextlib
import ctypes
import json
import math
import os
import secrets
import select
import socket
import socketserver
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path

ENV = "MH_GATE_ADMISSION"
CORE = 3


def configuration():
    raw = os.environ.get(ENV)
    if raw is None:
        return None
    try:
        c = json.loads(raw)
        if (
            set(c) != {"port", "key", "owner", "covered", "cpu_bound"}
            or type(c["port"]) is not int
            or not 0 < c["port"] < 65536
            or not isinstance(c["key"], str)
            or len(c["key"]) != 64
            or not isinstance(c["owner"], str)
            or not c["owner"]
            or type(c["covered"]) is not bool
            or type(c["cpu_bound"]) is not bool
        ):
            raise ValueError("invalid fields")
        return c
    except (ValueError, TypeError, KeyError) as exc:
        raise RuntimeError("invalid gate admission configuration") from exc


def enabled():
    return configuration() is not None


def _send(stream, value):
    stream.write((json.dumps(value, separators=(",", ":")) + "\n").encode("utf-8"))
    stream.flush()


def _receive(stream):
    line = stream.readline(8193)
    if not line or len(line) > 8192 or not line.endswith(b"\n"):
        raise RuntimeError("gate admission connection lost or malformed message")
    value = json.loads(line)
    if not isinstance(value, dict):
        raise RuntimeError("invalid gate admission reply")
    return value


@contextlib.contextmanager
def _connection(c):
    with socket.create_connection(("127.0.0.1", c["port"]), timeout=10) as sock:
        sock.set_inheritable(False)
        sock.settimeout(None)
        with sock.makefile("rwb") as stream:
            yield stream


class Lease:
    def __init__(self, config):
        self.config = config

    def covered_env(self, base=None):
        """Copy for a subprocess already charged to this lease; no global env mutation."""
        env = (os.environ if base is None else base).copy()
        if self.config is not None:
            env[ENV] = json.dumps(dict(self.config, covered=True))
        return env


@contextlib.contextmanager
def lease(label, *, seconds=30, credits=CORE, bootstrap=False, deadline=None, tail=()):
    c = configuration()
    ticket = Lease(c)
    if c is None or c["covered"]:
        yield ticket
        return
    with _connection(c) as stream:
        _send(
            stream,
            dict(
                c,
                op="acquire",
                label=label,
                seconds=seconds,
                credits=credits,
                bootstrap=bootstrap,
                deadline=deadline,
                tail=tail,
            ),
        )
        reply = _receive(stream)
        if reply.get("status") != "granted":
            raise RuntimeError("gate admission refused: " + str(reply.get("error")))
        try:
            yield ticket
        except BaseException as exc:
            # Flush outside per-row stdout capture BEFORE cancellation kills this Job.
            # Never serialize configuration/credentials; keep the RPC below its 8192-byte
            # framing bound even when every character needs a JSON surrogate pair.
            formatted = traceback.TracebackException.from_exception(exc)
            trace = "".join(formatted.format()).replace(c["key"], "[redacted]")
            detail = "".join(formatted.format_exception_only()).replace(c["key"], "[redacted]")
            with (
                contextlib.suppress(OSError),
                os.fdopen(
                    os.dup(2), "w", encoding="utf-8", errors="backslashreplace"
                ) as diagnostic,
            ):
                print(
                    f"[admission] owner={c['owner']} label={label}\n{trace}",
                    file=diagnostic,
                    flush=True,
                )
            # The owner may still have children. Only its job rundown can refund them.
            # A failed cancellation write must not replace the original exception.
            with contextlib.suppress(OSError):
                _send(stream, {"op": "cancel", "error": detail[:512]})
            raise
        else:
            _send(stream, {"op": "release"})
            if _receive(stream).get("status") != "released":
                raise RuntimeError("gate admission release was not acknowledged")


def relinquish_startup():
    """Do not reserve startup CPU while waiting for an external host-global lease."""
    c = configuration()
    if c is None or c["covered"]:
        return
    with _connection(c) as stream:
        _send(stream, dict(c, op="park"))
        if _receive(stream).get("status") != "parked":
            raise RuntimeError("gate startup handoff refused")


@dataclass
class _Request:
    row: dict
    owner: str
    root: bool = False
    queued: float = 0
    started: float | None = None
    ended: float | None = None
    error: str = ""
    event: threading.Event = field(default_factory=threading.Event)


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = False
    daemon_threads = True


def leaf_duration_hints(record):
    """Learn execution-only scheduling hints, never verdicts or timeout/budget changes."""
    passed = record.get("unit_verdicts", {})
    hints = {}
    for r in record.get("requests", []):
        if r.get("root") or r.get("error") or passed.get(r.get("owner")) != "PASS":
            continue
        start, end = r.get("started"), r.get("ended")
        if any(type(v) not in (int, float) or not math.isfinite(v) for v in (start, end)):
            continue
        if not 0 <= start <= end or end - start > 86400:
            continue
        key = (r["owner"], r["label"])
        hints[key] = max(hints.get(key, 1.0), end - start)
    return hints


# Deadlines are scheduling hints, not permission for a stream of new leaves to
# starve older work. After this wait, accumulate credits for the oldest eligible
# request. Leases must not span a wait for another admission lease.
MAX_PRIORITY_WAIT_S = 30


def pick_fair_units(pending, free, free_vm, running, now, capacity, pick_units):
    for r in pending:
        if r.get("vm") and free_vm < 1:
            r["eligible_since"] = None
        elif r.get("eligible_since") is None:
            # Root VM work must not age while the slot is unavailable, then
            # leapfrog leaves that could actually have run.
            r["eligible_since"] = now if r.get("vm") else r.get("queued", 0)
    eligible = [
        r
        for r in pending
        if r.get("eligible_since") is not None and now - r["eligible_since"] >= MAX_PRIORITY_WAIT_S
    ]
    if eligible:
        # min is stable: normal priority breaks simultaneous eligibility ties.
        oldest = min(eligible, key=lambda r: r["eligible_since"])
        # Do not backfill against estimated ends: a long-running leaf can keep
        # extending that estimate and steal the reservation indefinitely.
        pending = [oldest]
    return pick_units(pending, free, free_vm, running, now, capacity)


class Broker:
    def __init__(
        self,
        rows,
        cores,
        priority,
        pick_units,
        controllers=(),
        *,
        cpu_bounded=(),
        duration_hints=(),
        deadline=None,
    ):
        if type(cores) is not int or cores < 1:
            raise ValueError("invalid CPU capacity")
        self.capacity = cores * CORE
        self.free, self.free_vm = self.capacity, 1
        self.origin = time.monotonic()
        self.cv = threading.Condition()
        self.priority, self.pick_units = priority, pick_units
        self.key = secrets.token_hex(32)
        self.roots, self.pending, self.active, self.history = {}, [], [], []
        self.ready, self.cancel_callbacks = {}, {}
        self.cpu_bounded = frozenset(cpu_bounded)
        self.cpu_limits = {}
        self.duration_hints = dict(duration_hints)
        if any(
            type(v) not in (int, float) or not math.isfinite(v) or not 0 < v <= 86400
            for v in self.duration_hints.values()
        ):
            raise ValueError("invalid learned admission duration")
        if deadline is not None and (
            type(deadline) not in (int, float) or not math.isfinite(deadline)
        ):
            raise ValueError("invalid admission deadline")
        self.deadline = deadline
        self.stopped = False
        self.controllers = (
            dict.fromkeys(controllers, 1)
            if not isinstance(controllers, dict)
            else dict(controllers)
        )
        for row in rows:
            owner = row["name"]
            if owner in self.roots:
                raise ValueError("duplicate admission root " + owner)
            startup = self.controllers.get(owner)
            weight = (startup if startup is not None else row["weight"]) * CORE
            if type(weight) is not int or not 0 < weight <= self.capacity:
                raise ValueError("root exceeds admission capacity: " + owner)
            request = _Request(
                dict(
                    row,
                    weight=weight,
                    dur=2 if startup is not None else row["dur"],
                    bootstrap=startup is not None,
                    deadline=deadline,
                ),
                owner,
                root=True,
            )
            self.roots[owner] = request
            self.ready[owner] = threading.Event()
            self.pending.append(request)
            self.history.append(request)
        if self.controllers.keys() - self.roots.keys():
            raise ValueError("unknown admission controller")
        if self.cpu_bounded - self.roots.keys():
            raise ValueError("unknown CPU-bound admission root")
        broker = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                request = None
                try:
                    self.connection.settimeout(10)
                    msg = _receive(self.rfile)
                    if not secrets.compare_digest(str(msg.get("key", "")), broker.key):
                        raise RuntimeError("invalid admission credential")
                    owner = msg.get("owner")
                    if owner not in broker.roots:
                        raise RuntimeError("unknown admission owner")
                    self.connection.settimeout(None)
                    if msg.get("op") == "ready":
                        broker.ready[owner].wait()
                        with broker.cv:
                            root = broker.roots[owner]
                            if root.error or root.ended is not None:
                                raise RuntimeError(root.error or "owner already completed")
                        _send(self.wfile, {"status": "ready"})
                        return
                    if msg.get("op") == "park":
                        with broker.cv:
                            broker._drop_startup(owner)
                            broker.cv.notify_all()
                        _send(self.wfile, {"status": "parked"})
                        return
                    if msg.get("op") != "acquire":
                        raise RuntimeError("invalid admission operation")
                    request = broker._enqueue(owner, msg)
                    while not request.event.wait(0.1):
                        readable, _, _ = select.select([self.connection], [], [], 0)
                        if readable:
                            raise RuntimeError("queued admission client disconnected")
                    if request.error:
                        raise RuntimeError(request.error)
                    _send(self.wfile, {"status": "granted"})
                    msg = _receive(self.rfile)
                    if msg != {"op": "release"}:
                        reason = (
                            "active admission client cancelled "
                            f"({request.owner}/{request.row['name']})"
                        )
                        if isinstance(msg.get("error"), str):
                            reason += ": " + msg["error"].replace(broker.key, "[redacted]")
                        raise RuntimeError(reason)
                    broker._release(request)
                    request = None
                    _send(self.wfile, {"status": "released"})
                except (OSError, RuntimeError, ValueError, TypeError) as exc:
                    if request is not None:
                        with broker.cv:
                            request.error = str(exc)
                        broker.cancel_owner(request.owner, str(exc))
                    with contextlib.suppress(OSError):
                        _send(self.wfile, {"error": str(exc)})

        self.server = _Server(("127.0.0.1", 0), Handler)
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.dispatch_thread = threading.Thread(target=self._dispatch, daemon=True)
        self.server_thread.start()
        self.dispatch_thread.start()

    def child_env(self, owner):
        env = os.environ.copy()
        env[ENV] = json.dumps(
            dict(
                port=self.server.server_address[1],
                key=self.key,
                owner=owner,
                covered=owner not in self.controllers,
                cpu_bound=owner in self.cpu_bounded,
            )
        )
        return env

    def await_root(self, owner):
        root = self.roots[owner]
        root.event.wait()
        if root.error:
            raise RuntimeError(root.error)

    def process_ready(self, owner, cancel_callback, cpu_limit=None):
        with self.cv:
            root = self.roots[owner]
            if root.error or root.started is None or root.ended is not None:
                raise RuntimeError(root.error or "root is not admitted")
            if owner in self.cpu_bounded:
                if cpu_limit is None:
                    raise RuntimeError("CPU-bound owner requires an enforced job limit")
                cpu_limit(sum(r.row["weight"] for r in self.active if r.owner == owner))
                self.cpu_limits[owner] = cpu_limit
            self.cancel_callbacks[owner] = cancel_callback
            self.ready[owner].set()

    def _limit_owner(self, owner, credits):
        if owner in self.cpu_limits:
            # Idle coordinators still need to service IPC; actual work requires a leaf.
            self.cpu_limits[owner](max(1, credits))

    def _enqueue(self, owner, msg):
        weight, seconds, label = msg.get("credits"), msg.get("seconds"), msg.get("label")
        if type(weight) is not int or not 0 < weight <= self.capacity:
            raise RuntimeError("leaf exceeds admission capacity")
        if (
            type(seconds) not in (int, float)
            or not math.isfinite(seconds)
            or not 0 < seconds <= 86400
        ):
            raise RuntimeError("invalid admission duration")
        if not isinstance(label, str) or not 0 < len(label) <= 240:
            raise RuntimeError("invalid admission label")
        bootstrap = msg.get("bootstrap", False)
        if type(bootstrap) is not bool:
            raise RuntimeError("invalid admission bootstrap flag")
        deadline = msg.get("deadline")
        if deadline is not None and (
            type(deadline) not in (int, float) or not math.isfinite(deadline)
        ):
            raise RuntimeError("invalid admission deadline")
        due = [d for d in (deadline, self.deadline) if d is not None]
        tail = msg.get("tail", [])
        if not isinstance(tail, list) or len(tail) > 256:
            raise RuntimeError("invalid admission tail")
        tail_s = 0
        for item in tail:
            if (
                not isinstance(item, list)
                or len(item) != 2
                or not isinstance(item[0], str)
                or not 0 < len(item[0]) <= 240
                or type(item[1]) not in (int, float)
                or not math.isfinite(item[1])
                or not 0 < item[1] <= 86400
            ):
                raise RuntimeError("invalid admission tail phase")
            tail_s += self.duration_hints.get((owner, item[0]), item[1])
        with self.cv:
            root = self.roots[owner]
            if (
                root.error
                or root.ended is not None
                or root.started is None
                or not self.ready[owner].is_set()
            ):
                raise RuntimeError(root.error or "owner is not running")
            if owner not in self.controllers:
                raise RuntimeError("covered root must not acquire recursively")
            self._drop_startup(owner)
            queued = time.monotonic() - self.origin
            row = dict(
                name=label,
                weight=weight,
                dur=self.duration_hints.get((owner, label), seconds),
                first=root.row.get("first", 0),
                bootstrap=bootstrap,
                queued=queued,
                deadline=min(due) - tail_s if due else None,
            )
            request = _Request(row, owner, queued=queued)
            self.pending.append(request)
            self.history.append(request)
            self.cv.notify_all()
            return request

    def _drop_startup(self, owner):
        root = self.roots[owner]
        if (
            owner not in self.controllers
            or root.error
            or root.started is None
            or root.ended is not None
        ):
            raise RuntimeError("startup owner is not a running coordinator")
        if root.row["weight"]:
            self._limit_owner(
                owner,
                sum(r.row["weight"] for r in self.active if r.owner == owner) - root.row["weight"],
            )
            startup = _Request(
                dict(root.row, name=owner + ":startup"),
                owner,
                started=root.started,
                ended=time.monotonic() - self.origin,
            )
            self.history.append(startup)
            self.free += root.row["weight"]
            root.row["weight"] = 0

    def _dispatch(self):
        try:
            self._schedule()
        except BaseException as exc:
            self.abort("admission dispatcher failed: " + repr(exc))

    def _schedule(self):
        with self.cv:
            while not self.stopped:
                now = time.monotonic() - self.origin
                rows = self.priority([r.row for r in self.pending])
                running = [
                    (r.started + r.row["dur"], r.row["weight"], bool(r.row.get("vm")))
                    for r in self.active
                    if r.row["weight"] or r.row.get("vm")
                ]
                chosen = pick_fair_units(
                    rows, self.free, self.free_vm, running, now, self.capacity, self.pick_units
                )
                for row in chosen:
                    request = next(r for r in self.pending if r.row is row)
                    self._limit_owner(
                        request.owner,
                        sum(r.row["weight"] for r in self.active if r.owner == request.owner)
                        + row["weight"],
                    )
                    self.pending.remove(request)
                    self.active.append(request)
                    self.free -= row["weight"]
                    self.free_vm -= bool(row.get("vm"))
                    request.started = now
                    request.event.set()
                self.cv.wait(timeout=1)

    def _release(self, request, *, rundown=False):
        with self.cv:
            if request not in self.active or request.ended is not None:
                raise RuntimeError("admission request already released")
            if self.roots[request.owner].error and not rundown:
                # A sibling can finish while owner cancellation is still killing work.
                # Acknowledge it, but fence every refund until confirmed owner rundown.
                return
            if not rundown:
                self._limit_owner(
                    request.owner,
                    sum(r.row["weight"] for r in self.active if r.owner == request.owner)
                    - request.row["weight"],
                )
            self.active.remove(request)
            self.free += request.row["weight"]
            self.free_vm += bool(request.row.get("vm"))
            request.ended = time.monotonic() - self.origin
            self.cv.notify_all()

    def cancel_owner(self, owner, reason):
        callback = None
        with self.cv:
            root = self.roots[owner]
            if not root.error:
                root.error = reason
                callback = self.cancel_callbacks.get(owner)
            for request in list(self.pending):
                if request.owner == owner:
                    self.pending.remove(request)
                    request.error = reason
                    request.ended = time.monotonic() - self.origin
                    request.event.set()
            self.ready[owner].set()
            self.cv.notify_all()
        if callback is not None:
            callback()  # outside the scheduler lock; tokens stay held until complete_root

    def complete_root(self, owner):
        """Caller guarantees its Job Object has zero active processes, even on failure."""
        with self.cv:
            root = self.roots[owner]
            if not root.error and any(
                r.owner == owner and not r.root for r in self.active + self.pending
            ):
                root.error = "owner exited with unfinished admission work"
            for request in list(self.active):
                if request.owner == owner:
                    self._release(request, rundown=True)
            for request in list(self.pending):
                if request.owner == owner:
                    self.pending.remove(request)
                    request.error = root.error or "owner completed before admission"
                    request.ended = time.monotonic() - self.origin
                    request.event.set()
            root.ended = time.monotonic() - self.origin
            self.ready[owner].set()
            self.cancel_callbacks.pop(owner, None)
            self.cpu_limits.pop(owner, None)
            self.cv.notify_all()
            return root.error

    def snapshot(self):
        with self.cv:
            return dict(
                capacity=self.capacity,
                free=self.free,
                free_vm=self.free_vm,
                cpu_bounded_roots=sorted(self.cpu_bounded),
                requests=[
                    dict(
                        owner=r.owner,
                        label=r.row["name"],
                        root=r.root,
                        credits=r.row["weight"],
                        estimated_s=r.row["dur"],
                        deadline_s=(
                            r.row["deadline"] - self.origin
                            if r.row.get("deadline") is not None
                            else None
                        ),
                        vm=bool(r.row.get("vm")),
                        queued=r.queued,
                        started=r.started,
                        ended=r.ended,
                        error=r.error,
                    )
                    for r in self.history
                ],
            )

    def abort(self, reason):
        with self.cv:
            self.stopped = True
            self.cv.notify_all()
        for owner in self.roots:
            try:
                self.cancel_owner(owner, reason)
            except (OSError, RuntimeError):
                # Keep capacity held. Other owners must still receive cancellation.
                pass

    def close(self):
        if any(root.ended is None for root in self.roots.values()):
            self.abort("gate admission shutting down")
        with self.cv:
            self.stopped = True
            self.cv.notify_all()
        self.dispatch_thread.join()
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join()


class ContainedProcess:
    """A startup-barrier child whose descendants cannot escape the gate's job."""

    def __init__(self, command, *, env, stdout, cwd):
        import win_job

        self.lock = threading.RLock()
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k32.QueryInformationJobObject.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self.k32.QueryInformationJobObject.restype = ctypes.c_int
        self.k32.TerminateJobObject.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.k32.TerminateJobObject.restype = ctypes.c_int
        self.proc = subprocess.Popen(
            [sys.executable, "-I", "-S", str(Path(__file__).resolve()), "--child", *command],
            env=env,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            cwd=cwd,
        )
        self.job = win_job.assign_kill_on_close(handle=int(self.proc._handle))
        if not self.job:
            self.proc.kill()
            self.proc.wait()
            raise RuntimeError("gate child could not be contained; startup barrier stays closed")

    def limit_cpu(self, credits):
        """Enforce a whole descendant tree's admitted share, before opening its barrier."""
        logical_cpus = os.cpu_count()
        if type(credits) is not int or credits < 1 or not logical_cpus:
            raise RuntimeError("invalid CPU-bound job allocation")
        rate = 10000 * credits // (CORE * logical_cpus)
        if not 0 < rate <= 10000:
            raise RuntimeError("CPU-bound allocation is outside machine capacity")

        class CpuRate(ctypes.Structure):
            _fields_ = [("flags", ctypes.c_uint32), ("rate", ctypes.c_uint32)]

        with self.lock:
            if not self.job:
                raise RuntimeError("CPU-bound job is already closed")
            self.k32.SetInformationJobObject.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_void_p,
                ctypes.c_uint32,
            ]
            self.k32.SetInformationJobObject.restype = ctypes.c_int
            info = CpuRate(0x1 | 0x4, rate)  # ENABLE | HARD_CAP, never weight-only.
            if not self.k32.SetInformationJobObject(
                self.job, 15, ctypes.byref(info), ctypes.sizeof(info)
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            applied = CpuRate()
            if not self.k32.QueryInformationJobObject(
                self.job, 15, ctypes.byref(applied), ctypes.sizeof(applied), None
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            if (applied.flags, applied.rate) != (info.flags, info.rate):
                raise RuntimeError("job CPU hard cap was not applied")

    def accounting(self):
        class Accounting(ctypes.Structure):
            _fields_ = [
                (name, ctypes.c_int64)
                for name in ("user", "kernel", "period_user", "period_kernel")
            ] + [(name, ctypes.c_uint32) for name in ("faults", "total", "active", "terminated")]

        with self.lock:
            info = Accounting()
            if not self.k32.QueryInformationJobObject(
                self.job, 1, ctypes.byref(info), ctypes.sizeof(info), None
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            return dict(
                active=info.active,
                processes=info.total,
                cpu_s=(info.user + info.kernel) / 10_000_000,
            )

    def cancel(self):
        with self.lock:
            if self.job and not self.k32.TerminateJobObject(self.job, 1):
                raise ctypes.WinError(ctypes.get_last_error())

    def finish(self):
        """Reap remaining owned tool workers, then return confirmed zero-active accounting."""
        import win_job

        with self.lock:
            info = self.accounting()
            reaped = info["active"]
            if reaped:
                self.cancel()
                deadline = time.monotonic() + 10
                while (info := self.accounting())["active"]:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(
                            "gate job rundown did not complete; capacity remains held"
                        )
                    time.sleep(0.01)
            self.proc.wait()
            win_job.close_job(self.job)
            self.job = None
            return dict(info, reaped_after_exit=reaped)


def _child(command):
    c = configuration()
    if c is None or not command:
        raise RuntimeError("gate child requires an owner and command")
    with _connection(c) as stream:
        _send(stream, dict(c, op="ready"))
        reply = _receive(stream)
        if reply.get("status") != "ready":
            raise RuntimeError("gate startup refused: " + str(reply.get("error")))
    return subprocess.call(command)


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "--child":
        raise SystemExit("internal gate child: --child COMMAND [ARG ...]")
    raise SystemExit(_child(sys.argv[2:]))
