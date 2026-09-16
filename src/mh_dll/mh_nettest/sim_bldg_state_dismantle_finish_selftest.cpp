//
// sim_bldg_state_dismantle_finish_selftest.cpp -- `simtest` cases for
// llm_strat_bldg_state_dismantle_finish (sim/sim_bldg_state_dismantle.h/.cpp @0x004736a2), the
// DISMANTLE_FINISH-state building_tick handler.
//
// THIS FUNCTION IS DO-NOT-ARM (its write closure reaches game_SetEvent's ~780-function UI/gfx/snd/
// menu-teardown over-approximation) -- there is no rig run backing this up. This oracle is its ONLY
// evidence, so it pins every branch, every unconditional call, and every index the header/asm name.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_dismantle_finish_004736a2.asm), cross-checked against the
// .cpp/.h's own per-line address citations:
//
//   UNCONDITIONAL prologue (0x004736ba-0x004736e9): ai_notify_object_removed(cur_player|0x40,
//   cur_index, /*hard_remove=*/1); bldg_refund_resources_scaled_by_energy(cur_player, cur_index).
//
//   Shuttle-slot release, gated on cur_building->shuttle_slot != 0 (0x004736ee-0x0047374d): if the
//   slot equals profiles[cur_player].prod_queue_slot[planet_index], prod_unbind_planet(cur_player,
//   planet_index) fires FIRST; then UNCONDITIONALLY (slot nonzero, regardless of the unbind branch)
//   prod_shuttle_slot_release(cur_player, shuttle_slot).
//
//   HQ energy credit, gated on cur_building->energy != 0.0 (0x0047374d-0x00473793, a sign-masked
//   bit-pattern test that is exactly the IEEE `!= 0.0` predicate per the header -- true for both
//   +0.0/-0.0 as "not gated", i.e. the gate is NOT taken for either zero): energy = 0.0, then
//   buildings[cur_player][0].energy += BLDG_DISMANTLE_HQ_ENERGY_CREDIT (-1.0) -- the HQ (roster SLOT
//   0), a DIFFERENT building record from cur_building.
//
//   Unconditional tail (0x00473793-0x004737f1): bldg_unmap_footprint(cur_player, cur_index), then
//   profiles[cur_player].buildings_alive[planet_index] -= 1; if that reaches EXACTLY 0,
//   player_presence_lost(cur_player, /*mode=*/0).
//
//   Transition to RUBBLE_SIGHT_DECAY (0x004737f1-0x00473868, UNCONDITIONAL): cycle_progress = 0.0,
//   state = BLDG_STATE_RUBBLE_SIGHT_DECAY(4), anim[0] (raw uint32) seeded from
//   cfg_buildings[building_id].sight, then sight_add_circle(cur_player, cur_building->x,
//   cur_building->y, cur_building->building_id, anim[0]_as_byte).
//
//   Tail (UNCONDITIONAL): tick_budget = 0.0, game_SetEvent(MAP_OBJECTS_REFRESH=0xe),
//   bldg_notify_ui(cur_player, cur_index).
//
#include "sim/sim_bldg_state_dismantle.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all callees ---------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (9, one per bldg_state_dismantle_finish_calls member) -----------------
struct AiNotifyCall {
    uint32_t flags, object_index;
    int32_t  hard_remove;
};
std::vector<AiNotifyCall> g_ai_notify_calls;
void                      rec_ai_notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    tr("ai_notify_object_removed");
    g_ai_notify_calls.push_back({flags, object_index, hard_remove});
}

struct RefundCall {
    int32_t player, index;
};
std::vector<RefundCall> g_refund_calls;
void                    rec_refund_resources_scaled_by_energy(int32_t player, int32_t index) {
    tr("refund_resources_scaled_by_energy");
    g_refund_calls.push_back({player, index});
}

struct UnbindCall {
    int32_t player, planet_slot;
};
std::vector<UnbindCall> g_unbind_calls;
void                    rec_prod_unbind_planet(int32_t player, int32_t planet_slot) {
    tr("prod_unbind_planet");
    g_unbind_calls.push_back({player, planet_slot});
}

