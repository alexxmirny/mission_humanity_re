//
// sim_step_selftest.cpp -- `simtest` offline oracle for llm_strat_sim_step (sim/sim_step.{h,cpp},
// RI-SIM, the domain ROOT and the LAST function of the whole SIM migration). sim_step is the
// fixed-timestep pump: two head gates, a per-player loop (stat rollover + two sub-tick catch-up
// loops + a per-second production scan + per-unit / per-building tick loops), then two whole-pool
// walks (projectiles, fx-anims) OUTSIDE the per-player loop.
//
// It has a 686-function write closure, yet is fully OFFLINE-VERIFIABLE here because detail::sim_step
// reaches all twelve sub-tick callees through a `sim_step_calls` struct. We STUB all twelve with
// RECORDING stubs; sim_step's OWN orchestration + bookkeeping (the loop bounds, the stat rollover,
// the two catch-up loops, the per-second scan, the cur_* ambient-pointer marshalling, the whole-pool
// walks) is then bounded and testable over the fixture. The twelve callees have their own oracles /
// are original -- we verify only that sim_step calls them the right number of times, with the right
// args, in the right order, and does its own latch/zero/advance math correctly.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_strat_sim_step_0043f512.asm),
// not the .cpp. Key derived offsets used below (all confirmed against the .asm operands):
//   * ALIVE gate: profiles[player].status_flags & 0x2 (TEST byte [player*0x740+0xcff060],0x2).
//   * stat rollover latches pop.housing_prev=housing_accum, hs.cap_prev_*=cap_accum_*,
//     ss.cap_prev[i]=cap_accum[i]; the tick-end reset then ZEROES every one of those accumulators
//     (so post-call: *_prev == seeded accum, accum == 0).
//   * power_recompute fires iff generated!=prev_generated OR consumed!=prev_consumed.
//   * pop.pop_total = trunc-toward-zero(pop.pop_fraction).
//   * sub-tick A body: check_population_change(player); subtick_a_clock += A_PERIOD_POS;
//     delta_a += A_PERIOD_NEG; then the dead-docked purge scan. Loop runs while A_PERIOD <= delta_a.
//   * sub-tick B body: subtick_b_clock += B_PERIOD_POS; check_storage_overflow(player);
//     delta_b += B_PERIOD_NEG. Loop runs while B_PERIOD <= delta_b.
//   * per-second scan (gated on sim_active): while !(1.0 > delta_p) { for slot 1..9: if
//     prod_shuttle_slots[player*10+slot].status==200 -> production_complete(player,slot,delta_p);
//     prof.prod_check_clock += 1.0; delta_p += PROD_CHECK_PERIOD_NEG }. Threshold 1.0 is a LITERAL.
//   * per-unit loop: units[player][1..] bounded by units[player][0].unit_above read as a WORD; per
//     slot, if unit_proto_id!=0 -> unit_tick(); then game_SetEvent(6) if change_flag||change_flag2.
//   * per-building tick loop: buildings[player][1..] bounded by buildings[player][0].index; per slot,
//     if building_id!=0 -> building_tick() (NO online_state gate here); then game_SetEvent(8) if dirty.
//   * projectile walk: pool[1..] bounded by pool[0].active; per slot if active!=0 -> projectile_tick().
//   * fx-anim walk: pool[1..] bounded by pool[0].live; per slot if live!=0 -> fx_anim_tick().
//
// OFFLINE-SCOPE NOTE (from the .cpp's own OPEN comment): the cur_player handling is covered in the
// WRITE-by-sim_step / read-back direction only (each iteration sim_step writes set_cur_player(player)
// and we read it back through the ambient slot inside a per-player stub). Whether an ORIGINAL callee
// WRITES cur_player as a side effect is NOT observable offline with stubs -- we do NOT fake that; it
// is out of offline scope and belongs to a live-game arm.
//
// The seven sub-tick boot doubles are seeded DISTINCT and non-symmetric (period != period_pos !=
// |period_neg|, and the A-family != the B-family) so a translation that read the wrong one of the
// three in a given spot, or swapped the A/B constants, disagrees here even though the shipped game
// ships them symmetric.
//
#include "sim/sim_step.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recording stubs for the twelve sim_step_calls callees --------------------------------------
struct Purge {
    int32_t player, sub_id;
};
struct Prod {
    uint32_t player, slot;
    double   elapsed;
};
struct Tick {
    uint16_t player, index;
    int32_t  slot_off; // cur_*_ptr - roster.data()  (== player*PER_PLAYER + slot)
};

