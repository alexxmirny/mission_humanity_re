//
// sim_unit_fine_pos_selftest.cpp -- `simtest` cases for llm_strat_unit_get_coords
// (sim/sim_unit_fine_pos.h/.cpp), SIM1A. This is the ONLY possible oracle for
// get_coords: its shadow site is VACUOUS (writes only through its two OUT-pointer params -- 829086
// calls armed, zero regions compared, in the seventh-slice soak). Its three siblings in the same TU
// (calc_fine_axis_pos / calc_render_fine_y / calc_interp_pixel_pos) DO write a return value and were
// already T1-verified by the rig; only get_coords needs this offline oracle.
//
// EXPECTED VALUES ARE HAND-COMPUTED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_unit_get_coords_0044b141.asm), NOT read off the .cpp -- the two formulas the
// asm implements, confirmed instruction-by-instruction:
//   type < UNIT_TYPE_A_HELI (CMP type,0xe / JG -> airborne; so type<=0xe stays here):
//     remaining = 0x1f - move_microstep                              (0x0044b1a2-b1ad)
//     dx = facing_step_offset[facing_target].dx (table @0xae4768, stride 8)   (0x0044b1c3-b1d3)
//     dy = facing_step_offset[facing_target].dy (@0xae476c)                   (0x0044b1e9-b1f9)
//     out_x = (int)(big_width  + dx*remaining + x*0x20 + 0x10) % (int)big_width   (0x0044b20f-b23f)
//     out_y = (int)(big_height + dy*remaining + y*0x20 + 0x10) % (int)big_height  (0x0044b254-b284)
//   type >= UNIT_TYPE_A_HELI (0xf), LAB_0044b28b: NO wrap, NO +0x10:
//     idx  = move_heading*0x60 + move_microstep*3  (move_microsteps[move_heading*32 + move_microstep])
//     out_x = x*0x20 + move_microsteps[idx].x_off  (x_off is MOVZX -> zero-extended uint8)  (..b2ef)
//     out_y = y*0x20 + move_microsteps[idx].y_off                                            (..b355)
// The type gate is `CMP type,0xe / JG`, so type==0xe -> ground branch and type==0xf(=UNIT_TYPE_A_HELI)
// -> airborne branch. Both boundary sides are asserted below.
//
#include "sim/sim_unit_fine_pos.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// player/unit_index chosen non-zero and unequal so a translation that dropped either index term
// would land on a different slot than the one seeded.
constexpr uint16_t kPlayer = 1;
constexpr int32_t  kIndex  = 2;

void check_coords(sim_fixture &fx, int32_t want_x, int32_t want_y, const char *what_x,
                  const char *what_y) {
    int32_t ox = 0x0badf00d, oy = 0x0badf00d;
    detail::get_coords(fx.view(), kPlayer, kIndex, &ox, &oy);
    ck_eq((uint32_t)ox, (uint32_t)want_x, what_x);
    ck_eq((uint32_t)oy, (uint32_t)want_y, what_y);
}

} // namespace

