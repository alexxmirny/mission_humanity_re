//
// sim_projectile_tick_selftest.cpp -- `simtest` cases for llm_strat_projectile_tick @0x00440e1c
// (sim/sim_projectile_tick.h/.cpp). This is a SIM1E verification-debt oracle: the function's shadow
// closure is UNBOUNDED (shadow_region_closure.py --depth 4 -- apply_area_damage -> game_SetEvent's UI
// cluster, plus unit_notify_ui / ai_bldg_register_visible_building writes at depth 3), so it can never
// be armed against the live game. This file is the ONLY execution evidence this function will get.
//
// SCOPE (honest, not exhaustive): the flight-integration arithmetic (both branches' duration recompute
// gated behind controllable sqrt_fn/map_wrapped_delta knobs), all three inline trunc_* shapes each
// driven with a NEGATIVE operand at least once, the homing-valid AND-gate (all three legs, including
// the `homing==1` EQUALITY sense and the `energy>0.0` STRICT sense), the homing dst_x/dst_y_ground
// scatter writes, the dst_y_vis partial-16-bit-read derivation (independently re-derived below, not
// trusted from the .cpp), the flight-vs-impact boundary (equality resolves to IMPACT), the impact arm's
// full call order + argument tuples under both SIM_ACTIVE settings, the ballistic ring_count/damage
// weapon-slot indexing INCLUDING the documented 9..15 out-of-bounds alias, the facing-recompute type
// gate (ballistic only) and its arithmetic, the anim-advance loop's three arms (skip / wrap-to-loop /
// advance-by-next), the smoke-puff and explo-pulse loops (including the field-choice distinction that
// explo-pulse tiles off src_y_ground while smoke tiles off src_y_vis), and the projectile-pool slot
// bookkeeping (both the "cur is a normal slot" and the "cur IS slot 0" aliasing edge case).
// DOES NOT COVER: the sum-grouping-order claim for the sqrt distance argument (the .cpp's own header
// banner already argues this from the raw FADDP sequence; re-deriving it here would need FP inputs
// engineered to be rounding-order-sensitive, which is a separate, expensive derivation not attempted);
// w_sprintf-style text formatting (none exists in this function); multi-iteration smoke/explo loops
// (each loop is tested for exactly one iteration plus its gates, not a chained multi-puff sequence).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_projectile_tick_00440e1c.asm, NOT from the
// .cpp under test -- every constant/bound/index/call-arg below is justified by an instruction address
// cited inline. Two derivations were done independently here rather than trusted from the .cpp/.h
// banners, because trusting the banner would just be checking the .cpp against itself:
//
//   (1) THE "weapon_id*0x16c + 0xc3a520 + 0x5a == 0" SENTINEL IS UNREACHABLE. 0x16c (364) = 4*91, so
//       weapon_id*0x16c mod 4 == 0 for every int32 weapon_id (0x16c is a multiple of 4, and multiplying
//       any integer by a multiple of 4 stays a multiple of 4 mod 2^32). 0xc3a520 + 0x5a = 0xc3a57a;
//       0xc3a57a mod 4 == 2 (0x7a = 0b0111_1010, low two bits = 10). A sum of "multiple-of-4" and
//       "== 2 mod 4" is never == 0 mod 4, hence never exactly 0 (mod 2^32 arithmetic still respects mod
//       4). CONFIRMED independently for weapon_id=0: 0*0x16c+0xc3a520+0x5a = 0xc3a57a != 0. So the two
//       "reset scatter to 0" / "skip ballistic recompute" arms (0x00440e7c JZ 0x00440eba and
//       0x004410bb JZ 0x004411e9) never fire -- every case below that reaches the ballistic path proves
//       this empirically for its own weapon_id by observing the scatter/recompute effects still happen.
//       The function's helpers that reproduce this test are in an ANONYMOUS namespace in the .cpp under
//       test (internal linkage), so they cannot be called directly from this TU; this file can only
//       prove the REACHABLE side, same as the .cpp's own posture.
//
//   (2) THE dst_y_vis PARTIAL-16-BIT READ IS A MATHEMATICAL IDENTITY, NOT A MAGNITUDE-DEPENDENT CLAIM.
//       0x00440f91 `MOV AX, word ptr[dst_y_ground]` / 0x00440f95 `SUB AX, word ptr[elevation]` / 0x00440f9c
//       `MOV DX, word ptr[bh_mask]` / 0x00440fa3 `AND EDX,EAX` / 0x00440faa `MOV word ptr[dst_y_vis],DX`
//       computes the WHOLE chain at 16-bit width and stores only the low 16 bits. Bitwise AND is
//       per-bit, and truncation to 16 bits is bit SELECTION, so (mask32 & diff32) truncated to u16 ==
//       (mask32 mod 2^16) & (diff32 mod 2^16) == a 16-bit-native AND, for ANY operand magnitude -- this
//       holds regardless of whether the subtraction's true (unmasked) magnitude exceeds 16 bits, because
//       modular reduction mod 2^16 commutes with subtraction mod 2^32 (2^16 divides 2^32) and with AND.
//       test_homing_dst_y_vis_partial_width_derivation below still drives the subtraction's magnitude
//       well past 65535 (dst_y_ground=60000, elevation=-10000 -> raw diff 70000) specifically so that a
//       DIFFERENT bug -- e.g. a translation that forgot the final uint16_t truncation, used the wrong
//       sign extension, or read the wrong field -- would show up as a wrong low-16 result; the identity
//       itself cannot fail, so a green case here is evidence against those bugs, not proof the identity
//       "held this time".
//
#include "sim/sim_projectile_tick.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across all 10 projectile_tick_calls members -----------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knobs (set BEFORE seed_fixture/run_tick) ----------------------------------------
double  g_mwd_dx_ret = 0.0, g_mwd_dy_ret = 0.0; // map_wrapped_delta's two double outputs
int32_t g_row_ret  = 0;                         // map_wrap_delta_row's return
double  g_sqrt_ret = 1.0;                       // llm_sqrt's return (a knob, NOT a real sqrt --
                                                // decouples duration control from the distance calc)
int32_t g_coords_x_ret = 0, g_coords_y_ret = 0; // unit_get_coords's two outputs
int32_t g_dir_ret    = 0;                       // dir_from_to's return
int32_t g_stride_ret = 0;                       // fx_anim_dir_frame_stride's return

// ---- per-callee recorders (10, one per projectile_tick_calls member) ------------------------------
struct MwdCall {
    int32_t src_x, src_y, dst_x, dst_y;
};
std::vector<MwdCall> g_mwd_calls;
void                 rec_map_wrapped_delta(int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y,
                                           double *out_dx, double *out_dy) {
    tr("map_wrapped_delta");
    g_mwd_calls.push_back({src_x, src_y, dst_x, dst_y});
    *out_dx = g_mwd_dx_ret;
    *out_dy = g_mwd_dy_ret;
}

struct RowCall {
    int32_t x1, y1, x2, y2;
};
std::vector<RowCall> g_row_calls;
int32_t              rec_map_wrap_delta_row(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("map_wrap_delta_row");
    g_row_calls.push_back({x1, y1, x2, y2});
    return g_row_ret;
}

