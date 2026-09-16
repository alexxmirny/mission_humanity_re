"""Launch a process onto its own Windows DESKTOP OBJECT, so its windows cannot reach yours.

WHY. The rig's headless mode cannot keep the game's window unmapped. Measured 2026-08-02, in this
order and each by elimination rather than argument: the local `DDraw.dll` wrapper never touches the
window (one user32 import, `GetDCEx`, no delay imports, no API name strings to feed `GetProcAddress`);
the game never calls `ShowWindow`/`MoveWindow`/`SetForegroundWindow` after startup (the IAT swallow
counters read 0 for a whole run); the window is not re-created (the HWND is identical across every
intervention); and `no_window_hide=1` costs 8x wall clock, fails the scenario, and leaves the window
visible anyway. What is left is DirectDraw's own exclusive-mode window management, executed inside the
real ddraw that the wrapper loads dynamically -- through no import table reachable from the exe.

So stop hunting the caller. A window on a different desktop object cannot appear on the interactive
desktop, cannot take its focus, and cannot receive its input, **whatever** creates or shows it. That
makes this fix independent of the mechanism -- which matters, because three mechanism hypotheses have
already been wrong.

A SECOND PAYOFF WAS HOPED FOR AND IS **NOT** ESTABLISHED -- recorded because the temptation to claim
it was strong. The suite's `match_launch` failed in the 2026-08-02 loop with the client viewport
scrolled ~90 px, and the mechanism fits this fix beautifully: an RTS edge-scrolls when the cursor sits
at a screen edge, the game reads the real cursor (`SetCursorPos` is swallowed, `GetCursorPos` is not),
and a window that takes focus while the physical cursor rests near an edge would scroll exactly like
that. On an isolated desktop the cursor is that desktop's own and never moves.

It passed here (0.505% vs 24.985%). **But it also passed twice on the interactive desktop, and again
with the physical cursor pinned to the top screen edge.** Four passes, no reproduction: the failure is
intermittent and this change cannot be credited with fixing it. Attributing it would have been the
mirror image of a vacuous gate -- reading a green result as evidence about a condition the run never
actually contained. The edge-cursor test is weak evidence against the hypothesis rather than none: the
window sits parked at x=-25600, where the cursor never is, so an edge-scroll could only fire during
one of the brief windows in which something has moved it back to x=0.

WHAT THIS IS NOT. It is not a security boundary and not a sandbox -- same user, same session, same
window station. It only separates window/input namespaces.

THE HANDLE IS THE LIFETIME. A desktop is destroyed when the last handle to it closes and no threads
remain on it, so the launcher must hold the handle open for as long as the game runs. That is what
`desktop_session` is for; do not close it and expect the process to keep running.
"""

from __future__ import annotations

import contextlib
import ctypes
import os
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

GENERIC_ALL = 0x10000000
STARTF_USESHOWWINDOW = 0x00000001
SW_SHOWNORMAL = 1
CREATE_NEW_CONSOLE = 0x00000010
DEFAULT_DESKTOP = "mh_rig"


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]


user32.CreateDesktopW.restype = wintypes.HANDLE
user32.CreateDesktopW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.LPCWSTR,
    ctypes.c_void_p,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.c_void_p,
]
user32.CloseDesktop.restype = wintypes.BOOL
user32.CloseDesktop.argtypes = [wintypes.HANDLE]

kernel32.CreateProcessW.restype = wintypes.BOOL
kernel32.CreateProcessW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.LPWSTR,
    ctypes.c_void_p,
    ctypes.c_void_p,
    wintypes.BOOL,
    wintypes.DWORD,
    ctypes.c_void_p,
    wintypes.LPCWSTR,
    ctypes.POINTER(STARTUPINFOW),
    ctypes.POINTER(PROCESS_INFORMATION),
]


