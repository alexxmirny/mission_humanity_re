"""Windows Job-Object kill-on-close, shared by EVERY local child process this rig launches.

WHY THIS EXISTS (tooling:TL-SUITE-TEARDOWN). TL-RIG6 built this mechanism once, for
tools/net_shim.py alone: a runner killed by test_ui.py's per-test timeout
(`subprocess.run(cmd, ..., timeout=timeout)`) has ONLY its direct child (ui_test.py) torn down by
CPython -- TerminateProcess, no cooperation from ui_test.py's own exit path, so nothing IT spawned
(the shim, a game peer, a relay) is told anything. `finally:`/`atexit`/`shim_stop()` never run.
TL-SUITE-TEARDOWN generalises the SAME fix to every other local child the rig owns (the game peers
`peer_launch` launches, `tools/desktop.py`'s isolated-desktop CreateProcessW path, the relay/shim
processes `tools/test_ui.py` starts) instead of growing a second copy per call site -- see
`assign_kill_on_close` below.

THE MECHANISM. A fresh Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, with the child
assigned to it right after it is spawned, and the job handle kept alive for exactly as long as
the LAUNCHING process wants the child tied to its own lifetime (module-global list, same shape as
tools/desktop.py's own `_HELD`). Windows closes every handle a process owns when that process
exits BY ANY MEANS -- clean exit, an unhandled exception, `os._exit`, `TerminateProcess` from
outside -- so the job's last handle closing fires the kill with no code in the dying process
needing to run at all. That is what makes it survive exactly the shape a per-test timeout takes.

WHAT THIS IS NOT. Not a substitute for a normal, cooperative teardown (`proc.terminate()` /
`shim_stop()` / `peer_kill()`) -- those still run first, in the common case, and are still what
should confirm the child is actually GONE before a caller returns (see `wait_gone` below). This is
the backstop for when that code never gets to run.
"""

from __future__ import annotations

import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

# ---- process-liveness primitives (moved here from tools/ui_test.py, TL-RIG6 -> TL-SUITE-TEARDOWN;
#      unchanged in behaviour, just no longer private to one call site) ----------------------------


def _filetime_to_epoch(ft):
    t = (ft.dwHighDateTime << 32) | ft.dwLowDateTime
    return t / 10_000_000.0 - 11644473600.0  # 100ns ticks since 1601-01-01 -> Unix epoch seconds


def process_still_running(pid):
    """True iff `pid` is a process that is ACTUALLY STILL RUNNING; False if it can be opened but has
    already exited; None if it cannot be opened at all (fully gone, or never existed).

    OpenProcess succeeding is NOT "is it running" -- Windows keeps a process's kernel object alive,
    and OpenProcess-able, for as long as ANY handle anywhere still references it (including a
    deliberately-retained exit-code handle -- see ui_test.py's retain_exit_handle). GetExitCodeProcess's
    STILL_ACTIVE sentinel is the only thing that actually distinguishes the two.
    """
    try:
        k32 = ctypes.windll.kernel32
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not h:
            return None
        try:
            code = ctypes.c_ulong(0)
            if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
                return None
            return code.value == STILL_ACTIVE
        finally:
            k32.CloseHandle(h)
    except Exception:  # a diagnostic must never be the thing that fails a run
        return None


def process_created_at(pid):
    """The Unix-epoch creation time of `pid`'s CURRENT kernel object, or None if it cannot be opened.

    Deliberately NOT a liveness check (see process_still_running for that) -- this exists only to
    catch PID REUSE: a dead process's pid can be handed to an unrelated process later, and that
    process opens fine but was created at a different time.
    """
    try:

        class FILETIME(ctypes.Structure):
            _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

        k32 = ctypes.windll.kernel32
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not h:
            return None
        try:
            creation, exit_t, kernel_t, user_t = FILETIME(), FILETIME(), FILETIME(), FILETIME()
            if not k32.GetProcessTimes(
                h,
                ctypes.byref(creation),
                ctypes.byref(exit_t),
                ctypes.byref(kernel_t),
                ctypes.byref(user_t),
            ):
                return None
            return _filetime_to_epoch(creation)
        finally:
            k32.CloseHandle(h)
    except Exception:  # a diagnostic must never be the thing that fails a run
        return None


def process_terminate(pid):
    try:
        k32 = ctypes.windll.kernel32
        PROCESS_TERMINATE = 0x0001
        h = k32.OpenProcess(PROCESS_TERMINATE, False, int(pid))
        if not h:
            return False
        try:
            return bool(k32.TerminateProcess(h, 1))
        finally:
            k32.CloseHandle(h)
    except Exception:
        return False


