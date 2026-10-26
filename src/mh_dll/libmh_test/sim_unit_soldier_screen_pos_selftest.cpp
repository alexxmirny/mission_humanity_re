//
// sim_unit_soldier_screen_pos_selftest.cpp -- `simtest` cases for
// llm_strat_unit_soldier_get_sprite_screen_pos (sim/sim_unit_soldier_screen_pos.h/.cpp), SIM1A sixth
// slice. ONLY possible oracle: the shadow site is VACUOUS (writes only through its two OUT-pointer
// params -- 11891 calls armed, zero regions compared, in the sixth-slice soak).
//
// The one outward call (llm_strat_unit_calc_interp_pixel_pos) is indirected precisely so this offline
// test can drive it. We MOCK it to a fixed per-axis value (distinct for x vs y, so a swapped-axis
// translation is caught) -- the real function is itself already T1-verified by the rig, so pinning
// its result lets these cases isolate THIS function's chain-walk + sprite-index + mount/origin
// combine + mask arithmetic.
//
// EXPECTED VALUES ARE HAND-COMPUTED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_unit_soldier_get_sprite_screen_pos_00491086.asm), not read off the .cpp. The
// formula the asm implements:
//   soldier_idx = word(unit.unit_above);  repeat soldier_hop_count times: soldier_idx =
//                 soldiers[player*100 + soldier_idx].next_soldier               (0x004910a7-b0fc)
//   sprite_base = Unit[proto].sprite + ((soldier.sprite_frame % 24) / 3) * 8    (0x0049110f-b1a3)
//   div4        = (anim_change_count + unit.move_microstep) / 4, truncating toward zero
//                 (the SAR/SHL/SBB/SAR idiom @0x00491186-b190)
//   sprite_idx  = sprite_base + (div4 % 8);  meta = sprite_meta[sprite_idx]
//   out_x = general.bw_mask & (soldier.cur_x + (interp_x + meta.mount1_x - meta.origin_x))  (..b201)
//   out_y = general.bh_mask & (soldier.cur_y + (meta.mount1_y + interp_y - meta.origin_y))  (..b255)
// (SOLDIERS_PER_PLAYER == 100; sprite_meta fields origin_x/origin_y/mount1_x/mount1_y are int16.)
//
#include "sim/sim_unit_soldier_screen_pos.h"
#include "sim/sim_weapon_projectile_spawn.h" // mp:DSHEAL S3: the scatter draw count that hangs off the origin

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t kPlayer = 1;
constexpr int32_t  kIndex  = 2;

// The mocked calc_interp_pixel_pos: a fixed value per axis. Distinct x/y so an axis swap in the
// combine step below is caught. Set per case before the call.
int32_t g_interp_x = 0;
int32_t g_interp_y = 0;
int32_t mock_interp(uint16_t /*player*/, int32_t /*unit_idx*/, char axis_is_x) {
    return axis_is_x == 1 ? g_interp_x : g_interp_y;
}
const unit_soldier_get_sprite_screen_pos_calls &mock_calls() {
    static const unit_soldier_get_sprite_screen_pos_calls c = {&mock_interp};
    return c;
}

soldier &sol(sim_fixture &fx, int32_t player, int32_t idx) {
    return fx.soldiers[(size_t)(player * SOLDIERS_PER_PLAYER + idx)];
}

void check_screen_pos(sim_fixture &fx, int32_t hop, uint32_t want_x, uint32_t want_y,
                      const char *what_x, const char *what_y) {
    uint32_t ox = 0x0badf00d, oy = 0x0badf00d;
    detail::unit_soldier_get_sprite_screen_pos(fx.view(), mock_calls(), kPlayer, kIndex, hop, &ox, &oy);
    ck_eq(ox, want_x, what_x);
    ck_eq(oy, want_y, what_y);
}

// mp:DSHEAL S3 helpers: the scatter step of llm_strat_unit_fire_weapon (0x0048bf5a), run for real over an origin
// produced by the real screen-pos body. rand_below is COUNTED, pixel_delta_wrapped is the plain difference.
int32_t g_draws = 0;
int32_t counting_rand_below(int32_t) {
    ++g_draws;
    return 0;
}
void plain_pixel_delta(int32_t sx, int32_t sy, int32_t dx, int32_t dy, int32_t *ox, int32_t *oy) {
    *ox = dx - sx;
    *oy = dy - sy;
}

