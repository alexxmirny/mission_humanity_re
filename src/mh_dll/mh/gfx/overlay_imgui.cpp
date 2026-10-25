//
// overlay_imgui.cpp -- PT-GFX4: Dear ImGui (vendored, src/mh_dll/include/imgui) over the d3d11
// presenter. See overlay_imgui.h for the arming contract.
//
// ---- RENDERING -----------------------------------------------------------------------------------
//
// backend_d3d11.cpp calls on_frame() between the game-frame Draw(3, 0) and Present(), render target
// bound. imgui_impl_dx11 sets its own viewport to the whole back buffer, so the overlay is drawn at
// OUTPUT resolution over the scaled game image and its letterbox bars alike -- crisp at any window
// size, not upscaled with the game. imgui_impl_dx11 backs up and restores every piece of pipeline
// state it touches, so the presenter's next frame starts from what it set. Its two shaders come
// precompiled from gfx/imgui_d3dcompile.cpp (no d3dcompiler import).
//
// Device lifetime: ImGui_ImplDX11_Init AddRef's the device and context. backend_d3d11 calls
// on_device_release() first thing in release_device_resources(), so a device-lost rebuild or a detach
// never leaves ImGui holding the old device alive; on_frame() notices a new device pointer and
// re-initialises.
//
// ---- INPUT ---------------------------------------------------------------------------------------
//
// The game reads input in TWO ways, and both have to be stopped for "a click over the overlay never
// reaches the game" to be true (measured with ReVA on /eng/mh.exe, 2026-09-28):
//
//   1. WINDOW MESSAGES. llm_wnd_proc (0x004a0524, the WNDCLASS proc and its only reference) calls
//      llm_input_wndproc_tap (0x004d1194) on EVERY message before its own dispatch. When the DI
//      device pointers are null the tap turns WM_MOUSEMOVE / WM_[LR]BUTTON* / WM_MOUSEWHEEL and
//      WM_[SYS]KEY* into ring events itself ([input] mouse_absolute=1 forces that for the mouse).
//   2. DIRECTINPUT. By default the tap instead polls _G_LLM_DI_MOUSE_DEVICE (llm_input_di_mouse_poll
//      0x004d0cc4) and _G_LLM_DI_KEYBOARD_DEVICE (llm_input_di_keyboard_poll 0x004d0a48) -- buffered
//      GetDeviceData (vtable +0x28, cbObjectData 0x10), each called ONLY from the tap. Swallowing a
//      WM_LBUTTONDOWN does nothing against this path: the click is already sitting in DirectInput's
//      buffer, and the next message of any kind (a WM_PAINT) delivers it through the poll. The
//      cooperative level is DISCL_NONEXCLUSIVE|FOREGROUND for the keyboard always, and for the mouse
//      whenever the window is not exactly screen-sized (llm_wnd_is_fullscreen -> 6, else 5 =
//      EXCLUSIVE|FOREGROUND) -- so in the owned backend's windowed mode Windows still sends mouse
//      messages, which is what ImGui is fed from.
//
// DEFAULT PATH SINCE PT-GFX6 (DI mouse present): the mouse is not taken from messages at all. A filter
// on the mouse device's GetDeviceData withholds only the button/wheel records ImGui owns and feeds
// ImGui the same records, with its position taken from the GAME cursor -- see "THE CURSOR" below. The
// message rules that follow still apply to the keyboard always, and to the mouse on the fallback path
// (no DI mouse: [input] mouse_absolute=1, or the filter could not be installed).
//
// So the subclassed WndProc, while the overlay is OPEN:
//   * feeds ImGui_ImplWin32_WndProcHandler first, then
//   * SWALLOWS a mouse message while io.WantCaptureMouse OR when the message's own point lies on a panel
//     drawn last frame (PANEL RECTS below: the flag lags a frame), with every button UP following its
//     DOWN; and a key-down / char message while io.WantCaptureKeyboard (key-UPs always pass, so the
//     game's raw-path keystate cannot stick), and
//   * while capture is wanted, DRAINS the DirectInput buffer(s) before anything is forwarded to the
//     game -- the tap's poll then finds nothing. A drained DI key-UP is mirrored into
//     _G_LLM_INPUT_KEYSTATE (the same `&= 0xfe / 0xfd` the game's poll applies) so a key held when
//     capture began is released, not stuck; drained key-downs and all mouse events are just dropped.
//   * on_frame() drains once more when capture ENDS, because the consumer runs inside WM_PAINT: an
//     event buffered after the last forwarded message would otherwise reach the game on the first
//     message after the flag clears.
// While CLOSED the subclass calls straight through -- not one message or DI event is touched (it only
// watches for the toggle key's WM_KEYDOWN, which it forwards like any other message).
//
// ImGui's WantCaptureMouse stays false while a button that went down OUTSIDE ImGui is held, so a drag
// the game started over the map is never cut in half by the overlay. ImGui is told
// NoMouseCursorChange: the game owns the cursor. When the OS cursor is hidden (the game's own software
// cursor is showing) ImGui draws its own while the pointer is over an ImGui window.
//
// The subclass follows the presenter's window (WINDOW BINDING below): a different HWND unlinks the old
// one and links the new one, the backend's detach() unlinks, and WM_NCDESTROY unlinks a dying window.
//
// Coordinates: ImGui lives in CLIENT pixels, which is exactly the swap chain's back buffer
// (backend_d3d11 resizes it to the client rect every present), so the Win32 backend's client
// coordinates map 1:1 onto the letterboxed/scaled output with no transform.
//
// ---- DETERMINISM ---------------------------------------------------------------------------------
//
// Nothing here reads RNG, calls sim code, or writes sim state. The panel reads the debug overlay's
// provider registry (plain global reads). Consuming input is a LOCAL decision, like a player not
// clicking: anything that does reach the sim still goes through the normal order path.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <d3d11.h>

#include "imgui.h"
#include "imgui_internal.h" // FindWindowByName / SetWindowPos(ImGuiWindow*): the panel is clamped BEFORE its Begin, so a title-bar drag never shows an out-of-image frame
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "gfx/overlay_imgui.h"
#include "gfx/heat_xf.h"
#include "gfx/present_backend.h"       // fit_image_rect -- the presenter's image rect
#include "gfx/ddraw_own.h"             // owned_filter / owned_set_filter / owned_vsync: the live options
#include "input/dinput_own.h"          // PT-INPUT1: the owned DirectInput's mouse filter point
#include "addr/mh_addrs.gen.h"         // _G_LLM_DI_MOUSE_DEVICE, _G_LLM_INPUT_KEYSTATE
#include "config/ini_read.h"           // read_ini_string -- strips a trailing `;comment`
#include "include/mh_run_context.h"    // mh_proc_path
#include "include/mh_log_sink.h"       // LOG1: async log sink
#include "include/mh_overlay_export.h" // MH_Overlay_Provider* -- the debug overlay's readouts
#include "include/mh_uidrive_export.h" // MH_UIDrive_SynthKeyDown -- the harness `hotkey` action
#include "include/mh_mapzoom_export.h" // MH_MapZoom_Get -- the zoom crop (mods:M1WL v3 heat map)
#include "addr/mh_structs.gen.h"       // mh_llm_strat_map_geom (the camera wrap masks)
#include "state/region_runtime.h"      // mh::state::ptr / RID_GENERAL / RID_MAP_CAM_COL / ROW
#include "seams/heat_ui.h"             // mods:M1WL v3: the Stream heat section's reads and the live setter

#pragma comment(lib, "user32.lib")

