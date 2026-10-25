//
// ddraw_own.cpp -- PT-GFX1: mh.dll's own DirectDraw v1 ([video] backend=own|gdi|null|d3d11).
//
// The game software-renders every frame into a locked 16-bit back buffer and uses DirectDraw only to
// move those pixels to the window. So an "own DirectDraw" does not need to be a
// graphics driver: memory-backed surfaces, a handful of software copies, and one hand-off per frame to
// a present_backend (gdi / null / d3d11, gfx/present_backend.h). With it, no DDRAW.DLL -- the system
// one or dgVoodoo's -- is ever loaded, which is the point: dgVoodoo is closed source and grabs the
// display, and the system DirectDraw on modern Windows is an emulation layer with its own quirks.
//
// ---- THE SEAM: the game's own two-function DDRAW.DLL loader -------------------------------------
//
// DDRAW.DLL is not in the import table. llm_gfx_ddraw_dll_acquire (0x004da3f0) LoadLibraryA's it on
// first use, GetProcAddress'es "DirectDrawCreate" into _G_LLM_GFX_DDRAW_CREATE_PROC and bumps a
// refcount; llm_gfx_ddraw_dll_release (0x004da434) decrements and FreeLibrary's at zero. Every
// DirectDraw object in the process descends from that one proc pointer (verified: the only two
// readers of the proc are set_display_mode and read_current_mode, and nothing else in the binary
// creates a COM object of the ddraw family). So we REPLACE THOSE TWO FUNCTIONS WHOLE (install_jmp):
// acquire publishes our DirectDrawCreate and a non-null module handle, release only decrements.
//
// Why this and not the alternatives:
//   * IAT-hooking GetProcAddress / LoadLibraryA -- the exe's single IAT slot is shared by DirectInput
//     and DirectSound's loaders too (llm_input_dinput_acquire, llm_snd_dsound_*), so the hook would
//     have to filter by name on every call and the "never load ddraw.dll" guarantee would rest on a
//     string compare in someone else's loader path. Replacing the ddraw loader touches only ddraw.
//   * Pre-seeding the three globals from DllMain -- they sit in DGROUP, and whether a given DGROUP
//     dword survives the Watcom startup's own BSS clear is exactly the kind of thing we would rather
//     not depend on. The replaced acquire writes them at the moment the game first asks.
//   * Hooking set_display_mode / read_current_mode -- two larger functions whose bodies we would then
//     have to keep calling correctly. The loader pair is 0x8b bytes with no logic worth keeping.
// Both entries are checked against their exact generated 8 bytes (mh::exp::entry_*) first, so a
// different build leaves the game on its own loader and says so.
//
// ---- WHAT IS IMPLEMENTED: exactly the slots the binary calls ------------------------------------
//
// Enumerated 2026-09-28 by a ReVA sweep of every indirect CALL in the 22 functions that touch the
// device context / the movie + info-screen surface globals / the loader globals, plus the whole
// 0x004d9000..0x004da480 ddraw helper block (the implemented
// methods below ARE that table; each carries its call sites). Every other slot is still a real method -- the class shape comes from the
// SDK's ddraw.h, so the vtable layout and each slot's stdcall cleanup are the compiler's, not ours --
// but it logs "unsupported vtable slot" once and returns DDERR_UNSUPPORTED. done_when (5) counts
// those lines over the UI suite.
//
// ---- THE PRIMARY SURFACE IS THE SCREEN -----------------------------------------------------------
//
// The primary is a real memory surface at the display-mode size, and "put something on the screen" is
// any write to it: a Blt/BltFast with the primary as destination, a Flip, or an Unlock of a primary
// that was locked (the intro movie's 2x path and its subtitle overlay write the primary directly
// through llm_gfx_surface_lock(1)). Each of those composes into primary memory and then hands the
// whole primary to the backend. The per-frame present (llm_gfx_ddraw_present) is a Blt of the back
// buffer to the primary with the window's client rect IN SCREEN COORDINATES as destination; that exact
// shape is recognised and mapped to "the whole primary", so a window anywhere on the desktop (or a
// client scaled to a monitor) still lands 1:1 in the primary. Every other destination rect is taken as
// primary-local, which is what the game means by it: it was written for exclusive fullscreen, where
// screen and client coordinates coincide (the movie tick blits to _G_LLM_GFX_VIEWPORT_RECT, a client
// rect).
//
// NOTHING IS EVER LOST. IsLost is always DD_OK and Restore a no-op, so the post-alt-tab recovery point
// in llm_gfx_ddraw_present always proceeds. SetDisplayMode accepts any size and never touches the real
// display; it records the mode, and the window is sized to it by the backend owner (below).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <ddraw.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "gfx/ddraw_own.h"
#include "gfx/present_backend.h"
#include "gfx/ui_layout.h"
#include "gfx/classic_sizes.h"         // RES25: classic_sizes() -- Interface size scaling off
#include "gfx/overlay_list.h"          // RES23-S5: the overlay list, its stamps, the scratch-render helper
#include "gfx/overlay_imgui.h"         // PT-GFX4: arm the ImGui overlay with backend=d3d11
#include "addr/mh_addrs.gen.h"         // _G_LLM_GAME_MODE (RES4: which modes get the scaled HUD)
#include "addr/mh_export.gen.h"        // mh::exp::entry_llm_gfx_ddraw_dll_{acquire,release} -- the arm guards
#include "include/mh_run_context.h"    // mh_proc_path
#include "include/mh_log_sink.h"       // LOG1: async log sink
#include "include/mh_capture_export.h" // RES23-S0: MH_Capture_WriteBgra32 (bcapture)
#include "config/ini_read.h"           // read_ini_string -- strips a trailing `;comment`
#include "hook/detour.h"               // install_jmp
#include "en_guard.h"                  // EN-only build gate

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace mh::gfx {

// seams/video.cpp: overwrite one named import of the EXE, return the previous value (null = absent).
void *exe_iat_replace(const char *dll_name, const char *fn_name, void *repl);

namespace {

// ---- addresses (EN) -----------------------------------------------------------------------------
// Raw VAs with provenance rather than addr-manifest entries: a manifest DATA entry becomes a harness
// hash region and renumbers every recorded fixture (the fixture-fingerprint trap).
constexpr uintptr_t ADDR_DDRAW_MODULE   = 0x006699d0u; // _G_LLM_GFX_DDRAW_DLL_MODULE (HMODULE; the "loaded" test)
constexpr uintptr_t ADDR_DDRAW_REFCOUNT = 0x006699d4u; // _G_LLM_GFX_DDRAW_DLL_REFCOUNT
constexpr uintptr_t ADDR_DDRAW_CREATE   = 0x006699d8u; // _G_LLM_GFX_DDRAW_CREATE_PROC
constexpr uintptr_t ADDR_HWND_MAIN      = 0x00824fe8u; // hWnd_main (llm_game_create_main_window)
constexpr uintptr_t ADDR_GAME_RUNNING   = 0x005d5fe8u; // _G_LLM_GAME_RUNNING (WinMain's idle loop presents only while set)

template <class T>
T &at(uintptr_t a) { return *reinterpret_cast<T *>(a); }

// RES4: [video] ui_scale as asked. g_cfg.ui_scale_pct is what the presenter holds NOW: the asked value while the
// game is in a strategic-HUD mode (2 = strategic, 5 = paused), 100 everywhere else -- menus, lobby, the galaxy
// screen and tactical are never scaled (user, 2026-10-19). Refreshed on every present and by owned_present_layout.
int g_ui_scale_req = 100;
// RES23-S2: the COMMITTED HUD geometry (hud_geometry, ddraw_own.h) -- computed at ONE point, the strategic pre-input of a mode 2 / 5 frame (and recomputed
// for a new frame size when the picker applies a mode); epoch 0 = nothing committed yet (the asked value is then read live). A new Interface size is a
// PENDING value (g_ui_scale_req) that the next commit takes; nothing in game draw code builds a layout.
hud_geometry g_hud;
int          g_built_h = 0; // RES23-S3: the RES7 table height (owned_hud_set_built); the commit copies it into g_hud
// The frame the last strategic present showed, stamped with the epoch it was drawn under (w == 0: no strategic frame behind the dialogs). The
// dialog modes (3, 4) present THAT frame, so they use its percentage and size -- the RES19 latch, as a property of the frame instead of a side
// effect of a getter.
struct frame_stamp {
    unsigned epoch = 0;
    int      pct = 100, w = 0, h = 0;
} g_stamp;
present_view g_view;                  // RES23-S1: the view of the LAST present (present_surface builds it once); everything else reads it, see view_layout_now
bool         g_ui_scale_auto = false; // the ini said `auto`: the in-game options menu shows "Auto", not the resolved percentage

// RES18 / RES23-S4: a session size that is not a multiple of the 32 px tile is RENDERED rounded up and PRESENTED cropped to the size itself; that
// composition is part of the committed geometry (g_hud.comp_*, set by the picker through owned_set_crop). It applies to the strategic frame (and the dialogs
// over it) of exactly that frame size -- the 640x480 menus and the 1024x768 galaxy screen never match it.
bool g_comp_moved = false; // the composition changed since the last commit: the next commit counts it as a change (epoch + log)

// ---- log: mh_video.log, NOT mh_net.log (its arm-log sequence is baseline-diffed) ----------------

char          g_log_path[MAX_PATH];
unsigned long g_log_gen = 0;

void gfx_log(const char *fmt, ...) {
    mh_proc_path(g_log_path, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_log_path, line, lstrlenA(line));
}

// One line per slot, the first time it is reached. The phrase is what done_when (5) greps for.
// clang-format off
#define MH_GFX_UNSUPPORTED(what)                                                                  \
    do {                                                                                          \
        static bool once_ = false;                                                                \
        if (!once_) {                                                                             \
            once_ = true;                                                                         \
            gfx_log("; [gfx] unsupported vtable slot " what " -- returned DDERR_UNSUPPORTED");    \
        }                                                                                         \
        return DDERR_UNSUPPORTED;                                                                 \
    } while (0)
// clang-format on

// ---- configuration ------------------------------------------------------------------------------

enum class backend_kind : uint8_t { gdi,
                                    null,
                                    d3d11 };

bool           g_active = false;
backend_kind   g_kind   = backend_kind::gdi;
backend_config g_cfg;
int            g_bl_w = 0, g_bl_h = 0;               // [video] borderless_size (dev, RES23-S0): the borderless window covers WxH instead of its monitor (a rig whose monitor is smaller than the player's)
int            g_ui_screen_w = 0, g_ui_screen_h = 0; // [video] ui_screen (dev): the screen the quad layout / pcapture assumes; 0 = the client
int            g_fps_limit = 0;                      // [video] fps_limit; 0 = unlimited (the default)
bool           g_no_window = false;                  // [video] no_window: the headless keeper owns the window, hands off
HMODULE        g_self      = nullptr;

// ---- the display "mode" and the one window -------------------------------------------------------

SIZE_T g_reserve_bytes = 0; // owned_reserve_frame: the largest frame the session can reach (bytes)
int    g_mode_w        = 0; // 0 until SetDisplayMode: GetDisplayMode then reports the desktop size
int    g_mode_h        = 0;
HWND   g_hwnd          = nullptr;
bool   g_geom_placed   = false; // windowed: the first placement centres, later ones keep the user's spot
int    g_geom_tries    = 0;     // consecutive placements that did not reach the mode's client size
int    g_geom_logged_w = 0, g_geom_logged_h = 0;

// ---- the window the player owns (windowed resize, cursor clip, mouse mapping) ----------------------
bool               g_mouse_clip   = true;  // [video] mouse_clip (default 1): confine the OS pointer to the image while active
bool               g_user_sized   = false; // windowed: the player resized/maximised it -- keep THEIR size across mode changes
bool               g_sizing       = false; // inside a user size/move or menu modal loop: hands off geometry + clip
bool               g_in_geom      = false; // our own SetWindowPos is running: its WM_SIZE is not the player's
bool               g_clipped      = false; // ClipCursor is ours right now
RECT               g_clip_rect    = {0, 0, 0, 0};
HWND               g_subclassed   = nullptr;
WNDPROC            g_prev_proc    = nullptr;
bool               g_prev_unicode = false;
long               g_mapped_msgs  = 0;      // mouse messages whose coordinates were rewritten client -> game
long               g_timer_frames = 0;      // frames driven by the size-move timer
constexpr UINT_PTR SIZEMOVE_TIMER = 0x4d48; // 'MH'

// Geometry is ours to own (and the player's to change) only on a presenting backend with a window.
bool geometry_owned();
void install_window_subclass(HWND h);
void release_clip();

present_backend *g_backend              = nullptr;
HWND             g_backend_hwnd         = nullptr;
bool             g_first_present_logged = false;
long             g_presents             = 0;

// ---- the frame limiter ([video] fps_limit) -------------------------------------------------------
//
// Replaces dgVoodoo's FPSLimit=60, which is LOAD-BEARING on the rig (tooling TL-RIG2): without a
// vsync'd flip the game presents as fast as a core allows. It must be ACCURATE (done_when (3): within
// 5% of the limit), which rules out plain Sleep -- its ~15.6 ms default granularity turns 60 into
// anything from 32 to 64. timeBeginPeriod(1) would fix that by raising the timer rate of the whole
// machine, which is exactly what video.cpp's fps_cap refuses to do on shared lanes. A HIGH-RESOLUTION
// WAITABLE TIMER (Windows 10 1803+) gets sub-millisecond waits without touching the global rate; on
// older systems we fall back to Sleep for the coarse part and a yielding spin for the last ~2 ms.
//
// Fixed cadence, bounded debt: a frame that is late by less than one period keeps the schedule (so the
// AVERAGE is exact), one later than that re-bases on "now" (no catch-up burst).
struct frame_limiter {
    LARGE_INTEGER freq{};
    LONGLONG      next   = 0;
    HANDLE        timer  = nullptr;
    bool          hires  = false;
    bool          inited = false;

    void init() {
        inited = true;
        QueryPerformanceFrequency(&freq);
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2; refused (null) before Windows 10 1803.
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
        hires = timer != nullptr;
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    void wait_until(LONGLONG target) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG left = target - now.QuadPart;
        if (left <= 0) return;
        const LONGLONG hns = left * 10000000 / freq.QuadPart; // 100 ns units
        if (hires && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -hns;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
        // Fallback: coarse sleep to ~2 ms short, then yield-spin.
        const LONGLONG ms = hns / 10000;
        if (ms > 2) Sleep((DWORD)(ms - 2));
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= target) return;
            Sleep(0);
        }
    }

    void pace(int fps) {
        if (fps <= 0) return;
        if (!inited) init();
        const LONGLONG period = freq.QuadPart / fps;
        LARGE_INTEGER  now;
        QueryPerformanceCounter(&now);
        if (next == 0) {
            next = now.QuadPart + period;
            return;
        }
        if (now.QuadPart < next) {
            wait_until(next);
            next += period;
        } else if (now.QuadPart - next < period) {
            next += period; // slightly late: keep the cadence
        } else {
            next = now.QuadPart + period; // badly late: re-base, never sprint
        }
    }
};
frame_limiter g_limiter;

// ---- backend selection ---------------------------------------------------------------------------

const char *kind_name(backend_kind k) {
    return k == backend_kind::null ? "null" : k == backend_kind::d3d11 ? "d3d11"
                                                                       : "gdi";
}

// Bind the chosen backend to `h`, falling back to gdi (and, if even that refuses, to null) with a
// log line. Re-binds when the game hands over a different window.
void bind_backend(HWND h) {
    if (!h) return;
    if (g_backend && g_backend_hwnd == h) return;
    if (g_backend) {
        g_backend->detach();
        delete g_backend;
        g_backend = nullptr;
    }
    present_backend *b = nullptr;
    switch (g_kind) {
        case backend_kind::null: b = make_null_backend(); break;
        case backend_kind::d3d11: b = make_d3d11_backend(); break;
        default: b = make_gdi_backend(); break;
    }
    if (b && !b->attach(h, g_cfg)) {
        gfx_log("; [gfx] backend %s refused to attach -- falling back to gdi", b->name());
        delete b;
        b = nullptr;
    } else if (!b) {
        gfx_log("; [gfx] backend %s is not compiled into this build -- falling back to gdi", kind_name(g_kind));
    }
    if (!b) {
        b = make_gdi_backend();
        if (b && !b->attach(h, g_cfg)) {
            gfx_log("; [gfx] gdi backend refused to attach too -- presenting nothing (null)");
            delete b;
            b = make_null_backend();
            if (b) b->attach(h, g_cfg);
        }
    }
    g_backend      = b;
    g_backend_hwnd = h;
    install_window_subclass(h);
    if (b) {
        gfx_log("; [gfx] backend %s bound to hwnd=%p", b->name(), (void *)h);
        if (g_mode_w > 0) b->on_mode(g_mode_w, g_mode_h, pixel_format::rgb565);
    }
}

// ---- PT-GFX8: the window must stay a switchable top-level window ------------------------------------
//
// Player report (rc6, Win10 22H2, d3d11 + borderless): after an Alt-Tab the game kept running but had no
// taskbar button and could not be reached again. Windows lists a top-level window iff it is visible, not
// DWM-cloaked, and either unowned without WS_EX_TOOLWINDOW or WS_EX_APPWINDOW (the Alt-Tab / taskbar
// rule; tools/win_probe.py computes the same verdict from outside). The retail window is created
// WS_POPUP|WS_EX_TOPMOST with NO WS_EX_APPWINDOW, so its presence in the switcher rests entirely on the
// shell's own bookkeeping, and a monitor-sized borderless popup is exactly what the shell treats as a
// "fullscreen app" (taskbar suppressed, button bookkeeping skipped) -- a state it can be left in after the
// window loses the foreground. The guard therefore (1) pins the style bits -- APPWINDOW on, TOOLWINDOW off --
// on every geometry pass, and (2) tells the shell explicitly, through ITaskbarList, that the window has a
// tab, at first placement and on every deactivation. ([video] taskbar_guard=0 turns both off, the "before"
// arm of the alttab_borderless_d3d11 row.)
//
// NOT MarkFullscreenWindow(FALSE). v0.2.0-rc7 also told the shell the window was NOT fullscreen; the shell
// then kept the taskbar drawn ABOVE the monitor-sized borderless game for the whole session (user report,
// rc7 withdrawn 2026-10-05). The rig could not see it: these shell calls are skipped off the default
// desktop, and every lane runs on its own desktop. Let the shell detect fullscreen on its own.
bool g_taskbar_guard = true;

// The shell only exists on the interactive desktop; the rig runs lanes on its own desktop object, where an
// ITaskbarList call would talk to an explorer that does not own the window.
bool on_default_desktop() {
    char  n[64] = {0};
    DWORD need  = 0;
    HDESK d     = GetThreadDesktop(GetCurrentThreadId());
    return d && GetUserObjectInformationA(d, UOI_NAME, n, sizeof(n), &need) && lstrcmpiA(n, "Default") == 0;
}

// ITaskbarList2 by hand (CLSID/IID literals + dynamic ole32): no new link dependency in mh.dll.
constexpr GUID MH_CLSID_TaskbarList = {0x56FDF344, 0xFD6D, 0x11d0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
constexpr GUID MH_IID_ITaskbarList2 = {0x602D4995, 0xB13A, 0x429b, {0xA6, 0x6E, 0x19, 0x35, 0xE4, 0x4F, 0x43, 0x17}};
struct taskbar_list2 { // the first slots of ITaskbarList2 (IUnknown, ITaskbarList, MarkFullscreenWindow)
    struct vtbl {
        HRESULT(STDMETHODCALLTYPE *QueryInterface)(taskbar_list2 *, const GUID &, void **);
        ULONG(STDMETHODCALLTYPE *AddRef)(taskbar_list2 *);
        ULONG(STDMETHODCALLTYPE *Release)(taskbar_list2 *);
        HRESULT(STDMETHODCALLTYPE *HrInit)(taskbar_list2 *);
        HRESULT(STDMETHODCALLTYPE *AddTab)(taskbar_list2 *, HWND);
        HRESULT(STDMETHODCALLTYPE *DeleteTab)(taskbar_list2 *, HWND);
        HRESULT(STDMETHODCALLTYPE *ActivateTab)(taskbar_list2 *, HWND);
        HRESULT(STDMETHODCALLTYPE *SetActiveAlt)(taskbar_list2 *, HWND);
        HRESULT(STDMETHODCALLTYPE *MarkFullscreenWindow)(taskbar_list2 *, HWND, BOOL);
    } const *v;
};

volatile LONG g_taskbar_busy      = 0; // one refresh thread at a time
long          g_taskbar_refreshes = 0;
volatile LONG g_taskbar_last_hr   = 0x7fffffff; // HRESULT of the last AddTab (0x7fffffff = never ran), for the [tbstate] trace

// Runs off the window's thread: the calls are cross-process COM into explorer and must never be able to
// stall the frame loop. MTA, so no message pump is needed for the call to complete.
DWORD WINAPI taskbar_refresh_thread(LPVOID p) {
    const HWND h      = (HWND)p;
    using coinit_fn   = HRESULT(WINAPI *)(LPVOID, DWORD);
    using couninit_fn = void(WINAPI *)();
    using cocreate_fn = HRESULT(WINAPI *)(const GUID &, LPUNKNOWN, DWORD, const GUID &, LPVOID *);
    HMODULE ole       = LoadLibraryA("ole32.dll");
    if (ole) {
        auto init   = (coinit_fn)GetProcAddress(ole, "CoInitializeEx");
        auto uninit = (couninit_fn)GetProcAddress(ole, "CoUninitialize");
        auto create = (cocreate_fn)GetProcAddress(ole, "CoCreateInstance");
        if (init && uninit && create && SUCCEEDED(init(nullptr, 0 /* COINIT_MULTITHREADED */))) {
            taskbar_list2 *tb = nullptr;
            if (SUCCEEDED(create(MH_CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, MH_IID_ITaskbarList2, (void **)&tb)) && tb) {
                if (SUCCEEDED(tb->v->HrInit(tb)) && IsWindow(h)) {
                    InterlockedExchange(&g_taskbar_last_hr, (LONG)tb->v->AddTab(tb, h));
                    ++g_taskbar_refreshes;
                }
                tb->v->Release(tb);
            }
            uninit();
        }
        FreeLibrary(ole);
    }
    InterlockedExchange(&g_taskbar_busy, 0);
    return 0;
}

void request_taskbar_refresh(HWND h, const char *why) {
    if (!g_taskbar_guard || !h || !on_default_desktop()) return;
    if (InterlockedCompareExchange(&g_taskbar_busy, 1, 0) != 0) return;
    static int logged = 0;
    if (logged < 8) {
        ++logged;
        gfx_log("; [gfx] taskbar: AddTab told to the shell (%s)", why);
    }
    HANDLE t = CreateThread(nullptr, 0, &taskbar_refresh_thread, (LPVOID)h, 0, nullptr);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_taskbar_busy, 0);
}

// Style bits only; the caller owns the SetWindowLong. Returns the corrected ex-style.
LONG guard_ex_style(LONG ex) {
    return g_taskbar_guard ? ((ex | WS_EX_APPWINDOW) & ~(LONG)WS_EX_TOOLWINDOW) : ex;
}

// ---- PT-GFX8 evidence: [tbstate] lines in mh_video.log -----------------------------------------------
//
// The reporter's lost taskbar entry is shell state we cannot reproduce, so the NEXT report has to carry
// it. One compact line of everything the taskbar / Alt-Tab eligibility rests on (window_switchable, the
// same predicate the winswitchable harness op asserts) is written on each event that could change it
// (activate, show, pos change that hides/shows/moves, size, display change, DPI change) and from a 5 s poll
// that logs ONLY when the facts differ from the last line. Lines are capped at 4 per second (a drag fires
// WM_WINDOWPOSCHANGED continuously); a dropped event is counted into the next line's drop=. All of it runs
// on the window's own thread, no allocation, nothing per frame beyond one GetTickCount compare in the poll.
constexpr DWORD TB_WINDOW_MS = 1000, TB_POLL_MS = 5000;
constexpr int   TB_PER_WINDOW  = 4;   // lines per TB_WINDOW_MS (a burst of four passes whole)
char            g_tb_last[384] = {0}; // the fact fields of the last line written (poll dedup)
DWORD           g_tb_win_tick = 0, g_tb_poll_tick = 0;
int             g_tb_win_n   = 0;
int             g_tb_dropped = 0;
RECT            g_tb_last_wr = {0, 0, 0, 0};

void taskbar_trace(HWND h, const char *why, bool changed_only) {
    if (!h || !IsWindow(h)) return;
    const DWORD now = GetTickCount();
    char        facts[160];
    const bool  ok = window_switchable(h, facts, sizeof(facts));
    RECT        wr = {0, 0, 0, 0}, mr = {0, 0, 0, 0};
    GetWindowRect(h, &wr);
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) mr = mi.rcMonitor;
    const HWND fg     = GetForegroundWindow();
    DWORD      fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    char body[384];
    wsprintfA(body, "hwnd=%p %s zoomed=%d wr=%ld,%ld,%ld,%ld mon=%ld,%ld,%ld,%ld fg=%p ours=%d switchable=%d guard=%d addtab=%08lx n=%ld", (void *)h,
              facts, (int)IsZoomed(h), wr.left, wr.top, wr.right, wr.bottom, mr.left, mr.top, mr.right, mr.bottom, (void *)fg,
              (int)(fg == h || (fg && fg_pid == GetCurrentProcessId())), (int)ok, (int)g_taskbar_guard, (unsigned long)g_taskbar_last_hr,
              g_taskbar_refreshes);
    if (changed_only && lstrcmpA(body, g_tb_last) == 0) return;
    if (!g_tb_win_tick || now - g_tb_win_tick >= TB_WINDOW_MS) {
        g_tb_win_tick = now;
        g_tb_win_n    = 0;
    }
    if (g_tb_win_n >= TB_PER_WINDOW) {
        ++g_tb_dropped;
        return;
    }
    lstrcpynA(g_tb_last, body, sizeof(g_tb_last));
    ++g_tb_win_n;
    g_tb_last_wr = wr;
    gfx_log("; [tbstate] t=%lu %s drop=%d %s", (unsigned long)now, why, g_tb_dropped, body);
    g_tb_dropped = 0;
}

