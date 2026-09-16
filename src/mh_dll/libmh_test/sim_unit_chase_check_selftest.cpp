//
// sim_unit_chase_check_selftest.cpp -- `simtest` cases for llm_strat_unit_chase_check @0x004866fb
// (sim/sim_unit_chase_check.h/.cpp), SIM1-G5.
//
// OFFLINE ORACLE, not a rig-verified promotion: chase_check's own return value IS shadow-comparable,
// but its sibling call into llm_strat_unit_fire_at_target -> llm_strat_unit_fire_weapon reaches an
// 11-region UNBOUNDED write closure (fx-anim spawn, projectile pool, kill-credit's camera-pan/
// player-profile/buildings writes, RNG state, sound state) that shadow_region_closure.py flags as
// un-armable. So this file is the promotion evidence instead -- every branch pinned against the RAW
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_chase_check_004866fb.asm), re-derived address-by-address
// against the .cpp under test (not trusted from the header banner's prose alone): stage 1's
// get_coords-refresh gate (0x00486713-0x0048675d), stage 2's target_class/in-range test incl. the
// truncating-toward-zero /32 tile conversion (0x0048675d-0x004867ce), stage 3's mutually-exclusive
// building/unit alive-fires-immediately branches incl. the ORDERED NaN-is-alive compare
// (0x004867d6-0x0048681f), stage 4's fire+return-1 (0x0048686a-0x0048686f), stage 5's "target
// expired" release+clear+state-order arm (0x00486871-0x004868e0), and the lone return-0 "still
// tracking, out of range" arm which uses a DIFFERENT callee (unit_set_state, not
// unit_set_state_order) than stage 5 (0x004868e9-0x00486906).
//
// One recorder log_t + one recording_calls() table, same shape as sim_unit_target_tracking_selftest.
// cpp's own (get_coords/target_class/in_range/release/fire), extended with the two state-transition
// callees (unit_set_state_order, unit_set_state) this function alone needs.
//
#include "sim/sim_unit_chase_check.h"

#include <limits>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int      get_coords_calls = 0;
    uint16_t last_gc_player   = 0;
    int32_t  last_gc_index    = 0;
    int32_t  gc_out_x = 0, gc_out_y = 0; // KNOB: what the mock writes through get_coords' out-pointers

    int      target_class_calls  = 0;
    uint32_t last_tc_ref         = 0;
    int32_t  last_tc_index       = 0;
    int32_t  target_class_return = 0; // KNOB

    int      in_range_calls   = 0;
    int32_t  last_ir_player   = 0;
    int32_t  last_ir_unit_idx = 0;
    int32_t  last_ir_tile_x   = 0;
    int32_t  last_ir_tile_y   = 0;
    int32_t  last_ir_class    = 0;
    uint32_t in_range_return  = 0; // KNOB

    int fire_calls = 0;

    int      release_calls         = 0;
    uint32_t last_release_player   = 0;
    int32_t  last_release_unit_idx = 0;
    uint32_t last_release_mode     = 0;

    int      set_state_order_calls = 0;
    uint16_t last_sso_state        = 0;
    uint16_t last_sso_order        = 0;

    int      set_state_calls = 0;
    uint16_t last_ss_state   = 0;
};
log_t g_log;

void rec_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) {
    ++g_log.get_coords_calls;
    g_log.last_gc_player = player;
    g_log.last_gc_index  = unit_index;
    *out_x               = g_log.gc_out_x;
    *out_y               = g_log.gc_out_y;
}
void rec_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    ++g_log.release_calls;
    g_log.last_release_player   = player_idx;
    g_log.last_release_unit_idx = unit_idx;
    g_log.last_release_mode     = mode;
}
int32_t rec_target_class(uint32_t owner_and_kind_flag, int32_t roster_slot) {
    ++g_log.target_class_calls;
    g_log.last_tc_ref   = owner_and_kind_flag;
    g_log.last_tc_index = roster_slot;
    return g_log.target_class_return;
}
uint32_t rec_in_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y, int32_t target_class) {
    ++g_log.in_range_calls;
    g_log.last_ir_player   = player;
    g_log.last_ir_unit_idx = unit_idx;
    g_log.last_ir_tile_x   = tile_x;
    g_log.last_ir_tile_y   = tile_y;
    g_log.last_ir_class    = target_class;
    return g_log.in_range_return;
}
void rec_fire_at_target() { ++g_log.fire_calls; }
void rec_set_state_order(uint16_t state, uint16_t order) {
    ++g_log.set_state_order_calls;
    g_log.last_sso_state = state;
    g_log.last_sso_order = order;
}
void rec_set_state(uint16_t new_state) {
    ++g_log.set_state_calls;
    g_log.last_ss_state = new_state;
}