int                 g_sqrt_calls = 0;
std::vector<double> g_sqrt_args;
double              rec_sqrt(double x) {
    tr("sqrt");
    ++g_sqrt_calls;
    g_sqrt_args.push_back(x);
    return g_sqrt_ret;
}

struct CoordsCall {
    uint16_t player;
    int32_t  index;
};
std::vector<CoordsCall> g_coords_calls;
void                    rec_unit_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    tr("unit_get_coords");
    g_coords_calls.push_back({player, index});
    *out_x = g_coords_x_ret;
    *out_y = g_coords_y_ret;
}

struct AnimSpawnCall {
    uint32_t x, y, anim_id;
    double   elapsed;
    uint32_t owner_or_flag;
};
std::vector<AnimSpawnCall> g_anim_spawn_calls;
uint32_t                   rec_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t anim_id, double elapsed, uint32_t owner_or_flag) {
    tr("fx_anim_spawn");
    g_anim_spawn_calls.push_back({x, y, anim_id, elapsed, owner_or_flag});
    return 0;
}

struct SndAtCall {
    int32_t id, col, row;
};
std::vector<SndAtCall> g_snd_play_at_calls;
void                   rec_snd_play_at(int32_t id, int32_t tile_col, int32_t tile_row) {
    tr("snd_play_at");
    g_snd_play_at_calls.push_back({id, tile_col, tile_row});
}

struct DirCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DirCall> g_dir_calls;
int32_t              rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_dir_calls.push_back({x1, y1, x2, y2});
    return g_dir_ret;
}

std::vector<int32_t> g_stride_calls; // start_frame arg
int32_t              rec_fx_anim_dir_frame_stride(int32_t start_frame) {
    tr("fx_anim_dir_frame_stride");
    g_stride_calls.push_back(start_frame);
    return g_stride_ret;
}

struct AreaDamageCall {
    int32_t  x, y, kind;
    double   dmg;
    int32_t  ring;
    uint32_t owner;
    uint32_t killer_info;
    int32_t  killer_idx;
};
std::vector<AreaDamageCall> g_area_damage_calls;
void                        rec_apply_area_damage(int32_t x, int32_t y, int32_t kind, double dmg, int32_t ring, uint32_t owner,
                                                  uint32_t killer_info, int32_t killer_idx) {
    tr("apply_area_damage");
    g_area_damage_calls.push_back({x, y, kind, dmg, ring, owner, killer_info, killer_idx});
}

const projectile_tick_calls g_calls = {
    &rec_map_wrapped_delta,
    &rec_map_wrap_delta_row,
    &rec_sqrt,
    &rec_unit_get_coords,
    &rec_fx_anim_spawn,
    &rec_snd_play_at,
    &rec_dir_from_to,
    &rec_fx_anim_dir_frame_stride,
    &rec_apply_area_damage,
};

void reset_observations() {
    g_trace.clear();
    g_mwd_calls.clear();
    g_row_calls.clear();
    g_sqrt_calls = 0;
    g_sqrt_args.clear();
    g_coords_calls.clear();
    g_anim_spawn_calls.clear();
    g_snd_play_at_calls.clear();
    g_dir_calls.clear();
    g_stride_calls.clear();
    g_area_damage_calls.clear();
}

// ---- fixture seeding ------------------------------------------------------------------------------
//
// All distinct/non-symmetric per sim_test_support.h's rule. Geometry defaults are shared across most
// cases (bw_mask/bh_mask/big_width/big_height, both < the values they mask so most seeds pass through
// unmasked and the AND is a no-op unless a case deliberately drives a value past the mask).
struct Seed {
    int32_t cur_slot = 3; // projectile_pool slot repointed as "current"; 0 is the pool's live-count
                          // header slot -- see test_slot_bookkeeping's dedicated cur_slot==0 case.

    // ---- cur projectile fields ---------------------------------------------------------------------
    int32_t  weapon_id = 1;
    uint16_t src_x = 1000, src_y_ground = 1200, src_y_vis = 1100;
    uint16_t dst_x = 2000, dst_y_ground = 2100, dst_y_vis = 2050; // ballistic-branch LOCAL seed
    uint8_t  owner_player  = 4;
    double   launch_time   = 0.0;
    double   duration_seed = 1.0; // PRE-tick value -- the scatter ratio's denominator; must be nonzero
                                  // (0x00440e86 FDIV executes unconditionally -- see banner note 1).
    double   anim_clock        = 48.0;
    double   smoke_clock       = 10.0;
    double   explo_pulse_clock = 10.0;
    int32_t  scatter_x = 0, scatter_y = 0;
    int32_t  facing           = 5;
    int32_t  anim_frame       = 10;
    int32_t  explo_pulse_flag = 0;
    int32_t  anim_loop_frame  = 50;
    uint16_t homing_player    = 0;
    int32_t  homing_unit      = 0; // 0 -> ballistic branch by construction (0x00440ecd CMP ...,0 / JZ)
    uint16_t shooter_ref      = 0x02;
    int32_t  shooter_unit     = 99;
    int32_t  active_seed      = 1;

    // ---- cfg_weapons[weapon_id] ---------------------------------------------------------------------
    uint8_t w_type                     = 1; // != 8 -- see 0x004411d9's CMP ...,0x8
    uint8_t w_homing                   = 0; // must be EXACTLY 1 (0x00440edf CMP ...,0x1 / JZ) to matter
    double  w_speed                    = 1.0;
    int32_t w_length                   = 1;
    int32_t w_target_explo             = 777;
    int32_t w_smoke_sprite             = 0;
    double  w_smoke_time               = 0.0;
    double  w_explo_time               = 0.0;
    int32_t w_explo_time_              = 9;
    int32_t w_power_                   = 77;
    int32_t w_sound_target             = 555;
    uint8_t w_area_damage_owner_filter = 0x5A;
    int32_t w_bullet_anim              = 0x99;
    double  w_power[9]                 = {100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0, 108.0};
    double  w_fire_range[9]            = {200.0, 201.0, 202.0, 203.0, 204.0, 205.0, 206.0, 207.0, 208.0};

    // ---- homing target (units[homing_player][homing_unit]) ------------------------------------------
    double  target_energy    = 50.0;
    int32_t target_elevation = 0;

    // ---- callee-return knobs -------------------------------------------------------------------------
    double  mwd_dx_ret = 0.0, mwd_dy_ret = 0.0;
    int32_t row_ret      = 0;
    double  sqrt_ret     = 1.0;
    int32_t coords_x_ret = 0, coords_y_ret = 0;
    int32_t dir_ret    = 0;
    int32_t stride_ret = 0;

    // ---- geometry --------------------------------------------------------------------------------
    uint32_t bw_mask = 0x1fffu, bh_mask = 0x0fffu; // 8191 / 4095
    uint32_t big_width = 8192u, big_height = 4096u;

    // ---- globals ---------------------------------------------------------------------------------
    double  game_clock = 1.0;
    int32_t sim_active = 0;

    int32_t slot0_active_seed = 10; // only used when cur_slot != 0
};

