//
// ai/ai_group_split_off.cpp -- see ai_group_split_off.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_split_off_create_004e68dd.asm) -- the exported .c draft agrees with
// it instruction-for-instruction here (checked field offset by field offset against
// addr/mh_structs.gen.h's mh_llm_strat_ai_unit_group static_asserts), so this translation follows
// both.
//
#include "ai/ai_group_split_off.h"


namespace mh::ai {
namespace detail {

group_split_off_report group_split_off_create(const ai_view &v, const ai_store &own,
                                              const ai_calls &gc, uint32_t player,
                                              int32_t centroid_x, int32_t centroid_y,
                                              int32_t source_group_idx, uint32_t member_count) {
    group_split_off_report rep{};

    const player_data &pd = v.players[player];

    // The four-part AND-chain, in the ORIGINAL's order (0x004e690b-0x004e6943): each part exits to
    // the same epilogue on failure, so testing them in sequence with early returns is equivalent to
    // the original's single chained condition, not a restructuring of it.
    //
    // Part 1: ai_groups[3] is the FIXED SLOT-3 SHARED POOL (not source_group_idx's own count), and
    // both sides are read/compared UNSIGNED -- the field is signed int16_t in the struct, but the
    // original MOVZXes it (0x004e690b) before the CMP, matching the ushort cast in the exported
    // decompile.
    if (member_count > (uint16_t)pd.ai_groups[3].member_count) {
        rep.gate_failed = true;
        return rep;
    }
    // Part 2.
    if (member_count == 0) {
        rep.gate_failed = true;
        return rep;
    }
    const auto &src = pd.ai_groups[source_group_idx];
    // Parts 3 and 4: source_group_idx must not already be a split-off sub-group (goal 7) or goal 8.
    if (src.goal == 7 || src.goal == 8) {
        rep.gate_failed = true;
        return rep;
    }

    // Part 5: no existing linked splinter already covers source_group_idx.
    if (gc.group_has_split_group_link((int32_t)player, (uint32_t)source_group_idx) != 0) {
        rep.link_exists = true;
        return rep;
    }

    // Part 6: a free group slot must exist.
    const int32_t new_group = gc.group_create((int32_t)player);
    if (new_group == -1) {
        rep.create_failed = true;
        return rep;
    }

    // Stamp the new group. All three stores are 16-bit in the original (0x004e6970-0x004e6986),
    // truncating source_group_idx and member_count to the fields' uint16_t width -- reproduced, not
    // widened.
    auto &new_grp               = own.players[player].ai_groups[new_group];
    new_grp.link_target_group   = (uint16_t)source_group_idx;
    new_grp.goal                = 7;
    new_grp.active_member_count = (uint16_t)member_count;

    // The gather/move/idle task sequence, args exactly as pushed at 0x004e698a-0x004e69df. param_5/
    // param_6/param_9 are group_task_enqueue's STACK arguments (see the ai_calls comment on it for
    // the register/stack split); -1/-1 for the gather task means "centroid unset", matching the
    // original's literal -1 pushes rather than a derived value.
    gc.group_task_enqueue((int32_t)player, new_group, /*task_code=*/0x12, /*param_4=*/0x183,
                          /*param_5=*/0xffffffffu, /*param_6=*/0xffffffffu, /*param_7=*/0,
                          /*param_8=*/0, /*param_9=*/(uint16_t)member_count);
    gc.group_task_enqueue((int32_t)player, new_group, /*task_code=*/3, /*param_4=*/0x187,
                          /*param_5=*/(uint32_t)centroid_x, /*param_6=*/(uint32_t)centroid_y,
                          /*param_7=*/0, /*param_8=*/0, /*param_9=*/0);
    gc.group_task_enqueue((int32_t)player, new_group, /*task_code=*/8, /*param_4=*/0x183,
                          /*param_5=*/0, /*param_6=*/0, /*param_7=*/0, /*param_8=*/0,
                          /*param_9=*/0);

    rep.created = true;
    return rep;
}

} // namespace detail

void group_split_off_create(uint32_t player, int32_t centroid_x, int32_t centroid_y,
                            int32_t source_group_idx, uint32_t member_count) {
    const ai_state st = state();
    (void)detail::group_split_off_create(st.read, st.own, live_calls(), player, centroid_x,
                                         centroid_y, source_group_idx, member_count);
}


} // namespace mh::ai
