//
// sim/sim_prod_shuttle_load_passengers.cpp -- see sim_prod_shuttle_load_passengers.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_prod_shuttle_load_passengers_0048e5e5.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_prod_shuttle_load_passengers.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_load_passengers_calls &live_prod_shuttle_load_passengers_calls() {
    static const prod_shuttle_load_passengers_calls c = {
        MH_LIBMH_BIND(llm_strat_population_remove),
    };
    return c;
}

namespace detail {

uint32_t prod_shuttle_load_passengers(const sim_view &v, sim_store &own,
                                      const prod_shuttle_load_passengers_calls &c, uint16_t player,
                                      int32_t building_index, uint32_t cap) {
    // 0x0048e5fb-0x0048e647: both fields read off the SAME building_of() row, unconditionally --
    // see header banner for the address cross-check.
    const building &b           = building_of(v, static_cast<uint32_t>(player), building_index);
    const uint8_t   slot        = b.shuttle_slot;
    const uint16_t  building_id = b.building_id;

    // 0x0048e634-0x0048e647: the building's cfg TYPE's transport capacity.
    const int32_t transport_cap = v.cfg_buildings[building_id].human_transport;

    // 0x0048e64a-0x0048e657: no transport capacity at all -- nothing to embark, no slot record ever
    // touched.
    if (transport_cap == 0) return 0;

    // 0x0048e65c: LAB_0048e65c. ONE fetch, reused for both the read (avail net of reserved) and the
    // write (reserved bump) below.
    prod_shuttle_slot &s = own.prod_shuttle_slot_at(static_cast<uint32_t>(player),
                                                    static_cast<int32_t>(slot));

    // 0x0048e65c-0x0048e675: avail = transport_cap - passengers already reserved on this slot.
    int32_t avail = transport_cap - s.passengers_reserved;

    // 0x0048e678-0x0048e69a: clamp DOWN to the player's idle population if avail exceeds it.
    const int32_t human = v.population[player].human;
    if (avail > human) avail = human;

    // 0x0048e69a-0x0048e6aa: clamp DOWN to the requested cap, truncated to 16 bits at this one use
    // site -- see header banner on why `cap` is masked here rather than at the parameter.
    const uint16_t cap16 = static_cast<uint16_t>(cap);
    if (static_cast<int32_t>(cap16) < avail) avail = static_cast<int32_t>(cap16);

    // 0x0048e6aa-0x0048e6e0/e6e7: nothing to embark -- return 0 without calling population_remove or
    // touching the slot's reserved counter.
    if (avail == 0) return 0;

    // 0x0048e6b3-0x0048e6d8: embark -- pull `avail` colonists out of the idle population, then bump
    // the slot's reserved counter by the same amount, then return it.
    c.population_remove(static_cast<uint32_t>(player), avail);
    s.passengers_reserved += avail;
    return static_cast<uint32_t>(avail);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t prod_shuttle_load_passengers(uint16_t player, int32_t building_index, uint32_t cap) {
    sim_state st = state();
    return detail::prod_shuttle_load_passengers(st.read, st.own, live_prod_shuttle_load_passengers_calls(),
                                                player, building_index, cap);
}


} // namespace mh::sim
