//
// ui_drive.cpp -- UI automation harness Phase 2: drive the menu/dialog UI by
// injecting synthetic events into the game's OWN mouse ring, with no OS input.
//
// mh.exe latches mouse input into a 128-slot ring (_G_LLM_INPUT_MOUSE_EVENTS, stride 0x38) fed by the
// WndProc/DirectInput producers; llm_ui_widget_input_tick pops each event via llm_ui_mouse_poll_next_event,
// updates the cursor globals, and hit-tests the active widget list, activating whatever the cursor is over.
// We inject at that ring: write an event at _G_LLM_INPUT_MOUSE_WRITE_IDX and advance it &0x7f, exactly as
// the producers do. The game then does its real hit-test + activation -- so a synthetic click is
// indistinguishable from a physical one, and we don't reimplement any UI logic.
//
// To aim a click at a named widget we resolve its on-screen rect with the game's own layout helper
// (llm_ui_widget_layout_resolve_position), replicating input_tick's exact pre-hit-test sequence (seed the
// DRAW_X/Y/W/H scratch from the list origin, resolve the frame widgets, then resolve children) so the
// center we compute is the same rect the game will hit-test.
//
// Determinism-neutral (menu/UI layer only). Gated behind [uitest] in mh_net.ini. EN-only.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "include/mh_uidrive_export.h"
#include "include/mh_capture_export.h" // MH_Capture_Shot (named per-screen captures for the interpreter)
#include "include/mh_net_export.h"     // MH_Net_PeerCount (the `peers` predicate: all peers connected)
#include "include/mh_seam_export.h"    // MH_Seam_S8RetryArmed (the `retryready` predicate: S8 latch-clear fired)
#include "include/mh_harness_export.h" // MH_Harness_StepFence (the `simstep` predicate: the SIM's own step)
#include "include/mh_run_context.h"    // MH_RunDir
#include "addr/mh_addrs.gen.h"         // generated EN VAs
#include "state/region_runtime.h"      // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                       // where it IS, not where the binary put it
#include "addr/mh_structs.gen.h"       // generated game struct mirrors (mh::game::mh_llm_*)
#include "hook/watcall.h"              // call_watcall2 (resolve_position is __watcall)
#include "en_guard.h"                  // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / GetAsyncKeyState / GetPrivateProfileInt

namespace {

using mh::game::mh_llm_input_key_event;
using mh::game::mh_llm_input_mouse_event;
using mh::game::mh_llm_ui_widget;
using mh::game::mh_llm_ui_widget_list;

// mouse ring
constexpr uintptr_t EVENTS   = mh::addr::_G_LLM_INPUT_MOUSE_EVENTS;
constexpr uintptr_t WRITEIDX = mh::addr::_G_LLM_INPUT_MOUSE_WRITE_IDX;
constexpr uintptr_t READIDX  = mh::addr::_G_LLM_INPUT_MOUSE_READ_IDX;
// key ring -- the exact analogue (llm_input_key_event[128], same 0x38 stride, same &0x7f advance).
// Needed because several screens are reachable ONLY by keypress: the ESC/in-game menu and the
// enter-gameplay options screen are both opened from scancode branches in llm_strat_input_update, so a
// mouse-only driver cannot reach them at all.
// ---- TACT-REC: the VM mouse fix ----------------------------------------------------------------
// THE GAME HAS TWO MOUSE PRODUCERS AND THEY ARE MUTUALLY EXCLUSIVE. llm_input_wndproc_tap tests
// _G_LLM_DI_MOUSE_DEVICE at 0x004d11a8; non-null => it polls DirectInput and JUMPS PAST its entire
// WM_MOUSEMOVE/button switch. So the game runs on exactly one of two coordinate models:
//   * wndproc  -- x = lParam & 0xffff, y = lParam >> 16. TRUE ABSOLUTE client coordinates.
//   * DInput   -- buffered RAW RELATIVE counts, each divided by _G_LLM_INPUT_DI_MOUSE_DIVISOR
//                 (ship 1, i.e. no attenuation), DOUBLED when |delta| exceeds
//                 _G_LLM_INPUT_DI_MOUSE_ACCEL_THRESHOLD (ship 100), then accumulated into
//                 _G_LLM_INPUT_MOUSE_LAST_X/Y and clamped to the screen box.
//
// WHY THAT MAKES THE GAME UNPLAYABLE IN A VM (measured 2026-08-24, Hyper-V basic session, and it
// reproduces on STOCK RETAIL -- nothing this project added is involved). A hypervisor hands the guest
// an ABSOLUTE pointing device; the synthesised per-poll relative counts are large, trip the doubling,
// accumulate, and slam the cursor into the clamp. The symptom is "flies to an edge, jumps
// erratically" in BOTH the menu and in-game -- consistent with llm_input_mouse_delta_pump being
// literally the same code for strategic (0x00442a45) and tactical (0x0042a0c9).
//
// THE FIX IS TO PICK THE OTHER PRODUCER, not to tune this one: zero the device pointer and the tap
// falls through to its absolute-coordinate arm, which is exactly what an absolute pointer provides.
// Done EVERY FRAME rather than once, because llm_input_di_mouse_create runs on every display init
// (llm_gfx_display_init -> FUN_004d10bc), so a resolution change would undo a one-shot.
//
// SAFE, checked rather than assumed: all 18 references to the global live in four functions --
// llm_input_di_mouse_create, llm_input_dimouse_shutdown, llm_input_wndproc_tap and
// llm_input_di_mouse_poll -- and nothing dereferences it unconditionally. The one consequence is that
// llm_input_dimouse_shutdown then sees null and skips its Release, so the DI device leaks for the
// life of the process. Accepted: this is an opt-in knob for interactive/recording runs.
constexpr uintptr_t DI_MOUSE_DEVICE = mh::addr::_G_LLM_DI_MOUSE_DEVICE;
constexpr uintptr_t DI_MOUSE_ACCEL  = mh::addr::_G_LLM_INPUT_DI_MOUSE_ACCEL_THRESHOLD;
constexpr uintptr_t DI_MOUSE_DIV    = mh::addr::_G_LLM_INPUT_DI_MOUSE_DIVISOR;
// Traced state, one per suspect. LAST_X/Y is the DI path's own accumulator (the cursor position
// under DI); CURSOR_X/Y is what the game finally draws at; CURSOR_VISIBLE selects whether the pump
// WARPS the OS cursor each event or takes the event's absolute x/y.
constexpr uintptr_t MOUSE_LAST_X   = mh::addr::_G_LLM_INPUT_MOUSE_LAST_X;
constexpr uintptr_t MOUSE_LAST_Y   = mh::addr::_G_LLM_INPUT_MOUSE_LAST_Y;
constexpr uintptr_t CURSOR_VISIBLE = mh::addr::_G_LLM_CURSOR_VISIBLE;
constexpr uintptr_t CURSOR_XX      = mh::addr::_G_LLM_CURSOR_X;
constexpr uintptr_t CURSOR_YY      = mh::addr::_G_LLM_CURSOR_Y;

int  g_mouse_trace    = 0; // [input] mouse_trace=1 -> per-frame ring telemetry (diagnostic only)
int  g_mouse_absolute = 0; // [input] mouse_absolute=1 -> force the wndproc absolute path
int  g_mouse_div      = 0; // [input] mouse_div=N      -> override the DI divisor (0 = leave alone)
int  g_mouse_accel    = 0; // [input] mouse_accel=N    -> override the DI threshold (0 = leave alone)
bool g_mouse_logged   = false;


constexpr uintptr_t KEVENTS   = mh::addr::_G_LLM_INPUT_KEY_EVENTS;
constexpr uintptr_t KWRITEIDX = mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX;
constexpr uintptr_t KREADIDX  = mh::addr::_G_LLM_INPUT_KEY_READ_IDX;
// active widget list selectors (input_tick prefers the modal dialog, else the top-level menu list)
constexpr uintptr_t ACTIVE_DIALOG = mh::addr::_G_LLM_UI_ACTIVE_DIALOG;
constexpr uintptr_t MENU_LIST     = mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
// hit-test rect scratch, written by resolve_position and read by input_tick
constexpr uintptr_t DRAW_X      = mh::addr::_G_LLM_UI_WIDGET_DRAW_X;
constexpr uintptr_t DRAW_Y      = mh::addr::_G_LLM_UI_WIDGET_DRAW_Y;
constexpr uintptr_t DRAW_W      = mh::addr::_G_LLM_UI_WIDGET_DRAW_W;
constexpr uintptr_t DRAW_H      = mh::addr::_G_LLM_UI_WIDGET_DRAW_H;
constexpr uintptr_t RESOLVE_POS = mh::addr::llm_ui_widget_layout_resolve_position;

// llm_input_mouse_event.event_type bits (see mh_structs.gen.h)
enum { EV_MOVE  = 1,
       EV_LDOWN = 2,
       EV_LUP   = 4 };
constexpr uint32_t WIDGET_HIDDEN   = 0x80; // llm_ui_widget.flags: skipped by list_draw
constexpr uint32_t WIDGET_DISABLED = 0x40; // llm_ui_widget.flags: input_tick's hit-test skips it (greyed)
// A widget the harness may target: not hidden and not disabled -- exactly what a real click can land on
// (the game's own hit-test skips both). Lets click_label/value ignore a disabled decoy sharing a label
// (e.g. the lobby's disabled "Start" placeholder vs the real enabled Start action widget).
inline bool clickable(uint32_t flags) { return (flags & (WIDGET_HIDDEN | WIDGET_DISABLED)) == 0; }

uint32_t g_ts      = 0x10000; // synthetic monotonic timestamp; big gaps so poll never sees a double-click
bool     g_prev_f7 = false, g_prev_f8 = false;

// [uitest] config
bool g_enabled        = false;
char g_auto_label[64] = {0};
int  g_auto_value     = -1;  // click_value target (-1 = unset); the sprite-menu identifier
int  g_settle_frames  = 2;   // require the target present this many consecutive frames before auto-firing
int  g_settle_ms      = 120; // [uitest] settle_ms -- how long a widget center must hold still for `settled`
int  g_seen           = 0;
bool g_fired          = false;
bool g_dumped         = false; // one-shot: log the first non-empty active list (labels for authoring)

char g_log[MAX_PATH];
bool g_log_ready = false;

// Wall clock for the log, in ms since the first line. Every line carries it, because the frame counts
// the script already reports answer "how many frames did this wait" and not "how long did this TAKE" --
// and headless changes the frame rate, so the two are not convertible. A step that reads `waited 13422`
// says nothing about whether the run is slow; `+14.2s` does.
DWORD g_log_t0 = 0;

void ui_log(const char *fmt, ...) {
    if (!g_log_ready) {
        wsprintfA(g_log, "%smh_uidrive.log", MH_RunDir());
        g_log_ready = true;
        g_log_t0    = GetTickCount();
    }
    const DWORD ms = GetTickCount() - g_log_t0;
    char        line[512];
    // Prefix, not suffix, so a column of times reads down the page -- and deliberately BEFORE the "; "
    // the existing lines start with, which keeps every `"; [script] COMPLETE"` substring match working.
    wsprintfA(line, "[%3u.%03u] ", ms / 1000, ms % 1000);
    const int pre = lstrlenA(line);
    va_list   ap;
    va_start(ap, fmt);
    wvsprintfA(line + pre, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, line, lstrlenA(line), &w, nullptr);
    CloseHandle(h);
}

// Applied per frame from MH_UIDrive_OnPresent, BEFORE the [uitest] enable gate: a human playing the
// game interactively has uitest disabled, and they are exactly who needs this.
void apply_mouse_mode() {
    if (g_mouse_absolute) {
        volatile uint32_t *dev = (volatile uint32_t *)DI_MOUSE_DEVICE;
        if (*dev) {
            *dev = 0;
            if (!g_mouse_logged) {
                g_mouse_logged = true;
                ui_log("; [input] mouse_absolute=1 -- DI mouse device dropped; the wndproc "
                       "absolute-coordinate path is now the producer");
            }
        }
    }
    // The DI knobs are only meaningful while DI is still the producer, so they are independent of the
    // switch above rather than an else-branch -- setting both is a contradiction the log will show.
    // div is a SIGNED IDIV divisor: zero would fault, which is why 0 means "leave alone" and not "0".
    if (g_mouse_div > 0) *(volatile int *)DI_MOUSE_DIV = g_mouse_div;
    if (g_mouse_accel > 0) *(volatile int *)DI_MOUSE_ACCEL = g_mouse_accel;
}

// ---- [input] mouse_trace=1: the forensic artifact, because two guesses have already been wrong --
//
// The reported symptom is LAG, not sensitivity: motion continues in the old direction after the
// hand reverses, and does not drain while the mouse is still. That is a BACKLOG somewhere, and
// there are three candidates that reasoning about the listing cannot separate:
//   (a) the game's 128-slot event RING backs up -- but llm_input_mouse_delta_pump drains it to
//       EMPTY every frame (`while (!queue_is_empty()) pop`), so in-game it should not survive a
//       frame. The MENU consumer pops ONE per frame and can;
//   (b) the DIRECTINPUT device buffer backs up -- llm_input_di_mouse_poll runs from the WNDPROC
//       TAP, i.e. per window MESSAGE and not per frame, so if the guest stops sending messages the
//       buffered events are never collected at all. "Does not drain while still" fits this exactly;
//   (c) the ring OVERFLOWS and drops -- the enqueue guard drops the NEW event when full, keeping
//       the OLD ones, which is a stale-motion symptom rather than a dropped-motion one.
//
// So MEASURE rather than pick. Sampled once per present, which is a frame boundary the pump has
// already run at, and printed only when something changes so a still mouse costs one line.
void trace_mouse_ring() {
    static uint32_t s_prev_w  = 0xffffffffu;
    static uint32_t s_max_dep = 0;
    static uint32_t s_frame   = 0;
    static uint32_t s_quiet   = 0;
    const uint32_t  w         = *(volatile uint32_t *)WRITEIDX;
    const uint32_t  r         = *(volatile uint32_t *)READIDX;
    const uint32_t  depth     = (w - r) & 0x7f;
    const uint32_t  produced  = (s_prev_w == 0xffffffffu) ? 0u : ((w - s_prev_w) & 0x7f);
    ++s_frame;
    if (depth > s_max_dep) s_max_dep = depth;
    // A frame that produced nothing and holds nothing is the boring case; count them and print one
    // summary line when activity resumes, so the log stays readable during a real play session.
    // THE QUIET PATH MUST NOT SWALLOW A MOVING CURSOR. "It keeps moving after my hand stops" is the
    // whole report, so a frame with no new events but a CHANGING position is the most interesting
    // frame there is -- collapsing it into a quiet count would hide the answer.
    static int s_prev_lx = 0, s_prev_ly = 0, s_prev_cx = 0, s_prev_cy = 0;
    const int  lx = *(volatile int *)MOUSE_LAST_X, ly = *(volatile int *)MOUSE_LAST_Y;
    const int  cx = *(volatile int *)CURSOR_XX, cy = *(volatile int *)CURSOR_YY;
    const bool moved = (lx != s_prev_lx) || (ly != s_prev_ly) || (cx != s_prev_cx) || (cy != s_prev_cy);
    s_prev_lx = lx, s_prev_ly = ly, s_prev_cx = cx, s_prev_cy = cy;
    if (produced == 0 && depth == 0 && !moved) {
        ++s_quiet;
        s_prev_w = w; // update on the quiet path too, or `produced` lies after a still period
        return;
    }
    if (s_quiet) {
        ui_log("; [mtrace] %u quiet frame(s)", s_quiet);
        s_quiet = 0;
    }
    // ROUND 2 (2026-08-24): round 1 showed depth=0/max=0 and produced=2..6, i.e. the RING is not the
    // backlog and neither is the DI buffer -- and it also showed the sample point is blind, because
    // it is taken at present, AFTER the pump has drained. So trace the state that survives a frame:
    // the DI accumulator, the drawn cursor, and which pump arm is live.
    ui_log("; [mtrace] f=%u produced=%u depth=%u max=%u di=%s last=%d,%d cur=%d,%d vis=%d",
           s_frame, produced, depth, s_max_dep, *(volatile uint32_t *)DI_MOUSE_DEVICE ? "on" : "off",
           *(volatile int *)MOUSE_LAST_X, *(volatile int *)MOUSE_LAST_Y,
           *(volatile int *)CURSOR_XX, *(volatile int *)CURSOR_YY,
           (int)*(volatile uint32_t *)CURSOR_VISIBLE);
    s_prev_w = w;
}

mh_llm_ui_widget_list *active_list() {
    auto *dlg = *(mh_llm_ui_widget_list **)ACTIVE_DIALOG;
    return dlg ? dlg : *(mh_llm_ui_widget_list **)MENU_LIST;
}

// Push one event into the mouse ring, mirroring the producers (llm_input_di_mouse_poll /
// llm_input_wndproc_tap): write at WRITE_IDX, advance &0x7f, but only if the ring isn't full.
bool enqueue(uint32_t type, int x, int y) {
    uint32_t w    = *(volatile uint32_t *)WRITEIDX;
    uint32_t next = (w + 1) & 0x7f;
    if (next == *(volatile uint32_t *)READIDX) return false; // ring full -> drop (never happens in practice)
    auto *ev        = (mh_llm_input_mouse_event *)(EVENTS + (size_t)w * sizeof(mh_llm_input_mouse_event));
    ev->buttons     = (type == EV_LDOWN) ? 1u : 0u; // left-button bit; only the in-game delta pump reads this
    ev->event_type  = type;
    ev->dx          = 0;
    ev->dy          = 0;
    ev->wheel_delta = 0;
    ev->x           = (uint32_t)x;
    ev->y           = (uint32_t)y;
    ev->wheel_total = 0;
    ev->timestamp   = g_ts;
    g_ts += 0x4000;
    *(volatile uint32_t *)WRITEIDX = next;
    return true;
}

// Push one KEY event, mirroring llm_input_di_keyboard_poll / llm_input_wndproc_tap. `down` selects the
// event_type bit (0x100 down / 0x80 up); we do NOT set 0x200 (the DirectInput-sourced marker) because
// these events did not come from a DI poll -- llm_strat_input_update dispatches on the down/up bits and
// treats the raw-message form as first-class.
//
// The keystate ARRAY (_G_LLM_INPUT_KEYSTATE) is deliberately left alone: it is re-latched every frame
// from the real device by the DI poll, so a synthetic write there would be stomped immediately and
// would also make a held key look stuck. Every consumer we need reads the event ring.
bool enqueue_key(uint32_t scancode, bool down) {
    uint32_t w    = *(volatile uint32_t *)KWRITEIDX;
    uint32_t next = (w + 1) & 0x7f;
    if (next == *(volatile uint32_t *)KREADIDX) return false; // ring full -> drop
    auto *ev       = (mh_llm_input_key_event *)(KEVENTS + (size_t)w * sizeof(mh_llm_input_key_event));
    ev->scancode   = scancode;
    ev->event_type = down ? 0x100u : 0x80u;
    ev->timestamp  = g_ts;
    g_ts += 0x4000;
    *(volatile uint32_t *)KWRITEIDX = next;
    return true;
}

// Resolve `target`'s hit-test center, replicating llm_ui_widget_input_tick's pre-hit-test layout pass:
// seed the DRAW scratch from the list origin/size, resolve the frame widgets, then resolve children up to
// the target -- so the scratch state (which flow-layout widgets accumulate) matches the game's exactly.
bool widget_center(mh_llm_ui_widget_list *list, mh_llm_ui_widget *target, int *cx, int *cy) {
    if (!list || !target) return false;
    *(int *)DRAW_X = list->origin_x; // == input_tick's memcpy(DRAW_X, &list->origin_x, 0x10)
    *(int *)DRAW_Y = list->origin_y;
    *(int *)DRAW_W = list->width;
    *(int *)DRAW_H = list->height;
    for (mh_llm_ui_widget **wp = &list->frame; *wp; ++wp) // frame/background widgets first
        mh::hook::call_watcall2(RESOLVE_POS, *wp, list);
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp) {
        mh::hook::call_watcall2(RESOLVE_POS, *wp, list);
        if (*wp == target) {
            *cx = *(int *)DRAW_X + *(int *)DRAW_W / 2;
            *cy = *(int *)DRAW_Y + *(int *)DRAW_H / 2;
            return true;
        }
    }
    return false;
}

