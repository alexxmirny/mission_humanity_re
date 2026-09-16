//
// sim_unit_teardown_mapped_selftest.cpp -- `simtest` cases for llm_strat_unit_teardown_mapped
// @0x0048753e (sim/sim_unit_teardown_mapped.h/.cpp, SIM1-G1).
//
// SCOPE (11 steps, address ranges per the header's own derivation):
//   T1  path_slot_id!=0xff gate (0x0048756e CMP / 0x00487575 JZ), llm_strat_path_free_slot call args
//       (0x00487577-0x0048757e).
//   T2  housing_stats[player].used_soldiers -= 1, UNCONDITIONAL (0x0048758a DEC), exact row.
//   T3  unit.energy > 0.0 gate (x87 FLDZ 0x004875a3 / FCOMP 0x004875a5 / FNSTSW 0x004875ab / SAHF
//       0x004875ad / JNC 0x004875ae): this unit's OWN energy zeroed (0x004875c3/0x004875cd) THEN
//       units[player][SLOT 0].energy += -1.0 (0x004875e1-0x004875ed, DAT_00501482) -- the unit_index
//       term is DROPPED from the second address computation (0x004875d7-0x004875db only multiplies by
//       the player stride), so this is the slot-0 accumulator, not the target unit's own record.
//   T4  tile_objects[x][y].building=0 (0x0048763b, word) / .class_owner=0 (0x00487652, byte), x/y read
//       from the TARGET unit's own record post-energy-zero (0x00487606/0x00487623), neighbours and the
//       x/y-transposed cell untouched.
//   T5  passable[x][y] = unit.origin_tile_was_passable (0x00487675 read / 0x0048767b write).
//   T6  llm_strat_fow_remove_sight(player, x, y, cfg_units[unit.unit_proto_id].sight) call args
//       (0x00487659-0x004876b2; proto read confirmed by Ghidra's own inline comment at 0x00487694).
//   T7  session_mode==SESSION_MP_LOCKSTEP(3) gate (0x004876b7 CMP / 0x004876be JNZ):
//       unit.ai_group_index=0 (0x004876d3, word) only on that side.
//   T8  local-player branch (player==PlayerSide, 0x004876df CMP AX / 0x004876e6 JNZ) vs the
//       click-select branch are IF/ELSE-IF (0x004876fa JMP skips branch B entirely when A fires) --
//       when BOTH conditions would match, only ctrlgroup_leave+SetEvent fire, never the click-target
//       clear.
//   T9  click-select branch (0x004876fc-0x0048772b): flags==(player|0x80) (0x0048770b CMP/0x0048770d
//       JNZ) AND id==unit_index (0x00487716 CMP/0x00487719 JZ) -- both sub-gates, both sides.
//   T10 neither branch open -> no SetEvent/ctrlgroup_leave/click-clear at all.
//   T11 profile.units_alive[planet] -= 1, UNCONDITIONAL (0x00487745 DEC); ==0 gate
//       (0x0048775f CMP/0x00487766 JNZ) guards llm_strat_player_presence_lost(player, 0)
//       (0x0048776e CALL), both sides.
//   T12 llm_strat_unit_set_state_of(player, unit_index, 4) args (0x0048777f CALL); unit.move_microstep=0
//       (0x00487797, dword) happens AFTER that call in program order (no branch could reorder them).
//   T13 llm_strat_ai_notify_object_removed(player|0x80, unit_index, 0) ALWAYS, LAST (0x004877ae CALL).
//   T14/T15 full CALL ORDER across the 5 (of 7 seam members) calls that can co-occur, both the
//       local-player and the click-select variant of step 8/9.
//   T16 the `player & 0xffff` masking every roster/profile/housing/seam-arg computation applies in the
//       asm (every address computation MOVZXes the WORD half of the stored 32-bit `player` local) --
//       a raw player value with garbage high bits must resolve to the same low-16-bit row everywhere.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_teardown_mapped_0048753e.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_teardown_mapped.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across the seven unit_teardown_mapped_calls members ----------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (7, one per unit_teardown_mapped_calls member) ---------------------------
struct PathFreeCall {
    uint16_t player;
    int32_t  index;
};
std::vector<PathFreeCall> g_path_free_calls;
void                      rec_path_free_slot(uint16_t player, int32_t unit_index) {
    tr("path_free_slot");
    g_path_free_calls.push_back({player, unit_index});
}

struct FowCall {
    uint32_t player;
    int32_t  x, y;
    uint8_t  radius;
};
std::vector<FowCall> g_fow_calls;
void                 rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    tr("fow_remove_sight");
    g_fow_calls.push_back({player, x, y, radius});
}

