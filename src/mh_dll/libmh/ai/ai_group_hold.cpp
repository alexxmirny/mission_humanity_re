//
// ai/ai_group_hold.cpp -- see ai_group_hold.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_group_{enter_hold_004eb245,seed_resolved_target_004ec73c}.asm).
//
#include "ai/ai_group_hold.h"


namespace mh::ai {
namespace detail {

namespace {
// llm_strat_ai_group_task_activate's jump-table index 0x0b -> llm_strat_ai_group_task_hold. Read
// off that dispatch @0x004eb0cd rather than inferred from the function's name.
constexpr int16_t TASK_HOLD = 0x0b;
} // namespace

group_hold_report group_enter_hold(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   int32_t player_id, int32_t group_index) {
    (void)v;
    group_hold_report rep{};
    const unit_group &grp = own.players[player_id].ai_groups[group_index];

    if (grp.task_code == TASK_HOLD) { // CMP word [..+0x2a],0xb / JZ @0x004eb27b
        rep.already_holding = true;
        return rep;
    }
    // ECX = current_param (+0x0e) @0x004eb287; EBX = 0xb; the five stack arguments are all zero
    // (five PUSH 0 at 0x004eb27d-0x004eb285).
    gc.group_task_preempt(player_id, group_index, TASK_HOLD, grp.current_param, 0, 0, 0, 0, 0);
    rep.preempted = true;
    return rep;
}

group_hold_report group_seed_resolved_target(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, int32_t player_id,
                                             int32_t group_index) {
    (void)v;
    group_hold_report rep{};
    const unit_group &grp = own.players[player_id].ai_groups[group_index];

    if (grp.resolved_target_ref == 0) { // CMP dword [..+0x1e],0 / JZ @0x004ec772
        rep.no_target = true;
        return rep;
    }
    // EDX = resolved_target_ref, EBX = resolved_target_index, EAX = player @0x004ec774-0x004ec782.
    // The REF is the second argument and the INDEX the third; getting that pair round the wrong way
    // would still typecheck and would seed a target that does not exist.
    gc.scan_target_list_add(player_id, (uint32_t)grp.resolved_target_ref, grp.resolved_target_index);
    rep.seeded = true; // MOV EBX,1 @0x004ec787 -- EBX is the return register here
    return rep;
}

} // namespace detail

void group_enter_hold(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_enter_hold(st.read, st.own, live_calls(), player_id, group_index);
}

uint8_t group_seed_resolved_target(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    return detail::group_seed_resolved_target(st.read, st.own, live_calls(), player_id, group_index)
                   .seeded
               ? (uint8_t)1
               : (uint8_t)0;
}

// ---- the differential-oracle arms ---------------------------------------------------------------

} // namespace mh::ai
