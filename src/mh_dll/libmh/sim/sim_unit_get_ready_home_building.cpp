//
// sim/sim_unit_get_ready_home_building.cpp -- see sim_unit_get_ready_home_building.h. Translated
// from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_get_ready_home_building_00484a14.asm).
//
#include "sim/sim_unit_get_ready_home_building.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // BUILT_FLAGS_OPERATIONAL, already pinned there (SIM1C)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_get_ready_home_building_calls &live_unit_get_ready_home_building_calls() {
    static const unit_get_ready_home_building_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_type_accepts_unit),
    };
    return c;
}

namespace detail {

int32_t unit_get_ready_home_building(const sim_view &v, const unit_get_ready_home_building_calls &c) {
    const unit &u = *v.cur_unit; // _G_LLM_STRAT_CUR_UNIT dereferenced, read-only (see
                                 // sim_view::cur_unit's comment) -- this function writes
                                 // nothing.
    const uint32_t player = static_cast<uint32_t>(*v.cur_player);

    // 0x00484a2c-0x00484a3e: not docked anywhere -- no home slot assigned.
    if (u.home_storage_slot == 0) {
        return 0;
    }

    // 0x00484a43-0x00484a77: the slot's owning building index; 0 = slot not bound to any building.
    const int32_t building_index = storage_of(v, player, u.home_storage_slot).b_index;
    if (building_index == 0) {
        return 0;
    }

    // 0x00484a7c-0x00484af6: the building must be alive (energy>0.0), online, and fully operational
    // (built_flags == BUILT_FLAGS_OPERATIONAL, i.e. connected AND staffed) -- pure AND, any failure
    // funnels to "return 0". The FCOMP/JNC energy test is a single scalar compare against the literal
    // 0.0 (see header banner) -- reproduced as a plain `<=`.
    const building &b = building_of(v, player, building_index);
    if (b.energy <= 0.0 || b.online_state == 0 || b.built_flags != BUILT_FLAGS_OPERATIONAL) {
        return 0;
    }

    // 0x00484aff-0x00484b3d: does this building's type still accept CUR_UNIT's type? (raw ids, the
    // callee does its own cfg lookup internally.)
    const int32_t accepts = c.storage_type_accepts_unit(b.building_id, u.unit_proto_id);
    if (accepts == 0) {
        return 0;
    }

    return building_index;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_get_ready_home_building() {
    sim_state st = state();
    return detail::unit_get_ready_home_building(st.read, live_unit_get_ready_home_building_calls());
}


} // namespace mh::sim
