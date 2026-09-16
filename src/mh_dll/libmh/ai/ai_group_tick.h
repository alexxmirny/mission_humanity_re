//
// ai/ai_group_tick.h -- the AI unit-group task machine's per-player driver (RI-AI / AI1C layer 1).
//
// llm_strat_ai_unit_group_tick @0x004eb2ec, 324 bytes, one caller -- one of llm_strat_ai_players_tick's
// three phase-timer slots. It owns no logic of its own: it is a SCHEDULER, and everything it decides
// is WHICH group gets touched and HOW MANY TIMES. That makes it small and its control flow the whole
// specification, which is why each of the four loop shapes below is called out.
//
// TWO GATES, both early returns:
//   (1) _G_LLM_STRAT_PLAYERS[player].status_flags & 0x2 -- ALIVE (0x004eb302). The SESSION record,
//       not player_data.
//   (2) player_data[player].ai_phase_flags & 0x4 -- the unit-group phase-enable bit (0x004eb325).
//
// PASS 1, THE REAPER. Scans groups from index 5 (MOV EBX,0x5 @0x004eb338) -- slots 0..4 are
// llm_strat_spawn_ai_base's seed groups and are never reaped, the same floor
// llm_strat_ai_notify_object_removed enforces @0x004db8ee. It removes a group whose member_count is
// 0 AND whose live task_code is 9 (recruit-from-storage) or 0x14 (disband). AFTER EVERY REMOVAL THE
// SCAN RESTARTS AT 5, because llm_strat_ai_group_remove compacts the last group into the freed slot
// and every index above it shifts; the pass ends only when a whole sweep removes nothing. A
// reimplementation that instead continued from the current index would silently skip the group that
// moved down -- the same class of mistake target_list_remove makes ON PURPOSE and this one does not.
//
// PASS 2, THE TASK MACHINE. For every group 0..count (no floor here -- the seed groups DO get
// ticked), while its task_queue_count is non-zero:
//   * activate it if active_flag is clear (llm_strat_ai_group_task_activate), then RE-TEST the flag
//     and re-enter the same group's body from the top while it is still clear;
//   * once set, call llm_strat_ai_group_task_step and re-enter the same group's body while it
//     returns non-zero. ONLY A ZERO STEP RESULT ADVANCES TO THE NEXT GROUP -- the return is a
//     "run me again now" request, not a success flag.
// Both re-entries go to the TOP of the body, so task_queue_count is re-tested on every pass: a
// handler that drains the queue ends the group's turn without the step result having to say so.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// recording call stubs -- no game and no rig. The wrapper below is this applied to state().
namespace detail {

// WHAT A CALL ACTUALLY DID. This function delegates everything, so a shadow site's `calls` and even
// its divergence count say almost nothing on their own: a player whose ai_phase_flags bit is clear
// returns at gate 2 having read two bytes, and that is indistinguishable in the compared region from
// a player with no groups. These fields are what separate them.
struct group_tick_report {
    bool    alive_gate     = false; // passed gate 1
    bool    phase_gate     = false; // passed gate 2 (i.e. the body ran)
    int32_t disband_sweeps = 0;     // pass-1 sweeps started; always >= 1 once the body runs
    int32_t disbands       = 0;     // group_remove calls (== disband_sweeps - 1, by construction)
    int32_t groups_ticked  = 0;     // groups whose pass-2 body ran at least once
    int32_t bodies         = 0;     // pass-2 body entries, including re-entries
    int32_t idle_groups    = 0;     // bodies that returned at the task_queue_count == 0 test
    int32_t activates      = 0;     // group_task_activate calls
    int32_t reactivations  = 0;     // re-entries because active_flag was STILL clear after activate
    int32_t steps          = 0;     // group_task_step calls
    int32_t step_reentries = 0;     // re-entries because the step asked to be run again
};

// llm_strat_ai_unit_group_tick @0x004eb2ec.
group_tick_report unit_group_tick(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  int32_t player);

} // namespace detail

void unit_group_tick(int32_t player);

} // namespace mh::ai