@contextlib.contextmanager
def desktop_session(name: str = DEFAULT_DESKTOP):
    """Create (or open) the named desktop and hold it open for the duration of the block.

    `CreateDesktopW` opens an existing desktop of the same name rather than failing, which is what
    makes concurrent lanes work: every peer's launcher asks for the same name and they share one
    desktop, each holding its own handle. The last one out destroys it.
    """
    h = user32.CreateDesktopW(name, None, None, 0, GENERIC_ALL, None)
    if not h:
        raise OSError(ctypes.get_last_error(), f"CreateDesktopW({name!r}) failed")
    try:
        yield name
    finally:
        user32.CloseDesktop(h)


_HELD: list[int] = []


def hold(name: str = DEFAULT_DESKTOP) -> str:
    """Create/open the desktop and keep it open until THIS PROCESS EXITS. Returns the name.

    The launcher form of `desktop_session`: a runner spawns peers and then blocks for the whole
    scenario, so scoping the handle to a `with` block would only add a place to get the nesting
    wrong. The handle is parked in a module global and released by the OS at process exit, which is
    exactly when the last peer is gone. Idempotent -- calling it twice just holds two handles.
    """
    h = user32.CreateDesktopW(name, None, None, 0, GENERIC_ALL, None)
    if not h:
        raise OSError(ctypes.get_last_error(), f"CreateDesktopW({name!r}) failed")
    _HELD.append(h)
    return name


def spawn(exe: str, args: str = "", cwd: str | None = None, desktop: str = DEFAULT_DESKTOP) -> int:
    """CreateProcess `exe` onto `desktop`. Returns the pid.

    Python's `subprocess` cannot do this: `lpDesktop` lives in STARTUPINFO and nothing in the stdlib
    exposes it, which is why this is raw ctypes rather than a `subprocess` keyword.

    The desktop must already exist and be held open (see `desktop_session`) -- CreateProcess onto a
    destroyed desktop fails with ERROR_INVALID_HANDLE rather than creating one.
    """
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(STARTUPINFOW)
    # `winsta0\<name>` rather than a bare name: a bare name is resolved against the CALLING process's
    # window station, which is right today and silently wrong the moment anything runs as a service.
    si.lpDesktop = rf"winsta0\{desktop}"
    si.dwFlags = STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWNORMAL
    pi = PROCESS_INFORMATION()
    # The command line must be a MUTABLE buffer -- CreateProcessW writes into it. Passing a Python
    # str for lpCommandLine works by accident until it doesn't; create_unicode_buffer is the contract.
    cmdline = ctypes.create_unicode_buffer(f'"{exe}" {args}'.strip())
    ok = kernel32.CreateProcessW(
        exe,
        cmdline,
        None,
        None,
        False,
        CREATE_NEW_CONSOLE,
        None,
        cwd or os.path.dirname(exe),
        ctypes.byref(si),
        ctypes.byref(pi),
    )
    if not ok:
        raise OSError(
            ctypes.get_last_error(), f"CreateProcessW({exe!r}) on desktop {desktop!r} failed"
        )
    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)
    return int(pi.dwProcessId)


if __name__ == "__main__":  # smoke test: park a process on the isolated desktop, then let it die
    import subprocess
    import sys
    import time

    # NOT notepad.exe, which this test used until 2026-08-27. On Win11 it is a packaged Store app
    # stub: the spawned pid hands off and exits by itself, measured 1 time in 4 within 3 s. The
    # assertion below would then be made about a process that is already gone -- vacuously green,
    # and the same shape produced a false CONFIRMED in a D15 desktop-lifetime probe. Use a process
    # whose lifetime we own.
    target = sys.argv[1] if len(sys.argv) > 1 else sys.executable
    args = "" if len(sys.argv) > 1 else '-c "import time; time.sleep(30)"'
    with desktop_session() as d:
        pid = spawn(target, args, desktop=d)
        print(
            f"spawned pid {pid} on desktop {d!r}; sleeping 3s -- it must NOT appear on your screen"
        )
        time.sleep(3)
        still = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True
        ).stdout
        # The point of the check: an ALREADY-DEAD peer proves nothing about window isolation.
        print(f"peer still running after 3s: {str(pid) in still}  <- must be True to mean anything")
        subprocess.run(["taskkill", "/PID", str(pid), "/F", "/T"], capture_output=True)
    print("desktop closed")
