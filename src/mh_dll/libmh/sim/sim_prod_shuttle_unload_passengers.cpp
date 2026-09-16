//
// sim/sim_prod_shuttle_unload_passengers.cpp -- see sim_prod_shuttle_unload_passengers.h.
// Translated from the DISASSEMBLY
// (tmp/decomp/llm_prod_shuttle_unload_passengers_0048e6f2.asm), not from the Ghidra .c draft.
//
#include "sim/sim_prod_shuttle_unload_passengers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_unload_passengers_calls &live_prod_shuttle_unload_passengers_calls() {
    static const prod_shuttle_unload_passengers_calls c = {
        MH_LIBMH_BIND(llm_strat_population_add),
        MH_LIBMH_BIND(llm_strat_bldg_shuttle_slot_is_free),
        MH_LIBMH_BIND(llm_strat_bldg_flush_cargo_hold),
    };
    return c;
}

namespace detail {

int32_t prod_shuttle_unload_passengers(const sim_view &v, sim_store &own,
                                       const prod_shuttle_unload_passengers_calls &c, uint16_t player,
                                       int32_t building_index, int32_t cap) {
    // 0x0048e711-0x0048e72b: the building's shuttle_slot -- ONE field, unlike the load-passengers
    // sibling which also needs building_id.
    const building &b    = building_of(v, static_cast<uint32_t>(player), building_index);
    const uint8_t   slot = b.shuttle_slot;

    // 0x0048e72e-0x0048e747: ONE fetch, reused for the initial read below and the decrement later
    // (same shape as the load-passengers sibling's single prod_shuttle_slot_at() call).
    prod_shuttle_slot &s = own.prod_shuttle_slot_at(static_cast<uint32_t>(player), static_cast<int32_t>(slot));

    // 0x0048e747-0x0048e75d: amount = passengers_reserved, UNLESS cap != -1 and cap is smaller --
    // cap == -1 (checked first, unconditionally) skips the clamp entirely and releases everything
    // reserved. `cap` is NOT truncated anywhere here (unlike the load-passengers sibling): both
    // sides of the CMP are full 32-bit reads.
    int32_t amount = s.passengers_reserved;
    if (cap != -1 && cap < amount) amount = cap;

    // 0x0048e760-0x0048e764 (-> LAB_0048e7b3): nothing reserved to release -- return 0 without any
    // outward call and without touching passengers_reserved.
    if (amount == 0) return 0;

    // 0x0048e766-0x0048e78e: llm_strat_population_add(player, amount) (ORIGINAL, via `c`), then
    // decrement the slot's reserved counter by the same amount.
    c.population_add(player, amount);
    s.passengers_reserved -= amount;

    // 0x0048e78e-0x0048e7a9: llm_strat_bldg_shuttle_slot_is_free(player, building_index) (ORIGINAL,
    // via `c`) -- SCC sibling, called as the original. If the slot is now free (>0), flush the
    // cargo hold (also an ORIGINAL, ring-sibling call via `c`).
    if (c.bldg_shuttle_slot_is_free(static_cast<int32_t>(player), building_index) > 0) {
        c.bldg_flush_cargo_hold(static_cast<uint32_t>(player), building_index);
    }

    // 0x0048e7aa-0x0048e7b1: success.
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_shuttle_unload_passengers(uint16_t player, int32_t building_index, int32_t cap) {
    sim_state st = state();
    return detail::prod_shuttle_unload_passengers(st.read, st.own, live_prod_shuttle_unload_passengers_calls(),
                                                  player, building_index, cap);
}


} // namespace mh::sim
