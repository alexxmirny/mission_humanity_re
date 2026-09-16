#include "sim/sim_bldg_construction_complete.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/_A_SHUTTLE/_A_MAIN_BASE/_H_MOTHER/_H_BARRACKS/_H_HELIPAD/_H_SHUTTLE

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorder for bldg_completion_dispatch (the UNCONDITIONAL callee) --------------------------------
struct completion_dispatch_call {
    uint32_t player;
    uint32_t building_index;
    uint32_t param_3;
    uint32_t param_4;
    double   game_clock;
};
std::vector<completion_dispatch_call> g_completion_dispatch_calls;

// T7's mutation hook: lets the MOCK completion_dispatch itself rewrite building_id mid-call, so a case
// can observe whether the type dispatch that follows reads building_id/type FRESH (after this callee
// returns) or from a value cached before the call. nullptr = no mutation (every other case).
uint16_t *g_completion_dispatch_bid_target = nullptr;
uint16_t  g_completion_dispatch_new_bid    = 0;

void rec_completion_dispatch(uint32_t player, uint32_t building_index, uint32_t param_3,
                             uint32_t param_4, double game_clock) {
    g_completion_dispatch_calls.push_back({player, building_index, param_3, param_4, game_clock});
    if (g_completion_dispatch_bid_target != nullptr) *g_completion_dispatch_bid_target = g_completion_dispatch_new_bid;
}

// ---- recorder for bldg_start_special_anim (A_SHUTTLE arm only) ---------------------------------------
struct start_special_anim_call {
    uint16_t player;
    int32_t  b_index;
    uint32_t unused_ebx;
    uint32_t unused_ecx;
    double   timestamp;
};
std::vector<start_special_anim_call> g_start_special_anim_calls;
void                                 rec_start_special_anim(uint16_t player, int32_t b_index, uint32_t unused_ebx,
                                                            uint32_t unused_ecx, double timestamp) {
    g_start_special_anim_calls.push_back({player, b_index, unused_ebx, unused_ecx, timestamp});
}

// ---- recorder for bldg_anim_state_trigger (A_MOTHER/H_MOTHER arm only) -------------------------------
struct anim_state_trigger_call {
    uint32_t param_1;
    int32_t  param_2;
    uint32_t param_3;
    uint32_t param_4;
    uint32_t clock_lo;
    uint32_t clock_hi;
};
std::vector<anim_state_trigger_call> g_anim_state_trigger_calls;
void                                 rec_anim_state_trigger(uint32_t param_1, int32_t param_2, uint32_t param_3,
                                                            uint32_t param_4, uint32_t clock_lo, uint32_t clock_hi) {
    g_anim_state_trigger_calls.push_back({param_1, param_2, param_3, param_4, clock_lo, clock_hi});
}

// ---- recorder for bldg_set_staffed_flag (every LAND_ACTIVATE tail) -----------------------------------
struct set_staffed_flag_call {
    uint16_t player;
    int32_t  building_index;
};
std::vector<set_staffed_flag_call> g_set_staffed_flag_calls;
void                               rec_set_staffed_flag(uint16_t player, int32_t building_index) {
    g_set_staffed_flag_calls.push_back({player, building_index});
}

// ---- recorder for refresh_building (every LAND_ACTIVATE tail) ----------------------------------------
struct refresh_building_call {
    uint16_t p_id;
    int32_t  b_id;
};
std::vector<refresh_building_call> g_refresh_building_calls;
void                               rec_refresh_building(uint16_t p_id, int32_t b_id) {
    g_refresh_building_calls.push_back({p_id, b_id});
}

const bldg_construction_complete_calls g_calls = {
    &rec_completion_dispatch,
    &rec_start_special_anim,
    &rec_anim_state_trigger,
    &rec_set_staffed_flag,
    &rec_refresh_building,
};

