//
// sim_bldg_state_turret_attack_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_turret_attack
// (sim/sim_bldg_state_turret.h/.cpp), REVIEW_REQUIRED=true in tools/data/sim_migration.json --
// largest/most effectful function in the turret-state pair (0x5c2 bytes).
//
// SCOPE (honest, not exhaustive): this file pins the efficiency short-circuit (both zero bit
// patterns), the turret_attack-only per_shot_cost multiplier, all THREE "target lost" arms (building
// branch x2 checks, unit branch x3 checks including the unit-only dist_out_of_range gate), the
// elevation->fire_kind gate, the aim-tracking while loop's zero-pass/insufficient-budget/turn-decision
// (CW, CCW, both wrap directions) shapes and its UNCONDITIONAL post-loop store, the shared
// animation-frame tail's write-index (sprite_quantity-1, one below the read), and the final fire gate including turret_fire's full
// 8-argument marshalling (register args + the last_tick_time lo/hi stack-push split). It does NOT
// re-derive the tile<->fine conversion arithmetic or the frame_at/set_frame_at byte-copy shape a
// second time from raw bytes -- those are trusted from the .cpp's own header-banner derivation and
// from sim_bldg_state_destroyed_selftest.cpp's precedent.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_turret_attack_00471e53.asm), cross-checked against the
// sim_bldg_state_turret.h/.cpp per-line address citations -- NOT read off the .cpp body alone. Full
// narrative derivation lives in sim_bldg_state_turret.h's header banner; this file cites the specific
// instruction addresses each case below pins.
//
#include "sim/sim_bldg_state_turret.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// This file's own copy of the ONE `llm_strat_bldg_state` enum member value this closure needs
// (TURRET_SCAN=0x7a) -- not exposed by the header (the .cpp keeps it in an anonymous namespace, see
// sim_bldg_state_turret.h's own comment on why), so re-derived file-locally per this project's per-TU
// convention, same posture sim_bldg_state_destroyed_selftest.cpp's BUILDING_TYPE_* constants use.
constexpr uint16_t TURRET_SCAN_STATE = 0x7a;

// ---- shared trace: proves CALL ORDER, not just call presence -----------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knobs (settable per case AFTER bind(), see its own comment) ------------------
struct XY {
    int32_t x, y;
};
std::vector<XY> g_bldg_coords_returns; // consumed in call order: [0]=own coords, [1]=target coords
size_t          g_bldg_coords_idx = 0;
int32_t         g_unit_coords_x = 0, g_unit_coords_y = 0;
uint32_t        g_dist_out_of_range_ret = 0;
int32_t         g_dir_from_to_ret       = 0;

// ---- per-callee recorders (6, one per bldg_state_turret_attack_calls member) -------------------
struct CoordsCall {
    uint16_t player;
    int32_t  index;
};
std::vector<CoordsCall> g_bldg_coords_calls;
void                    rec_bldg_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    tr("bldg_get_coords");
    g_bldg_coords_calls.push_back({player, index});
    int32_t x = 0, y = 0;
    if (g_bldg_coords_idx < g_bldg_coords_returns.size()) {
        x = g_bldg_coords_returns[g_bldg_coords_idx].x;
        y = g_bldg_coords_returns[g_bldg_coords_idx].y;
    }
    ++g_bldg_coords_idx;
    *out_x = x;
    *out_y = y;
}

std::vector<CoordsCall> g_unit_coords_calls;
void                    rec_unit_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    tr("unit_get_coords");
    g_unit_coords_calls.push_back({player, index});
    *out_x = g_unit_coords_x;
    *out_y = g_unit_coords_y;
}

struct DistCall {
    int32_t owner_filter, threshold, own_x, own_y, tgt_x, tgt_y;
};
std::vector<DistCall> g_dist_calls;
uint32_t              rec_dist_out_of_range(int32_t owner_filter, int32_t threshold, int32_t own_x,
                                            int32_t own_y, int32_t tgt_x, int32_t tgt_y) {
    tr("dist_out_of_range");
    g_dist_calls.push_back({owner_filter, threshold, own_x, own_y, tgt_x, tgt_y});
    return g_dist_out_of_range_ret;
}

struct DirCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DirCall> g_dir_calls;
int32_t              rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_dir_calls.push_back({x1, y1, x2, y2});
    return g_dir_from_to_ret;
}

std::vector<CoordsCall> g_notify_calls; // .index reused for b_index
void                    rec_bldg_notify_ui(uint16_t player, uint32_t b_index) {
    tr("bldg_notify_ui");
    g_notify_calls.push_back({player, (int32_t)b_index});
}

struct FireCall {
    uint32_t player, bldg_index;
    int32_t  target_fine_x, target_fine_y, target_elevation;
    uint32_t lo, hi;
    uint8_t  fire_kind;
};
std::vector<FireCall> g_fire_calls;
void                  rec_turret_fire(uint32_t player, uint32_t bldg_index, int32_t target_fine_x,
                                      int32_t target_fine_y, int32_t target_elevation, uint32_t lo,
                                      uint32_t hi, uint8_t fire_kind) {
    tr("turret_fire");
    g_fire_calls.push_back(
        {player, bldg_index, target_fine_x, target_fine_y, target_elevation, lo, hi, fire_kind});
}

