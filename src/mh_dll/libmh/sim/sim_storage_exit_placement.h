#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_GARAGE/_A_SHUTTLE/_H_GARAGE/_H_SHUTTLE/_H_BARRACKS/_H_HELIPAD
#include "sim/sim_state.h"

namespace mh::sim {

// See the header banner's "UNCONFIRMED STATE LITERALS" note -- neither has an existing binding
// anywhere else in libmh/sim/, so both are declared here as this TU's own local copies, per the
// established per-TU-constant convention (same posture as sim_unit_state_exit.h's
// UNIT_STATE_EXIT_STORAGE_BEGIN).
inline constexpr uint16_t UNIT_STATE_EXIT_WALK_OUT    = 0x21; // llm_strat_storage_place_exit_ground's final state
inline constexpr uint16_t UNIT_STATE_EXIT_AIR_HELIPAD = 0x28; // llm_strat_storage_exit_air, H_HELIPAD storage
inline constexpr uint16_t UNIT_STATE_EXIT_AIR_DEFAULT = 0x27; // llm_strat_storage_exit_air, any other storage

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All eight
// are ORIGINAL functions outside this batch. Signatures copied verbatim from addr/mh_calls.gen.h.
struct storage_exit_placement_calls {
    void (*storage_remove_docked_unit)(uint16_t player, int32_t unit_index,
                                       int32_t storage_slot); // llm_strat_storage_remove_docked_unit @0x00489dc4
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2,
                           int32_t y2); // llm_strat_dir_from_to @0x0049482b
    int32_t (*prod_shuttle_unload_passengers)(
        uint16_t player, int32_t building_index,
        int32_t cap); // llm_prod_shuttle_unload_passengers @0x0048e6f2 (return value unused)
    void (*bldg_flush_cargo_hold)(uint32_t player,
                                  int32_t  building_index); // llm_strat_bldg_flush_cargo_hold @0x0048e046
    void (*unit_soldiers_set_heading)(
        uint16_t player, int32_t unit_index,
        uint8_t sprite_frame); // llm_strat_unit_soldiers_set_heading @0x00489ab6
    void (*cursor_lookup_offset_pair)(
        int32_t table_col, int32_t table_row, char *out_a,
        char *out_b); // llm_ui_cursor_lookup_offset_pair @0x00486b17 (pure query -- do not suppress)
    void (*unit_set_state_of)(int32_t player, int32_t unit_index,
                              int16_t state); // llm_strat_unit_set_state_of @0x004869b0
    void (*storage_setup_exit_path)(uint32_t player, int32_t unit_index, uint8_t exit_x, uint8_t exit_y,
                                    int32_t path_slot,
                                    int32_t storage_slot); // llm_strat_storage_setup_exit_path @0x00495cd3
};

const storage_exit_placement_calls &live_storage_exit_placement_calls();

namespace detail {

// llm_strat_storage_place_exit_ground @0x00489f54. See the header banner for the full derivation.
void storage_place_exit_ground(const sim_view &v, sim_store &own, const storage_exit_placement_calls &c,
                               uint16_t player, int32_t unit_index, int32_t storage_slot);

// llm_strat_storage_exit_air @0x0048b01d. See the header banner for the full derivation.
void storage_exit_air(const sim_view &v, sim_store &own, const storage_exit_placement_calls &c,
                      uint16_t player, int32_t unit_index, int32_t storage_slot, uint32_t param_4);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes (sig_llm_strat_storage_place_exit_ground /
// sig_llm_strat_storage_exit_air in addr/mh_export.gen.h) exactly.

void storage_place_exit_ground(uint16_t player, int32_t unit_index, int32_t storage_slot);
void storage_exit_air(uint16_t player, int32_t unit_index, int32_t storage_slot, uint32_t param_4);

namespace detail {
} // namespace detail

} // namespace mh::sim
