#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// UNIT_STATE_STOP_TO_DEFAULT is declared in sim_order_enqueue.h and reused here (see the header banner
// above) -- included via that header rather than re-declared, matching sim_unit_state_move_walker.h's
// and sim_unit_state_attack_building.h's own established sharing.

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All eight
// are ORIGINAL functions -- see the header banner's note on the three that already have their own
// sibling reimplementation elsewhere in this tree; this TU still calls all eight as the original.
struct unit_state_squad_merge_calls {
    int32_t (*wrap_delta_x)(int32_t pos_a, uint32_t unused_param, int32_t pos_b); // @0x004940a9
    int32_t (*wrap_delta_y)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);      // @0x00494131
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);       // @0x0049482b
    void (*cursor_apply_anim_frame_offset)(char *out_x, char *out_y,
                                           int32_t cursor_state_index); // @0x00486a8a
    void (*squad_pick_free_formation_anchor)(int32_t scratch_count, char *out_end_x,
                                             char *out_end_y);          // @0x004899ed
    void (*unit_teardown_mapped)(uint32_t player, uint32_t unit_index); // @0x0048753e
    void (*unit_change_proto_and_energy)(uint16_t player, int32_t unit_idx, int16_t proto_delta,
                                         int32_t unused, double energy_delta); // @0x00489521
    void (*unit_set_state)(uint16_t new_state);                                // @0x004866c9
};

const unit_state_squad_merge_calls &live_unit_state_squad_merge_calls();

namespace detail {

// llm_strat_unit_state_squad_merge @0x0047ea62. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_squad_merge(const sim_view &v, sim_store &own, const unit_state_squad_merge_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_squad_merge_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_squad_merge) exactly.
void unit_state_squad_merge();

namespace detail {
} // namespace detail

} // namespace mh::sim
