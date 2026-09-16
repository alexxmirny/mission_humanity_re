//
// sim/sim_unit_state_misc2.cpp -- see sim_unit_state_misc2.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_state_{enter_class_move,step_adjacent,production_ready}_*.asm) --
// see the header banner for the field-offset derivation, the step_adjacent control-flow CORRECTION,
// and the declared-need boot-constant double.
//
#include "sim/sim_unit_state_misc2.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_misc2_calls &live_unit_state_misc2_calls() {
    static const unit_state_misc2_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_tiles_adjacent),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_path_make_single_step),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(llm_strat_population_remove),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace detail {

void unit_state_enter_class_move(const sim_view &v, sim_store &own, const unit_state_misc2_calls &c) {
    // 0x00482f46-0x00482f5a: zero TICK_BUDGET, then dispatch straight into the unit's cfg-derived
    // move-class opcode. No other read/write anywhere in this function.
    own.tick_budget() = 0.0;
    c.unit_set_state(v.cfg_units[v.cur_unit->unit_proto_id].move_op_code);
}

void unit_state_step_adjacent(const sim_view &v, sim_store &own, const unit_state_misc2_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.

    // 0x00485bc3-0x00485bf3.
    const int32_t x        = u.x;
    const int32_t y        = u.y;
    const int32_t goal_x   = u.goal_x;
    const int32_t goal_y   = u.goal_y;
    const int32_t adjacent = c.tiles_adjacent(x, y, goal_x, goal_y);

    if (adjacent == 0) {
        // 0x00485bfc-0x00485c06: not adjacent to the goal tile -- give up and reset to default.
        c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
    } else {
        // 0x00485c24-0x00485c76: two blocking-tile gates over the GOAL tile -- the SAME
        // passable[]/tile_objects[].building regions every other sim TU binds (see header banner for
        // the address cross-check), indexed (goal_x<<8)|goal_y like tile_object_at()/passable_at().
        const int32_t goal_tile       = (goal_x << 8) | goal_y;
        const bool    tile_impassable = v.passable[goal_tile] == 0;
        const bool    tile_occupied   = v.tile_objects[goal_tile].building != 0;

        if (tile_impassable || tile_occupied) {
            // 0x00485c78-0x00485d37: falls straight to the TICK_BUDGET FP gate, no state-order call
            // first (see the header banner's CORRECTION note).
            if (own.tick_budget() <= *v.step_adjacent_budget_gate) {
                // 0x00485d14-0x00485d25: FLD budget; FSUBR activity_clock; FSTP -- i.e.
                // activity_clock = activity_clock - budget, reproduced in that literal term order.
                u.activity_clock = u.activity_clock - own.tick_budget();
            } else {
                // 0x00485cef-0x00485d12.
                c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
            }
        } else {
            // 0x00485c7a-0x00485cdc: both clear -- try to advance the unit one path step.
            const int32_t player     = *v.cur_player;
            const int32_t unit_index = *v.cur_index;
            const int32_t stepped =
                c.path_make_single_step(static_cast<uint32_t>(player), unit_index);
            if (stepped == 0) {
                // 0x00485c91-0x00485c9b: no step available.
                c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
            } else {
                // 0x00485cb9-0x00485cc3: stepped -- hand off to the walker.
                c.unit_set_state_order(UNIT_STATE_MOVE_WALKER, UNIT_STATE_STOP_TO_DEFAULT);
            }
        }
    }

    // Every one of the five exit paths above zeroes TICK_BUDGET before returning to the same tail
    // (0x00485c0b/0x00485ca0/0x00485cc8/0x00485cfe/0x00485d25 in the asm) -- factored once here per
    // the translator brief's no-triplication rule; exactly one branch executes per call, so
    // TICK_BUDGET==0.0 on return is the same observable result either way.
    own.tick_budget() = 0.0;
}

void unit_state_production_ready(const sim_view &v, sim_store &own, const unit_state_misc2_calls &c) {
    const unit   &u          = own.cur_unit(); // read-only in this function -- no unit field is written here.
    const int32_t player     = *v.cur_player;
    const int32_t unit_index = *v.cur_index;
    // 0x00486123-0x00486127: cached FIRST, before unlink_tile/teardown can touch anything.
    const int32_t shuttle_slot = u.shuttle_slot;

    // 0x0048612a-0x0048613d.
    c.unit_unlink_tile(static_cast<uint32_t>(player), static_cast<uint16_t>(unit_index));

    // 0x0048613d-0x00486177.
    c.fow_remove_sight(static_cast<uint32_t>(player), u.x, u.y,
                       v.cfg_units[u.unit_proto_id].sight);

    // 0x00486177-0x004861fc: if this unit's proto carries housing population, give it back.
    if (v.cfg_units[u.unit_proto_id].human != 0) {
        const int32_t human = v.cfg_units[u.unit_proto_id].human;
        own.population_at(static_cast<uint32_t>(player)).human += human;
        own.population_at(static_cast<uint32_t>(player)).human_in_field -= human;
        c.population_remove(static_cast<uint32_t>(player), human);
    }

    // 0x004861fc-0x00486280: a heli-mothership prototype needs its shuttle slot flagged for
    // special mobile-conversion handling at spawn time (see this field's own doc comment, which
    // already names this function as the writer). The proto_id here is read FRESH off the ROSTER
    // ARRAY (units[player][cur_index], 0x00486218/0048624a), not off the cur_unit pointer -- the
    // same "cur_unit POINTER vs ROSTER ARRAY asymmetric read" this batch's sim_unit_state_deploy.cpp
    // already documents. Nothing writes unit_proto_id in this function, so it is provably the same
    // VALUE as `u.unit_proto_id`, but reproduced via the same array expression the .asm actually
    // reads (the translation lint's region check is what caught the substitution).
    const uint16_t proto_id_fresh = unit_of(v, static_cast<uint32_t>(player), unit_index).unit_proto_id;
    if (v.cfg_units[proto_id_fresh].type == UNIT_TYPE_A_HELI_MOTHER ||
        v.cfg_units[proto_id_fresh].type == UNIT_TYPE_H_HELI_MOTHER) {
        own.prod_shuttle_slot_at(static_cast<uint32_t>(player), shuttle_slot)
            .is_heli_mother_pending = 1;
    }

    // 0x00486280-0x00486296: unconditionally mark the transfer as ready (status 200 -- this field's
    // own doc comment already names this function as the writer of this exact value).
    own.prod_shuttle_slot_at(static_cast<uint32_t>(player), shuttle_slot).status = 200;

    // 0x0048629f-0x004862b7.
    c.unit_teardown(static_cast<uint32_t>(player), static_cast<uint16_t>(unit_index));
    c.set_event(EVENT_INFO_REFRESH);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void unit_state_enter_class_move() {
    sim_state st = state();
    detail::unit_state_enter_class_move(st.read, st.own, live_unit_state_misc2_calls());
}

void unit_state_step_adjacent() {
    sim_state st = state();
    detail::unit_state_step_adjacent(st.read, st.own, live_unit_state_misc2_calls());
}

void unit_state_production_ready() {
    sim_state st = state();
    detail::unit_state_production_ready(st.read, st.own, live_unit_state_misc2_calls());
}


} // namespace mh::sim