const unit_chase_check_calls &recording_calls() {
    static const unit_chase_check_calls c = {
        &rec_get_coords,
        &rec_release_ref,
        &rec_target_class,
        &rec_in_range,
        &rec_fire_at_target,
        &rec_set_state_order,
        &rec_set_state,
    };
    return c;
}

// CUR_PLAYER/CUR_INDEX name the roster slot that _G_LLM_STRAT_CUR_UNIT/_CUR_PLAYER/_CUR_INDEX must
// agree on for the test to be sound: chase_check both READS *v.cur_unit and WRITES through
// own.unit_at(*v.cur_player, *v.cur_index), and those must alias the SAME record (the real-game
// invariant) or a stage-1 refresh/stage-5 clear would silently write somewhere the later reads never
// see. Distinct, non-zero values (not 0/0) so a swapped player<->index argument disagrees here.
constexpr int32_t CUR_PLAYER = 3;
constexpr int32_t CUR_INDEX  = 6;

// Seeds cur_unit == units[CUR_PLAYER][CUR_INDEX] (repointing cur_unit_ptr per sim_test_support.h's
// documented convention), a neutral state/target_ref/target_index/target_fine_x/y, and the log's
// default knobs (out-of-range, so a case must opt IN to reaching stage 3/4/5).
unit &seed(sim_fixture &f) {
    unit &u                   = f.u(CUR_PLAYER, CUR_INDEX);
    f.cur_unit_ptr            = &u;
    f.view_cur_player         = (uint16_t)CUR_PLAYER;
    f.view_cur_index          = (uint16_t)CUR_INDEX;
    u.state                   = 0;
    u.target_ref              = 0;
    u.target_index            = 0;
    u.target_fine_x           = -1000; // non-multiple of 32, negative -- pins truncating-toward-zero
    u.target_fine_y           = 1000;  // non-multiple of 32, positive
    g_log                     = log_t{};
    g_log.target_class_return = 77;
    g_log.in_range_return     = 0; // default posture: out of range, until a case opts in
    return u;
}

int32_t run(sim_fixture &f) {
    sim_view  v   = f.view();
    sim_store own = f.store();
    return detail::unit_chase_check(v, own, recording_calls());
}

// ==== stage 1 (0x00486713-0x0048675d): the target_fine_x/y refresh gate =============================

void test_stage1_no_refresh_when_target_ref_lacks_0xa0() {
    sim_fixture f;
    unit       &u  = seed(f);
    u.target_ref   = 0x40; // building bit only -- neither 0x80 nor 0x20 of the 0xa0 mask
    u.target_index = 4;

    int32_t ret = run(f);

    ck(g_log.get_coords_calls == 0,
       "stage1 (0x00486718 TEST byte[+0x8c],0xa0 / 0x0048671f JZ 0x0048675d): target_ref=0x40 has "
       "neither 0x80 nor 0x20 set -> get_coords NOT called");
    ck(u.target_fine_x == -1000 && u.target_fine_y == 1000,
       "stage1: target_fine_x/y are UNCHANGED when the refresh doesn't run");
    ck(g_log.target_class_calls == 1 && g_log.last_tc_ref == 0x40 && g_log.last_tc_index == 4,
       "stage2 (0x0048676e/0x00486775): target_class(target_ref=0x40 FULL UNMASKED, target_index=4)");
    ck(g_log.in_range_calls == 1 && g_log.last_ir_player == CUR_PLAYER &&
           g_log.last_ir_unit_idx == CUR_INDEX && g_log.last_ir_tile_x == (-1000 / 32) &&
           g_log.last_ir_tile_y == (1000 / 32) && g_log.last_ir_class == 77,
       "stage2 (0x004867c9): unit_in_weapon_range(cur_player=3, cur_index=6, "
       "tile_x=fine_to_tile(-1000)=-31, tile_y=fine_to_tile(1000)=31, target_class=77) -- pins the "
       "SAR/SHL/SBB/SAR truncating-toward-zero /32 idiom on a negative AND a positive non-multiple");
    ck(ret == 0, "stage2 (0x004867d0 JZ 0x004868e9): in_range==0 -> return 0");
    ck(g_log.fire_calls == 0 && g_log.release_calls == 0 && g_log.set_state_order_calls == 0,
       "stage2's out-of-range jump skips stages 3/4/5 entirely");
}

