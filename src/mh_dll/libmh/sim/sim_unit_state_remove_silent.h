#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI (shared there; see sim_unit_state_die_explode.h's identical include for the ODR-avoidance reasoning)
#include "sim/sim_state.h"

namespace mh::sim {

// unit::state == 4 ("corpse_fow_decay"), this function's own literal (0x00485b29's EAX operand). No
// backing Ghidra enum for unit state (checked, per sim_unit_teardown.h's / sim_unit_state_die_explode.h's
// identical DECLARED NEED) -- named locally, per-TU-prefixed to avoid the ODR collision risk those
// files document for this exact family of names (matches UNIT_STATE_DIE_EXPLODE_CORPSE_FOW_DECAY /
// UNIT_TEARDOWN_STATE_CORPSE_FOW_DECAY's sibling precedent).
inline constexpr uint16_t UNIT_STATE_REMOVE_SILENT_CORPSE_FOW_DECAY = 4;

// game::e::event member 14 ("MAP_OBJECTS_REFRESH" per the strategic-sim notes' resolved 25-member
// enum) -- no generated C++ enum exists yet (see sim_unit_state_die_explode.h's identical DECLARED
// NEED); named locally per this codebase's established per-TU convention.
inline constexpr uint32_t UNIT_STATE_REMOVE_SILENT_MAP_OBJECTS_REFRESH = 14u;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// twelve are ORIGINAL functions outside this batch -- none need reimplementing here;
// live_unit_state_remove_silent_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .cpp).
struct unit_state_remove_silent_calls {
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index,
                                     int32_t hard_remove);            // @0x004db551
    void (*unit_remove_from_map)(uint16_t player, uint32_t unit_idx); // @0x00487252
    void (*unit_on_destroyed)(uint16_t player, uint32_t unit_idx);    // @0x004877bc
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx,
                               uint32_t mode);                                      // @0x004dac44
    void (*unit_housing_count_remove)(int32_t player, int32_t unit_proto_id);       // @0x00497842
    int32_t (*storage_release_door_held_by_unit)(int32_t player, int32_t unit_idx); // @0x00485466
    void (*path_free_slot)(uint16_t player, int32_t unit_index);                    // @0x004969e8
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode);               // @0x00498089
    void (*unit_set_state)(uint16_t new_state);                                     // @0x004866c9
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y,
                                  uint8_t sight);               // @0x0049681a
    uint32_t (*game_SetEvent)(uint32_t type);                   // @0x00413a52
    void (*unit_notify_ui)(uint32_t side, uint32_t unit_index); // @0x00488a22
};

const unit_state_remove_silent_calls &live_unit_state_remove_silent_calls();

namespace detail {

// llm_strat_unit_state_remove_silent @0x0048592d. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_remove_silent(const sim_view &v, sim_store &own, const unit_state_remove_silent_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_remove_silent_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_remove_silent) exactly.
void unit_state_remove_silent();

namespace detail {
} // namespace detail

} // namespace mh::sim
