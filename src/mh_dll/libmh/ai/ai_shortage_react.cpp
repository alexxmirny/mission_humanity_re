//
// ai/ai_shortage_react.cpp -- see ai_shortage_react.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_react_resource_shortage_004e571d.asm), not from Ghidra's C: the
// decompile's four `CONCAT31(extraout_varN, bVarN)` terms are the Watcom "bool return, upper 24
// bits of EAX undefined to the decompiler" idiom, but llm_strat_ai_bldg_type_already_queued's own
// assembly (0x004d3a93) shows BOTH exits set the FULL EAX register (`MOV EAX,0x1` / `XOR EAX,EAX`),
// so the ADD at each call site really is adding a clean 0/1 -- there is no garbage-bits ambiguity to
// carry into this translation.
//
#include "ai/ai_shortage_react.h"


namespace mh::ai {
namespace detail {

void react_resource_shortage(const ai_view &v, const ai_calls &gc, int32_t player) {
    const player_data &pd = v.players[player];

    // 0x004e5741-0x004e5760: the race-paired early exit. Pair is (1, 0x15) -- see the header on
    // why that is not one of the named race pairs.
    const uint32_t pending_probe_type = (pd.is_alien_race == 0) ? 0x15u : 1u;
    if (gc.bldg_type_queue_has_pending(player, pending_probe_type) != 0) return;

    // The four-slot fall-through chain, 0x004e5766-0x004e585a. `queued` is what makes later slots
    // skip once an earlier one fires -- in the assembly this is a direct jump to the shared queue
    // site that never re-enters the chain, not four independent tests.
    bool queued = false;

    // slot 1 (ai_resource_shortage_candidates[0] / ai_score_cat_0x10), 0x004e5766-0x004e57a5.
    {
        const int32_t  type = pd.ai_resource_shortage_candidates[0];
        const uint32_t sum =
            gc.bldg_type_already_queued(player, (uint32_t)type) + (uint32_t)pd.ai_score_cat_0x10;
        if (sum == 0 && pd.ai_building_type_available[type] == 1) {
            gc.bldg_queue_construction(player, type, -1, 0);
            queued = true;
        }
    }
    // slot 2 (candidates[1] / ai_score_cat_0x11), 0x004e57a5-0x004e57e4.
    if (!queued) {
        const int32_t  type = pd.ai_resource_shortage_candidates[1];
        const uint32_t sum =
            gc.bldg_type_already_queued(player, (uint32_t)type) + (uint32_t)pd.ai_score_cat_0x11;
        if (sum == 0 && pd.ai_building_type_available[type] == 1) {
            gc.bldg_queue_construction(player, type, -1, 0);
            queued = true;
        }
    }
    // slot 3 (candidates[2] / ai_score_cat_0x12), 0x004e57e4-0x004e581f.
    if (!queued) {
        const int32_t  type = pd.ai_resource_shortage_candidates[2];
        const uint32_t sum =
            gc.bldg_type_already_queued(player, (uint32_t)type) + (uint32_t)pd.ai_score_cat_0x12;
        if (sum == 0 && pd.ai_building_type_available[type] == 1) {
            gc.bldg_queue_construction(player, type, -1, 0);
            queued = true;
        }
    }
    // slot 4 (candidates[3] / ai_score_cat_0x13), 0x004e581f-0x004e585a. THE ASSEMBLY'S BRANCH
    // ENCODING IS INVERTED HERE (JNZ-to-tail at 0x004e5848/0x004e5858 instead of JZ-to-queue) but
    // the predicate is the same "eligible" test as slots 1-3 -- see the header.
    if (!queued) {
        const int32_t  type = pd.ai_resource_shortage_candidates[3];
        const uint32_t sum =
            gc.bldg_type_already_queued(player, (uint32_t)type) + (uint32_t)pd.ai_score_cat_0x13;
        if (sum == 0 && pd.ai_building_type_available[type] == 1) {
            gc.bldg_queue_construction(player, type, -1, 0);
        }
    }

    // LAB_004e586a, 0x004e5880-0x004e58c7: a SECOND, independent chance against
    // ai_build_candidate_cat_0x30, gated on five conditions. Runs UNCONDITIONALLY after the chain
    // above -- the queue CALL at 0x004e5865 falls straight through into this block in the assembly,
    // so queuing in the chain does not skip it.
    if (pd.ai_score_cat_0x10 != 0 && pd.ai_score_cat_0x20 != 0 &&
        gc.bldg_type_already_queued(player, (uint32_t)pd.ai_build_candidate_cat_0x30) == 0 &&
        pd.ai_score_cat_0x30 == 0 &&
        pd.ai_building_type_available[pd.ai_build_candidate_cat_0x30] == 1) {
        gc.bldg_queue_construction(player, pd.ai_build_candidate_cat_0x30, -1, 0);
    }
}

} // namespace detail

void react_resource_shortage(int32_t player) {
    const ai_state st = state();
    detail::react_resource_shortage(st.read, live_calls(), player);
}


} // namespace mh::ai
