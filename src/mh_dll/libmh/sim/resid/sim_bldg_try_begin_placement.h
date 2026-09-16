#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. Both
// callees are frontier originals (per _CONTEXT_CE.md's per-slice callee table, neither is a
// sim_resid sibling), so they are reached through mh::call:: in
// live_bldg_try_begin_placement_calls(), never called directly.
struct bldg_try_begin_placement_calls {
    int32_t (*pay_build_cost)(uint32_t player, int32_t building_type_id);         // llm_bldg_pay_build_cost @0x00492eb1
    void (*print_queue_text_id)(int32_t text_id);                                 // llm_ui_print_queue_text_id @0x0049653d
    void (*grant_type_resources)(uint32_t player_idx, int32_t building_type_idx); // llm_strat_bldg_grant_type_resources @0x004937bf
};

const bldg_try_begin_placement_calls &live_bldg_try_begin_placement_calls();

namespace detail {

// llm_strat_bldg_try_begin_placement @0x00448c0b. Reads the mothership-presence gate through `v`,
// writes the placement id / ctrl-group-0 count / selected-building-index / suppression-bracket flag
// through `own`, and reaches both callees through `c`. Returns 1 on success, 0 on either refusal --
// see the header banner's three-exit walkthrough.
int bldg_try_begin_placement(const sim_view &v, sim_store &own, const bldg_try_begin_placement_calls &c,
                             uint32_t player_idx, int32_t building_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_try_begin_placement_calls().
int bldg_try_begin_placement(uint32_t player_idx, int32_t building_idx);

} // namespace mh::sim
