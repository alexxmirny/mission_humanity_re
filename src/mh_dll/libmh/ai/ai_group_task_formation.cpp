//
// ai/ai_group_task_formation.cpp -- see ai_group_task_formation.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_task_{advance_to_anchor_004eaf72,disperse_passable_004eb02e,
// wait_004eaf67}.asm and llm_strat_ai_group1_drain_to_group0_004eb2a0.asm), not from Ghidra's .c.
//
#include "ai/ai_group_task_formation.h"


namespace mh::ai {
namespace detail {

namespace {

// The harvest both movers share (0x004eafb8-0x004eafe1 / 0x004eb06c-0x004eb095 -- the two loops are
// the same instructions with different registers holding player/group). Reset the count, then append
// each member id while following unit::ai_group_next, terminating on the 0 link.
//
// The STORE IS A DWORD of a ZERO-EXTENDED WORD (MOVZX then `MOV dword ptr [.. *4 ..]`), and the
// count is bumped AFTER the store and BEFORE the link is followed. Both details are load-bearing:
// the callee reads dword entries, and an id of 0 can never be stored because it is the loop's own
// termination test.
void harvest_members_into_scratch(const ai_view &v, const ai_store &own, uint32_t player_id,
                                  int32_t group_index) {
    *own.group_relocation_scratch_count = 0;
    uint32_t unit_id                    = v.players[player_id].ai_groups[group_index].head_unit;
    while (unit_id != 0) {
        own.group_relocation_scratch_list[*own.group_relocation_scratch_count] = (int32_t)unit_id;
        ++*own.group_relocation_scratch_count;
        unit_id = unit_of(v, player_id, unit_id).ai_group_next;
    }
}

} // namespace

void group_task_advance_to_anchor(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  int32_t player_id, int32_t group_index) {
    harvest_members_into_scratch(v, own, (uint32_t)player_id, group_index);

    // 0x004eafe3-0x004eafed: the out-pointers are ECX = [EBP-0x18] and EBX = [EBP-0x14], and the
    // committed prototype puts out_x in EBX / out_y in ECX -- so [EBP-0x14] receives X and
    // [EBP-0x18] receives Y.
    uint32_t centroid_x = 0, centroid_y = 0; // committed llm_strat_ai_group_compute_centroid out-params
    gc.group_compute_centroid(player_id, group_index, &centroid_x, &centroid_y);

    // 0x004eb00e-0x004eb024: EDX <- [EBP-0x14] (X) and EBX <- [EBP-0x18] (Y), i.e. the centroid pair
    // in that order; ECX <- active_param_a and the PUSHed stack slot <- active_param_b.
    const unit_group &grp = v.players[player_id].ai_groups[group_index];
    gc.group_move_formation_rotating((uint32_t)player_id, centroid_x, centroid_y, grp.active_param_a,
                                     grp.active_param_b);
    // No dequeue -- see the header. 0x004eb029 jumps PAST it into the bare epilogue.
}

void group_task_disperse_passable(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  uint32_t player_id, int32_t group_index) {
    harvest_members_into_scratch(v, own, player_id, group_index);

    // 0x004eb0b3-0x004eb0c3: EDX <- active_param_a, EBX <- active_param_b, EAX <- player.
    const unit_group &grp = v.players[player_id].ai_groups[group_index];
    gc.group_scatter_to_passable_tile(player_id, grp.active_param_a, grp.active_param_b);
}

void group_task_wait() {
    // Intentionally empty -- the original is the stack probe and a RET. See the header.
}

void group1_drain_to_group0(const ai_view &v, const ai_store &own, const ai_calls &gc,
                            uint32_t player_id) {
    (void)own;
    // 0x004eb2b0 recomputes the player base every iteration; both reads below are therefore fresh,
    // which is what makes the loop terminate (group_member_move decrements group 1's count).
    while (v.players[player_id].ai_groups[1].member_count != 0) {          // 0x004eb2c6 CMP word,0
        const uint32_t head = v.players[player_id].ai_groups[1].head_unit; // 0x004eb2d0 MOVZX
        gc.group_member_move(player_id, 1, 0, (int32_t)head);              // 0x004eb2e0
    }
}

} // namespace detail

void group_task_advance_to_anchor(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_advance_to_anchor(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_disperse_passable(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_disperse_passable(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_wait() { detail::group_task_wait(); }
void group1_drain_to_group0(uint32_t player_id) {
    const ai_state st = state();
    detail::group1_drain_to_group0(st.read, st.own, live_calls(), player_id);
}


} // namespace mh::ai
