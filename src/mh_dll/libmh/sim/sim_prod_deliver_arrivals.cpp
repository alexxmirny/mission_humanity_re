//
// sim/sim_prod_deliver_arrivals.cpp -- see sim_prod_deliver_arrivals.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_prod_deliver_arrivals_0048dc65.asm), not from the Ghidra .c
// draft (independently re-walked and found to match it exactly -- see the header).
//
#include "sim/sim_prod_deliver_arrivals.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_deliver_arrivals_calls &live_prod_deliver_arrivals_calls() {
    static const prod_deliver_arrivals_calls c = {
        MH_LIBMH_BIND(llm_strat_locate_active_port),
        MH_LIBMH_BIND(llm_strat_prod_bind_planet),
        MH_LIBMH_BIND(llm_strat_bldg_find_mothership_position),
        MH_LIBMH_BIND(llm_strat_prod_spawn_arrived_unit),
    };
    return c;
}

namespace {

// This function's own literal operand with no backing generated C++ enum (DECLARED NEED 1, header
// banner): E_PLANET_STATUS member 0, which Ghidra's own decompile already renders as "UNKNOWN".
// Anonymous-namespace (internal linkage) rather than `mh::sim` scope on purpose -- see the header
// banner's citation of sim_unit_type_predicates.h's C2374/C2086 precedent for why a same-named
// namespace-scope constant declared independently in two sim/ headers is a real hazard here, not a
// hypothetical one.
constexpr int32_t PLANET_STATUS_UNKNOWN = 0;

// mh_llm_prod_shuttle_slot::status's own field comment already documents 0xc9 as "arrived, ready to
// spawn" -- re-derived locally per this project's per-TU convention (libmh/sim/ TUs each define their own
// copy of a small literal rather than sharing one across files; sim_dock_slot_is_busy.cpp documents the
// same choice for an even more literally-identical case). int16_t to match the field's own type.
constexpr int16_t STATUS_ARRIVED_READY_TO_SPAWN = static_cast<int16_t>(0xc9);

} // namespace

namespace detail {

void prod_deliver_arrivals(const sim_view &v, sim_store &own, const prod_deliver_arrivals_calls &c) {
    // 0x0048dc7d-0x0048de9d: player 0..7 (MAX_PLAYERS), slot 1..9 (PROD_SHUTTLE_SLOTS_PER_PLAYER is
    // 10 -- slot 0 is deliberately never visited; reproduced as-is, not "fixed").
    for (int32_t player = 0; player < MAX_PLAYERS; ++player) {
        for (int32_t slot = 1; slot < PROD_SHUTTLE_SLOTS_PER_PLAYER; ++slot) {
            // 0x0048dcb1-0x0048dcf0: ONE fetch, reused for every read below -- no outward call
            // separates any of them, so this is behaviourally identical to the asm's own four
            // re-derivations of the same address (see header banner).
            const prod_shuttle_slot &s =
                v.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot];

            // 0x0048dcb1-0x0048dceb: the arrival gate -- status==ARRIVED_READY_TO_SPAWN(0xc9) AND
            // origin_planet==G_PLANET_INDEX (origin_planet read unsigned/zero-extended, matching the
            // asm's MOVZX).
            if (s.status != STATUS_ARRIVED_READY_TO_SPAWN ||
                static_cast<int32_t>(static_cast<uint16_t>(s.origin_planet)) != *v.planet_index) {
                continue;
            }

            // 0x0048dcf0-0x0048ddd3: read once, reused for all four CMPs the asm performs against the
            // same value (see header banner).
            const uint32_t unit_type = v.cfg_units[s.type_ref_id].type;

            if (unit_type == UNIT_TYPE_A_HELI_CARGO || unit_type == UNIT_TYPE_H_HELI_CARGO) {
                // ---- cargo-heli branch (0x0048dd3c-0x0048dd7e) ----------------------------------
                // col/row/port_slot are LOCAL scratch for this call only -- NOT the persistent
                // CAM_PAN_TARGET_COL/_ROW globals (see header banner).
                // port_slot: uint32_t -- locate_active_port's committed 4th out-param is uint32_t *
                // (TACT1-P C6, 2026-09-04), unlike its int32_t * col/row pair.
                int32_t        col = 0, row = 0;
                uint32_t       port_slot = 0;
                const uint32_t found =
                    c.locate_active_port(static_cast<uint32_t>(player), &col, &row, &port_slot);
                if (found != 0) {
                    const int32_t bound = c.prod_bind_planet(player, *v.planet_index, slot);
                    if (bound != 0) {
                        // Return value discarded, matching the original.
                        c.spawn_arrived_unit(static_cast<uint16_t>(player), static_cast<uint32_t>(slot),
                                             static_cast<uint32_t>(col), static_cast<uint32_t>(row),
                                             port_slot);
                    }
                }
            } else if (unit_type == UNIT_TYPE_A_HELI_MOTHER || unit_type == UNIT_TYPE_H_HELI_MOTHER) {
                // ---- mothership branch (0x0048ddd3-0x0048de93) -----------------------------------
                // x/y: uint32_t -- find_mothership_position's committed out-params are uint32_t *
                // (TACT1-P C6, 2026-09-04).
                uint32_t       x = 0, y = 0;
                const uint32_t found =
                    c.find_mothership_position(player, &x, &y); // EAX=player, full dword (not truncated)
                if (found == 0) {
                    // 0x0048ddec-0x0048de17: fallback to the player's own recorded landing site for
                    // the current planet.
                    x = v.profiles[player].landing_x[*v.planet_index];
                    y = v.profiles[player].landing_y[*v.planet_index];
                }

                // 0x0048de1a-0x0048de2f: 5th arg is a LITERAL 0 (unlike the cargo branch's port_slot
                // pass-through). Return value IS captured this time.
                const uint32_t spawned =
                    c.spawn_arrived_unit(static_cast<uint16_t>(player), static_cast<uint32_t>(slot), x, y, 0);

                // 0x0048de32-0x0048de93: local-player control-group auto-select -- five chained AND
                // gates (see header banner for each one's address and the signed-return note in
                // uncertainties[]).
                if (static_cast<uint16_t>(*v.player_side) == static_cast<uint16_t>(player) &&
                    *v.sim_active == 0 && v.planet_status[*v.planet_index] == PLANET_STATUS_UNKNOWN &&
                    static_cast<int32_t>(spawned) > 0 &&
                    unit_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(spawned))
                            .unit_proto_id != 0) {
                    // 0x0048de80-0x0048de8f: two separate stores (count, then unit_ids[0]).
                    ctrl_group &grp = own.ctrl_group_at(0);
                    grp.count       = 1;
                    grp.unit_ids[0] = static_cast<uint16_t>(spawned);
                }
            }
            // else (0x0048de93): neither cargo nor mother -- no action, continue to the next slot.
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void prod_deliver_arrivals() {
    sim_state st = state();
    detail::prod_deliver_arrivals(st.read, st.own, live_prod_deliver_arrivals_calls());
}


} // namespace mh::sim
