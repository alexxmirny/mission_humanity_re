//
// sim/sim_bldg_mother_reelect_primary.cpp -- see sim_bldg_mother_reelect_primary.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_mother_reelect_primary_00498aad.asm).
//
#include "sim/sim_bldg_mother_reelect_primary.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER / BUILDING_TYPE_H_MOTHER (reused, not redeclared)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const mother_reelect_primary_calls &live_mother_reelect_primary_calls() {
    static const mother_reelect_primary_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
    };
    return c;
}

namespace {

// cfg_enum_E_UNIT_TYPE members the UNIT search branches on -- no existing header binds these (batch
// context file confirms), so declared locally per its instruction. NOT shared with
// sim_order_enqueue.h's own UNIT_TYPE_* block (a different enum's members entirely).
inline constexpr uint32_t UNIT_TYPE_A_HELI_MOTHER = 0x13u;
inline constexpr uint32_t UNIT_TYPE_H_HELI_MOTHER = 0x14u;

// llm_strat_bldg_state members this function tests, matching the batch's Ghidra enum dump verbatim
// (sim_state.h's own BLDG_STATE_RUBBLE_SIGHT_DECAY is a different function's member; these two are
// this TU's own copy of the shared local-block idiom).
inline constexpr uint16_t BLDG_STATE_IDLE_NOOP_8C        = 0x8cu; // gate for the BUILDING search
inline constexpr uint16_t BLDG_STATE_POWER_PRIMARY_CHECK = 0x8au; // stamped on the chosen building

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only). Both of this function's call sites
// (0x00498af7, 0x00498c4a) are ORDINARY calls to it, not compiler-inlined -- reproduced as this TU's
// own copy of the sim_unit_population_remove.cpp precedent's exact instruction sequence. FISTP width
// confirmed 32-bit AT THIS SITE (opcode bytes `db 5d dc` at both 0x00498afc and 0x00498c4f -> 0xDB
// ModRM 0x5D, reg field 3 -> 0xDB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