struct ShuttleReleaseCall {
    int32_t player, slot;
};
std::vector<ShuttleReleaseCall> g_shuttle_release_calls;
void                            rec_prod_shuttle_slot_release(int32_t player, int32_t slot) {
    tr("prod_shuttle_slot_release");
    g_shuttle_release_calls.push_back({player, slot});
}

struct PresenceLostCall {
    uint32_t player, mode;
};
std::vector<PresenceLostCall> g_presence_lost_calls;
uint32_t                      rec_player_presence_lost(uint32_t player, uint32_t mode) {
    tr("player_presence_lost");
    g_presence_lost_calls.push_back({player, mode});
    return 0;
}

struct UnmapCall {
    uint16_t player;
    int32_t  index;
};
std::vector<UnmapCall> g_unmap_calls;
void                   rec_bldg_unmap_footprint(uint16_t player, int32_t index) {
    tr("bldg_unmap_footprint");
    g_unmap_calls.push_back({player, index});
}

struct SightAddCircleCall {
    uint32_t player;
    int32_t  x, y, building_id;
    uint8_t  sight;
};
std::vector<SightAddCircleCall> g_sight_add_circle_calls;
uint32_t                        rec_sight_add_circle(uint32_t player, int32_t x, int32_t y, int32_t building_id, uint8_t sight) {
    tr("sight_add_circle");
    g_sight_add_circle_calls.push_back({player, x, y, building_id, sight});
    return 0;
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_game_SetEvent(uint32_t type) {
    tr("game_SetEvent");
    g_set_event_calls.push_back(type);
    return 0;
}

struct NotifyUiCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyUiCall> g_notify_ui_calls;
void                      rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_ui_calls.push_back({player, index});
}

