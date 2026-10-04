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
#include "include/mh_seam_export.h"    // MH_Seam_S8RetryArmed (`retryready`), MH_Seam_S3LobbyGen (`lobbygen`)
#include "include/mh_harness_export.h" // MH_Harness_StepFence (the `simstep` predicate: the SIM's own step)
#include "seams/ui_net_indicator.h"    // MH_Lockstep_StallBindingPeer -- the `stalled` predicate (TL-UISTALL)
#include "ui/lobby_ping.h"             // mp:L1f: lobby_ping_measured_rows -- the `lobbyping` predicate
#include "gfx/overlay_imgui.h"         // PT-GFX4: the `imgui` predicate reads the overlay's state
#include "gfx/ddraw_own.h"             // PT-GFX3: `winsize` logs the image rect the mouse is mapped through
#include "input/dinput_own.h"          // PT-INPUT1: the `raw*` verbs feed the owned DirectInput
#include "include/mh_run_context.h"    // MH_RunDir
#include "include/mh_log_sink.h"       // LOG1: the async log sink
#include "addr/mh_addrs.gen.h"         // generated EN VAs
#include "config/ini_read.h"           // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "addr/mh_calls.gen.h"         // mh::call::llm_time_get_ticks_ms (the key ring's timestamp)
#include "state/region_runtime.h"      // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                       // where it IS, not where the binary put it
#include "addr/mh_structs.gen.h"       // generated game struct mirrors (mh::game::mh_llm_*)
#include "hook/watcall.h"              // call_watcall2 (resolve_position is __watcall)
#include "en_guard.h"                  // EN-only build gate
#include "mh_net_proto/text_utf8.h"    // mp:MP-LANG: `wlobby` reads a plain-text needle as UTF-8

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

// ---- mp:SES3 -- the CAMERA half of the trace (the stuck-scroll hunt, SC2) ----------------------
// The eight latches plus the camera position they drive. llm_strat_input_update (0x00441b88) is the
// SOLE writer AND sole referrer of the whole 0x005d0bbc..0c2b block -- measured 2026-09-17 two ways
// (Ghidra's 60 write references, and a byte scan of the whole image for the addresses as immediates:
// 116 of 117 hits are inside that one function and the 117th is a coincidence in an unanalysed data
// table at 0x004f5347). So a latch that is set while nothing should be setting it was set BY that
// function on an earlier frame and never cleared -- which is the whole hypothesis space this trace
// has to separate. Two gates sit over the set/clear block and NOT over the apply loops -- an
// RMB/LMB gesture in state 1, and (for the RIGHT/DOWN pair only) an LMB drag-select -- so a
// latched direction keeps scrolling for as long as either holds, wherever the cursor has gone.
//
// LITERAL ADDRESSES, NOT `mh::addr::` CONSTANTS, AND THAT IS MEASURED RATHER THAN LAZY. The
// manifest is normally the single path from a VA to a constant, but a DATA entry there also
// becomes a STATE REGION: the registry is rebuilt from the manifest, region ids are assigned in
// address order, and nine new regions in the middle of that order renumber every later RID_* and
// change the world-snapshot block table. The blob carries a fingerprint over (rid, len, name) of
// every block precisely so a stale capture cannot be imported, so the whole cascade was tried and
// measured on 2026-09-17: all three committed LIB-REF fixtures went `libmh_import_world refused
// the fixture (rc=-3)`, i.e. re-recording them on the rig would be the price of registering these
// nine read-only words. Registering them belongs with the next fixture re-record, not with a
// telemetry line -- the trace only ever READS them, and nothing here binds or relocates them.
constexpr uintptr_t CAM_EDGE_L  = 0x005d0bfcu;                  // _G_LLM_CAM_EDGE_LEFT_ACTIVE
constexpr uintptr_t CAM_EDGE_R  = 0x005d0c00u;                  // _G_LLM_CAM_EDGE_RIGHT_ACTIVE
constexpr uintptr_t CAM_EDGE_U  = 0x005d0c04u;                  // _G_LLM_CAM_EDGE_UP_ACTIVE
constexpr uintptr_t CAM_EDGE_D  = 0x005d0c08u;                  // _G_LLM_CAM_EDGE_DOWN_ACTIVE
constexpr uintptr_t CAM_HELD_L  = 0x005d0bbcu;                  // _G_LLM_CAM_SCROLL_LEFT_HELD  (scancode 0x4b)
constexpr uintptr_t CAM_HELD_R  = 0x005d0bc0u;                  // _G_LLM_CAM_SCROLL_RIGHT_HELD (scancode 0x4d)
constexpr uintptr_t CAM_HELD_U  = 0x005d0bc4u;                  // _G_LLM_CAM_SCROLL_UP_HELD    (scancode 0x48)
constexpr uintptr_t CAM_HELD_D  = 0x005d0bc8u;                  // _G_LLM_CAM_SCROLL_DOWN_HELD  (scancode 0x50)
constexpr uintptr_t MAP_CAM_COL = mh::addr::_G_LLM_MAP_CAM_COL; // already a registered region
constexpr uintptr_t MAP_CAM_ROW = mh::addr::_G_LLM_MAP_CAM_ROW;
// THE TWO GATE BYTES, and they are on the line because of what the Ghidra read found. The edge
// set/clear block runs only `if (RMB_GESTURE_STATE != 1 && LMB_GESTURE_STATE != 1)`, while the
// apply loops that MOVE the camera sit outside that guard -- so a gesture stranded in state 1
// freezes the latches as they are and the camera keeps scrolling wherever the cursor goes. Without
// these two bytes a log cannot tell that case from a cursor genuinely held at the edge, which is
// the whole question SC2 has to answer from a player's file. 0 = none, 1 = drag begun (the gating
// value), 2 = drag active.
constexpr uintptr_t LMB_GESTURE_STATE = 0x00e589acu; // _G_LLM_LMB_GESTURE_STATE (byte)
constexpr uintptr_t RMB_GESTURE_STATE = 0x00e589adu; // _G_LLM_RMB_GESTURE_STATE (byte)
// U25 step 1: zero here means the DirectInput keyboard died silently at init and the raw-Win32
// WM_KEYDOWN fallback is the producer -- the path that appends every OS auto-repeat. Same
// literal-address reasoning as the block above.
constexpr uintptr_t DI_KEYBOARD_DEVICE = 0x0066155cu; // _G_LLM_DI_KEYBOARD_DEVICE

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
       EV_LUP   = 4,
       EV_RDOWN = 8,
       EV_RUP   = 0x10 };
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

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0; // SES1: 0 = not yet composed (mh_proc_path pins it to the process dir)

// ---- PT-GFX8: `winaway` / `winreturn` -- Alt-Tab away from, and back to, the game window ------------------
//
// A helper THREAD owns a small top-level window of its own. `winaway` makes it the foreground (the game
// gets WM_ACTIVATE(inactive), is NOT minimised -- an Alt-Tab away from a borderless window); `winreturn`
// activates the game from that thread, which holds the foreground and so is allowed to: the switcher's
// path (SW_RESTORE if iconic, then SetForegroundWindow) or, with `launcher`, the retail single-instance
// path (WinMain: FindWindowA + SetForegroundWindow, nothing else). The calls are made on the helper
// thread because the real switcher is another process: nothing here may run on the game's own thread.
struct away_t {
    HANDLE thread = nullptr;
    HWND   win    = nullptr;
    HANDLE ready  = nullptr;
};
away_t g_away;
bool   g_away_synth = false; // the foreground was refused, so the deactivation was delivered as messages