// Declared by imgui_impl_win32.h only inside `#if 0` (so including it cannot drag windows.h into
// every ImGui user); the backend's own comment says to forward-declare it like this.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mh::gfx::imgui_overlay {
namespace {

// ---- game addresses -----------------------------------------------------------------------------
constexpr uintptr_t ADDR_DI_MOUSE    = mh::addr::_G_LLM_DI_MOUSE_DEVICE; // IDirectInputDevice* (mouse)
constexpr uintptr_t ADDR_DI_KEYBOARD = 0x0066155cu;                      // _G_LLM_DI_KEYBOARD_DEVICE -- literal for the
                                                                         // same fixture-fingerprint reason as ui_drive.cpp
constexpr uintptr_t ADDR_KEYSTATE = mh::addr::_G_LLM_INPUT_KEYSTATE;     // byte[128], bit0/bit1 = held
// The game's cursor (see "THE CURSOR" below). CURSOR_X/Y: the drawn sprite's hotspot, game pixels.
// LAST_X/Y: the DirectInput accumulator llm_input_di_mouse_poll integrates each count into -- delta =
// dwData / DIVISOR (signed IDIV), doubled when |delta| > THRESHOLD, clamped to [0, W) x [0, H) with W/H
// at 0xe69da4/0xe69da8 (literals: _DAT_ in the DB, same fixture-fingerprint reason as the keyboard slot).
constexpr uintptr_t ADDR_CURSOR_X   = mh::addr::_G_LLM_CURSOR_X;
constexpr uintptr_t ADDR_CURSOR_Y   = mh::addr::_G_LLM_CURSOR_Y;
constexpr uintptr_t ADDR_DI_LAST_X  = mh::addr::_G_LLM_INPUT_MOUSE_LAST_X;
constexpr uintptr_t ADDR_DI_LAST_Y  = mh::addr::_G_LLM_INPUT_MOUSE_LAST_Y;
constexpr uintptr_t ADDR_DI_DIV     = mh::addr::_G_LLM_INPUT_DI_MOUSE_DIVISOR;
constexpr uintptr_t ADDR_DI_THRESH  = mh::addr::_G_LLM_INPUT_DI_MOUSE_ACCEL_THRESHOLD;
constexpr uintptr_t ADDR_DI_BOUND_W = 0x00e69da4u;
constexpr uintptr_t ADDR_DI_BOUND_H = 0x00e69da8u;

template <class T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void *)a, sizeof(v)); // CURSOR_X/Y are unaligned
    return v;
}

// ---- log (mh_video.log, like the rest of the presenter) ----------------------------------------
char          g_log_path[MAX_PATH];
unsigned long g_log_gen = 0;

