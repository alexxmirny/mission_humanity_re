//
// sim_planet_transition_finalize_selftest.cpp -- `simtest` oracle for
// llm_strat_planet_transition_finalize @0x0044d160 (sim/resid/sim_planet_transition_finalize.h/.cpp,
// RI-SIM sim_resid batch A/B).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle (net_selftest simtest) is the ONLY
// verification. See the module header banner for the full derivation; this file re-derives nothing,
// it only pins it.
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_planet_transition_finalize_0044d160.asm) -- the .asm is the spec,
// never the .c beside it:
//   0x0044d178: cam_pan_target_col forced to -1 unconditionally.
//   0x0044d182-0x0044d196: FOUR unconditional frontier calls, in order: time_resync_and_tick,
//     invasion_due_check, invasion_alert_clear(G_PLANET_INDEX), prod_deliver_arrivals.
//     G_PLANET_INDEX for the third call is a FRESH read at 0x0044d18c (after invasion_due_check has
//     already run), not a value cached at function entry.
//   0x0044d19b: _G_LLM_STRAT_SIM_ACTIVE forced to 1, unconditionally.
//   0x0044d1a5-0x0044d1da: the THREE-deep deploy_starting_squad gate -- outer
//     planet_transition_state==0 (0x0044d1a5/0x0044d1ac), middle G_PLANET_INDEX>6 (signed JG,
//     0x0044d1ae/0x0044d1b5), inner G_PLANET_STATUS[G_PLANET_INDEX]!=0 (0x0044d1c1/0x0044d1c8), and
//     an innermost RE-CHECK of planet_transition_state>0 (0x0044d1cc/0x0044d1d3) that reads the SAME
//     byte the outer check already pinned to 0 with nothing in between writing it -- PRESERVE-BUG:
//     this makes the CALL to deploy_starting_squad (0x0044d1d5) and its AL-truncated store into
//     planet_transition_state (0x0044d1da) UNREACHABLE along any path this translation (or this
//     oracle) can drive. T4-T7 below prove EVERY combination that gets closest to entering this
//     block still results in zero calls and an unchanged planet_transition_state -- that is the
//     bug, transcribed literally, not "fixed."
//   0x0044d1df-0x0044d229: the unconditional rejoin. planet = G_PLANET_INDEX (FRESH read, after the
//     four calls and the gate above -- not cached from function entry, 0x0044d1df); progress row =
//     player_side*300 + Planets[planet].invention_index (0x0044d1e9-0x0044d201, the *3 stride from
//     LEA EDX,[EDX+EDX*2]); if progress[...].acquired != 1 (0x0044d208) AND
//     G_PLANET_STATUS[planet] != UNKNOWN(0) (0x0044d212/0x0044d219), calls
//     player_presence_lost(local_player_slot, 0) (0x0044d21b-0x0044d224, mode hardcoded via XOR
//     EDX,EDX).
//   0x0044d229: cam_mark_viewport_dirty, unconditional tail, always last.
//
#include "sim/resid/sim_planet_transition_finalize.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Mirrors sim_planet_transition_finalize.cpp's own anonymous-namespace constant (0x0044d1ae) --
// that constant is not exported, so this oracle restates it for its own boundary cases.
constexpr int32_t PLANET_INDEX_GATE = 6;

// ---- fixture handle for callbacks that must mutate LIVE state (proves a value is read FRESH at
// its use site rather than cached before an earlier callee ran) -------------------------------------
sim_fixture *g_fx = nullptr;

// ---- call log: presence + count + args + relative ORDER for all six callees -----------------------
struct call_log {
    std::vector<const char *> order;

    int time_resync_and_tick_calls = 0;

    int invasion_due_check_calls = 0;

    int     invasion_alert_clear_calls       = 0;
    int32_t last_invasion_alert_clear_planet = -999;

    int prod_deliver_arrivals_calls = 0;

    int     deploy_starting_squad_calls  = 0;
    int32_t deploy_starting_squad_return = 0x1234; // distinctive -- would be visibly wrong if the
                                                   // PRESERVE-BUG dead code were ever mutated live

