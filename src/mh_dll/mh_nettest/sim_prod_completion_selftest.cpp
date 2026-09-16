//
// sim_prod_completion_selftest.cpp -- `simtest` cases for the SIM1D "production completion" pipeline:
//   llm_strat_production_complete       @0x0048d886  (sim/sim_prod_shuttle_complete.cpp)
//   llm_strat_prod_deliver_arrivals     @0x0048dc65  (sim/sim_prod_deliver_arrivals.cpp)
//   llm_strat_prod_spawn_arrived_unit   @0x0048f31e  (sim/sim_prod_spawn_arrived_unit.cpp)
//
// WHY THIS FILE IS THE ORACLE FOR THIS SLICE. All three are translated + reimpl-verify clean, but
// left `reviewed` (no evidence tier) because arming any of them under shadow double-fires real
// effects: production_complete/deliver_arrivals cascade into spawn_arrived_unit's llm_strat_unit_
// create + llm_strat_fx_anim_spawn/llm_snd_play, an unbounded, effectful write closure
// (shadow_region_closure.py reports each UNBOUNDED -- see each header's own DECLARED NEED section).
// SIM1D's own done_when clause 4 ("a production completion observed end-to-end in one ARMED run:
// queue slot status 0xc9 -> unit delivered -> slot freed") was closed as "blocked-by-closure" rather
// than satisfied for exactly this reason. This offline oracle is the practical alternative flagged
// at that closure: mock every outward call via each function's own `_calls` struct (Law 4 -- none of
// the three call each other's public `mh::sim::` wrapper directly, only through `_calls` -> `mh::
// call::<original>`) and verify each function's OWN logic + a manual state handoff between them.
//
// EVERY EXPECTED VALUE IS DERIVED FROM THE THREE HEADER BANNERS (sim_prod_shuttle_complete.h,
// sim_prod_deliver_arrivals.h, sim_prod_spawn_arrived_unit.h), which themselves cite the raw
// disassembly address-by-address -- not from the C++ translations under test. Re-cited inline where
// a specific branch/address matters.
//
// A caveat the shuttle-unload ring's oracle did not have: because the three do NOT call each other
// in-process (each reaches its neighbor only through `mh::call::<original>` when driven live), a
// true end-to-end run can only be simulated by the TEST manually threading state between two
// `detail::` calls in sequence (see test_pipeline_handoff_state below) -- weaker evidence than the
// ring's true single-boundary isolation, but still proves the STATE CONTRACT between stages, which
// is exactly what clause 4 asked to see.
//
#include <array>

#include "sim/sim_prod_deliver_arrivals.h"
#include "sim/sim_prod_shuttle_complete.h"
#include "sim/sim_prod_spawn_arrived_unit.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER       = 3;
constexpr uint16_t PLAYER_LOCAL = 7; // == sim_fixture::reset()'s player_side
constexpr int32_t  SLOT         = 4; // avoid slot 0 -- deliver_arrivals deliberately never visits it
constexpr int32_t  PLANET       = 2; // == sim_fixture::reset()'s planet_index default (0) is too easy
                                     // to satisfy by accident; tests that need "on the viewed planet"
                                     // set both dest_planet and f.planet_index to PLANET explicitly.

// cfg_enum_E_UNIT_TYPE members these three functions branch on -- same values as sim_order_enqueue.h
// / sim_unit_type_predicates.h, re-derived locally per this codebase's per-TU convention (every sim/
// TU declares its own copy of a small literal rather than sharing one).
constexpr uint32_t UNIT_TYPE_A_HELI_CARGO  = 0x17;
constexpr uint32_t UNIT_TYPE_A_HELI_MOTHER = 0x13;
constexpr uint32_t UNIT_TYPE_OTHER         = 0x05; // anything not in the four tested constants

constexpr int16_t STATUS_ARRIVED_READY_TO_SPAWN = static_cast<int16_t>(0xc9);
constexpr int16_t STATUS_DELIVERED              = static_cast<int16_t>(0xcc);

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)slot];
}

// ---- recorders -----------------------------------------------------------------------------------
// Captureless lambdas convert to plain function pointers, same shape as sim_prod_shuttle_unload_
// selftest.cpp's g_rec -- one file-scope recorder feeds all three `_calls` stub tables.
struct ev2 {
    int32_t a, b;
};
struct ev3 {
    int32_t a, b, c;
};

struct pc_recorder { // production_complete's own calls
    int32_t          w_str_copy_n = 0;
    int32_t          concat_n     = 0;
    std::vector<ev2> snd_play; // (sound_id, volume)
    int32_t          print_text_message_n       = 0;
    int32_t          locate_active_port_n       = 0;
    int32_t          find_mothership_position_n = 0;
    std::vector<ev2> slot_release; // (player, slot)
    int32_t          deliver_arrivals_n = 0;
    // control knobs
    uint32_t locate_ret          = 1;
    int32_t  locate_col          = 0;
    int32_t  locate_row          = 0;
    uint32_t find_mothership_ret = 1;
    int32_t  find_mothership_x   = 0;
    int32_t  find_mothership_y   = 0;
    void     reset() { *this = pc_recorder{}; }
};
pc_recorder g_pc;

struct da_recorder { // prod_deliver_arrivals's own calls
    int32_t                             locate_active_port_n = 0;
    std::vector<ev3>                    prod_bind_planet; // (player, queue_slot(planet), shuttle_slot)
    int32_t                             find_mothership_position_n = 0;
    std::vector<std::array<int32_t, 5>> spawn_arrived_unit; // (player, slot, a2, a3, a4)
    // control knobs
    uint32_t locate_ret          = 1;
    int32_t  locate_col          = 11;
    int32_t  locate_row          = 22;
    int32_t  locate_port_slot    = 33;
    int32_t  bind_ret            = 1;
    uint32_t find_mothership_ret = 1;
    int32_t  find_mothership_x   = 44;
    int32_t  find_mothership_y   = 55;
    uint32_t spawn_ret           = 0;
    void     reset() { *this = da_recorder{}; }
};
da_recorder g_da;