void seed_fixture(sim_fixture &fx, const Seed &s) {
    fx.reset();

    fx.geom.bw_mask    = s.bw_mask;
    fx.geom.bh_mask    = s.bh_mask;
    fx.geom.big_width  = s.big_width;
    fx.geom.big_height = s.big_height;

    fx.game_clock = s.game_clock;
    fx.sim_active = s.sim_active;

    cfg_weapon &w              = fx.cfg_weapons[(size_t)s.weapon_id];
    w.type                     = s.w_type;
    w.homing                   = s.w_homing;
    w.speed                    = s.w_speed;
    w.length                   = s.w_length;
    w.target_explo             = s.w_target_explo;
    w.smoke_sprite             = s.w_smoke_sprite;
    w.smoke_time               = s.w_smoke_time;
    w.explo_time               = s.w_explo_time;
    w.explo_time_              = s.w_explo_time_;
    w.power_                   = s.w_power_;
    w.sound_target             = s.w_sound_target;
    w.area_damage_owner_filter = s.w_area_damage_owner_filter;
    w.bullet_anim              = s.w_bullet_anim;
    for (int i = 0; i < 9; ++i) {
        w.power[i]      = s.w_power[i];
        w.fire_range[i] = s.w_fire_range[i];
    }

    if (s.cur_slot != 0) {
        std::memset(&fx.projectile_pool[0], 0, sizeof(projectile));
        fx.projectile_pool[0].active = s.slot0_active_seed;
    }

    fx.cur_projectile_ptr = &fx.projectile_pool[(size_t)s.cur_slot];
    projectile &p         = fx.projectile_pool[(size_t)s.cur_slot];
    p.active              = s.active_seed;
    p.weapon_id           = s.weapon_id;
    p.src_x               = s.src_x;
    p.src_y_ground        = s.src_y_ground;
    p.src_y_vis           = s.src_y_vis;
    p.dst_x               = s.dst_x;
    p.dst_y_ground        = s.dst_y_ground;
    p.dst_y_vis           = s.dst_y_vis;
    p.owner_player        = s.owner_player;
    p.delta_x             = 0.0; // overwritten by map_wrapped_delta's knob before use
    p.delta_y             = 0.0;
    p.launch_time         = s.launch_time;
    p.duration            = s.duration_seed;
    p.anim_clock          = s.anim_clock;
    p.smoke_clock         = s.smoke_clock;
    p.explo_pulse_clock   = s.explo_pulse_clock;
    p.scatter_x           = s.scatter_x;
    p.scatter_y           = s.scatter_y;
    p.facing              = s.facing;
    p.anim_frame          = s.anim_frame;
    p.smoke_sprite        = 0; // unused by this function
    p.explo_pulse_flag    = s.explo_pulse_flag;
    p.anim_loop_frame     = s.anim_loop_frame;
    p.homing_player       = s.homing_player;
    p.homing_unit         = s.homing_unit;
    p.shooter_ref         = s.shooter_ref;
    p.shooter_unit        = s.shooter_unit;

    if (s.homing_unit != 0) {
        unit &t     = fx.u((int32_t)s.homing_player, s.homing_unit);
        t.energy    = s.target_energy;
        t.elevation = s.target_elevation;
    }

    g_mwd_dx_ret   = s.mwd_dx_ret;
    g_mwd_dy_ret   = s.mwd_dy_ret;
    g_row_ret      = s.row_ret;
    g_sqrt_ret     = s.sqrt_ret;
    g_coords_x_ret = s.coords_x_ret;
    g_coords_y_ret = s.coords_y_ret;
    g_dir_ret      = s.dir_ret;
    g_stride_ret   = s.stride_ret;
}

void run_tick(sim_fixture &fx) {
    reset_observations();
    sim_store own = fx.store();
    detail::projectile_tick(fx.view(), own, g_calls);
}

void seed_and_run(sim_fixture &fx, const Seed &s) {
    seed_fixture(fx, s);
    run_tick(fx);
}

} // namespace

