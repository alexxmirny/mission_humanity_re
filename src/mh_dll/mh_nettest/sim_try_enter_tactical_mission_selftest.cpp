//
// sim_try_enter_tactical_mission_selftest.cpp -- `simtest` offline oracle for
// llm_strat_try_enter_tactical_mission @0x0044d34f (sim/resid/sim_squad_status_gather.h/.cpp).
//
// NO SHADOW SITE (see the header banner: both functions in that TU are offline-only) -- this oracle
// is the only verification (proof:OFFLINE).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_try_enter_tactical_mission_0044d34f.asm -- the .asm is the spec,
// never the .c beside it):
//   0x0044d377: count = bldg_gather_nearby_squad_status(v, own, c, player, target_owner,
//     target_bldg_idx) -- same-TU sibling call (that function's own oracle is a SEPARATE session's
//     responsibility; this file only needs to drive its RETURN VALUE to zero/nonzero).
//   0x0044d37f-0x0044d383: CMP dword ptr [count],0x0 / JZ skip -- count==0 takes the do-nothing arm
//     (no squad nearby, entry refused); nothing past this point executes.
//   0x0044d385-0x0044d38f: (count!=0 only) c.save_planet_to_disk(*v.planet_index, 1) -- EAX is the
//     planet index read FRESH from G_PLANET_INDEX at call time (MOV EAX,[G_PLANET_INDEX] sits AFTER
//     the JZ, not hoisted), EDX the literal 1 (MOV EDX,0x1 precedes it). Return value discarded (the
//     original's own defect per the header banner -- not this oracle's business to flag beyond that).
//   0x0044d394: byte ptr [_G_LLM_GAME_MODE] = 0x6 -- GAME_MODE 6 = tactical mission.
//   0x0044d39b: CALL llm_tact_mission_start -- no args, no return used.
//   LAB_0044d3a0 is the common exit for both arms (plain epilogue, nothing else observable).
//
#include "sim/resid/sim_squad_status_gather.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

int32_t g_seq = 0; // global call-order counter, shared by every mock below (save vs mission_start)

// ---- tile_dist_wrapped -----------------------------------------------------------------------------
// Only reached through the sibling gather's own scan loop. A fixed, controlled return lets this
// oracle drive the gather's returned squad count to a known NONZERO value (one real soldier-carrying
// unit judged "nearby") without reproducing the gather's own footprint/half-offset tile-position
// arithmetic -- that function has its own oracle; this file's only job is the gate at 0x0044d37f.
// The ZERO case needs no mock behaviour at all: an EMPTY ctrl_groups[0] makes the scan loop body
// never execute, so tile_dist_wrapped is never called on that arm (asserted below).
int32_t g_tile_dist_wrapped_calls  = 0;
int32_t g_tile_dist_wrapped_result = 0;
int32_t rec_tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    ++g_tile_dist_wrapped_calls;
    return g_tile_dist_wrapped_result;
}

// ---- save_planet_to_disk ---------------------------------------------------------------------------
struct save_call {
    uint32_t planet_index;
    uint32_t param_2;
    int32_t  seq;
};
std::vector<save_call> g_save_calls;
uint32_t               rec_save_planet_to_disk(uint32_t planet_index, uint32_t param_2) {
    g_save_calls.push_back({planet_index, param_2, g_seq++});
    return 1; // the original's own call site discards this return value -- see header banner
}

// ---- mission_start ---------------------------------------------------------------------------------
// No args, so to pin that the game_mode=6 store already ran BEFORE this call, the mock reads
// game_mode itself at call time through a pointer each case sets before invoking the function under
// test -- exactly the technique the brief calls for.
int32_t        g_mission_start_calls          = 0;
int32_t        g_mission_start_seq            = -1;
uint8_t        g_mission_start_seen_game_mode = 0xff;
const uint8_t *g_mission_start_mode_ptr       = nullptr;
void           rec_mission_start() {
    ++g_mission_start_calls;
    g_mission_start_seq = g_seq++;
    if (g_mission_start_mode_ptr != nullptr) g_mission_start_seen_game_mode = *g_mission_start_mode_ptr;
}

const squad_status_gather_calls g_calls = {
    &rec_tile_dist_wrapped,
    &rec_save_planet_to_disk,
    &rec_mission_start,
};

constexpr uint16_t PLAYER          = 0; // scan_player
constexpr uint16_t TARGET_OWNER    = 1;
constexpr int32_t  TARGET_BLDG_IDX = 3;
constexpr int32_t  SQUAD_UNIT_SLOT = 7; // index into units[PLAYER][] via ctrl_groups[0].unit_ids[0]
constexpr int32_t  BUILDING_ID     = 5; // cfg_buildings[] index the target building's building_id names
constexpr int32_t  UNIT_PROTO_ID   = 2; // cfg_units[] index the candidate unit's unit_proto_id names

void reset_recorders() {
    g_seq                      = 0;
    g_tile_dist_wrapped_calls  = 0;
    g_tile_dist_wrapped_result = 0;
    g_save_calls.clear();
    g_mission_start_calls          = 0;
    g_mission_start_seq            = -1;
    g_mission_start_seen_game_mode = 0xff;
    g_mission_start_mode_ptr       = nullptr;
}

// Seeds the target building + its cfg record with valid, NONZERO-energy geometry so the sibling
// gather's UNCONDITIONAL post-loop "building's own energy%" computation (energy_status_percent
// dividing raw_energy*scale by cfg energy) never divides by zero on EITHER arm below -- the gather
// runs its full body regardless of how many squad members it finds along the way.
void seed_target_building(sim_fixture &fx) {
    building &b   = fx.b(TARGET_OWNER, TARGET_BLDG_IDX);
    b.building_id = BUILDING_ID;
    b.x           = 10;
    b.y           = 12;
    b.energy      = 50.0;

    cfg_building &cb = fx.cfg_buildings[BUILDING_ID];
    cb.height        = 4;
    cb.width         = 6;
    cb.energy        = 100.0;
}

} // namespace

