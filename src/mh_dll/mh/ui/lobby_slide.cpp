//
// ===== THE MENU SLIDE TAKE-OVER (U3b / U22 / U29 / U37 / U38) ====================================
//
// READ the lobby-slide notes BEFORE CHANGING ANYTHING HERE. This block was re-derived from live
// instrumentation at fork F3E; the pre-F3E comment that used to stand in this place carried three
// claims that are all measured FALSE, and one open question that turned out to rest on withdrawn
// evidence. The document holds the measurements, the false claims and why each died. What follows
// is only what survived it, stated once.
//
// WHAT THE RETAIL FUNCTION IS. `llm_lobby_show_intro_wait(dir)` is a GENERIC menu-screen slide --
// 13 call sites across the lobby, both browsers, the map picker and the netsetup screens. It is a
// blocking ~400 ms tween that presents from the inside, moving the SHARED frame widget
// `llm_ui_widget_00651293` between two end states and re-deriving the right panel and the active
// widget list's centring from it each iteration. `dir` picks the end state: 0 = on-screen,
// 1 = parked off-screen left. `llm_ui_menu_screen_slide_transition` is its sibling, same shape,
// animating NET_SETUP's own frame `0x006516d3`.
//
// MEASURED, and this is the part that replaced the folklore: BOTH loops always terminate, always in
// ~400 ms, always leaving the frame exactly at the end state their direction names, and the shared
// tween accumulator reads 0 at every entry and every exit. There is no frozen clock, no
// non-terminating loop, and no slide that fails to bring the frame back (the lobby-slide notes,
// measurements M1-M3).
//
// SO WHAT IS THE TAKE-OVER FOR. Exactly one thing: the 400 ms is spent INSIDE
// `llm_lobby_host_net_dispatch`, upstream of that function's own packet-poll section, so a client
// entering the lobby stalls its lobby protocol for a frame-time it does not owe. Settling the slide
// at the end state its direction names and returning gets the dispatch to its poll immediately, at
// the same geometry retail would have reached. That is why this file says SETTLE, not "park" --
// the take-over is a zero-duration slide, not a different destination.
//
// WHAT IT IS NOT FOR ANY MORE. It is not what makes the client's lobby render: that is the U3/U7
// snap in lobby_widgets.cpp, which runs first and independently (measured in the same traces). And
// as of F3E it is not load-bearing for any registered scenario -- the whole client MP route passes
// with `[net] u3b_park=0` (the lobby-slide notes, measurement M4). It is kept because "no scenario
// needs it today" is not "dead" and because the latency it removes is real; the disposition
// question is filed for F4/F5, not answered here.
//
// =================================================================================================
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "ui/ui_internal.h"
#include "addr/mh_addrs.gen.h" // generated EN VAs (tools/gen_dll_addrs.py)
#include "hook/detour.h"       // install_trampoline (shared inline-detour toolkit)
#include "hook/watcall.h"      // call_watcall1 (Watcom __watcall(EAX) bridge)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::entry_claim;
using mh::hook::install_trampoline;
using mh::ui::detail::SLIDE_IN;
using mh::ui::detail::SLIDE_OUT;
using mh::ui::detail::ui_log;
using mh::ui::detail::ui_verbose;

// ---- the ini knobs ------------------------------------------------------------------------------
//
// Still `[net]` keys, still answered by launch.cpp's config accessors. They are about the MENU, not
// the wire, and the section is a leftover from when every knob in this DLL was -- recorded for F4,
// which is where the ini can move a key without breaking a lane in flight. These are plain config
// reads, so calling them is not a transport dependency.
extern "C" int MH_Cfg_U29SlideDir(void);      // [net] u29_slide_dir -- honour `dir` (negative arm)
extern "C" int MH_Cfg_U3bPark(void);          // [net] u3b_park=0 -- the take-over off entirely (the control)
extern "C" int MH_Cfg_DedupCancelSlide(void); // [net] dedup_cancel_slide -- the U38 double slide-in
extern "C" int MH_Cfg_SlideDiag(void);        // [net] slide_diag -- log every slide entry + exit
extern "C" int MH_Cfg_DedupDeadSlide(void);   // [net] dedup_dead_slide -- the U38 Create-game freeze

