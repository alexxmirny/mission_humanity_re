//
// sim_unit_state_deploy_to_building_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_deploy_to_building @0x00481a6b (sim/sim_unit_state_deploy.h/.cpp,
// RI-SIM / SIM1-G3 batch, largest function in this batch at 0x511 bytes).
//
// arm_ready:false (same posture as this file's sibling deploy_approach oracle -- see that file's own
// banner) -- this offline oracle is the evidence for this site.
//
// EXPECTED BEHAVIOUR, traced instruction-by-instruction from the .asm
// (tmp/decomp_sim/llm_strat_unit_state_deploy_to_building_00481a6b.asm) and cross-checked against the
// header derivation (sim/sim_unit_state_deploy.h):
//   step_cost = cfg_units[proto].step_speed[player] * 2.0                          -- 0x00481a91-0x00481ac4
//   GATE 1 (budget): if tick_budget < step_cost (JNC 0x00481af0 -- NOT taken means insufficient):
//     u.activity_clock -= tick_budget; tick_budget = 0.0; return.                  -- 0x00481ac6-0x00481aeb
//   else: tick_budget -= step_cost.                                                -- 0x00481af0-0x00481af9
//   u.elevation -= 1.                                                              -- 0x00481aff-0x00481b04
//   GATE 2 (elevation, a THIRD gate the header flags as easy to miss): if u.elevation (POST-decrement)
//     > cfg_units[proto].elevation_2 (JG 0x00481f72, strictly greater): return, nothing else runs.
//                                                                                    -- 0x00481b0c-0x00481b25
//   corner = calc_placement_corner_from_center(proto, u.x, u.y)  -- AROUND THE UNIT'S OWN POSITION,
//     unlike deploy_approach's neighbor-tile probe.                                 -- 0x00481b2b-0x00481b53
//   building_id = construct_finalize(0, corner.row, player, 2, corner.col, equivalent)
//                                                                                    -- 0x00481b58-0x00481b83
//   GATE 3 (building_id != 0, JZ 0x00481e10 on ==0):
//   ARM SUCCESS (0x00481b95-0x00481e0b):
//     b.energy = (u.energy / cfg_units[proto].energy) * cfg_buildings[equivalent].energy
//                                                                                    -- 0x00481b95-0x00481be8
//     bldg_update_charge_pips(player, building_id)  -- UNCONDITIONAL.                -- 0x00481bee-0x00481bfd
//     b.shuttle_slot = u.shuttle_slot  -- UNCONDITIONAL write, any value incl. 0.     -- 0x00481bfd-0x00481c1c
//     GATE 3a (slot != 0, JZ 0x00481cad on ==0): bind prod_shuttle_slot record
//       (status=0xca, src_building_index=building_id, type_ref_id=b.building_id).    -- 0x00481c34-0x00481ca6
//     proto_type = unit_of(player,index).unit_proto_id's cfg type (roster-array read).
//     GATE 3b (proto_type == A_HELI_MOTHER || == H_HELI_MOTHER, else skip entirely to population):
//                                                                                    -- 0x00481cad-0x00481d0f
//       GATE 3b-i (primary_mother_bldg[planet] == 0, JNZ 0x00481d57 on !=0): set it = building_id.
//                                                                                    -- 0x00481d15-0x00481d51
//       GATE 3b-ii (primary_mother_unit[planet] == unit_index, JNZ 0x00481db0 on mismatch): clear it
//         to 0, then mother_reelect_primary(player, corner.col, corner.row) -- REUSES the corner pair.
//                                                                                    -- 0x00481d57-0x00481dab
//     population[player].human += cfg_units[proto].human;
//     population[player].human_in_field -= cfg_units[proto].human.                  -- 0x00481db0-0x00481df6
//     bldg_construction_complete(player, building_id, <dead param_3>, <dead param_4>) -- UNCONDITIONAL;
//       param_3/param_4 are PROVEN dead in the callee (header's RESOLVED note) -- not tested here.
//                                                                                    -- 0x00481dfc-0x00481e06
//   ARM FAILURE (0x00481e10-0x00481f0d, building_id == 0):
//     masked_x = geom.bw_mask & (cfg_buildings[equivalent].width * 16 + corner.col * 32)
//     masked_y = geom.bh_mask & (cfg_buildings[equivalent].height * 16 + corner.row * 32)
//       -- bw_mask/bh_mask (fine/pixel-space), NOT width_mask/height_mask (tile-space).
//                                                                                    -- 0x00481e10-0x00481e79
//     GATE 4 (*sim_active != 0, JZ 0x00481ed6 on ==0):
//       the original offscreen_snd_volume+snd_play pair (0x00481e7c-0x00481ed1), emitted as ONE
//       snd_play_at(cfg_buildings[equivalent].sound_explo, fine_to_tile(masked_x),
//       fine_to_tile(masked_y)) record since the LIFT-NOTIFY offscreen conversion.
//     fx_anim_spawn(masked_x, masked_y, frame_at(anim,7), RAW game_clock, 1)
//       -- UNCONDITIONAL (does NOT depend on sim_active, unlike the two calls above) -- the header
//       flags this as the raw game_clock value, NOT the "elapsed" (game_clock - tick_budget) derivation
//       sim_bldg_state_destroyed.cpp's own fx_anim_spawn call uses.                  -- 0x00481ed6-0x00481f0d
//   SHARED TAIL (both arms, 0x00481f12-0x00481f72): unit_unlink_tile(player, unit_index);
//     fow_remove_sight(player, u.x, u.y, cfg_units[proto].sight)  -- position UNCHANGED, this function
//     never writes unit.x/y; unit_teardown(player, unit_index).
//
#include "sim/sim_unit_state_deploy.h"

