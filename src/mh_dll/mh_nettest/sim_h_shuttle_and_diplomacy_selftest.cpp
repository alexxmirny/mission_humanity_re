//
// sim_h_shuttle_and_diplomacy_selftest.cpp -- offline oracle for two SIM1-H host-reach callers that
// the rig's existing fixtures (an all-AI soak, a save/load) cannot reach: neither clicks a UI dialog
// button (the shuttle handler's only caller) nor switches planets (restore_relations' only caller,
// SwitchToPlanet), so the whole-closure A/B never observes either body. This file IS their evidence.
//
//   llm_strat_prod_shuttle_slot_spawn_arrival @0x004906c1
//     (sim/hostreach/sim_h_shuttle_slot_spawn_arrival.h/.cpp)
//   llm_diplomacy_restore_relations           @0x00465c6e
//     (sim/hostreach/sim_h_diplomacy_restore_relations.h/.cpp)
//
// Both are PROMOTED-ONLY / NO SHADOW SITE (see each header's banner). Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_prod_shuttle_slot_spawn_arrival_004906c1.asm,
// tmp/decomp_sim/llm_diplomacy_restore_relations_00465c6e.asm), never from the Ghidra .c drafts beside
// them -- addresses cited in every check message are read off those .asm files directly.
//
#include "sim/hostreach/sim_h_shuttle_slot_spawn_arrival.h"
#include "sim/hostreach/sim_h_diplomacy_restore_relations.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// =====================================================================================================
// llm_strat_prod_shuttle_slot_spawn_arrival @0x004906c1 -- recorder mocks
// =====================================================================================================

int32_t  g_mothership_calls      = 0;
int32_t  g_mothership_player_arg = -1;
uint32_t g_mothership_result     = 0;
uint32_t g_mothership_out_x      = 0;
uint32_t g_mothership_out_y      = 0;

uint32_t rec_find_mothership_position(int32_t player, uint32_t *out_x, uint32_t *out_y) {
    ++g_mothership_calls;
    g_mothership_player_arg = player;
    *out_x                  = g_mothership_out_x;
    *out_y                  = g_mothership_out_y;
    return g_mothership_result;
}

struct spawn_call {
    uint16_t player;
    uint32_t slot;
    uint32_t x;
    uint32_t y;
    int32_t  storage_idx;
};
std::vector<spawn_call> g_spawn_calls;

uint32_t rec_spawn_arrived_unit(uint16_t player, uint32_t slot, uint32_t x, uint32_t y,
                                int32_t storage_idx) {
    g_spawn_calls.push_back({player, slot, x, y, storage_idx});
    return 0;
}

const shuttle_slot_spawn_arrival_calls g_shuttle_calls = {
    &rec_find_mothership_position,
    &rec_spawn_arrived_unit,
};

void reset_shuttle_recorders() {
    g_mothership_calls      = 0;
    g_mothership_player_arg = -1;
    g_mothership_result     = 0;
    g_mothership_out_x      = 0;
    g_mothership_out_y      = 0;
    g_spawn_calls.clear();
}

// =====================================================================================================
// llm_diplomacy_restore_relations @0x00465c6e -- recorder mocks
// =====================================================================================================

// Set to the live sim_store right before the call under test, so the recorder can read the relation
// table LIVE at call time (see swap_call::relation's comment).
sim_store *g_dsr_own = nullptr;

struct swap_call {
    int32_t player_a;
    int32_t player_b;
    // Read live off own.player_relation_at(player_a, player_b) at call time. llm_diplomacy_set_relation
    // writes this exact slot as its own FIRST act (0x0049a0d2-0x0049a0dc, before any of the calls this
    // TU mocks), so by the time this mock runs the slot already holds the triple's relation value --
    // this is how the test recovers the raw relation (the mocked calls themselves never carry it,
    // only the derived SIGN reaches diplomacy_ai_relation_swap).
    uint8_t relation;
};
std::vector<swap_call> g_swap_calls;

void   *rec_ansi_to_wide_scratch(char   */*ansi*/) { return nullptr; }
int32_t rec_w_sprintf_visis(void * /*dst*/, const wchar_t * /*format*/, int32_t /*a0*/,
                            const wchar_t * /*a1*/, int32_t /*a2*/, const wchar_t * /*a3*/) {
    return 0;
}
int32_t rec_w_sprintf_vs(void * /*dst*/, const wchar_t * /*format*/, const wchar_t * /*a0*/) {
    return 0;
}
int32_t rec_diplomacy_ai_relation_swap(int32_t player, int32_t toward_player,
                                       int32_t /*new_relation_sign*/) {
    const uint8_t relation =
        g_dsr_own ? g_dsr_own->player_relation_at((uint32_t)player, (uint32_t)toward_player) : 0;
    g_swap_calls.push_back({player, toward_player, relation});
    return 0;
}
void rec_net_chat_ally_mask_rebuild() {}