// Captured by the naked detours before `pushad` hides them. extern "C" because the asm names them.
extern "C" int g_iw_dir = 0; // the EAX argument: 0 = slide IN, 1 = slide OUT
extern "C" int g_iw_ret = 0; // the caller's return address -- WHICH call site this is
extern "C" int g_st_dir = 0; // llm_ui_menu_screen_slide_transition's EAX / [esp], same idea
extern "C" int g_st_ret = 0;

namespace {
// Decides whether the detours wrap the retail body to read the geometry it leaves, or keep their
// plain tail-jmp. Only ever set from slide_diag, so the shipped path is unchanged.
int g_slide_wrap = 0;
// The two detours' captured return values from their take-over/observe call.
int g_iw_taken = 0;
int g_st_taken = 0;
// install_trampoline writes the stolen-prologue thunk here.
void *g_iw_tramp = nullptr;
void *g_st_tramp = nullptr;
} // namespace

namespace mh {
namespace ui {
namespace detail {

// The two end states. Transcribed from the retail body; see ui_internal.h for why this is the only
// copy. Returns false when the container has not been laid out yet (sprite widths unusable), which
// callers must read as "leave retail alone" -- a slide whose geometry we cannot compute is a slide
// we must not replace.
bool slide_end_state(int dir, int *out_frame_x, int *out_right_x) {
    const int refw  = *(const int *)mh::addr::lobby_frame_ref_width;  // right panel's sprite width
    const int leftw = *(const int *)mh::addr::lobby_left_frame_width; // frame's sprite width
    if (refw <= 0 || refw >= 1024 || leftw <= 0 || leftw >= 1024) return false;
    const int in_x  = -(refw / 2);    // the slide-IN target
    const int out_x = -0x140 - leftw; // the slide-OUT park
    const int x     = (dir == SLIDE_OUT) ? out_x : in_x;
    *out_frame_x    = x;
    // The retail loop re-derives the right panel from the frame every iteration; same formula.
    *out_right_x = leftw + ((x > in_x ? x - in_x : in_x - x) * 2);
    return true;
}

} // namespace detail
} // namespace ui
} // namespace mh

