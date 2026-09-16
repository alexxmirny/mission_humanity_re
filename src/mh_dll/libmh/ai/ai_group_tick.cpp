//
// ai/ai_group_tick.cpp -- see ai_group_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp_c1/llm_strat_ai_unit_group_tick_004eb2ec.asm), not from Ghidra's C. The draft is
// unusually close here, but it renders pass 2's inner control flow as a `do { do { ... goto } }`
// nest with a label inside the for-body, which reads as three loops where the original has one body
// with two re-entry edges -- and it prints the pass-1 restart as a `break` out of a `do/while`,
// which hides that the restart re-reads ai_group_count.
//
#include "ai/ai_group_tick.h"


namespace mh::ai {
namespace detail {

group_tick_report unit_group_tick(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  int32_t player) {
    group_tick_report rep{};

    // ---- the two gates (0x004eb2fc-0x004eb32d) --------------------------------------------------
    //
    // Both are byte TESTs on a wider field, and both are read fresh -- nothing here caches a player
    // record across the call.
    if ((v.strat_players[player].status_flags & PLAYER_STATUS_ALIVE) == 0) return rep;
    rep.alive_gate = true;
    if ((v.players[player].ai_phase_flags & AI_PHASE_UNIT_GROUPS) == 0) return rep;
    rep.phase_gate = true;

    player_data &pd = own.players[player];

    // ---- PASS 1: reap empty groups, restarting after every removal (0x004eb333-0x004eb393) -------
    //
    // `swept_clean` is the original's ESI: set 1 at the top of each sweep, cleared by a removal, and
    // tested at 0x004eb391 to decide whether to sweep again. The count is re-read on every iteration
    // AND on every restart, which matters because group_remove decrements it.
    for (bool swept_clean = false; !swept_clean;) {
        swept_clean = true;
        ++rep.disband_sweeps;
        for (uint32_t g = (uint32_t)AI_SEED_GROUP_COUNT; g < (uint32_t)pd.ai_group_count; ++g) {
            const unit_group &grp = pd.ai_groups[g];
            if (grp.member_count != 0) continue;
            if (grp.task_code != AI_GROUP_TASK_RECRUIT_FROM_STORAGE &&
                grp.task_code != AI_GROUP_TASK_DISBAND)
                continue;

            gc.group_remove(player, g);
            ++rep.disbands;
            swept_clean = false; // XOR ESI,ESI @0x004eb36e -- restart the sweep from the floor
            break;
        }
    }

    // ---- PASS 2: the task machine (0x004eb395-0x004eb425) ---------------------------------------
    //
    // NOTE THE FLOOR IS GONE: this pass starts at 0, so the five seed groups are ticked even though
    // pass 1 refuses to reap them.
    for (uint32_t g = 0; g < (uint32_t)pd.ai_group_count; ++g) {
        bool counted = false;
        // The body, with its two re-entry edges. Every path back here re-reads the group record --
        // that is the point of writing it as one loop rather than as nested ones.
        for (;;) {
            ++rep.bodies;
            unit_group &grp = pd.ai_groups[g];

            // An idle group (no queued task) ends its turn immediately (JZ @0x004eb3c2).
            if (grp.task_queue_count == 0) {
                ++rep.idle_groups;
                break;
            }
            if (!counted) {
                counted = true;
                ++rep.groups_ticked;
            }

            if (grp.active_flag == 0) {
                gc.group_task_activate(player, (int32_t)g);
                ++rep.activates;
            }

            // RE-READ the flag through the record rather than reusing what was tested above: the
            // original reloads it at 0x004eb3f2 after the call, and activation is what sets it.
            if (pd.ai_groups[g].active_flag == 0) {
                ++rep.reactivations;
                continue; // JZ @0x004eb3fa -> body top
            }

            ++rep.steps;
            if (gc.group_task_step((uint32_t)player, (int32_t)g) != 0) {
                ++rep.step_reentries;
                continue; // JNZ @0x004eb407 -> body top
            }
            break; // only a ZERO step result advances to the next group
        }
    }

    return rep;
}

} // namespace detail

void unit_group_tick(int32_t player) {
    const ai_state st = state();
    (void)detail::unit_group_tick(st.read, st.own, live_calls(), player);
}

// ---- the differential-oracle arm ---------------------------------------------------------------

} // namespace mh::ai
