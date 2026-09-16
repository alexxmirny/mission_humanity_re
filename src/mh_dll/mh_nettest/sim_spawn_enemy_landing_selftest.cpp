//
// sim_spawn_enemy_landing_selftest.cpp -- `simtest` offline oracle for llm_strat_spawn_enemy_landing
// (sim/sim_landing_spot.h/.cpp, RI-SIM / SIM1F). spawn_enemy_landing calls llm_strat_claim_landing_spot
// DIRECTLY, in-TU (not through `landing_spot_calls`), so a "claim succeeds" case here genuinely runs
// claim_landing_spot's own body too -- that is inherent to the two functions' translation shape (same
// posture as sim_invasion.h's chance_roll/handle_invasion pairing), driven here via the SAME
// `landing_spot_calls` (rand_below, set_landing_site) claim_landing_spot itself takes. The two callees
// genuinely external to BOTH functions (count_landing_spots, spawn_invasion_force) are recording stubs.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_strat_spawn_enemy_landing_
// 004998ae.asm, tmp/decomp/llm_strat_claim_landing_spot_00454eb2.asm), not the .cpp -- see
// sim_landing_spot.h's banner for the address-by-address pseudocode.
//
// _G_LLM_STRAT_PLAYERS (sim_view::profiles / sim_store::profile_at) is written here on the
// camera-fallback path (E3) -- this is the "writes _G_LLM_STRAT_PLAYERS" effect the task brief calls
// out; spawn_invasion_force is the one outward call recorded via the stub.
//
#include "sim/sim_landing_spot.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// TU-local literal operands -- NOT exported by the header (sim_landing_spot.cpp keeps them anonymous-
// namespace-local to avoid the cross-TU redefinition collision its own banner documents), so this test
// reproduces the SAME values, cross-checked against that banner and against
// sim_invasion_chance_roll_selftest.cpp's independent copy of the sibling PLANET_STATUS_* constants.
constexpr uint32_t STATUS_SLOT_ENABLED = 0x1u; // bit0
constexpr uint32_t STATUS_ALIVE        = 0x2u; // bit1
constexpr uint32_t RACE_ALIEN          = 2u;

std::vector<int32_t> g_rand_below_returns;
size_t               g_rand_below_pos = 0;
std::vector<int32_t> g_rand_below_args;
struct SetSiteCall {
    uint32_t player, planet, x, y, spot_index;
};
std::vector<SetSiteCall> g_set_site_calls;
int32_t                  g_count_landing_spots_return = 1;
int32_t                  g_count_landing_spots_calls  = 0;
struct SpawnForceCall {
    uint32_t player;
    int32_t  is_alien, home_x, home_y, invasion_points;
};
std::vector<SpawnForceCall> g_spawn_force_calls;
int32_t                     g_spawn_force_return = 1;

// Clears only the RECORDING accumulators (and rewinds the rand_below replay cursor) -- NOT the
// PROGRAMMED INPUTS (g_rand_below_returns, g_count_landing_spots_return, g_spawn_force_return),
// which each case seeds BEFORE run() (run() calls this first). An earlier revision reset those
// inputs to 1 here, wiping the seeding: E4's g_count_landing_spots_return=0 became 1 (so claim ran
// when the short-circuit OR should have skipped it) and E5's g_spawn_force_return=-3 became 1 (so
// the `>0` gate wrongly reported success). Recordings-only, matching the template selftest's run().
void reset_calls() {
    g_rand_below_pos = 0;
    g_rand_below_args.clear();
    g_set_site_calls.clear();
    g_count_landing_spots_calls = 0;
    g_spawn_force_calls.clear();
}

int32_t stub_rand_below(int32_t upper_bound) {
    g_rand_below_args.push_back(upper_bound);
    int32_t r = (g_rand_below_pos < g_rand_below_returns.size()) ? g_rand_below_returns[g_rand_below_pos] : 0;
    ++g_rand_below_pos;
    return r;
}
uint8_t stub_set_landing_site(uint32_t player, uint32_t planet, uint32_t x, uint32_t y, uint32_t spot_index) {
    g_set_site_calls.push_back({player, planet, x, y, spot_index});
    return (uint8_t)spot_index;
}
int32_t stub_count_landing_spots() {
    ++g_count_landing_spots_calls;
    return g_count_landing_spots_return;
}
int32_t stub_spawn_invasion_force(uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                                  int32_t home_tile_y, int32_t invasion_points) {
    g_spawn_force_calls.push_back({player, is_alien_race, home_tile_x, home_tile_y, invasion_points});
    return g_spawn_force_return;
}

