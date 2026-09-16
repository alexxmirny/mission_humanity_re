//
// tact/tact_teleport_zone.h -- TACT1D: the per-frame teleport-zone scanner.
//
//   llm_tact_teleport_zone_scan_tick @0x00432ba1 (0x24f)
//   void __watcall llm_tact_teleport_zone_scan_tick(void)
//
// TWO UNRELATED HALVES IN ONE BODY:
//
//   HALF 1 (@0x00432bb9-0x00432cf3): scans _G_LLM_TACT_TELEPORT_TABLE[1..0x3f] -- slot 0 is never
//   touched, same off-by-one every other teleport_zone_at loop in this domain preserves. Each
//   ACTIVE zone (`.id != 0`) has its own 2x2 origin block -- (start_col,start_row),
//   (start_col+1,start_row), (start_col,start_row+1), (start_col+1,start_row+1) -- probed for an
//   occupant via `tile_objects[...].building` (the SAME field this domain's teleport code already
//   reads as "occupied, by a building OR a unit", per tact_unit_teleport.h). THIS IS A CASCADE, NOT
//   four independent checks: the first corner with a nonzero `.building` teleports THAT value
//   (`llm_tact_unit_teleport(zone_index, building_value)`, the established teleport_id/unit_id
//   argument order per tact_teleport_cmdqueue_jump.cpp) and the zone's other three corners are never
//   probed this tick. Only if ALL FOUR corners come up empty does the zone's `field_28` "consumed"
//   latch (set by tact_unit_teleport.cpp's LINKED-zone success path) get cleared back to 0
//   (@0x00432ce3) -- so a zone that has just delivered someone stays flagged for as long as a unit
//   keeps standing in its origin block, and only opens back up once the block is genuinely empty.
//   NOTE: unlike the despawn half below, this cascade has NO upper-bound check on the read value
//   (only `> 0`) -- confirmed against the raw bytes (@0x00432c0e/0x432c4c/0x432c8c/0x432ccd are each
//   a single `CMP ...,0 / JLE`, no second compare), so it is not a "range-checked unit index" read
//   the way the despawn half's is.
//
//   HALF 2 (@0x00432cf3-0x00432de6): UNRELATED to teleport zones. Gated on
//   `_G_LLM_TACT_QUIT_TILE_COL + _G_LLM_TACT_QUIT_TILE_ROW > 0` AND the mine-blast window still
//   being open (`time_GetCurrentTime() < _G_LLM_TACT_MINE_BLAST_TIME_END`, the same deadline
//   tact_unit_enqueue_command.cpp's op==9 gate and tact_unit_mine_arm_tick.cpp's writer share).
//   While open, despawns up to 4 units standing on the quit-tile's OWN 2x2 block --
//   (quit_col,quit_row), (quit_col,quit_row+1), (quit_col+1,quit_row), (quit_col+1,quit_row+1) --
//   via the already-translated `llm_tact_unit_despawn`. These FOUR checks ARE independent: each
//   reads its own corner and calls despawn on its own account (no early-exit cascade like Half 1),
//   guarded `0 < building_value < 0x20` (a real unit-index range check the scan half lacks).
//
// THE +0xd1ec8a OFFSET IN BOTH HALVES IS `.building` OF THE ROW-ADJACENT TILE, NOT `.unit[1]`:
// every "second corner" read in this function (row+1 at a fixed col) is coded as the SAME computed
// tile address as the row's own read, plus a flat +8 (one whole `mh_map_tile_object_data` record,
// not a field within it) -- landing on offset 0xa from the array base, which is field-offset 2
// (`.building`) of element (index+1). Since this codebase's tile grid is indexed `(col<<8)|row`
// (column-major, row is the low/contiguous index), index+1 at a fixed col is exactly (col,row+1).
// `.unit[0..1]` actually lives at offsets 4..5 of the SAME element and is never touched here. This
// resolves to a plain `tile_at(tv, col, row + 1).building` / `tile_object_at(col, row + 1).building`
// call in the translation -- behaviourally identical to the raw +8 trick, without a byte offset.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The three outward calls this function's body makes, indirected for offline testability -- same
// shape as unit_teleport_calls / unit_despawn_calls.
struct teleport_zone_scan_tick_calls {
    void (*unit_teleport)(int32_t teleport_id, int32_t unit_id); // llm_tact_unit_teleport @0x00432df0
    void (*unit_despawn)(int32_t unit_idx);                      // llm_tact_unit_despawn @0x00432048
    double (*time_now)();                                        // time_GetCurrentTime @0x00427616
};

const teleport_zone_scan_tick_calls &live_teleport_zone_scan_tick_calls();

namespace detail {

// llm_tact_teleport_zone_scan_tick @0x00432ba1. See the header banner for the full derivation.
// Takes `own` (not merely `read`) because Half 1 read-modify-writes `field_28`, and because
// `_G_LLM_TACT_QUIT_TILE_COL`/`_ROW` have only a STORE binding in tact_state.h today (mission_load's
// write path; nothing yet reads them back through the view) -- this function reads them through
// `own`'s existing accessors without writing them.
void teleport_zone_scan_tick(const tact_view &tv, tact_store &own,
                             const teleport_zone_scan_tick_calls &c);

} // namespace detail

void teleport_zone_scan_tick();


} // namespace mh::tact
