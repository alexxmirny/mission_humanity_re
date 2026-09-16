//
// sim_unit_state_production_ready_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_production_ready @0x00486106 (sim/sim_unit_state_misc2.h/.cpp, RI-SIM /
// SIM1-G3).
//
// arm_ready:false -- shadow_region_closure.py's closure reaches 777 functions / 401 undeclared
// regions (the standard gfx/input/snd DLL-lifecycle escape this batch's remaining rows share), so a
// per-call shadow arm cannot evidence this site. This offline oracle is its evidence.
//
// EXPECTED BEHAVIOUR from tmp/decomp_sim/llm_strat_unit_state_production_ready_00486106.asm,
// cross-checked against the header banner's own derivation (sim_unit_state_misc2.h):
//   0x00486123-0x00486127: shuttle_slot = u.shuttle_slot, CACHED FIRST -- before unlink_tile/teardown
//     can invalidate anything.
//   0x0048612a-0x0048613d: unit_unlink_tile(player, unit_index).
//   0x0048613d-0x00486177: fow_remove_sight(player, u.x, u.y, cfg_units[proto_id].sight).
//   0x00486177-0x004861fc: if cfg_units[proto_id].human != 0 -- give housing population back:
//     pop_stats[player].human += human; pop_stats[player].human_in_field -= human;
//     population_remove(player, human). human==0 skips all three effects.
//   0x004861fc-0x00486280: UNCONDITIONALLY (regardless of the human gate) -- if a FRESH read of
//     units[player][unit_index].unit_proto_id's cfg type is UNIT_TYPE_A_HELI_MOTHER(0x13) or
//     UNIT_TYPE_H_HELI_MOTHER(0x14): prod_shuttle_slots[player][shuttle_slot].is_heli_mother_pending=1.
//   0x00486280-0x00486296: UNCONDITIONALLY -- prod_shuttle_slots[player][shuttle_slot].status = 200.
//   0x0048629f-0x004862b7: unit_teardown(player, unit_index); set_event(EVENT_INFO_REFRESH=6).
//
#include "sim/sim_unit_state_misc2.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct fow_call {
    uint32_t player;
    int32_t  x, y;
    uint8_t  sight;
};
std::vector<fow_call> g_fow_calls;
void                  rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t sight) {
    g_fow_calls.push_back({player, x, y, sight});
}

struct unlink_call {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<unlink_call> g_unlink_calls;
// T4's caching-order probe: when non-null, rec_unit_unlink_tile mutates this unit's shuttle_slot
// AFTER recording the call, to prove production_ready must have already cached the pre-call value.
unit   *g_mutate_shuttle_slot_target = nullptr;
uint8_t g_mutate_shuttle_slot_to     = 0;
void    rec_unit_unlink_tile(uint32_t player, uint16_t unit_index) {
    g_unlink_calls.push_back({player, unit_index});
    if (g_mutate_shuttle_slot_target) g_mutate_shuttle_slot_target->shuttle_slot = g_mutate_shuttle_slot_to;
}

struct pop_remove_call {
    uint32_t player;
    int32_t  count;
};
std::vector<pop_remove_call> g_pop_remove_calls;
void                         rec_population_remove(uint32_t player, int32_t count) {
    g_pop_remove_calls.push_back({player, count});
}

struct teardown_call {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<teardown_call> g_teardown_calls;
void                       rec_unit_teardown(uint32_t player, uint16_t unit_index) {
    g_teardown_calls.push_back({player, unit_index});
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_set_event(uint32_t type) {
    g_set_event_calls.push_back(type);
    return 0;
}

// Unused by production_ready but required by the shared calls struct.
void    rec_unused_set_state(uint16_t) {}
int32_t rec_unused_tiles_adjacent(int32_t, int32_t, int32_t, int32_t) { return 0; }
void    rec_unused_set_state_order(uint16_t, uint16_t) {}
int32_t rec_unused_path_make_single_step(uint32_t, int32_t) { return 0; }

const unit_state_misc2_calls g_calls = {
    &rec_unused_set_state,
    &rec_unused_tiles_adjacent,
    &rec_unused_set_state_order,
    &rec_unused_path_make_single_step,
    &rec_unit_unlink_tile,
    &rec_fow_remove_sight,
    &rec_population_remove,
    &rec_unit_teardown,
    &rec_set_event,
};

constexpr uint32_t PLAYER       = 1;
constexpr int32_t  UNIT_INDEX   = 3;
constexpr uint16_t PROTO_ID     = 5;
constexpr int32_t  SHUTTLE_SLOT = 7;

void reset_recorders() {
    g_fow_calls.clear();
    g_unlink_calls.clear();
    g_pop_remove_calls.clear();
    g_teardown_calls.clear();
    g_set_event_calls.clear();
    g_mutate_shuttle_slot_target = nullptr;
}

prod_shuttle_slot &slot_of(sim_fixture &fx) {
    return fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT];
}

unit &make_unit(sim_fixture &fx, uint32_t type, int32_t human) {
    unit &u         = fx.u(PLAYER, UNIT_INDEX);
    u.unit_proto_id = PROTO_ID;
    u.shuttle_slot  = static_cast<uint8_t>(SHUTTLE_SLOT);
    u.x             = 30;
    u.y             = 40;

    cfg_unit &cu = fx.cfg_units[PROTO_ID];
    cu.sight     = 9;
    cu.type      = type;
    cu.human     = human;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    return u;
}

} // namespace

void run_unit_state_production_ready_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- human==0, non-heli-mothership type: no population effects, is_heli_mother_pending stays 0,
    // status still forced to 200, unlink/fow/teardown/set_event all fire exactly once with the right
    // args.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                              = make_unit(fx, /*type=*/0x01, /*human=*/0);
        fx.population[PLAYER].human          = 10;
        fx.population[PLAYER].human_in_field = 20;
        slot_of(fx).is_heli_mother_pending   = 0;
        slot_of(fx).status                   = 0;

