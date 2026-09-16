//
// tact/tact_group_issue_order.h -- TACT1B: broadcasting a player order to the selected
// squad.
//
//   llm_tact_group_issue_order @0x0042b09f (0x2fe)
//
// Walks every unit slot [1, 0x80], and for each one currently OWNED/SELECTED (status bit 0),
// forwards `op` to `llm_tact_unit_enqueue_command` (a frontier callee, stays original per Law 4) --
// with several op-specific short-circuits along the way: an op-7 (turn-then-attack) target-direction
// override from the live UI click preview, an op-1 (MOVE) redundant-order dedup, an implicit STOP
// issued before any non-clear order on an idle unit, an op-0x46 (CLEAR) full queue flush, and an
// in-flight-move REDIRECT (re-pathing toward a new destination without waiting for the current step
// to finish) when a MOVE order arrives while the unit is already busy/animated.
//
// THE RETURN VALUE IS GENUINE WATCOM GARBAGE, NOT DESIGNED OUTPUT: the epilogue (0x0042b394) never
// writes EAX, so the committed `int` return is whatever the last executed instruction happened to
// leave there. All 13 call sites (llm_tact_frame, llm_tact_ui_order_buttons_minimap_tick; measured
// via find-cross-references) call it as a bare statement and never read the result. A translation
// cannot and must not try to reproduce that bit-for-bit -- the shadow site is armed with
// `compare_return: false` for exactly this reason (gen_dll_shadow.py's own vocabulary for "a return
// value a side-effect-free shadow arm cannot reproduce").
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_group_issue_order @0x0042b09f. `own` because this writes _G_LLM_TACT_UNITS and (via
// `own.planes()`) the shared _G_LLM_STRAT_PATH_SLOT_FLAGS plane.
//
// 1. @0x0042b0e4-0x0042b0f5: skip any unit not currently owned/selected (status bit 0).
// 2. @0x0042b0fb-0x0042b10f: op==7 (turn-then-attack) overrides the target-direction argument with
//    the unit's own LIVE click-preview facing (computed per-frame by llm_tact_frame's UI overlay).
// 3. @0x0042b112-0x0042b17a: op==1 (MOVE) dedup -- if the head cmd_queue entry is already a MOVE
//    whose (arg2, arg3) SCRATCH fields (the last pathfind's flood-result col/row, NOT an input --
//    see mh_llm_tact_unit_cmd_entry's field comment) already equal the newly requested (col, row),
//    mark the unit "redundant" (status |= 4) and move on to the next unit without issuing anything.
// 4. @0x0042b19c-0x0042b1d3: for any op OTHER than 2 (attack/aim) or 6 (face/turn), if the unit is
//    idle (status & 0x60 == 0), issue an implicit STOP (op 0x7f) first, to clear any residual queued
//    move before the real order lands.
// 5. @0x0042b1d3-0x0042b28e: op==0x46 (CLEAR) -- snapshot the current position into
//    move_redirect_col/row, zero every cmd_queue entry's op, reset cmd_index/face_cmd_op/attack_cmd_op
//    to 0, and set status bit 0x20.
// 6. @0x0042b28e-0x0042b33f: if the unit is busy/animated (status & 0x60 != 0) AND op==1 (MOVE),
//    REDIRECT the in-flight move: attempt one step toward the new destination
//    (llm_tact_move_step_attempt, frontier, return value UNUSED by the original -- only the global
//    _G_LLM_TACT_MOVE_PATH_SLOT_ID it sets is read afterward), and if a path slot was found, preview-
//    walk it (llm_tact_move_path_preview_walk, frontier) to refresh move_redirect_col/row and release
//    the path slot's flag (own.planes().path_slot_flag_at(0, slot) = 0 -- the "owner" index is a dead
//    local that is always 0 in this function, preserved literally rather than simplified away).
// 7. @0x0042b33f-0x0042b394: finally, forward the order via llm_tact_unit_enqueue_command --
//    interrupt_flag=1 if the unit is still busy/animated (status & 0x60 != 0), else interrupt_flag=0.
void group_issue_order(const tact_view &v, tact_store &own, int32_t op, uint32_t arg0, uint32_t arg1,
                       uint32_t arg2, uint32_t arg3);

} // namespace detail

void group_issue_order(int32_t op, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3);


} // namespace mh::tact
