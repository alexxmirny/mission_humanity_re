#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h"   // EVENT_INFO_REFRESH (shared -- see that header's own ODR-safety note)
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_SHUTTLE/_H_SHUTTLE
#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_storage_dock_list_append @0x0048b5a7. See the header SHAPE note above. No outward call,
// so no `_calls` struct -- same shape sim_storage_exit_query.cpp's storage_exit_tile_is_clear() uses
// for the same reason.
void storage_dock_list_append(const sim_view &v, sim_store &own, uint16_t player, uint16_t unit_idx,
                              uint16_t storage_slot);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_storage_dock_list_append) exactly. board_unit's own detail:: body calls the
// detail:: overload above directly instead (see the .cpp's FIXED note), not this wrapper.
void storage_dock_list_append(uint16_t player, uint16_t unit_idx, uint16_t storage_slot);

// ---- board_unit's outward calls (all besides dock_list_append, which is OUR OWN sibling -- see the
// .cpp) ------------------------------------------------------------------------------------------
struct storage_board_unit_calls {
    uint32_t (*set_event)(uint32_t type); // game_SetEvent @0x00413a52
    uint32_t (*shuttle_load_passengers)(uint16_t player, int32_t building_index,
                                        uint32_t cap); // llm_prod_shuttle_load_passengers @0x0048e5e5
    void (*unit_set_state_of)(int32_t player, int32_t unit_index,
                              int16_t state);                                        // @0x004869b0
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius); // @0x00496868
    int32_t (*ctrl_group_contains_unit)(uint32_t unit_id, int32_t count,
                                        int32_t group_index); // @0x00445f27
    void (*unit_ctrlgroup_remove_member)(uint32_t unit_idx, int32_t *count_ptr,
                                         int32_t group_idx); // @0x0044947e
    void (*ai_group_member_count_adjust)(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                                         uint32_t mode); // @0x004db499
};

const storage_board_unit_calls &live_storage_board_unit_calls();

namespace detail {

// llm_strat_storage_board_unit @0x0048b6ba. See the header SHAPE/HAZARD/STATE-VALUE notes above.
void storage_board_unit(const sim_view &v, sim_store &own, const storage_board_unit_calls &c,
                        uint16_t player, uint32_t unit_idx, uint32_t storage_idx);

} // namespace detail

// Live wrapper: matches the committed prototype (sig_llm_strat_storage_board_unit) exactly.
void storage_board_unit(uint16_t player, uint32_t unit_idx, uint32_t storage_idx);

// ---- accept_landing's outward calls --------------------------------------------------------------
struct storage_accept_landing_calls {
    void (*setup_approach_path)(uint32_t player, int32_t unit_index, int32_t path_slot,
                                int32_t storage_slot); // llm_strat_storage_setup_approach_path @0x00495b6f
    int32_t (*ctrl_group_contains_unit)(uint32_t unit_id, int32_t count,
                                        int32_t group_index); // @0x00445f27
    void (*unit_ctrlgroup_remove_member)(uint32_t unit_idx, int32_t *count_ptr,
                                         int32_t group_idx);                      // @0x0044947e
    uint32_t (*set_event)(uint32_t type);                                         // game_SetEvent @0x00413a52
    void (*unit_set_state_of)(int32_t player, int32_t unit_index, int16_t state); // @0x004869b0
};

const storage_accept_landing_calls &live_storage_accept_landing_calls();

namespace detail {

// llm_strat_storage_accept_landing @0x0048ba1a. See the header SHAPE note above.
void storage_accept_landing(const sim_view &v, sim_store &own, const storage_accept_landing_calls &c,
                            int32_t player, int32_t unit_index, int32_t storage_slot,
                            int32_t path_slot);

} // namespace detail

// Live wrapper: matches the committed prototype (sig_llm_strat_storage_accept_landing) exactly.
void storage_accept_landing(int32_t player, int32_t unit_index, int32_t storage_slot,
                            int32_t path_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