    int      player_presence_lost_calls       = 0;
    uint32_t last_player_presence_lost_player = 0xdeadbeefu;
    uint32_t last_player_presence_lost_mode   = 0xdeadbeefu;

    int cam_mark_viewport_dirty_calls = 0;

    void reset() { *this = call_log{}; }
};
call_log g_log;

// ---- "mutate G_PLANET_INDEX from inside a callee" knobs -- used only by the fresh-read/no-caching
// cases (T14/T15): which callee should perform the mutation, and to what value.
enum class mutate_at { none,
                       invasion_due_check,
                       prod_deliver_arrivals };
mutate_at g_mutate_at = mutate_at::none;
int32_t   g_mutate_to = 0;

void mutate_planet_index_if_armed(mutate_at site) {
    if (g_mutate_at == site && g_fx != nullptr) { g_fx->planet_index = g_mutate_to; }
}

void rec_time_resync_and_tick() {
    g_log.order.push_back("time_resync_and_tick");
    ++g_log.time_resync_and_tick_calls;
}
int32_t rec_invasion_due_check() {
    g_log.order.push_back("invasion_due_check");
    ++g_log.invasion_due_check_calls;
    mutate_planet_index_if_armed(mutate_at::invasion_due_check);
    // The original returns an int 0/1 flag (Ghidra prototype corrected 2026-08-31, EN v384) that
    // planet_transition_finalize discards, exactly as the original does at 0x0044d187. A distinctive
    // non-zero value so a translation that started CONSUMING it would not silently read a 0.
    return 0x5D1;
}

void rec_invasion_alert_clear(int32_t planet) {
    g_log.order.push_back("invasion_alert_clear");
    ++g_log.invasion_alert_clear_calls;
    g_log.last_invasion_alert_clear_planet = planet;
}
void rec_prod_deliver_arrivals() {
    g_log.order.push_back("prod_deliver_arrivals");
    ++g_log.prod_deliver_arrivals_calls;
    mutate_planet_index_if_armed(mutate_at::prod_deliver_arrivals);
}
int32_t rec_deploy_starting_squad() {
    g_log.order.push_back("deploy_starting_squad");
    ++g_log.deploy_starting_squad_calls;
    return g_log.deploy_starting_squad_return;
}
uint32_t rec_player_presence_lost(uint32_t player, uint32_t mode) {
    g_log.order.push_back("player_presence_lost");
    ++g_log.player_presence_lost_calls;
    g_log.last_player_presence_lost_player = player;
    g_log.last_player_presence_lost_mode   = mode;
    return 0;
}
void rec_cam_mark_viewport_dirty() {
    g_log.order.push_back("cam_mark_viewport_dirty");
    ++g_log.cam_mark_viewport_dirty_calls;
}

const planet_transition_finalize_calls g_calls = {
    &rec_time_resync_and_tick,
    &rec_invasion_due_check,
    &rec_invasion_alert_clear,
    &rec_prod_deliver_arrivals,
    &rec_deploy_starting_squad,
    &rec_player_presence_lost,
    &rec_cam_mark_viewport_dirty,
};

void reset_observations() {
    g_log.reset();
    g_mutate_at = mutate_at::none;
    g_mutate_to = 0;
}

bool order_is(const std::vector<const char *> &want) {
    if (g_log.order.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        if (std::strcmp(g_log.order[i], want[i]) != 0) return false;
    }
    return true;
}

void run_call(sim_fixture &fx) {
    g_fx          = &fx;
    sim_view  v   = fx.view();
    sim_store own = fx.store();
    detail::planet_transition_finalize(v, own, g_calls);
}

} // namespace

