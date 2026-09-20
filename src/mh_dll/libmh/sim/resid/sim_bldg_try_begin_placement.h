#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// THIS TRANSLATION DELIBERATELY DIVERGES FROM THE ORIGINAL (mp:D25, 2026-09-19).
//
// The original probes affordability by PAYING the cost (llm_bldg_pay_build_cost @0x00448c56) and,
// on success, immediately RE-GRANTING it (llm_strat_bldg_grant_type_resources @0x00448c90): the
// stock nets to zero, but game_UpdateResourceStats -- a gains-only counter, negative deltas dropped
// -- books the grant, so `resource_spent[id]` (player record +0x10054.., hashed as pK_ai_econ) grows
// by the building's full cost on every build-menu click. This function runs on the CLICKING peer
// only (HUD widget callback -> llm_ui_bldg_available_projects_tab_process_events @0x00416487), so
// in a lockstep match it is a peer-local write into hashed state: the first real internet match
// (2026-09-19) tripped the desync watch at step 750 with exactly one Academy's cost (800/450 of
// ids 1/2) on the host's own slot, and the 2026-09-01 3-human captures went odd in each peer's
// own slot at each human's first click. The real payment happens later, on every peer, through
// build order 0x19 (llm_strat_order_queue_dispatch @0x00467476).
//
// So the probe here is SIDE-EFFECT-FREE: bldg_can_afford_build_cost (sim/sim_bldg_pay_costs.h) is
// pay_build_cost's pass 1 alone -- same invention gate, same shortage codes, no game_SpendResource,
// no llm_resource_add -- and the grant is gone with the payment it undid. Every other observable
// (the return, the refusal text id and its suppression bracket, the three success-path writes) is
// the original's. `[config] mode=original` keeps the original body and therefore the bug.
struct bldg_try_begin_placement_calls {
    int32_t (*can_afford_build_cost)(uint32_t player, int32_t building_type_id); // bldg_can_afford_build_cost (ours; was pay @0x00492eb1 + grant @0x004937bf)
    void (*print_queue_text_id)(int32_t text_id);                                // llm_ui_print_queue_text_id @0x0049653d
};

const bldg_try_begin_placement_calls &live_bldg_try_begin_placement_calls();

namespace detail {

// llm_strat_bldg_try_begin_placement @0x00448c0b. Reads the mothership-presence gate through `v`,
// writes the placement id / ctrl-group-0 count / selected-building-index / suppression-bracket flag
// through `own`, and reaches the affordability probe + the refusal print through `c`. Returns 1 on
// success, 0 on either refusal -- see the header banner's three-exit walkthrough.
int bldg_try_begin_placement(const sim_view &v, sim_store &own, const bldg_try_begin_placement_calls &c,
                             uint32_t player_idx, int32_t building_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_try_begin_placement_calls().
int bldg_try_begin_placement(uint32_t player_idx, int32_t building_idx);

} // namespace mh::sim
