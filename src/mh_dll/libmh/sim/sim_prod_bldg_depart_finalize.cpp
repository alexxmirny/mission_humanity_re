//
// sim/sim_prod_bldg_depart_finalize.cpp -- see sim_prod_bldg_depart_finalize.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_prod_bldg_depart_finalize_0048ec51.asm), not from the Ghidra .c
// draft -- see the header banner for the one real bug the draft carries (the phantom 3rd argument on
// one of the two llm_strat_prod_unbind_planet calls).
//
#include "sim/sim_prod_bldg_depart_finalize.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_bldg_depart_finalize_calls &live_prod_bldg_depart_finalize_calls() {
    static const prod_bldg_depart_finalize_calls c = {
        MH_LIBMH_BIND(llm_strat_planet_distance),
        MH_LIBMH_BIND(llm_prod_planet_distance_factor),
        MH_LIBMH_BIND(llm_prod_shuttle_fuel_check),
        MH_LIBMH_BIND(llm_strat_unit_add_docked),
        MH_LIBMH_BIND(llm_strat_storage_launch_parked_to_orbit),
        MH_LIBMH_BIND(llm_strat_prod_unbind_planet),
    };
    return c;
}

namespace detail {

int32_t prod_bldg_depart_finalize(const sim_view &v, sim_store &own,
                                  const prod_bldg_depart_finalize_calls &c, uint16_t player,
                                  int32_t building_index, int32_t dest_planet) {
    // 0x0048ec67-0x0048eca7: shuttle_slot (byte @+0xc5) + building_id (word @+0x2), ONE read of the
    // building record -- the asm recomputes the row+col address twice for the two field reads.
    const uint8_t shuttle_slot = building_of(v, player, building_index).shuttle_slot;
    uint16_t      building_id  = building_of(v, player, building_index).building_id;

    // 0x0048ecaa-0x0048ece4: distance = llm_strat_planet_distance(Planets[G_PLANET_INDEX].coordinate_x,
    // .coordinate_y, Planets[dest_planet].coordinate_x, .coordinate_y) -- register order at the call
    // site matches this argument order exactly against the committed prototype.
    const cfg_planet &src_planet = v.cfg_planets[*v.planet_index];
    const cfg_planet &dst_planet = v.cfg_planets[dest_planet];
    const double      distance   = c.planet_distance(src_planet.coordinate_x, src_planet.coordinate_y,
                                                     dst_planet.coordinate_x, dst_planet.coordinate_y);

    // 0x0048ecec-0x0048ed02: slot.dest_planet = (int16_t)dest_planet -- BEFORE the fuel re-check, so
    // this write (and the two below) stick even when the departure ultimately fails fuel.
    prod_shuttle_slot &slot = own.prod_shuttle_slot_at(player, shuttle_slot);
    slot.dest_planet        = static_cast<int16_t>(dest_planet);

    // 0x0048ed09-0x0048ed19: vel_x_dist = Building[building_id].velocity * distance -- the FIRST x87
    // product, computed and stashed BEFORE distance_factor is even fetched.
    const double vel_x_dist = v.cfg_buildings[building_id].velocity * distance;

    // 0x0048ed1c-0x0048ed29: distance_factor = llm_prod_planet_distance_factor(G_PLANET_INDEX,
    // dest_planet); travel_duration = distance_factor * vel_x_dist -- i.e. distance_factor *
    // (velocity * distance), NOT (distance_factor * velocity) * distance. x87 multiplication is not
    // guaranteed bit-identical under re-association, so this grouping is preserved exactly.
    const double distance_factor = c.planet_distance_factor(*v.planet_index, dest_planet);
    slot.travel_duration         = distance_factor * vel_x_dist;

    // 0x0048ed3f-0x0048ed5e: travel_duration_copy = travel_duration, via a genuine reload from memory
    // (FLD from the just-written field, then FSTP) -- reproduced as a read-back rather than reusing the
    // `distance_factor * vel_x_dist` local, though the two are numerically identical either way.
    slot.travel_duration_copy = slot.travel_duration;

    // 0x0048ed64-0x0048ed99: PERMANENTLY DEAD BRANCH -- `(G_PLANET_INDEX > 4) && (false)`. The inner
    // guard is a Watcom fake-branch idiom (`local = 0; if (local > 0)`, always false) that Ghidra's own
    // decompiler already flags as unreachable. Translated faithfully: the branch shape stays, the
    // multiply can never execute, and no rig/live evidence can exercise it.
    if (*v.planet_index > 4) {
        constexpr int32_t always_zero = 0; // 0x0048ed6d: MOV dword ptr [local_40],0
        if (always_zero > 0) {             // 0x0048ed74/0x0048ed78: CMP/JLE -- never taken
            slot.travel_duration *= *v.shuttle_duration_mult_dead_branch;
        }
    }

    // 0x0048ed9f-0x0048edb5: re-validate fuel now that the ETA is committed; bail WITHOUT undoing the
    // writes above -- they are meant to stick even on a failed departure.
    if (c.shuttle_fuel_check(player, building_index, dest_planet) != 0) {
        return -1;
    }

    // 0x0048edc3-0x0048ede3: re-read building_id fresh (the fuel check is an opaque original call, so
    // per house rules a read is never cached across one) and its cfg type.
    building_id             = building_of(v, player, building_index).building_id;
    const uint8_t bldg_type = v.cfg_buildings[building_id].type;

    // ---- the type dispatch (0x0048edec-0x0048efba) -- see the header banner for the full branch-tree
    // derivation. Reproduced as the two live outcome sets it collapses to (verified exhaustively over
    // every byte value), not the raw CMP/JC/JBE chain.
    if (bldg_type == BUILDING_TYPE_A_PORT || bldg_type == BUILDING_TYPE_H_PORT) {
        // LAB_0048ee57.
        // 0x0048ee57-0x0048ee61: units[player][0].order += UNIT_STATE_STOP_TO_DEFAULT -- the roster's
        // reserved header-row queue-length-counter idiom (NOT a state transition on a real unit), same
        // pattern sim_unit_create.h/sim_unit_recruit.cpp already document for this exact field.
        unit &header_row = own.unit_at(player, 0);
        header_row.order = static_cast<uint16_t>(header_row.order + UNIT_STATE_STOP_TO_DEFAULT);

        // 0x0048ee68-0x0048eeac: fresh reads of sub_id and building_id (a THIRD independent building_id
        // read in this function), then add_docked = llm_strat_unit_add_docked(Building[building_id].
        // equivalent, player, sub_id).
        const uint8_t  sub_id       = building_of(v, player, building_index).sub_id;
        const uint16_t bid_for_dock = building_of(v, player, building_index).building_id;
        const int32_t  add_docked   = c.unit_add_docked(
            static_cast<uint32_t>(v.cfg_buildings[bid_for_dock].equivalent), player,
            static_cast<uint32_t>(sub_id));

        if (add_docked == 0) {
            // 0x0048eeb5-0x0048eedc: unbind, UNDO the header-row increment, fail. Both
            // llm_strat_prod_unbind_planet call sites in this function load exactly 2 registers
            // (EAX=player, EDX=G_PLANET_INDEX) before their CALL -- see the header's phantom-argument
            // hazard note.
            c.prod_unbind_planet(static_cast<int32_t>(player), *v.planet_index);
            header_row.order = static_cast<uint16_t>(header_row.order - UNIT_STATE_STOP_TO_DEFAULT);
            return -1;
        }

        // 0x0048eee1-0x0048eeeb: new_unit_slot = llm_strat_storage_launch_parked_to_orbit(player,
        // building_index). The asm loads EBX=shuttle_slot right before this CALL, but the committed
        // 2-arg prototype never consumes a third register -- a dead register load, not a real argument.
        const int32_t new_unit_slot = c.storage_launch_parked_to_orbit(player, building_index);

        if (new_unit_slot != 0) {
            // 0x0048ef10-0x0048ef60: stamp the newly-launched unit, clear the building's own
            // shuttle_slot, and mirror the unit's proto id into the slot's type_ref_id.
            own.unit_at(player, new_unit_slot).shuttle_slot      = shuttle_slot;
            own.building_at(player, building_index).shuttle_slot = 0;
            slot.type_ref_id                                     = own.unit_at(player, new_unit_slot).unit_proto_id;

            // 0x0048ef67-0x0048efa2: one more fresh building_id read, for src_building_type.
            const uint16_t bid_final = building_of(v, player, building_index).building_id;
            slot.src_building_type   = static_cast<uint16_t>(v.cfg_buildings[bid_final].type);
            return 1;
        }

        // 0x0048efab-0x0048efb5: launch failed -- unbind and fall through to the shared "return 1"
        // tail (NOT -1; unlike the add_docked failure above, this path is not treated as an error).
        c.prod_unbind_planet(static_cast<int32_t>(player), *v.planet_index);
        return 1;
    }

    if (bldg_type == BUILDING_TYPE_A_MOTHER || bldg_type == BUILDING_TYPE_A_SHUTTLE ||
        bldg_type == BUILDING_TYPE_H_MOTHER || bldg_type == BUILDING_TYPE_H_SHUTTLE) {
        // LAB_0048ee36: buildings[player][building_index].state = DEPLOY_START (0x7d) -- an existing
        // Ghidra `llm_strat_bldg_state` enum member, sim_state.h::BLDG_STATE_DEPLOY_START.
        own.building_at(player, building_index).state = BLDG_STATE_DEPLOY_START;
        return 1;
    }

    // every other type: silent no-op.
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_bldg_depart_finalize(uint16_t player, int32_t building_index, int32_t dest_planet) {
    sim_state st = state();
    return detail::prod_bldg_depart_finalize(st.read, st.own, live_prod_bldg_depart_finalize_calls(),
                                             player, building_index, dest_planet);
}


} // namespace mh::sim
