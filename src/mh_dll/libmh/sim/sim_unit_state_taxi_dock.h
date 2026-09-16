#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// llm_strat_unit_state values with no backing Ghidra enum -- own local copies (see the header
// derivation above for why these are not shared with sim_order_dispatch.cpp's / sim_unit_state_move_
// path.cpp's identically-valued but separately-declared constants).
inline constexpr uint16_t TAXI_DOCK_STATE_TAKEOFF        = 0x15; // takeoff_taxi's terminal state
inline constexpr uint16_t TAXI_DOCK_STATE_PLOT_TURN_PATH = 0x2d; // landing_request's failure state

// mh_map_object_building::online_state gate values -- see the header derivation above. Own local
// constants (this exact field/value pair has no other user in this tree).
inline constexpr int16_t TAXI_DOCK_BLDG_ONLINE_STATE_TAXI_OUT_READY = 2; // takeoff_taxi's gate
inline constexpr int16_t TAXI_DOCK_BLDG_ONLINE_STATE_DOCK_READY     = 1; // dock_taxi_in's A_PORT gate

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest.

struct unit_state_takeoff_taxi_calls {
    double (*dir_step_factor)(int32_t dir);                         // @0x00449b28
    void (*unit_set_state)(uint16_t new_state);                     // @0x004866c9
    void (*unit_takeoff_finalize)(uint32_t player, uint32_t index); // @0x0048b162
};

struct unit_state_dock_taxi_in_calls {
    double (*dir_step_factor)(int32_t dir);     // @0x00449b28
    void (*unit_set_state)(uint16_t new_state); // @0x004866c9
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx,
                               uint32_t mode);                                    // @0x004dac44
    void (*path_free_slot)(uint16_t player, int32_t unit_index);                  // @0x004969e8
    void (*bldg_flush_cargo_hold)(uint32_t player, int32_t building_index);       // @0x0048e046
    void (*prod_unbind_planet)(int32_t player, int32_t planet_slot);              // @0x0048ff73
    void (*storage_scrap_home_docked_units)(uint16_t player, int32_t unit_index); // @0x0048f89e
};

struct unit_state_landing_request_calls {
    int32_t (*unit_get_ready_home_building)();                            // @0x00484a14
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state); // @0x00486657
    void (*tile_neighbor_in_dir)(int32_t x, int32_t y, int32_t dir, int32_t *out_col,
                                 int32_t *out_row); // @0x0048b294
    void (*storage_get_approach_tile)(uint16_t param_1, uint16_t param_2, uint32_t *a2,
                                      uint32_t *param_4, uint32_t param_5); // @0x0048b37c
    int32_t (*storage_can_land)(int32_t player, uint32_t unit_index,
                                int32_t storage_slot); // @0x0048ae90
    int32_t (*path_find_free_slot)(int32_t player);    // @0x00495f1b
    void (*unit_set_state)(uint16_t new_state);        // @0x004866c9
    void (*storage_accept_landing)(int32_t player, int32_t unit_index, int32_t storage_slot,
                                   int32_t free_slot); // @0x0048ba1a
};

const unit_state_takeoff_taxi_calls    &live_unit_state_takeoff_taxi_calls();
const unit_state_dock_taxi_in_calls    &live_unit_state_dock_taxi_in_calls();
const unit_state_landing_request_calls &live_unit_state_landing_request_calls();

namespace detail {

// llm_strat_unit_state_takeoff_taxi @0x0047f69f. See the header derivation above. Zero-arg, ambient
// cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_takeoff_taxi(const sim_view &v, sim_store &own,
                             const unit_state_takeoff_taxi_calls &c);

// llm_strat_unit_state_dock_taxi_in @0x0047f981. See the header derivation above.
void unit_state_dock_taxi_in(const sim_view &v, sim_store &own,
                             const unit_state_dock_taxi_in_calls &c);

// llm_strat_unit_state_landing_request @0x004800a8. See the header derivation above.
void unit_state_landing_request(const sim_view &v, sim_store &own,
                                const unit_state_landing_request_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// committed prototypes (sig_llm_strat_unit_state_*) exactly.
void unit_state_takeoff_taxi();
void unit_state_dock_taxi_in();
void unit_state_landing_request();

namespace detail {
} // namespace detail

} // namespace mh::sim
