//
// sim/sim_population_change.h -- llm_strat_check_population_change, the per-player population/housing
// balancer (RI-SIM / SIM1F).
//
// One function: llm_strat_check_population_change @0x0043fe1e (0x7f bytes). Compares the player's
// current displayed population (pop_stats.pop_total) against the housing capacity latched at step
// start (pop_stats.housing_prev): if population is below capacity, grows it via
// llm_strat_population_add(player, 0); if population is above capacity, shrinks it via
// llm_strat_population_remove(player, 0). Equal -> no call at all. Read-only over sim state itself --
// all population-count mutation happens inside the two callees, which this function only invokes.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
// Both members mirror the ORIGINAL functions' own committed prototypes (addr/mh_calls.gen.h) --
// population_add's player is uint16_t there because that is genuinely the address-0x00491328
// function's own parameter width, not a choice made here.
struct population_change_calls {
    void (*population_add)(uint16_t player, int32_t count);    // llm_strat_population_add @0x00491328
    void (*population_remove)(uint32_t player, int32_t count); // llm_strat_population_remove @0x00491486
};

const population_change_calls &live_population_change_calls();

namespace detail {

// llm_strat_check_population_change @0x0043fe1e.
void check_population_change(const sim_view &v, sim_store &own, const population_change_calls &c,
                             uint32_t player);

} // namespace detail

// Live wrapper: the logic applied to state() and live_population_change_calls(). Matches the
// original's committed __watcall(EAX) shape.
void check_population_change(uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
