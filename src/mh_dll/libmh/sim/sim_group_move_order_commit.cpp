#include "sim/sim_group_move_order_commit.h"
#include "sim/sim_stack_guard.h"    // LIFT-TABLE S5: the stack probe is libmh-internal, not a host service
#include "sim/sim_move_fixes.h"     // mods:M1H the planner keys' switchboard (move_fix())
#include "sim/move/arrival.h"       // mods:M1H [net] move_distinct_goals
#include "sim/move/formation.h"     // mods:M1G 4b [net] move_formation_click
#include "sim/move/route_through.h" // mods:M1H [net] move_route_through_idle
#include "sim/move/planner_hits.h"  // mods:M1H the twins' hit counters

#include <cstring>

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

namespace {

inline uint32_t flat_x(uint32_t flat) { return flat >> 8; }
inline uint32_t flat_y(uint32_t flat) { return flat & 0xffu; }

// mods:M1H: move/route_through.h's World over the sim_view / sim_store (the twin of the live-memory accessor in the retail splice,
// mh/seams/move_proto_nudge.cpp).
struct commit_rt_world {
    const sim_view &v;
    sim_store      &own;
    int32_t         unit_count() const { return v.caps.units; }
    bool            unit(uint32_t player, int32_t i, move::rt_unit *out) {
        if (player >= 8 || i < 0 || i >= v.caps.units) return false;
        const sim::unit &u = unit_of(v, player, i);
        const cfg_unit  &c = v.cfg_units[u.unit_proto_id];
        uint64_t         e = 0; // energy > 0.0 read as raw bits (no x87 / SSE use), as the retail splice does
        std::memcpy(&e, &u.energy, sizeof(e));
        out->state                    = u.state;
        out->order_queued             = u.order_queued;
        out->move_microstep           = u.move_microstep;
        out->origin_tile_was_passable = u.origin_tile_was_passable;
        out->cfg_ground_idle          = c.move_op_code == 0x0f && c.default_op_code == move::RT_ST_IDLE;
        out->cfg_ground_move          = c.move_op_code == 0x0f;
        out->alive                    = (e >> 63) == 0 && (e << 1) != 0;
        out->x                        = u.x;
        out->y                        = u.y;
        return true;
    }
    uint16_t  tile_building(uint32_t flat) { return tile_at(v, static_cast<int32_t>(flat_x(flat)), static_cast<int32_t>(flat_y(flat))).building; }
    uint8_t   tile_class_owner(uint32_t flat) { return tile_at(v, static_cast<int32_t>(flat_x(flat)), static_cast<int32_t>(flat_y(flat))).class_owner; }
    uint8_t  &passable(uint32_t flat) { return own.passable_at(static_cast<int32_t>(flat_x(flat)), static_cast<int32_t>(flat_y(flat))); }
    uint32_t &cell_flags(uint32_t flat) { return own.region_cell_at(static_cast<int32_t>(flat_x(flat)), static_cast<int32_t>(flat_y(flat))).terrain_flags; }
};

// mods:M1H: move/arrival.h's World over the sim_view / sim_store / the commit's calls (the twin of the live-memory accessor in the
// retail splice, mh/seams/move_proto_arrival.cpp).
bool commit_rt_enabled(const move_fixes &mf) { return (mf.route_through_idle && mf.nudge_idle) || mf.route_through_moving || mf.route_through_replanning; }

// [net] move_route_through_*: lifts the tiles of idle / walking / re-planning friends for ONE planner call (the twin of the retail splice's
// rt_unmask(), mh/seams/move_proto_nudge.cpp). Used for the commit's own planner call AND (mods:M1DT fix A') for 2a's re-run of it.
void commit_rt_open(const sim_view &v, sim_store &own, const move_fixes &mf, int32_t player_id, move::rt_undo &rt_undo) {
    const bool rt_idle   = mf.route_through_idle && mf.nudge_idle;
    const bool rt_moving = mf.route_through_moving;     // mods:M1N 3e-6: walking friends are open ground too (needs no other key)
    const bool rt_replan = mf.route_through_replanning; // mods:M1DT fix 2: re-planning (state 0x0a) friends are open ground too
    if (rt_idle) move::planner_hit(move::PH_RT);
    if (rt_moving) move::planner_hit(move::PH_RTM);
    if (rt_replan) move::planner_hit(move::PH_RTR);
    commit_rt_world w{v, own};
    const int       opened = move::rt_unmask(w, rt_undo, rt_idle ? 1 : 0, true, static_cast<uint32_t>(player_id), own.group_order_goal_x_mut(),
                                             own.group_order_goal_y_mut(), map_width_mask(v), map_height_mask(v));
    if (rt_moving) {
        const int  opened_mv = move::rt_unmask_moving(w, rt_undo, mf.route_through_moving_cost, static_cast<uint32_t>(player_id),
                                                      own.group_order_goal_x_mut(), own.group_order_goal_y_mut(), map_width_mask(v), map_height_mask(v));
        static int logged_mv = 0;
        if (opened_mv > 0 && logged_mv++ < 200)
            mh::ai::ai_say("; M1N route-through p%u walking friends opened=%d cost=%d goal=%d,%d (%d)\n", static_cast<unsigned>(player_id), opened_mv,
                           mf.route_through_moving_cost, static_cast<int>(static_cast<uint32_t>(own.group_order_goal_x_mut()) & map_width_mask(v)),
                           static_cast<int>(static_cast<uint32_t>(own.group_order_goal_y_mut()) & map_height_mask(v)), logged_mv);
    }
    if (rt_replan) {
        const int  opened_rp = move::rt_unmask_replanning(w, rt_undo, mf.route_through_moving_cost, static_cast<uint32_t>(player_id),
                                                          own.group_order_goal_x_mut(), own.group_order_goal_y_mut(), map_width_mask(v), map_height_mask(v));
        static int logged_rp = 0;
        if (opened_rp > 0 && logged_rp++ < 200)
            mh::ai::ai_say("; M1DT route-through p%u replanning friends opened=%d cost=%d goal=%d,%d (%d)\n", static_cast<unsigned>(player_id), opened_rp,
                           mf.route_through_moving_cost, static_cast<int>(static_cast<uint32_t>(own.group_order_goal_x_mut()) & map_width_mask(v)),
                           static_cast<int>(static_cast<uint32_t>(own.group_order_goal_y_mut()) & map_height_mask(v)), logged_rp);
    }
    if (opened > 0) {
        static int logged = 0;
        if (logged++ < 200)
            mh::ai::ai_say("; M1N route-through p%u idle friends opened=%d goal=%d,%d (%d)\n", static_cast<unsigned>(player_id), opened,
                           static_cast<int>(static_cast<uint32_t>(own.group_order_goal_x_mut()) & map_width_mask(v)),
                           static_cast<int>(static_cast<uint32_t>(own.group_order_goal_y_mut()) & map_height_mask(v)), logged);
    }
}

struct commit_arrival_world {
    const sim_view                      &v;
    sim_store                           &own;
    const group_move_order_commit_calls &c;
    const move_fixes                    &mf; // mods:M1DT fix A': the re-run opens the route-through bracket the keys describe
    int32_t                              player_id;
    const move::rt_undo                 *undo = nullptr; // mods:M1G 4b: the OPEN bracket's list (the formation's true_passable)
    uint32_t                             path_mask() { return *v.path_wrap_mask; }
    uint32_t                             owner() { return static_cast<uint32_t>(*v.group_order_owner); }
    int32_t                              member_count() { return *v.group_member_count; }
    int32_t                              unit_limit() { return v.caps.units; }
    uint32_t                             goal_x() { return static_cast<uint32_t>(own.group_order_goal_x_mut()); }
    uint32_t                             goal_y() { return static_cast<uint32_t>(own.group_order_goal_y_mut()); }
    void                                 set_goal(int32_t x, int32_t y) {
        own.group_order_goal_x_mut() = x;
        own.group_order_goal_y_mut() = y;
    }
    uint32_t anchor_x() { return static_cast<uint32_t>(*v.group_anchor_x); }
    uint32_t anchor_y() { return static_cast<uint32_t>(*v.group_anchor_y); }
    uint32_t centroid_x() { return static_cast<uint32_t>(*v.group_centroid_x); }
    uint32_t centroid_y() { return static_cast<uint32_t>(*v.group_centroid_y); }
    uint32_t member_handle(int32_t m) { return v.group_members[m].unit_handle; }
    uint8_t  member_col(int32_t m) { return v.group_members[m].cur_col; }
    uint8_t  member_row(int32_t m) { return v.group_members[m].cur_row; }
    uint8_t  tile_byte(int32_t m, int k) { return v.group_member_tile[m * 2 + k]; }
    void     set_tile_byte(int32_t m, int k, uint8_t b) { own.group_member_tile_byte(m * 2 + k) = b; }
    uint8_t  passable(uint32_t t) { return v.passable[t]; }
    uint8_t  fog(uint32_t t) { return own.fog_discovered_at(static_cast<int32_t>(flat_x(t)), static_cast<int32_t>(flat_y(t))); }
    uint32_t region_of(uint32_t t) {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(own.region_cell_at(static_cast<int32_t>(flat_x(t)), static_cast<int32_t>(flat_y(t))).region));
    }
    uint32_t field_of(uint32_t t) { return own.region_cell_at(static_cast<int32_t>(flat_x(t)), static_cast<int32_t>(flat_y(t))).terrain_flags >> 8; }
    bool     unit(uint32_t owner_, uint32_t i, move::arr_unit *out) {
        if (owner_ >= 8 || i >= static_cast<uint32_t>(v.caps.units)) return false;
        const sim::unit &u = unit_of(v, owner_, static_cast<int32_t>(i));
        out->order         = u.order;
        out->state         = u.state;
        out->goal_x        = u.goal_x;
        out->goal_y        = u.goal_y;
        out->group         = u.move_group_id;
        out->x             = u.x;
        out->y             = u.y;
        out->path_slot     = u.path_slot_id;
        return true;
    }
    void set_unit_goal(uint32_t owner_, uint32_t i, uint8_t x, uint8_t y) {
        sim::unit &u = own.unit_at(owner_, static_cast<int32_t>(i));
        u.goal_x     = x;
        u.goal_y     = y;
    }
    // The planner through the commit's own table, inside the route-through bracket (mods:M1DT fix A', as in the retail splice): the friends
    // the keys open for the first run are open for the re-run too. No key on = no bracket.
    void rerun_pathfind(uint32_t mode, uint32_t target) {
        const bool    rt = commit_rt_enabled(mf);
        move::rt_undo ru;
        if (rt) commit_rt_open(v, own, mf, player_id, ru);
        c.group_move_order_pathfind(static_cast<int32_t>(mode), static_cast<uint8_t>(target));
        if (rt) {
            commit_rt_world w{v, own};
            move::rt_restore(w, ru);
        }
    }
    // ---- mods:M1G 4b: move/formation.h's World (the twin of formation_live_world in mh/seams/move_proto_arrival.cpp) ----
    bool true_passable(uint32_t t) {
        if (undo)
            for (int k = 0; k < undo->n; ++k)
                if (undo->flat[k] == t) return undo->pass[k] != 0;
        return v.passable[t] != 0;
    }
    int32_t occupant(uint32_t owner_, uint32_t t) {
        const auto &o = tile_at(v, static_cast<int32_t>(flat_x(t)), static_cast<int32_t>(flat_y(t)));
        if ((o.class_owner & 0x80u) == 0 || o.building == 0) return 0;
        return (o.class_owner & 0xfu) == owner_ ? static_cast<int32_t>(o.building) : -1;
    }
    void set_member_count(int32_t n) { own.group_member_count_mut() = n; }
    void set_member(int32_t m, uint32_t handle, uint8_t col, uint8_t row) {
        group_member &g = own.group_member_at(m);
        g.unit_handle   = handle;
        g.cur_col       = col;
        g.cur_row       = row;
    }
    void set_centroid(uint32_t x, uint32_t y) {
        own.group_centroid_x_mut() = static_cast<int32_t>(x);
        own.group_centroid_y_mut() = static_cast<int32_t>(y);
    }
    void     run_pathfind(uint32_t mode, uint32_t target) { c.group_move_order_pathfind(static_cast<int32_t>(mode), static_cast<uint8_t>(target)); } // the bracket is open
    uint32_t path_first_run(uint32_t owner_, uint32_t slot) { return own.path_buffer_at(owner_, static_cast<int32_t>(slot), 0).run_length; }
    void     path_terminate(uint32_t owner_, uint32_t slot) {
        path_waypoint &wp = own.path_buffer_at(owner_, static_cast<int32_t>(slot), 0);
        wp.run_length     = 0;
        wp.heading        = 0;
    }
    void log_assign(uint32_t owner_, uint32_t ui, uint32_t from, uint32_t to, int how) {
        static int lines;
        if (lines++ >= 400) return;
        mh::ai::ai_say("; M1G FC assign p%u u%u (%u,%u) -> (%u,%u) %s\n", owner_, ui, from >> 8, from & 0xffu, to >> 8, to & 0xffu, how == 1 ? "kept" : (how == 2 ? "moved" : "retail"));
    }
    void log_adjust(uint32_t owner_, uint32_t ui, uint32_t wanted, uint32_t got) {
        static int lines;
        if (lines++ >= 200) return;
        mh::ai::ai_say("; M1G FC adjust p%u u%u wanted (%u,%u) got (%u,%u)\n", owner_, ui, wanted >> 8, wanted & 0xffu, got >> 8, got & 0xffu);
    }
    void log_fc(uint32_t owner_, int n, int kept, int filled, int retail, int rk, int replans, int reused) {
        static int lines;
        if (lines++ >= 200) return;
        mh::ai::ai_say("; M1G FC p%u n=%d kept=%d filled=%d retail=%d rk=%d replans=%d reused=%d\n", owner_, n, kept, filled, retail, rk, replans, reused);
    }
    void log_single(uint32_t owner_, uint32_t ui, uint32_t gx, uint32_t gy, bool moved, uint32_t tx, uint32_t ty) {
        mh::ai::ai_say("; arrival (mods:M1X) p%u u%u single: goal (%u,%u) taken/blocked -> %s (%u,%u)\n", owner_, ui, gx, gy,
                       moved ? "moved to" : "no free tile, kept", tx, ty);
    }
    void log_wave(uint32_t owner_, int32_t n, uint32_t gx, uint32_t gy, bool fresh, int kept, int moved, int failed) {
        mh::ai::ai_say("; arrival (mods:M1X) p%u wave n=%d goal (%u,%u) field %s: kept %d, moved %d, no free tile %d\n", owner_, n, gx, gy,
                       fresh ? "fresh" : "stale", kept, moved, failed);
    }
};

} // namespace

