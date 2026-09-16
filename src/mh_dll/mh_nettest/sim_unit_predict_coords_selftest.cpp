#include "sim/sim_unit_predict_coords.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int      get_coords_calls       = 0;
    uint16_t last_get_coords_player = 0;
    int32_t  last_get_coords_unit   = -1;
    uint32_t get_coords_out_x       = 0; // what the mock WRITES (the unit's "current" fine coords)
    uint32_t get_coords_out_y       = 0;

    int      facing24_calls    = 0;
    uint32_t last_facing24_arg = 0;
    int32_t  facing24_out_dx   = 0; // what the mock WRITES
    int32_t  facing24_out_dy   = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_predict_coords_after_delay_calls &recording_calls() {
    static const unit_predict_coords_after_delay_calls c = {
        [](uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) -> void {
            ++g_log.get_coords_calls;
            g_log.last_get_coords_player = player;
            g_log.last_get_coords_unit   = unit_index;
            // committed llm_strat_unit_get_coords is int32_t * -- TACT1-P C6, 2026-09-04 -- the log
            // fields here are uint32_t, so the store re-interprets the same bit pattern.
            *out_x = (int32_t)g_log.get_coords_out_x;
            *out_y = (int32_t)g_log.get_coords_out_y;
        },
        [](uint32_t facing24, int32_t *out_dx, int32_t *out_dy) -> void {
            ++g_log.facing24_calls;
            g_log.last_facing24_arg = facing24;
            *out_dx                 = g_log.facing24_out_dx;
            *out_dy                 = g_log.facing24_out_dy;
        },
    };
    return c;
}

// ---- (a) time_delta <= 0 (both exactly 0 and negative): coords pass through UNCHANGED from
// whatever get_coords wrote, and facing24_to_delta is never called (the guard returns first). ----
void test_predict_coords_time_delta_zero_or_negative_passthrough() {
    sim_fixture    f;
    const sim_view v = f.view();

    g_log.reset();
    g_log.get_coords_out_x = 111;
    g_log.get_coords_out_y = 222;

    uint32_t out_x = 0xdeadbeef, out_y = 0xdeadbeef; // deliberately NOT the expected value yet
    detail::unit_predict_coords_after_delay(v, recording_calls(), /*player=*/1, /*unit_idx=*/2,
                                            /*time_delta=*/0.0, &out_x, &out_y);
    ck_eq(out_x, 111u, "predict_coords: time_delta==0.0 -> *out_x is exactly what get_coords wrote");
    ck_eq(out_y, 222u, "predict_coords: time_delta==0.0 -> *out_y is exactly what get_coords wrote");
    ck(g_log.facing24_calls == 0, "predict_coords: time_delta==0.0 -> facing24_to_delta never called");

    g_log.reset();
    g_log.get_coords_out_x = 333;
    g_log.get_coords_out_y = 444;
    out_x                  = 0xdeadbeef;
    out_y                  = 0xdeadbeef;
    detail::unit_predict_coords_after_delay(v, recording_calls(), 1, 2, /*time_delta=*/-5.0, &out_x, &out_y);
    ck_eq(out_x, 333u, "predict_coords: time_delta<0.0 (negative) -> *out_x unchanged from get_coords");
    ck_eq(out_y, 444u, "predict_coords: time_delta<0.0 (negative) -> *out_y unchanged from get_coords");
    ck(g_log.facing24_calls == 0, "predict_coords: time_delta<0.0 -> facing24_to_delta never called either");
}

// ---- (c) time_delta > 0 but state != move_op_code: still only get_coords runs. ------------------
void test_predict_coords_time_delta_positive_state_mismatch_no_advance() {
    sim_fixture    f;
    const sim_view v = f.view();

    const uint16_t player   = 2;
    const int32_t  unit_idx = 4;
    unit          &u        = f.u(player, unit_idx);
    u.unit_proto_id         = 7;
    u.state                 = 99; // deliberately NOT proto.move_op_code below

    cfg_unit &proto          = f.cfg_units[7];
    proto.move_op_code       = 42;
    proto.type               = 5; // < UNIT_TYPE_A_HELI, irrelevant here since the state gate fails first
    proto.step_speed[player] = 2.0;

    g_log.reset();
    g_log.get_coords_out_x = 555;
    g_log.get_coords_out_y = 666;

    uint32_t out_x = 0, out_y = 0;
    detail::unit_predict_coords_after_delay(v, recording_calls(), player, unit_idx, /*time_delta=*/7.0,
                                            &out_x, &out_y);

    ck_eq(out_x, 555u, "predict_coords: state != move_op_code -> *out_x still just get_coords' value");
    ck_eq(out_y, 666u, "predict_coords: state != move_op_code -> *out_y still just get_coords' value");
    ck(g_log.facing24_calls == 0,
       "predict_coords: state != move_op_code -> facing24_to_delta never called, no advance");
}