const diplomacy_set_relation_calls g_dsr_calls = {
    &rec_ansi_to_wide_scratch,
    &rec_w_sprintf_visis,
    &rec_w_sprintf_vs,
    &rec_diplomacy_ai_relation_swap,
    &rec_net_chat_ally_mask_rebuild,
};

void reset_diplomacy_recorders() {
    g_swap_calls.clear();
    g_dsr_own = nullptr;
}

// player_profile.status_flags bit0 -- "slot enabled" (E_STRAT_PLAYER_STATUS bit0, no committed Ghidra
// enum; same file-local copy every sim/ TU that tests this bit keeps -- see
// sim_h_diplomacy_restore_relations.cpp).
constexpr uint32_t SLOT_ENABLED = 0x1u;

} // namespace

void run_h_shuttle_and_diplomacy_tests() {
    sim_fixture fx;

    // =================================================================================================
    // llm_strat_prod_shuttle_slot_spawn_arrival @0x004906c1
    // =================================================================================================

    // S1 -- origin_planet != G_PLANET_INDEX (JNZ @0x00490703 taken): find_mothership_position is
    // NEVER called (path (a)); x/y reach spawn_arrived_unit as the translation's defined-0 substitute
    // for the original's never-written stack residue (see the header banner's uninitialised-locals
    // derivation); spawn_arrived_unit IS still called -- the tail call is unconditional.
    {
        fx.reset();
        reset_shuttle_recorders();
        constexpr uint16_t PLAYER                                                          = 0;
        constexpr uint32_t SLOT                                                            = 2;
        fx.planet_index                                                                    = 5;
        fx.player_side                                                                     = PLAYER;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].origin_planet = 9; // != 5

        detail::shuttle_slot_spawn_arrival(fx.view(), g_shuttle_calls, SLOT);

        ck_eq((uint32_t)g_mothership_calls, 0u,
              "S1: origin_planet!=G_PLANET_INDEX -- find_mothership_position NOT called, 0x00490703");
        ck(g_spawn_calls.size() == 1,
           "S1: spawn_arrived_unit IS still called -- the tail call is unconditional, 0x0049074c");
        if (g_spawn_calls.size() == 1) {
            const spawn_call &c = g_spawn_calls[0];
            ck_eq((uint32_t)c.player, (uint32_t)PLAYER, "S1: spawn_arrived_unit player arg, 0x00490758");
            ck_eq(c.slot, SLOT,
                  "S1: spawn_arrived_unit slot arg is the original slot_index param, not the "
                  "player*10+slot record index, 0x00490754");
            ck_eq(c.x, 0u, "S1: x = defined-0 substitute for the never-written residue local, 0x0049074e");
            ck_eq(c.y, 0u, "S1: y = defined-0 substitute for the never-written residue local, 0x0049074e");
            ck_eq((uint32_t)c.storage_idx, 0u,
                  "S1: storage_idx is the unconditional literal 0 (PUSH 0x0), 0x00490745");
        }
    }

    // S2 -- origin_planet == G_PLANET_INDEX AND find_mothership_position returns NONZERO (path (b)):
    // the callee's OWN out-params reach spawn_arrived_unit, not the landing fallback -- landing_x/
    // landing_y are seeded with different, plausible-looking values that must NOT arrive.
    {
        fx.reset();
        reset_shuttle_recorders();
        constexpr uint16_t PLAYER                                                          = 0;
        constexpr uint32_t SLOT                                                            = 2;
        fx.planet_index                                                                    = 5;
        fx.player_side                                                                     = PLAYER;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].origin_planet = 5;   // == planet_index
        fx.profiles[PLAYER].landing_x[5]                                                   = 999; // must NOT reach spawn_arrived_unit on this path
        fx.profiles[PLAYER].landing_y[5]                                                   = 888;
        g_mothership_result                                                                = 1; // nonzero: mothership found
        g_mothership_out_x                                                                 = 41;
        g_mothership_out_y                                                                 = 67;

        detail::shuttle_slot_spawn_arrival(fx.view(), g_shuttle_calls, SLOT);

        ck_eq((uint32_t)g_mothership_calls, 1u,
              "S2: origin_planet==G_PLANET_INDEX -- find_mothership_position(player) called once, "
              "0x0049070e");
        ck_eq((uint32_t)g_mothership_player_arg, (uint32_t)PLAYER,
              "S2: find_mothership_position's player arg, 0x0049070b");
        ck(g_spawn_calls.size() == 1, "S2: spawn_arrived_unit called once, 0x0049075c");
        if (g_spawn_calls.size() == 1) {
            const spawn_call &c = g_spawn_calls[0];
            ck_eq(c.x, 41u, "S2: x = the callee's OWN out_x (41), NOT the landing_x bait (999), 0x0049074e");
            ck_eq(c.y, 67u, "S2: y = the callee's OWN out_y (67), NOT the landing_y bait (888), 0x0049074e");
        }
    }

    // S3 -- origin_planet == G_PLANET_INDEX AND find_mothership_position returns ZERO (path (c)): the
    // landing fallback (profiles[player].landing_x/y[G_PLANET_INDEX]) is used, indexed by the CURRENT
    // planet -- a neighbouring planet's slot holds contradictory bait and must not be read.
    {
        fx.reset();
        reset_shuttle_recorders();
        constexpr uint16_t PLAYER                                                          = 0;
        constexpr uint32_t SLOT                                                            = 2;
        fx.planet_index                                                                    = 5;
        fx.player_side                                                                     = PLAYER;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].origin_planet = 5;
        fx.profiles[PLAYER].landing_x[5]                                                   = 123; // the CORRECT (current) planet's slot
        fx.profiles[PLAYER].landing_y[5]                                                   = 456;
        fx.profiles[PLAYER].landing_x[6]                                                   = 321; // a DIFFERENT planet's slot -- must NOT be read
        fx.profiles[PLAYER].landing_y[6]                                                   = 654;
        g_mothership_result                                                                = 0;      // zero: no mothership found
        g_mothership_out_x                                                                 = 999999; // stale on this path -- must be ignored
        g_mothership_out_y                                                                 = 999999;

        detail::shuttle_slot_spawn_arrival(fx.view(), g_shuttle_calls, SLOT);

        ck(g_spawn_calls.size() == 1, "S3: spawn_arrived_unit called once, 0x0049075c");
        if (g_spawn_calls.size() == 1) {
            const spawn_call &c = g_spawn_calls[0];
            ck_eq(c.x, 123u,
                  "S3: x = landing_x[G_PLANET_INDEX], the CURRENT planet's slot, 0x00490728-0x0049072e");
            ck_eq(c.y, 456u,
                  "S3: y = landing_y[G_PLANET_INDEX], the CURRENT planet's slot, 0x00490743-0x00490749");
        }
    }

    // S4 -- the slot record index is player*PROD_SHUTTLE_SLOTS_PER_PLAYER(10) + slot_index (0x1f18 =
    // 10*0x31c per player, 0x31c per slot -- 0x004906e6-0x004906f4). Running as PlayerSide=1, with
    // player 0's record at the SAME slot_index seeded to take the OTHER branch: a translation using
    // the wrong per-player stride would read player 0's record (and take path (a)) instead of player
    // 1's (path (b)), which this test can tell apart by whether find_mothership_position fires at all.
    {
        fx.reset();
        reset_shuttle_recorders();
        constexpr uint32_t SLOT                                                       = 3;
        fx.planet_index                                                               = 5;
        fx.player_side                                                                = 1; // PlayerSide=1
        fx.prod_shuttle_slots[0 * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].origin_planet = 9; // player0: path (a)
        fx.prod_shuttle_slots[1 * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].origin_planet = 5; // player1: path (b)
        g_mothership_result                                                           = 1;
        g_mothership_out_x                                                            = 71;
        g_mothership_out_y                                                            = 29;

        detail::shuttle_slot_spawn_arrival(fx.view(), g_shuttle_calls, SLOT);

        ck_eq((uint32_t)g_mothership_calls, 1u,
              "S4: player1's slot record read (its origin_planet==planet_index) -- a wrong stride "
              "reading player0's slot instead would skip this call entirely, 0x004906e6-0x004906f4");
        ck_eq((uint32_t)g_mothership_player_arg, 1u, "S4: find_mothership_position(player=1), 0x0049070b");
        ck(g_spawn_calls.size() == 1, "S4: spawn_arrived_unit called once, 0x0049075c");
        if (g_spawn_calls.size() == 1) {
            const spawn_call &c = g_spawn_calls[0];
            ck_eq((uint32_t)c.player, 1u, "S4: spawn_arrived_unit player=1 (PlayerSide), 0x00490758");
            ck_eq(c.slot, SLOT, "S4: spawn_arrived_unit slot=3 (the original slot_index param)");
            ck_eq(c.x, 71u, "S4: x from player1's mothership call, not player0's residue-0 path");
            ck_eq(c.y, 29u, "S4: y from player1's mothership call");
        }
    }

    // =================================================================================================
    // llm_diplomacy_restore_relations @0x00465c6e
    // =================================================================================================

    // D1 -- the two-table hazard + the AND conjunction (incl. i==j) + iteration order, in one fixture
    // shape: only players 2 and 5 have an ENABLED profile (v.profiles[i].status_flags bit0, base
    // 0xcff060, stride 0x740). Player 3's PROFILE stays disabled but its relation ROW (own.
    // player_relation_at -- a DIFFERENT table, base 0xe587e9, stride 0x34) is seeded with loud,
    // distinctive bait values that must never surface. The applied set must be EXACTLY the 2x2 cross
    // of {2,5}x{2,5} (i==j pairs included, per 0x00465cd0's shared IMUL i,0x34 + j indexing -- there is
    // no i!=j guard in the disassembly), in i-outer/j-inner order, each carrying the exact relation
    // value from player_desc_slots[i].relation[j] -- non-symmetric per-player rows so a swapped (i,j)
    // argument order is caught too.
    {
        fx.reset();
        reset_diplomacy_recorders();

        fx.profiles[2].status_flags = SLOT_ENABLED;
        fx.profiles[5].status_flags = SLOT_ENABLED;
        // player 3 stays DISABLED (status_flags==0 from reset()) -- but its relation row is loud bait.
        for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
            fx.player_desc_slots[3].relation[j] = (uint8_t)(90 + j); // must NEVER be applied
        }
        // Non-symmetric, distinct-per-cell rows for the two ENABLED players.
        for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
            fx.player_desc_slots[2].relation[j] = (uint8_t)(10 + j);
            fx.player_desc_slots[5].relation[j] = (uint8_t)(50 + j);
        }

        sim_store own = fx.store();
        g_dsr_own     = &own;
        detail::diplomacy_restore_relations(fx.view(), own, g_dsr_calls);

        ck_eq((uint32_t)g_swap_calls.size(), 4u,
              "D1: exactly the 2x2 {2,5}x{2,5} cross applied -- AND conjunction, not OR, "
              "0x00465cb5-0x00465ccc");
        if (g_swap_calls.size() == 4) {
            // i-outer/j-inner: (2,2),(2,5),(5,2),(5,5). A swapped loop nesting produces (2,2),(5,2),
            // (2,5),(5,5) instead -- a different sequence over this same asymmetric set.
            ck(g_swap_calls[0].player_a == 2 && g_swap_calls[0].player_b == 2 &&
                   g_swap_calls[0].relation == 12,
               "D1[0]: (i=2,j=2) relation=player_desc_slots[2].relation[2]=12 -- i==j pairs ARE applied");
            ck(g_swap_calls[1].player_a == 2 && g_swap_calls[1].player_b == 5 &&
                   g_swap_calls[1].relation == 15,
               "D1[1]: (i=2,j=5) relation=player_desc_slots[2].relation[5]=15 -- i-outer/j-inner order");
            ck(g_swap_calls[2].player_a == 5 && g_swap_calls[2].player_b == 2 &&
                   g_swap_calls[2].relation == 52,
               "D1[2]: (i=5,j=2) relation=player_desc_slots[5].relation[2]=52 -- proves i/j args are not "
               "swapped (52 != 15, the (2,5) value)");
            ck(g_swap_calls[3].player_a == 5 && g_swap_calls[3].player_b == 5 &&
                   g_swap_calls[3].relation == 55,
               "D1[3]: (i=5,j=5) relation=player_desc_slots[5].relation[5]=55");
        }
        // Never applied: player 3 on either side, despite its loud relation-table bait -- the ENABLED
        // gate is read from v.profiles[].status_flags only, never from the relation table.
        for (const swap_call &c : g_swap_calls) {
            ck(c.player_a != 3 && c.player_b != 3,
               "D1: player 3 (disabled profile, distinctive relation bait) never applied -- the two "
               "tables were not crossed, 0x00465cae-0x00465cbc");
        }
        g_dsr_own = nullptr;
    }

    // D2 -- all profiles disabled (the trivial "neither" case, isolated from D1's bait so an inverted
    // enabled-check mutant that fires when NO slot is enabled is caught on its own): zero calls.
    {
        fx.reset();
        reset_diplomacy_recorders();
        // fx.reset() already zeroes every profile's status_flags -- nothing to seed.

        sim_store own = fx.store();
        g_dsr_own     = &own;
        detail::diplomacy_restore_relations(fx.view(), own, g_dsr_calls);

        ck_eq((uint32_t)g_swap_calls.size(), 0u,
              "D2: no enabled profiles -- llm_diplomacy_set_relation never called, "
              "0x00465cbc/0x00465ccc");
        g_dsr_own = nullptr;
    }
}

} // namespace mh::sim::test