// Read a widget label into `out` as ASCII, transparently handling the UTF-16 text the dialogs use
// (cfg::G_TEXT_PTRS is a wide pool: "Ok"/"Cancel"/... are 2 bytes/char). Heuristic: a non-empty Latin
// string whose 2nd byte is 0 is UTF-16 (take every other byte); otherwise read it as ASCII. This makes
// the sprite-less text dialogs (name screen, save/load, options) matchable by their real labels.
void extract_label(const char *p, char *out, int cap) {
    out[0] = 0;
    if (!p) return;
    int n = 0;
    if (p[0] != 0 && p[1] == 0) { // wide
        for (const uint16_t *w = (const uint16_t *)p; *w && n < cap - 1; ++w)
            out[n++] = (*w < 128) ? (char)*w : '?';
    } else { // ascii
        while (p[n] && n < cap - 1) {
            out[n] = p[n];
            ++n;
        }
    }
    out[n] = 0;
}

bool label_matches(const char *label, const char *substr) {
    if (!label || !substr || !*substr) return false;
    char buf[80];
    extract_label(label, buf, sizeof(buf));
    for (const char *base = buf; *base; ++base) {
        const char *a = base, *b = substr;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (!*b) return true; // consumed all of substr
    }
    return false;
}

// A WIDGET'S CLICKABLE SET IS NOT ITS RECTANGLE when it carries a stencil, and targeting the
// rectangle's centre is how a click silently does nothing.
//
// llm_ui_widget_input_tick's hit test, after the usual rect bounds check:
//     (w->flags & 0x100) == 0
//  || _G_LLM_UI_MENU_MASK_BITMAPS[w->hit_mask_idx] == NULL
//  || mask[i >> 3] & (1 << (i & 7))     with i = dx + DRAW_W*dy
// So `flags & 0x100` means "hit-test against a 1bpp stencil", and a clear bit is a MISS even though
// the cursor is inside the rect. `hit_mask_idx` selects one of the 12 MENUMASK*.GFX stencils
// llm_ui_main_menu_screen_load builds.
//
// MEASURED 2026-09-07, and it is not an edge case. The new-game race picker's two buttons are
// byte-identical widgets -- same rect (160,72) 304x208, same action_cb (llm_menu_race_select_cb),
// same flags -- differing ONLY in `value` ('h'/'a') and `hit_mask_idx` (7 = RACEHM.GFX, 8 =
// RACEAM.GFX). Their shared rect centre (312,176) is transparent in both stencils, so `clickv 104`,
// `clickv 97` and a press/release pair at the recorded human coordinates ALL did nothing, and the
// game's own hovered-widget global stayed null while the cursor sat inside the rectangle. The whole
// main menu is stencil-tested too (mask 0); it worked only because a 219x30 text button's centre
// happens to be opaque -- luck, not design.
//
// So resolve a point the widget actually OWNS: the set pixel nearest the rect centre. Deterministic
// (single scan, strict <, so ties resolve by scan order), and unchanged for a widget with no stencil.
bool widget_hit_point(mh_llm_ui_widget_list *list, mh_llm_ui_widget *w, int *cx, int *cy) {
    if (!widget_center(list, w, cx, cy)) return false;
    if ((w->flags & 0x100) == 0) return true;
    if (w->hit_mask_idx < 0 || w->hit_mask_idx >= 12) return true;
    const uint8_t *const *masks = (const uint8_t *const *)mh::addr::_G_LLM_UI_MENU_MASK_BITMAPS;
    const uint8_t        *mask  = masks[w->hit_mask_idx];
    // NULL until llm_ui_main_menu_screen_load has run; the game treats that as "no stencil", so the
    // rect centre is then the right answer rather than a fallback.
    if (!mask) return true;
    // widget_center left the DRAW scratch on this widget -- the same values the game's hit test uses.
    const int rx = *(int *)DRAW_X, ry = *(int *)DRAW_Y;
    const int rw = *(int *)DRAW_W, rh = *(int *)DRAW_H;
    if (rw <= 0 || rh <= 0) return true;
    auto owned = [&](int dx, int dy) {
        const int i = dx + rw * dy;
        return (mask[i >> 3] >> (i & 7)) & 1;
    };
    const int mx = rw / 2, my = rh / 2;
    if (owned(mx, my)) return true; // the centre is already ours -- the common case, and free
    int  best_dx = -1, best_dy = -1;
    long best_d2 = 0;
    for (int dy = 0; dy < rh; ++dy)
        for (int dx = 0; dx < rw; ++dx) {
            if (!owned(dx, dy)) continue;
            const long ex = dx - mx, ey = dy - my;
            const long d2 = ex * ex + ey * ey;
            if (best_dx < 0 || d2 < best_d2) {
                best_d2 = d2;
                best_dx = dx;
                best_dy = dy;
            }
        }
    if (best_dx < 0) {
        // An all-clear stencil is not a miss to paper over: the widget is unclickable by
        // construction and every later "the click did nothing" would be this, unsaid.
        ui_log("; hit-mask %d for widget %p is EMPTY over its %dx%d rect -- nothing to click",
               w->hit_mask_idx, (void *)w, rw, rh);
        return true;
    }
    ui_log("; hit-mask %d moved the target of widget %p from (%d,%d) to (%d,%d)", w->hit_mask_idx,
           (void *)w, rx + mx, ry + my, rx + best_dx, ry + best_dy);
    *cx = rx + best_dx;
    *cy = ry + best_dy;
    return true;
}

