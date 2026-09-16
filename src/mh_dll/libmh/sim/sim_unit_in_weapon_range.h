#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_select_weapon.h" // UNIT_SELECT_WEAPON_FOUND / UNIT_SELECT_WEAPON_NOT_FOUND (reused, not redeclared)

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest. One
// callee, an ORIGINAL function outside this batch.
struct unit_in_weapon_range_calls {
    // llm_strat_tile_dist_wrapped @0x0049404e -- wrapped tile distance between (x1,y1) and (x2,y2).
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
};

const unit_in_weapon_range_calls &live_unit_in_weapon_range_calls();

namespace detail {

// llm_strat_unit_in_weapon_range @0x0044965d. See the header banner above for the full derivation.
// Pure read: no sim_store parameter (nothing in this closure writes anything here).
uint32_t unit_in_weapon_range(const sim_view &v, const unit_in_weapon_range_calls &c, int32_t player,
                              int32_t unit_idx, int32_t tile_x, int32_t tile_y, int32_t target_class);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_in_weapon_range_calls(). Matches the
// committed prototype (sig_llm_strat_unit_in_weapon_range) exactly.
uint32_t unit_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                              int32_t target_class);

namespace detail {
} // namespace detail

} // namespace mh::sim