// Called once per present (ensure_geometry): a tick compare, then at most one trace per TB_POLL_MS.
void taskbar_trace_poll(HWND h) {
    const DWORD now = GetTickCount();
    if (g_tb_poll_tick && now - g_tb_poll_tick < TB_POLL_MS) return;
    g_tb_poll_tick = now;
    taskbar_trace(h, "poll", true);
}

// ---- window geometry -----------------------------------------------------------------------------
//
// With no real mode switch the WINDOW is the display. The game creates a WS_POPUP|WS_EX_TOPMOST window
// and, on every mode change, MoveWindow's it to (0,0,w,h) -- right for exclusive fullscreen, wrong for
// us. So before each present the owner checks the geometry and re-asserts it when the game (or anything)
// moved it: one GetWindowLong + one GetClientRect in the steady state.
//
//   windowed    WS_OVERLAPPEDWINDOW: caption, sizing border, minimise + maximise. Until the player
//               resizes it, the client is exactly the mode size (1:1), centred on first placement and
//               then left wherever it is dragged. Once the player resizes it -- a border drag, a snap,
//               maximise -- it keeps THEIR size, across mode changes too, and the presenter scales the
//               frame aspect-preserving into it (gdi and d3d11 alike). The game's own render size never
//               changes: it still renders at its mode. A mode change on an un-resized window resizes it
//               to the new mode's native size. The game's MoveWindow is swallowed for this mode
//               (own_MoveWindow), so a mode change neither throws the window to the origin nor reads as
//               a player resize. Not topmost.
//   borderless  WS_POPUP covering the window's monitor, scaled. NOT topmost either: the game creates it
//               WS_EX_TOPMOST, and a topmost monitor-sized window stays over everything after an
//               alt-tab. The game's FIRST MoveWindow passes through here, so the DirectInput mouse's
//               cooperative level (llm_wnd_is_fullscreen, read right after it) is decided on the
//               mode-sized window exactly as before. Later ones are swallowed: the window stays on its
//               monitor and only the frame size changes, so a mode change (the in-game menu's 640x480)
//               never shrinks the window for a frame.
//
// Skipped entirely for the null backend and under [video] no_window, whose keeper owns the window, and
// while the player is inside a size/move modal loop (g_sizing).
constexpr LONG WINDOWED_STYLE = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
constexpr int  MIN_CLIENT_W = 320, MIN_CLIENT_H = 240;

bool g_move_hooked = false; // own_MoveWindow is in the exe's import table

bool geometry_owned() { return g_active && !g_no_window && g_kind != backend_kind::null; }

// The client size the windowed game wants at the current display mode: the mode itself, or (RES18) the cropped composition of the mode's frame.
bool crop_matches_mode() { return g_hud.comp_w > 0 && g_hud.comp_fw == g_mode_w && g_hud.comp_fh == g_mode_h && (g_hud.comp_w < g_mode_w || g_hud.comp_h < g_mode_h); }
int  native_client_w() { return crop_matches_mode() ? g_hud.comp_w : g_mode_w; }
int  native_client_h() { return crop_matches_mode() ? g_hud.comp_h : g_mode_h; }

bool backend_is_null() { return g_backend && lstrcmpA(g_backend->name(), "null") == 0; }

void ensure_geometry() {
    if (!geometry_owned() || !g_hwnd || g_mode_w <= 0 || g_sizing || backend_is_null()) return;
    taskbar_trace_poll(g_hwnd); // before the iconic early-out: a minimised window is exactly when it matters
    if (IsIconic(g_hwnd)) return;
    const LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
    LONG       ex    = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
    RECT       cr;
    GetClientRect(g_hwnd, &cr);
    bool changed = false;
    g_in_geom    = true;
    { // PT-GFX8: keep it a switchable window (APPWINDOW on, TOOLWINDOW off); one call, only on a change
        static bool first = true;
        const LONG  fix   = guard_ex_style(ex);
        if (fix != ex) {
            SetWindowLongA(g_hwnd, GWL_EXSTYLE, fix);
            gfx_log("; [gfx] taskbar: window ex-style %08lx -> %08lx (APPWINDOW on, TOOLWINDOW off)", (unsigned long)ex, (unsigned long)fix);
            ex = fix;
        }
        if (first && g_taskbar_guard) {
            first = false;
            request_taskbar_refresh(g_hwnd, "first placement");
        }
    }
    if (g_cfg.window == window_mode::windowed) {
        const LONG want = (style & ~(LONG)WS_POPUP) | WINDOWED_STYLE;
        if (IsZoomed(g_hwnd) || g_user_sized) {
            // The player's size. Only the style and the topmost bit are ours to correct.
            if (style != want || (ex & WS_EX_TOPMOST)) {
                SetWindowLongA(g_hwnd, GWL_STYLE, want);
                SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
                SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
                changed = true;
            }
            g_geom_placed = true;
        } else if (!(style == want && !(ex & WS_EX_TOPMOST) && cr.right == native_client_w() && cr.bottom == native_client_h())) {
            SetWindowLongA(g_hwnd, GWL_STYLE, want);
            SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
            RECT r = {0, 0, native_client_w(), native_client_h()};
            AdjustWindowRectEx(&r, (DWORD)want, FALSE, (DWORD)GetWindowLongA(g_hwnd, GWL_EXSTYLE));
            const int ww = r.right - r.left, wh = r.bottom - r.top;
            int       x = 0, y = 0;
            RECT      wr;
            GetWindowRect(g_hwnd, &wr);
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            // Centre on first placement, and -- only if the MoveWindow swallow could not be installed --
            // whenever the game has just thrown it back to the origin.
            if (!g_geom_placed || (!g_move_hooked && wr.left == mi.rcMonitor.left && wr.top == mi.rcMonitor.top)) {
                x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
                y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
            } else {
                // Keep the top-left; a mode change that grows the window pulls it back on screen.
                x = wr.left;
                y = wr.top;
                if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
                if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
            }
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (y < mi.rcWork.top) y = mi.rcWork.top;
            if (g_geom_tries < 3) { // could not converge for this mode -- stop fighting (logged below)
                changed = true;
                SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                // AdjustWindowRectEx answers at the SYSTEM dpi, and this process is per-monitor-v2 aware
                // (video.cpp apply_dpi_awareness): on a scaled monitor the real frame is thicker and the
                // client came out 4 px short (measured 636x476 for 640x480). Correct by what we got.
                GetClientRect(g_hwnd, &cr);
                if (cr.right != native_client_w() || cr.bottom != native_client_h())
                    SetWindowPos(g_hwnd, nullptr, 0, 0, ww + (native_client_w() - cr.right), wh + (native_client_h() - cr.bottom),
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                GetClientRect(g_hwnd, &cr);
                ++g_geom_tries;
                if (cr.right == native_client_w() && cr.bottom == native_client_h()) {
                    g_geom_tries = 0;
                    if (!g_geom_placed || g_geom_logged_w != native_client_w() || g_geom_logged_h != native_client_h())
                        gfx_log("; [gfx] window: windowed (resizable), client %dx%d at %d,%d", (int)cr.right, (int)cr.bottom, x, y);
                    g_geom_logged_w = native_client_w();
                    g_geom_logged_h = native_client_h();
                } else if (g_geom_tries >= 3) {
                    gfx_log("; [gfx] window: client stuck at %dx%d for a %dx%d mode after 3 tries -- presenting scaled",
                            (int)cr.right, (int)cr.bottom, native_client_w(), native_client_h());
                }
            }
            g_geom_placed = true;
        } else {
            g_geom_placed = true;
        }
    } else {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int  mw = g_bl_w > 0 ? g_bl_w : mi.rcMonitor.right - mi.rcMonitor.left, mh = g_bl_w > 0 ? g_bl_h : mi.rcMonitor.bottom - mi.rcMonitor.top;
        const LONG want = (style & ~WINDOWED_STYLE) | WS_POPUP;
        RECT       wr;
        GetWindowRect(g_hwnd, &wr);
        if (!(style == want && !(ex & WS_EX_TOPMOST) && wr.left == mi.rcMonitor.left && wr.top == mi.rcMonitor.top &&
              cr.right == mw && cr.bottom == mh)) {
            SetWindowLongA(g_hwnd, GWL_STYLE, want);
            SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (!g_geom_placed)
                gfx_log("; [gfx] window: borderless %dx%d on its monitor (not topmost), frame %dx%d scaled", mw, mh, g_mode_w,
                        g_mode_h);
            g_geom_placed = true;
            changed       = true;
        }
    }
    g_in_geom = false;
    if (changed && g_backend) g_backend->on_window_changed();
}

// ---- the mouse: where a click lands when the frame is scaled ---------------------------------------
//
// THE GAME'S DEFAULT MOUSE IS DIRECTINPUT, AND IT NEEDS NO MAPPING. llm_input_wndproc_tap (0x004d1194)
// polls the DI mouse's buffered RELATIVE counts (llm_input_di_mouse_poll 0x004d0cc4) into its own
// cursor integral, clamped to the mode box, and the game draws its OWN cursor sprite there
// (llm_gfx_draw_cursor_menu / _ingame) into the framebuffer. That sprite is scaled with the rest of
// the frame, so what the player sees and where the game hit-tests are the same point at any scale; the
// OS pointer is hidden over the window (llm_wnd_on_create / _on_activate ShowCursor(0)) and never read
// (the exe imports no GetCursorPos). What a scaled window DOES change is where the invisible OS pointer
// is: it moves on its own (Windows ballistics) and can leave the window, and a click out there goes to
// the desktop and deactivates the game. So the pointer is CLIPPED to the image while the game window is
// active (update_clip, [video] mouse_clip). DI counts are unaffected by a clip, and the cursor's speed
// in game pixels per count is the game's own -- nothing here scales it.
//
// THE MESSAGE PATH IS MAPPED. With no DI mouse ([input] mouse_absolute=1, and the UI harness's
// `wmclick`), the tap reads WM_MOUSE* lParam as GAME coordinates -- true only in exclusive fullscreen,
// where client, screen and game pixels coincide. The subclass below rewrites each mouse message's point
// from client pixels to game pixels through the same fit_image_rect the presenter draws with (a point
// in the letterbox bars clamps to the image edge); WM_MOUSEWHEEL, whose point is in SCREEN coordinates,
// is converted to client first. The opposite direction is the game's own SetCursorPos (display init
// centres the pointer; llm_input_mouse_delta_pump warps it to the game cursor while the cursor sprite
// is hidden -- the minimap drag), whose game coordinates own_SetCursorPos maps to the screen point the
// player sees them at. At 1:1 both maps are the identity.
//
// Order in the window's chain: this subclass is installed at SetCooperativeLevel, before the ImGui
// overlay's (first present), so the overlay sits OUTSIDE it and sees client pixels, which are ImGui's
// own coordinates; the game below sees game pixels.

int  effective_ui_pct();
int  effective_ui_pct_for(int fw, int fh);
bool ui_screen_size(int &cw, int &ch);
void dialogs_now(int fw, int fh, uilayout::dialog_set &ds, uintptr_t *lists = nullptr);

// Game mode m over a fw x fh frame shows the strategic HUD: 2 strategic, 5 paused, or a dialog mode (3 menus, 4 info) over the strategic frame of
// exactly that size (the frame the last strategic present stamped). The ONE copy of the mode test.
bool gameplay_dialog_for(int m, int fw, int fh) { return (m == 3 || m == 4) && g_stamp.w > 0 && g_stamp.w == fw && g_stamp.h == fh; }
bool hud_shown_for(int m, int fw, int fh) { return m == 2 || m == 5 || gameplay_dialog_for(m, fw, fh); }

// The quad list for a frame, with the dialog quad (RES4 stage 6) when a gameplay-size dialog is up: ONE place, so the presenters,
// the mouse maps, the cursor and pcapture all see the same list.
// RES18: the composition inside a fw x fh frame, when that frame is the session's rounded-up strategic frame and the game is in a mode that
// shows it (2 strategic, 5 paused, or the dialog modes 3 / 4 reached from it -- the same test as effective_ui_pct). False = the frame is the picture.
// The exact-frame key (fw x fh == comp_fw x comp_fh) is KEPT on purpose (RES23-S6 looked at dropping it): the composition is committed when the picker applies
// a mode, but the callers pass the size of the SURFACE they are asked about (present_surface's s->w x s->h, the framebuffer's WindowWidth/Height), and across a
// mode change a surface of the OLD size is presented (and a game-mode-2 test passes) before the new frame exists. Without the key that transient frame would
// take the new frame's crop. The epoch cannot replace it: it counts commits, it does not say which frame size a commit belongs to.
bool crop_for_frame(int fw, int fh, int &kw, int &kh) {
    kw = kh = 0;
    if (g_hud.comp_w <= 0 || fw != g_hud.comp_fw || fh != g_hud.comp_fh || (g_hud.comp_w >= fw && g_hud.comp_h >= fh)) return false;
    if (!hud_shown_for(at<int>(mh::addr::_G_LLM_GAME_MODE), fw, fh)) return false;
    kw = g_hud.comp_w;
    kh = g_hud.comp_h;
    return true;
}

// RES23-S1: is the last present's view the layout for exactly these inputs? (It is a pure function of them, so a match IS that layout.)
bool view_matches(const present_view &v, int cw, int ch, int fw, int fh, int pct, const uilayout::dialog_set &ds, int kw, int kh) {
    if (!v.valid || v.screen_w != cw || v.screen_h != ch || v.frame_w != fw || v.frame_h != fh || v.pct != pct || v.integer != g_cfg.integer_scale ||
        v.crop_w != kw || v.crop_h != kh || v.dialogs.n != ds.n)
        return false;
    for (int i = 0; i < ds.n; ++i) {
        const uilayout::irect &a = v.dialogs.r[i], &b = ds.r[i];
        if (a.x0 != b.x0 || a.y0 != b.y0 || a.x1 != b.x1 || a.y1 != b.y1 || v.dialogs.layered[i] != ds.layered[i]) return false;
    }
    return true;
}

// The quad layout of a (cw x ch screen, fw x fh frame, pct) as it stands NOW, for everything that is not the presenter: the mouse maps, the
// clip, the cursor, the dialog layers, pcapture. It is the last present's view (g_view) whenever that view was built from the very same
// inputs -- the normal case, since the presenter and its consumers look at one frame -- and a fresh build otherwise (a mode change or a
// dialog that appeared since the last present). The fresh one is NOT stored: g_view stays "what the last present showed". RES23-S6 kept the input
// comparison: the HUD epoch (S2) counts commits of the HUD geometry, but the view also depends on the screen size, the game mode, the presenter and the
// dialogs, none of which move the epoch -- an epoch key would serve a stale layout after a window resize or when a dialog opens between two presents.
void view_layout_now(uilayout::layout &L, int cw, int ch, int fw, int fh, int pct) {
    uilayout::dialog_set ds;
    if (pct > uilayout::MIN_PCT) dialogs_now(fw, fh, ds);
    int kw, kh;
    crop_for_frame(fw, fh, kw, kh);
    if (view_matches(g_view, cw, ch, fw, fh, pct, ds, kw, kh)) {
        L = g_view.layout;
        return;
    }
    present_view v;
    build_present_view(v, cw, ch, fw, fh, pct, g_cfg.integer_scale, &ds, kw, kh);
    L = v.layout;
}

// RES4 stage 4: the quad layout the player is looking at NOW, when ui_scale is in force (several quads). False when the
// presenter shows the single fitted rect (every mapping below then stays the legacy formula, byte for byte). The
// screen is the client area (or the dev `[video] ui_screen`, whose points are then taken as client pixels).
bool multi_layout(uilayout::layout &L) {
    if (!geometry_owned() || !g_hwnd || g_mode_w <= 0 || g_mode_h <= 0) return false;
    const int pct = effective_ui_pct();
    int       kw, kh;
    if (pct <= uilayout::MIN_PCT && !crop_for_frame(g_mode_w, g_mode_h, kw, kh)) return false; // (RES18: a crop is a several-quad layout at 100 too)
    int cw, ch;
    if (!ui_screen_size(cw, ch)) return false;
    view_layout_now(L, cw, ch, g_mode_w, g_mode_h, pct);
    return !L.single;
}

// The image's rect in CLIENT pixels, or false when there is nothing to map through. With quads: the rectangle they cover.
bool image_rect_client(RECT *out) {
    if (!geometry_owned() || !g_hwnd || g_mode_w <= 0 || g_mode_h <= 0) return false;
    uilayout::layout L;
    if (multi_layout(L)) {
        *out = RECT{L.used.x0, L.used.y0, L.used.x1, L.used.y1};
        return out->right > out->left && out->bottom > out->top;
    }
    RECT cr;
    if (!GetClientRect(g_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
    *out = fit_image_rect(cr.right, cr.bottom, g_mode_w, g_mode_h, g_cfg.integer_scale);
    return out->right > out->left && out->bottom > out->top;
}

void map_client_to_game(const RECT &r, int &x, int &y) {
    uilayout::layout L;
    if (multi_layout(L) && uilayout::screen_to_frame(L, x, y)) return;
    image_client_to_game(r, g_mode_w, g_mode_h, x, y);
}

// The CENTRE of game pixel (x,y) in client pixels, so client_to_game(game_to_client(p)) == p.
void map_game_to_client(const RECT &r, int &x, int &y) {
    uilayout::layout L;
    if (multi_layout(L)) {
        uilayout::frame_to_screen(L, x, y); // a hidden pixel lands on the nearest visible one
        return;
    }
    image_game_to_client(r, g_mode_w, g_mode_h, x, y);
}

void release_clip() {
    if (!g_clipped) return;
    RECT cur;
    // Only undo OUR clip: if something else re-clipped since, it is not ours to clear.
    if (GetClipCursor(&cur) && EqualRect(&cur, &g_clip_rect)) ClipCursor(nullptr);
    g_clipped = false;
}

// Once per present: confine the OS pointer to the image while the game window is the active foreground
// window and the player is not dragging/sizing it. The ImGui overlay can draw over the bars, so while it
// is open the whole client is allowed.
void update_clip() {
    if (!g_mouse_clip || !geometry_owned() || !g_hwnd) return;
    const bool want = !g_sizing && GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd) && at<int>(ADDR_GAME_RUNNING) != 0;
    if (!want) {
        release_clip();
        return;
    }
    RECT r;
    if (imgui_overlay::query(imgui_overlay::query_what::open) == 1) {
        if (!GetClientRect(g_hwnd, &r)) return;
    } else if (!image_rect_client(&r)) {
        return;
    }
    POINT tl = {r.left, r.top}, br = {r.right, r.bottom};
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    const RECT s = {tl.x, tl.y, br.x, br.y};
    RECT       cur;
    if (g_clipped && EqualRect(&s, &g_clip_rect) && GetClipCursor(&cur) && EqualRect(&cur, &s)) return;
    if (ClipCursor(&s)) {
        if (!g_clipped || !EqualRect(&s, &g_clip_rect)) {
            static int logged = 0;
            if (logged < 8) {
                ++logged;
                gfx_log("; [gfx] mouse: OS pointer clipped to the image %ld,%ld..%ld,%ld (screen)", s.left, s.top, s.right, s.bottom);
            }
        }
        g_clip_rect = s;
        g_clipped   = true;
    }
}

// A resize that was not ours: decide whether the player now owns the size.
void note_user_size(HWND h) {
    if (g_cfg.window != window_mode::windowed || g_mode_w <= 0) return;
    RECT cr;
    if (!GetClientRect(h, &cr) || cr.right <= 0) return;
    const bool was = g_user_sized;
    g_user_sized   = IsZoomed(h) || cr.right != native_client_w() || cr.bottom != native_client_h();
    if (was != g_user_sized || g_user_sized) {
        static int logged = 0;
        if (logged < 16) {
            ++logged;
            gfx_log("; [gfx] window: %s client %ldx%ld (mode %dx%d)%s", g_user_sized ? "player-sized" : "back to native",
                    cr.right, cr.bottom, native_client_w(), native_client_h(), IsZoomed(h) ? " maximised" : "");
        }
    }
}

LRESULT forward(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_prev_unicode ? CallWindowProcW(g_prev_proc, h, m, w, l) : CallWindowProcA(g_prev_proc, h, m, w, l);
}

LRESULT CALLBACK owned_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (h != g_subclassed || !g_prev_proc) return DefWindowProcA(h, m, w, l); // unreachable
    switch (m) {                                                              // PT-GFX8 [tbstate] triggers; each falls through to the normal handling below
        case WM_SHOWWINDOW: {
            char why[48];
            wsprintfA(why, "SHOWWINDOW show=%d src=%d", (int)w, (int)l);
            taskbar_trace(h, why, false);
            break;
        }
        case WM_WINDOWPOSCHANGED: {
            const WINDOWPOS *wp = (const WINDOWPOS *)l;
            if (wp && ((wp->flags & (SWP_HIDEWINDOW | SWP_SHOWWINDOW)) || wp->x != g_tb_last_wr.left || wp->y != g_tb_last_wr.top ||
                       wp->x + wp->cx != g_tb_last_wr.right || wp->y + wp->cy != g_tb_last_wr.bottom)) {
                char why[48];
                wsprintfA(why, "POSCHANGED flags=%x", (unsigned)wp->flags);
                taskbar_trace(h, why, false);
            }
            break;
        }
        case WM_DISPLAYCHANGE: {
            char why[64];
            wsprintfA(why, "DISPLAYCHANGE %dbpp %dx%d", (int)w, (int)LOWORD(l), (int)HIWORD(l));
            taskbar_trace(h, why, false);
            break;
        }
        case WM_DPICHANGED: {
            char why[32];
            wsprintfA(why, "DPICHANGED %u", (unsigned)LOWORD(w));
            taskbar_trace(h, why, false);
            break;
        }
        default: break;
    }
    switch (m) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK: {
            RECT r;
            if (image_rect_client(&r)) {
                int x = (short)LOWORD(l), y = (short)HIWORD(l);
                map_client_to_game(r, x, y);
                l = MAKELPARAM((WORD)x, (WORD)y);
                ++g_mapped_msgs;
            }
            break;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            RECT r;
            if (image_rect_client(&r)) {
                POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
                ScreenToClient(h, &p);
                int x = p.x, y = p.y;
                map_client_to_game(r, x, y);
                l = MAKELPARAM((WORD)x, (WORD)y);
                ++g_mapped_msgs;
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            const LRESULT res = forward(h, m, w, l);
            if (g_cfg.window == window_mode::windowed && l) {
                RECT r = {0, 0, MIN_CLIENT_W, MIN_CLIENT_H};
                AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongA(h, GWL_EXSTYLE));
                MINMAXINFO *mmi       = (MINMAXINFO *)l;
                mmi->ptMinTrackSize.x = r.right - r.left;
                mmi->ptMinTrackSize.y = r.bottom - r.top;
            }
            return res;
        }
        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP:
            // A modal loop owns the thread now: WinMain's idle RedrawWindow -- the only thing that runs
            // frames -- is not reached until it ends. A timer keeps frames coming (a lockstep peer that
            // stops presenting stalls every other peer), and the clip lets go so the player can drag.
            g_sizing = true;
            release_clip();
            SetTimer(h, SIZEMOVE_TIMER, 15, nullptr);
            break;
        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP:
            g_sizing = false;
            KillTimer(h, SIZEMOVE_TIMER);
            if (m == WM_EXITSIZEMOVE) note_user_size(h);
            if (g_backend) g_backend->on_window_changed();
            break;
        case WM_TIMER:
            if (w == SIZEMOVE_TIMER) {
                if (g_sizing && at<int>(ADDR_GAME_RUNNING) != 0) {
                    if (++g_timer_frames == 1) gfx_log("; [gfx] window: frames driven by the size-move timer while the player drags");
                    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_INTERNALPAINT | RDW_NOERASE);
                } else if (!g_sizing) {
                    KillTimer(h, SIZEMOVE_TIMER);
                }
                return 0;
            }
            break;
        case WM_SIZE: {
            char why[32];
            wsprintfA(why, "SIZE type=%d", (int)w);
            taskbar_trace(h, why, false);
        }
            if (w == SIZE_MINIMIZED) release_clip();
            else if (!g_in_geom && !g_sizing) note_user_size(h); // a snap, maximise, restore -- not a drag
            break;
        case WM_SYSCOMMAND:
            // A lone Alt (or F10) release reaches DefWindowProc as SC_KEYMENU and enters the system-menu
            // modal loop: the next key goes to the menu and frames only run off the size-move timer. The
            // game has no menu bar and reads Alt through DirectInput anyway, so eat it. Alt+F4 is
            // SC_CLOSE and Alt+Tab never reaches the window, so both still work. Alt+Space is also
            // SC_KEYMENU and goes with it -- the system menu stays reachable from the title bar.
            if ((w & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_ACTIVATE: {
            char why[48];
            wsprintfA(why, "ACTIVATE %s min=%d", LOWORD(w) == WA_INACTIVE ? "inactive" : (LOWORD(w) == WA_CLICKACTIVE ? "click" : "active"), (int)HIWORD(w));
            taskbar_trace(h, why, false);
        }
            if (LOWORD(w) == WA_INACTIVE) {
                release_clip();
                request_taskbar_refresh(h, "deactivated"); // PT-GFX8: the shell must keep a tab for it
            }
            break;
        case WM_ACTIVATEAPP: {
            char why[32];
            wsprintfA(why, "ACTIVATEAPP %d", (int)w);
            taskbar_trace(h, why, false);
        }
            if (!w) release_clip();
            break;
        case WM_KILLFOCUS:
            release_clip();
            break;
        case WM_NCDESTROY: {
            release_clip();
            const LRESULT  res = forward(h, m, w, l);
            const LONG_PTR cur = g_prev_unicode ? GetWindowLongPtrW(h, GWLP_WNDPROC) : GetWindowLongPtrA(h, GWLP_WNDPROC);
            if (cur == (LONG_PTR)&owned_wndproc) {
                if (g_prev_unicode) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)g_prev_proc);
                else SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)g_prev_proc);
            }
            g_subclassed = nullptr;
            return res;
        }
        default: break;
    }
    return forward(h, m, w, l);
}