void log(const char *fmt, ...) {
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

// ---- config + state -----------------------------------------------------------------------------
struct key_bind {
    int  vk    = VK_F11;
    bool ctrl  = false;
    bool alt   = false;
    bool shift = false;
};

bool     g_armed = false;
key_bind g_key;
char     g_key_spec[32] = "F11";
bool     g_key_prev     = false;

bool          g_open       = false;
ImGuiContext *g_ctx        = nullptr;
bool          g_win32_init = false;
bool          g_dx_init    = false;
ID3D11Device *g_dx_device  = nullptr; // identity only (ImGui holds its own reference)

// The window the overlay is bound to right now (the presenter's hwnd). See "WINDOW BINDING" below.
HWND g_hwnd = nullptr;

// One row per window this module has subclassed. A row outlives its binding when something else
// subclassed the window ON TOP of us (a chain cannot be unlinked from the middle): subclass_proc then
// still receives that window's messages and must still know where to forward them, but acts on none.
struct subclassed_window {
    HWND    hwnd;
    WNDPROC prev;
    bool    unicode;
};
constexpr int     MAX_SUBCLASSED = 8;
subclassed_window g_subclassed[MAX_SUBCLASSED];
int               g_subclassed_n = 0;

// Last frame's capture verdict. Read by the WndProc between frames, which is the ImGui contract
// (the flags describe the frame just built, and apply to the input that arrives before the next).
bool   g_want_mouse = false;
bool   g_want_kbd   = false;
ImVec2 g_mouse_pos(-1.0f, -1.0f); // ImGui's mouse position last frame (client px): the harness reads it

// Evidence counters (logged at every close, shown in the panel).
long g_swallowed_mouse = 0;
long g_swallowed_keys  = 0;
long g_drained_mouse   = 0;
long g_drained_keys    = 0;
long g_opens           = 0;
long g_rebinds         = 0; // the subclass moved to a different HWND (evidence for the rebind path)

// The panels drawn last frame, in CLIENT pixels (see "PANEL RECTS").
struct panel_rect {
    float x0, y0, x1, y1;
};
constexpr int MAX_PANELS = 4;
panel_rect    g_panels[MAX_PANELS];
int           g_npanels = 0;

// Buttons whose DOWN we swallowed (their UP is swallowed too, whatever ImGui says by then) and buttons
// whose DOWN reached the game (their UP reaches it too -- a drag the game owns is never cut in half).
// Bit 0 = left, 1 = right, 2 = middle, 3/4 = X1/X2.
unsigned g_btn_swallowed = 0;
unsigned g_btn_game      = 0;

bool g_open_at_start = false; // [video] imgui_open_at_start

// ---- toggle key ---------------------------------------------------------------------------------
// "F11" | "Ctrl+Alt+F9" | "Shift+Insert" | "0x7A" | "D". The default F11 (scancode 0x57) is outside
// every scancode llm_strat_input_update compares against (0x01..0x53 in scattered ranges; the F-keys
// it claims are F1/F2 and F5..F10), and F12 is the frame-capture key (gfx_capture.cpp).
int vk_from_name(const char *k) {
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '1' && k[1] <= '9') {
        int n = 0;
        for (const char *p = k + 1; *p; ++p) {
            if (*p < '0' || *p > '9') return 0;
            n = n * 10 + (*p - '0');
        }
        return (n >= 1 && n <= 24) ? VK_F1 + n - 1 : 0;
    }
    if (k[0] == '0' && (k[1] == 'x' || k[1] == 'X')) {
        int v = 0;
        for (const char *p = k + 2; *p; ++p) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                                                      : (*p >= 'A' && *p <= 'F')   ? *p - 'A' + 10
                                                                                   : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        return (v > 0 && v < 256) ? v : 0;
    }
    static const struct {
        const char *name;
        int         vk;
    } NAMED[] = {{"Insert", VK_INSERT}, {"Home", VK_HOME}, {"End", VK_END}, {"PgUp", VK_PRIOR}, {"PgDn", VK_NEXT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL}, {"Tilde", VK_OEM_3}, {"Apps", VK_APPS}, {"Delete", VK_DELETE}};
    for (const auto &n : NAMED)
        if (lstrcmpiA(n.name, k) == 0) return n.vk;
    if (k[0] && !k[1]) {
        char c = k[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    return 0;
}

bool parse_key(const char *spec, key_bind *out) {
    key_bind k;
    k.vk = 0;
    char buf[32];
    lstrcpynA(buf, spec, sizeof(buf));
    char *part = buf;
    for (;;) {
        char *plus = part;
        while (*plus && *plus != '+') ++plus;
        const bool last = (*plus == 0);
        *plus           = 0;
        if (last) {
            k.vk = vk_from_name(part);
            break;
        }
        if (lstrcmpiA(part, "Ctrl") == 0) k.ctrl = true;
        else if (lstrcmpiA(part, "Alt") == 0) k.alt = true;
        else if (lstrcmpiA(part, "Shift") == 0) k.shift = true;
        else return false;
        part = plus + 1;
    }
    if (!k.vk) return false;
    *out = k;
    return true;
}

// TWO toggle sources, neither of them GetAsyncKeyState:
//   * the game window's own WM_KEYDOWN / WM_SYSKEYDOWN for the bound key (first press only, not the
//     auto-repeats), seen by the subclass -- so the key counts exactly when the GAME has keyboard
//     focus, never when F11 is pressed in another application, and on any desktop that delivers
//     messages (GetAsyncKeyState answers 0 off the input desktop);
//   * the UI harness's synthesised chord (MH_UIDrive_SynthKeyDown, the `hotkey` action), rising edge,
//     polled per present -- the same route the [debug] overlay's hotkeys take.
bool g_toggle_pending = false;

bool mods_match_msg() { // message-time modifier state, i.e. what the user held with this key
    const bool c = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool a = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool s = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    return c == g_key.ctrl && a == g_key.alt && s == g_key.shift;
}

void note_key_msg(UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && (int)w == g_key.vk && (l & (1 << 30)) == 0 && mods_match_msg())
        g_toggle_pending = true;
}

bool toggle_fired() {
    bool down = MH_UIDrive_SynthKeyDown(g_key.vk) != 0;
    if (down && g_key.ctrl && !MH_UIDrive_SynthKeyDown(VK_CONTROL)) down = false;
    if (down && g_key.alt && !MH_UIDrive_SynthKeyDown(VK_MENU)) down = false;
    if (down && g_key.shift && !MH_UIDrive_SynthKeyDown(VK_SHIFT)) down = false;
    const bool fired = (down && !g_key_prev) || g_toggle_pending;
    g_key_prev       = down;
    g_toggle_pending = false;
    return fired;
}

// ---- DirectInput drain --------------------------------------------------------------------------
// IDirectInputDevice(A)::GetDeviceData, vtable slot 10 (+0x28) -- the slot and the 16-byte
// DIDEVICEOBJECTDATA the game's own polls use.
struct di_object_data {
    DWORD ofs, data, timestamp, sequence;
};
typedef HRESULT(__stdcall *get_device_data_fn)(void *self, DWORD cb, di_object_data *rgdod, DWORD *inout, DWORD flags);

// Read everything buffered on `dev` and hand each record to `each`; returns the count. Errors
// (DIERR_NOTACQUIRED / INPUTLOST: the game re-acquires on its own next poll) just end the drain.
template <class F>
long drain_device(uintptr_t slot, F each) {
    void *dev = *(void *volatile *)slot;
    if (!dev) return 0;
    auto fn    = reinterpret_cast<get_device_data_fn>((*reinterpret_cast<void ***>(dev))[10]);
    long total = 0;
    for (int guard = 0; guard < 64; ++guard) {
        di_object_data buf[32];
        DWORD          n = 32;
        if (FAILED(fn(dev, sizeof(di_object_data), buf, &n, 0)) || n == 0) break;
        for (DWORD i = 0; i < n; ++i) each(buf[i]);
        total += (long)n;
        if (n < 32) break;
    }
    return total;
}

bool di_filter_live();

// Only on the fallback path: with the DI filter live, motion must reach the game (see THE CURSOR).
void drain_mouse() {
    if (di_filter_live()) return;
    g_drained_mouse += drain_device(ADDR_DI_MOUSE, [](const di_object_data &) {});
}

void drain_keyboard() {
    g_drained_keys += drain_device(ADDR_DI_KEYBOARD, [](const di_object_data &d) {
        if ((d.data & 0x80) != 0) return; // a key-down: dropped
        // A key-UP: release it the way llm_input_di_keyboard_poll would, so it cannot stick.
        volatile uint8_t *ks = (volatile uint8_t *)ADDR_KEYSTATE;
        ks[d.ofs & 0x7f] &= (d.ofs & 0x80) ? 0xfd : 0xfe;
    });
}

// ---- THE CURSOR -----------------------------------------------------------------------------------
// The cursor the player SEES is the game's own sprite, drawn at _G_LLM_CURSOR_X/Y (game pixels) inside
// the scaled image. With the default DirectInput mouse that point is the integral of relative counts
// (1 count = 1 game px, the game's divisor/threshold/clamp) and has nothing to do with the hidden OS
// pointer, which Windows moves with its own ballistics in SCREEN pixels. Feeding ImGui the OS pointer
// (imgui_impl_win32's WM_MOUSEMOVE) therefore put ImGui's hover somewhere the player could not see --
// the 2026-09-28 report: "the cursor enters the widget not at its borderline but somewhere else". So:
//
//   * ImGui's mouse position IS the game cursor, mapped game -> client through the presenter's own
//     image rect (fit_image_rect over the client, the rect the frame is drawn into), at the centre of
//     that game pixel. One position for hover, for the swallow verdict, and for the drawn sprite.
//   * DI MOTION ALWAYS REACHES THE GAME. The overlay used to drain the DI buffer while ImGui had the
//     mouse, which froze the accumulator: the sprite stopped at the panel edge and could not be moved
//     back out. Now the mouse device's GetDeviceData is filtered instead (a vtable-slot hook, below):
//     X/Y records pass untouched, so the game's own integrate-and-clamp keeps running and the sprite
//     and ImGui move together; only BUTTON and WHEEL records ImGui owns are removed. ImGui is fed the
//     same records (position after each motion record, then the button), so it sees exactly the clicks
//     the game would have -- including in DISCL_EXCLUSIVE mode, where Windows sends no mouse messages.
//   * Under the panel the sprite is hidden by ImGui's own draw, so ImGui draws its arrow at the same
//     point (MouseDrawCursor while it has the mouse).
//
// With no DI mouse ([input] mouse_absolute=1, the harness's `wmclick`) the game cursor is the mapped OS
// pointer anyway; ImGui still takes its position from the game cursor, and its buttons from messages.
bool cursor_client(float *x, float *y) { // the game cursor in client pixels; false when unmappable
    // M1WL v6: the presenter's own mapping first -- it knows the ui_scale quad layout (map 1:1, HUD panel and strip enlarged), which the plain
    // letterbox below does not, so the drawn cursor and this point are the same pixel. The letterbox stays as the fallback (no owned presenter).
    {
        POINT p = {(LONG)*x, (LONG)*y};
        if (mh::gfx::owned_game_to_client(&p)) {
            *x = (float)p.x;
            *y = (float)p.y;
            return true;
        }
    }
    int mw = 0, mh = 0;
    if (!g_hwnd || !mh::gfx::owned_mode_size(&mw, &mh)) return false;
    RECT cr;
    if (!GetClientRect(g_hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
    const RECT r  = fit_image_rect(cr.right, cr.bottom, mw, mh, mh::gfx::owned_integer_scale());
    const int  rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw <= 0 || rh <= 0) return false;
    *x = (float)r.left + ((float)*x + 0.5f) * (float)rw / (float)mw;
    *y = (float)r.top + ((float)*y + 0.5f) * (float)rh / (float)mh;
    return true;
}

bool game_cursor_client(float *x, float *y) {
    *x = (float)rd<int>(ADDR_CURSOR_X);
    *y = (float)rd<int>(ADDR_CURSOR_Y);
    return cursor_client(x, y);
}

bool over_panel_pt(float x, float y) {
    for (int i = 0; i < g_npanels; ++i)
        if (x >= g_panels[i].x0 && x < g_panels[i].x1 && y >= g_panels[i].y0 && y < g_panels[i].y1) return true;
    return false;
}

// ---- the DirectInput mouse filter -----------------------------------------------------------------
// IDirectInputDevice::GetDeviceData is slot 10 of the device's vtable, which lives in dinput.dll (or a
// wrapper such as dinputto8) and is shared by every device of that class -- so the hook checks `self`
// against _G_LLM_DI_MOUSE_DEVICE and forwards everything else (the keyboard) untouched. Installed on
// the first OPEN, never removed; while the overlay is closed it is a pure pass-through except for the
// button UPs it still owes (a DOWN it swallowed has its UP swallowed too, whatever happens between).
struct di_hooked_vtbl {
    void             **vtbl;
    get_device_data_fn real;
};
di_hooked_vtbl g_di_vtbls[4];
int            g_di_nvtbls      = 0;
unsigned       g_di_swallowed   = 0; // DI button bits (1 L, 2 R, 4 M, 8 X) whose DOWN ImGui took
unsigned       g_di_game        = 0; // ... whose DOWN reached the game
long           g_di_filtered    = 0; // button/wheel records removed (evidence)
long           g_di_motion_seen = 0; // motion records passed while open (evidence)

get_device_data_fn di_real_for(void *self) {
    void **vt = *reinterpret_cast<void ***>(self);
    for (int i = 0; i < g_di_nvtbls; ++i)
        if (g_di_vtbls[i].vtbl == vt) return g_di_vtbls[i].real;
    return nullptr;
}

int di_step(uintptr_t div_addr, uintptr_t thr_addr, int data) { // llm_input_di_mouse_poll's delta rule
    const int div = rd<int>(div_addr);
    int       d   = div ? data / div : data;
    if ((d < 0 ? -d : d) > rd<int>(thr_addr)) d *= 2;
    return d;
}

void filter_mouse_records(di_object_data *rg, DWORD *inout) {
    if (!g_open && !g_di_swallowed) return; // closed and nothing owed: untouched
    const bool feed = g_open && g_ctx;
    if (feed) ImGui::SetCurrentContext(g_ctx);
    // M1WL v6: the position is the DRAWN cursor (the game's screen cursor), not the DirectInput accumulator: once the pointer is over the ui_scale HUD
    // panel the two differ (rc11 trace: 4441 of 8271 rows, e.g. 1759,332 against 1526,459), so clicks and the panel hit-test followed the wrong point.
    // The batch's motion records are added on top (the game integrates them after this filter runs), clamped to the mode box.
    int x = rd<int>(ADDR_CURSOR_X), y = rd<int>(ADDR_CURSOR_Y);
    int bw = rd<int>(ADDR_DI_BOUND_W), bh = rd<int>(ADDR_DI_BOUND_H);
    {
        int mw = 0, mh = 0;
        if (mh::gfx::owned_mode_size(&mw, &mh) && mw > 0 && mh > 0) {
            bw = mw;
            bh = mh;
        }
    }
    auto pos_event = [&]() {
        float cx = (float)x, cy = (float)y;
        if (feed && cursor_client(&cx, &cy)) ImGui::GetIO().AddMousePosEvent(cx, cy);
    };
    auto over = [&]() {
        float cx = (float)x, cy = (float)y;
        return cursor_client(&cx, &cy) && over_panel_pt(cx, cy);
    };
    DWORD out = 0;
    for (DWORD i = 0; i < *inout; ++i) {
        const di_object_data d    = rg[i];
        bool                 keep = true;
        if (d.ofs == 0 || d.ofs == 4) { // DIMOFS_X / DIMOFS_Y: always the game's; track it for ImGui
            if (d.ofs == 0) {
                x += di_step(ADDR_DI_DIV, ADDR_DI_THRESH, (int)d.data);
                x = x >= bw ? bw - 1 : x < 0 ? 0
                                             : x;
            } else {
                y += di_step(ADDR_DI_DIV, ADDR_DI_THRESH, (int)d.data);
                y = y >= bh ? bh - 1 : y < 0 ? 0
                                             : y;
            }
            if (feed) {
                ++g_di_motion_seen;
                pos_event();
            }
        } else if (d.ofs == 8) { // DIMOFS_Z: the wheel
            const bool mine = feed && (g_want_mouse || over());
            if (mine) {
                ImGui::GetIO().AddMouseWheelEvent(0.0f, (float)(int)d.data / (float)WHEEL_DELTA);
                keep = false;
            }
        } else if (d.ofs >= 12 && d.ofs <= 15) { // DIMOFS_BUTTON0..3
            const unsigned bit  = 1u << (d.ofs - 12);
            const bool     down = (d.data & 0x80) != 0;
            bool           mine;
            if (!down) {
                mine = (g_di_swallowed & bit) != 0;
                g_di_swallowed &= ~bit;
                g_di_game &= ~bit;
            } else {
                mine = feed && (g_want_mouse || (g_di_game == 0 && over()));
                if (mine) g_di_swallowed |= bit;
                else g_di_game |= bit;
            }
            if (feed) {
                pos_event();
                ImGui::GetIO().AddMouseButtonEvent((int)(d.ofs - 12), down);
            }
            keep = !mine;
        }
        if (keep) rg[out++] = d;
        else ++g_di_filtered;
    }
    *inout = out;
}

HRESULT __stdcall hooked_get_device_data(void *self, DWORD cb, di_object_data *rg, DWORD *inout, DWORD flags) {
    const get_device_data_fn real = di_real_for(self);
    if (!real) return E_FAIL; // unreachable: only a vtable we patched routes here
    const HRESULT hr = real(self, cb, rg, inout, flags);
    if (SUCCEEDED(hr) && rg && inout && cb == sizeof(di_object_data) && (flags & 1 /*DIGDD_PEEK*/) == 0 &&
        self == *(void *volatile *)ADDR_DI_MOUSE)
        filter_mouse_records(rg, inout);
    return hr;
}

// True once the CURRENT mouse device's vtable routes GetDeviceData through the filter.
bool di_filter_live() {
    void *dev = *(void *volatile *)ADDR_DI_MOUSE;
    if (!dev) return false;
    if (mh::input::owned_dinput_active()) { // PT-INPUT1: the owned device has a filter point -- no vtable patch
        static bool s_reg = false;
        if (!s_reg) {
            s_reg = true;
            mh::input::set_mouse_filter([](void *rg, DWORD *n) { filter_mouse_records((di_object_data *)rg, n); });
            log("; [imgui] DI mouse filter installed on the OWNED DirectInput mouse (set_mouse_filter)");
        }
        return true;
    }
    void **vt = *reinterpret_cast<void ***>(dev);
    if (vt[10] == (void *)&hooked_get_device_data) return true;
    static bool failed = false; // one failed install is final: no VirtualProtect per message after it
    if (failed || g_di_nvtbls >= (int)(sizeof(g_di_vtbls) / sizeof(g_di_vtbls[0]))) return false;
    DWORD old = 0;
    if (!VirtualProtect(&vt[10], sizeof(void *), PAGE_READWRITE, &old)) {
        failed = true;
        log("; [imgui] DI mouse filter NOT installed (VirtualProtect %lu) -- the overlay drains DI instead",
            GetLastError());
        return false;
    }
    g_di_vtbls[g_di_nvtbls].vtbl = vt;
    g_di_vtbls[g_di_nvtbls].real = reinterpret_cast<get_device_data_fn>(vt[10]);
    ++g_di_nvtbls;
    vt[10] = (void *)&hooked_get_device_data;
    DWORD dummy;
    VirtualProtect(&vt[10], sizeof(void *), old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), &vt[10], sizeof(void *));
    log("; [imgui] DI mouse filter installed (device %p, vtable %p, GetDeviceData %p): motion always reaches the game, "
        "ImGui takes position from the game cursor and the buttons/wheel it owns",
        dev, (void *)vt, (void *)g_di_vtbls[g_di_nvtbls - 1].real);
    return true;
}

// ---- the subclassed window procedure --------------------------------------------------------------
bool is_mouse_msg(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
bool is_key_consuming_msg(UINT m) { // key-UPs deliberately absent
    return m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_CHAR || m == WM_SYSCHAR || m == WM_DEADCHAR ||
           m == WM_SYSDEADCHAR || m == WM_UNICHAR;
}

// PANEL RECTS. io.WantCaptureMouse is last FRAME's verdict: it only turns true once a frame has been
// built with the pointer already over a panel. A hand that moves onto the panel and clicks before the
// next present -- or a MOVE+DOWN pair posted together, or a pointer ImGui's own WM_MOUSELEAVE tracking
// has just declared gone -- would reach the game on that flag alone. So a mouse message whose own
// coordinates lie inside a panel drawn last frame is ImGui's as well. Buttons are paired: an UP goes
// wherever its DOWN went (the bits above), so neither side ever sees half a click; and while the GAME
// holds a button the rect rule is off, exactly like WantCaptureMouse (ImGui never steals a drag the game
// started).
unsigned button_bit(UINT m, WPARAM w) {
    switch (m) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: return 1;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK: return 2;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: return 4;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK: return (GET_XBUTTON_WPARAM(w) == XBUTTON1) ? 8u : 16u;
        default: return 0;
    }
}
bool is_button_up(UINT m) { return m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP || m == WM_XBUTTONUP; }

bool over_panel(HWND h, UINT m, LPARAM l) {
    POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
    if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) ScreenToClient(h, &p); // the wheel carries SCREEN coords
    for (int i = 0; i < g_npanels; ++i)
        if (p.x >= g_panels[i].x0 && p.x < g_panels[i].x1 && p.y >= g_panels[i].y0 && p.y < g_panels[i].y1)
            return true;
    return false;
}

