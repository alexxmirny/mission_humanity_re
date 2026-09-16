//
// sim/sim_bldg_mothership_alive.cpp -- see sim_bldg_mothership_alive.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_game_check_players_mothership_alive_00498dc0.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_bldg_mothership_alive.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t game_check_players_mothership_alive(const sim_view &v) {
    // 0x00498dd8-0x00498de3: outer counter starts at 1, loop continues while < 3 -- players 1 and 2
    // ONLY. See the header banner: a genuine original limitation, not generalized to MAX_PLAYERS.
    for (int32_t player = 1; player < 3; ++player) {
        // 0x00498df2-0x00498dfd: inner counter starts at 1, loop continues while < 100
        // (BUILDINGS_PER_PLAYER) -- slot 0 (the roster's occupancy-count slot) is never tested.
        for (int32_t slot = 1; slot < v.caps.buildings; ++slot) {
            // 0x00498e0c-0x00498e53: buildings[player][slot], re-read every iteration (translator
            // brief rule 16: `const` on sim_view is not a promise of stability).
            const building &b = building_of(v, (uint32_t)player, slot);

            // Three short-circuited conditions, in this exact order (see header for the x87 NaN
            // derivation of the energy test and why it is NOT `0.0 < energy`):
            if (!(b.energy <= 0.0) && b.building_id != 0) {
                // 0x00498e45-0x00498e8f: cfg_buildings[building_id].type, same indexing idiom as
                // mh::ai::ai_attacker_intel.cpp's v.cfg_buildings[victim_index].type.
                const uint8_t type = v.cfg_buildings[b.building_id].type;
                if (type == BUILDING_TYPE_H_MOTHER || type == BUILDING_TYPE_A_MOTHER) {
                    // 0x00498e91-0x00498e98: hit -- return 0 IMMEDIATELY (inverted polarity, see
                    // header: 0 means "an alive mothership was found").
                    return 0;
                }
            }
        }
    }

    // 0x00498ea4: both loops exhausted without a hit -- return 1 (inverted polarity: "none found").
    return 1;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t game_check_players_mothership_alive() {
    const sim_view v = state().read;
    return detail::game_check_players_mothership_alive(v);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