void reset_recorders() {
    g_completion_dispatch_calls.clear();
    g_completion_dispatch_bid_target = nullptr;
    g_completion_dispatch_new_bid    = 0;
    g_start_special_anim_calls.clear();
    g_anim_state_trigger_calls.clear();
    g_set_staffed_flag_calls.clear();
    g_refresh_building_calls.clear();
}

constexpr uint16_t PLAYER         = 2;
constexpr int32_t  BUILDING_INDEX = 5;
constexpr uint32_t PARAM_3        = 0x1001; // dead-but-forwarded sentinel (never written by the body)
constexpr uint32_t PARAM_4        = 0x2002; // distinct from PARAM_3, same reason
constexpr double   GAME_CLOCK_VAL = 12345.6875;

// Splits a double's raw bit pattern the same way the .asm's two GAME_CLOCK PUSHes do (high dword
// first, then low) -- used here ONLY to derive an INDEPENDENT expected lo/hi for the bit-decompose
// consistency check, not to duplicate the production split_game_clock() under test.
void expected_split(double clock, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &clock, sizeof(bits));
    lo = static_cast<uint32_t>(bits);
    hi = static_cast<uint32_t>(bits >> 32);
}

} // namespace

void run_bldg_construction_complete_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- type == A_MOTHER(6): anim_state_trigger fires, THEN state=LAND_ACTIVATE(0x85) +
    // set_staffed_flag + refresh_building (LAB_00478e62's own tail). start_special_anim does NOT fire.
    // Also pins: completion_dispatch's UNCONDITIONAL fire with param_3/param_4 forwarded verbatim, and
    // the bit-decompose consistency check (anim_state_trigger's split lo/hi vs. completion_dispatch's
    // own *game_clock double, same seed, same call).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID                   = 10;
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID;
        fx.cfg_buildings[BID].type               = BUILDING_TYPE_A_MOTHER;
        fx.game_clock                            = GAME_CLOCK_VAL;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_completion_dispatch_calls.size(), 1u,
              "T1: bldg_completion_dispatch fires unconditionally, 0x00478da6-0x00478db9");
        ck_eq(g_completion_dispatch_calls[0].player, (uint32_t)PLAYER, "T1: completion_dispatch player&0xffff");
        ck_eq(g_completion_dispatch_calls[0].building_index, (uint32_t)BUILDING_INDEX,
              "T1: completion_dispatch building_index");
        ck_eq(g_completion_dispatch_calls[0].param_3, PARAM_3, "T1: completion_dispatch param_3 forwarded verbatim");
        ck_eq(g_completion_dispatch_calls[0].param_4, PARAM_4, "T1: completion_dispatch param_4 forwarded verbatim");
        ck_eq_d(g_completion_dispatch_calls[0].game_clock, GAME_CLOCK_VAL, "T1: completion_dispatch *game_clock, double");

        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 1u,
              "T1: bldg_anim_state_trigger fires for A_MOTHER, 0x00478e75");
        ck_eq((uint32_t)g_start_special_anim_calls.size(), 0u, "T1: bldg_start_special_anim does NOT fire for A_MOTHER");
        ck_eq(g_anim_state_trigger_calls[0].param_1, (uint32_t)PLAYER, "T1: anim_state_trigger player&0xffff");
        ck_eq((uint32_t)g_anim_state_trigger_calls[0].param_2, (uint32_t)BUILDING_INDEX,
              "T1: anim_state_trigger building_index");
        ck_eq(g_anim_state_trigger_calls[0].param_3, PARAM_3, "T1: anim_state_trigger param_3 forwarded verbatim");
        ck_eq(g_anim_state_trigger_calls[0].param_4, PARAM_4, "T1: anim_state_trigger param_4 forwarded verbatim");
        {
            uint32_t exp_lo, exp_hi;
            // Bit-decompose consistency check, independent of both the production split_game_clock()
            // AND of completion_dispatch's own recorded copy: derive lo/hi directly from the fixture's
            // SEEDED double (the same value both callees receive as *v.game_clock) and confirm
            // anim_state_trigger's param_5/param_6 split matches it -- comparing raw dwords, never
            // reconstructing a double from them (a lo/hi swap could hide behind equal doubles for a
            // palindromic bit pattern; comparing the dwords directly rules that out).
            expected_split(GAME_CLOCK_VAL, exp_lo, exp_hi);
            ck_eq(g_anim_state_trigger_calls[0].clock_lo, exp_lo,
                  "T1: anim_state_trigger clock_lo == GAME_CLOCK's low dword, split consistency");
            ck_eq(g_anim_state_trigger_calls[0].clock_hi, exp_hi,
                  "T1: anim_state_trigger clock_hi == GAME_CLOCK's high dword, split consistency");
        }

        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x85u,
              "T1: state = LAND_ACTIVATE(0x85), LAB_00478e62 tail, 0x00478e8a");
        ck((g_set_staffed_flag_calls.size() == 1 && g_set_staffed_flag_calls[0].player == PLAYER &&
            g_set_staffed_flag_calls[0].building_index == BUILDING_INDEX),
           "T1: bldg_set_staffed_flag(player, building_index) fires once, 0x00478e9a");
        ck((g_refresh_building_calls.size() == 1 && g_refresh_building_calls[0].p_id == PLAYER &&
            g_refresh_building_calls[0].b_id == BUILDING_INDEX),
           "T1: llm_strat_refresh_building(player, building_index) fires once, 0x00478ea6");
    }

    // =================================================================================================
    // T2 -- type == H_MOTHER(0x1a): SAME shape as T1 (anim_state_trigger + the LAND_ACTIVATE tail),
    // pinned as a SEPARATE case with a DIFFERENT type value to prove both values reach the same code by
    // the dispatch chain (0x00478df6 JBE 0x00478e62), not by coincidence.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID                   = 11;
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID;
        fx.cfg_buildings[BID].type               = BUILDING_TYPE_H_MOTHER;
        fx.game_clock                            = GAME_CLOCK_VAL;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 1u,
              "T2: bldg_anim_state_trigger fires for H_MOTHER too, 0x00478df6-0x00478e75 (separate value, same tail)");
        ck_eq((uint32_t)g_start_special_anim_calls.size(), 0u, "T2: bldg_start_special_anim does NOT fire for H_MOTHER");
        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x85u, "T2: state = LAND_ACTIVATE(0x85) for H_MOTHER too");
        ck((g_set_staffed_flag_calls.size() == 1), "T2: bldg_set_staffed_flag fires once for H_MOTHER");
        ck((g_refresh_building_calls.size() == 1), "T2: refresh_building fires once for H_MOTHER");
    }

    // =================================================================================================
    // T3 -- type == A_SHUTTLE(0xd): start_special_anim fires, THEN falls straight through (no jump)
    // into LAB_00478e2f's tail -- state=LAND_ACTIVATE(0x85) + set_staffed_flag + refresh_building.
    // anim_state_trigger does NOT fire.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID                   = 12;
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID;
        fx.cfg_buildings[BID].type               = BUILDING_TYPE_A_SHUTTLE;
        fx.game_clock                            = GAME_CLOCK_VAL;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_start_special_anim_calls.size(), 1u,
              "T3: bldg_start_special_anim fires for A_SHUTTLE, 0x00478e2a");
        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 0u, "T3: bldg_anim_state_trigger does NOT fire for A_SHUTTLE");
        ck_eq((uint32_t)g_start_special_anim_calls[0].player, (uint32_t)PLAYER, "T3: start_special_anim player&0xffff");
        ck_eq((uint32_t)g_start_special_anim_calls[0].b_index, (uint32_t)BUILDING_INDEX,
              "T3: start_special_anim building_index");
        ck_eq(g_start_special_anim_calls[0].unused_ebx, PARAM_3, "T3: start_special_anim param_3 forwarded verbatim");
        ck_eq(g_start_special_anim_calls[0].unused_ecx, PARAM_4, "T3: start_special_anim param_4 forwarded verbatim");
        ck_eq_d(g_start_special_anim_calls[0].timestamp, GAME_CLOCK_VAL, "T3: start_special_anim *game_clock, double");

        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x85u,
              "T3: state = LAND_ACTIVATE(0x85), LAB_00478e2f tail (fallthrough, no jump), 0x00478e3f");
        ck((g_set_staffed_flag_calls.size() == 1), "T3: bldg_set_staffed_flag fires once for A_SHUTTLE");
        ck((g_refresh_building_calls.size() == 1), "T3: refresh_building fires once for A_SHUTTLE");
    }

    // =================================================================================================
    // T4 -- type == H_SHUTTLE(0x21): THE ASYMMETRY the header flags as genuine, not a bug. Jumps
    // DIRECTLY into LAB_00478e2f's tail (0x00478e00 JZ 0x00478e2f) -- state=LAND_ACTIVATE(0x85) +
    // set_staffed_flag + refresh_building all fire, but NEITHER start_special_anim NOR
    // anim_state_trigger fires (unlike A_SHUTTLE, which calls start_special_anim before the same tail).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID                   = 13;
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID;
        fx.cfg_buildings[BID].type               = BUILDING_TYPE_H_SHUTTLE;
        fx.game_clock                            = GAME_CLOCK_VAL;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_start_special_anim_calls.size(), 0u,
              "T4 [asymmetry vs A_SHUTTLE, header's own note]: start_special_anim does NOT fire for H_SHUTTLE, "
              "0x00478e00 jumps DIRECTLY past it to LAB_00478e2f");
        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 0u, "T4: anim_state_trigger does NOT fire for H_SHUTTLE either");
        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x85u,
              "T4: state = LAND_ACTIVATE(0x85) still reached (direct jump into the tail), 0x00478e3f");
        ck((g_set_staffed_flag_calls.size() == 1), "T4: bldg_set_staffed_flag still fires for H_SHUTTLE (tail runs)");
        ck((g_refresh_building_calls.size() == 1), "T4: refresh_building still fires for H_SHUTTLE (tail runs)");
    }

    // =================================================================================================
    // T5 -- dead-zone gap 1: type == 0x14, STRICTLY BETWEEN A_MAIN_BASE(0x0e) and H_MOTHER(0x1a)
    // (exclusive of both named boundaries -- the header's own "0xe..0x19" dead range does include the
    // boundary 0xe itself, but this case deliberately picks a value with no CMP edge of its own nearby,
    // so it cannot be mistaken for a boundary off-by-one). Pins: (a) the UNCONDITIONAL
    // CONSTRUCTION(0x64) write at entry survives to the end (only observable when no later arm
    // overwrites it to LAND_ACTIVATE), (b) completion_dispatch still fires unconditionally, (c) NONE of
    // the four post-dispatch callees fire.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID                   = 14;
        constexpr uint8_t  TYPE                  = 0x14; // strictly between A_MAIN_BASE(0xe) and H_MOTHER(0x1a)
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID;
        fx.cfg_buildings[BID].type               = TYPE;
        fx.game_clock                            = GAME_CLOCK_VAL;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_completion_dispatch_calls.size(), 1u,
              "T5: completion_dispatch STILL fires unconditionally on a dead-zone type, 0x00478da6-0x00478db9");
        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x64u,
              "T5: state stays CONSTRUCTION(0x64) -- the unconditional entry write (0x00478d9d) survives "
              "because no later arm overwrites it on this dead-zone path, 0x00478e07/0x00478e12/0x00478eab");
        ck_eq((uint32_t)g_start_special_anim_calls.size(), 0u, "T5: start_special_anim does NOT fire (dead zone)");
        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 0u, "T5: anim_state_trigger does NOT fire (dead zone)");
        ck_eq((uint32_t)g_set_staffed_flag_calls.size(), 0u, "T5: set_staffed_flag does NOT fire (dead zone)");
        ck_eq((uint32_t)g_refresh_building_calls.size(), 0u, "T5: refresh_building does NOT fire (dead zone)");
    }

    // =================================================================================================
    // T6 -- dead-zone gap 2: type == H_HELIPAD(0x1e), inside the 0x1b..0x20 "between H_BARRACKS and
    // H_SHUTTLE" range -- the SECOND, DIFFERENT gap the header calls out, so this case cannot be
    // confused with T5 landing in the same branch by accident (different CMP/JC edges: 0x00478dfc's
    // CMP type,0x21 fails and falls to the final JMP 0x00478eab, vs. T5's 0x00478df4 JC path). Same
    // three assertions as T5.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID                   = 15;
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID;
        fx.cfg_buildings[BID].type               = BUILDING_TYPE_H_HELIPAD;
        fx.game_clock                            = GAME_CLOCK_VAL;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_completion_dispatch_calls.size(), 1u,
              "T6: completion_dispatch STILL fires unconditionally on the second dead zone too");
        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x64u,
              "T6: state stays CONSTRUCTION(0x64), second dead zone (0x1b..0x20), 0x00478e02");
        ck_eq((uint32_t)g_start_special_anim_calls.size(), 0u, "T6: start_special_anim does NOT fire (dead zone 2)");
        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 0u, "T6: anim_state_trigger does NOT fire (dead zone 2)");
        ck_eq((uint32_t)g_set_staffed_flag_calls.size(), 0u, "T6: set_staffed_flag does NOT fire (dead zone 2)");
        ck_eq((uint32_t)g_refresh_building_calls.size(), 0u, "T6: refresh_building does NOT fire (dead zone 2)");
    }

    // =================================================================================================
    // T7 -- building_id/type are read FRESH AFTER completion_dispatch returns, not cached from before
    // the call. Directly observable here: the MOCK completion_dispatch itself rewrites
    // buildings[player][building_index].building_id mid-call (via g_completion_dispatch_bid_target),
    // from an OLD building_id whose cfg type is a dead-zone value to a NEW building_id whose cfg type
    // is A_MOTHER. If the dispatch used a building_id/type cached BEFORE the call, this would land in
    // the dead zone (no callee fires, state stays 0x64); if it re-reads AFTER the call (as the header's
    // step 3 claims), anim_state_trigger fires and state ends at LAND_ACTIVATE(0x85).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t BID_OLD               = 20;
        constexpr uint16_t BID_NEW               = 55;
        fx.b(PLAYER, BUILDING_INDEX).building_id = BID_OLD;
        fx.cfg_buildings[BID_OLD].type           = BUILDING_TYPE_A_MAIN_BASE; // dead zone if read stale
        fx.cfg_buildings[BID_NEW].type           = BUILDING_TYPE_A_MOTHER;    // real arm if read fresh
        fx.game_clock                            = GAME_CLOCK_VAL;

        g_completion_dispatch_bid_target = &fx.b(PLAYER, BUILDING_INDEX).building_id;
        g_completion_dispatch_new_bid    = BID_NEW;

        sim_store own = fx.store();
        detail::bldg_construction_complete(fx.view(), own, g_calls, PLAYER,
                                           static_cast<uint32_t>(BUILDING_INDEX), PARAM_3, PARAM_4);

        ck_eq((uint32_t)g_anim_state_trigger_calls.size(), 1u,
              "T7: building_id/type read FRESH after completion_dispatch returns (0x00478dbe-0x00478de1) -- "
              "the mock rewrote building_id mid-call to a BID whose type is A_MOTHER, and anim_state_trigger "
              "fired, proving the dispatch used the NEW value, not one cached before the call");
        ck_eq((uint32_t)fx.b(PLAYER, BUILDING_INDEX).state, 0x85u,
              "T7: state reaches LAND_ACTIVATE(0x85) via the freshly-read A_MOTHER arm");
    }
}

} // namespace mh::sim::test
