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
// with the take-over off (the lobby-slide notes, measurement M4; that control knob is retired). It is kept because "no scenario
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

// Captured by the naked detours before `pushad` hides them. extern "C" because the asm names them.
extern "C" int g_iw_dir = 0; // the EAX argument: 0 = slide IN, 1 = slide OUT
extern "C" int g_iw_ret = 0; // the caller's return address -- WHICH call site this is
extern "C" int g_st_dir = 0; // llm_ui_menu_screen_slide_transition's EAX / [esp], same idea
extern "C" int g_st_ret = 0;

namespace {
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
    // mh::addr::map_picker_widget_origin,     } gated on the rendered title.
};

} // namespace

// ---- the intro-wait take-over -------------------------------------------------------------------
//
// Returns 1 = "handled, skip the retail body", 0 = "hand off". Called from the naked detour with the
// direction and the caller already captured.
extern "C" int mh_intro_wait_take_over() {
    // U38, THE DOUBLE SLIDE-IN, and it is FIRST because it is a retail pairing bug rather than a
    // latency fix -- it must be suppressed whatever the role or the session state.
    // llm_mp_netsetup_ip_field_cancel calls show_intro_wait(0) at 0x004bd851 and then, at the very
    // next instruction, llm_mp_local_browser_setup -- which ends with its OWN show_intro_wait(0).
    // Two 400 ms slide-ins back to back. The sibling llm_mp_session_browser_back_to_local_list does
    // show_intro_wait(1) + setup(), one out and one in, which is the correct pairing. Suppress the
    // FIRST and let setup's own call do the single slide. Suppressing rather than byte-patching
    // keeps it out of the patch manifest.
    if ((uintptr_t)g_iw_ret == mh::addr::netsetup_ip_cancel_intro_wait_ret) {
        static int logged = 0;
        if (ui_verbose() && logged < 4) {
            ++logged;
            ui_log("; slide: suppressed the redundant ip-cancel slide-in (local_browser_setup does it)\n");
        }
        return 1;
    }

    // The gate, in the order the three questions get cheaper to answer.
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
    // The direction is honoured. Before 2026-08-29 this always wrote the IN state, which pinned the
    // lobby ON-SCREEN during the very call whose job is to hide it (llm_ui_lobby_screen_widget_reset's
    // show_intro_wait(1), leaving the browser drawn underneath a dead lobby's slot rows).
    const int dir     = (g_iw_dir != 0) ? SLIDE_OUT : SLIDE_IN;
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
// unconditionally.
//
// U38, THE CREATE-GAME FREEZE (user-reported 2026-08-30: "clicking Create game there is a small
// freeze -- still presenting, cursor disappears, nothing moves, then the animation starts").
// llm_mp_create_game_action (0x004bd99c) runs slide_transition(1) -- animating NET_SETUP's frame
// 0x006516d3 -- while the LOCAL BROWSER is still the active list, whose frame is 0x00651293. So it
// blocks ~400 ms doing centre+draw+present_flip on a screen where the widget it is moving is not
// drawn, and these loops never call llm_ui_frame_tick, which is what draws the cursor. Presenting,
// cursorless, motionless: a freeze. MEASURED, not inferred: an entry trace over the full menu walk logged
// TEN slides and this was the ONLY one whose widget the active list did not draw. Suppressing it removes 400 ms of dead time and
// changes nothing visible -- the retail body's only side effect besides the loop is refreshing
// 0x006516d3's width, which the next screen's own slide_transition(0) redoes before it animates.
extern "C" int mh_slide_transition_observe() {
    if ((uintptr_t)g_st_ret == mh::addr::create_game_dead_slide_ret) {
        static int logged = 0;
        if (ui_verbose() && logged < 4) {
            ++logged;
            ui_log("; slide: suppressed the Create-game dead slide (wrong screen's frame, 400ms of nothing)\n");
        }
        return 1;
    }
    return 0;
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
        jmp  dword ptr [g_iw_tramp] // tail-jmp to the retail body
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
        jmp  dword ptr [g_st_tramp]
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

// Armed unconditionally: carries the Create-game dead-slide suppression.
bool install_screen_slide_observer() {
    const bool ok = install_trampoline(mh::addr::llm_ui_menu_screen_slide_transition,
                                       (void *)slide_transition_detour, &g_st_tramp, 8,
                                       entry_claim::exclusive, "the screen-slide observer");
    if (!ok) ui_log("; screen-slide observer NOT armed -- the Create-game dead slide will run\n");
    return ok;
}

} // namespace ui
} // namespace mh