def wait_gone(pid, timeout=5.0, poll=0.25):
    """Block until `pid` is no longer running (or `timeout` elapses). Returns True iff it is gone.

    tooling:TL-SUITE-TEARDOWN -- a caller's verdict path must know a peer is ACTUALLY down before it
    returns, not merely that it sent a kill: `Stop-Process`/`proc.terminate()` return as soon as the
    signal is ISSUED, not once Windows has torn the process down."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not process_still_running(pid):
            return True
        time.sleep(poll)
    return not process_still_running(pid)


# ---- the job object itself -------------------------------------------------------------------


class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_longlong),
        ("PerJobUserTimeLimit", ctypes.c_longlong),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.c_void_p),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        (n, ctypes.c_ulonglong)
        for n in (
            "ReadOperationCount",
            "WriteOperationCount",
            "OtherOperationCount",
            "ReadTransferCount",
            "WriteTransferCount",
            "OtherTransferCount",
        )
    ]


class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", _IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
_JobObjectExtendedLimitInformation = 9
_PROCESS_SET_QUOTA, _PROCESS_TERMINATE = 0x0100, 0x0001

# Handles kept alive so the OS never garbage-collects a job we still want kill-on-close for. A
# caller (ui_test.py, desktop.py, test_ui.py) that wants a NARROWER lifetime than "lives as long as
# this interpreter does" holds the returned handle itself and calls close_job() -- see shim_stop()
# for the existing pattern this preserves. This list is only a backstop for callers that don't.
_HELD_JOBS: list[int] = []


def assign_kill_on_close(pid=None, handle=None):
    """Assign a process to a fresh Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, owned by
    THIS process. Returns the job handle -- keep it alive (e.g. on the Popen object, like TL-RIG6's
    `proc.rig6_job`) for exactly as long as the child should die with this process; closing it
    early (once the child is already confirmed gone, e.g. after a clean stop) is safe and does not
    itself kill anything. Returns None if the job could not be created/assigned (e.g. no
    permission) -- never fatal: the child just runs unprotected by this mechanism, same as before
    it existed.

    Pass `handle` when the caller already has one open FROM CreateProcess (tools/desktop.py's raw
    CreateProcessW path) -- it avoids a PID-reuse race that opening a fresh handle from a bare pid
    has no way to close (the pid could, in principle, already have been recycled by the time this
    function opens it). Pass `pid` otherwise (net_shim.py, a `Start-Process -PassThru` peer, a
    `subprocess.Popen` result you only trust the reported pid of).
    """
    try:
        k32 = ctypes.windll.kernel32
        job = k32.CreateJobObjectW(None, None)
        if not job:
            return None
        info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not k32.SetInformationJobObject(
            job, _JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)
        ):
            k32.CloseHandle(job)
            return None
        owns_handle = handle is None
        h = handle
        if h is None:
            h = k32.OpenProcess(_PROCESS_SET_QUOTA | _PROCESS_TERMINATE, False, int(pid))
            if not h:
                k32.CloseHandle(job)
                return None
        try:
            if not k32.AssignProcessToJobObject(job, h):
                k32.CloseHandle(job)
                return None
        finally:
            if owns_handle:
                k32.CloseHandle(h)
        return job
    except Exception:
        return None


def close_job(job):
    """Close a job handle from assign_kill_on_close. Safe to call on None. Only closes a handle
    THIS process holds -- if the protected child is already gone (the normal, cooperative-stop
    case), closing triggers no kill of anything; if it is not, this DOES kill it (that is the
    point when the caller wants "stop tracking, and take the child down if it is somehow still
    up" in one call)."""
    if not job:
        return
    try:
        ctypes.windll.kernel32.CloseHandle(job)
    except Exception:
        pass


# ---- TL-SUITE-TEARDOWN: name the holder of a locked provisioning file, don't just raise ----------


def find_process_under(dir_path, proc_name=None, timeout=10):
    """Best-effort: (pid, executable_path) of a RUNNING process whose own executable lies inside
    `dir_path` (optionally restricted to `proc_name`), or None.

    This is a LOCATION inference, not a file-handle proof: it never asks "does this process have
    THIS FILE open", only "does this process live in the folder that owns the file". That is
    still the right question for a lane's mh.dll -- the lane's OWN mh.focus.exe is the only thing
    under this project that ever has it open (a loaded module), so a process whose own exe sits
    inside the lane folder IS the holder in every case this project produces. psutil (not in
    tools/requirements.txt today) could answer the stronger question directly via
    Process.open_files(); this is the documented fallback for when it is not installed.
    """
    filt = " -Filter \"Name='%s'\"" % proc_name.replace("'", "''") if proc_name else ""
    try:
        r = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "Get-CimInstance Win32_Process%s | Select-Object ProcessId,ExecutablePath "
                "| ConvertTo-Json -Compress" % filt,
            ],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except Exception:
        return None
    out = (r.stdout or "").strip()
    if not out:
        return None
    try:
        rows = json.loads(out)
    except ValueError:
        return None
    if isinstance(rows, dict):
        rows = [rows]
    dir_abs = os.path.normcase(os.path.abspath(dir_path)) + os.sep
    for row in rows:
        exe = row.get("ExecutablePath") or ""
        if not exe:
            continue
        if os.path.normcase(os.path.abspath(exe)).startswith(dir_abs):
            pid = row.get("ProcessId")
            if pid is not None:
                return int(pid), exe
    return None


