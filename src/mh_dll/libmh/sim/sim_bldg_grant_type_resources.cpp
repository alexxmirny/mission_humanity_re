#include "sim/sim_bldg_grant_type_resources.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const grant_type_resources_calls &live_grant_type_resources_calls() {
    static const grant_type_resources_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
    };
    return c;
}

namespace detail {

void bldg_grant_type_resources(const sim_view &v, const grant_type_resources_calls &c,
                               uint32_t player_idx, int32_t building_type_idx) {
    // 0x00493821: `MOVZX EAX, word ptr [player_idx]` -- truncated to the low 16 bits at the (only)
    // use site. The original re-derives this inside the loop every iteration; hoisted here since the
    // value never changes across iterations (same reasoning sim_bldg_pay_costs.cpp's/
    // sim_unit_refund.cpp's own single truncation gives).
    const int32_t player = (int32_t)(player_idx & 0xffffu);

    const cfg_building &cb = v.cfg_buildings[building_type_idx];

    // 0x004937e3-0x00493830: walk cb.resource[i], i = 0, 1, 2, ... . THE CONJUNCTION IS EVALUATED
    // id-FIRST: 0x004937f2 reads resource[i].id and 0x004937fb/0x004937ff tests it against 0 (the
    // UNDEFINED sentinel) BEFORE 0x00493801/0x00493805 tests i<CFG_RESOURCE_SLOTS(7) -- i.e. the READ
    // at index i always happens on entry to the loop body, even for i==7. Same evaluation order as
    // sim_bldg_pay_costs.cpp's/sim_unit_refund.cpp's walks; reproduced literally as an unbounded
    // `for(;;)` with two ordered breaks. See the header banner: the i==7 OOB read is provably inert
    // by the same general argument sim_bldg_pay_costs.h gives for its own identical loop shape.
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = (int32_t)cb.resource[i].id; // 0x004937f2 -- can read resource[7]
        if (resource_id == 0)                                   // 0x004937fb/0x004937ff: UNDEFINED
            break;
        if (!(i < CFG_RESOURCE_SLOTS)) // 0x00493801/0x00493805: bound check, evaluated SECOND
            break;

        const int32_t val = cb.resource[i].val;   // 0x00493818
        c.resource_add(player, resource_id, val); // 0x00493825
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_grant_type_resources(uint32_t player_idx, int32_t building_type_idx) {
    const sim_view v = state().read;
    detail::bldg_grant_type_resources(v, live_grant_type_resources_calls(), player_idx,
                                      building_type_idx);
}


} // namespace mh::sim