void test_stage1_refresh_when_target_ref_has_0xa0() {
    sim_fixture f;
    unit       &u             = seed(f);
    u.target_ref              = 0xa1; // 0xa0 (unit-kind) | owner nibble 1
    u.target_index            = 4;    // the TARGET's own slot index, distinct from CUR_INDEX
    g_log.gc_out_x            = 555;  // sentinels distinct from the pre-existing -1000/1000
    g_log.gc_out_y            = 666;
    g_log.target_class_return = 88;

    int32_t ret = run(f);

    ck(g_log.get_coords_calls == 1 && g_log.last_gc_player == (uint16_t)(0xa1 & 0xf) &&
           g_log.last_gc_index == 4,
       "stage1 (0x00486718 TEST / 0x00486758 CALL): target_ref=0xa1 has 0xa0 bit set -> "
       "get_coords(target_ref&0xf=1, target_index=4) called");
    ck(u.target_fine_x == 555 && u.target_fine_y == 666,
       "stage1 (0x00486721-0x00486738 out-pointer setup): self.target_fine_x/y REFRESHED to "
       "get_coords' own out-written values (555, 666) -- a write into the CURRENT unit performed "
       "through the callee's out-pointers, not this function's own body");
    ck(g_log.target_class_calls == 1 && g_log.last_tc_ref == 0xa1 && g_log.last_tc_index == 4,
       "stage2 (0x0048676e/0x00486775): target_class receives the FULL unmasked target_ref=0xa1=161, "
       "unlike stage1's masked player arg=1");
    ck(g_log.in_range_calls == 1 && g_log.last_ir_tile_x == (555 / 32) &&
           g_log.last_ir_tile_y == (666 / 32),
       "stage2 (0x004867a0-0x004867c9): tile_x/y computed from the REFRESHED target_fine_x/y "
       "(555->17, 666->20), proving stage1's write is visible to stage2's own read of the same field");
    ck(ret == 0, "stage2: in_range==0 (forced) -> return 0, same short-circuit as the previous case");
}

// ==== stage 3 (0x004867d6-0x0048681f): building/unit immediate-fire, incl. NaN-is-alive ============

void test_stage3_building_alive_fires() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0x42; // building bit + owner 2
    u.target_index        = 5;
    f.b(2, 5).energy      = 10.0; // > 0.0 -- alive
    g_log.in_range_return = 1;

    int32_t ret = run(f);

    ck(g_log.fire_calls == 1,
       "stage3 (0x004867db TEST byte[+0x8c],0x40 / 0x00486814 FCOMP+0x0048681d JC): building "
       "target_ref&0x40 set, energy=10.0>0.0 -> alive -> stage4 (0x0048686a) fires immediately");
    ck(ret == 1, "stage4 (0x0048686f JMP 0x004868e0): return=1 on the fire path");
    ck(g_log.release_calls == 0 && g_log.set_state_order_calls == 0,
       "stage3/4's immediate fire skips stage5's release/state-order logic entirely");
}

void test_stage3_building_dead_falls_to_stage5() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0x42;
    u.target_index        = 5;
    f.b(2, 5).energy      = -5.0; // <= 0.0 -- dead
    g_log.in_range_return = 1;
    u.state               = 0; // != 0x2e

    int32_t ret = run(f);

    ck(g_log.fire_calls == 0,
       "stage3 (0x0048681d JC not taken): building energy=-5.0<=0.0 -> NOT alive -> falls through "
       "(the unit-branch test at 0x0048681f also fails, since 0x42's 0xa0 bits are clear) to stage5");
    ck(g_log.release_calls == 1 && g_log.last_release_player == CUR_PLAYER &&
           g_log.last_release_unit_idx == CUR_INDEX && g_log.last_release_mode == 1,
       "stage5 (0x00486871 CMP / 0x00486893 CALL): target_ref=0x42!=0 -> "
       "target_release_ref(cur_player=3, cur_index=6, mode=1)");
    ck(u.target_ref == 0 && u.target_index == 0,
       "stage5 (0x0048689d / 0x004868ab): target_ref/target_index cleared to 0 in the CURRENT unit");
    ck(g_log.set_state_order_calls == 1 && g_log.last_sso_state == 0x13 && g_log.last_sso_order == 0x13,
       "stage5 (0x004868d1/0x004868db): state(0)!=0x2e -> unit_set_state_order(0x13, 0x13)");
    ck(ret == 1, "stage5 (0x004868e0): return=1 on the target-expired path");
}

