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
#include <ddraw.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <string.h>

#include "gfx/ddraw_own.h"
#include "gfx/present_backend.h"
#include "gfx/overlay_imgui.h"      // PT-GFX4: arm the ImGui overlay with backend=d3d11
#include "addr/mh_export.gen.h"     // mh::exp::entry_llm_gfx_ddraw_dll_{acquire,release} -- the arm guards
#include "include/mh_run_context.h" // mh_proc_path
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "config/ini_read.h"        // read_ini_string -- strips a trailing `;comment`
#include "hook/detour.h"            // install_jmp
#include "en_guard.h"               // EN-only build gate

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
int            g_fps_limit = 60;    // [video] fps_limit; 0 = unlimited
bool           g_no_window = false; // [video] no_window: the headless keeper owns the window, hands off
HMODULE        g_self      = nullptr;

// ---- the display "mode" and the one window -------------------------------------------------------

int  g_mode_w        = 0; // 0 until SetDisplayMode: GetDisplayMode then reports the desktop size
int  g_mode_h        = 0;
HWND g_hwnd          = nullptr;
bool g_geom_placed   = false; // windowed: the first placement centres, later ones keep the user's spot
int  g_geom_tries    = 0;     // consecutive placements that did not reach the mode's client size
int  g_geom_logged_w = 0, g_geom_logged_h = 0;

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
//               alt-tab. The game's MoveWindow passes through here, so the DirectInput mouse's
//               cooperative level (llm_wnd_is_fullscreen, read right after it) is decided on the
//               mode-sized window exactly as before.
//
// Skipped entirely for the null backend and under [video] no_window, whose keeper owns the window, and
// while the player is inside a size/move modal loop (g_sizing).
constexpr LONG WINDOWED_STYLE = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
constexpr int  MIN_CLIENT_W = 320, MIN_CLIENT_H = 240;

bool g_move_hooked = false; // own_MoveWindow is in the exe's import table