void group_move_order_commit(const sim_view &v, sim_store &own, const group_move_order_commit_calls &c,
                             int32_t goal_x, int32_t goal_y, int32_t player_id, int32_t member_count,
                             int32_t move_group_id, uint32_t is_plain_move_flag, uint32_t target_class) {
    // move_group_id (0x0041c88a-0x0041c890): loaded into a local at entry, never read by the ORIGINAL again -- the function's own
    // plate comment documents it as dead. The [net] move_distinct_goals twin below reads it (it is the retail splice's `group_id`).

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

    // [net] move_route_through_idle / _moving / _replanning (mods:M1H; twin of the retail splice at 0x0041c9ce, sim/move/route_through.h): the
    // planner sees friends' tiles as open for exactly this ONE call (idle needs the nudge key: the nudge / trade resolves the meeting).
    // Mode 1 only (the penalty variant the preset ships); the retail splice's measurement mode 2 has no twin. The bracket code is
    // commit_rt_open() above; 2a's re-run opens the same bracket (mods:M1DT fix A').
    const move_fixes &mf = move_fix();
    const bool        rt = commit_rt_enabled(mf);
    move::rt_undo     rt_undo;
    if (rt) commit_rt_open(v, own, mf, player_id, rt_undo);
    // 0x0041c9c8-0x0041c9ce: register order EAX=is_plain_move_flag, EDX=target_class, matching
    // mh_calls.gen.h's (int32_t mode, uint8_t target_mask) parameter order.
    c.group_move_order_pathfind(static_cast<int32_t>(is_plain_move_flag), target_mask);

    // [net] move_formation_click (mods:M1G 4b; twin of the retail splice, the stub at 0x0041c9ce after the planner CALL): inside the still-open
    // bracket, after the unchanged retail plan. Handled = 2a below is skipped for this commit.
    bool fc_handled = false;
    if (mf.formation_click > 0) {
        move::planner_hit(move::PH_FC);
        commit_arrival_world w{v, own, c, mf, player_id, rt ? &rt_undo : nullptr};
        fc_handled = move::formation_after_pathfind(w, move_group_id, is_plain_move_flag, static_cast<uint32_t>(target_mask), mf.formation_click - 1);
    }
    if (rt) {
        commit_rt_world w{v, own};
        move::rt_restore(w, rt_undo);
    }

    // [net] move_distinct_goals (mods:M1H; twin of the retail splice at 0x0041c9d3, sim/move/arrival.h): right after the planner
    // returned, before the writeback below. Its single-member re-run of the planner opens the route-through bracket again (mods:M1DT fix A').
    if (mf.distinct_goals && !fc_handled) {
        move::planner_hit(move::PH_ARR_COMMIT);
        commit_arrival_world w{v, own, c, mf, player_id};
        move::arrival_after_pathfind(w, move_group_id, is_plain_move_flag, static_cast<uint32_t>(target_mask));
    }

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
