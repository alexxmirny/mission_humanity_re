//
// sim/sim_unit_calc_range_approach_point.cpp -- see sim_unit_calc_range_approach_point.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_calc_range_approach_point_00488291.asm), not
// from the Ghidra .c draft.
//
#include "sim/sim_unit_calc_range_approach_point.h"

#include "addr/mh_calls.gen.h"    // typed callables for the original functions we still call OUT to
#include "sim/sim_target_class.h" // mh::sim::target_class -- reimplemented sibling

#include <cstdlib>              // std::abs
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_calc_range_approach_point_calls &live_unit_calc_range_approach_point_calls() {
    static const unit_calc_range_approach_point_calls c = {
        mh::sim::target_class,
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
        MH_LIBMH_BIND(llm_map_wrap_delta_x),
        MH_LIBMH_BIND(llm_map_wrap_delta_y),
    };
    return c;
}

namespace {
// Same "+-1 sign, 0 maps to +1" idiom the .asm computes via abs()/IDIV (see the header banner).
inline int32_t sign_or_one(int32_t x) { return (x == 0) ? 1 : ((x > 0) ? 1 : -1); }
} // namespace

namespace detail {

int32_t unit_calc_range_approach_point(const sim_view &v, const unit_calc_range_approach_point_calls &c,
                                       int32_t *io_target_x, int32_t *io_target_y) {
    const unit &u = *v.cur_unit;

    // 0x004882ae-0x004882bb: sentinel, deliberately larger than any real toroidal distance.
    int32_t best_delta = 2 * (*v.map_width + *v.map_height);

    // 0x004882be-0x004882db: classify the current target. target_index (int16_t) is ZERO-extended
    // (MOVZX) here in the original, matching every other target_class caller in this codebase
    // (sim_unit_chase_check.cpp, sim_unit_state_attack_unit.cpp, sim_unit_state_move_walker.cpp,
    // sim_unit_state_move_path.cpp) -- a bare int16_t->int32_t cast would sign-extend instead.
    const int32_t target_class_result = c.target_class(
        static_cast<uint32_t>(u.target_ref), static_cast<int32_t>(static_cast<uint16_t>(u.target_index)));

    // 0x004882de-0x004882eb: save the caller's incoming target position before overwriting it below.
    const int32_t saved_target_x = *io_target_x;
    const int32_t saved_target_y = *io_target_y;

    // 0x004882ee-0x0048831a: DEFAULT WRITE -- stay at the unit's own tile.
    *io_target_x = u.x;
    *io_target_y = u.y;

    // 0x0048831c-0x0048833b: distance from the unit's own tile to the (saved) target, computed once.
    const int32_t dist = c.tile_dist_wrapped(u.x, u.y, saved_target_x, saved_target_y);

    const uint8_t wanted_bit = (target_class_result == 1) ? WEAPON_TARGET_GROUND : WEAPON_TARGET_AIR;

    // 0x0048833e-0x00488469: scan mount slots 0..3. CORRECTED (reimpl-verify, 2026-08-22): a match
    // whose `dist` already lies inside [range_min, range_max] is an IMMEDIATE EARLY RETURN
    // (0x004883f9-0x00488402: `local_14 = match_count; JMP 0x00488614` -- 0x00488614 is the function's
    // real epilogue), NOT "continue scanning" -- the step-4 DEFAULT WRITE (the unit's own tile) stands
    // untouched and the wrap-delta/final-write block below never runs for this call at all. Only a
    // genuinely OUT-OF-RANGE match (dist<range_min or dist>range_max, the JL/JLE-not-taken paths at
    // 0x004883ed/0x004883f5) falls through to the shortfall computation and keeps scanning. This ALSO
    // means the earlier "reads an uninitialised stack local" reading of this block was a
    // misidentification of the control flow, not a real preserve-bug: reaching the shortfall compute
    // (0x00488404) is now PROVABLY only possible when dist is outside the range, so the shortfall local
    // is always freshly written there -- see the header banner's own correction.
    int32_t match_count = 0;
    for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS; ++slot) {
        const unit_weapon &uw = u.weapons[slot];
        if (uw.enabled_2 == 0) continue; // 0x00488368: JZ skip to next slot

        const cfg_weapon &w = v.cfg_weapons[uw.weapon_id];
        if ((w.target & wanted_bit) == 0) continue; // 0x00488395/0x004883a7: JZ skip to next slot

        ++match_count; // 0x004883a9-0x004883ac

        const int32_t range_min = w.range_min[*v.cur_player];
        const int32_t range_max = w.range_max[*v.cur_player];

        if (dist >= range_min && dist <= range_max) {
            // 0x004883f9-0x00488402: EARLY RETURN. *io_target_x/*io_target_y stay at the step-4
            // default (the unit's own tile); the wrap-delta/final-write block never runs.
            return match_count;
        }

        // 0x00488404-0x00488428: genuinely out of range -- compute the shortfall and maybe fold it
        // into best_delta (0x0048845c: JLE keeps the current best, else updates -- strict <).
        // CORRECTED (reimpl-verify, 2026-08-22): the "too close" branch (LAB_00488417,
        // 0x0048841f-0x00488425) computes `dist - range_min`, NOT `range_min - dist` -- a genuinely
        // NEGATIVE shortfall when dist<range_min, mirroring the "too far" branch's positive one. An
        // earlier pass had this sign flipped, which would have inverted sign_best and moved the unit
        // toward a target it should back away from (and vice versa).
        const int32_t delta = (dist > range_max) ? (dist - range_max) : (dist - range_min);
        if (std::abs(delta) < std::abs(best_delta)) best_delta = delta;
    }

