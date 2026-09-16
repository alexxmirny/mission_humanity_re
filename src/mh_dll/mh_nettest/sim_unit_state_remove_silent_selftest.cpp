//
// sim_unit_state_remove_silent_selftest.cpp -- `simtest` cases for llm_strat_unit_state_remove_silent
// @0x0048592d (sim/sim_unit_state_remove_silent.h/.cpp, SIM1-G1).
//
// SCOPE: the UNCONDITIONAL ai_notify_object_removed call (0x00485945-0x00485959), exact args
// (player|0x80, index, hard_remove=0 -- the same EBX the asm zeroes at entry and never rewrites
// before this call); the energy gate (0x00485965 FCOMP/0x0048596b JNC) -- energy always zeroed
// (0x00485972/0x00485979 or 0x004859a6/0x004859ad), PLUS (only when energy was >0.0 going in) the
// FADD of DAT_0050146a(-1.0) into units[player][SLOT 0].energy (0x0048598d-0x00485999), including the
// index==0 aliasing case where the removed unit's own slot IS slot 0 so the two writes collide on one
// address; the SIGNED type dispatch (0x004859dd CMP/0x004859e4 JG) -- boundary (0xe->small, 0xf->big)
// plus a high-bit-set value that separates the signed cast from a naive unsigned `<`; the common-tail
// target_ref/target2_ref release pair (0x00485a0e-0x00485a94, independent ifs, order, field clears);
// the UNCONDITIONAL housing_count_remove/storage_release_door_held_by_unit pair (0x00485a94-
// 0x00485ac1); the path_slot_id gate (0x00485ac1 CMP/0x00485ac8 JZ); the per-planet units_alive
// decrement + presence-lost gate (0x00485add-0x00485b29) -- INCLUDING that, unlike DIE_EXPLODE/
// teardown, this "silent" path does NOT bump units_lost_total (no such write exists anywhere in this
// asm); the state-commit (0x00485b29 unit_set_state(4)) + move_microstep-as-saved-sight overload
// (0x00485b33-0x00485b54) + FoW-refresh/SetEvent/notify-ui tail (0x00485b54-0x00485ba1); full CALL
// ORDER via one shared trace; and non-corruption of fields this function does not touch plus a
// neighbouring roster slot.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_state_remove_silent_0048592d.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_remove_silent.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across all 12 unit_state_remove_silent_calls members ---------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (12, one per unit_state_remove_silent_calls member) ----------------------
struct NotifyRemovedCall {
    uint32_t flags, idx;
    int32_t  hard_remove;
};
std::vector<NotifyRemovedCall> g_notify_removed_calls;
void                           rec_ai_notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    tr("ai_notify_object_removed");
    g_notify_removed_calls.push_back({flags, object_index, hard_remove});
}

struct RemoveCall {
    uint16_t player;
    uint32_t idx;
};
std::vector<RemoveCall> g_remove_calls;
void                    rec_unit_remove_from_map(uint16_t player, uint32_t unit_idx) {
    tr("unit_remove_from_map");
    g_remove_calls.push_back({player, unit_idx});
}

std::vector<RemoveCall> g_on_destroyed_calls;
void                    rec_unit_on_destroyed(uint16_t player, uint32_t unit_idx) {
    tr("unit_on_destroyed");
    g_on_destroyed_calls.push_back({player, unit_idx});
}

struct TargetReleaseCall {
    uint32_t player;
    int32_t  idx;
    uint32_t mode;
};
std::vector<TargetReleaseCall> g_target_release_calls;
void                           rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    tr("target_release_ref");
    g_target_release_calls.push_back({player_idx, unit_idx, mode});
}

struct HousingCall {
    int32_t player, proto_id;
};
std::vector<HousingCall> g_housing_calls;
void                     rec_unit_housing_count_remove(int32_t player, int32_t unit_proto_id) {
    tr("unit_housing_count_remove");
    g_housing_calls.push_back({player, unit_proto_id});
}

