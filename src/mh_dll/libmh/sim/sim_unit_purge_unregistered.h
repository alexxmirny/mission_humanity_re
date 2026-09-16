#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI
#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches -- see the ODR warning above for why this is its own
// uniquely-named struct rather than `mh::sim::calls`.
struct purge_unregistered_calls {
    int32_t (*unit_state_is_boarding)(int32_t state);
};

const purge_unregistered_calls &live_purge_unregistered_calls();

// The logic over an EXPLICIT state + calls table, so `net_selftest.exe simtest` can drive it over heap
// buffers with no game and no rig. The wrapper below is this applied to state() +
// live_purge_unregistered_calls(); the split costs one inlined call.
namespace detail {

// llm_strat_unit_purge_unregistered @0x00499bd0. See the header derivation above.
void unit_purge_unregistered(const sim_view &v, sim_store &own, const purge_unregistered_calls &gc,
                             uint32_t player);

} // namespace detail

// Live wrapper: the logic applied to state() + live_purge_unregistered_calls(). Matches the
// original's committed __watcall(EAX) shape (sig_llm_strat_unit_purge_unregistered).
void unit_purge_unregistered(uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
