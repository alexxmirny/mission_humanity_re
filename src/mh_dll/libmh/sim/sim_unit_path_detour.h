#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one ORIGINAL outward call this function makes (besides its own recursive self-call, which is a
// genuine local C++ call -- see the header banner). Indirected through a one-member struct for the
// same testability reason every other sim TU indirects its callees (see sim_bldg_defense_cost.h's
// banner for the fullest statement of why this applies even to a single callee).
struct unit_path_detour_calls {
    int32_t (*unit_path_queue_splice)(int32_t owner_index, int32_t unit_index, int32_t slot,
                                      int32_t count); // @0x00421758
};

const unit_path_detour_calls &live_unit_path_detour_calls();

namespace detail {

// llm_strat_unit_path_detour @0x004209b2. See the header banner for the full derivation.
int32_t unit_path_detour(const sim_view &v, sim_store &own, const unit_path_detour_calls &c,
                         int32_t player, int32_t unit_idx, int32_t alt_unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_path_detour_calls(). Matches the committed
// prototype (mh_calls.gen.h's llm_strat_unit_path_detour) exactly. Also the recursion TARGET this
// function's own body calls into -- see the header banner's recursion note.
int32_t unit_path_detour(int32_t player, int32_t unit_idx, int32_t alt_unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
