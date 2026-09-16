//
// sim_prod_shuttle_unload_selftest.cpp -- `simtest` cases for the SIM1D shuttle-unload SCC ring:
//   llm_prod_shuttle_unload_resource   @0x0048e4fb  (sim/sim_prod_shuttle_unload_resource.cpp)
//   llm_prod_shuttle_unload_passengers @0x0048e6f2  (sim/sim_prod_shuttle_unload_passengers.cpp)
//   llm_prod_shuttle_bay_unload_all    @0x0048f7a6  (sim/sim_prod_shuttle_bay_unload_all.cpp)
//   llm_strat_bldg_flush_cargo_hold    @0x0048e046  (sim/sim_bldg_flush_cargo_hold.cpp)
//
// WHY THIS FILE IS THE ORACLE FOR THIS SLICE. These four are the ONE non-trivial cycle left in the
// sim closure after SIM-CUT: flush_cargo_hold -> bay_unload_all -> {unload_resource,
// unload_passengers} -> flush_cargo_hold. A per-function SHADOW site cannot verify a ring member --
// arming one makes the original arm re-enter the same site recursively through the cycle (a
// snapshot/restore artifact). Because each
// detail() function takes its callee table as a parameter and calls EVERY sibling through it (Law 4,
// never through a mh::sim:: wrapper), this offline oracle mocks the siblings with recording stubs and
// verifies each ring member in isolation: its recorded call SEQUENCE and its own direct state
// writes. This is what closes SIM1D's done_when clause 2 ("the 4-function shuttle ring is covered by
// a simtest case exercising all four entry points").
//
// EVERY EXPECTED VALUE IS DERIVED FROM THE DISASSEMBLY (tmp/decomp/*_0048*.asm), not from the C++
// translations under test. The register-order / offset facts each case rests on are cited inline.
//
#include "sim/sim_bldg_flush_cargo_hold.h"
#include "sim/sim_prod_shuttle_bay_unload_all.h"
#include "sim/sim_prod_shuttle_unload_passengers.h"
#include "sim/sim_prod_shuttle_unload_resource.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-symmetric so a swapped arg lands in a different record and a wrong slot is caught.
// PLAYER (3) deliberately != the fixture's default player_side (7), so flush's UI-event gate is OFF
// by default; the "local player" case uses PLAYER_LOCAL (7) explicitly.
constexpr uint16_t PLAYER       = 3;
constexpr uint16_t PLAYER_LOCAL = 7; // == sim_fixture::reset()'s player_side
constexpr int32_t  BIDX         = 5;
constexpr int32_t  SLOT         = 4;

// game::e::event member 7 -- the event flush_cargo_hold fires for the local player (matches the TU's
// BLDG_FLUSH_CARGO_HOLD_BUILD_PROJECTS_REFRESH and sim_prod_unload_cargo_unit.cpp's own use).
constexpr uint32_t BUILD_PROJECTS_REFRESH = 7;

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)slot];
}

// Seed a cargo-manifest entry's leading int16 (the "occupied" flag bay_unload_all tests). Stride 0xe
// per sim_prod_unload_cargo_unit.cpp's kCargoEntryStride; the flag is the entry's first 2 bytes.
void put_cargo(sim_fixture &f, uint32_t player, int32_t slot, int32_t cargo_index, int16_t flag) {
    std::memcpy(&slot_of(f, player, slot).cargo_manifest_raw[(uint32_t)cargo_index * 0xe], &flag,
                sizeof(flag));
}

// ---- recorders ----------------------------------------------------------------------------------
// Captureless lambdas convert to the plain function pointers the *_calls structs hold, so every stub
// records into ONE file-scope recorder (same shape as sim_unit_refund_selftest.cpp's g_log).
struct ev3 {
    int32_t a, b, c;
};
struct ev2 {
    int32_t a, b;
};

struct recorder {
    std::vector<ev3>     resource_add; // (player, resource_id, amount)
    std::vector<ev2>     pop_add;      // (player, count)
    std::vector<ev2>     is_free;      // (player, building_index)
    std::vector<ev2>     flush;        // (player, building_index)
    std::vector<ev3>     unload_res;   // (player, building_index, resource_id) -- cap is always -1
    std::vector<ev2>     unload_pass;  // (player, building_index) -- cap is always -1
    std::vector<ev3>     unload_cargo; // (player, building_index, cargo_index)
    std::vector<ev2>     slot_release; // (player, slot)
    std::vector<ev2>     bay;          // (player, building_index)
    std::vector<int32_t> set_event;    // (type)

