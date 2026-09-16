#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches, indirected for offline testability (same reason as
// every other module here: a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest). The bldg_tick2_funcs[]
// dispatch is NOT here -- it is read straight off sim_view, same as unit_state_funcs in
// sim_unit_tick.h.
struct bldg_tick_animation_state_calls {
    // llm_cfg_anim_frame_at_progress @0x0046190d. PURE function of its args (no state touched) --
    // already committed in addr/mh_calls.gen.h.
    int32_t (*cfg_anim_frame_at_progress)(int32_t start_frame, double progress_fraction);
};

const bldg_tick_animation_state_calls &live_bldg_tick_animation_state_calls();

namespace detail {

// llm_strat_bldg_tick_animation_state @0x00476305. See the header banner above for the full
// derivation; the .cpp carries the per-line address citation.
void bldg_tick_animation_state(const sim_view &v, sim_store &own,
                               const bldg_tick_animation_state_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_tick_animation_state_calls(). Matches the
// committed prototype (sig_llm_strat_bldg_tick_animation_state) exactly: void(void), no parameters.
void bldg_tick_animation_state();

namespace detail {
} // namespace detail

// `[promote] bldg_tick_animation_state=1` -- see sim_building_tick.h's own install_promotion_
// building_tick for the full rationale (identical shape, G13/G19 dispatcher class).
int install_promotion_bldg_tick_animation_state(int default_on);

} // namespace mh::sim