// The whole mouse verdict for one message while the overlay is open: true = ImGui's, swallow it.
bool take_mouse(HWND h, UINT m, WPARAM w, LPARAM l) {
    const unsigned bit = button_bit(m, w);
    if (bit && is_button_up(m)) {
        const bool mine = (g_btn_swallowed & bit) != 0;
        g_btn_swallowed &= ~bit;
        g_btn_game &= ~bit;
        return mine;
    }
    const bool mine = g_want_mouse || (g_btn_game == 0 && over_panel(h, m, l));
    if (bit) {
        if (mine) g_btn_swallowed |= bit;
        else g_btn_game |= bit;
    }
    return mine;
}

subclassed_window *find_subclassed(HWND h) {
    for (int i = 0; i < g_subclassed_n; ++i)
        if (g_subclassed[i].hwnd == h) return &g_subclassed[i];
    return nullptr;
}

void unsubclass(HWND h, const char *why, bool dying = false);

LRESULT CALLBACK subclass_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const subclassed_window *row = find_subclassed(h);
    if (!row) return DefWindowProcA(h, m, w, l); // unreachable: every window routed here has a row
    const WNDPROC prev = row->prev;
    const bool    uni  = row->unicode;
    if (h == g_hwnd) {
        note_key_msg(m, w, l); // observe only: the key itself still reaches the game either way
        if (g_open && g_ctx && g_win32_init) {
            // DI filter live: the mouse reaches ImGui (and is withheld from the game) through the DI
            // records, so ImGui sees no mouse MESSAGE -- its buttons would double and its position is
            // the OS pointer's -- and the game, which ignores mouse messages while it has a DI mouse,
            // is not asked about them either.
            const bool di        = di_filter_live();
            const bool mouse_msg = is_mouse_msg(m) || m == WM_MOUSELEAVE || m == WM_MOUSEHOVER;
            if (!(di && mouse_msg)) ImGui_ImplWin32_WndProcHandler(h, m, w, l); // WM_SETCURSOR: NoMouseCursorChange
            const bool mouse_mine = !di && is_mouse_msg(m) && take_mouse(h, m, w, l);
            if (g_want_mouse || mouse_mine) drain_mouse();
            if (g_want_kbd) drain_keyboard();
            if (mouse_mine) {
                ++g_swallowed_mouse;
                return 0;
            }
            if (g_want_kbd && is_key_consuming_msg(m)) {
                ++g_swallowed_keys;
                return 0;
            }
        }
    }
    // The window is going away: take ourselves out of its chain (and drop the binding) on its last
    // message, so no row of ours is left on a dead HWND whose number Windows may hand out again.
    if (m == WM_NCDESTROY) {
        const LRESULT r = uni ? CallWindowProcW(prev, h, m, w, l) : CallWindowProcA(prev, h, m, w, l);
        unsubclass(h, "WM_NCDESTROY", true);
        return r;
    }
    return uni ? CallWindowProcW(prev, h, m, w, l) : CallWindowProcA(prev, h, m, w, l);
}

