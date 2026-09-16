//
// sim/sim_unit_state_enter.cpp -- see sim_unit_state_enter.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_state_enter_arrival_check_0047fe68.asm,
// tmp/decomp_sim/llm_strat_unit_state_enter_storage_begin_0047ff50.asm,
// tmp/decomp_sim/llm_strat_unit_state_enter_wait_00480218.asm,
// tmp/decomp_sim/llm_strat_unit_state_enter_walk_in_004803bf.asm) -- see the header banner for the
// field-offset correction, the two read/dispatch hazards, and the two declared-need constants.
//
#include "sim/sim_unit_state_enter.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_enter_calls &live_unit_state_enter_calls() {
    static const unit_state_enter_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_storage_can_enter),
        MH_LIBMH_BIND(llm_strat_storage_board_unit),
        MH_LIBMH_BIND(llm_strat_unit_queue_advance),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_unit_walk_step_allowed),
        MH_LIBMH_BIND(llm_strat_unit_soldiers_set_heading),
        MH_LIBMH_BIND(llm_strat_dir_step_factor),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
    };
    return c;
}

namespace detail {

void unit_state_enter_arrival_check(const sim_view &v, const unit_state_enter_calls &c) {
    // Writes nothing tracked -- see the header banner's NOT SHADOWABLE note. Read-only over `v`.
    const unit    &u      = *v.cur_unit; // _G_LLM_STRAT_CUR_UNIT dereferenced -- see sim_view::cur_unit's comment.
    const uint16_t player = *v.cur_player;
    const uint8_t  slot   = u.home_storage_slot; // 0x0047fe80-0x0047fe89

    // 0x0047fe8c-0x0047fee2: has the unit reached the storage's own exit/door tile? Compares the
    // unit's CURRENT tile (x/y), NOT its move-order goal -- see the header banner's DECLARED
    // CORRECTION note on why [unit+0x84]/[unit+0x85] are x/y, not goal_x/goal_y.
    const unit_storage &st           = storage_of(v, player, slot);
    const bool          at_exit_tile = (static_cast<int32_t>(u.x) == st.exit_tile_x) &&
                              (static_cast<int32_t>(u.y) == st.exit_tile_y);

    if (at_exit_tile) {
        // 0x0047fee2-0x0047ff2a: is the home building fully operational (connected AND staffed, the
        // established built_flags=='3' test)?
        const building &b = building_of(v, player, st.b_index);
        if (b.built_flags == 3) {
            c.unit_set_state(ENTER_ARRIVAL_CHECK_STATE_ENTER_STORAGE_BEGIN); // 0x0047ff3c, state 0x24
            return;
        }
    }
    // 0x0047ff30: not yet at the exit tile, OR the building isn't ready -- replan.
    c.unit_set_state(ENTER_ARRIVAL_CHECK_STATE_GROUP_MARSHAL); // state 0xa
}

void unit_state_enter_storage_begin(const sim_view &v, sim_store &own,
                                    const unit_state_enter_calls &c) {
    const unit    &u      = *v.cur_unit; // no unit-field writes anywhere in this function
    const uint16_t player = *v.cur_player;
    const uint16_t index  = *v.cur_index;
    const uint8_t  slot   = u.home_storage_slot; // 0x0047ff6d-0x0047ff71

    // 0x0047ff8f-0x0048000b: re-verify the SAME (x,y)-at-exit-tile + built_flags==3 test
    // enter_arrival_check already ran -- this state can be entered directly by the dispatch table, so
    // it does not trust the caller.
    const unit_storage &st           = storage_of(v, player, slot);
    const bool          at_exit_tile = (static_cast<int32_t>(u.x) == st.exit_tile_x) &&
                              (static_cast<int32_t>(u.y) == st.exit_tile_y);
    bool ready = false;
    if (at_exit_tile) {
        const building &b = building_of(v, player, st.b_index);
        ready             = (b.built_flags == 3);
    }
    if (!ready) {
        // 0x00480018-0x0048001d: abort call -- BUT NOT A RETURN. PRESERVE-BUG (re-read from the raw
        // .asm this slice, corroborated independently by the Ghidra .c draft, which has no `return`
        // here either): LAB_00480018's CALL falls straight through into LAB_00480022 -- the SAME
        // instruction the ready==true path jumps to at 0x00480016 JZ. There is no branch back out.
        // So this state-machine handler unconditionally ALSO runs the storage_can_enter/door logic
        // below on the abort path, and whichever of ENTER_WAIT/no-transition that logic picks
        // silently overwrites (or coexists with) the STOP_TO_DEFAULT just set. An earlier
        // translation added a `return` here that "fixed" this into sane-looking control flow -- that
        // was a Law-2 violation, not a correction, and is reverted here.
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
    }

    // 0x00480022-0x00480039: can we actually claim the door right now? Reached on BOTH the ready and
    // not-ready paths -- see the PRESERVE-BUG note above.
    const int32_t can_enter = c.storage_can_enter(player, index, slot);
    if (can_enter != 0) {
        // 0x0048003d-0x00480076: claim the door mutex for this unit and board it. No state transition
        // on this path.
        own.storage_at(player, slot).door_mutex_unit = static_cast<int32_t>(index);
        c.storage_board_unit(player, index, slot);
        return;
    }

    // 0x00480078-0x0048009e: can't claim it yet -- queue up and wait.
    own.storage_at(player, slot).door_waiter_count += 1;
    c.unit_set_state(ENTER_STORAGE_BEGIN_STATE_ENTER_WAIT); // state 0x25
}

void unit_state_enter_wait(const sim_view &v, sim_store &own, const unit_state_enter_calls &c) {
    unit          &u      = own.cur_unit(); // writes activity_clock on the WAIT path below
    const uint16_t player = *v.cur_player;
    const uint16_t index  = *v.cur_index;
    const uint8_t  slot   = u.home_storage_slot; // 0x00480230-0x00480239

    // 0x0048023c-0x00480253: can we claim the door now?
    const int32_t can_enter = c.storage_can_enter(player, index, slot);
    if (can_enter != 0) {
        // 0x00480257-0x0048027d: yes -- release our wait-queue slot and go claim it for real.
        own.storage_at(player, slot).door_waiter_count -= 1;
        c.unit_set_state(ENTER_WAIT_STATE_ENTER_STORAGE_BEGIN); // state 0x24
        return;
    }

    // 0x00480282-...: still can't enter -- is this still worth waiting for, or should we give up? All
    // three "give up" conditions below converge on the SAME abort code in the asm (0x0048037c), so
    // it's factored into one local closure called from each -- same shape sim_bldg_defense_cost.cpp's
    // shared-tail idiom uses, not a simplification of the branch structure itself (each condition still
    // gets its own independent if/return, matching each CMP/Jcc's own target).
    auto abort_to_default = [&]() {
        // 0x0048037c-0x004803b0
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        own.storage_at(player, slot).door_waiter_count -= 1;
        c.unit_queue_advance(player, index);
    };

    const unit_storage &st = storage_of(v, player, slot);
    const building     &b  = building_of(v, player, st.b_index);

    if (b.building_id == 0) { // 0x004802b1-0x004802f7
        abort_to_default();
        return;
    }
    if (!(b.energy > 0.0)) { // 0x004802ea-0x004802f7 (FLDZ/FCOMP/JC: reached only when energy>0.0)
        abort_to_default();
        return;
    }
    if (st.door_mutex_unit == 0 && b.online_state == 2) { // 0x0048030f-0x00480351
        abort_to_default();
        return;
    }

    // 0x00480355-0x0048037a: still queued and legitimately waiting -- drain the (already-idle) budget
    // and push a FIXED backoff into activity_clock (NOT the current-budget carryover idiom other
    // tick_budget gates use -- this always adds the same constant, regardless of the budget value,
    // which this state never spends). See the header's DECLARED NEED note on the constant itself.
    own.tick_budget() = 0.0;
    u.activity_clock += *v.enter_wait_activity_backoff_seconds; // _G_LLM_STRAT_UNIT_ENTER_WAIT_ACTIVITY_BACKOFF, 0x00501400
}

void unit_state_enter_walk_in(const sim_view &v, sim_store &own, const unit_state_enter_calls &c) {
    unit               &u      = own.cur_unit();
    const uint16_t      player = *v.cur_player;
    const int32_t       index  = static_cast<int32_t>(*v.cur_index);
    const uint8_t       slot   = u.home_storage_slot; // 0x004803dc-0x004803e0
    const unit_storage &st     = storage_of(v, player, slot);

    // 0x004803d7-0x0048045f: facing needed to walk from the storage's exit/door tile to its park tile.
    // park_x/park_y read as clean zero-extended bytes -- see the header's PARK_X/PARK_Y READ HAZARD note
    // on the raw asm's own full-dword load over the field-plus-padding.
    const int32_t facing_needed =
        c.dir_from_to(st.exit_tile_x << 5, st.exit_tile_y << 5, static_cast<int32_t>(st.park_x) << 5,
                      static_cast<int32_t>(st.park_y) << 5);

    if (static_cast<int32_t>(u.facing_target) != facing_needed) {
        // ---- 0x00480479-0x004805e6: TURN toward facing_needed --------------------------------------
        const double turn_cost = v.cfg_units[u.unit_proto_id].turn_speed[player];
        if (own.tick_budget() < turn_cost) {
            u.activity_clock -= own.tick_budget();
            own.tick_budget() = 0.0;
            return;
        }
        own.tick_budget() -= turn_cost;

        const int32_t facing_target_val  = static_cast<int32_t>(u.facing_target);
        const int32_t facing_current_val = static_cast<int32_t>(u.facing_current);
        int32_t       new_facing_target, new_facing_current;

        // 0x00480505-0x0048052b: shortest-arc direction, TRANSCRIBED as the three separate compare
        // blocks the asm actually branches on (per the batch hazard note), not a canonical mod-24
        // expression -- see the header's ROTATION HAZARD note. `goto` labels mirror the asm's own
        // LAB_00480518/LAB_0048052d/LAB_0048054f targets.
        {
            const int32_t diff_a = facing_target_val - facing_needed; // 0x00480505-0x00480508
            if (diff_a > 12 && facing_target_val > facing_needed) {   // 0x0048050b-0x00480516
                goto rotate_increment;
            }
            // fallthrough into the second block, matching the asm's own fallthrough from the failed JG
            // at 0x00480516 into LAB_00480518 (dead in practice for facing values 0..23, transcribed
            // literally regardless).
        }
        {
            const int32_t diff_b = facing_target_val - facing_needed;     // recomputed, 0x00480518-0x0048051b
            if (diff_b <= -12) goto rotate_decrement;                     // 0x0048051e-0x00480521
            if (facing_target_val < facing_needed) goto rotate_increment; // 0x00480523-0x00480529
            goto rotate_decrement;                                        // 0x0048052b fallthrough
        }

    rotate_increment:
        new_facing_target = facing_target_val + 1;                 // 0x0048052d-0x00480530
        if (new_facing_target > 0x18) new_facing_target -= 0x18;   // 0x00480533-0x00480539
        new_facing_current = facing_current_val + 1;               // 0x0048053d-0x00480540
        if (new_facing_current > 0x18) new_facing_current -= 0x18; // 0x00480543-0x00480549
        goto rotate_commit;

    rotate_decrement:
        new_facing_target = facing_target_val - 1;              // 0x0048054f-0x00480552
        if (new_facing_target < 1) new_facing_target += 0x18;   // 0x00480555-0x0048055b
        new_facing_current = facing_current_val - 1;            // 0x0048055f-0x00480562
        if (new_facing_current < 1) new_facing_current += 0x18; // 0x00480565-0x0048056b

    rotate_commit:
        u.facing_target = static_cast<uint8_t>(new_facing_target);                // 0x0048056f-0x0048057b
        if (static_cast<uint8_t>(c.unit_walk_step_allowed(player, index)) != 0) { // 0x0048057b-0x00480590
            u.facing_current = static_cast<uint8_t>(new_facing_current);          // 0x00480592-0x0048059b
        }
        if (v.cfg_units[u.unit_proto_id].soldier_count > 0) { // 0x0048059e-0x004805ce
            c.unit_soldiers_set_heading(player, index,
                                        static_cast<uint8_t>(new_facing_target)); // 0x004805d0-0x004805e1
        }
        return; // 0x004805e6 -- the TURN branch always returns here; it never falls into the STEP logic.
    }

    // ---- 0x004805eb-...: facing already matches -- take the sub-tile walk-in animation step --------
    double step_cost = c.dir_step_factor(static_cast<int32_t>(u.facing_target)) *
                       v.cfg_units[u.unit_proto_id].step_speed[player]; // 0x004805eb-0x0048061d
    if (v.cfg_units[u.unit_proto_id].soldier_count > 0) {               // 0x0048061d-0x0048064d
        step_cost *= *v.enter_walk_in_soldier_transport_mult;           // _G_LLM_STRAT_UNIT_ENTER_WALK_SOLDIER_STEP_SCALE, 0x00501408, 0x0048064f-0x00480658
    }
    if (own.tick_budget() < step_cost) { // 0x0048065b-0x00480667
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= step_cost; // 0x00480693-0x0048069c

    // 0x004806a2-0x00480722: is the home building a single-tile-group type (garage/shuttle-bay) or the
    // default two-tile-group? See the header's TABLE-DISPATCH HAZARD note -- this is NOT an index into
    // _G_LLM_STRAT_MOVE_MICROSTEPS despite the superficial SHL,0x5 resemblance to that table's own
    // indexing shift; it is pure scalar arithmetic on group_count, no table read happens here.
    const uint8_t building_type   = v.cfg_buildings[building_of(v, player, st.b_index).building_id].type;
    const bool    is_single_group = (building_type == BUILDING_TYPE_A_GARAGE) ||
                                 (building_type == BUILDING_TYPE_A_SHUTTLE) ||
                                 (building_type == BUILDING_TYPE_H_GARAGE);
    const int32_t group_count         = is_single_group ? 1 : 2;
    const int32_t microstep_threshold = group_count * 0x20 + 0x1f; // 0x0048072c-0x00480734

    if (u.move_microstep < microstep_threshold) { // 0x00480737-0x0048073d
        u.move_microstep += 1;                    // 0x0048073f-0x0048074a
        return;
    }

    // 0x0048074f-0x004807cf: the walk-in animation finished -- release the door, park, and clean up.
    own.storage_at(player, slot).door_mutex_unit = 0;
    c.unit_set_state(UNIT_STATE_PARKED); // state 0x1f
    if (u.target2_ref != 0) {
        c.target_release_ref(player, index, 3u); // mode 3, no established name (matches the sibling TU's
                                                 // own precedent of passing an un-named mode literal)
        u.target2_ref = 0;
    }
    if (u.path_slot_id != 0xff) {
        c.path_free_slot(player, index);
    }
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void unit_state_enter_arrival_check() {
    sim_state st = state();
    detail::unit_state_enter_arrival_check(st.read, live_unit_state_enter_calls());
}

void unit_state_enter_storage_begin() {
    sim_state st = state();
    detail::unit_state_enter_storage_begin(st.read, st.own, live_unit_state_enter_calls());
}

void unit_state_enter_wait() {
    sim_state st = state();
    detail::unit_state_enter_wait(st.read, st.own, live_unit_state_enter_calls());
}

void unit_state_enter_walk_in() {
    sim_state st = state();
    detail::unit_state_enter_walk_in(st.read, st.own, live_unit_state_enter_calls());
}


} // namespace mh::sim
