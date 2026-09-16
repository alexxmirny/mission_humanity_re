//
// sim_bldg_power_network_selftest.cpp -- `simtest` cases for the building POWER-connectivity network:
// llm_strat_bldg_propagate_network_connectivity (the recursive flood-fill core,
// sim/sim_bldg_propagate_network_connectivity.h/.cpp) and its driver
// llm_strat_bldg_power_network_recompute (sim/sim_bldg_power_network_recompute.h/.cpp), both SIM1B.
//
// WHY THIS FILE EXISTS (SIM1B done_when, clause 2): "the power-network recompute
// is covered by a simtest case over a HAND-BUILT adjacency fixture including a disconnected component
// and a newly-linked one (the flood fill is the piece most likely to be subtly wrong and least
// likely to be exercised by a default scenario)." propagate_network_connectivity is ALSO the one
// batch-B function that is structurally UN-shadowable per call (self-
// recursion re-enters its own shadow hook at every depth), so an offline oracle is not merely a
// nicety here, it is the only per-call oracle available. power_network_recompute is a pure delegator
// whose entire job is SELECTING which slots feed which of its four callees; this file drives that
// selection over recording stubs.
//
// THE RECORDING-STUB CONTRACT. Both functions are indirected through a plain-function-pointer calls
// struct (propagate: {set_connected_flag}; recompute: {mother_reelect_primary, clear_flag_bit0_notify,
// propagate_network_connectivity, set_connected_flag}) precisely so an offline test can observe the
// effect set without a live game. The flood-fill stub for set_connected_flag does TWO things, both
// faithful to the real llm_bldg_set_connected_flag @0x004965d6 (read from its own translation): it
// records the (player, slot) marked, AND it sets buildings[player][slot].built_flags bit0. That bit0
// write is not bookkeeping -- it is the flood's VISITED terminator: propagate only recurses into a
// neighbour whose built_flags bit0 is CLEAR, so a stub that merely recorded (and never set the bit)
// would recurse forever around any tile cycle. Setting it reproduces exactly the real callee's role.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_propagate_network_connectivity_0049209b.asm and
//  tmp/decomp/llm_strat_bldg_power_network_recompute_00491c76.asm), cross-checked against the two
// translations' header banners -- NOT read off the .cpp bodies alone.
//
#include "sim/sim_bldg_power_network_recompute.h"
#include "sim/sim_bldg_propagate_network_connectivity.h"

#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_* / ORDER_KIND_BUILDING
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// llm_strat_bldg_state members recompute pass (3) tests (see sim_bldg_power_network_recompute.cpp's
// own anonymous-namespace copy -- reproduced here, only the two members this test needs).
constexpr uint16_t BLDG_STATE_CONSTRUCTION = 0x64;
constexpr uint16_t BLDG_STATE_DISMANTLING  = 0x6b;

// ---- Part A: propagate flood-fill recorder -----------------------------------------------------
// A file-static handle so the set_connected_flag stub can set built_flags bit0 on the fixture's own
// storage (the real callee's visited-terminator side effect -- see the banner).
sim_fixture         *g_fx = nullptr;
std::vector<int32_t> g_marked; // slots marked connected, IN ORDER

void rec_set_connected_flood(uint16_t player, int32_t b_index) {
    g_marked.push_back(b_index);
    g_fx->b(player, b_index).built_flags = (uint8_t)(g_fx->b(player, b_index).built_flags | 0x1u);
}

const propagate_network_connectivity_calls g_flood_calls = {&rec_set_connected_flood};

bool marked_contains(int32_t slot) {
    for (int32_t s : g_marked)
        if (s == slot) return true;
    return false;
}

