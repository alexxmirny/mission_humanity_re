//
// tact/tact_unit_kneel_tick.h -- TACT1C: command-queue op 4 (KNEEL) per-tick dispatch handler.
//
//   llm_tact_unit_kneel_tick @0x00430363 (0x11a)
//   void __watcall llm_tact_unit_kneel_tick(int unit_id)
//
// PROOF: OFFLINE (revised from RIG by the conductor, 2026-08-26). The shadow generator derives an
// EMPTY region set for this site -- gen_dll_shadow.py classifies it VACUOUS and gen_shadow_ini.py
// refuses to arm it, so a rig run can produce no evidence here. The two outward calls therefore go
// through a `unit_kneel_tick_calls` struct (the offline siblings' shape) so the oracle can mock
// them and cover every arm; the original direct-`mh::call::` frontier semantics are unchanged (the
// live binder points at the same two originals).
//
// DERIVATION (tmp/decomp_tact/llm_tact_unit_kneel_tick_00430363.asm):
//
// 1. @0x00430380-0x0043038e: if unit.anim_state == 3 (already fully kneeled), do nothing -- return.
// 2. @0x00430394-0x004303b2: else if unit.anim_state == 2 (mid kneel-down anim) AND unit.progress ==
//    0 (no progress accrued in this anim yet), go straight to step 3 WITHOUT touching progress or
//    anim_state. Any other combination (a different anim_state, or anim_state == 2 with progress
//    already nonzero) falls through to step 4 instead.
// 3. @0x004303b6-0x004303f0: if unit.cmd_queue[unit.cmd_index].op == 4 (KNEEL is still the queue
//    head), dequeue it (llm_tact_unit_cmd_advance(unit_id, cmd_index)); either way, return.
// 4. @0x004303f5-0x00430474: unit.progress += 1 (byte INC, wraps mod 256 like the original);
//    llm_tact_unit_set_anim_state(unit_id, 2) (arms/keeps the kneel-down anim). Re-read progress
//    AFTER that call (matching the original's own re-load through the array rather than reusing a
//    pre-call value): if progress <= 0xf (15), return. Otherwise (progress just crossed 15):
//    unit.progress = 0; llm_tact_unit_set_anim_state(unit_id, 3) (now fully kneeled); re-read
//    cmd_index/cmd_queue[cmd_index].op AFTER that second call, and if still == 4, dequeue it the
//    same way step 3 does. Return regardless.
//
// Steps 3's check and step 4's tail check are the SAME test (cmd_queue head op == 4) against the
// SAME queue head, preserved as two separate call sites matching the original's two separate CALL
// instructions (0x004303eb and 0x0043046f) rather than consolidated into one shared branch.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The two frontier callees, mockable by the offline oracle (see the PROOF note above).
// Signatures match mh_calls.gen.h exactly (set_anim_state's uint8_t second arg included).
struct unit_kneel_tick_calls {
    void (*set_anim_state)(int32_t unit_id, uint8_t anim_state); // llm_tact_unit_set_anim_state
    void (*cmd_advance)(int32_t unit_id, int32_t cmd_index);     // llm_tact_unit_cmd_advance
};
const unit_kneel_tick_calls &live_unit_kneel_tick_calls();

namespace detail {

// llm_tact_unit_kneel_tick @0x00430363. See the header banner for the full derivation. Takes only
// `own` -- the unit record this function reads and writes is reached exclusively through the store.
void unit_kneel_tick(tact_store &own, const unit_kneel_tick_calls &c, int32_t unit_id);

} // namespace detail

void unit_kneel_tick(int32_t unit_id);


} // namespace mh::tact