    // Control knob: what the mocked llm_strat_bldg_shuttle_slot_is_free returns (the guard on the
    // conditional flush in both unload_* functions).
    int32_t slot_free_ret = 0;

    void reset() { *this = recorder{}; }
};
recorder g_rec;

const prod_shuttle_unload_resource_calls &rec_unload_resource_calls() {
    static const prod_shuttle_unload_resource_calls c = {
        [](int32_t p, int32_t rid, int32_t amt) { g_rec.resource_add.push_back({p, rid, amt}); },
        [](int32_t p, int32_t b) -> int32_t {
            g_rec.is_free.push_back({p, b});
            return g_rec.slot_free_ret;
        },
        [](uint32_t p, int32_t b) { g_rec.flush.push_back({(int32_t)p, b}); },
    };
    return c;
}

const prod_shuttle_unload_passengers_calls &rec_unload_passengers_calls() {
    static const prod_shuttle_unload_passengers_calls c = {
        [](uint16_t p, int32_t n) { g_rec.pop_add.push_back({(int32_t)p, n}); },
        [](int32_t p, int32_t b) -> int32_t {
            g_rec.is_free.push_back({p, b});
            return g_rec.slot_free_ret;
        },
        [](uint32_t p, int32_t b) { g_rec.flush.push_back({(int32_t)p, b}); },
    };
    return c;
}

const prod_shuttle_bay_unload_all_calls &rec_bay_calls() {
    static const prod_shuttle_bay_unload_all_calls c = {
        [](uint16_t p, int32_t b, uint16_t rid, int32_t /*cap*/) -> uint8_t {
            g_rec.unload_res.push_back({(int32_t)p, b, (int32_t)rid});
            return 0;
        },
        [](uint16_t p, int32_t b, int32_t /*cap*/) -> int32_t {
            g_rec.unload_pass.push_back({(int32_t)p, b});
            return 0;
        },
        [](uint16_t p, int32_t b, uint32_t ci) -> uint32_t {
            g_rec.unload_cargo.push_back({(int32_t)p, b, (int32_t)ci});
            return 0;
        },
    };
    return c;
}

const bldg_flush_cargo_hold_calls &rec_flush_calls() {
    static const bldg_flush_cargo_hold_calls c = {
        [](uint16_t p, int32_t b) { g_rec.bay.push_back({(int32_t)p, b}); },
        [](int32_t p, int32_t slot) { g_rec.slot_release.push_back({p, slot}); },
        [](uint32_t type) -> uint32_t {
            g_rec.set_event.push_back((int32_t)type);
            return 0;
        },
    };
    return c;
}

// ---- llm_prod_shuttle_unload_resource -----------------------------------------------------------
// From the .asm: amount = resources_reserved[rid] (0x0048e544, MOV), clamped to cap only when
// cap != -1 AND cap < amount (JZ cap==-1 / signed JL at 0x0048e55e-0x0048e574). amount==0 bails
// returning 0 (0x0048e574). Otherwise llm_resource_add(player, rid, amount) [EAX,EDX,EBX], then
// resources_reserved[rid] -= amount, then if bldg_shuttle_slot_is_free(player, bidx) > 0 ->
// bldg_flush_cargo_hold(player, bidx); return 1. resource_add's amount is CREDITED BACK (unload).
void test_unload_resource_full_credit_no_flush() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot                 = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).resources_reserved[6] = 25;
    g_rec.slot_free_ret                            = 0; // slot still occupied -> no flush

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const uint8_t r =
        detail::prod_shuttle_unload_resource(v, own, rec_unload_resource_calls(), PLAYER, BIDX, 6, -1);

    ck_eq(r, 1u, "unload_resource: cap=-1, reserved 25 -> returns 1 (credited nonzero)");
    ck_eq((uint32_t)g_rec.resource_add.size(), 1u, "unload_resource: exactly one llm_resource_add");
    ck(g_rec.resource_add.size() == 1 && g_rec.resource_add[0].a == PLAYER &&
           g_rec.resource_add[0].b == 6 && g_rec.resource_add[0].c == 25,
       "unload_resource: resource_add(player=3, id=6, amount=25) -- the full reserved amount credited back");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).resources_reserved[6], 0u,
          "unload_resource: resources_reserved[6] decremented 25 -> 0");
    ck_eq((uint32_t)g_rec.is_free.size(), 1u, "unload_resource: shuttle_slot_is_free queried once");
    ck_eq((uint32_t)g_rec.flush.size(), 0u, "unload_resource: slot not free -> no cargo-hold flush");
}