void run_projectile_tick_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- Shape A (trunc_scaled_int), NEGATIVE fractional operand, ballistic branch, two weapon_ids
    // (confirming the dead-sentinel proof doesn't depend on weapon_id -- banner note 1).
    // 0x00440e83-0x00440e98 (scatter_x) / 0x00440ea0-0x00440eb5 (scatter_y): divide happens BEFORE the
    // multiply. elapsed=4.0, duration=8.0 (pre-tick seed) -> ratio=0.5 exactly.
    //   scatter_dx_frac = trunc(0.5 * -161) = trunc(-80.5) = -80  (toward zero; floor would give -81)
    //   scatter_dy_frac = trunc(0.5 *  161) = trunc( 80.5) =  80
    // Observed via the two map_wrapped_delta calls' dst_x/dst_y args (0x0044110e/0x00441135), which ARE
    // imp_x/imp_y_vis/imp_y_ground (0x004410c1-0x00441106: local impact tile = dst_* + scatter, masked;
    // ballistic does NOT write dst_* back, so this is the only way to observe it without impact firing).
    // =================================================================================================
    {
        Seed s;
        s.duration_seed = 8.0;
        s.game_clock    = 4.0; // launch_time=0.0 -> elapsed_since_launch = 4.0
        s.scatter_x     = -161;
        s.scatter_y     = 161;
        s.dst_x         = 500;
        s.dst_y_ground  = 600;
        s.dst_y_vis     = 650;
        for (int32_t wid : {0, 31}) {
            s.weapon_id = wid;
            seed_and_run(fx, s);
            ck(g_mwd_calls.size() == 2, "T1: exactly 2 map_wrapped_delta calls in the ballistic branch");
            if (g_mwd_calls.size() == 2) {
                const auto &c0 = g_mwd_calls[0]; // 0x00440fee: uses src_y_vis, writes p.delta_x/y
                ck(c0.src_x == 1000 && c0.src_y == 1100 && c0.dst_x == 420 && c0.dst_y == 730,
                   "T1: mwd#1(src_x, src_y_vis, imp_x=420, imp_y_vis=730) -- scatter -80/-> +80 applied "
                   "(weapon_id-independent, weapon_id in {0,31})");
                const auto &c1 = g_mwd_calls[1]; // 0x0044101f: uses src_y_ground, ephemeral ground delta
                ck(c1.src_x == 1000 && c1.src_y == 1200 && c1.dst_x == 420 && c1.dst_y == 680,
                   "T1: mwd#2(src_x, src_y_ground, imp_x=420, imp_y_ground=680)");
            }
        }
    }

    // =================================================================================================
    // T2 -- the homing_valid AND-gate: all three legs, each a boundary sense re-derived from the asm.
    //   0x00440ecd CMP homing_unit,0 / JZ    -> leg 1: homing_unit != 0
    //   0x00440edf CMP homing,0x1   / JZ     -> leg 2: homing == 1 EXACTLY (2 is NOT truthy here)
    //   0x00440f09 FCOMP 0.0,energy / JC     -> leg 3: 0.0 < energy STRICTLY (energy==0.0 fails)
    // Ballistic-taken is observed as: unit_get_coords NOT called, AND p.dst_x stays at its pre-tick
    // seed (999) since the ballistic branch never writes dst_x/dst_y_ground/dst_y_vis (only the homing
    // branch does, at 0x00440f4e/0x00440f6b/0x00440faa).
    // =================================================================================================
    {
        Seed s;
        s.homing_player = 2;
        s.dst_x         = 999; // ballistic-local sentinel that must survive untouched
        s.duration_seed = 1.0;
        s.sqrt_ret      = 1.0;
        s.w_speed       = 1.0;
        s.w_length      = 1;
        s.game_clock    = 1.0; // elapsed==duration==1.0 -> impact, irrelevant to this test's assertions

        // (a) homing_unit == 0
        s.homing_unit = 0;
        seed_and_run(fx, s);
        ck(g_coords_calls.empty(), "T2a: homing_unit==0 -> unit_get_coords NOT called");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].dst_x, 999u, "T2a: p.dst_x unchanged (ballistic)");

        // (b) homing_unit != 0, homing != 1 (default w_homing=0)
        s.homing_unit = 5;
        s.w_homing    = 0;
        seed_and_run(fx, s);
        ck(g_coords_calls.empty(), "T2b: homing!=1 -> unit_get_coords NOT called");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].dst_x, 999u, "T2b: p.dst_x unchanged (ballistic)");

        // (c) homing==1, energy == 0.0 exactly (boundary: strict > required)
        s.w_homing      = 1;
        s.target_energy = 0.0;
        seed_and_run(fx, s);
        ck(g_coords_calls.empty(), "T2c: energy==0.0 (not >0.0) -> unit_get_coords NOT called");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].dst_x, 999u, "T2c: p.dst_x unchanged (ballistic)");

        // (d) homing==1, energy tiny positive -> homing branch taken
        s.target_energy = 0.0001;
        s.coords_x_ret  = 700;
        s.scatter_x     = 0; // isolate: scatter_dx_frac == 0
        seed_and_run(fx, s);
        ck(g_coords_calls.size() == 1 && g_coords_calls[0].player == 2 && g_coords_calls[0].index == 5,
           "T2d: homing_valid -> unit_get_coords(homing_player, homing_unit) called once");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].dst_x, 700u,
              "T2d: p.dst_x = bw_mask & (coord_x + 0) = 700 (homing branch WROTE it)");

        // (e) homing == 2 (nonzero but not exactly 1) -> ballistic (equality sense, not truthiness)
        s.w_homing      = 2;
        s.target_energy = 50.0;
        seed_and_run(fx, s);
        ck(g_coords_calls.empty(), "T2e: homing==2 (!=1) -> unit_get_coords NOT called");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].dst_x, 999u, "T2e: p.dst_x unchanged (ballistic)");
    }

    // =================================================================================================
    // T3 -- homing branch scatter writes dst_x/dst_y_ground as PERMANENT struct fields (0x00440f4e /
    // 0x00440f6b), same -161/161 scatter values and 0.5 ratio as T1 (independently re-checked here since
    // the homing branch is a different code path at different addresses, 0x00440f19-0x00440fae).
    // =================================================================================================
    {
        Seed s;
        s.homing_player    = 2;
        s.homing_unit      = 5;
        s.w_homing         = 1;
        s.target_energy    = 50.0;
        s.target_elevation = 0; // isolate from T4's dst_y_vis derivation
        s.duration_seed    = 8.0;
        s.game_clock       = 4.0; // elapsed=4.0, ratio=0.5
        s.scatter_x        = -161;
        s.scatter_y        = 161;
        s.coords_x_ret     = 500;
        s.coords_y_ret     = 600;
        seed_and_run(fx, s);

        const projectile &p = fx.projectile_pool[s.cur_slot];
        ck_eq((uint32_t)p.dst_x, 420u, "T3: p.dst_x = bw_mask & (500 + trunc(0.5*-161)) = bw_mask&420 = 420");
        ck_eq((uint32_t)p.dst_y_ground, 680u,
              "T3: p.dst_y_ground = bh_mask & (600 + trunc(0.5*161)) = bh_mask&680 = 680");
        ck_eq((uint32_t)p.dst_y_vis, 680u, "T3: p.dst_y_vis = bh_mask & (680 - elevation(0)) = 680");
    }

    // =================================================================================================
    // T4 -- dst_y_vis partial-16-bit-read derivation (banner note 2). bh_mask is locally widened to
    // 0xffff (still a valid 16-bit-representable mask, so the asm's 16-bit MOV DX reads it whole) and
    // dst_y_ground(60000)-elevation(-10000) is driven to a raw magnitude of 70000, well past 65535, to
    // catch a bug in the SIGN/TRUNCATION handling rather than to test the (unconditionally-true) AND
    // identity itself -- see banner note 2 for why the identity can't fail regardless of magnitude.
    //   AX = (uint16)60000 - (uint16)(-10000 as int16) = 0xEA60 - 0xD8F0 = 0x1170 = 4464  (0x00440f95)
    //   DX = 0xffff & 0x1170 = 0x1170 = 4464                                              (0x00440fa3)
    // =================================================================================================
    {
        Seed s;
        s.homing_player    = 2;
        s.homing_unit      = 5;
        s.w_homing         = 1;
        s.target_energy    = 50.0;
        s.target_elevation = -10000;
        s.scatter_x        = 0;
        s.scatter_y        = 0; // isolate from the scatter mechanism (T1/T3 already cover it)
        s.bh_mask          = 0xffffu;
        s.coords_x_ret     = 100; // irrelevant to this derivation
        s.coords_y_ret     = 60000;
        seed_and_run(fx, s);

        const projectile &p = fx.projectile_pool[s.cur_slot];
        ck_eq((uint32_t)p.dst_y_ground, 60000u, "T4: p.dst_y_ground = 0xffff & 60000 = 60000 (unmasked)");
        ck_eq((uint32_t)p.dst_y_vis, 4464u,
              "T4: p.dst_y_vis = 0xffff & (60000 - sext(int16(-10000))) truncated to u16 = 4464 -- "
              "raw (unmasked) magnitude 70000 exceeds 16 bits, low-16 result still matches the "
              "asm's native 16-bit computation (0x00440f95/0x00440fa3)");
    }

    // =================================================================================================
    // T5 -- Shape C (trunc_mul_div) via flight interpolation, NEGATIVE delta_x, still-flying tick.
    // 0x0044137d (dx) / 0x004413c4 (dy): multiply happens BEFORE the divide. duration = sqrt_ret(10.0) *
    // speed(4.0) / length(5) = 8.0 exactly (0x00441054-0x00441094); elapsed=1.0 < 8.0 -> flight.
    //   dx_frac = trunc(-100.0 * 1.0 / 8.0) = trunc(-12.5) = -12   (toward zero; floor would give -13)
    //   dy_frac = trunc(  61.0 * 1.0 / 8.0) = trunc(  7.625) =  7
    //   p.x = (1000 + -12 + 8192) % 8192 = 9180 % 8192 = 988
    //   p.y = (1100 +   7 + 4096) % 4096 = 5203 % 4096 = 1107
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret   = 10.0;
        s.w_speed    = 4.0;
        s.w_length   = 5;
        s.game_clock = 1.0; // launch_time=0.0 -> elapsed=1.0 < duration=8.0
        s.mwd_dx_ret = -100.0;
        s.mwd_dy_ret = 61.0;
        seed_and_run(fx, s);

        const projectile &p = fx.projectile_pool[s.cur_slot];
        ck_eq_d(p.delta_x, -100.0, "T5: p.delta_x persisted from map_wrapped_delta's first call");
        ck_eq_d(p.delta_y, 61.0, "T5: p.delta_y persisted from map_wrapped_delta's first call");
        ck_eq_d(p.duration, 8.0, "T5: p.duration = sqrt_ret(10.0)*speed(4.0)/length(5) = 8.0 exactly");
        ck_eq((uint32_t)p.x, 988u,
              "T5: p.x = (src_x + trunc(-100*1/8)=-12 + big_width) % big_width = 988 -- toward-zero "
              "truncation; floor(-12.5)=-13 would give 987");
        ck_eq((uint32_t)p.y, 1107u, "T5: p.y = (src_y_vis + trunc(61*1/8)=7 + big_height) % big_height = 1107");
    }

    // =================================================================================================
    // T6 -- flight-vs-impact boundary: elapsed_since_launch == duration EXACTLY resolves to IMPACT, not
    // flight (0x004411f1 FCOMP elapsed,duration / 0x004411f7 JC flight -- JC fires only on STRICT <, so
    // equality falls through to the impact arm). Proven by giving the flight-formula a wildly different
    // outcome (mwd_dx_ret=999.0) than the impact formula (p.x=imp_x=500) so the two are unmistakable.
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret     = 5.0;
        s.w_speed      = 2.0;
        s.w_length     = 2;
        s.game_clock   = 5.0; // duration = 5.0*2.0/2.0 = 5.0; elapsed = 5.0 == duration
        s.dst_x        = 500;
        s.dst_y_ground = 600;
        s.dst_y_vis    = 650;
        s.mwd_dx_ret   = 999.0; // if flight ran instead: dx_frac=trunc(999*5/5)=999 -> p.x=1999, NOT 500
        seed_and_run(fx, s);

        const projectile &p = fx.projectile_pool[s.cur_slot];
        ck_eq_d(p.duration, 5.0, "T6: p.duration == elapsed_since_launch == 5.0 (the boundary)");
        ck_eq((uint32_t)p.x, 500u, "T6: equality -> IMPACT arm ran (p.x=imp_x=500, not the flight-formula's 1999)");
        ck_eq((uint32_t)p.y, 650u, "T6: p.y=imp_y_vis=650 (impact arm)");
    }

    // =================================================================================================
    // T7 -- Shape B (trunc_only) via the impact ring_count, NEGATIVE fractional fire_range value.
    // 0x004412df-0x004412ea: trunc(fire_range[idx]) alone (no multiply/divide). idx = shooter_ref(0x03)
    // & 0xf = 3 (in-bounds). fire_range[3] = -31.5 -> trunc = -31 (toward zero; floor would give -32).
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret        = 1.0;
        s.w_speed         = 1.0;
        s.w_length        = 1;
        s.game_clock      = 2.0; // elapsed(2.0)>=duration(1.0) -> impact
        s.shooter_ref     = 0x03;
        s.w_fire_range[3] = -31.5;
        seed_and_run(fx, s);

        ck(g_area_damage_calls.size() == 1, "T7: apply_area_damage called once (impact fired)");
        if (g_area_damage_calls.size() == 1) {
            ck_eq((uint32_t)g_area_damage_calls[0].ring, (uint32_t)-31,
                  "T7: ring_count = trunc(-31.5) = -31 (toward zero; floor would give -32)");
            ck_eq_d(g_area_damage_calls[0].dmg, 103.0, "T7: dmg = w.power[3] (in-bounds) = 103.0");
        }
    }

    // =================================================================================================
    // T8 -- the shooter_ref&0xf weapon-slot index: boundary between in-bounds (idx 8, last valid) and
    // the documented ORIGINAL out-of-bounds alias (idx 9 reads power[9], which per addr/mh_structs
    // .gen.h's byte-exact offsets (power@0x92 + 9*8 == 0xda == fire_range@0xda's own base) lands exactly
    // on fire_range[0] -- 0x00441312/0x00441318 push [weapon_base + idx*8 + 0x96]/[+0x92], the double
    // at power's base+idx*8, which for idx=9 IS fire_range[0]'s memory). This is the SIGNED/UNSIGNED-
    // adjacent "boundary sense" case the brief calls out: idx 8 must read cleanly, idx 9 must alias.
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret   = 1.0;
        s.w_speed    = 1.0;
        s.w_length   = 1;
        s.game_clock = 2.0; // force impact

        // idx == 8 (in-bounds, last valid power slot)
        s.shooter_ref = 0x18; // & 0xf == 8
        seed_and_run(fx, s);
        ck(g_area_damage_calls.size() == 1 && g_area_damage_calls[0].dmg == 108.0,
           "T8a: idx=8 (in-bounds) -> dmg = w.power[8] = 108.0");

        // idx == 9 (OOB by one -> aliases fire_range[0], NOT power's own storage)
        s.shooter_ref     = 0x19;  // & 0xf == 9
        s.w_fire_range[0] = 44.25; // distinct sentinel at the aliased address; power[9] itself is never set
        seed_and_run(fx, s);
        ck(g_area_damage_calls.size() == 1 && g_area_damage_calls[0].dmg == 44.25,
           "T8b: idx=9 (ORIGINAL out-of-bounds) -> dmg = fire_range[0] = 44.25, reproducing the aliasing "
           "read exactly (a bounds-clamped translation would read power[8]=108.0 or crash instead)");
    }

    // =================================================================================================
    // T9 -- impact arm, SIM_ACTIVE=1: full call order + argument tuples for the whole impact sequence.
    // Order per 0x00440fee..0x00441465: map_wrapped_delta x2, map_wrap_delta_row, sqrt, fx_anim_spawn
    // (impact, 0x0044122b), [SIM_ACTIVE: snd_play_at -- the original offscreen_snd_volume
    // 0x00441279 + snd_play 0x00441292 pair, one record since the LIFT-NOTIFY offscreen conversion],
    // apply_area_damage (0x0044134d), dir_from_to (0x00441414), fx_anim_dir_frame_stride (0x00441439).
    // =================================================================================================
    {
        Seed s;
        s.cur_slot                   = 5;
        s.slot0_active_seed          = 10;
        s.weapon_id                  = 3;
        s.w_type                     = 1; // != 8 -> recompute_facing fires
        s.sqrt_ret                   = 6.0;
        s.w_speed                    = 2.0;
        s.w_length                   = 4;   // duration = 6.0*2.0/4.0 = 3.0
        s.game_clock                 = 5.0; // elapsed=5.0 >= 3.0 -> impact
        s.sim_active                 = 1;
        s.shooter_ref                = 0x02; // idx=2
        s.w_power[2]                 = 33.5;
        s.w_fire_range[2]            = 12.75; // ring_count = trunc(12.75) = 12
        s.w_area_damage_owner_filter = 0x5A;
        s.dir_ret                    = 15; // old facing=5 -> delta_facing=10
        s.stride_ret                 = 3;
        seed_and_run(fx, s);

        ck(trace_eq({"map_wrapped_delta", "map_wrapped_delta", "map_wrap_delta_row", "sqrt",
                     "fx_anim_spawn", "snd_play_at", "apply_area_damage",
                     "dir_from_to", "fx_anim_dir_frame_stride"}),
           "T9: exact call order for a ballistic impact tick with SIM_ACTIVE=1 and recompute_facing");

        ck(g_anim_spawn_calls.size() == 1, "T9: fx_anim_spawn called once (impact)");
        if (g_anim_spawn_calls.size() == 1) {
            const auto &a = g_anim_spawn_calls[0];
            ck(a.x == 2000 && a.y == 2050 && a.anim_id == 777 && a.owner_or_flag == 4,
               "T9: fx_anim_spawn(imp_x=2000, imp_y_vis=2050, w.target_explo=777, owner_player=4)");
            ck_eq_d(a.elapsed, 5.0, "T9: fx_anim_spawn elapsed = game_clock = 5.0");
        }
        ck(g_snd_play_at_calls.size() == 1 && g_snd_play_at_calls[0].id == 555 &&
               g_snd_play_at_calls[0].col == 62 && g_snd_play_at_calls[0].row == 65,
           "T9: snd_play_at(w.sound_target=555, fine_to_tile(2000)=62, fine_to_tile(2100)=65)");

        ck(g_area_damage_calls.size() == 1, "T9: apply_area_damage called once (impact)");
        if (g_area_damage_calls.size() == 1) {
            const auto &d = g_area_damage_calls[0];
            ck(d.x == 62 && d.y == 65 && d.kind == 4 && d.ring == 12 && d.owner == 0x5Au &&
                   d.killer_info == 2u && d.killer_idx == 99,
               "T9: apply_area_damage(tile62,65, kind=owner_player=4, ring=12, owner_filter=0x5A, "
               "killer_info=shooter_ref=2, killer_idx=shooter_unit=99)");
            ck_eq_d(d.dmg, 33.5, "T9: apply_area_damage damage = w.power[2] = 33.5");
        }

        const projectile &p = fx.projectile_pool[5];
        ck_eq((uint32_t)p.x, 2000u, "T9: p.x = imp_x = 2000");
        ck_eq((uint32_t)p.y, 2050u, "T9: p.y = imp_y_vis = 2050");
        ck_eq((uint32_t)p.active, 0u, "T9: p.active = 0 (this slot died)");
        ck_eq((uint32_t)fx.projectile_pool[0].active, 9u,
              "T9: projectile_pool[0].active decremented 10 -> 9 (live-count header, slot 0 != cur slot 5)");

        ck(g_dir_calls.size() == 1 && g_dir_calls[0].x1 == 2000 && g_dir_calls[0].y1 == 2050 &&
               g_dir_calls[0].x2 == 2000 && g_dir_calls[0].y2 == 2050,
           "T9: dir_from_to(p.x,p.y,imp_x,imp_y_vis) -- identical points on the impact arm since p.x/y "
           "were JUST set to imp_x/imp_y_vis");
        ck_eq((uint32_t)p.facing, 15u, "T9: p.facing = dir_from_to's return = 15");
        ck(g_stride_calls.size() == 1 && g_stride_calls[0] == 0x99, "T9: fx_anim_dir_frame_stride(w.bullet_anim=0x99)");
        ck_eq((uint32_t)p.anim_loop_frame, 80u, "T9: p.anim_loop_frame = 50 + (15-5)*3 = 80");
        ck_eq((uint32_t)p.anim_frame, 40u, "T9: p.anim_frame = 10 + (15-5)*3 = 40");
    }

    // =================================================================================================
    // T10 -- same impact tick as T9 but SIM_ACTIVE=0: snd_play_at does not fire, and it is absent
    // from the trace entirely (not merely uncalled -- ORDER around it collapses).
    // =================================================================================================
    {
        Seed s;
        s.cur_slot   = 5;
        s.weapon_id  = 3;
        s.w_type     = 1;
        s.sqrt_ret   = 6.0;
        s.w_speed    = 2.0;
        s.w_length   = 4;
        s.game_clock = 5.0;
        s.sim_active = 0;
        s.dir_ret    = 15;
        s.stride_ret = 3;
        seed_and_run(fx, s);

        ck(trace_eq({"map_wrapped_delta", "map_wrapped_delta", "map_wrap_delta_row", "sqrt",
                     "fx_anim_spawn", "apply_area_damage", "dir_from_to", "fx_anim_dir_frame_stride"}),
           "T10: SIM_ACTIVE=0 -- snd_play_at absent from the trace (0x0044124a JZ)");
        ck(g_snd_play_at_calls.empty(), "T10: no sound record recorded");
    }

    // =================================================================================================
    // T11 -- ballistic facing-recompute type gate: w.type==8 skips it (0x004411d9 CMP type,0x8 / JZ);
    // any other type takes it. dir_from_to/fx_anim_dir_frame_stride are the tell (they're the ONLY
    // callees inside the recompute_facing block).
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret   = 1.0;
        s.w_speed    = 1.0;
        s.w_length   = 1;
        s.game_clock = 0.5; // elapsed(0.5)<duration(1.0) -> flight

        s.w_type = 8;
        seed_and_run(fx, s);
        ck(g_dir_calls.empty() && g_stride_calls.empty(), "T11a: w.type==8 -> recompute_facing NOT taken");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].facing, 5u, "T11a: p.facing unchanged (seed=5)");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].anim_frame, 10u, "T11a: p.anim_frame unchanged (seed=10)");

        s.w_type     = 1;
        s.dir_ret    = 77;
        s.stride_ret = 4;
        seed_and_run(fx, s);
        ck(g_dir_calls.size() == 1 && g_stride_calls.size() == 1, "T11b: w.type!=8 -> recompute_facing taken");
        ck_eq((uint32_t)fx.projectile_pool[s.cur_slot].facing, 77u, "T11b: p.facing = dir_from_to's return");
    }

    // =================================================================================================
    // T12 -- homing branch ALWAYS recomputes facing regardless of w.type (0x00441097: unconditional MOV
    // local_34,1, no type check -- contrast with T11's ballistic-only gate).
    // =================================================================================================
    {
        Seed s;
        s.homing_player = 2;
        s.homing_unit   = 5;
        s.w_homing      = 1;
        s.target_energy = 50.0;
        s.w_type        = 8; // would have SKIPPED recompute on the ballistic path (T11a)
        s.sqrt_ret      = 1.0;
        s.w_speed       = 1.0;
        s.w_length      = 1;
        s.game_clock    = 0.5; // flight
        seed_and_run(fx, s);
        ck(g_dir_calls.size() == 1, "T12: homing branch recomputes facing even though w.type==8");
    }

    // =================================================================================================
    // T13 -- facing-advance arithmetic (0x00441419-0x00441465), forced FLIGHT with a zero delta so
    // p.x/p.y stay at src_x/src_y_vis and dir_from_to's argument tuple is easy to hand-check.
    //   delta_facing = new_facing(15) - old_facing(5) = 10
    //   p.anim_loop_frame = 50 + 10*3 = 80 ; p.anim_frame = 10 + 10*3 = 40 ; p.facing = 15
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret   = 100.0;
        s.w_speed    = 1.0;
        s.w_length   = 1;
        s.game_clock = 1.0; // duration=100, elapsed=1 -> flight
        s.mwd_dx_ret = 0.0;
        s.mwd_dy_ret = 0.0;
        s.dir_ret    = 15;
        s.stride_ret = 3;
        seed_and_run(fx, s);

        const projectile &p = fx.projectile_pool[s.cur_slot];
        ck_eq((uint32_t)p.x, 1000u, "T13: p.x = src_x (zero delta)");
        ck_eq((uint32_t)p.y, 1100u, "T13: p.y = src_y_vis (zero delta)");
        ck(g_dir_calls.size() == 1 && g_dir_calls[0].x1 == 1000 && g_dir_calls[0].y1 == 1100 &&
               g_dir_calls[0].x2 == 2000 && g_dir_calls[0].y2 == 2050,
           "T13: dir_from_to(p.x=1000,p.y=1100, imp_x=2000,imp_y_vis=2050) -- ballistic locals unaffected by flight");
        ck_eq((uint32_t)p.facing, 15u, "T13: p.facing = new_facing = 15");
        ck_eq((uint32_t)p.anim_loop_frame, 80u, "T13: p.anim_loop_frame = 50 + (15-5)*3 = 80");
        ck_eq((uint32_t)p.anim_frame, 40u, "T13: p.anim_frame = 10 + (15-5)*3 = 40");
    }

    // =================================================================================================
    // T14 -- anim-advance loop, all three arms (0x00441486-0x00441539). w.type=8 disables the facing
    // recompute (T11) so p.anim_frame==10 is exactly what the loop's `frame0` reads. game_clock=50.0,
    // anim_clock(seed)=48.0 -> time_left=2.0; anim_frames[11].time=2.0 -> `time_step<=time_left` fires
    // exactly once (0x004414c6-0x004414ef).
    // =================================================================================================
    {
        Seed s;
        s.w_type        = 8; // disable facing recompute (irrelevant to this loop, avoids interference)
        s.sqrt_ret      = 1.0;
        s.w_speed       = 1.0;
        s.w_length      = 1;
        s.game_clock    = 50.0;
        s.duration_seed = 1.0;
        // force flight so the impact arm doesn't also fire: elapsed(50)-vs-duration -- irrelevant here
        // since duration is recomputed to 1.0 (sqrt_ret*speed/length) regardless, making elapsed(50)>=1
        // -> IMPACT. Impact doesn't touch anim_clock/anim_frame, so it's harmless noise for this test.
        s.anim_clock      = 48.0;
        s.anim_loop_frame = 99;

        // (a) time_step == 0 (anim_frames zeroed by fx.reset()) -> whole block skipped
        seed_and_run(fx, s);
        {
            const projectile &p = fx.projectile_pool[s.cur_slot];
            ck_eq_d(p.anim_clock, 48.0, "T14a: time_step==0 -> anim_clock UNCHANGED (block skipped)");
            ck_eq((uint32_t)p.anim_frame, 10u, "T14a: anim_frame UNCHANGED");
        }

        // (b) time_step != 0, next == 0 -> wrap to anim_loop_frame. NOTE: anim_frames[11] must be set
        // AFTER seed_fixture's internal fx.reset() (which zeroes the whole vector) and BEFORE run_tick
        // -- seed_and_run's reset would otherwise wipe it before the tick ever sees it.
        seed_fixture(fx, s);
        fx.anim_frames[11].time = 2.0;
        fx.anim_frames[11].next = 0;
        run_tick(fx);
        {
            const projectile &p = fx.projectile_pool[s.cur_slot];
            ck_eq_d(p.anim_clock, 50.0, "T14b: anim_clock = game_clock (set once before the loop)");
            ck_eq((uint32_t)p.anim_frame, 99u, "T14b: next==0 -> anim_frame = anim_loop_frame (99)");
        }

        // (c) time_step != 0, next == 7 -> advance by next
        seed_fixture(fx, s);
        fx.anim_frames[11].time = 2.0;
        fx.anim_frames[11].next = 7;
        run_tick(fx);
        {
            const projectile &p = fx.projectile_pool[s.cur_slot];
            ck_eq_d(p.anim_clock, 50.0, "T14c: anim_clock = game_clock");
            ck_eq((uint32_t)p.anim_frame, 17u, "T14c: next==7 -> anim_frame = 10 + 7 = 17");
        }
    }

    // =================================================================================================
    // T15 -- smoke-puff loop (0x0044154a-0x004416ce), gated on w.smoke_sprite!=0 AND w.smoke_time!=0,
    // tiled off src_y_VIS (0x004416e2/0x00441643 read offset 0x10 = src_y_vis). Forced still-flying
    // (duration=100 > elapsed=50) so the impact arm doesn't add fx_anim_spawn noise. delta_y is NEGATIVE
    // to reinforce Shape C's toward-zero truncation at this INDEPENDENT call site (0x00441636/0x00441672).
    //   puff_dt = smoke_clock(new=50.0) - launch_time(0.0) = 50.0
    //   dy_frac = trunc(-41.0 * 50.0 / 100.0) = trunc(-20.5) = -20
    //   dx_frac = trunc(  61.0 * 50.0 / 100.0) = trunc( 30.5) =  30
    //   tile_y = (4096 + src_y_vis(1100) + -20) % 4096 = 5176 % 4096 = 1080
    //   tile_x = (8192 + src_x(1000)     +  30) % 8192 = 9222 % 8192 = 1030
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret   = 10.0;
        s.w_speed    = 10.0;
        s.w_length   = 1;    // duration = 10*10/1 = 100.0
        s.game_clock = 50.0; // elapsed = 50.0 < 100.0 -> flight
        s.mwd_dx_ret = 61.0;
        s.mwd_dy_ret = -41.0;

        // (a) smoke_sprite == 0 -> loop entirely skipped
        s.w_smoke_sprite = 0;
        seed_and_run(fx, s);
        ck(g_anim_spawn_calls.empty(), "T15a: smoke_sprite==0 -> no fx_anim_spawn at all (flight tick)");

        // (b) smoke_sprite != 0, smoke_time == 0 -> loop still entirely skipped (outer gate)
        s.w_smoke_sprite = 42;
        s.w_smoke_time   = 0.0;
        seed_and_run(fx, s);
        ck(g_anim_spawn_calls.empty(), "T15b: smoke_time==0 -> smoke loop skipped (0x00441590 JNZ / 0x00441596 JZ)");

        // (c) smoke_sprite != 0, smoke_time != 0, timed for exactly one puff
        s.w_smoke_time = 3.0;
        s.smoke_clock  = 47.0; // time_left = 50.0-47.0 = 3.0 == smoke_period -> fires once, then time_left=0
        seed_and_run(fx, s);
        ck(g_anim_spawn_calls.size() == 1, "T15c: exactly one smoke puff fx_anim_spawn call");
        if (g_anim_spawn_calls.size() == 1) {
            const auto &a = g_anim_spawn_calls[0];
            ck(a.x == 1030 && a.y == 1080 && a.anim_id == 42 && a.owner_or_flag == 1,
               "T15c: fx_anim_spawn(tile_x=1030, tile_y=1080, w.smoke_sprite=42, owner_or_flag=1 literal) "
               "-- tile_y derived from src_y_VIS(1100), toward-zero trunc(-20.5)=-20");
            ck_eq_d(a.elapsed, 50.0, "T15c: fx_anim_spawn elapsed = p.smoke_clock (new) = 50.0");
        }
        ck_eq_d(fx.projectile_pool[s.cur_slot].smoke_clock, 50.0, "T15c: p.smoke_clock advanced 47.0+3.0=50.0");
    }

    // =================================================================================================
    // T16 -- explo-pulse loop (0x004416ce-0x0044189b), gated on p.explo_pulse_flag!=0, tiled off
    // src_y_GROUND (0x004417f3 reads offset 0xe = src_y_ground) -- DELIBERATELY chosen distinct from
    // T15's src_y_vis(1100) so a copy-paste-from-smoke bug (using src_y_vis here too) is caught: with
    // src_y_ground=1200 the two tile_y values (1180 vs T15's 1080) are unmistakably different.
    //   pulse_dt = explo_pulse_clock(new=50.0) - launch_time(0.0) = 50.0 (same numbers as T15's puff_dt,
    //   deliberately -- isolates the FIELD CHOICE as the only variable between the two tests)
    //   tile_y = (4096 + src_y_ground(1200) + -20) % 4096 = 5276 % 4096 = 1180
    //   tile_x = (8192 + src_x(1000)        +  30) % 8192 = 9222 % 8192 = 1030
    // =================================================================================================
    {
        Seed s;
        s.sqrt_ret     = 10.0;
        s.w_speed      = 10.0;
        s.w_length     = 1;    // duration = 100.0
        s.game_clock   = 50.0; // flight
        s.mwd_dx_ret   = 61.0;
        s.mwd_dy_ret   = -41.0;
        s.src_y_ground = 1200; // distinct from src_y_vis(1100), see banner above

        // (a) explo_pulse_flag == 0 -> loop entirely skipped
        s.explo_pulse_flag = 0;
        seed_and_run(fx, s);
        ck(g_area_damage_calls.empty(), "T16a: explo_pulse_flag==0 -> no apply_area_damage at all (flight tick)");

        // (b) explo_pulse_flag != 0, timed for exactly one pulse
        s.explo_pulse_flag           = 1;
        s.w_explo_time               = 5.0;
        s.explo_pulse_clock          = 45.0; // time_left = 50.0-45.0 = 5.0 == explo_period -> fires once
        s.w_power_                   = 77;
        s.w_explo_time_              = 9;
        s.w_area_damage_owner_filter = 0x5A;
        s.shooter_ref                = 0x02;
        s.shooter_unit               = 99;
        s.owner_player               = 4;
        seed_and_run(fx, s);
        ck(g_area_damage_calls.size() == 1, "T16b: exactly one explo-pulse apply_area_damage call");
        if (g_area_damage_calls.size() == 1) {
            const auto &d = g_area_damage_calls[0];
            ck(d.x == 32 && d.y == 36 && d.kind == 4 && d.ring == 9 && d.owner == 0x5Au &&
                   d.killer_info == 2u && d.killer_idx == 99,
               "T16b: apply_area_damage(fine_to_tile(1030)=32, fine_to_tile(1180)=36, owner_player=4, "
               "ring=w.explo_time_=9 (a plain int field, NOT trunc'd), owner_filter=0x5A, "
               "killer_info=shooter_ref=2, killer_idx=shooter_unit=99) -- y uses src_y_GROUND, not vis");
            ck_eq_d(d.dmg, 77.0, "T16b: dmg = (double)w.power_ = 77.0");
        }
        ck_eq_d(fx.projectile_pool[s.cur_slot].explo_pulse_clock, 50.0,
                "T16b: p.explo_pulse_clock advanced 45.0+5.0=50.0");
    }

    // =================================================================================================
    // T17 -- projectile-pool slot bookkeeping (0x00441352-0x0044135d): p.active=0 then a SEPARATE
    // decrement of projectile_pool[0].active, both fired on impact. Two sub-cases:
    //   (a) cur slot != 0 -- adjacent slots (cur-1, cur+1) must be byte-for-byte untouched, and slot 0's
    //       counter is independent bookkeeping (10 -> 9), not aliased with cur's own p.active (0).
    //   (b) cur slot == 0 -- p.active AND projectile_pool[0].active are the SAME memory: the asm does
    //       "MOV [slot0],0" THEN "DEC [slot0]" on that same address in that order, so the final value is
    //       -1 (0-1), NOT (old_active - 1). A translation that "optimized" to compute count-1 from the
    //       PRE-tick active value would get a different, wrong answer here.
    // =================================================================================================
    {
        // (a) cur slot != 0, neighbors poisoned to prove non-interference.
        Seed s;
        s.cur_slot          = 5;
        s.slot0_active_seed = 10;
        s.sqrt_ret          = 1.0;
        s.w_speed           = 1.0;
        s.w_length          = 1;
        s.game_clock        = 2.0; // force impact
        seed_fixture(fx, s);
        std::memset(&fx.projectile_pool[4], 0xAB, sizeof(projectile));
        std::memset(&fx.projectile_pool[6], 0xAB, sizeof(projectile));
        projectile expect4, expect6;
        std::memcpy(&expect4, &fx.projectile_pool[4], sizeof(projectile));
        std::memcpy(&expect6, &fx.projectile_pool[6], sizeof(projectile));
        run_tick(fx);

        ck(std::memcmp(&fx.projectile_pool[4], &expect4, sizeof(projectile)) == 0,
           "T17a: neighbor slot cur-1 (index 4) byte-for-byte untouched");
        ck(std::memcmp(&fx.projectile_pool[6], &expect6, sizeof(projectile)) == 0,
           "T17a: neighbor slot cur+1 (index 6) byte-for-byte untouched");
        ck_eq((uint32_t)fx.projectile_pool[5].active, 0u, "T17a: cur slot (5) p.active = 0");
        ck_eq((uint32_t)fx.projectile_pool[0].active, 9u,
              "T17a: slot 0's live-count header independently decremented 10 -> 9");

        // (b) cur slot == 0 -- the aliasing edge case.
        Seed s0;
        s0.cur_slot    = 0;
        s0.active_seed = 7; // pre-tick value; irrelevant to the final result (overwritten before the DEC reads it)
        s0.sqrt_ret    = 1.0;
        s0.w_speed     = 1.0;
        s0.w_length    = 1;
        s0.game_clock  = 2.0; // force impact
        seed_and_run(fx, s0);
        ck_eq((uint32_t)fx.projectile_pool[0].active, (uint32_t)-1,
              "T17b: cur==slot0 -- p.active=0 THEN projectile_pool[0].active-=1 on the SAME memory -> "
              "final value -1, NOT active_seed(7)-1=6 (which a count-from-pre-tick-value bug would give)");
    }
}

} // namespace mh::sim::test
