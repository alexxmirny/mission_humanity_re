#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_power_primary_check's callees ---------------------------------------------
struct bldg_state_power_primary_check_calls {
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_power_primary_check_calls &live_bldg_state_power_primary_check_calls();

namespace detail {

// llm_strat_bldg_state_power_primary_check @0x00474707. No parameters (void(void), committed
// prototype).
void bldg_state_power_primary_check(const sim_view &v, sim_store &own,
                                    const bldg_state_power_primary_check_calls &c);

// llm_strat_bldg_state_power_generate @0x0047477a. No parameters (void(void), committed prototype).
// No callees, so no `_calls` table is needed for this TU (sim_game_update_resource_stats.h precedent).
void bldg_state_power_generate(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state() and (for primary_check) its own live_*_calls(). Match
// the originals' committed void(void) prototypes exactly (addr/mh_export.gen.h's
// sig_llm_strat_bldg_state_power_*).
void bldg_state_power_primary_check();
void bldg_state_power_generate();

namespace detail {
} // namespace detail

} // namespace mh::sim
