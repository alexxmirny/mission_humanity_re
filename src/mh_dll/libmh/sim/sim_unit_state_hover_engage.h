#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Signatures copied verbatim from addr/mh_calls.gen.h (checked against this function's own
// register-order use in the .cpp).
struct unit_state_hover_engage_calls {
    int32_t (*order_queue_find_index)(int32_t player, int32_t unit_idx,
                                      int32_t kind_tag); // llm_strat_order_queue_find_index @0x00469996
    void (*order_queue_apply_and_dequeue)(uint32_t player, int32_t unit_idx,
                                          int32_t queue_idx);              // @0x00469a37
    void (*unit_set_state)(uint16_t new_state);                            // @0x004866c9
    void (*unit_set_state_order)(uint16_t new_state, uint16_t new_order);  // @0x00486657
    int32_t (*unit_hover_tile_crowded)(int32_t player, uint32_t unit_idx); // @0x0048cf3e
    int32_t (*unit_chase_check)();                                         // @0x004866fb
};

const unit_state_hover_engage_calls &live_unit_state_hover_engage_calls();

namespace detail {

// llm_strat_unit_state_hover_engage @0x004807d9. See the header derivation above. Zero-arg, ambient
// cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_hover_engage(const sim_view &v, sim_store &own, const unit_state_hover_engage_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_hover_engage_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_hover_engage) exactly.
void unit_state_hover_engage();

namespace detail {
} // namespace detail

} // namespace mh::sim
