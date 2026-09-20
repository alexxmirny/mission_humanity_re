//
// sim/sim_bldg_pay_costs.cpp -- see sim_bldg_pay_costs.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_bldg_pay_build_cost_00492eb1.asm, tmp/decomp/llm_strat_bldg_pay_cycle_inputs_00492ff9.asm),
// not from the Ghidra .c drafts -- both drafts read correctly for the value comparisons, the
// error-accumulation shape, and the loop-bound-vs-id-read order (independently re-walked below
// against the raw CMP/JZ/JL targets and addr/mh_structs.gen.h's static_assert'd offsets), so they are
// cited only as corroboration, not as the source of truth.
//
#include "sim/sim_bldg_pay_costs.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const pay_costs_calls &live_pay_costs_calls() {
    static const pay_costs_calls gc = {
        MH_LIBMH_BIND(game_SpendResource),
    };
    return gc;
}

namespace detail {

int32_t bldg_can_afford_build_cost(const sim_view &v, uint32_t player, int32_t building_type_id) {
    // Truncated to the low 16 bits at EVERY use site in the original (0x00492edf/0x00492fd5); one
    // local here, matching the .c draft's own `player & 0xffff` at every site.
    const int32_t p = (int32_t)(player & 0xffffu);

    const cfg_building &cb = v.cfg_buildings[building_type_id];

    // 0x00492ecb-0x00492efb: gate on the prerequisite invention BEFORE touching any resource. No
    // resource walk at all if it is not researched.
    if (progress_of(v, p, cb.invention).available == 0) {
        return 0x13;
    }

    // ---- pass 1: scan all up to 7 resource slots, accumulating an error code WITHOUT short-
    // circuiting on the first shortage (0x00492f0e-0x00492f82). See the header banner for the
    // accumulation rule and the proven-inert id-read-before-bound-check order.
    int32_t error = 0;
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = cb.resource[i].id; // 0x00492f1d -- can read resource[7] (into
                                                        // build_time_2's low bytes); see header banner.
        if (resource_id == 0) break;                    // 0x00492f2a: UNDEFINED
        if (!(i < CFG_RESOURCE_SLOTS)) break;           // 0x00492f30: bound check, evaluated SECOND

        // 0x00492f52-0x00492f5e: holdings < cost?
        if (player_resource_of(v, p, (int32_t)resource_id) < cb.resource[i].val) {
            // 0x00492f60-0x00492f77: first shortage sets id+0x89; any later one collapses to the
            // bare sentinel 0x89, discarding which resource(s) were short.
            error = (error == 0) ? (int32_t)(resource_id + 0x89u) : 0x89;
        }
    }
    return error; // 0x00492f86-0x00492f8e: nonzero is the reason code, 0 means every slot is covered
}

int32_t bldg_pay_build_cost(const sim_view &v, const pay_costs_calls &gc, uint32_t player,
                            int32_t building_type_id) {
    // ---- pass 1 (0x00492ecb-0x00492f8e): the gate + the shortage scan, split out as
    // bldg_can_afford_build_cost for mp:D25 -- identical code, identical return.
    const int32_t error = bldg_can_afford_build_cost(v, player, building_type_id);
    if (error != 0) return error;

    const int32_t       p  = (int32_t)(player & 0xffffu);
    const cfg_building &cb = v.cfg_buildings[building_type_id];

    // ---- pass 2: everything affordable -- actually charge (0x00492f90-0x00492fe4).
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = cb.resource[i].id; // 0x00492fa6
        if (resource_id == 0) break;
        if (!(i < CFG_RESOURCE_SLOTS)) break;

        gc.spend_resource(p, (int32_t)resource_id, cb.resource[i].val); // 0x00492fd9
    }
    return 0; // 0x00492fe6
}

int32_t bldg_pay_cycle_inputs(const sim_view &v, const pay_costs_calls &gc, uint32_t player,
                              uint32_t b_index) {
    // Truncated to the low 16 bits at EVERY use site (0x00493016/0x00493108), same as
    // bldg_pay_build_cost above.
    const int32_t p = (int32_t)(player & 0xffffu);

    // 0x00493016-0x00493030: buildings[player][b_index].building_id -- the roster INSTANCE's cfg
    // type. NOT a direct type-id parameter, unlike bldg_pay_build_cost's building_type_id.
    const uint16_t      building_id = building_of(v, (uint32_t)p, (int32_t)b_index).building_id;
    const cfg_building &cb          = v.cfg_buildings[building_id];

    // No invention gate here -- unlike bldg_pay_build_cost, this function goes straight from the
    // building lookup into the resource walk (see header banner point (1)).

    // ---- pass 1: same accumulation shape as bldg_pay_build_cost, but over resource_2[] (0x00493041-
    // 0x004930b5) -- a DIFFERENT array on the same cfg_building record, see header banner point (3).
    int32_t error = 0;
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = cb.resource_2[i].id; // 0x00493050 -- can read resource_2[7]
                                                          // (into build_time_d's low bytes).
        if (resource_id == 0) break;
        if (!(i < CFG_RESOURCE_SLOTS)) break;

        if (player_resource_of(v, p, (int32_t)resource_id) < cb.resource_2[i].val) {
            error = (error == 0) ? (int32_t)(resource_id + 0x89u) : 0x89;
        }
    }
    if (error != 0) return error; // 0x004930b9-0x004930c1

    // ---- pass 2: charge (0x004930c3-0x00493117).
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = cb.resource_2[i].id; // 0x004930d9
        if (resource_id == 0) break;
        if (!(i < CFG_RESOURCE_SLOTS)) break;

        gc.spend_resource(p, (int32_t)resource_id, cb.resource_2[i].val); // 0x0049310c
    }
    return 0; // 0x00493119
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t bldg_can_afford_build_cost(uint32_t player, int32_t building_type_id) {
    const sim_view v = state().read;
    return detail::bldg_can_afford_build_cost(v, player, building_type_id);
}

int32_t bldg_pay_build_cost(uint32_t player, int32_t building_type_id) {
    const sim_view v = state().read;
    return detail::bldg_pay_build_cost(v, live_pay_costs_calls(), player, building_type_id);
}

int32_t bldg_pay_cycle_inputs(uint32_t player, uint32_t b_index) {
    const sim_view v = state().read;
    return detail::bldg_pay_cycle_inputs(v, live_pay_costs_calls(), player, b_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
