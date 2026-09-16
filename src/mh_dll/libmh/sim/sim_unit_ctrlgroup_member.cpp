//
// sim/sim_unit_ctrlgroup_member.cpp -- see sim_unit_ctrlgroup_member.h. Translated from the
// DISASSEMBLY:
//   tmp/decomp/llm_strat_unit_ctrlgroup_add_member_00449401.asm
//   tmp/decomp/llm_strat_unit_ctrlgroup_remove_member_0044947e.asm
// (Ghidra's .c drafts for both were re-walked against the raw CMP/JZ/JNZ/JL/JMP targets and matched
// the assembly faithfully for both functions -- cited in the header derivation as corroboration, not
// as the source of truth.)
//
#include "sim/sim_unit_ctrlgroup_member.h"

#include "addr/mh_calls.gen.h"  // typed callables for the two original ctrlgrp notify thunks
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_ctrlgroup_member_calls &live_unit_ctrlgroup_member_calls() {
    static const unit_ctrlgroup_member_calls c = {
        MH_LIBMH_BIND(llm_strat_order_ctrlgrp_select_member),
        MH_LIBMH_BIND(llm_strat_order_ctrlgrp_flash_member),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_ctrlgroup_add_member @0x00449401 (0x00449401-0x0044947d) -------------------
void unit_ctrlgroup_add_member(const sim_view &v, sim_store &own, const unit_ctrlgroup_member_calls &c,
                               int32_t unit_id, int32_t *count_ptr, int32_t group_idx) {
    // 0x00449420-0x00449433: append, no bounds check against the 200-entry array (matches original).
    own.ctrl_group_at(group_idx).unit_ids[*count_ptr] = (uint16_t)unit_id;
    // 0x0044943a-0x0044943d.
    *count_ptr = *count_ptr + 1;

    // PlayerSide read once, reused below -- see header note (nothing between the two original reads
    // touches it).
    const int32_t side = (int32_t)(uint16_t)*v.player_side;

    // 0x0044943f-0x0044945e: group 0 ("no group") never stamps the roster field.
    if (group_idx != 0) {
        own.unit_at((uint32_t)side, unit_id).ctrl_group_id = (uint8_t)group_idx;
    }

    // 0x00449464-0x00449471: unconditional tail notify, whether or not the roster write ran.
    c.order_ctrlgrp_select_member((uint32_t)side, (uint16_t)unit_id, (uint32_t)group_idx);
}

// ---- llm_strat_unit_ctrlgroup_remove_member @0x0044947e (0x0044947e-0x0044955e) -----------------
void unit_ctrlgroup_remove_member(const sim_view &v, sim_store &own, const unit_ctrlgroup_member_calls &c,
                                  uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx) {
    // 0x0044949d-0x0044951e: bounded scan + shift-compact on match. NOT `break`ing after a match and
    // NOT re-checking the slot a match just shifted a value into are both faithful to the original
    // (see header derivation) -- preserved, not "improved".
    for (int32_t i = 0; i < *count_ptr; ++i) {
        if (own.ctrl_group_at(group_idx).unit_ids[i] == unit_idx) {
            *count_ptr = *count_ptr - 1;
            for (int32_t j = i; j < *count_ptr; ++j) {
                own.ctrl_group_at(group_idx).unit_ids[j] = own.ctrl_group_at(group_idx).unit_ids[j + 1];
            }
        }
    }

    // PlayerSide read once, reused below -- see header note.
    const int32_t side = (int32_t)(uint16_t)*v.player_side;

    // 0x00449522-0x0044953e: group 0 ("no group") never clears the roster field either (mirrors the
    // add side's own group_idx!=0 gate).
    if (group_idx != 0) {
        own.unit_at((uint32_t)side, (int32_t)unit_idx).ctrl_group_id = 0;
    }

    // 0x00449545-0x00449552: unconditional tail notify, whether or not the roster write ran.
    c.order_ctrlgrp_flash_member((uint16_t)side, (uint16_t)unit_idx, (uint32_t)group_idx);
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

void unit_ctrlgroup_add_member(int32_t param_1, int32_t *param_2, int32_t a2) {
    sim_state st = state();
    detail::unit_ctrlgroup_add_member(st.read, st.own, live_unit_ctrlgroup_member_calls(), param_1,
                                      param_2, a2);
}

void unit_ctrlgroup_remove_member(uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx) {
    sim_state st = state();
    detail::unit_ctrlgroup_remove_member(st.read, st.own, live_unit_ctrlgroup_member_calls(), unit_idx,
                                         count_ptr, group_idx);
}


} // namespace mh::sim
