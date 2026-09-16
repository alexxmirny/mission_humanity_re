//
// tact/tact_unit_enqueue_command.cpp -- see tact_unit_enqueue_command.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_enqueue_command.h"


#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): stance_on/_off, time_GetCurrentTime
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_stance.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::tact {
namespace detail {

int32_t unit_enqueue_command(const tact_view &v, tact_store &own, int32_t unit_id, int32_t op,
                             uint8_t interrupt_flag, int32_t arg0, uint16_t arg1, uint16_t arg2,
                             uint16_t arg3) {
    tact_unit &u = own.unit_at(unit_id);

    // @0x0042b3be-0x0042b3d6: the mine-blast cooldown gate, op==9 only.
    if (op == 9) {
        if (MH_PROMOTED_ROW(time_GetCurrentTime)() < *v.mine_blast_time_end) return 0;
    }

    // @0x0042b3e2-0x0042b447: the re-entry gate. mh_llm_tact_unit_cmd_entry::interrupt_flag's own
    // field comment names this exact mechanism: "0 marks a protected/uninterruptible command...
    // refuse to act on the current slot when it is 0 while op != 0 and move_retry_wait == 0" --
    // the gate fires ONLY on a PROTECTED (interrupt_flag==0), OCCUPIED (op!=0) head slot, not on
    // "any non-empty slot" (the two JNZs at 0x0042b407/0x0042b41e both route AWAY from the refusal
    // check -- to LAB_0042b420 -> 428 -> straight to dispatch -- whenever interrupt_flag_slot!=0,
    // and only fall into the check at LAB_0042b422 when interrupt_flag_slot==0 AND op_slot!=0).
    int32_t head_idx = u.cmd_index;
    bool    head_slot_protected_and_occupied =
        (u.cmd_queue[head_idx].interrupt_flag == 0) && (u.cmd_queue[head_idx].op != 0);
    if (head_slot_protected_and_occupied && interrupt_flag == 1 && u.move_retry_wait == 0) {
        return 0;
    }

    // @0x0042b447-0x0042b587: op==0x47, "advance/repeat".
    if (op == 0x47) {
        u.status &= 0x9f;
        if (u.cmd_queue[head_idx].op == 0x40) {
            int32_t idx        = (head_idx + 1) % 0x80;
            int32_t scan_count = 0;
            for (int32_t i = 0; i < 0x7f; ++i) {
                mh::game::mh_llm_tact_unit_cmd_entry &e = u.cmd_queue[idx];
                if (e.op == 0) break;
                if (e.op == 7 && e.arg0 == 0) break;
                idx = (idx + 1) % 0x80;
                ++scan_count;
            }
            if (scan_count > 0)
                u.cmd_queue[head_idx].arg0 = (uint16_t)scan_count;
            else
                u.cmd_queue[head_idx].op = 0;
        }
        return 1;
    }

    // @0x0042b593-0x0042b670: op==0x46, "clear". The loop runs 0x7f iterations and PRE-increments
    // the index with wrap BEFORE each store (@0x0042b616-0x0042b62c), so it clears head+1..head+0x7f
    // and the HEAD slot is PRESERVED -- the in-flight command survives the clear. (Cleared all 0x80
    // including head until 2026-09-02; caught in the TACT1-P red audit.)
    if (op == 0x46) {
        u.status |= 0x20;
        u.move_redirect_col = u.pos_col;
        u.move_redirect_row = u.pos_row;
        int32_t idx         = head_idx;
        for (int32_t i = 0; i < 0x7f; ++i) {
            idx                 = (idx + 1) % 0x80;
            u.cmd_queue[idx].op = 0;
        }
        u.face_cmd_op   = 0;
        u.attack_cmd_op = 0;
        return 1;
    }

    // @0x0042b670-0x0042b6c9: op==0x7f, "stop/interrupt". Head op==0 -> plain return 1
    // (@0x0042b69c JZ 0x0042b6bd -> [EBP-0x1c]=1, no status write). Head op!=0 -> status|=2 AND
    // FALL THROUGH (@0x0042b6bb JMP 0x0042b6c9) past the op==6/op==2 tests into the DEFAULT
    // queue-append below, enqueueing an op-0x7f marker entry. (Both halves were wrong until
    // 2026-09-02 -- the bit was set on the empty case and the fall-through append was dropped;
    // caught in the TACT1-P red audit.) Expressed as a fall-through here too: only the empty
    // case returns.
    if (op == 0x7f) {
        if (u.cmd_queue[head_idx].op == 0) return 1;
        u.status |= 0x2;
        // falls through to the default queue-append at the bottom of the function.
    }

    // @0x0042b6c9-0x0042b78f: op==6, immediate FACE/TURN short-circuit. The re-entry refusal fires
    // iff iflag==1 AND face_cmd_op==6 AND face_interrupt_flag==0 (@0x0042b6f3 `CMP ...,0; JZ
    // 0x0042b6fe` -- the JZ on ZERO is the refuse arm), and BOTH refusals return 1, not 0
    // (@0x0042b6fe / @0x0042b716 each store [EBP-0x1c]=1). (Condition inverted and returns wrong
    // until 2026-09-02; caught in the TACT1-P red audit.)
    if (op == 6) {
        if (interrupt_flag == 1 && u.face_cmd_op == 6 && u.face_interrupt_flag == 0) return 1;
        if (arg0 < 1 || arg0 > 0x18) return 1;
        u.face_interrupt_flag = interrupt_flag;
        u.face_cmd_op         = 6;
        u.face_cmd_target_dir = (uint16_t)arg0;
        u.face_cmd_arg1       = 0;
        u.face_cmd_arg2       = 0;
        u.face_cmd_arg3       = 0;
        return 1;
    }

    // @0x0042b78f-0x0042b805: op==2, immediate ATTACK/AIM short-circuit.
    if (op == 2) {
        u.attack_interrupt_flag = interrupt_flag;
        u.attack_cmd_op         = 2;
        u.attack_gun_toggle     = (uint16_t)arg0;
        u.attack_cmd_arg1       = arg1;
        u.aim_x                 = arg2;
        u.aim_y                 = arg3;
        return 1;
    }

    // @0x0042b805-0x0042b822: op==0x1f, stance on.
    if (op == 0x1f) {
        MH_LIBMH_BIND(llm_tact_unit_cmd_stance_on)(unit_id);
        return 1;
    }

    // @0x0042b822-0x0042b83f: op==0x1d, stance off.
    if (op == 0x1d) {
        MH_LIBMH_BIND(llm_tact_unit_cmd_stance_off)(unit_id);
        return 1;
    }

    // @0x0042b83f-0x00431163 (well, 0x0042b93c): default -- append to the circular queue.
    int32_t idx = head_idx;
    for (int32_t i = 0; i < 0x80; ++i) {
        if (u.cmd_queue[idx].op == 0) {
            u.cmd_queue[idx].interrupt_flag = interrupt_flag;
            u.cmd_queue[idx].op             = (uint16_t)op;
            u.cmd_queue[idx].arg0           = (uint16_t)arg0;
            u.cmd_queue[idx].arg1           = arg1;
            u.cmd_queue[idx].arg2           = arg2;
            u.cmd_queue[idx].arg3           = arg3;
            return 1;
        }
        idx = (idx + 1) % 0x80;
    }
    return 0; // queue genuinely full
}

} // namespace detail

int32_t unit_enqueue_command(int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0,
                             uint16_t arg1, uint16_t arg2, uint16_t arg3) {
    tact_state st = state();
    return detail::unit_enqueue_command(st.read, st.own, unit_id, op, interrupt_flag, arg0, arg1,
                                        arg2, arg3);
}


// ---- THE PROMOTED ARM IS GONE (fork F2E: tactical mode is demoted permanently) ------------------
//
// This TU's MH_EXPORT_REPLACE was never an install -- its installer was deliberately never called,
// because the TJ order-enqueue recorder owns this entry and a second entry patch would be refused.
// What the macro supplied was a THUNK for that recorder's detour to fall through to, so a journal
// replay could record the call AND still run our body (SIM1-P clause 6b; without it the recorder
// silently un-promoted the body it was recording, measured as 80 -> 46 covered lines).
//
// It goes anyway, and the reason is upstream of it: with the tactical pump demoted (tact_frame.cpp)
// our tactical chain does not execute in ANY hosted configuration, so there is no body of ours for
// the recorder to fall through to. The recorder now falls through to the original, which is what
// runs. Keeping the thunk would keep a route to a body nothing else on that path reaches.
//
// THE BODY ABOVE IS UNTOUCHED, and its rebind row survives (ruling Q2).

} // namespace mh::tact