namespace {

// ---- the instruments ([net] slide_diag) ---------------------------------------------------------
//
// These are what re-derived the mechanism, and they are kept armed-but-silent for the same reason:
// a pixel diff compares layout and screen state at once, so it can never be the evidence for a claim
// about this code. An entry/exit pair naming fx/rx/elapsed can.

// Is `w` one of the drawn children of widget list `wl`? The membership that decides whether a 400 ms
// blocking slide is an animation or a stall: both loops centre+draw the ACTIVE list every iteration,
// but each animates one FIXED widget, and the two loops animate different ones. Bounded walk over
// the NULL-terminated children array; returns -1 if the list pointer is unusable.
int slide_widget_is_drawn(const void *wl, uintptr_t w) {
    if (!wl) return -1;
    // FRAME FIRST, and it is the case that matters: both animated widgets are their containers'
    // FRAME (llm_ui_widget_list.frame, +0x08) -- 0x00651293 for the four MP containers, 0x006516d3
    // for NET_SETUP -- not entries in children[]. Testing children alone reports "not drawn" for
    // every slide in the game, which is what the first version of this did.
    if (*(void *const *)((const char *)wl + 8) == (void *)w) return 1;
    void **kids = *(void ***)wl; // llm_ui_widget_list.children (+0x00)
    if (!kids) return -1;
    for (int i = 0; i < 64 && kids[i]; ++i)
        if (kids[i] == (void *)w) return 1;
    return 0;
}

// One line per slide entry, i.e. where the PREVIOUS slide left things. `fn` names which loop, `ret`
// which call site. `el` is the anim struct's elapsed accumulator, which is also slide_tick's
// "first call" sentinel: 0 means the last user completed cleanly, non-zero means it was abandoned
// mid-tween -- and BOTH loops share that one struct, so a non-zero here would be a cross-call
// coupling between them. It has never been observed non-zero (the lobby-slide notes, M3).
void slide_diag_log(const char *fn, int dir, int ret, uintptr_t widget) {
    if (!MH_Cfg_SlideDiag()) return;
    const void *wl    = *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    const int   drawn = slide_widget_is_drawn(wl, widget);
    const int   fx    = *(const int *)mh::addr::lobby_frame_x;
    const int   rx    = *(const int *)mh::addr::lobby_right_panel_x;
    const int   el    = *(const int *)mh::addr::slide_anim_elapsed;
    char        b[288];
    wsprintfA(b, "; slide %s dir=%d ret=%08X list=%08X widget=%08X drawn=%s fx=%d rx=%d el=%d t=%u\n", fn,
              dir, (unsigned)ret, (unsigned)wl, (unsigned)widget,
              drawn < 0 ? "?" : (drawn ? "yes" : "NO -- 400ms of nothing"), fx, rx, el,
              (unsigned)GetTickCount());
    ui_log(b);
}

// The SAME geometry read, taken after the retail body returns. Entry-only logging could only ever
// report where the PREVIOUS slide finished, which cannot distinguish "this loop did nothing" from
// "this loop worked and something after it reset the frame" -- and telling those apart is what the
// exit pair is for.
void slide_exit_log(const char *fn, int dir, int ret) {
    if (!MH_Cfg_SlideDiag()) return;
    const int fx = *(const int *)mh::addr::lobby_frame_x;
    const int rx = *(const int *)mh::addr::lobby_right_panel_x;
    const int el = *(const int *)mh::addr::slide_anim_elapsed;
    char      b[224];
    wsprintfA(b, ";   slide/exit %s dir=%d ret=%08X fx=%d rx=%d el=%d t=%u\n", fn, dir, (unsigned)ret, fx,
              rx, el, (unsigned)GetTickCount());
    ui_log(b);
}

// ---- the gate -----------------------------------------------------------------------------------
//
// THE SCOPE IS THE CALL SITE, NOT THE SCREEN -- and the screen is how the call site is identified.
// The take-over applies to the lobby container only. Three earlier scopes are recorded because each
// was tried and cost something:
//
//   * "client, on one of four containers" (U22, 2026-08-29) ate the browser and map-picker
//     animations on every route (U36). The three extra containers are kept below, commented, with
//     the history -- re-widening is a one-line change and the reason not to is written down.
//   * "`!*is_host`" alone never meant "client": _G_LLM_NET_IS_HOST is zero for a player walking
//     browser -> Create -> map picker to HOST a game, so that stretch had its animation eaten too.
//     The session test is what excludes it, and it excludes nothing else -- every case U3b/U29/U22
//     fixed has a live session by construction.
//   * "the two call sites with the dispatch on the stack" (tried 2026-08-30) is, on every measured
//     route, the SAME SET this gate selects -- the only two show_intro_wait calls made while the
//     lobby is the active list are 0x004c0176 and 0x004bf044. The note that used to stand here
//     saying that scope FAILED rested on two capture diffs that were, three commits later, proven
//     to be capture-gating races and never re-measured (the lobby-slide notes, M5).
constexpr uintptr_t k_slide_containers[] = {
    mh::addr::lobby_widget_origin,
    // mh::addr::local_browser_widget_origin,  } U22, retired 2026-08-30 by U37: the failures these
    // mh::addr::browser_widget_array_ptr,     } masked were capture-gating races in c3/c7, both now
    // mh::addr::map_picker_widget_origin,     } gated on the rendered title. [net] u3b_park=0 is the control.
};

} // namespace