int32_t mother_reelect_primary(const sim_view &v, sim_store &own, const mother_reelect_primary_calls &c,
                               int32_t player, int32_t x, int32_t y) {
    const uint32_t p      = static_cast<uint32_t>(player);
    const int32_t  planet = *v.planet_index;

    // ---- search (1): the mobile heli-mother UNIT, only if none is currently tracked ---------------
    // (0x00498ad3-0x00498ae4). Re-read at 0x00498c1f below independently -- not cached across the two
    // blocks, matching translator-brief rule 16.
    if (v.profiles[p].primary_mother_unit[planet] == 0) {
        // 0x00498aea-0x00498afc: headcount countdown, trunc(units[player][0].energy).
        int32_t unit_remaining = trunc_to_int32(unit_of(v, p, 0).energy);
        int32_t unit_chosen    = 0;
        // 0x00498b06-0x00498b13: sentinel "farther than anything real" starting distance.
        int32_t unit_best_dist = (*v.map_width + *v.map_height) * 2;

        // 0x00498b1d-0x00498bfe: slot 0 is the headcount field itself, never a candidate -- the walk
        // starts at 1. Loop bound is BOTH `i < 100` AND `unit_remaining != 0` (0x00498b1d-0x00498b36),
        // either ending the walk.
        for (int32_t i = 1; i < v.caps.units && unit_remaining != 0; ++i) {
            const unit &u = unit_of(v, p, i);

            // 0x00498b46-0x00498b51: `!(energy <= 0.0)`, not `energy > 0.0` -- see the header's NaN
            // note (same FLDZ/FCOMP/FNSTSW/SAHF/JNC idiom sim_bldg_alive.cpp already resolved).
            if (!(u.energy <= 0.0)) {
                // 0x00498b57-0x00498b5a: decrement happens unconditionally once energy>0, BEFORE the
                // type gate below -- preserved in that order even though it has no further effect on
                // this iteration once the type gate fails.
                --unit_remaining;

                // 0x00498b6d-0x00498ba7: cfg type gate, A_HELI_MOTHER or H_HELI_MOTHER.
                const uint32_t unit_type = v.cfg_units[u.unit_proto_id].type;
                if (unit_type == UNIT_TYPE_A_HELI_MOTHER || unit_type == UNIT_TYPE_H_HELI_MOTHER) {
                    // 0x00498bb9-0x00498bdd: llm_strat_tile_dist_wrapped(x, y, cand.x, cand.y) -- see
                    // the header's byte-offset cross-check on why cand.x/cand.y map this way, not
                    // swapped.
                    const int32_t dist = c.tile_dist_wrapped(x, y, u.x, u.y);
                    // 0x00498be5-0x00498bf6: strict `<`, update on strictly nearer only.
                    if (dist < unit_best_dist) {
                        unit_best_dist = dist;
                        unit_chosen    = i;
                    }
                }
            }
        }

        // 0x00498bfe-0x00498c1f: write only if a candidate was actually found.
        if (unit_chosen != 0) {
            own.profile_at(p).primary_mother_unit[planet] = unit_chosen;
        }
    }

    // ---- search (2): the deployed MOTHER building, only if none is currently tracked --------------
    // (0x00498c1f-0x00498c37). If already nonzero, the WHOLE building search is skipped and the
    // function returns 1 -- the one short-circuit path, independent of what search (1) did above.
    int32_t result;
    if (v.profiles[p].primary_mother_bldg[planet] != 0) {
        result = 1;
    } else {
        // 0x00498c3d-0x00498c4f: headcount countdown, trunc(buildings[player][0].energy). Reuses the
        // same physical stack slot search (1) used, but this is a fresh, independent value -- not
        // state carried over from the unit search above.
        int32_t bldg_remaining = trunc_to_int32(building_of(v, p, 0).energy);
        int32_t bldg_chosen    = 0;
        int32_t bldg_best_dist = (*v.map_width + *v.map_height) * 2;

        // 0x00498c70-0x00498d6c: same two-condition loop bound as search (1).
        for (int32_t i = 1; i < v.caps.buildings && bldg_remaining != 0; ++i) {
            const building &b = building_of(v, p, i);

            // 0x00498c99-0x00498ca4: same `!(energy <= 0.0)` NaN-safe idiom as search (1).
            if (!(b.energy <= 0.0)) {
                // 0x00498caa-0x00498cad: decrement unconditional once energy>0, BEFORE the state gate.
                --bldg_remaining;

                // 0x00498cc0-0x00498cc9: state gate, IDLE_NOOP_8C -- unique to the building search,
                // units have no equivalent gate.
                if (b.state == BLDG_STATE_IDLE_NOOP_8C) {
                    // 0x00498cdb-0x00498d15: cfg type gate, H_MOTHER or A_MOTHER (reused from
                    // sim_order_enqueue.h, not redeclared).
                    const uint8_t bldg_type = v.cfg_buildings[b.building_id].type;
                    if (bldg_type == BUILDING_TYPE_H_MOTHER || bldg_type == BUILDING_TYPE_A_MOTHER) {
                        // 0x00498d27-0x00498d4b: same (x, y, cand.x, cand.y) argument order as
                        // search (1).
                        const int32_t dist = c.tile_dist_wrapped(x, y, b.x, b.y);
                        if (dist < bldg_best_dist) {
                            bldg_best_dist = dist;
                            bldg_chosen    = i;
                        }
                    }
                }
            }
        }

        // 0x00498d6c-0x00498da6: on success, write BOTH the profile slot and the chosen building's
        // state -- the ONLY sim-state writes search (2) performs besides the headline slot itself.
        if (bldg_chosen != 0) {
            own.profile_at(p).primary_mother_bldg[planet] = bldg_chosen;
            own.building_at(p, bldg_chosen).state         = BLDG_STATE_POWER_PRIMARY_CHECK;
        }
        result = bldg_chosen; // 0 if none found -- matches the original's uninitialised-to-0 local
    }

    return result;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t mother_reelect_primary(int32_t player, int32_t x, int32_t y) {
    sim_state st = state();
    return detail::mother_reelect_primary(st.read, st.own, live_mother_reelect_primary_calls(), player, x, y);
}


} // namespace mh::sim
