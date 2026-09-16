//
// sim/sim_bldg_power_network_recompute.cpp -- see sim_bldg_power_network_recompute.h. Translated
// from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_power_network_recompute_00491c76.asm), not from the Ghidra .c draft
// (which reads correctly here and was used as a cross-check for the boolean gates, but the loop
// exit-test and the trunc idiom were re-derived from the raw bytes -- see the header).
//
#include "sim/sim_bldg_power_network_recompute.h"

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_* constants (this closure's cfg_building.type gates)

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const power_network_recompute_calls &live_power_network_recompute_calls() {
    static const power_network_recompute_calls gc = {
        MH_LIBMH_BIND(llm_strat_mother_reelect_primary),
        MH_LIBMH_BIND(llm_strat_bldg_clear_flag_bit0_notify),
        MH_LIBMH_BIND(llm_strat_bldg_propagate_network_connectivity),
        MH_LIBMH_BIND(llm_bldg_set_connected_flag),
    };
    return gc;
}

namespace {

// llm_strat_bldg_state members this function tests (Ghidra enum dump, per the batch context file --
// reused verbatim, this TU's own anonymous-namespace copy of only the members it needs, matching the
// established per-TU idiom in this subsystem).
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This call site
// (0x00491d5b-0x00491d66) FLDs buildings[player][0].energy directly AS a double and CALLs the
// original, then FISTPs the truncated result as a 32-bit int -- same "ordinary call, not an inlined
// copy" situation as sim_unit_population_remove.cpp's trunc_to_int32() (copied verbatim per the batch
// context file's instruction; each TU keeps its own copy of this idiom, it is not a new shared
// helper).
//
// WIDTH VERIFIED AT THIS SITE: opcode bytes at 0x00491d66 are `db 5d e8` -> 0xDB, ModRM 0x5D
// (mod=01,reg=011,rm=101) -> reg field 3 -> 0xDB /3 == FISTP m32int. 32-bit, matching the precedent
// group, not the 64-bit alternative -- verified, not assumed, per the batch context file's
// instruction.
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

void power_network_recompute(const sim_view &v, const power_network_recompute_calls &gc,
                             uint16_t player) {
    const int32_t         planet = *v.planet_index;
    const player_profile &prof   = v.profiles[player];

    // ---- (0) re-elect the primary mother if unset (0x00491c8d-0x00491cef) ----------------------
    if (prof.primary_mother_bldg[planet] == 0) {
        gc.mother_reelect_primary(player, prof.landing_x[planet], prof.landing_y[planet]);
    }

    // ---- (1) per-building maintenance pass (0x00491cef-0x00491d20) ------------------------------
    // Same `.index`-as-occupancy-and-count idiom as sim_bldg_refresh_all_buildings.h documents, but
    // with a DIFFERENT loop-exit test -- see the header banner: `!= 0`, not `> 0`.
    {
        int32_t live_count = (uint16_t)building_of(v, player, 0).index;
        int32_t slot       = 1;
        while (slot < v.caps.buildings && live_count != 0) {
            if (building_of(v, player, slot).index != 0) {
                --live_count;
                gc.bldg_clear_flag_bit0_notify(player, slot);
            }
            ++slot;
        }
    }

    // ---- (2) flood-seed connectivity from every online mother/plant (0x00491d51-0x00491ec9) -----
    {
        int32_t live_count = trunc_to_int32(building_of(v, player, 0).energy);
        int32_t slot       = 1;
        while (slot < v.caps.buildings && live_count != 0) {
            const building &b = building_of(v, player, slot);
            // !(b.energy <= 0.0), NOT the plain `0.0 < b.energy` a first draft used here (energy =
            // the HP/charge stat, NOT the POWER resource -- docs/conventions.md#energy-is-not-power).
            // CONFIRMED A REAL
            // DIVERGENCE by reimpl-verify (2026-08-13): the original tests this with x87 FLDZ/FCOMP/
            // FNSTSW/SAHF/JNC (0x00491d9c-0x00491da7), which takes the "proceed" arm on CF=1 --
            // true both for energy>0.0 AND for an UNORDERED (NaN) compare (C0=1 on unordered FCOM
            // loads into CF via SAHF, the same bit JNC reads for the ordinary ST0<src case). A plain
            // `0.0 < b.energy` is false for NaN per IEEE 754 and wrongly takes the "skip" arm. This
            // is the SAME NaN-safe idiom sim_bldg_mothership_alive.cpp / sim_bldg_mother_reelect_
            // primary.cpp already use correctly for the byte-identical FLDZ/FCOMP/FNSTSW/SAHF/JNC
            // sequence -- this site was the one place in the batch that had it backwards.
            if (!(b.energy <= 0.0)) {
                // Decrement is UNCONDITIONAL once energy > 0.0 -- happens before the type/state gate
                // below is even evaluated (0x00491dad-0x00491db3), matching the .c draft's comma
                // expression.
                --live_count;
                const uint8_t type      = v.cfg_buildings[b.building_id].type;
                const bool    is_mother = (type == BUILDING_TYPE_H_MOTHER) || (type == BUILDING_TYPE_A_MOTHER);
                const bool    is_plant  = (type == BUILDING_TYPE_H_PLANT) || (type == BUILDING_TYPE_A_PLANT);
                const bool    call_it =
                    (is_mother && (prof.primary_mother_bldg[planet] == slot || b.online_state == 0)) ||
                    (is_plant && b.online_state != 0);
                if (call_it) {
                    gc.bldg_propagate_network_connectivity(player, slot);
                }
            }
            ++slot;
        }
    }

    // ---- (3) force-mark shuttles/ports/idle-mothers/dismantling buildings connected
    // (0x00491ec9-0x00492092) -----------------------------------------------------------------------
    {
        int32_t live_count = (uint16_t)building_of(v, player, 0).index;
        int32_t slot       = 1;
        while (slot < v.caps.buildings && live_count != 0) {
            const building &b = building_of(v, player, slot);
            if (b.index != 0) {
                // Decrement is UNCONDITIONAL once index != 0 -- before the type/state gate below,
                // same shape as (1)/(2).
                --live_count;
                const uint8_t  type       = v.cfg_buildings[b.building_id].type;
                const uint16_t state      = b.state;
                const bool     is_shuttle = (type == BUILDING_TYPE_H_SHUTTLE) || (type == BUILDING_TYPE_A_SHUTTLE);
                const bool     is_port    = (type == BUILDING_TYPE_H_PORT) || (type == BUILDING_TYPE_A_PORT);
                const bool     is_mother  = (type == BUILDING_TYPE_H_MOTHER) || (type == BUILDING_TYPE_A_MOTHER);
                // The DISMANTLING arm is a TOP-LEVEL alternative -- reached for every type that fails
                // the shuttle/port checks, mother or not (see the header banner's control-flow note),
                // not nested under the mother gate.
                const bool call_it =
                    is_shuttle || is_port ||
                    ((is_mother && state != BLDG_STATE_CONSTRUCTION && prof.primary_mother_bldg[planet] != slot) ||
                     (state == BLDG_STATE_DISMANTLING));
                if (call_it) {
                    gc.bldg_set_connected_flag(player, slot);
                }
            }
            ++slot;
        }
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void power_network_recompute(uint16_t player) {
    const sim_view v = state().read;
    detail::power_network_recompute(v, live_power_network_recompute_calls(), player);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
