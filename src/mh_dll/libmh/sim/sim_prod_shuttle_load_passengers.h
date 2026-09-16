#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the one outward call ---------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe
// simtest.
struct prod_shuttle_load_passengers_calls {
    void (*population_remove)(uint32_t player, int32_t count); // llm_strat_population_remove @0x00491486
};

const prod_shuttle_load_passengers_calls &live_prod_shuttle_load_passengers_calls();

namespace detail {

// llm_prod_shuttle_load_passengers @0x0048e5e5. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. Returns the number of colonists
// actually embarked (0 if none).
uint32_t prod_shuttle_load_passengers(const sim_view &v, sim_store &own,
                                      const prod_shuttle_load_passengers_calls &c, uint16_t player,
                                      int32_t building_index, uint32_t cap);

} // namespace detail

// Live wrapper: the logic applied to state() and live_prod_shuttle_load_passengers_calls(). Matches
// the committed prototype (sig_llm_prod_shuttle_load_passengers) exactly.
uint32_t prod_shuttle_load_passengers(uint16_t player, int32_t building_index, uint32_t cap);

namespace detail {
} // namespace detail

} // namespace mh::sim