void test_unload_resource_full_credit_then_flush() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot                 = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).resources_reserved[6] = 25;
    g_rec.slot_free_ret                            = 9; // slot now free -> flush must fire

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const uint8_t r =
        detail::prod_shuttle_unload_resource(v, own, rec_unload_resource_calls(), PLAYER, BIDX, 6, -1);

    ck_eq(r, 1u, "unload_resource: credited -> returns 1");
    ck_eq((uint32_t)g_rec.flush.size(), 1u, "unload_resource: slot freed -> exactly one flush");
    ck(g_rec.flush.size() == 1 && g_rec.flush[0].a == PLAYER && g_rec.flush[0].b == BIDX,
       "unload_resource: flush_cargo_hold(player=3, building_index=5)");
}

void test_unload_resource_cap_clamps_partial() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot                 = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).resources_reserved[6] = 25;
    g_rec.slot_free_ret                            = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const uint8_t r =
        detail::prod_shuttle_unload_resource(v, own, rec_unload_resource_calls(), PLAYER, BIDX, 6, 10);

    ck_eq(r, 1u, "unload_resource: cap 10 < reserved 25 -> returns 1");
    ck(g_rec.resource_add.size() == 1 && g_rec.resource_add[0].c == 10,
       "unload_resource: cap 10 clamps amount to 10 (min(cap, reserved))");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).resources_reserved[6], 15u,
          "unload_resource: reserved 25 - 10 = 15 (partial unload)");
}

void test_unload_resource_cap_ge_reserved_keeps_reserved() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot                 = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).resources_reserved[6] = 25;
    g_rec.slot_free_ret                            = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const uint8_t r =
        detail::prod_shuttle_unload_resource(v, own, rec_unload_resource_calls(), PLAYER, BIDX, 6, 100);

    ck(g_rec.resource_add.size() == 1 && g_rec.resource_add[0].c == 25,
       "unload_resource: cap 100 >= reserved 25 -> amount stays the full 25 (clamp is min, not assign)");
    ck_eq(r, 1u, "unload_resource: returns 1");
}

void test_unload_resource_nothing_reserved_returns_zero() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot                 = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).resources_reserved[6] = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const uint8_t r =
        detail::prod_shuttle_unload_resource(v, own, rec_unload_resource_calls(), PLAYER, BIDX, 6, -1);

    ck_eq(r, 0u, "unload_resource: reserved 0 -> returns 0, no work");
    ck_eq((uint32_t)g_rec.resource_add.size(), 0u, "unload_resource: no resource_add when nothing reserved");
    ck_eq((uint32_t)g_rec.is_free.size(), 0u, "unload_resource: no slot query on the zero path");
}

void test_unload_resource_cap_zero_is_noop() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot                 = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).resources_reserved[6] = 25;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const uint8_t r =
        detail::prod_shuttle_unload_resource(v, own, rec_unload_resource_calls(), PLAYER, BIDX, 6, 0);

    ck_eq(r, 0u, "unload_resource: cap 0 (not -1) clamps amount to 0 -> returns 0");
    ck_eq((uint32_t)g_rec.resource_add.size(), 0u, "unload_resource: cap 0 -> no resource_add");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).resources_reserved[6], 25u,
          "unload_resource: cap 0 leaves reserved untouched at 25");
}

