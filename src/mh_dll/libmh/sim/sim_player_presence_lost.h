//
// sim/sim_player_presence_lost.h -- llm_strat_player_presence_lost @0x00498089, the player-elimination
// / win-loss resolver (RI-SIM / SIM1F batch F,; the batch done_when's centrepiece).
//
// Called after a player may have lost their last presence (natural loss-of-presence with mode==0, or a
// forced MP drop with mode!=0). It first RETURNS EARLY (a truthy incidental value the callers ignore)
// the moment `player` is found to still hold ANY building / unit / shuttle / unacquired planet in the
// current system -- i.e. NOT actually eliminated. Only when no presence remains does it run the
// elimination path:
//
//   SP campaign (session_mode == SESSION_SP):
//     * an ENEMY was cleared -> prints "<name> eliminated" (text 0x7b) / "<system> captured" (0x7c),
//       grants the planet/system invention to the human via game_UpdateProgress, and (debug-cheat only)
//       halves the resource yield.
//     * the LOCAL player lost their last shuttle -> defeat (outcome 4).
//     * outcome codes: 0 (self, transient) / 1 (enemy eliminated) / 3 (system captured) / 5 (victory).
//
//   MP (session_mode 2/3): clears the player's ALIVE bit, then over the surviving roster evaluates
//     last-man-standing (bVar4) / no-other-human (bVar3) / all-allied (bVar5), downgrades a lockstep
//     session (mode 3 -> 2, dismissing the overlay) when no human remains, and picks an outcome code
//     (4 defeat / 5 victory / 6 continue-natural / 8 continue-forced) that drives the end fanfare
//     (snd 0xa5 win / 0xa6 loss) and the outcome dialog. The local player's own drop sends
//     llm_net_lockstep_send_presence_lost.
//
// DO-NOT-ARM / offline-oracle only. This function makes effectful outward calls that a shadow site
// would DOUBLE-FIRE under snapshot/restore -- a network send (llm_net_lockstep_send_presence_lost), a
// blocking modal (llm_ui_outcome_dialog), on-screen text (game_ui_PrintTextMessage /
// llm_ui_print_floating_msg_red / llm_ui_print_queue_text_id) and the SP fanfare -- and its write
// closure reaches the whole UI/net cluster through those originals. Same DO-NOT-ARM class as
// sim_bldg_apply_damage. So there is NO entry-point seam here (cf. sim_resource_spend.cpp): the
// only verification is the offline oracle net_selftest simtest (sim_player_presence_lost_selftest.cpp),
// which covers both done_when cases -- a surviving-player run (early return, no dialog) and a
// no-survivor run (elimination, dialog fires with the right code).
//
// ---- Translated from the DISASSEMBLY (tmp/decomp/llm_strat_player_presence_lost_00498089.asm); the
// .c is a draft. The split-double at 0xe58356 (DEBUG_RESOURCE_YIELD_CUT, low/high halves 0/0x3fe00000
// = 0.5), the 5-boolean win/loss logic, the two session-downgrade points and the fanfare selector
// were each re-derived against the raw instructions.
//
#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_SP / SESSION_MP_LOCKSTEP / SESSION_MP_LOCAL
#include "sim/sim_state.h"

namespace mh::lockstep {
struct overlay_hoist_ops; // LIB-ABI stage E -- lockstep/overlay_hoist.h
}

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest.
struct player_presence_lost_calls {
    int32_t (*w_sprintf_vss)(void *dst, const wchar_t *fmt, const wchar_t *a0,
                             const wchar_t *a1);         // w_sprintf__vss @0x004d0320
    uint32_t (*ui_print_text_message)(void *text);       // game_ui_PrintTextMessage @0x00496508
    void (*snd_play)(int32_t sound_id, int32_t volume);  // llm_snd_play @0x00425233
    void (*update_progress)(uint16_t plr, uint16_t inv); // game_UpdateProgress @0x004402b0
    void (*print_queue_text_id)(int32_t text_id);        // llm_ui_print_queue_text_id @0x0049653d
    void *(*ansi_to_wide_scratch)(char *src);            // llm_str_ansi_to_wide_scratch @0x004cf3e0
    void *(*w_str_copy)(void *src, void *dst);           // utils_w_str_copy @0x004d02d2
    int32_t (*print_floating_msg_red)(void *message);    // llm_ui_print_floating_msg_red @0x004964bd
    void (*net_send_presence_lost)();                    // llm_net_lockstep_send_presence_lost @0x0049e328
    int32_t (*net_overlay_dismiss)();                    // llm_net_lockstep_overlay_dismiss @0x004c7d1c
    int32_t (*outcome_dialog)(uint8_t outcome);          // llm_ui_outcome_dialog @0x004c6c4f
    // LIB-ABI stage E hoist ops (lockstep/overlay_hoist.h) -- tail member, null-skipped by suites.
    const mh::lockstep::overlay_hoist_ops *hoist;
};

const player_presence_lost_calls &live_player_presence_lost_calls();

namespace detail {

// llm_strat_player_presence_lost @0x00498089. Reads the roster/planet/session inputs through `v`,
// mutates status_flags / session_mode / debug_resource_yield_cut / the text-scratch buffer through
// `own`, and reaches the effectful UI/net originals through `c`. Returns the original's incidental
// uint result (ignored by every caller; see the header).
uint32_t player_presence_lost(const sim_view &v, sim_store &own, const player_presence_lost_calls &c,
                              uint32_t player, uint32_t mode);

} // namespace detail

// Live wrapper: the logic applied to state() and live_player_presence_lost_calls().
uint32_t player_presence_lost(uint32_t player, uint32_t mode);

} // namespace mh::sim
