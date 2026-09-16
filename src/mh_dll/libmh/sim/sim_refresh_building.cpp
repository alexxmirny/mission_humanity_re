//
// sim/sim_refresh_building.cpp -- see sim_refresh_building.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_refresh_building_004705de.asm); every branch target and the two FP idioms were
// walked by hand rather than trusted from the Ghidra .c draft's plate prose (which is wrong about the
// first branch's actual write value -- see the header).
//
#include "sim/sim_refresh_building.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_*/H_* -- pinned there (SIM1C), reused rather
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
// than re-declared (would be a C2374/C2086 redefinition otherwise).

namespace mh::sim {

const refresh_building_calls &live_refresh_building_calls() {
    static const refresh_building_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
    };
    return c;
}

namespace {

// llm_strat_bldg_state / building-TYPE members this function tests (Ghidra enum members read off the
// raw CMP immediates, per this batch's established per-TU idiom -- see e.g.
// sim_bldg_power_network_recompute.cpp's own anonymous-namespace copy of the same BLDG_STATE_* names).
// The four cfg BUILDING_TYPE_* constants already exist as shared vocabulary (sim_order_enqueue.h) and
// are reused rather than re-declared.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
inline constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;

} // namespace

namespace detail {

void refresh_building(const sim_view &v, sim_store &own, const refresh_building_calls &c,
                      uint16_t p_id, int32_t b_id) {
    building &b = own.building_at(p_id, b_id);

    // ---- (0) FIRST BRANCH (0x0047060e-0x00470619 / 0x00470b26-0x00470b4d): no-energy sentinel. -----
    // `energy <= 0.0` here is the JNC-based idiom -- ordered-only, NaN-safe as a plain C++ `<=` with
    // NO flip needed (see the header's derivation; this is NOT the JC-based idiom used elsewhere in
    // this batch). ENERGY is the building's HP-like charge stat, not the power-economy resource.
    if (b.energy <= 0.0) {
        // 0x00470b39-0x00470b4d: the original pokes the RAW bit pattern {0x00000000, 0x3ff00000} into
        // efficiency's two dwords -- that is exactly the IEEE754 double 1.0, not 0.0 (the stale plate
        // prose is wrong here; see the header). A destroyed/dead building's efficiency pins to 1.0.
        b.efficiency = 1.0;
        return;
    }

    const uint16_t state = b.state;
    if (state == BLDG_STATE_CONSTRUCTION || state == BLDG_STATE_UPGRADING ||
        state == BLDG_STATE_DISMANTLING || state == BLDG_STATE_CHARGE_STEP) {
        // ---- (1) SPECIAL STATES (0x0047061f-0x00470718): current_workers-weighted energy fraction,
        // using the POST-upgrade builder_count (via cfg Building.upgrade_index) when state ==
        // UPGRADING, and the building's own cfg builder_count for the other three states. ----
        const cfg_building &cb            = v.cfg_buildings[b.building_id];
        const int32_t       builder_count = (state == BLDG_STATE_UPGRADING)
                                                ? v.cfg_buildings[cb.upgrade_index].builder_count
                                                : cb.builder_count;
        if (builder_count == 0) {
            b.efficiency = b.energy / cb.energy;
        } else {
            b.efficiency = ((double)b.current_workers * b.energy) / ((double)builder_count * cb.energy);
        }
        return;
    }

    // ---- (2) NOT a special state (0x004707fe onward): dispatch on whether this building TYPE staffs
    // workers during normal operation. `uses_workers` is the sibling llm_strat_bldg_uses_workers,
    // called through the ORIGINAL address (mh::call::, per Law 4) even though it is being translated
    // in this same batch -- this site must behave as if it were not yet promoted. ----
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    if (c.uses_workers((uint32_t)p_id, b_id) == 0) {
        // Doesn't use workers: SHUTTLE/MOTHER are always-full (matches llm_strat_done_shuttle's no-op
        // DONE handler); everything else additionally scales by the player's global POWER ratio (the
        // power-economy multiplier -- a DIFFERENT concept from ENERGY, see
        // docs/conventions.md#energy-is-not-power).
        if (cb.type == BUILDING_TYPE_H_SHUTTLE || cb.type == BUILDING_TYPE_A_SHUTTLE ||
            cb.type == BUILDING_TYPE_H_MOTHER || cb.type == BUILDING_TYPE_A_MOTHER) {
            b.efficiency = b.energy / cb.energy;
        } else {
            b.efficiency = (b.energy / cb.energy) * v.power_stats[p_id].ratio;
        }
    } else if (cb.type == BUILDING_TYPE_H_PLANT || cb.type == BUILDING_TYPE_A_PLANT) {
        // PLANT: scales by its own worker occupancy only, NO power-ratio multiply -- plants themselves
        // generate power, so gating their own output by the network ratio would be circular.
        b.efficiency = ((double)b.current_workers * b.energy) / ((double)cb.worker_count * cb.energy);
    } else {
        // Every other worker-using building: worker occupancy AND the global power ratio.
        b.efficiency = (((double)b.current_workers * b.energy) / ((double)cb.worker_count * cb.energy)) *
                       v.power_stats[p_id].ratio;
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void refresh_building(uint16_t p_id, int32_t b_id) {
    sim_state st = state();
    detail::refresh_building(st.read, st.own, live_refresh_building_calls(), p_id, b_id);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