// SetForegroundWindow is refused when another process owns the foreground and this one has had no
// input yet -- the usual state right after a launcher starts the game. Attaching to the foreground
// thread's input queue for the call is the documented way through for a window the user just launched.
void take_foreground(HWND h) {
    if (!h || GetForegroundWindow() == h) return;
    if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
    const HWND  fg     = GetForegroundWindow();
    const DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD my_tid = GetCurrentThreadId();
    const bool  attach = fg_tid && fg_tid != my_tid && AttachThreadInput(my_tid, fg_tid, TRUE);
    BringWindowToTop(h);
    const BOOL ok = SetForegroundWindow(h);
    SetFocus(h);
    if (attach) AttachThreadInput(my_tid, fg_tid, FALSE);
    gfx_log("; [gfx] window: foreground %s", ok && GetForegroundWindow() == h ? "taken" : "REFUSED by the shell");
}

void install_window_subclass(HWND h) {
    if (!h || !geometry_owned() || g_subclassed == h) return;
    if (g_subclassed) return; // one window per process; a second HWND keeps the first's subclass (never seen)
    g_prev_unicode    = IsWindowUnicode(h) != 0;
    g_prev_proc       = g_prev_unicode ? (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) : (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
    g_subclassed      = h;
    const WNDPROC was = g_prev_unicode ? (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)&owned_wndproc)
                                       : (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)&owned_wndproc);
    if (was) g_prev_proc = was;
    gfx_log("; [gfx] window hwnd=%p subclassed: mouse messages mapped client -> game, resize/activate tracked, mouse_clip=%d",
            (void *)h, g_mouse_clip ? 1 : 0);
}

// ---- the game's own window calls, for a window the player owns ------------------------------------
using move_fn               = BOOL(WINAPI *)(HWND, int, int, int, int, BOOL);
using setcur_fn             = BOOL(WINAPI *)(int, int);
move_fn   g_real_move       = nullptr;
setcur_fn g_real_setcur     = nullptr;
long      g_swallowed_moves = 0;

// llm_gfx_apply_window_resolution MoveWindow's the main window to (0,0,w,h) on every mode change.
// Windowed: swallowed -- ensure_geometry sizes it (native, or the player's size) at the next present,
// and the player's position survives. Borderless: the FIRST call passes through (see the geometry note: the
// window is not ours yet and the DirectInput cooperative level is decided on it); once ensure_geometry has
// placed the window on its monitor it is swallowed too, because the game's MoveWindow(0,0,w,h) on every
// mode change (the in-game menu runs at 640x480) would shrink the window for one frame until the next
// present restored it -- the flicker on ESC > Return to game. The frame size still changes (SetDisplayMode);
// the presenter scales it into the monitor-sized window.
BOOL WINAPI own_MoveWindow(HWND h, int x, int y, int w, int hgt, BOOL repaint) {
    if (geometry_owned() && (h == g_hwnd || h == at<HWND>(ADDR_HWND_MAIN)) &&
        (g_cfg.window == window_mode::windowed || g_geom_placed)) {
        if (++g_swallowed_moves <= 4)
            gfx_log("; [gfx] window: game MoveWindow(%d,%d,%d,%d) swallowed (%s geometry is ours)", x, y, w, hgt,
                    g_cfg.window == window_mode::windowed ? "windowed" : "borderless");
        return TRUE;
    }
    return g_real_move(h, x, y, w, hgt, repaint);
}

// The game's SetCursorPos takes GAME coordinates (it was written for exclusive fullscreen). Map them to
// where that game pixel is on the screen now.
BOOL WINAPI own_SetCursorPos(int x, int y) {
    // llm_gfx_display_init warps right after a mode change, before the next present has restyled the
    // window the game just MoveWindow'd -- measured on the rig VM, borderless 1024x768: the pointer
    // went to (320,240), the centre of the mode-sized window, not of the image. Settle the geometry
    // first (the DI mouse's cooperative level was already decided, earlier in the same init).
    ensure_geometry();
    RECT r;
    if (image_rect_client(&r)) {
        map_game_to_client(r, x, y);
        POINT p = {x, y};
        ClientToScreen(g_hwnd, &p);
        x = p.x;
        y = p.y;
    }
    return g_real_setcur(x, y);
}

void install_owned_window_hooks() {
    if (g_kind == backend_kind::null || g_no_window) return;
    g_real_move   = (move_fn)exe_iat_replace("user32.dll", "MoveWindow", (void *)&own_MoveWindow);
    g_move_hooked = g_real_move != nullptr;
    g_real_setcur = (setcur_fn)exe_iat_replace("user32.dll", "SetCursorPos", (void *)&own_SetCursorPos);
    gfx_log("; [gfx] window hooks: MoveWindow %s, SetCursorPos %s (game -> screen mapped)",
            g_real_move ? (g_cfg.window == window_mode::windowed ? "swallowed for the main window" : "passed through once, then swallowed")
                        : "NOT hooked",
            g_real_setcur ? "hooked" : "NOT hooked");
}

// done_when (1): "the process never loads any ddraw.dll (module list logged at first present)".
void log_modules_once() {
    if (g_first_present_logged) return;
    g_first_present_logged = true;
    HANDLE snap            = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        gfx_log("; [gfx] first present: module snapshot failed (%lu)", GetLastError());
        return;
    }
    MODULEENTRY32W me = {sizeof(me)}; // the W form, spelled out: this project builds UNICODE
    char           line[400];
    int            len = 0, count = 0;
    bool           ddraw = false;
    line[0]              = 0;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        ++count;
        char mod[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, mod, sizeof(mod), nullptr, nullptr);
        if (lstrcmpiA(mod, "ddraw.dll") == 0) {
            ddraw = true;
            char path[MAX_PATH];
            WideCharToMultiByte(CP_ACP, 0, me.szExePath, -1, path, sizeof(path), nullptr, nullptr);
            gfx_log("; [gfx] first present: DDRAW.DLL IS LOADED -- %s (the owned device should have kept it out)",
                    path);
        }
        const int n = lstrlenA(mod);
        if (len + n + 2 >= (int)sizeof(line) - 1) {
            gfx_log("; [gfx]   modules: %s", line);
            len     = 0;
            line[0] = 0;
        }
        if (len) line[len++] = ' ';
        lstrcpyA(line + len, mod);
        len += n;
    }
    if (len) gfx_log("; [gfx]   modules: %s", line);
    CloseHandle(snap);
    gfx_log("; [gfx] first present: %d modules loaded, ddraw.dll %s", count, ddraw ? "LOADED" : "NOT loaded");
}

// ---- memory surfaces -----------------------------------------------------------------------------

struct OwnSurface;
struct OwnClipper;
struct OwnPalette;

void present_surface(OwnSurface *s);

DDPIXELFORMAT pf565() {
    DDPIXELFORMAT pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.dwSize            = sizeof(pf);
    pf.dwFlags           = DDPF_RGB;
    pf.dwRGBBitCount     = 16;
    pf.dwRBitMask        = 0xF800;
    pf.dwGBitMask        = 0x07E0;
    pf.dwBBitMask        = 0x001F;
    pf.dwRGBAlphaBitMask = 0;
    return pf;
}

HRESULT qi_self(IUnknown *self, REFIID riid, void **out, const GUID &own_iid, const char *what) {
    static const GUID IID_UNK = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    if (!out) return E_POINTER;
    if (riid == IID_UNK || riid == own_iid) {
        self->AddRef();
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    gfx_log("; [gfx] unsupported vtable slot %s::QueryInterface for {%08lx-...} -- returned E_NOINTERFACE", what,
            riid.Data1);
    return E_NOINTERFACE;
}

// IIDs from ddraw.h, spelled out so no dxguid.lib is needed.
const GUID IID_DD   = {0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const GUID IID_DDS  = {0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const GUID IID_DDC  = {0x6C14DB85, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const GUID IID_DDPL = {0x6C14DB84, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

struct OwnClipper final : IDirectDrawClipper {
    LONG ref  = 1;
    HWND hwnd = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override { return qi_self(this, riid, out, IID_DDC, "IDirectDrawClipper"); }
    ULONG STDMETHODCALLTYPE   AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE   Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE GetClipList(LPRECT, LPRGNDATA, LPDWORD) override { MH_GFX_UNSUPPORTED("IDirectDrawClipper::GetClipList"); }
    HRESULT STDMETHODCALLTYPE GetHWnd(HWND *out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = hwnd;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsClipListChanged(BOOL *out) override {
        if (out) *out = FALSE;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetClipList(LPRGNDATA, DWORD) override { MH_GFX_UNSUPPORTED("IDirectDrawClipper::SetClipList"); }
    HRESULT STDMETHODCALLTYPE SetHWnd(DWORD, HWND h) override {
        hwnd = h;
        return DD_OK;
    }
};

struct OwnPalette final : IDirectDrawPalette {
    LONG         ref  = 1;
    DWORD        caps = 0;
    PALETTEENTRY entries[256];

    OwnPalette() { ZeroMemory(entries, sizeof(entries)); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override { return qi_self(this, riid, out, IID_DDPL, "IDirectDrawPalette"); }
    ULONG STDMETHODCALLTYPE   AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE   Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDWORD out) override {
        if (out) *out = caps;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEntries(DWORD, DWORD base, DWORD n, LPPALETTEENTRY out) override {
        if (!out || base >= 256 || n > 256 - base) return DDERR_INVALIDPARAMS;
        memcpy(out, entries + base, n * sizeof(PALETTEENTRY));
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetEntries(DWORD, DWORD base, DWORD n, LPPALETTEENTRY in) override {
        if (!in || base >= 256 || n > 256 - base) return DDERR_INVALIDPARAMS;
        memcpy(entries + base, in, n * sizeof(PALETTEENTRY));
        return DD_OK;
    }
};

struct OwnSurface final : IDirectDrawSurface {
    LONG        ref     = 1;
    int         w       = 0;
    int         h       = 0;
    int         pitch   = 0; // bytes; exactly w*2 -- the game derives its own pitch from the width
    uint8_t    *bits    = nullptr;
    SIZE_T      alloc   = 0;
    DWORD       caps    = 0;
    bool        primary = false;
    int         locks   = 0;
    OwnSurface *back    = nullptr; // attached back buffer (flip chain only -- unreachable at flags=7)
    OwnClipper *clipper = nullptr;
    OwnPalette *palette = nullptr;

    bool create(int width, int height, DWORD c) {
        const bool primary_like = (c & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) != 0;
        w                       = width;
        h                       = height;
        pitch                   = width * 2;
        caps                    = c;
        // Slack past the end: the software renderer has measured history of reading a few rows past a
        // surface (video.cpp's hittest_begin note). dgVoodoo's allocations tolerated that; an exact-size
        // VirtualAlloc would turn a benign over-read into an access violation at a page boundary.
        SIZE_T body = (SIZE_T)pitch * (SIZE_T)height;
        // The game frame (primary / back buffer / a mode-sized offscreen) is allocated for the LARGEST mode the
        // session can reach, not the current one: the menu frame drawn right after a mode switch still uses the
        // previous mode's layout and the bevel/blit helpers clip x but not y (PT-GFX9 crash on 1280x1024 -> 640x480).
        if (primary_like || (width == g_mode_w && height == g_mode_h)) {
            const SIZE_T r = g_reserve_bytes;
            if (r > body) body = r;
        }
        const SIZE_T slack = body / 8 > 65536 ? body / 8 : 65536;
        alloc              = body + slack;
        bits               = (uint8_t *)VirtualAlloc(nullptr, alloc, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return bits != nullptr;
    }
    ~OwnSurface() {
        if (back) back->Release();
        if (clipper) clipper->Release();
        if (palette) palette->Release();
        if (bits) VirtualFree(bits, 0, MEM_RELEASE);
    }

    void fill_desc(LPDDSURFACEDESC d, bool with_bits) const {
        // Fill at the fixed 0x6c layout regardless of the caller's dwSize: every caller in this binary
        // passes a 0x6c buffer, and one of them (llm_gfx_ddraw_lock_surface) only zero-fills it.
        ZeroMemory(d, sizeof(DDSURFACEDESC));
        d->dwSize          = sizeof(DDSURFACEDESC);
        d->dwFlags         = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (with_bits ? DDSD_LPSURFACE : 0);
        d->dwHeight        = (DWORD)h;
        d->dwWidth         = (DWORD)w;
        d->lPitch          = pitch;
        d->lpSurface       = with_bits ? bits : nullptr;
        d->ddpfPixelFormat = pf565();
        d->ddsCaps.dwCaps  = caps;
    }

    RECT full() const { return RECT{0, 0, w, h}; }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override { return qi_self(this, riid, out, IID_DDS, "IDirectDrawSurface"); }
    ULONG STDMETHODCALLTYPE   AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE   Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // ---- the slots the game calls
    HRESULT STDMETHODCALLTYPE Blt(LPRECT dst_rc, LPDIRECTDRAWSURFACE src, LPRECT src_rc, DWORD flags, LPDDBLTFX fx) override;
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT src_rc, DWORD trans) override;
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE, DWORD) override {
        if (!primary || !back) return DDERR_NOTFLIPPABLE;
        memcpy(bits, back->bits, (SIZE_T)pitch * (SIZE_T)h);
        present_surface(this);
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE *out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        if (!back || !c || !(c->dwCaps & DDSCAPS_BACKBUFFER)) return DDERR_NOTFOUND;
        back->AddRef();
        *out = back;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        out->dwCaps = caps;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = pf565();
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        fill_desc(out, false);
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE Lock(LPRECT rc, LPDDSURFACEDESC d, DWORD, HANDLE) override {
        if (!d) return DDERR_INVALIDPARAMS;
        fill_desc(d, true);
        if (rc) {
            if (rc->left < 0 || rc->top < 0 || rc->right > w || rc->bottom > h || rc->left >= rc->right || rc->top >= rc->bottom)
                return DDERR_INVALIDRECT;
            d->lpSurface = bits + (SIZE_T)rc->top * pitch + (SIZE_T)rc->left * 2;
        }
        ++locks; // nesting tolerated: the game's own locked_mask already prevents a double lock
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER c) override {
        if (clipper) clipper->Release();
        clipper = static_cast<OwnClipper *>(c);
        if (clipper) clipper->AddRef();
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE p) override {
        if (palette) palette->Release();
        palette = static_cast<OwnPalette *>(p);
        if (palette) palette->AddRef();
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID) override {
        if (locks <= 0) return DDERR_NOTLOCKED;
        --locks;
        // A locked primary is the movie path writing the SCREEN directly -- unlocking it is a present.
        if (primary && locks == 0) present_surface(this);
        return DD_OK;
    }

    // ---- everything else: no caller in the binary
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::AddAttachedSurface"); }
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::AddOverlayDirtyRect"); }
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::BltBatch"); }
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::DeleteAttachedSurface"); }
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::EnumAttachedSurfaces"); }
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::EnumOverlayZOrders"); }
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetBltStatus"); }
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER *) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetClipper"); }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD, LPDDCOLORKEY) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetColorKey"); }
    HRESULT STDMETHODCALLTYPE GetDC(HDC *) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetDC"); }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetFlipStatus"); }
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetOverlayPosition"); }
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE *) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::GetPalette"); }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::ReleaseDC"); }
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD, LPDDCOLORKEY) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::SetColorKey"); }
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::SetOverlayPosition"); }
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::UpdateOverlay"); }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::UpdateOverlayDisplay"); }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) override { MH_GFX_UNSUPPORTED("IDirectDrawSurface::UpdateOverlayZOrder"); }
};

