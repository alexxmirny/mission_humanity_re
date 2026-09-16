#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls this function makes, ALL original callees -- see the header banner on why
// this is an indirected struct rather than a direct `mh::call::` inside `detail::` (same reasoning as
// every other module here: a direct call would be untestable by net_selftest.exe simtest / the
// offline fixture), and on why `housing_count_remove` is bound here rather than through sibling file
// sim_unit_housing_count.h's own `mh::sim::detail::unit_housing_count_remove`, even though that
// sibling has already landed.
struct apply_production_completion_calls {
    void (*resource_add)(int32_t player, int32_t resource_id, int32_t amount); // llm_resource_add @0x00497f4a
    void (*population_add)(uint16_t player, int32_t count);                    // llm_strat_population_add @0x00491328
    void (*housing_count_remove)(int32_t player,
                                 int32_t unit_proto_id); // llm_strat_unit_housing_count_remove @0x00497842
};

const apply_production_completion_calls &live_apply_production_completion_calls();

namespace detail {

// llm_unit_apply_production_completion @0x00492bf1. See the header banner for the full derivation.
// WRITES `units[player][0].order` (the header-row decrement) through `own`.
void unit_apply_production_completion(const sim_view &v, sim_store &own,
                                      const apply_production_completion_calls &c, uint32_t player,
                                      int32_t unit_proto_id);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_unit_apply_production_completion) exactly.
void unit_apply_production_completion(uint32_t player, int32_t unit_proto_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
