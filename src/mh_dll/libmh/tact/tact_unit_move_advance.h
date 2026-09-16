//
// tact/tact_unit_move_advance.h -- TACT1B: advance one unit's per-tick move-progress counter, and
// on completing a tile step, commit the position and consume the path RLE entry.
//
//   llm_tact_unit_move_advance @0x00430f5a (0x20a)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_move_advance @0x00430f5a. Params 2 (EDX) and 3 (EBX) are read into locals and
// compared against pos_col/pos_row -- but the comparison RESULT is never consumed (no Jcc follows;
// @0x00430f90, @0x00430fa1), and the locals are then UNCONDITIONALLY overwritten with pos_col/
// pos_row (@0x00430fab-0x00430fc3) before either is read again. A Watcom debug-assert idiom with
// the branch stripped, not a translation gap -- both params are dead and the committed prototype
// already marks them unused; this is why no C++ parameter for them exists here.
//
// 1. col/row = pos_col/pos_row (the two dead params are never actually consulted, see above).
// 2. @0x00430fcd-0x00430ffe: only when `progress == 0` (a freshly-started step, not a step already
//    in flight): mark passable[col][row] = PASSABLE_DEFAULT (2, the tile being vacated) and
//    passable[col+delta_col][row+delta_row] = PASSABLE_BLOCKED (0, the tile being claimed) --
//    state/mode_planes.h's own comment cites this exact function as the source of that reading.
// 3. @0x00430ffe-0x00431019: progress++; if progress <= 0x1f (31), STOP -- the step is still
//    animating, no position/path/anim-state change yet.
// 4. @0x0043101f-0x00431033: llm_tact_unit_vision_remove(unit_idx) (the type-field read at
//    0x00431026 is stored to a local nothing else reads -- dead, not replicated).
// 5. @0x00431038-0x004310b3: decrement the CURRENT path_waypoint's run_length (owner 0, slot
//    move_path_slot, entry move_path_step); if it reaches 0, advance move_path_step.
// 6. @0x004310b3-0x004310c1: clear tile_objects[col][row].building (the OLD tile) -- tactical
//    mode's occupancy stamp reuses the strategic `building` id field to mark "a unit stands here"
//    (see tact_unit_move_tick.h step 12, which reads this same field as an obstacle gate).
// 7. @0x004310ca-0x004310e7: col/row += delta_col/delta_row; write
//    tile_objects[new_col][new_row].building = unit_idx (the NEW tile, claimed).
// 8. @0x004310ee-0x0043111c: commit pos_col/pos_row = new col/row (truncated to the field's byte
//    width, exactly as the original's byte store does); progress = 0.
// 9. @0x0043111c-0x00431155: anim_state==0 -> set_anim_state(1); anim_state==1 -> set_anim_state(0);
//    any other value -> no call (a 2-state walk-cycle toggle, not a wraparound).
// 10. @0x00431155-0x0043115d: llm_tact_unit_vision_add(unit_idx).
void unit_move_advance(tact_store &own, int32_t unit_idx, int32_t delta_col, int32_t delta_row);

} // namespace detail

void unit_move_advance(int32_t unit_idx, int32_t delta_col, int32_t delta_row);

// DECLARED HERE so another TU's rebind can name it (TACT1-P C4, 2026-09-04). The ORIGINAL's ABI
// carries two DEAD arguments in EDX/EBX that the wrapper above drops; the generated dispatcher
// marshals them, so a three-argument target would consume the caller's registers wrongly.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {
void unit_move_advance(int32_t unit_idx, uint32_t arg_edx_unused, uint32_t arg_ebx_unused,
                       int32_t delta_col, int32_t delta_row);
} // namespace rebind_arm


} // namespace mh::tact