// A PURE read: the percentage the HUD shows at the current game mode over a fw x fh frame. Mode 2 / 5: the committed value (the asked one live until
// the first commit); a dialog mode over the stamped strategic frame: that frame's percentage (an Options apply moves the stamp at once, RES26: owned_hud_commit_applied; the game
// does not wait for the resume); everything else 100.
int effective_ui_pct_for(int fw, int fh) {
    const int m = at<int>(mh::addr::_G_LLM_GAME_MODE);
    if (m == 2 || m == 5) {
        const int p = g_hud.epoch ? g_hud.pct : g_ui_scale_req;
        return p <= uilayout::MIN_PCT ? uilayout::MIN_PCT : p;
    }
    if (m == 3 || m == 4) return gameplay_dialog_for(m, fw, fh) && g_stamp.pct > uilayout::MIN_PCT ? g_stamp.pct : uilayout::MIN_PCT;
    return uilayout::MIN_PCT;
}
int effective_ui_pct() { return effective_ui_pct_for(g_mode_w, g_mode_h); }

// ---- RES4 stage 6: dialog quads ------------------------------------------------------------------------------------
//
// A dialog drawn at gameplay size lives in the frame as a widget list centred by llm_ui_widget_list_center (0x004b6fb4), whose rect the
// function stores in the list at +0x18 x, +0x1c y, +0x20 w, +0x24 h (frame pixels; the widgets are list-relative). The presenter shows
// that rectangle a second time, scaled by ui_scale about the screen's centre, ABOVE the HUD quads and below the cursor
// (uilayout::place_dialog); the mouse maps a point inside the quad back to the rectangle's frame pixels, so a click lands on the
// widget drawn under it. Read afresh at every present (the rect is recomputed every frame the dialog is up). Which list:
//   * _G_LLM_UI_ACTIVE_DIALOG (0x00654295), when set: the modal dialogs of the strategic view -- generic dialogs (llm_ui_dlg_build_from_table),
//     the shipment contents, the building construction status; closed by llm_ui_dlg_close_cb, which clears it;
//   * else, in game mode 3 reached FROM the strategic view, _G_LLM_UI_MENU_WIDGET_LIST (0x0065428d): the ESC menu, the Options screens
//     it pushes, the lockstep "Player not responding" modal (_G_LLM_UI_WGT_LIST_LOCKSTEP_SYNC).
// Mode 3 is also every front-end menu: those run at 640x480 with another frame size, and a menu entered without a strategic present
// before it is never a dialog over the HUD (g_stamp: the frame and epoch of the last mode 2 / 5 present, forgotten by any other mode).
// A list with no frame widget is the whole window (w x h = the frame): it fits no map and gets no quad (stays 1x).
constexpr uintptr_t ADDR_UI_ACTIVE_DIALOG = 0x00654295u; // _G_LLM_UI_ACTIVE_DIALOG
constexpr uintptr_t ADDR_UI_MENU_LIST     = 0x0065428du; // _G_LLM_UI_MENU_WIDGET_LIST

// One list's rect, or false (null, not a list of the exe's image, or a rect that is not inside the frame).
bool list_rect(uintptr_t list, int fw, int fh, uilayout::irect &r) {
    if (list < 0x00400000u || list > 0x01200000u) return false;
    const int x = at<int>(list + 0x18), y = at<int>(list + 0x1c), w = at<int>(list + 0x20), h = at<int>(list + 0x24);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > fw || y + h > fh) return false;
    r = uilayout::irect{x, y, x + w, y + h};
    return true;
}

// The dialogs up now, bottom to top: the mode-3 menu list (ESC menu, Options, the lockstep modal), then _G_LLM_UI_ACTIVE_DIALOG over it
// (a confirmation, the shipment list, ...; drawn after the menu list by the game, so it is the upper one). `lists` (optional) gets each
// one's list address. ds.layered[i] is set when a stage 6b layer exists for exactly that list and rect (layer_valid_for, below).
bool layer_valid_for(int slot, uintptr_t list, const uilayout::irect &r);
void layers_drop_all();
void dialogs_now(int fw, int fh, uilayout::dialog_set &ds, uintptr_t *lists) {
    ds          = uilayout::dialog_set();
    const int m = at<int>(mh::addr::_G_LLM_GAME_MODE);
    if (!hud_shown_for(m, fw, fh)) {
        layers_drop_all();
        return;
    }
    const uintptr_t act  = (uintptr_t)at<uint32_t>(ADDR_UI_ACTIVE_DIALOG);
    uintptr_t       menu = 0;
    if (m == 3 || m == 4) {
        menu = (uintptr_t)at<uint32_t>(ADDR_UI_MENU_LIST);
        if (menu == mh::addr::_G_LLM_UI_WGT_LIST_GAMEPLAY_HUD || menu == act) menu = 0;
    }
    uintptr_t l[uilayout::MAX_DIALOGS] = {};
    if (menu && list_rect(menu, fw, fh, ds.r[ds.n])) l[ds.n++] = menu;
    if (act && list_rect(act, fw, fh, ds.r[ds.n])) l[ds.n++] = act;
    for (int i = 0; i < ds.n; ++i) {
        ds.layered[i] = layer_valid_for(i, l[i], ds.r[i]);
        if (lists) lists[i] = l[i];
    }
    if (ds.n == 0) layers_drop_all();
}

