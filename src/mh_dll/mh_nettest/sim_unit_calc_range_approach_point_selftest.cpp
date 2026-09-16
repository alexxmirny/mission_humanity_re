//
// sim_unit_calc_range_approach_point_selftest.cpp -- `simtest` cases for the offline oracle of
//   llm_strat_unit_calc_range_approach_point @0x00488291 (sim/sim_unit_calc_range_approach_point.h/.cpp)
//
// GENUINELY NOT SHADOWABLE (see the .h banner) -- both real outputs are writes through CALLER-OWNED
// out-pointers, so this file is the only correctness gate this function gets.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY -- NOT from the .cpp body, NOT from the .h
// banner's own prose (re-derived address-by-address from tmp/decomp_sim/
// llm_strat_unit_calc_range_approach_point_00488291.asm, 0x38f bytes, read in full).
//
// ---- SCOPE ------------------------------------------------------------------------------------------
// COVERS: the target_class call's exact argument tuple (0x004882be-0x004882db); the unconditional
// DEFAULT WRITE and that it clobbers the caller's original out-params even on a total no-match
// (0x004882ee-0x0048831a); that `dist` is computed from the unit's OWN tile and the CALLER's ORIGINAL
// target position, saved BEFORE the default write (0x0048832a-0x00488336); the 4-slot scan's
// enabled_2/target-mask gates and that ALL FOUR SLOTS ARE ALWAYS VISITED, no first-match-wins early-out
// (0x0048833e-0x00488469); match_count==0 leaving the default write standing and returning 0
// (0x00488469-0x0048846d); the wrap_delta_x/wrap_delta_y call argument tuples (0x00488473-0x00488498);
// the sign_or_one("0 maps to +1") reduction on delta_x/delta_y/best_delta; BOTH the dist!=0 branch
// (dx_mag = |best_delta|*|delta_x|/dist TRUNCATING, dy_mag = |best_delta|-dx_mag as the REMAINDER, not
// an independent calc) and the dist==0 branch (dx_mag = best_delta SIGNED, dy_mag = 0); and the final
// combine's AND-mask wrap on a negative two's-complement sum (0x004885dc-0x0048860c).
//
// CONDUCTOR NOTE (2026-08-22, after this file was authored): findings (2) and (3) below -- the
// missing early return and the sign-flipped "too close" shortfall -- were CONFIRMED and FIXED in the
// production .cpp (independently, via the reimpl-verify workflow, which found the same two bugs).
// Cases A-E below were authored to route AROUND both bugs (their own stated purpose) and remain valid,
// passing evidence for everything they cover -- they just don't exercise the two fixed paths directly.
// Cases F and G were added AFTER the fix specifically to pin the corrected behaviour: F proves the
// early return (match_count returned immediately, default write standing, no wrap_delta_x/y calls);
// G proves the "too close" shortfall is now negative (dist-range_min), not positive, and that its sign
// propagates through sign_best into the final combine. Finding (1) (the header's own original DECLARED
// DIVERGENCE, an uninitialised-stack read) is now provably unreachable given the early-return fix (see
// sim_unit_calc_range_approach_point.h's own correction) and finding (4) (target_class's target_index
// sign- vs zero-extension) was also fixed in the .cpp.
//
// EXCLUDES, by construction, THREE things -- one is the header's own DECLARED DIVERGENCE, the other
// two are NEW divergences this file's derivation found and is reporting rather than silently coding
// around (see the session report for the full write-up):
//
//   (1) DECLARED (the .h banner's own note): the very first matching weapon slot being exactly
//       in-range reads an uninitialised stack local in the original. Not applicable here anyway, once
//       (2) below is accounted for -- see (2).
//
//   (2) NEW -- EARLY RETURN ON ANY IN-RANGE MATCH, not just the first (0x004883ea-0x004883ff): the
//       raw bytes show that the moment ANY matching weapon's `dist` falls inside [range_min, range_max]
//       (CMP/JL at 0x004883ed against range_min, CMP/JLE at 0x004883f5 against range_max -- both
//       conditions false-through means in-range), the function stages match_count as the return value
//       and JMPs STRAIGHT to the epilogue at 0x00488614 -- LAB_004883f9's three instructions ARE the
//       return sequence, not a "fall through to the abs-compare step" as the .h banner's DECLARED
//       DIVERGENCE section describes. This skips every remaining weapon slot AND the entire
//       wrap-delta/combine section, leaving the step-4 DEFAULT WRITE standing. The .cpp has no such
//       branch at all -- it just skips the best_delta update and keeps scanning. This is NOT the
//       declared divergence (which is about one uninitialised READ on a narrower sub-case); it is a
//       missing CONTROL-FLOW early-return the .cpp never implements, for ANY in-range match at ANY
//       slot. Consequence for this oracle: every case below that needs match_count>0 uses ONLY
//       out-of-range matches (dist outside [range_min, range_max] for every matching weapon), which is
//       also the only way step 8 (the wrap-delta/combine math) is ever reached in the real binary.
//
//   (3) NEW -- WRONG SIGN on the "dist < range_min" shortfall (0x00488417-0x00488425): the raw SUB at
//       0x00488422 computes `delta = dist - range_min` (NEGATIVE, since this sub-branch is only reached
//       when dist < range_min) -- but sim_unit_calc_range_approach_point.cpp's matching line computes
//       `delta = range_min - dist` (POSITIVE), the opposite sign. The sibling "dist > range_max"
//       sub-branch (0x0048840c-0x00488412, `delta = dist - range_max`) is verified CORRECT -- same
//       operand order as the .cpp's `delta = dist - range_max`, no divergence. Because abs(delta) is
//       what the JLE-keeps-else-updates comparison at 0x00488456 uses to pick a winning best_delta,
//       this sign flip is invisible to that comparison -- but it DOES flip the sign of `best_delta`
//       itself, which feeds `sign_best = sign_or_one(best_delta)` and therefore mirrors the FINAL
//       combine's direction on both axes whenever the winning update came from a "dist < range_min"
//       weapon. Consequence for this oracle: every out-of-range match below uses ONLY "dist >
//       range_max" (never "dist < range_min"), which is also the reason Case D below cannot construct
//       a legitimately-reachable NEGATIVE best_delta -- every delta the CONFIRMED-CORRECT branch can
//       produce is >= 0 (the sentinel is positive too), so "dx_mag = best_delta signed, not abs()'d" is
//       demonstrated with a positive best_delta only; see Case D's own comment.
//
// A FOURTH, separate finding (not scope-affecting for this file, since it is entirely mocked out via
// the target_class recorder and every sentinel below is chosen to sidestep it): sim_target_class's two
// arguments are read via MOVZX (0x004882c3 word[+0x8a], 0x004882cf word[+0x8c]) -- a ZERO-extend of the
// 16-bit field. Every OTHER target_class caller in this codebase (sim_unit_state_attack_unit.cpp,
// sim_unit_chase_check.cpp, sim_unit_state_move_walker.cpp, sim_unit_state_move_path.cpp, ...) goes
// through an explicit `static_cast<uint16_t>(u.target_ref)` / `static_cast<uint16_t>(u.target_index)`
// before widening, matching MOVZX. THIS function's .cpp does not -- it casts the int16_t fields
// directly to uint32_t/int32_t, which SIGN-EXTENDS for any value with the 0x8000 bit set. Untested here
// (both target_ref/target_index sentinels below stay under 0x8000, matching every OTHER case in this
// batch's own convention of using small distinct sentinels) -- flagged for the conductor instead.
//
#include "sim/sim_unit_calc_range_approach_point.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves call order (same idiom as sim_unit_fire_weapon_selftest.cpp's own
// g_trace/tr/trace_eq -- this file re-declares them locally since sim_test_support.h only shares
// ck/ck_eq/ck_eq_d, not the trace helpers) --------------------------------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
bool                      trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- recorders for the four outward calls, KNOB-style (record args, return a per-case-set value) --
// tile_dist_wrapped/wrap_delta_x/wrap_delta_y are ORIGINAL functions this batch never reimplements, so
// (like sim_unit_fire_weapon_selftest.cpp's dist_out_of_range/dir_from_to) they are knobs here, not
// formula mocks -- the expected combine values in each case below are hand-derived from whatever the
// case sets the knob to, not from any real toroidal-geometry computation.