void test_stage3_unit_alive_fires() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0xa1; // unit-kind bit + owner 1
    u.target_index        = 4;
    f.u(1, 4).energy      = 20.0; // > 0.0 -- alive
    g_log.in_range_return = 1;

    int32_t ret = run(f);

    ck(g_log.fire_calls == 1,
       "stage3 (0x0048681f TEST byte[+0x8c],0xa0 / 0x0048685d FCOMP+0x00486866 JC): unit target_ref&"
       "0xa0 set, energy=20.0>0.0 -> alive -> stage4 fires immediately");
    ck(ret == 1, "stage4: return=1 on the fire path");
    ck(g_log.release_calls == 0 && g_log.set_state_order_calls == 0,
       "stage3/4's immediate fire skips stage5 entirely, same as the building case");
}

void test_stage3_unit_dead_falls_to_stage5() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0xa1;
    u.target_index        = 4;
    f.u(1, 4).energy      = -20.0; // <= 0.0 -- dead
    g_log.in_range_return = 1;
    u.state               = 0;

    int32_t ret = run(f);

    ck(g_log.fire_calls == 0,
       "stage3 (0x00486866 JC not taken): unit energy=-20.0<=0.0 -> NOT alive -> falls through to stage5");
    ck(g_log.release_calls == 1 && g_log.last_release_mode == 1,
       "stage5 (0x00486893): target_ref=0xa1!=0 -> target_release_ref(..., mode=1)");
    ck(u.target_ref == 0 && u.target_index == 0, "stage5: target_ref/target_index cleared to 0");
    ck(g_log.set_state_order_calls == 1 && g_log.last_sso_state == 0x13 && g_log.last_sso_order == 0x13,
       "stage5: state(0)!=0x2e -> unit_set_state_order(0x13, 0x13)");
    ck(ret == 1, "stage5: return=1");
}

void test_stage3_building_nan_energy_is_alive() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0x42;
    u.target_index        = 5;
    f.b(2, 5).energy      = std::numeric_limits<double>::quiet_NaN();
    g_log.in_range_return = 1;

    int32_t ret = run(f);

    ck(g_log.fire_calls == 1,
       "stage3 (0x00486814 FCOMP/0x0048681a FNSTSW/0x0048681c SAHF/0x0048681d JC): the ORDERED "
       "`0.0 < energy` compare treats a NaN energy as ALIVE (unordered sets CF=1 the same as the "
       "true case) -> fires, same NaN-is-alive idiom as sim_unit_target_tracking.cpp documents");
    ck(ret == 1, "stage4: return=1 on the NaN-is-alive fire path");
}

// ==== stage 5 (0x00486871-0x004868e0): "target expired" -- release/clear x state==0x2e matrix ======

void test_stage5_zero_target_ref_state_2e() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0; // already 0 -- release must be SKIPPED
    u.target_index        = 0;
    g_log.in_range_return = 1;
    u.state               = 0x2e;

    int32_t ret = run(f);

    ck(g_log.fire_calls == 0,
       "stage3: target_ref=0 has neither 0x40 nor 0xa0 set -> both branches skipped -> straight to stage5");
    ck(g_log.release_calls == 0,
       "stage5 (0x00486876 CMP word[+0x8c],0 / 0x0048687e JZ 0x004868b4): target_ref==0 -> "
       "target_release_ref is SKIPPED");
    ck(u.target_ref == 0 && u.target_index == 0, "stage5: target_ref/target_index remain 0");
    ck(g_log.set_state_order_calls == 1 && g_log.last_sso_state == 0x2f && g_log.last_sso_order == 0x13,
       "stage5 (0x004868b9 CMP/0x004868c0-0x004868ca): state==0x2e -> unit_set_state_order(0x2f, 0x13), "
       "run UNCONDITIONALLY regardless of the release skip");
    ck(ret == 1, "stage5: return=1");
}

void test_stage5_zero_target_ref_state_not_2e() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0;
    u.target_index        = 0;
    g_log.in_range_return = 1;
    u.state               = 0x7; // != 0x2e

    int32_t ret = run(f);

    ck(g_log.release_calls == 0, "stage5: target_ref==0 -> release skipped");
    ck(g_log.set_state_order_calls == 1 && g_log.last_sso_state == 0x13 && g_log.last_sso_order == 0x13,
       "stage5 (0x004868d1/0x004868db): state(0x7)!=0x2e -> unit_set_state_order(0x13, 0x13)");
    ck(ret == 1, "stage5: return=1");
}