// WINDOW BINDING. The presenter hands on_frame its hwnd every present, and present_backend.h's contract
// lets the game hand over a DIFFERENT window (bind_backend detaches, then attaches to the new one). The
// subclass follows the window: unlink from the old one, link into the new one, and re-bind ImGui's
// Win32 backend (it caches the HWND at init). Unlinking is only possible while our proc is still the
// window's current one; if something subclassed on top of us since, the row stays so subclass_proc can
// keep forwarding, and it is inert for that window from then on (h != g_hwnd).
void unsubclass(HWND h, const char *why, bool dying) {
    subclassed_window *row = find_subclassed(h);
    if (!row) return;
    const bool     uni   = row->unicode;
    const bool     alive = IsWindow(h) != 0;
    const LONG_PTR cur   = !alive ? 0 : uni ? GetWindowLongPtrW(h, GWLP_WNDPROC)
                                            : GetWindowLongPtrA(h, GWLP_WNDPROC);
    // `dying` (WM_NCDESTROY, already forwarded down the chain): the window delivers nothing after this
    // message, so the row goes whether or not we are still the chain's head.
    if (!alive || dying || cur == (LONG_PTR)&subclass_proc) {
        if (alive && cur == (LONG_PTR)&subclass_proc) {
            if (uni) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)row->prev);
            else SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)row->prev);
        }
        log("; [imgui] window %p unsubclassed (%s), proc %p restored", (void *)h, why, (void *)row->prev);
        *row = g_subclassed[--g_subclassed_n];
    } else {
        log("; [imgui] window %p NOT unsubclassed (%s): proc %p was subclassed over ours -- left forwarding, inert",
            (void *)h, why, (void *)cur);
    }
    if (h == g_hwnd) g_hwnd = nullptr;
}

void subclass(HWND hwnd) {
    if (!hwnd) return;
    if (find_subclassed(hwnd)) { // still linked (a window we could not unlink earlier): just re-bind
        g_hwnd = hwnd;
        return;
    }
    if (g_subclassed_n >= MAX_SUBCLASSED) {
        log("; [imgui] window %p NOT subclassed: %d windows already carry the subclass", (void *)hwnd, g_subclassed_n);
        return;
    }
    subclassed_window &r = g_subclassed[g_subclassed_n];
    r.hwnd               = hwnd;
    r.unicode            = IsWindowUnicode(hwnd) != 0;
    // Row first, then the swap: a message the swap itself provokes must already find its row.
    r.prev = r.unicode ? (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    ++g_subclassed_n;
    const WNDPROC was = r.unicode ? (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&subclass_proc)
                                  : (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)&subclass_proc);
    if (was) r.prev = was;
    g_hwnd = hwnd;
    log("; [imgui] game window %p subclassed (%s proc, previous %p)", (void *)hwnd, r.unicode ? "W" : "A",
        (void *)r.prev);
}

// Everything bound to the current window, released: capture ended cleanly (DI drained, button pairing
// reset), ImGui's Win32 backend shut down (it caches the HWND), the subclass unlinked. The ImGui context
// and the open/closed state survive, so a rebind -- or a device-lost re-attach to the SAME window --
// picks up where it was.
void release_window(const char *why) {
    if (g_want_mouse) drain_mouse();
    if (g_want_kbd) drain_keyboard();
    g_want_mouse = g_want_kbd = false;
    g_btn_swallowed = g_btn_game = 0;
    g_npanels                    = 0;
    if (g_win32_init) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui_ImplWin32_Shutdown();
        g_win32_init = false;
    }
    if (g_hwnd) unsubclass(g_hwnd, why);
}

void bind_window(HWND hwnd) {
    if (hwnd == g_hwnd) return;
    if (g_hwnd) {
        ++g_rebinds;
        log("; [imgui] presenter window changed %p -> %p: re-binding", (void *)g_hwnd, (void *)hwnd);
        release_window("rebind");
    }
    subclass(hwnd);
}

// ---- ImGui lifetime ------------------------------------------------------------------------------
bool ensure_context(HWND hwnd) {
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        g_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO &io    = ImGui::GetIO();
        io.IniFilename = nullptr; // no imgui.ini beside the game
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // the game owns the cursor
        ImGui::StyleColorsDark();
        log("; [imgui] context created (Dear ImGui %s)", IMGUI_VERSION);
    }
    ImGui::SetCurrentContext(g_ctx);
    if (!g_win32_init) {
        g_win32_init = ImGui_ImplWin32_Init(hwnd);
        if (!g_win32_init) {
            log("; [imgui] ImGui_ImplWin32_Init failed -- overlay stays closed");
            return false;
        }
    }
    return true;
}

bool ensure_dx(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if (g_dx_init && g_dx_device == dev) return true;
    if (g_dx_init) {
        ImGui_ImplDX11_Shutdown();
        g_dx_init = false;
    }
    g_dx_init   = ImGui_ImplDX11_Init(dev, ctx);
    g_dx_device = g_dx_init ? dev : nullptr;
    log("; [imgui] DX11 backend %s on device %p", g_dx_init ? "initialised" : "FAILED to initialise", (void *)dev);
    return g_dx_init;
}

// The presenter's live options (present_backend::set_config): filter and vsync apply on the next present
// without tearing the device down. The window mode is not live (the owner restyles, then re-attaches).
void draw_presenter_controls() {
    const int filter = mh::gfx::owned_filter();
    ImGui::TextUnformatted("filter");
    constexpr int N = mh::gfx::OWNED_FILTER_COUNT;
    ImVec2        at[N + 2];
    for (int i = 0; i < N; ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(mh::gfx::owned_filter_name(i), filter == i)) mh::gfx::owned_set_filter(i);
        at[i] = ImGui::GetItemRectMin();
    }
    bool vsync = mh::gfx::owned_vsync();
    if (ImGui::Checkbox("vsync", &vsync)) mh::gfx::owned_set_vsync(vsync);
    at[N]        = ImGui::GetItemRectMin();
    bool integer = mh::gfx::owned_integer_scale();
    ImGui::SameLine();
    if (ImGui::Checkbox("integer scale", &integer)) mh::gfx::owned_set_integer_scale(integer);
    at[N + 1] = ImGui::GetItemRectMin();
    // Once, for script authors: where the controls are, in client pixels (a `wmclick` target is the
    // control's box, i.e. a few pixels right of and below these corners).
    static bool s_logged = false;
    if (!s_logged && at[0].x > 0.0f) {
        s_logged = true;
        log("; [imgui] panel controls (client, top-left): point %d,%d linear %d,%d sharp %d,%d area %d,%d vsync %d,%d "
            "integer %d,%d",
            (int)at[0].x, (int)at[0].y, (int)at[1].x, (int)at[1].y, (int)at[2].x, (int)at[2].y, (int)at[3].x,
            (int)at[3].y, (int)at[4].x, (int)at[4].y, (int)at[5].x, (int)at[5].y);
    }
}

// ---- mods:M1WL v3: the "Stream heat" section and the heat map ---------------------------------------------------------------------------------
// The section changes the movement planner's stream-heat keys LIVE (single player only; greyed in a lockstep session, seams/heat_ui.h), and draws a
// heat map over the strategic map. The map is DISPLAY ONLY: it reads the unit table (traffic), a DLL-side snapshot the heat rule fills on each plan
// (last plan) and the planning unit's live path; it writes no sim memory and consumes no RNG. It draws only while the panel is open (the overlay
// does not exist otherwise) and only in the strategic map mode, clipped to the map view so the HUD panel and the bottom strip stay clean.
//
// TILE -> SCREEN. The strategic view draws world pixel (cam_col * 32 + p) at viewport pixel p, with the zoom module's crop in between (MH_MapZoom_Get:
// crop origin zl / zt, size zcw x zch, stretched over the W x H viewport): a tile at column offset dc from the camera starts at viewport pixel
// (dc * 32 - zl) * W / zcw. The camera, like the map, is a torus: dc is folded into [-w/2, w/2). The viewport pixel is a GAME pixel (the viewport
// sits at the frame's origin); game -> client goes through the presenter's mapping (owned_game_to_client) at two far corners, which gives the linear map
// even where [video] ui_scale lays the frame out as several quads (the map quad is 1:1).
constexpr uintptr_t ADDR_GAME_MODE = mh::addr::_G_LLM_GAME_MODE;
constexpr uintptr_t ADDR_VIEW_W    = mh::addr::G_WIN_W; // the strategic viewport (window minus the 160 px panel)
constexpr uintptr_t ADDR_VIEW_H    = mh::addr::G_WIN_H;
constexpr int       HEAT_TILE_PX   = mh::gfx::heat::HEAT_TILE_PX;

