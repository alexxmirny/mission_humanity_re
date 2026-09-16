//
// sim/resid/sim_landing_spots_reroll.cpp -- see sim_landing_spots_reroll.h. Translated from the
// DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_landing_spots_reroll_out_of_bounds_00454de5.asm), the Ghidra .c
// being a draft.
//
#include "sim/resid/sim_landing_spots_reroll.h"

#include "addr/mh_calls.gen.h"  // typed callables for the effectful/frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const landing_spots_reroll_calls &live_landing_spots_reroll_calls() {
    static const landing_spots_reroll_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
    };
    return c;
}

namespace detail {

// ---- llm_strat_landing_spots_reroll_out_of_bounds @0x00454de5 ---------------------------------
void landing_spots_reroll_out_of_bounds(const sim_view &v, sim_store &own, const landing_spots_reroll_calls &c) {
    // 0x00454e2b/0x00454e3d and 0x00454e4f/0x00454e77 all compare against the SAME two globals --
    // read once. These are the map tile counts (RID_WIDTH/RID_HEIGHT), NOT width_m/height_m (the
    // torus-wrap masks) -- see the header banner.
    const int32_t width  = *v.map_width;
    const int32_t height = *v.map_height;

    // 0x00454dfd-0x00454e97: two-part guard evaluated BEFORE each iteration -- status != -1, THEN
    // index < 16 (short-circuit, in that order) -- same shape as sim_landing_queries.cpp's
    // count_landing_spots and sim_landing_spot.cpp's claim_landing_spot scan loops. Transcribed
    // literally, not normalised to a bare `i < 16` for-loop.
    for (int32_t i = 0; own.landing_spot_at(i).status != -1 && i < 16; ++i) {
        // 0x00454e21-0x00454e43: entry test -- x > width OR y > height (signed, strict `>`; this
        // is the preserved off-by-one against a 0-based grid, see the header banner). Skip the
        // whole reroll block when neither axis is out of range.
        if (own.landing_spot_at(i).x > width || own.landing_spot_at(i).y > height) {
            // 0x00454e45-0x00454e6d: x is rerolled first, re-testing `x > width` on entry to the
            // block (same condition, re-evaluated, not assumed from the outer test).
            if (own.landing_spot_at(i).x > width) {
                own.landing_spot_at(i).x = c.rand_below(width); // 0x00454e57-0x00454e67
            }
            // 0x00454e6d-0x00454e95: then y, re-testing `y > height`. Runs unconditionally after
            // the x arm (whether or not x was rerolled), so a spot with x in range and y out of
            // range draws exactly once (y); a spot with both out of range draws twice, x then y.
            if (own.landing_spot_at(i).y > height) {
                own.landing_spot_at(i).y = c.rand_below(height); // 0x00454e7f-0x00454e8f
            }
        }
    }

    // 0x00454e97-0x00454ea4: the exit dispatch re-runs the loop header's two comparisons (status
    // != -1, index < 16) but neither result feeds a conditional jump before the epilogue -- dead,
    // no observable effect. Omitted; see the structured report's uncertainties[].
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void landing_spots_reroll_out_of_bounds() {
    sim_state st = state();
    detail::landing_spots_reroll_out_of_bounds(st.read, st.own, live_landing_spots_reroll_calls());
}

} // namespace mh::sim