// RES4: one log line per distinct (screen, frame, scale) so a capture's layout is in mh_video.log.
// The screen the quad layout is built for: the dev override [video] ui_screen, else the game window's client area.
bool ui_screen_size(int &cw, int &ch) {
    cw = g_ui_screen_w;
    ch = g_ui_screen_h;
    if (cw > 0 && ch > 0) return true;
    RECT cr;
    if (!g_hwnd || !GetClientRect(g_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
    cw = cr.right;
    ch = cr.bottom;
    return true;
}

void log_ui_layout_change(int fw, int fh, int pct, const uilayout::layout &l, int kw) {
    static int key[6] = {};
    const int  cw = l.screen_w, ch = l.screen_h;
    if (cw <= 0 || ch <= 0) return;
    if (key[0] == cw && key[1] == ch && key[2] == fw && key[3] == fh && key[4] == pct && key[5] == kw) return;
    key[0] = cw, key[1] = ch, key[2] = fw, key[3] = fh, key[4] = pct, key[5] = kw;
    const uilayout::render_size ideal = uilayout::ideal_render(cw, ch, pct);
    if (!l.single) {
        const int k = (uilayout::whole_panel_rows(l) - uilayout::LIST_TOP) / uilayout::LIST_ROW;
        gfx_log("; [ui] ui_scale: %d%% -> effective %d.%02d (fits %d whole rows)", pct, l.pct / 100, l.pct % 100, k < 0 ? 0 : k);
    }
    if (l.single)
        gfx_log("; [ui] ui_scale %d%% on %dx%d with a %dx%d frame: single quad (%s); ideal render %dx%d", pct, cw, ch, fw, fh,
                l.clamped ? "clamped to 100 by the screen size" : "nothing to scale", ideal.w, ideal.h);
    else {
        char mapk[24] = "1:1"; // the map factor: 1:1 unless the crop / overlay fit enlarges the composition (whole when integer scaling is on)
        if (l.map_src_w > 0 && l.map_w != l.map_src_w) {
            const int k100 = (int)(((int64_t)l.map_w * 100 + l.map_src_w / 2) / l.map_src_w);
            wsprintfA(mapk, "x%d.%02d", k100 / 100, k100 % 100);
        }
        gfx_log("; [ui] ui_scale %d%%%s on %dx%d, frame %dx%d: map %dx%d %s, panel %d wide (%d frame rows), strip %d high, "
                "composition %dx%d at %d,%d; ideal render %dx%d%s",
                l.pct, l.clamped ? " (clamped from the request)" : "", cw, ch, fw, fh, l.map_w, l.map_h, mapk, l.panel_w, l.panel_rows, l.strip_h,
                l.used.w(), l.used.h(), l.used.x0, l.used.y0, ideal.w, ideal.h, ideal.capped ? " (render caps cut the map)" : "");
    }
}

// RES18: one log line per distinct (screen, frame, crop) -- what the rig reads (check_res_crop.py).
void log_crop_layout(int fw, int fh, int kw, int kh, const uilayout::layout &l) {
    static int key[6] = {};
    const int  cw = l.screen_w, ch = l.screen_h;
    if (cw <= 0 || ch <= 0) return;
    if (key[0] == cw && key[1] == ch && key[2] == fw && key[3] == fh && key[4] == kw && key[5] == kh) return;
    key[0] = cw, key[1] = ch, key[2] = fw, key[3] = fh, key[4] = kw, key[5] = kh;
    if (!l.single && !l.crop && l.pct > uilayout::MIN_PCT) return; // (above 100 the `ui_scale` layout line carries the crop's map; the composition line is the 100 form)
    if (l.single || !l.crop) {
        gfx_log("; [ui] crop %dx%d of a %dx%d frame on %dx%d: not applied (single quad)", kw, kh, fw, fh, cw, ch);
        return;
    }
    const uilayout::quad &mq = l.q[0], &sq = l.q[1], &pq = l.q[2];
    const bool            bars = l.used.x0 != 0 || l.used.y0 != 0 || l.used.w() != cw || l.used.h() != ch;
    gfx_log("; [ui] crop: render %dx%d, composition %dx%d on a %dx%d screen at %d,%d (bars: %s); map %dx%d (frame %dx%d) %s, strip y %d..%d (frame rows %d..%d), "
            "panel x %d..%d (frame cols %d..%d, %d rows)",
            fw, fh, l.used.w(), l.used.h(), cw, ch, l.used.x0, l.used.y0, bars ? "yes" : "none", mq.dst.w(), mq.dst.h(), mq.src.w(), mq.src.h(),
            mq.dst.w() == mq.src.w() ? "1:1" : "scaled", sq.dst.y0, sq.dst.y1, sq.src.y0, sq.src.y1, pq.dst.x0, pq.dst.x1, pq.src.x0, pq.src.x1, pq.src.h());
}

// ---- RES4 stage 5: the owned, scaled cursor ----------------------------------------------------------------
//
// llm_gfx_draw_cursor_menu (0x004a5b4d) is the one routine every cursor draw of the strategic frame and the dialogs goes through
// (six callers). It blits the cursor sprite (plus up to four overlay sprites) into the frame at _G_LLM_CURSOR_MENU_X/Y minus the
// hotspot, with three blend modes (opaque, 50% blend, halve-the-backdrop), and STAMPS the tile-visibility / tile-drawn maps for the
// tiles it covers so the next frame redraws the backdrop. In a scaled layout the cursor must instead be at ui_scale like the rest of
// the HUD, so this detour (installed only when ui_scale > 100 is asked for):
//   * scaled layout in force (game mode 2 / 5, several quads) and the cursor visible: runs the STOCK routine twice through the
//     trampoline, into a CURSOR_SRC^2 scratch (cleared to black, then to white) with the frame globals pointed at it -- surface,
//     pitch, WindowWidth/Height (the routine clips to G_WIN_W/H = these), the cursor position (the hotspot lands on pixel
//     CURSOR_HOT), the clip rows and both tile maps (dummy buffers, so no real tile is stamped). The two renderings give the
//     sprite's premultiplied colour and coverage per pixel (cursor_pixel_from_passes), stored with the game's cursor pixel. Nothing
//     is drawn into the frame, so the frame never carries a second cursor and leaves no trail to redraw. Every global it touched is
//     put back; the routine's own leftovers (current sprite, destination) are rewritten before each use by every blit.
//   * anything else (100%, menus, lobby, galaxy, tactical, a hidden cursor, no layout): drops the stored cursor and runs the
//     original untouched, so those paths are byte for byte what they were.
// present_surface then places the stored cursor with uilayout::place_cursor (scaled by s, hotspot on frame_to_screen of the cursor
// pixel -- the same map a click goes back through) and hands it to the backend as frame_view::cursor, drawn over the quads.
// Presentation only: it reads the cursor position and writes render globals + scratch, never simulation state.
constexpr uintptr_t ADDR_CURSOR_MENU_DRAW = 0x004a5b4du;
constexpr uintptr_t ADDR_CURSOR_MENU_X    = scratch_addr::cursor_menu_x;
constexpr uintptr_t ADDR_CURSOR_MENU_Y    = scratch_addr::cursor_menu_y;
constexpr int       CURSOR_MAP_MAX_TW     = 240; // the dummy tile maps hold 8 rows of up to this many tiles

// The stock cursor routine only reads the cursor position and the sprite state and blits into the frame globals' target; the tile-visibility /
// tile-drawn maps it stamps are dummies in the scratch. Running it twice into a scratch is therefore harmless.
constexpr scratch_target SCRATCH_CURSOR = {"cursor", ADDR_CURSOR_MENU_DRAW,
                                           "reads the cursor position + sprite state; its only stores go to the frame target and the tile maps, which the scratch swaps for dummies"};

typedef void(__cdecl *cursor_fn)();
void         *g_cursor_tramp      = nullptr;
bool          g_cursor_hook_tried = false;
overlay_stamp g_cur_stamp; // the present the stored cursor was captured for (seq 0 = none)
int           g_cur_fx = 0, g_cur_fy = 0;
int           g_cur_bbox[4] = {0, 0, -1, -1}; // the stored sprite's opaque box, scratch pixels (the rig sizes the overlay with it)
uint32_t      g_cur_src[uilayout::CURSOR_SRC * uilayout::CURSOR_SRC];
uint32_t      g_cur_buf[uilayout::CURSOR_MAX * uilayout::CURSOR_MAX];
uint16_t      g_cur_black[uilayout::CURSOR_SRC * uilayout::CURSOR_SRC];
uint16_t      g_cur_white[uilayout::CURSOR_SRC * uilayout::CURSOR_SRC];
uint8_t       g_cur_vis_map[8 * CURSOR_MAP_MAX_TW];
uint8_t       g_cur_drawn_map[8 * CURSOR_MAP_MAX_TW];

// The present a source captured NOW is for: the next one (the frame being drawn is shown by it). Every capture is stamped with it and the HUD
// epoch in force (overlay_stamp); the overlay list takes only stamps that match the frame it is built for (ov_window) and drops the rest.
bool     g_in_present     = false; // present_surface is building / showing its list
unsigned g_newest_capture = 0;     // the newest present number any source was stamped for
unsigned capture_seq() { return (unsigned)g_presents + 1; }
void     stamp_capture(overlay_stamp &s) {
    s.set(capture_seq(), g_hud.epoch);
    g_newest_capture = capture_seq();
}
// The stamps that belong to the frame in question: inside a present, that present's; anywhere else (pcapture, the mouse, the game thread mid-draw)
// the frame the framebuffer holds now -- the one drawn since the last present when any source was captured for it, else the last presented one.
overlay_window ov_window() {
    overlay_window w;
    const unsigned p = (unsigned)g_presents;
    w.seq_lo = w.seq_hi = g_in_present || g_newest_capture <= p ? p : p + 1;
    w.epoch             = g_hud.epoch;
    return w;
}

// THE hook layout (RES23-S7): the several-quad layout the presenter will use for the frame the game renders NOW, for every hook that keeps something out of
// the frame to draw it at the Interface size afterwards -- the cursor, the dialog layers, the notices, the overlay collect. Like multi_layout, but a lane
// with no owned window (a null presenter with the dev `ui_screen`) still gets one, so pcapture can show the cursor there. False = STOCK: the thing is drawn
// into the frame by the game. That includes a requested percentage the layout cannot use: uilayout::build falls back to the 100 base layout (L.pct ==
// MIN_PCT) when the requested panel / strip does not exceed the fit zoom (a 1280x720 session on a 2560x1440 screen at 175%), and a hook that had looked
// only at the REQUEST kept the cursor and the ESC menu out of a frame whose presenter placed no overlay for them.
bool scaled_layout(uilayout::layout &L, int fw, int fh) {
    if (!g_active || fw <= 0 || fh <= 0) return false;
    const int pct = effective_ui_pct();
    if (pct <= uilayout::MIN_PCT) return false;
    int cw, ch;
    if (!ui_screen_size(cw, ch)) return false;
    view_layout_now(L, cw, ch, fw, fh, pct);
    return !L.single && L.pct > uilayout::MIN_PCT;
}

// Returns true when the cursor was handled here (nothing is to be drawn into the frame).
bool __cdecl cursor_scaled_draw() {
    constexpr int    N = uilayout::CURSOR_SRC;
    uilayout::layout L;
    const int        fw = at<int>(mh::addr::WindowWidth), fh = at<int>(mh::addr::WindowHeight);
    const int        tw = at<int>(mh::addr::_G_LLM_VIEW_TILES_W);
    if (!g_cursor_tramp || at<int>(mh::addr::_G_LLM_CURSOR_VISIBLE) != 1 || tw <= 0 || tw > CURSOR_MAP_MAX_TW || !scaled_layout(L, fw, fh)) {
        g_cur_stamp.clear();
        return false;
    }
    const int   saved_mx = at<int>(ADDR_CURSOR_MENU_X), saved_my = at<int>(ADDR_CURSOR_MENU_Y);
    scratch_cfg c;
    c.win_w = N, c.win_h = N;                                                        // the routine clips to WindowWidth / Height
    c.cursor = true, c.cur_x = uilayout::CURSOR_HOT, c.cur_y = uilayout::CURSOR_HOT; // the hotspot lands on pixel CURSOR_HOT
    c.vis = g_cur_vis_map, c.drawn = g_cur_drawn_map;
    c.clip = true;
    scratch_render_bw(SCRATCH_CURSOR, c, N, N, g_cur_black, g_cur_white, g_cur_src, [] { ((cursor_fn)g_cursor_tramp)(); }, g_cur_bbox);
    g_cur_fx = saved_mx;
    g_cur_fy = saved_my;
    stamp_capture(g_cur_stamp);
    return true;
}

// clang-format off
__declspec(naked) void cursor_thunk() {
    __asm {
        pushad
        pushfd
        call cursor_scaled_draw
        test al, al
        jz   run_original
        popfd
        popad
        ret
    run_original:
        popfd
        popad
        jmp  dword ptr [g_cursor_tramp]
    }
}
// clang-format on

// Arm the detour once (when ui_scale above 100 is asked for, at boot or live). The entry is `cmp dword [_G_LLM_CURSOR_VISIBLE], 1`
// (7 bytes, then jz): checked against the generated 8 bytes, 7 stolen.
void install_cursor_hook() {
    if (g_cursor_hook_tried) return;
    g_cursor_hook_tried = true;
    if (*(const uint64_t *)ADDR_CURSOR_MENU_DRAW != mh::exp::entry_llm_gfx_draw_cursor_menu) {
        gfx_log("; [ui] owned cursor NOT armed -- the cursor routine's entry bytes differ (wrong build, or hooked)");
        return;
    }
    if (mh::hook::install_trampoline(ADDR_CURSOR_MENU_DRAW, (void *)&cursor_thunk, &g_cursor_tramp, 7, mh::hook::entry_claim::exclusive,
                                     "the RES4 owned cursor (llm_gfx_draw_cursor_menu)", 0))
        gfx_log("; [ui] owned cursor armed (llm_gfx_draw_cursor_menu detour): scaled layouts draw the cursor at ui_scale");
    else
        gfx_log("; [ui] owned cursor NOT armed -- the entry was refused (see the [interlock] line)");
}

// ---- RES20 / RES23-S5: the scaled floating messages (notices) --------------------------------------------------------------------------------
//
// llm_strat_draw_floating_messages (0x0044eb60) prints the notices ("order refused", "????", ...) at frame (10,10) downwards, once per frame, at the
// tail of the strategic render. They are drawn INTO the map area of the frame, so at ui_scale they stayed 1x while the panel grew. In a scaled layout
// (game mode 2, or mode 3 over the strategic frame: several quads) this detour runs the STOCK routine twice through the trampoline into a scratch (over black, then over white: the same
// scratch_render_bw as the cursor), wrapping at the visible map width / s; the pair gives each pixel's coverage and premultiplied colour, and the
// presenter shows the layer on the map quad scaled by s (uilayout::place_scaled_layer), as the notice slot of the overlay list. NOTHING is drawn into
// the frame (so the dirty-tile maps need no stamping: the scratch has dummy ones). Any other case runs the original untouched. The capture is stamped
// for the present of its frame: a mode that does not run this routine (a dialog over the HUD) presents no notice. [video] ui_scale_notices=0 (dev
// kill switch) never arms the detour: the notices are the game's own, drawn into the frame at 1x.
//
// The routine only reads state (the message timers against TOTAL_GAME_TIME, the font, the message queue) and draws; the blink phase follows the
// game clock, so a second rendering in the same frame is the same picture (REPORT.md 3.5).
constexpr uintptr_t      ADDR_FLOAT_MSGS = 0x0044eb60u; // llm_strat_draw_floating_messages
constexpr int            MSG_MAX_W       = 1280;        // the scratch: wrap width <= this, 30 lines of a font <= 17 px fit 512 rows
constexpr int            MSG_H           = 512;
constexpr int            MSG_MAP_MAX_TW = 240, MSG_MAP_ROWS = MSG_H / 32;
constexpr scratch_target SCRATCH_NOTICES  = {"notices", ADDR_FLOAT_MSGS,
                                             "only reads state (message timers vs TOTAL_GAME_TIME, the queue, the font) and draws -- checked in Ghidra 2026-10-22 (REPORT.md 3.5)"};
void                    *g_msg_tramp      = nullptr;
bool                     g_msg_hook_tried = false;
bool                     g_msg_enabled    = true; // [video] ui_scale_notices (default on); off: the detour is never armed
overlay_stamp            g_msg_stamp;
int                      g_msg_w = 0, g_msg_h = 0;
uint16_t                *g_msg_black = nullptr, *g_msg_white = nullptr;
size_t                   g_msg_black_cap = 0, g_msg_white_cap = 0;
uint32_t                *g_msg_src     = nullptr;
size_t                   g_msg_src_cap = 0, g_msg_buf_cap = 0;
uint32_t                *g_msg_buf = nullptr;
uint8_t                  g_msg_vis_map[MSG_MAP_ROWS * MSG_MAP_MAX_TW];
uint8_t                  g_msg_drawn_map[MSG_MAP_ROWS * MSG_MAP_MAX_TW];

template <class T>
bool grow(T *&p, size_t &cap, size_t n);

bool __cdecl msgs_scaled_draw() {
    uilayout::layout L;
    const int        fw = at<int>(mh::addr::WindowWidth), fh = at<int>(mh::addr::WindowHeight);
    const int        tw = at<int>(mh::addr::_G_LLM_VIEW_TILES_W);
    const int        gm = at<int>(mh::addr::_G_LLM_GAME_MODE); // 2 strategic, 3 the ESC menu etc. over it (the game still prints its notices behind the dialog)
    if (!g_msg_tramp || (gm != 2 && gm != 3) || tw <= 0 || tw > MSG_MAP_MAX_TW || !scaled_layout(L, fw, fh)) {
        g_msg_stamp.clear();
        return false;
    }
    // wrap at the width the screen shows: the visible map width in frame pixels
    int ww = (int)((int64_t)L.map_w * 100 / L.pct);
    if (ww > MSG_MAX_W) ww = MSG_MAX_W;
    if (ww > fw - uilayout::PANEL_W) ww = fw - uilayout::PANEL_W;
    if (ww < 160) ww = 160;
    const size_t n = (size_t)ww * MSG_H;
    if (!grow(g_msg_black, g_msg_black_cap, n) || !grow(g_msg_white, g_msg_white_cap, n) || !grow(g_msg_src, g_msg_src_cap, n)) {
        g_msg_stamp.clear();
        return false;
    }
    scratch_cfg c;
    c.win_w = ww, c.win_h = MSG_H;
    c.game_win = true; // the routine wraps at the viewport width G_WIN_W
    c.vis = g_msg_vis_map, c.drawn = g_msg_drawn_map;
    c.clip         = true;
    const long any = scratch_render_bw(SCRATCH_NOTICES, c, ww, MSG_H, g_msg_black, g_msg_white, g_msg_src, [] { ((cursor_fn)g_msg_tramp)(); });
    g_msg_w        = ww;
    g_msg_h        = MSG_H;
    if (any)
        stamp_capture(g_msg_stamp);
    else
        g_msg_stamp.clear();
    return true;
}

// clang-format off
__declspec(naked) void msgs_thunk() {
    __asm {
        pushad
        pushfd
        call msgs_scaled_draw
        test al, al
        jz   run_original
        popfd
        popad
        ret
    run_original:
        popfd
        popad
        jmp  dword ptr [g_msg_tramp]
    }
}
// clang-format on

void install_msgs_hook() {
    if (g_msg_hook_tried || !g_msg_enabled) return;
    g_msg_hook_tried = true;
    if (*(const uint64_t *)ADDR_FLOAT_MSGS != mh::exp::entry_llm_strat_draw_floating_messages) {
        gfx_log("; [ui] scaled notices NOT armed -- the message routine's entry bytes differ (wrong build, or hooked)");
        return;
    }
    if (mh::hook::install_trampoline(ADDR_FLOAT_MSGS, (void *)&msgs_thunk, &g_msg_tramp, 8, mh::hook::entry_claim::exclusive,
                                     "the RES20 scaled notices (llm_strat_draw_floating_messages)", 0))
        gfx_log("; [ui] scaled notices armed (llm_strat_draw_floating_messages detour): notices are drawn at ui_scale");
    else
        gfx_log("; [ui] scaled notices NOT armed -- the entry was refused (see the [interlock] line)");
}

// ---- RES4 stage 6b: translucent dialog layers --------------------------------------------------------------------------
//
// The game draws every gameplay-size dialog with ONE routine, llm_ui_widget_list_draw (0x004b7384, EAX = the list). Its frame is a RLE
// sprite whose blend ops (BNK_FORMAT.md section 5) make the dialog translucent over the map. Stage 6 scaled that 1x copy out of the frame,
// which carried the map's pixels (under the dialog) along. In 6b, with ui_scale above 100 and the list being a dialog the presenter
// scales (dialogs_now + a layout quad for it), this detour:
//   * runs the STOCK draw three times into a frame-shaped scratch (pitch WindowWidth*2, WindowWidth unchanged, the real dirty maps
//     kept: the stamps are idempotent and the next frame repaints all anyway), the dialog's rect cleared to black, white and mid grey,
//     and recovers a premultiplied B8G8R8A8 layer per pixel (layer_pixel_from_passes);
//   * draws NOTHING into the game frame, so the frame under the dialog is the true background (a lower dialog's layer is
//     composited under the upper one by the presenter);
//   * the presenter draws the layer scaled, alpha-blended, above the HUD quads and below the cursor (frame_view::overlays).
// Only the five per-frame callers are taken (return addresses below); any other draw of a list runs stock, and a dialog without a layer
// is still drawn from the frame by its stage 6 quad. [video] ui_scale_dialog_check=1 (dev) additionally draws the list over the true
// background in the stock way and diffs it against the layer composited at 1x: the max error is logged. Presentation only.
constexpr uintptr_t ADDR_LIST_DRAW      = 0x004b7384u;
constexpr uintptr_t ADDR_UI_SPRITE_BASE = 0x00520395u;                                                                    // [x] - 1 = _G_LLM_GFX_UI_SPRITE_BASE_INDEX
constexpr uintptr_t LIST_DRAW_CALLERS[] = {0x004b7f04u, 0x004b7f17u, 0x004c7c79u, 0x004c7c8cu, 0x004c75b1u, 0x004cac4fu}; // return addresses (0x004cac4f: llm_ui_paged_list_frame, the info screen)

// The stock list draw writes only the frame target and the dirty-tile stamps (idempotent: the next frame repaints all anyway), so drawing it three
// more times into a scratch is harmless; the real dirty maps are kept.
constexpr scratch_target SCRATCH_LIST = {"dialog list", ADDR_LIST_DRAW, "stores only to the frame target and the dirty-tile stamps, which are idempotent (the next frame repaints all anyway)"};

struct dlg_layer {
    overlay_stamp   stamp; // RES23-S5: the present + HUD epoch this layer was captured for (seq 0 = none)
    uintptr_t       list = 0;
    uilayout::irect r;
    uint32_t       *px  = nullptr;
    size_t          cap = 0;
};
dlg_layer g_layers[uilayout::MAX_DIALOGS];

// RES16: the info screen (game mode 4). Its AVI clip is a widget draw callback that BltFast's into the frame (outside the list's own drawing), so
// a recovered list layer lacks it. While the layer passes run, the info-clip seam (seams/ui_info_avi.cpp) is told which pass this is
// (owned_info_layer_pass: 1 = the first two, where it does nothing, 2 = the last, where it decodes the clip and STASHES it instead of blitting it
// into the frame); the stash is composited into the layer as opaque pixels, and the seam's post step (the cursor and the flip the tick would have
// run) runs once the layer is stored. Mode 4 is layered only when the seam registered that post step (its tick guard is armed).
int g_info_pass       = 0;
void (*g_info_post)() = nullptr;
struct clip_stash {
    bool      valid = false;
    int       x = 0, y = 0, w = 0, h = 0;
    uint16_t *px  = nullptr;
    size_t    cap = 0;
};
clip_stash g_clip;
void      *g_list_tramp                     = nullptr;
bool       g_list_hook_tried                = false;
bool       g_dlg_check                      = false; // [video] ui_scale_dialog_check
uint16_t  *g_dscratch                       = nullptr;
size_t     g_dscratch_cap                   = 0;
uint16_t  *g_dpass[5]                       = {};
size_t     g_dpass_cap                      = 0;
uint32_t  *g_dov_buf[uilayout::MAX_DIALOGS] = {};
size_t     g_dov_cap[uilayout::MAX_DIALOGS] = {};

bool layer_valid_for(int slot, uintptr_t list, const uilayout::irect &r) {
    if (slot < 0 || slot >= uilayout::MAX_DIALOGS) return false;
    const dlg_layer &l = g_layers[slot];
    return l.stamp.fresh(ov_window()) && l.list == list && l.r.x0 == r.x0 && l.r.y0 == r.y0 && l.r.x1 == r.x1 && l.r.y1 == r.y1;
}

void layers_drop_all() {
    for (auto &l : g_layers) l.stamp.clear();
}

template <class T>
bool grow(T *&p, size_t &cap, size_t n) {
    if (cap >= n) return true;
    T *q = (T *)realloc(p, n * sizeof(T));
    if (!q) return false;
    p   = q;
    cap = n;
    return true;
}

void net_log(const char *fmt, ...) {
    static char          path[MAX_PATH];
    static unsigned long gen = 0;
    mh_proc_path(path, MAX_PATH, "%smh_net.log", &gen);
    char    line[600];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(path, line, lstrlenA(line));
}

// One-time opcode histogram of the dialog frame sprites (ESC / Options panel = widget disp 41, generic dialog frame = disp 40):
// the V1 RLE stream at sprite header + 0x0E. Pixel counts per op; decides whether black+white recovery is exact (ops 0x40 / 0x60 are not).
void log_dialog_blend_ops_once() {
    static bool done = false;
    if (done) return;
    done                = true;
    const int   base    = at<int>(ADDR_UI_SPRITE_BASE) - 1;
    const char *nm[2]   = {"esc_options_panel_disp41", "generic_dialog_frame_disp40"};
    const int   disp[2] = {41, 40};
    for (int k = 0; k < 2; ++k) {
        const int id = base + disp[k];
        if (id < 0 || id >= 22000) continue;
        const uint8_t *hdr     = (const uint8_t *)(uintptr_t)(at<uint32_t>(mh::addr::_G_LLM_GFX_BANK_PIXELS) +
                                                          ((const uint32_t *)mh::addr::_G_LLM_SPRITE_PIX_OFFSETS)[id]);
        const uint8_t *p       = hdr + 0x0E;
        long           cmds[8] = {}, px[8] = {};
        long           rows = 0;
        for (int guard = 0; guard < 400000; ++guard) {
            const uint8_t b = *p++;
            if (b == 0xFF) break;
            if (b == 0xFE) {
                ++rows;
                continue;
            }
            const int t = b >> 5, n = b & 0x1F;
            ++cmds[t];
            px[t] += n;
            switch (t) {
                case 1: p += n; break;     // opaque: 1 byte per pixel
                case 2: p += 2 * n; break; // 0x40 palette + scaled background
                case 3: p += 2 * n; break; // 0x60 clamped
                case 4: p += n; break;     // 0x80 tint
                default: break;
            }
        }
        net_log("; RES4: dialog blend ops %s sprite %d: rows %ld cmds/px: skip %ld/%ld opaque %ld/%ld op40 %ld/%ld op60 %ld/%ld tint80 %ld/%ld "
                "halveA0 %ld/%ld skiplinesC0 %ld",
                nm[k], id, rows, cmds[0], px[0], cmds[1], px[1], cmds[2], px[2], cmds[3], px[3], cmds[4], px[4], cmds[5], px[5], cmds[6]);
    }
}

__declspec(naked) void call_list_stock(uintptr_t) {
    __asm {
        pushad
        mov  eax, [esp + 36]
        call dword ptr [g_list_tramp]
        popad
        ret
    }
}

inline int c8(uint16_t p, int c) { // channel c (0 R, 1 G, 2 B) of an RGB565 pixel, expanded to 8 bits
    if (c == 0) return ((p >> 11) & 0x1f) * 255 / 31;
    if (c == 1) return ((p >> 5) & 0x3f) * 255 / 63;
    return (p & 0x1f) * 255 / 31;
}

// A premultiplied layer pixel over an RGB565 backdrop, back to RGB565 (truncated like the game's own blend, whose LUT terms all floor).
uint16_t comp565(uint32_t pm, uint16_t bg) {
    const int a = (int)(pm >> 24);
    int       o[3];
    const int pc[3] = {(int)((pm >> 16) & 0xff), (int)((pm >> 8) & 0xff), (int)(pm & 0xff)};
    for (int c = 0; c < 3; ++c) {
        o[c] = pc[c] + (c8(bg, c) * (255 - a) + 127) / 255;
        if (o[c] > 255) o[c] = 255;
    }
    return (uint16_t)(((o[0] * 31 / 255) << 11) | ((o[1] * 63 / 255) << 5) | (o[2] * 31 / 255)); // truncation: the game's LUTs floor each term
}

// The dev proof: the list drawn the stock way over the true backdrop (frame rect + the lower layers) vs the recovered layer composited at 1x.
// The scratch is the current frame globals' target when this runs.
void dialog_check(int slot, uintptr_t list, const uilayout::irect &r, int fw, int saved_fb, int saved_pitch) {
    const int w = r.w(), h = r.h();
    uint16_t *bg = g_dpass[3], *st = g_dpass[4];
    for (int y = 0; y < h; ++y)
        memcpy(bg + (size_t)y * w, (const uint8_t *)(intptr_t)saved_fb + (size_t)(r.y0 + y) * saved_pitch + (size_t)r.x0 * 2, (size_t)w * 2);
    for (int j = 0; j < slot; ++j) { // the lower dialogs' layers are part of the backdrop
        if (!g_layers[j].stamp.seq) continue;
        const uilayout::irect &lr = g_layers[j].r;
        for (int y = lr.y0; y < lr.y1; ++y)
            for (int x = lr.x0; x < lr.x1; ++x)
                if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
                    uint16_t &b = bg[(size_t)(y - r.y0) * w + (x - r.x0)];
                    b           = comp565(g_layers[j].px[(size_t)(y - lr.y0) * lr.w() + (x - lr.x0)], b);
                }
    }
    for (int y = 0; y < h; ++y) memcpy(g_dscratch + (size_t)(r.y0 + y) * fw + r.x0, bg + (size_t)y * w, (size_t)w * 2);
    call_list_stock(list);
    for (int y = 0; y < h; ++y) memcpy(st + (size_t)y * w, g_dscratch + (size_t)(r.y0 + y) * fw + r.x0, (size_t)w * 2);
    int  mx[3] = {0, 0, 0};
    long over  = 0;
    char badtxt[3][200];
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint16_t o = comp565(g_layers[slot].px[i], bg[i]), s = st[i];
        const int      e[3] = {abs(((o >> 11) & 31) - ((s >> 11) & 31)), abs(((o >> 5) & 63) - ((s >> 5) & 63)), abs((o & 31) - (s & 31))};
        bool           bad  = false;
        for (int c = 0; c < 3; ++c) {
            if (e[c] > mx[c]) mx[c] = e[c];
            if (e[c] > 1) bad = true;
        }
        if (bad && ++over <= 3) snprintf(badtxt[over - 1], sizeof(badtxt[0]), "bad px %d,%d: bg %04x stock %04x ours %04x | black %04x white %04x grey %04x layer %08x",
                                         (int)(i % w), (int)(i / w), bg[i], s, o, g_dpass[0][i], g_dpass[1][i], g_dpass[2][i], (unsigned)g_layers[slot].px[i]);
    }
    // One line per distinct result (a steady dialog is checked every frame).
    static long lastkey[uilayout::MAX_DIALOGS][6] = {};
    const long  key[6]                            = {(long)list, ((long)r.x0 << 16) | r.y0, mx[0], mx[1], mx[2], over};
    if (memcmp(lastkey[slot], key, sizeof(key)) != 0) {
        memcpy(lastkey[slot], key, sizeof(key));
        net_log("; RES4: dialog layer check slot %d list %08x rect %d,%d %dx%d: max err r=%d g=%d b=%d (565 units), %ld of %d px above 1 -> %s", slot,
                (unsigned)list, r.x0, r.y0, w, h, mx[0], mx[1], mx[2], over, w * h, over == 0 ? "PASS" : "FAIL");
        for (long k = 0; k < over && k < 3; ++k) net_log("; RES4: dialog layer check %s", badtxt[k]);
    }
}

