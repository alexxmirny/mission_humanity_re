//
// ai/ai_group_hold.h -- two small AI unit-group helpers (RI-AI / AI1C layer 2).
//
// llm_strat_ai_group_enter_hold           @0x004eb245 (0x5b bytes)
// llm_strat_ai_group_seed_resolved_target @0x004ec73c (0x56 bytes)
//
// Neither writes a byte itself; each is a guarded single call, and the guard IS the function. They
// share a translation unit because they share that shape and because both index the group record
// with the identical player/group address arithmetic -- side by side, a transposed offset shows.
//
// enter_hold: if the group's LIVE task_code (+0x2a) is already 0xb, do nothing (CMP/JZ
// @0x004eb273); otherwise FRONT-INSERT a hold task via llm_strat_ai_group_task_preempt with
// task_code 0xb, pending_param = the group's current_param (+0x0e, loaded @0x004eb287) and all five
// stack arguments zero. Two things follow from "live task_code" rather than "the queue": a group
// with a hold already QUEUED but not yet activated still gets preempted, and a group holding for
// any other reason is left alone. Note also that preempt, not enqueue, is what makes 0xb the ACTIVE
// task -- the sibling llm_strat_ai_group_task_enqueue would have appended it behind whatever is
// running.
//
// seed_resolved_target: returns false and does nothing when resolved_target_ref (+0x1e) is zero
// (CMP/JZ @0x004ec772); otherwise appends (resolved_target_ref, resolved_target_index) to the
// GLOBAL scan-target scratch via llm_strat_ai_scan_target_list_add and returns true. NOTE WHICH
// ARRAY THAT IS: _G_LLM_STRAT_AI_SCAN_TARGETS, stride 0x16 -- NOT player_data's ai_target_list,
// whose adder is the similarly-named llm_strat_ai_target_list_add @0x004d6d67. The two were
// conflated once already (see the ai_calls comment on scan_target_list_add). It follows that this
// function's shadow site declares the two scratch regions and NOT player_data: player_data is read
// here and never written.
//
// NEITHER TESTS ITS GROUP INDEX. Both compute group_index * 0xa66 straight off the argument
// (0x004eb26b / 0x004ec763) with no bound against ai_group_count and no negative check, exactly
// like every other member of this family.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID. Both bodies are a guard over one call, so a guarded call writes nothing
// and compares clean vacuously. These four flags are what separate "the call arm ran" from "the
// site was reached".
struct group_hold_report {
    bool already_holding = false; // enter_hold: task_code was already HOLD, so nothing was done
    bool preempted       = false; // enter_hold: group_task_preempt was called
    bool no_target       = false; // seed_resolved_target: resolved_target_ref was 0
    bool seeded          = false; // seed_resolved_target: scan_target_list_add was called (= its
                                  // return value)
};

// llm_strat_ai_group_enter_hold @0x004eb245.
group_hold_report group_enter_hold(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   int32_t player_id, int32_t group_index);
// llm_strat_ai_group_seed_resolved_target @0x004ec73c. Returns the original's bool in `.seeded`.
group_hold_report group_seed_resolved_target(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, int32_t player_id,
                                             int32_t group_index);

} // namespace detail

void    group_enter_hold(int32_t player_id, int32_t group_index);
uint8_t group_seed_resolved_target(int32_t player_id, int32_t group_index);

} // namespace mh::ai