int g_heat_map = 0; // 0 off, 1 last plan, 2 traffic ([video] imgui_heat_map = the mode at start)

using mh::gfx::heat::heat_xf;

bool heat_transform(heat_xf *x) {
    if (rd<uint8_t>(ADDR_GAME_MODE) != 2) return false; // the strategic map only
    const int   vw = rd<int32_t>(ADDR_VIEW_W), vh = rd<int32_t>(ADDR_VIEW_H);
    const auto *geom = mh::state::ptr<const mh::game::mh_llm_strat_map_geom>(mh::state::RID_GENERAL);
    if (vw < 64 || vh < 64 || !geom) return false;
    x->mw = (int)geom->width_mask + 1;
    x->mh = (int)geom->height_mask + 1;
    if (x->mw < 8 || x->mh < 8 || (x->mw & (x->mw - 1)) || (x->mh & (x->mh - 1))) return false;
    x->vw    = vw;
    x->vh    = vh;
    x->camc  = *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_COL);
    x->camr  = *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_ROW);
    int z100 = 100, zl = 0, zt = 0, zcw = 0, zch = 0;
    MH_MapZoom_Get(&z100, &zl, &zt, &zcw, &zch);
    x->zl    = zcw > 0 ? zl : 0;
    x->zt    = zch > 0 ? zt : 0;
    x->zcw   = zcw > 0 ? zcw : vw;
    x->zch   = zch > 0 ? zch : vh;
    POINT p0 = {0, 0}, p1 = {vw / 2, vh / 2}, pc = {vw - 1, vh - 1};
    if (!mh::gfx::owned_game_to_client(&p0) || !mh::gfx::owned_game_to_client(&p1) || !mh::gfx::owned_game_to_client(&pc)) return false;
    if (!x->set_map_points(p0.x, p0.y, p1.x, p1.y, pc.x, pc.y)) return false;
    x->ok = true;
    return true;
}

uint8_t g_heat_level[65536]; // traffic mode: the decayed units-per-tile level (0 .. 255)
uint8_t g_heat_now[65536];

ImU32 heat_color_plan(int count) { // 1 mate yellow -> 4 and more red
    const int   c = count < 1 ? 1 : (count > 4 ? 4 : count);
    const float t = (float)(c - 1) / 3.0f;
    return IM_COL32(255, (int)(220.0f - 180.0f * t), 0, (int)(95.0f + 80.0f * t));
}

ImU32 heat_color_traffic(int level) { // blue -> magenta -> red as the level climbs
    const float t = (float)level / 255.0f;
    return IM_COL32((int)(40.0f + 215.0f * t), (int)(90.0f * (1.0f - t)), (int)(255.0f * (1.0f - t * 0.6f)), (int)(70.0f + 120.0f * t));
}

void draw_heat_map() {
    if (g_heat_map == 0) return;
    heat_xf x;
    if (!heat_transform(&x)) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    // clip to the map view: the viewport corners in client pixels
    dl->PushClipRect(ImVec2(x.ox, x.oy), ImVec2(x.cx1, x.cy1), true);
    if (g_heat_map == 1) {
        const mh::heatui::snapshot &sn = mh::heatui::last_plan();
        for (int k = 0; k < sn.n; ++k) {
            if (sn.cnt[k] == 0) continue;
            const ImU32 col = heat_color_plan(sn.cnt[k]);
            x.each_copy((int)(sn.flat[k] >> 8), (int)(sn.flat[k] & 0xff), [&](int di, int dj) {
                ImVec2 a, b;
                if (x.tile_rect(di, dj, &a, &b)) dl->AddRectFilled(a, b, col);
            });
        }
        if (sn.seq != 0) { // the planning unit's route from where it is now
            static uint32_t route[300 * 4];
            const int       n = mh::heatui::unit_route(sn.player, sn.unit, route, 300 * 4);
            // The route is UNWRAPPED: the first tile at its first visible offset, every later one a small signed step from the one before, so the line
            // stays one piece across the fold seam. It is then drawn at each map-width shift that lands in view (a view wider than the map repeats it).
            int pc = 0, pr = 0, ui = 0, uj = 0, pui = 0, puj = 0;
            for (int k = 0; k < n; ++k) {
                const int c = (int)(route[k] >> 8), r = (int)(route[k] & 0xff);
                pui = ui;
                puj = uj;
                if (k == 0) {
                    ui = x.first_i(c);
                    uj = x.first_j(r);
                } else {
                    ui += x.fold(c - pc, x.mw);
                    uj += x.fold(r - pr, x.mh);
                }
                pc = c;
                pr = r;
                for (int sj = -1; sj <= 1; ++sj)
                    for (int si = -1; si <= 1; ++si) {
                        ImVec2 a, b;
                        if (!x.tile_rect(ui + si * x.mw, uj + sj * x.mh, &a, &b)) continue;
                        const ImVec2 c2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                        dl->AddRectFilled(ImVec2(c2.x - 2.0f, c2.y - 2.0f), ImVec2(c2.x + 2.0f, c2.y + 2.0f), IM_COL32(0, 230, 255, 230));
                        ImVec2 pa, pb;
                        if (k > 0 && x.tile_rect(pui + si * x.mw, puj + sj * x.mh, &pa, &pb))
                            dl->AddLine(ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), c2, IM_COL32(0, 230, 255, 200), 1.5f);
                    }
            }
        }
    } else {
        mh::heatui::traffic_sample(g_heat_now);
        for (int i = 0; i < 65536; ++i) {
            int lv = g_heat_level[i];
            lv -= (lv >> 3) + (lv ? 1 : 0); // a short decay (about 20 presents)
            const int now = g_heat_now[i] * 64;
            if (now > lv) lv = now > 255 ? 255 : now;
            g_heat_level[i] = (uint8_t)lv;
        }
        const int i0 = (x.zl / HEAT_TILE_PX) - 1, i1 = (x.zl + x.zcw) / HEAT_TILE_PX + 1;
        const int j0 = (x.zt / HEAT_TILE_PX) - 1, j1 = (x.zt + x.zch) / HEAT_TILE_PX + 1;
        for (int dj = j0; dj <= j1; ++dj)
            for (int di = i0; di <= i1; ++di) {
                const uint32_t c = (uint32_t)(x.camc + di) & (uint32_t)(x.mw - 1), r = (uint32_t)(x.camr + dj) & (uint32_t)(x.mh - 1);
                const int      lv = g_heat_level[(c << 8) | r];
                if (lv == 0) continue;
                ImVec2 a, b;
                if (!x.tile_rect(di, dj, &a, &b)) continue;
                dl->AddRectFilled(a, b, heat_color_traffic(lv));
            }
    }
    dl->PopClipRect();
}

// A slider whose value is applied when the drag ENDS (one log line, one hook run per edit instead of one per frame); while it is held it shows the
// value being dragged, not the one in force.
struct heat_edit {
    bool active = false;
    int  v      = 0;
};
bool heat_slider(const char *label, int cur, int lo, int hi, heat_edit &e, const char *shown, int *applied) {
    int v = e.active ? e.v : cur;
    if (ImGui::SliderInt(label, &v, lo, hi, shown, ImGuiSliderFlags_AlwaysClamp)) {
        e.active = true;
        e.v      = v;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        e.active = false;
        *applied = v;
        return true;
    }
    if (!ImGui::IsItemActive() && !ImGui::IsItemEdited()) e.active = false;
    return false;
}