// ---- llm_prod_shuttle_unload_passengers ---------------------------------------------------------
// Mirror of unload_resource with llm_strat_population_add(player, amount) [EAX=player, EDX=amount]
// instead of resource_add, over passengers_reserved. Returns int 0/1. cap is NOT truncated here
// (both sides of the compare are full 32-bit, unlike the load-passengers sibling).
void test_unload_passengers_release_all_then_flush() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot               = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 30;
    g_rec.slot_free_ret                          = 4; // slot free -> flush fires

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_shuttle_unload_passengers(v, own, rec_unload_passengers_calls(), PLAYER, BIDX, -1);

    ck_eq((uint32_t)r, 1u, "unload_passengers: cap=-1, reserved 30 -> returns 1");
    ck(g_rec.pop_add.size() == 1 && g_rec.pop_add[0].a == PLAYER && g_rec.pop_add[0].b == 30,
       "unload_passengers: population_add(player=3, count=30) -- all reserved released");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 0u,
          "unload_passengers: passengers_reserved 30 -> 0");
    ck(g_rec.flush.size() == 1 && g_rec.flush[0].a == PLAYER && g_rec.flush[0].b == BIDX,
       "unload_passengers: slot freed -> flush_cargo_hold(player=3, building_index=5)");
}

void test_unload_passengers_no_flush_when_slot_busy() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot               = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 30;
    g_rec.slot_free_ret                          = 0; // slot still busy

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_shuttle_unload_passengers(v, own, rec_unload_passengers_calls(), PLAYER, BIDX, -1);

    ck_eq((uint32_t)g_rec.is_free.size(), 1u, "unload_passengers: slot query happens once");
    ck_eq((uint32_t)g_rec.flush.size(), 0u, "unload_passengers: slot busy -> no flush");
}

void test_unload_passengers_cap_clamps_partial() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot               = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 30;
    g_rec.slot_free_ret                          = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_shuttle_unload_passengers(v, own, rec_unload_passengers_calls(), PLAYER, BIDX, 12);

    ck_eq((uint32_t)r, 1u, "unload_passengers: cap 12 < reserved 30 -> returns 1");
    ck(g_rec.pop_add.size() == 1 && g_rec.pop_add[0].b == 12,
       "unload_passengers: cap 12 clamps release to 12");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT).passengers_reserved, 18u,
          "unload_passengers: reserved 30 - 12 = 18");
}

void test_unload_passengers_nothing_reserved_returns_zero() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot               = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_shuttle_unload_passengers(v, own, rec_unload_passengers_calls(), PLAYER, BIDX, -1);

    ck_eq((uint32_t)r, 0u, "unload_passengers: reserved 0 -> returns 0");
    ck_eq((uint32_t)g_rec.pop_add.size(), 0u, "unload_passengers: no population_add when nothing reserved");
    ck_eq((uint32_t)g_rec.is_free.size(), 0u, "unload_passengers: no slot query on the zero path");
}

// ---- llm_prod_shuttle_bay_unload_all ------------------------------------------------------------
// Reads shuttle_slot once; loops resource_id 1..9 calling unload_resource(player, bidx, rid, -1);
// then, iff passengers_reserved != 0, calls unload_passengers(player, bidx, -1); then walks cargo
// entries 0..49 (0x32) and, for each whose leading int16 is nonzero, calls unload_cargo_unit(player,
// bidx, cargo_index). This is a CONTROL-FLOW oracle -- every callee is a recording stub.
void test_bay_unload_empty_hold_is_nine_resources_only() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot               = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 0; // no passengers, no cargo (all zeroed)

    sim_view v = f.view();
    detail::prod_shuttle_bay_unload_all(v, rec_bay_calls(), PLAYER, BIDX);

    ck_eq((uint32_t)g_rec.unload_res.size(), 9u,
          "bay_unload_all: exactly nine resource unloads (resource_id 1..9)");
    bool order_ok = true;
    for (int32_t i = 0; i < 9 && (size_t)i < g_rec.unload_res.size(); ++i) {
        if (g_rec.unload_res[(size_t)i].a != PLAYER || g_rec.unload_res[(size_t)i].b != BIDX ||
            g_rec.unload_res[(size_t)i].c != i + 1)
            order_ok = false;
    }
    ck(order_ok, "bay_unload_all: resource unloads are (player=3, bidx=5, rid=1..9) in ascending order");
    ck_eq((uint32_t)g_rec.unload_pass.size(), 0u,
          "bay_unload_all: passengers_reserved 0 -> no passenger unload");
    ck_eq((uint32_t)g_rec.unload_cargo.size(), 0u, "bay_unload_all: empty manifest -> no cargo unloads");
}

