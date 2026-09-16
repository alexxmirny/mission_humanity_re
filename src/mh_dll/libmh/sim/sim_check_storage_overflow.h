#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (llm_strat_decay_excess_resources @0x00491604,
// already committed in mh_calls.gen.h -- not part of this migration slice, stays original),
// indirected for offline testability like every other sim/ TU's `_calls` struct.
struct check_storage_overflow_calls {
    // llm_strat_decay_excess_resources @0x00491604. EAX=player (int32_t), EDX=res (int32_t) --
    // matches mh_calls.gen.h's own signature for this callee exactly. Return value unused by the
    // original caller; kept uint8_t here rather than narrowed to void so the live binding assigns
    // directly from mh::call:: without a wrapper.
    uint8_t (*decay_excess_resources)(int32_t player, int32_t res);
};

const check_storage_overflow_calls &live_check_storage_overflow_calls();

namespace detail {

// llm_strat_check_storage_overflow @0x0043fe9d. See the header banner above.
void check_storage_overflow(const sim_view &v, const check_storage_overflow_calls &c, int32_t player);

} // namespace detail

// Live wrapper: the logic applied to state().read / live_check_storage_overflow_calls(). Matches
// the committed prototype (sig_llm_strat_check_storage_overflow) exactly.
void check_storage_overflow(int32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