        sim_store own = fx.store();
        detail::unit_state_production_ready(fx.view(), own, g_calls);

        ck(g_unlink_calls.size() == 1 && g_unlink_calls[0].player == PLAYER &&
               g_unlink_calls[0].unit_index == UNIT_INDEX,
           "T1: unit_unlink_tile(player, unit_index) called once, 0x0048612a");
        ck(g_fow_calls.size() == 1 && g_fow_calls[0].player == PLAYER && g_fow_calls[0].x == u.x &&
               g_fow_calls[0].y == u.y && g_fow_calls[0].sight == 9,
           "T1: fow_remove_sight(player, u.x, u.y, cfg_units[proto].sight) called once, 0x0048613d");
        ck(g_pop_remove_calls.empty(), "T1: human==0 -- population_remove NOT called");
        ck_eq(fx.population[PLAYER].human, 10, "T1: human==0 -- pop_stats.human untouched");
        ck_eq(fx.population[PLAYER].human_in_field, 20, "T1: human==0 -- pop_stats.human_in_field untouched");
        ck_eq(slot_of(fx).is_heli_mother_pending, 0, "T1: non-heli-mothership type -- pending stays 0");
        ck_eq((int32_t)slot_of(fx).status, 200, "T1: status forced to 200 regardless, 0x00486280-0x00486296");
        ck(g_teardown_calls.size() == 1 && g_teardown_calls[0].player == PLAYER &&
               g_teardown_calls[0].unit_index == UNIT_INDEX,
           "T1: unit_teardown(player, unit_index) called once, 0x0048629f");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 6,
           "T1: set_event(EVENT_INFO_REFRESH=6) called once, 0x004862b2");
    }

    // =================================================================================================
    // T2 -- human!=0: pop_stats.human += human, human_in_field -= human, population_remove(player,
    // human) called with the SAME human value.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx, /*type=*/0x01, /*human=*/4);
        fx.population[PLAYER].human          = 10;
        fx.population[PLAYER].human_in_field = 20;

        sim_store own = fx.store();
        detail::unit_state_production_ready(fx.view(), own, g_calls);

        ck_eq(fx.population[PLAYER].human, 14, "T2: pop_stats.human += 4 (10 -> 14), 0x004861d6");
        ck_eq(fx.population[PLAYER].human_in_field, 16, "T2: pop_stats.human_in_field -= 4 (20 -> 16), 0x004861e8");
        ck(g_pop_remove_calls.size() == 1 && g_pop_remove_calls[0].player == PLAYER &&
               g_pop_remove_calls[0].count == 4,
           "T2: population_remove(player, human=4) called once, 0x004861f4");
    }

    // =================================================================================================
    // T3a -- type == UNIT_TYPE_A_HELI_MOTHER (0x13): is_heli_mother_pending set to 1, UNCONDITIONALLY
    // of the human gate (human==0 here, isolating this effect from T2's).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx, /*type=*/0x13, /*human=*/0);
        slot_of(fx).is_heli_mother_pending = 0;

        sim_store own = fx.store();
        detail::unit_state_production_ready(fx.view(), own, g_calls);

        ck_eq(slot_of(fx).is_heli_mother_pending, 1,
              "T3a: UNIT_TYPE_A_HELI_MOTHER(0x13) sets is_heli_mother_pending=1, 0x004861fc-0x00486280");
    }

    // =================================================================================================
    // T3b -- type == UNIT_TYPE_H_HELI_MOTHER (0x14): same effect, the OTHER accepted type value.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx, /*type=*/0x14, /*human=*/0);
        slot_of(fx).is_heli_mother_pending = 0;

        sim_store own = fx.store();
        detail::unit_state_production_ready(fx.view(), own, g_calls);

        ck_eq(slot_of(fx).is_heli_mother_pending, 1, "T3b: UNIT_TYPE_H_HELI_MOTHER(0x14) also sets pending=1");
    }

    // =================================================================================================
    // T4 -- shuttle_slot is CACHED BEFORE unit_unlink_tile/fow_remove_sight/unit_teardown run: a
    // callee that mutates the unit's shuttle_slot field must NOT change which prod_shuttle_slot record
    // gets written. Proves the caching-order claim, not just its final value.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit             &u                                                               = make_unit(fx, /*type=*/0x01, /*human=*/0);
        constexpr int32_t OTHER_SLOT                                                      = 2;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + OTHER_SLOT].status = 0;

        u.shuttle_slot               = static_cast<uint8_t>(SHUTTLE_SLOT); // starting value the cache should capture
        g_mutate_shuttle_slot_target = &u;
        g_mutate_shuttle_slot_to     = static_cast<uint8_t>(OTHER_SLOT);

        sim_store own = fx.store();
        detail::unit_state_production_ready(fx.view(), own, g_calls);

        ck_eq((int32_t)slot_of(fx).status, 200,
              "T4: the ORIGINAL shuttle_slot's record is updated even though unlink_tile mutated the "
              "unit's shuttle_slot field afterward -- proves the value was cached before the call, "
              "0x00486123-0x00486127");
        ck_eq((int32_t)fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + OTHER_SLOT].status, 0,
              "T4: the OTHER (post-mutation) slot is NOT touched");
    }
}

} // namespace mh::sim::test
