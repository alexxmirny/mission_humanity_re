#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// game_SpendResource is an ORIGINAL effectful callee outside this batch -- see the DECLARED NEED above
// for its write closure.
struct unit_try_pay_action_cost_calls {
    void (*game_SpendResource)(int32_t player, int32_t res_id, int32_t amount); // @0x00497f94
};

const unit_try_pay_action_cost_calls &live_unit_try_pay_action_cost_calls();

namespace detail {

// llm_strat_unit_try_pay_action_cost @0x00493254. See the header banner above for the full
// derivation. Needs no `own` -- this function's own body writes nothing; every write is delegated to
// the ORIGINAL game_SpendResource callee, which manages its own state access.
int32_t unit_try_pay_action_cost(const sim_view &v, const unit_try_pay_action_cost_calls &c,
                                 uint32_t player, int32_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_try_pay_action_cost_calls(). Matches
// the committed prototype (sig_llm_strat_unit_try_pay_action_cost) exactly.
int32_t unit_try_pay_action_cost(uint32_t player, int32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
