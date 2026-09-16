#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_hangar_recharge_check's callees ---------------------------------------------
struct bldg_state_hangar_recharge_check_calls {
    int32_t (*hangar_any_unit_needs_energy)(int32_t player, int32_t index);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
};

const bldg_state_hangar_recharge_check_calls &live_bldg_state_hangar_recharge_check_calls();

// ---- llm_strat_bldg_state_hangar_recharge_units's callees ----------------------------------------------
struct bldg_state_hangar_recharge_units_calls {
    void (*hangar_recharge_pulse)(uint32_t player, int32_t index);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_hangar_recharge_units_calls &live_bldg_state_hangar_recharge_units_calls();

namespace detail {

// llm_strat_bldg_state_hangar_recharge_check @0x00472b26. No parameters (void(void), committed
// prototype).
void bldg_state_hangar_recharge_check(const sim_view &v, sim_store &own,
                                      const bldg_state_hangar_recharge_check_calls &c);

// llm_strat_bldg_state_hangar_recharge_units @0x00472b9d. No parameters (void(void), committed
// prototype).
void bldg_state_hangar_recharge_units(const sim_view &v, sim_store &own,
                                      const bldg_state_hangar_recharge_units_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// originals' committed void(void) prototypes exactly (addr/mh_export.gen.h's
// sig_llm_strat_bldg_state_hangar_recharge_*).
void bldg_state_hangar_recharge_check();
void bldg_state_hangar_recharge_units();

namespace detail {
} // namespace detail

} // namespace mh::sim