std::vector<uint32_t> g_ctrlgroup_leave_calls;
void                  rec_unit_ctrlgroup_leave(uint32_t unit_index) {
    tr("unit_ctrlgroup_leave");
    g_ctrlgroup_leave_calls.push_back(unit_index);
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_set_event(uint32_t type) {
    tr("set_event");
    g_set_event_calls.push_back(type);
    return 0;
}

struct PresenceLostCall {
    uint32_t player;
    uint32_t mode;
};
std::vector<PresenceLostCall> g_presence_lost_calls;
uint32_t                      rec_player_presence_lost(uint32_t player, uint32_t mode) {
    tr("player_presence_lost");
    g_presence_lost_calls.push_back({player, mode});
    return 0;
}

// Global watch pointer: lets rec_unit_set_state_of observe the TARGET unit's move_microstep field AT
// THE MOMENT of the call, so T12 can prove the 0x00487797 write happens AFTER the call rather than
// before (a reordered translation would not be distinguishable any other way -- both orders leave the
// same value after the function returns).
unit   *g_watch_unit_ptr               = nullptr;
int32_t g_move_microstep_at_state_call = -12345; // sentinel: "not observed this run"

struct SetStateOfCall {
    int32_t player;
    int32_t unit_index;
    int16_t state;
};
std::vector<SetStateOfCall> g_set_state_of_calls;
void                        rec_unit_set_state_of(int32_t player, int32_t unit_index, int16_t state) {
    tr("unit_set_state_of");
    g_set_state_of_calls.push_back({player, unit_index, state});
    if (g_watch_unit_ptr) g_move_microstep_at_state_call = g_watch_unit_ptr->move_microstep;
}

struct AiNotifyCall {
    uint32_t flags;
    uint32_t object_index;
    int32_t  hard_remove;
};
std::vector<AiNotifyCall> g_ai_notify_calls;
void                      rec_ai_notify_object_removed(uint32_t flags, uint32_t object_index,
                                                       int32_t hard_remove) {
    tr("ai_notify_object_removed");
    g_ai_notify_calls.push_back({flags, object_index, hard_remove});
}

const unit_teardown_mapped_calls g_calls = {
    &rec_path_free_slot,
    &rec_fow_remove_sight,
    &rec_unit_ctrlgroup_leave,
    &rec_set_event,
    &rec_player_presence_lost,
    &rec_unit_set_state_of,
    &rec_ai_notify_object_removed,
};

void reset_observations() {
    g_trace.clear();
    g_path_free_calls.clear();
    g_fow_calls.clear();
    g_ctrlgroup_leave_calls.clear();
    g_set_event_calls.clear();
    g_presence_lost_calls.clear();
    g_set_state_of_calls.clear();
    g_ai_notify_calls.clear();
    g_move_microstep_at_state_call = -12345;
    g_watch_unit_ptr               = nullptr;
}

// ---- fixed identities used throughout ---------------------------------------------------------------
// Nonzero, distinct, away from the origin so an off-by-one/transposition in the tile-index arithmetic
// or a player/index mix-up is separable.
constexpr uint32_t TARGET_PLAYER = 3;
constexpr uint32_t TARGET_INDEX  = 5; // != 0, so it can be told apart from the slot-0 accumulator
constexpr uint8_t  TARGET_X      = 40;
constexpr uint8_t  TARGET_Y      = 90; // != TARGET_X, so an x/y swap is separable
constexpr uint16_t TARGET_PROTO  = 12;
constexpr uint32_t GUARD_PLAYER  = 1; // a player row this call never touches
constexpr int32_t  GUARD_INDEX   = 2; // a unit slot this call never touches
constexpr int32_t  OTHER_INDEX   = 6; // same player, a DIFFERENT unit index from TARGET_INDEX

// ---- fixture seeding ---------------------------------------------------------------------------------
struct Params {
    uint32_t player = TARGET_PLAYER;
    uint32_t index  = TARGET_INDEX;

    uint8_t path_slot_id = 0xff; // 0xff == none -- gate closed by default
    double  energy       = 5.0;  // > 0.0 -- gate open by default

    uint8_t x = TARGET_X, y = TARGET_Y;
    uint8_t origin_tile_was_passable = 0x5a;

    uint16_t unit_proto_id = TARGET_PROTO;
    uint8_t  sight         = 7;

    uint16_t ai_group_index = 0x99; // nonzero sentinel

    int32_t session_mode = SESSION_SP; // 1, NOT lockstep -- gate closed by default
    int16_t player_side  = 40;         // != player by default -- local-player branch closed

    uint16_t click_flags = 0; // mismatched by default -- click-select branch closed
    uint16_t click_id    = 0;

    int32_t units_alive  = 5; // decrements to 4, not the ==0 threshold, by default
    int32_t planet_index = 2;

    double slot0_energy = 50.0; // units[player][0].energy, seeded DISTINCT from `energy` above
};

void seed_and_run(sim_fixture &fx, const Params &p) {
    fx.reset();

    // guard unit slot (a wholly different player+index) -- must read back UNCHANGED.
    unit &g          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id  = 77;
    g.path_slot_id   = 0x11;
    g.x              = 200;
    g.y              = 210;
    g.energy         = 999.0;
    g.ai_group_index = 0x1234;
    g.move_microstep = 555;

    // same-player, DIFFERENT-index guard -- must read back UNCHANGED (the only per-unit writes this
    // function makes are on `own.unit_at(p, unit_index)`, not on any other index).
    unit &og         = fx.u(p.player, OTHER_INDEX);
    og.energy        = 321.0;
    og.unit_proto_id = 88;
    og.x             = 210;
    og.y             = 220;

    // housing rows: target player's row (decremented) and a guard row (untouched).
    fx.unit_housing[p.player].used_soldiers     = 10;
    fx.unit_housing[GUARD_PLAYER].used_soldiers = 40;

    // profile rows: target (player, planet) decremented; same-player-other-planet and
    // other-player-same-planet must stay untouched.
    fx.profiles[p.player].units_alive[p.planet_index]            = p.units_alive;
    fx.profiles[p.player].units_alive[(p.planet_index + 1) % 32] = 999;
    fx.profiles[GUARD_PLAYER].units_alive[p.planet_index]        = 888;

    // tiles: the target cell (must clear), its four neighbours, and the x/y-TRANSPOSED cell (all must
    // stay untouched -- the transposed cell catches an x<->y swap the neighbour checks alone would not).
    auto seed_tile = [&](uint8_t tx, uint8_t ty, uint16_t building, uint8_t owner) {
        tile_object &t = fx.t(tx, ty);
        t.building     = building;
        t.class_owner  = owner;
    };
    seed_tile(p.x, p.y, 0xbeef, 0xaa);
    seed_tile((uint8_t)(p.x + 1), p.y, 0x1111, 0x22);
    seed_tile(p.x, (uint8_t)(p.y + 1), 0x1111, 0x22);
    seed_tile((uint8_t)(p.x - 1), p.y, 0x1111, 0x22);
    seed_tile(p.x, (uint8_t)(p.y - 1), 0x1111, 0x22);
    seed_tile(p.y, p.x, 0x1111, 0x22); // transposed guard

    fx.passable[((uint32_t)p.x << 8) | p.y]       = 0x11; // pre-value at the target cell
    fx.passable[(((uint32_t)p.x + 1) << 8) | p.y] = 0x77; // guard neighbour

    // target unit.
    unit &u                    = fx.u(p.player, p.index);
    u.path_slot_id             = p.path_slot_id;
    u.energy                   = p.energy;
    u.x                        = p.x;
    u.y                        = p.y;
    u.origin_tile_was_passable = p.origin_tile_was_passable;
    u.unit_proto_id            = p.unit_proto_id;
    u.ai_group_index           = p.ai_group_index;
    u.move_microstep           = 77; // sentinel -- must read back 0 after every run

    // slot-0 accumulator (index != TARGET_INDEX, so this is a genuinely different record) and a
    // guard slot-0 on a DIFFERENT player.
    fx.u(p.player, 0).energy     = p.slot0_energy;
    fx.u(GUARD_PLAYER, 0).energy = 111.0;

    fx.cfg_units[p.unit_proto_id].sight = p.sight;

    fx.session_mode              = p.session_mode;
    fx.player_side               = p.player_side;
    fx.click_select_target_flags = p.click_flags;
    fx.click_select_target_id    = p.click_id;
    fx.planet_index              = p.planet_index;

    reset_observations();
    g_watch_unit_ptr = &u;

    sim_store own = fx.store();
    detail::unit_teardown_mapped(fx.view(), own, g_calls, p.player, p.index);
}

} // namespace