void draw_heat_section() {
    if (!ImGui::CollapsingHeader("Stream heat", ImGuiTreeNodeFlags_DefaultOpen)) return;
    using namespace mh::gameplay;
    mh::heatui::live_values lv;
    mh::heatui::get_live(&lv);
    const char *why    = nullptr;
    const bool  locked = heat_locked(&why);
    if (locked) ImGui::TextDisabled("locked: %s", why);
    else ImGui::TextDisabled("single player; acts from the next plan");
    static const int LEGAL[16] = {0, 2, 32, 34, 64, 66, 96, 98, 128, 130, 160, 162, 192, 194, 224, 226}; // sim/move/stream_heat.h sh_legal_cost
    int              idx       = 1;
    for (int i = 0; i < 16; ++i)
        if (LEGAL[i] == lv.cost) idx = i;
    static heat_edit e_cost, e_win, e_mates, e_turn, e_cap, e_margin;
    int              ap = 0;
    ImGui::BeginDisabled(locked);
    bool on = lv.on != 0;
    if (ImGui::Checkbox("heat on", &on)) heat_set_live(HEAT_ON, on ? 1 : 0);
    ImGui::SameLine();
    bool scale = lv.scale != 0;
    if (ImGui::Checkbox("scale by mates", &scale)) heat_set_live(HEAT_SCALE, scale ? 1 : 0);
    bool retrace = lv.retrace != 0;
    if (ImGui::Checkbox("re-trace", &retrace)) heat_set_live(HEAT_RETRACE, retrace ? 1 : 0);
    char shown[24];
    wsprintfA(shown, "%d", LEGAL[e_cost.active ? e_cost.v : idx]);
    if (heat_slider("cost", idx, 0, 15, e_cost, shown, &ap)) heat_set_live(HEAT_COST, LEGAL[ap]);
    wsprintfA(shown, "%d tiles", e_win.active ? e_win.v : lv.window);
    if (heat_slider("window", lv.window, 1, 160, e_win, shown, &ap)) heat_set_live(HEAT_WINDOW, ap);
    wsprintfA(shown, "%d mates", e_mates.active ? e_mates.v : lv.min_mates);
    if (heat_slider("min mates", lv.min_mates, 1, 64, e_mates, shown, &ap)) heat_set_live(HEAT_MIN_MATES, ap);
    wsprintfA(shown, "%d", e_turn.active ? e_turn.v : lv.turn);
    if (heat_slider("turn penalty", lv.turn, 0, 32, e_turn, shown, &ap)) heat_set_live(HEAT_TURN, ap);
    wsprintfA(shown, "%d mates", e_cap.active ? e_cap.v : lv.scale_cap);
    if (heat_slider("scale cap", lv.scale_cap, 1, 64, e_cap, shown, &ap)) heat_set_live(HEAT_SCALE_CAP, ap);
    if (e_margin.active ? e_margin.v == 0 : lv.margin == 0) wsprintfA(shown, "off");
    else wsprintfA(shown, "%d%%", e_margin.active ? e_margin.v : lv.margin);
    if (heat_slider("margin %", lv.margin, 0, 200, e_margin, shown, &ap)) heat_set_live(HEAT_MARGIN, ap);
    ImGui::EndDisabled();
    const mh::heatui::snapshot &sn = mh::heatui::last_plan();
    if (sn.seq != 0)
        ImGui::TextWrapped("hits %u | last plan: p%u unit %u, %d mates, %d of %d tiles priced (cost %d%s), step %u", (unsigned)mh::heatui::hit_counter(), (unsigned)sn.player, (unsigned)sn.unit,
                           sn.mates, sn.priced, sn.tiles, sn.cost, sn.scale ? " x mates" : "", (unsigned)sn.step);
    else ImGui::Text("hits %u | no plan priced yet", (unsigned)mh::heatui::hit_counter());
    {
        uint32_t rt_plans = 0, rt_changed = 0, rt_refused = 0;
        mh::heatui::retrace_counts(&rt_plans, &rt_changed, &rt_refused);
        ImGui::Text("re-traced %u | changed %u | refused %u", (unsigned)rt_plans, (unsigned)rt_changed, (unsigned)rt_refused);
        uint32_t mg_checks = 0, mg_kept = 0;
        mh::heatui::margin_counts(&mg_checks, &mg_kept);
        ImGui::Text("margin checked %u | margin kept %u", (unsigned)mg_checks, (unsigned)mg_kept);
    }
    ImGui::TextUnformatted("heat map");
    ImGui::SameLine();
    ImGui::RadioButton("off", &g_heat_map, 0);
    ImGui::SameLine();
    ImGui::RadioButton("last plan", &g_heat_map, 1);
    ImGui::SameLine();
    ImGui::RadioButton("traffic", &g_heat_map, 2);
    if (g_heat_map == 1) ImGui::TextDisabled("yellow 1 .. red 4+ mates; cyan = route");
    else if (g_heat_map == 2) ImGui::TextDisabled("blue 1 .. red 4+ units, fading");
    if (g_heat_map != 0 && rd<uint8_t>(ADDR_GAME_MODE) != 2) ImGui::TextDisabled("(strategic map only)");
    // Once, for script authors: where the section's controls are, in client pixels.
    static bool s_logged = false;
    if (!s_logged && ImGui::GetItemRectMin().x > 0.0f) {
        s_logged = true;
        log("; [imgui] heat section drawn: locked=%d on=%d cost=%d window=%d mates=%d scale=%d retrace=%d turn=%d cap=%d margin=%d map=%d", locked ? 1 : 0, lv.on, lv.cost, lv.window, lv.min_mates, lv.scale, lv.retrace, lv.turn, lv.scale_cap, lv.margin, g_heat_map);
    }
}