bool click_center_of(mh_llm_ui_widget_list *list, mh_llm_ui_widget *w, const char *why) {
    int cx = 0, cy = 0;
    if (!widget_hit_point(list, w, &cx, &cy)) {
        ui_log("; click FAILED (%s): could not resolve widget %p center", why, (void *)w);
        return false;
    }
    MH_UIDrive_Click(cx, cy);
    ui_log("; click %s -> widget %p @ (%d,%d) label='%s'", why, (void *)w, cx, cy,
           w->label ? w->label : "(none)");
    return true;
}

// D15 (2026-08-06) -- the cause of the UI suite's "flakiness", and where it is fixed.
//
// A click resolves only CLICKABLE widgets (clickable() excludes WIDGET_DISABLED); every WAIT resolves
// merely VISIBLE ones. So `settled Create` could go stable on a button that was still greyed, and the
// very next line's `clickl Create` then matched nothing -- in the SAME FRAME, which reads as
// impossible and is why this went undiagnosed through two sessions.
//
// The window is real, generous, and reproducible on an IDLE box: measured with a probe script, after
// the player-name confirm transitions to the local-games browser, `Create` is present-and-DISABLED
// for ~400 ms before it goes live, while `settled` needs only 120 ms of stillness -- which a greyed
// stationary button satisfies trivially. EVERY host-side script in tools/uiscripts/ contains
// `settled Create` / `clickl Create`, which is exactly why the failing SET moved from run to run
// while the fact of failure stayed: whichever test sampled the window lost that run.
//
// WHY THE FIX IS HERE AND NOT IN `settled`. Making the waits clickable-filtered was tried first and
// is WRONG: `settled` is also used as a pure animation probe on a deliberately greyed widget
// (esc_menu.txt rides out the lobby slide-in with `settled Start` while Start is correctly disabled,
// adds a computer player, and only then gates on `enabled Start`). That change hung esc_menu for
// 80001 frames. The two uses of `settled` are both legitimate; what was never legitimate is a click
// treating "not ready yet" and "not there at all" as the same outcome.
//
// So the CLICK distinguishes them:
//   CLICK_OK      clicked it.
//   CLICK_RETRY   the label IS on this screen but every match is greyed -- a TIMING condition. Stay
//                 on this step and try again next frame, bounded by the step's own frame watchdog,
//                 exactly as a wait would be. This is what removes the whole failure class, without
//                 editing 40 scripts and without a script author having to remember `enabled` first.
//   CLICK_ABSENT  nothing on this screen carries the label at all -- a real script/screen error, so
//                 fail FAST and say so at the offending step.
enum click_result { CLICK_OK,
                    CLICK_RETRY,
                    CLICK_ABSENT };

// Shared by the label and value forms: scan once, and report which of the three outcomes it was.
template <typename Pred>
click_result try_click(Pred match, const char *why) {
    mh_llm_ui_widget_list *list = active_list();
    if (!list) return CLICK_ABSENT;
    bool present_but_greyed = false;
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp) {
        mh_llm_ui_widget *w = *wp;
        if (!match(w)) continue;
        if (clickable(w->flags)) return click_center_of(list, w, why) ? CLICK_OK : CLICK_ABSENT;
        if ((w->flags & WIDGET_HIDDEN) == 0) present_but_greyed = true;
    }
    return present_but_greyed ? CLICK_RETRY : CLICK_ABSENT;
}

} // namespace

extern "C" void MH_Seam_InjectStartReceived(void); // net_discovery -- U29 (b) test hook
extern "C" void MH_UIDrive_CursorTo(int x, int y) { enqueue(EV_MOVE, x, y); }

// UI-REC: the active screen's identity, for the input journal's barrier records. Same expression the
// OP_W_SCREEN predicate compares, so a journal barrier and a script `screen` wait mean the same thing.
extern "C" unsigned MH_UIDrive_ActiveScreen(void) {
    return (unsigned)(uintptr_t)active_list();
}

// UI-REC: is the active screen geometrically STILL, by the same wall-clock rule OP_W_SETTLED uses?
//
// The journal replay needs this for the reason the OP_W_SETTLED comment states and the input journal
// had to learn: a panel slides in over a WALL-CLOCK duration, so "wait the number of presents the
// recording waited" is only correct at the rate the recording ran at. A replay runs headless at a
// different rate, reaches the click's present index sooner in real time, and clicks into a panel that
// is still moving. Gating on stillness is rate-independent; gating on a present count is not.
//
// Sampled in MH_UIDrive_OnPresent (every present, whether or not anything is asking) so the answer
// does not depend on how often it is polled.
namespace {
unsigned g_scr_sig      = 0; // geometry signature of the active list, last present
unsigned g_scr_still_at = 0; // settle_now() when it last CHANGED
unsigned g_scr_last     = 0; // which screen that signature belongs to
unsigned g_scr_id       = 0; // CONTENT identity of the active list, last present (screen_id_now below)
unsigned g_scr_id_at    = 0; // settle_now() when THAT last changed

// G146: WHICH CLOCK THE SCREEN DWELL IS MEASURED IN, and it is the game's, not the wall's, whenever
// the harness has pinned one. Armed by MH_UIDrive_SetSettleClockPinned from the harness.
//
// The dwell used GetTickCount unconditionally, with the reasoning that measuring stillness in presents
// would make the PREDICATE rate-dependent. That is right about the predicate and wrong about the cost:
// the wait's length IN PRESENTS is then rate-dependent by construction, so at 60 fps a 120 ms dwell is
// ~7 presents and uncapped it is ~24 -- and a journal replay is denominated in presents throughout.
// Every barrier therefore spent a frame-rate-dependent number of presents, which is how the menu's
// frame rate leaked into the sim.
//
// Reading the pinned clock instead keeps BOTH properties. It is still a duration, so a screen that
// takes real game-time to settle is still waited out rather than counted in frames; and because the
// pinned clock advances a fixed dt per present, that duration is the SAME number of presents at any
// frame rate.
//
// THE SOURCE IS THE HARNESS'S OWN COUNTER, NOT `_G_LLM_TIME_TICKS_MS`, and that distinction hung a
// replay before it was made. The game global looks like the obvious source -- under pin_menu_clock it
// holds the pinned value -- but it is only WRITTEN when the game CALLS llm_time_get_ticks_ms. The menu
// calls it every frame; in-game nothing on that path runs, so the global stalls at its last menu value
// while the pinned clock keeps advancing. A dwell reading it then never elapses: measured, both arms
// held at the in-game barrier (record 308) forever, identically, which is a stall wearing determinism's
// clothes. The harness's counter advances on BOTH cadences by construction, so it cannot stall.
unsigned (*g_settle_clock)(void) = nullptr;

unsigned settle_now() { return g_settle_clock ? g_settle_clock() : GetTickCount(); }

unsigned screen_id_now(); // defined below, next to the long comment that explains why it exists

void screen_settle_tick() {
    mh_llm_ui_widget_list *list = active_list();
    unsigned               sig  = 0;
    if (list) {
        for (mh_llm_ui_widget **wp = list->children; *wp; ++wp) {
            int cx = 0, cy = 0;
            if (!widget_center(list, *wp, &cx, &cy)) continue;
            sig = sig * 31u + (unsigned)(cx * 4096 + cy);
        }
    }
    const unsigned scr = (unsigned)(uintptr_t)list;
    const unsigned now = settle_now();
    if (sig != g_scr_sig || scr != g_scr_last) {
        g_scr_sig      = sig;
        g_scr_last     = scr;
        g_scr_still_at = now;
    }
    const unsigned id = screen_id_now();
    if (id != g_scr_id) {
        g_scr_id    = id;
        g_scr_id_at = now;
    }
}
} // namespace

// UI-REC: a CONTENT signature of the active screen -- what is on it, not which container holds it.
//
// The journal's barriers first used active_list() itself, and that identity cannot do the job: it is
// `*ACTIVE_DIALOG ?: *MENU_LIST`, so every screen with no dialog open shares ONE value. On a real
// recorded session that meant the race picker and the main menu were the same "screen", 10 of 17
// barriers could not be confirmed, and two arms replaying the SAME journal forced different barriers
// and played different games -- an A/B that compared nothing.
//
// The signature is the widget geometry already accumulated for the settle test, combined with the
// container. Two different screens agree only if they have identical widget counts AND identical
// centers, which is far stronger than sharing a container pointer.
extern "C" unsigned MH_UIDrive_ScreenSig(void) {
    return g_scr_sig * 31u + (unsigned)(uintptr_t)active_list();
}

// UI-REC / SPCAMP-SYNC: the screen identity a JOURNAL BARRIER matches on, and it deliberately shares
// nothing with the two functions above.
//
// MH_UIDrive_ScreenSig is GEOMETRY, and geometry cannot identify a screen -- it identifies a MOMENT.
// Measured 2026-09-07 on the campaign race picker: one of its children walks (500,22) -> (320,34) over
// about eight seconds, so the geometry signature is a different number on every present and the
// recorded one names an instant no replay will ever be standing on. The same motion also keeps
// MH_UIDrive_ScreenSettled permanently false there, and a barrier requires BOTH -- so every barrier on
// a screen with any moving decoration was structurally guaranteed to be FORCED, whatever the identity.
// That is the residue the content signature did not explain: 13 of 37 on session 2.
//
// So identify a screen by WHAT IS ON IT: the child count, and per child the builder-assigned `value`
// (the sprite-menu/hotkey id), `disp_idx` (the label-table or sprite index), `flags` (which carries the
// hidden bit, so a widget appearing or disappearing counts) and the label text. Every one of those is
// written by the dialog BUILDER and is invariant while the panel travels. x/y/width/height are
// excluded on purpose -- width and height are overwritten at DRAW time with the rendered extent, which
// is exactly why the geometry hash moves.
namespace {
unsigned screen_id_now() {
    mh_llm_ui_widget_list *list = active_list();
    unsigned               id   = (unsigned)(uintptr_t)list;
    unsigned               n    = 0;
    if (list) {
        for (mh_llm_ui_widget **wp = list->children; *wp; ++wp) {
            const mh_llm_ui_widget *w = *wp;
            id                        = id * 31u + (unsigned)w->value;
            id                        = id * 31u + (unsigned)w->disp_idx;
            // FLAGS MINUS THE HIGHLIGHT BITS. 0x18000 gates the highlight-frame overlay, so the
            // game sets it on whichever widget the cursor is over -- which makes an identity built
            // on raw `flags` a function of the MOUSE POSITION. Measured 2026-09-07: the campaign
            // race picker hashed to two different ids depending on whether Human was hovered, the
            // recorder emitted a barrier for the hovered one, and the replay then waited forever
            // for a screen state that only a hover could produce while the records that would
            // produce it queued behind that barrier. Same deadlock shape as a barrier inside a
            // click, one level up. A screen's IDENTITY may not depend on where the pointer is.
            id = id * 31u + (w->flags & ~0x18000u);
            // ASCII run only, capped. A wide (UTF-16) label hashes as its first character, which is
            // weaker but still deterministic -- and value/disp_idx/count already carry the
            // discrimination. Reading past a 64-byte cap is the risk this bound exists to refuse.
            if (w->label)
                for (const char *p = w->label; *p && p - w->label < 64; ++p)
                    id = id * 31u + (unsigned char)*p;
            ++n;
        }
    }
    return id * 31u + n;
}
} // namespace