void test_bay_unload_full_sequence() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot               = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).passengers_reserved = 5; // nonzero -> passengers unload fires
    put_cargo(f, PLAYER, SLOT, 0, 0x1234);            // occupied
    put_cargo(f, PLAYER, SLOT, 1, 0);                 // empty -> skipped
    put_cargo(f, PLAYER, SLOT, 3, 0x5678);            // occupied
    put_cargo(f, PLAYER, SLOT, 49, 0x0009);           // occupied, LAST valid entry (index 49)

    sim_view v = f.view();
    detail::prod_shuttle_bay_unload_all(v, rec_bay_calls(), PLAYER, BIDX);

    ck_eq((uint32_t)g_rec.unload_res.size(), 9u, "bay_unload_all: nine resource unloads regardless of cargo");
    ck(g_rec.unload_pass.size() == 1 && g_rec.unload_pass[0].a == PLAYER &&
           g_rec.unload_pass[0].b == BIDX,
       "bay_unload_all: passengers_reserved 5 -> one unload_passengers(player=3, bidx=5)");
    ck_eq((uint32_t)g_rec.unload_cargo.size(), 3u,
          "bay_unload_all: three occupied manifest entries -> three cargo unloads (0,3,49); empties skipped");
    ck(g_rec.unload_cargo.size() == 3 && g_rec.unload_cargo[0].c == 0 &&
           g_rec.unload_cargo[1].c == 3 && g_rec.unload_cargo[2].c == 49,
       "bay_unload_all: cargo unloads are indices 0,3,49 in ascending order (stride 0xe, leading-word flag)");
}

// ---- llm_strat_bldg_flush_cargo_hold ------------------------------------------------------------
// Calls bay_unload_all(player, bidx); reads slot = buildings[player][bidx].shuttle_slot; calls
// prod_shuttle_slot_release(player, slot); clears the slot record's type_ref_id/src_building_type/
// src_building_index; clears buildings[player][bidx].shuttle_slot LAST; and iff player == PlayerSide,
// game_SetEvent(BUILD_PROJECTS_REFRESH=7). The slot value passed to release and used for the field
// clears is the PRE-CLEAR shuttle_slot -- the ordering that the test pins.
void test_flush_non_local_player_no_event() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BIDX).shuttle_slot              = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).type_ref_id        = 0x0011;
    slot_of(f, PLAYER, SLOT).src_building_type  = 0x0022;
    slot_of(f, PLAYER, SLOT).src_building_index = 0x0033;
    // A neighbouring slot record seeded nonzero: it must be left untouched (proves the clear targets
    // exactly player*10 + SLOT).
    slot_of(f, PLAYER, SLOT + 1).type_ref_id = 0xAAAA;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_flush_cargo_hold(v, own, rec_flush_calls(), PLAYER, BIDX);

    ck(g_rec.bay.size() == 1 && g_rec.bay[0].a == PLAYER && g_rec.bay[0].b == BIDX,
       "flush: bay_unload_all(player=3, bidx=5) called first");
    ck(g_rec.slot_release.size() == 1 && g_rec.slot_release[0].a == PLAYER &&
           g_rec.slot_release[0].b == SLOT,
       "flush: prod_shuttle_slot_release(player=3, slot=4) -- the PRE-CLEAR shuttle_slot value");
    ck(slot_of(f, PLAYER, SLOT).type_ref_id == 0 && slot_of(f, PLAYER, SLOT).src_building_type == 0 &&
           slot_of(f, PLAYER, SLOT).src_building_index == 0,
       "flush: the slot record's three leading fields are cleared to 0");
    ck_eq((uint32_t)f.b(PLAYER, BIDX).shuttle_slot, 0u,
          "flush: buildings[player][bidx].shuttle_slot cleared to 0 (LAST, after every read)");
    ck_eq((uint32_t)slot_of(f, PLAYER, SLOT + 1).type_ref_id, 0xAAAAu,
          "flush: the adjacent slot record is untouched (clear targets exactly player*10+slot)");
    ck_eq((uint32_t)g_rec.set_event.size(), 0u,
          "flush: player 3 != PlayerSide 7 -> no BUILD_PROJECTS_REFRESH event");
}

