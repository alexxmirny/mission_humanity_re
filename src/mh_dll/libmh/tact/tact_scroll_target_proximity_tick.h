//
// tact/tact_scroll_target_proximity_tick.h -- TACT1E: the scroll target's proximity-to-blast-marker
// energy-pct decay.
//
//   llm_tact_scroll_target_proximity_tick @0x00439005 (0xfe)
//
// Pure data function -- ZERO outward calls of its own (no calls-struct needed at all). It derives
// two nested, axis-asymmetric bounding boxes around the scroll target tile
// (_G_LLM_TACT_TARGET_TILE_COL/ROW, read here via tact_store's own accessors -- there is no
// tact_view binding for this pair) and tests the mine-blast marker tile
// (_G_LLM_TACT_BLAST_MARKER_COL/ROW, likewise store-only) against both:
//
//   hard box:  col in [target_col-5,  target_col+5],  row in [target_row-7,  target_row+7]
//   soft box:  col in [target_col-10, target_col+10], row in [target_row-12, target_row+12]
//
// The margins are NOT symmetric between col and row (5/10 for col, 7/12 for row) -- read directly
// off the four SUB/ADD immediate pairs @0x00439022-0x00439072, not inferred.
//
// THREE MUTUALLY EXCLUSIVE OUTCOMES, only two of which touch _G_LLM_SQUAD_BB_TARGET_ENERGY_PCT
// (the squad-assault blackboard's target-energy-percent field; tact_store::squad_bb_target_energy_pct()
// is the write path -- tact_view::squad_bb_target_energy_pct stays read-only everywhere else it is touched):
//
//   1. blast marker inside the HARD box            (@0x004390a3)             -> energy_pct := 0
//   2. blast marker inside the SOFT ring, not hard  (@0x004390dd-0x004390f9) -> if energy_pct < 0x32
//      (50) then energy_pct := 0, else energy_pct -= 0x32
//   3. blast marker outside BOTH boxes              (@0x004390db/0x004390cf/0x004390c3->0x004390f9)
//      -> NO WRITE AT ALL -- the easiest branch to get wrong, since it is a third exit distinct
//      from case 1's early "inside hard" exit and from case 2's threshold split.
//
// All eight comparisons are SIGNED (JL/JGE/JLE throughout) on plain int32_t coordinates -- no
// truncation or width games anywhere in this body.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_scroll_target_proximity_tick @0x00439005. See the file banner for the full three-way
// outcome table; this signature takes only `own` because every global this body touches
// (_G_LLM_TACT_TARGET_TILE_COL/ROW, _G_LLM_TACT_BLAST_MARKER_COL/ROW,
// _G_LLM_SQUAD_BB_TARGET_ENERGY_PCT) is bound store-side only -- there is no tact_view member for
// any of them, so threading an unused `const tact_view &` would be false ceremony (tact_pilot.h's
// units_reset_hp_for_active is the precedent for an own-only detail signature).
void scroll_target_proximity_tick(tact_store &own);

} // namespace detail

void scroll_target_proximity_tick();

// Declared here per the module convention; DEFINED in tact_scroll_target_proximity_tick.cpp,
// CALLED from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
