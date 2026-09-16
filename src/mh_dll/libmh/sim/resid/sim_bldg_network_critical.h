#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call, indirected so detail:: stays testable under simtest (the calls-seam
// convention, translator-brief rule 3b/3c) -- `llm_strat_bldg_power_network_recompute` is a frontier
// original per this batch's measured callee table (sim_resid rule 2's table lists it as a plain
// callee, not one of the three flagged intra-slice/sibling edges), so it is reached through
// `mh::call::` in `live_bldg_network_critical_calls()`, never called directly and never through a
// sibling `detail::` binding.
struct bldg_network_critical_calls {
    void (*bldg_power_network_recompute)(uint16_t player); // llm_strat_bldg_power_network_recompute @0x00491c76
};

const bldg_network_critical_calls &live_bldg_network_critical_calls();

namespace detail {

// llm_strat_bldg_is_network_critical @0x00497405. Reads the roster through `v`/`building_of`, writes
// the candidate's and the sentinel slot's `.energy` through `own.building_at(...)`, and reaches the
// one outward call through `c`. Returns 0/1 (bool-shaped `int32_t`, matching the committed
// `sig_llm_strat_bldg_is_network_critical` return type).
int32_t bldg_is_network_critical(const sim_view &v, sim_store &own, const bldg_network_critical_calls &c,
                                 int32_t player, int32_t b_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_network_critical_calls().
int32_t bldg_is_network_critical(int32_t player, int32_t b_index);

} // namespace mh::sim