#include <cstring>
#include <string>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// A single shared call-order trace, appended to by every recorder below -- this is what lets a case
// PIN a claim about ORDER (e.g. "snd_play_at before fx_anim", "pips before complete").
std::vector<std::string> g_order;

// mh_cfg_final_struct_Building::anim is cfg_t_frame_index[12] flattened to uint8_t[48] (same layout
// sim_unit_state_deploy.cpp's own file-local frame_at() reads) -- this is the INVERSE, for seeding a
// specific frame id at a given slot without needing that file-local helper.
void write_frame_at(uint8_t (&anim)[48], int32_t slot, int32_t value) {
    std::memcpy(&anim[slot * 4], &value, sizeof(value));
}

struct corner_call {
    uint16_t proto_id;
    int32_t  center_x, center_y;
};
std::vector<corner_call> g_corner_calls;
int32_t                  g_corner_col_out = 200, g_corner_row_out = 300;
void                     rec_calc_placement_corner_from_center(uint16_t unit_index, int32_t center_x, int32_t center_y,
                                                               uint32_t *out_col, uint32_t *out_row) {
    g_corner_calls.push_back({unit_index, center_x, center_y});
    g_order.push_back("corner");
    *out_col = (uint32_t)g_corner_col_out;
    *out_row = (uint32_t)g_corner_row_out;
}

struct construct_call {
    uint32_t param_1;
    int32_t  y_b;
    uint16_t player;
    char     param_4;
    uint32_t x_b;
    uint32_t building_id;
};
std::vector<construct_call> g_construct_calls;
int32_t                     g_construct_result = 0;
int32_t                     rec_construct_finalize(uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                                   uint32_t x_b, uint32_t building_id) {
    g_construct_calls.push_back({param_1, y_b, player, param_4, x_b, building_id});
    g_order.push_back("construct");
    return g_construct_result;
}

struct pips_call {
    uint16_t player;
    uint32_t building_id;
};
std::vector<pips_call> g_pips_calls;
void                   rec_bldg_update_charge_pips(uint16_t player, uint32_t building_id) {
    g_pips_calls.push_back({player, building_id});
    g_order.push_back("pips");
}

struct reelect_call {
    int32_t player, x, y;
};
std::vector<reelect_call> g_reelect_calls;
int32_t                   rec_mother_reelect_primary(int32_t player, int32_t x, int32_t y) {
    g_reelect_calls.push_back({player, x, y});
    g_order.push_back("reelect");
    return 0;
}