extern "C" unsigned MH_UIDrive_ScreenId(void) { return g_scr_id; }

// The barrier's settle test. Same wall-clock dwell rule as OP_W_SETTLED, but over the IDENTITY rather
// than the geometry, so a screen whose decoration never stops moving still settles the moment its
// content stops changing -- which is the thing a barrier is actually waiting for.
extern "C" int MH_UIDrive_ScreenIdSettled(void) {
    return (int)(settle_now() - g_scr_id_at) >= (g_settle_ms > 0 ? g_settle_ms : 120);
}

// G146: the harness arms this when it pins the game clock, so the dwell above is measured in the same
// clock the game runs on. See settle_now(). Only the two SCREEN predicates move -- OP_W_SETTLED, the
// script `settled <target>` wait, keeps GetTickCount: different consumer, not implicated, and the 18
// committed UI scenarios are gated on it.
extern "C" void MH_UIDrive_SetSettleClock(unsigned (*fn)(void)) { g_settle_clock = fn; }

extern "C" int MH_UIDrive_ScreenSettled(void) {
    // Same clock as the identity dwell above -- see settle_now(). The note that used to stand here
    // ("GetTickCount, not the game's clock ... would reintroduce exactly the rate dependence the
    // predicate exists to remove") had it backwards: the pinned clock advances a fixed dt per PRESENT,
    // so measuring in it makes the dwell a constant number of presents at any frame rate, which is the
    // rate-independence wanted. It is the WALL clock that made the wait rate-dependent.
    return (int)(settle_now() - g_scr_still_at) >= (g_settle_ms > 0 ? g_settle_ms : 120);
}

extern "C" void MH_UIDrive_Click(int x, int y) {
    enqueue(EV_MOVE, x, y);
    enqueue(EV_LDOWN, x, y);
    enqueue(EV_LUP, x, y);
}

// Button widgets hit-test the click position directly, so a same-frame MOVE+DOWN+UP (MH_UIDrive_Click)
// activates them fine. A scrollable LIST row instead commits on the HOVERED row, which the game only
// re-derives at render time -- so a same-frame click commits a stale hover (the row selection is flaky /
// wrong). press/release let a script spread a click across FRAMES (cursor -> a render settles the hover ->
// press -> release), exactly as a physical click arrives, which the game selects reliably.
extern "C" void MH_UIDrive_Press(int x, int y) { enqueue(EV_LDOWN, x, y); }
extern "C" void MH_UIDrive_Release(int x, int y) { enqueue(EV_LUP, x, y); }

extern "C" int MH_UIDrive_ClickWidget(int idx) {
    mh_llm_ui_widget_list *list = active_list();
    if (!list) return 0;
    int i = 0;
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp, ++i)
        if (i == idx) return click_center_of(list, *wp, "click_widget") ? 1 : 0;
    ui_log("; click_widget(%d) out of range (%d children)", idx, i);
    return 0;
}

// D15: when a click finds nothing, say WHY. "no clickable widget matched" has two completely
// different causes -- the label is absent from this screen, or it is PRESENT and merely
// disabled/hidden -- and they send you to opposite places. Worse, the second one is how a script
// dies immediately after its own `settled <label>` wait SUCCEEDED, which reads as impossible and
// cost a session (2026-08-06: `settled Create` ok and `clickl Create` failed in the same frame).
// Report every label match with its flags so the next reader gets the answer from the log.
static void log_click_label_rejects(mh_llm_ui_widget_list *list, const char *substr) {
    int matched = 0;
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp) {
        mh_llm_ui_widget *w = *wp;
        if (!label_matches(w->label, substr)) continue;
        ++matched;
        ui_log("; click_label:   label match %p flags=0x%X%s%s label='%s'", (void *)w,
               (unsigned)w->flags, (w->flags & WIDGET_DISABLED) ? " DISABLED" : "",
               (w->flags & WIDGET_HIDDEN) ? " HIDDEN" : "", w->label ? w->label : "(none)");
    }
    if (!matched)
        ui_log("; click_label:   no widget on this screen carries that label at all");
}

// The tri-state forms the script interpreter uses. The extern "C" wrappers below keep the historical
// 0/1 contract for the interactive F8 path, which has no step to retry on.
static click_result click_label_try(const char *substr) {
    return try_click([&](mh_llm_ui_widget *w) { return label_matches(w->label, substr); },
                     "click_label");
}

static click_result click_value_try(int value) {
    return try_click([&](mh_llm_ui_widget *w) { return w->value == value; }, "click_value");
}

extern "C" int MH_UIDrive_ClickLabel(const char *substr) {
    mh_llm_ui_widget_list *list = active_list();
    if (!list) {
        ui_log("; click_label('%s') FAILED: no active widget list", substr ? substr : "");
        return 0;
    }
    click_result r = click_label_try(substr);
    if (r == CLICK_OK) return 1;
    ui_log("; click_label('%s') FAILED: %s", substr ? substr : "",
           r == CLICK_RETRY ? "matched only DISABLED widget(s)" : "no clickable widget matched");
    log_click_label_rejects(list, substr);
    return 0;
}

extern "C" int MH_UIDrive_ClickValue(int value) {
    mh_llm_ui_widget_list *list = active_list();
    if (!list) {
        ui_log("; click_value(%d) FAILED: no active widget list", value);
        return 0;
    }
    click_result r = click_value_try(value);
    if (r == CLICK_OK) return 1;
    ui_log("; click_value(%d) FAILED: %s", value,
           r == CLICK_RETRY ? "matched only DISABLED widget(s)" : "no clickable widget matched");
    return 0;
}

extern "C" void MH_UIDrive_DumpWidgets(void) {
    mh_llm_ui_widget_list *list   = active_list();
    bool                   is_dlg = (*(mh_llm_ui_widget_list **)ACTIVE_DIALOG) != nullptr;
    if (!list) {
        ui_log("; dump: no active widget list");
        return;
    }
    ui_log("; --- active %s list %p: origin=(%d,%d) size=(%d,%d) ---", is_dlg ? "DIALOG" : "MENU",
           (void *)list, list->origin_x, list->origin_y, list->width, list->height);
    // The state llm_ui_widget_input_tick's own gates read, because "the click did nothing" has
    // several causes and a widget dump distinguishes none of them. dlg_flags bit1 (0x02) gates the
    // WHOLE mouse-event loop -- with it clear the hit test never runs, so nothing hovers and no
    // stencil is ever consulted; input_lock counts down before an activation is honoured.
    ui_log(";   state: dlg_flags=0x%02x (mouse loop %s) input_lock=%d event_kind=%d hovered=%p "
           "selected=%p cursor=(%d,%d)",
           (unsigned)*(volatile uint8_t *)mh::addr::_G_LLM_DLG_STATE_FLAGS,
           (*(volatile uint8_t *)mh::addr::_G_LLM_DLG_STATE_FLAGS & 0x02) ? "ARMED" : "*** OFF ***",
           *(volatile int *)mh::addr::_G_LLM_UI_MENU_INPUT_LOCK_TIMER,
           (int)*(volatile uint8_t *)mh::addr::_G_LLM_UI_MOUSE_EVENT_KIND,
           *(void **)mh::addr::_G_LLM_UI_WIDGET_HOVERED, *(void **)mh::addr::_G_LLM_UI_WIDGET_SELECTED,
           *(volatile int *)CURSOR_XX, *(volatile int *)CURSOR_YY);
    int i = 0;
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp, ++i) {
        mh_llm_ui_widget *w  = *wp;
        int               cx = 0, cy = 0, hx = 0, hy = 0;
        widget_center(list, w, &cx, &cy);
        // Both points, because on a stencil widget they differ and the CENTRE is the one that lies.
        // A dump is read to pick a click target; printing only the rect centre is how an author
        // concludes a widget is at a point no click of it will ever land on.
        widget_hit_point(list, w, &hx, &hy);
        char lbl[80];
        extract_label(w->label, lbl, sizeof(lbl));
        if (hx == cx && hy == cy)
            ui_log(";   [%d] flags=0x%08x center=(%d,%d) value=%d label='%s'", i, w->flags, cx, cy,
                   w->value, w->label ? lbl : "(none)");
        else
            ui_log(";   [%d] flags=0x%08x center=(%d,%d) HIT=(%d,%d) mask=%d value=%d label='%s'", i,
                   w->flags, cx, cy, hx, hy, w->hit_mask_idx, w->value, w->label ? lbl : "(none)");
    }
    ui_log(";   (%d widgets)", i);
}

static bool have_auto_target() { return g_auto_value >= 0 || g_auto_label[0]; }

// Is the configured auto-target (by value, else by label) a visible widget in the active list now?
static bool auto_target_present() {
    mh_llm_ui_widget_list *list = active_list();
    if (!list) return false;
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp) {
        if (((*wp)->flags & WIDGET_HIDDEN) != 0) continue;
        if (g_auto_value >= 0) {
            if ((*wp)->value == g_auto_value) return true;
        } else if (label_matches((*wp)->label, g_auto_label)) {
            return true;
        }
    }
    return false;
}

static int fire_auto_target() {
    return g_auto_value >= 0 ? MH_UIDrive_ClickValue(g_auto_value) : MH_UIDrive_ClickLabel(g_auto_label);
}