struct TargetClassCall {
    uint32_t owner_and_kind_flag;
    int32_t  roster_slot;
};
std::vector<TargetClassCall> g_tc_calls;
int32_t                      g_tc_ret = 1;
int32_t                      rec_target_class(uint32_t owner_and_kind_flag, int32_t roster_slot) {
    tr("target_class");
    g_tc_calls.push_back({owner_and_kind_flag, roster_slot});
    return g_tc_ret;
}

struct DistCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DistCall> g_dist_calls;
int32_t               g_dist_ret = 0;
int32_t               rec_tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("tile_dist_wrapped");
    g_dist_calls.push_back({x1, y1, x2, y2});
    return g_dist_ret;
}

struct WrapDxCall {
    int32_t  pos_a;
    uint32_t unused_param;
    int32_t  pos_b;
};
std::vector<WrapDxCall> g_wdx_calls;
int32_t                 g_wdx_ret = 0;
int32_t                 rec_wrap_delta_x(int32_t pos_a, uint32_t unused_param, int32_t pos_b) {
    tr("wrap_delta_x");
    g_wdx_calls.push_back({pos_a, unused_param, pos_b});
    return g_wdx_ret;
}

struct WrapDyCall {
    int32_t x1, y1, x2, y2;
};
std::vector<WrapDyCall> g_wdy_calls;
int32_t                 g_wdy_ret = 0;
int32_t                 rec_wrap_delta_y(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("wrap_delta_y");
    g_wdy_calls.push_back({x1, y1, x2, y2});
    return g_wdy_ret;
}