void run_unit_fine_pos_tests() {
    sim_fixture fx;

    // ---- G1: ground unit (type<0xf), move_microstep==0x1f so remaining==0 -> the facing_step_offset
    // term drops out, isolating the (big + tile*0x20 + 0x10) % big torus wrap.
    // out_x = (0x400 + 0 + 3*0x20 + 0x10) % 0x400 = 0x470 % 0x400 = 0x70 = 112
    // out_y = (0x400 + 0 + 5*0x20 + 0x10) % 0x400 = 0x4b0 % 0x400 = 0xb0 = 176
    fx.reset();
    fx.geom.big_width                    = 0x400;
    fx.geom.big_height                   = 0x400;
    fx.u(kPlayer, kIndex).unit_proto_id  = 5;
    fx.cfg_units[5].type                 = 5; // < UNIT_TYPE_A_HELI (ground/soldier)
    fx.u(kPlayer, kIndex).move_microstep = 0x1f;
    fx.u(kPlayer, kIndex).x              = 3;
    fx.u(kPlayer, kIndex).y              = 5;
    fx.u(kPlayer, kIndex).facing_target  = 7;
    fx.facing_step_offset[7]             = {3, -2};  // present but multiplied by remaining==0
    fx.facing_step_offset[0]             = {50, 60}; // decoy: if the code read [0] the result differs
    check_coords(fx, 112, 176, "get_coords G1 ground remaining=0 .x", "get_coords G1 ground remaining=0 .y");

    // ---- G2: ground unit, move_microstep==0x1e so remaining==1 -> the facing_step_offset[7] delta is
    // now live, and dy is negative (tests the unsigned dy*remaining fold + signed modulo).
    // out_x = (0x400 + 3*1 + 3*0x20 + 0x10) % 0x400 = 0x473 % 0x400 = 0x73 = 115
    // out_y = (0x400 + (-2)*1 + 5*0x20 + 0x10) % 0x400 = 0x4ae % 0x400 = 0xae = 174
    fx.reset();
    fx.geom.big_width                    = 0x400;
    fx.geom.big_height                   = 0x400;
    fx.u(kPlayer, kIndex).unit_proto_id  = 5;
    fx.cfg_units[5].type                 = 5;
    fx.u(kPlayer, kIndex).move_microstep = 0x1e;
    fx.u(kPlayer, kIndex).x              = 3;
    fx.u(kPlayer, kIndex).y              = 5;
    fx.u(kPlayer, kIndex).facing_target  = 7;
    fx.facing_step_offset[7]             = {3, -2};
    fx.facing_step_offset[0]             = {50, 60}; // decoy (wrong-index would give x=162, not 115)
    check_coords(fx, 115, 174, "get_coords G2 ground remaining=1 .x", "get_coords G2 ground remaining=1 .y");

    // ---- G3: airborne unit (type>=0xf) -> move_microsteps table, no wrap, no +0x10.
    // idx = move_heading*32 + move_microstep = 2*32 + 3 = 67.
    // out_x = 4*0x20 + 50  = 128 + 50  = 178
    // out_y = 6*0x20 + 200 = 192 + 200 = 392
    fx.reset();
    fx.geom.big_width                    = 0x400; // unused on this branch; set anyway so a stray read is not a div0
    fx.geom.big_height                   = 0x400;
    fx.u(kPlayer, kIndex).unit_proto_id  = 6;
    fx.cfg_units[6].type                 = 0x11; // >= UNIT_TYPE_A_HELI (plane)
    fx.u(kPlayer, kIndex).move_heading   = 2;
    fx.u(kPlayer, kIndex).move_microstep = 3;
    fx.u(kPlayer, kIndex).x              = 4;
    fx.u(kPlayer, kIndex).y              = 6;
    fx.move_microsteps[67]               = {50, 200, 0}; // {x_off, y_off, facing}
    fx.move_microsteps[0].x_off          = 99;           // decoy for a dropped index term
    check_coords(fx, 178, 392, "get_coords G3 airborne .x", "get_coords G3 airborne .y");

    // ---- G4: type gate boundary. type==0xf (== UNIT_TYPE_A_HELI) must take the AIRBORNE branch
    // (CMP type,0xe / JG). idx = 1*32 + 0 = 32. out_x = 2*0x20 + 10 = 74; out_y = 1*0x20 + 20 = 52.
    fx.reset();
    fx.geom.big_width                    = 0x400;
    fx.geom.big_height                   = 0x400;
    fx.u(kPlayer, kIndex).unit_proto_id  = 7;
    fx.cfg_units[7].type                 = 0x0f; // exactly UNIT_TYPE_A_HELI -> airborne
    fx.u(kPlayer, kIndex).move_heading   = 1;
    fx.u(kPlayer, kIndex).move_microstep = 0;
    fx.u(kPlayer, kIndex).x              = 2;
    fx.u(kPlayer, kIndex).y              = 1;
    fx.move_microsteps[32]               = {10, 20, 0};
    check_coords(fx, 74, 52, "get_coords G4 type==0xf -> airborne .x", "get_coords G4 type==0xf -> airborne .y");

    // ---- G4b: the OTHER side of the boundary. type==0xe must take the GROUND branch (remaining=0
    // here). out_x = (0x400 + 1*0x20 + 0x10) % 0x400 = 0x30 = 48; out_y same.
    fx.reset();
    fx.geom.big_width                    = 0x400;
    fx.geom.big_height                   = 0x400;
    fx.u(kPlayer, kIndex).unit_proto_id  = 8;
    fx.cfg_units[8].type                 = 0x0e; // just below UNIT_TYPE_A_HELI -> ground
    fx.u(kPlayer, kIndex).move_microstep = 0x1f;
    fx.u(kPlayer, kIndex).x              = 1;
    fx.u(kPlayer, kIndex).y              = 1;
    fx.u(kPlayer, kIndex).facing_target  = 0;
    check_coords(fx, 48, 48, "get_coords G4b type==0xe -> ground .x", "get_coords G4b type==0xe -> ground .y");
}

} // namespace mh::sim::test