struct sa_recorder {                                  // prod_spawn_arrived_unit's own calls
    std::vector<std::array<uint32_t, 5>> unit_create; // (x, y, unit, player, is_ship)
    std::vector<std::array<int32_t, 3>>  snd_play_at; // (sound_id, tile_col, tile_row)
    int32_t                              fx_anim_spawn_n = 0;
    std::vector<std::array<int32_t, 5>>  exit_storage_auto; // (player, unit_id, storage_idx, x, y)
    // control knobs
    uint32_t create_ret = 0;
    void     reset() { *this = sa_recorder{}; }
};
sa_recorder g_sa;

const production_complete_calls &rec_pc_calls() {
    static const production_complete_calls c = {
        [](void *, void *) -> void * {
            g_pc.w_str_copy_n++;
            return nullptr;
        },
        [](void *, void *) -> void * {
            g_pc.concat_n++;
            return nullptr;
        },
        [](int32_t sound_id, int32_t vol) { g_pc.snd_play.push_back({sound_id, vol}); },
        [](void *) -> uint32_t {
            g_pc.print_text_message_n++;
            return 0;
        },
        [](uint32_t, int32_t *out_col, int32_t *out_row, uint32_t *) -> uint32_t {
            g_pc.locate_active_port_n++;
            *out_col = g_pc.locate_col;
            *out_row = g_pc.locate_row;
            return g_pc.locate_ret;
        },
        [](int32_t, uint32_t *out_x, uint32_t *out_y) -> uint32_t {
            g_pc.find_mothership_position_n++;
            *out_x = (uint32_t)g_pc.find_mothership_x;
            *out_y = (uint32_t)g_pc.find_mothership_y;
            return g_pc.find_mothership_ret;
        },
        [](int32_t p, int32_t slot) { g_pc.slot_release.push_back({p, slot}); },
        []() { g_pc.deliver_arrivals_n++; },
    };
    return c;
}

const prod_deliver_arrivals_calls &rec_da_calls() {
    static const prod_deliver_arrivals_calls c = {
        [](uint32_t, int32_t *out_col, int32_t *out_row, uint32_t *out_port_slot) -> uint32_t {
            g_da.locate_active_port_n++;
            *out_col       = g_da.locate_col;
            *out_row       = g_da.locate_row;
            *out_port_slot = (uint32_t)g_da.locate_port_slot;
            return g_da.locate_ret;
        },
        [](int32_t p, int32_t queue_slot, int32_t shuttle_slot) -> int32_t {
            g_da.prod_bind_planet.push_back({p, queue_slot, shuttle_slot});
            return g_da.bind_ret;
        },
        [](int32_t, uint32_t *out_x, uint32_t *out_y) -> uint32_t {
            g_da.find_mothership_position_n++;
            *out_x = (uint32_t)g_da.find_mothership_x;
            *out_y = (uint32_t)g_da.find_mothership_y;
            return g_da.find_mothership_ret;
        },
        [](uint16_t p, uint32_t slot, uint32_t a2, uint32_t a3, int32_t a4) -> uint32_t {
            g_da.spawn_arrived_unit.push_back(
                {(int32_t)p, (int32_t)slot, (int32_t)a2, (int32_t)a3, a4});
            return g_da.spawn_ret;
        },
    };
    return c;
}

const prod_spawn_arrived_unit_calls &rec_sa_calls() {
    static const prod_spawn_arrived_unit_calls c = {
        [](uint32_t x, uint32_t y, uint16_t unit, uint16_t player, uint8_t is_ship) -> uint32_t {
            g_sa.unit_create.push_back({x, y, unit, player, is_ship});
            return g_sa.create_ret;
        },
        [](int32_t sound_id, int32_t tile_col, int32_t tile_row) {
            g_sa.snd_play_at.push_back({sound_id, tile_col, tile_row});
        },
        [](uint32_t, uint32_t, uint32_t, double, uint32_t) -> uint32_t {
            g_sa.fx_anim_spawn_n++;
            return 0;
        },
        [](uint32_t player, uint32_t unit_id, int32_t storage_idx, uint32_t x, uint32_t y) {
            g_sa.exit_storage_auto.push_back(
                {(int32_t)player, (int32_t)unit_id, storage_idx, (int32_t)x, (int32_t)y});
        },
    };
    return c;
}

// ==== production_complete ==========================================================================
// From sim_prod_shuttle_complete.h/.cpp: the entry-point 16-bit masking (both player and slot), the
// travel_duration<=elapsed_time gate, the throttle bump (cap 5), the throttle==10 quiet-release
// short-circuit gated on cfg_units[type].type NOT being a heli-mother, the local-player message/pan
// block, and the unconditional tail (travel_duration=0, status=0xc9, origin_planet=dest_planet,
// prod_deliver_arrivals() always).

void test_pc_transit_not_finished_decrements_only() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.travel_duration    = 10.0;
    s.status             = 0; // untouched sentinel

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER, SLOT, 3.0);

    ck_eq_d(s.travel_duration, 7.0, "pc: travel_duration(10) > elapsed(3) -> decrement only -> 7.0");
    ck_eq((uint32_t)s.status, 0u, "pc: transit not finished -> status untouched");
    ck_eq((uint32_t)g_pc.deliver_arrivals_n, 0u, "pc: transit not finished -> no deliver_arrivals call");
}