// ===================================================================================================
// UI-state script interpreter (Phase 3) -- the UI-input notes.
//
// A script is a text file of one directive per line, run SEQUENTIALLY. WAIT directives block until
// their UI-STATE predicate holds; ACTION directives fire once and advance. There are NO frame/time
// predicates -- a step fires on a screen-state transition, so a menu walk replays identically on a fast
// host and a slow VM. Grammar (first token = opcode; '#' or ';' line = comment):
//   WAITS   screen <hex>   present <target>   absent <target>   onscreen <target>   settled <target>
//           enabled <target>   hovered <target>   gamemode <N>   tactsel <N>
//     hovered  = THE GAME'S OWN HIT TEST says the cursor is over <target>. It re-injects the move
//                itself, so it is the "this screen is really LIVE" gate: every other predicate reads
//                the active widget LIST, which a menu activation swaps to the new screen instantly
//                while the old frame is still on the glass. Use it before clicking anything on a
//                screen you have just navigated to. See the OP_W_HOVERED comment.
//     onscreen = present AND the widget's resolved center is inside the window.
//     tactsel <N> = exactly N tactical units are SELECTED, per the game's own recount. The only
//                selection predicate that works in tactical mode (there is no widget list there),
//                and the only one that makes a tactical capture non-vacuous: the counter is written
//                by llm_tact_active_unit_count_hud_draw alone, so waiting on it is waiting for the
//                panel-refresh path to have RUN, not merely for a frame to have been drawn.
//     settled  = present AND that center is UNCHANGED since last frame -- i.e. the slide-in animation
//                has stopped. This is the robust "ready to click an animated panel" predicate (a click
//                mid-slide lands where the widget WAS, and misses); state-based, so speed-independent.
//     screensettled = the WHOLE active screen is geometrically still (every child's resolved center
//                unchanged for settle_ms). `settled <target>` needs a target that is (a) resolvable and
//                (b) the thing that moves; the campaign race picker satisfies neither -- its two race
//                widgets resolve to ONE identical center that never moves, while the decoration that IS
//                sliding carries no label and no value, so there is nothing to name. Measured
//                2026-09-07: a click fired 1 ms after `screen 6634567` matched did nothing at all, and
//                the game's own hover global reported NO widget under the cursor, because the panel was
//                still travelling (its one moving child walked 500,22 -> 320,34 over ~8 s). This is
//                `settled`'s rule -- a wall-clock dwell, not a frame count -- applied to the screen
//                instead of to one widget, for the screens where no single widget can stand for it.
//     sessions <N> = _G_LLM_NET_SESSION_COUNT >= N (a MP session-browser wait: the client blocks here
//                until a host is discovered, exactly the gate llm_lobby_join_head checks before joining).
//     peers <N> = MH_Net_PeerCount() >= N (transport-connected peers). The host blocks here until all N
//                clients have joined the lobby, THEN clicks Start to launch the match (e.g. `peers 1` for
//                a 2-player game, `peers 2` for 3-player). Composes with any action -- the general
//                "do X once the session has N peers" gate.
//     occ <N> = >= N OCCUPIED lobby slots on THIS peer's view (_G_LLM_LOBBY_SLOTS status HUMAN|AI, bounded
//                by the map player count). The deterministic "a player is really SEATED (and thus rendered)
//                here" gate -- use it to gate a lobby capture (e.g. `occ 2` before capturing the joined
//                lobby) where `settled <button>` goes stable frames before the slot snapshot arrives.
//     pokerace <SLOT> <V> = ACTION, test hook: write slot SLOT's race field on THIS peer only, WITHOUT
//                the 0x0c push the real spinner sends. Manufactures the end-state of an edit the host
//                never saw, deterministically -- which is how U28's "a client slot change in flight at
//                Start is lost" is testable without racing the Start click.
//     injectstart = ACTION, test hook: latch this client's "host pressed Start" flag as a BARE retail
//                FLAG_START would. For U29 (b): with the client sitting on the discovery browser after a
//                host-left, entry MUST NOT happen. Assert the no-op by following it with a predicate
//                that only holds ON THE BROWSER and takes real time to become true (`sessions 1`) --
//                never a frame or clock wait. If entry had fired, those predicates time out.
//     race <SLOT> <V> = lobby slot SLOT's race field (_G_LLM_LOBBY_SLOTS + 0x05) equals V, on THIS peer's
//                view. 1 = Human, 2 = Alien (the values the lobby's own race spinner cycles). Added for
//                U28: it makes SLOT-STATE PROPAGATION the gate instead of a timing proxy. A client that
//                changed its own race gates on its own row; the HOST gates on the CLIENT's row, so
//                "the client's push arrived and was applied here" is a predicate rather than a hope --
//                and if the U28 payload/push path ever regresses the host TIMES OUT instead of quietly
//                starting a match the two peers disagree about. There is no time in it, so it replays
//                identically at any frame rate.
//     awaitsignal <name> / signal <name> = a PEER RENDEZVOUS, ferried by the runner. One peer runs
//                `signal lobby`; ui_test sees the marker in its log and drops rig_lobby.flag into every
//                OTHER peer's run dir; their `awaitsignal lobby` then passes. Use it when the fact one
//                peer must wait on is only observable on the OTHER peer -- "the client's UI has reached
//                its lobby" is the case it was built for: the host's earliest protocol-level observable
//                is the JOIN admit, which is ~a second too early, and clicking Start there gives the
//                client ~2 frames of lobby (measured 2026-07-28). Still a state the rig OBSERVES, so it
//                does not reintroduce time predicates.
//     gameclock <MS> = _G_LLM_STRAT_GAME_CLOCK >= MS: the SIM has actually advanced this far. Not a time
//                wait -- the game clock only moves when steps are simulated, so it stalls exactly when the
//                sim stalls (a horizon-starved peer never reaches the threshold and the script times out,
//                which is the correct failure). Use it to gate an in-game capture on "the match is really
//                running", e.g. before reading the debug overlay's net.*/perf.* panel.
//                NOT ENOUGH TO PIN A CAPTURE, though -- see `simstep` below.
//     simstep <N> = the sim has reached step N, AND IS HELD THERE. The predicate for an in-game CAPTURE,
//                where `gameclock` is not: both flip at a deterministic step, but both are read on a
//                PRESENT, and llm_strat_sim_tick's catch-up loop runs 0..k steps per frame -- so the step
//                the frame renders is the burst size, not the target. On a lockstep CLIENT the burst is
//                set by the wire (the host's horizon arrives in chunks), which is why F4H's wall-clock pin
//                converged the host capture to a pixel and left the client's two frames 3.5% apart.
//                This one fires ON the step: it arms the harness's SIM FENCE (MH_Harness_StepFence), which
//                truncates that frame's burst at N from inside the sim_step hook and then holds the clock
//                there, so every present from N renders the identical state and the capture is exact on any
//                peer. REQUIRES an armed harness ([harness] enable=1 -- a `harness_extra` on the registry
//                entry) because the step counter is the instrument's; without one the script is REFUSED by
//                name rather than left to time out. Also refused if the sim has ALREADY passed N when the
//                predicate first runs, since a fence can only truncate a burst it was armed before.
//                The hold is NOT released automatically and the grammar has no release verb -- use it to
//                take the LAST captures of a scenario. (`simstep 0` is not one: it parses as a target
//                already in the past and aborts, which is right for a typo and wrong for a release. The
//                day a script must resume, give it its own directive over MH_Harness_StepFence(0).)
//     retryready [N] = MH_Seam_S8RetryArmed() >= N (default 1): a client's FAILED connect (dead/typo'd IP)
//                has cleared the connect latches, so a corrected-IP re-Connect will re-kick. The S8(b)
//                round-trip test gates its 2nd Connect on this instead of a time wait for the ~4s fail.
//   ACTIONS clickv <N>  clickl <label>  clicki <idx>  cursor <x> <y>  click <x> <y>
//           press <x> <y>  release <x> <y>  capture <name>  log <text>  dump (widget list)  end
//     press/release are the two halves of a click (DOWN-only / UP-only) -- use them to spread a click over
//     FRAMES for a scrollable LIST row (cursor -> [render settles the hover] -> press -> release), which
//     the game selects reliably; a same-frame `click` commits the list's stale (un-rendered) hover row.
//   <target> = "value:<N>" (sprite-menu id) OR a label substring (case-insensitive, rest of line).
//   Clicks (clickl/clickv) and the CLICK-TARGETING waits (settled/onscreen) target only CLICKABLE
//   widgets (not hidden, not disabled flag 0x40) -- so a disabled decoy sharing a label (e.g. the
//   lobby's greyed "Start" placeholder) is ignored and the real enabled action widget is resolved.
//   present/absent/enabled/disabled deliberately do NOT filter greyed widgets: they are EXISTENCE
//   assertions, and `disabled <label>` would be permanently false if they did.
//   This paragraph used to claim `present` filtered too, while the code filtered for NEITHER present
//   nor settled -- that gap WAS the D15 UI-suite flakiness (see find_clickable_target below).
// A WAIT that never comes true aborts the script after `timeout_frames` (failure detection, NOT pacing).
// ===================================================================================================
namespace {

constexpr uintptr_t HOVERED_ADDR    = mh::addr::_G_LLM_UI_WIDGET_HOVERED;
constexpr uintptr_t GAMEMODE_ADDR   = mh::addr::_G_LLM_GAME_MODE;
constexpr uintptr_t WINW_ADDR       = mh::addr::WindowWidth;
constexpr uintptr_t WINH_ADDR       = mh::addr::WindowHeight;
constexpr uintptr_t SESSIONCNT_ADDR = mh::addr::_G_LLM_NET_SESSION_COUNT; // discovered-session count (join gate)
constexpr uintptr_t GAMECLOCK_ADDR  = mh::addr::_G_LLM_STRAT_GAME_CLOCK;  // double, ms -- `gameclock <MS>` gate
// `tactsel <N>` predicate: how many tactical units are SELECTED, as the game itself last counted
// them. There is no widget list in tactical mode -- the HUD is drawn by the render path, not by
// llm_ui_widget_input_tick -- so none of the widget predicates can say "the selection took". This
// counter can, and it is the RIGHT one rather than merely an available one: it is written ONLY by
// llm_tact_active_unit_count_hud_draw, so a script that waits on it has waited for that body to run.
// That property is what makes a tactical capture non-vacuous; gating on anything else produced a
// frame the panel-refresh path had not touched (measured 2026-09-03: with all three of LIFT-TACT's
// converted draw scopes no-op'd, an ungated tactical capture was still byte-identical).
// SB-HOSTFREE: a FUNCTION, not a `constexpr uintptr_t`. This region is MOVABLE -- a
// relocating host puts it in its own arena and fills the .bss it left with 0xCD -- so a
// constant baked at compile time reads poison. tools/check_movable_addresses.py is the gate.
inline uintptr_t TACTSELCNT_ADDR() {
    return mh::state::live_base(mh::state::RID_TACT_ACTIVE_UNIT_COUNT_CACHED);
}
// `occ <N>` predicate: count OCCUPIED lobby slots (a player is really seated on THIS peer's view). Reads the
// live lobby slot table directly -- the same _G_LLM_LOBBY_SLOTS the S7 occ/cap math uses. slot_status @ +0x0b:
// OPEN=0 HUMAN=1 AI=2 CLOSED=3; occupied = HUMAN|AI. Bound by the map's player-slot count (current_map_data+8).
// This is the state that lags a re-JOIN snapshot -- gating a capture on `occ N` makes the frame deterministic
// (fires only once the peer's own slot is seated+rebuilt), where `settled <button>` goes stable far too early.
constexpr uintptr_t LOBBY_SLOTS_ADDR = mh::addr::_G_LLM_LOBBY_SLOTS;      // per-slot record, stride 0x39
constexpr uintptr_t MAP_PCOUNT_ADDR  = mh::addr::current_map_data + 0x08; // map's player-slot count (int)
constexpr int       SLOT_STRIDE = 0x39, SLOT_STATUS_OFF = 0x0b, SLOT_HUMAN = 1, SLOT_AI = 2;
constexpr int       SLOT_RACE_OFF = 0x05; // `race <slot> <v>`: 1=Human 2=Alien (the spinner's values)
// WIDGET_DISABLED (0x40) + clickable() are defined in the top anon namespace (shared with the click helpers).

enum {
    OP_NONE = 0,
    // waits (block until the predicate holds)
    OP_W_SCREEN,
    OP_W_PRESENT,
    OP_W_ABSENT,
    OP_W_ONSCREEN,
    OP_W_SETTLED,
    OP_W_SCRSETTLED,
    OP_W_ENABLED,
    OP_W_DISABLED,
    OP_W_HOVERED,
    OP_W_GAMEMODE,
    OP_W_TACTSEL,
    OP_W_SESSIONS,
    OP_W_PEERS,
    OP_W_OCC,
    OP_W_RACE,
    OP_W_RETRYREADY,
    OP_W_GAMECLOCK,
    OP_W_SIMSTEP,
    OP_W_AWAITSIGNAL,
    OP_W_LAST = OP_W_AWAITSIGNAL,
    // actions (fire once, then advance)
    OP_A_CLICKV,
    OP_A_CLICKL,
    OP_A_CLICKI,
    OP_A_CURSOR,
    OP_A_CLICK,
    OP_A_PRESS,
    OP_A_RELEASE,
    OP_A_KEY,
    OP_A_CAPTURE,
    OP_A_LOG,
    OP_A_SIGNAL,
    OP_A_DUMP,
    OP_A_POKERACE,
    OP_A_INJECTSTART,
    OP_A_END,
};

struct Target {
    bool by_value;
    int  value;
    char label[48];
};

struct Step {
    int      op;
    int      a, b;     // numeric args (value / idx / gamemode / x ; y)
    unsigned scr;      // screen container VA (OP_W_SCREEN)
    Target   tgt;      // target widget (present/absent/enabled/hovered/clickl)
    char     text[64]; // capture name / log message
    char     raw[80];  // original line, for logging (Phase 4 pass/fail parsing)
};

constexpr int          MAX_STEPS = 64;
Step                   g_steps[MAX_STEPS];
int                    g_nstep        = 0;
int                    g_cur          = 0;
int                    g_wait         = 0;
DWORD                  g_step_t0      = 0; // wall clock at the first tick of the current step
bool                   g_script_on    = false;
bool                   g_script_done  = false;
int                    g_timeout      = 1500; // watchdog frames before a stuck WAIT aborts the run
bool                   g_dump_screens = false;
mh_llm_ui_widget_list *g_last_seen    = nullptr;
// OP_W_SETTLED state: the target's resolved center from the previous frame (motion-stopped detection)
int  g_settle_cx = 0, g_settle_cy = 0;
bool g_settle_primed = false;
// TIMEOUT diagnostics for `settled`. A stuck settle has exactly two shapes and they need opposite
// fixes: the target NEVER RESOLVED (wrong label / wrong screen / the widget list is not the one you
// think) versus it resolved but KEPT MOVING (a slide-in that never parks). Without these counters the
// log says only "TIMEOUT ... settled Cancel", which is the same line for both and sends you looking in
// the wrong half. Cost: two ints on a path that already runs per frame.
int g_settle_seen  = 0;                   // frames on which the target resolved at all
int g_settle_moves = 0;                   // times its center CHANGED after the first sighting
int g_settle_minx = 0, g_settle_maxx = 0; // the SPREAD of its x -- a jitter tells you the amplitude
// The lobby geometry sampled AT THE MOMENT the widget was seen at its extreme x. net_seams' own trace
// samples these in on_lobby_dispatch and reports them rock-steady (frame_x=-124 right_x=363) while the
// harness sees the widget sweep 558 px -- so the two observers disagree, and only a sample taken at the
// SAME instant as the extreme can say which one is looking at the transient.
int   g_settle_fx_min = 0, g_settle_rx_min = 0, g_settle_fx_max = 0, g_settle_rx_max = 0;
DWORD g_settle_since = 0; // when the center last MOVED -- `settled` is a dwell, not a frame count
// `simstep <N>` state: which SCRIPT step index the fence was armed for, so the "target already in the
// past" refusal is decided once, on the predicate's FIRST evaluation, and not re-tested every present.
int g_simstep_idx = -1;

bool is_wait_op(int op) { return op >= OP_W_SCREEN && op <= OP_W_LAST; }

bool tgt_matches(const Target &t, mh_llm_ui_widget *w) {
    return t.by_value ? (w->value == t.value) : label_matches(w->label, t.label);
}

// VISIBLE = not hidden. Says nothing about whether it can be clicked, and `enabled`/`disabled`/
// `absent` all depend on that: filtering greyed widgets out here would make `disabled <label>`
// permanently false, which is the opposite of what it asserts.
mh_llm_ui_widget *find_visible(const Target &t) {
    mh_llm_ui_widget_list *list = active_list();
    if (!list) return nullptr;
    for (mh_llm_ui_widget **wp = list->children; *wp; ++wp)
        if (((*wp)->flags & WIDGET_HIDDEN) == 0 && tgt_matches(t, *wp)) return *wp;
    return nullptr;
}

const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// Counting predicates (`sessions` / `peers` / `occ`) read ">= N" by default -- they were all written to
// wait for something to APPEAR. Waiting for one to go AWAY needs the other direction, and that is not a
// nicety: "the transport lost its last peer" (`peers <1`) is the whole assertion behind R-live-ui, and
// "a dropped peer's lobby slot was freed" (`occ <2`) is R-live-start's. Write `<N` for strictly-less-than.
// Parsed here rather than as separate ops so all three predicates gain it at once.
int parse_count(const char *arg, int *less_than) {
    const char *q = skip_ws(arg);
    *less_than    = 0;
    if (*q == '<') {
        *less_than = 1;
        q          = skip_ws(q + 1);
    }
    return (int)strtol(q, nullptr, 0);
}

// Copy src into dst (capped), trimming trailing whitespace / CR.
void copy_trim(char *dst, int cap, const char *src) {
    int n = 0;
    while (src[n] && n < cap - 1) {
        dst[n] = src[n];
        ++n;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t' || dst[n - 1] == '\r')) --n;
    dst[n] = 0;
}

void parse_target(const char *s, Target *t) {
    s = skip_ws(s);
    if (strncmp(s, "value:", 6) == 0) {
        t->by_value = true;
        t->value    = (int)strtol(s + 6, nullptr, 0);
        t->label[0] = 0;
    } else {
        t->by_value = false;
        t->value    = 0;
        copy_trim(t->label, sizeof(t->label), s);
    }
}

void parse_line(const char *line) {
    const char *p = skip_ws(line);
    if (*p == 0 || *p == '#' || *p == ';') return;
    char op[16];
    int  n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && n < (int)sizeof(op) - 1) op[n++] = *p++;
    op[n]           = 0;
    const char *arg = skip_ws(p);
    if (g_nstep >= MAX_STEPS) return;
    Step *s = &g_steps[g_nstep];
    memset(s, 0, sizeof(*s));
    copy_trim(s->raw, sizeof(s->raw), line);

    if (strcmp(op, "screen") == 0) {
        s->op  = OP_W_SCREEN;
        s->scr = (unsigned)strtoul(arg, nullptr, 0);
    } else if (strcmp(op, "present") == 0) {
        s->op = OP_W_PRESENT;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "absent") == 0) {
        s->op = OP_W_ABSENT;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "onscreen") == 0) {
        s->op = OP_W_ONSCREEN;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "screensettled") == 0) {
        s->op = OP_W_SCRSETTLED; // no target: the whole active screen
    } else if (strcmp(op, "settled") == 0) {
        s->op = OP_W_SETTLED;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "enabled") == 0) {
        s->op = OP_W_ENABLED;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "disabled") == 0) {
        s->op = OP_W_DISABLED;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "hovered") == 0) {
        s->op = OP_W_HOVERED;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "gamemode") == 0) {
        s->op = OP_W_GAMEMODE;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "tactsel") == 0) {
        s->op = OP_W_TACTSEL;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "sessions") == 0) {
        s->op = OP_W_SESSIONS;
        s->a  = parse_count(arg, &s->b);
    } else if (strcmp(op, "peers") == 0) {
        s->op = OP_W_PEERS;
        s->a  = parse_count(arg, &s->b);
    } else if (strcmp(op, "occ") == 0) {
        s->op = OP_W_OCC;
        s->a  = parse_count(arg, &s->b);
    } else if (strcmp(op, "race") == 0) {
        // `race <slot> <value>` -- two ints, unlike every other count predicate, so parse both here.
        s->op       = OP_W_RACE;
        char *after = nullptr;
        s->a        = (arg && *arg) ? (int)strtol(arg, &after, 0) : -1;
        s->b        = (after && *after) ? (int)strtol(after, nullptr, 0) : -1;
    } else if (strcmp(op, "retryready") == 0) {
        s->op = OP_W_RETRYREADY;
        s->a  = (arg && *arg) ? (int)strtol(arg, nullptr, 0) : 1;
    } else if (strcmp(op, "awaitsignal") == 0) {
        s->op = OP_W_AWAITSIGNAL;
        lstrcpynA(s->text, arg ? arg : "", (int)sizeof(s->text));
    } else if (strcmp(op, "signal") == 0) {
        s->op = OP_A_SIGNAL;
        lstrcpynA(s->text, arg ? arg : "", (int)sizeof(s->text));
    } else if (strcmp(op, "gameclock") == 0) {
        s->op = OP_W_GAMECLOCK;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "simstep") == 0) {
        s->op = OP_W_SIMSTEP;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "clickv") == 0) {
        s->op = OP_A_CLICKV;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "clickl") == 0) {
        s->op = OP_A_CLICKL;
        parse_target(arg, &s->tgt);
    } else if (strcmp(op, "clicki") == 0) {
        s->op = OP_A_CLICKI;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "cursor") == 0 || strcmp(op, "click") == 0 || strcmp(op, "press") == 0 ||
               strcmp(op, "release") == 0) {
        s->op = strcmp(op, "cursor") == 0  ? OP_A_CURSOR
                : strcmp(op, "click") == 0 ? OP_A_CLICK
                : strcmp(op, "press") == 0 ? OP_A_PRESS
                                           : OP_A_RELEASE;
        char *end;
        s->a = (int)strtol(arg, &end, 0);
        s->b = (int)strtol(end, nullptr, 0);
    } else if (strcmp(op, "key") == 0) {
        // key <scancode>  -- one down+up pair. Scancodes are PC set-1, the same values
        // llm_strat_input_update compares against (e.g. 0x01 ESC, 0x3f..0x41 F5/F6/F7, 0x1c Enter).
        s->op = OP_A_KEY;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "capture") == 0) {
        s->op = OP_A_CAPTURE;
        copy_trim(s->text, sizeof(s->text), arg);
    } else if (strcmp(op, "log") == 0) {
        s->op = OP_A_LOG;
        copy_trim(s->text, sizeof(s->text), arg);
    } else if (strcmp(op, "pokerace") == 0) {
        // `pokerace <slot> <value>` -- TEST HOOK, two ints like the `race` predicate.
        s->op       = OP_A_POKERACE;
        char *after = nullptr;
        s->a        = (arg && *arg) ? (int)strtol(arg, &after, 0) : -1;
        s->b        = (after && *after) ? (int)strtol(after, nullptr, 0) : -1;
    } else if (strcmp(op, "injectstart") == 0) {
        s->op = OP_A_INJECTSTART; // `injectstart` -- U29 (b) test hook, no arguments
    } else if (strcmp(op, "dump") == 0) {
        s->op = OP_A_DUMP; // debug aid: log the active widget list at this point in the walk
    } else if (strcmp(op, "end") == 0) {
        s->op = OP_A_END;
    } else {
        ui_log("; [script] IGNORED unknown op '%s' (line: %s)", op, s->raw);
        return;
    }
    ++g_nstep;
}