struct DoorCall {
    int32_t player, idx;
};
std::vector<DoorCall> g_door_calls;
int32_t               rec_storage_release_door_held_by_unit(int32_t player, int32_t unit_idx) {
    tr("storage_release_door_held_by_unit");
    g_door_calls.push_back({player, unit_idx});
    return 0;
}

struct PathFreeCall {
    uint16_t player;
    int32_t  idx;
};
std::vector<PathFreeCall> g_path_free_calls;
void                      rec_path_free_slot(uint16_t player, int32_t unit_index) {
    tr("path_free_slot");
    g_path_free_calls.push_back({player, unit_index});
}

struct PresenceCall {
    uint32_t player, mode;
};
std::vector<PresenceCall> g_presence_calls;
uint32_t                  rec_player_presence_lost(uint32_t player, uint32_t mode) {
    tr("player_presence_lost");
    g_presence_calls.push_back({player, mode});
    return 0;
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct FowCall {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<FowCall> g_fow_calls;
void                 rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    tr("map_fow_UpdateFoWPlus");
    g_fow_calls.push_back({player, x, y, sight});
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_game_SetEvent(uint32_t type) {
    tr("game_SetEvent");
    g_set_event_calls.push_back(type);
    return 0;
}

struct NotifyCall {
    uint32_t side, idx;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_unit_notify_ui(uint32_t side, uint32_t unit_index) {
    tr("unit_notify_ui");
    g_notify_calls.push_back({side, unit_index});
}

const unit_state_remove_silent_calls g_calls = {
    &rec_ai_notify_object_removed,
    &rec_unit_remove_from_map,
    &rec_unit_on_destroyed,
    &rec_target_release_ref,
    &rec_unit_housing_count_remove,
    &rec_storage_release_door_held_by_unit,
    &rec_path_free_slot,
    &rec_player_presence_lost,
    &rec_unit_set_state,
    &rec_map_fow_UpdateFoWPlus,
    &rec_game_SetEvent,
    &rec_unit_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_notify_removed_calls.clear();
    g_remove_calls.clear();
    g_on_destroyed_calls.clear();
    g_target_release_calls.clear();
    g_housing_calls.clear();
    g_door_calls.clear();
    g_path_free_calls.clear();
    g_presence_calls.clear();
    g_set_state_calls.clear();
    g_fow_calls.clear();
    g_set_event_calls.clear();
    g_notify_calls.clear();
}

// Fixed "guard" slot/planet no test's own (player,index,planet) ever touches -- seeded with sentinel
// nonzero data each run so a wrong-index write lands somewhere observable.
constexpr uint16_t GUARD_PLAYER = 6;
constexpr int32_t  GUARD_INDEX  = 9;
constexpr int32_t  GUARD_PLANET = 17;

void seed_guard_slot(sim_fixture &fx) {
    unit &g                                                  = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id                                          = 88;
    g.energy                                                 = 12.5;
    g.x                                                      = 111;
    g.y                                                      = 122;
    g.target_ref                                             = 5;
    g.target_index                                           = 6;
    g.target2_ref                                            = 7;
    g.target2_index                                          = 8;
    g.path_slot_id                                           = 44;
    g.move_microstep                                         = 999;
    g.state                                                  = 0x33;
    g.order                                                  = 0x55;
    fx.profiles[GUARD_PLAYER].units_alive[GUARD_PLANET]      = 555;
    fx.profiles[GUARD_PLAYER].units_lost_total[GUARD_PLANET] = 333;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    uint32_t type  = 1;  // small/non-heli by default (< UNIT_TYPE_A_HELI==0xf)
    uint8_t  sight = 57; // distinct non-default sentinel

    double energy       = 3.5;  // >0.0 by default -- FADD-to-slot0 arm
    double slot0_energy = 42.5; // sentinel for units[player][0].energy when index!=0

    int16_t target_ref    = 0;
    int16_t target_index  = 0;
    int16_t target2_ref   = 0;
    int16_t target2_index = 0;
    uint8_t path_slot_id  = 0xff; // 0xff == none

    uint8_t ux = 37, uy = 91; // the unit's own tile x/y (distinct from each other and from GUARD's)

    int32_t planet           = 4;
    int32_t units_alive_seed = 9;
    int32_t units_lost_seed  = 777; // sentinel -- must NEVER move (this path bumps no such counter)

    uint16_t state = 0x11; // sentinel -- this function never writes state itself (owned by the
                           // stubbed unit_set_state callee)
    uint16_t order = 0x77; // sentinel -- this function never reaches Order
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.cfg_row;
    u.energy         = s.energy;
    u.target_ref     = s.target_ref;
    u.target_index   = s.target_index;
    u.target2_ref    = s.target2_ref;
    u.target2_index  = s.target2_index;
    u.path_slot_id   = s.path_slot_id;
    u.x              = s.ux;
    u.y              = s.uy;
    u.move_microstep = -777; // sentinel, distinct from any proto.sight -- must be overwritten
    u.state          = s.state;
    u.order          = s.order;

    // units[player][0].energy -- the FADD-to-slot0 target. When index==0 this IS `u` itself (aliased,
    // see T2c), so only seed it separately when index!=0 -- seeding it again for index==0 would just
    // overwrite the u.energy write above with the same value, which is harmless, but keep the
    // aliasing case honest by not double-writing through two different named locals for the same slot.
    if (s.index != 0) fx.u(s.player, 0).energy = s.slot0_energy;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    cfg_unit &cu = fx.cfg_units[s.cfg_row];
    cu.type      = s.type;
    cu.sight     = s.sight;

    fx.planet_index                                  = s.planet;
    fx.profiles[s.player].units_alive[s.planet]      = s.units_alive_seed;
    fx.profiles[s.player].units_lost_total[s.planet] = s.units_lost_seed;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_remove_silent(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_remove_silent_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- ai_notify_object_removed is UNCONDITIONAL (0x00485945-0x00485959): exact args
    // (player|0x80, index, hard_remove=0 -- the SAME EBX the asm zeroes at 0x00485945 and never
    // rewrites before this call, i.e. this call's own third argument, not dead code).
    // =================================================================================================
    {
        Seed s;
        s.player = 2;
        s.index  = 3;
        seed_and_run(fx, s);
        ck(g_notify_removed_calls.size() == 1, "T1: ai_notify_object_removed fires exactly once");
        if (g_notify_removed_calls.size() == 1) {
            const auto &c = g_notify_removed_calls[0];
            ck_eq(c.flags, 0x82u, "T1: flags = player(2)|0x80 = 0x82 (0x0048594e-0x00485956)");
            ck_eq(c.idx, 3u, "T1: object_index = cur_index(3) (0x00485947)");
            ck_eq((uint32_t)c.hard_remove, 0u, "T1: hard_remove = 0 (the entry-zeroed EBX, 0x00485945/0x00485959)");
        }
    }

    // =================================================================================================
    // T2 -- energy gate (0x00485965 FCOMP/0x0048596b JNC): energy always zeroed; PLUS, only when energy
    // was >0.0 going in, FADD DAT_0050146a(-1.0) into units[player][SLOT 0].energy (0x0048598d-
    // 0x00485999) -- slot 0 of the player's roster, NOT units[player][index]. T2c pins the index==0
    // aliasing case where the two writes collide on the SAME address (real ordering matters there).
    // =================================================================================================
    {
        // T2a: energy <= 0.0 going in -- zeroed only, slot 0 (a DIFFERENT slot here, index!=0)
        // untouched.
        Seed s;
        s.player       = 1;
        s.index        = 2;
        s.energy       = -5.0; // <=0.0 -- else-arm, no FADD
        s.slot0_energy = 42.5;
        seed_and_run(fx, s);
        ck_eq_d(fx.u(1, 2).energy, 0.0, "T2a: energy<=0.0 going in -- energy zeroed (0x004859a6/0x004859ad)");
        ck_eq_d(fx.u(1, 0).energy, 42.5, "T2a: energy<=0.0 going in -- units[player][0].energy untouched (else-arm has no FADD)");

        // T2b: energy > 0.0 going in -- zeroed AND slot 0 (distinct slot, index!=0) gets -1.0.
        s.energy       = 3.5; // >0.0 -- if-arm, zero + FADD to slot 0
        s.slot0_energy = 42.5;
        seed_and_run(fx, s);
        ck_eq_d(fx.u(1, 2).energy, 0.0, "T2b: energy>0.0 going in -- the REMOVED unit's own energy still ends at 0.0 (0x00485972/0x00485979)");
        ck_eq_d(fx.u(1, 0).energy, 41.5, "T2b: units[player][SLOT 0].energy += -1.0 (42.5 -> 41.5), NOT units[player][index] (0x0048598d-0x00485999)");

        // T2c: index==0 -- the removed unit's own slot IS slot 0, so the zero-store and the FADD
        // collide on the SAME address. Real order (asm straight-line): zero first, THEN FLD the
        // (now-zero) value and FADD -1.0, giving a final -1.0 -- NOT (energy - 1.0) computed from the
        // ORIGINAL pre-zero value (which would give 2.5 for energy=3.5).
        s.player = 1;
        s.index  = 0;
        s.energy = 3.5;
        seed_and_run(fx, s);
        ck_eq_d(fx.u(1, 0).energy, -1.0,
                "T2c: index==0 aliasing -- zero-then-FADD on the SAME address gives -1.0, not (orig-1.0)=2.5 "
                "(0x0048596d-0x00485999: the zero-store and the slot-0 FADD address the identical dword pair "
                "when index==0)");
    }

    // =================================================================================================
    // T3 -- SIGNED type dispatch (0x004859dd CMP/0x004859e4 JG): boundary (0xe -> small/
    // unit_remove_from_map, 0xf -> big/unit_on_destroyed) plus a high-bit-set value that separates the
    // SIGNED cast (`static_cast<int32_t>(type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)`) from a naive
    // unsigned `<` translation.
    // =================================================================================================
    {
        Seed s;
        s.player = 0;
        s.index  = 1;
        s.type   = 0xe; // just below UNIT_TYPE_A_HELI(0xf) -- SMALL arm
        seed_and_run(fx, s);
        ck(g_remove_calls.size() == 1 && g_remove_calls[0].player == 0 && g_remove_calls[0].idx == 1,
           "T3a: type=0xe -- unit_remove_from_map(cur_player, cur_index) fires (0x004859f4)");
        ck(g_on_destroyed_calls.empty(), "T3a: type=0xe does NOT take the BIG arm");

        s.type = 0xf; // exactly UNIT_TYPE_A_HELI -- BIG arm
        seed_and_run(fx, s);
        ck(g_on_destroyed_calls.size() == 1 && g_on_destroyed_calls[0].player == 0 && g_on_destroyed_calls[0].idx == 1,
           "T3b: type=0xf -- unit_on_destroyed(cur_player, cur_index) fires (0x004859e4 JG taken, 0x00485a09)");
        ck(g_remove_calls.empty(), "T3b: type=0xf does NOT take the SMALL arm");

        s.type = 0x80000000u; // high bit set -- signed vs unsigned comparison disagree
        seed_and_run(fx, s);
        ck(g_remove_calls.size() == 1,
           "T3c: type=0x80000000 -- SMALL arm under the SIGNED compare (0x004859dd CMP/0x004859e4 JG is "
           "signed -- INT_MIN < 0xe); an unsigned `<` translation would wrongly take the BIG arm here");
        ck(g_on_destroyed_calls.empty(), "T3c: type=0x80000000 must NOT reach unit_on_destroyed");
    }

    // =================================================================================================
    // T4 -- target_ref/target2_ref release: two INDEPENDENT ifs (both can fire), each clearing its own
    // index+ref pair, in target_ref(mode=1)-then-target2_ref(mode=3) order (0x00485a0e-0x00485a94).
    // =================================================================================================
    {
        // both zero -> neither fires
        Seed s;
        s.player = 0;
        s.index  = 1;
        seed_and_run(fx, s);
        ck(g_target_release_calls.empty(), "T4a: target_ref==target2_ref==0 -- no release calls (both JZ taken)");

        // only target_ref set
        s.target_ref   = 0x21;
        s.target_index = 3;
        seed_and_run(fx, s);
        ck(g_target_release_calls.size() == 1 && g_target_release_calls[0].player == 0 &&
               g_target_release_calls[0].idx == 1 && g_target_release_calls[0].mode == 1u,
           "T4b: target_ref!=0 -- target_release_ref(cur_player, cur_index, mode=1) (0x00485a22-0x00485a30)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target_ref, 0u, "T4b: target_ref cleared to 0 (0x00485a3a)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target_index, 0u, "T4b: target_index cleared to 0 (0x00485a48)");

        // only target2_ref set
        s.target_ref    = 0;
        s.target_index  = 0;
        s.target2_ref   = 0x22;
        s.target2_index = 4;
        seed_and_run(fx, s);
        ck(g_target_release_calls.size() == 1 && g_target_release_calls[0].mode == 3u,
           "T4c: target2_ref!=0 -- target_release_ref(..., mode=3) (0x00485a65-0x00485a73)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target2_ref, 0u, "T4c: target2_ref cleared to 0 (0x00485a7d)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target2_index, 0u, "T4c: target2_index cleared to 0 (0x00485a8b)");

        // both set -> both fire, target_ref (mode=1) BEFORE target2_ref (mode=3)
        s.target_ref    = 0x21;
        s.target_index  = 3;
        s.target2_ref   = 0x22;
        s.target2_index = 4;
        seed_and_run(fx, s);
        ck(g_target_release_calls.size() == 2 && g_target_release_calls[0].mode == 1u && g_target_release_calls[1].mode == 3u,
           "T4d: both set -- mode=1 release fires BEFORE mode=3 (asm's target_ref block precedes target2_ref block)");
    }

    // =================================================================================================
    // T5 -- housing_count_remove / storage_release_door_held_by_unit fire UNCONDITIONALLY, common to
    // BOTH dispatch arms (0x00485a94-0x00485ac1), with the exact args.
    // =================================================================================================
    {
        Seed s;
        s.player  = 1;
        s.index   = 5;
        s.cfg_row = 33;
        seed_and_run(fx, s);
        ck(g_housing_calls.size() == 1 && g_housing_calls[0].player == 1 && g_housing_calls[0].proto_id == 33,
           "T5: unit_housing_count_remove(cur_player, unit_proto_id) fires unconditionally (0x00485a94-0x00485aa4)");
        ck(g_door_calls.size() == 1 && g_door_calls[0].player == 1 && g_door_calls[0].idx == 5,
           "T5: storage_release_door_held_by_unit(cur_player, cur_index) fires unconditionally (0x00485aa9-0x00485ab7)");
    }

    // =================================================================================================
    // T6 -- path_slot_id gate (0x00485ac1 CMP/0x00485ac8 JZ): 0xff (none) vs an assigned slot.
    // =================================================================================================
    {
        Seed s;
        s.player       = 0;
        s.index        = 1;
        s.path_slot_id = 0xff;
        seed_and_run(fx, s);
        ck(g_path_free_calls.empty(), "T6a: path_slot_id==0xff -- path_free_slot does NOT fire (0x00485ac8 JZ taken)");

        s.path_slot_id = 12;
        seed_and_run(fx, s);
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == 0 && g_path_free_calls[0].idx == 1,
           "T6b: path_slot_id!=0xff -- path_free_slot(cur_player, cur_index) fires (0x00485ad8)");
    }

    // =================================================================================================
    // T7 -- units_alive[planet] -= 1 (0x00485add-0x00485b19), presence-lost gate on the count reaching
    // exactly 0 (0x00485b19 JNZ/0x00485b24), and -- the "silent" removal's own distinguishing property
    // -- units_lost_total is NEVER bumped anywhere in this asm (unlike DIE_EXPLODE/teardown).
    // =================================================================================================
    {
        Seed s;
        s.player           = 0;
        s.index            = 1;
        s.planet           = 4;
        s.units_alive_seed = 5; // -> 4 after decrement, stays nonzero
        s.units_lost_seed  = 777;
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.profiles[0].units_alive[4], 4u, "T7a: units_alive[planet] -= 1 (5 -> 4) (0x00485af5)");
        ck(g_presence_calls.empty(), "T7a: units_alive stayed nonzero -- player_presence_lost does NOT fire (0x00485b19 JNZ taken)");
        ck_eq((uint32_t)fx.profiles[0].units_lost_total[4], 777u,
              "T7a: units_lost_total UNCHANGED -- this silent path never bumps it (no such write in the asm)");

        s.units_alive_seed = 1; // -> 0 after decrement
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.profiles[0].units_alive[4], 0u, "T7b: units_alive[planet] reaches exactly 0");
        ck(g_presence_calls.size() == 1 && g_presence_calls[0].player == 0 && g_presence_calls[0].mode == 0u,
           "T7b: units_alive 1->0 -- player_presence_lost(cur_player, mode=0) fires (0x00485b1b-0x00485b24)");
        ck_eq((uint32_t)fx.profiles[0].units_lost_total[4], 777u,
              "T7b: units_lost_total UNCHANGED even when presence_lost fires -- still no bump anywhere");
    }

    // =================================================================================================
    // T8 -- full CALL ORDER (0x00485b29 unit_set_state(4) commits the state BEFORE the move_microstep
    // overload write and the FoW/SetEvent/notify-ui tail): both a minimal spine (no target refs, no
    // path slot, alive stays nonzero) and a fully-firing case (everything conditional fires) pin the
    // straight-line block order the asm never branches to reorder.
    // =================================================================================================
    {
        // Minimal spine.
        Seed s;
        s.player           = 0;
        s.index            = 1;
        s.type             = 1; // small arm
        s.units_alive_seed = 9; // stays nonzero after -1
        seed_and_run(fx, s);
        ck(trace_eq({"ai_notify_object_removed", "unit_remove_from_map", "unit_housing_count_remove",
                     "storage_release_door_held_by_unit", "unit_set_state", "map_fow_UpdateFoWPlus",
                     "game_SetEvent", "unit_notify_ui"}),
           "T8a: minimal-spine call order (no target refs/path slot/presence-lost)");

        // Everything conditional fires: BIG arm, both target refs, path slot assigned, alive -> 0.
        Seed s2;
        s2.player           = 1;
        s2.index            = 2;
        s2.type             = UNIT_TYPE_A_HELI; // BIG arm
        s2.target_ref       = 0x21;
        s2.target_index     = 3;
        s2.target2_ref      = 0x22;
        s2.target2_index    = 4;
        s2.path_slot_id     = 7;
        s2.planet           = 4;
        s2.units_alive_seed = 1; // -> 0
        seed_and_run(fx, s2);
        ck(trace_eq({"ai_notify_object_removed", "unit_on_destroyed", "target_release_ref", "target_release_ref",
                     "unit_housing_count_remove", "storage_release_door_held_by_unit", "path_free_slot",
                     "player_presence_lost", "unit_set_state", "map_fow_UpdateFoWPlus", "game_SetEvent",
                     "unit_notify_ui"}),
           "T8b: full call order with every conditional firing (asm's single straight-line layout, no "
           "branch reorders any of these relative to each other)");
    }

    // =================================================================================================
    // T9 -- state-commit + move_microstep-as-saved-sight overload + FoW refresh args (0x00485b29-
    // 0x00485ba1): unit_set_state(CORPSE_FOW_DECAY=4); move_microstep = cfg_units[proto].sight;
    // map_fow_UpdateFoWPlus(player, unit.x, unit.y, sight-as-byte, the SAME value just written).
    // =================================================================================================
    {
        Seed s;
        s.player  = 0;
        s.index   = 1;
        s.cfg_row = 40;
        s.sight   = 0x39; // 57, distinct sentinel
        s.ux      = 37;
        s.uy      = 91;
        seed_and_run(fx, s);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_REMOVE_SILENT_CORPSE_FOW_DECAY,
           "T9: unit_set_state(CORPSE_FOW_DECAY=4) (0x00485b29-0x00485b2e)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0x39u,
              "T9: move_microstep = cfg_units[proto].sight (saved-FoW-decay overload) (0x00485b42-0x00485b4e)");
        ck(g_fow_calls.size() == 1 && g_fow_calls[0].player == 0 && g_fow_calls[0].x == 37 &&
               g_fow_calls[0].y == 91 && g_fow_calls[0].sight == 0x39,
           "T9: map_fow_UpdateFoWPlus(cur_player, unit.x, unit.y, move_microstep-as-byte==sight) (0x00485b54-0x00485b7f)");
    }

    // =================================================================================================
    // T10 -- game_SetEvent(MAP_OBJECTS_REFRESH=14) and unit_notify_ui(player,index) fire unconditionally
    // at the very end (0x00485b84-0x00485ba1), after everything else.
    // =================================================================================================
    {
        Seed s;
        s.player = 3;
        s.index  = 6;
        seed_and_run(fx, s);
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == UNIT_STATE_REMOVE_SILENT_MAP_OBJECTS_REFRESH,
           "T10: game_SetEvent(14 == MAP_OBJECTS_REFRESH) (0x00485b84-0x00485b89)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].side == 3 && g_notify_calls[0].idx == 6,
           "T10: unit_notify_ui(cur_player, cur_index) (0x00485b8e-0x00485b9c)");
    }

    // =================================================================================================
    // T11 -- non-corruption: fields this function never writes directly (unit_proto_id/x/y/order,
    // state -- owned by the stubbed unit_set_state callee, path_slot_id -- owned by the stubbed
    // path_free_slot callee) read back exactly as seeded; a neighbouring roster slot and a different
    // planet index in the acted-on player's own profile stay untouched.
    // =================================================================================================
    {
        Seed s;
        s.player       = 0;
        s.index        = 1;
        s.cfg_row      = 21;
        s.path_slot_id = 3; // path gate open -- the REAL path_free_slot would clear this in the live
                            // game; the STUB here does not, so it must read back UNCHANGED
        s.state  = 0x11;
        s.order  = 0x77;
        s.planet = 4;
        seed_and_run(fx, s);

        const unit &u = fx.u(0, 1);
        ck(u.unit_proto_id == 21, "T11: cur_unit->unit_proto_id untouched (only read, never written)");
        ck(u.x == 37 && u.y == 91, "T11: cur_unit->x/y untouched (only read, never written)");
        ck(u.order == 0x77, "T11: cur_unit->order untouched (this function never reaches Order)");
        ck(u.state == 0x11, "T11: cur_unit->state unchanged (owned by unit_set_state, stubbed as a no-op here)");
        ck(u.path_slot_id == 3, "T11: cur_unit->path_slot_id unchanged (owned by path_free_slot, stubbed as a no-op here)");

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 88 && g.x == 111 && g.y == 122, "T11: guard unit's core fields untouched");
        ck_eq_d(g.energy, 12.5, "T11: guard unit's energy untouched");
        ck(g.target_ref == 5 && g.target_index == 6 && g.target2_ref == 7 && g.target2_index == 8,
           "T11: guard unit's target/target2 pairs untouched");
        ck(g.path_slot_id == 44 && g.move_microstep == 999, "T11: guard unit's path_slot_id/move_microstep untouched");
        ck(g.state == 0x33 && g.order == 0x55, "T11: guard unit's state/order untouched");
        ck_eq((uint32_t)fx.profiles[GUARD_PLAYER].units_alive[GUARD_PLANET], 555u,
              "T11: guard player's profile.units_alive untouched");
        ck_eq((uint32_t)fx.profiles[GUARD_PLAYER].units_lost_total[GUARD_PLANET], 333u,
              "T11: guard player's profile.units_lost_total untouched");
        // A different planet slot within the SAME player's profile (adjacent array element).
        ck_eq((uint32_t)fx.profiles[0].units_alive[GUARD_PLANET], 0u,
              "T11: a DIFFERENT planet index in the acted-on player's OWN profile is untouched (array-index precision)");
    }
}

} // namespace mh::sim::test
