#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as sim_unit_on_destroyed.h: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe
// simtest. All ten are ORIGINAL functions outside this batch -- none need reimplementing here.
// Signatures copied verbatim from addr/mh_calls.gen.h (checked against each function's own
// register-order use in the .cpp, not assumed). Shared by BOTH detail:: bodies below -- field uses
// every member; docked uses spawn_docked/soldier_unlink/ctrl_group_contains_unit/ctrl_group_assign/
// cursor_lookup_offset_pair only (spawn_on_tile/dir_from_to/squad_pick_lead_soldier_in_direction/
// cursor_apply_anim_frame_offset are field-only).
struct unit_transport_unload_calls {
    int32_t (*spawn_on_tile)(uint32_t x, uint32_t y, uint16_t unit_proto_id, uint16_t player);
    int32_t (*spawn_docked)(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot);
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    uint32_t (*squad_pick_lead_soldier_in_direction)(int32_t player, uint32_t unit_above_word,
                                                     uint32_t dir);
    void (*soldier_unlink)(uint16_t player, int32_t unit_idx, uint32_t soldier_idx);
    void (*cursor_apply_anim_frame_offset)(char *out_x, char *out_y, int32_t cursor_state_index);
    void (*soldiers_start_walk_anim)(uint32_t player, int32_t unit_index);
    int32_t (*ctrl_group_contains_unit)(uint32_t unit_id, int32_t count, int32_t group_index);
    void (*ctrl_group_assign)(int32_t unit_id, int32_t new_group_id);
    void (*cursor_lookup_offset_pair)(int32_t table_col, int32_t table_row, char *out_a, char *out_b);
};

const unit_transport_unload_calls &live_unit_transport_unload_calls();

namespace detail {

// llm_unit_transport_unload_field @0x00488a8b. See the header banner above.
void unload_field(const sim_view &v, sim_store &own, const unit_transport_unload_calls &c,
                  uint32_t transport_player, uint32_t transport_unit_index);

// llm_unit_transport_unload_docked @0x004891d8. See the header banner above.
void unload_docked(const sim_view &v, sim_store &own, const unit_transport_unload_calls &c,
                   uint32_t player, uint32_t unit_index);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_transport_unload_calls(). Match the
// committed __watcall(EAX,EDX) shapes (sig_llm_unit_transport_unload_field/_docked) exactly.
void unload_field(uint32_t transport_player, uint32_t transport_unit_index);
void unload_docked(uint32_t player, uint32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
