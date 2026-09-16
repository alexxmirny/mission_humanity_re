//
// ai_players_tick_selftest.cpp -- llm_strat_ai_players_tick @0x004db905, the AI's five-loop
// per-frame scheduler (RI-AI batch D / AI1D). This file pins: the map-extent mask re-stamp being
// unconditional and independent of active_player_count; loop 1's map-influence / turret-rescan
// triggers firing on a BITWISE AND of (pending, ai_enabled) rather than a logical `&&`; loop 2's
// ai_clock accumulate happening through a double-wide intermediate (FADD double) rather than
// rounding dt to float first, and running for EVERY player regardless of ai_enabled; the three
// catch-up loops (strategy/tactic/move) each advancing their OWN threshold by their OWN period
// every iteration regardless of whether the phase call actually fired, with the phase call gated
// on ai_enabled for strategy/tactic but CHOSEN by ai_enabled for move (active_unit_tick vs
// passive_engage_tick -- always exactly one, never neither); the multi-tick catch-up fire COUNT
// that only the real "threshold += period" shape reproduces (a "snap the threshold to the current
// clock" shape would fire on every tick instead); the call ORDER within loop 1 and across the three
// phase loops; and active_player_count bounding every one of the five walks, including up to the
// fixture's spare 9th slot.
//
#include "ai_test_support.h"

#include "ai/ai_players_tick.h"

#include <string>

namespace mh::ai::test {
namespace {

// A calls set with all six of players_tick's outward edges bound to recorders -- there is no
// seventh edge to leave deliberately null here, unlike most of this file's siblings. One shared
// ORDER log across all six so cross-loop and cross-phase SEQUENCE is directly observable, not just
// per-callee counts (which a swapped-callee mutation could still satisfy by accident).
struct rec_t {
    std::vector<int32_t>     map_influence;
    std::vector<int32_t>     turret_rescan;
    std::vector<int32_t>     strategy;
    std::vector<int32_t>     tactic;
    std::vector<int32_t>     active;
    std::vector<int32_t>     passive;
    std::vector<std::string> order;

    void clear() {
        map_influence.clear();
        turret_rescan.clear();
        strategy.clear();
        tactic.clear();
        active.clear();
        passive.clear();
        order.clear();
    }
};

rec_t g_rec;

void st_map_influence(int32_t p) {
    g_rec.map_influence.push_back(p);
    g_rec.order.push_back("map_influence(" + std::to_string(p) + ")");
}
void st_turret_rescan(uint32_t p) {
    g_rec.turret_rescan.push_back((int32_t)p);
    g_rec.order.push_back("turret_rescan(" + std::to_string(p) + ")");
}
void st_strategy(int32_t p) {
    g_rec.strategy.push_back(p);
    g_rec.order.push_back("strategy(" + std::to_string(p) + ")");
}
void st_tactic(int32_t p) {
    g_rec.tactic.push_back(p);
    g_rec.order.push_back("tactic(" + std::to_string(p) + ")");
}
void st_active(int32_t p) {
    g_rec.active.push_back(p);
    g_rec.order.push_back("active(" + std::to_string(p) + ")");
}
void st_passive(int32_t p) {
    g_rec.passive.push_back(p);
    g_rec.order.push_back("passive(" + std::to_string(p) + ")");
}

const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.recompute_map_influence = &st_map_influence;
        t.turret_threat_rescan    = &st_turret_rescan;
        t.player_tick             = &st_strategy;
        t.ai_unit_group_tick      = &st_tactic;
        t.active_unit_tick        = &st_active;
        t.passive_engage_tick     = &st_passive;
        return t;
    }();
    return c;
}

// A clock value far ahead of anything this file drives through dt, so a player's catch-up loop for
// that clock never enters -- used to PARK a phase away from a case that means to isolate a
// different mechanism.
constexpr float PARK = 1.0e9f;

} // namespace