const bldg_state_turret_attack_calls g_calls = {
    &rec_bldg_get_coords,
    &rec_unit_get_coords,
    &rec_dist_out_of_range,
    &rec_dir_from_to,
    &rec_bldg_notify_ui,
    &rec_turret_fire,
};

void reset_observations() {
    g_trace.clear();
    g_bldg_coords_returns.clear();
    g_bldg_coords_idx       = 0;
    g_unit_coords_x         = 0;
    g_unit_coords_y         = 0;
    g_dist_out_of_range_ret = 0;
    g_dir_from_to_ret       = 0;
    g_bldg_coords_calls.clear();
    g_unit_coords_calls.clear();
    g_dist_calls.clear();
    g_dir_calls.clear();
    g_notify_calls.clear();
    g_fire_calls.clear();
}

// ---- anim-slot helpers, re-derived file-locally (see the .cpp's own frame_at()/set_frame_at()) ----
int32_t read_anim_slot(const uint8_t (&anim)[48], int32_t slot) {
    int32_t v;
    std::memcpy(&v, &anim[slot * 4], sizeof(v));
    return v;
}
void write_anim_slot(uint8_t (&anim)[48], int32_t slot, int32_t v) {
    std::memcpy(&anim[slot * 4], &v, sizeof(v));
}

// ---- fixture wiring ------------------------------------------------------------------------------
// Points cur_building at buildings[player][index], binds cur_player/cur_index, and clears the
// observation globals. Call AFTER fx.reset() and after setting the cur building's own fields (this
// does not touch them), and set the return-value knobs (g_*_returns/g_*_ret) AFTER calling this --
// it clears them via reset_observations().
sim_store bind(sim_fixture &fx, uint16_t player, int32_t index) {
    fx.cur_building_ptr = &fx.b(player, index);
    fx.view_cur_player  = player;
    fx.view_cur_index   = (uint16_t)index;
    reset_observations();
    return fx.store();
}

// ---- T14-T17 shared rig: the turn-decision truth table + wrap, isolated from everything else ----
// Uses a valid BUILDING counter-target (owner=2, slot=4) with a per-shot cost of exactly 1.0 (cfg 2.0
// halved) and a tick_budget of 5.0 -- comfortably enough for ONE iteration without draining, and
// every (initial_heading, bearing) pair below is chosen so the turn step lands EXACTLY on the bearing,
// which trips the "close enough" break (0x00472290-0x004722e9) after exactly one pass -- verified by
// hand against the raw CMP/JLE/JG/JL chain, not assumed.
void run_turn_case(sim_fixture &fx, int32_t initial_heading, int32_t bearing, int32_t &out_heading,
                   int32_t &out_dir) {
    fx.reset();
    building &b          = fx.b(0, 1);
    b.efficiency         = 5.0;
    b.building_id        = 20;
    b.sub_id             = 3;
    building &target_b   = fx.b(2, 4);
    target_b.building_id = 55;
    target_b.energy      = 40.0;
    cfg_building &cb     = fx.cfg_buildings[20];
    cb.per_shot_cost     = 2.0; // halved -> 1.0
    fx.tick_budget       = 5.0; // >> 1.0, so the single pass never takes the insufficient-budget arm

    sim_store own         = bind(fx, 0, 1);
    turret   &t           = own.turret_at(0, 3);
    t.counter_ref         = 2; // BUILDING branch, target_owner=2
    t.counter_target_slot = 4;
    t.aim_heading         = initial_heading;

    g_bldg_coords_returns = {{0, 0}, {0, 0}};
    g_dir_from_to_ret     = bearing;

    detail::bldg_state_turret_attack(fx.view(), own, g_calls);

    out_heading = t.aim_heading;
    out_dir     = t.aim_step_dir;
}

} // namespace