struct complete_call {
    uint32_t player, building_index, param_3, param_4;
};
std::vector<complete_call> g_complete_calls;
void                       rec_bldg_construction_complete(uint32_t player, uint32_t building_index, uint32_t param_3,
                                                          uint32_t param_4) {
    g_complete_calls.push_back({player, building_index, param_3, param_4});
    g_order.push_back("complete");
}

struct snd_play_at_call {
    int32_t sound_id, tile_col, tile_row;
};
std::vector<snd_play_at_call> g_snd_play_at_calls;
void                          rec_snd_play_at(int32_t sound_id, int32_t tile_col, int32_t tile_row) {
    g_snd_play_at_calls.push_back({sound_id, tile_col, tile_row});
    g_order.push_back("snd_play_at");
}

struct fx_anim_call {
    uint32_t x, y, param_3;
    double   param_4;
    uint32_t param_5;
};
std::vector<fx_anim_call> g_fx_anim_calls;
uint32_t                  rec_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t param_3, double param_4,
                                            uint32_t param_5) {
    g_fx_anim_calls.push_back({x, y, param_3, param_4, param_5});
    g_order.push_back("fx_anim");
    return 0;
}

struct unlink_call {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<unlink_call> g_unlink_calls;
void                     rec_unit_unlink_tile(uint32_t player, uint16_t unit_index) {
    g_unlink_calls.push_back({player, unit_index});
    g_order.push_back("unlink");
}

struct fow_call {
    uint32_t player;
    int32_t  x, y;
    uint8_t  radius;
};
std::vector<fow_call> g_fow_calls;
void                  rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    g_fow_calls.push_back({player, x, y, radius});
    g_order.push_back("fow");
}

struct teardown_call {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<teardown_call> g_teardown_calls;
void                       rec_unit_teardown(uint32_t player, uint16_t unit_index) {
    g_teardown_calls.push_back({player, unit_index});
    g_order.push_back("teardown");
}

constexpr uint16_t PLAYER       = 3;
constexpr int32_t  UNIT_INDEX   = 5;
constexpr uint16_t PROTO_ID     = 9;
constexpr int32_t  EQUIVALENT   = 21;
constexpr int32_t  PLANET_INDEX = 4;

const unit_state_deploy_to_building_calls g_calls = {
    &rec_calc_placement_corner_from_center,
    &rec_construct_finalize,
    &rec_bldg_update_charge_pips,
    &rec_mother_reelect_primary,
    &rec_bldg_construction_complete,
    &rec_snd_play_at,
    &rec_fx_anim_spawn,
    &rec_unit_unlink_tile,
    &rec_fow_remove_sight,
    &rec_unit_teardown,
};

void reset_recorders() {
    g_order.clear();
    g_corner_calls.clear();
    g_corner_col_out = 200;
    g_corner_row_out = 300;
    g_construct_calls.clear();
    g_construct_result = 0;
    g_pips_calls.clear();
    g_reelect_calls.clear();
    g_complete_calls.clear();
    g_snd_play_at_calls.clear();
    g_fx_anim_calls.clear();
    g_unlink_calls.clear();
    g_fow_calls.clear();
    g_teardown_calls.clear();
}

bool order_is(std::initializer_list<const char *> want) {
    if (g_order.size() != want.size()) return false;
    size_t i = 0;
    for (const char *w : want) {
        if (g_order[i++] != w) return false;
    }
    return true;
}

// Common fixture setup shared by every case below: the unit + its cfg row + the building type's cfg
// row + the fine-space mask pair + a nonzero population baseline. `elevation`/`shuttle_slot`/`cfg_type`
// are the fields that vary case-to-case, so they are parameters rather than baked in here.
unit &setup(sim_fixture &fx, int32_t elevation, uint8_t shuttle_slot, uint32_t cfg_type) {
    unit &u          = fx.u(PLAYER, UNIT_INDEX);
    u.unit_proto_id  = PROTO_ID;
    u.x              = 17;
    u.y              = 23;
    u.energy         = 25.0;
    u.elevation      = elevation;
    u.shuttle_slot   = shuttle_slot;
    u.activity_clock = 1000.0;

    cfg_unit &cu          = fx.cfg_units[PROTO_ID];
    cu.step_speed[PLAYER] = 1.5; // step_cost = 1.5 * 2.0 = 3.0
    cu.elevation_2        = 10;
    cu.energy             = 50.0;
    cu.equivalent         = EQUIVALENT;
    cu.human              = 7;
    cu.sight              = 12;
    cu.type               = cfg_type;

    cfg_building &cb = fx.cfg_buildings[EQUIVALENT];
    cb.width         = 4;
    cb.height        = 6;
    cb.sound_explo   = 555;
    cb.energy        = 200.0;
    write_frame_at(cb.anim, 7, 888);

    // DISTINCT from geom.width_mask/height_mask (0xff/0x3f, set by sim_fixture::reset()) -- see the
    // header CORRECTION: this function masks against bw_mask/bh_mask (fine/pixel-space), not the
    // tile-space pair. If a translation reached for width_mask/height_mask instead, T9 below diverges.
    fx.geom.bw_mask = 0x3ff;
    fx.geom.bh_mask = 0x1ff;

    // Nonzero, distinct baseline so += vs -= (and a dropped write) are all observable, not masked by
    // a 0 -> 7 / 0 -> -7 coincidence.
    fx.population[PLAYER].human          = 100;
    fx.population[PLAYER].human_in_field = 50;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = static_cast<uint16_t>(UNIT_INDEX);
    fx.planet_index    = PLANET_INDEX;
    return u;
}

} // namespace

