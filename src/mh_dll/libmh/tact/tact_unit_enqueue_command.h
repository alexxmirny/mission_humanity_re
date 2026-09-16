//
// tact/tact_unit_enqueue_command.h -- TACT1B: the real player/AI order entry point. Dispatches a
// handful of ops to immediate single-slot records, clears/counts the queue for two housekeeping
// ops, and otherwise appends to the circular 128-entry command queue.
//
//   llm_tact_unit_enqueue_command @0x0042b39d (0x5ab)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_enqueue_command @0x0042b39d. Returns 1 on success, 0 on refusal.
//
// 1. @0x0042b3be-0x0042b3d6: op==9 (mine-arm) is refused while time_GetCurrentTime() is still
//    below _G_LLM_TACT_MINE_BLAST_TIME_END (a global mine-blast cooldown, shared across units).
// 2. @0x0042b3e2-0x0042b447: the re-entry gate. If the CURRENT head slot (cmd_queue[cmd_index]) is
//    PROTECTED AND OCCUPIED (interrupt_flag==0 AND op!=0) AND the caller's own interrupt_flag
//    param == 1 AND move_retry_wait == 0, refuse. (mh_llm_tact_unit_cmd_entry::interrupt_flag's own
//    field comment names this mechanism directly. Every other combination falls through to
//    dispatch -- including a slot with interrupt_flag!=0, i.e. an UN-protected/overridable one,
//    regardless of its op.)
// 3. @0x0042b447-0x0042b587: op==0x47 ("advance/repeat"). Clears status bits 0x20/0x40
//    unconditionally. If the head slot's op is 0x40 ("run"), scans forward (wrapping, up to 0x7f
//    entries) counting consecutive non-empty, non-"turn-then-attack-with-unset-arg0" slots; writes
//    that count into the head slot's arg0 if nonzero, else clears the head slot's op to 0. (Struct
//    field mh_llm_tact_unit_cmd_entry::arg0's own comment independently names this exact mechanism:
//    "op 0x47: the run length written at 0x0042b55b and read back as the resubmit count at
//    0x004306d5" -- llm_tact_unit_cmd_queue_resubmit_run.) Always returns 1.
// 4. @0x0042b593-0x0042b670: op==0x46 ("clear"). Sets status bit 0x20, snapshots
//    move_redirect_col/row from the current position, zeroes EVERY cmd_queue slot's op (all 128,
//    starting at cmd_index, wrapping -- the whole queue), then zeroes face_cmd_op and
//    attack_cmd_op. Always returns 1.
// 5. @0x0042b670-0x0042b6c9: op==0x7f ("stop/interrupt"). Sets status bit 0x2 only if the head slot
//    is already empty (op==0). Always returns 1.
// 6. @0x0042b6c9-0x0042b78f: op==6 (immediate FACE/TURN short-circuit). Refuses if
//    interrupt_flag==1 AND a FACE is already pending with its own face_interrupt_flag set. Refuses
//    if arg0 is outside [1, 0x18] (an invalid heading). Otherwise arms the immediate FACE record
//    (face_interrupt_flag/face_cmd_op/face_cmd_target_dir/face_cmd_arg1..3, the last three zeroed)
//    and returns 1.
// 7. @0x0042b78f-0x0042b805: op==2 (immediate ATTACK/AIM short-circuit). Arms the immediate ATTACK
//    record (attack_interrupt_flag/attack_cmd_op/attack_gun_toggle/attack_cmd_arg1/aim_x/aim_y) and
//    returns 1 unconditionally -- no validity check on this path.
// 8. @0x0042b805-0x0042b83f: op==0x1f -> llm_tact_unit_cmd_stance_on(unit_id) (frontier; the
//    already-committed prototype takes ONLY unit_id -- op itself is loaded into EDX at the call site
//    but the committed __watcall signature never consumes it, matching this function's OWN dead
//    EDX/EBX pattern in tact_unit_move_advance). op==0x1d -> llm_tact_unit_cmd_stance_off(unit_id),
//    same shape. Both return 1 unconditionally.
// 9. @0x0042b83f-0x00431163 (queue append, the default path): scans cmd_queue starting at
//    cmd_index, wrapping, for the first op==0 slot, up to 0x80 (128) tries. If none is empty
//    (queue genuinely full), returns 0. Otherwise writes
//    interrupt_flag/op/arg0/arg1/arg2/arg3 at that slot and returns 1. NOTE: cmd_index itself is
//    never advanced here -- only llm_tact_unit_cmd_advance (frontier) moves the head.
int32_t unit_enqueue_command(const tact_view &v, tact_store &own, int32_t unit_id, int32_t op,
                             uint8_t interrupt_flag, int32_t arg0, uint16_t arg1, uint16_t arg2,
                             uint16_t arg3);

} // namespace detail

int32_t unit_enqueue_command(int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0,
                             uint16_t arg1, uint16_t arg2, uint16_t arg3);


} // namespace mh::tact
