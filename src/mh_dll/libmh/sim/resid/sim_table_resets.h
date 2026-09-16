#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call llm_strat_prod_reset_system makes, indirected so detail:: stays testable
// under simtest.
struct prod_reset_system_calls {
    void (*prod_shuttle_slot_release)(int32_t player,
                                      int32_t slot); // llm_strat_prod_shuttle_slot_release @0x0046318d
};

const prod_reset_system_calls &live_prod_reset_system_calls();

namespace detail {

// llm_strat_tech_tables_reset @0x00455b5a. Reads the placeholder name string through `v`, writes the
// three CFG tech tables plus the per-player acquisition state through `own`. No callees (the only
// CALL in the body is the inert Watcom stack probe, translator brief rule 6). void return, matching
// the original.
void tech_tables_reset(const sim_view &v, sim_store &own);

// llm_strat_prod_reset_system @0x0048ffe9. Writes _G_LLM_STRAT_PLAYERS[player].prod_queue_slot[32]
// through `own` for player 0..7, and for each player calls
// llm_strat_prod_shuttle_slot_release(player, slot) for slot 0..9 through `c`. void return.
void prod_reset_system(sim_store &own, const prod_reset_system_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().
void tech_tables_reset();

// Live wrapper: the logic applied to state() and live_prod_reset_system_calls().
void prod_reset_system();

} // namespace mh::sim
