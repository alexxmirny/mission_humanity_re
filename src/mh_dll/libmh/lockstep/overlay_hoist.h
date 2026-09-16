//
// lockstep/overlay_hoist.h -- LIB-ABI stage E: the hoisted load-bearing writes of the four
// GAME_MODE overlay callees (llm_net_lockstep_overlay_dismiss / _sync_overlay_show /
// _wait_player_overlay_show / llm_net_mp_leave_reset_game_mode), shared by every libmh call
// site so the split's predicate lives ONCE, not five times.
//
// THE DOCTRINE (state-in/visuals-out, LIB-ABI): each callee's load-bearing state write happens
// in libmh at the call site; the visual remainder stays behind the host-callback table entry.
// Hosted, the original callee still executes and performs the SAME writes -- the hoist re-stores
// identical values (idempotent double-write); under a no-op host arm the hoist is the only
// writer and libmh keeps its inputs. GAME_MODE became a libmh determinism input at LIB-TRANS
// (sim_lt_frame.cpp gates the sim tick on it), which is what forced this split.
//
// ORDERING LAW (same as sim_lt_menu_teardown.h's): these callees READ GAME_MODE as their own
// entry guard, so the mode store must NEVER be hoisted above the call -- latch the predicate
// BEFORE, call, store AFTER. The one exception is mp_leave's saved-mode restore, which the
// callee re-executes first thing and which its own dismiss guard then reads.
//
// THE DISMISS RULE, host-arm-independent: the original evaluates
//   mode = (list==NULL || list==GAMEPLAY_HUD) ? 2 : 3       (post-teardown list, 0x004c7d7d..a5)
// after having nulled the list IF it was LOCKSTEP_SYNC (0x004c7d73). A no-op host arm performs
// no teardown, so the hoist folds list==LOCKSTEP_SYNC into the mode-2 case -- identical under
// the real arm (post-teardown the list is never LOCKSTEP_SYNC) and correct under the no-op arm.
//
#pragma once

#include <cstdint>

#include "state/host_events.h" // the pushed-answer slot the unbound arm reads (R9)

