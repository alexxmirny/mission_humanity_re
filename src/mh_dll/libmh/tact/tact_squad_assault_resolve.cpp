//
// tact/tact_squad_assault_resolve.cpp -- see tact_squad_assault_resolve.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_squad_assault_resolve.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): llm_strat_unit_remove_from_map/_teardown
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::tact {

const squad_assault_resolve_calls &live_squad_assault_resolve_calls() {
    static const squad_assault_resolve_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_remove_from_map),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (ST0-in/ST0-out, x87-register-only -- not stack-passable, hence not a
// mh::call:: stub). This TU's own copy of the sim_bldg_economy.cpp precedent's exact instruction
// sequence (each TU keeps its own copy, per that file's own comment). FISTP width confirmed 32-bit
// at this call site (0x0044d9db: `db5db8`, ModRM reg field 3 -> DB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

void squad_assault_resolve(const tact_view &tv, tact_store &own, const mh::sim::sim_view &sim_read,
                           mh::sim::sim_store &sim_own, const squad_assault_resolve_calls &c) {
    const int32_t scan_player = *tv.squad_bb_scan_player;

    // @0x0044d844-0x0044d982: walk the 64-slot squad-status blackboard (tact-owned,
    // _G_LLM_SQUAD_STATUS).
    int32_t cursor = 0;
    while (cursor < TACT_SQUAD_STATUS_SLOTS) {
        const int32_t unit_idx = own.squad_status_at(cursor).unit_slot_index;
        if (unit_idx == 0) {
            ++cursor;
            continue;
        }

        mh::sim::unit           &u     = sim_own.unit_at(scan_player, unit_idx);
        const mh::sim::cfg_unit &proto = sim_read.cfg_units[u.unit_proto_id];

        // Sum this unit's soldiers' energy_pct, one blackboard slot per soldier -- UNCHECKED
        // against the 64-slot bound, matching the original (@0x0044d89b-0x0044d8c2).
        int32_t energy_pct_sum = 0;
        for (int32_t i = 0; i < proto.soldier_count; ++i) {
            energy_pct_sum += own.squad_status_at(cursor).energy_pct;
            ++cursor;
        }

        const double loss = (static_cast<double>(energy_pct_sum) * proto.energy) /
                            (static_cast<double>(proto.soldier_count) *
                             *tv.squad_assault_power_percent_scale);

        // GENUINE 3-WAY BRANCH (reimpl-verify caught a 2-way collapse here, 2026-08-26): the
        // JC @0x0044d900 takes loss>0 to 0x0044d935; the JNC @0x0044d959 there takes
        // delta(=energy-loss)<=0 STRAIGHT to the tail (0x0044d97a), past the kill block AND the
        // pending_damage add -- a real no-op, not a kill. Only loss<=0 falls into the kill block.
        if (loss <= 0.0) {
            // @0x0044d902-0x0044d933: the unit is destroyed.
            c.unit_remove_from_map(static_cast<uint16_t>(scan_player), unit_idx);
            c.unit_teardown(static_cast<uint32_t>(scan_player), static_cast<uint16_t>(unit_idx));
            sim_own.profile_at(scan_player).units_lost_total[*sim_read.planet_index] += 1;
        } else {
            const double delta = u.energy - loss;
            if (delta > 0.0) {
                u.pending_damage += delta;
            }
            // else: loss>0.0 && delta<=0.0 -- no-op, matching the 0x0044d959 JNC fall-through.
        }
    }

    // @0x0044d987-0x0044da4f: the target building's own energy-loss check. Re-read fresh here --
    // the original re-reads both globals every LOOP ITERATION above too (@0x0044d844), harmlessly,
    // since nothing in the loop body writes either one.
    const int32_t target_owner        = *tv.squad_bb_target_owner;
    const int32_t target_building_idx = *tv.squad_bb_target_building_idx;

    mh::sim::building           &b      = sim_own.building_at(target_owner, target_building_idx);
    const mh::sim::cfg_building &bproto = sim_read.cfg_buildings[b.building_id];

    const int32_t pct =
        trunc_to_int32(b.energy * *tv.bldg_energy_to_percent_scale / bproto.energy);

    if (*tv.squad_bb_target_energy_pct < pct) {
        b.pending_damage += b.energy - (static_cast<double>(*tv.squad_bb_target_energy_pct) *
                                        bproto.energy / *tv.bldg_energy_percent_to_abs_scale);
    }
}

} // namespace detail

void squad_assault_resolve() {
    tact_state         st     = state();
    mh::sim::sim_state sim_st = mh::sim::state();
    detail::squad_assault_resolve(st.read, st.own, sim_st.read, sim_st.own,
                                  live_squad_assault_resolve_calls());
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
