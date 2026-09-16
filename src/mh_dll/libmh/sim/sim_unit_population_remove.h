#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event member 7 (the strategic-sim notes table) -- see the DECLARED NEED above on why this is
// a local constant rather than a shared enum member.
inline constexpr uint32_t EVENT_BUILD_PROJECTS_REFRESH = 7;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
//
// utils_math_trunc is deliberately NOT a member here -- it is
// `MH_UNAVAILABLE__parameter_storage_not_marshallable` in mh_calls.gen.h (an x87-register-only leaf,
// ST0 in / ST0 out, no stack-passable signature) and is reproduced inline in the .cpp instead, matching
// sim_unit_refund.cpp's refund_amount() / sim_unit_update_soldiers.cpp's trunc_axis_delta precedent
// for an ORDINARY (non-compiler-inlined) `CALL utils_math_trunc` -- which is exactly what this
// function's one call site (0x00491507) is.
struct population_remove_calls {
    void (*population_layoff_workers)(int32_t player,
                                      int32_t worker_count); // llm_strat_population_layoff_workers @0x00491689
    uint32_t (*set_event)(uint32_t type);                    // game_SetEvent @0x00413a52
};

const population_remove_calls &live_population_remove_calls();

namespace detail {

// llm_strat_population_remove @0x00491486. See the header banner above and the .cpp for the full
// per-instruction derivation. Writes ONLY _G_LLM_STRAT_POP_STATS[player] (via sim_store::
// population_at(), already bound by sim_unit_on_destroyed.cpp) -- no roster write.
void population_remove(const sim_view &v, sim_store &own, const population_remove_calls &c,
                       uint32_t player, int32_t count);

} // namespace detail

// Live wrapper: the logic applied to state() and live_population_remove_calls(). Matches the
// original's committed __watcall(EAX,EDX) shape (sig_llm_strat_population_remove).
void population_remove(uint32_t player, int32_t count);

namespace detail {
} // namespace detail

} // namespace mh::sim