bool geometry_owned() { return g_active && !g_no_window && g_kind != backend_kind::null; }

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
        } else if (!(style == want && !(ex & WS_EX_TOPMOST) && cr.right == g_mode_w && cr.bottom == g_mode_h)) {
            SetWindowLongA(g_hwnd, GWL_STYLE, want);
            SetWindowLongA(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TOPMOST);
            RECT r = {0, 0, g_mode_w, g_mode_h};
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
                if (cr.right != g_mode_w || cr.bottom != g_mode_h)
                    SetWindowPos(g_hwnd, nullptr, 0, 0, ww + (g_mode_w - cr.right), wh + (g_mode_h - cr.bottom),
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                GetClientRect(g_hwnd, &cr);
                ++g_geom_tries;
                if (cr.right == g_mode_w && cr.bottom == g_mode_h) {
                    g_geom_tries = 0;
                    if (!g_geom_placed || g_geom_logged_w != g_mode_w || g_geom_logged_h != g_mode_h)
                        gfx_log("; [gfx] window: windowed (resizable), client %dx%d at %d,%d", (int)cr.right, (int)cr.bottom, x, y);
                    g_geom_logged_w = g_mode_w;
                    g_geom_logged_h = g_mode_h;
                } else if (g_geom_tries >= 3) {
                    gfx_log("; [gfx] window: client stuck at %dx%d for a %dx%d mode after 3 tries -- presenting scaled",
                            (int)cr.right, (int)cr.bottom, g_mode_w, g_mode_h);
                }
            }
            g_geom_placed = true;
        } else {
            g_geom_placed = true;
        }
    } else {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int  mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
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

// The image's rect in CLIENT pixels, or false when there is nothing to map through.
bool image_rect_client(RECT *out) {
    if (!geometry_owned() || !g_hwnd || g_mode_w <= 0 || g_mode_h <= 0) return false;
    RECT cr;
    if (!GetClientRect(g_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
    *out = fit_image_rect(cr.right, cr.bottom, g_mode_w, g_mode_h, g_cfg.integer_scale);
    return out->right > out->left && out->bottom > out->top;
}

void map_client_to_game(const RECT &r, int &x, int &y) {
    const int rw = r.right - r.left, rh = r.bottom - r.top;
    const int cx = x < r.left ? r.left : x >= r.right ? r.right - 1
                                                      : x;
    const int cy = y < r.top ? r.top : y >= r.bottom ? r.bottom - 1
                                                     : y;
    x            = (int)((long long)(cx - r.left) * g_mode_w / rw);
    y            = (int)((long long)(cy - r.top) * g_mode_h / rh);
}

// The CENTRE of game pixel (x,y) in client pixels, so client_to_game(game_to_client(p)) == p.
void map_game_to_client(const RECT &r, int &x, int &y) {
    const int rw = r.right - r.left, rh = r.bottom - r.top;
    const int gx = x < 0 ? 0 : x >= g_mode_w ? g_mode_w - 1
                                             : x;
    const int gy = y < 0 ? 0 : y >= g_mode_h ? g_mode_h - 1
                                             : y;
    x            = r.left + (int)(((long long)gx * 2 + 1) * rw / (2LL * g_mode_w));
    y            = r.top + (int)(((long long)gy * 2 + 1) * rh / (2LL * g_mode_h));
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
    g_user_sized   = IsZoomed(h) || cr.right != g_mode_w || cr.bottom != g_mode_h;
    if (was != g_user_sized || g_user_sized) {
        static int logged = 0;
        if (logged < 16) {
            ++logged;
            gfx_log("; [gfx] window: %s client %ldx%ld (mode %dx%d)%s", g_user_sized ? "player-sized" : "back to native",
                    cr.right, cr.bottom, g_mode_w, g_mode_h, IsZoomed(h) ? " maximised" : "");
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
// and the player's position survives. Borderless: passed through (see the geometry note).
BOOL WINAPI own_MoveWindow(HWND h, int x, int y, int w, int hgt, BOOL repaint) {
    if (geometry_owned() && g_cfg.window == window_mode::windowed && (h == g_hwnd || h == at<HWND>(ADDR_HWND_MAIN))) {
        if (++g_swallowed_moves <= 4) gfx_log("; [gfx] window: game MoveWindow(%d,%d,%d,%d) swallowed (windowed geometry is ours)", x, y, w, hgt);
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
            g_real_move ? (g_cfg.window == window_mode::windowed ? "swallowed for the main window" : "passed through")
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
        w     = width;
        h     = height;
        pitch = width * 2;
        caps  = c;
        // Slack past the end: the software renderer has measured history of reading a few rows past a
        // surface (video.cpp's hittest_begin note). dgVoodoo's allocations tolerated that; an exact-size
        // VirtualAlloc would turn a benign over-read into an access violation at a page boundary.
        const SIZE_T body  = (SIZE_T)pitch * (SIZE_T)height;
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

void present_surface(OwnSurface *s) {
    ++g_presents;
    if (!g_backend) bind_backend(g_hwnd ? g_hwnd : at<HWND>(ADDR_HWND_MAIN));
    log_modules_once();
    ensure_geometry();
    update_clip();
    g_limiter.pace(g_fps_limit);
    if (!g_backend) return;
    frame_view f;
    f.pixels = s->bits;
    f.width  = s->w;
    f.height = s->h;
    f.pitch  = s->pitch;
    f.format = pixel_format::rgb565;
    g_backend->present(f); // false = a dropped frame (minimised, device lost), never an error
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

bool owned_user_sized() { return g_user_sized; }

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

bool install_owned_ddraw(const char *ini) {
    static bool done = false;
    if (done) return g_active;
    done = true;

    char v[32];
    mh::config::read_ini_string("video", "backend", "system", v, sizeof(v), ini);
    if (lstrcmpiA(v, "system") == 0 || v[0] == 0) return false; // the default: not one byte touched
    bool ok = false;
    g_kind  = parse_kind(v, &ok);
    if (!ok) {
        gfx_log("; [gfx] [video] backend=%s not recognised (system | own | gdi | null | d3d11) -- staying on system", v);
        return false;
    }
    if (!mh::en_build_ok()) return false;

    char s[32];
    mh::config::read_ini_string("video", "window", "windowed", s, sizeof(s), ini);
    g_cfg.window = lstrcmpiA(s, "borderless") == 0 ? window_mode::borderless : window_mode::windowed;
    mh::config::read_ini_string("video", "filter", "point", s, sizeof(s), ini);
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
    g_cfg.vsync         = GetPrivateProfileIntA("video", "vsync", 0, ini) != 0;
    g_fps_limit         = (int)GetPrivateProfileIntA("video", "fps_limit", 60, ini);
    g_no_window         = GetPrivateProfileIntA("video", "no_window", 0, ini) != 0;
    g_mouse_clip        = GetPrivateProfileIntA("video", "mouse_clip", 1, ini) != 0;
    g_taskbar_guard     = GetPrivateProfileIntA("video", "taskbar_guard", 1, ini) != 0; // PT-GFX8
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
    if (g_kind == backend_kind::d3d11) imgui_overlay::arm(ini); // [video] imgui=1 -- inert otherwise
    gfx_log("; [gfx] backend=%s ARMED: owned DirectDraw v1 (loader replaced at %08x/%08x), fps_limit=%d, window=%s, "
            "filter=%s, scale=%s%s",
            v, (unsigned)ACQ, (unsigned)REL, g_fps_limit, g_cfg.window == window_mode::borderless ? "borderless" : "windowed",
            owned_filter_name((int)g_cfg.filter), g_cfg.integer_scale ? "integer" : "fit",
            g_no_window ? " (no_window: geometry left to the keeper)" : "");
    return true;
}

} // namespace mh::gfx