void test_stage5_nonzero_target_ref_state_2e() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0x42; // building, dead -- falls through stage3 to stage5
    u.target_index        = 5;
    f.b(2, 5).energy      = -5.0;
    g_log.in_range_return = 1;
    u.state               = 0x2e;

    int32_t ret = run(f);

    ck(g_log.release_calls == 1 && g_log.last_release_player == CUR_PLAYER &&
           g_log.last_release_unit_idx == CUR_INDEX && g_log.last_release_mode == 1,
       "stage5 (0x00486893): target_ref=0x42!=0 -> target_release_ref(cur_player, cur_index, mode=1)");
    ck(u.target_ref == 0 && u.target_index == 0,
       "stage5 (0x0048689d/0x004868ab): target_ref/target_index cleared to 0");
    ck(g_log.set_state_order_calls == 1 && g_log.last_sso_state == 0x2f && g_log.last_sso_order == 0x13,
       "stage5 (0x004868b9/0x004868c0-0x004868ca): state==0x2e -> unit_set_state_order(0x2f, 0x13), "
       "combined with a release+clear that DID happen (the fourth cell of the release x state matrix)");
    ck(ret == 1, "stage5: return=1");
}

void test_stage5_nonzero_target_ref_state_not_2e() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0x42;
    u.target_index        = 5;
    f.b(2, 5).energy      = -5.0;
    g_log.in_range_return = 1;
    u.state               = 0x7;

    int32_t ret = run(f);

    ck(g_log.release_calls == 1 && g_log.last_release_mode == 1,
       "stage5: target_ref=0x42!=0 -> target_release_ref(..., mode=1) (matches "
       "test_stage3_building_dead_falls_to_stage5's own cell, restated here for the full 2x2 matrix)");
    ck(u.target_ref == 0 && u.target_index == 0, "stage5: target_ref/target_index cleared to 0");
    ck(g_log.set_state_order_calls == 1 && g_log.last_sso_state == 0x13 && g_log.last_sso_order == 0x13,
       "stage5: state(0x7)!=0x2e -> unit_set_state_order(0x13, 0x13)");
    ck(ret == 1, "stage5: return=1");
}

// ==== "still tracking, out of range" (0x004868e9-0x00486906): the ONLY path returning 0 =============

void test_out_of_range_state_2e_calls_set_state() {
    sim_fixture f;
    unit       &u         = seed(f);
    u.target_ref          = 0; // irrelevant -- this arm never reaches stage5's target_ref test at all
    g_log.in_range_return = 0;
    u.state               = 0x2e;

    int32_t ret = run(f);

    ck(ret == 0, "stage2 (0x004867d0 JZ 0x004868e9): in_range==0 -> return 0, the ONLY path that does");
    ck(g_log.set_state_calls == 1 && g_log.last_ss_state == 0x2f,
       "out-of-range arm (0x004868ee CMP/0x004868f5-0x004868fa): state==0x2e -> unit_set_state(0x2f) -- "
       "note: unit_set_state, NOT unit_set_state_order, a DIFFERENT callee than stage5 uses");
    ck(g_log.set_state_order_calls == 0 && g_log.release_calls == 0 && g_log.fire_calls == 0,
       "out-of-range arm never touches stage5's release/set_state_order machinery at all");
}

void test_out_of_range_state_not_2e_calls_nothing() {
    sim_fixture f;
    unit       &u         = seed(f);
    g_log.in_range_return = 0;
    u.state               = 0x9; // != 0x2e

    int32_t ret = run(f);

    ck(ret == 0, "out-of-range arm: return 0 regardless of state");
    ck(g_log.set_state_calls == 0,
       "out-of-range arm (0x004868f3 JNZ 0x004868ff): state!=0x2e -> NO call at all, not even a no-op");
}

} // namespace

void run_unit_chase_check_tests() {
    test_stage1_no_refresh_when_target_ref_lacks_0xa0();
    test_stage1_refresh_when_target_ref_has_0xa0();
    test_stage3_building_alive_fires();
    test_stage3_building_dead_falls_to_stage5();
    test_stage3_unit_alive_fires();
    test_stage3_unit_dead_falls_to_stage5();
    test_stage3_building_nan_energy_is_alive();
    test_stage5_zero_target_ref_state_2e();
    test_stage5_zero_target_ref_state_not_2e();
    test_stage5_nonzero_target_ref_state_2e();
    test_stage5_nonzero_target_ref_state_not_2e();
    test_out_of_range_state_2e_calls_set_state();
    test_out_of_range_state_not_2e_calls_nothing();
}

} // namespace mh::sim::test