void run_planet_transition_finalize_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- cam_pan_target_col forced to -1 (0xffffffff) unconditionally, 0x0044d178. Seeded to a
    // distinct nonzero sentinel first so the assertion proves an overwrite, not a lucky default.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.cam_pan_target_col = 42;

        run_call(fx);

        ck_eq((uint32_t)fx.cam_pan_target_col, 0xffffffffu,
              "T1: cam_pan_target_col forced to -1 unconditionally, 0x0044d178 (seeded 42 beforehand "
              "to prove overwrite)");
    }

    // =================================================================================================
    // T2 -- the four unconditional frontier calls (time_resync_and_tick, invasion_due_check,
    // invasion_alert_clear, prod_deliver_arrivals), each exactly once, in ORIGINAL ORDER, with
    // cam_mark_viewport_dirty as the unconditional TAIL (0x0044d229) -- and invasion_alert_clear
    // receives the live G_PLANET_INDEX value (0x0044d18c/0x0044d191). planet_index=4 is chosen so the
    // deploy gate's middle test (>6) is false (no deploy call) and G_PLANET_STATUS[4]==UNKNOWN(0) by
    // default blocks player_presence_lost, giving a clean, fully-known 5-entry order.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index = 4;

        run_call(fx);

        ck(order_is({"time_resync_and_tick", "invasion_due_check", "invasion_alert_clear",
                     "prod_deliver_arrivals", "cam_mark_viewport_dirty"}),
           "T2: call order matches 0x0044d182-0x0044d196 then the 0x0044d229 tail (deploy and "
           "presence-lost both correctly absent here)");
        ck_eq((uint32_t)g_log.time_resync_and_tick_calls, 1u,
              "T2: time_resync_and_tick fires exactly once, 0x0044d182");
        ck_eq((uint32_t)g_log.invasion_due_check_calls, 1u,
              "T2: invasion_due_check fires exactly once, 0x0044d187");
        ck_eq((uint32_t)g_log.invasion_alert_clear_calls, 1u,
              "T2: invasion_alert_clear fires exactly once, 0x0044d191");
        ck_eq((uint32_t)g_log.last_invasion_alert_clear_planet, 4u,
              "T2: invasion_alert_clear(G_PLANET_INDEX) -- arg is the live planet index, "
              "0x0044d18c/0x0044d191");
        ck_eq((uint32_t)g_log.prod_deliver_arrivals_calls, 1u,
              "T2: prod_deliver_arrivals fires exactly once, 0x0044d196");
        ck_eq((uint32_t)g_log.cam_mark_viewport_dirty_calls, 1u,
              "T2: cam_mark_viewport_dirty fires exactly once, 0x0044d229");
        ck_eq((uint32_t)g_log.deploy_starting_squad_calls, 0u,
              "T2: deploy_starting_squad NOT called -- planet_index(4) is not >PLANET_INDEX_GATE(6), "
              "0x0044d1ae/0x0044d1b5");
        ck_eq((uint32_t)g_log.player_presence_lost_calls, 0u,
              "T2: player_presence_lost NOT called -- G_PLANET_STATUS[4]==UNKNOWN(0) by default, "
              "0x0044d219");
    }

    // =================================================================================================
    // T3 -- _G_LLM_STRAT_SIM_ACTIVE forced to 1 unconditionally, 0x0044d19b. Seeded to a distinct
    // nonzero sentinel first so the assertion proves an overwrite, not a default already at 1.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.sim_active = 777;

        run_call(fx);

        ck_eq((uint32_t)fx.sim_active, 1u,
              "T3: _G_LLM_STRAT_SIM_ACTIVE forced to 1 unconditionally, 0x0044d19b (seeded 777 "
              "beforehand to prove overwrite)");
    }

    // =================================================================================================
    // T4 -- deploy gate, OUTER test FALSE (planet_transition_state != 0): the whole block is skipped
    // at 0x0044d1a5/0x0044d1ac before even reading G_PLANET_INDEX. planet_index/G_PLANET_STATUS are
    // seeded as if every deeper gate would also pass, so only the outer test can be responsible for
    // the skip.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_transition_state = 9; // != 0
        fx.planet_index            = 20;
        fx.planet_status[20]       = 5; // != 0, would satisfy the middle gate if reached

        run_call(fx);

        ck_eq((uint32_t)g_log.deploy_starting_squad_calls, 0u,
              "T4: planet_transition_state(9)!=0 -> outer gate FALSE, deploy never reached, "
              "0x0044d1a5/0x0044d1ac");
        ck_eq((uint32_t)fx.planet_transition_state, 9u,
              "T4: planet_transition_state left at 9 -- untouched by the skipped block");
    }

    // =================================================================================================
    // T5 -- deploy gate, MIDDLE test FALSE (planet_index(6) not > PLANET_INDEX_GATE(6), signed JG):
    // outer test passes (state==0) but the middle boundary fails, so the block is skipped at
    // 0x0044d1ae/0x0044d1b5 without ever reading G_PLANET_STATUS.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_transition_state          = 0;
        fx.planet_index                     = PLANET_INDEX_GATE; // == 6, not > 6
        fx.planet_status[PLANET_INDEX_GATE] = 5;                 // != 0, would satisfy the inner gate too

        run_call(fx);

        ck_eq((uint32_t)g_log.deploy_starting_squad_calls, 0u,
              "T5: planet_index==6, NOT >PLANET_INDEX_GATE(6) (signed JG boundary) -> middle gate "
              "FALSE, deploy never reached, 0x0044d1ae/0x0044d1b5");
        ck_eq((uint32_t)fx.planet_transition_state, 0u,
              "T5: planet_transition_state left at 0 -- untouched by the skipped block");
    }

    // =================================================================================================
    // T6 -- deploy gate, INNER test FALSE (G_PLANET_STATUS[planet_index] == UNKNOWN(0)): outer and
    // middle both pass (state==0, planet_index(7)>6) but the inner UNKNOWN check fails, skipping at
    // 0x0044d1c1/0x0044d1c8 before the innermost re-check is ever reached.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_transition_state              = 0;
        fx.planet_index                         = PLANET_INDEX_GATE + 1; // == 7, > 6
        fx.planet_status[PLANET_INDEX_GATE + 1] = 0;                     // UNKNOWN

        run_call(fx);

        ck_eq((uint32_t)g_log.deploy_starting_squad_calls, 0u,
              "T6: planet_index==7>6 but G_PLANET_STATUS[7]==UNKNOWN(0) -> inner gate FALSE, deploy "
              "never reached, 0x0044d1c1/0x0044d1c8");
        ck_eq((uint32_t)fx.planet_transition_state, 0u,
              "T6: planet_transition_state left at 0 -- untouched by the skipped block");
    }

    // =================================================================================================
    // T7 -- PRESERVE-BUG: outer(state==0), middle(planet_index(7)>6) AND inner(status!=0) ALL pass --
    // every gate that this translation can prove reachable is satisfied -- yet the INNERMOST re-check
    // (planet_transition_state>0, 0x0044d1cc/0x0044d1d3) reads the SAME byte the outer check just
    // pinned to 0 with nothing in between writing it, so it is ALWAYS false: deploy_starting_squad is
    // NEVER called and planet_transition_state is NEVER written by this block. A "fix" that changes
    // the inner check (e.g. `>0` to `>=0`) would make this test fail by making the call fire.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_transition_state              = 0;
        fx.planet_index                         = PLANET_INDEX_GATE + 1; // == 7, > 6
        fx.planet_status[PLANET_INDEX_GATE + 1] = 3;                     // != 0

        run_call(fx);

        ck_eq((uint32_t)g_log.deploy_starting_squad_calls, 0u,
              "T7 (PRESERVE-BUG): all three outer gates pass (state==0, idx=7>6, status=3!=0) but the "
              "innermost re-check of the SAME state byte (still 0) is never true -- deploy_starting_"
              "squad is unreachable, 0x0044d1cc-0x0044d1da");
        ck_eq((uint32_t)fx.planet_transition_state, 0u,
              "T7 (PRESERVE-BUG): planet_transition_state stays 0 -- the AL-truncated store at "
              "0x0044d1da never executes");
    }

    // =================================================================================================
    // T8 -- final block, acquired==1 (the exact boundary value) skips player_presence_lost regardless
    // of G_PLANET_STATUS, 0x0044d201/0x0044d208.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index                                   = 3;
        fx.cfg_planets[3].invention_index                 = 10;
        fx.player_side                                    = 2;
        fx.progress[2 * PROGRESS_ROW_COUNT + 10].acquired = 1;
        fx.planet_status[3]                               = 99; // != 0, would otherwise satisfy the 2nd condition
        fx.local_player_slot                              = 321;

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 0u,
              "T8: progress[...].acquired==1 -> first condition FALSE, player_presence_lost skipped "
              "even though G_PLANET_STATUS[3]!=0, 0x0044d201/0x0044d208");
    }

    // =================================================================================================
    // T9 -- final block, G_PLANET_STATUS==UNKNOWN(0) skips player_presence_lost even though acquired
    // is not 1, 0x0044d212/0x0044d219.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index                                   = 3;
        fx.cfg_planets[3].invention_index                 = 10;
        fx.player_side                                    = 2;
        fx.progress[2 * PROGRESS_ROW_COUNT + 10].acquired = 0; // not acquired -- 1st condition true
        fx.planet_status[3]                               = 0; // UNKNOWN -- blocks via 2nd condition

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 0u,
              "T9: acquired(0)!=1 is true, but G_PLANET_STATUS[3]==UNKNOWN(0) -> 2nd condition FALSE, "
              "player_presence_lost skipped, 0x0044d212/0x0044d219");
    }

    // =================================================================================================
    // T10 -- final block FIRES: acquired==0 (not 1), G_PLANET_STATUS==-1 (a negative, non-UNKNOWN
    // sentinel -- the "other side" of the UNKNOWN(0) boundary from T9). Confirms the args passed to
    // player_presence_lost: (local_player_slot, mode=0 hardcoded via XOR EDX,EDX).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index                                   = 3;
        fx.cfg_planets[3].invention_index                 = 10;
        fx.player_side                                    = 2;
        fx.progress[2 * PROGRESS_ROW_COUNT + 10].acquired = 0;
        fx.planet_status[3]                               = -1; // != 0, negative-sentinel side of the boundary
        fx.local_player_slot                              = 444;

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 1u,
              "T10: acquired(0)!=1 AND G_PLANET_STATUS[3]==-1(!=0) -> player_presence_lost fires, "
              "0x0044d21b-0x0044d224");
        ck_eq(g_log.last_player_presence_lost_player, 444u,
              "T10: player arg == local_player_slot (444), 0x0044d21d/0x0044d224");
        ck_eq(g_log.last_player_presence_lost_mode, 0u,
              "T10: mode arg hardcoded to 0 via XOR EDX,EDX, 0x0044d21b");
    }

    // =================================================================================================
    // T11 -- final block FIRES: acquired==2, another "not 1" value one step past the boundary from
    // the low side, with G_PLANET_STATUS at a typical positive nonzero value.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index                                   = 3;
        fx.cfg_planets[3].invention_index                 = 10;
        fx.player_side                                    = 2;
        fx.progress[2 * PROGRESS_ROW_COUNT + 10].acquired = 2;
        fx.planet_status[3]                               = 1;
        fx.local_player_slot                              = 555;

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 1u,
              "T11: acquired==2 (!=1) -> player_presence_lost fires, 0x0044d201/0x0044d208");
        ck_eq(g_log.last_player_presence_lost_player, 555u, "T11: player arg == local_player_slot (555)");
    }

    // =================================================================================================
    // T12 -- final block FIRES: acquired==0xff, the byte-field max sentinel (still != 1).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index                                   = 3;
        fx.cfg_planets[3].invention_index                 = 10;
        fx.player_side                                    = 2;
        fx.progress[2 * PROGRESS_ROW_COUNT + 10].acquired = 0xff;
        fx.planet_status[3]                               = 7;
        fx.local_player_slot                              = 666;

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 1u,
              "T12: acquired==0xff (max byte sentinel, !=1) -> player_presence_lost fires, "
              "0x0044d201/0x0044d208");
        ck_eq(g_log.last_player_presence_lost_player, 666u, "T12: player arg == local_player_slot (666)");
    }

    // =================================================================================================
    // T13 -- full indexing correctness / wrong-index trap: every progress cell is seeded acquired=1
    // EXCEPT progress[player_side*300 + Planets[planet_index].invention_index], which is 0. planet
    // index (9), player_side (5) and invention_index (222) are all DISTINCT and non-symmetric. If the
    // translation read the wrong row (e.g. used planet_index instead of invention_index as the row, or
    // swapped player_side for another field), it would land on an acquired==1 cell and wrongly skip.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        constexpr int32_t PLANET = 9;
        constexpr int32_t PSIDE  = 5;
        constexpr int32_t INVENT = 222;
        for (auto &p : fx.progress) p.acquired = 1;                    // every cell "already acquired"
        fx.progress[PSIDE * PROGRESS_ROW_COUNT + INVENT].acquired = 0; // ONLY the correct cell is not
        fx.planet_index                                           = PLANET;
        fx.cfg_planets[PLANET].invention_index                    = INVENT;
        fx.player_side                                            = PSIDE;
        fx.planet_status[PLANET]                                  = 42; // != 0
        fx.local_player_slot                                      = 888;

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 1u,
              "T13: only progress[player_side*300 + Planets[planet_index].invention_index] is "
              "unacquired -- a wrong player/planet/invention index would read an acquired==1 cell and "
              "wrongly skip, 0x0044d1df-0x0044d208");
        ck_eq(g_log.last_player_presence_lost_player, 888u, "T13: player arg == local_player_slot (888)");
    }

    // =================================================================================================
    // T14 -- FRESH READ, first site: invasion_alert_clear's G_PLANET_INDEX argument is read AFTER
    // invasion_due_check runs (0x0044d18c, between the two calls), not cached at function entry.
    // invasion_due_check's mock mutates G_PLANET_INDEX from 2 to 9; invasion_alert_clear must see 9.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_index = 2;
        g_mutate_at     = mutate_at::invasion_due_check;
        g_mutate_to     = 9;

        run_call(fx);

        ck_eq((uint32_t)g_log.last_invasion_alert_clear_planet, 9u,
              "T14: invasion_alert_clear(*v.planet_index) reads G_PLANET_INDEX FRESH after "
              "invasion_due_check mutated it 2->9, matching the reload at 0x0044d18c between "
              "0x0044d187 and 0x0044d191 (a value cached before invasion_due_check would read 2)");
    }

    // =================================================================================================
    // T15 -- FRESH READ, second site: the final rejoin's `planet = *v.planet_index` (0x0044d1df) is
    // read AFTER prod_deliver_arrivals runs (the last of the four unconditional calls), not cached at
    // function entry. planet_transition_state is set nonzero so the deploy gate short-circuits
    // regardless of the index, isolating this case to the final block only. prod_deliver_arrivals's
    // mock mutates G_PLANET_INDEX from 2 (whose progress row is pre-seeded ACQUIRED, would skip) to 5
    // (whose row is pre-seeded NOT acquired, must fire).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        fx.planet_transition_state                        = 1; // != 0 -> deploy gate skipped outright, regardless of index
        fx.planet_index                                   = 2;
        fx.cfg_planets[2].invention_index                 = 50;
        fx.player_side                                    = 4;
        fx.progress[4 * PROGRESS_ROW_COUNT + 50].acquired = 1; // if idx stayed 2 -> would SKIP
        fx.cfg_planets[5].invention_index                 = 77;
        fx.progress[4 * PROGRESS_ROW_COUNT + 77].acquired = 0; // mutated-to idx(5)'s row -> must FIRE
        fx.planet_status[2]                               = 3; // != 0 at BOTH indices, so only the acquired-cell differs
        fx.planet_status[5]                               = 3;
        fx.local_player_slot                              = 999;
        g_mutate_at                                       = mutate_at::prod_deliver_arrivals;
        g_mutate_to                                       = 5;

        run_call(fx);

        ck_eq((uint32_t)g_log.player_presence_lost_calls, 1u,
              "T15: final block uses the MUTATED planet_index==5 (progress row not-acquired -> fires) "
              "rather than the pre-mutation idx==2's row (acquired -> would skip), proving `planet = "
              "*v.planet_index` at 0x0044d1df is a fresh read taken after prod_deliver_arrivals runs, "
              "not a value cached at function entry");
        ck_eq(g_log.last_player_presence_lost_player, 999u,
              "T15: player arg == local_player_slot (999), unaffected by the index mutation");
    }
}

} // namespace mh::sim::test