    // 0x00488469-0x0048846d: no matching weapon -- the DEFAULT WRITE above stands.
    if (match_count == 0) return match_count;

    // 0x00488473-0x00488498: toroidal-wrapped delta from the unit's own tile to the (saved) target.
    const int32_t delta_x = c.wrap_delta_x(u.x, u.y, saved_target_x);
    const int32_t delta_y = c.wrap_delta_y(u.x, u.y, saved_target_x, saved_target_y);

    const int32_t sign_x    = sign_or_one(delta_x);
    const int32_t sign_y    = sign_or_one(delta_y);
    const int32_t sign_best = sign_or_one(best_delta);

    int32_t dx_mag, dy_mag;
    if (dist != 0) {
        // 0x00488549-0x0048859e: dx_mag = |best_delta| * |delta_x| / dist -- the x axis' share of the
        // total shortfall, proportional to delta_x's share of the (unwrapped) distance.
        const int32_t abs_dx   = std::abs(delta_x);
        const int32_t abs_best = std::abs(best_delta);
        dx_mag                 = abs_best * abs_dx / dist;
        // 0x004885a1-0x004885c7: dy_mag = |best_delta| - dx_mag -- the REMAINDER, not an independent
        // ratio calc against delta_y (re-derived from the raw bytes: this is a SUB, not a second
        // IMUL/IDIV pair) -- a partition of the total magnitude, not two separate proportions.
        dy_mag = abs_best - dx_mag;
    } else {
        // 0x004885cf-0x004885d5: dist==0 -- dx_mag is the SIGNED best_delta (NOT its absolute value,
        // unlike the dist!=0 branch above -- re-verified, this asymmetry is genuine, not a translation
        // slip), dy_mag is 0. The final combine below still multiplies dx_mag by sign_x*sign_best, so
        // this branch's sign gets applied on top of best_delta's own sign -- reproduced as-is.
        dx_mag = best_delta;
        dy_mag = 0;
    }

    // 0x004885dc-0x0048860c: re-sign each axis, add to the unit's own tile, wrap through the torus
    // masks -- the FINAL write, replacing the step-4 default.
    *io_target_x = (u.x + dx_mag * sign_x * sign_best) & static_cast<int32_t>(v.geom->width_mask);
    *io_target_y = (u.y + dy_mag * sign_y * sign_best) & static_cast<int32_t>(v.geom->height_mask);

    return match_count;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_calc_range_approach_point(uint32_t *io_target_x, uint32_t *io_target_y) {
    sim_state st = state();
    // Committed row is `uint *io_target_x, uint *io_target_y`; detail:: keeps its verified `int32_t *`
    // params -- reinterpret_cast across the same-width signedness mismatch (TACT1-P C6, 2026-09-04).
    return detail::unit_calc_range_approach_point(
        st.read, live_unit_calc_range_approach_point_calls(),
        reinterpret_cast<int32_t *>(io_target_x), reinterpret_cast<int32_t *>(io_target_y));
}

} // namespace mh::sim
