//
// sim/sim_prod_shuttle_fuel_apply.cpp -- see sim_prod_shuttle_fuel_apply.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_prod_shuttle_fuel_apply_00493705.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_prod_shuttle_fuel_apply.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_fuel_apply_calls &live_prod_shuttle_fuel_apply_calls() {
    static const prod_shuttle_fuel_apply_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
    };
    return c;
}

namespace detail {

int32_t prod_shuttle_fuel_apply(const sim_view &v, const prod_shuttle_fuel_apply_calls &c,
                                uint16_t player, int32_t building_index, int32_t dest_planet) {
    // 0x00493724-0x00493735: same-planet bail -- no fuel is spent for a shuttle staying on the
    // current planet.
    if (*v.planet_index == dest_planet) return 0;

    // 0x0049373a-0x00493754: building_id = buildings[player][building_index].building_id.
    const uint16_t      building_id = building_of(v, static_cast<uint32_t>(player), building_index).building_id;
    const cfg_building &cb          = v.cfg_buildings[building_id];

    // 0x0049375e-0x004937ab: walk cb.fuel[i], id-read-before-bound-check -- `fuel`'s offset/size were
    // corrected 2026-08-14 (see the header banner's RESOLVED notes) to match this function's (and its
    // sibling llm_prod_shuttle_fuel_check's) own address arithmetic. Same evaluation order as
    // sim_bldg_grant_type_resources.cpp's identical `resource[]` walk: read the id first, break on
    // ==0 (UNDEFINED sentinel), THEN test the bound and break if not satisfied.
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = static_cast<int32_t>(cb.fuel[i].id); // 0x0049376d
        if (resource_id == 0) break;                                     // 0x00493776/0x0049377a
        if (!(i < CFG_RESOURCE_SLOTS)) break;                            // 0x0049377c/0x00493780

        const int32_t amount = cb.fuel[i].val;                             // 0x00493793
        c.resource_add(static_cast<int32_t>(player), resource_id, amount); // 0x004937a0
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_shuttle_fuel_apply(uint16_t player, int32_t building_index, int32_t dest_planet) {
    const sim_view v = state().read;
    return detail::prod_shuttle_fuel_apply(v, live_prod_shuttle_fuel_apply_calls(), player, building_index,
                                           dest_planet);
}


} // namespace mh::sim
