"""win_probe.py -- is the game window still a normal, switchable top-level window? (PT-GFX8)

Dumps, for every top-level window of the game's class (or one pid / hwnd), the facts that decide whether
Windows lists it on the taskbar and in the Alt-Tab switcher: visibility, iconic/zoomed, style and
ex-style bits, owner, DWM cloaking, and the Alt-Tab eligibility verdict computed by the documented rule
(Raymond Chen, "Which windows appear in the Alt+Tab list?"): visible, not cloaked, and either
unowned-without-TOOLWINDOW or WS_EX_APPWINDOW. The taskbar button follows the same rule (the shell
creates it from HSHELL_WINDOWCREATED, which it receives for exactly the eligible windows).

    python tools/win_probe.py                 # one snapshot, all windows of class "Mission: Humanity Class"
    python tools/win_probe.py --pid 1234 --watch 0.25 --seconds 60   # print every CHANGE
    python tools/win_probe.py --json          # one JSON line (what the harness rows assert on)
    python tools/win_probe.py --activate HWND # restore the way the launcher does (SetForegroundWindow)

Importable: snapshot_all(pid=None) -> list[dict]; eligible(info) -> bool.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
try:
    dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
except OSError:  # pragma: no cover
    dwmapi = None

GAME_CLASS = "Mission: Humanity Class"

GWL_STYLE, GWL_EXSTYLE = -16, -20
GW_OWNER = 4
DWMWA_CLOAKED = 14
WS_EX_TOOLWINDOW, WS_EX_APPWINDOW = 0x80, 0x40000

user32.GetWindowLongW.restype = ctypes.c_long
user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindow.restype = wintypes.HWND
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetForegroundWindow.restype = wintypes.HWND
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsIconic.argtypes = [wintypes.HWND]
user32.IsZoomed.argtypes = [wintypes.HWND]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetAncestor.restype = wintypes.HWND
user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]


def _class(h) -> str:
    buf = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(h, buf, 128)
    return buf.value


def _cloaked(h) -> int:
    if dwmapi is None:
        return 0
    v = wintypes.DWORD(0)
    if dwmapi.DwmGetWindowAttribute(wintypes.HWND(h), DWMWA_CLOAKED, ctypes.byref(v), 4) != 0:
        return 0
    return int(v.value)


def eligible(i: dict) -> bool:
    """Alt-Tab / taskbar eligibility by the documented rule."""
    if not i["visible"] or i["cloaked"]:
        return False
    if i["ex"] & WS_EX_APPWINDOW:
        return True
    if i["ex"] & WS_EX_TOOLWINDOW:
        return False
    return not i["owner"]


def describe(h) -> dict:
    pid = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    r = wintypes.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    info = {
        "hwnd": int(h),
        "pid": int(pid.value),
        "cls": _class(h),
        "visible": bool(user32.IsWindowVisible(h)),
        "iconic": bool(user32.IsIconic(h)),
        "zoomed": bool(user32.IsZoomed(h)),
        "style": user32.GetWindowLongW(h, GWL_STYLE) & 0xFFFFFFFF,
        "ex": user32.GetWindowLongW(h, GWL_EXSTYLE) & 0xFFFFFFFF,
        "owner": int(user32.GetWindow(h, GW_OWNER) or 0),
        "cloaked": _cloaked(h),
        "rect": [r.left, r.top, r.right, r.bottom],
        "foreground": int(user32.GetForegroundWindow() or 0) == int(h),
    }
    info["alttab"] = eligible(info)
    info["taskbar_over"] = taskbar_over(h, r)
    return info


GA_ROOT = 2
user32.WindowFromPoint.restype = wintypes.HWND
user32.WindowFromPoint.argtypes = [wintypes.POINT]
user32.GetAncestor.restype = wintypes.HWND
user32.FindWindowW.restype = wintypes.HWND


def taskbar_over(h, r) -> int:
    """1 if the taskbar is DRAWN OVER the game window, 0 if not, -1 if it cannot be said.

    rc7 (2026-10-05) told the shell the borderless window was NOT fullscreen and the taskbar stayed on top
    of the game for the whole session. The test is what the screen shows: hit-test the middle of the part
    of the taskbar that overlaps the game window -- whose top-level window is there, the game's or the
    taskbar's? -1 when there is no taskbar on this desktop (the rig) or it does not overlap the window."""
    tray = user32.FindWindowW("Shell_TrayWnd", None)
    if not tray or not user32.IsWindowVisible(tray):
        return -1
    t = wintypes.RECT()
    user32.GetWindowRect(tray, ctypes.byref(t))
    left, top = max(t.left, r.left), max(t.top, r.top)
    right, bottom = min(t.right, r.right), min(t.bottom, r.bottom)
    if right <= left or bottom <= top:
        return -1
    pt = wintypes.POINT((left + right) // 2, (top + bottom) // 2)
    hit = user32.WindowFromPoint(pt)
    root = user32.GetAncestor(hit, GA_ROOT) if hit else None
    if not root:
        return -1
    if int(root) == int(h):
        return 0
    return 1 if _class(root) == "Shell_TrayWnd" else 0


def snapshot_all(pid: int | None = None, cls: str = GAME_CLASS) -> list[dict]:
    out: list[dict] = []
    cb_t = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, _l):
        d = describe(h)
        if (pid is not None and d["pid"] == pid) or (pid is None and d["cls"] == cls):
            out.append(d)
        return True

    user32.EnumWindows(cb_t(cb), 0)
    return out


def fmt(d: dict) -> str:
    return (
        "hwnd=%08x pid=%d vis=%d iconic=%d fg=%d style=%08x ex=%08x owner=%x cloaked=%d rect=%s ALTTAB=%d TASKBAR_OVER=%d"
        % (
            d["hwnd"],
            d["pid"],
            d["visible"],
            d["iconic"],
            d["foreground"],
            d["style"],
            d["ex"],
            d["owner"],
            d["cloaked"],
            d["rect"],
            d["alttab"],
            d["taskbar_over"],
        )
    )


def activate_like_launcher(h: int) -> bool:
    """What mh.exe's own single-instance path does: SetForegroundWindow, nothing else (no SW_RESTORE)."""
    return bool(user32.SetForegroundWindow(h))


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--pid", type=int)
    ap.add_argument("--cls", default=GAME_CLASS)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--watch", type=float, default=0.0, help="poll period s; print changes only")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--activate", type=lambda s: int(s, 0))
    a = ap.parse_args()
    if a.activate:
        print("SetForegroundWindow ->", activate_like_launcher(a.activate))
        return 0
    if a.watch <= 0:
        s = snapshot_all(a.pid, a.cls)
        if a.json:
            print(json.dumps(s))
        else:
            print("\n".join(fmt(d) for d in s) or "(no window)")
        return 0 if s else 1
    last = None
    end = time.time() + a.seconds
    while time.time() < end:
        s = snapshot_all(a.pid, a.cls)
        key = [fmt(d) for d in s]
        if key != last:
            print(
                "[%s] %s" % (time.strftime("%H:%M:%S"), " | ".join(key) or "(no window)"),
                flush=True,
            )
            last = key
        time.sleep(a.watch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
