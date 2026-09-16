#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI, UNIT_TYPE_A_HELI_MOTHER, UNIT_TYPE_H_HELI_MOTHER

namespace mh::sim {

// unit::state == 4 ("corpse_fow_decay"), this function's own literal (0x004858ab's EAX operand).
// No backing Ghidra enum for unit state (checked, per sim_unit_teardown.h's identical DECLARED NEED)
// -- named locally per rule 17a's fallback, matching UNIT_TEARDOWN_STATE_CORPSE_FOW_DECAY /
// UNIT_STATE_CORPSE_FOW_DECAY's sibling precedents rather than importing either (avoids the ODR
// collision risk sim_unit_teardown.h's own DECLARED NEED documents for this exact family of names).
inline constexpr uint16_t UNIT_STATE_DIE_EXPLODE_CORPSE_FOW_DECAY = 4;

// game::e::event member 14 ("MAP_OBJECTS_REFRESH" per the strategic-sim notes' resolved 25-member
// enum) -- no generated C++ enum exists yet (see sim_unit_on_destroyed.h's identical DECLARED NEED);
// named locally per this codebase's established per-TU convention (sim_bldg_state_destroyed.cpp's
// BLDG_STATE_DESTROYED_MAP_OBJECTS_REFRESH, sim_unit_init_record.h's own MAP_OBJECTS_REFRESH, etc.).
inline constexpr uint32_t UNIT_STATE_DIE_EXPLODE_MAP_OBJECTS_REFRESH = 14u;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// eighteen are ORIGINAL functions outside this batch -- none need reimplementing here;
// live_unit_state_die_explode_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .cpp).
struct unit_state_die_explode_calls {
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y); // llm_strat_unit_get_coords @0x0044b141
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col,
                        int32_t tile_row); // event record: the hosted sink runs the original
                                           // offscreen_snd_volume+snd_play pair at emit
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t anim_id, double elapsed,
                              uint32_t owner_or_flag);                      // @0x00464855
    int32_t (*rand_below)(int32_t upper_bound);                             // llm_rand_below @0x00499f49
    void (*unit_remove_from_map)(uint16_t player, uint32_t unit_idx);       // @0x00487252
    uint32_t (*unit_calc_render_fine_y)(uint16_t player, int32_t unit_idx); // @0x0048ccb4
    void (*unit_on_destroyed)(uint16_t player, uint32_t unit_idx);          // @0x004877bc
    void (*game_sp_outcome_announce)();                                     // @0x0049803b
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

const unit_state_die_explode_calls &live_unit_state_die_explode_calls();

namespace detail {

// llm_strat_unit_state_die_explode @0x004854f3. See the header derivation above for the full shape
// and both declared needs. Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's
// void(void) signature.
void unit_state_die_explode(const sim_view &v, sim_store &own, const unit_state_die_explode_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_die_explode_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_die_explode) exactly.
void unit_state_die_explode();

namespace detail {
} // namespace detail

} // namespace mh::sim
