#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes, indirected via mh::call:: for testability -- same shape
// as every other sim/ TU. See the header banner above for why this is NOT a direct call to the
// sibling's public mh::sim:: wrapper.
struct prod_unbind_planet_calls {
    void (*prod_deliver_arrivals)(); // llm_strat_prod_deliver_arrivals @0x0048dc65
};

const prod_unbind_planet_calls &live_prod_unbind_planet_calls();

namespace detail {

// llm_strat_prod_unbind_planet @0x0048ff73. See the header banner above for the full derivation; the
// .cpp carries the per-step address citation.
void prod_unbind_planet(sim_store &own, const prod_unbind_planet_calls &c, int32_t player,
                        int32_t planet_slot);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (addr/mh_calls.gen.h's llm_strat_prod_unbind_planet trampoline signature) exactly.
void prod_unbind_planet(int32_t player, int32_t planet_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
