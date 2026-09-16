//
// sim/sim_unit_state_takeoff.cpp -- see sim_unit_state_takeoff.h for the full derivation, the
// CORRECTIONs to the batch hazard note, the two DECLARED NEED boot-constant doubles, and the
// facing_target/facing_current `.facing` field resolution.
//
#include "sim/sim_unit_state_takeoff.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_takeoff_calls &live_unit_state_takeoff_calls() {
    static const unit_state_takeoff_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_storage_dock_list_append),
        MH_LIBMH_BIND(llm_strat_ai_group_member_count_adjust),
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(map_unit_PutOnMap),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_storage_remove_docked_unit),
    };
    return c;
}

namespace detail {

namespace {

// Shared continuation reached from THREE origins in llm_strat_unit_state_takeoff_landing's own body
// (all converging on 0x004816f4): (a) climbing (state==UNIT_STATE_TAKEOFF) but elevation has not yet
// reached the per-proto ceiling, (b) descending (state==UNIT_STATE_LANDING) but elevation has not yet
// reached the per-proto floor, (c) the state is neither of the two at all. All three reach this point
// with NO elevation change of their own -- see the header derivation. Pulled into one helper (internal
// linkage, private to this TU -- not a new shared API) rather than duplicated three times.
void takeoff_landing_taxi_continue(const sim_view &v, sim_store &own, const unit_state_takeoff_calls &c,
                                   unit &u, uint16_t player, int32_t index, uint16_t proto) {
    // 0x004816f4-0x00481758: a helicopter-class unit skips the taxi animation entirely this tick.
    // Four CMPs (0xf/0x10/0x17/0x18) all converge on the same return -- verified by walking every
    // JZ/JNZ target in the .asm.
    const uint32_t type = v.cfg_units[proto].type;
    if (type == UNIT_TYPE_A_HELI || type == UNIT_TYPE_H_HELI || type == UNIT_TYPE_A_HELI_CARGO ||
        type == UNIT_TYPE_H_HELI_CARGO) {
        return;
    }

    if (u.move_microstep < 0x1f) {
        // 0x0048175d-0x004817d0: still mid taxi-animation -- advance one microstep and re-sample the
        // facing pair. BOTH bytes come from the SAME table entry's `.facing` field (see the header
        // derivation: the raw instruction displacement 0xae3742 is _G_LLM_STRAT_MOVE_MICROSTEPS's
        // declared base 0x00ae3740 + 2, i.e. move_microstep::facing, NOT offset+0).
        u.move_microstep += 1;
        const uint8_t src =
            v.move_microsteps[static_cast<int32_t>(u.move_heading) * MICROSTEPS_PER_HEADING +
                              u.move_microstep]
                .facing;
        u.facing_target  = src;
        u.facing_current = src;
        return;
    }

    // 0x004817d5-0x00481953: taxi-animation complete -- step one tile along move_heading (torus-
    // wrapped), relocate, and restart the microstep animation.
    const uint8_t heading = u.move_heading;
    const int32_t new_col = (static_cast<int32_t>(u.x) + v.dir_step_offsets[heading].dx) &
                            static_cast<int32_t>(map_width_mask(v));
    const int32_t new_row = (static_cast<int32_t>(u.y) + v.dir_step_offsets[heading].dy) &
                            static_cast<int32_t>(map_height_mask(v));

    c.unit_unlink_tile(player, static_cast<uint16_t>(index));
    c.fow_remove_sight(player, u.x, u.y, v.cfg_units[proto].sight); // OLD position, before the move.

    c.map_unit_PutOnMap(player, static_cast<uint16_t>(index), static_cast<uint8_t>(new_col),
                        static_cast<uint8_t>(new_row));
    u.x = static_cast<uint8_t>(new_col);
    u.y = static_cast<uint8_t>(new_row);

    c.map_fow_UpdateFoWPlus(player, static_cast<uint32_t>(new_col), static_cast<uint32_t>(new_row),
                            v.cfg_units[proto].sight);

    u.move_microstep = 0;
    u.move_heading   = heading; // 0x004818ed-0x004818f6: re-affirms the value captured at entry.

    const uint8_t src =
        v.move_microsteps[static_cast<int32_t>(u.move_heading) * MICROSTEPS_PER_HEADING + 0].facing;
    u.facing_target  = src;
    u.facing_current = src;
}

} // namespace

void unit_state_takeoff_landing(const sim_view &v, sim_store &own, const unit_state_takeoff_calls &c) {
    unit          &u      = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player = *v.cur_player;
    const int32_t  index  = static_cast<int32_t>(*v.cur_index);
    const uint16_t proto  = u.unit_proto_id;

    // 0x00481409-0x0048142a: TICK_BUDGET gate. DECLARED NEED boot constant (see header banner).
    const double cost = v.cfg_units[proto].step_speed[player] * *v.takeoff_landing_step_cost_scale;
    if (own.tick_budget() < cost) {
        // 0x00481441-0x00481466: insufficient -- drain and return via the shared tail.
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= cost; // 0x0048146b-0x00481474.

    // 0x0048147a-0x00481486: this handler's OWN dispatch key -- see the CORRECTION in the header
    // banner (this is unit.state@+0x6, not unit_proto_id again).
    const uint16_t sub_state = u.state;

    if (sub_state == UNIT_STATE_TAKEOFF) {
        // 0x004814a5-0x004814cb: climbing.
        u.elevation += 1;
        if (u.elevation < v.cfg_units[proto].elevation) {
            // 0x004814cb (JL taken): not yet at the ceiling -- keep taxiing, no clamp/transition yet.
            takeoff_landing_taxi_continue(v, own, c, u, player, index, proto);
            return;
        }
        // 0x004814cd-0x004814f9: reached/passed the ceiling -- clamp and promote the queued order.
        u.elevation = v.cfg_units[proto].elevation;
        c.unit_set_state(u.order); // u.order (+0x4), NOT a literal -- see the header derivation.
        return;
    }

    if (sub_state == UNIT_STATE_LANDING) {
        // 0x00481503-0x00481529: descending.
        u.elevation -= 1;
        if (u.elevation > v.cfg_units[proto].elevation_2) {
            // 0x00481529 (JG taken): not yet at the floor -- keep taxiing.
            takeoff_landing_taxi_continue(v, own, c, u, player, index, proto);
            return;
        }
        // 0x0048152f-0x004816ef: reached/passed the floor -- dock.
        u.elevation = v.cfg_units[proto].elevation_2;

        // 0x0048154d-0x00481599: population bookkeeping -- regain housing capacity and drop the
        // in-field count (mirror image of the boarding path sim_unit_create.h/sim_population_*.cpp
        // document: there, human -= / human_in_field +=).
        pop_stats &pop = own.population_at(player);
        pop.human += v.cfg_units[proto].human;
        pop.human_in_field -= v.cfg_units[proto].human;

        c.unit_unlink_tile(player, static_cast<uint16_t>(index)); // 0x004815a7.

        const int32_t storage_slot = static_cast<int32_t>(u.home_storage_slot);
        c.storage_dock_list_append(player, static_cast<uint16_t>(index),
                                   static_cast<uint16_t>(storage_slot)); // 0x004815ca.
        // 0x004815e5: mode=1u -- the OPPOSITE literal from llm_strat_unit_takeoff_finalize's own call
        // to the SAME function (mode=0u); see the header UNCERTAINTY on which direction each means.
        c.ai_group_member_count_adjust(player, static_cast<uint32_t>(index),
                                       static_cast<uint32_t>(storage_slot), 1u);
        c.game_SetEvent(7u);                                            // 0x004815ea-0x004815ef. See the header UNCERTAINTY on this call.
        c.fow_remove_sight(player, u.x, u.y, v.cfg_units[proto].sight); // 0x00481603-0x00481629.

        // 0x0048162e-0x00481672: the SAME storage_of/building_of composition
        // sim_unit_state_exit.cpp uses.
        const building &b = building_of(v, player, storage_of(v, player, storage_slot).b_index);
        if (v.cfg_buildings[b.building_id].type == BUILDING_TYPE_H_HELIPAD) {
            // 0x0048167b-0x00481685.
            c.unit_set_state(UNIT_STATE_DOCK_TAXI_H);
            return;
        }
        // 0x00481687-0x004816ec.
        c.unit_set_state(UNIT_STATE_DOCK_TAXI_A);
        if (v.cfg_buildings[b.building_id].type == BUILDING_TYPE_A_HELIPAD) {
            // 0x004816de-0x004816ec: FLD activity_clock; FADD DECLARED-NEED bump; FSTP -- reproduced
            // in that literal term order per the translator brief's FP-order rule.
            u.activity_clock = u.activity_clock + *v.takeoff_landing_a_helipad_activity_bump;
        }
        return;
    }

    // 0x0048149b/0x004814a0: neither TAKEOFF nor LANDING -- straight into the taxi continuation with
    // no elevation change.
    takeoff_landing_taxi_continue(v, own, c, u, player, index, proto);
}

void unit_takeoff_finalize(const sim_view &v, sim_store &own, const unit_state_takeoff_calls &c,
                           uint16_t player, int32_t unit_index) {
    unit &u = own.unit_at(player, unit_index);

    // 0x0048b17f-0x0048b18f: home_storage_slot -- units[player][unit_index]'s OWN field, NOT a
    // unit_storage-relative one (see the header CORRECTION). Read three times in the assembly; read
    // once here and reused (see the header UNCERTAINTY on whether that is provably safe across the
    // storage_remove_docked_unit call below).
    const int32_t storage_slot = static_cast<int32_t>(u.home_storage_slot);

    c.storage_remove_docked_unit(player, unit_index, storage_slot); // 0x0048b196-0x0048b19d.

    // 0x0048b1a2-0x0048b1c8: release the door reservation
    // (unit_storage[player][storage_slot].door_mutex_unit) -- see the header CORRECTION.
    own.storage_at(player, storage_slot).door_mutex_unit = 0;

    // 0x0048b1e2-0x0048b208: the unit's OWN current tile (units[player][unit_index].x/.y), NOT a
    // storage-relative position pair -- see the header CORRECTION.
    const uint8_t x = u.x;
    const uint8_t y = u.y;
    c.map_unit_PutOnMap(player, static_cast<uint16_t>(unit_index), x, y);

    c.map_fow_UpdateFoWPlus(player, x, y, v.cfg_units[u.unit_proto_id].sight); // 0x0048b21d-0x0048b262.

    // 0x0048b267-0x0048b286: literal mode=0u -- the OPPOSITE literal from takeoff_landing's own call
    // to the same function (mode=1u); see the header UNCERTAINTY. "Callees stay original" -- neither
    // literal is renamed or interpreted here.
    c.ai_group_member_count_adjust(player, static_cast<uint32_t>(unit_index),
                                   static_cast<uint32_t>(storage_slot), 0u);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void unit_state_takeoff_landing() {
    sim_state st = state();
    detail::unit_state_takeoff_landing(st.read, st.own, live_unit_state_takeoff_calls());
}

void unit_takeoff_finalize(uint32_t player, uint32_t unit_index) {
    sim_state st = state();
    detail::unit_takeoff_finalize(st.read, st.own, live_unit_state_takeoff_calls(),
                                  static_cast<uint16_t>(player), static_cast<int32_t>(unit_index));
}


} // namespace mh::sim
