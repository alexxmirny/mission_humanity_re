//
// sim/sim_resource_spend.h -- the two resource-expenditure tallies (RI-SIM / SIM0 pilot).
//
//   llm_strat_econ_track_unit_resource_spend         @0x004e215f (0x85)  batch A layer 0
//   llm_strat_bldg_record_resource_expenditure_stats @0x004e685b (0x82)  batch B layer 0
//
// THE PILOT'S BOTH-HALVES CASE: each reads a CFG table through `sim_view` (Unit[] / Building[],
// regions the sim must never write -- W1) and writes `player_data` through `sim_store` (W2). One
// function, one of each, which is the shape most of the 138 roster-writing members will have.
//
// THE TWO ARE THE SAME LOOP OVER DIFFERENT CFG TABLES. They are kept as two bodies sharing one
// helper rather than one function with a table pointer, because that is exactly what they are:
// their listings differ only in the cfg base and the record stride, and the building one does not
// even have its own epilogue -- it JMPs into the unit one's at 0x004e21de (0x004e68d8). Watcom tail-
// merged them, which is as strong a statement as one gets that the bodies are identical past the
// table.
//
// TWO THINGS TO KEEP, AND THE SECOND IS THE ONE THAT WILL BITE A LATER TRANSLATION:
//
//   THE WALK IS BOUNDED AT SEVEN AND TERMINATES ON `.id == 0`. Seven, not four -- the cfg parser
//   only fills four because the section grammar exposes four keywords, and entries 4..6 read back
//   zero, which is what stops the walk. Both the bound (CMP ECX,0x7 @0x004e21d9 / 0x004e68d3) and
//   the early exit (JZ @0x004e2190 / 0x004e6886) are the original's; do not replace the pair with a
//   fixed four.
//
//   THE SPEND RING IS ADDRESSED FROM A BASE FOUR BYTES BEFORE THE ARRAY. The originals compute
//   `cursor * 0x10 + id * 4` off a displacement of +0x102a4, and `ai_spend_rate_numer_ring` starts
//   at +0x102a8 -- the bias exists because resource ids are ONE-based and the array is not. So the
//   element written is `[cursor * 4 + id - 1]`. Index it as `[cursor * 4 + id]` and every resource
//   lands one column high, silently, in a field with no other writer to disagree with.
//
// NOT ARMED under SIM0 (see sim_bldg_alive.h); the shadow sites belong to SIM1A / SIM1B.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// The shared body: walk `costs[0 .. CFG_RESOURCE_SLOTS)` and accumulate each (id, val) pair into the
// player's lifetime total and into the current spend-ring slot. Stops at the first `.id == 0`.
void accumulate_resource_spend(sim_store &own, uint32_t player, const cfg_resource *costs);

// llm_strat_econ_track_unit_resource_spend @0x004e215f -- the cfg UNIT type's build cost.
void econ_track_unit_resource_spend(const sim_view &v, sim_store &own, int32_t player_idx,
                                    int32_t unit_idx);

// llm_strat_bldg_record_resource_expenditure_stats @0x004e685b -- the cfg BUILDING type's cost.
void bldg_record_resource_expenditure_stats(const sim_view &v, sim_store &own, int32_t player_id,
                                            int32_t building_id);

} // namespace detail

// Live wrappers: the logic applied to state(). Signatures match the originals' __watcall shape.
void econ_track_unit_resource_spend(int32_t player_idx, int32_t unit_idx);
void bldg_record_resource_expenditure_stats(int32_t player_id, int32_t building_id);

} // namespace mh::sim
