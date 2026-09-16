//
// tact/tact_unit_mine_arm_tick.h -- TACT1C: the per-tick progress driver for a unit executing
// command-queue op 9 (mine-arm).
//
//   llm_tact_unit_mine_arm_tick @0x0042f935 (0x176)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_mine_arm_tick @0x0042f935.
//
// 1. @0x0042f952-0x0042f96a: snapshot `type` (the character-class index, for the mine_time add at
//    the end); increment `progress`.
// 2. @0x0042f970-0x0042f9bb: set anim_state=4 (a "progress anim", per the field's own comment).
//    If progress (post-increment) is now > 0x3f (unsigned byte compare), the arming animation has
//    completed one full cycle: reset progress=0, anim_state=0, then dequeue this command
//    (unit_cmd_advance(unit_idx, cmd_index) -- cmd_index read BEFORE cmd_advance mutates it, since
//    cmd_advance's own job is to advance/clear that index).
// 3. @0x0042f9bb-0x0042fa30: UNCONDITIONALLY (not gated on step 2's branch), if progress <= 0x1f,
//    skip the blast-marker update below entirely (JA taken -> continue; JBE -> jump past). So the
//    marker only updates while progress is in (0x1f, 0xff] -- i.e. every tick after the SECOND
//    quarter of the arm animation, including every tick after a reset-to-0-then-immediately-
//    incremented-past-0x1f cycle on a later call.
// 4. @0x0042fa30-0x0042fa43: stamp MINE_BLAST_TIME_END = time_GetCurrentTime() + MINE_BLAST_DURATION
//    (an unconditional OVERWRITE every time this arm runs, not an accumulator -- each tick pushes
//    the deadline further out while the unit keeps arming).
// 5. @0x0042fa43-0x0042fa85: BLAST_MARKER_COL/ROW = this unit's pos_col/pos_row (re-latched every
//    tick this branch runs); then facing = facing_dir - 1, and if (uint8_t)facing > 0x17 (i.e.
//    facing_dir is out of its normal 1..0x18 range), skip the switch below (falls to step 6 with
//    the marker left at pos_col/pos_row, no nudge).
// 6. @0x0042fa88-0x0042fae5: a 24-way jump table keyed on `facing` (0..0x17), read RAW from
//    0x0042f9d0 (24 x 4-byte pointers) rather than assumed from the case labels alone -- the first
//    translation of this got it wrong exactly the way the read-only warning predicts: EVERY index
//    resolves to one of 8 targets (the case labels name only the FIRST index that reaches each
//    target; Ghidra does not print the repeats), grouped as 8 near-equal octant bands, not "8
//    active indices, 16 no-ops":
//      0,1,0x17(23)      -> row++            (band centred on facing=0, wraps across the 0/23 seam)
//      2,3,4              -> col--, row++
//      5,6,7               -> col--
//      8,9,0xa(10)         -> col--, row--
//      0xb(11),0xc(12),0xd(13) -> row--
//      0xe(14),0xf(15),0x10(16) -> col++, row--
//      0x11(17),0x12(18),0x13(19) -> col++
//      0x14(20),0x15(21),0x16(22) -> col++, row++
//    Every facing value 0..0x17 nudges the marker by exactly one of these 8 (dcol,drow) pairs --
//    there is no no-op arm inside the switch (the only way to skip a nudge entirely is step 5's
//    facing>0x17 guard, taken when facing_dir is out of its normal 1..0x18 range).
// 7. @0x0042fae5-0x0042fafc: UNCONDITIONAL tail (runs whether or not step 3's branch fired):
//    move_state_timer += character_types[type].mine_time.
void unit_mine_arm_tick(const tact_view &tv, tact_store &own, int32_t unit_idx);

} // namespace detail

void unit_mine_arm_tick(int32_t unit_idx);


} // namespace mh::tact