LRESULT CALLBACK away_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP + 1) { // l = the game window, w = 1: the launcher path
        HWND game = (HWND)l;
        if (w == 1) { // WinMain's single-instance path: FindWindowA(class, title) then SetForegroundWindow
            char cls[64] = {0}, title[128] = {0};
            GetClassNameA(game, cls, sizeof(cls));
            GetWindowTextA(game, title, sizeof(title));
            if (HWND found = FindWindowA(cls, title[0] ? title : nullptr)) game = found;
        }
        if (w == 0 && IsIconic(game)) ShowWindow(game, SW_RESTORE);
        SetForegroundWindow(game);
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

DWORD WINAPI away_thread(LPVOID) {
    WNDCLASSA wc     = {};
    wc.lpfnWndProc   = &away_proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "mh_uitest_away";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    g_away.win = CreateWindowExA(0, wc.lpszClassName, "mh uitest away", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 300, 160, nullptr,
                                 nullptr, wc.hInstance, nullptr);
    if (g_away.win) SetForegroundWindow(g_away.win);
    SetEvent(g_away.ready);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_away.win = nullptr;
    return 0;
}

void away_start(HWND game) {
    if (g_away.thread) return;
    g_away.ready  = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_away.thread = CreateThread(nullptr, 0, &away_thread, nullptr, 0, nullptr);
    if (g_away.thread) WaitForSingleObject(g_away.ready, 3000);
    for (int i = 0; i < 50 && GetForegroundWindow() != g_away.win; ++i) Sleep(10); // let the deactivation land
    // A lane on the rig's ISOLATED desktop is not the input desktop: the shell refuses every foreground
    // request there ("foreground REFUSED"), so there is no real deactivation to be had. Deliver what Alt-Tab
    // would -- WM_ACTIVATE(inactive) + WM_ACTIVATEAPP(0) -- so the window's own handlers (the clip release,
    // the shell refresh) run; the style-level eligibility rule is then asserted on the same window.
    g_away_synth = GetForegroundWindow() != g_away.win;
    if (g_away_synth) {
        SendMessageA(game, WM_ACTIVATE, WA_INACTIVE, 0);
        SendMessageA(game, WM_ACTIVATEAPP, 0, 0);
    }
}

void away_return(HWND game, bool launcher) {
    if (!g_away.thread) { // no helper (never went away): the plain activation
        SetForegroundWindow(game);
        return;
    }
    if (g_away_synth) {
        SendMessageA(game, WM_ACTIVATEAPP, 1, 0);
        SendMessageA(game, WM_ACTIVATE, WA_ACTIVE, 0);
    }
    if (g_away.win) SendMessageA(g_away.win, WM_APP + 1, launcher ? 1 : 0, (LPARAM)game);
    WaitForSingleObject(g_away.thread, 3000);
    CloseHandle(g_away.thread);
    CloseHandle(g_away.ready);
    g_away.thread = nullptr;
    g_away.ready  = nullptr;
    // No wait for the foreground here: it lands once the GAME thread (this one) pumps again, so the script's
    // next step is `winactive`, a predicate that polls it.
}

// Wall clock for the log, in ms since the first line. Every line carries it, because the frame counts
// the script already reports answer "how many frames did this wait" and not "how long did this TAKE" --
// and headless changes the frame rate, so the two are not convertible. A step that reads `waited 13422`
// says nothing about whether the run is slow; `+14.2s` does.
DWORD g_log_t0 = 0;

void ui_log(const char *fmt, ...) {
    // SES1: PROCESS-scoped, and this one is the hard case rather than a preference. The runner polls
    // exactly ONE file for `; [script] COMPLETE` / `SIGNAL` / `TIMEOUT`, and it resolves that path
    // once, from the directory that existed at launch (tools/ui_test.py local_new_run /
    // remote_newest_run). A log that followed the session would strand the runner on an empty file
    // the instant a lobby opened: every multi-peer scenario would time out instead of run.
    if (mh_proc_path(g_log, MAX_PATH, "%smh_uidrive.log", &g_log_gen)) g_log_t0 = GetTickCount();
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
    // LOG1: ENQUEUED (the runner polls this file live; the writer flushes within a scheduler wake-up).
    mh_logq_write(g_log, line, lstrlenA(line));
}

// ---- mp:SES7: the mouse/camera trace FOLLOWS THE MATCH (mh_mtrace.log) ---------------------------
//
// WHY A SECOND FILE. Until SES7 every `[mtrace]` row went through ui_log into mh_uidrive.log, which
// is PROCESS-scoped for the rig's sake (see ui_log). A real rc3 report (2026-09-26, a 35-minute
// online match, "the map scroll got stuck") then could not answer its own question: the process file
// held the menu and the lobby as well as the match, passed the launcher's 8 MB per-file cap, and the
// tail-keeping packager cut the first 11.5 minutes of the match. So the trace now has its own stream
// that follows the SESSION through mh_run_path -- a match's rows land in the match's folder, the menu
// phase stays in the process folder. It is NOT size-capped (user decision 2026-09-26): SES7 first
// rotated it at 7 MB to fit the packager's old per-file cap, which again kept only the newest part
// of a long match. The packager now budgets the compressed zip and trims least-important folders
// first (the launcher's report.rs), so each match's file is written whole.
//
// mh_uidrive.log STILL GETS THE ROWS WHEN [uitest] IS ON, and only then. tools/check_cam_trace.py
// reads `[mtrace]` samples out of mh_uidrive.log INTERLEAVED with the script's own `; [script] LOG:`
// markers (its --segment split), so the rig's copy has to stay exactly where it was. A player has
// [uitest] off: nothing reads their mh_uidrive.log for mtrace, and dropping the rows there is what
// keeps that file small in a report.
//
// Every line carries the absolute wall-clock stamp mh_net.log uses (mh_log_stamp), not ui_log's
// relative one: the questions this file answers are "what was the input state when THAT net/chat
// line happened", so it must interleave with the session's other logs by eye.
char          g_mt_path[MAX_PATH];
unsigned long g_mt_gen = 0;

void mt_write_header(const char *path); // below the trace state it reads

// One line into mh_mtrace.log (`text` without a newline). LOG1: enqueued to the async sink -- this
// runs on the present thread and used to open/append/close the file per line (the 638fc214 hitches
// sat between two consecutive lines of this very file).
void mt_write(const char *text) {
    const bool rebuilt = mh_run_path(g_mt_path, MAX_PATH, "%smh_mtrace.log", &g_mt_gen);
    if (g_mt_path[0] == '\0') return;
    if (rebuilt) mt_write_header(g_mt_path); // a session's file is readable on its own
    char line[600];
    int  n = mh_log_stamp(line);
    lstrcpynA(line + n, text, (int)sizeof(line) - n - 2);
    n         = lstrlenA(line);
    line[n++] = '\n';
    line[n]   = 0;
    mh_logq_write(g_mt_path, line, n);
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
//
// ---- ROUND 3 (mp:SES3, 2026-09-17): the CAMERA LATCHES ride the same line ----------------------
//
// The stuck-strategic-scroll report (SC2) does not reproduce here, so the fix has to come out of a
// player's log -- which means the trace has to already carry the state that decides it. Three new
// fields, all read from the block llm_strat_input_update owns:
//
//   edge=LRUD  the four _G_LLM_CAM_EDGE_*_ACTIVE flags (cursor at a screen edge)
//   held=LRUD  the four _G_LLM_CAM_SCROLL_*_HELD latches (an arrow key is down)
//   cam=col,row  _G_LLM_MAP_CAM_COL / _G_LLM_MAP_CAM_ROW -- what those latches MOVE
//
// A set flag prints its letter, a clear one prints '-', so `edge=-R--` reads at a glance and a
// transition is a diff of two adjacent lines. WHY BOTH HALVES: the latch says what the game
// BELIEVES the input is, the camera says what it DID with that belief -- a latch stuck on with the
// camera moving is the bug, a latch stuck on with the camera pinned at a map edge is not.
//
// SAMPLING, and the cost clause it answers. The line is a diagnostic a player is asked to leave on,
// so a frame where nothing changes must cost nothing and a stuck scroll must not cost one line per
// frame for minutes. Three triggers, in order of how much they matter:
//   1. a LATCH TRANSITION always prints -- that is the event the whole item exists to capture;
//   2. the mouse-ring conditions print as before (produced / depth / a moving cursor);
//   3. the camera moving with the latches UNCHANGED is coalesced to at most one line per
//      CAM_HEARTBEAT_MS, carrying `camd=` (how many tiles it moved since the last printed line) so
//      the coalescing loses the cadence but not the distance.
// Everything else increments the quiet counter, which still prints one summary line when activity
// resumes.
constexpr DWORD CAM_HEARTBEAT_MS = 500; // rate cap for the "still scrolling, nothing changed" line

// mp:SES3c (Wave 2): true while a harness `keyhold` is holding its key down. A held key injects
// nothing after its DOWN, so without this every hold frame is QUIET and a headless hold (hundreds of
// presents in well under one CAM_HEARTBEAT_MS) prints ONE sample -- the rig run showed held=L---
// latched f=84558..85157 with a single line inside it. `cursorhold` never had this problem because
// it injects a move every present (produced>0). Harness-only: nothing sets it outside `keyhold`.
static bool g_keyhold_trace = false;

// Pack the eight latches into one byte: bit0..3 = edge L,R,U,D; bit4..7 = held L,R,U,D.
inline uint32_t cam_latch_mask() {
    const uintptr_t edge[4] = {CAM_EDGE_L, CAM_EDGE_R, CAM_EDGE_U, CAM_EDGE_D};
    const uintptr_t held[4] = {CAM_HELD_L, CAM_HELD_R, CAM_HELD_U, CAM_HELD_D};
    uint32_t        m       = 0;
    for (int i = 0; i < 4; ++i) {
        if (*(volatile uint32_t *)edge[i]) m |= 1u << i;
        if (*(volatile uint32_t *)held[i]) m |= 1u << (i + 4);
    }
    return m;
}

// "LRUD" with a '-' for each clear bit, from the low nibble of `bits`.
inline void cam_latch_str(uint32_t bits, char out[5]) {
    static const char L[4] = {'L', 'R', 'U', 'D'};
    for (int i = 0; i < 4; ++i) out[i] = (bits & (1u << i)) ? L[i] : '-';
    out[4] = 0;
}

// mp:SES7: the two context facts a stuck latch is most often explained by, sampled per traced
// present and printed ONLY on a transition (into mh_mtrace.log alone -- mh_uidrive.log's format is
// the rig's and does not grow). FOCUS: the game losing the foreground while an arrow key is down is
// the classic way a key-UP never arrives and a HELD latch stays set. DI KEYBOARD: the device pointer
// going null mid-run means the raw WM_KEYDOWN fallback took over (U25). Both are one cheap read.
int g_mt_focus = -1; // -1 = not sampled yet
int g_mt_dikbd = -1;

int mt_focus_now() {
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

void mt_write_header(const char *path) {
    char line[512];
    int  n = mh_log_stamp(line);
    wsprintfA(line + n,
              "; [mtrace-ctx] OPEN %s (session=%d) mouse_absolute=%d mouse_div=%d mouse_accel=%d "
              "di_mouse=%s di_keyboard=%d focus=%d -- row: f=present# produced/depth/max=mouse "
              "ring, last=DI accumulator, cur=drawn cursor, edge/held=camera latches LRUD, cam=col,row, "
              "camd=tiles since last row, gest=LMB/RMB gesture state\n",
              MH_RunDir(), MH_RunDir_SessionActive(), g_mouse_absolute, g_mouse_div, g_mouse_accel,
              *(volatile uint32_t *)DI_MOUSE_DEVICE ? "on" : "off",
              *(volatile uint32_t *)DI_KEYBOARD_DEVICE ? 1 : 0, mt_focus_now());
    mh_logq_write(path, line, lstrlenA(line));
}

// One `[mtrace]` row: always into the session-following mh_mtrace.log, and into mh_uidrive.log
// exactly when [uitest] is on (check_cam_trace's reader -- see mt_write's banner).
void mt_line(const char *fmt, ...) {
    char    buf[480];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    mt_write(buf);
    if (g_enabled) ui_log("%s", buf);
}

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

    // SES3: the camera half. `s_first` exists so the very first sample prints rather than being
    // diffed against a zero that means "not read yet" -- a run that starts with a latch already set
    // would otherwise never show it.
    static uint32_t s_prev_mask = 0;
    static int      s_prev_col = 0, s_prev_row = 0;
    static DWORD    s_last_cam_ms = 0;
    static bool     s_first       = true;
    static int      s_camd        = 0; // camera tiles moved since the last PRINTED line
    // The two gate bytes ride in the high half of the same mask, so a GESTURE change is a
    // transition the sampler must print: it is the other way a latch stops being cleared, and a
    // log that shows the latch without the gate cannot tell the two apart.
    const uint32_t mask = cam_latch_mask() | ((uint32_t)*(volatile uint8_t *)LMB_GESTURE_STATE << 8) |
                          ((uint32_t)*(volatile uint8_t *)RMB_GESTURE_STATE << 16);
    const int  col = *(volatile int *)MAP_CAM_COL, row = *(volatile int *)MAP_CAM_ROW;
    const bool latched  = (mask != s_prev_mask) || s_first;
    const bool cam_move = (col != s_prev_col) || (row != s_prev_row);
    if (cam_move && !s_first) ++s_camd;
    s_prev_mask = mask, s_prev_col = col, s_prev_row = row;
    const DWORD now = GetTickCount();
    // The camera-only heartbeat: rate-capped, so a scroll that never stops stays readable.
    const bool cam_beat = cam_move && (now - s_last_cam_ms) >= CAM_HEARTBEAT_MS;

    // mp:SES7: a SESSION BOUNDARY forces a full sample, so every match's mh_mtrace.log opens on the
    // complete state (latches, gesture bytes, camera) rather than on whatever changes next -- a latch
    // already stuck when the match's folder opened would otherwise never appear in that folder.
    static unsigned long s_gen  = 0;
    const unsigned long  gen    = MH_RunDirGeneration();
    const bool           newdir = gen != s_gen;
    s_gen                       = gen;
    const int  focus            = mt_focus_now();
    const int  dikbd            = *(volatile uint32_t *)DI_KEYBOARD_DEVICE ? 1 : 0;
    const bool ctx              = focus != g_mt_focus || dikbd != g_mt_dikbd;

    if (produced == 0 && depth == 0 && !moved && !latched && !cam_beat && !g_keyhold_trace && !newdir &&
        !ctx) {
        ++s_quiet;
        s_prev_w = w; // update on the quiet path too, or `produced` lies after a still period
        return;
    }
    if (s_quiet) {
        mt_line("; [mtrace] %u quiet frame(s)", s_quiet);
        s_quiet = 0;
    }
    if (ctx) {
        char cl[128];
        wsprintfA(cl, "; [mtrace-ctx] focus=%d di_keyboard=%d (was %d/%d)", focus, dikbd, g_mt_focus,
                  g_mt_dikbd);
        mt_write(cl); // mh_mtrace.log only: mh_uidrive.log's rows are the rig's format
        g_mt_focus = focus;
        g_mt_dikbd = dikbd;
    }
    // ROUND 2 (2026-08-24): round 1 showed depth=0/max=0 and produced=2..6, i.e. the RING is not the
    // backlog and neither is the DI buffer -- and it also showed the sample point is blind, because
    // it is taken at present, AFTER the pump has drained. So trace the state that survives a frame:
    // the DI accumulator, the drawn cursor, and which pump arm is live.
    char edge_s[5], held_s[5];
    cam_latch_str(mask, edge_s);
    cam_latch_str(mask >> 4, held_s);
    mt_line("; [mtrace] f=%u produced=%u depth=%u max=%u di=%s last=%d,%d cur=%d,%d vis=%d "
            "edge=%s held=%s cam=%d,%d camd=%d gest=%d%d",
            s_frame, produced, depth, s_max_dep, *(volatile uint32_t *)DI_MOUSE_DEVICE ? "on" : "off",
            lx, ly, cx, cy, (int)*(volatile uint32_t *)CURSOR_VISIBLE, edge_s, held_s, col, row, s_camd,
            (int)*(volatile uint8_t *)LMB_GESTURE_STATE, (int)*(volatile uint8_t *)RMB_GESTURE_STATE);
    s_prev_w      = w;
    s_camd        = 0;
    s_first       = false;
    s_last_cam_ms = now;
}

// One line into mh_net.log, the log a crash/bug report actually carries (mh_uidrive.log is the
// harness's own file and a player has no reason to have one). Same shape as mp_menu.cpp's
// menu_log -- session-scoped through mh_run_path, append-only, no rotation concern for a one-shot.
void net_log_line(const char *text) {
    static char          path[MAX_PATH];
    static unsigned long gen = 0;
    mh_run_path(path, MAX_PATH, "%smh_net.log", &gen);
    char line[224];
    lstrcpynA(line, text, (int)sizeof(line) - 2);
    int n     = lstrlenA(line);
    line[n++] = '\n';
    line[n]   = 0;
    mh_logq_write(path, line, n);
}

// ---- U25 step 1: say ONCE, in the log a player sends us, which keyboard path this machine ran ---
//
// llm_input_dinput_keyboard_init (0x004d0948) has five ways to fail (LoadLibraryA,
// DirectInputCreateA, CreateDevice, SetDataFormat/SetProperty, SetCooperativeLevel); every one of
// them Releases the device, NULLs _G_LLM_DI_KEYBOARD_DEVICE and returns with no error path and no
// log line. On such a machine llm_input_wndproc_tap's raw WM_KEYDOWN arm is the producer, and that
// arm never tests lParam bit 30 -- so every OS auto-repeat is appended as a fresh key-down (U25).
// Nothing observed has hit it, which is exactly why a report from a machine in that state has to
// diagnose itself instead of costing a session of guesswork.
//
// WHERE THE READ IS VALID, since the U25 note left it open: NOT at install time -- mh.dll binds
// during the loader, long before the game builds its DirectInput devices. The first PRESENT is the
// earliest point that is unambiguously after llm_game_init_subsystems, so the one-shot fires there.
// It is NOT gated behind [input] mouse_trace: one line per run is what makes every future report
// self-describing, and a knob nobody set would make it zero.
void log_di_keyboard_once() {
    static bool s_done = false;
    if (s_done) return;
    s_done             = true;
    const uint32_t dev = *(volatile uint32_t *)DI_KEYBOARD_DEVICE;
    char           line[192];
    wsprintfA(line, "; [input] di_keyboard=%d dev=0x%08x -- %s", dev ? 1 : 0, dev,
              dev ? "DirectInput keyboard live (no OS auto-repeat)"
                  : "FALLBACK: raw WM_KEYDOWN path, OS auto-repeat UNFILTERED (U25)");
    net_log_line(line);
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
    auto *ev = (mh_llm_input_mouse_event *)(EVENTS + (size_t)w * sizeof(mh_llm_input_mouse_event));
    // The wndproc's own snapshot: bit 0 left / bit 1 right, held during DOWN, cleared by UP; only the
    // in-game delta pump reads it.
    ev->buttons     = (type == EV_LDOWN) ? 1u : (type == EV_RDOWN) ? 2u
                                                                   : 0u;
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
// ---- THE KEY-INJECTION JOURNAL (F4) ------------------------------------------------------------
// Every key event this driver writes into the ring is recorded here exactly as it was written:
// scancode + event_type, which ARE the injected input. The timestamp is deliberately NOT recorded --
// it is a DLL-side monotonic counter that exists only to keep the poll from mis-reading a
// double-click, so including it would make two identical keystrokes compare unequal.
//
// It exists so `type` is PROVABLE rather than merely plausible. `type host1` and the five `key`
// lines that spell the same string must put a byte-identical event sequence into the ring, and that
// is an assertion the harness can make about ITSELF -- no pixels, no second run to diff against, and
// no reliance on the field rendering (the Cyrillic case has no glyphs at all). `keyjournal mark`
// cuts a segment; `keyjournal same` asserts the last two are identical and ABORTS the script if not.
struct KeyEvRec {
    uint16_t scancode;
    uint16_t event_type;
};
constexpr int KEYJ_MAX = 256, KEYJ_SEGS = 8;
KeyEvRec      g_keyj[KEYJ_MAX];
int           g_keyj_n = 0;
int           g_keyj_seg[KEYJ_SEGS]; // start index of each segment
int           g_keyj_nseg = 0;

bool enqueue_key(uint32_t scancode, bool down) {
    uint32_t w    = *(volatile uint32_t *)KWRITEIDX;
    uint32_t next = (w + 1) & 0x7f;
    if (next == *(volatile uint32_t *)KREADIDX) return false; // ring full -> drop
    auto *ev       = (mh_llm_input_key_event *)(KEVENTS + (size_t)w * sizeof(mh_llm_input_key_event));
    ev->scancode   = scancode;
    ev->event_type = down ? 0x100u : 0x80u;
    // A KEY EVENT'S TIMESTAMP IS LOAD-BEARING, AND IT COST THIS ACTION A WHOLE SESSION (F4).
    // The mouse ring's timestamp is only read for double-click timing, which is why the driver gives
    // mouse events a private counter with big gaps. A KEY event's timestamp is a scheduling input:
    // llm_input_key_dequeue_translate_ascii copies the WHOLE 0x38-byte event into KEYREC, and
    // KEYREC+0x20 is the field named `_G_LLM_UI_MODAL_STEP_DEADLINE_MS` (0x006542ea) -- the two
    // are the SAME ADDRESS (0x006542ea), so the pump's `publish when NEXT_MS >= DEADLINE_MS` really
    // reads "publish this key once the 150 ms accumulator has caught up to WHEN IT WAS PRESSED".
    // With the counter's value that meant ~164 SECONDS in the future: the first key after a screen
    // settle published anyway (the settle parks NEXT_MS at 0xffffffff), and every key after it sat
    // in the ring undelivered. `key` therefore only ever delivered ONE keystroke per screen
    // transition -- invisible until now because no script had used two in a row.
    // So: the game's own master ms clock, the same clock _G_LLM_UI_MENU_NOW_MS is sampled from and
    // the same one a real producer stamps a keypress with.
    ev->timestamp                   = mh::call::llm_time_get_ticks_ms();
    *(volatile uint32_t *)KWRITEIDX = next;
    if (g_keyj_n < KEYJ_MAX) {
        g_keyj[g_keyj_n].scancode   = (uint16_t)scancode;
        g_keyj[g_keyj_n].event_type = (uint16_t)(down ? 0x100u : 0x80u);
        ++g_keyj_n;
    }
    return true;
}

// The key ring is EMPTY -- i.e. every event we pushed has been dequeued, and dequeue IS translate
// (llm_input_key_dequeue_translate_ascii pops and resolves in one call). That makes it the exact
// "the previous character has been consumed" predicate `type` needs before it may change the
// modifier state for the next one. A pure state test: no frame count, no sleep.
bool key_ring_empty() { return *(volatile uint32_t *)KREADIDX == *(volatile uint32_t *)KWRITEIDX; }

// One journal segment, verbatim, into the log -- the EVIDENCE behind `keyjournal same`. An assertion
// that only prints its verdict is an assertion you have to take on trust; these two lines are what a
// reader compares by eye when the verdict is questioned.
void log_keyj(const char *tag, int from, int to) {
    char line[480];
    int  k = wsprintfA(line, "; [script] keyjournal %s [%d..%d) %d event(s):", tag, from, to, to - from);
    for (int i = from; i < to && k < (int)sizeof(line) - 16; ++i)
        k += wsprintfA(line + k, " %02x/%03x", g_keyj[i].scancode, g_keyj[i].event_type);
    ui_log("%s", line);
}

// ---- KEYBOARD LAYOUT + TEXT TYPING (F4) --------------------------------------------------------
// `type <utf8 text>` drives the game's OWN keyboard path -- it does not write the text field. Each
// character is resolved to the (scancode, modifier) pair a player would physically press on the
// ACTIVE layout, injected into the key ring, and translated by the game's own
// llm_input_key_dequeue_translate_ascii (MapVirtualKeyA -> GetKeyboardState -> ToAscii).
//
// TWO HALVES OF A KEYSTROKE, AND ONLY ONE OF THEM FITS IN THE RING.
//   * the KEY itself  -> the ring, as `key` already does.
//   * the MODIFIERS   -> NOT the ring. The game's translate reads shift/ctrl/alt from
//     GetKeyboardState, the OS's per-thread key-state table, never from the event it just popped --
//     so a synthetic Shift event in the ring would change nothing about the translation. The
//     faithful move is the one a real Shift press actually causes: put the modifier into that table
//     (SetKeyboardState) for exactly as long as the character is in flight. It is also why we do NOT
//     inject a 0x2a event: it would add ring traffic whose only effect is to exercise the game's
//     ToAscii-failure fallback table, and it would break the byte-identity `type` owes `key`.
//
// The layout is loaded for the run by `layout <klid>` and restored when the script ends. Resolution
// is VkKeyScanExW + MapVirtualKeyExW, so it is table-driven from the layout itself rather than a
// hardcoded scancode map -- ASCII on 00000409, Cyrillic on 00000419 and Polish on 00000415 all fall
// out of the same code, and a character the layout cannot produce is a reported script ERROR.
struct TypeChar {
    wchar_t wc;
    uint8_t vk;
    uint8_t sh;     // VkKeyScanEx shift state: 1 shift, 2 ctrl, 4 alt (6 = AltGr)
    uint8_t sc;     // PC set-1 scancode
    uint8_t native; // what the LAYOUT's own codepage says this key produces (ToAsciiEx)
    uint8_t acp;    // what the GAME's ToAscii will store (CP_ACP) -- see the F3 note in do_action
};
constexpr int TYPE_MAX = 48;
TypeChar      g_type[TYPE_MAX];
int           g_type_n        = 0;
int           g_type_i        = 0;
bool          g_type_pushed   = false; // character g_type_i is in the ring and not yet consumed
int           g_type_step     = -1;    // script step index this resolution belongs to
DWORD         g_type_progress = 0;     // wall clock of the last character actually pushed
DWORD         g_type_diag_ms  = 0;     // throttle for the pump-state diagnostic below
int           g_type_stall_ms = 15000;

HKL g_hkl_orig = nullptr; // whatever the process had before the script touched it
HKL g_hkl_cur  = nullptr;

bool g_kb_touched = false; // this script has used `type`/`layout` -- so there IS something to restore

// Put the modifier keys into the OS key-state table the game's ToAscii will read. Everything else in
// the table is preserved -- we are adding a held Shift, not inventing a keyboard.
void apply_mod_state(uint8_t sh) {
    g_kb_touched = true;
    BYTE st[256];
    if (!GetKeyboardState(st)) return;
    const BYTE dn   = 0x80;
    st[VK_SHIFT]    = (sh & 1) ? dn : 0;
    st[VK_LSHIFT]   = (sh & 1) ? dn : 0;
    st[VK_RSHIFT]   = 0;
    st[VK_CONTROL]  = (sh & 2) ? dn : 0;
    st[VK_LCONTROL] = (sh & 2) ? dn : 0;
    st[VK_RCONTROL] = 0;
    st[VK_MENU]     = (sh & 4) ? dn : 0;
    st[VK_LMENU]    = 0;
    st[VK_RMENU]    = (sh & 4) ? dn : 0; // AltGr is reported by VkKeyScanEx as ctrl+alt
    SetKeyboardState(st);
}

// Restores BOTH halves of what typing touched, and does nothing at all for the scripts that never
// typed -- the scenarios in this suite that only click must not have their key-state table rewritten
// on the way out just because the exit path is shared.
void restore_layout() {
    if (!g_kb_touched) return;
    if (g_hkl_orig && g_hkl_cur && g_hkl_cur != g_hkl_orig) {
        ActivateKeyboardLayout(g_hkl_orig, KLF_SETFORPROCESS);
        ActivateKeyboardLayout(g_hkl_orig, 0);
        ui_log("; [script] layout restored to %08x", (unsigned)(uintptr_t)g_hkl_orig);
    }
    g_hkl_cur = g_hkl_orig;
    apply_mod_state(0);
}

// `layout <klid>` / `layout default`. The ORIGINAL is recorded on the first call and restored on
// `default`, at script end and on every abort path -- a run that leaves the box on a Russian layout
// is a side effect nobody asked for.
bool set_layout(const char *klid) {
    if (!g_hkl_orig) g_hkl_orig = GetKeyboardLayout(0);
    g_kb_touched = true;
    if (!klid || !*klid || lstrcmpiA(klid, "default") == 0 || lstrcmpA(klid, "0") == 0) {
        restore_layout();
        return true;
    }
    HKL h = LoadKeyboardLayoutA(klid, KLF_ACTIVATE);
    if (!h) {
        ui_log("; [script] layout '%s' FAILED to load (GetLastError=%lu)", klid, GetLastError());
        return false;
    }
    ActivateKeyboardLayout(h, KLF_SETFORPROCESS); // best effort: cover threads we are not on
    ActivateKeyboardLayout(h, 0);
    g_hkl_cur = h;
    HKL live  = GetKeyboardLayout(0);
    ui_log("; [script] layout '%s' -> HKL %08x (active on tid %lu: %08x)%s", klid,
           (unsigned)(uintptr_t)h, GetCurrentThreadId(), (unsigned)(uintptr_t)live,
           live == h ? "" : "  -- MISMATCH: the present thread is not the thread that translates");
    return live == h;
}

// Resolve a whole UTF-8 string to per-character (scancode, modifiers) against the ACTIVE layout.
// Resolved UP FRONT, so an unproducible character aborts before half the word has been typed.
// Returns false and logs the offending character on any failure.
bool type_resolve(const char *utf8) {
    g_type_n = g_type_i = 0;
    g_type_pushed       = false;
    wchar_t wbuf[TYPE_MAX + 1];
    int     n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wbuf, TYPE_MAX + 1);
    if (n <= 1) {
        ui_log("; [script] type: '%s' is not valid UTF-8 or is empty (MultiByteToWideChar=%d, err=%lu)",
               utf8, n, GetLastError());
        return false;
    }
    n -= 1; // drop the NUL
    HKL hkl = GetKeyboardLayout(0);
    for (int i = 0; i < n; ++i) {
        SHORT vks = VkKeyScanExW(wbuf[i], hkl);
        if (vks == -1) {
            ui_log("; [script] type: character U+%04X (#%d of '%s') is NOT PRODUCIBLE on the active "
                   "keyboard layout %08x -- a script ERROR, not a character to drop",
                   (unsigned)wbuf[i], i + 1, utf8, (unsigned)(uintptr_t)hkl);
            return false;
        }
        TypeChar &t = g_type[i];
        t.wc        = wbuf[i];
        t.vk        = (uint8_t)(vks & 0xff);
        t.sh        = (uint8_t)((vks >> 8) & 0xff);
        UINT sc     = MapVirtualKeyExW(t.vk, MAPVK_VK_TO_VSC, hkl);
        if (sc == 0 || sc > 0x7f) {
            ui_log("; [script] type: U+%04X maps to VK %02x which has no set-1 scancode on layout "
                   "%08x (MapVirtualKeyEx=%u)",
                   (unsigned)wbuf[i], t.vk, (unsigned)(uintptr_t)hkl, sc);
            return false;
        }
        t.sc = (uint8_t)sc;
        // What this keystroke WILL become, measured on both codecs, because they disagree and the
        // difference is a real defect this action is built to make visible (F3):
        //   native = ToAsciiEx under the layout's own codepage   -- e.g. CP1251 0xCF for U+041F
        //   acp    = ToAscii, which is what the GAME calls       -- the process ANSI codepage
        // On a CP1252 box every Cyrillic character comes back 0x3F '?' from the game's codec while
        // the layout resolves it perfectly. The scancode path is right; the CODEC is the gap.
        BYTE st[256];
        memset(st, 0, sizeof(st));
        if (t.sh & 1) st[VK_SHIFT] = st[VK_LSHIFT] = 0x80;
        if (t.sh & 2) st[VK_CONTROL] = st[VK_LCONTROL] = 0x80;
        if (t.sh & 4) st[VK_MENU] = st[VK_RMENU] = 0x80;
        BYTE out[8];
        memset(out, 0, sizeof(out));
        UINT gvk = MapVirtualKeyExA(t.sc, MAPVK_VSC_TO_VK, hkl); // the game's own first step
        int  rn  = ToAsciiEx(gvk, 0, st, (LPWORD)out, 0, hkl);
        if (rn < 0) ToAsciiEx(gvk, 0, st, (LPWORD)out, 0, hkl); // flush a dead key
        t.native = (rn == 1) ? out[0] : 0;
        memset(out, 0, sizeof(out));
        rn = ToAscii(gvk, 0, st, (LPWORD)out, 0);
        if (rn < 0) ToAscii(gvk, 0, st, (LPWORD)out, 0);
        t.acp = (rn == 1) ? out[0] : 0;
    }
    g_type_n = n;
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

// A NON-ASCII needle (a UTF-8 script label -- mods:LANG3's RU-pack scenarios, 2026-09-29) is matched
// against the label as UTF-16, code unit for code unit. extract_label's ASCII fold above turns every
// Cyrillic character into '?' (and a Cyrillic wide string, whose 2nd byte is 0x04, is not even read as
// wide), so under a language pack no dialog label could be matched by its text at all. The label is
// read as wide only when it looks wide -- 2nd byte 0 (Latin) or 0x04 (the Cyrillic block) -- so an ANSI
// label is never scanned past its NUL; exact match, no case fold (the needle is copied from the pack's
// own initlang TEXT).
static bool wide_label_matches(const char *label, const char *utf8) {
    if (!(label[0] != 0 && (label[1] == 0 || label[1] == 0x04))) return false;
    wchar_t needle[80];
    int     n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, needle, 80);
    if (n <= 1) return false;
    --n; // code units, without the NUL
    const uint16_t *w   = (const uint16_t *)label;
    int             len = 0;
    while (len < 128 && w[len]) ++len;
    for (int i = 0; i + n <= len; ++i) {
        int k = 0;
        while (k < n && w[i + k] == (uint16_t)needle[k]) ++k;
        if (k == n) return true;
    }
    return false;
}

bool label_matches(const char *label, const char *substr) {
    if (!label || !substr || !*substr) return false;
    for (const unsigned char *p = (const unsigned char *)substr; *p; ++p)
        if (*p >= 0x80) return wide_label_matches(label, substr);
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
// 2026-09-20: the harness's synthesised key state (see the `hotkey` verb below). Pollers OR this
// into their GetAsyncKeyState read; it is 0 for every key unless a running script holds a chord.
namespace {
uint8_t g_synth_vk[256] = {0};
} // namespace
extern "C" int MH_UIDrive_SynthKeyDown(int vk) {
    return (vk > 0 && vk < 256 && g_synth_vk[vk] != 0) ? 1 : 0;
}

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

// ---- mp:D25 (2026-09-19): the RIGHT button, optionally under a held Shift --------------------------
//
// Shift+right-click on the map is how a player LANDS the mothership (deploy: order 0x10 move + 0x18
// deploy; states 0x18 DEPLOY_APPROACH -> 0x17 DEPLOY_TO_BUILDING), and a fresh MP human owns nothing else -- no landing, no
// base, no build menu, so no scenario could reach llm_strat_bldg_try_begin_placement's affordability
// probe until this existed. Two facts decide the shape, both measured before by the journal replay
// (mh_harness/harness.cpp, "THE KEYSTATE ARRAY IS A SECOND INPUT CHANNEL"): the strategic input
// reads Shift from _G_LLM_INPUT_KEYSTATE[0x2a] (bit 0), NOT from the key ring, and a DirectInput
// keyboard re-latches that array from the real device every poll -- so the byte is asserted on
// EVERY present the click is in flight, exactly the replay's re-assert convention, and cleared at
// the end so a held key cannot stick. The button events are spread over presents like a physical
// click (MOVE, then DOWN, then UP), for the same reason `press`/`release` exist.
constexpr uint32_t SC_LSHIFT = 0x2a;
void               keystate_shift(bool held) {
    volatile uint8_t *ks = (volatile uint8_t *)mh::addr::_G_LLM_INPUT_KEYSTATE;
    ks[SC_LSHIFT]        = held ? (uint8_t)(ks[SC_LSHIFT] | 5u) : (uint8_t)(ks[SC_LSHIFT] & 0xfeu);
}
// One frame of the sequence; returns true when it is complete. `frame` is the caller's per-step wait
// counter (0 on the first present the step runs).
bool rclick_frame(int frame, int x, int y, bool shift) {
    if (shift) keystate_shift(true);
    switch (frame) {
        case 0: enqueue(EV_MOVE, x, y); return false;
        case 1: enqueue(EV_RDOWN, x, y); return false;
        case 2: enqueue(EV_RUP, x, y); return false;
        default:
            if (shift) keystate_shift(false);
            return true;
    }
}
extern "C" void MH_UIDrive_Release(int x, int y) { enqueue(EV_LUP, x, y); }

// `simclick <x> <y>` (tooling:TL-SUITE-SPLICE-HOSTCLICK). An in-game press/release one present apart
// was lost under suite load (TL-GATE-LOADFLAKE-0925: HUD Menu, status X, the mother's select click)
// while the same pair passes alone. This holds the button like a hand does: MOVE, DOWN, then the
// cursor re-asserted every present until the SIM has advanced SIMCLICK_HOLD_MS and at least two
// presents passed, UP, and the same again after it -- so whatever consumes the button, per present
// or per sim step, sees it down and then up. Bounded: a stalled sim (a modal that opened on the
// DOWN) releases after SIMCLICK_MAX_FRAMES presents instead of waiting.
constexpr int SIMCLICK_HOLD_MS    = 30; // three 10 ms sim steps
constexpr int SIMCLICK_MAX_FRAMES = 30; // a modal that opens on the DOWN stalls the sim
static double g_simclick_clk0     = 0.0;
static int    g_simclick_phase = 0, g_simclick_f0 = 0;
static double simclick_clock_ms() { // the `gameclock` global, game-SECONDS
    double clk;
    memcpy(&clk, (const void *)mh::addr::_G_LLM_STRAT_GAME_CLOCK, sizeof(clk));
    return clk * 1000.0;
}
// `simrclick <x> <y>` (mp:X2g) is the same hold with the RIGHT button. The HUD's row widgets open an
// entity's info screen on a right-click only when BOTH llm_ui_hud_slot_rmb_click_gate's latches
// (press and release, 0x00413251) saw the button inside the rect; the three-present `rclick` was not
// seen by them at all (measured: four tries on the build tab's Academy row, no info screen), the
// hand-like hold is.
// `simrclick <x> <y> shift` (mp:D37) holds the game's Shift latch for the whole hold, the way `rclick
// ... shift` does -- the mothership's landing order (shift+right-click) was lost under suite load with
// the three-present `rclick`.
static bool simclick_frame(int frame, int x, int y, bool right = false, bool shift = false) {
    if (shift) keystate_shift(true);
    if (frame == 0) {
        g_simclick_phase = 0;
        enqueue(EV_MOVE, x, y);
        return false;
    }
    enqueue(EV_MOVE, x, y); // the in-game cursor resets every pump unless an event re-asserts it
    const bool held = frame - g_simclick_f0 >= 2 &&
                      simclick_clock_ms() - g_simclick_clk0 >= (double)SIMCLICK_HOLD_MS;
    const bool cap = frame - g_simclick_f0 >= SIMCLICK_MAX_FRAMES;
    switch (g_simclick_phase) {
        case 0:
            enqueue(right ? EV_RDOWN : EV_LDOWN, x, y);
            g_simclick_phase = 1;
            break;
        case 1:
            if (!held && !cap) return false;
            enqueue(right ? EV_RUP : EV_LUP, x, y);
            g_simclick_phase = 2;
            break;
        default:
            if ((held || cap) && shift) keystate_shift(false);
            return held || cap;
    }
    g_simclick_clk0 = simclick_clock_ms();
    g_simclick_f0   = frame;
    return false;
}

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
//           enabled <target>   hovered <target>   gamemode <N>   tactsel <N>   bldgsel <N|any|none>
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
//     bldgsel <N|any|none> = the strategic HUD's selected building (_G_LLM_STRAT_UI_SELECTED_BLDG_INDEX,
//                0 = none) is N / any nonzero / none. mp:D37a: the HUD's building panel is not a widget
//                list either, so this is what says "the select click took" (any) and "the selected
//                building left the map" (none: llm_strat_bldg_unmap_footprint clears it, e.g. a
//                pioneer's take-off) -- the `retry` gates for the lift-off walk.
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
//     team <SLOT> <V> = THIS peer's view of slot SLOT's TEAM byte (+0x0c) == V (0 "-", 1..4 T1..T4). mp:U51.
//     lobbymode <V> = THIS peer's view of the lobby MODE (host slot 0 +0x07): 0 FFA, 1 Team. mp:U51.
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
//     lobbyping [N] = N occupied lobby slot rows are showing a REAL ping number on THIS peer's screen
//                (default 1; mh/ui/lobby_ping.cpp's own per-tick count, its own row never among them).
//                mp:L1f's gate, and it is a claim rather than a timer: the transport is a
//                client-server STAR, so a CLIENT can measure exactly one row itself (the host's) --
//                `lobbyping 2` on a client is reachable only once the HOST'S PUBLISHED summary has
//                arrived and been rendered. A 3-peer scenario therefore gates its lobby capture on
//                the mechanism having run rather than on a frame budget, which is the difference
//                between a row that proves something and one that usually does.
//     lobbygen [N] = MH_Seam_S3LobbyGen() >= N (default 1): this client has stored SESSION_INFO for at
//                least N DISTINCT lobbies (by lobby id) since boot. mp:GS1: a host that cancels and
//                re-creates while the client browses replaces the row in place -- `sessions` never dips,
//                so only the generation can say "the lobby listed now is the one made AFTER the churn".
//     field <name|game|chat|lobby> <text|hex:..> = that text BUFFER holds exactly these bytes. `name`
//                and `game` are the NET_SETUP menu fields; `chat` (mp:F3) is the in-game chat edit line
//                and `lobby` the lobby's chat edit field (both UTF-8 since mp:MP-LANG), each read BEFORE
//                the Enter that submits it (submitting clears the line).
//     wlobby <text|hex:..> = SOME line of the lobby's text log (the list llm_lobby_announce_line
//                appends to: joins, leaves, every chat line) CONTAINS this UTF-16 sequence -- the
//                receiving half of a LOBBY chat assertion. Plain text is read as UTF-8.
//     wmsg <text|hex:..> = the NEWEST on-screen floating message (MESSAGE_QUEUE slot 0, a UTF-16
//                string) CONTAINS this UTF-16 sequence. The RECEIVING half of a chat assertion, and
//                it has to be its own predicate for two reasons `field` cannot meet: the buffer is
//                UTF-16 (every second byte of ASCII text is 0x00, which `field`'s NUL-terminated
//                compare reads as end-of-buffer), and the line the game shows is "<player>: <text>",
//                so the assertion is CONTAINS rather than equals. `hex:` bytes are the UTF-16LE code
//                units; plain text is widened as Latin-1, which covers ASCII needles.
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
// `bldgsel` predicate (mp:D37a): the strategic HUD's selected building index, uint16, 0 = none. Read
// through the region registry for the same SB-HOSTFREE reason as TACTSELCNT_ADDR.
inline uintptr_t BLDGSEL_ADDR() {
    return mh::state::live_base(mh::state::RID_STRAT_UI_SELECTED_BLDG_INDEX);
}
constexpr int BLDGSEL_ANY = -1; // `bldgsel any` -- nonzero; `bldgsel none` is a == 0
// `occ <N>` predicate: count OCCUPIED lobby slots (a player is really seated on THIS peer's view). Reads the
// live lobby slot table directly -- the same _G_LLM_LOBBY_SLOTS the S7 occ/cap math uses. slot_status @ +0x0b:
// OPEN=0 HUMAN=1 AI=2 CLOSED=3; occupied = HUMAN|AI. Bound by the map's player-slot count (current_map_data+8).
// This is the state that lags a re-JOIN snapshot -- gating a capture on `occ N` makes the frame deterministic
// (fires only once the peer's own slot is seated+rebuilt), where `settled <button>` goes stable far too early.
constexpr uintptr_t LOBBY_SLOTS_ADDR = mh::addr::_G_LLM_LOBBY_SLOTS;      // per-slot record, stride 0x39
constexpr uintptr_t MAP_PCOUNT_ADDR  = mh::addr::current_map_data + 0x08; // map's player-slot count (int)
constexpr int       SLOT_STRIDE = 0x39, SLOT_STATUS_OFF = 0x0b, SLOT_HUMAN = 1, SLOT_AI = 2;
constexpr int       SLOT_RACE_OFF = 0x05; // `race <slot> <v>`: 1=Human 2=Alien (the spinner's values)

// `field <slot> <spec>` reads the MENU TEXT-FIELD BUFFERS the NET_SETUP screen edits in place (see
// the NET_SETUP container 0x653ef3 is re-wired per use, each use pointing at a field record (stride
// 0x18) whose +0 is one of these buffers). Asserting the BUFFER rather than the pixels is not a
// shortcut -- it is the only assertion available for non-ASCII text, because the EN build's fonts
// carry no Cyrillic glyphs at all, so a rendered frame cannot tell a correctly stored Cyrillic name
// from a wrong one. It is also the stronger claim: it names the exact bytes the game stored.
constexpr uintptr_t FIELD_NAME_ADDR = mh::addr::mp_player_name; // 0x005d0d88, 32-byte ASCII
constexpr uintptr_t FIELD_GAME_ADDR = mh::addr::mp_game_name;   // 0x005d0da8, host-typed create name
// mp:F3 added the third slot: the IN-GAME CHAT edit line, `_G_LLM_STRAT_CHAT_INPUT_LINE`
// (0x0050a84c, char[81] = a 41-byte line followed by the 40-byte scancode queue at +0x29). It is the
// same kind of assertion as the two above and for the same reason -- the chat is where F3's second
// hook lives, and a Cyrillic chat line on stock EN fonts renders as substitutes, so the buffer is the
// only thing that can tell a correct line from a wrong one. Read BEFORE the Enter that submits it:
// llm_chat_history_push clears the line.
constexpr uintptr_t FIELD_CHAT_ADDR = mh::addr::_G_LLM_STRAT_CHAT_INPUT_LINE; // 0x0050a84c
// mp:MP-LANG lobby follow-up added the fourth: the LOBBY chat edit field's buffer, the text half of the
// lobby chat packet (0x0064434c + 10; cap 0x40). Read BEFORE the Enter that sends it -- the send clears
// the widget. Like `chat` it holds UTF-8, so a `hex:` spec is the byte-level claim.
constexpr uintptr_t FIELD_LOBBY_ADDR = 0x00644356u; // DAT_00644356 (EN), llm_lobby_chat_send_cb's text
enum { FIELD_NAME  = 0,
       FIELD_GAME  = 1,
       FIELD_CHAT  = 2,
       FIELD_LOBBY = 3 };
uintptr_t field_addr(int slot) {
    if (slot == FIELD_GAME) return FIELD_GAME_ADDR;
    if (slot == FIELD_CHAT) return FIELD_CHAT_ADDR;
    if (slot == FIELD_LOBBY) return FIELD_LOBBY_ADDR;
    return FIELD_NAME_ADDR;
}
// `wlobby`: the LOBBY's scrolling text log -- the list widget _G_LLM_UI_WGT_LOBBY_LOG_LIST (0x00650083)
// that llm_lobby_announce_line appends every line to (joins, leaves, the map banner, and every chat
// line, local echo and received). Its param block (widget + 0x30) holds [0] the entry-pointer array and
// [6] the entry count (llm_ui_list_widget_append_entry); each entry record's +0 is its UTF-16 text.
constexpr uintptr_t LOBBY_LOG_WIDGET = 0x00650083u;
constexpr int       LOBBY_LOG_MAX    = 256; // sanity bound on the count a stale block could claim
// `wmsg`: the floating on-screen message queue's NEWEST entry. game_ui_AddTextToPrintQueue keeps 30
// slots of 120 UTF-16 code units (0xf0 bytes each) and copies the new line into SLOT 0 after shifting
// the rest down, so slot 0 is always the most recent message -- which for a received chat line is
// "<sender>: <text>", built by llm_net_lockstep_dispatch's chat arm (opcode 4).
constexpr uintptr_t MSGQ_ADDR       = mh::addr::MESSAGE_QUEUE; // 0x005ce484, slot 0
constexpr int       MSGQ_SLOT_UNITS = 120;                     // 0xf0 bytes
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
    OP_W_BLDGSEL, // mp:D37a: `bldgsel <N|any|none>` -- the strategic HUD's selected building
    OP_W_SESSIONS,
    OP_W_PEERS,
    OP_W_OCC,
    OP_W_RACE,
    OP_W_TEAM,      // mp:U51: `team <slot> <v>` -- the slot's TEAM byte (+0x0c) on THIS peer
    OP_W_LOBBYMODE, // mp:U51: `lobbymode <0|1>` -- host slot 0's MODE byte (+0x07): 0 FFA, 1 Team
    OP_W_RETRYREADY,
    OP_W_LOBBYGEN,
    OP_W_LOBBYPING, // mp:L1f: N lobby slot rows are showing a real ping number on THIS screen
    OP_W_GAMECLOCK,
    OP_W_SIMSTEP,
    OP_W_AWAITSIGNAL,
    OP_W_FIELD,
    OP_W_WMSG,
    OP_W_WLOBBY,
    OP_W_STALLED,       // TL-UISTALL: true while the sim is lockstep-blocked, optionally for >= <ms>
    OP_W_IMGUI,         // PT-GFX4: `imgui open|closed|swallowed <N>` -- the ImGui overlay's own state
    OP_W_WINCLIENT,     // PT-GFX3: `winclient <w> <h>` -- the game window's client area is exactly w x h
    OP_W_DIMOUSE,       // PT-INPUT1: `dimouse <x> <y>` -- the DI accumulator (_G_LLM_INPUT_MOUSE_LAST_X/Y) is exactly x,y
    OP_W_KEYSTATE,      // PT-INPUT1: `keystate <dik> <0|1>` -- the game's keystate says that key is up / down
    OP_W_WINSWITCHABLE, // PT-GFX8: `winswitchable` -- the game window passes the taskbar / Alt-Tab eligibility rule
    OP_W_WINACTIVE,     // PT-GFX8: `winactive` -- the game window is the foreground window
    OP_W_LAST = OP_W_WINACTIVE,
    // actions (fire once, then advance)
    OP_A_CLICKV,
    OP_A_CLICKL,
    OP_A_CLICKI,
    OP_A_CURSOR,
    OP_A_CURSORHOLD,
    OP_A_CLICK,
    OP_A_PRESS,
    OP_A_RELEASE,
    OP_A_RCLICK,    // mp:D25: `rclick <x> <y> [shift]`
    OP_A_SIMCLICK,  // TL-SUITE-SPLICE-HOSTCLICK: `simclick <x> <y>` -- a left click held across sim steps
    OP_A_SIMRCLICK, // mp:X2g: `simrclick <x> <y>` -- the same hold with the right button
    OP_A_WMCLICK,   // PT-GFX4: `wmclick <x> <y>` -- a left click POSTED to the game window as messages
    OP_A_WINSIZE,   // PT-GFX3: `winsize <w> <h>` -- resize the game window's client as a player's drag would
    OP_A_WINMIN,    // mp:P17: `winmin <ms>` -- minimise the game window now, a helper thread restores it after <ms>
    OP_A_WINAWAY,   // PT-GFX8: `winaway` -- another top-level window takes the foreground (what Alt-Tab does)
    OP_A_WINRETURN, // PT-GFX8: `winreturn [launcher]` -- bring the game back the way the switcher / the launcher does
    OP_A_WINPROBE,  // PT-GFX8: `winprobe` -- log the window facts the eligibility rule rests on
    OP_A_WINEX,     // PT-GFX8: `winex <set-hex> <clear-hex>` -- change the ex-style (the negative arm's injected fault)
    OP_A_RAWFOCUS,  // PT-INPUT1: `rawfocus -1|0|1` -- the foreground the owned DirectInput sees
    OP_A_RAWMOUSE,  // PT-INPUT1: `rawmouse abs|rel|down|up ...` -- a Raw Input packet into the owned mouse
    OP_A_RAWKEY,    // PT-INPUT1: `rawkey <make> [e0] [down|up]` -- a Raw Input packet into the owned keyboard
    OP_A_KEY,
    OP_A_KEYHOLD, // mp:SES3c: `keyhold <scancode> <frames>` -- the unpaired-DOWN sibling of `key`
    OP_A_HOTKEY,  // `hotkey Ctrl+Alt+D` -- a GetAsyncKeyState-style chord, synthesised (2026-09-20)
    OP_A_TYPE,
    OP_A_LAYOUT,
    OP_A_KEYJOURNAL,
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
    int      a, b;      // numeric args (value / idx / gamemode / x ; y)
    int      c;         // third numeric arg -- `cursorhold`/`keyhold`'s frame count, `key`/`rclick`'s shift flag
    unsigned scr;       // screen container VA (OP_W_SCREEN)
    Target   tgt;       // target widget (present/absent/enabled/hovered/clickl)
    char     text[160]; // capture name / log message / predicate spec (a full 63-byte lobby field as hex)
    char     raw[176];  // original line, for logging (Phase 4 pass/fail parsing)
    // `retry <game-ms> <back> <tries> <wait>` (tooling:TL-SUITE-SPLICE-HOSTCLICK): see script_tick.
    int    retry_ms, retry_back, retry_tries, retry_used;
    double retry_clk0; // game clock (ms) at this attempt's first tick
};

// THE STEP AND FILE CAPS ARE LOUD (mp:RM1, 2026-09-21). Both used to be silent: parse_line dropped
// every step past the 64th and load_script read 8 KB and stopped, so a script that outgrew either
// simply ENDED EARLY and reported `COMPLETE (64/64 steps)` -- a green verdict on a walk that never
// reached its assertion. The first script to cross the line (mp_host_rematch2.txt, 69 directives:
// host_rematch's 61 plus a second Start) passed its truncated half and was caught only because the
// post_check read the logs. A script the interpreter cannot hold is REFUSED at load with a
// `; [script] ABORT at step 0` line, the terminal status ui_test.py already knows, so the runner
// goes red at once instead of waiting out its budget. The caps themselves are just larger: a Step
// is ~200 bytes, so 256 of them is ~50 KB of .bss, and the longest committed script is ~7.3 KB.
constexpr int          MAX_STEPS     = 256;
constexpr int          SCRIPT_MAX_KB = 32;
Step                   g_steps[MAX_STEPS];
int                    g_overflow_steps = 0; // directives parse_line had to drop (load_script refuses on > 0)
int                    g_nstep          = 0;
int                    g_cur            = 0;
int                    g_wait           = 0;
DWORD                  g_step_t0        = 0; // wall clock at the first tick of the current step
bool                   g_script_on      = false;
bool                   g_script_done    = false;
int                    g_timeout        = 1500; // watchdog frames before a stuck WAIT aborts the run
bool                   g_dump_screens   = false;
mh_llm_ui_widget_list *g_last_seen      = nullptr;
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

// ---- `hotkey`: a synthesised GetAsyncKeyState chord (2026-09-20) --------------------------------
//
// The DLL's own hotkeys ([debug] toggle_key, [hud] net_indicator_key, F12 capture) poll
// GetAsyncKeyState, and GetAsyncKeyState answers 0 for a process whose desktop is not the input
// desktop -- which is every headless lane (tools/ui_test.py resolve_desktop). So an in-game hotkey
// could never be exercised by the suite. This table is the harness's view of "held": the pollers
// OR it into their OS read through MH_UIDrive_SynthKeyDown, so the real binding parse, modifier
// check and rising-edge logic run, and only the OS read is substituted. Nothing here touches the
// game's own key ring (that is `key <scancode>`), and an unarmed run never sets a byte of it.
// (The table itself is defined beside its export, above MH_UIDrive_CursorTo.)

// `Ctrl+Alt+D` / `Alt+F9` / `Shift+PgDn` / `VK_F10` / `0x79` / `D` -> vk + a modifier mask
// (1 ctrl, 2 alt, 4 shift). The same grammar gfx_overlay.cpp parse_key accepts, kept small.
bool hotkey_parse(const char *spec, int *vk, int *mods) {
    *vk           = 0;
    *mods         = 0;
    const char *s = spec;
    for (;;) {
        if (_strnicmp(s, "ctrl+", 5) == 0) {
            *mods |= 1;
            s += 5;
        } else if (_strnicmp(s, "control+", 8) == 0) {
            *mods |= 1;
            s += 8;
        } else if (_strnicmp(s, "alt+", 4) == 0) {
            *mods |= 2;
            s += 4;
        } else if (_strnicmp(s, "shift+", 6) == 0) {
            *mods |= 4;
            s += 6;
        } else {
            break;
        }
    }
    if ((s[0] == 'V' || s[0] == 'v') && (s[1] == 'K' || s[1] == 'k') && s[2] == '_') s += 3;
    static const struct {
        const char *name;
        int         vk;
    } NAMED[] = {
        {"pgup", VK_PRIOR},
        {"prior", VK_PRIOR},
        {"pgdn", VK_NEXT},
        {"next", VK_NEXT},
        {"home", VK_HOME},
        {"end", VK_END},
        {"ins", VK_INSERT},
        {"insert", VK_INSERT},
        {"del", VK_DELETE},
        {"delete", VK_DELETE},
        {"up", VK_UP},
        {"down", VK_DOWN},
        {"left", VK_LEFT},
        {"right", VK_RIGHT},
        {"space", VK_SPACE},
        {"tab", VK_TAB},
    };
    for (const auto &e : NAMED)
        if (lstrcmpiA(e.name, s) == 0) {
            *vk = e.vk;
            return true;
        }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') {
        const int f_n = (int)strtol(s + 1, nullptr, 10);
        if (f_n >= 1 && f_n <= 24) {
            *vk = VK_F1 + f_n - 1;
            return true;
        }
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        *vk = (int)strtol(s, nullptr, 16);
        return *vk > 0 && *vk < 256;
    }
    if (s[0] && !s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        *vk = (unsigned char)c;
        return true;
    }
    return false;
}

void synth_chord_set(int vk, int mods, bool down) {
    const uint8_t v = down ? 1u : 0u;
    if (mods & 1) g_synth_vk[VK_CONTROL] = v;
    if (mods & 2) g_synth_vk[VK_MENU] = v;
    if (mods & 4) g_synth_vk[VK_SHIFT] = v;
    if (vk > 0 && vk < 256) g_synth_vk[vk] = v;
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
    if (g_nstep >= MAX_STEPS) {
        ++g_overflow_steps; // counted, not dropped on the quiet -- load_script refuses the script
        return;
    }
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
    } else if (strcmp(op, "bldgsel") == 0) {
        s->op = OP_W_BLDGSEL;
        s->a  = strcmp(arg, "any") == 0 ? BLDGSEL_ANY : strcmp(arg, "none") == 0 ? 0
                                                                                 : (int)strtol(arg, nullptr, 0);
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
    } else if (strcmp(op, "team") == 0) {
        // `team <slot> <value>` -- two ints, like `race`.
        s->op       = OP_W_TEAM;
        char *after = nullptr;
        s->a        = (arg && *arg) ? (int)strtol(arg, &after, 0) : -1;
        s->b        = (after && *after) ? (int)strtol(after, nullptr, 0) : -1;
    } else if (strcmp(op, "lobbymode") == 0) {
        s->op = OP_W_LOBBYMODE;
        s->b  = (arg && *arg) ? (int)strtol(arg, nullptr, 0) : -1;
    } else if (strcmp(op, "retryready") == 0) {
        s->op = OP_W_RETRYREADY;
        s->a  = (arg && *arg) ? (int)strtol(arg, nullptr, 0) : 1;
    } else if (strcmp(op, "lobbygen") == 0) {
        s->op = OP_W_LOBBYGEN;
        s->a  = (arg && *arg) ? (int)strtol(arg, nullptr, 0) : 1;
    } else if (strcmp(op, "lobbyping") == 0) {
        s->op = OP_W_LOBBYPING;
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
    } else if (strcmp(op, "stalled") == 0) {
        // stalled [<ms>] -- TL-UISTALL. True while MH_Lockstep_StallBindingPeer() >= 0 (the sim is
        // lockstep-blocked), optionally for at least <ms>. `gameclock` CANNOT gate a capture on a
        // stall by construction (OP_W_GAMECLOCK's own comment: "stalls when the sim stalls" -- the
        // clock it reads freezes with the sim), which is exactly why L1's "WAITING FOR <name>" frame
        // was never captured: every timed blackhole landed after the message's 1 s hold had already
        // expired. `<ms>` defaults to 0 (true the instant a block is observed at all).
        s->op = OP_W_STALLED;
        s->a  = (arg && *arg) ? (int)strtol(arg, nullptr, 0) : 0;
    } else if (strcmp(op, "simstep") == 0) {
        s->op = OP_W_SIMSTEP;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "retry") == 0) {
        // `retry <game-ms> <back> <tries> <wait directive>` -- a WAIT that, when it has not held after
        // <game-ms> of SIM time, rewinds <back> steps (the click meant to cause it) and tries again, at
        // most <tries> times, then ABORTs. Sim time, not frames or wall clock: a lost click is one the
        // game had steps to act on, and a modal dialog that did open stalls the sim, so it never
        // retries over an open dialog (tooling:TL-SUITE-SPLICE-HOSTCLICK, TL-GATE-LOADFLAKE-0925).
        char       *after = nullptr;
        const int   ms    = (int)strtol(arg, &after, 0);
        const int   back  = (int)strtol(after, &after, 0);
        const int   tries = (int)strtol(after, &after, 0);
        const char *rest  = skip_ws(after);
        const int   idx   = g_nstep;
        const bool  ok    = ms > 0 && back >= 1 && back <= idx && tries >= 1 && strncmp(rest, "retry", 5) != 0;
        parse_line(rest);               // the wait itself, parsed as an ordinary line -- kept even if the prefix is bad
        if (g_nstep != idx + 1) return; // inner line ignored or overflowed; already logged
        Step *w = &g_steps[idx];
        if (!ok || !is_wait_op(w->op)) {
            ui_log("; [script] IGNORED retry prefix (need ms>0, back 1..%d, tries>0, then a WAIT): %s",
                   idx, line);
            return;
        }
        w->retry_ms    = ms;
        w->retry_back  = back;
        w->retry_tries = tries;
        copy_trim(w->raw, sizeof(w->raw), line);
        return;
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
    } else if (strcmp(op, "simclick") == 0 || strcmp(op, "simrclick") == 0) {
        // `simclick <x> <y>` / `simrclick <x> <y>` -- see simclick_frame.
        s->op = strcmp(op, "simclick") == 0 ? OP_A_SIMCLICK : OP_A_SIMRCLICK;
        char *end;
        s->a = (int)strtol(arg, &end, 0);
        s->b = (int)strtol(end, &end, 0);
        while (*end == ' ' || *end == '	') ++end;
        s->c = (s->op == OP_A_SIMRCLICK && strncmp(end, "shift", 5) == 0) ? 1 : 0;
    } else if (strcmp(op, "rclick") == 0) {
        // `rclick <x> <y> [shift]` -- a right-click spread over three presents, under a held Shift
        // when the word is given. See rclick_frame.
        s->op = OP_A_RCLICK;
        char *end;
        s->a = (int)strtol(arg, &end, 0);
        s->b = (int)strtol(end, &end, 0);
        while (*end == ' ' || *end == '	') ++end;
        s->c = (strncmp(end, "shift", 5) == 0) ? 1 : 0;
    } else if (strcmp(op, "cursorhold") == 0) {
        // `cursorhold <x> <y> <frames>` -- SES3, and the harness could not express this before.
        //
        // `cursor` enqueues ONE move event, and in-game that does not hold: llm_input_mouse_delta_pump
        // RESETS _G_LLM_CURSOR_X/Y to _G_LLM_CURSOR_MENU_X/Y at entry on every call and only an event
        // in the ring overwrites them (0x00426645, the visible-cursor arm). So one event moves the
        // cursor for exactly one frame. Re-injecting every present is what a DWELL is -- and it is
        // also what a clamped VM pointer does for real: llm_input_di_mouse_poll integrates relative
        // counts and clamps to the screen box, and a session measured 558 events arriving while the
        // accumulator sat pinned at X=639 -- a pinned cursor is a stream of events that do not move
        // it, not an absence of events.
        //
        // Budgeted in FRAMES under the ordinary action watchdog ([uitest] timeout_frames, default
        // 1500), so a hold longer than that aborts the step rather than hanging the run.
        s->op       = OP_A_CURSORHOLD;
        char *after = nullptr;
        s->a        = (int)strtol(arg, &after, 0);
        s->b        = (int)strtol(after, &after, 0);
        s->c        = (int)strtol(after, nullptr, 0);
        if (s->c < 1) s->c = 1;
    } else if (strcmp(op, "key") == 0) {
        // key <scancode> [shift]  -- one down+up pair. Scancodes are PC set-1, the same values
        // llm_strat_input_update compares against (e.g. 0x01 ESC, 0x3f..0x41 F5/F6/F7, 0x1c Enter).
        // `shift` (mp:CH1) holds the game's OWN Shift latch (_G_LLM_INPUT_KEYSTATE[0x2a], the byte
        // the strategic input reads -- NOT the OS table `type` uses) across the presents the pair
        // is in flight, the way `rclick ... shift` does: Shift+Enter is how the strategic view opens
        // the hidden debug console instead of the chat line (the branch at 0x00441c82).
        s->op       = OP_A_KEY;
        char *after = nullptr;
        s->a        = (int)strtol(arg, &after, 0);
        s->c        = (strncmp(skip_ws(after), "shift", 5) == 0) ? 1 : 0;
    } else if (strcmp(op, "keyhold") == 0) {
        // `keyhold <scancode> <frames>` -- mp:SES3c. `key` always sends a DOWN and an UP, so a
        // held arrow key (the four CAM_SCROLL_*_HELD latches -- 0x4b/0x4d/0x48/0x50 LEFT/RIGHT/
        // UP/DOWN) is not expressible with it. The fix is NOT a new producer, it is the ABSENCE of
        // the paired UP: llm_strat_input_update reads the same key ring `key` does, latching the
        // HELD bit on the scancode's DOWN event and clearing it on that scancode's own UP event
        // (the ordinary key-state model `key`'s own down+up pair already relies on), so one
        // unpaired DOWN held across presents IS the hold. Multi-present like `cursorhold`, whose
        // `<frames>` contract this mirrors exactly: DOWN on present 0, held through `frames`
        // presents (no event re-injected -- the latch does not need refreshing), UP on the last
        // one, then advance.
        s->op       = OP_A_KEYHOLD;
        char *after = nullptr;
        s->a        = (int)strtol(arg, &after, 0);
        s->c        = (int)strtol(after, nullptr, 0);
        if (s->c < 1) s->c = 1;
    } else if (strcmp(op, "hotkey") == 0) {
        // hotkey <spec>  -- `Ctrl+Alt+D`, `Alt+F9`, `0x79`, `D`: the DLL's OWN hotkey grammar (the
        // [debug] overlay's toggle_key and the [hud] net_indicator_key read it), held for two
        // presents and released. NOT the game's key ring: those hotkeys poll GetAsyncKeyState, which
        // reads 0 on the isolated desktop every headless lane runs on, so no injected event can reach
        // them. The chord is synthesised in the DLL's own key-state view instead (MH_UIDrive_-
        // SynthKeyDown), which the pollers OR into their GetAsyncKeyState read -- the binding parse,
        // the modifier check and the rising-edge detector all run for real; only the OS read is
        // substituted. Built for the installed-hidden -> chord -> visible -> chord -> hidden proof
        // of `[debug] overlay=0` (user ruling 2026-09-20; the debug_overlay scenario).
        s->op = OP_A_HOTKEY;
        copy_trim(s->text, sizeof(s->text), arg);
        if (!hotkey_parse(s->text, &s->a, &s->b)) {
            ui_log("; [script] IGNORED hotkey '%s' -- not a key spec (line: %s)", s->text, s->raw);
            return;
        }
    } else if (strcmp(op, "type") == 0) {
        // type <utf8 text> -- the rest of the line, UTF-8, trailing whitespace trimmed (so a literal
        // trailing space is not expressible; nothing has wanted one). Resolved against the layout at
        // RUN time, not here, because `layout` may not have run yet when the script is parsed.
        s->op = OP_A_TYPE;
        copy_trim(s->text, sizeof(s->text), arg);
    } else if (strcmp(op, "layout") == 0) {
        // layout <klid> | layout default  -- e.g. 00000409 US, 00000419 Russian, 00000415 Polish.
        s->op = OP_A_LAYOUT;
        copy_trim(s->text, sizeof(s->text), arg);
    } else if (strcmp(op, "keyjournal") == 0) {
        // keyjournal mark | keyjournal same
        s->op = OP_A_KEYJOURNAL;
        copy_trim(s->text, sizeof(s->text), arg);
    } else if (strcmp(op, "field") == 0) {
        // field <name|game|chat> <text | hex:xx..>  -- that text BUFFER equals this, exactly.
        s->op         = OP_W_FIELD;
        const char *q = skip_ws(arg);
        s->a          = (strncmp(q, "game", 4) == 0)    ? FIELD_GAME
                        : (strncmp(q, "chat", 4) == 0)  ? FIELD_CHAT
                        : (strncmp(q, "lobby", 5) == 0) ? FIELD_LOBBY
                                                        : FIELD_NAME;
        while (*q && *q != ' ' && *q != '\t') ++q;
        copy_trim(s->text, sizeof(s->text), skip_ws(q));
    } else if (strcmp(op, "wmclick") == 0) {
        // `wmclick <x> <y>` -- PT-GFX4. Every other click verb writes the game's input RING, below the
        // window procedure, so nothing that sits IN the window procedure -- the ImGui overlay's
        // subclass -- can ever see it. This one posts WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP to
        // the game window instead, the way the OS delivers a real click, so the subclass decides and
        // the game's own tap (llm_input_wndproc_tap 0x004d1194) turns whatever it forwards into ring
        // events. Needs `[input] mouse_absolute=1`: with a DirectInput mouse the tap ignores mouse
        // MESSAGES and polls DI instead, and a posted message carries nothing DI would see. <x> <y>
        // are CLIENT pixels, exactly as the OS would deliver them; in a scaled window (borderless, or
        // resized) the owned device's subclass maps them to game pixels on the way in (ddraw_own.cpp
        // "the mouse"), which is what the `win_resize` scenario asserts.
        s->op = OP_A_WMCLICK;
        char *end;
        s->a = (int)strtol(arg, &end, 0);
        s->b = (int)strtol(end, nullptr, 0);
    } else if (strcmp(op, "winsize") == 0 || strcmp(op, "winclient") == 0) {
        // `winsize <w> <h>` -- PT-GFX3: resize the game window so its CLIENT area is w x h, bracketed by
        // WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE, i.e. what a player's border drag delivers (SendInput is
        // not available on the rig, so the drag itself cannot be synthesised). `winclient <w> <h>` --
        // wait until the client area IS w x h (the owned device may re-assert a native size first).
        s->op = strcmp(op, "winsize") == 0 ? OP_A_WINSIZE : OP_W_WINCLIENT;
        char *end;
        s->a = (int)strtol(arg, &end, 0);
        s->b = (int)strtol(end, nullptr, 0);
    } else if (strcmp(op, "winmin") == 0) {
        // `winmin <ms>` -- mp:P17: the alt-tab rig check. Minimise the game window (what Alt+Tab to another
        // window / the taskbar button does to the frame loop) and restore it <ms> later from a HELPER thread,
        // because a minimised window that stops presenting would never reach the next script op itself.
        s->op = OP_A_WINMIN;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "winswitchable") == 0) {
        s->op = OP_W_WINSWITCHABLE;
    } else if (strcmp(op, "winactive") == 0) {
        s->op = OP_W_WINACTIVE;
    } else if (strcmp(op, "winaway") == 0) {
        // PT-GFX8: a second top-level window (a tiny one, on its own thread) is created and made the
        // foreground, so the game gets WM_ACTIVATE(inactive) without being minimised -- the Alt-Tab
        // away from a borderless window. `winreturn` brings the game back.
        s->op = OP_A_WINAWAY;
    } else if (strcmp(op, "winreturn") == 0) {
        // `winreturn` = the Alt-Tab switcher's activation (SW_RESTORE if iconic, then SetForegroundWindow from
        // the process that owns the foreground); `winreturn launcher` = the retail single-instance path
        // (FindWindowA + SetForegroundWindow, nothing else).
        s->op = OP_A_WINRETURN;
        s->a  = strstr(arg, "launcher") ? 1 : 0;
    } else if (strcmp(op, "winprobe") == 0) {
        s->op = OP_A_WINPROBE;
    } else if (strcmp(op, "winex") == 0) {
        s->op = OP_A_WINEX;
        char *end;
        s->a = (int)strtoul(arg, &end, 16);
        s->b = (int)strtoul(end, nullptr, 16);
    } else if (strcmp(op, "rawfocus") == 0) {
        // `rawfocus -1|0|1` -- PT-INPUT1. The foreground the owned DirectInput ([input] backend=own)
        // acquires against: -1 the real one, 1 focused (a headless lane's window never is, so a script
        // that injects `rawmouse`/`rawkey` sets this first), 0 lost -- the Alt-Tab path, which releases
        // every held key by synthetic key-up.
        s->op = OP_A_RAWFOCUS;
        s->a  = (int)strtol(arg, nullptr, 0);
    } else if (strcmp(op, "rawmouse") == 0) {
        // `rawmouse abs <gx> <gy>` (a GAME pixel, sent as an absolute virtual-desktop packet through
        // the inverse of the mapping a real absolute pointer takes) | `rawmouse rel <dx> <dy>` (raw
        // counts) | `rawmouse down|up l|r|m`. Into the owned mouse's queue through the same packet
        // handler WM_INPUT uses -- the harness equivalent of a Raw Input packet, since a posted
        // WM_INPUT carries no data. s->c: 0 abs, 1 rel, 2 down, 3 up.
        s->op         = OP_A_RAWMOUSE;
        const char *q = skip_ws(arg);
        s->c          = strncmp(q, "abs", 3) == 0 ? 0 : strncmp(q, "rel", 3) == 0 ? 1
                                                    : strncmp(q, "down", 4) == 0  ? 2
                                                                                  : 3;
        while (*q && *q != ' ' && *q != '\t') ++q;
        q = skip_ws(q);
        if (s->c >= 2) {
            s->a = (*q == 'r') ? 1 : (*q == 'm') ? 2
                                                 : 0;
        } else {
            char *end;
            s->a = (int)strtol(q, &end, 0);
            s->b = (int)strtol(end, nullptr, 0);
        }
    } else if (strcmp(op, "rawkey") == 0) {
        // `rawkey <make> [e0] [down|up]` -- a set-1 make code (e0 = the E0 prefix: arrows, right Ctrl,
        // numpad Enter...). No direction = DOWN on this present, UP on the next.
        s->op         = OP_A_RAWKEY;
        char *after   = nullptr;
        s->a          = (int)strtol(arg, &after, 0);
        const char *q = skip_ws(after);
        s->b          = 0;
        if (strncmp(q, "e0", 2) == 0) {
            s->b = 1;
            q    = skip_ws(q + 2);
        }
        s->c = strncmp(q, "down", 4) == 0 ? 1 : strncmp(q, "up", 2) == 0 ? 2
                                                                         : 0;
    } else if (strcmp(op, "dimouse") == 0 || strcmp(op, "keystate") == 0) {
        // `dimouse <x> <y>` -- the DirectInput accumulator is exactly (x,y). `keystate <dik> <0|1>` --
        // the game's keystate byte for that DIK (index dik&0x7f, bit 2 when dik&0x80 else bit 1) is
        // clear / set. PT-INPUT1.
        s->op = strcmp(op, "dimouse") == 0 ? OP_W_DIMOUSE : OP_W_KEYSTATE;
        char *end;
        s->a = (int)strtol(arg, &end, 0);
        s->b = (int)strtol(end, nullptr, 0);
    } else if (strcmp(op, "imgui") == 0) {
        // `imgui open` / `imgui closed` / `imgui swallowed <N>` (>= N mouse messages swallowed since
        // boot) -- PT-GFX4. The overlay's own state, so a script gates on "the toggle took" and "the
        // subclass consumed the click" rather than on a frame count. `imgui hover <0|1>` (PT-GFX6): the
        // last frame's io.WantCaptureMouse equals it -- ImGui's hover, fed from the GAME cursor.
        // `imgui difiltered <N>` (PT-INPUT1): >= N DirectInput button/wheel records withheld from the game by
        // the overlay's DI filter -- the swallow on the DI path, which `swallowed` (messages) cannot see.
        s->op         = OP_W_IMGUI;
        const char *q = skip_ws(arg);
        s->a          = (strncmp(q, "open", 4) == 0) ? 1 : (strncmp(q, "closed", 6) == 0)    ? 0
                                                       : (strncmp(q, "hover", 5) == 0)       ? 3
                                                       : (strncmp(q, "difiltered", 10) == 0) ? 4
                                                                                             : 2;
        while (*q && *q != ' ' && *q != '\t') ++q;
        s->b = (int)strtol(skip_ws(q), nullptr, 0);
    } else if (strcmp(op, "wmsg") == 0) {
        // wmsg <text | hex:xx..> -- the newest floating message contains this UTF-16 sequence.
        s->op = OP_W_WMSG;
        copy_trim(s->text, sizeof(s->text), skip_ws(arg));
    } else if (strcmp(op, "wlobby") == 0) {
        // wlobby <text | hex:xx..> -- SOME line of the lobby's text log contains this UTF-16 sequence.
        // `hex:` = UTF-16LE code units; plain text is read as UTF-8 (the script file's own encoding).
        s->op = OP_W_WLOBBY;
        copy_trim(s->text, sizeof(s->text), skip_ws(arg));
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
    static char buf[SCRIPT_MAX_KB * 1024];
    DWORD       rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (rd >= sizeof(buf) - 1) {
        // The file did not fit: whatever follows would be parsed as if the script ended here.
        ui_log("; [script] ABORT at step 0 -- REFUSED: '%s' is larger than the %d KB the loader holds "
               "(raise SCRIPT_MAX_KB in ui_drive.cpp or split the walk)",
               path, SCRIPT_MAX_KB);
        g_nstep = 0;
        return false;
    }
    buf[rd]    = 0;
    char *line = buf;
    for (char *q = buf;; ++q) {
        if (*q == '\n' || *q == 0) {
            char end = *q;
            *q       = 0;
            // CRLF scripts: drop the '\r' here, once, rather than in each op. Label and predicate ops
            // happened to tolerate it, but `awaitsignal lobby\r` waited for rig_lobby\r.flag and hung
            // until the row's timeout (dead-ends G306).
            if (q > line && q[-1] == '\r') q[-1] = 0;
            parse_line(line);
            line = q + 1;
            if (end == 0) break;
        }
    }
    if (g_overflow_steps > 0) {
        ui_log("; [script] ABORT at step 0 -- REFUSED: '%s' has %d directive(s) more than the %d the "
               "interpreter holds (raise MAX_STEPS in ui_drive.cpp or split the walk)",
               path, g_overflow_steps, MAX_STEPS);
        g_nstep = 0;
        return false;
    }
    return g_nstep > 0;
}