void run_bldg_state_turret_attack_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1/T2 -- THE efficiency==0.0 BIT-PATTERN SHORT-CIRCUIT (0x00471e72-0x00471e86/0x00471ea6):
    // BOTH the +0.0 and the -0.0 bit patterns take it -- tick_budget zeroed, NOTHING else touched (no
    // callee, no turret write, no cur_building write beyond tick_budget).
    // =================================================================================================
    {
        fx.reset();
        building &b    = fx.b(0, 1);
        b.efficiency   = 0.0; // +0.0, exact bit pattern 0x0000000000000000
        b.state        = 0xBEEF;
        fx.tick_budget = 42.5;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, b.sub_id);
        t.aim_heading           = 9;
        t.aim_step_dir          = -7;
        t.acquire_retry_counter = 77;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 0.0,
                "T1: efficiency==+0.0 (0x00471e72-0x00471e86) -- tick_budget zeroed (0x00471ea6)");
        ck(g_trace.empty(), "T1: efficiency==+0.0 -- NO callee is ever reached (early return @0x00471eba)");
        ck_eq((uint32_t)fx.b(0, 1).state, 0xBEEFu, "T1: cur_building->state untouched");
        ck_eq((uint32_t)t.aim_heading, 9u, "T1: turret.aim_heading untouched (returns before the loop)");
        ck_eq((uint32_t)t.aim_step_dir, (uint32_t)-7, "T1: turret.aim_step_dir untouched");
        ck_eq((uint32_t)t.acquire_retry_counter, 77u, "T1: turret.acquire_retry_counter untouched");
    }
    {
        fx.reset();
        building &b = fx.b(0, 1);
        // -0.0: the SIGN-BIT-ONLY pattern, constructed via memcpy (not a `-0.0` literal) so the case
        // pins the raw bit pattern the TEST/JNZ+CMP idiom actually reads (0x00471e77-0x00471e84),
        // rather than trusting the compiler's unary-minus semantics to reproduce it.
        uint64_t neg_zero_bits = 0x8000000000000000ull;
        std::memcpy(&b.efficiency, &neg_zero_bits, sizeof(double));
        b.state        = 0xBEEF;
        fx.tick_budget = 42.5;

        sim_store own  = bind(fx, 0, 1);
        turret   &t    = own.turret_at(0, b.sub_id);
        t.aim_heading  = 9;
        t.aim_step_dir = -7;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 0.0,
                "T2: efficiency==-0.0 (bit pattern 0x8000000000000000) ALSO short-circuits "
                "(0x00471e7e JNZ not taken, high dword is 0 either way)");
        ck(g_trace.empty(), "T2: efficiency==-0.0 -- no callee reached");
        ck_eq((uint32_t)t.aim_heading, 9u, "T2: turret.aim_heading untouched");
    }

    // =================================================================================================
    // T3 -- PER-SHOT COST HAS THE EXTRA 0.5x MULTIPLIER (0x00471e86-0x00471ea4, FMUL DAT_005012e4 =
    // TURRET_ATTACK_INTERVAL_SCALE @0x00471e9b). cfg per_shot_cost=10.0, tick_budget=7.0: with the
    // correct halved cost (5.0) the loop takes the SUFFICIENT-budget arm and reaches the fire gate; if
    // the *0.5 were dropped (cost stays 10.0), 7.0<10.0 takes the INSUFFICIENT-budget arm instead,
    // drains the whole 7.0 into last_tick_time, zeroes tick_budget, and never reaches the fire gate.
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(3, 7);
        target_b.building_id = 55;
        target_b.energy      = 50.0;
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 10.0; // halved -> 5.0
        fx.tick_budget       = 7.0;

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 3; // BUILDING branch, target_owner=3
        t.counter_target_slot = 7;
        t.reload_ready_flag   = 1;
        t.aim_heading         = 5; // == the bearing dir_from_to reports, so the turn-decision is skipped

        g_bldg_coords_returns = {{100, 200}, {300, 400}};
        g_dir_from_to_ret     = 5;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 2.0,
                "T3: per_shot_cost = cfg(10.0) * TURRET_ATTACK_INTERVAL_SCALE(0.5) = 5.0 -- "
                "tick_budget 7.0-5.0=2.0 (0x00471e9b, 0x004721fa-0x00472203)");
        ck(g_fire_calls.size() == 1,
           "T3: with the correct halved cost, budget stays >0 after the loop and the fire gate "
           "(0x00472394) fires -- an unhalved cost would drain to 0 and never reach it");
    }

    // =================================================================================================
    // T4/T5 -- BUILDING-branch target lost, both checks (0x00472121-0x0047211c): building_id==0 (T4)
    // and energy<=0.0 with a NEGATIVE value (T5) -- both converge on the SAME lost arm (LAB_00472158),
    // which does NOT call bldg_get_coords a second time, and returns before the aim loop ever computes
    // a bearing (so aim_heading/aim_step_dir are provably untouched).
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 0;     // TARGET LOST: building_id==0 (0x00472131-0x00472139)
        target_b.energy      = 999.0; // irrelevant -- building_id==0 alone is enough
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 4.0;
        fx.tick_budget       = 7.0;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, 3);
        t.counter_ref           = 2; // BUILDING branch, target_owner=2
        t.counter_target_slot   = 4;
        t.acquire_retry_counter = 77; // sentinel, expect -> 1
        t.aim_heading           = 9;  // sentinel, expect UNCHANGED
        t.aim_step_dir          = -9; // sentinel, expect UNCHANGED

        g_bldg_coords_returns = {{100, 200}};

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(trace_eq({"bldg_get_coords", "bldg_notify_ui"}),
           "T4: building_id==0 -- only the OWN bldg_get_coords (0x00471ee6) + notify_ui fire; the "
           "target's own coords are NEVER fetched (no second bldg_get_coords call)");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)TURRET_SCAN_STATE,
              "T4: state -> TURRET_SCAN (0x00472158)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == 0 && g_notify_calls[0].index == 1,
           "T4: bldg_notify_ui(cur_player, cur_index) -- NOT the target's owner/slot (0x00472163-0x00472171)");
        ck_eq((uint32_t)t.acquire_retry_counter, 1u, "T4: acquire_retry_counter = 1 (0x00472194)");
        ck_eq((uint32_t)t.aim_heading, 9u,
              "T4: aim_heading untouched -- lost BEFORE the aim-tracking loop ever computes a bearing");
        ck_eq((uint32_t)t.aim_step_dir, (uint32_t)-9, "T4: aim_step_dir untouched");
        ck(g_fire_calls.empty(), "T4: turret_fire never reached");
    }
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 55;   // nonzero -- passes the building_id==0 check
        target_b.energy      = -3.0; // TARGET LOST via energy<=0.0, NEGATIVE (0x0047214d-0x00472156)
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 4.0;
        fx.tick_budget       = 7.0;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, 3);
        t.counter_ref           = 2;
        t.counter_target_slot   = 4;
        t.acquire_retry_counter = 55;

        g_bldg_coords_returns = {{100, 200}};

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(trace_eq({"bldg_get_coords", "bldg_notify_ui"}),
           "T5: energy<=0.0 (-3.0) -- same lost arm as building_id==0 (both converge on LAB_00472158)");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)TURRET_SCAN_STATE, "T5: state -> TURRET_SCAN");
        ck_eq((uint32_t)t.acquire_retry_counter, 1u, "T5: acquire_retry_counter = 1 (0x00472194)");
    }

    // =================================================================================================
    // T6 -- BUILDING-branch VALID target: reaches the aim loop, fires, and the shared animation-frame
    // tail writes the RIGHT index (sprite_quantity, no -1 -- 0x00472337-0x0047238e) while leaving the
    // NEIGHBOUR slot (sprite_quantity-1) untouched, proving the DEC only decrements the stored value.
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 55;
        target_b.energy      = 40.0;
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 4.0; // halved -> 2.0
        cb.sprite_quantity   = 7;
        write_anim_slot(cb.anim, 7, 0x3000); // distinct sentinel base frame for slot 7
        fx.tick_budget = 10.0;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, 3);
        t.counter_ref           = 2;
        t.counter_target_slot   = 4;
        t.attack_range          = 20;
        t.reload_ready_flag     = 1;
        t.aim_heading           = 5;  // == the bearing, so the turn-decision block is skipped
        t.acquire_retry_counter = 55; // sentinel, expect UNCHANGED on the valid path

        // BOTH slots pre-filled with distinct sentinels, because the tail's write index and read index
        // differ by one and an off-by-one either way must be caught. Slot 6 (sprite_quantity-1) is the
        // DESTINATION; slot 7 (sprite_quantity) is the READ slot in cfg and must be left alone in the
        // building record. Writing 7 instead of 6 is the 2026-08-22 defect SIM-DEEP-DIV caught at
        // step 3215 -- see the scan selftest's TS-H for the asm that settles the asymmetry.
        write_anim_slot(b.anim, 6, (int32_t)0xCDCDCDCD);
        write_anim_slot(b.anim, 7, (int32_t)0xABABABAB);

        g_bldg_coords_returns = {{100, 200}, {300, 400}};
        g_dir_from_to_ret     = 5;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(trace_eq({"bldg_get_coords", "bldg_get_coords", "dir_from_to", "turret_fire"}),
           "T6: valid building target -- own coords, target coords, ONE bearing calc, then fire "
           "(no notify_ui, no lost path)");
        ck(g_bldg_coords_calls.size() == 2 && g_bldg_coords_calls[0].player == 0 &&
               g_bldg_coords_calls[0].index == 1 && g_bldg_coords_calls[1].player == 2 &&
               g_bldg_coords_calls[1].index == 4,
           "T6: bldg_get_coords(cur_player,cur_index) THEN bldg_get_coords(target_owner,target_slot) "
           "(0x00471ee6, 0x004721a3-0x004721b0)");
        ck(g_dist_calls.empty(),
           "T6: BUILDING branch NEVER calls dist_out_of_range -- that check is UNIT-branch-only "
           "(0x004720a2)");
        ck(g_dir_calls.size() == 1 && g_dir_calls[0].x1 == 100 && g_dir_calls[0].y1 == 200 &&
               g_dir_calls[0].x2 == 300 && g_dir_calls[0].y2 == 400,
           "T6: dir_from_to(own_fine_x,own_fine_y,target_fine_x,target_fine_y) (0x004721bc-0x004721c8), "
           "computed ONCE");
        ck_eq((uint32_t)t.acquire_retry_counter, 55u,
              "T6: acquire_retry_counter UNTOUCHED on the valid path -- only the three lost-arms write it");
        ck_eq((uint32_t)t.aim_step_dir, 1u,
              "T6: default turn_dir(1) stored unconditionally (0x0047230a) -- aim_heading==bearing "
              "skipped the turn-decision block entirely, yet the store still happens");

        {
            int32_t written = read_anim_slot(b.anim, 6);
            ck_eq((uint32_t)written, (uint32_t)(0x3000 + 5 - 1),
                  "T6: anim tail -- WRITE index is sprite_quantity-1 (6), value = "
                  "cfg.anim[sprite_quantity=7] + aim_heading - 1 (0x00472337-0x0047238e; the store's "
                  "0x8f displacement is anim(+0x93) - 4)");
            int32_t read_slot = read_anim_slot(b.anim, 7);
            ck_eq((uint32_t)read_slot, 0xABABABABu,
                  "T6: anim slot sprite_quantity (7) UNTOUCHED in the BUILDING record -- it is the cfg "
                  "READ slot only; the DEC (0x0047238d) is the VALUE's -1, a SEPARATE one from the "
                  "address's");
        }

        ck(g_fire_calls.size() == 1, "T6: reload_ready_flag!=0 -- turret_fire fires once (0x004723f0)");
        if (g_fire_calls.size() == 1) {
            const auto &fc = g_fire_calls[0];
            ck(fc.target_fine_x == 300 && fc.target_fine_y == 400 && fc.target_elevation == 0,
               "T6: turret_fire target coords/elevation -- BUILDING branch always uses "
               "target_elevation=0 (0x004721b5)");
            ck_eq((uint32_t)fc.fire_kind, 1u, "T6: fire_kind stays default 1 on the BUILDING branch");
        }
    }

    // =================================================================================================
    // T7/T8 -- UNIT-branch target lost via unit_proto_id==0 (T7) and via energy<=0.0 with a NEGATIVE
    // value (T8) -- 0x00471faa-0x00471fd1. Neither calls unit_get_coords (that call only happens on
    // the NOT-lost path, 0x0047201c onward) -- contrast with T9 below, where it DOES fire.
    // =================================================================================================
    {
        fx.reset();
        building &b            = fx.b(0, 1);
        b.efficiency           = 5.0;
        b.building_id          = 20;
        b.sub_id               = 3;
        unit &target_u         = fx.u(3, 6);
        target_u.unit_proto_id = 0; // TARGET LOST: unit_proto_id==0 (0x00471faa-0x00471fb2)
        target_u.energy        = 999.0;
        cfg_building &cb       = fx.cfg_buildings[20];
        cb.per_shot_cost       = 4.0;
        fx.tick_budget         = 7.0;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, 3);
        t.counter_ref           = 0x80 | 3; // UNIT branch (bit 0x80 set), target_owner=3
        t.counter_target_slot   = 6;
        t.acquire_retry_counter = 88;

        g_bldg_coords_returns = {{100, 200}};

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(trace_eq({"bldg_get_coords", "bldg_notify_ui"}),
           "T7: unit_proto_id==0 -- lost BEFORE unit_get_coords is ever called (0x00471fb2 JZ straight "
           "to the lost arm)");
        ck(g_unit_coords_calls.empty(), "T7: unit_get_coords never reached");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)TURRET_SCAN_STATE, "T7: state -> TURRET_SCAN (0x00471fd6)");
        ck_eq((uint32_t)t.acquire_retry_counter, 1u, "T7: acquire_retry_counter = 1 (0x0047200d)");
    }
    {
        fx.reset();
        building &b            = fx.b(0, 1);
        b.efficiency           = 5.0;
        b.building_id          = 20;
        b.sub_id               = 3;
        unit &target_u         = fx.u(3, 6);
        target_u.unit_proto_id = 42;   // nonzero -- passes the proto_id==0 check
        target_u.energy        = -1.0; // TARGET LOST via energy<=0.0, NEGATIVE (0x00471fb4-0x00471fcf)
        cfg_building &cb       = fx.cfg_buildings[20];
        cb.per_shot_cost       = 4.0;
        fx.tick_budget         = 7.0;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, 3);
        t.counter_ref           = 0x80 | 3;
        t.counter_target_slot   = 6;
        t.acquire_retry_counter = 88;

        g_bldg_coords_returns = {{100, 200}};

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(trace_eq({"bldg_get_coords", "bldg_notify_ui"}),
           "T8: unit energy<=0.0 (-1.0) -- same lost arm as unit_proto_id==0 (both converge on "
           "LAB_00471fd1)");
        ck(g_unit_coords_calls.empty(), "T8: unit_get_coords never reached");
        ck_eq((uint32_t)t.acquire_retry_counter, 1u, "T8: acquire_retry_counter = 1");
    }

    // =================================================================================================
    // T9 -- UNIT-branch target lost via dist_out_of_range returning NONZERO (0x004720a2-0x004720ab) --
    // the SECOND, unit-only, lost-check. Distinguishes itself from T7/T8 by proving unit_get_coords
    // WAS called (the unit passed the proto_id/energy gate) before the range check took it out.
    // =================================================================================================
    {
        fx.reset();
        building &b            = fx.b(0, 1);
        b.efficiency           = 5.0;
        b.building_id          = 20;
        b.sub_id               = 3;
        unit &target_u         = fx.u(3, 6);
        target_u.unit_proto_id = 42;
        target_u.energy        = 40.0;
        cfg_building &cb       = fx.cfg_buildings[20];
        cb.per_shot_cost       = 4.0;
        fx.tick_budget         = 7.0;

        sim_store own           = bind(fx, 0, 1);
        turret   &t             = own.turret_at(0, 3);
        t.counter_ref           = 0x80 | 3;
        t.counter_target_slot   = 6;
        t.attack_range          = 15;
        t.acquire_retry_counter = 99;

        g_bldg_coords_returns   = {{320, 640}}; // own fine coords -> tile (10,20)
        g_unit_coords_x         = 640;          // target fine coords -> tile (20,30)
        g_unit_coords_y         = 960;
        g_dist_out_of_range_ret = 1; // OUT OF RANGE -> lost

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(trace_eq({"bldg_get_coords", "unit_get_coords", "dist_out_of_range", "bldg_notify_ui"}),
           "T9: unit valid so far -- unit_get_coords DOES fire, then dist_out_of_range, THEN lost -- "
           "contrast with T7/T8 where unit_get_coords is never reached");
        ck(g_unit_coords_calls.size() == 1 && g_unit_coords_calls[0].player == 3 &&
               g_unit_coords_calls[0].index == 6,
           "T9: unit_get_coords(target_owner,target_slot) (0x0047201f-0x00472029)");
        ck(g_dist_calls.size() == 1, "T9: dist_out_of_range called once");
        if (g_dist_calls.size() == 1) {
            const auto &dc = g_dist_calls[0];
            ck(dc.owner_filter == 0 && dc.threshold == 16, // attack_range(15)+1
               "T9: dist_out_of_range(0, attack_range+1, ...) (0x0047209b INC, 0x004720a2 CALL)");
            ck(dc.own_x == 10 && dc.own_y == 20 && dc.tgt_x == 20 && dc.tgt_y == 30,
               "T9: dist_out_of_range tile args = fine_to_tile(own/target) (0x0047202e-0x00472076)");
        }
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)TURRET_SCAN_STATE,
              "T9: state -> TURRET_SCAN on out-of-range (0x004720b0)");
        ck_eq((uint32_t)t.acquire_retry_counter, 1u, "T9: acquire_retry_counter = 1 (0x004720e7)");
        ck(g_dir_calls.empty() && g_fire_calls.empty(),
           "T9: never reaches the bearing calc or the fire gate");
    }

    // =================================================================================================
    // T10/T11 -- UNIT-branch VALID + in-range: target_elevation = units[...].elevation, and a NONZERO
    // elevation (T10) bumps fire_kind to 2 while a ZERO elevation (T11) leaves the default 1
    // (0x00472106-0x00472115).
    // =================================================================================================
    {
        fx.reset();
        building &b            = fx.b(0, 1);
        b.efficiency           = 5.0;
        b.building_id          = 20;
        b.sub_id               = 3;
        unit &target_u         = fx.u(3, 6);
        target_u.unit_proto_id = 42;
        target_u.energy        = 40.0;
        target_u.elevation     = 17; // NONZERO -> fire_kind=2
        cfg_building &cb       = fx.cfg_buildings[20];
        cb.per_shot_cost       = 4.0; // halved -> 2.0
        fx.tick_budget         = 10.0;

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 0x80 | 3;
        t.counter_target_slot = 6;
        t.attack_range        = 50;
        t.reload_ready_flag   = 1;
        t.aim_heading         = 8;

        g_bldg_coords_returns   = {{0, 0}};
        g_unit_coords_x         = 64;
        g_unit_coords_y         = 96;
        g_dist_out_of_range_ret = 0; // IN RANGE
        g_dir_from_to_ret       = 8; // == aim_heading, skip the turn-decision

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(g_dist_calls.size() == 1,
           "T10: dist_out_of_range called, returns 0 (in range) -- passes the SECOND lost-check "
           "(0x004720a7 JZ taken)");
        ck(g_fire_calls.size() == 1, "T10: reaches the fire gate");
        if (g_fire_calls.size() == 1) {
            ck_eq((uint32_t)g_fire_calls[0].target_elevation, 17u,
                  "T10: target_elevation = units[...].elevation (0x00472106-0x0047210c)");
            ck_eq((uint32_t)g_fire_calls[0].fire_kind, 2u,
                  "T10: nonzero elevation -> fire_kind=2 (0x00472113-0x00472115)");
        }
    }
    {
        fx.reset();
        building &b            = fx.b(0, 1);
        b.efficiency           = 5.0;
        b.building_id          = 20;
        b.sub_id               = 3;
        unit &target_u         = fx.u(3, 6);
        target_u.unit_proto_id = 42;
        target_u.energy        = 40.0;
        target_u.elevation     = 0; // ZERO -> fire_kind stays default 1
        cfg_building &cb       = fx.cfg_buildings[20];
        cb.per_shot_cost       = 4.0;
        fx.tick_budget         = 10.0;

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 0x80 | 3;
        t.counter_target_slot = 6;
        t.attack_range        = 50;
        t.reload_ready_flag   = 1;
        t.aim_heading         = 8;

        g_bldg_coords_returns   = {{0, 0}};
        g_unit_coords_x         = 64;
        g_unit_coords_y         = 96;
        g_dist_out_of_range_ret = 0;
        g_dir_from_to_ret       = 8;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(g_fire_calls.size() == 1, "T11: reaches the fire gate");
        if (g_fire_calls.size() == 1) {
            ck_eq((uint32_t)g_fire_calls[0].target_elevation, 0u, "T11: target_elevation = 0");
            ck_eq((uint32_t)g_fire_calls[0].fire_kind, 1u,
                  "T11: elevation==0 -- fire_kind stays default 1 (0x00472113 JZ taken, no write)");
        }
    }

    // =================================================================================================
    // T12 -- THE AIM LOOP RUNNING ZERO PASSES (tick_budget<=0 at entry, 0x004721d7 JNC taken
    // immediately): turn_dir's DEFAULT (1, set at function entry 0x00471e6b) and the UNCHANGED
    // aim_heading are still stored UNCONDITIONALLY (0x0047230a-0x00472331) -- the case a naive
    // translation that guards the store with "if the loop ran" would skip.
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 55;
        target_b.energy      = 40.0;
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 4.0;
        fx.tick_budget       = 0.0; // <=0 AT ENTRY of the while loop

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 2;
        t.counter_target_slot = 4;
        t.aim_heading         = 17; // arbitrary -- must survive unchanged, the loop never runs
        t.aim_step_dir        = -5; // sentinel, DISTINCT from the default turn_dir(1)

        g_bldg_coords_returns = {{100, 200}, {300, 400}};
        g_dir_from_to_ret     = 999; // irrelevant to the outcome -- bearing is still computed once

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(g_dir_calls.size() == 1,
           "T12: dir_from_to is computed ONCE before the loop regardless of pass count "
           "(0x004721bc-0x004721c8)");
        ck_eq((uint32_t)t.aim_heading, 17u, "T12: aim_heading unchanged (loop body never ran)");
        ck_eq((uint32_t)t.aim_step_dir, 1u,
              "T12: turn_dir's default(1) is STORED UNCONDITIONALLY even for a ZERO-pass loop "
              "(0x0047230a) -- the case a naive translation skips");
        ck_eq_d(fx.tick_budget, 0.0, "T12: tick_budget stays 0.0 (never went positive)");
        ck(g_fire_calls.empty(), "T12: tick_budget<=0 -- the final fire gate (0x00472394) never fires");
    }

    // =================================================================================================
    // T13 -- THE AIM LOOP'S INSUFFICIENT-BUDGET ARM (0x0047226b-0x00472290): tick_budget POSITIVE but
    // LESS than per_shot_cost drains the WHOLE remainder into last_tick_time and zeroes tick_budget --
    // a DIFFERENT mechanism from T12's zero-pass case (here the loop body runs once and takes this
    // arm), even though both end with tick_budget==0 and no fire.
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 55;
        target_b.energy      = 40.0;
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 10.0; // halved -> 5.0
        b.last_tick_time     = 1000.0;
        fx.tick_budget       = 3.0; // POSITIVE but < per_shot_cost(5.0) -- insufficient on pass 1

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 2;
        t.counter_target_slot = 4;
        t.aim_heading         = 9;
        t.aim_step_dir        = -3;

        g_bldg_coords_returns = {{100, 200}, {300, 400}};
        g_dir_from_to_ret     = 9;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck_eq_d(fx.b(0, 1).last_tick_time, 1000.0 - 3.0,
                "T13: insufficient-budget arm banks the WHOLE remaining budget into last_tick_time "
                "(0x00472276-0x00472279 FSUBR/FSTP)");
        ck_eq_d(fx.tick_budget, 0.0, "T13: tick_budget zeroed by the same arm (0x0047227c-0x00472286)");
        ck_eq((uint32_t)t.aim_heading, 9u,
              "T13: aim_heading untouched -- the insufficient-budget arm never reaches the "
              "turn-decision code");
        ck_eq((uint32_t)t.aim_step_dir, 1u,
              "T13: turn_dir default(1) still stored unconditionally after the loop exits");
        ck(g_fire_calls.empty(),
           "T13: tick_budget<=0 after the loop -- fire gate skipped, a DIFFERENT mechanism from T12's "
           "zero-pass case even though the final observable state looks similar");
    }

    // =================================================================================================
    // T14-T17 -- THE TURN-DECISION TRUTH TABLE (0x00472209-0x00472269), all four combinations: CW/CCW
    // x no-wrap/wrap. Each pair is chosen so the single turn step lands EXACTLY on the bearing, which
    // hand-verification against the raw CMP/JLE/JG/JL chain confirms converges in ONE pass.
    // =================================================================================================
    {
        int32_t heading = 0, dir = 0;
        run_turn_case(fx, /*initial=*/1, /*bearing=*/2, heading, dir);
        ck_eq((uint32_t)heading, 2u, "T14: CW, no wrap (1->2) -- aim_heading steps to the bearing");
        ck_eq((uint32_t)dir, 1u, "T14: turn_dir=+1 (CW branch, 0x00472239)");
    }
    {
        int32_t heading = 0, dir = 0;
        run_turn_case(fx, /*initial=*/24, /*bearing=*/23, heading, dir);
        ck_eq((uint32_t)heading, 23u, "T15: CCW, no wrap (24->23)");
        ck_eq((uint32_t)dir, (uint32_t)-1, "T15: turn_dir=-1 (CCW branch, 0x00472252)");
    }
    {
        int32_t heading = 0, dir = 0;
        run_turn_case(fx, /*initial=*/1, /*bearing=*/24, heading, dir);
        ck_eq((uint32_t)heading, 24u,
              "T16: CCW WITH WRAP -- 1 decrements to 0, wraps to 24 (0x0047225f-0x00472265 CMP 1/ADD 0x18)");
        ck_eq((uint32_t)dir, (uint32_t)-1, "T16: turn_dir=-1 (CCW branch)");
    }
    {
        int32_t heading = 0, dir = 0;
        run_turn_case(fx, /*initial=*/24, /*bearing=*/1, heading, dir);
        ck_eq((uint32_t)heading, 1u,
              "T17: CW WITH WRAP -- 24 increments to 25, wraps to 1 (0x00472246-0x0047224c CMP 0x18/SUB 0x18)");
        ck_eq((uint32_t)dir, 1u, "T17: turn_dir=+1 (CW branch)");
    }

    // =================================================================================================
    // T18 -- FINAL FIRE GATE, reload_ready_flag==0 (0x004723bf-0x004723c6 JZ taken): tick_budget is
    // POSITIVE after the loop, but the gate zeroes it instead of firing (0x004723f7-0x00472401).
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 55;
        target_b.energy      = 40.0;
        cfg_building &cb     = fx.cfg_buildings[20];
        cb.per_shot_cost     = 2.0; // halved -> 1.0
        fx.tick_budget       = 5.0;

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 2;
        t.counter_target_slot = 4;
        t.aim_heading         = 8;
        t.reload_ready_flag   = 0; // NOT ready

        g_bldg_coords_returns = {{0, 0}, {0, 0}};
        g_dir_from_to_ret     = 8; // == aim_heading, one pass, budget 5-1=4 remains >0 entering the gate

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 0.0,
                "T18: tick_budget>0 but reload_ready_flag==0 -- the gate zeroes it instead of firing "
                "(0x004723bf-0x004723c6, 0x004723f7-0x00472401)");
        ck(g_fire_calls.empty(), "T18: turret_fire NEVER called when reload_ready_flag==0");
    }

    // =================================================================================================
    // T19 -- FINAL FIRE GATE, reload_ready_flag!=0: turret_fire's FULL 8-argument marshalling
    // (0x004723c8-0x004723f0), including the last_tick_time lo/hi STACK-PUSH split (param_6=lo pushed
    // second-to-last @0x004723d5, param_7=hi pushed third-to-last @0x004723d2), verified via memcpy on
    // a seeded double whose lo/hi halves are genuinely distinct.
    // =================================================================================================
    {
        fx.reset();
        building &b          = fx.b(0, 1);
        b.efficiency         = 5.0;
        b.building_id        = 20;
        b.sub_id             = 3;
        building &target_b   = fx.b(2, 4);
        target_b.building_id = 55;
        target_b.energy      = 40.0;
        // A double whose two 32-bit halves are individually distinguishable, and which stays UNTOUCHED
        // by this run (tick_budget stays sufficient the whole loop, so the insufficient-budget arm
        // that would otherwise mutate last_tick_time never fires).
        b.last_tick_time = 123456.789;
        cfg_building &cb = fx.cfg_buildings[20];
        cb.per_shot_cost = 2.0;
        fx.tick_budget   = 5.0;

        sim_store own         = bind(fx, 0, 1);
        turret   &t           = own.turret_at(0, 3);
        t.counter_ref         = 2;
        t.counter_target_slot = 4;
        t.aim_heading         = 8;
        t.reload_ready_flag   = 1;

        g_bldg_coords_returns = {{111, 222}, {333, 444}};
        g_dir_from_to_ret     = 8;

        detail::bldg_state_turret_attack(fx.view(), own, g_calls);

        ck(g_fire_calls.size() == 1, "T19: reload_ready_flag!=0 -- turret_fire fires (0x004723f0)");
        if (g_fire_calls.size() == 1) {
            const auto &fc = g_fire_calls[0];
            ck(fc.player == 0 && fc.bldg_index == 1,
               "T19: turret_fire's cur_player/cur_index register args (EAX/EDX, 0x004723e2-0x004723e9)");
            ck(fc.target_fine_x == 333 && fc.target_fine_y == 444,
               "T19: turret_fire's target coords (ECX/EBX registers, 0x004723dc-0x004723df)");
            ck_eq((uint32_t)fc.target_elevation, 0u,
                  "T19: BUILDING branch -- target_elevation pushed LAST is 0 (0x004723db)");

            uint64_t bits;
            std::memcpy(&bits, &b.last_tick_time, sizeof(bits));
            const uint32_t want_lo = (uint32_t)bits;
            const uint32_t want_hi = (uint32_t)(bits >> 32);
            ck(want_lo != want_hi,
               "T19 sanity: the seeded last_tick_time's lo/hi halves are genuinely distinguishable");
            ck_eq(fc.lo, want_lo,
                  "T19: last_tick_time's LOW dword, pushed SECOND-to-last (0x004723d5) -> param_6");
            ck_eq(fc.hi, want_hi,
                  "T19: last_tick_time's HIGH dword, pushed THIRD-to-last (0x004723d2) -> param_7");
            ck_eq((uint32_t)fc.fire_kind, 1u,
                  "T19: fire_kind pushed FIRST (0x004723cc) -> param_8, default 1 on the BUILDING branch");
        }
    }
}

} // namespace mh::sim::test
