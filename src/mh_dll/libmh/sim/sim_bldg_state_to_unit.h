#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/H_MOTHER, UNIT_STATE_IDLE_SCATTER
#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra enum (rule 17a fallback) --------
// See the DECLARED NEED banner above -- all three need an enum-member check before arming.
inline constexpr uint16_t BLDG_STATE_NO_EQUIVALENT_UNIT = 1;    // early-exit AND unit_create-failure state
inline constexpr uint16_t UNIT_STATE_JUST_DEPLOYED      = 0x7c; // new unit's initial `state`
inline constexpr uint16_t UNIT_ORDER_AWAY_FROM_DEST     = 0x31; // new unit's `order` when off destination planet

// The new unit's initial `move_microstep`, stamped AFTER it is used once to index
// _G_LLM_STRAT_MOVE_MICROSTEPS for the initial facing (0x1f = 31, presumably "not mid-interpolation").
inline constexpr int32_t MOVE_MICROSTEP_FULL = 0x1f;

// `llm_strat_unit_create`'s is_ship parameter -- ONLY the literal value 1 means "ship" per that
// function's own hazard note (an equality test, not truthiness); this call passes 2, matching
// sim_prod_spawn_arrived_unit.cpp's identical literal for the same non-ship-narrowed roster scan.
inline constexpr uint8_t UNIT_CREATE_IS_SHIP_SENTINEL = 2;

// See the DECLARED NEED above: NOT in mh_llm_prod_shuttle_slot::status's own field comment yet.
inline constexpr int16_t SHUTTLE_SLOT_STATUS_UNIT_DEPLOYED = static_cast<int16_t>(0xcb);

// ---- the outward calls -----------------------------------------------------------------------
struct bldg_state_to_unit_calls {
    // out_x/out_y: committed pointee is uint32_t * (TACT1-P C6, 2026-09-04), not the old blunt void *.
    void (*calc_placement_corner_from_center_by_type)(uint16_t building_type, int32_t center_x,
                                                      int32_t center_y, uint32_t *out_x, uint32_t *out_y);
    void (*population_remove)(uint32_t player, int32_t count);
    uint32_t (*unit_create)(uint32_t x, uint32_t y, uint16_t unit, uint16_t player, uint8_t is_ship);
    int32_t (*mother_reelect_primary)(int32_t player, int32_t x, int32_t y);
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index, int32_t hard_remove);
    void (*bldg_unmap_footprint)(uint16_t player, int32_t building_index);
    void (*unit_purge_unregistered)(uint32_t player);
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode);
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col, int32_t tile_row); // event record: the
                                                                               // hosted sink runs the
                                                                               // original offscreen_
                                                                               // snd_volume+snd_play
                                                                               // pair at emit
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t anim_frame, double elapsed, uint32_t flag);
};

const bldg_state_to_unit_calls &live_bldg_state_to_unit_calls();

namespace detail {

// llm_strat_bldg_state_to_unit @0x00471443. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. No parameters -- operates on `v`/`own`'s ambient
// cur_building/cur_player/cur_index/planet_index alone, matching the original's void(void) signature.
void bldg_state_to_unit(const sim_view &v, sim_store &own, const bldg_state_to_unit_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_state_to_unit_calls(). Matches the
// committed prototype (sig_llm_strat_bldg_state_to_unit) exactly.
void bldg_state_to_unit();

namespace detail {
} // namespace detail

} // namespace mh::sim