def find_file_holder(path):
    """Best-effort: (pid, name, exe) of a process that currently has `path` OPEN (a real handle
    proof, unlike find_process_under's location inference), or None.

    This is deliberately a documented DEAD END, not a psutil call: psutil is not in
    tools/requirements.txt today, and lint_requirements.py (tools/lint_repo.py) refuses an
    unpinned third-party import on sight -- an `import psutil` here, even guarded by
    `try/except ImportError`, is still a static import of an unpinned distribution and reds the
    gate the moment this file is scanned, whether or not psutil is actually installed. So this
    always returns None; find_process_under (below) is the real, dependency-free answer this
    project's own call sites use. Upgrading this to a real psutil-backed handle proof needs BOTH
    an `import psutil` here AND a pinned entry in tools/requirements.txt added in the same change,
    or the gate goes red on the import alone."""
    return None


def copy_or_refuse(src, dst, lane_dir, proc_name="mh.focus.exe"):
    """shutil.copy(src, dst); a PermissionError becomes a REFUSAL naming the process holding `dst`
    open, instead of propagating a bare PermissionError three frames from anything that says whose
    lane this was or which run still owned it (tooling:TL-SUITE-TEARDOWN -- a share_lanes row's
    provision step used to die at exactly this copy with nothing but
    "PermissionError: [WinError 32] ..." once an earlier row's peer outlived its own verdict).

    Raises SystemExit on any failure (named holder or not); returns None on success, like
    shutil.copy."""
    try:
        shutil.copy(src, dst)
    except PermissionError as e:
        holder = find_file_holder(dst) or find_process_under(lane_dir, proc_name)
        if holder:
            pid = holder[0]
            what = (
                holder[2]
                if len(holder) > 2 and holder[2]
                else (holder[1] if len(holder) > 1 else "")
            )
            raise SystemExit(
                "REFUSED: cannot provision %s -- PID %s (%s) still has it open. A prior run's peer "
                "was not torn down before this lane was reused; the caller must kill it (or wait for "
                "it) before retrying. See tracker:TL-SUITE-TEARDOWN." % (dst, pid, what)
            ) from e
        raise SystemExit(
            "REFUSED: cannot provision %s (%s) -- could not identify the holder: no running %s has "
            "an executable under %s, and psutil (which could answer this by file handle instead of "
            "by process location) is not installed. Check `Get-Process %s` by hand."
            % (dst, e, proc_name, lane_dir, os.path.splitext(proc_name)[0])
        ) from e


# ---- offline selftest (tooling:TL-SUITE-TEARDOWN; no rig, no game) --------------------------------