// ---- the intro-wait take-over -------------------------------------------------------------------
//
// Returns 1 = "handled, skip the retail body", 0 = "hand off". Called from the naked detour with the
// direction and the caller already captured.
extern "C" int mh_intro_wait_take_over() {
    g_slide_wrap = MH_Cfg_SlideDiag(); // decides whether the detours wrap the body to log its exit
    slide_diag_log("intro_wait", g_iw_dir, g_iw_ret, mh::addr::slide_frame_widget_shared);

    // U38, THE DOUBLE SLIDE-IN, and it is FIRST because it is a retail pairing bug rather than a
    // latency fix -- it must be suppressed whatever the role or the session state.
    // llm_mp_netsetup_ip_field_cancel calls show_intro_wait(0) at 0x004bd851 and then, at the very
    // next instruction, llm_mp_local_browser_setup -- which ends with its OWN show_intro_wait(0).
    // Two 400 ms slide-ins back to back. The sibling llm_mp_session_browser_back_to_local_list does
    // show_intro_wait(1) + setup(), one out and one in, which is the correct pairing. Suppress the
    // FIRST and let setup's own call do the single slide. Suppressing rather than byte-patching
    // keeps it reversible from the ini and out of the patch manifest.
    if (MH_Cfg_DedupCancelSlide() && (uintptr_t)g_iw_ret == mh::addr::netsetup_ip_cancel_intro_wait_ret) {
        static int logged = 0;
        if (ui_verbose() && logged < 4) {
            ++logged;
            ui_log("; slide: suppressed the redundant ip-cancel slide-in (local_browser_setup does it)\n");
        }
        return 1;
    }

    // The gate, in the order the three questions get cheaper to answer.
    if (!MH_Cfg_U3bPark()) return 0;                    // the negative control -- retail everywhere
    if (!mh::ui::detail::ui_client_session()) return 0; // client, and a session exists
    void     *wl       = *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    const int is_lobby = (wl == (void *)mh::addr::lobby_widget_origin);
    int       known    = 0;
    for (uintptr_t c : k_slide_containers)
        if (wl == (void *)c) known = 1;
    if (!known) return 0; // some other screen -- leave retail alone

    // SETTLE: write the end state this direction names, then re-centre, which is what the retail
    // loop does on every iteration and the only side effect of it that outlives the tween.
    //
    // [net] u29_slide_dir=0 ignores `dir` and always writes the IN state -- what this function did
    // before 2026-08-29, and what pinned the lobby ON-SCREEN during the very call whose job is to
    // hide it (llm_ui_lobby_screen_widget_reset's show_intro_wait(1), leaving the browser drawn
    // underneath a dead lobby's slot rows). The negative arm for the render fix.
    const int dir     = (g_iw_dir != 0 && MH_Cfg_U29SlideDir()) ? SLIDE_OUT : SLIDE_IN;
    int       frame_x = 0, right_x = 0;
    if (!mh::ui::detail::slide_end_state(dir, &frame_x, &right_x)) return 0; // not laid out -- let it run
    *(int *)mh::addr::lobby_frame_x       = frame_x;
    *(int *)mh::addr::lobby_right_panel_x = right_x;
    // Centre only. Adding the loop's list_draw here -- the obvious "be faithful to the retail end
    // state" move -- REGRESSED match_launch (2026-07-28), so the redraw is left to the normal frame
    // path. Consequence recorded separately: the client's lobby can be captured before its
    // header and map-info are refreshed, which is what host_recreate's c1_lobby still sees.
    mh::hook::call_watcall1(mh::addr::llm_ui_widget_list_center, wl);

    static int logged_in = 0, logged_out = 0;
    if (ui_verbose() && g_iw_dir == 0 && logged_in < 6) {
        ++logged_in;
        char b[128];
        wsprintfA(b, "; U3b lobby-dispatch slide-IN parked at its end state (list=%08X lobby=%d ret=%08X)\n",
                  (unsigned)wl, is_lobby, (unsigned)g_iw_ret);
        ui_log(b);
    }
    if (ui_verbose() && g_iw_dir != 0 && logged_out < 6) {
        ++logged_out;
        char b[128];
        wsprintfA(b, "; U29 lobby-dispatch slide-OUT parked off-screen (list=%08X lobby=%d ret=%08X)\n",
                  (unsigned)wl, is_lobby, (unsigned)g_iw_ret);
        ui_log(b);
    }
    return 1;
}