const unit_calc_range_approach_point_calls &rec_calls() {
    static const unit_calc_range_approach_point_calls c = {
        &rec_target_class,
        &rec_tile_dist_wrapped,
        &rec_wrap_delta_x,
        &rec_wrap_delta_y,
    };
    return c;
}

// Observations reset per case; KNOBS (g_tc_ret/g_dist_ret/g_wdx_ret/g_wdy_ret) are deliberately NOT
// reset here -- every case sets them explicitly BEFORE calling run_case, matching
// sim_unit_fire_weapon_selftest.cpp's reset_f2_observations()'s own documented rule.
void reset_observations() {
    g_trace.clear();
    g_tc_calls.clear();
    g_dist_calls.clear();
    g_wdx_calls.clear();
    g_wdy_calls.clear();
}

int32_t run_case(sim_fixture &f, int32_t &tx, int32_t &ty) {
    reset_observations();
    sim_view v = f.view();
    return detail::unit_calc_range_approach_point(v, rec_calls(), &tx, &ty);
}

void run_cases() {
    sim_fixture fx; // reused across every case below (no fx.reset() between them) -- each case
                    // UNCONDITIONALLY zeroes all four weapon slots itself, matching
                    // sim_unit_fire_weapon_selftest.cpp's seed_fw_unit() convention, so no case can
                    // silently inherit an earlier case's weapon state.

    // ==== A. Steps 1-2 + 3-4 + 7: target_class's argument tuple, the unconditional DEFAULT WRITE
    // (clobbers the caller's ORIGINAL out-params even when nothing ever matches), and the match_count
    // ==0 return (0x004882be-0x004882db target_class; 0x004882ee-0x0048831a default write;
    // 0x00488469-0x0048846d the match_count==0 return) ==============================================
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {}; // every slot enabled_2==0 -> nothing can ever match
        u.x                = 17;
        u.y                = 201;
        u.target_ref       = 0x0125; // distinct sentinel, well under the int16_t sign bit (0x8000) --
        u.target_index     = 0x02ab; // see the file banner's signedness finding for why
        fx.view_cur_player = 0;
        g_tc_ret           = 1;
        int32_t tx = 250, ty = 9; // caller's ORIGINAL candidate, distinct from u.x/u.y -- a
                                  // clobbered-vs-preserved mixup is directly observable
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 0u, "A (0x0048846d JZ): match_count==0 -> returns 0");
        ck_eq((uint32_t)g_tc_calls.size(), 1u, "A (0x004882d6 CALL): target_class called exactly once");
        if (!g_tc_calls.empty()) {
            ck_eq(g_tc_calls[0].owner_and_kind_flag, 0x0125u,
                  "A (0x004882cf MOVZX EAX, word[cur_unit+0x8c]): target_class's 1st arg = "
                  "cur_unit->target_ref");
            ck_eq((uint32_t)g_tc_calls[0].roster_slot, 0x02abu,
                  "A (0x004882c3 MOVZX EDX, word[cur_unit+0x8a]): target_class's 2nd arg = "
                  "cur_unit->target_index");
        }
        ck_eq((uint32_t)g_dist_calls.size(), 1u,
              "A (0x00488336): tile_dist_wrapped still called exactly once even though match_count==0 "
              "leaves `dist` itself unused afterward -- step 5 is an unconditional prefix");
        ck_eq((uint32_t)g_wdx_calls.size(), 0u,
              "A (0x0048846d JZ to 0x0048860e): wrap_delta_x never called -- match_count==0 skips all "
              "of step 8");
        ck_eq((uint32_t)g_wdy_calls.size(), 0u, "A: wrap_delta_y never called either, same reason");
        ck(trace_eq({"target_class", "tile_dist_wrapped"}),
           "A: call order target_class -> tile_dist_wrapped and NOTHING further -- steps 1-2 and 5 run "
           "unconditionally, step 8's wrap_delta_x/y never fire when match_count==0");
        ck_eq((uint32_t)tx, 17u,
              "A (0x00488303 default write): *io_target_x = cur_unit->x (17), NOT the caller's "
              "original (250)");
        ck_eq((uint32_t)ty, 201u,
              "A (0x0048831a default write): *io_target_y = cur_unit->y (201), NOT the caller's "
              "original (9)");
    }

    // ==== B. Step 5: dist = tile_dist_wrapped(unit's OWN tile, the CALLER'S ORIGINAL target position),
    // saved at 0x004882de-0x004882eb BEFORE the default write clobbers *io_target_x/*io_target_y
    // (0x0048832a-0x00488336) -- pinned via the exact recorder-call argument tuple, since match_count
    // ==0 makes `dist`'s numeric value otherwise unobservable ========================================
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {};
        u.x                = 63;
        u.y                = 44;
        u.target_ref       = 1;
        u.target_index     = 1; // irrelevant here -- target_class is a knob, not asserted in this case
        fx.view_cur_player = 0;
        int32_t tx = 199, ty = 8; // caller's ORIGINAL target -- distinct from u.x/u.y and from each
                                  // other, so each of the four call-argument slots is unambiguous
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 0u, "B: match_count==0 (sanity floor for this case)");
        ck_eq((uint32_t)g_dist_calls.size(), 1u, "B (0x00488336): tile_dist_wrapped called exactly once");
        if (!g_dist_calls.empty()) {
            const auto &d = g_dist_calls[0];
            ck(d.x1 == 63 && d.y1 == 44,
               "B (0x00488333 EAX=x1 / 0x00488330 EDX=y1): tile_dist_wrapped's x1/y1 = cur_unit's OWN "
               "tile (63,44), not the target");
            ck(d.x2 == 199 && d.y2 == 8,
               "B (0x0048832d EBX=x2 / 0x0048832a ECX=y2): tile_dist_wrapped's x2/y2 = the CALLER'S "
               "ORIGINAL *io_target_x/*io_target_y (199,8) as saved BEFORE the default write clobbers "
               "them at 0x004882ee -- not the just-written default (63,44) and not some post-default "
               "value");
        }
    }

    // ==== C. Step 6 -- the 4-slot scan: a disabled slot and a wrong-target-class slot are both
    // skipped, and ALL FOUR SLOTS ARE ALWAYS VISITED (no first-match-wins early-out, unlike
    // sim_unit_in_weapon_range.cpp) -- 0x0048833e-0x00488469. PLUS Step 8's dist!=0 combine
    // (0x00488473-0x0048860c) and the wrap_delta_x/y call arguments (0x00488473-0x00488498). Both
    // matching weapons are deliberately "dist > range_max" (the CONFIRMED-CORRECT shortfall
    // sub-branch, per the file banner's finding (3) -- "dist < range_min" is excluded from this
    // oracle) ==========================================================================================
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {};
        u.x                = 50;
        u.y                = 30;
        u.target_ref       = 1;
        u.target_index     = 1;
        fx.view_cur_player = 3;
        g_tc_ret           = 1; // GROUND wanted (target_class_result==1 -> WEAPON_TARGET_GROUND)

        // slot 0: disabled -- must be skipped regardless of weapon_id/target (0x00488368 JZ). Given a
        // weapon_id that WOULD match if the enabled_2 gate were ever bypassed, so this is a real proof
        // of the gate, not an accident of an unmatching weapon_id.
        u.weapons[0].enabled_2   = 0;
        u.weapons[0].weapon_id   = 9;
        fx.cfg_weapons[9].target = WEAPON_TARGET_GROUND;

        // slot 1: enabled, but its weapon's target mask is AIR-only while GROUND is wanted -> skipped
        // (0x0048838e TEST bit 0x1 / 0x00488395 JZ).
        u.weapons[1].enabled_2   = 1;
        u.weapons[1].weapon_id   = 4;
        fx.cfg_weapons[4].target = WEAPON_TARGET_AIR;

        // slot 2: enabled, GROUND-matching, OUT OF RANGE HIGH: dist=100 > range_max=60 -> delta=40.
        u.weapons[2].enabled_2         = 1;
        u.weapons[2].weapon_id         = 5;
        fx.cfg_weapons[5].target       = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[5].range_min[3] = 10;
        fx.cfg_weapons[5].range_max[3] = 60;

        // slot 3: enabled, GROUND-matching, ALSO out of range high but with a SMALLER shortfall:
        // dist=100 > range_max=90 -> delta=10. If the scan stopped after the first match (slot 2),
        // best_delta would stay 40 and the combine below would differ -- only because slot 3 genuinely
        // runs does best_delta become 10, so the final combine values are a falsifiable proof that all
        // four slots are visited, not just the first match.
        u.weapons[3].enabled_2         = 1;
        u.weapons[3].weapon_id         = 6;
        fx.cfg_weapons[6].target       = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[6].range_min[3] = 10;
        fx.cfg_weapons[6].range_max[3] = 90;

        g_dist_ret = 100;
        g_wdx_ret  = 37;  // delta_x (positive)
        g_wdy_ret  = -15; // delta_y (negative)

        int32_t tx = 90, ty = 40; // caller's original target
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 2u,
              "C (0x004883ac incremented twice): match_count==2 -- both slot 2 and slot 3 matched, "
              "proving slot 3 was reached (falsifies a first-match-wins early-out)");
        ck_eq((uint32_t)g_wdx_calls.size(), 1u, "C (0x0048847f): wrap_delta_x called exactly once");
        ck_eq((uint32_t)g_wdy_calls.size(), 1u, "C (0x00488493): wrap_delta_y called exactly once");
        if (!g_wdx_calls.empty())
            ck(g_wdx_calls[0].pos_a == 50 && g_wdx_calls[0].unused_param == 30 &&
                   g_wdx_calls[0].pos_b == 90,
               "C (0x0048847c EAX=pos_a=u.x / 0x00488479 EDX=unused=u.y / 0x00488476 EBX=pos_b="
               "saved_target_x): wrap_delta_x(50, 30, 90)");
        if (!g_wdy_calls.empty())
            ck(g_wdy_calls[0].x1 == 50 && g_wdy_calls[0].y1 == 30 && g_wdy_calls[0].x2 == 90 &&
                   g_wdy_calls[0].y2 == 40,
               "C (0x00488490 EAX=x1=u.x / 0x0048848d EDX=y1=u.y / 0x0048848a EBX=x2=saved_target_x / "
               "0x00488487 ECX=y2=saved_target_y): wrap_delta_y(50, 30, 90, 40)");
        ck(trace_eq({"target_class", "tile_dist_wrapped", "wrap_delta_x", "wrap_delta_y"}),
           "C: call order target_class -> tile_dist_wrapped -> wrap_delta_x -> wrap_delta_y, matching "
           "steps 1-2, 5, and 8's sequence (the 4-slot scan in between makes no outward calls)");

        // best_delta: sentinel(640) -> 40 (slot 2, |40|<640) -> 10 (slot 3, |10|<40). abs_best=10.
        // delta_x=37 (sign_x=+1), delta_y=-15 (sign_y=-1). sign_best=sign_or_one(10)=+1.
        // dist=100 != 0: dx_mag = |10|*|37|/100 = 370/100 = 3 (TRUNCATING). dy_mag = 10-3 = 7 (the
        // REMAINDER -- re-derived from the raw SUB at 0x004885c7, not an independent calc against
        // delta_y=-15).
        ck_eq((uint32_t)tx, 53u,
              "C (0x004885dc-0x004885f3, dx_mag=3 from the TRUNCATING 10*37/100): *io_target_x = "
              "(u.x=50 + dx_mag(3)*sign_x(+1)*sign_best(+1)) & width_mask(0xff) = 53");
        ck_eq((uint32_t)ty, 23u,
              "C (0x004885f5-0x0048860c, dy_mag=7=abs_best(10)-dx_mag(3)): *io_target_y = (u.y=30 + "
              "dy_mag(7)*sign_y(-1)*sign_best(+1)) & height_mask(0x3f) = 23");
        // Re-derived from the observed outputs (dx_mag/dy_mag aren't directly exposed): dx_mag =
        // (tx-u.x)/(sign_x*sign_best) = (53-50)/(+1*+1) = 3; dy_mag = (ty-u.y)/(sign_y*sign_best) =
        // (23-30)/(-1*+1) = 7. Their sum is exactly abs(best_delta), confirming the REMAINDER
        // relationship (0x004885c7 SUB, not a second IMUL/IDIV against delta_y).
        const int32_t derived_dx_mag = (tx - (int32_t)u.x) / (1 * 1);  // sign_x=+1, sign_best=+1
        const int32_t derived_dy_mag = (ty - (int32_t)u.y) / (-1 * 1); // sign_y=-1, sign_best=+1
        ck_eq((uint32_t)(derived_dx_mag + derived_dy_mag), 10u,
              "C: dx_mag(3) + dy_mag(7) == abs(best_delta)(10) -- the partition-of-magnitude "
              "relationship holds");
    }

    // ==== D. Step 8's dist==0 branch (0x004885cf-0x004885d5): dx_mag = best_delta DIRECTLY (SIGNED,
    // no abs()), dy_mag = 0 unconditionally. Uses a single OUT-OF-RANGE-HIGH match with a synthetic
    // negative range (range_max=-5) so dist==0 still satisfies "dist > range_max" -- the
    // CONFIRMED-CORRECT shortfall sub-branch (finding (3)) -- while landing on dist==0. See the file
    // banner: this oracle cannot construct a NEGATIVE best_delta at all, because the only sub-branch
    // that can legitimately produce one ("dist < range_min") has a confirmed sign-flip divergence, so
    // "signed, not abs()'d" is demonstrated here with a POSITIVE best_delta only (where the two would
    // coincide numerically) -- see the report for why the fully-general case can't be built ============
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {};
        u.x                = 8;
        u.y                = 20;
        u.target_ref       = 1;
        u.target_index     = 1;
        fx.view_cur_player = 2;
        g_tc_ret           = 1;

        u.weapons[0].enabled_2         = 1;
        u.weapons[0].weapon_id         = 1;
        fx.cfg_weapons[1].target       = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[1].range_min[2] = -10;
        fx.cfg_weapons[1].range_max[2] = -5; // dist(0) > range_max(-5) -> delta = 0-(-5) = 5

        g_dist_ret = 0;  // dist == 0 -- selects the dist==0 combine branch
        g_wdx_ret  = -3; // delta_x -- only its SIGN matters in this branch
        g_wdy_ret  = 9;  // delta_y -- irrelevant: dy_mag is forced to 0 regardless of delta_y

        int32_t tx = 0, ty = 0;
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 1u, "D: match_count==1");
        // best_delta = 5 (positive, via the confirmed-correct dist>range_max branch). sign_best=+1.
        // sign_x = sign_or_one(-3) = -1. dist==0: dx_mag = best_delta = 5, dy_mag = 0.
        ck_eq((uint32_t)tx, 3u,
              "D (0x004885cf dx_mag=best_delta=5, un-abs'd): *io_target_x = (u.x=8 + "
              "dx_mag(5)*sign_x(-1)*sign_best(+1)) & width_mask(0xff) = 3");
        ck_eq((uint32_t)ty, 20u,
              "D (0x004885d5 dy_mag=0 unconditionally when dist==0): *io_target_y = (u.y=20 + 0) & "
              "height_mask(0x3f) = 20, unchanged from u.y");
    }

    // ==== E. Step 8 wrap + sign edge cases: the final combine's AND-mask on a NEGATIVE two's-complement
    // sum (0x004885e8-0x0048860c, both axes), delta_y==0 maps to sign +1 not 0
    // (0x004884b8-style "0 maps to +1"), and TRUNCATING division can push dx_mag PAST abs(best_delta)
    // so dy_mag goes negative -- still the exact remainder (dx_mag+dy_mag==abs(best_delta)), just
    // partitioned unevenly. Also exercises the AIR side of the target-class-mask ternary
    // (target_class_result != 1) ======================================================================
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {};
        u.x                = 5;
        u.y                = 50;
        u.target_ref       = 1;
        u.target_index     = 1;
        fx.view_cur_player = 1;
        g_tc_ret           = 2; // AIR wanted (target_class_result != 1 -> WEAPON_TARGET_AIR)

        u.weapons[0].enabled_2         = 1;
        u.weapons[0].weapon_id         = 2;
        fx.cfg_weapons[2].target       = WEAPON_TARGET_AIR;
        fx.cfg_weapons[2].range_min[1] = -50;
        fx.cfg_weapons[2].range_max[1] = 0; // dist(10) > range_max(0) -> delta = 10-0 = 10

        g_dist_ret = 10;   // dist != 0 -- the truncating-division branch
        g_wdx_ret  = -100; // delta_x: NEGATIVE and |delta_x|(100) > dist(10)
        g_wdy_ret  = 0;    // delta_y == 0 -- must map to sign +1, NOT 0

        int32_t tx = 0, ty = 0;
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 1u, "E: match_count==1 (via the AIR side of the target-class ternary, "
                                 "0x004883a0 TEST bit 0x2)");
        // abs_best=10, abs_dx=100, dist=10 -> dx_mag = 10*100/10 = 100 (exact here, still the
        // TRUNCATING formula). dy_mag = abs_best(10) - dx_mag(100) = -90: dx_mag alone already exceeds
        // abs_best, so the "remainder" goes negative -- dx_mag+dy_mag == 100+(-90) == 10 ==
        // abs(best_delta) STILL holds, confirming the partition relationship is exact even here.
        // sign_x = sign_or_one(-100) = -1. sign_y = sign_or_one(0) = +1 ("0 maps to +1", not 0).
        // sign_best = sign_or_one(10) = +1.
        ck_eq((uint32_t)tx, 161u,
              "E (0x004885e6 ADD / 0x004885f1 AND): pre-mask sum = u.x(5) + dx_mag(100)*sign_x(-1)*"
              "sign_best(+1) = -95; -95 & width_mask(0xff) = 161 -- x86 AND on the two's-complement bit "
              "pattern, not a modulo-with-sign-correction");
        ck_eq((uint32_t)ty, 24u,
              "E (delta_y==0 -> sign_y=sign_or_one(0)=+1, then 0x00488601 AND): pre-mask sum = u.y(50) "
              "+ dy_mag(-90)*sign_y(+1)*sign_best(+1) = -40; -40 & height_mask(0x3f) = 24");
    }

    // ==== F. REGRESSION for the EARLY-RETURN fix (2026-08-22, applied after this file's own findings
    // (2) above): a single matching weapon whose dist lands INSIDE [range_min, range_max] must return
    // match_count IMMEDIATELY (0x004883f9-0x00488402 -> epilogue at 0x00488614), leaving *io_target_x/
    // *io_target_y at the step-4 DEFAULT (the unit's own tile) and calling NEITHER wrap_delta_x NOR
    // wrap_delta_y ==================================================================================
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {};
        u.x                = 12;
        u.y                = 34;
        u.target_ref       = 1;
        u.target_index     = 1;
        fx.view_cur_player = 4;
        g_tc_ret           = 1; // GROUND

        u.weapons[0].enabled_2         = 1;
        u.weapons[0].weapon_id         = 3;
        fx.cfg_weapons[3].target       = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[3].range_min[4] = 10;
        fx.cfg_weapons[3].range_max[4] = 20;

        g_dist_ret = 15; // squarely inside [10,20] -- the early-return path

        int32_t tx = 999, ty = 888; // caller's original -- must NOT survive into the output either;
                                    // the DEFAULT write (u.x/u.y) already clobbered them before the scan
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 1u, "F (0x004883f9-0x00488402): match_count==1, staged as the return "
                                 "value at the moment of the early return");
        ck_eq((uint32_t)tx, 12u, "F (0x00488614 epilogue, no combine): *io_target_x stays the step-4 "
                                 "default (u.x=12), untouched by any wrap combine");
        ck_eq((uint32_t)ty, 34u, "F: *io_target_y stays the step-4 default (u.y=34)");
        ck_eq((uint32_t)g_wdx_calls.size(), 0u,
              "F: wrap_delta_x never called -- the early return skips step 8 entirely");
        ck_eq((uint32_t)g_wdy_calls.size(), 0u, "F: wrap_delta_y never called either, same reason");
        ck(trace_eq({"target_class", "tile_dist_wrapped"}),
           "F: call order stops at tile_dist_wrapped -- no wrap_delta_x/y, matching the early return");
    }

    // ==== G. REGRESSION for the "dist < range_min" sign fix (0x00488417-0x00488425): a single
    // matching weapon that is TOO CLOSE (dist < range_min) must produce delta = dist-range_min
    // (NEGATIVE), not range_min-dist (POSITIVE) -- the sign flip that was fixed. best_delta therefore
    // ends up NEGATIVE, which flips sign_best and mirrors the final combine's direction on both axes.
    // u.x=100, u.y=100, one GROUND-matching weapon at range_min=20/range_max=30, dist=5 (< range_min,
    // and dist<=range_max so this is NOT the "too far" branch) -> delta = dist-range_min = 5-20 = -15.
    // best_delta = -15 (only candidate, sentinel is larger). sign_best = sign_or_one(-15) = -1.
    //
    // CONDUCTOR NOTE (2026-08-22, drain session): this case was originally attempted with a hand-arithmetic
    // slip -- 5-20 was computed as -25 instead of -15 -- which is exactly why the pulled attempt's
    // "expected" tx/ty (60, 51) did not match the built binary's actual (76, 45). Confirmed by
    // instrumenting the production function with a temporary debug print of every intermediate
    // (dist/best_delta/delta_x/delta_y/sign_x/sign_y/sign_best/dx_mag/dy_mag), rebuilding, and reading
    // the real trace: best_delta=-15, dx_mag=15*8/5=24, dy_mag=15-24=-9 -- all consistent with THIS
    // function's own confirmed-correct formulas, not a further bug. The debug print was removed before
    // this commit; the corrected numbers below are re-derived by hand from delta=5-20=-15 and verified
    // against the actual instrumented run, not merely asserted.
    {
        unit &u = fx.u(0, 0);
        for (auto &ws : u.weapons) ws = {}; // this session's session-report-flagged gap: MUST reset
                                            // every slot, or a prior case's weapon survives into G.
        u.x                = 100;
        u.y                = 100;
        u.target_ref       = 1;
        u.target_index     = 1;
        fx.view_cur_player = 5;
        g_tc_ret           = 1; // GROUND wanted

        u.weapons[0].enabled_2         = 1;
        u.weapons[0].weapon_id         = 7;
        fx.cfg_weapons[7].target       = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[7].range_min[5] = 20;
        fx.cfg_weapons[7].range_max[5] = 30;

        g_dist_ret = 5; // < range_min(20), <= range_max(30) -- the "too close" sub-branch
        g_wdx_ret  = 8; // delta_x (positive)
        g_wdy_ret  = 0; // delta_y == 0 -- maps to sign +1, not 0

        int32_t tx = 0, ty = 0;
        int32_t ret = run_case(fx, tx, ty);

        ck_eq((uint32_t)ret, 1u, "G: match_count==1");
        // best_delta=-15 (the ONLY candidate, delta=dist-range_min=5-20=-15). sign_x=sign_or_one(8)=+1,
        // sign_y=sign_or_one(0)=+1, sign_best=sign_or_one(-15)=-1. dist=5 != 0: abs_best=15, abs_dx=8 ->
        // dx_mag = 15*8/5 = 24 (TRUNCATING, exact here). dy_mag = abs_best(15) - dx_mag(24) = -9
        // (remainder, can go negative -- same partition rule as Case E).
        ck_eq((uint32_t)tx, 76u,
              "G (0x004885dc ADD / 0x004885f1 AND): pre-mask sum = u.x(100) + dx_mag(24)*sign_x(+1)*"
              "sign_best(-1) = 76; 76 & width_mask(0xff) = 76 -- the NEGATIVE best_delta (from the "
              "fixed sign) flips the x direction relative to Case C's positive-best_delta case");
        ck_eq((uint32_t)ty, 45u,
              "G (0x004885f5 ADD / 0x00488601 AND): pre-mask sum = u.y(100) + dy_mag(-9)*sign_y(+1)*"
              "sign_best(-1) = 109; 109 & height_mask(0x3f) = 45");
    }
}

} // namespace

// ---- the one entry point (required exact signature) --------------------------------------------
void run_unit_calc_range_approach_point_tests() {
    printf("-- llm_strat_unit_calc_range_approach_point --\n");
    run_cases();
}

} // namespace mh::sim::test