def _selftest_kill_on_close():
    """ARM: a child assigned to a kill-on-close job dies within a few seconds of its PARENT's
    ABRUPT exit (os._exit -- no `finally`, no `atexit`, exactly what a `TerminateProcess`-from-
    outside per-test timeout does to a runner). MUTATION: the same shape with NO job assigned
    proves the child would otherwise have survived, so the ARM result is not vacuous."""
    here = os.path.dirname(os.path.abspath(__file__))

    def run(use_job):
        script = (
            "import subprocess, sys, os\n"
            "sys.path.insert(0, %r)\n"
            "import win_job\n"
            # DEVNULL, not inherited: an inherited stdout pipe stays open in the GRANDCHILD for as
            # long as IT lives, which (in the no-job MUTATION arm) is the full 60s -- and the
            # harness's own subprocess.run(capture_output=True) below blocks on THAT handle closing,
            # not on the immediate parent's os._exit(0). Without this the mutation arm always timed
            # out instead of proving the child survives.
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], "
            "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
            "%s\n"
            "print(child.pid, flush=True)\n"
            "os._exit(0)\n"
        ) % (here, "win_job.assign_kill_on_close(child.pid)" if use_job else "pass")
        out = subprocess.run(
            [sys.executable, "-c", script], capture_output=True, text=True, timeout=10
        )
        lines = [ln for ln in out.stdout.splitlines() if ln.strip()]
        if not lines:
            raise RuntimeError("parent produced no pid (stderr: %s)" % out.stderr[-400:])
        return int(lines[-1])

    arm_pid = run(True)
    arm_gone = wait_gone(arm_pid, timeout=5, poll=0.2)
    if not arm_gone:
        process_terminate(arm_pid)
        return (
            False,
            "ARM: child pid %d still alive 5s after its job-assigned parent os._exit(0)'d"
            % arm_pid,
        )

    mut_pid = run(False)
    mut_gone = wait_gone(mut_pid, timeout=2, poll=0.2)
    process_terminate(mut_pid)  # clean up regardless of the outcome
    if mut_gone:
        return (
            False,
            "MUTATION: child pid %d died even with NO job assigned -- proves nothing" % mut_pid,
        )
    return True, ""


def _selftest_copy_refusal():
    """A file held open by a REAL child process (not psutil -- the documented find_process_under
    fallback) is named in copy_or_refuse's message, not raised as a bare PermissionError."""
    import tempfile

    tmp = tempfile.mkdtemp(prefix="win_job_selftest_")
    proc = None
    try:
        lane = os.path.join(tmp, "lane")
        os.makedirs(lane)
        fake_exe = os.path.join(lane, "mh.focus.exe")  # a real, runnable exe under the fake lane
        shutil.copy(sys.executable, fake_exe)
        locked = os.path.join(lane, "mh.dll")
        with open(locked, "wb") as f:
            f.write(b"placeholder")
        # Hold `locked` open with NO SHARING -- Python's own open() shares read/write by default
        # and would NOT reproduce the PermissionError a loaded DLL under a real lane produces.
        holder_script = (
            "import ctypes, sys, time\n"
            "GENERIC_WRITE = 0x40000000\n"
            "OPEN_EXISTING = 3\n"
            "h = ctypes.windll.kernel32.CreateFileW(sys.argv[1], GENERIC_WRITE, 0, None, "
            "OPEN_EXISTING, 0, None)\n"
            "if h == -1:\n"
            "    sys.exit('open failed: %s' % ctypes.get_last_error())\n"
            "print('locked', flush=True)\n"
            "time.sleep(30)\n"
        )
        proc = subprocess.Popen(
            [fake_exe, "-c", holder_script, locked], stdout=subprocess.PIPE, text=True
        )
        line = proc.stdout.readline()
        if line.strip() != "locked":
            return False, "child did not confirm the lock (got %r)" % line
        src = os.path.join(tmp, "source.dll")
        with open(src, "wb") as f:
            f.write(b"new bytes")
        try:
            copy_or_refuse(src, locked, lane)
            return False, "copy_or_refuse did not raise for a file a child holds open"
        except SystemExit as e:
            msg = str(e)
            if str(proc.pid) not in msg:
                return False, "refusal did not name the holder's pid %d: %r" % (proc.pid, msg)
            return True, ""
    finally:
        if proc is not None:
            proc.kill()
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
        shutil.rmtree(tmp, ignore_errors=True)


def selftest():
    """`python tools/win_job.py --selftest` -- offline (no rig, no game); see tooling:TL-SUITE-TEARDOWN."""
    fails = []
    for name, fn in (
        ("kill-on-close: ARM (job) dies, MUTATION (no job) survives", _selftest_kill_on_close),
        ("copy_or_refuse names a real child's held-open file", _selftest_copy_refusal),
    ):
        try:
            ok, why = fn()
        except Exception as e:  # a selftest that crashes is a FAIL, not a hang or a traceback dump
            ok, why = False, "raised %r" % (e,)
        print("   %-70s %s" % (name, "ok" if ok else ("FAIL: " + why)))
        if not ok:
            fails.append(name)
    print("win_job.selftest: %s" % ("PASS" if not fails else ("FAIL: " + ", ".join(fails))))
    return 0 if not fails else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    print(__doc__.splitlines()[0])
    sys.exit(0)
