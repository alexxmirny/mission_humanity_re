#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_select_weapon.h" // UNIT_SELECT_WEAPON_FOUND / UNIT_SELECT_WEAPON_NOT_FOUND (reused, not redeclared)

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
struct unit_goal_in_weapon_range_calls {
    // llm_strat_tile_dist_wrapped @0x0049404e -- wrapped tile distance between (x1,y1) and (x2,y2).
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
};

const unit_goal_in_weapon_range_calls &live_unit_goal_in_weapon_range_calls();

namespace detail {

// llm_strat_unit_goal_in_weapon_range @0x0044988f. See the header banner above for the full
// derivation. Pure read: no sim_store parameter (nothing in this closure writes anything here).
int32_t unit_goal_in_weapon_range(const sim_view &v, const unit_goal_in_weapon_range_calls &c,
                                  int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                  int32_t target_class);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_goal_in_weapon_range_calls(). Matches
// the committed prototype (sig_llm_strat_unit_goal_in_weapon_range) exactly -- parameter names here
// are the prototype's own (target_x/target_y/target_kind), see the header banner for why the body
// uses tile_x/tile_y/target_class internally instead.
int32_t unit_goal_in_weapon_range(int32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                                  int32_t target_kind);

namespace detail {
} // namespace detail

} // namespace mh::sim