void run_players_tick_tests() {
    printf("-- ai_players_tick (llm_strat_ai_players_tick) --\n");

    // ---- T0: the map-extent mask re-stamp is UNCONDITIONAL -- not one of the five per-player
    // loops, so it runs even with active_player_count == 0, and active_player_count == 0 itself
    // means NONE of the five loops touch player 0 even though it is set up to fire every one of
    // them (0x004db985 loop-1 bound compare).
    {
        fixture f;
        f.map_w                               = 64;
        f.map_h                               = 48;
        f.active_players                      = 0;
        f.players[0].ai_enabled               = 1;
        f.players[0].ai_map_changed_pending   = 1;
        f.players[0].ai_turret_rescan_pending = 1;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 0.0);
        ck(f.map_wm == 63, "players_tick: map_width_mask is re-stamped to width-1 even with "
                           "active_player_count==0 -- it is not one of the five per-player loops, "
                           "0x004db914-0x004db91a");
        ck(f.map_hm == 47, "players_tick: map_height_mask is re-stamped to height-1 even with "
                           "active_player_count==0, 0x004db91f-0x004db925");
        ck(g_rec.order.empty(),
           "players_tick: active_player_count==0 means none of the five per-player loops run at "
           "all, even though player 0 was armed to fire every one of them (0x004db985 JC)");
    }

    // ---- T1: loop 1's map-influence trigger is a BITWISE AND of ai_map_changed_pending and
    // ai_enabled (0x004db944 MOV/0x004db94a TEST/0x004db950 JZ), not a logical `&&` -- two
    // fields that are both individually nonzero but share no set bit must NOT fire.
    {
        auto run_map_case = [&](int32_t pending, int32_t enabled) {
            fixture f;
            f.active_players                    = 1;
            f.players[0].ai_map_changed_pending = pending;
            f.players[0].ai_enabled             = enabled;
            g_rec.clear();
            // dt==0 and default-zero clocks (ai_clock==ai_clock_s==ai_clock_t==ai_clock_m==0) keep
            // all three catch-up loops from entering (0>=0 is true), isolating loop 1.
            detail::players_tick(f.view(), f.store(), calls(), 0.0);
        };
        run_map_case(/*pending=*/1 /*bit0*/, /*enabled=*/2 /*bit1*/);
        ck(g_rec.map_influence.empty(),
           "players_tick: ai_map_changed_pending=1 (bit0) and ai_enabled=2 (bit1) are both nonzero "
           "but share no bit -- the bitwise AND is 0, so recompute_map_influence must NOT fire; a "
           "logical && translation would fire here (0x004db94a TEST dword[pending],ai_enabled)");
        run_map_case(/*pending=*/1, /*enabled=*/3 /*bits0+1*/);
        ck(g_rec.map_influence == std::vector<int32_t>{0},
           "players_tick: ai_map_changed_pending=1 and ai_enabled=3 share bit0 -- the bitwise AND "
           "is 1, so recompute_map_influence(0) must fire exactly once (0x004db954 call)");
    }

    // ---- T2: loop 1's turret-rescan trigger is the SAME bitwise-AND shape against a DIFFERENT
    // pending field (0x004db96f TEST/0x004db97b JZ/0x004db97f call) -- independent of the map flag.
    {
        auto run_turret_case = [&](int32_t pending, int32_t enabled) {
            fixture f;
            f.active_players                      = 1;
            f.players[0].ai_turret_rescan_pending = pending;
            f.players[0].ai_enabled               = enabled;
            g_rec.clear();
            detail::players_tick(f.view(), f.store(), calls(), 0.0);
        };
        run_turret_case(/*pending=*/2 /*bit1*/, /*enabled=*/1 /*bit0*/);
        ck(g_rec.turret_rescan.empty(),
           "players_tick: ai_turret_rescan_pending=2 and ai_enabled=1 share no bit -- turret_"
           "threat_rescan must NOT fire (0x004db975 TEST dword[pending],ai_enabled)");
        ck(g_rec.map_influence.empty(),
           "players_tick: a turret-only setup must not also fire recompute_map_influence -- the two "
           "triggers read DIFFERENT pending fields (0x004db944 vs 0x004db96f)");
        run_turret_case(/*pending=*/3 /*bits0+1*/, /*enabled=*/1 /*bit0*/);
        ck(g_rec.turret_rescan == std::vector<int32_t>{0},
           "players_tick: ai_turret_rescan_pending=3 and ai_enabled=1 share bit0 -- turret_threat_"
           "rescan(0) must fire exactly once (0x004db97f call, arg is the plain player index cast "
           "uint32_t)");
    }

    // ---- T3: loop 1's per-player call ORDER -- map-influence before turret-rescan for the SAME
    // player (0x004db954 precedes 0x004db97f in program order), and player 0's whole pair completes
    // before player 1's (the two checks are inside one for-iteration, not split across two loops).
    {
        fixture f;
        f.active_players = 2;
        for (int p = 0; p < 2; ++p) {
            f.players[p].ai_enabled               = 1;
            f.players[p].ai_map_changed_pending   = 1;
            f.players[p].ai_turret_rescan_pending = 1;
        }
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 0.0); // dt==0: phases 3-5 stay silent
        const std::vector<std::string> want = {"map_influence(0)", "turret_rescan(0)",
                                               "map_influence(1)", "turret_rescan(1)"};
        ck(g_rec.order == want,
           "players_tick: loop-1 order is map_influence(p) then turret_rescan(p) for EACH player in "
           "turn (0,0,1,1), not both players' map checks before either's turret check -- the two "
           "tests are nested inside one for-iteration (0x004db92e-0x004db98d)");
    }

    // ---- T4: loop 2's ai_clock accumulate is `(float)((double)ai_clock + dt)` -- a genuine
    // double-wide intermediate (FADD double ptr, 0x004db9ac) rounded to float ONCE on the store
    // (0x004db9af) -- NOT `(float)(ai_clock + (float)dt)`, which rounds dt down to float BEFORE the
    // add. The two constants below are checked to actually disagree, so this case cannot pass
    // vacuously if they happened to coincide.
    {
        // 100.0f/0.1 was the first pair tried and the sanity check below REJECTED it: both shapes
        // round to the same float, so the case would have proved nothing. This pair does separate
        // them -- 16777218.0f sits just above 2^24 where the float ULP is 2, and 0.99999999 rounds
        // UP to exactly 1.0f, which turns the second shape's add into a round-half-to-EVEN tie that
        // lands on 16777220.0f while the one-rounding shape stays at 16777218.0f.
        const float  clock0 = 16777218.0f;
        const double dt     = 0.99999999;
        const float  correct =
            static_cast<float>(static_cast<double>(clock0) + dt); // the shape 0x004db9a5-0x004db9af
        const float wrong =
            static_cast<float>(clock0 + static_cast<float>(dt)); // the shape the header rules out
        ck(correct != wrong,
           "players_tick sanity: (16777218.0f, 0.99999999) must make the double-wide accumulate "
           "and the "
           "round-dt-to-float-first accumulate disagree, or the case below proves nothing");

        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 1;
        f.players[0].ai_clock   = clock0;
        f.players[0].ai_clock_s = PARK; // park the three catch-up loops away from this clock value
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), dt);
        ck(f.players[0].ai_clock == correct,
           "players_tick: ai_clock += dt widens ai_clock to double, adds the real double dt, and "
           "rounds to float in ONE step (0x004db9a5 FLD/0x004db9ac FADD double/0x004db9af FSTP) -- "
           "got a result consistent with rounding dt to float first instead if this fails");
    }

    // ---- T5: loop 2 runs for EVERY player regardless of ai_enabled -- there is no gate in the
    // instruction range 0x004db991-0x004db9b7 at all, unlike loops 1/3/4/5.
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 0; // NOT AI-enabled
        f.players[0].ai_clock   = 5.0f;
        f.players[0].ai_clock_s = PARK;
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 2.0);
        ck(f.players[0].ai_clock == 7.0f,
           "players_tick: ai_clock accumulates dt for a non-AI-enabled player exactly as it does "
           "for an AI one -- loop 2 (0x004db991-0x004db9b7) has no ai_enabled gate to skip");
    }

    // ---- T6: the STRATEGY catch-up fires llm_strat_ai_player_tick exactly once when the new clock
    // crosses ai_clock_s by less than two periods, advances ai_clock_s by EXACTLY the period
    // (0x0066931c, fixture's 3.0f), and leaves ai_clock_t / ai_clock_m completely untouched.
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 1;
        f.players[0].ai_clock   = 10.0f;
        f.players[0].ai_clock_s = 9.0f; // one period (3) below the post-dt clock of 11
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0); // new ai_clock = 11.0f
        ck(g_rec.strategy == std::vector<int32_t>{0},
           "players_tick: strategy catch-up (0x004db9d9-0x004db9e8 test, 0x004db9f5 call) fires "
           "llm_strat_ai_player_tick(0) exactly once");
        ck(g_rec.tactic.empty() && g_rec.active.empty() && g_rec.passive.empty(),
           "players_tick: the strategy fire above must not also fire the tactic or move phases -- "
           "their clocks were parked ahead of the new ai_clock");
        ck(f.players[0].ai_clock_s == 12.0f,
           "players_tick: ai_clock_s advances by EXACTLY ai_strategy_period (9+3=12, 0x004dba0e "
           "FLD period/0x004dba1b FSTP) -- a swapped period (e.g. tactic's 5, or move's 7) would "
           "give 14 or 16 here instead");
        ck(f.players[0].ai_clock_t == PARK && f.players[0].ai_clock_m == PARK,
           "players_tick: the strategy loop must not touch ai_clock_t or ai_clock_m -- a "
           "swapped-accumulator bug would advance the wrong player_data field here");
    }

    // ---- T7: the STRATEGY catch-up's period advance is UNCONDITIONAL -- it still happens when
    // ai_enabled==0, even though the call itself (0x004db9f5) is skipped (0x004db9ea CMP/0x004db9f1
    // JZ). A translation that folded the advance inside the `if (ai_enabled)` arm would leave
    // ai_clock_s at 9 here instead of 12.
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 0;
        f.players[0].ai_clock   = 10.0f;
        f.players[0].ai_clock_s = 9.0f;
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0);
        ck(g_rec.strategy.empty(),
           "players_tick: ai_enabled==0 means llm_strat_ai_player_tick must NOT be called "
           "(0x004db9ea CMP dword[ai_enabled],0 / 0x004db9f1 JZ)");
        ck(f.players[0].ai_clock_s == 12.0f,
           "players_tick: ai_clock_s still advances by the period even though the call was gated "
           "off -- the advance (0x004dba0e-0x004dba1b) is OUTSIDE the ai_enabled check, so a gated "
           "player's schedule does not fall permanently behind");
    }

    // ---- T8: the TACTIC catch-up is the identical shape, calling llm_strat_ai_unit_group_tick and
    // advancing by ai_tactic_period (fixture's 5.0f).
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 1;
        f.players[0].ai_clock   = 10.0f;
        f.players[0].ai_clock_s = PARK;
        f.players[0].ai_clock_t = 9.0f; // one period (5) below the post-dt clock of 11
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0);
        ck(g_rec.tactic == std::vector<int32_t>{0},
           "players_tick: tactic catch-up (0x004dba47-0x004dba56 test, 0x004dba63 call) fires "
           "llm_strat_ai_unit_group_tick(0) exactly once");
        ck(g_rec.strategy.empty() && g_rec.active.empty() && g_rec.passive.empty(),
           "players_tick: the tactic fire above must not also fire strategy or move");
        ck(f.players[0].ai_clock_t == 14.0f,
           "players_tick: ai_clock_t advances by EXACTLY ai_tactic_period (9+5=14, 0x004dba7c "
           "FLD period/0x004dba89 FSTP) -- a swapped period would give 12 (strategy's) or 16 "
           "(move's) instead");
    }

    // ---- T9: the TACTIC catch-up's period advance is likewise unconditional on ai_enabled.
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 0;
        f.players[0].ai_clock   = 10.0f;
        f.players[0].ai_clock_s = PARK;
        f.players[0].ai_clock_t = 9.0f;
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0);
        ck(g_rec.tactic.empty(),
           "players_tick: ai_enabled==0 means llm_strat_ai_unit_group_tick must NOT be called "
           "(0x004dba58 CMP/0x004dba5f JZ)");
        ck(f.players[0].ai_clock_t == 14.0f,
           "players_tick: ai_clock_t still advances by the tactic period even though the call was "
           "gated off (0x004dba7c-0x004dba89 sits outside the ai_enabled check)");
    }

    // ---- T10: the MOVE catch-up, ai_enabled!=0 arm -- calls active_unit_tick, NEVER passive_
    // engage_tick, and advances by ai_move_period (fixture's 7.0f).
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 1;
        f.players[0].ai_clock   = 10.0f;
        f.players[0].ai_clock_s = PARK;
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = 9.0f; // one period (7) below the post-dt clock of 11
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0);
        ck(g_rec.active == std::vector<int32_t>{0} && g_rec.passive.empty(),
           "players_tick: move catch-up with ai_enabled!=0 calls llm_strat_ai_active_unit_tick(0) "
           "(0x004dbb00) and never llm_strat_unit_passive_engage_tick (0x004dbaa1)");
        ck(f.players[0].ai_clock_m == 16.0f,
           "players_tick: ai_clock_m advances by EXACTLY ai_move_period (9+7=16, 0x004dbaba FLD "
           "period/0x004dbac7 FSTP)");
    }

    // ---- T11: the MOVE catch-up, ai_enabled==0 arm -- this is NOT a skip like strategy/tactic's
    // gate: it calls passive_engage_tick instead. A naive oracle (or a naive translation) that
    // reused the "if (ai_enabled) call(); " shape with no else would leave BOTH vectors empty here,
    // passing vacuously -- this case is the one that catches that.
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 0;
        f.players[0].ai_clock   = 10.0f;
        f.players[0].ai_clock_s = PARK;
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = 9.0f;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0);
        ck(g_rec.passive == std::vector<int32_t>{0} && g_rec.active.empty(),
           "players_tick: move catch-up with ai_enabled==0 calls llm_strat_unit_passive_engage_"
           "tick(0) (0x004dbaa1) -- exactly one of active/passive ALWAYS fires once the catch-up "
           "condition holds, there is no 'do nothing' arm (0x004dbaf5 CMP/0x004dbafc JZ)");
        ck(f.players[0].ai_clock_m == 16.0f,
           "players_tick: ai_clock_m still advances by the move period on the passive arm too -- "
           "both arms fall into the same advance-then-retest tail");
    }

    // ---- T12: the multi-tick catch-up fire COUNT over several calls -- only the real "threshold
    // += period" shape reproduces this; a "snap the threshold to the current clock on every fire"
    // shape would instead fire once per call (5 fires, not 2) because it never lets debt
    // accumulate across a tick where the threshold was already close enough not to fire.
    {
        fixture f;
        f.active_players        = 1;
        f.players[0].ai_enabled = 1;
        f.players[0].ai_clock   = 0.0f;
        f.players[0].ai_clock_s = 0.0f; // ai_strategy_period == 3.0f (fixture default)
        f.players[0].ai_clock_t = PARK;
        f.players[0].ai_clock_m = PARK;
        g_rec.clear();
        // clock sequence 0->1->2->3->4->5; threshold sequence (fires marked *): 0*->3, 3, 3*->6, 6
        // i.e. fires on tick 1 (0<1) and tick 4 (3<4), not on ticks 2/3/5 (3 is not < 2, 3, or 5<6).
        for (int i = 0; i < 5; ++i) detail::players_tick(f.view(), f.store(), calls(), 1.0);
        ck(g_rec.strategy.size() == 2,
           "players_tick: 5 ticks of dt=1 against ai_strategy_period=3, starting ai_clock_s==0, "
           "fires llm_strat_ai_player_tick exactly TWICE (at clock 1 and clock 4) -- a threshold "
           "that snapped to the current clock on every fire would instead fire on all 5 ticks");
        ck(g_rec.strategy == (std::vector<int32_t>{0, 0}),
           "players_tick: both of those fires are for player 0 (the only active player)");
        ck(f.players[0].ai_clock == 5.0f,
           "players_tick: ai_clock after 5 ticks of dt=1 is 5.0 (loop 2, unconditional every tick)");
        ck(f.players[0].ai_clock_s == 6.0f,
           "players_tick: ai_clock_s ends at 6.0 (0 -> 3 on tick 1's fire, held at 3 through ticks "
           "2-3 since 3 is not < 2 or < 3, -> 6 on tick 4's fire, held at 6 since 6 is not < 5) -- "
           "this exact value is what a 'snap to clock' shape (which would leave it at 5.0) gets "
           "wrong even when the fire COUNT happened to be checked less precisely");
    }

    // ---- T13: active_player_count bounds ALL FIVE loops, re-read fresh each call, with no
    // compiled-in MAX_PLAYERS cap -- exercised on loop 1 (map trigger) and loop 2 (clock accumulate)
    // together so the bound is shown to apply across different loops, not just one.
    {
        fixture f;
        f.active_players = 1; // only player 0 is in range
        for (int p = 0; p < 2; ++p) {
            f.players[p].ai_enabled             = 1;
            f.players[p].ai_map_changed_pending = 1;
            f.players[p].ai_clock               = 5.0f;
            f.players[p].ai_clock_s             = PARK;
            f.players[p].ai_clock_t             = PARK;
            f.players[p].ai_clock_m             = PARK;
        }
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 2.0);
        ck(g_rec.map_influence == std::vector<int32_t>{0},
           "players_tick: active_player_count==1 -- player 0's map trigger fires (0x004db985 JC)");
        ck(f.players[0].ai_clock == 7.0f,
           "players_tick: active_player_count==1 -- player 0's ai_clock accumulates dt "
           "(0x004db9b7 JC, the loop-2 bound)");
        ck(f.players[1].ai_clock == 5.0f,
           "players_tick: active_player_count==1 -- player 1 is OUT OF RANGE and its ai_clock is "
           "left completely untouched, even though it was set up identically to player 0");
    }
    {
        // The fixture's spare 9th slot (index MAX_PLAYERS == 8): active_player_count==8 must NOT
        // reach it, and active_player_count==9 (the fixture's own vector extent) must.
        fixture f;
        f.players[8].ai_enabled             = 1;
        f.players[8].ai_map_changed_pending = 1;

        f.active_players = 8;
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 0.0);
        ck(g_rec.map_influence.empty(),
           "players_tick: active_player_count==8 (MAX_PLAYERS) does not reach index 8 -- the loop "
           "runs p=0..7 (0x004db985 JC), so player 8's armed trigger must not fire");

        f.active_players = 9; // the fixture's spare slot, MAX_PLAYERS+1
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 0.0);
        ck(g_rec.map_influence == std::vector<int32_t>{8},
           "players_tick: active_player_count==9 DOES reach index 8 -- the loop bound has no "
           "compiled-in MAX_PLAYERS cap, it trusts whatever active_player_count says (0x004db985)");
    }

    // ---- T16: the full cross-phase call ORDER across two players, all three catch-up loops firing
    // for both -- pins that the three phase loops are SEPARATE per-loop passes over every player
    // (strategy for p0 then p1, THEN tactic for p0 then p1, THEN move for p0 then p1), not one loop
    // that runs all three phases per player before moving to the next player.
    {
        fixture f;
        f.active_players = 2;
        for (int p = 0; p < 2; ++p) {
            f.players[p].ai_enabled = 1;
            f.players[p].ai_clock   = 10.0f;
            f.players[p].ai_clock_s = 9.0f; // -> fires once each, clock_s becomes 12
            f.players[p].ai_clock_t = 9.0f; // -> fires once each, clock_t becomes 14
            f.players[p].ai_clock_m = 9.0f; // -> fires once each, clock_m becomes 16
        }
        g_rec.clear();
        detail::players_tick(f.view(), f.store(), calls(), 1.0); // new ai_clock = 11.0f for both
        const std::vector<std::string> want = {"strategy(0)", "strategy(1)", "tactic(0)",
                                               "tactic(1)", "active(0)", "active(1)"};
        ck(g_rec.order == want,
           "players_tick: cross-phase order is ALL of strategy, THEN all of tactic, THEN all of "
           "move -- (p0,p1) within each phase, not (strategy,tactic,move) interleaved per player -- "
           "loops 3/4/5 are three separate for-loops over active_player_count (0x004db9c3/"
           "0x004dba31/0x004dba9f), not one merged loop");
        ck(f.players[0].ai_clock_s == 12.0f && f.players[1].ai_clock_s == 12.0f &&
               f.players[0].ai_clock_t == 14.0f && f.players[1].ai_clock_t == 14.0f &&
               f.players[0].ai_clock_m == 16.0f && f.players[1].ai_clock_m == 16.0f,
           "players_tick: both players' three clocks land on their own period's post-fire value -- "
           "rules out a translation that shared one threshold across players");
    }
}

} // namespace mh::ai::test
