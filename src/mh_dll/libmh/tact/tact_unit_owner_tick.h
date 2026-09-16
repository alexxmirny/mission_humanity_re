//
// tact/tact_unit_owner_tick.h -- TACT1B: the per-side AI unit-behaviour dispatcher.
//
//   llm_tact_unit_owner_tick @0x004335a0 (0x658)
//
// Called once per side per frame with `owner` (the side/faction id). Rebuilds the occupancy layer
// for that side's map half, then walks every unit slot [1, 0x80] belonging to that owner and
// dispatches on its `def_stat` ("defense stance": 0 none, 1 GUARD1, 2 GUARD2, 3 ATTACK, 4 SNIPER --
// see the tactical-probe work). Cross-checked against that doc's own table (built independently,
// 2026-08-24) before writing this derivation; the two agree on every def_stat's high-level shape.
//
// THE JUMP TABLE HAS 5 ENTRIES FOR 4 CODE BODIES (read via read-memory @0x00433682, 5*4 bytes):
// [0]=no-op, [1]=caseD_1, [2]=caseD_2, [3]=caseD_3, [4]=caseD_1 AGAIN -- def_stat 1 (GUARD1) and 4
// (SNIPER) run the IDENTICAL body. Preserved as a literal switch over 0..4, not simplified to 0..3.
//
// THE THREE CASE BODIES ARE NOT THREE VARIANTS OF ONE SHAPE -- read each on its own:
//   caseD_1: fire path ONLY (no cell2 fallback). UNIQUELY has a discarded tile_objects.building
//            read (@0x0043370f-0x00433724, computed and stored but never read again -- omitted
//            below, a pure dead load with no side effect) and a facing-ALIGNMENT gate: only fires
//            if |facing_dir - dir24(unit -> far cell)| < 2, i.e. the unit is already turned toward
//            the target. If that gate or the far-cell/weapon-range gate fails, the case ends with
//            no action at all.
//   caseD_2: fire path (SAME kneel/attack decision, but NO facing gate and NO dead read) PLUS, only
//            when the far-cell probe itself found nothing, a cell2 fallback: turn to face cell2's
//            approach direction, gated by the queue-busy check shared with the loop's own prelude
//            gate. Either way the case ends there -- no wander.
//   caseD_3: the SAME fire path and the SAME cell2 fallback as caseD_2, BUT when the fallback's own
//            gate is not satisfiable (no valid cell2, or the queue is busy), it falls through into
//            the WANDER block instead of ending -- an idle-AI re-roll gated by
//            _G_LLM_TACT_UNIT_WANDER_RETRY_INTERVAL (2.0s) that picks a random octant via llm_rand,
//            checks passable[], and issues a FACE order toward a passable neighbour tile.
// This asymmetry is why fire_or_kneel/try_face_cell2 below are shared helpers used differently by
// each case, not one templated "the def_stat body".
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_owner_tick @0x004335a0.
//
//  0. @0x004335bb-0x004335d4: rebuild the occupancy layer for THIS owner's map half --
//     llm_tact_tile_rebuild_occupancy_layer_for_map(1) when owner==0, else (0) (frontier, Law 4).
//  1. @0x004335db-0x004335ef: walk unit slots [1, 0x80].
//  2. @0x004335f1-0x00433614: skip a slot whose owner != the `owner` param, or whose type <= 0
//     (empty/dead).
//  3. @0x00433619-0x0043367c: ABORT THE WHOLE FUNCTION (not skip -- 0x0043367c jumps to the
//     epilogue at 0x00433c03, where every per-unit skip jumps to the loop increment at 0x00433bfe)
//     when status bit 8 (FIRE) is set, OR when the CURRENT cmd_index slot's op != 0 AND
//     interrupt_flag == 0 AND move_retry_wait == 0 -- the "has an uninterruptible live command and
//     is not mid-retry" gate (llm_tact_unit_cmd_entry.interrupt_flag's own field doc documents the
//     site). So the first busy/firing same-owner unit ends def_stat AI for ALL higher slots that
//     frame. (Read as a skip until 2026-09-02 -- the TACT1-P POZ3 red; see the .cpp.)
//  4. @0x00433696-0x004336aa: skip if def_stat > 4.
//  5. @0x004336b7: dispatch on def_stat via the 5-entry table above.
void unit_owner_tick(const tact_view &v, tact_store &own, uint32_t owner);

} // namespace detail

void unit_owner_tick(uint32_t owner);


} // namespace mh::tact