// s->b carries the `<` flag parsed by parse_count: default is ">= a", `<a` is strictly less.
bool count_ok(const Step *s, int v) { return s->b ? (v < s->a) : (v >= s->a); }

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode a `field` spec into the exact bytes expected. `hex:cff0e8e2e5f2` states a BYTE-LEVEL claim,
// which is the only readable form for text the fonts cannot draw; anything else is taken literally
// as ASCII. Returns the length, or -1 on a malformed hex run.
int field_expect(const char *spec, uint8_t *out, int cap) {
    if (strncmp(spec, "hex:", 4) == 0) {
        const char *p = spec + 4;
        int         n = 0;
        while (p[0] && n < cap) {
            int hi = hexval(p[0]), lo = p[1] ? hexval(p[1]) : -1;
            if (hi < 0 || lo < 0) return -1;
            out[n++] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        return (*p == 0) ? n : -1;
    }
    int n = 0;
    while (spec[n] && n < cap) {
        out[n] = (uint8_t)spec[n];
        ++n;
    }
    return n;
}

// The buffer AS BYTES, for the timeout diagnostic. Without this a `field` timeout says only "the
// bytes are not what you asked for" and sends you to a debugger for the one thing the run knew.
// The EVIDENCE behind a `wmsg` timeout: what the newest message actually holds, as UTF-16 hex plus
// a printable rendering. Without it a red says only "the needle was not there", which is the one
// thing the reader already knows.
// A `wlobby` needle as UTF-16 code units: `hex:` gives the units' LE bytes, anything else is UTF-8.
int wlobby_needle(const char *spec, uint16_t *out, int cap) {
    if (strncmp(spec, "hex:", 4) == 0) {
        uint8_t   b[64];
        const int n = field_expect(spec, b, (int)sizeof(b));
        if (n < 2 || (n & 1)) return -1;
        int u = 0;
        for (int i = 0; i + 1 < n && u < cap; i += 2) out[u++] = (uint16_t)(b[i] | (b[i + 1] << 8));
        return u;
    }
    const int u = (int)mh_net_proto::utf8_to_utf16(spec, strlen(spec), out, (size_t)cap);
    return u > 0 ? u : -1;
}

// The lobby log's entries, guarded: a block the menu has not built yet reads as "no entries".
// Returns the count and fills `out` with up to `cap` entry texts, OLDEST first.
int lobby_log_entries(const uint16_t **out, int cap) {
    __try {
        const uintptr_t pb = *(volatile const uintptr_t *)(LOBBY_LOG_WIDGET + 0x30);
        if (!pb) return 0;
        const uintptr_t *arr = *(const uintptr_t *const volatile *)pb;
        const int        n   = *(volatile const int *)(pb + 6 * 4);
        if (!arr || n <= 0 || n > LOBBY_LOG_MAX) return 0;
        int k = 0;
        for (int i = (n > cap ? n - cap : 0); i < n; ++i) {
            const uintptr_t rec = arr[i];
            if (rec) out[k++] = *(const uint16_t *const *)rec;
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool wlobby_contains(const uint16_t *needle, int nn) {
    const uint16_t *e[LOBBY_LOG_MAX];
    const int       n = lobby_log_entries(e, LOBBY_LOG_MAX);
    __try {
        for (int i = 0; i < n; ++i) {
            const uint16_t *h = e[i];
            if (!h) continue;
            for (int off = 0; off < 512 && h[off]; ++off) {
                int k = 0;
                while (k < nn && h[off + k] == needle[k]) ++k;
                if (k == nn) return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The EVIDENCE behind a `wlobby` timeout: the newest four lines, as UTF-16 hex + a printable render.
void log_wlobby() {
    const uint16_t *e[4];
    const int       n = lobby_log_entries(e, 4);
    ui_log("; [script]   wlobby: newest %d lobby log line(s):", n);
    for (int i = 0; i < n; ++i) {
        char hx[5 * 24 + 1];
        char as[24 + 1];
        int  u = 0, k = 0;
        __try {
            for (; u < 24 && e[i] && e[i][u]; ++u) {
                const unsigned cu = e[i][u];
                wsprintfA(hx + k, "%04x ", cu);
                k += 5;
                as[u] = (cu >= 0x20 && cu < 0x7f) ? (char)cu : '.';
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        hx[k] = 0;
        as[u] = 0;
        ui_log("; [script]     [%d] %s| \"%s\"", i, hx, as);
    }
}

void log_wmsg() {
    const uint8_t *q = (const uint8_t *)MSGQ_ADDR;
    char           hx[3 * 40 + 1];
    char           as[40 + 1];
    int            u = 0, k = 0;
    for (; u < 40 && (q[u * 2] || q[u * 2 + 1]); ++u) {
        static const char H[] = "0123456789abcdef";
        hx[k++]               = H[q[u * 2] >> 4];
        hx[k++]               = H[q[u * 2] & 15];
        hx[k++]               = ' ';
        const unsigned cu     = (unsigned)q[u * 2] | ((unsigned)q[u * 2 + 1] << 8);
        as[u]                 = (cu >= 0x20 && cu < 0x7f) ? (char)cu : '.';
    }
    hx[k] = 0;
    as[u] = 0;
    ui_log("; [script]   wmsg: %d code unit(s), low bytes = %s | \"%s\"", u, hx, as);
}

void log_field(int slot) {
    const uint8_t *b = (const uint8_t *)field_addr(slot);
    char           hx[3 * 64 + 1];
    char           as[64 + 1];
    int            n = 0, k = 0;
    const int      lim = (slot == FIELD_LOBBY) ? 63 : 32; // the name globals are char[32]
    for (; n < lim && b[n]; ++n) {
        static const char H[] = "0123456789abcdef";
        hx[k++]               = H[b[n] >> 4];
        hx[k++]               = H[b[n] & 15];
        hx[k++]               = ' ';
        as[n]                 = (b[n] >= 0x20 && b[n] < 0x7f) ? (char)b[n] : '.';
    }
    hx[k] = 0;
    as[n] = 0;
    ui_log("; [script]   field %s: %d byte(s) = %s | \"%s\"",
           slot == FIELD_GAME    ? "game"
           : slot == FIELD_CHAT  ? "chat"
           : slot == FIELD_LOBBY ? "lobby"
                                 : "name",
           n, hx, as);
}

double game_clock_ms() {
    double clk;
    memcpy(&clk, (const void *)GAMECLOCK_ADDR, sizeof(clk));
    return clk * 1000.0; // the global is game-SECONDS (see OP_W_GAMECLOCK)
}

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
        case OP_W_BLDGSEL: {
            const int sel = *(const uint16_t *)BLDGSEL_ADDR();
            return s->a == BLDGSEL_ANY ? sel != 0 : sel == s->a;
        }
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
            // SES1: MH_ProcessDir -- the runner DROPS this file into the directory it discovered at
            // launch. Reader and writer have to name the same folder, and only the process one is
            // known to both sides before a lobby exists.
            wsprintfA(flag, "%srig_%s.flag", MH_ProcessDir(), s->text);
            return GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
        }
        case OP_W_GAMECLOCK: { // the SIM has advanced >= N ms (stalls when the sim stalls -- not a time wait)
            double clk;
            memcpy(&clk, (const void *)GAMECLOCK_ADDR, sizeof(clk));
            return clk * 1000.0 >= (double)s->a; // the global is game-SECONDS (cf. net_internal.h ms_of)
        }
        case OP_W_WINCLIENT: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            RECT cr;
            return hw && GetClientRect(hw, &cr) && cr.right == s->a && cr.bottom == s->b;
        }
        case OP_W_WINSWITCHABLE: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            return hw && mh::gfx::window_switchable(hw, nullptr, 0);
        }
        case OP_W_WINACTIVE: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            return hw && GetForegroundWindow() == hw;
        }
        case OP_W_DIMOUSE:
            return *(volatile int *)MOUSE_LAST_X == s->a && *(volatile int *)MOUSE_LAST_Y == s->b;
        case OP_W_KEYSTATE: {
            const uint8_t ks  = ((volatile uint8_t *)mh::addr::_G_LLM_INPUT_KEYSTATE)[s->a & 0x7f];
            const uint8_t bit = (s->a & 0x80) ? 2 : 1;
            return ((ks & bit) != 0) == (s->b != 0);
        }
        case OP_W_IMGUI: {
            namespace ov = mh::gfx::imgui_overlay;
            if (s->a == 1) return ov::query(ov::query_what::open) == 1;
            if (s->a == 0) return ov::query(ov::query_what::armed) == 1 && ov::query(ov::query_what::open) == 0;
            if (s->a == 3) return ov::query(ov::query_what::open) == 1 && ov::query(ov::query_what::want_mouse) == s->b;
            if (s->a == 4) return ov::query(ov::query_what::di_filtered) >= s->b; // PT-INPUT1: DI records withheld
            return ov::query(ov::query_what::swallowed_mouse) >= s->b;
        }
        case OP_W_STALLED: { // TL-UISTALL: the sim IS lockstep-blocked right now, optionally >= s->a ms
            unsigned long blocked_ms = 0;
            const int     bind       = MH_Lockstep_StallBindingPeer(&blocked_ms);
            return bind >= 0 && blocked_ms >= (unsigned long)(s->a > 0 ? s->a : 0);
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
        case OP_W_TEAM: { // mp:U51: lobby slot's TEAM byte on THIS peer (0 = "-", 1..4 = T1..T4)
            if (s->a < 0 || s->a > 7 || s->b < 0) return false;
            return *(const unsigned char *)(LOBBY_SLOTS_ADDR + s->a * SLOT_STRIDE + 0x0c) == (unsigned char)s->b;
        }
        case OP_W_LOBBYMODE: { // mp:U51: host slot 0's MODE byte on THIS peer (0 FFA, 1 Team)
            if (s->b < 0) return false;
            return (*(const unsigned char *)(LOBBY_SLOTS_ADDR + 0x07) != 0 ? 1 : 0) == s->b;
        }
        case OP_W_FIELD: {    // the menu text-field BUFFER is exactly these bytes (NUL-terminated)
            uint8_t want[72]; // the lobby field holds 63 bytes
            int     wn = field_expect(s->text, want, (int)sizeof(want));
            if (wn < 0) return false; // malformed spec -- the timeout diagnostic names it
            const uint8_t *b = (const uint8_t *)field_addr(s->a);
            for (int i = 0; i < wn; ++i)
                if (b[i] != want[i]) return false;
            return b[wn] == 0; // and nothing after it: an exact buffer, not a prefix
        }
        case OP_W_WMSG: { // the newest floating message CONTAINS this UTF-16 sequence
            uint8_t want[34];
            int     wn = field_expect(s->text, want, (int)sizeof(want));
            if (wn < 0) return false; // malformed spec -- the timeout diagnostic names it
            // A `hex:` spec is already UTF-16LE bytes; a plain-text one is widened here (Latin-1), so
            // an ASCII needle can be written readably and a non-ASCII one goes in as hex.
            uint8_t needle[68];
            int     nn = 0;
            if (strncmp(s->text, "hex:", 4) == 0) {
                for (int i = 0; i < wn; ++i) needle[nn++] = want[i];
            } else {
                for (int i = 0; i < wn && nn + 1 < (int)sizeof(needle); ++i) {
                    needle[nn++] = want[i];
                    needle[nn++] = 0;
                }
            }
            if (nn < 2 || (nn & 1)) return false; // a UTF-16 needle is a whole number of units
            const uint8_t *q     = (const uint8_t *)MSGQ_ADDR;
            int            units = 0;
            while (units < MSGQ_SLOT_UNITS && (q[units * 2] || q[units * 2 + 1])) ++units;
            const int haystack = units * 2;
            for (int off = 0; off + nn <= haystack; off += 2) {
                int k = 0;
                while (k < nn && q[off + k] == needle[k]) ++k;
                if (k == nn) return true;
            }
            return false;
        }
        case OP_W_WLOBBY: { // some lobby log line CONTAINS this UTF-16 sequence
            uint16_t  needle[64];
            const int nn = wlobby_needle(s->text, needle, 64);
            if (nn <= 0) return false; // malformed spec -- the timeout diagnostic names it
            return wlobby_contains(needle, nn);
        }
        case OP_W_RETRYREADY:                      // S8(b): the failed-connect latch-clear has fired -> a corrected-IP re-Connect
            return MH_Seam_S8RetryArmed() >= s->a; // will re-kick. Gates the round-trip test's 2nd Connect.
        case OP_W_LOBBYGEN:                        // mp:GS1: the host's Nth distinct lobby is the one this client has stored now
            return MH_Seam_S3LobbyGen() >= s->a;
        case OP_W_LOBBYPING: // mp:L1f: N slot rows carry a REAL ping number on this peer's screen
            return mh::ui::lobby_ping_measured_rows() >= s->a;
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
        case OP_A_CURSORHOLD:
            // One event per present for `c` presents, then advance. CLICK_RETRY is the existing
            // "stay on this step" contract and it is what increments g_wait, so the counter the
            // watchdog already keeps is the same one that ends the dwell -- no second clock.
            MH_UIDrive_CursorTo(s->a, s->b);
            return (g_wait + 1 >= s->c) ? CLICK_OK : CLICK_RETRY;
        case OP_A_CLICK:
            MH_UIDrive_Click(s->a, s->b);
            break;
        case OP_A_PRESS:
            MH_UIDrive_Press(s->a, s->b);
            break;
        case OP_A_RELEASE:
            MH_UIDrive_Release(s->a, s->b);
            break;
        case OP_A_SIMCLICK:
            return simclick_frame(g_wait, s->a, s->b) ? CLICK_OK : CLICK_RETRY;
        case OP_A_SIMRCLICK:
            return simclick_frame(g_wait, s->a, s->b, true, s->c != 0) ? CLICK_OK : CLICK_RETRY;
        case OP_A_WMCLICK: {
            // Spread over presents like a hand: MOVE on 0 and 1 (a frame is built with the pointer
            // there), DOWN on 2, UP on 3, advance on 4. Posted, not sent: the game's own message loop
            // dispatches them, in order, between presents.
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            const LPARAM xy = MAKELPARAM((WORD)(short)s->a, (WORD)(short)s->b);
            if (g_wait <= 1) {
                PostMessageA(hw, WM_MOUSEMOVE, 0, xy);
                if (g_wait == 0) ui_log("; wmclick %d,%d -- posted to hwnd %p (move)", s->a, s->b, (void *)hw);
                return CLICK_RETRY;
            }
            if (g_wait == 2) {
                PostMessageA(hw, WM_LBUTTONDOWN, MK_LBUTTON, xy);
                return CLICK_RETRY;
            }
            if (g_wait == 3) {
                PostMessageA(hw, WM_LBUTTONUP, 0, xy);
                ui_log("; wmclick %d,%d -- down+up posted", s->a, s->b);
                return CLICK_RETRY;
            }
            break;
        }
        case OP_A_RAWFOCUS:
            mh::input::harness_focus(s->a < 0 ? -1 : s->a ? 1
                                                          : 0);
            break;
        case OP_A_RAWMOUSE: {
            bool ok = false;
            if (s->c == 0) ok = mh::input::harness_mouse_abs_game(s->a, s->b);
            else if (s->c == 1) ok = mh::input::harness_mouse_rel(s->a, s->b);
            else ok = mh::input::harness_mouse_button(s->a, s->c == 2);
            if (!ok) {
                ui_log("; rawmouse -- no owned DirectInput mouse ([input] backend=own, device created?)");
                return CLICK_ABSENT;
            }
            break;
        }
        case OP_A_RAWKEY: {
            const bool e0 = s->b != 0;
            if (s->c == 0 && g_wait == 0) {
                if (!mh::input::harness_key((uint16_t)s->a, e0, true)) return CLICK_ABSENT;
                return CLICK_RETRY; // UP on the next present, so the game's poll sees a press
            }
            if (!mh::input::harness_key((uint16_t)s->a, e0, s->c == 1)) {
                ui_log("; rawkey -- no owned DirectInput keyboard ([input] backend=own, device created?)");
                return CLICK_ABSENT;
            }
            break;
        }
        case OP_A_WINSIZE: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            SendMessageA(hw, WM_ENTERSIZEMOVE, 0, 0);
            RECT r = {0, 0, s->a, s->b};
            AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(hw, GWL_STYLE), FALSE, (DWORD)GetWindowLongA(hw, GWL_EXSTYLE));
            const int ww = r.right - r.left, wh = r.bottom - r.top;
            SetWindowPos(hw, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT cr;
            GetClientRect(hw, &cr); // per-monitor DPI: the real frame may differ from AdjustWindowRectEx's
            if (cr.right != s->a || cr.bottom != s->b)
                SetWindowPos(hw, nullptr, 0, 0, ww + (s->a - cr.right), wh + (s->b - cr.bottom), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetClientRect(hw, &cr);
            SendMessageA(hw, WM_EXITSIZEMOVE, 0, 0);
            RECT ir = {0, 0, 0, 0};
            mh::gfx::owned_image_rect(&ir);
            ui_log("; winsize %dx%d -> client %ldx%ld, image %ld,%ld..%ld,%ld", s->a, s->b, cr.right, cr.bottom, ir.left, ir.top,
                   ir.right, ir.bottom);
            break;
        }
        case OP_A_WINMIN: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            struct Restore {
                static DWORD WINAPI run(LPVOID p) {
                    Sleep((DWORD)(uintptr_t)p);
                    HWND w = *(HWND *)mh::addr::hWnd_main;
                    if (w) ShowWindow(w, SW_RESTORE);
                    return 0;
                }
            };
            HANDLE t = CreateThread(nullptr, 0, &Restore::run, (LPVOID)(uintptr_t)s->a, 0, nullptr);
            if (t) CloseHandle(t);
            ShowWindow(hw, SW_MINIMIZE);
            ui_log("; winmin: window minimised, restore in %d ms (iconic=%d)", s->a, (int)IsIconic(hw));
            break;
        }
        case OP_A_WINAWAY: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            away_start(hw);
            ui_log("; winaway: helper window up, %s", g_away_synth ? "foreground REFUSED (isolated desktop): deactivation delivered as messages"
                                                                   : "the foreground moved to it (real deactivation)");
            break;
        }
        case OP_A_WINRETURN: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            away_return(hw, s->a != 0);
            ui_log("; winreturn%s: activation requested (iconic=%d)", s->a ? " (launcher path)" : "", (int)IsIconic(hw));
            break;
        }
        case OP_A_WINPROBE: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            char       facts[160];
            const bool ok = mh::gfx::window_switchable(hw, facts, sizeof(facts));
            ui_log("; winprobe: switchable=%d fg=%d %s", (int)ok, (int)(GetForegroundWindow() == hw), facts);
            break;
        }
        case OP_A_WINEX: {
            HWND hw = *(HWND *)mh::addr::hWnd_main;
            if (!hw) return CLICK_ABSENT;
            const LONG ex = GetWindowLongA(hw, GWL_EXSTYLE);
            SetWindowLongA(hw, GWL_EXSTYLE, (ex | (LONG)s->a) & ~(LONG)s->b);
            ui_log("; winex: ex-style %08lx -> %08lx", (unsigned long)ex, (unsigned long)GetWindowLongA(hw, GWL_EXSTYLE));
            break;
        }
        case OP_A_RCLICK:
            // Multi-present like cursorhold: g_wait is the frame index while the step retries.
            return rclick_frame(g_wait, s->a, s->b, s->c != 0) ? CLICK_OK : CLICK_RETRY;
        case OP_A_KEY:
            if (s->c == 0) {
                // The DOWN and the UP go into the ring on DIFFERENT presents, not back to back on
                // one: the consumer drains the ring on the frame AFTER the present that queued it
                // (the F4 note at enqueue_key), so a down and an up that arrive in ONE drain are a
                // press the edge detector never sees. `key <sc> shift` already spread its pair for
                // exactly this reason; the plain form did not, and nothing said why not. Now both
                // do -- DOWN on present 0, UP on 1, advance on 2, the same CLICK_RETRY dwell
                // contract cursorhold and rclick use, so the frame watchdog's counter stays the only
                // clock involved. Measured working: `(down)` at 15.750 and `(up)` at 15.766 in a
                // healthy run's mh_uidrive.log, one present apart.
                //
                // THIS DOES NOT FIX tooling:TL-QUIT3-FLAKE, and the first version of this comment
                // claimed it did. It was written against the right symptom -- on a flaked run the
                // key and the following `dump` carry IDENTICAL timestamps while a healthy run has
                // them ~16 ms apart -- and an unproven cause. Re-measured AFTER the change, a flaked
                // run still reads `(down)`, `(up)` and the next step all inside one millisecond, and
                // the ESC menu is still absent 89 s later. Note what that does and does not say: the
                // log stamps at millisecond resolution and these lanes are headless, so at the
                // flaked run's ~887 fps those can be separate presents -- "no present in
                // between" is NOT established. Left in place because the split is independently
                // correct (it is what the shift variant already did, and it is measured working:
                // `(down)` 15.750, `(up)` 15.766 on a healthy run), but it is not that flake's fix
                // and a reader chasing it should not stop at this block.
                if (g_wait == 0) {
                    enqueue_key((uint32_t)s->a, true);
                    ui_log("; key scancode 0x%02x (down)", s->a);
                    return CLICK_RETRY;
                }
                if (g_wait == 1) {
                    enqueue_key((uint32_t)s->a, false);
                    ui_log("; key scancode 0x%02x (up)", s->a);
                    return CLICK_RETRY;
                }
                break;
            }
            // `key <sc> shift` (mp:CH1): multi-present like rclick. The pair goes into the ring on
            // present 0; the latch is re-asserted on presents 0..2 (the consumer drains the ring on
            // the frame AFTER the present that queued it, and the DI poll re-latches the byte every
            // frame -- the D25 convention) and cleared on present 3 so a held Shift cannot stick.
            keystate_shift(true);
            if (g_wait == 0) {
                enqueue_key((uint32_t)s->a, true);
                enqueue_key((uint32_t)s->a, false);
                ui_log("; key scancode 0x%02x (down+up) under a held Shift latch", s->a);
            }
            if (g_wait < 3) return CLICK_RETRY;
            keystate_shift(false);
            break;
        case OP_A_KEYHOLD:
            // DOWN once, on present 0; presents 1..c-1 are the hold itself and inject nothing
            // (the unpaired DOWN is what stays latched); UP on present c-1, mirroring
            // cursorhold's `g_wait + 1 >= s->c` dwell-length contract exactly so `held=` reads
            // set for presents 0..c-2 and clear again once the UP has been drained.
            if (g_wait == 0) {
                enqueue_key((uint32_t)s->a, true);
                ui_log("; key scancode 0x%02x (down, held)", s->a);
            }
            g_keyhold_trace = true; // sample every hold frame (see its declaration)
            if (g_wait + 1 < s->c) return CLICK_RETRY;
            g_keyhold_trace = false;
            enqueue_key((uint32_t)s->a, false);
            ui_log("; key scancode 0x%02x (up, after %d held frame(s))", s->a, s->c);
            break;
        case OP_A_HOTKEY:
            // Multi-present like cursorhold: presents 0 and 1 hold the chord, present 2 releases it
            // and advances. Two held presents, not one, because MH_Overlay_OnPresent runs BEFORE
            // this driver on the same present (net_lockstep on_present), so the poll that sees the
            // chord down is the NEXT present's -- and the release must come after that poll.
            if (g_wait < 2) {
                synth_chord_set(s->a, s->b, true);
                if (g_wait == 0) ui_log("; hotkey %s -- vk 0x%02x mods 0x%x held (synthesised)", s->text, s->a, s->b);
                return CLICK_RETRY;
            }
            synth_chord_set(s->a, s->b, false);
            ui_log("; hotkey %s released", s->text);
            break;
        case OP_A_TYPE: {
            // ONE CHARACTER PER RING DRAIN. The modifier state is a single process-wide table, so it
            // can only be correct for one character at a time -- pushing the whole string at once
            // would type "Привет" with whatever shift state the LAST character wanted. The gate is
            // the ring going empty, which (dequeue == translate) means "the previous character has
            // been translated", so this is a state predicate, not a pace.
            if (g_type_step != g_cur) {
                if (!type_resolve(s->text)) {
                    g_type_step = -1;
                    return CLICK_ABSENT; // reported as a script ABORT, never silently dropped
                }
                g_type_step     = g_cur;
                g_type_progress = GetTickCount();
            }
            if (!key_ring_empty()) {
                // Character g_type_i is still in flight: hold its modifiers. Re-applied every tick
                // rather than once, so a message the game pumps in between cannot quietly reset the
                // key-state table under us.
                if (g_type_i < g_type_n) apply_mod_state(g_type[g_type_i].sh);
                // WHY THE RING IS NOT DRAINING, throttled, in the pump's own terms. A stall here is
                // never "the harness lost an event": it is llm_ui_modal_key_pump declining to latch
                // the next one, and the four numbers below are the whole state machine that decides
                // that: NOW_MS is the menu clock, NEXT_MS the 150 ms accumulator, DEAD_MS the latched key's own
                // timestamp, and dlgflags bit 0x80 means a key is latched but not yet published. Without them the
                // only symptom is a 15 s silence.
                const DWORD t = GetTickCount();
                if (t - g_type_diag_ms > 2000) {
                    g_type_diag_ms = t;
                    ui_log("; [script]   type waiting: ring r=%u w=%u | pump now=%u next=%u dead=%u "
                           "dlgflags=%02x/%02x published sc=%u ch=%u",
                           *(volatile uint32_t *)KREADIDX, *(volatile uint32_t *)KWRITEIDX,
                           *(volatile uint32_t *)mh::addr::_G_LLM_UI_MENU_NOW_MS,
                           *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_STEP_NEXT_MS,
                           *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_STEP_DEADLINE_MS,
                           *(volatile uint8_t *)mh::addr::_G_LLM_DLG_STATE_FLAGS,
                           *(volatile uint8_t *)(mh::addr::_G_LLM_DLG_STATE_FLAGS + 1),
                           *(volatile uint32_t *)mh::addr::_G_LLM_UI_MODAL_KEY_SCANCODE,
                           *(volatile uint16_t *)mh::addr::_G_LLM_UI_MODAL_KEY_ASCII);
                }
                return CLICK_RETRY;
            }
            if (g_type_pushed) { // the ring drained -> that character has been translated
                ++g_type_i;
                g_type_pushed   = false;
                g_type_progress = GetTickCount();
            }
            if (g_type_i >= g_type_n) {
                apply_mod_state(0);
                ui_log("; [script] type '%s': %d character(s) delivered", s->text, g_type_n);
                g_type_step = -1;
                return CLICK_OK;
            }
            const TypeChar &t = g_type[g_type_i];
            apply_mod_state(t.sh);
            enqueue_key(t.sc, true);
            enqueue_key(t.sc, false);
            g_type_pushed = true;
            ui_log("; [script] type #%d/%d U+%04X -> sc=0x%02x vk=0x%02x sh=%u | layout byte 0x%02x, "
                   "game ToAscii byte 0x%02x%s",
                   g_type_i + 1, g_type_n, (unsigned)t.wc, t.sc, t.vk, t.sh, t.native, t.acp,
                   (t.native != t.acp)
                       ? "  <-- CODEPAGE GAP: the layout resolves it, the game's ToAscii (CP_ACP) does "
                         "not -- this is F3, not a harness fault"
                       : "");
            return CLICK_RETRY;
        }
        case OP_A_LAYOUT:
            if (!set_layout(s->text)) return CLICK_ABSENT;
            break;
        case OP_A_KEYJOURNAL: {
            if (lstrcmpiA(s->text, "mark") == 0) {
                if (g_keyj_nseg >= KEYJ_SEGS) {
                    ui_log("; [script] keyjournal: too many segments (max %d)", KEYJ_SEGS);
                    return CLICK_ABSENT;
                }
                g_keyj_seg[g_keyj_nseg++] = g_keyj_n;
                ui_log("; [script] keyjournal mark #%d at event %d", g_keyj_nseg, g_keyj_n);
                break;
            }
            if (lstrcmpiA(s->text, "same") != 0) {
                ui_log("; [script] keyjournal: unknown sub-verb '%s' (want `mark` or `same`)", s->text);
                return CLICK_ABSENT;
            }
            if (g_keyj_nseg < 2) {
                ui_log("; [script] keyjournal same: needs TWO `keyjournal mark` segments, have %d",
                       g_keyj_nseg);
                return CLICK_ABSENT;
            }
            const int a0 = g_keyj_seg[g_keyj_nseg - 2], a1 = g_keyj_seg[g_keyj_nseg - 1];
            const int b0 = a1, b1 = g_keyj_n;
            log_keyj("A", a0, a1);
            log_keyj("B", b0, b1);
            if (a1 - a0 != b1 - b0) {
                ui_log("; [script] keyjournal same: LENGTHS DIFFER (%d vs %d)", a1 - a0, b1 - b0);
                return CLICK_ABSENT;
            }
            for (int i = 0; i < a1 - a0; ++i) {
                if (g_keyj[a0 + i].scancode == g_keyj[b0 + i].scancode &&
                    g_keyj[a0 + i].event_type == g_keyj[b0 + i].event_type)
                    continue;
                ui_log("; [script] keyjournal same: MISMATCH at event %d -- A %02x/%03x vs B %02x/%03x", i,
                       g_keyj[a0 + i].scancode, g_keyj[a0 + i].event_type, g_keyj[b0 + i].scancode,
                       g_keyj[b0 + i].event_type);
                return CLICK_ABSENT;
            }
            ui_log("; [script] keyjournal same: IDENTICAL (%d events, byte for byte)", a1 - a0);
            break;
        }
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
    if (g_wait == 0) {
        g_step_t0 = GetTickCount(); // first tick on this step -- start its clock
        if (s->retry_ms) s->retry_clk0 = game_clock_ms();
    }
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
        } else if (s->retry_ms > 0 && game_clock_ms() - s->retry_clk0 >= (double)s->retry_ms) {
            // `retry`: the sim ran <ms> without the wait holding -- the click that should have caused
            // it was lost. Rewind to it, bounded; exhausted is a terminal ABORT naming the step.
            if (s->retry_used >= s->retry_tries) {
                ui_log("; [script] ABORT at step %d: retry exhausted -- %d re-attempt(s), the wait never held: %s",
                       g_cur, s->retry_used, s->raw);
                ui_log("; [script]   active screen at abort:");
                MH_UIDrive_DumpWidgets();
                MH_Capture_Shot("script_timeout");
                g_script_done = true;
                return;
            }
            ++s->retry_used;
            ui_log("; [script] RETRY %d/%d at step %d after %d game-ms: rewinding %d step(s) to: %s", s->retry_used,
                   s->retry_tries, g_cur, s->retry_ms, s->retry_back, g_steps[g_cur - s->retry_back].raw);
            g_cur -= s->retry_back;
            g_wait = 0;
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
            // A `field` timeout is answered by the bytes themselves -- "expected these, the buffer
            // holds those" is the whole diagnosis, and for text the fonts cannot draw it is also the
            // only one a capture could never give.
            if (s->op == OP_W_FIELD) log_field(s->a);
            if (s->op == OP_W_WMSG) log_wmsg();
            if (s->op == OP_W_WLOBBY) log_wlobby();
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
            //
            // `type` IS BUDGETED IN WALL CLOCK, NOT FRAMES, and that is deliberate. Its pace is set
            // by llm_ui_modal_key_pump's 150 ms fixed-step accumulator -- a wall-clock quantity --
            // while the frame budget is denominated in presents, which headless runs at a rate two
            // orders of magnitude off a visible one. A frame budget would therefore mean a different
            // number of characters on every lane. Measured FROM PROGRESS (the last character
            // actually consumed), the same origin fix the `S` barrier's escape hatch needed: a
            // string that is still being typed keeps its budget however slowly it types, and only a
            // run where nothing is moving spends it.
            ++g_wait;
            const bool type_stalled = (s->op == OP_A_TYPE) &&
                                      (int)(GetTickCount() - g_type_progress) > g_type_stall_ms;
            if (s->op == OP_A_TYPE ? type_stalled : (g_wait > g_timeout)) {
                ui_log("; [script] TIMEOUT at step %d after %d frames (%u.%03us) -- ABORT: %s (%s)",
                       g_cur, g_wait, step_ms / 1000, step_ms % 1000, s->raw,
                       s->op == OP_A_TYPE ? "the key ring stopped draining -- is the text field "
                                            "focused? a menu field only accepts keys once clicked"
                                          : "target present but never became clickable");
                if (s->op == OP_A_TYPE)
                    ui_log("; [script]   type stalled at character %d/%d (sc=0x%02x), ring read=%u "
                           "write=%u",
                           g_type_i + 1, g_type_n, g_type_i < g_type_n ? g_type[g_type_i].sc : 0,
                           *(volatile uint32_t *)KREADIDX, *(volatile uint32_t *)KWRITEIDX);
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
    // U25 step 1, unconditional and one-shot: the first present is the earliest point that is
    // provably after the game's own DirectInput init, so this is where the answer is readable.
    log_di_keyboard_once();
    mh::input::on_present(); // PT-INPUT1: module check, [input] mouse_trace element / key_trace ring lines
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
        const bool was_done = g_script_done;
        script_tick();
        // RESTORE THE KEYBOARD LAYOUT ON EVERY EXIT PATH, not just a clean `end`. There are five
        // ways a script finishes (end, running off the last step, a wait TIMEOUT, an action ABORT, a
        // predicate's own refusal) and a `layout 00000419` left behind by any of them would be a
        // side effect on the machine that outlives the run. One place, checked once per present.
        if (g_script_done && !was_done) restore_layout();
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
    mh::config::read_ini_string("uitest", "click_label", "", g_auto_label, sizeof(g_auto_label), // TL-HARN4
                                ini);
    g_auto_value    = GetPrivateProfileIntA("uitest", "click_value", -1, ini); // sprite-menu id (-1 = unset)
    g_settle_frames = GetPrivateProfileIntA("uitest", "settle_frames", 2, ini);
    if (g_settle_frames < 1) g_settle_frames = 1;
    // How long the center must hold still for `settled` (ms). A DURATION so the predicate means the
    // same thing at 60 fps and at headless rates -- see the OP_W_SETTLED comment.
    g_settle_ms = GetPrivateProfileIntA("uitest", "settle_ms", 120, ini);
    if (g_settle_ms < 0) g_settle_ms = 0;
    g_dump_screens = GetPrivateProfileIntA("uitest", "dump_screens", 0, ini) != 0;
    g_timeout      = GetPrivateProfileIntA("uitest", "timeout_frames", 1500, ini);
    // `type`'s stall budget, in MILLISECONDS since the last character was consumed -- see the
    // script_tick comment for why this one op is not budgeted in frames.
    g_type_stall_ms = GetPrivateProfileIntA("uitest", "type_stall_ms", 15000, ini);

    // Script mode: [uitest] script=<file> (relative to the exe dir). Loaded once here.
    char scriptname[64];
    mh::config::read_ini_string("uitest", "script", "", scriptname, sizeof(scriptname), ini); // TL-HARN4
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
