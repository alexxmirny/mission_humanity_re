//
// sim/sim_bldg_alive.h -- the generic "is this building operational" predicate (RI-SIM / SIM0 pilot).
//
// One function: llm_strat_bldg_is_alive @0x004d3d45 (0x66 bytes), batch B layer 0, 6 callers.
//
// Translated under SIM0 as part of the pilot slice that exercises the state interface. Its purpose
// here is the READ half: it touches `buildings` (a roster the sim writes elsewhere) and writes
// nothing, so it takes a `sim_view` and no `sim_store` at all -- which is what a translation whose
// state footprint is read-only should look like.
//
// NOT ARMED. SIM0 owes a green `net_selftest.exe simtest`, not a rig verdict; the shadow site and
// the evidence tier belong to SIM1B. Nothing here is promoted and the ledger row stays not_started
// until that batch runs it.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_bldg_is_alive @0x004d3d45.
//
// Returns 1 iff the building's energy is positive AND its state is not RUBBLE_SIGHT_DECAY (4),
// else 0. Two guards, in that order, both required.
//
// THE ENERGY TEST IS SPELLED `!(energy <= 0.0)`, NOT `0.0 < energy`, AND THAT IS DELIBERATE. The
// original is FLDZ / FCOMP energy / FNSTSW / SAHF / JNC at 0x004d3d83-0x004d3d8e: it continues when
// the carry flag is SET, which x87 sets both for "0.0 < energy" and for UNORDERED. `0.0 < energy`
// is false on a NaN; `!(energy <= 0.0)` is true on one, matching the original. No reachable state
// puts a NaN in a building's energy, so this is not a live divergence -- it is one line that costs
// nothing and removes the need to argue about it.
int32_t bldg_is_alive(const sim_view &v, int32_t player, int32_t building_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the original's __watcall shape.
int32_t bldg_is_alive(int32_t player, int32_t building_index);

} // namespace mh::sim
