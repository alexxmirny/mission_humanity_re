#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// One report type for the two arms with anything worth counting (disband, recall_home). hold /
// rally_formup / scatter_random stay void, like ai_group_task_attack.h's engage_target -- there is
// nothing here for a shadow arm to aggregate beyond the call count it already tracks itself.
struct group_task_lifecycle_report {
    int32_t members_disbanded = 0; // disband only: members processed by the drain loop
    int32_t queue_drained     = 0; // recall_home only: queued sub-tasks dequeued before re-enqueuing
};

// llm_strat_ai_group_task_disband @0x004eaaa4. Task code 0x14 (AI_GROUP_TASK_DISBAND in ai_state.h).
group_task_lifecycle_report group_task_disband(const ai_view &v, const ai_store &own,
                                               const ai_calls &gc, uint32_t player_id,
                                               int32_t group_index);
// llm_strat_ai_group_task_hold @0x004eab33. Task code 0xb. NO PARAMETERS -- the committed prototype
// (mh_export.gen.h's sig_llm_strat_ai_group_task_hold) is void(void); the whole body is the inert
// stack-capacity probe.
void group_task_hold(const ai_view &v, const ai_store &own, const ai_calls &gc);
// llm_strat_ai_group_task_recall_home @0x004eab3e. Task code 8.
group_task_lifecycle_report group_task_recall_home(const ai_view &v, const ai_store &own,
                                                   const ai_calls &gc, int32_t player_id,
                                                   int32_t group_index);
// llm_strat_ai_group_task_rally_formup @0x004eaea5. Task code 2. Tail-call thunk.
void group_task_rally_formup(const ai_view &v, const ai_store &own, const ai_calls &gc,
                             int32_t player_id, int32_t group_index);
// llm_strat_ai_group_task_scatter_random @0x004eaeb4. Task code 0xd. Forwarding thunk (class/radius
// argument fixed at 0xd, matching the task's own code).
void group_task_scatter_random(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               int32_t player_id, int32_t group_index);

} // namespace detail

void group_task_disband(uint32_t player_id, int32_t group_index);
void group_task_hold();
void group_task_recall_home(int32_t player_id, int32_t group_index);
void group_task_rally_formup(int32_t player_id, int32_t group_index);
void group_task_scatter_random(int32_t player_id, int32_t group_index);

} // namespace mh::ai