void test_pc_finished_bumps_throttle_and_always_calls_deliver_arrivals() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s     = slot_of(f, PLAYER, SLOT);
    s.travel_duration        = 3.0;
    s.dest_planet            = 9; // != fixture's planet_index(0) and != PLAYER_LOCAL's compare path
    f.prod_complete_throttle = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER, SLOT, 5.0);

    ck_eq((uint32_t)f.prod_complete_throttle, 1u, "pc: throttle bumped 0 -> 1 on a finished transit");
    ck_eq_d(s.travel_duration, 0.0, "pc: tail always zeroes travel_duration");
    ck_eq((uint32_t)(uint16_t)s.status, (uint32_t)(uint16_t)STATUS_ARRIVED_READY_TO_SPAWN,
          "pc: tail always sets status=0xc9");
    ck_eq((uint32_t)(uint16_t)s.origin_planet, (uint32_t)(uint16_t)s.dest_planet,
          "pc: tail always sets origin_planet=dest_planet");
    ck_eq((uint32_t)g_pc.deliver_arrivals_n, 1u,
          "pc: exactly one deliver_arrivals call regardless of the local-player gate");
    ck_eq((uint32_t)g_pc.print_text_message_n, 0u, "pc: non-local player -> no message printed");
}

void test_pc_throttle_bump_caps_at_five() {
    sim_fixture f;
    g_pc.reset();
    slot_of(f, PLAYER, SLOT).travel_duration = 1.0;
    f.prod_complete_throttle                 = 5;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER, SLOT, 2.0);

    ck_eq((uint32_t)f.prod_complete_throttle, 5u,
          "pc: throttle already at THROTTLE_INCREMENT_CAP(5) -> bump is a no-op");
}

void test_pc_local_player_off_planet_prints_message_no_pan() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s  = slot_of(f, PLAYER_LOCAL, SLOT);
    s.travel_duration     = 1.0;
    s.dest_planet         = 5;
    s.type_ref_id         = 1;
    f.planet_index        = 0; // dest(5) != viewed(0) -> off-planet
    f.text_ptrs[0x1d]     = L"stub";
    f.cfg_planets[5].name = 7;
    f.text_ptrs[7]        = L"Planet Five";

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER_LOCAL, SLOT, 3.0);

    ck_eq((uint32_t)g_pc.w_str_copy_n, 1u, "pc(local, off-planet): base message copied once");
    ck_eq((uint32_t)g_pc.concat_n, 3u,
          "pc(local, off-planet): 3 concats (prefix + planet name + suffix)");
    ck_eq((uint32_t)g_pc.print_text_message_n, 1u, "pc(local, off-planet): message printed once");
    ck_eq((uint32_t)g_pc.locate_active_port_n, 0u, "pc(local, off-planet): no camera pan at all");
    ck_eq((uint32_t)g_pc.find_mothership_position_n, 0u, "pc(local, off-planet): no camera pan at all");
    ck_eq((uint32_t)g_pc.snd_play.size(), 0u, "pc(local, off-planet): no arrival sound off-planet");
}

void test_pc_local_player_on_planet_cargo_heli_pans_via_locate_port() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER_LOCAL, SLOT);
    s.travel_duration    = 1.0;
    s.dest_planet        = 4;
    s.type_ref_id        = 9;
    f.planet_index       = 4; // dest == viewed -> pan block runs
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_A_HELI_CARGO;
    f.text_ptrs[0x1d]    = L"stub";
    g_pc.locate_ret      = 1;
    g_pc.locate_col      = 320; // fine coords, divisible by 32 for a clean fine_to_tile check
    g_pc.locate_row      = 640;
    f.sim_active         = 1;
    f.player_race        = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER_LOCAL, SLOT, 3.0);

    ck_eq((uint32_t)g_pc.locate_active_port_n, 1u,
          "pc(local, on-planet, cargo-heli): locate_active_port called once");
    ck_eq((uint32_t)g_pc.find_mothership_position_n, 0u,
          "pc(local, on-planet, cargo-heli): mothership locator NOT called");
    ck(g_pc.snd_play.size() == 1 && g_pc.snd_play[0].a == 0xe,
       "pc(local, cargo-heli, race!=2): snd_play(sound=0xe, vol=100)");
    ck_eq((int32_t)f.cam_pan_target_col, 320 / 32, "pc: locate succeeded -> col = fine_to_tile(320)");
    ck_eq((int32_t)f.cam_pan_target_row, 640 / 32, "pc: locate succeeded -> row = fine_to_tile(640)");
}

void test_pc_local_player_on_planet_mothership_pans_via_find_mothership() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s     = slot_of(f, PLAYER_LOCAL, SLOT);
    s.travel_duration        = 1.0;
    s.dest_planet            = 4;
    s.type_ref_id            = 9;
    f.planet_index           = 4;
    f.cfg_units[9].type      = (int32_t)UNIT_TYPE_A_HELI_MOTHER; // NOT a cargo-heli -> mothership arm
    f.text_ptrs[0x1d]        = L"stub";
    g_pc.find_mothership_ret = 1;
    g_pc.find_mothership_x   = 96;
    g_pc.find_mothership_y   = 64;
    f.sim_active             = 1;
    f.player_race            = 2; // race==2 adds RACE2_SOUND_OFFSET(0x12)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER_LOCAL, SLOT, 3.0);

    ck_eq((uint32_t)g_pc.find_mothership_position_n, 1u,
          "pc(local, on-planet, mothership): find_mothership_position called once");
    ck_eq((uint32_t)g_pc.locate_active_port_n, 0u,
          "pc(local, on-planet, mothership): port locator NOT called");
    ck(g_pc.snd_play.size() == 1 && g_pc.snd_play[0].a == (0x12 + 0xb),
       "pc(local, mothership, race==2): snd_play(sound=0x12+0xb=0x1d, vol=100)");
    ck_eq((int32_t)f.cam_pan_target_col, 96 / 32, "pc: mothership locate succeeded -> col=fine_to_tile(96)");
    ck_eq((int32_t)f.cam_pan_target_row, 64 / 32, "pc: mothership locate succeeded -> row=fine_to_tile(64)");
}