const landing_spot_calls g_calls = {
    stub_rand_below,
    stub_set_landing_site,
    stub_count_landing_spots,
    stub_spawn_invasion_force,
};

int32_t run(sim_fixture &fx) {
    reset_calls();
    sim_store own = fx.store();
    return detail::spawn_enemy_landing(fx.view(), own, g_calls);
}

} // namespace

void run_spawn_enemy_landing_tests() {
    sim_fixture fx;
    // See the header banner's "FAITHFULLY-REPRODUCED OUT-OF-BOUNDS READ" note: when the eligibility
    // loop exhausts all MAX_PLAYERS(8) slots, the original unconditionally reads Players[8] (one past
    // the declared extent) before the `player == 8` check. Same "real extents" posture as
    // sim_test_support.h's own `landing_spots` (sized 17, not 16) for llm_strat_count_landing_spots'
    // identical one-past pattern -- resized ONCE here, locally to this test's own fixture instance (not
    // touching the shared header), so that read lands in real, observable (zero-initialized) memory
    // instead of a true out-of-bounds access. Indices 0..7 behave identically either way, so this does
    // not affect any of the other cases below.
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));

    // ==== E1: eligibility loop STOPS at a slot-enabled-but-not-alive player -- two SEPARATE checks ===
    // (the loop's own exit condition tests SLOT_ENABLED; the ALIVE bit is a DIFFERENT, subsequent,
    // unconditional check on whatever player the loop landed on). Mutation note: merging these into one
    // combined-flags test would make this case pass even though the two checks are logically distinct.
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));     // reset() doesn't touch vector size, but be explicit
    fx.player_side              = 0;                   // self -> player 0 always skipped
    fx.profiles[1].status_flags = 0;                   // disabled -> skipped
    fx.profiles[2].status_flags = STATUS_SLOT_ENABLED; // enabled, but NOT alive -> loop stops HERE
    ck_eq((uint32_t)run(fx), 0u, "E1: loop stops at an enabled-but-dead slot -> ALIVE check fails -> 0");
    ck_eq((uint32_t)g_count_landing_spots_calls, 0u, "E1: never reaches count_landing_spots (returns before it)");
    ck((uint32_t)g_spawn_force_calls.size() == 0, "E1: spawn_invasion_force never called");

    // ==== E1b: PlayerSide is skipped REGARDLESS of its own flags -- the self-check is an unconditional
    // OR term, not merely a tiebreaker consulted only when disabled. Player 2 (== PlayerSide) is fully
    // enabled+alive, yet the loop must still pass over it and land on player 3.
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side               = 2;
    fx.profiles[2].status_flags  = STATUS_SLOT_ENABLED | STATUS_ALIVE; // fully eligible, but IS PlayerSide
    fx.profiles[3].status_flags  = STATUS_SLOT_ENABLED | STATUS_ALIVE;
    fx.planet_index              = 1;
    fx.landing_spots[0].status   = -1; // empty table -> claim fails -> camera fallback (simplest path)
    g_count_landing_spots_return = 0;  // short-circuit straight to fallback
    g_spawn_force_return         = 1;
    ck_eq((uint32_t)run(fx), 1u, "E1b: player 2 (self) skipped despite being eligible");
    ck(g_spawn_force_calls.size() == 1 && g_spawn_force_calls[0].player == 3,
       "E1b: player 3, not player 2, is the one spawn_invasion_force is called for");

    // ==== E2: an eligible player is found; claim SUCCEEDS -> landing_x/y are read AS-IS (NOT
    // overwritten by the camera fallback), and set_landing_site/spawn_invasion_force fire with the
    // right (player, planet) pair. race != ALIEN -> is_alien=0. =====================================
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side               = 0;
    fx.profiles[1].status_flags  = 0;                                  // disabled -> skipped
    fx.profiles[2].status_flags  = STATUS_SLOT_ENABLED | STATUS_ALIVE; // eligible
    fx.profiles[2].race          = 0;                                  // != RACE_ALIEN
    fx.planet_index              = 4;
    fx.profiles[2].landing_x[4]  = 123; // pre-set: must survive untouched on the claim-succeeds path
    fx.profiles[2].landing_y[4]  = 456;
    fx.landing_spots[0].status   = 0;   // free (not -2, not the -1 terminator)
    fx.landing_spots[0].x        = 77;  // claim's own set_landing_site call args (recorded, not applied
    fx.landing_spots[0].y        = 88;  // to fx.profiles by our stub -- see the header banner)
    fx.landing_spots[1].status   = -1;  // terminator -> claim's scan sees exactly one candidate (i ends at 1)
    g_rand_below_returns         = {0}; // claim_landing_spot's own rand_below(i=1) -> chosen=0
    g_count_landing_spots_return = 5;   // nonzero, so the short-circuit OR does not preempt the claim call
    g_spawn_force_return         = 9;   // > 0 -> success
    ck_eq((uint32_t)run(fx), 1u, "E2: eligible player + successful claim + spawned>0 -> return 1");
    ck_eq((uint32_t)g_count_landing_spots_calls, 1u, "E2: count_landing_spots called once");
    ck(g_set_site_calls.size() == 1 && g_set_site_calls[0].player == 2 && g_set_site_calls[0].planet == 4 &&
           g_set_site_calls[0].x == 77 && g_set_site_calls[0].y == 88 && g_set_site_calls[0].spot_index == 0,
       "E2: claim_landing_spot's own set_landing_site(player=2, planet=4, x=77, y=88, spot=0)");
    ck_eq((uint32_t)fx.landing_spots[0].status, 0xfffffffeu /* -2 as uint32 */, "E2: claim committed landing_spots[0].status = -2");
    ck_eq((uint32_t)fx.profiles[2].landing_x[4], 123u, "E2: landing_x[4] UNCHANGED (claim succeeded, no fallback write)");
    ck_eq((uint32_t)fx.profiles[2].landing_y[4], 456u, "E2: landing_y[4] UNCHANGED");
    ck(g_spawn_force_calls.size() == 1 && g_spawn_force_calls[0].player == 2 &&
           g_spawn_force_calls[0].is_alien == 0 && g_spawn_force_calls[0].home_x == 123 &&
           g_spawn_force_calls[0].home_y == 456 && g_spawn_force_calls[0].invasion_points == 4 * 2 + 0x14,
       "E2: spawn_invasion_force(player=2, is_alien=0, x=123, y=456, points=planet*2+0x14=28)");

    // ==== E3: claim FAILS (free_count==0 -- an exhausted table) -> the camera-relative fallback writes
    // landing_x/y THROUGH sim_store (own.profile_at), the one _G_LLM_STRAT_PLAYERS write this function
    // makes; spawn_invasion_force then uses those FRESH values. =====================================
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side              = 0;
    fx.profiles[2].status_flags = STATUS_SLOT_ENABLED | STATUS_ALIVE;
    fx.profiles[2].race         = RACE_ALIEN; // also exercises the is_alien=1 arm
    fx.planet_index             = 7;
    fx.cam_col                  = 100;
    fx.cam_row                  = 50;
    // geom.width_mask/height_mask are set by fx.reset() to 0xff / 0x3f (see sim_test_support.h).
    fx.landing_spots[0].status = -1;  // EMPTY table (the first slot is already the terminator) ->
                                      // claim_landing_spot's scan runs zero iterations -> free_count=0
                                      // -> claim returns 0 (failure).
    g_count_landing_spots_return = 5; // nonzero -- it's the CLAIM that fails here, not the spot count
    g_spawn_force_return         = 1;
    ck_eq((uint32_t)run(fx), 1u, "E3: claim fails -> camera fallback -> spawn still succeeds -> 1");
    // width_mask(0xff) & (cam_col(100) - 15) = 0xff & 85 = 85; height_mask(0x3f) & (cam_row(50)-15) =
    // 0x3f & 35 = 35 (both operands already fit under their mask, so the AND is a no-op here -- chosen
    // deliberately so the expected values are easy to hand-verify).
    ck_eq((uint32_t)fx.profiles[2].landing_x[7], 85u, "E3: landing_x[7] = width_mask & (cam_col-15) = 85");
    ck_eq((uint32_t)fx.profiles[2].landing_y[7], 35u, "E3: landing_y[7] = height_mask & (cam_row-15) = 35");
    ck(g_spawn_force_calls.size() == 1 && g_spawn_force_calls[0].is_alien == 1 &&
           g_spawn_force_calls[0].home_x == 85 && g_spawn_force_calls[0].home_y == 35 &&
           g_spawn_force_calls[0].invasion_points == 7 * 2 + 0x14,
       "E3: spawn_invasion_force reads the FRESH fallback (x=85,y=35), is_alien=1 (RACE_ALIEN), points=34");

    // ==== E4: count_landing_spots()==0 short-circuits the OR -- claim_landing_spot is NEVER called
    // (landing_spots[0] is left provably untouched: seeded to a value claim would have overwritten). ==
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side               = 0;
    fx.profiles[2].status_flags  = STATUS_SLOT_ENABLED | STATUS_ALIVE;
    fx.planet_index              = 1;
    fx.landing_spots[0].status   = 3; // an arbitrary "free" sentinel; claim would have set this to -2
    g_count_landing_spots_return = 0; // spot_count == 0 -> short-circuit, skip the claim call entirely
    g_spawn_force_return         = 1;
    ck_eq((uint32_t)run(fx), 1u, "E4: spot_count==0 -> fallback path, still returns 1 via spawn success");
    ck_eq((uint32_t)fx.landing_spots[0].status, 3u, "E4: landing_spots[0] untouched -- claim_landing_spot never ran (short-circuit OR)");
    ck((uint32_t)g_set_site_calls.size() == 0, "E4: set_landing_site never called either");

    // ==== E5: spawned <= 0 -> overall return 0, testing the `> 0` (not `!= 0`) semantics with a
    // NEGATIVE return -- a `!=0` mistranslation would wrongly report success here. ====================
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side               = 0;
    fx.profiles[2].status_flags  = STATUS_SLOT_ENABLED | STATUS_ALIVE;
    fx.planet_index              = 1;
    fx.landing_spots[0].status   = -1;
    g_count_landing_spots_return = 0;  // take the short fallback path, same shape as E4
    g_spawn_force_return         = -3; // NEGATIVE, nonzero
    ck_eq((uint32_t)run(fx), 0u, "E5: spawned=-3 (nonzero but not >0) -> return 0");

    // boundary companion: spawned==1 (the smallest value satisfying >0) -> return 1.
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side               = 0;
    fx.profiles[2].status_flags  = STATUS_SLOT_ENABLED | STATUS_ALIVE;
    fx.planet_index              = 1;
    fx.landing_spots[0].status   = -1;
    g_count_landing_spots_return = 0;
    g_spawn_force_return         = 1;
    ck_eq((uint32_t)run(fx), 1u, "E5b: spawned==1 (boundary) -> return 1");

    // ==== E6: the faithfully-reproduced one-past read -- ALL 8 players are either self or disabled,
    // so the loop exhausts to player==8, which reads the (resized, zero-initialized) 9th slot; its
    // ALIVE bit is 0 by construction -> return 0, no landing_spot_calls callee reached. =================
    fx.reset();
    fx.profiles.resize((size_t)(MAX_PLAYERS + 1));
    fx.player_side = 0;
    for (int32_t p = 1; p < MAX_PLAYERS; ++p) fx.profiles[(size_t)p].status_flags = 0; // all disabled
    ck_eq((uint32_t)run(fx), 0u, "E6: eligibility loop exhausts all 8 -> one-past read at [8] -> return 0");
    ck_eq((uint32_t)g_count_landing_spots_calls, 0u, "E6: never reaches count_landing_spots");
}

} // namespace mh::sim::test