// The detour body. True = the list was captured as a layer (the caller returns without drawing it).
bool __cdecl dialog_list_draw(uintptr_t list, uintptr_t ret) {
    bool ours = false;
    for (uintptr_t a : LIST_DRAW_CALLERS) ours |= a == ret;
    if (!ours || !g_list_tramp || !g_active) return false;
    const int fw = at<int>(mh::addr::WindowWidth), fh = at<int>(mh::addr::WindowHeight);
    const int m = at<int>(mh::addr::_G_LLM_GAME_MODE);
    if (m != 2 && m != 3 && m != 4 && m != 5) return false;
    if (m == 4 && !g_info_post) return false;
    uilayout::layout L; // the ONE hook layout (scaled_layout): the presenter's, crop included; false = the fitted base, the game draws the dialog into the frame
    if (fw <= 0 || fh <= 0 || !scaled_layout(L, fw, fh)) return false;
    uilayout::dialog_set ds;
    uintptr_t            lists[uilayout::MAX_DIALOGS] = {};
    dialogs_now(fw, fh, ds, lists);
    int slot = -1;
    for (int i = 0; i < ds.n; ++i)
        if (lists[i] == list) slot = i;
    if (slot < 0) return false;
    bool quad = false;
    for (int j = 0; j < L.ndialogs; ++j) {
        const uilayout::irect &s = L.dialog_src[j], &d = ds.r[slot];
        quad |= s.x0 == d.x0 && s.y0 == d.y0 && s.x1 == d.x1 && s.y1 == d.y1;
    }
    if (!quad) return false;

    const uilayout::irect r = ds.r[slot];
    const int             w = r.w(), h = r.h();
    const size_t          n = (size_t)w * h;
    if (!grow(g_dscratch, g_dscratch_cap, (size_t)fw * fh) || !grow(g_dpass[0], g_dpass_cap, n * 5)) return false;
    for (int i = 1; i < 5; ++i) g_dpass[i] = g_dpass[0] + n * i; // one allocation, five w*h arrays
    if (!grow(g_layers[slot].px, g_layers[slot].cap, n)) return false;
    log_dialog_blend_ops_once();

    static const uint16_t fills[3] = {0x0000, 0xFFFF, 0x8410};
    uint32_t             *px       = g_layers[slot].px;
    long                  grey = 0, covered = 0;
    {
        scratch_scope scope(SCRATCH_LIST, scratch_cfg()); // a frame-shaped scratch: WindowWidth unchanged, the real dirty maps kept
        if (!scope.ok()) return false;
        const int saved_fb = scope.saved_fb(), saved_pitch = scope.saved_pitch();
        scope.retarget(g_dscratch, fw);
        g_clip.valid = false;
        for (int p = 0; p < 3; ++p) {
            scratch_fill_rect(g_dscratch, fw, r.x0, r.y0, w, h, fills[p]);
            g_info_pass = m == 4 ? (p == 2 ? 2 : 1) : 0;
            call_list_stock(list);
            g_info_pass = 0;
            for (int y = 0; y < h; ++y) memcpy(g_dpass[p] + (size_t)y * w, g_dscratch + (size_t)(r.y0 + y) * fw + r.x0, (size_t)w * 2);
        }
        for (size_t i = 0; i < n; ++i) {
            bool ug;
            px[i] = uilayout::layer_pixel_from_passes(g_dpass[0][i], g_dpass[1][i], g_dpass[2][i], &ug);
            grey += ug;
            covered += (px[i] >> 24) != 0;
        }
        if (m == 4 && g_clip.valid) { // the AVI clip: opaque pixels of the layer, where the tick would have blitted it into the frame
            for (int cy = 0; cy < g_clip.h; ++cy) {
                const int ly = g_clip.y + cy - r.y0;
                if (ly < 0 || ly >= h) continue;
                for (int cx = 0; cx < g_clip.w; ++cx) {
                    const int lx = g_clip.x + cx - r.x0;
                    if (lx < 0 || lx >= w) continue;
                    const uint16_t c  = g_clip.px[(size_t)cy * g_clip.w + cx];
                    const uint32_t r5 = (c >> 11) & 0x1f, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;
                    px[(size_t)ly * w + lx] = 0xFF000000u | (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
                }
            }
        }
        stamp_capture(g_layers[slot].stamp);
        g_layers[slot].list = list;
        g_layers[slot].r    = r;
        if (g_dlg_check) {
            dialog_check(slot, list, r, fw, saved_fb, saved_pitch);
            static long reported = -1;
            if (grey != reported) {
                reported = grey;
                net_log("; RES4: dialog layer slot %d: %ld of %d px recovered with the grey pass, %ld px non-transparent", slot, grey, w * h, covered);
            }
        }
    } // the scratch scope ends: the frame globals are the real frame's again before the info tick's post step
    if (m == 4 && g_info_post) g_info_post(); // the cursor + flip the info tick skipped (the layer is complete now)
    return true;
}

// clang-format off
__declspec(naked) void list_thunk() {
    __asm {
        pushad
        pushfd
        mov  ecx, [esp + 36]
        push ecx
        push eax
        call dialog_list_draw
        add  esp, 8
        test al, al
        jz   run_original
        popfd
        popad
        ret
    run_original:
        popfd
        popad
        jmp  dword ptr [g_list_tramp]
    }
}
// clang-format on

void install_dialog_hook() {
    if (g_list_hook_tried) return;
    g_list_hook_tried = true;
    if (*(const uint64_t *)ADDR_LIST_DRAW != mh::exp::entry_llm_ui_widget_list_draw) {
        gfx_log("; [ui] dialog layers NOT armed -- the list draw's entry bytes differ (wrong build, or hooked)");
        return;
    }
    if (mh::hook::install_trampoline(ADDR_LIST_DRAW, (void *)&list_thunk, &g_list_tramp, 8, mh::hook::entry_claim::exclusive,
                                     "the RES4 translucent dialog layers (llm_ui_widget_list_draw)", 0))
        gfx_log("; [ui] dialog layers armed (llm_ui_widget_list_draw detour): scaled dialogs stay translucent");
    else
        gfx_log("; [ui] dialog layers NOT armed -- the entry was refused (see the [interlock] line)");
}

// RES23-S5: THE overlay list of a fw x fh frame shown now, bottom to top: the scaled notice, the layered dialogs, the cursor -- each placed per ONE
// layout (L) and taken only when its capture is stamped for this frame (ov_window: the present being built inside present_surface, the frame the
// framebuffer holds elsewhere) under the HUD epoch in force. A stale source is dropped, never drawn. The callers keep their own gates (the cursor and
// notice need ui_scale asked or in force, the dialogs need it in force) through `want_*`. sprite_bbox (optional) gets the cursor sprite's box.
struct overlay_want {
    bool cursor = false, dialogs = false, notices = false;
};
// RES23-S7 defensive: a source captured (so kept OUT of the frame) for the frame in question that the layout cannot place is drawn nowhere -- the bug that
// hid the cursor and the ESC menu at 1280x720 @175. Logged once per HUD epoch and kind; the hooks decide from the same layout (scaled_layout), so a line
// here means they disagree again.
void log_unplaced_once(const char *what, const overlay_window &win) {
    static unsigned last_epoch[3] = {~0u, ~0u, ~0u};
    const int       k             = what[0] == 'c' ? 0 : what[0] == 'd' ? 1
                                                                        : 2;
    if (last_epoch[k] == win.epoch) return;
    last_epoch[k] = win.epoch;
    gfx_log("; [ui] OVERLAY LOST: a %s was captured for this frame (kept out of it) but the present layout places no overlay for it (epoch %u)", what, (unsigned)win.epoch);
}
void overlay_collect(overlay_list &out, int fw, int fh, const overlay_want &want, int *sprite_bbox = nullptr) {
    out = overlay_list();
    uilayout::layout L;
    if (!g_active) return;
    const overlay_window win = ov_window();
    if (!scaled_layout(L, fw, fh)) {
        if (want.cursor && g_cur_stamp.fresh(win)) log_unplaced_once("cursor", win);
        if (want.notices && g_msg_stamp.fresh(win)) log_unplaced_once("notice", win);
        if (want.dialogs)
            for (const dlg_layer &c : g_layers)
                if (c.stamp.fresh(win)) {
                    log_unplaced_once("dialog layer", win);
                    break;
                }
        return;
    }
    if (want.notices && g_msg_stamp.fresh(win)) {
        const size_t             need = (size_t)L.screen_w * L.screen_h;
        uilayout::cursor_overlay ov;
        if (grow(g_msg_buf, g_msg_buf_cap, need) && uilayout::place_scaled_layer(L, g_msg_src, g_msg_w, g_msg_h, 0, 0, g_msg_buf, (int)g_msg_buf_cap, ov))
            out.add(ov, overlay_slot::notice);
    }
    if (want.dialogs) {
        int n = 0;
        for (int j = 0; j < L.ndialogs && n < uilayout::MAX_DIALOGS; ++j) {
            if (!L.dialog_layered[j]) continue;
            const dlg_layer *lay = nullptr;
            for (const dlg_layer &c : g_layers) {
                const uilayout::irect &s = L.dialog_src[j];
                if (c.stamp.fresh(win) && c.r.x0 == s.x0 && c.r.y0 == s.y0 && c.r.x1 == s.x1 && c.r.y1 == s.y1) lay = &c;
            }
            if (!lay) continue;
            const uilayout::irect &d = L.dialog_dst[j];
            if (!grow(g_dov_buf[n], g_dov_cap[n], (size_t)d.w() * d.h())) continue;
            uilayout::cursor_overlay ov;
            if (uilayout::place_dialog_layer(L, j, lay->px, g_dov_buf[n], (int)g_dov_cap[n], ov)) {
                out.add(ov, overlay_slot::dialog0 + n);
                ++n;
            }
        }
    }
    if (want.cursor && g_cur_stamp.fresh(win)) {
        uilayout::cursor_overlay ov;
        if (uilayout::place_cursor(L, g_cur_src, g_cur_fx, g_cur_fy, g_cur_buf, (int)(sizeof(g_cur_buf) / sizeof(g_cur_buf[0])), ov)) {
            out.add(ov, overlay_slot::cursor);
            if (sprite_bbox)
                for (int i = 0; i < 4; ++i) sprite_bbox[i] = g_cur_bbox[i];
        }
    }
}

// One log line per distinct set of dialog quads (mh_video.log), and one when they are gone: the rig's checker reads them.
void log_dialog_quads(const uilayout::dialog_set &ds) {
    static int key[1 + 8 * uilayout::MAX_DIALOGS] = {-1};
    if (!g_view.valid) return;
    const uilayout::layout &L                                = g_view.layout; // (the quads of this present: g_view.dialogs == ds)
    int                     k[1 + 8 * uilayout::MAX_DIALOGS] = {ds.n};
    for (int i = 0; i < ds.n; ++i) {
        k[1 + 8 * i] = ds.r[i].x0, k[2 + 8 * i] = ds.r[i].y0, k[3 + 8 * i] = ds.r[i].x1, k[4 + 8 * i] = ds.r[i].y1;
    }
    for (int i = 0; i < L.ndialogs; ++i) k[5 + 8 * i] = L.dialog_dst[i].x0, k[6 + 8 * i] = L.dialog_dst[i].y0, k[7 + 8 * i] = L.dialog_dst[i].x1, k[8 + 8 * i] = L.dialog_dst[i].y1;
    if (!memcmp(key, k, sizeof(k))) return;
    memcpy(key, k, sizeof(k));
    if (ds.n == 0) gfx_log("; [ui] dialogs gone");
    int q = 0;
    for (int i = 0; i < ds.n; ++i) {
        const uilayout::irect &d = ds.r[i];
        if (q < L.ndialogs && L.dialog_src[q].x0 == d.x0 && L.dialog_src[q].y0 == d.y0 && L.dialog_src[q].x1 == d.x1 && L.dialog_src[q].y1 == d.y1) {
            const uilayout::irect &t = L.dialog_dst[q++];
            gfx_log("; [ui] dialog quad %d: frame %d,%d-%d,%d (%dx%d) -> screen %d,%d-%d,%d (%dx%d) at %d%%", i, d.x0, d.y0, d.x1, d.y1, d.w(), d.h(), t.x0, t.y0, t.x1, t.y1,
                    t.w(), t.h(), L.pct);
        } else
            gfx_log("; [ui] dialog %d: frame %d,%d-%d,%d (%dx%d): no quad (stays 1x)", i, d.x0, d.y0, d.x1, d.y1, d.w(), d.h());
    }
}

// ---- RES23-S0: the view epoch log line and the presenter-output capture (`bcapture`) ----------------------------------
//
// Both are OBSERVATION only: nothing here changes what is drawn. The epoch line is logged once per CHANGE of (pct in force, screen,
// frame, composition, game mode, presenter), never per frame, so a later stage can correlate a captured image with the geometry that
// produced it. `bcapture` asks the presenter for the image it really put on screen (cursor, dialog layers and overlays included);
// pcapture is the CPU re-composition of the same moment, and tools/check_bcapture.py compares the two.
char g_bcap_name[96];
bool g_bcap_pending = false;
int  g_view_epoch   = 0;

void log_view_epoch(const OwnSurface *s) {
    static int         key[9]       = {};
    static const void *last_backend = nullptr;
    const int          cw = g_view.valid ? g_view.screen_w : 0, ch = g_view.valid ? g_view.screen_h : 0;
    const int          kw = g_view.crop_w, kh = g_view.crop_h;
    const int          m   = at<int>(mh::addr::_G_LLM_GAME_MODE);
    const int          pct = g_cfg.ui_scale_pct;
    if (key[0] == pct && key[1] == cw && key[2] == ch && key[3] == s->w && key[4] == s->h && key[5] == kw && key[6] == kh && key[7] == m &&
        last_backend == g_backend)
        return;
    key[0] = pct, key[1] = cw, key[2] = ch, key[3] = s->w, key[4] = s->h, key[5] = kw, key[6] = kh, key[7] = m;
    last_backend             = g_backend;
    const uilayout::layout l = g_view.valid ? g_view.layout : uilayout::layout();
    gfx_log("; [ui] view epoch %d: present %u, pct in force %d (requested %d), screen %dx%d, frame %dx%d, composition %dx%d at %d,%d%s, game mode %d, presenter %s, hud epoch %u",
            ++g_view_epoch, (unsigned)g_presents, pct, g_ui_scale_req, cw, ch, s->w, s->h, l.used.w(), l.used.h(), l.used.x0, l.used.y0,
            l.single ? " (single quad)" : "", m, g_backend ? g_backend->name() : "none", g_stamp.epoch);
}

// RES23-S1: the view of this present, built ONCE from the presenter's real target size (the d3d11 swap chain is resized to the client rect first),
// the frame, the percentage in force (g_cfg.ui_scale_pct, synced just before), the integer flag, the dialogs up and the crop. The dev override
// [video] ui_screen still wins over the presenter's size, for both presenters.
void make_present_view(const OwnSurface *s) {
    int  cw = 0, ch = 0;
    bool have = false;
    if (g_ui_screen_w > 0 && g_ui_screen_h > 0) {
        cw   = g_ui_screen_w;
        ch   = g_ui_screen_h;
        have = true;
    } else if (g_backend && g_backend->target_size(cw, ch))
        have = true;
    else
        have = ui_screen_size(cw, ch);
    const int            pct = g_cfg.ui_scale_pct;
    uilayout::dialog_set ds;
    if (pct > uilayout::MIN_PCT) dialogs_now(s->w, s->h, ds);
    int kw = 0, kh = 0;
    crop_for_frame(s->w, s->h, kw, kh);
    if (!have) {
        g_view = present_view();
        return;
    }
    build_present_view(g_view, cw, ch, s->w, s->h, pct, g_cfg.integer_scale, &ds, kw, kh);
}

void present_surface(OwnSurface *s) {
    ++g_presents;
    struct in_present_guard { // the overlay stamps are read against THIS present until it returns (ov_window)
        in_present_guard() { g_in_present = true; }
        ~in_present_guard() { g_in_present = false; }
    } in_present;
    if (!g_backend) bind_backend(g_hwnd ? g_hwnd : at<HWND>(ADDR_HWND_MAIN));
    log_modules_once();
    ensure_geometry();
    {
        const int m = at<int>(mh::addr::_G_LLM_GAME_MODE);
        if (m == 2 || m == 5) {
            g_stamp.epoch = g_hud.epoch;
            g_stamp.pct   = effective_ui_pct_for(s->w, s->h);
            g_stamp.w = s->w, g_stamp.h = s->h;
        } else if (m != 3 && m != 4)
            g_stamp.w = g_stamp.h = 0;
    }
    int eff_logged = 0;
    if (g_ui_scale_req > uilayout::MIN_PCT || g_cfg.ui_scale_pct > uilayout::MIN_PCT) { // (the second term: a request back to 100 must release the quads)
        const int eff = effective_ui_pct();
        if (eff != g_cfg.ui_scale_pct) {
            g_cfg.ui_scale_pct = eff;
            if (g_backend) g_backend->set_config(g_cfg);
            gfx_log("; [ui] ui_scale in force: %d%% (game mode %d)", eff, at<int>(mh::addr::_G_LLM_GAME_MODE));
        }
        eff_logged = eff;
    }
    make_present_view(s); // RES23-S1: the ONE view of this present; the clip, the cursor, the dialogs, the backend and the logs below all read it
    if (eff_logged > uilayout::MIN_PCT) log_ui_layout_change(s->w, s->h, eff_logged, g_view.layout, g_view.crop_w);
    update_clip();
    g_limiter.pace(g_fps_limit);
    if (!g_backend) return;
    frame_view f;
    f.pixels = s->bits;
    f.width  = s->w;
    f.height = s->h;
    f.pitch  = s->pitch;
    f.format = pixel_format::rgb565;
    if (crop_for_frame(s->w, s->h, f.crop_w, f.crop_h)) log_crop_layout(s->w, s->h, f.crop_w, f.crop_h, g_view.layout); // RES18
    // RES23-S5: ONE overlay list (notice, dialog layers, cursor), every source stamped for this present or dropped (overlay_collect)
    overlay_want want;
    want.cursor = want.notices = g_ui_scale_req > uilayout::MIN_PCT || g_cfg.ui_scale_pct > uilayout::MIN_PCT; // (the second term: a request back to 100 must release the quads)
    if (g_cfg.ui_scale_pct > uilayout::MIN_PCT) {                                                              // RES4 stage 6: the dialog quad's source
        f.dialogs = g_view.dialogs;
        log_dialog_quads(f.dialogs);
        want.dialogs = true; // RES4 stage 6b
    }
    if (want.cursor || want.dialogs) overlay_collect(f.overlays, s->w, s->h, want);
    log_view_epoch(s);
    f.view          = &g_view;
    const bool bcap = g_bcap_pending;
    if (bcap) g_backend->arm_output_capture();
    g_backend->present(f); // false = a dropped frame (minimised, device lost), never an error
    if (bcap) {
        g_bcap_pending = false;
        std::vector<uint32_t> px;
        int                   w = 0, h = 0;
        if (g_backend->take_output_capture(px, w, h))
            MH_Capture_WriteBgra32(g_bcap_name, px.data(), w, h);
        else
            MH_Capture_WriteBgra32(g_bcap_name, nullptr, 0, 0); // logs "bcapture <name> SKIPPED"
    }
}

// Clip `d` to the destination bounds and move `s` by the same amount (1:1 copies only).
bool clip_pair(RECT &d, RECT &s, int dw, int dh, int sw, int sh) {
    if (d.left < 0) {
        s.left -= d.left;
        d.left = 0;
    }
    if (d.top < 0) {
        s.top -= d.top;
        d.top = 0;
    }
    if (s.left < 0) {
        d.left -= s.left;
        s.left = 0;
    }
    if (s.top < 0) {
        d.top -= s.top;
        s.top = 0;
    }
    int cw = d.right - d.left, ch = d.bottom - d.top;
    if (cw > s.right - s.left) cw = s.right - s.left;
    if (ch > s.bottom - s.top) ch = s.bottom - s.top;
    if (d.left + cw > dw) cw = dw - d.left;
    if (d.top + ch > dh) ch = dh - d.top;
    if (s.left + cw > sw) cw = sw - s.left;
    if (s.top + ch > sh) ch = sh - s.top;
    if (cw <= 0 || ch <= 0) return false;
    d.right  = d.left + cw;
    d.bottom = d.top + ch;
    s.right  = s.left + cw;
    s.bottom = s.top + ch;
    return true;
}

void copy_rect(OwnSurface *dst, RECT d, OwnSurface *src, RECT s) {
    const int dw = d.right - d.left, dh = d.bottom - d.top;
    const int sw = s.right - s.left, sh = s.bottom - s.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    // Same-surface blits: stage the source so an overlap cannot smear.
    uint8_t       *stage  = nullptr;
    const uint8_t *sbase  = src->bits;
    int            spitch = src->pitch;
    if (src == dst) {
        stage = (uint8_t *)VirtualAlloc(nullptr, (SIZE_T)sw * 2 * sh, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!stage) return;
        for (int y = 0; y < sh; ++y)
            memcpy(stage + (SIZE_T)y * sw * 2, src->bits + (SIZE_T)(s.top + y) * src->pitch + (SIZE_T)s.left * 2, (SIZE_T)sw * 2);
        sbase  = stage;
        spitch = sw * 2;
        s      = RECT{0, 0, sw, sh};
    }
    if (dw == sw && dh == sh) {
        RECT dd = d, ss = s;
        if (clip_pair(dd, ss, dst->w, dst->h, stage ? sw : src->w, stage ? sh : src->h)) {
            const SIZE_T row = (SIZE_T)(dd.right - dd.left) * 2;
            for (int y = 0; y < dd.bottom - dd.top; ++y)
                memcpy(dst->bits + (SIZE_T)(dd.top + y) * dst->pitch + (SIZE_T)dd.left * 2,
                       sbase + (SIZE_T)(ss.top + y) * spitch + (SIZE_T)ss.left * 2, row);
        }
    } else {
        // Scaled: nearest neighbour, destination-clipped, source indices from the UNclipped rects.
        const int y0 = d.top < 0 ? 0 : d.top, y1 = d.bottom > dst->h ? dst->h : d.bottom;
        const int x0 = d.left < 0 ? 0 : d.left, x1 = d.right > dst->w ? dst->w : d.right;
        const int slim_w = stage ? sw : src->w, slim_h = stage ? sh : src->h;
        for (int y = y0; y < y1; ++y) {
            const int sy = s.top + (int)((int64_t)(y - d.top) * sh / dh);
            if (sy < 0 || sy >= slim_h) continue;
            const uint16_t *srow = (const uint16_t *)(sbase + (SIZE_T)sy * spitch);
            uint16_t       *drow = (uint16_t *)(dst->bits + (SIZE_T)y * dst->pitch);
            for (int x = x0; x < x1; ++x) {
                const int sx = s.left + (int)((int64_t)(x - d.left) * sw / dw);
                if (sx >= 0 && sx < slim_w) drow[x] = srow[sx];
            }
        }
    }
    if (stage) VirtualFree(stage, 0, MEM_RELEASE);
}

// The present shape: llm_gfx_ddraw_present passes the window's client rect in SCREEN coordinates.
// Anything else is primary-local (see the header).
RECT map_primary_dest(const OwnSurface *prim, const RECT &r) {
    if (g_hwnd) {
        RECT  cr;
        POINT o = {0, 0};
        if (GetClientRect(g_hwnd, &cr) && ClientToScreen(g_hwnd, &o) && r.left == o.x && r.top == o.y &&
            r.right == o.x + cr.right && r.bottom == o.y + cr.bottom)
            return prim->full();
    }
    return r;
}

constexpr DWORD BLT_KNOWN = DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT | DDBLT_COLORFILL;

HRESULT STDMETHODCALLTYPE OwnSurface::Blt(LPRECT dst_rc, LPDIRECTDRAWSURFACE src_i, LPRECT src_rc, DWORD flags, LPDDBLTFX fx) {
    if (flags & ~BLT_KNOWN) {
        static bool once = false;
        if (!once) {
            once = true;
            gfx_log("; [gfx] Blt flags 0x%08lx carry bits this device ignores (0x%08lx) -- copied as a plain blit", flags,
                    flags & ~BLT_KNOWN);
        }
    }
    RECT d = dst_rc ? *dst_rc : full();
    if (primary && dst_rc) d = map_primary_dest(this, d);
    if (flags & DDBLT_COLORFILL) {
        if (!fx) return DDERR_INVALIDPARAMS;
        const uint16_t c  = (uint16_t)fx->dwFillColor;
        const int      x0 = d.left < 0 ? 0 : d.left, x1 = d.right > w ? w : d.right;
        const int      y0 = d.top < 0 ? 0 : d.top, y1 = d.bottom > h ? h : d.bottom;
        for (int y = y0; y < y1; ++y) {
            uint16_t *row = (uint16_t *)(bits + (SIZE_T)y * pitch);
            for (int x = x0; x < x1; ++x) row[x] = c;
        }
    } else {
        OwnSurface *src = static_cast<OwnSurface *>(src_i);
        if (!src) return DDERR_INVALIDPARAMS;
        const RECT s = src_rc ? *src_rc : src->full();
        copy_rect(this, d, src, s);
    }
    if (primary) present_surface(this);
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE OwnSurface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src_i, LPRECT src_rc, DWORD trans) {
    if (trans & (DDBLTFAST_SRCCOLORKEY | DDBLTFAST_DESTCOLORKEY)) {
        static bool once = false;
        if (!once) {
            once = true;
            gfx_log("; [gfx] BltFast colour-key flags 0x%08lx ignored -- copied opaque", trans);
        }
    }
    OwnSurface *src = static_cast<OwnSurface *>(src_i);
    if (!src) return DDERR_INVALIDPARAMS;
    const RECT s = src_rc ? *src_rc : src->full();
    const RECT d = {(LONG)x, (LONG)y, (LONG)x + (s.right - s.left), (LONG)y + (s.bottom - s.top)};
    copy_rect(this, d, src, s);
    if (primary) present_surface(this);
    return DD_OK;
}

// ---- the device ----------------------------------------------------------------------------------

struct OwnDDraw final : IDirectDraw {
    LONG ref = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override { return qi_self(this, riid, out, IID_DD, "IDirectDraw"); }
    ULONG STDMETHODCALLTYPE   AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE   Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE Compact() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, LPDIRECTDRAWCLIPPER *out, IUnknown *) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = new OwnClipper();
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD caps, LPPALETTEENTRY init, LPDIRECTDRAWPALETTE *out, IUnknown *) override {
        if (!out) return DDERR_INVALIDPARAMS;
        OwnPalette *p = new OwnPalette();
        p->caps       = caps;
        if (init) memcpy(p->entries, init, sizeof(p->entries));
        *out = p;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE *out, IUnknown *) override {
        if (!d || !out) return DDERR_INVALIDPARAMS;
        *out             = nullptr;
        const DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
        if (caps & DDSCAPS_OVERLAY) {
            // Only reachable if GetCaps advertised overlays, which it does not.
            gfx_log("; [gfx] CreateSurface: overlay requested -- refused (DDERR_NOOVERLAYHW)");
            return DDERR_NOOVERLAYHW;
        }
        if ((d->dwFlags & DDSD_PIXELFORMAT) && d->ddpfPixelFormat.dwRGBBitCount != 16) {
            gfx_log("; [gfx] CreateSurface: %lu bpp requested -- this device is 16 bpp only", d->ddpfPixelFormat.dwRGBBitCount);
            return DDERR_INVALIDPIXELFORMAT;
        }
        int w = 0, h = 0;
        if (caps & DDSCAPS_PRIMARYSURFACE) {
            w = g_mode_w > 0 ? g_mode_w : GetSystemMetrics(SM_CXSCREEN);
            h = g_mode_h > 0 ? g_mode_h : GetSystemMetrics(SM_CYSCREEN);
        } else {
            if (!(d->dwFlags & DDSD_WIDTH) || !(d->dwFlags & DDSD_HEIGHT) || (int)d->dwWidth <= 0 || (int)d->dwHeight <= 0)
                return DDERR_INVALIDPARAMS;
            w = (int)d->dwWidth;
            h = (int)d->dwHeight;
        }
        OwnSurface *s = new OwnSurface();
        if (!s->create(w, h, caps | ((caps & DDSCAPS_PRIMARYSURFACE) ? DDSCAPS_VISIBLE : 0))) {
            delete s;
            return DDERR_OUTOFMEMORY;
        }
        s->primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
        if (s->primary && (caps & DDSCAPS_FLIP) && (d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount >= 1) {
            OwnSurface *b = new OwnSurface();
            if (!b->create(w, h, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX)) {
                delete b;
                delete s;
                return DDERR_OUTOFMEMORY;
            }
            s->back = b;
        }
        *out = s;
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS drv, LPDDCAPS hel) override {
        // Report nothing: no overlays (llm_game_boot_init then takes the system-memory movie surface,
        // exactly as it does under dgVoodoo), no video memory to budget.
        LPDDCAPS both[2] = {drv, hel};
        for (LPDDCAPS c : both) {
            if (!c) continue;
            const DWORD sz = c->dwSize;
            if (sz >= sizeof(DWORD) && sz <= 0x400) {
                ZeroMemory(c, sz);
                c->dwSize = sz;
            }
        }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC d) override {
        if (!d) return DDERR_INVALIDPARAMS;
        ZeroMemory(d, sizeof(DDSURFACEDESC));
        d->dwSize          = sizeof(DDSURFACEDESC);
        d->dwFlags         = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d->dwWidth         = (DWORD)(g_mode_w > 0 ? g_mode_w : GetSystemMetrics(SM_CXSCREEN));
        d->dwHeight        = (DWORD)(g_mode_h > 0 ? g_mode_h : GetSystemMetrics(SM_CYSCREEN));
        d->lPitch          = (LONG)d->dwWidth * 2;
        d->dwRefreshRate   = 60;
        d->ddpfPixelFormat = pf565(); // the game picks 555 vs 565 from this green mask
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD flags) override {
        if (h) {
            g_hwnd = h;
            bind_backend(h);
            // DDSCL_FULLSCREEN MAPS THE WINDOW, and the game depends on it. Its frames are WM_PAINTs
            // (WinMain's idle loop RedrawWindow's, llm_wnd_proc runs llm_frame_dispatch), and Windows
            // paints no window that was never shown -- so under [video] no_window, whose byte patches
            // strip WS_VISIBLE and cut WinMain's ShowWindow, the game ran ZERO frames on this device
            // (measured 2026-09-28: script loaded, walk stalled, 0 presents) while under dgVoodoo the
            // same lane's keeper logs "window VISIBLE at x=0" from the first frame. So do what a real
            // exclusive-mode device does. SW_SHOWNA: no activation, no focus theft; the keeper then
            // parks (or hides, no_window_hide=1) it exactly as it did before.
            // Not under null (PT-GFX2): that window stays unmapped and own_RedrawWindow posts its frames.
            if ((flags & DDSCL_FULLSCREEN) && g_kind != backend_kind::null && !IsWindowVisible(h)) ShowWindow(h, SW_SHOWNA);
            // A PLAYER window must also take the foreground, which a real exclusive-mode device does as
            // part of DDSCL_EXCLUSIVE: without it the game started from a launcher or Explorer ran with
            // the launcher still focused (2026-09-28 report) -- keyboard dead until a click. Only when
            // the DLL owns the geometry: a headless lane (no_window / null) must never steal focus.
            if ((flags & DDSCL_EXCLUSIVE) && geometry_owned()) take_foreground(h);
        }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp) override {
        if (w == 0 || h == 0 || w > 16384 || h > 16384) return DDERR_INVALIDMODE;
        if (bpp != 16) {
            // Only llm_gfx_ddraw_teardown's flip-chain branch asks for 8 bpp (unreachable at flags=7).
            gfx_log("; [gfx] SetDisplayMode %lux%lux%lu refused -- 16 bpp only", w, h, bpp);
            return DDERR_INVALIDMODE;
        }
        if ((int)w != g_mode_w || (int)h != g_mode_h) gfx_log("; [gfx] SetDisplayMode %lux%lu (no real mode switch)", w, h);
        g_mode_w     = (int)w;
        g_mode_h     = (int)h;
        g_geom_tries = 0;
        if (g_backend) g_backend->on_mode(g_mode_w, g_mode_h, pixel_format::rgb565);
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { return DD_OK; }

    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE *) override { MH_GFX_UNSUPPORTED("IDirectDraw::DuplicateSurface"); }
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK) override { MH_GFX_UNSUPPORTED("IDirectDraw::EnumDisplayModes"); }
    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { MH_GFX_UNSUPPORTED("IDirectDraw::EnumSurfaces"); }
    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD, LPDWORD) override { MH_GFX_UNSUPPORTED("IDirectDraw::GetFourCCCodes"); }
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE *) override { MH_GFX_UNSUPPORTED("IDirectDraw::GetGDISurface"); }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD) override { MH_GFX_UNSUPPORTED("IDirectDraw::GetMonitorFrequency"); }
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD) override { MH_GFX_UNSUPPORTED("IDirectDraw::GetScanLine"); }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL) override { MH_GFX_UNSUPPORTED("IDirectDraw::GetVerticalBlankStatus"); }
    HRESULT STDMETHODCALLTYPE Initialize(GUID *) override { return DDERR_ALREADYINITIALIZED; }
};

