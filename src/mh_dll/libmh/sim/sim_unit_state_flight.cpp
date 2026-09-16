//
// sim/sim_unit_state_flight.cpp -- see sim_unit_state_flight.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_state_climb_vertical_00481960.asm,
// tmp/decomp/llm_strat_unit_state_descend_cruise_00481f7c.asm,
// tmp/decomp/llm_strat_unit_state_ascend_to_orbit_0048214d.asm), each transcribed SEPARATELY per the
// header banner's warning that the three siblings differ in cost multiplier, elevation step, cap
// arithmetic, and tail effects despite sharing one skeleton.
//
#include "sim/sim_unit_state_flight.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const unit_state_flight_calls &live_unit_state_flight_calls() {
    static const unit_state_flight_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_prod_unbind_planet),
        MH_CRT(utils_w_str_copy),
        MH_CRT(utils_concat),
        mh::state::evt::text_print_u32,
    };
    return c;
}

namespace detail {

void unit_state_climb_vertical(const sim_view &v, sim_store &own, const unit_state_flight_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced, read+write -- see sim_view::cur_unit's
                              // comment: bound from the SAME resolved pointer as *v.cur_player/*v.cur_index.
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    // 0x00481978-0x004819a2: cost multiplier -- 1.5 if unit.order==ASCEND_TO_ORBIT(0x31) else 2.0,
    // loaded in the asm as INLINE HI-DWORD IMMEDIATES (0x3ff80000 / 0x40000000 into the stack slot
    // pair that forms one double, lo dword always 0) rather than a DAT_ table read -- these are the
    // HIGH 32 bits of the IEEE-754 doubles 1.5 (0x3FF8000000000000) and 2.0 (0x4000000000000000).
    const double cost_mult = (u.order == UNIT_STATE_ASCEND_TO_ORBIT) ? 1.5 : 2.0;
    const double cost      = proto.step_speed[*v.cur_player] * cost_mult; // 0x004819a2-0x004819c6

    // 0x004819c9-0x004819d5: FLD TICK_BUDGET; FCOMP cost; FNSTSW/SAHF; JNC -- budget>=cost takes the
    // spend branch (JNC jumps when CF=0, i.e. NOT(budget<cost)).
    if (own.tick_budget() >= cost) {
        own.tick_budget() -= cost; // 0x004819fe-0x00481a07
        u.elevation += 1;          // 0x00481a0d-0x00481a12

        // 0x00481a15-0x00481a33: JL skips the clamp+set_state when elevation < cap, i.e. this branch
        // is taken when elevation >= Unit[proto].elevation (the SAME cfg field climb approaches from
        // below as a ceiling -- descend_cruise approaches it from above as a floor).
        if (u.elevation >= proto.elevation) {
            u.elevation = proto.elevation; // 0x00481a35-0x00481a50
            c.unit_set_state(u.order);     // 0x00481a53-0x00481a5c -- the PENDING order, not a literal
        }
    } else {
        u.activity_clock -= own.tick_budget(); // 0x004819d7-0x004819e5 (FSUBR form; algebraically
                                               // identical to this subtraction)
        own.tick_budget() = 0.0;               // 0x004819e8-0x004819f2
    }
}

void unit_state_descend_cruise(const sim_view &v, sim_store &own, const unit_state_flight_calls &c) {
    unit           &u     = own.cur_unit();
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    // 0x00481f94-0x00481f9b: cost multiplier is UNCONDITIONALLY 1.5 here -- no order-code branch,
    // unlike climb_vertical.
    const double cost = proto.step_speed[*v.cur_player] * 1.5;

    if (own.tick_budget() >= cost) { // 0x00481fc9-0x00481fd5, same JNC-on-FCOMP gate as climb_vertical
        own.tick_budget() -= cost;   // 0x00482001-0x0048200a
        u.elevation -= 1;            // 0x00482010-0x00482015

        // 0x00482018-0x00482036: JG skips the clamp+everything-after when elevation > cap, i.e. this
        // branch is taken when elevation <= Unit[proto].elevation (the floor test, opposite direction
        // from climb_vertical's ceiling test over the same field).
        if (u.elevation <= proto.elevation) {
            u.elevation = proto.elevation; // 0x0048203c-0x00482056
            c.unit_set_state(u.order);     // 0x00482059-0x00482062

            // 0x00482067-0x0048212f: re-derives units[cur_player][cur_index].unit_proto_id FRESH via
            // roster row/column arithmetic (the asm does this three times) -- transcribed via the same
            // v.units[cur_player * UNITS_PER_PLAYER + cur_index] roster access the disassembly uses,
            // rather than reusing `u`/`proto` above, so the access pattern matches the .asm. It is the
            // IDENTICAL memory (cur_unit points at exactly this slot and nothing writes unit_proto_id
            // in this function), but reading it the roster way keeps the transcription literal.
            const unit    &gate_u    = v.units[*v.cur_player * v.caps.units + *v.cur_index];
            const uint32_t gate_type = v.cfg_units[gate_u.unit_proto_id].type;
            if (gate_type == UNIT_TYPE_A_HELI_MOTHER || gate_type == UNIT_TYPE_H_HELI_MOTHER ||
                gate_type == UNIT_TYPE_A_HELI_SHUTTLE || gate_type == UNIT_TYPE_H_HELI_SHUTTLE) {
                // 0x00482131-0x00482143: EDX=G_PLANET_INDEX loaded first, EAX=cur_player loaded
                // second -- matches llm_strat_prod_unbind_planet(player, planet_slot)'s committed
                // (EAX,EDX) register order.
                c.prod_unbind_planet(static_cast<int32_t>(*v.cur_player), *v.planet_index);
            }
        }
    } else {
        u.activity_clock -= own.tick_budget(); // 0x00481fd7-0x00481fe5
        own.tick_budget() = 0.0;               // 0x00481fe8-0x00481ff2
    }
}

void unit_state_ascend_to_orbit(const sim_view &v, sim_store &own, const unit_state_flight_calls &c) {
    unit           &u     = own.cur_unit();
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    // 0x00482165-0x0048216c: cost multiplier is UNCONDITIONALLY 1.5, same as descend_cruise.
    const double cost = proto.step_speed[*v.cur_player] * 1.5;

    if (own.tick_budget() >= cost) { // 0x0048219a-0x004821a6, same JNC-on-FCOMP gate
        own.tick_budget() -= cost;   // 0x004821d2-0x004821db
        u.elevation += 3;            // 0x004821e1-0x004821e6

        const int32_t cap = proto.elevation + 300; // 0x004821ea-0x00482205 (0x12c == 300)

        // 0x00482205-0x0048220d: JG skips everything below when cap > elevation, i.e. this branch is
        // taken when cap <= elevation (elevation has reached/passed cfg elevation + 300).
        if (cap <= u.elevation) {
            u.elevation = cap;                             // 0x00482213-0x00482233
            c.unit_set_state(UNIT_STATE_PRODUCTION_READY); // 0x00482236-0x0048223b -- a LITERAL
                                                           // state, not unit.order (unlike the
                                                           // other two functions in this file).

            // 0x00482240-0x00482252: EDX=G_PLANET_INDEX, EAX=cur_player -- same register order as
            // descend_cruise's call, but UNCONDITIONAL here once the cap test passes (no type gate).
            c.prod_unbind_planet(static_cast<int32_t>(*v.cur_player), *v.planet_index);

            // 0x00482252-0x004822cd: the SIM-CUT UI message. shuttle_slot is read here, AFTER the
            // elevation/state/unbind writes above but BEFORE any of them could touch it -- nothing in
            // this function writes unit.shuttle_slot, so the value read is whatever it was at entry.
            const uint8_t            shuttle_slot = u.shuttle_slot; // 0x00482252-0x0048225b
            const prod_shuttle_slot &slot =
                v.prod_shuttle_slots[*v.cur_player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot];

            // 1. base message: G_TEXT_PTRS[0x1c] (a FIXED text id, NOT the unit's own name -- see the
            //    header banner). Param order (src, dst) matches the committed
            //    utils_w_str_copy(void *src, void *dst) signature, same as
            //    sim_prod_shuttle_complete.cpp's identical idiom.
            c.w_str_copy(const_cast<wchar_t *>(v.text_ptrs[TEXT_ID_ASCEND_TO_ORBIT_BASE]),
                         own.text_scratch()); // 0x0048225e-0x0048226e

            // 2. " (" separator (DAT_00501438, supplied as our own literal -- see the header banner).
            c.concat(own.text_scratch(),
                     const_cast<wchar_t *>(ASCEND_TO_ORBIT_MSG_SEP_OPEN)); // 0x0048226e-0x0048227d

            // 3. destination planet's name: PROD_SHUTTLE_SLOTS[player][shuttle_slot].dest_planet ->
            //    Planets[dest_planet].name -> G_TEXT_PTRS[name_id]. 0x0048227d-0x004822af.
            const int32_t planet_name_id =
                v.cfg_planets[static_cast<uint16_t>(slot.dest_planet)].name;
            c.concat(own.text_scratch(), const_cast<wchar_t *>(v.text_ptrs[planet_name_id]));

            // 4. ")" separator (DAT_0050143e, supplied as our own literal). 0x004822b9-0x004822c8.
            c.concat(own.text_scratch(), const_cast<wchar_t *>(ASCEND_TO_ORBIT_MSG_SEP_CLOSE));

            // 5. print, return value discarded (matches the original). 0x004822c8-0x004822cd.
            c.print_text_message(own.text_scratch());
        }
    } else {
        u.activity_clock -= own.tick_budget(); // 0x004821a8-0x004821b6
        own.tick_budget() = 0.0;               // 0x004821b9-0x004821c3
    }
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void unit_state_climb_vertical() {
    sim_state st = state();
    detail::unit_state_climb_vertical(st.read, st.own, live_unit_state_flight_calls());
}

void unit_state_descend_cruise() {
    sim_state st = state();
    detail::unit_state_descend_cruise(st.read, st.own, live_unit_state_flight_calls());
}

void unit_state_ascend_to_orbit() {
    sim_state st = state();
    detail::unit_state_ascend_to_orbit(st.read, st.own, live_unit_state_flight_calls());
}


} // namespace mh::sim