struct Rec {
    int                   ai_calls    = 0;
    double                ai_dt       = 0.0;
    int                   order_calls = 0;
    std::vector<uint16_t> power_players;
    std::vector<uint32_t> pop_players;      // check_population_change arg
    std::vector<uint16_t> pop_view_players; // ambient cur_player read back at call time
    std::vector<Purge>    purges;
    std::vector<int32_t>  overflow_players;
    std::vector<Prod>     prods;
    std::vector<Tick>     unit_ticks;
    std::vector<Tick>     bldg_ticks;
    std::vector<int32_t>  proj_ticks; // slot offset in projectile_pool
    std::vector<int32_t>  fx_ticks;   // slot offset in fx_anim_pool
    std::vector<uint32_t> events;
};

Rec          g;
sim_fixture *g_fx              = nullptr;
bool         g_unit_tick_dirty = false;
bool         g_bldg_tick_dirty = false;

void s_ai(double dt) {
    ++g.ai_calls;
    g.ai_dt = dt;
}
void s_order() { ++g.order_calls; }
void s_power(uint16_t p) { g.power_players.push_back(p); }
void s_pop(uint32_t p) {
    g.pop_players.push_back(p);
    g.pop_view_players.push_back(g_fx->view_cur_player); // set_cur_player() read-back
}
void s_purge(int32_t p, int32_t sub) { g.purges.push_back({p, sub}); }
void s_overflow(int32_t p) { g.overflow_players.push_back(p); }
void s_prod(uint32_t p, uint32_t slot, double e) { g.prods.push_back({p, slot, e}); }
void s_unit_tick() {
    Tick t;
    t.player   = g_fx->view_cur_player;
    t.index    = g_fx->view_cur_index;
    t.slot_off = (int32_t)(g_fx->cur_unit_ptr - g_fx->units.data());
    g.unit_ticks.push_back(t);
    if (g_unit_tick_dirty) g_fx->geom.change_flag = 1;
}
void s_bldg_tick() {
    Tick t;
    t.player   = g_fx->view_cur_player;
    t.index    = g_fx->view_cur_index;
    t.slot_off = (int32_t)(g_fx->cur_building_ptr - g_fx->buildings.data());
    g.bldg_ticks.push_back(t);
    if (g_bldg_tick_dirty) g_fx->geom.change_flag = 1;
}
void s_proj_tick() {
    g.proj_ticks.push_back((int32_t)(g_fx->cur_projectile_ptr - g_fx->projectile_pool.data()));
}
void s_fx_tick() {
    g.fx_ticks.push_back((int32_t)(g_fx->cur_fx_anim_ptr - g_fx->fx_anim_pool.data()));
}
uint32_t s_event(uint32_t type) {
    g.events.push_back(type);
    return 0;
}

const sim_step_calls g_calls = {s_ai, s_order, s_power, s_pop,
                                s_purge, s_overflow, s_prod, s_unit_tick,
                                s_bldg_tick, s_proj_tick, s_fx_tick, s_event};

void run(sim_fixture &fx, bool unit_dirty = false, bool bldg_dirty = false) {
    g                 = Rec{};
    g_fx              = &fx;
    g_unit_tick_dirty = unit_dirty;
    g_bldg_tick_dirty = bldg_dirty;
    sim_store own     = fx.store();
    detail::sim_step(fx.view(), own, g_calls);
}

// Make one player alive (ALIVE = status_flags bit 0x2). All other players stay status_flags==0 and
// are skipped by the per-player loop body.
void alive(sim_fixture &fx, int p) { fx.profiles[(size_t)p].status_flags = 0x2u; }

// Distinct, non-symmetric sub-tick constants: period != pos != |neg|, and A-family != B-family, so a
// wrong-pointer or A/B swap disagrees here. neg magnitude == period (clean pass counting) but a
// period<->neg swap flips the loop's sign and diverges structurally regardless.
void seed_subtick_constants(sim_fixture &fx) {
    fx.subtick_a_period      = 5.0;
    fx.subtick_a_period_pos  = 3.0;
    fx.subtick_a_period_neg  = -5.0;
    fx.subtick_b_period      = 6.0;
    fx.subtick_b_period_pos  = 4.0;
    fx.subtick_b_period_neg  = -6.0;
    fx.prod_check_period_neg = -1.0;
}

} // namespace

