//
// sim/sim_unit_refund.h -- llm_strat_unit_refund_build_cost_by_health (RI-SIM / SIM1A).
//
// One function: llm_strat_unit_refund_build_cost_by_health @0x0048d706 (0xcf bytes), `void __watcall
// (int player, int unit_index)`. Refunds a fraction of a unit's cfg build cost
// (cfg_unit::resource[7], the (id, val) pairs Unit.SCR's per-type resource_* keywords fill) back to
// the player via llm_resource_add, scaled by how much of the unit's max ENERGY remains -- ENERGY here
// is the HP-like stat (unit hit points), NOT the POWER resource; see
// docs/conventions.md#energy-is-not-power ("ENERGY and POWER
// are two different game concepts" note. Per the existing Ghidra plate (confidence "med", not
// re-derived by this translation): a scrap/disband refund.
//
//   ratio = (units[player][unit_index].energy * 0.5) / cfg_units[proto].energy
//   for each cfg_units[proto].resource[i] (i = 0, 1, 2, ... until .id == 0 or the array is exhausted):
//     llm_resource_add(player, resource[i].id, trunc(resource[i].val * ratio))
//
// UNGUARDED DIVISOR (0x0048d75d-0x0048d766): cfg_units[proto].energy is never checked against 0.0 --
// see the .cpp's refund_amount() for what a 0 divisor produces downstream through the FISTP.
//
// THE LOOP'S ID READ HAPPENS BEFORE ITS BOUND CHECK (0x0048d77f vs. 0x0048d78e/0x0048d792): the
// original tests `resource[i].id != UNDEFINED(0)` FIRST and `i < 7` SECOND, so the READ at index i
// happens unconditionally on every loop-body entry, including i==7 -- one slot past the declared
// 7-entry array, landing on cfg_unit::resource_2[0] (immediately adjacent in the struct, per
// addr/mh_structs.gen.h's own layout comment). Reproduced literally in the .cpp, not clamped; see its
// uncertainties.
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
// sim_unit_update_soldiers.cpp's trunc_axis_delta precedent for an ORDINARY (non-compiler-inlined)
// `CALL utils_math_trunc` -- which is exactly what this function's one call site (0x0048d7ae) is.
struct unit_refund_build_cost_by_health_calls {
    void (*resource_add)(int32_t player, int32_t resource_index,
                         int32_t amount); // llm_resource_add @0x00497f4a
};

const unit_refund_build_cost_by_health_calls &live_unit_refund_build_cost_by_health_calls();

namespace detail {

// llm_strat_unit_refund_build_cost_by_health @0x0048d706. See the header banner above and the .cpp
// for the full per-instruction derivation. No sim-state writes: a pure read over `units`/`cfg_units`
// plus one outward call.
void unit_refund_build_cost_by_health(const sim_view &v, const unit_refund_build_cost_by_health_calls &c,
                                      int32_t player, int32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_refund_build_cost_by_health_calls().
// Matches the original's committed __watcall(player, unit_index) shape.
void unit_refund_build_cost_by_health(int32_t player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