bool load_script(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    static char buf[8192];
    DWORD       rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    buf[rd]    = 0;
    char *line = buf;
    for (char *q = buf;; ++q) {
        if (*q == '\n' || *q == 0) {
            char end = *q;
            *q       = 0;
            parse_line(line);
            line = q + 1;
            if (end == 0) break;
        }
    }
    return g_nstep > 0;
}

// s->b carries the `<` flag parsed by parse_count: default is ">= a", `<a` is strictly less.
bool count_ok(const Step *s, int v) { return s->b ? (v < s->a) : (v >= s->a); }

bool wait_satisfied(const Step *s) {
    switch (s->op) {
        case OP_W_SCREEN:
            return (unsigned)(uintptr_t)active_list() == s->scr;
        case OP_W_PRESENT:
            return find_visible(s->tgt) != nullptr;
        case OP_W_ABSENT:
            return find_visible(s->tgt) == nullptr;
        case OP_W_ONSCREEN: {                           // present AND center inside the window -- waits out slide-in animations
            mh_llm_ui_widget *w = find_visible(s->tgt); // animation probe, same as settled
            if (!w) return false;
            int cx = 0, cy = 0;
            if (!widget_center(active_list(), w, &cx, &cy)) return false;
            int ww = *(int *)WINW_ADDR, wh = *(int *)WINH_ADDR;
            return cx >= 0 && cx < ww && cy >= 0 && cy < wh;
        }
        // present AND the resolved center has held still for `settle_ms` -- a DURATION, not a frame
        // count, and that distinction is load-bearing.
        //
        // This used to mean "unchanged since the LAST FRAME". The slide-in animation advances off a
        // millisecond clock, so the faster the frame loop runs the smaller the per-frame movement --
        // and once two consecutive frames fall inside one clock tick the panel looks stationary while
        // it is still travelling. Under [video] no_present=1 (no blit, so no vsync wait) that is
        // exactly what happened: `settled Ok` was satisfied after ONE frame and the following click
        // landed at x=-389, off the left edge, on a panel still sliding in. Both peers of a local
        // 2-peer run failed that way (2026-07-28).
        //
        // Frame-relative UI semantics are only ever correct at the frame rate they were tuned at.
        // Holding the center still for a WALL-CLOCK duration is right at any rate -- the same lesson
        // as "measure deadlines with GetTickCount(), never an iteration count".
        case OP_W_SCRSETTLED:
            // Whole-screen stillness, sampled every present by screen_settle_tick() -- so the answer
            // does not depend on when the script happens to ask, and a screen that never parks
            // (a marquee) TIMES OUT instead of quietly satisfying a one-frame comparison.
            return MH_UIDrive_ScreenSettled() != 0;
        case OP_W_SETTLED: {
            if (g_wait == 0) {
                g_settle_primed = false; // first eval of this step: re-prime
                g_settle_seen   = 0;     // and reset the "was it ever there" diagnostic
                g_settle_moves  = 0;
                g_settle_minx = g_settle_maxx = 0;
            }
            // Deliberately find_VISIBLE: `settled` is also used as a pure ANIMATION PROBE on a widget
            // that is legitimately greyed at that moment (esc_menu.txt waits `settled Start` to ride
            // out the lobby slide-in, adds a computer player, and only then gates on `enabled Start`).
            // Requiring clickable here breaks that use -- measured, it hung esc_menu for 80001 frames.
            // The click-time race this used to cause is fixed IN THE CLICK instead; see try_click_*.
            mh_llm_ui_widget *w  = find_visible(s->tgt);
            int               cx = 0, cy = 0;
            if (!w || !widget_center(active_list(), w, &cx, &cy)) {
                g_settle_primed = false;
                return false;
            }
            ++g_settle_seen; // the target RESOLVED at least once -- see the TIMEOUT diagnostic
            if (g_settle_seen == 1 || cx < g_settle_minx) {
                g_settle_minx   = cx;
                g_settle_fx_min = *(const int *)mh::addr::lobby_frame_x;
                g_settle_rx_min = *(const int *)mh::addr::lobby_right_panel_x;
            }
            if (g_settle_seen == 1 || cx > g_settle_maxx) {
                g_settle_maxx   = cx;
                g_settle_fx_max = *(const int *)mh::addr::lobby_frame_x;
                g_settle_rx_max = *(const int *)mh::addr::lobby_right_panel_x;
            }
            const DWORD now = GetTickCount();
            if (!g_settle_primed || cx != g_settle_cx || cy != g_settle_cy) {
                if (g_settle_primed) ++g_settle_moves; // it actually MOVED, vs first look
                g_settle_cx     = cx;
                g_settle_cy     = cy;
                g_settle_since  = now; // it MOVED (or this is the first look) -- restart the dwell
                g_settle_primed = true;
                return false;
            }
            return (now - g_settle_since) >= (DWORD)g_settle_ms;
        }
        case OP_W_ENABLED: {
            mh_llm_ui_widget *w = find_visible(s->tgt);
            return w && (w->flags & WIDGET_DISABLED) == 0;
        }
        // Deliberately NOT `!enabled`: the widget must EXIST and be greyed. "Absent" is a different
        // fact with a different op, and conflating them would let a screen that lost the control
        // entirely pass a test meaning to assert "the control is there but refuses".
        case OP_W_DISABLED: {
            mh_llm_ui_widget *w = find_visible(s->tgt);
            return w && (w->flags & WIDGET_DISABLED) != 0;
        }
        // THE ONLY PREDICATE THAT ASKS THE GAME'S OWN HIT TEST, and therefore the only one that can
        // tell a screen that is LIVE from one that is merely selected. It re-injects the move itself,
        // which is what makes it work at all.
        //
        // MEASURED 2026-09-07, four probes wasted before the frame was looked at: activating a menu
        // item writes its `nav_target` into the active-list pointer IMMEDIATELY, so `screen <VA>`,
        // `present`, `settled` and `dump` all report the new screen while the frame still shows the
        // old one -- the new-game race picker took ~8 s of real time to arrive while every one of
        // those predicates said it was already there, and the clicks aimed at it went nowhere.
        //
        // Hover cannot be polled either: llm_ui_widget_input_tick only runs its hit test while
        // llm_ui_mouse_poll_next_event has EVENTS to drain, so a `cursor` issued once (before the
        // screen existed) is consumed against the old screen and nothing ever re-evaluates. A human
        // never hits this because a real hand keeps the mouse moving. So re-inject a MOVE at the
        // target's hit point on every evaluation: one event per present, drained the same present,
        // and the answer then comes from the game's own hit test rather than from our model of it.
        case OP_W_HOVERED: {
            mh_llm_ui_widget *t  = find_visible(s->tgt);
            int               hx = 0, hy = 0;
            if (t && widget_hit_point(active_list(), t, &hx, &hy)) MH_UIDrive_CursorTo(hx, hy);
            mh_llm_ui_widget *h = *(mh_llm_ui_widget **)HOVERED_ADDR;
            return h && tgt_matches(s->tgt, h);
        }
        case OP_W_GAMEMODE:
            return *(unsigned char *)GAMEMODE_ADDR == (unsigned char)s->a;
        case OP_W_TACTSEL: return *(const int *)TACTSELCNT_ADDR() == s->a;
        case OP_W_AWAITSIGNAL: {
            // A PEER rendezvous, ferried by the runner: the other peer's script ran `signal <name>`,
            // ui_test saw the marker in its log and dropped rig_<name>.flag next to our exe.
            //
            // Why this cannot be a game-protocol predicate: "the client's UI has reached its lobby" is a
            // fact only the client holds. The host's earliest observable is the JOIN admit, which lands
            // ~12 log lines before the client's first lobby message and well before its lobby is drawn --
            // measured 2026-07-28, and that gap is exactly the race that made match_launch flaky.
            // Deliberately NOT a time predicate: this is still a state the rig OBSERVES, not a sleep.
            char flag[MAX_PATH];
            wsprintfA(flag, "%srig_%s.flag", MH_RunDir(), s->text);
            return GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
        }
        case OP_W_GAMECLOCK: { // the SIM has advanced >= N ms (stalls when the sim stalls -- not a time wait)
            double clk;
            memcpy(&clk, (const void *)GAMECLOCK_ADDR, sizeof(clk));
            return clk * 1000.0 >= (double)s->a; // the global is game-SECONDS (cf. net_internal.h ms_of)
        }
        case OP_W_SIMSTEP: { // F5J: the sim has reached step N -- and the harness is HOLDING it there
            // The call both ARMS the fence (idempotently -- we re-ask every present) and returns the
            // live step, so one crossing answers the predicate and places it. See
            // mh/include/mh_harness_export.h for why a fence is what makes an in-game capture exact.
            const int now = MH_Harness_StepFence(s->a);
            if (now < 0) {
                // -1 is the contract's absent value AND the armed-but-unhooked answer. Both mean the
                // same thing here: nothing is going to move this counter, so the wait can never end.
                // Refuse BY NAME -- a 1500-frame watchdog on `simstep 600` reads as "the sim stalled",
                // which sends the next reader into the netcode instead of into the lane's ini.
                ui_log("; [script] ABORT at step %d: `%s` needs an ARMED HARNESS -- the sim step "
                       "counter is the instrument's, and this lane has none ([harness] enable=1, i.e. "
                       "a `harness_extra` on the test's registry entry). Refusing rather than timing "
                       "out on a predicate nothing in this process can satisfy.",
                       g_cur, s->raw);
                g_script_done = true;
                return false;
            }
            if (g_simstep_idx != g_cur) { // the FIRST evaluation of this script step -- i.e. the arm
                g_simstep_idx = g_cur;
                if (now >= s->a) {
                    ui_log("; [script] ABORT at step %d: `%s` -- the sim is ALREADY at step %d, so the "
                           "target is in the past. The fence can only truncate a catch-up burst it was "
                           "armed BEFORE; passing here would hand the capture an unpinned step, which "
                           "is exactly the near-miss this predicate replaced. Raise the target.",
                           g_cur, s->raw, now);
                    g_script_done = true;
                    return false;
                }
            }
            return now >= s->a;
        }
        case OP_W_SESSIONS: // discovered joinable sessions (the browser's join gate)
            return count_ok(s, *(int *)SESSIONCNT_ADDR);
        case OP_W_PEERS: // transport-connected peers (host: "all clients joined" before Start)
            return count_ok(s, MH_Net_PeerCount());
        case OP_W_OCC: { // OCCUPIED lobby slots on THIS peer (HUMAN|AI) -- the seated-and-rendered gate
            int mapmax = *(const int *)MAP_PCOUNT_ADDR;
            if (mapmax < 1) mapmax = 1;
            if (mapmax > 8) mapmax = 8;
            int occ = 0;
            for (int i = 0; i < mapmax; ++i) {
                unsigned char st =
                    *(const unsigned char *)(LOBBY_SLOTS_ADDR + i * SLOT_STRIDE + SLOT_STATUS_OFF);
                if (st == SLOT_HUMAN || st == SLOT_AI) ++occ;
            }
            return count_ok(s, occ);
        }
        case OP_W_RACE: { // lobby slot's race field on THIS peer -- the U28 propagation gate
            if (s->a < 0 || s->a > 7 || s->b < 0) return false;
            return *(const unsigned char *)(LOBBY_SLOTS_ADDR + s->a * SLOT_STRIDE + SLOT_RACE_OFF) ==
                   (unsigned char)s->b;
        }
        case OP_W_RETRYREADY:                      // S8(b): the failed-connect latch-clear has fired -> a corrected-IP re-Connect
            return MH_Seam_S8RetryArmed() >= s->a; // will re-kick. Gates the round-trip test's 2nd Connect.
    }
    return true;
}