void test_pc_locator_failure_sets_col_only_row_untouched() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER_LOCAL, SLOT);
    s.travel_duration    = 1.0;
    s.dest_planet        = 4;
    s.type_ref_id        = 9;
    f.planet_index       = 4;
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_A_HELI_CARGO;
    f.text_ptrs[0x1d]    = L"stub";
    g_pc.locate_ret      = 0;      // FAILS
    g_pc.locate_row      = 0x1357; // distinguishable from 0/the col fallback -- the real callee
                                   // writes ITS OWN value into row via the out-param regardless of
                                   // its return; the point of this test is that the CALLER's
                                   // post-processing does not additionally touch row on failure
                                   // (only col gets forced to -1), so whatever the callee left there
                                   // must survive exactly.

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER_LOCAL, SLOT, 3.0);

    ck_eq((int32_t)f.cam_pan_target_col, -1, "pc: locator failed -> col set to -1");
    ck_eq((int32_t)f.cam_pan_target_row, 0x1357,
          "pc: locator failed -> row deliberately left as whatever the callee wrote (not additionally "
          "reset), per the header banner");
}

void test_pc_throttle_10_non_mother_quiet_release() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s     = slot_of(f, PLAYER, SLOT);
    s.travel_duration        = 1.0;
    s.type_ref_id            = 9;
    f.prod_complete_throttle = 10; // direct state seed -- the bump logic itself caps at 5 and can
                                   // never organically reach 10 (see the header's own uncertainties[]
                                   // note); this is the only way to exercise the branch at all.
    f.cfg_units[9].type = (int32_t)UNIT_TYPE_OTHER;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER, SLOT, 3.0);

    ck_eq((uint32_t)g_pc.slot_release.size(), 1u,
          "pc(throttle==10, non-mother): exactly one quiet slot_release");
    ck(g_pc.slot_release.size() == 1 && g_pc.slot_release[0].a == PLAYER &&
           g_pc.slot_release[0].b == SLOT,
       "pc(throttle==10, non-mother): prod_shuttle_slot_release(player=3, slot=4)");
    ck_eq((uint32_t)g_pc.deliver_arrivals_n, 0u,
          "pc(throttle==10, non-mother): quiet release skips the unconditional tail entirely -- no "
          "deliver_arrivals");
    ck_eq((uint32_t)g_pc.print_text_message_n, 0u,
          "pc(throttle==10, non-mother): no message either");
}

void test_pc_throttle_10_heli_mother_falls_through_to_normal_path() {
    sim_fixture f;
    g_pc.reset();
    prod_shuttle_slot &s     = slot_of(f, PLAYER, SLOT);
    s.travel_duration        = 1.0;
    s.type_ref_id            = 9;
    s.dest_planet            = 8;
    f.prod_complete_throttle = 10;
    f.cfg_units[9].type      = (int32_t)UNIT_TYPE_A_HELI_MOTHER; // IS a heli-mother -> falls through

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER, SLOT, 3.0);

    ck_eq((uint32_t)g_pc.slot_release.size(), 0u,
          "pc(throttle==10, heli-mother): NOT a quiet release -- falls through");
    ck_eq((uint32_t)g_pc.deliver_arrivals_n, 1u,
          "pc(throttle==10, heli-mother): normal path -> unconditional tail -> deliver_arrivals called");
    ck_eq((uint32_t)(uint16_t)s.status, (uint32_t)(uint16_t)STATUS_ARRIVED_READY_TO_SPAWN,
          "pc(throttle==10, heli-mother): tail still runs -> status=0xc9");
}

// REGRESSION: same class as sim_bldg_flush_cargo_hold.cpp's own truncation fix (reimpl-verify,
// 2026-08-13) -- player/slot are re-narrowed to their low 16 bits at every record-address computation
// and every outward call.
void test_pc_dirty_high_bits_masked_regression() {
    sim_fixture f;
    g_pc.reset();
    const uint32_t player_dirty              = 0x00010000u | PLAYER;         // low16 == 3, garbage ONLY above bit 15
    const uint32_t slot_dirty                = 0x00020000u | (uint32_t)SLOT; // low16 == 4, garbage ONLY above bit 15
    slot_of(f, PLAYER, SLOT).travel_duration = 1.0;
    slot_of(f, PLAYER, SLOT).type_ref_id     = 9;
    f.prod_complete_throttle                 = 10;
    f.cfg_units[9].type                      = (int32_t)UNIT_TYPE_OTHER; // non-mother -> the slot_release path (cheapest to
                                                                         // observe the masked args on)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), player_dirty, slot_dirty, 3.0);

    ck(g_pc.slot_release.size() == 1 && g_pc.slot_release[0].a == PLAYER &&
           g_pc.slot_release[0].b == SLOT,
       "pc(dirty bits): upper 16 bits masked off both args -> slot_release(player=3, slot=4)");
}

// ==== prod_deliver_arrivals =========================================================================
// From sim_prod_deliver_arrivals.h/.cpp: the double loop (player 0..7, slot 1..9 -- slot 0 NEVER
// visited), the arrival gate (status==0xc9 && origin_planet==planet_index), the cargo-heli vs.
// mothership type split, and the local-player control-group auto-select's five chained gates.

void test_da_no_arrivals_gate_closed_zero_calls() {
    sim_fixture f;
    g_da.reset();
    // every slot defaults to status=0 (memset) -- gate never opens.

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.locate_active_port_n, 0u, "da: no arrived slots -> zero locate_active_port calls");
    ck_eq((uint32_t)g_da.find_mothership_position_n, 0u,
          "da: no arrived slots -> zero find_mothership_position calls");
    ck_eq((uint32_t)g_da.spawn_arrived_unit.size(), 0u, "da: no arrived slots -> zero spawn calls");
}

void test_da_slot_zero_never_visited() {
    sim_fixture f;
    g_da.reset();
    // Slot 0, fully arrived + cargo-heli -- must be SKIPPED (the inner loop starts at 1).
    prod_shuttle_slot &s0 = slot_of(f, PLAYER, 0);
    s0.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s0.origin_planet      = 0;
    s0.type_ref_id        = 9;
    f.planet_index        = 0;
    f.cfg_units[9].type   = (int32_t)UNIT_TYPE_A_HELI_CARGO;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.locate_active_port_n, 0u,
          "da: slot 0 is fully 'arrived' but the loop starts at slot 1 -- must be invisible");
}