// ---- PT-GFX2: a window that is never mapped still gets its frames ----------------------------------
//
// Every game frame is a WM_PAINT: WinMain's idle loop is `while (!PeekMessage) { RedrawWindow(hwnd, 0,
// 0, RDW_INVALIDATE|RDW_INTERNALPAINT|RDW_NOERASE); WaitMessage(); }` (0x004a0cb3 / 0x004a0cba, the
// ONLY RedrawWindow call in the binary), and llm_wnd_proc runs llm_frame_dispatch on WM_PAINT, whose
// BeginPaint/EndPaint pair is all the painting it does. Windows neither invalidates nor internal-
// paints an INVISIBLE window, so an unmapped game ran zero frames (PT-GFX1, 2026-09-28) -- or ~2 s
// apart under dgVoodoo's no_window_hide (measured 2026-07-28: 92% of a hidden menu_walk spent in
// message waits, 8x the wall clock of a parked-visible one). So the exe's RedrawWindow import is replaced: for a visible window it is the real call;
// for an invisible one it POSTS the WM_PAINT itself. The post lands between the loop's failed
// PeekMessage and its WaitMessage, so WaitMessage returns at once and the next PeekMessage
// dispatches exactly one frame -- the same one-frame-per-idle-pass cadence a mapped window gets.
// Posting is safe only because nothing in this message's handler depends on an update region.
//
// backend=null additionally keeps the window from ever being mapped: the exe's CreateWindowExA
// drops WS_VISIBLE (main window, splash, and the sound notify window -- the only three callers) and
// its ShowWindow swallows every show of the main window and the splash (never a dialog's). That is
// independent of [video] no_window's byte patches and composes with them (no_window re-points
// ShowWindow at its own swallow afterwards).
using redraw_fn           = BOOL(WINAPI *)(HWND, const RECT *, HRGN, UINT);
using create_fn           = HWND(WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using show_fn             = BOOL(WINAPI *)(HWND, int);
redraw_fn g_real_redraw   = nullptr;
create_fn g_real_create   = nullptr;
show_fn   g_real_show     = nullptr;
long      g_posted_paints = 0;
long      g_swallow_shows = 0;

BOOL WINAPI own_RedrawWindow(HWND h, const RECT *rc, HRGN rgn, UINT flags) {
    if (h && !IsWindowVisible(h) && (flags & (RDW_INVALIDATE | RDW_INTERNALPAINT))) {
        if (++g_posted_paints == 1)
            gfx_log("; [gfx] window hwnd=%p is not mapped -- frames driven by posted WM_PAINT (PT-GFX2)", (void *)h);
        return PostMessageA(h, WM_PAINT, 0, 0);
    }
    if (g_kind == backend_kind::null && h && IsWindowVisible(h)) {
        // Detector: something mapped the null backend's window. Unmap it and say who we are.
        static int reports = 0;
        if (reports < 4) {
            ++reports;
            gfx_log("; [gfx] null backend: window hwnd=%p found MAPPED -- hidden again", (void *)h);
        }
        ShowWindow(h, SW_HIDE); // mh.dll's own import, not the swallowed exe slot
        return PostMessageA(h, WM_PAINT, 0, 0);
    }
    return g_real_redraw ? g_real_redraw(h, rc, rgn, flags) : RedrawWindow(h, rc, rgn, flags);
}

HWND WINAPI own_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int hgt,
                                HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    // Top-level windows only (main, splash, the sound notify window); a child keeps its style.
    if (!(style & WS_CHILD)) {
        style &= ~(DWORD)WS_VISIBLE;
        ex &= ~(DWORD)WS_EX_TOPMOST;
    }
    return g_real_create(ex, cls, name, style, x, y, w, hgt, parent, menu, inst, param);
}

// Only the MAIN window and the splash are kept unmapped. The other callers of the exe's ShowWindow are
// the error/message dialogs (win_ShowErrorMessage, llm_ui_dialog_message_show, llm_ui_error_dialog_
// proc): swallowing THEIR shows would leave a modal nobody can see or dismiss -- a hang, not headless.
bool keep_unmapped(HWND h) {
    if (!h) return false;
    if (h == at<HWND>(ADDR_HWND_MAIN) || h == g_hwnd) return true;
    char cls[32];
    return GetClassNameA(h, cls, sizeof(cls)) > 0 && lstrcmpiA(cls, "tgl_splash") == 0;
}

BOOL WINAPI own_ShowWindow(HWND h, int cmd) {
    if (cmd == SW_HIDE || !keep_unmapped(h)) return g_real_show(h, cmd);
    ++g_swallow_shows;
    return FALSE; // "was hidden" -- the honest answer; WinMain and the splash discard it
}

void install_headless_window_hooks() {
    g_real_redraw = (redraw_fn)exe_iat_replace("user32.dll", "RedrawWindow", (void *)&own_RedrawWindow);
    if (!g_real_redraw) gfx_log("; [gfx] RedrawWindow import not found -- an unmapped window will run no frames");
    if (g_kind != backend_kind::null) return;
    g_real_create = (create_fn)exe_iat_replace("user32.dll", "CreateWindowExA", (void *)&own_CreateWindowExA);
    g_real_show   = (show_fn)exe_iat_replace("user32.dll", "ShowWindow", (void *)&own_ShowWindow);
    if (g_real_create && g_real_show)
        gfx_log("; [gfx] null backend: exe CreateWindowExA drops WS_VISIBLE, ShowWindow swallowed -- the window is never mapped");
    else
        gfx_log("; [gfx] null backend: window hooks INCOMPLETE (CreateWindowExA=%d ShowWindow=%d)", (int)(g_real_create != nullptr),
                (int)(g_real_show != nullptr));
    // A half-installed pair must not leave a slot pointing at a stub whose real target is null.
    if (g_real_create && !g_real_show) exe_iat_replace("user32.dll", "CreateWindowExA", (void *)g_real_create);
    if (g_real_show && !g_real_create) exe_iat_replace("user32.dll", "ShowWindow", (void *)g_real_show);
}

// ---- DirectDrawCreate + the replaced loader pair -------------------------------------------------

long g_creates = 0;

HRESULT WINAPI own_DirectDrawCreate(GUID *, LPDIRECTDRAW *out, IUnknown *) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new OwnDDraw();
    if (++g_creates == 1) gfx_log("; [gfx] DirectDrawCreate -> owned IDirectDraw (DDRAW.DLL not loaded)");
    return DD_OK;
}

// llm_gfx_ddraw_dll_acquire, replaced: publish our proc and a non-null "module" (mh.dll's own handle,
// never passed to FreeLibrary -- see own_release), then bump the refcount like the original.
void __cdecl own_acquire() {
    at<HMODULE>(ADDR_DDRAW_MODULE) = g_self;
    at<void *>(ADDR_DDRAW_CREATE)  = (void *)&own_DirectDrawCreate;
    at<int>(ADDR_DDRAW_REFCOUNT) += 1;
}

// llm_gfx_ddraw_dll_release, replaced: decrement (floored at 0), and NEVER FreeLibrary or clear the
// cached proc -- the module handle is mh.dll's, and a cleared proc would send the next acquire to
// LoadLibraryA("DDRAW.DLL") if anything ever reached the original body again.
void __cdecl own_release() {
    int &rc = at<int>(ADDR_DDRAW_REFCOUNT);
    if (rc > 0) --rc;
}

// Both originals are __watcall void(void) that preserve every register but EAX (acquire saves ECX/EDX,
// release EBX/ECX/EDX/ESI/EBP); the thunks preserve everything, flags included.
// clang-format off
__declspec(naked) void acquire_thunk() {
    __asm {
        pushad
        pushfd
        call own_acquire
        popfd
        popad
        ret
    }
}
__declspec(naked) void release_thunk() {
    __asm {
        pushad
        pushfd
        call own_release
        popfd
        popad
        ret
    }
}
// clang-format on

backend_kind parse_kind(const char *v, bool *ok) {
    *ok = true;
    if (lstrcmpiA(v, "own") == 0 || lstrcmpiA(v, "gdi") == 0) return backend_kind::gdi;
    if (lstrcmpiA(v, "null") == 0) return backend_kind::null;
    if (lstrcmpiA(v, "d3d11") == 0) return backend_kind::d3d11;
    *ok = false;
    return backend_kind::gdi;
}

} // namespace

bool owned_ddraw_active() { return g_active; }

bool owned_image_rect(RECT *out) { return out && image_rect_client(out); }

// PT-GFX8: the Alt-Tab / taskbar eligibility rule over any window (the UI harness's `winswitchable`), with the
// facts it decided on written to `facts` (visible/iconic/style/ex/owner/cloaked) for the log.
bool window_switchable(HWND h, char *facts, int n) {
    if (!h || !IsWindow(h)) return false;
    DWORD          cloaked = 0;
    static HMODULE dwm     = LoadLibraryA("dwmapi.dll");
    using attr_fn          = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static attr_fn get     = dwm ? (attr_fn)GetProcAddress(dwm, "DwmGetWindowAttribute") : nullptr;
    if (get && FAILED(get(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked)))) cloaked = 0;
    const bool  vis   = IsWindowVisible(h) != 0;
    const DWORD ex    = (DWORD)GetWindowLongA(h, GWL_EXSTYLE);
    const DWORD style = (DWORD)GetWindowLongA(h, GWL_STYLE);
    const HWND  owner = GetWindow(h, GW_OWNER);
    bool        ok    = vis && !cloaked;
    if (ok) ok = (ex & WS_EX_APPWINDOW) ? true : (!(ex & WS_EX_TOOLWINDOW) && !owner);
    if (facts && n > 0)
        wsprintfA(facts, "visible=%d iconic=%d style=%08lx ex=%08lx owner=%p cloaked=%lu", (int)vis, (int)IsIconic(h), (unsigned long)style,
                  (unsigned long)ex, (void *)owner, (unsigned long)cloaked);
    return ok;
}

bool owned_client_to_game(POINT *p) {
    RECT r;
    if (!p || !image_rect_client(&r)) return false;
    int x = p->x, y = p->y;
    map_client_to_game(r, x, y);
    p->x = x;
    p->y = y;
    return true;
}

bool owned_game_to_client(POINT *p) {
    RECT r;
    if (!p || !image_rect_client(&r)) return false;
    int x = p->x, y = p->y;
    map_game_to_client(r, x, y);
    p->x = x;
    p->y = y;
    return true;
}

// RES6: the cursor carry across the galaxy screen's mode switch. Screen coordinates, so the point survives a
// windowed client that the switch resizes; ensure_geometry first, as own_SetCursorPos does, because the switch has
// just changed the mode the rect is computed from.
bool owned_game_to_screen(POINT *p) {
    if (!p) return false;
    ensure_geometry();
    RECT r;
    if (!image_rect_client(&r)) return false;
    int x = p->x, y = p->y;
    map_game_to_client(r, x, y);
    POINT s = {x, y};
    if (!ClientToScreen(g_hwnd, &s)) return false;
    *p = s;
    return true;
}

bool owned_screen_to_game(POINT *p) {
    if (!p) return false;
    ensure_geometry();
    RECT r;
    if (!image_rect_client(&r)) return false;
    POINT c = *p;
    if (!ScreenToClient(g_hwnd, &c)) return false;
    int x = c.x, y = c.y;
    map_client_to_game(r, x, y);
    p->x = x;
    p->y = y;
    return true;
}

void owned_refresh_clip() { update_clip(); }

bool owned_user_sized() { return g_user_sized; }

SIZE_T owned_probe_frame_alloc(int w, int h, bool primary) {
    OwnSurface s;
    if (!s.create(w, h, primary ? DDSCAPS_PRIMARYSURFACE : 0)) return 0;
    return s.alloc; // ~OwnSurface frees the pages
}

void owned_reserve_frame(int w, int h) {
    if (w <= 0 || h <= 0) return;
    const SIZE_T b = (SIZE_T)w * 2 * (SIZE_T)h;
    if (b > g_reserve_bytes) g_reserve_bytes = b;
}

bool owned_mode_size(int *w, int *h) {
    if (!g_active || g_mode_w <= 0 || g_mode_h <= 0) return false;
    if (w) *w = g_mode_w;
    if (h) *h = g_mode_h;
    return true;
}

int owned_filter() { return (int)g_cfg.filter; }

const char *owned_filter_name(int filter) {
    static const char *const NAMES[] = {"point", "linear", "sharp", "area"};
    return filter >= 0 && filter < OWNED_FILTER_COUNT ? NAMES[filter] : "?";
}

void owned_set_filter(int filter) {
    if (!g_active || filter < 0 || filter >= OWNED_FILTER_COUNT || (int)g_cfg.filter == filter) return;
    g_cfg.filter = (scale_filter)filter;
    if (g_backend) g_backend->set_config(g_cfg);
    gfx_log("; [gfx] filter -> %s (set_config, live, backend %s)", owned_filter_name(filter),
            g_backend ? g_backend->name() : "none");
}

bool owned_integer_scale() { return g_cfg.integer_scale; }

void owned_set_integer_scale(bool on) {
    if (!g_active || g_cfg.integer_scale == on) return;
    g_cfg.integer_scale = on;
    if (g_backend) {
        g_backend->set_config(g_cfg);
        g_backend->on_window_changed(); // gdi repaints its bars for the new rect
    }
    gfx_log("; [gfx] scale -> %s (set_config, live, backend %s)", on ? "integer" : "fit", g_backend ? g_backend->name() : "none");
}

bool owned_vsync() { return g_cfg.vsync; }

void owned_set_vsync(bool on) {
    if (!g_active || g_cfg.vsync == on) return;
    g_cfg.vsync = on;
    if (g_backend) g_backend->set_config(g_cfg);
    gfx_log("; [gfx] vsync -> %d (set_config, live, backend %s)", on ? 1 : 0, g_backend ? g_backend->name() : "none");
}

// ---- RES4: the UI scale ----------------------------------------------------------------------------------

