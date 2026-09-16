//
// tact/tact_group_issue_order.cpp -- see tact_group_issue_order.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_group_issue_order.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): unit_enqueue_command, move_step_attempt, move_path_preview_walk
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_move_path_preview_walk.h"
#include "tact/tact_move_step_attempt.h"
#include "tact/tact_unit_enqueue_command.h"
#include "state/mode_planes.h"

namespace mh::tact {
namespace detail {

void group_issue_order(const tact_view &v, tact_store &own, int32_t op, uint32_t arg0, uint32_t arg1,
                       uint32_t arg2, uint32_t arg3) {
    // -0x24 / -0x20 in the original: mutable copies of arg0/arg1, later REUSED as the out_col/out_row
    // scratch for the in-flight-move redirect's preview walk (step 6) -- see the header's note. Not
    // simplified into fresh locals: the final enqueue call (step 7) reads these SAME variables, so a
    // redirect that ran changes what gets forwarded.
    int32_t  local_arg0 = (int32_t)arg0;
    uint32_t local_arg1 = arg1;

    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        tact_unit &u = own.unit_at(i);

        // @0x0042b0eb-0x0042b0f5: only owned/selected units.
        if ((u.status & 1) != 1) {
            continue;
        }

        // @0x0042b0fb-0x0042b10f: op==7 override from the live UI click preview.
        if (op == 7) {
            local_arg0 = (int32_t)u.click_preview_facing;
        }

        // @0x0042b112-0x0042b17a: op==1 redundant-MOVE dedup against the head queue entry's scratch
        // arg2/arg3 (the last pathfind's flood-result col/row, NOT an input).
        if (op == 1) {
            const int32_t                               idx   = u.cmd_index;
            const mh::game::mh_llm_tact_unit_cmd_entry &entry = u.cmd_queue[idx];
            if (entry.op == 1 && (uint32_t)entry.arg2 == (uint32_t)local_arg0 &&
                (uint32_t)entry.arg3 == local_arg1) {
                u.status |= 4;
                continue;
            }
        }

        // @0x0042b19c-0x0042b1d3: an implicit STOP before any non-CLEAR, non-ATTACK/AIM,
        // non-FACE/TURN order lands on an idle unit.
        if (op != 2 && op != 6) {
            if ((u.status & 0x60) == 0) {
                MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(i, /*op=*/0x7f, /*interrupt_flag=*/0,
                                                             /*arg0=*/0, /*arg1=*/0, /*arg2=*/0, /*arg3=*/0);
            }
        }

        // @0x0042b1d3-0x0042b28e: op==0x46 (CLEAR) -- flush the whole queue.
        if (op == 0x46) {
            u.move_redirect_col = u.pos_col;
            u.move_redirect_row = u.pos_row;
            for (int32_t k = 0; k < 0x80; ++k) {
                u.cmd_queue[k].op = 0;
            }
            u.cmd_index     = 0;
            u.face_cmd_op   = 0;
            u.attack_cmd_op = 0;
            u.status |= 0x20;
        }

        // @0x0042b28e-0x0042b33f: an in-flight MOVE redirect, only when the unit is busy/animated
        // AND the new order is itself a MOVE.
        const uint8_t busy = u.status & 0x60;
        if (busy != 0 && op == 1) {
            // @0x0042b2ac-0x0042b2ce: return value UNUSED by the original -- only the global path
            // slot id it sets is read afterward.
            (void)MH_LIBMH_BIND(llm_tact_move_step_attempt)(u.move_redirect_col, u.move_redirect_row,
                                                            local_arg0, local_arg1);
            if (*v.move_path_slot_id != -1) {
                int32_t out_col = 0;
                int32_t out_row = 0;
                MH_LIBMH_BIND(llm_tact_move_path_preview_walk)(u.move_redirect_col, u.move_redirect_row,
                                                               *v.move_path_slot_id, &out_col, &out_row);
                local_arg0          = out_col;           // reuse: local_arg0 <- walked column
                local_arg1          = (uint32_t)out_row; // reuse: local_arg1 <- walked row
                u.move_redirect_col = (uint8_t)out_col;
                u.move_redirect_row = (uint8_t)out_row;
                // @0x0042b32e-0x0042b338: release the path slot's flag. The "owner" index (the
                // original's -0x18 local) is always 0 in this function -- a dead multiply, preserved
                // literally rather than simplified away.
                own.planes().path_slot_flag_at(0, *v.move_path_slot_id) = 0;
            }
        }

        // @0x0042b33f-0x0042b394: forward the order. local_arg0/local_arg1 carry the REDIRECTED
        // (walked) position if step 6 ran, or the (possibly op==7-overridden) original arguments
        // otherwise.
        const bool still_busy = (u.status & 0x60) != 0;
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(i, op, /*interrupt_flag=*/still_busy ? 1 : 0,
                                                     (uint32_t)local_arg0, local_arg1, arg2, arg3);
    }
}

} // namespace detail

void group_issue_order(int32_t op, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    tact_state st = state();
    detail::group_issue_order(st.read, st.own, op, arg0, arg1, arg2, arg3);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