void test_da_cargo_heli_success_full_chain() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_A_HELI_CARGO;
    g_da.locate_ret      = 1;
    g_da.bind_ret        = 1;
    // persistent camera-pan globals must stay untouched -- this call site uses LOCAL scratch, per the
    // header banner's explicit note that this is a DIFFERENT out-param target from production_complete's.
    f.cam_pan_target_col = 0x1234;
    f.cam_pan_target_row = 0x5678;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.locate_active_port_n, 1u, "da(cargo-heli): locate_active_port called once");
    ck(g_da.prod_bind_planet.size() == 1 && g_da.prod_bind_planet[0].a == PLAYER &&
           g_da.prod_bind_planet[0].b == PLANET && g_da.prod_bind_planet[0].c == SLOT,
       "da(cargo-heli): prod_bind_planet(player=3, queue_slot=planet_index, shuttle_slot=4)");
    ck(g_da.spawn_arrived_unit.size() == 1 && g_da.spawn_arrived_unit[0][0] == PLAYER &&
           g_da.spawn_arrived_unit[0][1] == SLOT && g_da.spawn_arrived_unit[0][2] == g_da.locate_col &&
           g_da.spawn_arrived_unit[0][3] == g_da.locate_row,
       "da(cargo-heli): spawn_arrived_unit(player, slot, LOCAL col, LOCAL row, port_slot) -- NOT the "
       "persistent cam-pan globals");
    ck_eq((int32_t)f.cam_pan_target_col, 0x1234, "da(cargo-heli): persistent CAM_PAN_TARGET_COL untouched");
    ck_eq((int32_t)f.cam_pan_target_row, 0x5678, "da(cargo-heli): persistent CAM_PAN_TARGET_ROW untouched");
}

void test_da_cargo_heli_locate_fails_no_bind_no_spawn() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_A_HELI_CARGO;
    g_da.locate_ret      = 0; // FAILS

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.prod_bind_planet.size(), 0u,
          "da(cargo-heli, locate fails): prod_bind_planet NOT called");
    ck_eq((uint32_t)g_da.spawn_arrived_unit.size(), 0u,
          "da(cargo-heli, locate fails): spawn_arrived_unit NOT called");
}

void test_da_cargo_heli_bind_fails_no_spawn() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_A_HELI_CARGO;
    g_da.locate_ret      = 1;
    g_da.bind_ret        = 0; // FAILS

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.prod_bind_planet.size(), 1u, "da(cargo-heli, bind fails): bind WAS attempted");
    ck_eq((uint32_t)g_da.spawn_arrived_unit.size(), 0u,
          "da(cargo-heli, bind fails): spawn_arrived_unit NOT called");
}

void test_da_mothership_success_locator_ok() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s     = slot_of(f, PLAYER, SLOT);
    s.status                 = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet          = PLANET;
    s.type_ref_id            = 9;
    f.planet_index           = PLANET;
    f.cfg_units[9].type      = (int32_t)UNIT_TYPE_A_HELI_MOTHER;
    g_da.find_mothership_ret = 1;
    g_da.find_mothership_x   = 111;
    g_da.find_mothership_y   = 222;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.find_mothership_position_n, 1u,
          "da(mothership): find_mothership_position called once");
    ck(g_da.spawn_arrived_unit.size() == 1 && g_da.spawn_arrived_unit[0][0] == PLAYER &&
           g_da.spawn_arrived_unit[0][1] == SLOT && g_da.spawn_arrived_unit[0][2] == 111 &&
           g_da.spawn_arrived_unit[0][3] == 222 && g_da.spawn_arrived_unit[0][4] == 0,
       "da(mothership): spawn_arrived_unit(player, slot, x=111, y=222, LITERAL 0) -- the 5th arg is "
       "literal 0, unlike the cargo branch's port_slot pass-through");
}

void test_da_mothership_locator_fails_falls_back_to_landing_site() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s                 = slot_of(f, PLAYER, SLOT);
    s.status                             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet                      = PLANET;
    s.type_ref_id                        = 9;
    f.planet_index                       = PLANET;
    f.cfg_units[9].type                  = (int32_t)UNIT_TYPE_A_HELI_MOTHER;
    g_da.find_mothership_ret             = 0; // FAILS
    f.profiles[PLAYER].landing_x[PLANET] = 777;
    f.profiles[PLAYER].landing_y[PLANET] = 888;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck(g_da.spawn_arrived_unit.size() == 1 && g_da.spawn_arrived_unit[0][2] == 777 &&
           g_da.spawn_arrived_unit[0][3] == 888,
       "da(mothership, locator fails): falls back to profiles[player].landing_x/y[planet]");
}

void test_da_ctrl_group_autoselect_all_gates_pass() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s                = slot_of(f, PLAYER_LOCAL, SLOT);
    s.status                            = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet                     = PLANET;
    s.type_ref_id                       = 9;
    f.planet_index                      = PLANET;
    f.cfg_units[9].type                 = (int32_t)UNIT_TYPE_A_HELI_MOTHER;
    g_da.find_mothership_ret            = 1;
    g_da.spawn_ret                      = 42; // > 0, signed
    f.sim_active                        = 0;  // gate 2
    f.planet_status[PLANET]             = 0;  // gate 3: PLANET_STATUS_UNKNOWN
    f.u(PLAYER_LOCAL, 42).unit_proto_id = 5;  // gate 5: unit_of(...).unit_proto_id != 0

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls()); // player == PLAYER_LOCAL -> gate 1

    ck_eq((uint32_t)f.ctrl_groups[0].count, 1u,
          "da(ctrl-group autoselect, all 5 gates pass): ctrl_groups[0].count = 1");
    ck_eq((uint32_t)f.ctrl_groups[0].unit_ids[0], 42u,
          "da(ctrl-group autoselect): ctrl_groups[0].unit_ids[0] = spawned(42)");
}