// ---- the second loop ----------------------------------------------------------------------------
//
// `llm_ui_menu_screen_slide_transition`. Observe-only apart from one suppression, and armed
// unconditionally so the slide_diag trace has no holes -- without it the trace is missing exactly
// the interleavings that made the mechanism readable (llm_mp_netsetup_enter_game_name_screen and
// llm_mp_create_game_action each run one of each, back to back).
//
// U38, THE CREATE-GAME FREEZE (user-reported 2026-08-30: "clicking Create game there is a small
// freeze -- still presenting, cursor disappears, nothing moves, then the animation starts").
// llm_mp_create_game_action (0x004bd99c) runs slide_transition(1) -- animating NET_SETUP's frame
// 0x006516d3 -- while the LOCAL BROWSER is still the active list, whose frame is 0x00651293. So it
// blocks ~400 ms doing centre+draw+present_flip on a screen where the widget it is moving is not
// drawn, and these loops never call llm_ui_frame_tick, which is what draws the cursor. Presenting,
// cursorless, motionless: a freeze. MEASURED, not inferred: slide_diag over the full menu walk logs
// TEN slides and this is the ONLY one with drawn=NO. Suppressing it removes 400 ms of dead time and
// changes nothing visible -- the retail body's only side effect besides the loop is refreshing
// 0x006516d3's width, which the next screen's own slide_transition(0) redoes before it animates.
extern "C" int mh_slide_transition_observe() {
    g_slide_wrap = MH_Cfg_SlideDiag();
    slide_diag_log("screen_slide", g_st_dir, g_st_ret, mh::addr::slide_frame_widget_netsetup);
    if (MH_Cfg_DedupDeadSlide() && (uintptr_t)g_st_ret == mh::addr::create_game_dead_slide_ret) {
        static int logged = 0;
        if (ui_verbose() && logged < 4) {
            ++logged;
            ui_log("; slide: suppressed the Create-game dead slide (wrong screen's frame, 400ms of nothing)\n");
        }
        return 1;
    }
    return 0;
}

extern "C" void mh_intro_wait_exit(void) {
    slide_exit_log("intro_wait", g_iw_dir, g_iw_ret);
}
extern "C" void mh_slide_transition_exit(void) {
    slide_exit_log("screen_slide", g_st_dir, g_st_ret);
}

// clang-format off
// __watcall(param in EAX) -> EAX. Either hand off to the original (EAX preserved across pushad/popad)
// or return 1, which is what the retail body returns.
__declspec(naked) void intro_wait_detour() {
    __asm {
        push eax                    // capture the CALLER first -- which call site this is decides
        mov  eax, [esp+4]           //   whether we may settle (esp+0 = saved eax, esp+4 = return addr).
        mov  g_iw_ret, eax          //   push/mov/pop touch no flags, so EAX+flags reach the body intact.
        pop  eax
        mov  g_iw_dir, eax          // the direction argument, before pushad hides it (0=in, 1=out)
        pushad
        pushfd
        call mh_intro_wait_take_over
        mov  g_iw_taken, eax
        popfd
        popad
        cmp  dword ptr [g_iw_taken], 0
        jne  settled
        cmp  dword ptr [g_slide_wrap], 0
        jne  wrapped
        jmp  dword ptr [g_iw_tramp] // shipped path: unchanged tail-jmp
    wrapped:                        // slide_diag: run the body, then read the geometry it left
        call dword ptr [g_iw_tramp]
        pushad                      // pushad/popad restore EAX, so the body's return value survives
        pushfd
        call mh_intro_wait_exit
        popfd
        popad
        ret
    settled:
        mov  eax, 1
        ret
    }
}

