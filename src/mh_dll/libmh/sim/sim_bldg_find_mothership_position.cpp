//
// sim/sim_bldg_find_mothership_position.cpp -- see sim_bldg_find_mothership_position.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_strat_bldg_find_mothership_position_0048fdd0.asm).
//
#include "sim/sim_bldg_find_mothership_position.h"

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER / BUILDING_TYPE_H_MOTHER (reused, not redeclared)
#include "fp/x87.h"                // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only). This function's one call site (0x0048fdfc)
// is an ORDINARY call to it, not compiler-inlined -- reproduced as this TU's own copy of the
// sim_bldg_mother_reelect_primary.cpp / sim_unit_population_remove.cpp precedent's exact instruction
// sequence, per the established per-TU convention (not a new shared helper). FISTP width confirmed
// 32-bit AT THIS SITE (opcode bytes `db 5d ec` @0x0048fe01 -> 0xDB ModRM 0x5D, reg field 3, i.e.
// 0xDB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

uint32_t bldg_find_mothership_position(const sim_view &v, int32_t player, uint32_t *out_x, uint32_t *out_y) {
    const uint32_t p = static_cast<uint32_t>(player);

    // 0x0048fdef-0x0048fe04: headcount countdown, trunc(buildings[player][0].energy) -- the
    // roster's SENTINEL slot, not a real building (see the header's ENERGY-vs-POWER cross-ref).
    int32_t remaining = trunc_to_int32(building_of(v, p, 0).energy);

    // 0x0048fe0b-0x0048fee4: slot cursor starts at 1 -- slot 0 is the count field just read, never
    // itself a candidate. Loop bound is TWO ANDed conditions (0x0048fe0b-0x0048fe17), either ending
    // the walk: `slot < 100` (BUILDINGS_PER_PLAYER) and `remaining != 0`.
    for (int32_t slot = 1; slot < v.caps.buildings && remaining != 0; ++slot) {
        const building &b = building_of(v, p, slot);

        // 0x0048fe34-0x0048fe3f: `!(energy <= 0.0)`, not `0.0 < energy` -- see the header's NaN
        // note (same FLDZ/FCOMP/FNSTSW/SAHF/JNC idiom sim_bldg_mother_reelect_primary.cpp already
        // resolved for the identical pattern).
        if (!(b.energy <= 0.0)) {
            // 0x0048fe45-0x0048fe48: decrement happens unconditionally once energy>0, BEFORE the
            // type gate below -- preserved in that order even though it has no further effect on
            // this iteration once the type gate fails.
            --remaining;

            // 0x0048fe4b-0x0048fe95: cfg type gate, A_MOTHER(0x06) checked first, H_MOTHER(0x1a)
            // second -- building_id is re-fetched independently for each CMP in the original; a
            // single reference here is value-identical (no write can occur between the two reads in
            // this straight-line body).
            const uint8_t bldg_type = v.cfg_buildings[b.building_id].type;
            if (bldg_type == BUILDING_TYPE_A_MOTHER || bldg_type == BUILDING_TYPE_H_MOTHER) {
                // 0x0048fe97-0x0048fecd: found -- write both out params and return 1 WITHOUT
                // visiting any further slot (the asm's fallthrough from either type-match branch
                // straight into the write-and-return tail).
                *out_x = static_cast<uint32_t>(b.x);
                *out_y = static_cast<uint32_t>(b.y);
                return 1;
            }
        }
    }

    // 0x0048fedd: exhausted with no match -- result 0, *out_x/*out_y left UNTOUCHED (the asm never
    // touches EDX/EBX's out-pointers on this path).
    return 0;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

uint32_t bldg_find_mothership_position(int32_t player, uint32_t *out_x, uint32_t *out_y) {
    const sim_view v = state().read;
    return detail::bldg_find_mothership_position(v, player, out_x, out_y);
}


} // namespace mh::sim