void test_da_ctrl_group_autoselect_wrong_player_gate_fails() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s          = slot_of(f, PLAYER, SLOT); // NOT PLAYER_LOCAL
    s.status                      = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet               = PLANET;
    s.type_ref_id                 = 9;
    f.planet_index                = PLANET;
    f.cfg_units[9].type           = (int32_t)UNIT_TYPE_A_HELI_MOTHER;
    g_da.find_mothership_ret      = 1;
    g_da.spawn_ret                = 42;
    f.sim_active                  = 0;
    f.planet_status[PLANET]       = 0;
    f.u(PLAYER, 42).unit_proto_id = 5;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)f.ctrl_groups[0].count, 0u,
          "da(ctrl-group autoselect, wrong player): gate 1 fails -> ctrl_groups[0] untouched");
}

void test_da_neither_type_no_action() {
    sim_fixture f;
    g_da.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_OTHER;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.locate_active_port_n, 0u, "da(neither type): no cargo-heli path");
    ck_eq((uint32_t)g_da.find_mothership_position_n, 0u, "da(neither type): no mothership path");
    ck_eq((uint32_t)g_da.spawn_arrived_unit.size(), 0u, "da(neither type): no spawn at all");
}

// ==== prod_spawn_arrived_unit =======================================================================
// From sim_prod_spawn_arrived_unit.h/.cpp: slot masked to 16 bits, the 3-condition gate, the failure
// path's fallback FX/sound at a PIXEL-space-masked position (general.bw_mask/bh_mask, NOT the
// tile-space width_mask/height_mask), the success path's field writes + primary-mother election +
// status=0xcc, and the storage-exit orientation block gated on storage_idx!=0.

void test_sa_gate_fails_wrong_status_returns_zero() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = 0; // not 0xc9
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 1, 1, 0);

    ck_eq(r, 0u, "sa(gate: wrong status): returns 0");
    ck_eq((uint32_t)g_sa.unit_create.size(), 0u, "sa(gate: wrong status): unit_create NOT called");
}

void test_sa_gate_fails_wrong_planet_returns_zero() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET + 1; // mismatch

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 1, 1, 0);

    ck_eq(r, 0u, "sa(gate: wrong planet): returns 0");
    ck_eq((uint32_t)g_sa.unit_create.size(), 0u, "sa(gate: wrong planet): unit_create NOT called");
}

void test_sa_gate_fails_zero_type_returns_zero() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 0; // gate 3 fails
    f.planet_index       = PLANET;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 1, 1, 0);

    ck_eq(r, 0u, "sa(gate: type_ref_id==0): returns 0");
    ck_eq((uint32_t)g_sa.unit_create.size(), 0u, "sa(gate: type_ref_id==0): unit_create NOT called");
}

void test_sa_create_fails_fallback_fx_and_sound() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s       = slot_of(f, PLAYER, SLOT);
    s.status                   = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet            = PLANET;
    s.type_ref_id              = 9;
    f.planet_index             = PLANET;
    f.cfg_units[9].sound_explo = 0x77;
    g_sa.create_ret            = 0; // FAILS
    f.sim_active               = 1;
    f.geom.bw_mask             = 0xff;
    f.geom.bh_mask             = 0xff;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck_eq(r, 0u, "sa(create fails): returns 0");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).is_heli_mother_pending, 0u,
          "sa(create fails): is_heli_mother_pending still cleared (step 1 runs before the create call)");
    ck(g_sa.snd_play_at.size() == 1 && g_sa.snd_play_at[0][0] == 0x77,
       "sa(create fails, sim_active): snd_play_at(sound=cfg_units[type].sound_explo, tile) -- the "
       "original offscreen_snd_volume+snd_play pair as one record");
    ck_eq((uint32_t)g_sa.fx_anim_spawn_n, 1u,
          "sa(create fails): fx_anim_spawn ALWAYS fires on the failure path (unconditional)");
}

void test_sa_create_fails_sim_inactive_skips_sound_not_fx() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    g_sa.create_ret      = 0;
    f.sim_active         = 0; // INACTIVE

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck_eq(r, 0u, "sa(create fails, sim inactive): returns 0");
    ck_eq((uint32_t)g_sa.snd_play_at.size(), 0u,
          "sa(create fails, sim inactive): snd_play_at NOT called");
    ck_eq((uint32_t)g_sa.fx_anim_spawn_n, 1u,
          "sa(create fails, sim inactive): fx_anim_spawn STILL fires -- unconditional either way");
}

void test_sa_create_succeeds_sets_unit_fields_and_status() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s     = slot_of(f, PLAYER, SLOT);
    s.status                 = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet          = PLANET;
    s.type_ref_id            = 9;
    f.planet_index           = PLANET;
    f.cfg_units[9].elevation = 500;
    f.cfg_units[9].type      = (int32_t)UNIT_TYPE_OTHER; // not a mother -> no election path here
    g_sa.create_ret          = 77;
    // The stub does not populate the created record the way the real llm_strat_unit_create would --
    // seed unit_proto_id ourselves so the elevation/type re-reads the function performs AFTER the
    // create call have something real to read, same posture as every other sim/ oracle's create-style
    // stubs (e.g. sim_unit_create's own siblings).
    f.u(PLAYER, 77).unit_proto_id = 9;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck_eq(r, 77u, "sa(create succeeds): returns the new unit id");
    unit &nu = f.u(PLAYER, 77);
    ck_eq((uint32_t)nu.elevation, 800u, "sa(create succeeds): elevation = cfg_units[proto].elevation(500) + 300");
    ck_eq((uint32_t)nu.state, 0x30u, "sa(create succeeds): state = UNIT_STATE_DESCEND_CRUISE(0x30)");
    ck_eq((uint32_t)nu.order, 0x13u, "sa(create succeeds): order = UNIT_STATE_IDLE_SCATTER(0x13)");
    ck_eq((uint32_t)nu.shuttle_slot, (uint32_t)SLOT, "sa(create succeeds): shuttle_slot = slot");
    ck_eq((uint32_t)(uint16_t)slot_of(f, PLAYER, SLOT).status, (uint32_t)(uint16_t)STATUS_DELIVERED,
          "sa(create succeeds): the shuttle slot's own status -> 0xcc (delivered)");
}