// Observe-only sibling: same capture, then always hand off unless the U38 suppression fires.
__declspec(naked) void slide_transition_detour() {
    __asm {
        push eax
        mov  eax, [esp+4]
        mov  g_st_ret, eax
        pop  eax
        mov  g_st_dir, eax
        pushad
        pushfd
        call mh_slide_transition_observe
        mov  g_st_taken, eax
        popfd
        popad
        cmp  dword ptr [g_st_taken], 0
        jne  skipped
        cmp  dword ptr [g_slide_wrap], 0
        jne  wrapped2
        jmp  dword ptr [g_st_tramp]
    wrapped2:
        call dword ptr [g_st_tramp]
        pushad
        pushfd
        call mh_slide_transition_exit
        popfd
        popad
        ret
    skipped:
        mov  eax, 1
        ret
    }
}
// clang-format on

namespace mh {
namespace ui {

// Whole-body-conditional: the detour hands off to the original everywhere except the lobby's own
// slide on a client in a session, where the take-over settles it instead.
bool install_lobby_slide_takeover() {
    const bool ok = install_trampoline(mh::addr::llm_lobby_show_intro_wait, (void *)intro_wait_detour,
                                       &g_iw_tramp, 8, entry_claim::exclusive,
                                       "the U3b client lobby-slide park");
    if (!ok) ui_log("; U3b NOT armed -- the client lobby slide will keep animating (settled never fires)\n");
    return ok;
}

// Armed unconditionally: the detour costs one predictable branch when the diag is off, and the
// trace is only readable with both loops in it.
bool install_screen_slide_observer() {
    const bool ok = install_trampoline(mh::addr::llm_ui_menu_screen_slide_transition,
                                       (void *)slide_transition_detour, &g_st_tramp, 8,
                                       entry_claim::exclusive, "the slide_diag screen-slide observer");
    if (!ok)
        ui_log("; slide_diag: screen-slide observer NOT armed -- that loop will be missing from the trace\n");
    return ok;
}

// U37's third instrument: WHO ELSE writes the frame geometry. The entry/exit pairs prove every slide
// completes correctly, so a value that changes between them is changed by something that is not a
// slide. Log-on-change from the present hook catches that writer without guessing who it is.
void slide_geom_watch() {
    if (!MH_Cfg_SlideDiag()) return;
    static int  last_fx = 0x7fffffff, last_rx = 0x7fffffff, last_ox = 0x7fffffff, last_ow = 0x7fffffff;
    const int   fx = *(const int *)mh::addr::lobby_frame_x;
    const int   rx = *(const int *)mh::addr::lobby_right_panel_x;
    const char *wl = *(const char **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    // The CONTAINER's laid-out box, which is what actually places the panels on screen:
    // llm_ui_widget_list_center writes origin_x/y (+0x18/+0x1c) and width/height (+0x20/+0x24) from
    // the FRAME widget's sprite size. fx/rx above are widget-local; these are the screen placement,
    // and the 2026-08-30 measurement showed fx/rx CORRECT on a visibly off-centre frame -- so the
    // offset is here.
    const int ox = wl ? *(const int *)(wl + 0x18) : -1;
    const int ow = wl ? *(const int *)(wl + 0x20) : -1;
    if (fx == last_fx && rx == last_rx && ox == last_ox && ow == last_ow) return;
    char b[240];
    wsprintfA(b, ";   slide/geom CHANGED fx %d->%d rx %d->%d origin_x %d->%d width %d->%d (list=%08X) t=%u\n",
              last_fx, fx, last_rx, rx, last_ox, ox, last_ow, ow, (unsigned)wl, (unsigned)GetTickCount());
    last_fx = fx;
    last_rx = rx;
    last_ox = ox;
    last_ow = ow;
    ui_log(b);
}

} // namespace ui
} // namespace mh
