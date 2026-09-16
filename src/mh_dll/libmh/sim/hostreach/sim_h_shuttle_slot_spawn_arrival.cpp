//
// sim/hostreach/sim_h_shuttle_slot_spawn_arrival.cpp -- see sim_h_shuttle_slot_spawn_arrival.h.
// Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_prod_shuttle_slot_spawn_arrival_004906c1.asm), not the Ghidra .c draft.
//
#include "sim/hostreach/sim_h_shuttle_slot_spawn_arrival.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const shuttle_slot_spawn_arrival_calls &live_shuttle_slot_spawn_arrival_calls() {
    static const shuttle_slot_spawn_arrival_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_find_mothership_position),
        MH_LIBMH_BIND(llm_strat_prod_spawn_arrived_unit),
    };
    return c;
}

namespace detail {

// ---- llm_strat_prod_shuttle_slot_spawn_arrival @0x004906c1 ------------------------------------------
void shuttle_slot_spawn_arrival(const sim_view &v, const shuttle_slot_spawn_arrival_calls &c,
                                uint32_t slot_index) {
    // 0x004906dc-0x004906e3: PlayerSide, zero-extended (MOVZX word).
    const uint32_t player = static_cast<uint32_t>(static_cast<uint16_t>(*v.player_side));

    // 0x004906e6-0x004906f4: slot_index used UNTRUNCATED (full 32-bit) for the record-index arithmetic
    // -- see header banner. Same stride/indexing sim_prod_deliver_arrivals.cpp / own.prod_shuttle_slot_at()
    // already use for this array.
    const uint32_t slot_rec_index = player * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot_index;

    // x/y: [EBP-0x1c] / [EBP-0x18]. DEFINED to 0 rather than left uninitialised -- see the header
    // banner's derivation of why the one path that never writes them (origin_planet mismatch) also
    // never has them read by the callee.
    uint32_t x = 0;
    uint32_t y = 0;

    // 0x004906f6-0x00490703: origin_planet compared zero-extended against G_PLANET_INDEX.
    if (static_cast<int32_t>(static_cast<uint16_t>(v.prod_shuttle_slots[slot_rec_index].origin_planet)) ==
        *v.planet_index) {
        // 0x00490705-0x00490715: try the mothership position first. player passed as a FULL dword (not
        // truncated), matching the committed (int32_t, uint32_t*, uint32_t*) prototype.
        const uint32_t found = c.find_mothership_position(static_cast<int32_t>(player), &x, &y);
        if (found == 0) {
            // 0x00490717-0x00490749: no mothership -- fall back to the player's own recorded landing
            // site for the current planet. (found != 0: x/y were already written by the callee itself,
            // on its own return-1 path -- see header banner's citation of that function's body.)
            x = static_cast<uint32_t>(v.profiles[player].landing_x[*v.planet_index]);
            y = static_cast<uint32_t>(v.profiles[player].landing_y[*v.planet_index]);
        }
    }
    // else (0x00490703 JNZ taken): x/y stay 0 -- the original leaves them as stack residue here, which
    // is provably never read by the callee (see header banner). Reproducing that residue in C++ would be
    // undefined behaviour with no corresponding behavioural payoff, so a defined 0 is substituted.

    // 0x0049074c-0x0049075c: storage_idx is an unconditional literal 0 (PUSH 0x0). Return value
    // discarded, matching the original's void prototype.
    c.spawn_arrived_unit(static_cast<uint16_t>(player), slot_index, x, y, 0);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void shuttle_slot_spawn_arrival(uint32_t slot_index) {
    const sim_view v = state().read;
    detail::shuttle_slot_spawn_arrival(v, live_shuttle_slot_spawn_arrival_calls(), slot_index);
}

} // namespace mh::sim
