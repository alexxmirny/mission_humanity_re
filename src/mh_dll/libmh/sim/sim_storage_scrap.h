#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_STATE_PARKED, UNIT_STATE_EXIT_WAIT
#include "sim/sim_state.h"

namespace mh::sim {

// ---- scrap_home_docked_units's two outward calls. ----
struct storage_scrap_home_calls {
    void (*storage_remove_docked_unit)(uint16_t player, int32_t unit_index,
                                       int32_t storage_slot);    // @0x00489dc4
    void (*unit_teardown)(uint32_t player, uint16_t unit_index); // @0x00487ba5
};

const storage_scrap_home_calls &live_storage_scrap_home_calls();

// ---- scrap_docked_units_of_type's four outward calls. ----
struct storage_scrap_of_type_calls {
    int32_t (*order_queue_find_index)(int32_t player, int32_t unit_idx,
                                      int32_t kind_tag); // @0x00469996
    void (*order_queue_apply_and_dequeue)(uint32_t player, int32_t unit_idx,
                                          int32_t queue_idx); // @0x00469a37
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx,
                               uint32_t mode); // @0x004dac44
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code);                             // @0x004dae0e
    void (*unit_set_state_of)(int32_t player, int32_t unit_index, int16_t state); // @0x004869b0
};

const storage_scrap_of_type_calls &live_storage_scrap_of_type_calls();

// ---- resolve_exit_blockage's two outward calls. ----
struct storage_resolve_exit_blockage_calls {
    void (*unit_queue_advance)(uint32_t player, uint32_t unit_index); // @0x004dbf27
    void (*storage_scrap_docked_units_of_type)(uint32_t player,
                                               int32_t  building_index); // @0x0046cb5a
};

const storage_resolve_exit_blockage_calls &live_storage_resolve_exit_blockage_calls();

namespace detail {

// llm_strat_storage_scrap_home_docked_units @0x0048f89e. See the header banner above.
void storage_scrap_home_docked_units(const sim_view &v, const storage_scrap_home_calls &c,
                                     uint16_t player, int32_t unit_index);

// llm_strat_storage_scrap_docked_units_of_type @0x0046cb5a. See the header banner above -- WRITES
// `unit_storage`/`units` DIRECTLY (own.storage_at/own.unit_at), hence the mutable sim_store param.
void storage_scrap_docked_units_of_type(const sim_view &v, sim_store &own,
                                        const storage_scrap_of_type_calls &c, uint32_t player,
                                        int32_t building_index);

// llm_strat_storage_resolve_exit_blockage @0x00489b40. See the header banner above.
void storage_resolve_exit_blockage(const sim_view &v, const storage_resolve_exit_blockage_calls &c,
                                   uint32_t player, int32_t storage_slot);

} // namespace detail

// Live wrappers: the logic applied to state(). Match the committed prototypes
// (sig_llm_strat_storage_{scrap_home_docked_units,scrap_docked_units_of_type,resolve_exit_blockage}
// in addr/mh_export.gen.h / addr/mh_calls.gen.h) exactly.

void storage_scrap_home_docked_units(uint16_t player, int32_t unit_index);
void storage_scrap_docked_units_of_type(uint32_t player, int32_t building_index);
void storage_resolve_exit_blockage(uint32_t player, int32_t storage_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