// Seed one building slot as an online, non-turret conduit. width/height 0 so the 31x31 window is
// centred exactly on (x,y) and the arithmetic is trivial to hand-check.
void seed_conduit(sim_fixture &fx, int32_t slot, int32_t cfg_row, uint8_t type, int32_t x, int32_t y,
                  double energy) {
    building &b     = fx.b(0, slot);
    b.building_id   = (uint16_t)cfg_row;
    b.x             = (uint8_t)x;
    b.y             = (uint8_t)y;
    b.online_state  = 1;
    b.energy        = energy;
    b.built_flags   = 0;
    cfg_building &c = fx.cfg_buildings[cfg_row];
    c.type          = type;
    c.width         = 0;
    c.height        = 0;
}

// Tag a tile as a same-owner (player 0) BUILDING-class tile pointing at roster slot `slot`.
void link_tile(sim_fixture &fx, int32_t tx, int32_t ty, int32_t slot) {
    fx.t(tx, ty).class_owner = (uint8_t)(0u | ORDER_KIND_BUILDING);
    fx.t(tx, ty).building    = (uint16_t)slot;
}

// ---- Part B: driver recorder -------------------------------------------------------------------
struct DrvCall {
    int32_t player;
    int32_t a; // slot, or landing_x for reelect
    int32_t b; // landing_y for reelect, else unused
};
std::vector<DrvCall> g_reelect, g_clear, g_prop, g_setconn;

int32_t rec_reelect(int32_t player, int32_t x, int32_t y) {
    g_reelect.push_back({player, x, y});
    return 0; // the original discards this
}
void rec_clear(uint16_t p, int32_t i) { g_clear.push_back({p, i, 0}); }
void rec_prop(uint16_t p, int32_t i) { g_prop.push_back({p, i, 0}); }
void rec_setconn_drv(uint16_t p, int32_t i) { g_setconn.push_back({p, i, 0}); }

const power_network_recompute_calls g_drv_calls = {&rec_reelect, &rec_clear, &rec_prop,
                                                   &rec_setconn_drv};

void drv_reset() {
    g_reelect.clear();
    g_clear.clear();
    g_prop.clear();
    g_setconn.clear();
}

// An occupied building slot for the driver's `.index`-occupancy passes (1) and (3). `index != 0`
// marks occupancy; the driver never dereferences x/y here.
void seed_occupied(sim_fixture &fx, int32_t slot, int32_t cfg_row, uint8_t type, uint16_t state) {
    building &b                    = fx.b(0, slot);
    b.index                        = 1;
    b.building_id                  = (uint16_t)cfg_row;
    b.state                        = state;
    fx.cfg_buildings[cfg_row].type = type;
}

} // namespace