// ---- (b) time_delta > 0 AND state == move_op_code: advances by the hand-computed step count,
// scaled by move_step_speed_scale for ground classes (type < UNIT_TYPE_A_HELI), and the final add
// is masked (AND) by the map's PIXEL-space bw_mask/bh_mask -- exercised here with a small mask so
// the wraparound is actually visible, not just an addition that happens to stay in range. ----------
void test_predict_coords_advances_ground_unit_with_speed_scale_and_wrap() {
    sim_fixture f;
    f.geom.bw_mask   = 0xff; // small on purpose: the +12 add below must wrap through this
    f.geom.bh_mask   = 0xff; // small on purpose: the -8 add below must wrap through this (underflow)
    const sim_view v = f.view();

    const uint16_t player   = 2;
    const int32_t  unit_idx = 4;
    unit          &u        = f.u(player, unit_idx);
    u.unit_proto_id         = 7;
    u.state                 = 42;  // == proto.move_op_code below -> the move-step branch runs
    u.facing_target         = 9;   // arbitrary; just has to reach facing24_to_delta unchanged
    u.move_step_speed_scale = 1.0; // scale factor for GROUND classes (type < A_HELI)

    cfg_unit &proto          = f.cfg_units[7];
    proto.move_op_code       = 42;
    proto.type               = 5; // < UNIT_TYPE_A_HELI (0xf) -> move_step_speed_scale DOES apply
    proto.step_speed[player] = 2.0;

    g_log.reset();
    g_log.get_coords_out_x = 250; // near the 0xff wrap boundary on purpose
    g_log.get_coords_out_y = 5;   // near zero, so the negative delta underflows through the mask
    g_log.facing24_out_dx  = 3;
    g_log.facing24_out_dy  = -2;

    uint32_t out_x = 0, out_y = 0;
    detail::unit_predict_coords_after_delay(v, recording_calls(), player, unit_idx, /*time_delta=*/7.0,
                                            &out_x, &out_y);

    // steps = trunc(time_delta/step_speed + 0.51) = trunc(7.0/2.0 + 0.51) = trunc(3.5 + 0.51) =
    // trunc(4.01) = 4 (PREDICT_STEP_ROUND_OFFSET is 0.51, NOT 0.5 -- see the header's
    // CONDUCTOR-CONFIRMED note; a 0.5 offset would give the SAME answer here since 3.5+0.5=4.0
    // truncates to 4 too, so this particular ratio does not discriminate the two constants -- see
    // the dedicated 0.51-vs-0.5 case below for one that does).
    // delta_x = dx*steps = 3*4 = 12; delta_y = dy*steps = -2*4 = -8.
    // out_x = (250 + 12) & 0xff = 262 & 0xff = 6.
    // out_y = (5 + (uint32_t)(-8)) & 0xff = (5 - 8) mod 256 = 253.
    ck(g_log.facing24_calls == 1 && g_log.last_facing24_arg == 9u,
       "predict_coords: facing24_to_delta is called with the unit's facing_TARGET (not facing_current)");
    ck_eq(out_x, 6u, "predict_coords: ground unit advance, scale=1.0, steps=4 -> *out_x wraps to 6");
    ck_eq(out_y, 253u, "predict_coords: ground unit advance -> *out_y underflows through the mask to 253");
}

