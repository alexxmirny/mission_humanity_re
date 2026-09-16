//
// sim/sim_resource_decay.h -- llm_strat_decay_excess_resources (RI-SIM / SIM1F).
//
// One function: llm_strat_decay_excess_resources @0x00491604 (0x85 bytes), `bool __watcall
// (int player, int res)`. Storage-overflow decay: when a player's held amount of a resource exceeds
// the capacity latched at step start (`_G_LLM_STRAT_STORAGE_STATS[player].cap_prev[res]`), a fixed
// fraction of the excess (`_G_LLM_STRAT_RESOURCE_DECAY_RATE`, 0.1) is spent away via
// `game_SpendResource` -- resources parked above storage capacity rot. Returns whether anything was
// spent (amount > 0).
//
//   diff   = player_resources[player][res] - storage_stats[player].cap_prev[res]     (int32 subtract)
//   amount = trunc((double)diff * resource_decay_rate)     (x87, toward zero -- see the .cpp)
//   if (amount > 0) { game_SpendResource(player, res, amount); return true; }
//   return false;
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
//
// utils_math_trunc is deliberately NOT a member here -- it is
// `MH_UNAVAILABLE__parameter_storage_not_marshallable` in mh_calls.gen.h (an x87-register-only leaf,
// ST0 in / ST0 out, no stack-passable signature) and is reproduced inline in the .cpp instead, matching
// sim_unit_refund.cpp's refund_amount() / sim_unit_update_soldiers.cpp's trunc_axis_delta precedent for
// an ORDINARY (non-compiler-inlined) `CALL utils_math_trunc` -- which is exactly what this function's
// one call site (0x00491651) is.
//
// llm_strat_decay_excess_resources is NOT bound here as a member of its own calls struct -- it IS this
// function; mh_calls.gen.h's own binding of the same name/address exists for OTHER callers elsewhere in
// the codebase and is unrelated to this translation.
struct resource_decay_calls {
    void (*spend_resource)(int32_t player, int32_t resource_id,
                           int32_t amount); // game_SpendResource @0x00497f94
};

const resource_decay_calls &live_resource_decay_calls();

namespace detail {

// llm_strat_decay_excess_resources @0x00491604. See the header banner above and the .cpp for the
// full per-instruction derivation. No sim-state writes of its own: a pure read over
// `player_resources`/`storage_stats` plus one outward call (which itself writes state, but that write
// belongs to game_SpendResource's own closure, not this function's).
bool decay_excess_resources(const sim_view &v, const resource_decay_calls &c, int32_t player,
                            int32_t res);

namespace rebind_arm {
uint8_t decay_excess_resources(int32_t player, int32_t res);
} // namespace rebind_arm

} // namespace detail

// Live wrapper: the logic applied to state() and live_resource_decay_calls(). Matches the original's
// committed __watcall(EAX=player, EDX=res) shape.
bool decay_excess_resources(int32_t player, int32_t res);


} // namespace mh::sim