void test_sa_mother_type_elects_primary_when_none_set() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s                           = slot_of(f, PLAYER, SLOT);
    s.status                                       = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet                                = PLANET;
    s.type_ref_id                                  = 9;
    f.planet_index                                 = PLANET;
    f.cfg_units[9].type                            = (int32_t)UNIT_TYPE_A_HELI_MOTHER;
    g_sa.create_ret                                = 55;
    f.u(PLAYER, 55).unit_proto_id                  = 9;
    f.profiles[PLAYER].primary_mother_unit[PLANET] = 0;
    f.profiles[PLAYER].primary_mother_bldg[PLANET] = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck_eq((uint32_t)f.profiles[PLAYER].primary_mother_unit[PLANET], 55u,
          "sa(mother, none set): elected as the new primary_mother_unit");
}

void test_sa_mother_type_does_not_reelect_when_bldg_already_primary() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s                           = slot_of(f, PLAYER, SLOT);
    s.status                                       = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet                                = PLANET;
    s.type_ref_id                                  = 9;
    f.planet_index                                 = PLANET;
    f.cfg_units[9].type                            = (int32_t)UNIT_TYPE_A_HELI_MOTHER;
    g_sa.create_ret                                = 55;
    f.u(PLAYER, 55).unit_proto_id                  = 9;
    f.profiles[PLAYER].primary_mother_unit[PLANET] = 0;
    f.profiles[PLAYER].primary_mother_bldg[PLANET] = 999; // ALREADY has a primary (building form)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck_eq((uint32_t)f.profiles[PLAYER].primary_mother_unit[PLANET], 0u,
          "sa(mother, bldg already primary): primary_mother_unit NOT overwritten");
}

void test_sa_non_mother_type_no_primary_mother_touch() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s          = slot_of(f, PLAYER, SLOT);
    s.status                      = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet               = PLANET;
    s.type_ref_id                 = 9;
    f.planet_index                = PLANET;
    f.cfg_units[9].type           = (int32_t)UNIT_TYPE_OTHER;
    g_sa.create_ret               = 55;
    f.u(PLAYER, 55).unit_proto_id = 9;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck_eq((uint32_t)f.profiles[PLAYER].primary_mother_unit[PLANET], 0u,
          "sa(non-mother): primary_mother_unit untouched");
    ck_eq((uint32_t)f.profiles[PLAYER].primary_mother_bldg[PLANET], 0u,
          "sa(non-mother): primary_mother_bldg untouched");
}

void test_sa_storage_idx_zero_no_orientation_no_exit_order() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    g_sa.create_ret      = 55;
    unit &nu             = f.u(PLAYER, 55);
    nu.unit_proto_id     = 9;
    nu.move_heading      = 0x5a;
    nu.facing_target     = 0x5a;
    nu.facing_current    = 0x5a;
    nu.home_storage_slot = 0x5a;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0); // storage_idx=0

    ck_eq((uint32_t)nu.move_heading, 0x5au, "sa(storage_idx=0): move_heading untouched");
    ck_eq((uint32_t)nu.facing_target, 0x5au, "sa(storage_idx=0): facing_target untouched");
    ck_eq((uint32_t)nu.home_storage_slot, 0x5au, "sa(storage_idx=0): home_storage_slot untouched");
    ck_eq((uint32_t)g_sa.exit_storage_auto.size(), 0u,
          "sa(storage_idx=0): unit_order_exit_storage_auto NOT called");
}

void test_sa_storage_idx_nonzero_orients_and_issues_exit_order() {
    sim_fixture f;
    g_sa.reset();
    constexpr int32_t  STORAGE_IDX = 3;
    prod_shuttle_slot &s           = slot_of(f, PLAYER, SLOT);
    s.status                       = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet                = PLANET;
    s.type_ref_id                  = 9;
    f.planet_index                 = PLANET;
    g_sa.create_ret                = 55;
    f.u(PLAYER, 55).unit_proto_id  = 9;

    f.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].b_index = 6;
    f.b(PLAYER, 6).building_id                                   = 12;
    f.cfg_buildings[12].door_approach_route[0]                   = 5;    // a heading 0..7
    f.move_microsteps[5 * MICROSTEPS_PER_HEADING + 0].facing     = 0x2a; // move_microstep==0 default

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, STORAGE_IDX);

    unit &nu = f.u(PLAYER, 55);
    ck_eq((uint32_t)nu.move_heading, 5u, "sa(storage_idx!=0): move_heading = cfg_buildings[bid].facing");
    ck_eq((uint32_t)nu.facing_target, 0x2au,
          "sa(storage_idx!=0): facing_target = move_microsteps[heading][microstep].facing");
    ck_eq((uint32_t)nu.facing_current, 0x2au,
          "sa(storage_idx!=0): facing_current = SAME value (single computation, not the original's "
          "redundant second recompute)");
    ck_eq((uint32_t)nu.home_storage_slot, (uint32_t)STORAGE_IDX,
          "sa(storage_idx!=0): home_storage_slot = storage_idx");
    ck(g_sa.exit_storage_auto.size() == 1 && g_sa.exit_storage_auto[0][0] == PLAYER &&
           g_sa.exit_storage_auto[0][1] == 55 && g_sa.exit_storage_auto[0][2] == STORAGE_IDX &&
           g_sa.exit_storage_auto[0][3] == 100 && g_sa.exit_storage_auto[0][4] == 200,
       "sa(storage_idx!=0): unit_order_exit_storage_auto(player, new_unit_id, storage_idx, x, y)");
}