// ---- move_step_speed_scale is IGNORED for non-ground classes (type >= UNIT_TYPE_A_HELI): a huge
// scale value must NOT change the step count for a heli-class unit. -------------------------------
void test_predict_coords_speed_scale_ignored_for_non_ground_class() {
    sim_fixture f;
    f.geom.bw_mask   = 0xffff;
    f.geom.bh_mask   = 0xffff;
    const sim_view v = f.view();

    const uint16_t player   = 3;
    const int32_t  unit_idx = 9;
    unit          &u        = f.u(player, unit_idx);
    u.unit_proto_id         = 8;
    u.state                 = 42;
    u.facing_target         = 1;
    u.move_step_speed_scale = 100.0; // deliberately huge -- must be IGNORED for this class

    cfg_unit &proto          = f.cfg_units[8];
    proto.move_op_code       = 42;
    proto.type               = UNIT_TYPE_A_HELI; // >= A_HELI -> scale does NOT apply
    proto.step_speed[player] = 2.0;

    g_log.reset();
    g_log.get_coords_out_x = 0;
    g_log.get_coords_out_y = 0;
    g_log.facing24_out_dx  = 1;
    g_log.facing24_out_dy  = 0;

    uint32_t out_x = 0, out_y = 0;
    detail::unit_predict_coords_after_delay(v, recording_calls(), player, unit_idx, /*time_delta=*/7.0,
                                            &out_x, &out_y);

    // If the scale were (wrongly) applied: ratio = 7.0/(2.0*100.0) + 0.51 = 0.035 + 0.51 = 0.545,
    // trunc -> 0 steps, *out_x would stay 0. Applied correctly (ignored for this class): ratio =
    // 7.0/2.0 + 0.51 = 4.01, trunc -> 4 steps -> delta_x = 1*4 = 4.
    ck_eq(out_x, 4u,
          "predict_coords: type>=A_HELI -> move_step_speed_scale is NOT applied, steps computed from "
          "the unscaled cfg step_speed alone");
}

// ---- discriminates PREDICT_STEP_ROUND_OFFSET == 0.51 from a plausible-but-wrong 0.5: a ratio
// whose fractional part sits in the narrow band the header's CONDUCTOR-CONFIRMED note describes
// (time_delta/step_speed == 3.4921875 exactly, a dyadic value with no double-rounding surprise).
// +0.51 -> 4.0021875 -> trunc 4 steps. +0.5 (the translator's original, WRONG inference) would give
// 3.9921875 -> trunc 3 steps instead -- a different, observable step count. -----------------------
void test_predict_coords_round_offset_is_0_51_not_0_5() {
    sim_fixture f;
    f.geom.bw_mask   = 0xffff;
    f.geom.bh_mask   = 0xffff;
    const sim_view v = f.view();

    const uint16_t player   = 4;
    const int32_t  unit_idx = 6;
    unit          &u        = f.u(player, unit_idx);
    u.unit_proto_id         = 9;
    u.state                 = 42;
    u.facing_target         = 1;
    u.move_step_speed_scale = 1.0; // identity, isolate the round-offset question alone

    cfg_unit &proto          = f.cfg_units[9];
    proto.move_op_code       = 42;
    proto.type               = 5; // ground class; scale is 1.0 above so it doesn't matter here
    proto.step_speed[player] = 1.0;

    g_log.reset();
    g_log.get_coords_out_x = 0;
    g_log.get_coords_out_y = 0;
    g_log.facing24_out_dx  = 1; // so delta_x == steps directly, easy to read off
    g_log.facing24_out_dy  = 0;

    uint32_t out_x = 0, out_y = 0;
    detail::unit_predict_coords_after_delay(v, recording_calls(), player, unit_idx,
                                            /*time_delta=*/3.4921875, &out_x, &out_y);

    ck_eq(out_x, 4u,
          "predict_coords: ratio 3.4921875 + ROUND_OFFSET(0.51) truncates to 4 steps -- a 0.5 offset "
          "would wrongly give 3 (see the header's CONDUCTOR-CONFIRMED 0.51-vs-0.5 note)");
}

} // namespace

void run_unit_predict_coords_tests() {
    test_predict_coords_time_delta_zero_or_negative_passthrough();
    test_predict_coords_time_delta_positive_state_mismatch_no_advance();
    test_predict_coords_advances_ground_unit_with_speed_scale_and_wrap();
    test_predict_coords_speed_scale_ignored_for_non_ground_class();
    test_predict_coords_round_offset_is_0_51_not_0_5();
}

} // namespace mh::sim::test