void run_unit_state_deploy_to_building_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- GATE 1 (budget) insufficient: tick_budget (2.0) < step_cost (3.0). activity_clock -=
    // tick_budget, tick_budget zeroed, function returns before the elevation decrement or any call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit     &u       = setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/0);
        sim_view  v       = fx.view();
        sim_store own     = fx.store();
        own.tick_budget() = 2.0;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq_d(u.activity_clock, 998.0, "T1: activity_clock -= tick_budget (1000.0-2.0), 0x00481ac6-0x00481ad4");
        ck_eq_d(own.tick_budget(), 0.0, "T1: tick_budget zeroed, 0x00481ad7-0x00481aeb");
        ck_eq((uint32_t)u.elevation, 11u, "T1: elevation NOT decremented -- gate 1 returns first, 0x00481aeb JMP");
        ck(g_corner_calls.empty() && g_construct_calls.empty(),
           "T1: no corner/construct call -- insufficient-budget path never reaches gate 2/3");
        ck(g_order.empty(), "T1: no call fires at all on the insufficient-budget path");
    }

    // =================================================================================================
    // T2 -- GATE 1 boundary: tick_budget (3.0) == step_cost (3.0) is NOT insufficient (JNC on
    // "not below"), so budget IS spent and elevation IS decremented; then GATE 2 (elevation, post-
    // decrement 14 > elevation_2 10) returns before any call. Isolates gate 2 from gate 3.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit     &u       = setup(fx, /*elevation=*/15, /*shuttle_slot=*/0, /*cfg_type=*/0);
        sim_view  v       = fx.view();
        sim_store own     = fx.store();
        own.tick_budget() = 3.0;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq_d(own.tick_budget(), 0.0,
                "T2: budget==step_cost is NOT the insufficient path (JNC), so tick_budget -= step_cost still "
                "runs, 0x00481af0-0x00481aff");
        ck_eq((uint32_t)u.elevation, 14u, "T2: elevation decremented (15-1), 0x00481aff-0x00481b04");
        ck(g_corner_calls.empty(),
           "T2: GATE 2 (elevation 14 > elevation_2 10, JG) returns before calc_placement_corner_from_center, "
           "0x00481b0c-0x00481b25");
        ck(g_order.empty(), "T2: no call fires -- gate 2 returns first");
    }

    // =================================================================================================
    // T3 -- GATE 2 boundary (elevation post-decrement 10 == elevation_2 10, NOT greater, so JG does
    // NOT fire -- proceeds) into the full SUCCESS arm with shuttle_slot==0 and a non-mother cfg type,
    // to pin the calls that fire UNCONDITIONALLY (pips, construction_complete) as distinct from the
    // ones gated by slot!=0 / mother-type (which must NOT fire here).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u = setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/0);
        // Poison the roster slot's shuttle_slot BEFORE the call so a value of 0 afterward proves the
        // unconditional `b.shuttle_slot = u.shuttle_slot` write actually ran (0 is also the untouched
        // default, so without the poison this check would pass vacuously).
        fx.b(PLAYER, 42).shuttle_slot = 0xAB;
        sim_view  v                   = fx.view();
        sim_store own                 = fx.store();
        own.tick_budget()             = 10.0;
        g_construct_result            = 42;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck(g_corner_calls.size() == 1 && g_corner_calls[0].proto_id == PROTO_ID &&
               g_corner_calls[0].center_x == u.x && g_corner_calls[0].center_y == u.y,
           "T3: calc_placement_corner_from_center(proto, u.x, u.y) -- AROUND THE UNIT'S OWN POSITION (no "
           "neighbor probe, unlike deploy_approach), 0x00481b2b-0x00481b53");
        ck(g_construct_calls.size() == 1 && g_construct_calls[0].param_1 == 0 &&
               g_construct_calls[0].y_b == g_corner_row_out && g_construct_calls[0].player == PLAYER &&
               (uint32_t)g_construct_calls[0].param_4 == 2u && g_construct_calls[0].x_b == (uint32_t)g_corner_col_out &&
               g_construct_calls[0].building_id == (uint32_t)EQUIVALENT,
           "T3: construct_finalize(0, corner.row, player, 2, corner.col, equivalent) register/stack layout, "
           "0x00481b58-0x00481b83");
        building &b = fx.b(PLAYER, 42);
        ck_eq_d(b.energy, 100.0,
                "T3: b.energy = (u.energy/cfg_energy)*cfg_bldg_energy = (25/50)*200, 0x00481bc3-0x00481be8 -- "
                "division-then-multiply order (a swapped order gives 0.0025, not 100.0)");
        ck(g_pips_calls.size() == 1 && g_pips_calls[0].player == PLAYER && g_pips_calls[0].building_id == 42u,
           "T3: bldg_update_charge_pips(player, building_id) fires UNCONDITIONALLY (slot==0, non-mother type "
           "here), 0x00481bee-0x00481bfd");
        ck_eq((uint32_t)b.shuttle_slot, 0u,
              "T3: b.shuttle_slot = u.shuttle_slot (0) -- UNCONDITIONAL write, poisoned to 0xAB beforehand, "
              "0x00481bfd-0x00481c1c");
        prod_shuttle_slot &s0 = fx.store().prod_shuttle_slot_at(PLAYER, 0);
        ck_eq((uint32_t)(uint16_t)s0.status, 0u,
              "T3: slot==0 -- the shuttle-record bind block is SKIPPED, 0x00481c2e-0x00481c32 JZ");
        ck(g_reelect_calls.empty(), "T3: cfg type 0 is neither heli-mother value -- reelect never called");
        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX], 0u,
              "T3: non-mother type -- primary_mother_bldg untouched, stays fixture default 0");
        ck_eq((uint32_t)fx.population[PLAYER].human, 107u,
              "T3: population.human += cfg human (100+7), 0x00481db0-0x00481dd6");
        ck_eq((uint32_t)(int32_t)fx.population[PLAYER].human_in_field, (uint32_t)(int32_t)43,
              "T3: population.human_in_field -= cfg human (50-7), 0x00481dd6-0x00481df6");
        ck(g_complete_calls.size() == 1 && g_complete_calls[0].player == PLAYER &&
               g_complete_calls[0].building_index == 42u,
           "T3: bldg_construction_complete(player, building_id, ...) fires UNCONDITIONALLY (param_3/param_4 "
           "are the PROVEN-dead register leak -- not asserted, per the header's RESOLVED note), "
           "0x00481dfc-0x00481e06");
        ck(g_snd_play_at_calls.empty() && g_fx_anim_calls.empty(),
           "T3: the FAILURE arm's calls do not fire on the success path, 0x00481e0b JMP over 0x00481e10-0x00481f0d");
        ck(g_unlink_calls.size() == 1 && g_unlink_calls[0].player == PLAYER &&
               g_unlink_calls[0].unit_index == (uint16_t)UNIT_INDEX,
           "T3: shared-tail unit_unlink_tile(player, unit_index), 0x00481f12-0x00481f25");
        ck(g_fow_calls.size() == 1 && g_fow_calls[0].x == 17 && g_fow_calls[0].y == 23 && g_fow_calls[0].radius == 12,
           "T3: shared-tail fow_remove_sight at the UNCHANGED position (17,23) -- this function never writes "
           "unit.x/y, 0x00481f25-0x00481f5a");
        ck(g_teardown_calls.size() == 1 && g_teardown_calls[0].player == PLAYER,
           "T3: shared-tail unit_teardown(player, unit_index), 0x00481f5f-0x00481f6d");
        ck(order_is({"corner", "construct", "pips", "complete", "unlink", "fow", "teardown"}),
           "T3: full call ORDER for the slot==0/non-mother success path");
    }

    // =================================================================================================
    // T4 -- success arm, GATE 3a (slot != 0): the shuttle-slot record bind. building_id (55) and the
    // building's OWN building_id FIELD (777, pre-seeded distinctly) must land in DIFFERENT output
    // fields (src_building_index vs type_ref_id) -- a translation that confused "the roster index" with
    // "the field of the same name" would swap or duplicate these.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/4, /*cfg_type=*/0);
        fx.b(PLAYER, 55).building_id = 777; // the FIELD, distinct from the roster index 55 itself
        sim_view  v                  = fx.view();
        sim_store own                = fx.store();
        own.tick_budget()            = 10.0;
        g_construct_result           = 55;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq((uint32_t)fx.b(PLAYER, 55).shuttle_slot, 4u, "T4: b.shuttle_slot = u.shuttle_slot (4)");
        prod_shuttle_slot &s = fx.store().prod_shuttle_slot_at(PLAYER, 4);
        ck_eq((uint32_t)(uint16_t)s.status, 0xcau, "T4: s.status = 0xca, 0x00481c4a");
        ck_eq((uint32_t)(int16_t)s.src_building_index, 55u,
              "T4: s.src_building_index = building_id (the ROSTER INDEX, 55), 0x00481c69-0x00481c6c");
        ck_eq((uint32_t)s.type_ref_id, 777u,
              "T4: s.type_ref_id = b.building_id (the FIELD, 777 -- NOT the roster index 55), "
              "0x00481c73-0x00481ca6");
    }

    // =================================================================================================
    // T5 -- success arm, GATE 3b (A_HELI_MOTHER) with BOTH inner gates true: primary_mother_bldg[planet]
    // unset (0) gets set to building_id; primary_mother_unit[planet] matches unit_index, gets cleared,
    // and mother_reelect_primary(player, corner.col, corner.row) fires -- corner.col/row seeded to
    // DISTINCT values (31/47) so a swapped-argument translation disagrees here.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/UNIT_TYPE_A_HELI_MOTHER);
        fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX] = 0;
        fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX] = UNIT_INDEX;
        sim_view  v                                           = fx.view();
        sim_store own                                         = fx.store();
        own.tick_budget()                                     = 10.0;
        g_construct_result                                    = 63;
        g_corner_col_out                                      = 31;
        g_corner_row_out                                      = 47;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX], 63u,
              "T5: primary_mother_bldg[planet] (was 0) set to building_id, 0x00481d15-0x00481d51");
        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX], 0u,
              "T5: primary_mother_unit[planet] cleared (unit_index matched), 0x00481d7d-0x00481d94");
        ck(g_reelect_calls.size() == 1 && g_reelect_calls[0].player == PLAYER &&
               g_reelect_calls[0].x == 31 && g_reelect_calls[0].y == 47,
           "T5: mother_reelect_primary(player, corner.col=31, corner.row=47) -- REUSES construct_finalize's "
           "own corner pair, not re-derived; a col/row swap would read (47,31), 0x00481d9e-0x00481dab");
        ck(order_is({"corner", "construct", "pips", "reelect", "complete", "unlink", "fow", "teardown"}),
           "T5: reelect fires between pips and complete");
    }

    // =================================================================================================
    // T6 -- success arm, H_HELI_MOTHER (the OTHER of the two OR'd type values), bldg-gate FALSE (already
    // set to 999) / unit-gate TRUE: primary_mother_bldg must stay 999 (NOT overwritten with the new
    // building_id) while primary_mother_unit still clears and reelect still fires -- the two inner gates
    // are independent ifs, not an if/else.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/UNIT_TYPE_H_HELI_MOTHER);
        fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX] = 999;
        fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX] = UNIT_INDEX;
        sim_view  v                                           = fx.view();
        sim_store own                                         = fx.store();
        own.tick_budget()                                     = 10.0;
        g_construct_result                                    = 71;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX], 999u,
              "T6: primary_mother_bldg[planet] (already 999, != 0) NOT overwritten, 0x00481d34 JNZ skip");
        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX], 0u,
              "T6: primary_mother_unit[planet] still cleared (unit-gate independently true)");
        ck(g_reelect_calls.size() == 1, "T6: reelect still fires (unit-gate true even though bldg-gate false)");
    }

    // =================================================================================================
    // T7 -- success arm, A_HELI_MOTHER, bldg-gate TRUE (unset) / unit-gate FALSE (mismatch): the
    // opposite split from T6 -- primary_mother_bldg DOES get set, but primary_mother_unit is untouched
    // and reelect does NOT fire.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/UNIT_TYPE_A_HELI_MOTHER);
        fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX] = 0;
        fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX] = UNIT_INDEX + 1; // mismatch
        sim_view  v                                           = fx.view();
        sim_store own                                         = fx.store();
        own.tick_budget()                                     = 10.0;
        g_construct_result                                    = 83;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX], 83u,
              "T7: primary_mother_bldg[planet] (was 0) set to building_id, bldg-gate independently true");
        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX], (uint32_t)(UNIT_INDEX + 1),
              "T7: primary_mother_unit[planet] untouched (mismatch), 0x00481d7b JNZ skip");
        ck(g_reelect_calls.empty(), "T7: reelect NOT called on a primary_mother_unit mismatch");
    }

    // =================================================================================================
    // T8 -- success arm, cfg type is NEITHER A_HELI_MOTHER nor H_HELI_MOTHER, even though BOTH inner
    // gates are individually primed true (bldg unset, unit matches) -- pins the OUTER type gate itself:
    // without it this case would set primary_mother_bldg and fire reelect, so seeing neither confirms
    // the type check actually runs and actually gates the whole block.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/UNIT_TYPE_A_GROUND);
        fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX] = 0;
        fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX] = UNIT_INDEX;
        sim_view  v                                           = fx.view();
        sim_store own                                         = fx.store();
        own.tick_budget()                                     = 10.0;
        g_construct_result                                    = 91;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_bldg[PLANET_INDEX], 0u,
              "T8: A_GROUND is neither mother type -- primary_mother_bldg untouched despite the inner gate "
              "being primed true, 0x00481cd6-0x00481d0f");
        ck_eq((uint32_t)fx.profiles[PLAYER].primary_mother_unit[PLANET_INDEX], (uint32_t)UNIT_INDEX,
              "T8: primary_mother_unit likewise untouched");
        ck(g_reelect_calls.empty(), "T8: reelect NOT called -- the whole heli-mother block is skipped");
        ck(order_is({"corner", "construct", "pips", "complete", "unlink", "fow", "teardown"}),
           "T8: no 'reelect' in the order -- confirms the block is skipped, not just its writes");
    }

    // =================================================================================================
    // T9 -- FAILURE arm (construct_finalize returns 0), GATE 4 (*sim_active != 0) true: masked_x/
    // masked_y computed from bw_mask/bh_mask (NOT width_mask/height_mask), fed through fine_to_tile()
    // into the snd_play_at record; fx_anim_spawn gets the RAW game_clock (tick_budget left
    // deliberately nonzero afterward so "raw" and "elapsed = game_clock - tick_budget" are numerically
    // distinguishable).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/0);
        sim_view  v        = fx.view();
        sim_store own      = fx.store();
        own.tick_budget()  = 10.0; // step_cost 3.0 -> 7.0 left over, deliberately nonzero
        fx.game_clock      = 4242.25;
        fx.sim_active      = 1;
        g_construct_result = 0;
        g_corner_col_out   = 2;
        g_corner_row_out   = 3;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        // masked_x = 0x3ff & (width(4)*16 + col(2)*32) = 0x3ff & (64+64) = 128
        // masked_y = 0x1ff & (height(6)*16 + row(3)*32) = 0x1ff & (96+96) = 192
        ck(g_snd_play_at_calls.size() == 1 && g_snd_play_at_calls[0].sound_id == 555 &&
               g_snd_play_at_calls[0].tile_col == 4 && g_snd_play_at_calls[0].tile_row == 6,
           "T9: snd_play_at(cfg sound_explo=555, fine_to_tile(masked_x)=128/32=4, "
           "fine_to_tile(masked_y)=192/32=6), 0x00481e10-0x00481ed1 -- pins bw_mask/bh_mask "
           "(fine-space), not width_mask/height_mask");
        ck(g_fx_anim_calls.size() == 1 && g_fx_anim_calls[0].x == 128u && g_fx_anim_calls[0].y == 192u &&
               g_fx_anim_calls[0].param_3 == 888u && g_fx_anim_calls[0].param_5 == 1u,
           "T9: fx_anim_spawn(masked_x=128, masked_y=192, frame_at(anim,7)=888, game_clock, 1), "
           "0x00481ed6-0x00481f0d");
        ck_eq_d(g_fx_anim_calls[0].param_4, 4242.25,
                "T9: fx_anim_spawn's param_4 is the RAW game_clock (4242.25), NOT game_clock-tick_budget "
                "(which would be 4235.25 here) -- the asm pushes GAME_CLOCK's two raw dwords with no "
                "intervening FSUB, 0x00481edc-0x00481ee8");
        ck(g_pips_calls.empty() && g_complete_calls.empty() && g_reelect_calls.empty(),
           "T9: success-arm-only calls do not fire on the failure path");
        ck(order_is({"corner", "construct", "snd_play_at", "fx_anim", "unlink", "fow", "teardown"}),
           "T9: failure-arm call ORDER with sim_active!=0");
    }

    // =================================================================================================
    // T10 -- FAILURE arm, GATE 4 (*sim_active == 0): snd_play_at must NOT fire, but
    // fx_anim_spawn STILL fires -- it does not share sim_active's gate (a naive oracle that gated
    // both failure-arm calls together would pass T9 and miss this).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        setup(fx, /*elevation=*/11, /*shuttle_slot=*/0, /*cfg_type=*/0);
        sim_view  v        = fx.view();
        sim_store own      = fx.store();
        own.tick_budget()  = 10.0;
        fx.game_clock      = 555.5;
        fx.sim_active      = 0;
        g_construct_result = 0;
        g_corner_col_out   = 2;
        g_corner_row_out   = 3;

        detail::unit_state_deploy_to_building(v, own, g_calls);

        ck(g_snd_play_at_calls.empty(),
           "T10: sim_active==0 -- snd_play_at does NOT fire, 0x00481e7c-0x00481e83 JZ");
        ck(g_fx_anim_calls.size() == 1,
           "T10: fx_anim_spawn fires REGARDLESS of sim_active -- it is UNCONDITIONAL in the failure arm, "
           "0x00481ed6-0x00481f0d is reached whether or not 0x00481e85-0x00481ed1 ran");
        ck(order_is({"corner", "construct", "fx_anim", "unlink", "fow", "teardown"}),
           "T10: failure-arm call ORDER with sim_active==0 -- no 'snd_play_at' token");
    }
}

} // namespace mh::sim::test
