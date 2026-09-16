#include "sim/sim_bldg_staffed_flag.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_staffed_flag_calls &live_bldg_staffed_flag_calls() {
    static const bldg_staffed_flag_calls gc = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_state_change),
    };
    return gc;
}

namespace detail {

void set_staffed_flag(sim_store &own, const bldg_staffed_flag_calls &gc, uint16_t player,
                      int32_t building_index) {
    // 0x004966bb-0x004966f0: OR bit 0x2 into built_flags. The assembly re-derives the record's
    // address a second time for the store (two independent IMUL/ADD passes over the same
    // player/building_index) rather than reusing the load's address -- observationally identical
    // here (no intervening write can change which record this is), so a single accessor call is
    // used for both the read and the write side of the |=.
    own.building_at((uint32_t)player, building_index).built_flags |= 0x2u;

    // 0x004966f6-0x00496701: unconditional notify, same two params re-widened from the 16-bit
    // player.
    gc.notify_state_change(player, (uint32_t)building_index);
}

void clear_staffed_flag(sim_store &own, const bldg_staffed_flag_calls &gc, uint16_t player,
                        uint32_t building_id) {
    // 0x00496722-0x00496757: AND bit 0x2 out of built_flags (mask 0xfd == ~0x2 as a byte). Mirror of
    // set_staffed_flag above -- same re-derived-address non-issue.
    own.building_at((uint32_t)player, (int32_t)building_id).built_flags &= 0xfdu;

    // 0x0049675d-0x00496768: unconditional notify.
    gc.notify_state_change(player, building_id);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void set_staffed_flag(uint16_t player, int32_t building_index) {
    sim_state st = state();
    detail::set_staffed_flag(st.own, live_bldg_staffed_flag_calls(), player, building_index);
}

void clear_staffed_flag(uint16_t player, uint32_t building_id) {
    sim_state st = state();
    detail::clear_staffed_flag(st.own, live_bldg_staffed_flag_calls(), player, building_id);
}


} // namespace mh::sim