void run_unit_teardown_mapped_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- path_slot_id!=0xff gate (0x0048756e CMP / 0x00487575 JZ): 0xff (none) vs assigned, plus the
    // exact args (player low-word, unit_index) when it fires (0x00487577-0x0048757e).
    // =================================================================================================
    {
        Params p;
        p.path_slot_id = 0xff;
        seed_and_run(fx, p);
        ck(g_path_free_calls.empty(),
           "T1a: path_slot_id==0xff -- path_free_slot does NOT fire (0x00487575 JZ taken)");

        p.path_slot_id = 0x33;
        seed_and_run(fx, p);
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == TARGET_PLAYER &&
               g_path_free_calls[0].index == (int32_t)TARGET_INDEX,
           "T1b: path_slot_id!=0xff -- path_free_slot(player=3, unit_index=5) fires with the RIGHT "
           "args, not swapped (0x00487577 MOV EDX,unit_index / 0x0048757a MOVZX EAX,player / "
           "0x0048757e CALL)");
    }

    // =================================================================================================
    // T2 -- housing_stats[player].used_soldiers -= 1, UNCONDITIONAL (0x0048758a DEC), exact row; a
    // guard row on a DIFFERENT player stays untouched.
    // =================================================================================================
    {
        Params p;
        p.path_slot_id = 0xff; // isolate: no path-free call needed for this property
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.unit_housing[TARGET_PLAYER].used_soldiers, 9,
              "T2: housing_stats[player=3].used_soldiers 10 -> 9 (0x0048758a DEC, unconditional)");
        ck_eq((uint32_t)fx.unit_housing[GUARD_PLAYER].used_soldiers, 40,
              "T2: housing_stats[GUARD_PLAYER=1].used_soldiers untouched (40)");
    }

    // =================================================================================================
    // T3 -- unit.energy > 0.0 gate (0x004875a3-0x004875ae, x87 FLDZ/FCOMP/FNSTSW/SAHF/JNC): when it
    // fires, THIS unit's own energy is zeroed (0x004875c3/0x004875cd) and THEN units[player][SLOT 0]
    // (NOT [player][unit_index] -- 0x004875d7-0x004875db drops the unit_index term) gets
    // UNIT_TEARDOWN_MAPPED_ENERGY_DELTA (-1.0, DAT_00501482) added (0x004875e1-0x004875ed). Neither
    // side touches a different index or a different player's slot 0.
    // =================================================================================================
    {
        Params p;
        p.energy       = 5.0;  // > 0.0 -- fires
        p.slot0_energy = 50.0; // distinct from `energy` so a swap is separable
        seed_and_run(fx, p);
        ck_eq_d(fx.u(TARGET_PLAYER, TARGET_INDEX).energy, 0.0,
                "T3a: energy>0.0 -- this unit's OWN energy zeroed (0x004875c3/0x004875cd)");
        ck_eq_d(fx.u(TARGET_PLAYER, 0).energy, 49.0,
                "T3a: units[player][SLOT 0].energy 50.0 + (-1.0) == 49.0, NOT [player][unit_index]'s "
                "own record (0x004875d7-0x004875ed drops the unit_index term)");
        ck_eq_d(fx.u(TARGET_PLAYER, OTHER_INDEX).energy, 321.0,
                "T3a: a DIFFERENT index's energy untouched");
        ck_eq_d(fx.u(GUARD_PLAYER, 0).energy, 111.0,
                "T3a: a DIFFERENT player's slot-0 energy untouched");

        p.energy       = 0.0; // NOT > 0.0 -- gate closed (boundary)
        p.slot0_energy = 50.0;
        seed_and_run(fx, p);
        ck_eq_d(fx.u(TARGET_PLAYER, TARGET_INDEX).energy, 0.0,
                "T3b: energy==0.0 -- gate closed, own energy unchanged (still 0.0) (0x004875ae JNC taken)");
        ck_eq_d(fx.u(TARGET_PLAYER, 0).energy, 50.0, "T3b: slot-0 accumulator untouched when gate closed");

        p.energy       = -3.0; // < 0.0 -- gate closed
        p.slot0_energy = 50.0;
        seed_and_run(fx, p);
        ck_eq_d(fx.u(TARGET_PLAYER, TARGET_INDEX).energy, -3.0,
                "T3c: energy==-3.0 -- gate closed, own energy unchanged (still -3.0)");
        ck_eq_d(fx.u(TARGET_PLAYER, 0).energy, 50.0, "T3c: slot-0 accumulator untouched when gate closed");
    }

    // =================================================================================================
    // T4 -- tile_objects[x][y].building=0 (0x0048763b, word) / .class_owner=0 (0x00487652, byte) at the
    // EXACT (x,y) the target unit's own record holds (read post-energy-zero, 0x00487606/0x00487623);
    // the four neighbours and the x/y-TRANSPOSED cell stay untouched.
    // =================================================================================================
    {
        Params p;
        seed_and_run(fx, p);
        tile_object &t = fx.t(TARGET_X, TARGET_Y);
        ck_eq((uint32_t)t.building, 0, "T4: tile_objects[x=40][y=90].building cleared (0x0048763b)");
        ck_eq((uint32_t)t.class_owner, 0, "T4: tile_objects[x=40][y=90].class_owner cleared (0x00487652)");

        ck(fx.t((uint8_t)(TARGET_X + 1), TARGET_Y).building == 0x1111 &&
               fx.t((uint8_t)(TARGET_X + 1), TARGET_Y).class_owner == 0x22,
           "T4: neighbour (x+1,y) untouched");
        ck(fx.t(TARGET_X, (uint8_t)(TARGET_Y + 1)).building == 0x1111 &&
               fx.t(TARGET_X, (uint8_t)(TARGET_Y + 1)).class_owner == 0x22,
           "T4: neighbour (x,y+1) untouched");
        ck(fx.t((uint8_t)(TARGET_X - 1), TARGET_Y).building == 0x1111 &&
               fx.t((uint8_t)(TARGET_X - 1), TARGET_Y).class_owner == 0x22,
           "T4: neighbour (x-1,y) untouched");
        ck(fx.t(TARGET_X, (uint8_t)(TARGET_Y - 1)).building == 0x1111 &&
               fx.t(TARGET_X, (uint8_t)(TARGET_Y - 1)).class_owner == 0x22,
           "T4: neighbour (x,y-1) untouched");
        ck(fx.t(TARGET_Y, TARGET_X).building == 0x1111 && fx.t(TARGET_Y, TARGET_X).class_owner == 0x22,
           "T4: the x/y-TRANSPOSED cell (y,x) untouched -- catches an x<->y argument swap");
    }

    // =================================================================================================
    // T5 -- passable[x][y] = unit.origin_tile_was_passable (0x00487675 read / 0x0048767b write); the
    // (x+1,y) neighbour's passable byte is untouched.
    // =================================================================================================
    {
        Params p;
        p.origin_tile_was_passable = 0x5a;
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.passable[((uint32_t)TARGET_X << 8) | TARGET_Y], 0x5a,
              "T5: passable[x=40][y=90] == unit.origin_tile_was_passable (0x5a), not the pre-seeded "
              "0x11 (0x0048767b)");
        ck_eq((uint32_t)fx.passable[(((uint32_t)TARGET_X + 1) << 8) | TARGET_Y], 0x77,
              "T5: passable[x+1][y] untouched (0x77)");
    }

    // =================================================================================================
    // T6 -- llm_strat_fow_remove_sight(player, x, y, cfg_units[unit.unit_proto_id].sight) exact args
    // (0x00487659-0x004876b2). The proto row is the TARGET unit's own unit_proto_id (Ghidra's own
    // inline comment at 0x00487694 confirms the read), not the unit's index or a fixed row.
    // =================================================================================================
    {
        Params p;
        p.unit_proto_id = TARGET_PROTO; // seed_and_run's fx.reset() zeroes every OTHER cfg_units row
                                        // (including proto+1), so 0 vs this row's 7 already separates
                                        // "read the right proto row" from "read the wrong one".
        p.sight = 7;
        seed_and_run(fx, p);
        ck(g_fow_calls.size() == 1, "T6: fow_remove_sight fires exactly once (unconditional)");
        if (g_fow_calls.size() == 1) {
            const FowCall &c = g_fow_calls[0];
            ck_eq(c.player, TARGET_PLAYER, "T6: fow_remove_sight player arg == 3");
            ck_eq((uint32_t)c.x, TARGET_X, "T6: fow_remove_sight x arg == 40");
            ck_eq((uint32_t)c.y, TARGET_Y, "T6: fow_remove_sight y arg == 90");
            ck_eq((uint32_t)c.radius, 7,
                  "T6: fow_remove_sight radius arg == cfg_units[proto=12].sight (7), read from the "
                  "unit's OWN unit_proto_id row (every other row is 0 after reset())");
        }
    }

    // =================================================================================================
    // T7 -- session_mode==SESSION_MP_LOCKSTEP(3) gate (0x004876b7 CMP / 0x004876be JNZ): only on that
    // side does unit.ai_group_index get zeroed (0x004876d3, word).
    // =================================================================================================
    {
        Params p;
        p.session_mode   = SESSION_MP_LOCKSTEP; // 3
        p.ai_group_index = 0x99;
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.u(TARGET_PLAYER, TARGET_INDEX).ai_group_index, 0,
              "T7a: session_mode==3 -- ai_group_index cleared (0x004876d3)");

        p.session_mode   = SESSION_SP; // 1, not lockstep
        p.ai_group_index = 0x99;
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.u(TARGET_PLAYER, TARGET_INDEX).ai_group_index, 0x99,
              "T7b: session_mode==1 (not lockstep) -- ai_group_index untouched (0x004876be JNZ taken)");
    }

    // =================================================================================================
    // T8 -- the local-player branch (player==PlayerSide, 0x004876df/0x004876e6) and the click-select
    // branch (0x004876fc..) are IF/ELSE-IF (0x004876fa JMP skips branch B entirely on branch A) -- when
    // BOTH conditions would independently match, only ctrlgroup_leave+SetEvent fire; the click-target
    // id is NOT cleared. This is the separating case for a translation that used two independent `if`s
    // instead of if/else-if.
    // =================================================================================================
    {
        Params p;
        p.player_side = (int16_t)TARGET_PLAYER;            // branch A condition true
        p.click_flags = (uint16_t)(TARGET_PLAYER | 0x80u); // branch B condition ALSO true...
        p.click_id    = (uint16_t)TARGET_INDEX;            // ...both sub-gates of B match too
        seed_and_run(fx, p);
        ck(g_ctrlgroup_leave_calls.size() == 1 && g_ctrlgroup_leave_calls[0] == TARGET_INDEX,
           "T8: local-player branch fires -- unit_ctrlgroup_leave(unit_index=5) called exactly once "
           "(0x004876eb)");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == EVENT_INFO_REFRESH,
           "T8: SetEvent(EVENT_INFO_REFRESH=6) called exactly once, from branch A (0x004876f5)");
        ck_eq((uint32_t)fx.click_select_target_id, TARGET_INDEX,
              "T8: click_select_target_id UNCHANGED (still 5) -- branch B's clear did NOT run even "
              "though its own condition also matched (0x004876fa JMP skips it)");
    }

    // =================================================================================================
    // T9 -- click-select branch (player != PlayerSide): flags==(player|0x80) (0x0048770b/0x0048770d)
    // AND id==unit_index (0x00487716/0x00487719), both sub-gates, both sides.
    // =================================================================================================
    {
        Params p;
        p.player_side = 40; // != TARGET_PLAYER -- branch A closed for all of T9

        // T9a: flags mismatch -> nothing fires.
        p.click_flags = 0x0000;
        p.click_id    = (uint16_t)TARGET_INDEX;
        seed_and_run(fx, p);
        ck(g_set_event_calls.empty() && g_ctrlgroup_leave_calls.empty(),
           "T9a: click flags mismatch -- neither SetEvent nor ctrlgroup_leave fires (0x0048770d JNZ)");
        ck_eq((uint32_t)fx.click_select_target_id, TARGET_INDEX, "T9a: click_select_target_id untouched");

        // T9b: flags match, id mismatch -> nothing fires.
        p.click_flags = (uint16_t)(TARGET_PLAYER | 0x80u);
        p.click_id    = (uint16_t)(TARGET_INDEX + 1);
        seed_and_run(fx, p);
        ck(g_set_event_calls.empty() && g_ctrlgroup_leave_calls.empty(),
           "T9b: click flags match but id mismatch -- nothing fires (0x00487719 JZ not taken)");
        ck_eq((uint32_t)fx.click_select_target_id, TARGET_INDEX + 1, "T9b: click_select_target_id untouched");

        // T9c: both match -> id cleared + SetEvent.
        p.click_flags = (uint16_t)(TARGET_PLAYER | 0x80u);
        p.click_id    = (uint16_t)TARGET_INDEX;
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.click_select_target_id, 0,
              "T9c: both sub-gates match -- click_select_target_id cleared to 0 (0x0048771d)");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == EVENT_INFO_REFRESH,
           "T9c: SetEvent(EVENT_INFO_REFRESH=6) fires (0x0048772b)");
        ck(g_ctrlgroup_leave_calls.empty(), "T9c: unit_ctrlgroup_leave does NOT fire (branch B, not A)");
    }

    // =================================================================================================
    // T10 -- neither branch open (player != PlayerSide, click flags mismatched) -> no SetEvent /
    // ctrlgroup_leave / click-clear at all (0x0048771b JMP straight past both).
    // =================================================================================================
    {
        Params p;
        p.player_side = 40; // != TARGET_PLAYER
        p.click_flags = 0;  // mismatched
        p.click_id    = (uint16_t)TARGET_INDEX;
        seed_and_run(fx, p);
        ck(g_set_event_calls.empty() && g_ctrlgroup_leave_calls.empty(),
           "T10: neither branch fires -- no SetEvent, no ctrlgroup_leave (0x0048771b)");
        ck_eq((uint32_t)fx.click_select_target_id, TARGET_INDEX, "T10: click_select_target_id untouched");
    }

    // =================================================================================================
    // T11 -- profile.units_alive[planet] -= 1, UNCONDITIONAL (0x00487745 DEC); ==0 gate
    // (0x0048775f CMP / 0x00487766 JNZ) guards player_presence_lost(player, 0) (0x0048776e), both
    // sides. A same-player-other-planet slot and an other-player-same-planet slot stay untouched.
    // =================================================================================================
    {
        Params p;
        p.units_alive  = 5; // -> 4, not the threshold
        p.planet_index = 2;
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.profiles[TARGET_PLAYER].units_alive[2], 4,
              "T11a: units_alive[planet=2] 5 -> 4 (0x00487745 DEC, unconditional)");
        ck(g_presence_lost_calls.empty(),
           "T11a: units_alive!=0 after decrement -- player_presence_lost does NOT fire (0x00487766 JNZ)");
        ck_eq((uint32_t)fx.profiles[TARGET_PLAYER].units_alive[3], 999,
              "T11a: a DIFFERENT planet slot (3) on the same player untouched");
        ck_eq((uint32_t)fx.profiles[GUARD_PLAYER].units_alive[2], 888,
              "T11a: the SAME planet slot (2) on a DIFFERENT player untouched");

        p.units_alive  = 1; // -> 0, the threshold
        p.planet_index = 2;
        seed_and_run(fx, p);
        ck_eq((uint32_t)fx.profiles[TARGET_PLAYER].units_alive[2], 0,
              "T11b: units_alive[planet=2] 1 -> 0");
        ck(g_presence_lost_calls.size() == 1 && g_presence_lost_calls[0].player == TARGET_PLAYER &&
               g_presence_lost_calls[0].mode == 0,
           "T11b: units_alive==0 after decrement -- player_presence_lost(player=3, mode=0) fires "
           "(0x0048776e)");
    }

    // =================================================================================================
    // T12 -- llm_strat_unit_set_state_of(player, unit_index, 4) exact args (0x0048777f CALL); the
    // unit.move_microstep=0 write (0x00487797) happens AFTER that call in program order -- observed by
    // reading the watched unit's move_microstep AT THE MOMENT of the call, which must still be the
    // seeded sentinel (77), not 0 yet.
    // =================================================================================================
    {
        Params p;
        seed_and_run(fx, p);
        ck(g_set_state_of_calls.size() == 1 && g_set_state_of_calls[0].player == (int32_t)TARGET_PLAYER &&
               g_set_state_of_calls[0].unit_index == (int32_t)TARGET_INDEX &&
               g_set_state_of_calls[0].state == 4,
           "T12a: unit_set_state_of(player=3, unit_index=5, state=4 /* corpse_fow_decay */) called "
           "exactly once with the right args (0x0048777f)");
        ck_eq((uint32_t)g_move_microstep_at_state_call, 77,
              "T12b: move_microstep was STILL the seeded sentinel (77) at the moment of the "
              "unit_set_state_of call -- the 0x00487797 zero-write happens AFTER it, not before");
        ck_eq((uint32_t)fx.u(TARGET_PLAYER, TARGET_INDEX).move_microstep, 0,
              "T12c: move_microstep == 0 after the function returns (0x00487797)");
    }

    // =================================================================================================
    // T13 -- llm_strat_ai_notify_object_removed(player|0x80, unit_index, 0) fires ALWAYS and LAST
    // (0x004877ae), even when every other gate is closed.
    // =================================================================================================
    {
        Params p;
        p.path_slot_id = 0xff;       // step 1 closed
        p.energy       = 0.0;        // step 3 closed
        p.session_mode = SESSION_SP; // step 7 closed
        p.player_side  = 40;         // step 8/9 both closed
        p.click_flags  = 0;
        p.units_alive  = 5; // step 9 (presence_lost) closed
        seed_and_run(fx, p);
        ck(g_ai_notify_calls.size() == 1 && g_ai_notify_calls[0].flags == (TARGET_PLAYER | 0x80u) &&
               g_ai_notify_calls[0].object_index == TARGET_INDEX && g_ai_notify_calls[0].hard_remove == 0,
           "T13: ai_notify_object_removed(flags=player|0x80=0x83, object_index=5, hard_remove=0) fires "
           "even with every OTHER gate closed (0x004877ae)");
        ck(!g_trace.empty() && std::strcmp(g_trace.back(), "ai_notify_object_removed") == 0,
           "T13: ai_notify_object_removed is the LAST call this function ever makes");
    }

    // =================================================================================================
    // T14 -- full CALL ORDER, local-player variant, every gate open: path_free_slot, fow_remove_sight,
    // unit_ctrlgroup_leave, set_event, player_presence_lost, unit_set_state_of,
    // ai_notify_object_removed -- in that exact order (straight-line block layout, no branch reorders
    // any of these relative to each other).
    // =================================================================================================
    {
        Params p;
        p.path_slot_id = 0x33;                   // step 1 open
        p.energy       = 5.0;                    // step 3 open
        p.session_mode = SESSION_MP_LOCKSTEP;    // step 7 open
        p.player_side  = (int16_t)TARGET_PLAYER; // step 8 (local-player) open
        p.units_alive  = 1;                      // step 9 (presence_lost) open
        seed_and_run(fx, p);
        ck(trace_eq({"path_free_slot", "fow_remove_sight", "unit_ctrlgroup_leave", "set_event",
                     "player_presence_lost", "unit_set_state_of", "ai_notify_object_removed"}),
           "T14: local-player call order == path_free_slot, fow_remove_sight, unit_ctrlgroup_leave, "
           "set_event, player_presence_lost, unit_set_state_of, ai_notify_object_removed");
        // and the guard slot/tiles are still untouched under the "everything fires" load.
        ck(fx.u(GUARD_PLAYER, GUARD_INDEX).unit_proto_id == 77 &&
               fx.u(GUARD_PLAYER, GUARD_INDEX).path_slot_id == 0x11 &&
               fx.u(GUARD_PLAYER, GUARD_INDEX).ai_group_index == 0x1234 &&
               fx.u(GUARD_PLAYER, GUARD_INDEX).move_microstep == 555,
           "T14: guard unit slot (a different player+index) fully untouched even with every gate open");
    }

    // =================================================================================================
    // T15 -- full CALL ORDER, click-select variant, every gate open EXCEPT the local-player one:
    // path_free_slot, fow_remove_sight, set_event, player_presence_lost, unit_set_state_of,
    // ai_notify_object_removed -- unit_ctrlgroup_leave does NOT appear in this trace.
    // =================================================================================================
    {
        Params p;
        p.path_slot_id = 0x33;
        p.energy       = 5.0;
        p.session_mode = SESSION_MP_LOCKSTEP;
        p.player_side  = 40; // local-player branch closed
        p.click_flags  = (uint16_t)(TARGET_PLAYER | 0x80u);
        p.click_id     = (uint16_t)TARGET_INDEX; // click branch open instead
        p.units_alive  = 1;
        seed_and_run(fx, p);
        ck(trace_eq({"path_free_slot", "fow_remove_sight", "set_event", "player_presence_lost",
                     "unit_set_state_of", "ai_notify_object_removed"}),
           "T15: click-select call order == path_free_slot, fow_remove_sight, set_event, "
           "player_presence_lost, unit_set_state_of, ai_notify_object_removed (NO ctrlgroup_leave)");
    }

    // =================================================================================================
    // T16 -- every player-dependent address computation MOVZXes the WORD half of the stored 32-bit
    // `player` local (0x0048755b et al.) -- a raw player value with garbage high bits must resolve to
    // the SAME low-16-bit row everywhere (housing, profile, roster, and every seam call's player arg),
    // not crash / touch the wrong row / pass the raw value through uninterpreted.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint32_t MASKED     = 3;
        constexpr uint32_t RAW_PLAYER = 0x00010003u; // low 16 bits == MASKED (3)
        constexpr int32_t  INDEX      = 5;

        unit &u                          = fx.u(MASKED, INDEX);
        u.path_slot_id                   = 0x40; // gate open -- exercises path_free_slot's player arg too
        u.energy                         = 0.0;  // keep the energy/slot-0 gate closed to isolate this property
        u.x                              = TARGET_X;
        u.y                              = TARGET_Y;
        u.origin_tile_was_passable       = 0x5a;
        u.unit_proto_id                  = TARGET_PROTO;
        fx.cfg_units[TARGET_PROTO].sight = 9;

        fx.unit_housing[MASKED].used_soldiers = 20;
        fx.profiles[MASKED].units_alive[0]    = 5; // planet_index==0 after reset()

        tile_object &t = fx.t(TARGET_X, TARGET_Y);
        t.building     = 0x1234;
        t.class_owner  = 0x56;

        reset_observations();
        g_watch_unit_ptr = &u;

        sim_store own = fx.store();
        detail::unit_teardown_mapped(fx.view(), own, g_calls, RAW_PLAYER, (uint32_t)INDEX);

        ck_eq((uint32_t)fx.unit_housing[MASKED].used_soldiers, 19,
              "T16a: housing row resolved from the LOW 16 BITS of the raw player value (20 -> 19)");
        ck_eq((uint32_t)fx.profiles[MASKED].units_alive[0], 4,
              "T16b: profile row resolved from the same low-16-bit player (5 -> 4)");
        ck_eq((uint32_t)t.building, 0, "T16c: tile clear still lands on the target cell");
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == (uint16_t)MASKED,
           "T16d: path_free_slot's player arg is the MASKED value (3), not the raw 0x10003");
        ck(g_fow_calls.size() == 1 && g_fow_calls[0].player == MASKED,
           "T16e: fow_remove_sight's player arg is the MASKED value (3)");
        ck(g_set_state_of_calls.size() == 1 && g_set_state_of_calls[0].player == (int32_t)MASKED,
           "T16f: unit_set_state_of's player arg is the MASKED value (3)");
        ck(g_ai_notify_calls.size() == 1 && g_ai_notify_calls[0].flags == (MASKED | 0x80u),
           "T16g: ai_notify_object_removed's flags arg is (MASKED|0x80)=0x83, not (raw|0x80)");
    }
}

} // namespace mh::sim::test