void run_bldg_power_network_tests() {
    sim_fixture fx;
    g_fx = &fx;

    // ================================================================================================
    // PART A -- propagate_network_connectivity: the recursive flood fill (done_when clause 2)
    // ================================================================================================

    // ---- A1: a 2-HOP chain 1->2->3 plus a DISCONNECTED component (slot 5). This is the clause-2
    // fixture: slot 3 is reachable ONLY through slot 2's window (its linking tile at (68,68) is OUTSIDE
    // slot 1's 35..65 window but INSIDE slot 2's 40..70 window), so it is only ever marked once slot 2
    // has itself been marked and its own window scanned -- the "newly-linked" case. Slot 5 has no
    // linking tile anywhere and must stay unmarked -- the "disconnected component" case. ------------
    fx.reset();
    fx.geom.width_mask  = 0xff; // 256-wide, matches fx.t()'s (tx<<8)|ty indexing exactly
    fx.geom.height_mask = 0xff;
    seed_conduit(fx, 1, 10, BUILDING_TYPE_A_PLANT, 50, 50, 10.0);   // root
    seed_conduit(fx, 2, 11, BUILDING_TYPE_A_PLANT, 55, 55, 10.0);   // 1 hop
    seed_conduit(fx, 3, 12, BUILDING_TYPE_A_PLANT, 68, 68, 10.0);   // 2 hops (only via slot 2)
    seed_conduit(fx, 5, 13, BUILDING_TYPE_A_PLANT, 200, 200, 10.0); // disconnected
    link_tile(fx, 55, 55, 2);                                       // in slot 1's window (35..65)
    link_tile(fx, 68, 68, 3);                                       // in slot 2's window (40..70) only

    g_marked.clear();
    detail::propagate_network_connectivity(fx.view(), g_flood_calls, 0, 1);
    ck_eq((uint32_t)g_marked.size(), 3, "propagate A1: exactly 3 buildings marked (chain 1->2->3)");
    ck_eq((uint32_t)g_marked[0], 1, "propagate A1: root (slot 1) marked FIRST, at entry");
    ck(marked_contains(2), "propagate A1: slot 2 marked (1 hop from root's window)");
    ck(marked_contains(3), "propagate A1: slot 3 marked (2 hops -- only reachable via slot 2)");
    ck(!marked_contains(5), "propagate A1: DISCONNECTED slot 5 never marked (no linking tile)");
    // Order: slot 2 must be marked before slot 3 (3 is discovered inside 2's recursion).
    {
        int32_t i2 = -1, i3 = -1;
        for (int32_t i = 0; i < (int32_t)g_marked.size(); ++i) {
            if (g_marked[i] == 2) i2 = i;
            if (g_marked[i] == 3) i3 = i;
        }
        ck(i2 >= 0 && i3 > i2, "propagate A1: slot 2 marked before slot 3 (fixpoint ordering)");
    }

    // ---- A2: the root is a TURRET -- a power SINK, not a conduit. It marks ITSELF connected at entry
    // but returns before scanning its window, so the reachable chain is NOT flooded. --------------
    fx.reset();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    seed_conduit(fx, 1, 10, BUILDING_TYPE_A_TURRET, 50, 50, 10.0); // turret root
    seed_conduit(fx, 2, 11, BUILDING_TYPE_A_PLANT, 55, 55, 10.0);
    link_tile(fx, 55, 55, 2);

    g_marked.clear();
    detail::propagate_network_connectivity(fx.view(), g_flood_calls, 0, 1);
    ck_eq((uint32_t)g_marked.size(), 1, "propagate A2: turret marks only itself, does not conduit");
    ck(marked_contains(1) && !marked_contains(2), "propagate A2: neighbour slot 2 NOT flooded");

    // ---- A3: the root is OFFLINE (online_state == 0). Same as A2: self-marked, no window scan. ----
    fx.reset();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    seed_conduit(fx, 1, 10, BUILDING_TYPE_A_PLANT, 50, 50, 10.0);
    fx.b(0, 1).online_state = 0; // offline
    seed_conduit(fx, 2, 11, BUILDING_TYPE_A_PLANT, 55, 55, 10.0);
    link_tile(fx, 55, 55, 2);

    g_marked.clear();
    detail::propagate_network_connectivity(fx.view(), g_flood_calls, 0, 1);
    ck_eq((uint32_t)g_marked.size(), 1, "propagate A3: offline root marks only itself");
    ck(!marked_contains(2), "propagate A3: offline root does not flood its neighbour");

    // ---- A4: a neighbour that is ALREADY connected (built_flags bit0 SET) is skipped -- the visited
    // guard. Slot 2 starts with bit0 set, so the chain stops at the root. -------------------------
    fx.reset();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    seed_conduit(fx, 1, 10, BUILDING_TYPE_A_PLANT, 50, 50, 10.0);
    seed_conduit(fx, 2, 11, BUILDING_TYPE_A_PLANT, 55, 55, 10.0);
    fx.b(0, 2).built_flags = 0x1; // already reached
    link_tile(fx, 55, 55, 2);

    g_marked.clear();
    detail::propagate_network_connectivity(fx.view(), g_flood_calls, 0, 1);
    ck_eq((uint32_t)g_marked.size(), 1, "propagate A4: already-connected neighbour (bit0 set) skipped");

    // ---- A5: the energy gate is the NaN-INCLUSIVE `!(energy <= 0.0)`, NOT `0.0 < energy`. A neighbour
    // with energy == 0.0 is skipped; a neighbour with a NaN energy is TREATED AS ALIVE and recursed
    // into (the x87 unordered-compare branch the's reimpl-verify caught as a real bug). --
    fx.reset();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    seed_conduit(fx, 1, 10, BUILDING_TYPE_A_PLANT, 50, 50, 10.0);
    seed_conduit(fx, 2, 11, BUILDING_TYPE_A_PLANT, 55, 55, 0.0); // energy == 0 -> skipped
    link_tile(fx, 55, 55, 2);
    g_marked.clear();
    detail::propagate_network_connectivity(fx.view(), g_flood_calls, 0, 1);
    ck(!marked_contains(2), "propagate A5a: energy == 0.0 neighbour skipped (energy>0 gate)");

    fx.reset();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    seed_conduit(fx, 1, 10, BUILDING_TYPE_A_PLANT, 50, 50, 10.0);
    seed_conduit(fx, 2, 11, BUILDING_TYPE_A_PLANT, 55, 55, 10.0);
    // Build a quiet NaN by bit pattern so no compile-time FP contraction hides it.
    {
        const uint64_t nan_bits = 0x7ff8000000000000ULL;
        double         nan_val;
        static_assert(sizeof(nan_val) == sizeof(nan_bits), "double is 8 bytes");
        memcpy(&nan_val, &nan_bits, sizeof(nan_val));
        fx.b(0, 2).energy = nan_val;
    }
    link_tile(fx, 55, 55, 2);
    g_marked.clear();
    detail::propagate_network_connectivity(fx.view(), g_flood_calls, 0, 1);
    ck(marked_contains(2),
       "propagate A5b: NaN energy neighbour IS recursed into (!(NaN<=0) is true -- NaN-inclusive gate)");

    // ================================================================================================
    // PART B -- power_network_recompute: the driver's per-pass SELECTION logic
    // ================================================================================================

    // ---- B0a: pass (0), primary mother UNSET -> re-elect with (player, landing_x[planet],
    // landing_y[planet]). No occupied slots (index 0) and zero energy-count so passes (1)/(2)/(3) are
    // silent -- this isolates pass (0). --------------------------------------------------------------
    fx.reset();
    fx.planet_index                       = 0;
    fx.profiles[2].primary_mother_bldg[0] = 0; // unset -> re-elect
    fx.profiles[2].landing_x[0]           = 77;
    fx.profiles[2].landing_y[0]           = 88;
    fx.b(0, 0); // (player 2's own slot-0 counts are zero after reset)
    drv_reset();
    detail::power_network_recompute(fx.view(), g_drv_calls, 2);
    ck_eq((uint32_t)g_reelect.size(), 1, "recompute B0a: unset primary mother -> re-elect fired once");
    if (!g_reelect.empty()) {
        ck_eq((uint32_t)g_reelect[0].player, 2, "recompute B0a: re-elect player");
        ck_eq((uint32_t)g_reelect[0].a, 77, "recompute B0a: re-elect landing_x[planet]");
        ck_eq((uint32_t)g_reelect[0].b, 88, "recompute B0a: re-elect landing_y[planet]");
    }
    ck_eq((uint32_t)(g_clear.size() + g_prop.size() + g_setconn.size()), 0,
          "recompute B0a: other passes silent (no occupied slots, zero energy count)");

    // ---- B0b: pass (0), primary mother already SET (!= 0) -> NO re-election. ------------------------
    fx.reset();
    fx.planet_index                       = 0;
    fx.profiles[2].primary_mother_bldg[0] = 5; // already set
    drv_reset();
    detail::power_network_recompute(fx.view(), g_drv_calls, 2);
    ck_eq((uint32_t)g_reelect.size(), 0, "recompute B0b: primary mother already set -> no re-election");

    // ---- B1: pass (1), the clear-flag walk. buildings[player][0].index is the live COUNT; every
    // occupied slot (index != 0) gets clear_flag_bit0_notify. Occupied slots 1,4,7 (interleaved with
    // empty 2,3,5,6) and count 3 -- the empty slots must NOT consume the count. All slots are plain
    // PRODUCTION in a normal state so pass (3) selects none of them, isolating pass (1). Energy count
    // 0 so pass (2) is silent. ----------------------------------------------------------------------
    fx.reset();
    fx.planet_index                       = 0;
    fx.profiles[0].primary_mother_bldg[0] = 9;   // set so pass (0) is quiet
    fx.b(0, 0).index                      = 3;   // live count for passes (1)/(3)
    fx.b(0, 0).energy                     = 0.0; // pass (2) count 0
    seed_occupied(fx, 1, 20, BUILDING_TYPE_A_PRODUCTION, 0);
    seed_occupied(fx, 4, 20, BUILDING_TYPE_A_PRODUCTION, 0);
    seed_occupied(fx, 7, 20, BUILDING_TYPE_A_PRODUCTION, 0);
    drv_reset();
    detail::power_network_recompute(fx.view(), g_drv_calls, 0);
    ck_eq((uint32_t)g_clear.size(), 3, "recompute B1: clear-flag fired on exactly the 3 occupied slots");
    if (g_clear.size() == 3) {
        ck_eq((uint32_t)g_clear[0].a, 1, "recompute B1: clear slot 1");
        ck_eq((uint32_t)g_clear[1].a, 4, "recompute B1: clear slot 4 (empty 2,3 did not consume count)");
        ck_eq((uint32_t)g_clear[2].a, 7, "recompute B1: clear slot 7 (empty 5,6 did not consume count)");
    }
    ck_eq((uint32_t)g_prop.size(), 0, "recompute B1: pass (2) silent");
    ck_eq((uint32_t)g_setconn.size(), 0, "recompute B1: pass (3) selects no plain production slot");

    // ---- B2: pass (2), the flood-SEED selection. propagate is called for a slot iff:
    //   mother AND (primary == slot OR offline),  OR  plant AND online.
    // Five candidates, all energy > 0 (so all decrement the trunc(slot0.energy) count of 5):
    //   slot 1: mother, primary == 1        -> YES
    //   slot 2: mother, online, NOT primary -> NO
    //   slot 3: mother, OFFLINE              -> YES
    //   slot 4: plant,  online              -> YES
    //   slot 5: plant,  OFFLINE             -> NO
    // index is 0 on every slot so passes (1)/(3) stay silent (their count is slot0.index == 0). ------
    fx.reset();
    fx.planet_index                       = 0;
    fx.profiles[0].primary_mother_bldg[0] = 1;   // slot 1 is the primary mother
    fx.b(0, 0).index                      = 0;   // passes (1)/(3) count 0
    fx.b(0, 0).energy                     = 5.0; // pass (2) walks 5 energised slots
    auto seed_seed                        = [&](int32_t slot, uint8_t type, int16_t online) {
        building &b                      = fx.b(0, slot);
        b.building_id                    = (uint16_t)(30 + slot);
        b.energy                         = 8.0; // > 0 -> decrements the count and is type/state-gated
        b.online_state                   = online;
        fx.cfg_buildings[30 + slot].type = type;
    };
    seed_seed(1, BUILDING_TYPE_A_MOTHER, 1); // primary mother
    seed_seed(2, BUILDING_TYPE_A_MOTHER, 1); // online non-primary mother
    seed_seed(3, BUILDING_TYPE_A_MOTHER, 0); // offline mother
    seed_seed(4, BUILDING_TYPE_A_PLANT, 1);  // online plant
    seed_seed(5, BUILDING_TYPE_A_PLANT, 0);  // offline plant
    drv_reset();
    detail::power_network_recompute(fx.view(), g_drv_calls, 0);
    ck_eq((uint32_t)g_prop.size(), 3, "recompute B2: propagate seeded for exactly 3 slots");
    ck(g_prop.size() == 3 && g_prop[0].a == 1 && g_prop[1].a == 3 && g_prop[2].a == 4,
       "recompute B2: seeds are {1 primary-mother, 3 offline-mother, 4 online-plant}");
    ck_eq((uint32_t)(g_clear.size() + g_setconn.size()), 0,
          "recompute B2: passes (1)/(3) silent (index count 0)");

    // ---- B3: pass (3), the force-mark selection. set_connected is called for a slot iff:
    //   shuttle OR port OR (mother AND state != CONSTRUCTION AND primary != slot) OR state == DISMANTLING.
    //   slot 1: shuttle                                  -> YES
    //   slot 2: port                                     -> YES
    //   slot 3: mother, normal state, primary != 3       -> YES
    //   slot 4: mother, but primary == 4                 -> NO (mother arm fails, not dismantling)
    //   slot 5: production, state == DISMANTLING          -> YES (dismantling is a TOP-LEVEL alternative)
    //   slot 6: production, normal state                 -> NO
    // Energy count 0 so pass (2) is silent; pass (1) still clears all 6 occupied slots. --------------
    fx.reset();
    fx.planet_index                       = 0;
    fx.profiles[0].primary_mother_bldg[0] = 4;   // slot 4 is the primary mother
    fx.b(0, 0).index                      = 6;   // live count for passes (1)/(3)
    fx.b(0, 0).energy                     = 0.0; // pass (2) silent
    seed_occupied(fx, 1, 41, BUILDING_TYPE_A_SHUTTLE, 0);
    seed_occupied(fx, 2, 42, BUILDING_TYPE_A_PORT, 0);
    seed_occupied(fx, 3, 43, BUILDING_TYPE_A_MOTHER, 0); // normal state (0 != CONSTRUCTION)
    seed_occupied(fx, 4, 44, BUILDING_TYPE_A_MOTHER, 0); // normal state, but IS primary
    seed_occupied(fx, 5, 45, BUILDING_TYPE_A_PRODUCTION, BLDG_STATE_DISMANTLING);
    seed_occupied(fx, 6, 46, BUILDING_TYPE_A_PRODUCTION, 0);
    drv_reset();
    detail::power_network_recompute(fx.view(), g_drv_calls, 0);
    ck_eq((uint32_t)g_clear.size(), 6, "recompute B3: pass (1) clears all 6 occupied slots");
    ck_eq((uint32_t)g_setconn.size(), 4, "recompute B3: force-mark selected exactly 4 slots");
    ck(g_setconn.size() == 4 && g_setconn[0].a == 1 && g_setconn[1].a == 2 && g_setconn[2].a == 3 &&
           g_setconn[3].a == 5,
       "recompute B3: force-marked {1 shuttle, 2 port, 3 idle non-primary mother, 5 dismantling}");

    // ---- B4: pass (3) mother state gate -- a mother in CONSTRUCTION state is NOT force-marked even
    // when it is not the primary (the state != CONSTRUCTION conjunct), but the top-level DISMANTLING
    // alternative still fires for a construction-state building of a type that would otherwise fail.
    // Here: slot 1 mother, primary != 1, state == CONSTRUCTION -> NO; slot 2 production, state ==
    // CONSTRUCTION -> NO (not shuttle/port/mother, and CONSTRUCTION != DISMANTLING). -----------------
    fx.reset();
    fx.planet_index                       = 0;
    fx.profiles[0].primary_mother_bldg[0] = 9; // neither slot is primary
    fx.b(0, 0).index                      = 2;
    fx.b(0, 0).energy                     = 0.0;
    seed_occupied(fx, 1, 51, BUILDING_TYPE_A_MOTHER, BLDG_STATE_CONSTRUCTION);
    seed_occupied(fx, 2, 52, BUILDING_TYPE_A_PRODUCTION, BLDG_STATE_CONSTRUCTION);
    drv_reset();
    detail::power_network_recompute(fx.view(), g_drv_calls, 0);
    ck_eq((uint32_t)g_setconn.size(), 0,
          "recompute B4: mother/production in CONSTRUCTION state are not force-marked");
}

} // namespace mh::sim::test