void test_flush_local_player_fires_event() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER_LOCAL, BIDX).shuttle_slot              = (uint8_t)SLOT;
    slot_of(f, PLAYER_LOCAL, SLOT).type_ref_id        = 0x0044;
    slot_of(f, PLAYER_LOCAL, SLOT).src_building_type  = 0x0055;
    slot_of(f, PLAYER_LOCAL, SLOT).src_building_index = 0x0066;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_flush_cargo_hold(v, own, rec_flush_calls(), PLAYER_LOCAL, BIDX);

    ck(g_rec.slot_release.size() == 1 && g_rec.slot_release[0].a == PLAYER_LOCAL &&
           g_rec.slot_release[0].b == SLOT,
       "flush(local): prod_shuttle_slot_release(player=7, slot=4)");
    ck(g_rec.set_event.size() == 1 && g_rec.set_event[0] == (int32_t)BUILD_PROJECTS_REFRESH,
       "flush(local): player 7 == PlayerSide -> game_SetEvent(BUILD_PROJECTS_REFRESH=7)");
    ck(slot_of(f, PLAYER_LOCAL, SLOT).type_ref_id == 0 &&
           slot_of(f, PLAYER_LOCAL, SLOT).src_building_index == 0,
       "flush(local): slot fields still cleared regardless of the local-player branch");
    ck_eq((uint32_t)f.b(PLAYER_LOCAL, BIDX).shuttle_slot, 0u, "flush(local): building shuttle_slot cleared");
}

// REGRESSION for the reimpl-verify divergence (2026-08-14): flush_cargo_hold takes player as a
// uint32_t, and the original re-narrows it to the low 16 bits (MOVZX word) at every record-address
// computation and outward call. A caller with garbage in bits 16-31 must behave EXACTLY as the clean
// low-16 value -- the pre-fix draft used the raw uint32_t and would index buildings[0x10003*100+...]
// out of bounds. This case passes only with the entry-point truncation in place.
void test_flush_player_high_bits_truncated_regression() {
    sim_fixture f;
    g_rec.reset();
    const uint32_t player_dirty                 = 0x00010000u | PLAYER; // low16 == 3, garbage above
    f.b(PLAYER, BIDX).shuttle_slot              = (uint8_t)SLOT;
    slot_of(f, PLAYER, SLOT).type_ref_id        = 0x0077;
    slot_of(f, PLAYER, SLOT).src_building_index = 0x0088;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_flush_cargo_hold(v, own, rec_flush_calls(), player_dirty, BIDX);

    ck(g_rec.slot_release.size() == 1 && g_rec.slot_release[0].a == PLAYER &&
           g_rec.slot_release[0].b == SLOT,
       "flush(dirty player): upper bits masked -> prod_shuttle_slot_release(player=3, slot=4)");
    ck(slot_of(f, PLAYER, SLOT).type_ref_id == 0 && slot_of(f, PLAYER, SLOT).src_building_index == 0,
       "flush(dirty player): the low-16 slot record is the one cleared (no OOB index)");
    ck_eq((uint32_t)f.b(PLAYER, BIDX).shuttle_slot, 0u,
          "flush(dirty player): buildings[3][5].shuttle_slot cleared");
    ck_eq((uint32_t)g_rec.set_event.size(), 0u,
          "flush(dirty player): low16=3 != PlayerSide 7 -> no event (the CMP AX narrows too)");
}

} // namespace

void run_prod_shuttle_unload_tests() {
    test_unload_resource_full_credit_no_flush();
    test_unload_resource_full_credit_then_flush();
    test_unload_resource_cap_clamps_partial();
    test_unload_resource_cap_ge_reserved_keeps_reserved();
    test_unload_resource_nothing_reserved_returns_zero();
    test_unload_resource_cap_zero_is_noop();

    test_unload_passengers_release_all_then_flush();
    test_unload_passengers_no_flush_when_slot_busy();
    test_unload_passengers_cap_clamps_partial();
    test_unload_passengers_nothing_reserved_returns_zero();

    test_bay_unload_empty_hold_is_nine_resources_only();
    test_bay_unload_full_sequence();

    test_flush_non_local_player_no_event();
    test_flush_local_player_fires_event();
    test_flush_player_high_bits_truncated_regression();
}

} // namespace mh::sim::test