int owned_ui_scale() { return g_ui_scale_req; }

void owned_set_ui_scale(int pct) {
    if (!g_active) return;
    if (classic_sizes()) return; // RES25: classic sizes only -- no Interface size scaling, live or at boot
    if (pct < uilayout::MIN_PCT) pct = uilayout::MIN_PCT;
    if (pct > uilayout::MAX_PCT) pct = uilayout::MAX_PCT;
    if (g_ui_scale_req == pct) return;
    g_ui_scale_req = pct; // the next present applies it (strategic modes only) and logs the layout
    if (pct > uilayout::MIN_PCT) install_cursor_hook(), install_dialog_hook(), install_msgs_hook();
    gfx_log("; [ui] ui_scale -> %d%% (applied at the next strategic present)", pct);
}

// RES23-S0 `bcapture <name>`: the NEXT present writes capture_<name>__b.bmp from what the presenter really put on screen.
void owned_request_bcapture(const char *name) {
    lstrcpynA(g_bcap_name, name ? name : "bcapture", (int)sizeof(g_bcap_name));
    g_bcap_pending = true;
}

bool owned_present_layout(int frame_w, int frame_h, uilayout::layout *out) {
    if (!g_active || !out || frame_w <= 0 || frame_h <= 0) return false;
    int cw, ch;
    if (!ui_screen_size(cw, ch)) return false;
    const int pct = effective_ui_pct();
    view_layout_now(*out, cw, ch, frame_w, frame_h, pct);
    int        kw, kh;
    const bool cropped = crop_for_frame(frame_w, frame_h, kw, kh);
    if (pct > uilayout::MIN_PCT) log_ui_layout_change(frame_w, frame_h, pct, *out, kw); // a headless lane presents nothing: log here too
    if (cropped) log_crop_layout(frame_w, frame_h, kw, kh, *out);
    return true;
}

// RES18: the picker applied a mode whose frame is fw x fh and whose session size is lw x lh (lw <= fw, lh <= fh; equal = no crop).
void owned_set_crop(int fw, int fh, int lw, int lh) {
    if (!g_active) return;
    if (lw <= 0 || lh <= 0 || lw > fw || lh > fh || (lw == fw && lh == fh)) lw = lh = 0;
    if (g_hud.comp_fw == fw && g_hud.comp_fh == fh && g_hud.comp_w == lw && g_hud.comp_h == lh) return;
    g_hud.comp_fw = fw, g_hud.comp_fh = fh, g_hud.comp_w = lw, g_hud.comp_h = lh;
    g_comp_moved = true; // (the picker's mode apply IS the commit point of the composition; the epoch follows at the geometry recommit)
    if (lw > 0)
        gfx_log("; [ui] crop armed: session size %dx%d rendered as %dx%d, presented cropped (applies to the strategic frame)", lw, lh, fw, fh);
    else
        gfx_log("; [ui] crop off: the %dx%d frame is the picture", fw, fh);
    if (g_backend) g_backend->on_window_changed();
}

// RES4 stage 4: the quad layout in force NOW (several quads) for the mouse; false = the legacy single-rect mapping.
int  owned_info_layer_pass() { return g_info_pass; }
void owned_set_info_post(void (*cb)()) { g_info_post = cb; }
void owned_info_clip_stash(void *surface, int x, int y) {
    OwnSurface *s = static_cast<OwnSurface *>(surface);
    if (!s || !s->bits || s->w <= 0 || s->h <= 0) return;
    const size_t n = (size_t)s->w * s->h;
    if (!grow(g_clip.px, g_clip.cap, n)) return;
    for (int row = 0; row < s->h; ++row) memcpy(g_clip.px + (size_t)row * s->w, s->bits + (size_t)row * s->pitch, (size_t)s->w * 2);
    g_clip.x = x, g_clip.y = y, g_clip.w = s->w, g_clip.h = s->h;
    g_clip.valid = true;
}

bool owned_ui_layout(uilayout::layout *out) { return g_active && out && multi_layout(*out); }

// RES23-S5: the overlay list (notice, layered dialogs, cursor) over the presented image of a frame_w x frame_h frame (pcapture / bcapture's CPU twin), in
// static buffers: the same collect the presenter runs, against the frame the framebuffer holds now. sprite_bbox (optional) gets the cursor sprite's box.
bool owned_overlay_list(int frame_w, int frame_h, overlay_list *out, int *sprite_bbox) {
    if (!g_active || !out) return false;
    overlay_want want;
    want.cursor = want.dialogs = want.notices = true;
    overlay_collect(*out, frame_w, frame_h, want, sprite_bbox);
    return true;
}

// RES4 stage 3: how many frame rows of the side panel the screen shows (Hp = ceil(composition height / s)), or 0 when ui_scale is
// not in force. The six panel row-count sites take min(WindowHeight, cap, Hp), so no list draws a row nobody sees.
// WHOLE rows only: floor(composition height / s), not the quad's ceil -- a list row that ends past the visible bottom would be cut by the
// screen, so the lists size themselves to the rows that fit whole above it (the strip corner included: the panel quad runs to the
// composition's bottom).
namespace {
// The whole panel rows the screen shows NOW (0 = ui_scale not in force), built from the current frame, screen and percentage in force. Only the
// commit (and the pre-first-commit fallback below) calls it; the P6 thunks read the committed value.
int live_panel_rows() {
    uilayout::layout L;
    return g_active && multi_layout(L) ? uilayout::whole_panel_rows(L) : 0;
}
} // namespace

// RES23-S2: the one commit point. `take_pending`: take the asked Interface size (the strategic pre-input of a mode 2 / 5 frame); false = recompute the
// frame-dependent part for a new frame size at the percentage already committed (the picker applied a mode). Returns true when the percentage or
// the whole panel rows changed since the previous commit, i.e. every panel mode and the bottom strip are owed a redraw (never on the first commit).
bool owned_hud_commit(bool take_pending) {
    if (!g_active) return false;
    if (!take_pending && !g_hud.epoch) return false; // nothing committed yet: the thunks read the live value
    const hud_geometry old   = g_hud;
    const bool         first = old.epoch == 0;
    if (take_pending) g_hud.pct = g_ui_scale_req <= uilayout::MIN_PCT ? uilayout::MIN_PCT : g_ui_scale_req;
    if (first) g_hud.epoch = 1; // effective_ui_pct reads g_hud.pct from here on
    g_hud.hp         = live_panel_rows();
    g_hud.frame_w    = g_mode_w;
    g_hud.frame_h    = g_mode_h;
    const int comp_h = crop_matches_mode() ? g_hud.comp_h : g_hud.frame_h; // the rows of the frame the picture occupies
    g_hud.vis_h      = g_hud.hp > 0 && g_hud.hp < comp_h ? g_hud.hp : comp_h;
    g_hud.h_built    = g_built_h;
    g_hud.list_cap   = g_built_h > 768 ? g_built_h : 768;
    // Owed: the percentage, Hp or the frame rows the screen shows moved (the lists size themselves to them). A frame size change alone is not.
    const bool owed    = !first && (g_hud.pct != old.pct || g_hud.hp != old.hp || g_hud.vis_h != old.vis_h);
    const bool changed = first || owed || g_comp_moved || g_hud.frame_w != old.frame_w || g_hud.frame_h != old.frame_h || g_hud.list_cap != old.list_cap;
    g_comp_moved       = false;
    if (changed && !first) ++g_hud.epoch;
    if (first || g_hud.hp != old.hp)
        gfx_log("; [ui] side-panel list rows follow the visible panel: Hp %d rows (WindowHeight %d)", g_hud.hp > 0 ? g_hud.hp : 0, *(const int *)0x005202ccu);
    if (changed)
        gfx_log("; [ui] hud geometry epoch %u: pct %d, whole panel rows Hp %d, frame %dx%d%s; visible rows %d, list cap %d", g_hud.epoch, g_hud.pct, g_hud.hp, g_hud.frame_w,
                g_hud.frame_h, owed ? " (changed: every panel mode and the strip are redrawn)" : "", g_hud.vis_h, g_hud.list_cap);
    return owed;
}
// RES26: the picker (or the Options Ok) applied a mode while a dialog mode (3, 4) is up over a strategic frame. The dialog modes present THAT frame at the
// stamp's size and percentage, so a plain recommit read the OLD frame's stamp (pct 100, one quad, Hp 0) until the strategic pump ran on resume. Here the stamp
// moves to the new frame and takes the asked Interface size (the commit that follows reads it through effective_ui_pct_for), so the HUD shows the requested
// size right after Apply. Fields only: nothing is drawn or presented (the surfaces are released inside the mode change). Not for the galaxy switch (the caller
// gates on it). Returns the commit's `owed`.
bool owned_hud_commit_applied() {
    if (!g_active) return false;
    const int m = at<int>(mh::addr::_G_LLM_GAME_MODE);
    if ((m != 3 && m != 4) || g_stamp.w <= 0 || !g_hud.epoch) return owned_hud_commit(false);
    g_stamp.w       = g_mode_w;
    g_stamp.h       = g_mode_h;
    g_stamp.pct     = g_ui_scale_req <= uilayout::MIN_PCT ? uilayout::MIN_PCT : g_ui_scale_req;
    const bool owed = owned_hud_commit(true);
    g_stamp.epoch   = g_hud.epoch;
    gfx_log("; [ui] RES26: mode %d applied over the strategic frame: the HUD stamp moves to %dx%d at %d%% now (not at resume)", m, g_stamp.w, g_stamp.h, g_stamp.pct);
    return owed;
}
void owned_hud_set_built(int h_built) {
    g_built_h = h_built;
    if (g_hud.epoch) owned_hud_commit(false);
}
const hud_geometry &owned_hud_geometry() { return g_hud; }
// The row-count cap of the scrollable lists: the committed geometry's, and before the first commit what the tables were grown for (the first panel draw runs
// before the first commit; reading 768 there left the first frame's lists 768 px tall for good, as nothing owes the first commit a redraw).
int owned_hud_list_cap() { return g_hud.epoch ? g_hud.list_cap : (g_built_h > 768 ? g_built_h : 768); }

int owned_ui_panel_rows() { return g_hud.epoch ? g_hud.hp : live_panel_rows(); }

// True when a file named ddraw.dll (any case) sits in the directory of the running exe.
// The Windows display scale (percent) of the primary monitor, or 0 when it cannot be read. Effective DPI (what the process is laid out at).
static int monitor_display_scale_pct(int *dpi_out) {
    typedef HRESULT(WINAPI * PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
    HMODULE sh = LoadLibraryA("shcore.dll");
    if (!sh) return 0;
    PFN_GetDpiForMonitor f   = (PFN_GetDpiForMonitor)GetProcAddress(sh, "GetDpiForMonitor");
    int                  pct = 0;
    if (f) {
        UINT        dx = 0, dy = 0;
        const POINT o = {0, 0};
        if (SUCCEEDED(f(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx >= 96) {
            pct = (int)((dx * 100 + 48) / 96);
            if (dpi_out) *dpi_out = (int)dx;
        }
    }
    FreeLibrary(sh);
    return pct;
}

int owned_resolve_ui_scale(const char *ini) {
    if (classic_sizes(ini)) { // RES25: "Classic sizes only" disables Interface size scaling -- the ini value is kept, not acted on
        static bool logged = false;
        if (!logged) {
            logged = true;
            char u[16];
            mh::config::read_ini_string("video", "ui_scale", "100", u, sizeof(u), ini);
            if (u[0] && lstrcmpA(u, "100") != 0) gfx_log("; [ui] ui_scale ignored: classic sizes only (ini says %s, in force 100%%)", u);
        }
        return uilayout::MIN_PCT;
    }
    static int  cached = 0;
    static char cached_for[64];
    char        u[16], ov[16];
    mh::config::read_ini_string("video", "ui_scale", "100", u, sizeof(u), ini);
    mh::config::read_ini_string("video", "display_scale", "", ov, sizeof(ov), ini);
    char key[64];
    wsprintfA(key, "%s|%s", u, ov);
    if (cached && lstrcmpA(key, cached_for) == 0) return cached;
    int r = uilayout::MIN_PCT;
    if (lstrcmpiA(u, "auto") == 0) {
        int       dpi = 96;
        const int o   = atoi(ov);
        int       ds  = o > 0 ? o : monitor_display_scale_pct(&dpi);
        if (ds <= 0) ds = 100;
        r = uilayout::auto_scale_pct(ds);
        gfx_log("; [ui] ui_scale=auto: display scale %d%% (%s) -> %d%%", ds, o > 0 ? "[video] display_scale override" : "primary monitor effective DPI", r);
    } else {
        const int p = atoi(u);
        if (p >= uilayout::MIN_PCT && p <= uilayout::MAX_PCT)
            r = p;
        else if (u[0])
            gfx_log("; [ui] ui_scale=%s not recognised (100..400 percent | auto) -- using 100", u);
    }
    cached = r;
    lstrcpynA(cached_for, key, sizeof(cached_for));
    return r;
}

// The choices of the in-game Interface size row, in the launcher's order: Auto, then the percentages ([video] ui_scale).
static const int UI_SCALE_CHOICES[OWNED_UI_SCALE_CHOICES] = {0, 100, 125, 150, 175, 200};

int owned_ui_scale_choice() {
    if (g_ui_scale_auto) return 0;
    int best = 1; // the nearest listed percentage (a hand-written 130 shows as 125)
    for (int i = 1; i < OWNED_UI_SCALE_CHOICES; ++i)
        if (abs(UI_SCALE_CHOICES[i] - g_ui_scale_req) < abs(UI_SCALE_CHOICES[best] - g_ui_scale_req)) best = i;
    return best;
}

void owned_set_ui_scale_choice(int ix, const char *ini) {
    if (ix < 0 || ix >= OWNED_UI_SCALE_CHOICES) return;
    int pct = UI_SCALE_CHOICES[ix];
    if (ix == 0) { // Auto = the Windows display scale of the primary monitor ([video] display_scale overrides it, as at boot)
        char ov[16];
        mh::config::read_ini_string("video", "display_scale", "", ov, sizeof(ov), ini);
        const int o  = atoi(ov);
        int       ds = o > 0 ? o : monitor_display_scale_pct(nullptr);
        if (ds <= 0) ds = 100;
        pct = uilayout::auto_scale_pct(ds);
        gfx_log("; [ui] ui_scale=auto (options menu): display scale %d%% -> %d%%", ds, pct);
    }
    g_ui_scale_auto = ix == 0;
    owned_set_ui_scale(pct);
}

static bool system_ddraw_wrapper_present() {
    char  path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return true; // cannot tell: say nothing
    char *slash = path + n;
    while (slash > path && slash[-1] != '\\' && slash[-1] != '/') --slash;
    lstrcpynA(slash, "ddraw.dll", (int)(MAX_PATH - (slash - path)));
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; // NTFS lookup is case-insensitive
}

// RES4 stage 2: [video] ui_scale = 100|125|150|175|200 (percent) | auto, and the dev key ui_screen=WxH. Drives the
// presenter's quad list only; the game still renders its own size and takes input as before until stages 3-4.
static void read_ui_scale_keys(const char *ini) {
    g_ui_scale_req = owned_resolve_ui_scale(ini);
    {
        char ck[8];
        mh::config::read_ini_string("video", "ui_scale_dialog_check", "0", ck, sizeof(ck), ini); // dev: diff the dialog layer against the stock draw
        g_dlg_check = ck[0] == '1';
        mh::config::read_ini_string("video", "ui_scale_notices", "1", ck, sizeof(ck), ini); // dev kill switch (RES20): 0 = the game's own notices, drawn into the frame at 1x
        g_msg_enabled = ck[0] != '0';
    }
    char ui_raw[16];
    mh::config::read_ini_string("video", "ui_scale", "100", ui_raw, sizeof(ui_raw), ini);
    g_ui_scale_auto = lstrcmpiA(ui_raw, "auto") == 0 && !classic_sizes(ini);
    char sc[24];
    mh::config::read_ini_string("video", "ui_screen", "", sc, sizeof(sc), ini); // dev: WxH for pcapture on a headless lane
    int sw = 0, sh = 0;
    if (sc[0] && sscanf(sc, "%dx%d", &sw, &sh) == 2 && sw >= 320 && sh >= 240 && sw <= 8192 && sh <= 8192) {
        g_ui_screen_w = sw;
        g_ui_screen_h = sh;
    }
    mh::config::read_ini_string("video", "borderless_size", "", sc, sizeof(sc), ini); // dev: the borderless window's size on a rig whose monitor is smaller
    if (sc[0] && sscanf(sc, "%dx%d", &sw, &sh) == 2 && sw >= 320 && sh >= 240 && sw <= 8192 && sh <= 8192) {
        g_bl_w = sw;
        g_bl_h = sh;
    }
}

bool install_owned_ddraw(const char *ini) {
    static bool done = false;
    if (done) return g_active;
    done = true;
    (void)classic_sizes(ini); // RES25: read once, before anything asks for the ui_scale

    char v[32];
    mh::config::read_ini_string("video", "backend", "d3d11", v, sizeof(v), ini);
    if (lstrcmpiA(v, "system") == 0 || v[0] == 0) {
        // Informational only: Windows' own DirectDraw is used either way (the default: not one byte touched).
        if (!system_ddraw_wrapper_present())
            gfx_log("; [video] backend=system with no DirectDraw wrapper beside mh.exe -- using Windows' own DirectDraw");
        return false;
    }
    bool ok = false;
    g_kind  = parse_kind(v, &ok);
    if (!ok) {
        gfx_log("; [gfx] [video] backend=%s not recognised (system | own | gdi | null | d3d11) -- staying on system", v);
        return false;
    }
    if (!mh::en_build_ok()) return false;

    char s[32];
    mh::config::read_ini_string("video", "window", "borderless", s, sizeof(s), ini);
    g_cfg.window = lstrcmpiA(s, "borderless") == 0 ? window_mode::borderless : window_mode::windowed;
    mh::config::read_ini_string("video", "filter", "area", s, sizeof(s), ini);
    g_cfg.filter      = scale_filter::point;
    bool filter_known = lstrcmpiA(s, "point") == 0;
    for (int i = 1; i < OWNED_FILTER_COUNT; ++i)
        if (lstrcmpiA(s, owned_filter_name(i)) == 0) {
            g_cfg.filter = (scale_filter)i;
            filter_known = true;
        }
    if (!filter_known) gfx_log("; [gfx] [video] filter=%s not recognised (point | linear | sharp | area) -- using point", s);
    mh::config::read_ini_string("video", "scale", "fit", s, sizeof(s), ini);
    g_cfg.integer_scale = lstrcmpiA(s, "integer") == 0;
    g_cfg.vsync         = mh_ini_get_int("video", "vsync", 1, ini) != 0;
    read_ui_scale_keys(ini);
    g_fps_limit     = (int)mh_ini_get_int("video", "fps_limit", 0, ini);
    g_no_window     = mh_ini_get_int("video", "no_window", 0, ini) != 0;
    g_mouse_clip    = mh_ini_get_int("video", "mouse_clip", 1, ini) != 0;
    g_taskbar_guard = mh_ini_get_int("video", "taskbar_guard", 1, ini) != 0; // PT-GFX8
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&install_owned_ddraw, &g_self);

    // Exact-bytes guard on BOTH entries before writing EITHER: a half-owned loader (ours acquire, the
    // original release) would FreeLibrary mh.dll's handle.
    constexpr uintptr_t ACQ = mh::exp::addr_llm_gfx_ddraw_dll_acquire;
    constexpr uintptr_t REL = mh::exp::addr_llm_gfx_ddraw_dll_release;
    if (*(const uint64_t *)ACQ != mh::exp::entry_llm_gfx_ddraw_dll_acquire ||
        *(const uint64_t *)REL != mh::exp::entry_llm_gfx_ddraw_dll_release) {
        gfx_log("; [gfx] backend=%s NOT armed -- the ddraw loader entry bytes differ (wrong build, or hooked)", v);
        return false;
    }
    if (!mh::hook::install_jmp(ACQ, (const void *)&acquire_thunk, mh::hook::entry_claim::exclusive,
                               "the PT-GFX1 owned DirectDraw (loader acquire)", 0)) {
        gfx_log("; [gfx] backend=%s NOT armed -- acquire entry refused (see the [interlock] line)", v);
        return false;
    }
    if (!mh::hook::install_jmp(REL, (const void *)&release_thunk, mh::hook::entry_claim::exclusive,
                               "the PT-GFX1 owned DirectDraw (loader release)", 0)) {
        // Acquire is already ours. Its module handle is mh.dll's, so the ORIGINAL release must never
        // reach FreeLibrary: pin the refcount high enough that it cannot hit zero.
        at<int>(ADDR_DDRAW_REFCOUNT) += 0x10000000;
        gfx_log("; [gfx] backend=%s: release entry refused -- refcount pinned so the original never frees", v);
    }
    install_headless_window_hooks();
    install_owned_window_hooks();
    g_active = true;
    if (g_ui_scale_req > uilayout::MIN_PCT) install_cursor_hook(), install_dialog_hook(), install_msgs_hook(); // RES4 stage 5: the owned cursor; 6b: the dialog layers; RES20: the notices
    if (g_kind == backend_kind::d3d11) imgui_overlay::arm(ini);                                                // [video] imgui=1 -- inert otherwise
    gfx_log("; [gfx] backend=%s ARMED: owned DirectDraw v1 (loader replaced at %08x/%08x), fps_limit=%d, window=%s, "
            "filter=%s, scale=%s%s",
            v, (unsigned)ACQ, (unsigned)REL, g_fps_limit, g_cfg.window == window_mode::borderless ? "borderless" : "windowed",
            owned_filter_name((int)g_cfg.filter), g_cfg.integer_scale ? "integer" : "fit",
            g_no_window ? " (no_window: geometry left to the keeper)" : "");
    return true;
}

} // namespace mh::gfx
