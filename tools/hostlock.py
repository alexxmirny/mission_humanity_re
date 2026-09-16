#!/usr/bin/env python3
"""hostlock.py -- host-global advisory leases that serialize the shared SINGLETONS across git trees.

The migration loop can run in a `git worktree` (../mh_loop) while you work interactively in the main
tree. A worktree isolates the source, build outputs, commits and telemetry -- but NOT the two things
that are one-per-machine: the shared Ghidra DB's WRITES, and the rig (VMs / ports / desktop). This
gives those a cooperative lock BOTH trees see, because it lives at machine.SHARED_LOCK_DIR -- an
absolute path outside any repo tree, so a worktree and the main tree resolve the SAME lock.

    from hostlock import lease
    with lease("ghidra-write", "loop:s3"):
        save_and_checkin(...)          # held only for THIS call (seconds), then released

READS TAKE NO LOCK. Concurrent Ghidra reads are safe (that is the project's standing rule); only
WRITES serialize. So the loop and your interactive session both read the one shared DB freely, and
the lease brackets only the durable writers -- save_checkin / apply_annotations / commit_prototypes.

Human priority, the cheap way: a write lease is held for one tool call, so when you want to write you
wait seconds at most -- no preemption machinery needed. For the RIG, whose runs last minutes, a waiter
publishes a `want` marker and the LOOP checks wanted("rig") BEFORE starting a run and defers, so it
never makes you queue behind a determinism pass it had not started yet.

Crash safety: the holder's PID + a timestamp are in the lockfile. The next acquirer REAPS a lease
whose holder PID is dead, or (for the manual `hold` below, which has no live process) one older than a
safety TTL. A manual `hold` is the sticky lease the Ghidra write-lease CLI takes around a GUI edit.

CLI:
    python tools/hostlock.py status [name]        # what is held / wanted
    python tools/hostlock.py hold <name> [holder]  # take a STICKY lease (manual GUI edit)
    python tools/hostlock.py release <name> [holder]
    python tools/hostlock.py --selftest
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import sys
import time
from pathlib import Path

import machine_config as machine

LOCK_DIR = Path(machine.SHARED_LOCK_DIR)
LEASE_TTL = 600  # a live lease with a DEAD/unknown holder older than this is reaped (s)
STICKY_TTL = 1800  # a manual `hold` (no live process) older than this is reaped, with a warning (s)
WANT_TTL = 30  # a `want` marker older than this is treated as gone (the waiter left) (s)
POLL_S = 0.5


def _now() -> float:
    return time.time()


def _lock_path(name: str) -> Path:
    return LOCK_DIR / f"{name}.lock"


def _want_path(name: str) -> Path:
    return LOCK_DIR / f"{name}.want"


def _read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):  # absent, or a torn read mid-write -- both mean "nothing usable"
        return None


def _write_json_excl(path: Path, obj) -> bool:
    """Atomically create `path` with `obj`. False if it already exists (someone else holds it)."""
    try:
        fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except FileExistsError:
        return False
    try:
        os.write(fd, json.dumps(obj).encode("utf-8"))
    finally:
        os.close(fd)
    return True


def _write_json(path: Path, obj) -> None:
    tmp = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(obj), encoding="utf-8")
    os.replace(tmp, path)  # atomic on Windows + POSIX


def _pid_alive(pid) -> bool:
    """Is `pid` a live process? Conservative: an UNKNOWN result reads as alive, so a lease is never
    reaped out from under a holder we merely cannot query."""
    if not pid:
        return False
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if os.name == "nt":
        import ctypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            return False  # no such process (or gone)
        try:
            code = ctypes.c_ulong()
            if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
                return True  # cannot tell -> assume alive
            return code.value == STILL_ACTIVE
        finally:
            k32.CloseHandle(h)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # alive, just not ours
    except OSError:
        return True  # unknown -> assume alive


def _reap_if_stale(name: str) -> None:
    entry = _read_json(_lock_path(name))
    if not entry:
        return
    age = _now() - float(entry.get("at", 0) or 0)
    if entry.get("sticky"):
        if age > STICKY_TTL:
            sys.stderr.write(
                f"hostlock: reaping STALE manual hold on '{name}' "
                f"(held {age / 60:.0f} min by {entry.get('holder')!r}; > {STICKY_TTL / 60:.0f} min)\n"
            )
            _lock_path(name).unlink(missing_ok=True)
        return
    if not _pid_alive(entry.get("pid")) or age > LEASE_TTL:
        _lock_path(name).unlink(missing_ok=True)


def acquire(
    name: str,
    holder: str,
    *,
    sticky: bool = False,
    timeout: float | None = None,
    poll: float = POLL_S,
    polite: bool = False,
) -> None:
    """Block until this process holds the `name` lease. `timeout` (s) raises TimeoutError instead of
    waiting forever. `sticky` marks a manual hold (reaped by age, not by a dead PID).

    `polite=True` yields to any OTHER waiter: while someone else has a fresh `want` on this lease, a
    polite acquirer does not take it even when it is free. This is how the LOOP's rig runs defer to
    the interactive session -- the loop acquires `rig` politely (MH_RIG_POLITE=1), a human's rig tool
    does not, so when both queue for a free rig the human wins. It cannot preempt a run already in
    progress; nothing can.

    While waiting, publish a `want` (PID-keyed, so several waiters coexist) so a polite holder yields."""
    LOCK_DIR.mkdir(parents=True, exist_ok=True)
    deadline = None if timeout is None else _now() + timeout
    announced = False

    def _giveup_check():
        nonlocal announced
        if deadline is not None and _now() > deadline:
            if announced:
                _clear_want(name)
            held = held_by(name)
            raise TimeoutError(
                f"hostlock: '{name}' held by {held.get('holder') if held else '?'} after {timeout}s"
            )

    while True:
        _reap_if_stale(name)
        if polite and wanted(name) is not None:  # someone else is queued -- let them go first
            _set_want(name, holder)
            announced = True
            _giveup_check()
            time.sleep(poll)
            continue
        entry = {"pid": os.getpid(), "holder": holder, "at": _now(), "sticky": bool(sticky)}
        if _write_json_excl(_lock_path(name), entry):
            if announced:
                _clear_want(name)
            return
        _giveup_check()
        _set_want(name, holder)  # refresh each poll so `wanted()` stays fresh while we wait
        announced = True
        time.sleep(poll)


def release(name: str, holder: str) -> None:
    """Release the `name` lease if we still hold it. A no-op if it was reaped and retaken -- never
    yank a lease a different holder now owns."""
    entry = _read_json(_lock_path(name))
    if entry and entry.get("holder") == holder:
        _lock_path(name).unlink(missing_ok=True)


@contextlib.contextmanager
def lease(
    name: str,
    holder: str,
    *,
    timeout: float | None = None,
    poll: float = POLL_S,
    polite: bool = False,
):
    """`with lease("ghidra-write", "loop:s3"): ...` -- acquire, run the write, always release."""
    acquire(name, holder, timeout=timeout, poll=poll, polite=polite)
    try:
        yield
    finally:
        release(name, holder)


def held_by(name: str):
    """The current holder entry (after reaping a stale one), or None if free."""
    _reap_if_stale(name)
    return _read_json(_lock_path(name))


def _set_want(name: str, holder: str) -> None:
    d = _want_path(name)
    d.mkdir(parents=True, exist_ok=True)
    _write_json(d / f"{os.getpid()}.json", {"holder": holder, "pid": os.getpid(), "at": _now()})


def _clear_want(name: str) -> None:
    (_want_path(name) / f"{os.getpid()}.json").unlink(missing_ok=True)


def _wants(name: str):
    """Every fresh waiter on `name`, reaping stale/torn entries. PID-keyed so many coexist."""
    d = _want_path(name)
    if not d.exists():
        return []
    out = []
    for p in d.glob("*.json"):
        entry = _read_json(p)
        if not entry or _now() - float(entry.get("at", 0) or 0) > WANT_TTL:
            p.unlink(missing_ok=True)
            continue
        out.append(entry)
    return out


def wanted(name: str):
    """A fresh waiter OTHER than this process, if one is queued for `name`, else None. Drives the
    polite yield (the loop defers to an interactive session that wants the rig) and `status`."""
    for entry in _wants(name):
        if entry.get("pid") != os.getpid():
            return entry
    return None


# A rig tool that already holds the lease exports its PID here, so a rig tool it SPAWNS runs inside
# the parent's lease instead of queueing behind it. Set only while the lease is actually held.
RIG_LEASE_ENV = "MH_RIG_LEASE_HELD_BY"


def run_rig_tool(main_fn, tool: str):
    """__main__ wrapper for a rig tool (test_ui / mp_run / migration_sweep): run `main_fn()` while
    holding the host-global `rig` lease, so it never overlaps another rig run on the shared VMs /
    ports / desktop. The cheap OFFLINE flags (--list / --help) bypass the lease -- they must never
    wait on the rig. The loop sets MH_RIG_POLITE=1 in the env of the rig runs it launches, so those
    defer to an interactive session that wants the rig; a human's direct run is not polite and wins.

    RE-ENTRANCY, and it is not a nicety -- without it the DOCUMENTED GATE DEADLOCKS (measured
    2026-09-01). `test_ui.py --det-standard` holds the lease for the whole run, and its fourth shape
    (U32 conquest) SPAWNS `mp_run.py`, which came through here and waited forever for the lease its
    own parent was holding: `hostlock status` read `rig  lease by 'test_ui:13040' ... wanted by
    'mp_run:7556'`, both processes at 0% CPU. It had never gone red -- the run simply never finished,
    which is the worse failure: a gate that hangs looks like a gate that is still working.

    The pass-through is keyed on the LIVE HOLDER, not on the env var alone: the child runs
    lease-free only when the PID its parent exported is the pid the lockfile currently names. A
    stale variable inherited from a finished run therefore grants nothing -- such a child waits and
    acquires normally, which is the safe direction (an extra wait, never an unserialized rig run).
    The child does NOT re-export it under its own pid; the ancestor owns the lease and its release."""
    if {"--list", "-h", "--help"} & set(sys.argv[1:]):
        return main_fn()
    holder = f"{tool}:{os.getpid()}"
    cur = held_by("rig")
    inherited = os.environ.get(RIG_LEASE_ENV)
    if cur and inherited and str(cur.get("pid")) == inherited:
        sys.stderr.write(
            f"[rig] running inside {cur.get('holder')!r}'s lease (pid {inherited}) -- not re-acquiring\n"
        )
        return main_fn()
    if cur:
        sys.stderr.write(f"[rig] waiting for the rig lease -- held by {cur.get('holder')!r}...\n")
    with lease("rig", holder, polite=bool(os.environ.get("MH_RIG_POLITE"))):
        prev = os.environ.get(RIG_LEASE_ENV)
        os.environ[RIG_LEASE_ENV] = str(os.getpid())
        try:
            return main_fn()
        finally:
            if prev is None:
                os.environ.pop(RIG_LEASE_ENV, None)
            else:
                os.environ[RIG_LEASE_ENV] = prev


# ---- CLI ----------------------------------------------------------------------------------------


def _cmd_status(args) -> int:
    names = [args.name] if args.name else _known_names()
    if not names:
        print(f"no leases under {LOCK_DIR}")
        return 0
    for name in names:
        held = held_by(name)
        want = wanted(name)
        if held:
            age = _now() - float(held.get("at", 0) or 0)
            kind = "HOLD (manual)" if held.get("sticky") else "lease"
            print(
                f"{name:<16} {kind} by {held.get('holder')!r} pid={held.get('pid')} for {age:.0f}s"
            )
        else:
            print(f"{name:<16} free")
        if want:
            print(f"{'':<16}   wanted by {want.get('holder')!r}")
    return 0


def _known_names():
    if not LOCK_DIR.exists():
        return []
    return sorted(
        {p.stem for p in LOCK_DIR.glob("*.lock")} | {p.stem for p in LOCK_DIR.glob("*.want")}
    )


def _cmd_hold(args) -> int:
    holder = args.holder or "gui"
    acquire(args.name, holder, sticky=True, timeout=args.timeout)
    print(
        f"held '{args.name}' as {holder!r} (sticky). Release with: hostlock.py release {args.name}"
    )
    print(f"  safety: auto-reaped after {STICKY_TTL / 60:.0f} min if you forget.")
    return 0


def _cmd_release(args) -> int:
    holder = args.holder or "gui"
    entry = _read_json(_lock_path(args.name))
    if entry and entry.get("holder") != holder:
        print(f"'{args.name}' is held by {entry.get('holder')!r}, not {holder!r} -- not released.")
        return 1
    release(args.name, holder)
    print(f"released '{args.name}'")
    return 0


def _selftest() -> int:
    import tempfile

    global LOCK_DIR
    saved = LOCK_DIR
    LOCK_DIR = Path(tempfile.mkdtemp())
    fails = []

    def check(desc, ok):
        print(f"  [{'ok' if ok else 'XX'}] {desc}")
        if not ok:
            fails.append(desc)

    try:
        check("a free lease acquires", (acquire("t", "a"), held_by("t") is not None)[1])
        check("held_by names the holder", (held_by("t") or {}).get("holder") == "a")
        check("a second acquirer times out while it is held", _times_out("t", "b"))
        # a waiter that gave up clears its own want (nobody is queued any more)
        check("a timed-out waiter left no stale want", wanted("t") is None)
        release("t", "a")
        check("released -> free", held_by("t") is None)
        # want is PID-keyed: another process's want is visible; a stale one is reaped
        _forge_want("t", holder="human", pid=424242)
        check(
            "wanted() reports another process's want", (wanted("t") or {}).get("holder") == "human"
        )
        check(
            "a polite acquire DEFERS to that waiter on a free lock",
            _times_out("t", "loop", polite=True),
        )
        check("a non-polite acquire TAKES the free lock despite the want", _grabs("t", "greedy"))
        release("t", "greedy")
        _forge_want("t", holder="old", pid=424243, age=WANT_TTL + 1)
        check("a stale want is reaped and ignored", (wanted("t") or {}).get("holder") == "human")
        # dead-PID reap: forge a lease owned by a PID that cannot be alive
        _write_json(_lock_path("t"), {"pid": 2**31 - 1, "holder": "ghost", "at": _now()})
        check("a dead-holder lease is reaped on next look", held_by("t") is None)
        # sticky hold is NOT reaped by dead PID (its process is meant to be gone)
        _write_json(
            _lock_path("t"), {"pid": 2**31 - 1, "holder": "gui", "at": _now(), "sticky": True}
        )
        check("a fresh sticky HOLD survives a dead PID", held_by("t") is not None)
        _write_json(
            _lock_path("t"),
            {"pid": 2**31 - 1, "holder": "gui", "at": _now() - STICKY_TTL - 1, "sticky": True},
        )
        check("a stale sticky HOLD is reaped by age", held_by("t") is None)
        with lease("t", "ctx"):
            inside = held_by("t") is not None
        check("the lease() context holds then releases", inside and held_by("t") is None)

        # ---- run_rig_tool's re-entrancy (the 2026-09-01 --det-standard deadlock) -----------------
        # Both directions, because only the pair distinguishes "the nested tool ran" from "the guard
        # is a no-op that lets anything through".
        saved_env = os.environ.pop(RIG_LEASE_ENV, None)
        saved_argv = sys.argv
        sys.argv = ["mp_run.py"]  # no --list/--help, so the offline bypass is not what is measured
        try:
            seen = {}

            def _parent():
                seen["parent_holder"] = (held_by("rig") or {}).get("holder") or ""
                seen["parent_env"] = os.environ.get(RIG_LEASE_ENV)
                # the SPAWNED child, simulated in-process: same env, same lockfile
                run_rig_tool(lambda: seen.update(child_ran=True), "mp_run")
                return 0

            run_rig_tool(_parent, "test_ui")
            check("a nested rig tool RUNS instead of deadlocking", seen.get("child_ran") is True)
            check(
                "the parent's lease was real", seen.get("parent_holder", "").startswith("test_ui:")
            )
            check(
                "the holder exports its pid for children",
                seen.get("parent_env") == str(os.getpid()),
            )
            check(
                "the env var is cleared once the lease is released", RIG_LEASE_ENV not in os.environ
            )

            # A STALE variable (naming a pid that is not the live holder) must NOT grant the bypass:
            # with the lease held by someone else, such a child has to wait and time out.
            os.environ[RIG_LEASE_ENV] = "424244"
            acquire("rig", "someone-else")
            check(
                "a STALE lease env grants nothing -- the child still waits",
                _times_out("rig", "mp_run"),
            )
            release("rig", "someone-else")
        finally:
            sys.argv = saved_argv
            os.environ.pop(RIG_LEASE_ENV, None)
            if saved_env is not None:
                os.environ[RIG_LEASE_ENV] = saved_env
    finally:
        LOCK_DIR = saved
    print("hostlock selftest: " + ("PASS" if not fails else f"FAIL ({len(fails)})"))
    return 0 if not fails else 1


def _times_out(name, holder, polite=False):
    try:
        acquire(name, holder, timeout=0.3, poll=0.05, polite=polite)
        release(name, holder)
        return False
    except TimeoutError:
        return True


def _grabs(name, holder):
    try:
        acquire(name, holder, timeout=0.3, poll=0.05)
        return True
    except TimeoutError:
        return False


def _forge_want(name, holder, pid, age=0):
    """Write a want entry as if from another process `pid` (selftest only)."""
    d = _want_path(name)
    d.mkdir(parents=True, exist_ok=True)
    _write_json(d / f"{pid}.json", {"holder": holder, "pid": pid, "at": _now() - age})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true", help="run the self-test and exit")
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("status", help="what is held / wanted")
    s.add_argument("name", nargs="?")
    s.set_defaults(fn=_cmd_status)
    h = sub.add_parser("hold", help="take a STICKY lease (manual GUI edit)")
    h.add_argument("name")
    h.add_argument("holder", nargs="?")
    h.add_argument("--timeout", type=float, default=120)
    h.set_defaults(fn=_cmd_hold)
    r = sub.add_parser("release", help="release a lease you hold")
    r.add_argument("name")
    r.add_argument("holder", nargs="?")
    r.set_defaults(fn=_cmd_release)
    a = ap.parse_args()
    if a.selftest:
        return _selftest()
    if not getattr(a, "fn", None):
        ap.print_help()
        return 2
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
