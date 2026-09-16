#include "sim/sim_group_move_order_commit.h"
#include "sim/sim_stack_guard.h" // LIFT-TABLE S5: the stack probe is libmh-internal, not a host service

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const group_move_order_commit_calls &live_group_move_order_commit_calls() {
    static const group_move_order_commit_calls c = {
        MH_LIBMH_BIND(llm_strat_group_move_order_pathfind),
        mh::sim::stack_capacity_guard_noop,
    };
    return c;
}

namespace detail {

void group_move_order_commit(const sim_view &v, sim_store &own, const group_move_order_commit_calls &c,
                             int32_t goal_x, int32_t goal_y, int32_t player_id, int32_t member_count,
                             int32_t move_group_id, uint32_t is_plain_move_flag, uint32_t target_class) {
    (void)move_group_id; // 0x0041c88a-0x0041c890: loaded into a local at entry, never read again -- the
                         // function's own plate comment already documents this as dead.

    // 0x0041c894-0x0041c8a2: guard -- do nothing at all unless both hold.
    if (member_count == 0 || player_id == -1) return;

    // 0x0041c8a8-0x0041c8bc: seed the route with a terminal sentinel; refresh the wrap mask from the
    // live map width. WRITE ONLY -- this function does not read _G_LLM_STRAT_GROUP_ROUTE_STEPS itself.
    route_step &route0       = own.group_route_step_at(0);
    route0.dir_code          = 0;
    route0.run_length        = 0;
    own.path_wrap_mask_mut() = static_cast<uint32_t>(*v.map_width - 1);

    // 0x0041c8c1-0x0041c8dc: goal AND anchor both take (goal_x, goal_y) -- not a goal/anchor split.
    own.group_order_goal_x_mut() = goal_x;
    own.group_order_goal_y_mut() = goal_y;
    own.group_anchor_x_mut()     = goal_x;
    own.group_anchor_y_mut()     = goal_y;

    // 0x0041c8e1-0x0041c8f7: normalize target_class to the WEAPON_TARGET_* mask (1 stays GROUND,
    // anything else becomes AIR). See the .h banner: this reuses the existing WEAPON_TARGET_GROUND/AIR
    // constants (values 1/2 match exactly) per house rule 17a rather than a new local constant -- see
    // uncertainties[] for why this is flagged as an inferred domain match, not a literal transcription.
    // The intervening `CMP is_plain_move_flag,0` at 0x0041c8f7 sets flags nothing branches on -- dead,
    // omitted.
    const uint8_t target_mask = (target_class == 1) ? WEAPON_TARGET_GROUND : WEAPON_TARGET_AIR;

    own.group_member_count_mut() = member_count; // 0x0041c8fe-0x0041c903

    // 0x0041c90a-0x0041c9b6: per-member copy + impassable-start-tile check. The asm's loop condition
    // re-reads the GLOBAL group_member_count on every iteration, but nothing between here and the loop
    // can change it, so bounding on the local `member_count` is value-identical.
    int32_t bad_member_count = 0;
    for (int32_t i = 0; i < member_count; ++i) {
        const group_scratch_member &scratch = v.group_move_scratch[i];

        group_member &m = own.group_member_at(i);
        m.unit_handle   = static_cast<uint32_t>(scratch.unit_idx); // full dword copy
        if (scratch.unit_idx < 1) ++bad_member_count;              // 0x0041c93a-0x0041c948
        m.cur_col = static_cast<uint8_t>(scratch.tile_col);        // low byte only (0x0041c951-0x0041c957)
        m.cur_row = static_cast<uint8_t>(scratch.tile_row);        // low byte only (0x0041c965-0x0041c96b)

        // 0x0041c971-0x0041c98a: (tile_col<<8)|tile_row, the codebase's standard passable index.
        if (v.passable[(scratch.tile_col << 8) | scratch.tile_row] == 0) {
            // 0x0041c98a-0x0041c9ac: this member's own start tile is impassable -- abort the WHOLE
            // commit immediately (not just this member), after the partial writes above already landed.
            c.stack_capacity_guard_0x20();
            return;
        }
    }

    if (bad_member_count != 0) return; // 0x0041c9b6-0x0041c9ba: any unit_idx<1 -- bail without committing.

    own.group_order_owner_mut() = player_id; // 0x0041c9c0-0x0041c9c3
    // 0x0041c9c8-0x0041c9ce: register order EAX=is_plain_move_flag, EDX=target_class, matching
    // mh_calls.gen.h's (int32_t mode, uint8_t target_mask) parameter order.
    c.group_move_order_pathfind(static_cast<int32_t>(is_plain_move_flag), target_mask);

    // 0x0041c9d3-0x0041ca2e: only for a plain (non-attack) MULTI-member move, write the planner's
    // per-member destination tiles back into the scratch rows (short-circuit member_count>1 THEN
    // is_plain_move_flag!=0, matching the asm's JLE-then-JNZ chain).
    if (member_count > 1 && is_plain_move_flag != 0) {
        for (int32_t i = 0; i < member_count; ++i) {
            group_scratch_member &scratch = own.group_move_scratch_at(i);
            scratch.tile_col              = v.group_member_tile[i * 2 + 0];
            scratch.tile_row              = v.group_member_tile[i * 2 + 1];
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void group_move_order_commit(int32_t goal_x, int32_t goal_y, int32_t player_id, int32_t member_count,
                             int32_t move_group_id, uint32_t is_plain_move_flag, uint32_t target_class) {
    sim_state st = state();
    detail::group_move_order_commit(st.read, st.own, live_group_move_order_commit_calls(), goal_x, goal_y,
                                    player_id, member_count, move_group_id, is_plain_move_flag,
                                    target_class);
}


} // namespace mh::sim
