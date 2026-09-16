//
// sim/sim_population_add.h -- population/crew increase (RI-SIM / SIM1F):
//
//   llm_strat_population_add   @0x00491328 (0x15e bytes)
//
// `void __watcall llm_strat_population_add(game_t_Player_s player, int count)`. The rising-housing
// sibling of sim_unit_population_remove.cpp. Updates pop_stats[player] (sim_store::population_at):
//
//   if (count == 0)  // called by check_population_change when housing rose: apply the colony-growth
//                    // curve (0x00491363-0x004913af, the .asm's FP order preserved below):
//        pop_fraction += (double)human * pop_growth_factor * (double)(colony_hp_sum + 1)
//                          / (double)(colony_hp_max_sum + 1);
//   else             // e.g. Unit.human from a docking unit: add the count directly (0x0049134b-):
//        pop_fraction += (double)count;
//   pop_total = trunc_to_int32(pop_fraction);          // trunc-toward-zero then FISTP, see below
//   if (count == 0 && housing_prev < pop_total) {      // clamp to housing on the growth path only
//        pop_total    = housing_prev;
//        pop_fraction = (double)pop_total;
//   }
//   human = (pop_total - workers_employed) - human_in_field;   // recompute idle population
//   if ((uint16_t)player == PlayerSide)                        // local human only
//        game_SetEvent(BUILD_PROJECTS_REFRESH);                // UI refresh, event 7
//
// pop_growth_factor is sim_view::pop_growth_factor (_G_LLM_STRAT_POP_GROWTH_FACTOR @0x005014e4 = 0.1,
// bound this slice). PlayerSide is sim_view::player_side (compared 16-bit, matching `CMP AX,[..]`).
//
// utils_math_trunc @0x004d0596 is the unmarshallable x87-register-only leaf (same as
// sim_unit_population_remove.cpp / sim_dir_headings.cpp): the caller's FLD of pop_fraction, the
// inlined trunc-toward-zero control-word swap + frndint, and the caller's own FISTP under the
// restored round-to-nearest word are reproduced as ONE __asm block (`trunc_to_int32`), identical to
// the population_remove sibling.
//
// PRECISION NOTE: the growth formula is written as plain C `double` arithmetic (the identical
// precedent to sim_unit_population_remove.cpp's decay formula), following the .asm's OPERATION order
// to minimise any x87-80-bit-vs-64-bit low-bit divergence; the clamp/round logic the done_when
// exercises is integer and unaffected.
//
// game_SetEvent stays an ORIGINAL, reached through this file's `_calls` struct (Law 4): it only sets
// a UI dirty flag, so it is a benign idempotent outward call under shadow.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event has no generated C++ enum; a locally-prefixed copy of the one member used, same as
// sim_bldg_flush_cargo_hold.h's BLDG_FLUSH_CARGO_HOLD_BUILD_PROJECTS_REFRESH.
inline constexpr uint32_t POPULATION_ADD_BUILD_PROJECTS_REFRESH = 7u;

// The one outward callee (game_SetEvent), through a `_calls` struct per Law 4.
struct population_add_calls {
    // game_SetEvent @0x00413a52. Return value discarded by the original.
    uint32_t (*set_event)(uint32_t type);
};

const population_add_calls &live_population_add_calls();

namespace detail {

// llm_strat_population_add @0x00491328.
void population_add(const sim_view &v, sim_store &own, const population_add_calls &c, uint16_t player,
                    int32_t count);


} // namespace detail

// Public wrapper. Signature matches the committed prototype in addr/mh_calls.gen.h exactly.
void population_add(uint16_t player, int32_t count);

} // namespace mh::sim