void test_sa_slot_high_bits_masked_regression() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s          = slot_of(f, PLAYER, SLOT);
    s.status                      = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet               = PLANET;
    s.type_ref_id                 = 9;
    f.planet_index                = PLANET;
    g_sa.create_ret               = 66;
    f.u(PLAYER, 66).unit_proto_id = 9;
    const uint32_t slot_dirty     = 0x00fe0000u | (uint32_t)SLOT;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r   = detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, slot_dirty, 100,
                                                         200, 0);

    ck_eq(r, 66u, "sa(dirty slot bits): upper 16 bits masked -> gate reads slot=4 -> succeeds");
    ck_eq((uint32_t)(uint16_t)slot_of(f, PLAYER, SLOT).status, (uint32_t)(uint16_t)STATUS_DELIVERED,
          "sa(dirty slot bits): the LOW-16 slot record (slot=4) is the one updated");
}

void test_sa_is_ship_literal_is_2_not_1() {
    sim_fixture f;
    g_sa.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.status             = STATUS_ARRIVED_READY_TO_SPAWN;
    s.origin_planet      = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET;
    g_sa.create_ret      = 0; // don't care about the success path here

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_spawn_arrived_unit(v, own, rec_sa_calls(), PLAYER, SLOT, 100, 200, 0);

    ck(g_sa.unit_create.size() == 1 && g_sa.unit_create[0][4] == 2u,
       "sa: unit_create's is_ship argument is the LITERAL 2 (MOV EAX,0x2), not true/1 -- a real "
       "correction the .c draft's decompile got wrong (see the header banner)");
}

// ==== pipeline: the state handoff between production_complete and prod_deliver_arrivals ============
// The two do not call each other in-process (Law 4 -- only through `_calls` -> `mh::call::<original>`
// when live), so this test manually threads the SAME fixture's prod_shuttle_slots state from a REAL
// prod_shuttle_complete call into a REAL prod_deliver_arrivals call, proving the tail write
// (status=0xc9, origin_planet=dest_planet) is exactly what the next stage's arrival gate accepts --
// the "queue slot 0xc8/0xc9 -> unit delivered" contract SIM1D's closed done_when clause 4 wanted
// observed, minus the live game process.
void test_pipeline_handoff_state() {
    sim_fixture f;
    g_pc.reset();
    g_da.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, SLOT);
    s.travel_duration    = 1.0; // finishes this call
    s.dest_planet        = PLANET;
    s.type_ref_id        = 9;
    f.planet_index       = PLANET; // deliver_arrivals' own gate compares origin_planet==planet_index
    f.cfg_units[9].type  = (int32_t)UNIT_TYPE_A_HELI_CARGO;

    sim_view  v   = f.view();
    sim_store own = f.store();

    // Stage 1: production_complete, with deliver_arrivals STUBBED (we call the real one ourselves next).
    detail::prod_shuttle_complete(v, own, rec_pc_calls(), PLAYER, SLOT, 5.0);

    ck_eq((uint32_t)g_pc.deliver_arrivals_n, 1u,
          "pipeline: stage 1 (production_complete) invoked its deliver_arrivals hook exactly once");
    ck_eq((uint32_t)(uint16_t)s.status, (uint32_t)(uint16_t)STATUS_ARRIVED_READY_TO_SPAWN,
          "pipeline: stage 1 left status=0xc9");
    ck_eq((uint32_t)(uint16_t)s.origin_planet, (uint32_t)(uint16_t)PLANET,
          "pipeline: stage 1 left origin_planet=dest_planet=PLANET, matching planet_index");

    // Stage 2: the REAL prod_deliver_arrivals, over the SAME fixture state stage 1 just wrote.
    detail::prod_deliver_arrivals(v, own, rec_da_calls());

    ck_eq((uint32_t)g_da.locate_active_port_n, 1u,
          "pipeline: stage 2 (deliver_arrivals) accepted stage 1's state -- its arrival gate opened "
          "and it dispatched into the cargo-heli branch, exactly the state-handoff contract clause 4 "
          "wanted observed");
}

} // namespace

void run_prod_completion_pipeline_tests() {
    test_pc_transit_not_finished_decrements_only();
    test_pc_finished_bumps_throttle_and_always_calls_deliver_arrivals();
    test_pc_throttle_bump_caps_at_five();
    test_pc_local_player_off_planet_prints_message_no_pan();
    test_pc_local_player_on_planet_cargo_heli_pans_via_locate_port();
    test_pc_local_player_on_planet_mothership_pans_via_find_mothership();
    test_pc_locator_failure_sets_col_only_row_untouched();
    test_pc_throttle_10_non_mother_quiet_release();
    test_pc_throttle_10_heli_mother_falls_through_to_normal_path();
    test_pc_dirty_high_bits_masked_regression();

    test_da_no_arrivals_gate_closed_zero_calls();
    test_da_slot_zero_never_visited();
    test_da_cargo_heli_success_full_chain();
    test_da_cargo_heli_locate_fails_no_bind_no_spawn();
    test_da_cargo_heli_bind_fails_no_spawn();
    test_da_mothership_success_locator_ok();
    test_da_mothership_locator_fails_falls_back_to_landing_site();
    test_da_ctrl_group_autoselect_all_gates_pass();
    test_da_ctrl_group_autoselect_wrong_player_gate_fails();
    test_da_neither_type_no_action();

    test_sa_gate_fails_wrong_status_returns_zero();
    test_sa_gate_fails_wrong_planet_returns_zero();
    test_sa_gate_fails_zero_type_returns_zero();
    test_sa_create_fails_fallback_fx_and_sound();
    test_sa_create_fails_sim_inactive_skips_sound_not_fx();
    test_sa_create_succeeds_sets_unit_fields_and_status();
    test_sa_mother_type_elects_primary_when_none_set();
    test_sa_mother_type_does_not_reelect_when_bldg_already_primary();
    test_sa_non_mother_type_no_primary_mother_touch();
    test_sa_storage_idx_zero_no_orientation_no_exit_order();
    test_sa_storage_idx_nonzero_orients_and_issues_exit_order();
    test_sa_slot_high_bits_masked_regression();
    test_sa_is_ship_literal_is_2_not_1();

    test_pipeline_handoff_state();
}

} // namespace mh::sim::test