const bldg_state_dismantle_finish_calls g_calls = {
    &rec_ai_notify_object_removed,
    &rec_refund_resources_scaled_by_energy,
    &rec_prod_unbind_planet,
    &rec_prod_shuttle_slot_release,
    &rec_player_presence_lost,
    &rec_bldg_unmap_footprint,
    &rec_sight_add_circle,
    &rec_game_SetEvent,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_ai_notify_calls.clear();
    g_refund_calls.clear();
    g_unbind_calls.clear();
    g_shuttle_release_calls.clear();
    g_presence_lost_calls.clear();
    g_unmap_calls.clear();
    g_sight_add_circle_calls.clear();
    g_set_event_calls.clear();
    g_notify_ui_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
// Every scalar below is a DISTINCT value from every other (player/index/planet_index/cfg_row/sight/
// bx/by), so a translation that swapped any two of them fails here, not in the game.
struct Seed {
    uint16_t player = 3;
    int32_t  index  = 4; // cur_building's OWN roster slot -- deliberately NOT 0, so the HQ
                         // (buildings[player][0]) is a genuinely different record.
    int32_t  planet_index = 5;
    uint16_t cfg_row      = 12; // building_id -> cfg_buildings index
    uint8_t  sight        = 75; // cfg_buildings[cfg_row].sight
    uint8_t  bx = 21, by = 33;  // building's OWN tile position

    uint8_t shuttle_slot = 0; // 0 = unbound (case overrides to probe the gate)
    // profiles[player].prod_queue_slot[planet_index] -- the slot the unbind-match compares against.
    int32_t prod_queue_slot_at_planet = 0;
    // DECOY: profiles[player].prod_queue_slot[index] -- a translation that indexed by cur_index
    // instead of planet_index would read THIS value instead; kept deliberately mismatched from
    // prod_queue_slot_at_planet so such a bug disagrees with the expected branch.
    int32_t prod_queue_slot_at_index_decoy = 999;

    double cur_building_energy_in = 0.0;  // cur_building's OWN energy -- the gate's real input
    double hq_energy_in           = 77.0; // buildings[player][0].energy BEFORE the call -- distinct
                                          // sentinel, and deliberately NONZERO even when the gate
                                          // must NOT fire, so a bug that read the HQ's energy for
                                          // the gate (instead of cur_building's) is caught.

    // profiles[player].buildings_alive[planet_index] -- decremented unconditionally.
    int32_t buildings_alive_at_planet_in = 5;
    // DECOY: profiles[player].buildings_alive[index] -- a translation that indexed by cur_index
    // instead of planet_index would touch THIS slot instead; must stay untouched.
    int32_t buildings_alive_at_index_decoy = 999;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b    = fx.b(s.player, s.index);
    b.building_id  = s.cfg_row;
    b.x            = s.bx;
    b.y            = s.by;
    b.shuttle_slot = s.shuttle_slot;
    b.energy       = s.cur_building_energy_in;
    // Distinct sentinels on the fields the function unconditionally overwrites, so "left alone" and
    // "overwritten with the right value" are both observable.
    b.cycle_progress = 123.0;
    b.state          = 0xbeef & 0xffff; // any value other than BLDG_STATE_RUBBLE_SIGHT_DECAY(4)
    std::memset(b.anim, 0xcc, sizeof(b.anim));

    building &hq = fx.b(s.player, 0); // the HQ record -- a DIFFERENT slot from cur_building (index!=0)
    hq.energy    = s.hq_energy_in;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;
    fx.planet_index     = s.planet_index;

    cfg_building &cb = fx.cfg_buildings[s.cfg_row];
    cb.sight         = s.sight;

    player_profile &prof                 = fx.profiles[s.player];
    prof.prod_queue_slot[s.planet_index] = s.prod_queue_slot_at_planet;
    prof.prod_queue_slot[s.index]        = s.prod_queue_slot_at_index_decoy;
    prof.buildings_alive[s.planet_index] = s.buildings_alive_at_planet_in;
    prof.buildings_alive[s.index]        = s.buildings_alive_at_index_decoy;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_dismantle_finish(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_dismantle_finish_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- everything OFF: shuttle_slot=0 (no unbind/release), cur_building energy=0.0 (no HQ
    // credit, even though the HQ's OWN energy is a nonzero sentinel), buildings_alive stays above 0
    // after the decrement (no presence_lost). Pins every UNCONDITIONAL call fires on the "nothing
    // gated" path, in order, and that none of the three gated call-pairs fire.
    // =================================================================================================
    {
        Seed s;
        s.shuttle_slot                 = 0;
        s.cur_building_energy_in       = 0.0;
        s.hq_energy_in                 = 77.0; // nonzero HQ sentinel -- must stay untouched
        s.buildings_alive_at_planet_in = 5;    // decrements to 4, not 0 -- no presence_lost
        seed_and_run(fx, s);

        ck(trace_eq({"ai_notify_object_removed", "refund_resources_scaled_by_energy", "bldg_unmap_footprint",
                     "sight_add_circle", "game_SetEvent", "notify_ui"}),
           "C1: gate-off order -- unconditional prologue -> unmap/roster tail -> RUBBLE_SIGHT_DECAY "
           "transition -> final tail, none of the 3 gated call-pairs fire (0x004736ba full path minus "
           "0x004736ee-0x0047374d, 0x0047374d-0x00473793, 0x004737e3-0x004737f1)");

        ck(g_ai_notify_calls.size() == 1, "C1: ai_notify_object_removed fires exactly once (unconditional)");
        if (g_ai_notify_calls.size() == 1) {
            const auto &c = g_ai_notify_calls[0];
            ck_eq(c.flags, (uint32_t)s.player | 0x40u,
                  "C1: ai_notify_object_removed flags = cur_player | 0x40 (0x004736cc `OR AL,0x40` on the "
                  "zero-extended cur_player, 3|0x40=0x43)");
            ck_eq(c.object_index, (uint32_t)s.index, "C1: ai_notify_object_removed object_index = cur_index (0x004736bf)");
            ck_eq((uint32_t)c.hard_remove, 1u, "C1: ai_notify_object_removed hard_remove = literal 1 (0x004736d1 call arg)");
        }

        ck(g_refund_calls.size() == 1 && g_refund_calls[0].player == s.player && g_refund_calls[0].index == s.index,
           "C1: bldg_refund_resources_scaled_by_energy(cur_player, cur_index) fires unconditionally (0x004736e4)");

        ck(g_unbind_calls.empty(), "C1: prod_unbind_planet does NOT fire when shuttle_slot==0 (0x004736ee JZ taken)");
        ck(g_shuttle_release_calls.empty(),
           "C1: prod_shuttle_slot_release does NOT fire when shuttle_slot==0 (whole block skipped)");

        ck_eq_d(fx.b(s.player, s.index).energy, 0.0,
                "C1: cur_building.energy gate NOT taken (0.0 != 0.0 is false) -- stays 0.0 (0x0047374d TEST/CMP)");
        ck_eq_d(fx.b(s.player, 0).energy, 77.0,
                "C1: HQ (buildings[player][0]).energy UNTOUCHED -- proves the gate reads cur_building's OWN "
                "energy, not the HQ's (a swapped-source bug would see the HQ's nonzero 77.0 and wrongly credit it)");

        ck(g_unmap_calls.size() == 1 && g_unmap_calls[0].player == s.player && g_unmap_calls[0].index == s.index,
           "C1: bldg_unmap_footprint(cur_player, cur_index) fires unconditionally (0x004737a1)");
        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.planet_index], 4u,
              "C1: buildings_alive[planet_index] decremented 5 -> 4 (0x004737bd DEC, indexed by planet_index)");
        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.index], 999u,
              "C1: buildings_alive[cur_index] (decoy slot) UNTOUCHED -- proves the decrement is indexed by "
              "planet_index, not cur_index");
        ck(g_presence_lost_calls.empty(),
           "C1: player_presence_lost does NOT fire -- buildings_alive reached 4, not 0 (0x004737e1 JNZ taken)");

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 0.0,
                "C1: cycle_progress reset to 0.0 unconditionally (0x004737f6)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_RUBBLE_SIGHT_DECAY,
              "C1: state = BLDG_STATE_RUBBLE_SIGHT_DECAY(4) unconditionally (0x00473809)");
        ck_eq((uint32_t)fx.b(s.player, s.index).anim[0], (uint32_t)s.sight,
              "C1: anim[0] byte 0 = cfg_buildings[building_id].sight (0x0047381e-0x0047382a store_u32_le)");
        ck_eq((uint32_t)fx.b(s.player, s.index).anim[1], 0u, "C1: anim[1] = 0 (zero-extended dword store, not a raw byte copy)");
        ck_eq((uint32_t)fx.b(s.player, s.index).anim[2], 0u, "C1: anim[2] = 0 (zero-extended dword store)");
        ck_eq((uint32_t)fx.b(s.player, s.index).anim[3], 0u, "C1: anim[3] = 0 (zero-extended dword store)");

        ck(g_sight_add_circle_calls.size() == 1, "C1: sight_add_circle fires exactly once (0x00473863)");
        if (g_sight_add_circle_calls.size() == 1) {
            const auto &c = g_sight_add_circle_calls[0];
            ck(c.player == s.player, "C1: sight_add_circle player = cur_player");
            ck_eq((uint32_t)c.x, (uint32_t)s.bx, "C1: sight_add_circle x = cur_building->x (0x0047383b-0x00473849 arg build)");
            ck_eq((uint32_t)c.y, (uint32_t)s.by, "C1: sight_add_circle y = cur_building->y (distinct field from x)");
            ck_eq((uint32_t)c.building_id, (uint32_t)s.cfg_row, "C1: sight_add_circle building_id = cur_building->building_id");
            ck_eq((uint32_t)c.sight, (uint32_t)s.sight,
                  "C1: sight_add_circle's 5th arg = the byte round-tripped through anim[0] (load_u32_le then "
                  "truncate to uint8_t), equal to the cfg sight value written above");
        }

        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 0xeu,
           "C1: game_SetEvent(MAP_OBJECTS_REFRESH=0xe) fires unconditionally (0x0047387c/0x00473881)");
        ck_eq_d(fx.tick_budget, 0.0, "C1: tick_budget zeroed unconditionally (0x00473868/0x00473872)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C1: bldg_notify_ui(cur_player, cur_index) fires at the tail (0x0047388d-0x00473894)");
    }

    // =================================================================================================
    // C2 -- everything ON: shuttle_slot bound AND matching prod_queue_slot[planet_index] (unbind
    // fires), cur_building energy nonzero (HQ credit fires), buildings_alive reaches EXACTLY 0
    // (presence_lost fires). Pins the full call order with every gated pair present, and that the
    // HQ-credit add lands on the DIFFERENT (slot-0) record while cur_building's own energy is zeroed.
    // =================================================================================================
    {
        Seed s;
        s.shuttle_slot                   = 6;
        s.prod_queue_slot_at_planet      = 6; // matches shuttle_slot -> unbind fires
        s.prod_queue_slot_at_index_decoy = 9; // MISMATCHES shuttle_slot -- if a bug indexed by
                                              // cur_index instead of planet_index, unbind would
                                              // wrongly NOT fire here
        s.cur_building_energy_in         = 2.5;
        s.hq_energy_in                   = 77.0;
        s.buildings_alive_at_planet_in   = 1; // decrements to EXACTLY 0 -> presence_lost fires
        s.buildings_alive_at_index_decoy = 999;
        seed_and_run(fx, s);

        ck(trace_eq({"ai_notify_object_removed", "refund_resources_scaled_by_energy", "prod_unbind_planet",
                     "prod_shuttle_slot_release", "bldg_unmap_footprint", "player_presence_lost",
                     "sight_add_circle", "game_SetEvent", "notify_ui"}),
           "C2: full call order with all 3 gated pairs firing -- unbind BEFORE release "
           "(0x00473730 before 0x00473748), unmap BEFORE presence_lost (0x004737a1 before 0x004737ec)");

        ck(g_unbind_calls.size() == 1 && g_unbind_calls[0].player == s.player &&
               g_unbind_calls[0].planet_slot == s.planet_index,
           "C2: prod_unbind_planet(cur_player, planet_index) fires -- shuttle_slot(6) == "
           "prod_queue_slot[planet_index](6) (0x0047371b CMP / 0x00473721 JNZ not taken)");
        ck(g_shuttle_release_calls.size() == 1 && g_shuttle_release_calls[0].player == s.player &&
               g_shuttle_release_calls[0].slot == (int32_t)s.shuttle_slot,
           "C2: prod_shuttle_slot_release(cur_player, shuttle_slot) fires with the RAW shuttle_slot value "
           "(0x00473748), not planet_index or the match flag");

        ck_eq_d(fx.b(s.player, s.index).energy, 0.0,
                "C2: cur_building.energy zeroed -- gate WAS taken (2.5 != 0.0) (0x00473766)");
        ck_eq_d(fx.b(s.player, 0).energy, 76.0,
                "C2: HQ (buildings[player][0]).energy = 77.0 + BLDG_DISMANTLE_HQ_ENERGY_CREDIT(-1.0) = 76.0 "
                "-- the credit lands on the HQ record, a DIFFERENT slot from cur_building (index 4 != 0) "
                "(0x0047377b IMUL EAX,EAX,0x6aa4 -- row stride, no column offset)");

        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.planet_index], 0u,
              "C2: buildings_alive[planet_index] decremented 1 -> 0 exactly");
        ck(g_presence_lost_calls.size() == 1 && g_presence_lost_calls[0].player == (uint32_t)s.player &&
               g_presence_lost_calls[0].mode == 0u,
           "C2: player_presence_lost(cur_player, mode=0) fires -- buildings_alive reached EXACTLY 0 "
           "(0x004737da CMP ...,0x0 / 0x004737e1 JNZ not taken)");
    }

    // =================================================================================================
    // C3 -- shuttle_slot bound but MISMATCHING prod_queue_slot[planet_index]: prod_unbind_planet does
    // NOT fire, but prod_shuttle_slot_release STILL fires -- the inverse of C2's unbind branch, pinning
    // "unconditional once the slot is nonzero, regardless of the unbind branch" (0x00473735 LAB is the
    // JNZ target AND the fallthrough of the unbind call -- both paths reach the release call).
    // =================================================================================================
    {
        Seed s;
        s.shuttle_slot                   = 6;
        s.prod_queue_slot_at_planet      = 9; // MISMATCHES shuttle_slot(6) -> unbind must NOT fire
        s.prod_queue_slot_at_index_decoy = 6; // MATCHES shuttle_slot -- if a bug indexed by cur_index
                                              // instead of planet_index, unbind would wrongly fire
        s.cur_building_energy_in       = 0.0; // isolate: no HQ credit noise in this case
        s.buildings_alive_at_planet_in = 5;   // isolate: no presence_lost noise in this case
        seed_and_run(fx, s);

        ck(trace_eq({"ai_notify_object_removed", "refund_resources_scaled_by_energy", "prod_shuttle_slot_release",
                     "bldg_unmap_footprint", "sight_add_circle", "game_SetEvent", "notify_ui"}),
           "C3: prod_unbind_planet is ABSENT from the trace (mismatch), prod_shuttle_slot_release still "
           "fires (0x00473721 JNZ TAKEN -> LAB_00473735, which is also 0x0047372e's own fallthrough)");
        ck(g_unbind_calls.empty(),
           "C3: prod_unbind_planet does NOT fire -- shuttle_slot(6) != prod_queue_slot[planet_index](9)");
        ck(g_shuttle_release_calls.size() == 1 && g_shuttle_release_calls[0].slot == (int32_t)s.shuttle_slot,
           "C3: prod_shuttle_slot_release still fires with shuttle_slot=6 even though the unbind branch "
           "was skipped -- the register-reuse/unconditional-release hazard this function shares with the "
           "shuttle-slot-release family");
    }

    // =================================================================================================
    // C4 -- HQ energy FP boundary, negative-zero side: cur_building.energy = -0.0. The asm's gate is a
    // SIGN-MASKED bit-pattern test (TEST high_dword,0x7fffffff OR low_dword!=0), which is exactly the
    // IEEE `!= 0.0` predicate -- and IEEE `-0.0 != 0.0` is FALSE, so the gate must NOT fire here either,
    // pinning that -0.0 is treated the same as +0.0 (both "masked zero"), not as some nonzero bit pattern.
    // =================================================================================================
    {
        Seed s;
        s.cur_building_energy_in = -0.0;
        s.hq_energy_in           = 77.0;
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).energy, -0.0,
                "C4: cur_building.energy left AT -0.0 (untouched) -- gate not taken for -0.0, matching the "
                "sign-masked bit-pattern test's equivalence to IEEE `-0.0 != 0.0` == false");
        ck_eq_d(fx.b(s.player, 0).energy, 77.0, "C4: HQ energy untouched -- no credit for -0.0 either");
    }

    // =================================================================================================
    // C5 -- HQ energy FP boundary, negative-nonzero side: cur_building.energy = -3.5. The sign-masked
    // bit-pattern test is nonzero-after-masking for ANY nonzero magnitude regardless of sign, so the
    // gate MUST fire here (a naive `energy > 0.0` translation would wrongly skip this).
    // =================================================================================================
    {
        Seed s;
        s.cur_building_energy_in = -3.5;
        s.hq_energy_in           = 10.0;
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).energy, 0.0,
                "C5: cur_building.energy zeroed -- gate taken for a NEGATIVE nonzero value (-3.5 != 0.0), "
                "pinning that the test is sign-agnostic, not `energy > 0.0`");
        ck_eq_d(fx.b(s.player, 0).energy, 9.0, "C5: HQ energy = 10.0 + (-1.0) = 9.0 -- credit still applied");
    }

    // =================================================================================================
    // C6 -- buildings_alive boundary, the "already past zero" side: buildings_alive_in = 0 decrements
    // to -1, which is NOT exactly 0, so player_presence_lost must NOT fire -- pinning the gate is
    // `== 0`, not `<= 0`.
    // =================================================================================================
    {
        Seed s;
        s.buildings_alive_at_planet_in = 0;
        s.cur_building_energy_in       = 0.0;
        s.shuttle_slot                 = 0;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.planet_index], (uint32_t)-1,
              "C6: buildings_alive[planet_index] decremented 0 -> -1");
        ck(g_presence_lost_calls.empty(),
           "C6: player_presence_lost does NOT fire for -1 -- the gate is EXACTLY `== 0`, not `<= 0` "
           "(0x004737da CMP ...,0x0 / 0x004737e1 JNZ taken for a nonzero result, -1 included)");
    }
}

} // namespace mh::sim::test
