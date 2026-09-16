//
// sim/sim_unit_ctrlgroup_leave.cpp -- see sim_unit_ctrlgroup_leave.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_ctrlgroup_leave_0044928f.asm), not from Ghidra's .c draft:
// the draft's overall shape (unbounded first scan, bound-checked second scan, both compactions,
// the a2!=0 gate around the whole first block) was re-walked branch-by-branch against the raw
// CMP/JZ/JNZ/JL/JMP targets and matched the assembly faithfully -- cited in the header derivation
// as corroboration, not as the source of truth.
//
#include "sim/sim_unit_ctrlgroup_leave.h"

#include "addr/mh_calls.gen.h"  // typed callable for the original ctrlgrp notify thunk
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_ctrlgroup_leave_calls &live_unit_ctrlgroup_leave_calls() {
    static const unit_ctrlgroup_leave_calls c = {
        MH_LIBMH_BIND(llm_strat_order_ctrlgrp_flash_member),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_ctrlgroup_leave @0x0044928f (0x0044928f-0x00449400) -----------------------
void unit_ctrlgroup_leave(const sim_view &v, sim_store &own, const unit_ctrlgroup_leave_calls &c,
                          uint32_t unit_index) {
    // 0x004492aa: PlayerSide read #1 -- indexes the roster for the unit's CURRENT group id. Cast
    // through uint16_t first: the .asm reads it with MOVZX (zero-extend), and sim_view::player_side
    // is int16_t-typed storage -- a direct int32_t cast would sign-extend, which is wrong if
    // PlayerSide were ever negative. Same cast shape as sim_unit_ctrlgroup_member.cpp's own
    // PlayerSide reads.
    const int32_t side1 = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
    const int32_t a2 =
        own.unit_at(static_cast<uint32_t>(side1), static_cast<int32_t>(unit_index)).ctrl_group_id;

    // 0x004492ca: the whole first block is gated on a2 != 0 (a unit whose current group is 0 --
    // "no group" -- has nothing to leave here; only the group-0 transient scan below still runs).
    if (a2 != 0) {
        // 0x004492db-0x00449301: UNBOUNDED linear scan (NO comparison against `.count` anywhere in
        // this loop) for unit_index within group a2's unit_ids[] -- matches the .asm exactly. Not
        // "fixed" with a bound; this is the original's own contract (see header note).
        int32_t i = 0;
        while (own.ctrl_group_at(a2).unit_ids[i] != unit_index) {
            ++i;
        }
        // 0x00449308: count -= 1, BEFORE the shift below.
        own.ctrl_group_at(a2).count -= 1;
        // 0x00449314-0x0044935a: shift-compact survivors down by one slot from i. Bound re-read
        // from `.count` every iteration (the post-decrement value), matching the .asm's fresh
        // MOV+CMP each pass.
        for (int32_t j = i; j < own.ctrl_group_at(a2).count; ++j) {
            own.ctrl_group_at(a2).unit_ids[j] = own.ctrl_group_at(a2).unit_ids[j + 1];
        }

        // 0x0044935c: PlayerSide read #2 -- reused for both the roster clear and the notify call
        // below (nothing between the two original reads touches PlayerSide -- see header note).
        const int32_t side2 = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
        // 0x00449372: single BYTE store.
        own.unit_at(static_cast<uint32_t>(side2), static_cast<int32_t>(unit_index)).ctrl_group_id =
            0;
        // 0x00449379-0x0044938b: unconditional (within this a2!=0 block) tail notify.
        c.order_ctrlgrp_flash_member(static_cast<uint16_t>(side2), static_cast<uint16_t>(unit_index),
                                     static_cast<uint32_t>(a2));
    }

    // 0x0044938b-0x004493f7: a SEPARATE, INDEPENDENT compaction over the transient selection group
    // 0 -- ALWAYS runs, whether or not the a2!=0 block above ran or matched, and shares no
    // bookkeeping (counters, indices) with it. Unlike the a2 scan above, THIS scan IS bound-checked
    // against group 0's own `.count` (re-read every iteration).
    int32_t k = 0;
    while (true) {
        if (own.ctrl_group_at(0).count <= k) {
            // 0x0044939d: ran off the end without a match -- return with no further write.
            return;
        }
        if (own.ctrl_group_at(0).unit_ids[k] == unit_index) {
            break;
        }
        ++k;
    }
    // 0x004493b8: count -= 1, BEFORE the shift below.
    own.ctrl_group_at(0).count -= 1;
    // 0x004493c4-0x004493f1: shift-compact survivors down by one slot from k. No roster write and
    // no notify call for this block (group 0 is a transient selection scratch, not a real
    // Ctrl+digit assignment -- see sim_state.h's ctrl_groups comment).
    for (int32_t j = k; j < own.ctrl_group_at(0).count; ++j) {
        own.ctrl_group_at(0).unit_ids[j] = own.ctrl_group_at(0).unit_ids[j + 1];
    }
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------

void unit_ctrlgroup_leave(uint32_t unit_index) {
    sim_state st = state();
    detail::unit_ctrlgroup_leave(st.read, st.own, live_unit_ctrlgroup_leave_calls(), unit_index);
}


} // namespace mh::sim
