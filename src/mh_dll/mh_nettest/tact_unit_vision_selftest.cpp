//
// tact_unit_vision_selftest.cpp -- offline oracle for llm_tact_unit_vision_add/_remove (TACT1D,
// 2026-08-27). See tact/tact_unit_vision.h for the derivation.
//
// WHY THIS ORACLE EXISTS EVEN THOUGH THE FUNCTIONS ARE proof:RIG: the fog-of-war write these two
// functions exist to perform (tile_objects[...].visibility/.flags via a FOLDED -31/-31 index bias)
// is INVISIBLE to tmp/state_matrix.json's write-attribution scanner and therefore to
// the measured shadow region closure. A rig arm built from the closure's 18 declared
// regions would report "N calls, 0 divergences" while never once comparing the bytes that matter.
// This oracle is the load-bearing proof of the actual mechanic; a rig arm (if run) only adds T2
// evidence for the FOV-scratch chain these functions also touch through vision_cone_setup.
//
#include "tact/tact_unit_vision.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

// Both mocks are NO-OPS: every case pre-seeds fx.fov_stencil directly, so the raycaster's own
// correctness is out of scope here (it is covered by tact_fov's own oracle/rig evidence) and the
// call count is what proves unit_vision_add/remove reached the setup step at all.
struct call_log {
    int vision_cone_setup_calls   = 0;
    int fov_raycast_stencil_calls = 0;
};

call_log &log() {
    static call_log l;
    return l;
}

void reset_log() { log() = call_log{}; }

void mock_vision_cone_setup(int32_t, int32_t, int32_t, uint32_t, int32_t, void *, void *, void *,
                            void *) {
    ++log().vision_cone_setup_calls;
}

void mock_fov_raycast_stencil() { ++log().fov_raycast_stencil_calls; }

unit_vision_calls mock_calls() { return {mock_vision_cone_setup, mock_fov_raycast_stencil}; }

// Stencil index = j*64 + i (j = inner/"row" multiplier, i = outer/"col" adder) -- matches
// tact_store::fov_stencil_at's own comment and tact_unit_vision.h's derivation.
int32_t stencil_index(int32_t j, int32_t i) { return j * 64 + i; }

// A distinctive, non-{0,1} sentinel so an untouched fog-record is observably different from both
// "never written" (0) and "written once" (a small int) -- fixture-rule discipline.
constexpr uint8_t POISON_VIS   = 200;
constexpr uint8_t POISON_FLAGS = 0x00;

void poison_all(tact_fixture &fx) {
    for (auto &t : fx.tile_objects) {
        t.visibility = POISON_VIS;
        t.flags[1]   = POISON_FLAGS;
        t.flags[0]   = 0x11; // distinct low byte -- must survive untouched everywhere (RMW targets
                             // only flags[1] per the header's byte-op derivation)
    }
}

} // namespace

