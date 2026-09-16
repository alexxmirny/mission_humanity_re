#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by the offline harness.
struct power_recompute_calls {
    // llm_strat_refresh_all_buildings @0x00470b56. Applies the freshly-clamped ratio per building
    // (operational flag +0x17 / efficiency +0x29, per the plate) -- a real sim/AI sibling
    // migration-set function, called via its original address (translator brief rule 3).
    void (*refresh_all_buildings)(uint32_t player);
};

const power_recompute_calls &live_power_recompute_calls();

namespace detail {

// llm_strat_power_recompute @0x00491260. See the header derivation above for the full x87 read.
void power_recompute(sim_store &own, const power_recompute_calls &c, uint16_t player);

} // namespace detail

// Live wrapper: the logic applied to state().own and live_power_recompute_calls(). Matches the
// original's committed __watcall(AX) shape (sig_llm_strat_power_recompute in mh_export.gen.h).
void power_recompute(uint16_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