// D15: this used to be `void`, and script_tick() advanced to the next step regardless. A click that
// hit nothing was therefore SILENT: the script marched on to its next wait, which could never come
// true because the click that was supposed to change the screen never happened, and the run died
// MUCH later on that unrelated wait -- after burning the frame watchdog (1.2 M frames for
// link_death) and then the wall-clock budget. So the reported failure named a step nowhere near the
// real cause, and the wall clock looked like a contention/timeout problem. It was neither.
//
// Only the three widget CLICKS can report anything but CLICK_OK -- they are the only actions with a
// resolvable target that can be missing or not-yet-ready.
click_result do_action(const Step *s) {
    switch (s->op) {
        case OP_A_CLICKV:
            return click_value_try(s->a);
        case OP_A_CLICKL:
            return click_label_try(s->tgt.label);
        case OP_A_CLICKI:
            return MH_UIDrive_ClickWidget(s->a) != 0 ? CLICK_OK : CLICK_ABSENT;
        case OP_A_CURSOR:
            MH_UIDrive_CursorTo(s->a, s->b);
            break;
        case OP_A_CLICK:
            MH_UIDrive_Click(s->a, s->b);
            break;
        case OP_A_PRESS:
            MH_UIDrive_Press(s->a, s->b);
            break;
        case OP_A_RELEASE:
            MH_UIDrive_Release(s->a, s->b);
            break;
        case OP_A_KEY:
            enqueue_key((uint32_t)s->a, true);
            enqueue_key((uint32_t)s->a, false);
            ui_log("; key scancode 0x%02x (down+up)", s->a);
            break;
        case OP_A_CAPTURE:
            MH_Capture_Shot(s->text);
            break;
        case OP_A_POKERACE: {
            // U28 TEST HOOK: write a lobby slot's race field LOCALLY, without the spinner and so
            // WITHOUT the 0x0c push the spinner would send. That is the whole point -- it manufactures,
            // deterministically and with no timing race, the exact end-state of a client edit that the
            // host never saw: this peer's array disagrees with the host's at the Start click. It is how
            // U28 (a)/(c) are testable at all; a real in-flight edit is a race and a race is not a test.
            // Harness-only (the interpreter is [uitest], absent from a shipping run) and deliberately
            // NOT routed through llm_lobby_push_local_slot_state.
            if (s->a < 0 || s->a > 7 || s->b < 0) {
                ui_log("; [script] pokerace BAD ARGS slot=%d value=%d", s->a, s->b);
                break;
            }
            unsigned char *r =
                (unsigned char *)(LOBBY_SLOTS_ADDR + s->a * SLOT_STRIDE + SLOT_RACE_OFF);
            ui_log("; [script] pokerace slot[%d] race %d -> %d (LOCAL only, no 0x0c push)", s->a, *r,
                   s->b);
            *r = (unsigned char)s->b;
            break;
        }
        case OP_A_INJECTSTART:
            // U29 (b): the host's Start, arriving when the client is no longer in a lobby. Two
            // independent guards must swallow it -- the torn-down slot array (the driver never
            // reaches its entry conditions) and the screen gate (it refuses even if they are met).
            // [net] u29_teardown=0 / u29_screen_gate=0 remove them one at a time, which is how this
            // hook's negative arms are reached without editing code.
            ui_log("; [script] injectstart -- latching a bare FLAG_START (entry must be a no-op here)");
            MH_Seam_InjectStartReceived();
            break;
        case OP_A_LOG:
            ui_log("; [script] LOG: %s", s->text);
            break;
        case OP_A_SIGNAL:
            // The runner ferries this to the other peers (see ui_test.py). We only announce it.
            ui_log("; [script] SIGNAL %s", s->text);
            break;
        case OP_A_DUMP:
            MH_UIDrive_DumpWidgets();
            break;
        case OP_A_END:
            ui_log("; [script] COMPLETE (end)"); // same terminal marker as running off the last step
            g_script_done = true;
            break;
    }
    return CLICK_OK; // every other action is unconditional
}

