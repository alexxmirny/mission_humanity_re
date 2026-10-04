//
// imgui_bind_selftest.cpp -- `net_selftest.exe imguibindtest`: PT-GFX4, the ImGui overlay's WINDOW
// BINDING (gfx/overlay_imgui.cpp). The subclass follows the presenter's window: a different HWND
// unlinks the old window and links the new one, detach() unlinks, a window something else subclassed
// on top of us is left forwarding (a chain cannot be unlinked from the middle), and WM_NCDESTROY
// unlinks a dying window.
//
// A suite and not a rig check: the shipped game has one main window, never hands the presenter a
// second one, and exits without destroying it, so none of these paths runs in any UI scenario.
//
// Driven through the real entry points (arm / on_frame / on_detach / query) on two message-only
// windows. The overlay stays CLOSED throughout, which is the contract that makes the fake device
// pointers safe: a closed overlay binds the window and polls its toggle, and returns before it
// touches the device.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

#include "gfx/overlay_imgui.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

constexpr UINT PING         = WM_APP + 0x31;
int            g_pings_base = 0; // PINGs that reached the windows' OWN procedure
WNDPROC        g_over_prev  = nullptr;
int            g_pings_over = 0; // PINGs seen by the proc subclassed on top of the overlay's

LRESULT CALLBACK base_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == PING) {
        ++g_pings_base;
        return 0x5a;
    }
    return DefWindowProcA(h, m, w, l);
}

LRESULT CALLBACK over_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == PING) ++g_pings_over;
    return CallWindowProcA(g_over_prev, h, m, w, l);
}

WNDPROC proc_of(HWND h) { return (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC); }

namespace ov = mh::gfx::imgui_overlay;

// Fake presenter objects. Never dereferenced while the overlay is closed (see the file comment).
ID3D11Device *const        FAKE_DEV = reinterpret_cast<ID3D11Device *>(static_cast<uintptr_t>(0x10));
ID3D11DeviceContext *const FAKE_CTX = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(0x20));

} // namespace

int run_imguibindtest() {
    printf("=== imguibindtest (PT-GFX4: the ImGui overlay's window binding) ===\n");

    // Arm from a throwaway ini: [video] imgui=1 is the only key the binding needs.
    char dir[MAX_PATH], ini[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    wsprintfA(ini, "%smh_imguibindtest_%lu.ini", dir, GetCurrentProcessId());
    WritePrivateProfileStringA("video", "imgui", "1", ini);
    check("0: armed from [video] imgui=1", ov::arm(ini) && ov::query(ov::query_what::armed) == 1);
    DeleteFileA(ini);

    WNDCLASSA wc     = {};
    wc.lpfnWndProc   = base_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "mh_imguibindtest";
    RegisterClassA(&wc);
    HWND h1 = CreateWindowExA(0, wc.lpszClassName, "a", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    HWND h2 = CreateWindowExA(0, wc.lpszClassName, "b", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    check("0: two windows", h1 && h2);
    if (!h1 || !h2) return 1;
    const WNDPROC base = proc_of(h1);

    // A. first bind: h1 carries the subclass, messages still reach the window's own proc.
    ov::on_frame(FAKE_DEV, FAKE_CTX, h1);
    check("A1: h1 subclassed", proc_of(h1) != base && ov::query(ov::query_what::subclassed) == 1);
    check("A2: overlay stays closed", ov::query(ov::query_what::open) == 0);
    check("A3: a message to h1 still reaches its own proc", SendMessageA(h1, PING, 0, 0) == 0x5a && g_pings_base == 1);
    ov::on_frame(FAKE_DEV, FAKE_CTX, h1);
    check("A4: the same window again is not a rebind", ov::query(ov::query_what::rebinds) == 0 &&
                                                           ov::query(ov::query_what::subclassed) == 1);

    // B. the presenter hands over a DIFFERENT window: h1 unlinked, h2 linked.
    ov::on_frame(FAKE_DEV, FAKE_CTX, h2);
    check("B1: h1's own proc restored", proc_of(h1) == base);
    check("B2: h2 subclassed", proc_of(h2) != base);
    check("B3: one rebind, one window carrying the subclass",
          ov::query(ov::query_what::rebinds) == 1 && ov::query(ov::query_what::subclassed) == 1);
    check("B4: both windows still answer", SendMessageA(h1, PING, 0, 0) == 0x5a &&
                                               SendMessageA(h2, PING, 0, 0) == 0x5a && g_pings_base == 3);

    // C. detach: the current window unlinked, nothing left subclassed.
    ov::on_detach();
    check("C1: detach restores h2's own proc", proc_of(h2) == base);
    check("C2: nothing subclassed after detach", ov::query(ov::query_what::subclassed) == 0);
    ov::on_detach();
    check("C3: a second detach is a no-op", ov::query(ov::query_what::subclassed) == 0);

    // D. someone subclasses h1 ON TOP of the overlay, then the presenter moves to h2: h1 cannot be
    //    unlinked, so it keeps forwarding (and is inert); h2 is linked as usual.
    ov::on_frame(FAKE_DEV, FAKE_CTX, h1);
    const WNDPROC ours = proc_of(h1);
    g_over_prev        = (WNDPROC)SetWindowLongPtrA(h1, GWLP_WNDPROC, (LONG_PTR)&over_proc);
    check("D0: the stacked proc sits on top of the overlay's", g_over_prev == ours);
    ov::on_frame(FAKE_DEV, FAKE_CTX, h2);
    check("D1: h1 left alone (its proc is still the stacked one)", proc_of(h1) == (WNDPROC)&over_proc);
    check("D2: h1's row kept + h2 linked", ov::query(ov::query_what::subclassed) == 2);
    const int before = g_pings_base;
    check("D3: h1 still forwards through the whole chain",
          SendMessageA(h1, PING, 0, 0) == 0x5a && g_pings_over == 1 && g_pings_base == before + 1);

    // E. h1 dies: WM_NCDESTROY runs down the chain and the overlay drops its row.
    DestroyWindow(h1);
    check("E1: WM_NCDESTROY dropped h1's row", ov::query(ov::query_what::subclassed) == 1);
    ov::on_detach();
    check("E2: detach unlinks h2", proc_of(h2) == base && ov::query(ov::query_what::subclassed) == 0);
    DestroyWindow(h2);
    check("E3: still nothing subclassed", ov::query(ov::query_what::subclassed) == 0);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    printf("imguibindtest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
