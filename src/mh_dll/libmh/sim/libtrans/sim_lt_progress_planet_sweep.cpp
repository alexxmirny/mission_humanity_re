//
// sim/libtrans/sim_lt_progress_planet_sweep.cpp -- see sim_lt_progress_planet_sweep.h. Translated
// from the DISASSEMBLY (tmp/decomp_lib_trans/llm_progress_recheck_planet_system_all_players_00454846.asm,
// tmp/decomp_lib_trans/game_UpdatePlanetProgress_004548ce.asm); the Ghidra .c drafts agree with the
// assembly on every branch for both functions (no discrepancy found).
//
#include "sim/libtrans/sim_lt_progress_planet_sweep.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// llm_strat_player_profile::status_flags bit 0 (E_STRAT_PLAYER_STATUS bit0, no committed Ghidra enum
// -- mh_structs.gen.h's own field comment: "bit0 slot enabled"). LOCAL copy per
// sim_game_update_progress.h's precedent (its own file-local copy, not a shared include) -- no
// collision risk, unlike INVENTION_TYPE_PLANET/_SYSTEM below: neither sibling header declares this
// constant (sim_game_update_progress.h's own copy lives in ITS .cpp's anonymous namespace, invisible
// outside that TU).
constexpr uint32_t STRAT_PLAYER_STATUS_SLOT_ENABLED = 0x1u;

// NOTE: INVENTION_TYPE_PLANET / INVENTION_TYPE_SYSTEM are NOT redefined here -- see the header
// banner's "DEVIATION FROM CONTEXT" section. This file includes sim_game_handle_progress.h (for
// handle_progress_calls / detail::game_handle_progress), and that header already declares both
// constants at `namespace mh::sim` scope (lines 153-158 there). A second definition in this
// translation unit -- namespace-scope or anonymous -- would be a hard compile error (duplicate
// definition, or an ambiguous unqualified lookup via the anonymous-namespace using-directive), not a
// harmless shadow. `mh::sim::INVENTION_TYPE_PLANET` / `mh::sim::INVENTION_TYPE_SYSTEM` (values 4, 5)
// are used directly below.

} // namespace

namespace detail {

// ---- llm_progress_recheck_planet_system_all_players @0x00454846 -----------------------------------
void recheck_planet_system_all_players(const sim_view &v, sim_store &own,
                                       const handle_progress_calls &c_hp) {
    // 0x0045485e-0x0045486e: outer loop, i = 1..299 inclusive (seeded at 1, NOT 0).
    for (int32_t i = 1; i < 300; ++i) {
        // 0x00454879-0x0045488d: Progress[i].type == PLANET(4) or == SYSTEM(5).
        if (v.cfg_inventions[i].type == INVENTION_TYPE_PLANET ||
            v.cfg_inventions[i].type == INVENTION_TYPE_SYSTEM) {
            // 0x0045488f-0x0045489c: inner loop, p = 0..7 inclusive.
            for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
                // 0x004548a3-0x004548b1: profiles[p].status_flags & SLOT_ENABLED.
                if ((v.profiles[p].status_flags & STRAT_PLAYER_STATUS_SLOT_ENABLED) != 0) {
                    // 0x004548b3-0x004548bb: game_HandleProgress(player=p, inv=i), both narrowed to
                    // 16 bits at the call boundary (the loop variables themselves stay int32_t).
                    mh::sim::detail::game_handle_progress(v, own, c_hp, static_cast<uint16_t>(p),
                                                          static_cast<uint16_t>(i));
                }
            }
        }
    }
}

// ---- game_UpdatePlanetProgress @0x004548ce ---------------------------------------------------------
void game_update_planet_progress(const sim_view &v, sim_store &own, const handle_progress_calls &c_hp,
                                 const game_update_progress_calls &c_up) {
    // Identical sweep to recheck_planet_system_all_players above (0x004548e6-0x00454927 mirror
    // 0x0045485e-0x0045489c exactly) -- written out separately rather than sharing a helper, per the
    // translator brief's "no new helpers" rule and this unit's own GROUPING hazard note.
    for (int32_t i = 1; i < 300; ++i) {
        if (v.cfg_inventions[i].type == INVENTION_TYPE_PLANET ||
            v.cfg_inventions[i].type == INVENTION_TYPE_SYSTEM) {
            for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
                if ((v.profiles[p].status_flags & STRAT_PLAYER_STATUS_SLOT_ENABLED) != 0) {
                    const uint16_t player = static_cast<uint16_t>(p);
                    const uint16_t inv    = static_cast<uint16_t>(i);
                    // 0x00454946: game_HandleProgress(player, inv) ...
                    mh::sim::detail::game_handle_progress(v, own, c_hp, player, inv);
                    // ... 0x00454953: THEN game_UpdateProgress(player, inv) -- the one difference from
                    // recheck_planet_system_all_players's body above.
                    mh::sim::detail::game_update_progress(v, own, c_up, player, inv);
                }
            }
        }
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void recheck_planet_system_all_players() {
    sim_state st = state();
    detail::recheck_planet_system_all_players(st.read, st.own, live_handle_progress_calls());
}

void game_update_planet_progress() {
    sim_state st = state();
    detail::game_update_planet_progress(st.read, st.own, live_handle_progress_calls(),
                                        live_game_update_progress_calls());
}


} // namespace mh::sim
