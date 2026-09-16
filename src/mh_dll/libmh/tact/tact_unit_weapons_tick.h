#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The 15 distinct outward function calls this dispatcher makes, plus time_GetCurrentTime (called 7
// times at 7 distinct decision points -- see the detail:: derivation below; every call is a FRESH
// read, none of the 7 reuses another's return value). Indirected for offline testability (Law 3b) --
// mandatory here, not optional, because several of these are real-VA calls that fault inside
// net_selftest.exe and because at least one (unit_cmd_teleport_jump_tick) is itself
// proven-hazardous under shadow (see the header banner above).
struct unit_weapons_tick_calls {
    double (*time_get_current_time)(); // time_GetCurrentTime @0x00427616

    void (*unit_cmd_queue_advance)(int32_t  unit_idx,
                                   uint32_t cmd_index); // llm_tact_unit_cmd_queue_advance @0x004313cf
    int32_t (*unit_death_tick)(int32_t unit_idx);       // llm_tact_unit_death_tick @0x0042fb0b
    void (*unit_fire_weapon)(int32_t building_id, int32_t weapon_subindex,
                             int32_t fire_arg); // llm_tact_unit_fire_weapon @0x00430855
    void (*unit_cmd_advance)(int32_t unit_index,
                             int32_t cmd_slot_index); // llm_tact_unit_cmd_advance @0x00431229
    void (*unit_rotate_tick)(int32_t unit_idx);       // llm_tact_unit_rotate_tick @0x004307c5
    void (*unit_move_tick)(int32_t unit_idx, int32_t cmd_slot,
                           double dt);           // llm_tact_unit_move_tick @0x0042fb9b
    void (*unit_kneel_tick)(int32_t unit_id);    // llm_tact_unit_kneel_tick @0x00430363
    void (*unit_stand_tick)(int32_t unit_idx);   // llm_tact_unit_stand_tick @0x0043047d
    void (*unit_mine_arm_tick)(int32_t unit_id); // llm_tact_unit_mine_arm_tick @0x0042f935
    void (*unit_set_anim_state)(int32_t building_id,
                                uint8_t state); // llm_tact_unit_set_anim_state @0x00430f03 (always
                                                // called with 0 here)
    void (*unit_cmd_teleport_jump_tick)(
        int32_t unit_idx, int32_t cmd_queue_slot); // llm_tact_unit_cmd_teleport_jump_tick @0x00433464
    void (*unit_cmd_advance_with_defstat)(
        int32_t unit_idx, int32_t cmd_or_slot_index); // llm_tact_unit_cmd_advance_with_defstat @0x0042f8e4
    void (*unit_cmd_stance_off)(int32_t unit_index);  // llm_tact_unit_cmd_stance_off @0x0043060f
    void (*unit_cmd_stance_on)(int32_t unit_index);   // llm_tact_unit_cmd_stance_on @0x00430580
    void (*unit_cmd_queue_resubmit_run)(
        int32_t unit_idx, int32_t queue_slot); // llm_tact_unit_cmd_queue_resubmit_run @0x0043069e
};

const unit_weapons_tick_calls &live_unit_weapons_tick_calls();