namespace mh::lockstep {

// R3b (LIFT-R3B, 2026-09-09) -- outcome_dialog's GATED panel pair, the fourth readback the
// notify-readback gate found real. llm_ui_outcome_dialog @0x004c6c5e opens with
// `if (_G_LLM_PLANET_SEL_SCREEN_OPEN != 0)` and that branch fires game_SetEvent(BUILD_OPEN_AUTOPAGE)
// TWICE, writing STRAT_UI_PANEL_MODE / _PAGE / _SWITCH_PENDING. Both readers of that pair --
// sim_game_set_event.cpp and sim_bldg_unmap_footprint.cpp:222/230/243 -- and both of the dialog's
// sim-side emitters are reachable from llm_strat_sim_tick, so unlike the other panel writers this
// one genuinely shares a frame: a poll host would leave the readers deciding on the pre-dialog
// panel state. Applied AFTER the call, like every other hoist here (the callee reads its own gate).
// Idempotent under the real host arm, which performs the same writes; the sole writer under a
// no-op arm. The event fires twice in the original and the second is a no-op over the first, so
// applying the effect once is exact.
void outcome_dialog_panel_hoist();

// Latch BEFORE a dismiss-bearing call: GAME_MODE != 4 (the dismiss guard @0x004c7d7d).
bool overlay_mode_not4();

// Latch BEFORE an overlay-show call: the widget slot is free (list==NULL || list==GAMEPLAY_HUD)
// -- the shows' arming guard reads the PRE-call list (0x004c7f02/0x004c7e75).
bool overlay_slot_free();

// Latch BEFORE wait_player_overlay_show: its arming guard is the TIGHTER GAME_MODE == 2
// (0x004c7e8b), unlike its siblings' != 4.
bool overlay_mode_is2();

// AFTER a call whose tail is (or is) overlay_dismiss: WAIT_PLAYER_IDX = -1 (unconditional first
// store @0x004c7d34) and, if `not4`, GAME_MODE = the folded dismiss rule above.
void overlay_dismiss_hoist(bool not4);

// AFTER wait_player_overlay_show(pidx): the unconditional prologue pair (OVERLAY_RESULT = -1
// @0x004c7ddb, WAIT_PLAYER_IDX = pidx @0x004c7de8) and, if `armed` (slot free && mode==2 --
// note the TIGHTER ==2 guard @0x004c7e8b, unlike its siblings' !=4), GAME_MODE = 3.
void wait_player_hoist(int32_t player_idx, bool armed);

// The kick modal's ANSWER (LIFT-SCREEN, R9): _G_LLM_NET_LOCKSTEP_OVERLAY_RESULT @0x0065679a --
// what the modal's "Disconnect player" button callback (0x004c7bd4) last wrote, or -1. This read
// is what llm_net_lockstep_sync_overlay_show used to perform and RETURN; the entry is now a void
// screen request and the site does the read itself.
int32_t overlay_result();

// The consume path's OVERLAY_RESULT = -1 @0x004c7fa4, run after an answer >= 0 is taken.
// (The original then tail-calls overlay_dismiss -- pair this with overlay_dismiss_hoist.)
void overlay_result_clear();

// _G_LLM_GAME_MODE_SAVED @0x006444cb -- read for mp_leave's restore hoist, written for
// extend_ui_enter's split (its first statement, @0x004c85c8). resync_complete_local reads this
// slot, so the write is load-bearing for libmh and belongs at the call site.
uint8_t game_mode_saved();
void    game_mode_saved_set(uint8_t mode);

// Plain GAME_MODE store for sites whose callee's write is unconditional (outcome_dialog's
// mode=3 @0x004c6cec) or for the mp_leave restore. Lockstep-module counterpart of the sim
// modules' own.game_mode() accessor.
void game_mode_set(uint8_t mode);

// ---- the calls-struct seam -----------------------------------------------------------------
//
// The functions above dereference live game VAs, which do not exist in the selftest process --
// so call sites never reach them directly. Each affected calls struct carries a
// `const overlay_hoist_ops *hoist` TAIL member: the production binding points it at
// live_overlay_hoist_ops(); a suite's aggregate initializer zero-fills it and the null-tolerant
// wrappers below skip the hoist, which matches how the suites already model these host callees
// (stubs with no state effect). The hoist's own behavior is covered by the hosted oracle
// (determinism + UI suite bit-identity), not the offline fixtures.
struct overlay_hoist_ops {
    bool (*mode_not4)();
    bool (*mode_is2)();
    bool (*slot_free)();
    void (*dismiss_hoist)(bool not4);
    void (*wait_player)(int32_t player_idx, bool armed);
    int32_t (*overlay_result)();
    void (*result_clear)();
    uint8_t (*mode_saved)();
    void (*mode_set)(uint8_t mode);
    void (*panel_hoist)(); // R3b: outcome_dialog's gated panel pair
};

const overlay_hoist_ops &live_overlay_hoist_ops();

// Null-tolerant call-site wrappers (see above). The latches return false with no ops bound so
// the paired store wrapper is a no-op too.
inline bool hoist_mode_not4(const overlay_hoist_ops *h) { return h != nullptr && h->mode_not4(); }
inline bool hoist_mode_is2(const overlay_hoist_ops *h) { return h != nullptr && h->mode_is2(); }
inline bool hoist_slot_free(const overlay_hoist_ops *h) { return h != nullptr && h->slot_free(); }
// R3b. Unbound -> no-op, and that is the CORRECT offline answer, not a shortcut: with no
// game process there is no panel state to keep consistent, and reaching the registry
// directly is what made the offline suites fault (ASan named the line on run #1).
inline void hoist_outcome_panel(const overlay_hoist_ops *h) {
    if (h != nullptr) h->panel_hoist();
}
inline void hoist_dismiss(const overlay_hoist_ops *h, bool not4) {
    if (h != nullptr) h->dismiss_hoist(not4);
}
inline void hoist_wait_player(const overlay_hoist_ops *h, int32_t pidx, bool armed) {
    if (h != nullptr) h->wait_player(pidx, armed);
}
// The one PAIR that is not a plain no-op when unbound. Hosted, the answer lives in the game's own
// global and these two reach it through the ops; standalone there is no game process, so they
// reach libmh's pushed-answer slot instead (libmh_submit_screen_answer). Same read-then-clear
// shape either way, which is what lets ONE call site serve both configs.
inline int32_t hoist_overlay_result(const overlay_hoist_ops *h) {
    return h != nullptr ? h->overlay_result()
                        : mh::state::screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK);
}
inline void hoist_result_clear(const overlay_hoist_ops *h) {
    if (h != nullptr)
        h->result_clear();
    else
        mh::state::screen_answer_clear(LIBMH_SCR_ANS_LOCKSTEP_KICK);
}
inline uint8_t hoist_mode_saved(const overlay_hoist_ops *h) {
    return h != nullptr ? h->mode_saved() : 0;
}
inline void hoist_mode_set(const overlay_hoist_ops *h, uint8_t mode) {
    if (h != nullptr) h->mode_set(mode);
}

} // namespace mh::lockstep
