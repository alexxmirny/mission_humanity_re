#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h"   // EVENT_INFO_REFRESH -- shared across sim/ TUs
#include "sim/sim_order_enqueue.h" // UNIT_STATE_IDLE_SCATTER
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER / _H_HELI_MOTHER

namespace mh::sim {

// llm_strat_unit_state value with no backing Ghidra enum (rule 17a) -- same value as
// sim_unit_apply_damage.h's own UNIT_STATE_DEPLOY_TO_BUILDING. Deliberately NOT re-declared at this
// header's namespace scope (unlike that per-TU convention's usual shape): mh_nettest's selftest TU
// pulls in multiple sim/*.h headers together, and two `inline constexpr` symbols with the same name at
// the same namespace scope is a hard ODR redefinition once both are visible in one translation unit
// (caught at build time -- see sim_unit_state_deploy.cpp, which declares it file-local instead).

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. Signatures
// copied verbatim from addr/mh_calls.gen.h.
struct unit_state_deploy_approach_calls {
    void (*tile_neighbor_in_dir)(int32_t x, int32_t y, int32_t dir, int32_t *out_col,
                                 int32_t *out_row); // @0x0048b294
    void (*calc_placement_corner_from_center)(uint16_t unit_index, int32_t center_x, int32_t center_y,
                                              uint32_t *out_col, uint32_t *out_row); // @0x0048d054
    int32_t (*footprint_is_clear)(int32_t x, int32_t y, int32_t building_type,
                                  uint32_t viewer); // @0x0049396b
    void (*footprint_clear_passable)(int32_t origin_x, int32_t origin_y,
                                     int32_t building_idx);                                         // @0x0049383b
    void (*unit_unlink_tile)(uint32_t unit_player, uint16_t unit_index);                            // @0x00486f41
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius);                // @0x00496868
    void (*map_unit_put_on_map)(uint16_t player, uint16_t unit_index, uint8_t x, uint8_t y);        // @0x00486c6a
    void (*map_fow_update_fow_plus)(uint32_t player, uint32_t x, uint32_t y, uint8_t sight);        // @0x0049681a
    int32_t (*ctrl_group_contains_unit)(uint32_t unit_id, int32_t count, int32_t group_index);      // @0x00445f27
    void (*unit_ctrlgroup_remove_member)(uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx); // @0x0044947e
    uint32_t (*game_set_event)(uint32_t type);                                                      // @0x00413a52
    void (*unit_set_state)(uint16_t new_state);                                                     // @0x004866c9
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state);                           // @0x00486657
};

struct unit_state_deploy_to_building_calls {
    void (*calc_placement_corner_from_center)(uint16_t unit_index, int32_t center_x, int32_t center_y,
                                              uint32_t *out_col, uint32_t *out_row); // @0x0048d054
    int32_t (*construct_finalize)(uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                  uint32_t x_b, uint32_t building_id);       // @0x00462e66
    void (*bldg_update_charge_pips)(uint16_t player, uint32_t building_id);  // @0x00478eb4
    int32_t (*mother_reelect_primary)(int32_t player, int32_t x, int32_t y); // @0x00498aad
    void (*bldg_construction_complete)(uint32_t player, uint32_t building_index, uint32_t param_3,
                                       uint32_t param_4); // @0x00478d70
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col,
                        int32_t tile_row); // event record: the hosted sink runs the original
                                           // offscreen_snd_volume+snd_play pair at emit
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t param_3, double param_4,
                              uint32_t param_5);                                     // @0x00464855
    void (*unit_unlink_tile)(uint32_t unit_player, uint16_t unit_index);             // @0x00486f41
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius); // @0x00496868
    void (*unit_teardown)(uint32_t player, uint16_t unit_index);                     // @0x00487ba5
};

const unit_state_deploy_approach_calls    &live_unit_state_deploy_approach_calls();
const unit_state_deploy_to_building_calls &live_unit_state_deploy_to_building_calls();

namespace detail {

// llm_strat_unit_state_deploy_approach @0x0048117c. See the header derivation above (in particular the
// out-pointer-pair CORRECTION).
void unit_state_deploy_approach(const sim_view &v, sim_store &own,
                                const unit_state_deploy_approach_calls &c);

// llm_strat_unit_state_deploy_to_building @0x00481a6b. See the header derivation above (in particular
// the UNMODELABLE REGISTER LEAK note).
void unit_state_deploy_to_building(const sim_view &v, sim_store &own,
                                   const unit_state_deploy_to_building_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// committed prototypes (sig_llm_strat_unit_state_deploy_*) exactly -- both zero-arg void(void).
void unit_state_deploy_approach();
void unit_state_deploy_to_building();

namespace detail {
} // namespace detail

} // namespace mh::sim
