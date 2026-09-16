//
// sim/sim_game_speed_recompute.cpp -- see sim_game_speed_recompute.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_game_speed_recompute_00497623.asm); the Ghidra .c draft's control flow
// and plate both agree with it exactly.
//
#include "sim/sim_game_speed_recompute.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// llm_strat_player_profile::status_flags bits (E_STRAT_PLAYER_STATUS bit1/bit2 -- no committed
// Ghidra enum exists for this field, see the header's rule-17a note). Named to match this file's
// nearest sibling testing the same struct/field, sim_step.cpp's STRAT_PLAYER_STATUS_ALIVE.
constexpr uint32_t STRAT_PLAYER_STATUS_ALIVE = 0x2u; // has presence (SIM-owned half of the field)
constexpr uint32_t STRAT_PLAYER_STATUS_HUMAN = 0x4u; // human-controlled (NET-owned half of the field)

} // namespace

namespace detail {

void game_speed_recompute(const sim_view &v, sim_store &own) {
    // 0x0049763b-0x00497645: unconditional `game_speed = 1.0` before the loop.
    double &game_speed = own.game_speed();
    game_speed         = 1.0;

    // 0x00497656-0x004976a0: for every player slot, if BOTH status bits are set, fold that player's
    // own speed factor into the running product. Re-reads `v.profiles[i].status_flags` fresh each
    // iteration (task-brief hazard note: never cache/hoist a read out of a loop) even though nothing
    // in this function itself writes it.
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        const uint32_t flags = v.profiles[i].status_flags;
        if ((flags & STRAT_PLAYER_STATUS_ALIVE) != 0 && (flags & STRAT_PLAYER_STATUS_HUMAN) != 0) {
            game_speed = v.game_speed_player_factor[i] * game_speed;
        }
    }

    // 0x004976a2-0x004976c3: FLDZ; FCOMP game_speed; JC skips the reset when 0.0 < game_speed. The
    // reset fires exactly when that compare is ORDERED and false, i.e. `game_speed <= 0.0` under
    // ordinary IEEE comparison (NaN makes `<=` false too, matching the x87 unordered-sets-CF case
    // "skip reset") -- see the header banner for the full equivalence argument.
    if (game_speed <= 0.0) {
        game_speed = 1.0;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void game_speed_recompute() {
    sim_state st = state();
    detail::game_speed_recompute(st.read, st.own);
}


} // namespace mh::sim
