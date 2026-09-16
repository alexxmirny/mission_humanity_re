//
// sim/sim_combat_credit_planet_conquest_kills.cpp -- see sim_combat_credit_planet_conquest_kills.h.
// Translated from the DISASSEMBLY
// (tmp/decomp/llm_combat_credit_planet_conquest_kills_0049928a.asm), not from the Ghidra .c draft
// (whose local variable names' hex suffixes disagree with the raw stack offsets -- see the header).
//
#include "sim/sim_combat_credit_planet_conquest_kills.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const combat_credit_planet_conquest_kills_calls &live_combat_credit_planet_conquest_kills_calls() {
    static const combat_credit_planet_conquest_kills_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_kill_credit),
        MH_LIBMH_BIND(llm_strat_bldg_kill_credit),
    };
    return c;
}

namespace detail {

void combat_credit_planet_conquest_kills(const sim_view &v, uint32_t player,
                                         const combat_credit_planet_conquest_kills_calls &c) {
    // ---- (1) witness scan (0x004992a5-0x004992e6): first index 1..99 of `player`'s own units with
    // energy>0.0. Sentinel 0 (the local's init value) if none ever qualifies -- the loop just runs to
    // completion without ever assigning the witness.
    int32_t witness_unit_index = 0;
    for (int32_t unit_index = 1; unit_index < v.caps.units; ++unit_index) {
        if (!(unit_of(v, player, unit_index).energy <= 0.0)) {
            witness_unit_index = unit_index;
            break;
        }
    }

    // ---- (2) outer guard (0x004992ea/0x00499416): no witness, no work.
    if (witness_unit_index == 0) return;

    // ---- (3) single-enemy scan (0x004992f4-0x0049935e): first index 0..7 (MAX_PLAYERS), skipping
    // `player`, that is ALIVE and has buildings_alive>0 OR units_alive>0 on the current planet. Every
    // skip (self, not alive, neither counter positive) falls through to the next `enemy` -- matching
    // the asm's own increment-and-continue path exactly.
    for (uint32_t enemy = 0; enemy < static_cast<uint32_t>(MAX_PLAYERS); ++enemy) {
        if (enemy == player) continue;

        const player_profile &prof = v.profiles[enemy];
        if ((prof.status_flags & STRAT_PLAYER_STATUS_ALIVE) == 0) continue;

        // Read once and reused for both counter checks below -- the asm re-fetches G_PLANET_INDEX
        // twice back-to-back with nothing writing it in between, so this is behaviourally identical.
        const int32_t planet = *v.planet_index;
        if (!(prof.buildings_alive[planet] > 0 || prof.units_alive[planet] > 0)) continue;

        // ---- (4) THIS enemy only: credit `player` for every energy>0.0 unit, then building, it still
        // has, passing the step-(1) witness as the 5th arg to both -- then return unconditionally
        // (0x0049940f->0x00499416). The original never loops back to check a second matching enemy.
        //
        // `killer_info`/`victim_player` are recomputed identically at both original call sites in the
        // asm (once per credited unit/building); hoisted here since both depend only on `player`/
        // `enemy`, neither of which changes across either loop -- same reasoning as
        // sim_bldg_state_destroyed.cpp's `elapsed` precedent.
        const uint32_t killer_info   = (player & 0xffffu) | 0x80u;
        const uint32_t victim_player = static_cast<uint32_t>(static_cast<uint16_t>(enemy));

        for (int32_t unit_index = 1; unit_index < v.caps.units; ++unit_index) {
            if (!(unit_of(v, enemy, unit_index).energy <= 0.0)) {
                c.unit_kill_credit(victim_player, unit_index, CONQUEST_UNIT_KILL_DAMAGE, killer_info,
                                   witness_unit_index);
            }
        }
        for (int32_t building_index = 1; building_index < v.caps.buildings; ++building_index) {
            if (!(building_of(v, enemy, building_index).energy <= 0.0)) {
                c.bldg_kill_credit(victim_player, building_index, CONQUEST_BUILDING_KILL_DAMAGE,
                                   killer_info, witness_unit_index);
            }
        }
        return;
    }
    // Loop exhausted without a match (0x00499301 / the 0x0049935e skip path looping back to
    // 0x004992fb until it hits 8): return with nothing credited.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void combat_credit_planet_conquest_kills(uint32_t player) {
    const sim_view v = state().read;
    detail::combat_credit_planet_conquest_kills(v, player,
                                                live_combat_credit_planet_conquest_kills_calls());
}


} // namespace mh::sim