namespace detail {

// llm_tact_unit_weapons_tick @0x0042f338. See the header banner for the loop shape; addresses below
// are the derivation for each numbered step.
//
//  1. @0x0042f34a-0x0042f361: char_type = units[unit_idx].type -- read ONCE, before the loop.
//  2. @0x0042f364-0x0042f37f (loop top, every iteration): now = time_GetCurrentTime() [call 1/7]; if
//     now <= move_state_timer -> @0x0042f8bd: status &= 0xf7; return.
//  3. @0x0042f37f-0x0042f3b6: MOVE_CUR_COL = pos_col, MOVE_CUR_ROW = pos_row (this function is the
//     documented sole writer of both -- tact_state.h's own comment on tact_view::move_cur_col/row);
//     cmd_index = units[unit_idx].cmd_index, RE-READ every iteration (not hoisted out of the loop).
//  4. @0x0042f3b9-0x0042f3e4: unit_cmd_queue_advance(unit_idx, cmd_index). If anim_state == 0x1f:
//     death_result = unit_death_tick(unit_idx) [ORIGINAL, not the sibling translation -- see banner];
//     death_result != 0 -> direct return @0x0042f8da (no status-bit clear); == 0 -> loop restart.
//  5. @0x0042f3e9-0x0042f42a: if attack_cmd_op == 2: unit_fire_weapon(unit_idx, cmd_index,
//     attack_gun_toggle); status |= 8.
//  6. @0x0042f430-0x0042f4dd: if cmd_queue[cmd_index].op == 7: if arg0 != 0, arm the immediate
//     FACE/TURN record (face_interrupt_flag=0, face_cmd_op=6, face_cmd_target_dir=arg0,
//     face_cmd_arg1/2/3=0); THEN unconditionally unit_cmd_advance(unit_idx, cmd_index) (called even
//     when arg0 == 0).
//  7. @0x0042f4dd-0x0042f509: if face_cmd_op == 6 && progress == 0: unit_rotate_tick(unit_idx); loop
//     restart (skips everything below for this iteration).
//  8. @0x0042f50e-0x0042f533: if (status & 0x40) != 0: move_state_timer = time_GetCurrentTime() [call
//     2/7]; loop restart.
//  9. @0x0042f538-0x0042f58d: if (status & 0x20) != 0 && progress == 0: status = (status & 0xdf) |
//     0x40; move_state_timer = time_GetCurrentTime() [call 3/7]; loop restart.
// 10. @0x0042f592-0x0042f5d6: the deferred-WAIT gate. If cmd_wait_until_time > 0.0: t =
//     time_GetCurrentTime() [call 4/7]; if t < cmd_wait_until_time (still waiting): move_state_timer =
//     time_GetCurrentTime() [call 5/7 -- a SECOND, independent read, NOT a reuse of t]; return
//     directly (no status-bit clear). Otherwise (cmd_wait_until_time <= 0.0, or the wait elapsed)
//     fall through to the dispatch.
// 11. @0x0042f5d6-0x0042f5ed: op = cmd_queue[cmd_index].op, read once and reused by every arm below.
// 12. @0x0042f5ed-0x0042f8b8: the op dispatch (explicit compare chain, not a jump table):
//       op==0  @0x0042f6ad: if owner==0: status &= 0xfb (always, regardless of owner); then
//              move_state_timer = time_GetCurrentTime() [call 6/7].
//       op==1  @0x0042f760: cmd_wait_until_time = 0.0; dt = (status&4)==0 ?
//              CHARACTER_TYPES[char_type].speed : CHARACTER_TYPES[char_type].speed /
//              CHARACTER_TYPES[char_type].run_speed; unit_move_tick(unit_idx, cmd_index, dt).
//       op==4  @0x0042f7d6: unit_kneel_tick(unit_idx); move_state_timer +=
//              CHARACTER_TYPES[char_type].kneel_time.
//       op==5  @0x0042f803: unit_stand_tick(unit_idx); move_state_timer +=
//              CHARACTER_TYPES[char_type].kneel_time -- SAME field as the op==4 arm, not death_time or
//              a stand-specific timer; transcribed literally, not "fixed".
//       op==8  @0x0042f6f1: cmd_wait_until_time = time_GetCurrentTime() [call 7/7] + (double)
//              cmd_queue[cmd_index].arg0; unit_cmd_advance(unit_idx, cmd_index).
//       op==9  @0x0042f84d: if MINES_ENABLED == 0: progress = 0; unit_set_anim_state(unit_idx, 0);
//              unit_cmd_advance(unit_idx, units[unit_idx].cmd_index) -- a FRESH re-read of the field,
//              not the cached `cmd_index` local (0x0042f882, matches the original's own re-read).
//              Else: unit_mine_arm_tick(unit_idx).
//       op==0xa @0x0042f893: unit_cmd_teleport_jump_tick(unit_idx, cmd_index).
//       op==0xb @0x0042f8a0: unit_cmd_advance_with_defstat(unit_idx, cmd_index).
//       op==0x1c @0x0042f840: unit_cmd_stance_off(unit_idx).
//       op==0x1e @0x0042f830: unit_cmd_stance_on(unit_idx).
//       op==0x40 @0x0042f733: unit_cmd_queue_resubmit_run(unit_idx, cmd_index); move_state_timer +=
//              CHARACTER_TYPES[char_type].speed.
//       op==0x7f @0x0042f8ad: unit_cmd_advance(unit_idx, cmd_index).
//       every other op value (2,3,6,7,0xc..0x1b,0x1d,0x1f..0x3f,0x41..0x7e,0x80..): no-op.
// 13. @0x0042f8b8: every arm above -- and the no-op default -- reconverges here and jumps straight
//     back to step 2. Reproduced as the bottom of the C++ `for(;;)` body (no explicit `continue`
//     needed for arms that simply fall off the end).
void unit_weapons_tick(const tact_view &tv, tact_store &own, const unit_weapons_tick_calls &c,
                       int32_t unit_idx);

} // namespace detail

void unit_weapons_tick(int32_t unit_idx);


} // namespace mh::tact