void run_unit_vision_tests() {
    // T1: unit_vision_add, owner != 1 (guard does not apply regardless of see_enemy_flag) -- two
    // stencil hits at DIFFERENT (j,i) land on the two DIFFERENT tile_objects cells the -31/-31 bias
    // predicts, and increment/re-explore ONLY those two.
    {
        tact_fixture fx;
        poison_all(fx);
        tact_unit &u      = fx.units[5];
        u.pos_col         = 50;
        u.pos_row         = 60;
        u.owner           = 2; // not 1 -- guard passes unconditionally
        u.facing_dir      = 1;
        u.vision_angle    = 10;
        u.vision_dist     = 3;
        fx.see_enemy_flag = 0;

        // center cell: j=31 (-> x = 50+31-31 = 50), i=31 (-> y = 60+31-31 = 60)
        fx.fov_stencil[(size_t)stencil_index(31, 31)] = 1;
        // an off-center cell: j=35 (-> x = 50+35-31 = 54), i=20 (-> y = 60+20-31 = 49)
        fx.fov_stencil[(size_t)stencil_index(35, 20)] = 1;

        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        reset_log();
        detail::unit_vision_add(own, p, mock_calls(), 5);

        ck_eq(log().vision_cone_setup_calls, 1, "T1: vision_cone_setup called exactly once, 0x0042e439");
        ck_eq(p.tile_object_at(50, 60).visibility, POISON_VIS + 1,
              "T1: center cell (j=31,i=31 -> x=50,y=60) refcount++, 0x0042e4a1");
        ck_eq(p.tile_object_at(50, 60).flags[1], 0x80,
              "T1: center cell FOGGED cleared + EXPLORED set, 0x0042e4c5/0x0042e508");
        ck_eq(p.tile_object_at(54, 49).visibility, POISON_VIS + 1,
              "T1: off-center cell (j=35,i=20 -> x=54,y=49) via the SAME -31/-31 bias, 0x0042e4a1");
        ck_eq(p.tile_object_at(54, 49).flags[1], 0x80, "T1: off-center cell fog bits");
        // A cell NEVER hit in the stencil must stay at the poison sentinel -- proves this is not a
        // blanket 64x64 sweep of every tile, only stencil-nonzero ones.
        ck_eq(p.tile_object_at(51, 61).visibility, POISON_VIS,
              "T1: an unhit cell is untouched (not a blanket sweep)");
        ck_eq(p.tile_object_at(51, 61).flags[1], POISON_FLAGS, "T1: an unhit cell's flags untouched");
        // flags[0] must survive every RMW here untouched (the .asm loads BX whole but only ANDs/ORs
        // BH -- BL round-trips unchanged).
        ck_eq(p.tile_object_at(50, 60).flags[0], 0x11, "T1: flags[0] (BL) untouched by the RMW");
    }

    // T2: unit_vision_add, owner == 1 AND see_enemy_flag == 0 -- must be a COMPLETE no-op (not even
    // vision_cone_setup is called).
    {
        tact_fixture fx;
        poison_all(fx);
        tact_unit &u                                  = fx.units[5];
        u.pos_col                                     = 50;
        u.pos_row                                     = 60;
        u.owner                                       = 1;
        u.facing_dir                                  = 1;
        u.vision_angle                                = 10;
        u.vision_dist                                 = 3;
        fx.see_enemy_flag                             = 0;
        fx.fov_stencil[(size_t)stencil_index(31, 31)] = 1; // would hit if the guard were bypassed

        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        reset_log();
        detail::unit_vision_add(own, p, mock_calls(), 5);

        ck_eq(log().vision_cone_setup_calls, 0, "T2: owner==1 && !see_enemy_flag -> early return, 0x0042e3bb");
        ck_eq(p.tile_object_at(50, 60).visibility, POISON_VIS, "T2: no write at all when guarded");
    }

    // T2b: unit_vision_add, owner == 1 BUT see_enemy_flag == 1 -- the guard's OTHER escape: proceeds
    // despite owner==1.
    {
        tact_fixture fx;
        poison_all(fx);
        tact_unit &u                                  = fx.units[5];
        u.pos_col                                     = 50;
        u.pos_row                                     = 60;
        u.owner                                       = 1;
        u.facing_dir                                  = 1;
        u.vision_angle                                = 10;
        u.vision_dist                                 = 3;
        fx.see_enemy_flag                             = 1;
        fx.fov_stencil[(size_t)stencil_index(31, 31)] = 1;

        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        reset_log();
        detail::unit_vision_add(own, p, mock_calls(), 5);

        ck_eq(log().vision_cone_setup_calls, 1,
              "T2b: owner==1 but see_enemy_flag==1 -> proceeds, 0x0042e3b0-0x0042e3bb");
        ck_eq(p.tile_object_at(50, 60).visibility, POISON_VIS + 1, "T2b: write happens under the override");
    }

    // T3: unit_vision_remove, refcount > 1 after the decrement -- fog bits must NOT change (the
    // check is POST-decrement equality to exactly 0).
    {
        tact_fixture fx;
        poison_all(fx);
        tact_unit &u                                         = fx.units[7];
        u.pos_col                                            = 80;
        u.pos_row                                            = 90;
        u.owner                                              = 2;
        u.facing_dir                                         = 1;
        u.vision_angle                                       = 10;
        u.vision_dist                                        = 3;
        fx.see_enemy_flag                                    = 0;
        fx.fov_stencil[(size_t)stencil_index(31, 31)]        = 1;
        fx.tile_objects[(size_t)((80 << 8) | 90)].visibility = 3; // will decrement to 2, not 0

        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        reset_log();
        detail::unit_vision_remove(own, p, mock_calls(), 7);

        ck_eq(p.tile_object_at(80, 90).visibility, 2, "T3: refcount-- from 3 to 2, 0x0042e65b");
        ck_eq(p.tile_object_at(80, 90).flags[1], POISON_FLAGS,
              "T3: refcount != 0 after decrement -> fog bits untouched (post-decrement test), 0x0042e67a");
    }

    // T4: unit_vision_remove, refcount hits EXACTLY 0 -- both fog bits set (0xc0 on the high byte).
    {
        tact_fixture fx;
        poison_all(fx);
        tact_unit &u                                         = fx.units[7];
        u.pos_col                                            = 80;
        u.pos_row                                            = 90;
        u.owner                                              = 2;
        u.facing_dir                                         = 1;
        u.vision_angle                                       = 10;
        u.vision_dist                                        = 3;
        fx.see_enemy_flag                                    = 0;
        fx.fov_stencil[(size_t)stencil_index(31, 31)]        = 1;
        fx.tile_objects[(size_t)((80 << 8) | 90)].visibility = 1; // decrements to exactly 0

        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        reset_log();
        detail::unit_vision_remove(own, p, mock_calls(), 7);

        ck_eq(p.tile_object_at(80, 90).visibility, 0, "T4: refcount-- from 1 to 0, 0x0042e65b");
        ck_eq(p.tile_object_at(80, 90).flags[1], 0xc0,
              "T4: refcount==0 -> FOGGED+EXPLORED both set, 0x0042e6a1 (post-decrement test true)");
    }
}

} // namespace mh::tact::test