void draw_panel() {
    const ImGuiIO &io = ImGui::GetIO();
    // M1WL v6: the window lives INSIDE the game image. Client pixels (16, 16) are the letterbox bar when the frame is centred (the image starts at x=282 or
    // 320 at ui_scale), where the cursor cannot go (it is clamped to the image) -- the user found the panel "created outside of screen area". So: the first
    // position is the image's top-left + 16, every frame the window is clamped back inside the image, and its height is capped to the image's (less 32)
    // so it scrolls. The image is small while the menus show (640x480): the CLAMP is what is drawn, the position the user chose (s_want) is kept apart and
    // restored when the image grows, and the cap never goes below 480 (the smallest image the game has), so the first-use auto-fit made at menu time is
    // not cut short by the menu-sized image.
    RECT img = {0, 0, (LONG)io.DisplaySize.x, (LONG)io.DisplaySize.y};
    if (!mh::gfx::owned_image_rect(&img)) img = RECT{0, 0, (LONG)io.DisplaySize.x, (LONG)io.DisplaySize.y};
    static ImVec2 s_want, s_left; // the position kept (what the user chose); where the window stood at the end of the last frame
    const float   cap = (float)(img.bottom - img.top) - 32.0f > 480.0f ? (float)(img.bottom - img.top) - 32.0f : 480.0f;
    if (ImGuiWindow *win = ImGui::FindWindowByName("mh debug")) {
        // A position that is not where this code left the window is the user's drag: keep it as the wish. The wish is clamped into the image every frame,
        // BEFORE Begin (a drag has already moved the window in NewFrame, so no out-of-image frame is ever drawn); the window is only moved when the clamp
        // differs, so the drag itself is never fought.
        if (win->Pos.x != s_left.x || win->Pos.y != s_left.y) s_want = win->Pos;
        float       px = s_want.x, py = s_want.y;
        const float xmax = (float)img.right - win->Size.x, ymax = (float)img.bottom - win->Size.y;
        px = px > xmax ? xmax : px;
        py = py > ymax ? ymax : py;
        px = px < (float)img.left ? (float)img.left : px;
        py = py < (float)img.top ? (float)img.top : py;
        if (px != win->Pos.x || py != win->Pos.y) ImGui::SetWindowPos(win, ImVec2(px, py), ImGuiCond_Always);
        s_left = ImVec2(px, py);
    } else {
        s_want = ImVec2((float)img.left + 16.0f, (float)img.top + 16.0f);
        ImGui::SetNextWindowPos(s_want, ImGuiCond_Always);
        s_left = s_want;
    }
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, cap));
    // NoResize: a resizable window is hovered up to style.WindowBorderHoverPadding (4 px) OUTSIDE its
    // drawn edge (the resize-grab band; the style asserts it > 0). The player judges "am I on the panel"
    // by the cursor they see, so hover -- and with it the swallow -- flips exactly at the drawn edge.
    const bool body = ImGui::Begin("mh debug", nullptr, ImGuiWindowFlags_NoResize);
    // Valid between Begin and End whether or not the window is collapsed (Size is the title bar then).
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    g_npanels             = 0;
    g_panels[g_npanels++] = {wp.x, wp.y, wp.x + ws.x, wp.y + ws.y};
    {
        static int s_last[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, s_wlog = 0;
        const int  now[8] = {(int)wp.x, (int)wp.y, (int)(wp.x + ws.x), (int)(wp.y + ws.y), (int)img.left, (int)img.top, (int)img.right, (int)img.bottom};
        if (s_wlog < 24 && memcmp(s_last, now, sizeof(now)) != 0) { // every move / resize of the window or the image (capped)
            memcpy(s_last, now, sizeof(now));
            ++s_wlog;
            log("; [imgui] window %d,%d..%d,%d image %d,%d..%d,%d", now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
        }
    }
    if (body) {
        ImGui::Text("%s closes  |  output %.0fx%.0f  |  %.1f fps", g_key_spec, io.DisplaySize.x, io.DisplaySize.y,
                    io.Framerate);
        ImGui::TextDisabled("swallowed msgs m=%ld k=%ld  drained DI m=%ld k=%ld", g_swallowed_mouse, g_swallowed_keys,
                            g_drained_mouse, g_drained_keys);
        draw_presenter_controls();
        ImGui::Separator();
        draw_heat_section();
        ImGui::Separator();
        // The same readouts the framebuffer debug overlay can page through ([debug] pages), every one
        // of them -- built-in first, then the families other seams registered (net.*).
        if (ImGui::BeginTable("readouts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            const int n = MH_Overlay_ProviderCount();
            for (int i = 0; i < n; ++i) {
                const char *name = MH_Overlay_ProviderName(i);
                if (!name) continue;
                char value[128];
                MH_Overlay_ProviderValue(i, value, sizeof(value));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(value);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void on_close() {
    // Anything buffered while capture was wanted must not reach the game after we let go.
    if (g_want_mouse) drain_mouse();
    if (g_want_kbd) drain_keyboard();
    g_want_mouse = g_want_kbd = false;
    g_btn_swallowed = g_btn_game = 0;
    g_npanels                    = 0; // a closed overlay has no panels: the rect rule is off
    if (g_ctx) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui::GetIO().ClearInputKeys(); // reopening must not see a key or button held from before
        ImGui::GetIO().ClearInputMouse();
    }
    log("; [imgui] overlay CLOSED -- swallowed mouse=%ld key=%ld msgs, drained DI mouse=%ld key=%ld events",
        g_swallowed_mouse, g_swallowed_keys, g_drained_mouse, g_drained_keys);
}

} // namespace

bool arm(const char *ini) {
    static bool done = false;
    if (done) return g_armed;
    done = true;
    if (mh_ini_get_int("video", "imgui", 0, ini) == 0) return false;
    char spec[32];
    mh::config::read_ini_string("video", "imgui_key", "F11", spec, sizeof(spec), ini);
    if (spec[0] && !parse_key(spec, &g_key)) {
        log("; [imgui] [video] imgui_key=%s not understood -- using F11", spec);
        lstrcpynA(spec, "F11", sizeof(spec));
        g_key = key_bind{};
    }
    if (!spec[0]) lstrcpynA(spec, "F11", sizeof(spec));
    lstrcpynA(g_key_spec, spec, sizeof(g_key_spec));
    g_open_at_start = mh_ini_get_int("video", "imgui_open_at_start", 0, ini) != 0;
    g_heat_map      = (int)mh_ini_get_int("video", "imgui_heat_map", 0, ini); // mods:M1WL v3: the heat map mode at start (0 off, 1 last plan, 2 traffic)
    if (g_heat_map < 0 || g_heat_map > 2) g_heat_map = 0;
    g_armed = true;
    log("; [imgui] ARMED: Dear ImGui overlay on the d3d11 presenter, toggle=%s (vk 0x%02x%s%s%s)", g_key_spec, g_key.vk,
        g_key.ctrl ? " +ctrl" : "", g_key.alt ? " +alt" : "", g_key.shift ? " +shift" : "");
    if (g_open_at_start) log("; [imgui] imgui_open_at_start=1 -- the overlay opens on the first present");
    return true;
}

void on_frame(ID3D11Device *device, ID3D11DeviceContext *context, HWND hwnd) {
    if (!g_armed || !device || !context || !hwnd) return;
    bind_window(hwnd);

    bool toggle = toggle_fired();
    if (g_open_at_start) {
        g_open_at_start = false;
        if (!g_open) toggle = true;
    }
    if (toggle) {
        g_open = !g_open;
        if (g_open) {
            ++g_opens;
            log("; [imgui] overlay OPEN (#%ld)", g_opens);
        } else {
            on_close();
        }
    }
    if (!g_open) return;

    if (!ensure_context(hwnd) || !ensure_dx(device, context)) {
        g_open = false;
        return;
    }

    di_filter_live(); // install on the first open frame (the DI mouse exists long before)
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // THE CURSOR: ImGui's position is the game's cursor, queued after the Win32 backend's own
    // GetCursorPos fallback so it is the one this frame ends on.
    {
        float cx, cy;
        if (game_cursor_client(&cx, &cy)) ImGui::GetIO().AddMousePosEvent(cx, cy);
    }
    // A hidden OS cursor means the game is drawing its own; give ImGui a visible one over its windows.
    CURSORINFO ci                  = {sizeof(ci)};
    const bool os_cursor_hidden    = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) == 0;
    ImGui::GetIO().MouseDrawCursor = os_cursor_hidden && g_want_mouse;
    ImGui::NewFrame();
    draw_heat_map(); // mods:M1WL v3: behind the panel, over the game frame; display only
    draw_panel();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const ImGuiIO &io        = ImGui::GetIO();
    const bool     was_mouse = g_want_mouse, was_kbd = g_want_kbd;
    g_want_mouse = io.WantCaptureMouse;
    g_mouse_pos  = io.MousePos;
    g_want_kbd   = io.WantCaptureKeyboard;
    { // M1WL v6 evidence: where ImGui's pointer is for a game cursor (tools/check_imgui_heat.py maps the game point through the logged layout independently)
        static int s_gx = -9999, s_gy = -9999, s_n = 0;
        const int  gx = rd<int>(ADDR_CURSOR_X), gy = rd<int>(ADDR_CURSOR_Y);
        if (s_n < 200 && (abs(gx - s_gx) >= 8 || abs(gy - s_gy) >= 8)) {
            ++s_n;
            s_gx = gx;
            s_gy = gy;
            log("; [imgui] cursor game %d,%d = client %d,%d (di %d,%d bound %d,%d)", gx, gy, (int)io.MousePos.x, (int)io.MousePos.y, rd<int>(ADDR_DI_LAST_X), rd<int>(ADDR_DI_LAST_Y), rd<int>(ADDR_DI_BOUND_W), rd<int>(ADDR_DI_BOUND_H));
        }
    }
    // Capture just ENDED: drop what DirectInput buffered since the last forwarded message (see INPUT).
    if (was_mouse && !g_want_mouse) drain_mouse();
    if (was_kbd && !g_want_kbd) drain_keyboard();
    // Transitions are the evidence a report needs ("was the click over the overlay?"); capped so a
    // pointer wandering in and out all session cannot turn the banner log into a firehose.
    static int s_logged = 0;
    if ((was_mouse != g_want_mouse || was_kbd != g_want_kbd) && s_logged < 64) {
        ++s_logged;
        log("; [imgui] capture mouse=%d key=%d at game cursor %d,%d = client %d,%d (panel %d,%d..%d,%d; swallowed "
            "m=%ld k=%ld, drained DI m=%ld k=%ld, DI filtered %ld)",
            g_want_mouse ? 1 : 0, g_want_kbd ? 1 : 0, rd<int>(ADDR_CURSOR_X), rd<int>(ADDR_CURSOR_Y), (int)io.MousePos.x,
            (int)io.MousePos.y, g_npanels ? (int)g_panels[0].x0 : -1, g_npanels ? (int)g_panels[0].y0 : -1,
            g_npanels ? (int)g_panels[0].x1 : -1, g_npanels ? (int)g_panels[0].y1 : -1, g_swallowed_mouse,
            g_swallowed_keys, g_drained_mouse, g_drained_keys, g_di_filtered);
    }
}

void on_detach() {
    if (!g_armed) return;
    release_window("presenter detach");
}

int query(query_what what) {
    switch (what) {
        case query_what::armed: return g_armed ? 1 : 0;
        case query_what::open: return g_open ? 1 : 0;
        case query_what::swallowed_mouse: return (int)g_swallowed_mouse;
        case query_what::swallowed_keys: return (int)g_swallowed_keys;
        case query_what::drained_mouse: return (int)g_drained_mouse;
        case query_what::rebinds: return (int)g_rebinds;
        case query_what::subclassed: return g_subclassed_n;
        case query_what::want_mouse: return g_open && g_want_mouse ? 1 : 0;
        case query_what::mouse_x: return (int)g_mouse_pos.x;
        case query_what::mouse_y: return (int)g_mouse_pos.y;
        case query_what::di_filtered: return (int)g_di_filtered;
    }
    return 0;
}

void on_device_release() {
    if (!g_dx_init) return;
    ImGui::SetCurrentContext(g_ctx);
    ImGui_ImplDX11_Shutdown();
    g_dx_init   = false;
    g_dx_device = nullptr;
    log("; [imgui] DX11 backend released with the presenter's device");
}

} // namespace mh::gfx::imgui_overlay