// The origin the shooter's soldier reports and the number of RNG draws the scatter then makes toward the target.
struct shot {
    uint32_t x, y;
    int32_t  draws;
};
shot fire_from(sim_fixture &fx, uint8_t anim_change_count, int32_t target_x, int32_t target_y) {
    sol(fx, kPlayer, 5).anim_change_count = anim_change_count;
    shot s{};
    detail::unit_soldier_get_sprite_screen_pos(fx.view(), mock_calls(), kPlayer, kIndex, 0, &s.x, &s.y);
    const weapon_scatter_offset_calls sc = {&plain_pixel_delta, &counting_rand_below};
    int32_t                           ox = 0, oy = 0;
    g_draws = 0;
    detail::weapon_scatter_offset(/*experience*/ 0, /*weapon missing*/ 1, (int32_t)s.x, (int32_t)s.y, target_x, target_y, &ox,
                                  &oy, sc);
    s.draws = g_draws;
    return s;
}

} // namespace

void run_unit_soldier_screen_pos_tests() {
    sim_fixture fx;

    // ---- S1: hop_count 0 (soldier_idx stays at the chain head), non-negative div4, and masks that
    // ACTUALLY TRUNCATE (bw_mask=0xff, bh_mask=0x3f) so the final `& mask` is exercised, not a no-op.
    //   head = word({5,0}) = 5;  s = soldiers[1*100 + 5]
    //   sprite_base = 100 + ((6 % 24) / 3) * 8 = 100 + 2*8 = 116
    //   div4 = (2 + 6) / 4 = 2;  sprite_idx = 116 + (2 % 8) = 118
    //   out_x = 0xff & (10 + (1000 + 7 - 3)) = 0xff & 1014(0x3f6) = 0xf6 = 246
    //   out_y = 0x3f & (20 + (9 + 2000 - 4)) = 0x3f & 2025(0x7e9) = 0x29 = 41
    fx.reset();
    fx.geom.bw_mask                       = 0xff;
    fx.geom.bh_mask                       = 0x3f;
    fx.u(kPlayer, kIndex).unit_above[0]   = 5;
    fx.u(kPlayer, kIndex).unit_above[1]   = 0;
    fx.u(kPlayer, kIndex).unit_proto_id   = 3;
    fx.cfg_units[3].sprite                = 100;
    fx.u(kPlayer, kIndex).move_microstep  = 6;
    sol(fx, kPlayer, 5).sprite_frame      = 6;
    sol(fx, kPlayer, 5).anim_change_count = 2;
    sol(fx, kPlayer, 5).cur_x             = 10;
    sol(fx, kPlayer, 5).cur_y             = 20;
    fx.sprite_meta[118].mount1_x          = 7;
    fx.sprite_meta[118].mount1_y          = 9;
    fx.sprite_meta[118].origin_x          = 3;
    fx.sprite_meta[118].origin_y          = 4;
    g_interp_x                            = 1000;
    g_interp_y                            = 2000;
    check_screen_pos(fx, 0, 246, 41, "soldier_screen_pos S1 hop0 mask-truncate .x",
                     "soldier_screen_pos S1 hop0 mask-truncate .y");

    // ---- S2: hop_count 2 (walk the chain 5->9->4), NEGATIVE div4 (the signed SAR/SBB idiom), wide
    // masks. Decoys at the head (idx 2), the intermediate (idx 9) and at the wrong-base slot (idx 4,
    // player 0) catch a mis-walked chain or a dropped player*100 stride.
    //   head = word({2,0}) = 2; hop0: idx = soldiers[102].next_soldier = 9;
    //                           hop1: idx = soldiers[109].next_soldier = 4; final s = soldiers[104]
    //   sprite_base = 200 + ((15 % 24) / 3) * 8 = 200 + 5*8 = 240
    //   div4 = (1 + (-9)) / 4 = -8/4 = -2 (trunc toward zero);  sprite_idx = 240 + (-2 % 8) = 238
    //   out_x = 0xffff & (30 + (100 + 1 - 5)) = 126
    //   out_y = 0xffff & (40 + (2 + 500 - 6)) = 536
    fx.reset();
    fx.geom.bw_mask                       = 0xffff;
    fx.geom.bh_mask                       = 0xffff;
    fx.u(kPlayer, kIndex).unit_above[0]   = 2;
    fx.u(kPlayer, kIndex).unit_above[1]   = 0;
    fx.u(kPlayer, kIndex).unit_proto_id   = 6;
    fx.cfg_units[6].sprite                = 200;
    fx.u(kPlayer, kIndex).move_microstep  = -9;
    sol(fx, kPlayer, 2).next_soldier      = 9;  // head -> hop0
    sol(fx, kPlayer, 2).sprite_frame      = 99; // decoy: read if hop_count were ignored
    sol(fx, kPlayer, 9).next_soldier      = 4;  // hop0 -> hop1
    sol(fx, kPlayer, 9).sprite_frame      = 77; // decoy
    sol(fx, kPlayer, 4).sprite_frame      = 15; // the ACTUAL soldier reached
    sol(fx, kPlayer, 4).anim_change_count = 1;
    sol(fx, kPlayer, 4).cur_x             = 30;
    sol(fx, kPlayer, 4).cur_y             = 40;
    sol(fx, 0, 4).cur_x                   = 55; // decoy: wrong player*100 base would read here
    fx.sprite_meta[238].mount1_x          = 1;
    fx.sprite_meta[238].mount1_y          = 2;
    fx.sprite_meta[238].origin_x          = 5;
    fx.sprite_meta[238].origin_y          = 6;
    g_interp_x                            = 100;
    g_interp_y                            = 500;
    check_screen_pos(fx, 2, 126, 536, "soldier_screen_pos S2 hop2 chain+neg-div4 .x",
                     "soldier_screen_pos S2 hop2 chain+neg-div4 .y");

    // ---- S3 (mp:DSHEAL): the shot origin must not depend on the RENDER-ONLY anim_change_count ----------
    // The renderers write soldier.anim_change_count and the state hash masks it, yet the retail body adds it to
    // move_microstep to pick the sprite frame whose mount offsets become llm_strat_unit_fire_weapon's shot origin.
    // The origin sets the scatter bound (weapon_scatter_offset: missing * |delta| / 200), and the bound is the
    // difference between 0 and 2 rand_below draws -- two peers whose screens drew different counters then diverge in
    // the RNG stream (v0.2.2-rc1 public match 44f21f93).
    //   sprite_base = 100 + ((0 % 24) / 3) * 8 = 100;  move_microstep = 0
    //   counter 0  -> div4 = 0  -> sprite 100 (mount1_x 0);  counter 40 -> div4 = 10 -> 10 % 8 = 2 -> sprite 102 (mount1_x 200)
    //   target (210, 0), missing 1: counter 0 -> |210|/200 = 1 -> 2 draws;  counter 40 -> |10|/200 = 0 -> 0 draws.
    fx.reset();
    fx.geom.bw_mask                      = 0xffff;
    fx.geom.bh_mask                      = 0xffff;
    fx.u(kPlayer, kIndex).unit_above[0]  = 5;
    fx.u(kPlayer, kIndex).unit_above[1]  = 0;
    fx.u(kPlayer, kIndex).unit_proto_id  = 3;
    fx.cfg_units[3].sprite               = 100;
    fx.u(kPlayer, kIndex).move_microstep = 0;
    sol(fx, kPlayer, 5).sprite_frame     = 0;
    fx.sprite_meta[100].mount1_x         = 0;
    fx.sprite_meta[102].mount1_x         = 200;
    g_interp_x                           = 0;
    g_interp_y                           = 0;

    set_soldier_origin_anim_fix(false); // retail: the counter reaches the origin -- the test BITES
    const shot off_a = fire_from(fx, 0, 210, 0);
    const shot off_b = fire_from(fx, 40, 210, 0);
    ck(off_a.x != off_b.x, "soldier_screen_pos S3 fix OFF: a different anim_change_count moves the shot origin (retail)");
    ck_eq((uint32_t)off_a.draws, 2u, "soldier_screen_pos S3 fix OFF: counter 0 -> scatter bound 1 -> 2 draws");
    ck_eq((uint32_t)off_b.draws, 0u, "soldier_screen_pos S3 fix OFF: counter 40 -> scatter bound 0 -> 0 draws (the desync)");

    set_soldier_origin_anim_fix(true);
    const shot on_a = fire_from(fx, 0, 210, 0);
    const shot on_b = fire_from(fx, 40, 210, 0);
    const shot on_c = fire_from(fx, 255, 210, 0);
    ck(on_a.x == on_b.x && on_a.y == on_b.y && on_a.x == on_c.x && on_a.y == on_c.y,
       "soldier_screen_pos S3 fix ON: the shot origin is the same whatever anim_change_count is");
    ck(on_a.draws == on_b.draws && on_a.draws == on_c.draws, "soldier_screen_pos S3 fix ON: the same scatter RNG draw count");
    ck(on_a.x == off_a.x && on_a.y == off_a.y && on_a.draws == off_a.draws,
       "soldier_screen_pos S3 fix ON == the retail body with the counter read as 0");
    // move_microstep still moves the frame (the rule drops ONLY the render counter)
    fx.u(kPlayer, kIndex).move_microstep = 40;
    const shot on_m                      = fire_from(fx, 0, 210, 0);
    ck(on_m.x == 200, "soldier_screen_pos S3 fix ON: unit.move_microstep still selects the sprite frame (40 -> sprite 102)");
    set_soldier_origin_anim_fix(false);
}

} // namespace mh::sim::test
