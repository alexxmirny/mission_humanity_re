#pragma once
#include <cstdint>

namespace mh::sim {

// The two endpoints of the "boarding/loading-related" llm_strat_unit_state range this predicate
// tests (see the header banner on why these are local constants rather than enum members).
inline constexpr int32_t UNIT_STATE_BOARDING_RANGE_LO = 0x1f;
inline constexpr int32_t UNIT_STATE_BOARDING_RANGE_HI = 0x2b;

// The logic over an explicit state code, matching sim_facing24_from_points.h's shape -- no
// sim_view/sim_store parameter at all, because this function reads no game state.
namespace detail {

// llm_unit_state_is_boarding @0x004967ce. Returns 1 iff
// UNIT_STATE_BOARDING_RANGE_LO <= state <= UNIT_STATE_BOARDING_RANGE_HI, else 0.
int32_t unit_state_is_boarding(int32_t state);

} // namespace detail

// Live wrapper: the logic applied directly (there is no state to bind). Matches the committed
// __watcall(EAX) shape (sig_llm_unit_state_is_boarding / mh_calls.gen.h's own prototype).
int32_t unit_state_is_boarding(int32_t state);

namespace detail {
} // namespace detail

} // namespace mh::sim