void run_try_enter_tactical_mission_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- squad gather returns a count of ZERO, driven FOR REAL (ctrl_groups[0] genuinely empty, not
    // a mocked shortcut around the gate itself): the do-nothing arm must write NOTHING past the gather
    // call. game_mode is seeded to a distinctive non-6/non-default value and must come back UNCHANGED;
    // both outward mocks must see zero calls. A body that always entered would otherwise pass every
    // check in T2 too, so this arm is what makes that mutant visible.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_target_building(fx);
        fx.ctrl_groups[0].count = 0; // no candidates at all -- the scan loop body never executes
        fx.planet_index         = 999;
        fx.game_mode            = 2; // distinctive non-6 value (2 == live strategic gameplay)

        sim_store own = fx.store();
        detail::try_enter_tactical_mission(fx.view(), own, g_calls, PLAYER, TARGET_OWNER,
                                           TARGET_BLDG_IDX);

        ck_eq((uint32_t)fx.squad_status_count, 0u,
              "T1: real gather drive -- empty ctrl_groups[0] -> squad_status_count 0, 0x0044d37c");
        ck_eq((uint32_t)g_tile_dist_wrapped_calls, 0u,
              "T1: scan loop body never entered -- tile_dist_wrapped uncalled");
        ck_eq((uint32_t)fx.game_mode, 2u,
              "T1: GATE CLOSED (count==0) -- game_mode UNCHANGED, 0x0044d37f/0x0044d383 JZ taken");
        ck_eq((uint32_t)g_save_calls.size(), 0u,
              "T1: save_planet_to_disk never called on the do-nothing arm, 0x0044d38a-d38f skipped");
        ck_eq((uint32_t)g_mission_start_calls, 0u,
              "T1: mission_start never called on the do-nothing arm, 0x0044d39b skipped");
    }

    // =================================================================================================
    // T2 -- squad gather returns a NONZERO count (one real soldier-carrying unit seeded into
    // ctrl_groups[0], judged "nearby" by the mocked tile_dist_wrapped): the enter arm runs, in order --
    // save_planet_to_disk(planet_index, 1), then game_mode=6, then mission_start(). mission_start's
    // mock reads game_mode AT THE MOMENT OF ITS OWN CALL to pin that the mode store already happened.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_target_building(fx);

        fx.ctrl_groups[0].count       = 1;
        fx.ctrl_groups[0].unit_ids[0] = SQUAD_UNIT_SLOT;

        unit &u         = fx.u(PLAYER, SQUAD_UNIT_SLOT);
        u.unit_proto_id = UNIT_PROTO_ID;
        u.x             = 1;
        u.y             = 1;
        u.energy        = 30.0;

        cfg_unit &proto     = fx.cfg_units[UNIT_PROTO_ID];
        proto.soldier_count = 2; // nonzero -> soldier-carrying; appends 2 identical slots
        proto.soldier_type  = 1; // != SOLDIER_TYPE_COMMANDO(2)
        proto.energy        = 60.0;

        g_tile_dist_wrapped_result = 0; // well under SQUAD_SCAN_RADIUS_TILES(15) -- unit is "nearby"

        fx.planet_index = 777; // distinctive nonzero
        fx.game_mode    = 2;   // distinctive non-6, must become 6

        g_mission_start_mode_ptr = &fx.game_mode;

        sim_store own = fx.store();
        detail::try_enter_tactical_mission(fx.view(), own, g_calls, PLAYER, TARGET_OWNER,
                                           TARGET_BLDG_IDX);

        ck_eq((uint32_t)fx.squad_status_count, 2u,
              "T2: real gather drive -- one soldier_count=2 unit -> squad_status_count 2, 0x0044d37c");
        ck_eq((uint32_t)fx.squad_bb_target_owner, (uint32_t)TARGET_OWNER,
              "T2: squad blackboard left exactly as the gather wrote it (squad_bb_target_owner), "
              "try_enter itself touches only game_mode -- 0x0044d7ff");

        ck_eq((uint32_t)g_save_calls.size(), 1u,
              "T2: save_planet_to_disk called exactly once, 0x0044d38a-d38f");
        if (g_save_calls.size() == 1) {
            ck_eq(g_save_calls[0].planet_index, 777u,
                  "T2: save_planet_to_disk EAX = *G_PLANET_INDEX read fresh, 0x0044d38a");
            ck_eq(g_save_calls[0].param_2, 1u, "T2: save_planet_to_disk EDX = literal 1, 0x0044d385");
        }

        ck_eq((uint32_t)fx.game_mode, 6u,
              "T2: _G_LLM_GAME_MODE = 6 (tactical), byte store 0x0044d394");

        ck_eq((uint32_t)g_mission_start_calls, 1u,
              "T2: llm_tact_mission_start() called exactly once, 0x0044d39b");

        if (g_save_calls.size() == 1 && g_mission_start_calls == 1) {
            ck(g_save_calls[0].seq < g_mission_start_seq,
               "T2: ORDER -- save_planet_to_disk (0x0044d38f) runs before mission_start (0x0044d39b)");
        }
        ck_eq((uint32_t)g_mission_start_seen_game_mode, 6u,
              "T2: mission_start's mock observed game_mode ALREADY 6 -- the mode store (0x0044d394) "
              "precedes the mission_start call (0x0044d39b)");
    }
}

} // namespace mh::sim::test
