//
// sim/sim_unit_ctrl_group.cpp -- see sim_unit_ctrl_group.h. Translated from the DISASSEMBLY:
//   tmp/decomp/llm_strat_ctrl_group_contains_unit_00445f27.asm
//   tmp/decomp/llm_strat_unit_ctrl_group_assign_0044aa18.asm
// (Ghidra's .c drafts for both were re-walked against the raw CMP/JZ/JNZ/JNC targets and, unlike some
// other functions in this project, matched the assembly faithfully -- they are cited below only as
// corroboration, not as the source of truth.)
//
#include "sim/sim_unit_ctrl_group.h"

#include "addr/mh_calls.gen.h"  // typed callables for the two original ctrlgroup helpers
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_ctrl_group_calls &live_unit_ctrl_group_calls() {
    static const unit_ctrl_group_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_add_member),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_remove_member),
    };
    return c;
}

namespace detail {

// ---- llm_strat_ctrl_group_contains_unit @0x00445f27 (0x00445f27-0x00445f95) -------------------
//
// LAB_00445f4d/LAB_00445f57/LAB_00445f5f/LAB_00445f84/LAB_00445f8b: a plain bounded linear scan --
// `local_18` runs 0..count-1 (JL 0x00445f5f continues, else falls to the count<=local_18 exit at
// 0x00445f84 returning 0); each iteration reads _G_LLM_STRAT_CTRL_GROUPS[group_index].unit_ids
// [local_18] (IMUL group_index,0x194 / local_18*2 / +0xb63be4, i.e. base+4 -- the `unit_ids` field
// offset) and compares it (MOVZX, so unsigned/zero-extended) against unit_id; a match (JNZ not taken)
// sets the return value to 1 and jumps straight to the epilogue, skipping the rest of the scan.
int32_t ctrl_group_contains_unit(const sim_view &v, uint32_t unit_id, int32_t count, int32_t group_index) {
    for (int32_t i = 0; i < count; ++i) {
        if (v.ctrl_groups[group_index].unit_ids[i] == unit_id) return 1;
    }
    return 0;
}

// ---- llm_strat_unit_ctrl_group_assign @0x0044aa18 (0x0044aa18-0x0044aac4) ---------------------
//
// 0x0044aa35-0x0044aa52: read PlayerSide (ambient global, MOVZX word [0x00e58354]), compute
// units[PlayerSide][unit_id]'s address (IMUL PlayerSide,0x5b04 [UNITS_PER_PLAYER stride] + IMUL
// unit_id,0xe9 [unit record stride]), and CMP its ctrl_group_id byte (+0xdd8c77 folds the units-array
// base and the +0x2f field offset into one immediate) against 0; JZ 0x0044aaa3 skips the whole
// remove-step block when ctrl_group_id == 0 (unit is not currently in any group).
//
// 0x0044aa54-0x0044aa9e (only when ctrl_group_id != 0): re-derive the same unit address twice more
// (Watcom recomputing rather than keeping a value live across the branch -- nothing runs between these
// re-reads that could plausibly change ctrl_group_id, so caching the ONE read into `old_group_id`
// below is value-identical, not a behaviour change) to (a) fetch ctrl_group_id itself as the group
// index (EBX, the 3rd/EBX-register arg) and (b) compute &_G_LLM_STRAT_CTRL_GROUPS[ctrl_group_id]
// (IMUL ctrl_group_id,0x194 + the 0xb63be0 array base -- `.count` is the group struct's first field,
// so this address IS &group.count, matching the EDX arg CALL llm_strat_unit_ctrlgroup_remove_member
// receives). unit_id (EAX, from the entry-saved local) is the 1st arg.
//
// 0x0044aaa3-0x0044aabc (unconditional): the mirror-image add step -- EBX=new_group_id, EDX=
// &_G_LLM_STRAT_CTRL_GROUPS[new_group_id] (== &group.count), EAX=unit_id -> CALL
// llm_strat_unit_ctrlgroup_add_member. Always runs, whether or not the remove step ran above.
void unit_ctrl_group_assign(const sim_view &v, sim_store &own, const unit_ctrl_group_calls &c,
                            int32_t unit_id, int32_t new_group_id) {
    const int32_t side = (int32_t)(uint16_t)*v.player_side;
    const unit   &u    = unit_of(v, (uint32_t)side, unit_id);

    if (u.ctrl_group_id != 0) {
        const int32_t old_group_id = u.ctrl_group_id;
        ctrl_group   &old_group    = own.ctrl_group_at(old_group_id);
        c.ctrlgroup_remove_member((uint32_t)unit_id, &old_group.count, old_group_id);
    }

    ctrl_group &new_group = own.ctrl_group_at(new_group_id);
    c.ctrlgroup_add_member(unit_id, &new_group.count, new_group_id);
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

int32_t ctrl_group_contains_unit(uint32_t unit_id, int32_t count, int32_t group_index) {
    const sim_view v = state().read;
    return detail::ctrl_group_contains_unit(v, unit_id, count, group_index);
}

void unit_ctrl_group_assign(int32_t unit_id, int32_t new_group_id) {
    sim_state st = state();
    detail::unit_ctrl_group_assign(st.read, st.own, live_unit_ctrl_group_calls(), unit_id, new_group_id);
}


} // namespace mh::sim