void run_sim_step_tests() {
    sim_fixture fx;

    // =============================================================================================
    // 1. HEAD GATES (players all dead so only the head gates + the empty trailing pool walks run).
    // =============================================================================================
    // HG1: ai_enabled != 0 -> ai_players_tick(game_time_delta); order_count == 0 -> no dispatch.
    fx.reset();
    fx.ai_enabled        = 1;
    fx.game_time_delta   = 0.25; // exactly representable; forwarded as the dt arg
    fx.order_queue_count = 0;
    run(fx);
    ck_eq((uint32_t)g.ai_calls, 1u, "HG1: ai_players_tick fired once (ai_enabled!=0)");
    ck_eq_d(g.ai_dt, 0.25, "HG1: ai_players_tick got dt = game_time_delta (0.25)");
    ck_eq((uint32_t)g.order_calls, 0u, "HG1: order_queue_dispatch NOT fired (order_count==0)");
    ck_eq((uint32_t)fx.view_cur_player, 8u, "HG1: cur_player left at MAX_PLAYERS(8) after loop");

    // HG2: ai_enabled == 0 -> no ai tick; order_count != 0 -> dispatch fires.
    fx.reset();
    fx.ai_enabled        = 0;
    fx.order_queue_count = 5;
    run(fx);
    ck_eq((uint32_t)g.ai_calls, 0u, "HG2: ai_players_tick NOT fired (ai_enabled==0)");
    ck_eq((uint32_t)g.order_calls, 1u, "HG2: order_queue_dispatch fired (order_count!=0)");

    // HG3: both gates closed.
    fx.reset();
    run(fx);
    ck_eq((uint32_t)g.ai_calls, 0u, "HG3: no ai tick");
    ck_eq((uint32_t)g.order_calls, 0u, "HG3: no dispatch");
    ck(g.proj_ticks.empty() && g.fx_ticks.empty(), "HG3: empty pools -> no projectile/fx ticks");

    // =============================================================================================
    // 2. PER-PLAYER LOOP: alive gate, set_cur_player read-back, power_recompute dispatch.
    // =============================================================================================
    // PP1: only player 3 alive. Seed a dead player 4 with differing power stats to prove a NOT-ALIVE
    // slot takes no body (power_recompute must NOT fire for 4). Drive sub-tick A one pass so a
    // per-player stub can read cur_player back.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 5.0;
    fx.population[3].subtick_a_clock         = 0.0; // delta_a = 5 == period -> exactly 1 pass
    fx.storage_stats_rows[3].subtick_b_clock = 5.0; // delta_b = 0 -> no sub-tick B
    fx.power_stats_rows[3].generated         = 5;   // differs from prev_generated(0) -> recompute
    fx.power_stats_rows[3].prev_generated    = 2;
    fx.power_stats_rows[4].generated         = 9; // player 4 is DEAD; must be ignored
    fx.power_stats_rows[4].prev_generated    = 1;
    run(fx);
    ck(g.power_players.size() == 1 && g.power_players[0] == 3,
       "PP1: power_recompute fired for player 3 only (dead player 4 skipped)");
    ck(g.pop_players.size() == 1 && g.pop_players[0] == 3,
       "PP1: check_population_change(player=3) once (sub-tick A one pass)");
    ck(g.pop_view_players.size() == 1 && g.pop_view_players[0] == 3,
       "PP1: ambient cur_player read back == 3 (set_cur_player wrote the slot)");
    ck_eq_d(fx.population[3].subtick_a_clock, 3.0,
            "PP1: subtick_a_clock advanced by A_PERIOD_POS(3), not period(5)");
    ck(g.overflow_players.empty(), "PP1: no sub-tick B (delta_b==0)");
    ck_eq((uint32_t)fx.view_cur_player, 8u, "PP1: cur_player == 8 after loop");

    // PP2a: power_recompute does NOT fire when generated==prev AND consumed==prev.
    fx.reset();
    alive(fx, 2);
    fx.power_stats_rows[2].generated      = 7;
    fx.power_stats_rows[2].prev_generated = 7;
    fx.power_stats_rows[2].consumed       = 4;
    fx.power_stats_rows[2].prev_consumed  = 4;
    run(fx);
    ck(g.power_players.empty(), "PP2a: no power_recompute (generated==prev && consumed==prev)");

    // PP2b: fires on the OR's SECOND operand alone (generated==prev, consumed!=prev).
    fx.reset();
    alive(fx, 2);
    fx.power_stats_rows[2].generated      = 7;
    fx.power_stats_rows[2].prev_generated = 7;
    fx.power_stats_rows[2].consumed       = 4;
    fx.power_stats_rows[2].prev_consumed  = 9; // differs
    run(fx);
    ck(g.power_players.size() == 1 && g.power_players[0] == 2,
       "PP2b: power_recompute fires when only consumed differs");

    // MULTI: players 1 and 4 alive (non-adjacent), each takes one sub-tick A pass -> per-player
    // dispatch order + set_cur_player tracking across players. Player 1 power differs, player 4 not.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 1);
    alive(fx, 4);
    fx.game_clock                            = 5.0;
    fx.population[1].subtick_a_clock         = 0.0;
    fx.population[4].subtick_a_clock         = 0.0;
    fx.storage_stats_rows[1].subtick_b_clock = 5.0;
    fx.storage_stats_rows[4].subtick_b_clock = 5.0;
    fx.power_stats_rows[1].generated         = 3; // differs -> recompute
    fx.power_stats_rows[1].prev_generated    = 1;
    fx.power_stats_rows[4].generated         = 8; // equal -> no recompute
    fx.power_stats_rows[4].prev_generated    = 8;
    run(fx);
    ck(g.pop_players.size() == 2 && g.pop_players[0] == 1 && g.pop_players[1] == 4,
       "MULTI: check_population_change visited players 1 then 4 in order");
    ck(g.pop_view_players.size() == 2 && g.pop_view_players[0] == 1 && g.pop_view_players[1] == 4,
       "MULTI: ambient cur_player tracked each alive player (1,4)");
    ck(g.power_players.size() == 1 && g.power_players[0] == 1,
       "MULTI: power_recompute only for player 1 (player 4 stats equal)");

    // =============================================================================================
    // 3. STAT ROLLOVER + tick-end reset + pop_total trunc.
    // =============================================================================================
    // SR1: latch *_prev = accum, then zero every accumulator at tick end. No sub-ticks (delta==0).
    fx.reset();
    alive(fx, 3);
    fx.game_clock = 0.0;
    // pop-stats accumulators
    fx.population[3].housing_accum     = 41;
    fx.population[3].housing_prev      = 99; // must be OVERWRITTEN to 41
    fx.population[3].colony_hp_sum     = 13;
    fx.population[3].colony_hp_max_sum = 17;
    fx.population[3].pop_fraction      = 7.5; // -> pop_total 7
    // housing-cap accumulators (distinct so a mis-mapped field is caught)
    fx.unit_housing[3].cap_accum_vehicles = 11;
    fx.unit_housing[3].cap_accum_soldiers = 22;
    fx.unit_housing[3].cap_accum_planes   = 33;
    fx.unit_housing[3].cap_accum_helis    = 44;
    // storage-cap accumulators (distinct per slot)
    for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) fx.storage_stats_rows[3].cap_accum[i] = 100 + i;
    fx.population[3].subtick_a_clock         = 0.0; // delta_a = 0 -> no sub-tick A
    fx.storage_stats_rows[3].subtick_b_clock = 0.0; // delta_b = 0 -> no sub-tick B
    run(fx);
    ck_eq((uint32_t)fx.population[3].housing_prev, 41u, "SR1: housing_prev latched from accum(41)");
    ck_eq((uint32_t)fx.population[3].housing_accum, 0u, "SR1: housing_accum zeroed at tick end");
    ck_eq((uint32_t)fx.population[3].colony_hp_sum, 0u, "SR1: colony_hp_sum zeroed");
    ck_eq((uint32_t)fx.population[3].colony_hp_max_sum, 0u, "SR1: colony_hp_max_sum zeroed");
    ck_eq((uint32_t)fx.population[3].pop_total, 7u, "SR1: pop_total = trunc(pop_fraction 7.5) = 7");
    ck_eq((uint32_t)fx.unit_housing[3].cap_prev_vehicles, 11u, "SR1: cap_prev_vehicles=accum(11)");
    ck_eq((uint32_t)fx.unit_housing[3].cap_prev_soldiers, 22u, "SR1: cap_prev_soldiers=accum(22)");
    ck_eq((uint32_t)fx.unit_housing[3].cap_prev_planes, 33u, "SR1: cap_prev_planes=accum(33)");
    ck_eq((uint32_t)fx.unit_housing[3].cap_prev_helis, 44u, "SR1: cap_prev_helis=accum(44)");
    ck_eq((uint32_t)fx.unit_housing[3].cap_accum_vehicles, 0u, "SR1: cap_accum_vehicles zeroed");
    ck_eq((uint32_t)fx.unit_housing[3].cap_accum_helis, 0u, "SR1: cap_accum_helis zeroed");
    {
        bool prev_ok = true, accum_ok = true;
        for (int32_t i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) {
            if (fx.storage_stats_rows[3].cap_prev[i] != 100 + i) prev_ok = false;
            if (fx.storage_stats_rows[3].cap_accum[i] != 0) accum_ok = false;
        }
        ck(prev_ok, "SR1: storage cap_prev[i] latched from cap_accum[i] (100+i)");
        ck(accum_ok, "SR1: storage cap_accum[i] all zeroed at tick end");
    }

    // SR2: pop_total truncates TOWARD ZERO for a negative fraction (-7.5 -> -7, not -8).
    fx.reset();
    alive(fx, 3);
    fx.population[3].pop_fraction = -7.5;
    run(fx);
    ck_eq((uint32_t)fx.population[3].pop_total, (uint32_t)(-7), "SR2: trunc(-7.5) = -7 (toward zero)");

    // =============================================================================================
    // 4. SUB-TICK A catch-up: zero / one / multiple passes.
    // =============================================================================================
    // STA0: delta_a < period -> zero passes.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 3.0;
    fx.population[3].subtick_a_clock         = 0.0; // delta_a = 3 < 5
    fx.storage_stats_rows[3].subtick_b_clock = 3.0; // no B
    run(fx);
    ck(g.pop_players.empty(), "STA0: sub-tick A zero passes (delta_a<period)");
    ck_eq_d(fx.population[3].subtick_a_clock, 0.0, "STA0: subtick_a_clock unchanged");

    // STA_MULTI: overshoot by >1 interval -> 3 passes (delta 15, period 5). Assert callee count and
    // the POS-advance of the clock (3 passes * pos 3 = 9).
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 15.0;
    fx.population[3].subtick_a_clock         = 0.0;  // delta_a = 15 -> passes at 15,10,5
    fx.storage_stats_rows[3].subtick_b_clock = 15.0; // no B
    run(fx);
    ck(g.pop_players.size() == 3 && g.pop_players[0] == 3 && g.pop_players[1] == 3 &&
           g.pop_players[2] == 3,
       "STA_MULTI: check_population_change fired 3x for player 3");
    ck_eq_d(fx.population[3].subtick_a_clock, 9.0,
            "STA_MULTI: subtick_a_clock advanced 3*POS(3)=9");

    // =============================================================================================
    // 5. SUB-TICK B catch-up (independent of A; DIFFERENT constants + callee).
    // =============================================================================================
    // STB_MULTI: delta_b 18, period 6 -> 3 passes; clock advances 3*POS(4)=12; A suppressed.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 18.0;
    fx.population[3].subtick_a_clock         = 18.0; // delta_a = 0 -> no A
    fx.storage_stats_rows[3].subtick_b_clock = 0.0;  // delta_b = 18 -> passes at 18,12,6
    run(fx);
    ck(g.overflow_players.size() == 3 && g.overflow_players[0] == 3 && g.overflow_players[2] == 3,
       "STB_MULTI: check_storage_overflow fired 3x for player 3");
    ck(g.pop_players.empty(), "STB_MULTI: sub-tick A suppressed (delta_a==0)");
    ck_eq_d(fx.storage_stats_rows[3].subtick_b_clock, 12.0,
            "STB_MULTI: subtick_b_clock advanced 3*B_POS(4)=12");

    // STB0: delta_b < period -> zero passes.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 18.0;
    fx.population[3].subtick_a_clock         = 18.0;
    fx.storage_stats_rows[3].subtick_b_clock = 15.0; // delta_b = 3 < 6
    run(fx);
    ck(g.overflow_players.empty(), "STB0: sub-tick B zero passes (delta_b<period)");

    // =============================================================================================
    // 6. DEAD-DOCKED PURGE SCAN (inside sub-tick A; buildings[player][1..] walk).
    // =============================================================================================
    // PURGE1: one sub-tick A pass. Cover: a building_id==0 slot is skipped WITHOUT decrementing the
    // sentinel count; two in-set types (one H-family 0x21, one A-family 0x08) fire with the correct
    // sub_id + player; the online_state gate is respected.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 5.0;
    fx.population[3].subtick_a_clock         = 0.0; // 1 sub-tick A pass
    fx.storage_stats_rows[3].subtick_b_clock = 5.0; // no B
    fx.b(3, 0).index                         = 2;   // sentinel: 2 real (building_id!=0) slots to consume
    fx.b(3, 1).building_id                   = 0;   // skipped, does NOT decrement remaining
    fx.b(3, 2).building_id                   = 10;
    fx.b(3, 2).online_state                  = 1;
    fx.b(3, 2).sub_id                        = 0x33;
    fx.cfg_buildings[10].type                = 0x21; // SHUTTLE_H, in set
    fx.b(3, 3).building_id                   = 11;
    fx.b(3, 3).online_state                  = 1;
    fx.b(3, 3).sub_id                        = 0x44;
    fx.cfg_buildings[11].type                = 0x08; // GARAGE_A, in set
    run(fx);
    ck(g.purges.size() == 2, "PURGE1: two storage_purge_dead_docked calls (id==0 slot skipped)");
    ck(g.purges.size() == 2 && g.purges[0].player == 3 && g.purges[0].sub_id == 0x33,
       "PURGE1: first purge (player 3, sub_id 0x33, type 0x21 SHUTTLE_H)");
    ck(g.purges.size() == 2 && g.purges[1].player == 3 && g.purges[1].sub_id == 0x44,
       "PURGE1: second purge (player 3, sub_id 0x44, type 0x08 GARAGE_A)");

    // PURGE_NEG: online building with an OUT-of-set type does not fire; an offline (online_state==0)
    // building of an IN-set type does not fire either.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.game_clock                            = 5.0;
    fx.population[3].subtick_a_clock         = 0.0;
    fx.storage_stats_rows[3].subtick_b_clock = 5.0;
    fx.b(3, 0).index                         = 2;
    fx.b(3, 1).building_id                   = 20;
    fx.b(3, 1).online_state                  = 1;
    fx.cfg_buildings[20].type                = 0x1f; // between 0x1e and 0x20: NOT in set
    fx.b(3, 2).building_id                   = 21;
    fx.b(3, 2).online_state                  = 0;    // offline -> gated out even though type is in set
    fx.cfg_buildings[21].type                = 0x1c; // GARAGE_H (in set) but online_state==0
    run(fx);
    ck(g.purges.empty(), "PURGE_NEG: out-of-set type + offline in-set type both skip the purge");

    // =============================================================================================
    // 7. PER-SECOND PRODUCTION SCAN (gated on sim_active; delta-vs-literal-1.0).
    // =============================================================================================
    // PROD1: sim_active=1, delta_p=1.0 -> exactly one pass. Slots 4 and 7 have status 200; slot 6
    // does not. production_complete fires for 4 and 7 with elapsed==delta_p(1.0). prod_check_clock
    // advances by the LITERAL 1.0.
    fx.reset();
    seed_subtick_constants(fx);
    alive(fx, 3);
    fx.sim_active                            = 1;
    fx.game_clock                            = 1.0;
    fx.profiles[3].prod_check_clock          = 0.0; // delta_p = 1.0
    fx.population[3].subtick_a_clock         = 1.0; // no A
    fx.storage_stats_rows[3].subtick_b_clock = 1.0; // no B
    fx.prod_shuttle_slots[3 * 10 + 4].status = 200;
    fx.prod_shuttle_slots[3 * 10 + 6].status = 100; // not ready -> skipped
    fx.prod_shuttle_slots[3 * 10 + 7].status = 200;
    run(fx);
    ck(g.prods.size() == 2, "PROD1: production_complete fired for the two status==200 slots");
    ck(g.prods.size() == 2 && g.prods[0].player == 3 && g.prods[0].slot == 4,
       "PROD1: first production_complete(player=3, slot=4)");
    ck(g.prods.size() == 2 && g.prods[1].slot == 7, "PROD1: second at slot 7 (slot 6 skipped)");
    ck(g.prods.size() == 2 && g.prods[0].elapsed == 1.0 && g.prods[1].elapsed == 1.0,
       "PROD1: elapsed arg == delta_p (1.0) on the pass");
    ck_eq_d(fx.profiles[3].prod_check_clock, 1.0, "PROD1: prod_check_clock advanced by literal 1.0");

    // PROD0a: delta_p < 1.0 -> zero passes (no production_complete) even with a ready slot.
    fx.reset();
    alive(fx, 3);
    fx.sim_active                            = 1;
    fx.game_clock                            = 0.5;
    fx.profiles[3].prod_check_clock          = 0.0; // delta_p = 0.5 < 1.0
    fx.prod_shuttle_slots[3 * 10 + 4].status = 200;
    run(fx);
    ck(g.prods.empty(), "PROD0a: no production scan when delta_p < 1.0");

    // PROD0b: sim_active == 0 -> the whole scan is gated off regardless of delta_p.
    fx.reset();
    alive(fx, 3);
    fx.sim_active                            = 0;
    fx.game_clock                            = 100.0;
    fx.profiles[3].prod_check_clock          = 0.0;   // huge delta, but gated
    fx.population[3].subtick_a_clock         = 100.0; // suppress sub-tick A
    fx.storage_stats_rows[3].subtick_b_clock = 100.0; // suppress sub-tick B
    fx.prod_shuttle_slots[3 * 10 + 4].status = 200;
    run(fx);
    ck(g.prods.empty(), "PROD0b: production scan gated off when sim_active==0");

    // =============================================================================================
    // 8. PER-UNIT TICK LOOP + game_SetEvent(6=EVENT_INFO_REFRESH).
    // =============================================================================================
    // UNIT1: units[3][1..] bounded by units[3][0].unit_above (word). Slot 2 has proto==0 -> skipped
    // WITHOUT decrement. cur_unit_ptr + cur_index marshalled correctly. No dirty -> no set_event(6).
    fx.reset();
    alive(fx, 3);
    fx.u(3, 0).unit_above[0] = 3; // sentinel count = 3 live units to consume (high byte 0)
    fx.u(3, 0).unit_above[1] = 0;
    fx.u(3, 1).unit_proto_id = 50;
    fx.u(3, 2).unit_proto_id = 0; // skipped, no decrement
    fx.u(3, 3).unit_proto_id = 51;
    fx.u(3, 4).unit_proto_id = 52;
    run(fx, /*unit_dirty*/ false);
    ck(g.unit_ticks.size() == 3, "UNIT1: unit_tick fired for the 3 live units (slot 2 skipped)");
    ck(g.unit_ticks.size() == 3 && g.unit_ticks[0].index == 1 && g.unit_ticks[1].index == 3 &&
           g.unit_ticks[2].index == 4,
       "UNIT1: cur_index marshalled to slots 1,3,4");
    ck(g.unit_ticks.size() == 3 && g.unit_ticks[0].slot_off == 301 &&
           g.unit_ticks[1].slot_off == 303 && g.unit_ticks[2].slot_off == 304,
       "UNIT1: cur_unit_ptr -> units[3][1/3/4] (offsets 301,303,304)");
    ck(g.unit_ticks.size() == 3 && g.unit_ticks[0].player == 3,
       "UNIT1: cur_player is 3 during the unit ticks");
    ck(g.events.empty(), "UNIT1: no game_SetEvent (change_flag stayed 0)");

    // UNIT_EVENT: a unit_tick that dirties change_flag -> game_SetEvent(6) after the unit loop.
    fx.reset();
    alive(fx, 3);
    fx.u(3, 0).unit_above[0] = 1;
    fx.u(3, 1).unit_proto_id = 50;
    run(fx, /*unit_dirty*/ true);
    ck(g.unit_ticks.size() == 1, "UNIT_EVENT: one unit ticked");
    ck(g.events.size() == 1 && g.events[0] == 6,
       "UNIT_EVENT: game_SetEvent(6) fired after per-unit loop on dirty change_flag");

    // =============================================================================================
    // 9. PER-BUILDING TICK LOOP + game_SetEvent(8=BUILD_UNITS_REFRESH). NO online_state gate here.
    // =============================================================================================
    // BLDG1: buildings[3][1..] bounded by buildings[3][0].index. Slot 2 building_id==0 -> skipped
    // without decrement. A ticked building with online_state==0 STILL ticks (proving no online gate).
    // Dirty -> game_SetEvent(8).
    fx.reset();
    alive(fx, 3);
    fx.b(3, 0).index        = 2;
    fx.b(3, 1).building_id  = 10;
    fx.b(3, 1).online_state = 0; // online_state 0 must NOT gate building_tick
    fx.b(3, 2).building_id  = 0; // skipped, no decrement
    fx.b(3, 3).building_id  = 11;
    run(fx, /*unit_dirty*/ false, /*bldg_dirty*/ true);
    ck(g.bldg_ticks.size() == 2, "BLDG1: building_tick fired for 2 buildings (no online gate)");
    ck(g.bldg_ticks.size() == 2 && g.bldg_ticks[0].index == 1 && g.bldg_ticks[1].index == 3,
       "BLDG1: cur_index marshalled to building slots 1,3");
    ck(g.bldg_ticks.size() == 2 && g.bldg_ticks[0].slot_off == 301 &&
           g.bldg_ticks[1].slot_off == 303,
       "BLDG1: cur_building_ptr -> buildings[3][1/3] (offsets 301,303)");
    ck(g.events.size() == 1 && g.events[0] == 8,
       "BLDG1: game_SetEvent(8) fired after per-building loop on dirty change_flag");

    // BLDG_NODIRTY: building ticks but change_flag stays 0 -> no game_SetEvent(8).
    fx.reset();
    alive(fx, 3);
    fx.b(3, 0).index       = 1;
    fx.b(3, 1).building_id = 10;
    run(fx, /*unit_dirty*/ false, /*bldg_dirty*/ false);
    ck(g.bldg_ticks.size() == 1, "BLDG_NODIRTY: one building ticked");
    ck(g.events.empty(), "BLDG_NODIRTY: no game_SetEvent when change_flag stayed 0");

    // =============================================================================================
    // 10. WHOLE-POOL projectile + fx-anim walks (OUTSIDE the per-player loop -- run once total).
    // =============================================================================================
    // POOLS: seed both pools with an interspersed dead slot so the "skip inactive, don't decrement"
    // shape is exercised. No player alive: the walks still run. projectile: pool[0].active=3 with live
    // slots 1,2,4 (slot 3 inactive) -> ticks 1,2,4. fx: pool[0].live=2 with live slots 1,3 (slot 2
    // dead) -> ticks 1,3. cur_*_ptr offsets == slot index.
    fx.reset();
    fx.projectile_pool[0].active = 3;
    fx.projectile_pool[1].active = 1;
    fx.projectile_pool[2].active = 1;
    fx.projectile_pool[3].active = 0; // skipped, no decrement
    fx.projectile_pool[4].active = 1;
    fx.fx_anim_pool[0].live      = 2;
    fx.fx_anim_pool[1].live      = 1;
    fx.fx_anim_pool[2].live      = 0; // skipped
    fx.fx_anim_pool[3].live      = 1;
    run(fx);
    ck(g.proj_ticks.size() == 3 && g.proj_ticks[0] == 1 && g.proj_ticks[1] == 2 &&
           g.proj_ticks[2] == 4,
       "POOLS: projectile_tick at live slots 1,2,4 (slot 3 skipped); cur_projectile marshalled");
    ck(g.fx_ticks.size() == 2 && g.fx_ticks[0] == 1 && g.fx_ticks[1] == 3,
       "POOLS: fx_anim_tick at live slots 1,3 (slot 2 skipped); cur_fx_anim marshalled");

    // -----------------------------------------------------------------------------------------------
    // MUTATION NOTES (which one-line perturbation of sim_step.cpp each check catches red):
    //   * HG1 "dt = game_time_delta (0.25)": swap the head gate to forward some other double (e.g.
    //     *v.game_clock) -> ai_dt != 0.25.
    //   * PP1 "subtick_a_clock advanced by A_PERIOD_POS(3), not period(5)": change
    //     `pop.subtick_a_clock += *v.subtick_a_period_pos` to `+= *v.subtick_a_period` -> clock == 5.
    //   * SR1 "cap_prev[i] latched from cap_accum[i]" + "cap_accum[i] zeroed": drop either the latch
    //     loop or the tick-end zero loop -> cap_prev stays 0 / cap_accum stays 100+i.
    //   * PURGE1 "id==0 slot skipped without decrement": move `--remaining` outside the
    //     `if (b.building_id != 0)` guard -> the run consumes count too early, the 2nd purge is lost.
    //   * BLDG1 "no online gate": add an `if (b.online_state != 0)` guard to the per-building loop
    //     (copying the purge scan's gate) -> the online_state==0 building stops ticking, size==1.
    //   * POOLS "projectile_tick at 1,2,4 (slot 3 skipped)": decrement remaining unconditionally
    //     (outside `if (pr.active != 0)`) -> the walk stops early and slot 4 is never ticked.
    // -----------------------------------------------------------------------------------------------
}

} // namespace mh::sim::test
