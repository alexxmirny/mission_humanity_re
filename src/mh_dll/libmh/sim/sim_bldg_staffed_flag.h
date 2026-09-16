#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee both functions reach, ORIGINAL and already committed in
// addr/mh_calls.gen.h -- indirected for offline testability, matching every other sibling `_calls`
// struct in this subsystem (e.g. sim_bldg_worker_assign.h's worker_assign_calls).
struct bldg_staffed_flag_calls {
    // llm_strat_bldg_notify_state_change @0x00470c5c. EAX=player, EDX=building_index. Called
    // unconditionally by both functions, after the bit flip.
    void (*notify_state_change)(uint16_t player, uint32_t building_id);
};

const bldg_staffed_flag_calls &live_bldg_staffed_flag_calls();

namespace detail {

// llm_strat_bldg_set_staffed_flag @0x004966a4. ORs bit 0x2 into
// buildings[player][building_index].built_flags, then notifies unconditionally.
void set_staffed_flag(sim_store &own, const bldg_staffed_flag_calls &gc, uint16_t player,
                      int32_t building_index);

// llm_strat_bldg_clear_staffed_flag @0x0049670b. Mirror of set_staffed_flag above: ANDs bit 0x2 out
// of buildings[player][building_id].built_flags, then notifies unconditionally.
void clear_staffed_flag(sim_store &own, const bldg_staffed_flag_calls &gc, uint16_t player,
                        uint32_t building_id);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the COMMITTED prototypes (addr/mh_calls.gen.h / addr/mh_export.gen.h) --
// see the header banner above on why the two are not unified to one type.

void set_staffed_flag(uint16_t player, int32_t building_index);
void clear_staffed_flag(uint16_t player, uint32_t building_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