// Log each new active screen + its widgets ([uitest] dump_screens=1) -- the authoring aid for building a
// script: run once, read mh_uidrive.log to learn each screen's container VA + widget labels/values.
void dump_on_change() {
    if (!g_dump_screens) return;
    mh_llm_ui_widget_list *list = active_list();
    if (list && list != g_last_seen && list->children && *list->children) {
        ui_log("; [screen-change]");
        MH_UIDrive_DumpWidgets();
        g_last_seen = list;
    }
}

void script_tick() {
    if (!g_script_on || g_script_done) return;
    if (g_cur >= g_nstep) {
        ui_log("; [script] COMPLETE (%d/%d steps)", g_cur, g_nstep);
        g_script_done = true;
        return;
    }
    Step *s = &g_steps[g_cur];
    if (g_wait == 0) g_step_t0 = GetTickCount(); // first tick on this step -- start its clock
    const DWORD step_ms = GetTickCount() - g_step_t0;
    if (is_wait_op(s->op)) {
        if (wait_satisfied(s)) {
            // Frames AND milliseconds: the frame count is what the timeout is budgeted in, the ms is
            // what a human reads to find where a 2-minute walk actually spends its time.
            ui_log("; [script] %d ok (waited %d fr, %u.%03us): %s", g_cur, g_wait, step_ms / 1000, step_ms % 1000, s->raw);
            ++g_cur;
            g_wait = 0;
        } else if (g_script_done) {
            // A PREDICATE ITSELF ABORTED (`simstep` with no armed harness / a target already passed).
            // It has already logged the reason by name; falling through would add a watchdog line for
            // a wait that is not waiting on anything, which is the misleading half of the two.
            return;
        } else if (++g_wait > g_timeout) {
            ui_log("; [script] TIMEOUT at step %d after %d frames (%u.%03us) -- ABORT: %s", g_cur, g_wait,
                   step_ms / 1000, step_ms % 1000, s->raw);
            if (s->op == OP_W_SETTLED)
                ui_log("; [script]   settle diag: target resolved on %d frames, moved %d times, x in [%d..%d] -- %s",
                       g_settle_seen, g_settle_moves, g_settle_minx, g_settle_maxx,
                       g_settle_seen == 0   ? "NEVER FOUND (wrong label, wrong screen, or not in the active list)"
                       : g_settle_moves > 0 ? "found but NEVER PARKED (a slide-in that keeps moving)"
                                            : "found and still, but the dwell never elapsed (clock?)");
            // Dump the active screen on ANY wait timeout. A timeout that does not say what was on screen
            // sends you to a live debugger to answer a question the run already knew -- and by then the
            // frame is gone. This is the same dump `dump_screens=1` produces, spent only where it pays.
            if (s->op == OP_W_SETTLED && g_settle_seen)
                ui_log("; [script]   lobby geom at x=%d: frame_x=%d right_x=%d | at x=%d: frame_x=%d right_x=%d",
                       g_settle_minx, g_settle_fx_min, g_settle_rx_min, g_settle_maxx, g_settle_fx_max,
                       g_settle_rx_max);
            ui_log("; [script]   active screen at timeout:");
            MH_UIDrive_DumpWidgets();
            // AND A PICTURE OF IT. A widget dump describes the active LIST, which is not the same
            // thing as what is on the glass: measured 2026-09-07, `clickv 103` swaps the active list
            // to the new-game screen's IMMEDIATELY (the menu widget's nav_target is written on
            // activation), so `screen`/`present`/`dump` all reported the race picker while the frame
            // still showed the main menu and its ship. Four probes were spent theorising about a
            // screen nobody had looked at. A timeout is exactly the moment the frame is worth
            // keeping, and it is gone a millisecond later.
            MH_Capture_Shot("script_timeout");
            g_script_done = true;
        }
    } else {
        // Log the step ONCE, not on every retry frame -- a retried click would otherwise emit
        // thousands of identical "do:" lines while it waits for a button to go live.
        if (g_wait == 0) ui_log("; [script] %d do: %s", g_cur, s->raw);
        const click_result r = do_action(s);
        if (r == CLICK_RETRY) {
            // The target is on screen but still greyed -- a TIMING condition, so wait for it exactly
            // as a wait op would, under the same frame watchdog. This is the D15 fix: a ~400 ms
            // disabled window after a screen transition no longer kills the run.
            if (++g_wait > g_timeout) {
                ui_log("; [script] TIMEOUT at step %d after %d frames (%u.%03us) -- ABORT: %s "
                       "(target present but never became clickable)",
                       g_cur, g_wait, step_ms / 1000, step_ms % 1000, s->raw);
                ui_log("; [script]   active screen at timeout:");
                MH_UIDrive_DumpWidgets();
                g_script_done = true;
            }
            return; // stay on this step
        }
        if (r == CLICK_ABSENT) {
            // Nothing on this screen carries the target at all: a real error, so FAIL FAST at the
            // step that is actually wrong instead of letting a doomed script run into a later wait.
            ui_log("; [script] ABORT at step %d: the action could not be performed: %s", g_cur,
                   s->raw);
            ui_log("; [script]   active screen at abort:");
            MH_UIDrive_DumpWidgets();
            g_script_done = true;
            return;
        }
        ++g_cur;
        g_wait = 0;
    }
}

} // namespace

extern "C" void MH_UIDrive_OnPresent(void) {
    apply_mouse_mode(); // TACT-REC: before the g_enabled gate -- an interactive player has uitest off
    // UI-REC: before the g_enabled gate too. A journal replay has [uitest] OFF -- there is no script
    // -- and it is exactly the caller that needs this sampled every present.
    screen_settle_tick();
    if (g_mouse_trace) trace_mouse_ring();

    // Hotkeys (interactive testing): F7 = dump active list, F8 = click the configured target.
    bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !g_prev_f7) MH_UIDrive_DumpWidgets();
    g_prev_f7 = f7;
    bool f8   = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !g_prev_f8 && have_auto_target()) fire_auto_target();
    g_prev_f8 = f8;

    if (!g_enabled) return;

    dump_on_change(); // [uitest] dump_screens=1: log each new screen (script-authoring aid)

    // Script mode ([uitest] script=FILE) takes precedence over the single-target auto-click.
    if (g_script_on) {
        script_tick();
        return;
    }

    // One-shot: log the first non-empty active list so a headless run reveals the real clickable labels
    // (used to author click_label targets without a live keyboard).
    if (!g_dumped) {
        mh_llm_ui_widget_list *list = active_list();
        if (list && list->children && *list->children) {
            MH_UIDrive_DumpWidgets();
            g_dumped = true;
        }
    }

    // Auto-click: fire ONCE, keyed purely on UI state (the target widget appearing), not on a frame/time
    // offset -- with a small consecutive-frames settle so we don't click mid screen-transition.
    if (g_fired || !have_auto_target()) return;
    if (auto_target_present()) {
        if (++g_seen >= g_settle_frames) {
            if (fire_auto_target())
                ui_log("; auto-click fired (state predicate: target present %d frames)", g_seen);
            g_fired = true;
        }
    } else {
        g_seen = 0;
    }
}

extern "C" int MH_UIDrive_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);
    // TACT-REC: the VM mouse fix. Its own [input] section, not [uitest]: it applies to an
    // INTERACTIVE run where uitest is off, and coupling it to that flag would make it unreachable
    // for the one case it exists for. All three default to 0 = untouched, so no existing run moves.
    g_mouse_trace    = GetPrivateProfileIntA("input", "mouse_trace", 0, ini);
    g_mouse_absolute = GetPrivateProfileIntA("input", "mouse_absolute", 0, ini);
    g_mouse_div      = GetPrivateProfileIntA("input", "mouse_div", 0, ini);
    g_mouse_accel    = GetPrivateProfileIntA("input", "mouse_accel", 0, ini);

    g_enabled = GetPrivateProfileIntA("uitest", "enable", 0, ini) != 0;
    GetPrivateProfileStringA("uitest", "click_label", "", g_auto_label, sizeof(g_auto_label), ini);
    g_auto_value    = GetPrivateProfileIntA("uitest", "click_value", -1, ini); // sprite-menu id (-1 = unset)
    g_settle_frames = GetPrivateProfileIntA("uitest", "settle_frames", 2, ini);
    if (g_settle_frames < 1) g_settle_frames = 1;
    // How long the center must hold still for `settled` (ms). A DURATION so the predicate means the
    // same thing at 60 fps and at headless rates -- see the OP_W_SETTLED comment.
    g_settle_ms = GetPrivateProfileIntA("uitest", "settle_ms", 120, ini);
    if (g_settle_ms < 0) g_settle_ms = 0;
    g_dump_screens = GetPrivateProfileIntA("uitest", "dump_screens", 0, ini) != 0;
    g_timeout      = GetPrivateProfileIntA("uitest", "timeout_frames", 1500, ini);

    // Script mode: [uitest] script=<file> (relative to the exe dir). Loaded once here.
    char scriptname[64];
    GetPrivateProfileStringA("uitest", "script", "", scriptname, sizeof(scriptname), ini);
    if (scriptname[0]) {
        char spath[MAX_PATH];
        wsprintfA(spath, "%s%s", exe, scriptname); // `exe` is the dir (trailing '\') after the loop above
        if (load_script(spath)) {
            g_script_on = true;
            ui_log("; [script] loaded '%s' (%d steps)", scriptname, g_nstep);
        } else {
            ui_log("; [script] FAILED to load '%s'", spath);
        }
    }
    ui_log("; uidrive enabled=%d (F7=dump F8=click; [uitest] click_label='%s' click_value=%d settle_frames=%d "
           "script='%s' dump_screens=%d timeout=%d)",
           g_enabled, g_auto_label, g_auto_value, g_settle_frames, scriptname, g_dump_screens, g_timeout);
    return 1;
}
