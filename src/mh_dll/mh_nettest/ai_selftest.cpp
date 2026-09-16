//
// ai_selftest.cpp -- `net_selftest.exe aitest`: the strategic AI's logic over heap buffers, with no
// game and no rig (RI-AI / AI0).
//
// WHY THIS IS AI0'S REAL LEVERAGE. The AI cluster is ~215 small, mechanical, PURE-over-its-state
// functions. A rig run costs ~150 s and can only exercise whatever branches that scenario happens to
// reach; this runs in milliseconds and reaches the ones a scenario never will -- an empty candidate
// list, a NEGATIVE damage tally, the swap-remove's "the victim IS the last entry" branch, the
// player-7 record overrun. It is the same arrangement `orderstest` uses for the order container and
// for the same reason: the logic takes its state as a PARAMETER, so the production binding and the
// test binding are the same code over different memory.
//
// WHAT IT DOES NOT PROVE. That the reimplementation matches the ORIGINAL -- only that it matches
// what this file says the original does. The independent arms are the shadow site (differential,
// against the real function) and the disassembly a reviewer reads. Treat a green aitest as "my
// understanding is self-consistent", which is worth having and is not equivalence.
//
// MUTATION-CHECKED: see the notes on the individual assertions. Every check here was watched to go
// red against a deliberately broken body before being accepted.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "ai/ai_army_milestone.h"
#include "ai/ai_attacker_intel.h"
#include "ai/ai_bldg_queue.h"
#include "ai/ai_bldg_queue_dispatch.h"
#include "ai/ai_bldg_weapon_range.h"
#include "ai/ai_notify_removed.h"
#include "ai/ai_nearest_flagged.h"
#include "ai/ai_queue_rotate.h"
#include "ai/ai_queue_type_query.h"
#include "ai/ai_site_dispatch.h"
#include "ai/ai_launch_storage.h"
#include "ai/ai_site_worth.h"
#include "ai/ai_build.h"
#include "ai/ai_build_score.h"
#include "ai/ai_construction_plan.h"
#include "ai/ai_engage.h"
#include "ai/ai_engage_scan.h"
#include "ai/ai_mine_plan.h"
#include "ai/ai_mine_rebalance.h"
#include "ai/ai_mine_yield.h"
#include "ai/ai_shortage_gate.h"
#include "ai/ai_turret_plan.h"
#include "ai/ai_worker_rebalance.h"
#include "ai/ai_grid.h"
#include "ai/ai_grid_stencil.h"
#include "ai/ai_group_form.h"
#include "ai/ai_group_move_helpers.h"
#include "ai/ai_group_task_workers.h"
#include "ai/ai_notify_map_changed.h"
#include "ai/ai_group_hold.h"
#include "ai/ai_group_task_machine.h"
#include "ai/ai_invasion.h"
#include "ai/ai_group_redistribute.h"
#include "ai/ai_group_tick.h"
#include "ai/ai_map_influence.h"
#include "ai/ai_queue_enqueue.h"
#include "ai/ai_queue_reconcile.h"
#include "ai/ai_queue_release.h"
#include "ai/ai_shortage_react.h"
#include "ai/ai_site_sort.h"
#include "ai/ai_spend_rate.h"
#include "ai/ai_state.h"
#include "ai/ai_train_flush.h"
#include "ai/ai_build_sources.h"
#include "ai/ai_train_plan.h"
#include "ai/ai_target.h"
#include "ai/ai_target_query.h"
#include "ai/ai_target_ref_predicates.h"
#include "ai/ai_worker_priority.h"
#include "ai/ai_turret_threat.h"
#include "ai/ai_unit_housing.h"
#include "ai/ai_group_task_formation.h"
#include "ai/ai_opponent_relations.h"
// (ai/ai_group_scratch_centroid.h was included here but never used -- the unit has no aitest
//  case. It moved to sim/sim_group_scratch_centroid.h 2026-08-07; see tracker AI1E.)
#include "ai/ai_group_member_count.h"
#include "ai/ai_spiral_table_init.h"
#include "ai/ai_build_plan_push.h"
#include "ai/ai_scan_target_sort_cmp.h"
#include "ai/ai_notify_bldg_constructed.h"
#include "ai/ai_hq_attack_scenario.h"
#include "ai/ai_scr_parse.h"
// RI-AI batch E, 2026-08-07 -- the six functions this addition tests.
#include "ai/ai_player_tick.h"
#include "ai/ai_active_unit_tick.h"
#include "ai/ai_reinforcements.h"
#include "ai/ai_hq_attack_commit.h"
#include "ai/ai_group_centroid.h"
#include "ai/ai_group_task_lifecycle.h"

// The fixture and the check helper live here since 2026-08-29 (AI1D debt drain), so that the
// per-TU oracle files added beside this one share ONE fixture and ONE check counter.
#include "ai_test_support.h"

// The per-TU oracle files that sit beside this one (RI-AI batch D / AI1D). Each holds its
// cases in its own translation unit and shares this suite's fixture and check counter
// through ai_test_support.h. Declared OUT HERE, above the anonymous namespace: inside it
// `namespace mh::ai::test` would name a nested `<anon>::mh`, not the real one.
namespace mh::ai::test {
void run_group_member_list_tests();
void run_attack_commit_tests();
void run_group_remove_tests();
void run_players_tick_tests();
void run_notify_unit_lifecycle_tests();
// The zero-call batch-C T3 rows, given the compensating offline oracle AI1's done_when
// asks for (2026-08-30, AI1 close).
void run_queue_remove_tests();
void run_group_building_scan_tests();
void run_group_no_member_near_centroid_tests();
void run_group_task_workers_tests();
} // namespace mh::ai::test

namespace {

using namespace mh::ai;
using namespace mh::ai::test;


// ---- recording stubs for every outward call -----------------------------------------------------
struct recorder {
    struct add_call {
        uint32_t ref;
        int32_t  index;
    };
    struct commit_call {
        uint32_t player;
        int32_t  unit_index;
        uint32_t target_ref;
        int32_t  target_index;
        bool     alt;
    };
    struct enqueue_call {
        // uint32_t since SIM-READY 2026-08-07 widened the two enqueue prototypes to the full
        // registers their entry sequences spill; the recorder keeps whatever the caller passed.
        uint32_t player;
        int32_t  unit_index;
        uint32_t target_owner;
        int32_t  target_index;
        uint32_t weapon_id;
        bool     building;
    };
    struct find_call {
        int32_t  player;
        uint32_t ai_build;
        uint32_t type;
    };

    std::vector<add_call>     adds;
    std::vector<commit_call>  commits;
    std::vector<enqueue_call> enqueues;
    std::vector<find_call>    finds;
    int                       sorts = 0, partitions = 0;
    uint32_t                  last_sort_ref   = 0;
    int32_t                   last_sort_index = 0;
    // The third argument the caller hands the sort helper -- the ORIGINAL's ambient ESI. Seeded to
    // 0 so an un-passed flag would read as "sort", i.e. the WRONG value, and the contract arm below
    // reddens instead of silently agreeing with a default.
    int32_t last_sort_flag = 0;

    // What the pure predicates should answer. Set per case.
    int32_t ground = 1, aa = 1;
    // Indices (into the scratch as it was BUILT) whose refs are "dead".
    uint32_t dead_ref   = 0xffffffffu;
    int32_t  dead_index = -1;

    // Whatever the next find lookup should return; incremented so each of the 21 slots is distinct
    // and a mis-ordered write is visible rather than plausible.
    uint32_t next_find = 1000;

    // ---- batch A layer 1 ----
    struct grid_call {
        void   *grid;
        int32_t w, h;
    };
    struct stamp_call {
        void   *grid;
        int32_t w, h, x, y, radius;
    };
    struct spiral_call {
        int32_t  player, x, y, ring_index;
        uint32_t target_mask;
    };
    struct group_index_call {
        int32_t player, unit_index;
    };
    std::vector<grid_call>        grid_clears;
    std::vector<stamp_call>       stamps;
    std::vector<spiral_call>      spirals;
    std::vector<group_index_call> group_lookups;

    // ---- batch A layer 3's two thin sites, covered offline (2026-08-01) ----
    struct toroidal_call {
        int32_t x1, y1, x2, y2;
    };
    struct weapon_pred_call {
        int which; // 1 = target_ref_has_ground_weapon, 2 = target_ref_has_aa_weapon,
                   // 3 = bldg_has_aa_weapon (layer 4; `ref` is then the MASKED owner nibble),
                   // 4 = unit_has_ground_weapon, 5 = unit_has_aa_weapon
        uint32_t ref;
        int32_t  index;
    };
    std::vector<toroidal_call>    toroidals;
    std::vector<weapon_pred_call> weapon_preds;
    // What the two PACKED-REF predicates answer. Separate from `ground`/`aa` above, which drive the
    // UNIT-ref pair -- the two families are different ai_calls members and different tests.
    int32_t ref_ground = 0, ref_aa = 0;
    // ---- batch A layer 4 ----
    int32_t bldg_aa = 0; // what llm_strat_bldg_has_aa_weapon should answer

    // Answers for the pure layer-1 predicates, set per case.
    uint32_t group_index  = 0;
    uint8_t  sight        = 0;
    uint32_t weapon_range = 0;
    // Building slots (owner, index) that bldg_is_alive should call DEAD.
    int32_t dead_bldg_owner = -1, dead_bldg_index = -1;
    // The map dims the toroidal stub wraps on -- kept in sync with the fixture by each case.
    int32_t map_w = 64, map_h = 48;

    // ---- batch B layer 1 ----
    // The influence-grid primitives. One record per call, in call order, because
    // llm_strat_ai_recompute_map_influence is a pure DRIVER -- the ordered argument sequence IS its
    // entire observable behaviour, so the sequence is what gets asserted rather than a side effect.
    struct influence_call {
        bool    flood; // false = fill_below_threshold, true = flood_step
        void   *grid;
        int32_t width, height;
        int32_t level; // threshold (fill) / source_level (flood)
        int32_t fill;
    };
    std::vector<influence_call> grid_calls;
    // Set non-zero to have the flood stub rewrite the fixture's map width AFTER that many calls.
    // That is how the "the extents are re-read per call, never hoisted" property is tested: a
    // translation that cached them keeps reporting the old value afterwards.
    int32_t  grid_width_change_after = 0;
    int32_t  grid_width_new          = 0;
    int32_t *grid_width_cell         = nullptr;

    // The build-queue reconcile callees.
    std::vector<int32_t> flushes; // llm_strat_ai_queue_flush_unit_train_entries_2(player)
    struct queue_construction_call {
        int32_t  player;
        int32_t  building_type;
        int16_t  x;
        uint16_t y;
    };
    std::vector<queue_construction_call> constructions;
    std::vector<int32_t>                 cost_queries; // the building_type each cost query asked about
    int32_t                              cost_answer = 0;

    // ---- batch B layer 2, the rest of it ----
    // What the count_unit_build_sources stub should answer, per unit type. The stub copies it into
    // the caller's buffer over indices 1..99 ONLY -- index 0 is deliberately left alone, because
    // that is what the real callee does (it fills 1..G_UNIT_COUNT_TOTAL inclusive @0x004e2201).
    std::vector<int32_t> build_sources = std::vector<int32_t>(100, 0);
    std::vector<int32_t> source_queries; // the player each count_unit_build_sources call asked for
    struct train_call {
        int32_t  player;
        uint32_t unit_id;
    };
    std::vector<train_call> trains;
    struct grant_call {
        uint16_t player;
        uint32_t res_type;
        uint32_t amount;
    };
    std::vector<grant_call> grants;
    // One record per queue-dispatch call: `kind` is the nibble that routed it (0/1/2/3, with 3 used
    // for BOTH nibble 3 and 4 since they share a handler -- which is why the test checks the nibble
    // through by_kind[] as well as through this list).
    struct dispatch_call {
        int32_t  kind;
        uint32_t player;
        int32_t  index;
    };
    std::vector<dispatch_call> dispatches;
    // Set non-zero to have the nibble-1 (process_entry) stub APPEND an entry to the player's queue,
    // the way the real one can. That is how "the count is re-read at the loop head, never hoisted"
    // is tested: a body that cached the count stops before the appended entry.
    int32_t  append_on_process_entry = 0;
    void    *append_target_queue     = nullptr;
    int32_t *append_target_count     = nullptr;
    uint8_t  append_status           = 0;

    // ---- batch B layer 3, the four build-queue dispatch arms -----------------------------------
    //
    // Every one of these records its FULL argument tuple rather than just a count. These four bodies
    // are almost nothing BUT argument marshalling -- the same (player, type, x, y) quadruple goes to
    // four different callees in three different register orders -- so a call-counting recorder would
    // pass a translation that handed a callee the wrong two of them, which is the error the
    // committed storage exists to prevent and the one a reviewer is least likely to spot.
    struct producer_query {
        int32_t player, unit_id;
    };
    struct recruit_order {
        uint32_t unit_id, player;
    };
    struct production_add {
        uint16_t player;
        int32_t  producer;
        int32_t  unit_type;
    };
    struct econ_track {
        int32_t player, unit;
    };
    struct type_dispatch {
        uint32_t player;
        int32_t  building_id;
    };
    // notify_map_changed and notify_map_changed_2 are DIFFERENT ai_calls members and different
    // functions in the image; `second` is which one ran, so a translation that called the wrong
    // notifier at the end of stage D fails by name instead of passing on argument equality.
    struct map_notify {
        bool    second;
        int32_t player, building_type, tile_x, tile_y;
    };
    struct footprint_query {
        void   *grid;
        int32_t w, h;
        void   *mask;
        int32_t span_x, span_y, x, y;
    };
    struct construction_order {
        uint32_t x, y, type;
        uint16_t player;
    };
    struct instant_order {
        uint32_t x, y;
        int32_t  type;
        uint32_t param_4;
        uint16_t player;
    };
    struct expenditure_stats {
        uint32_t player;
        int32_t  building_id;
    };
    struct population_delta {
        uint32_t player;
        int32_t  delta;
    };
    // The upgrade and cancel enqueues share a shape, so `cancel` is what separates them -- the whole
    // point of the nibble test is WHICH of the two ran.
    struct upgrade_order {
        bool     cancel;
        uint32_t player;
        int32_t  building_index;
    };
    std::vector<producer_query>     producer_queries;
    std::vector<recruit_order>      recruit_orders;
    std::vector<production_add>     production_adds;
    std::vector<econ_track>         econ_tracks;
    std::vector<type_dispatch>      type_dispatches;
    std::vector<map_notify>         map_notifies;
    std::vector<footprint_query>    footprints;
    std::vector<construction_order> construction_orders;
    std::vector<instant_order>      instant_orders;
    std::vector<expenditure_stats>  expenditures;
    std::vector<population_delta>   population_deltas;
    std::vector<upgrade_order>      upgrade_orders;
    // What the two queries answer. `producer_answer` 0 is the recruit arm's early return, and
    // `footprint_answer` is INVERTED-polarity (nonzero = the footprint fits) exactly as the callee is.
    int32_t producer_answer  = 0;
    int32_t footprint_answer = 0;
    // Set non-null to have the type-dispatch stub PUBLISH a candidate, the way the real one does --
    // it is a producer, not a query, so a stub that only recorded would leave stage A unable to
    // resolve a tile and every downstream branch untestable.
    int32_t *publish_site_count = nullptr;
    void    *publish_site_list  = nullptr;
    int32_t  publish_count      = 0; // what to set the count to
    int32_t  publish_x = 0, publish_y = 0;

    // ---- batch B layer 3: the build-category / housing planners' callees --------------------
    //
    // kind: 0 count_by_id, 1 count_by_type, 2 count_by_category, 3 already_queued,
    //       4 queue_has_pending, 5 has_heli_unit, 6 side_has_aircraft_producer.
    // The SEQUENCE is asserted, not just the set: llm_strat_ai_score_build_categories issues ten
    // category queries in an order that is deliberately not numeric (0x23 before 0x22), and a
    // translation that sorted them would still write the right values into the right fields.
    struct type_query {
        int32_t kind, player, arg;
    };
    std::vector<type_query> type_queries;
    // The answers are PER-ARGUMENT, not one scalar per callee. llm_strat_ai_react_resource_shortage
    // asks `already_queued` about four DIFFERENT candidate types inside one call and stops at the
    // first eligible slot, so a single scalar answer could not tell "slot 2 was reached" from
    // "slot 1 answered". Index 256 is the bucket for a NEGATIVE type, which is what an unset
    // candidate slot (-1) is.
    int32_t count_by_id_ans[257]{};
    int32_t count_by_type_ans[257]{};
    int32_t count_by_cat_ans[257]{};
    uint8_t already_queued_ans[257]{};
    int32_t queue_pending_ans[257]{};
    int32_t has_heli_ans     = 0;
    int32_t has_aircraft_ans = 0;
    // ---- batch B layer 3, the mine planner ----
    // llm_strat_ai_queue_rotate_newest_to_front(player) -- one entry per call.
    std::vector<int32_t> rotates;
    // llm_strat_ai_mine_portfolio_rebalance(player, out_yield). It records the out-POINTER because
    // the original derives it by LEA from the player_data base and getting the player index wrong
    // there is silent, and the value of _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT AS SEEN AT THE
    // CALL, because the real callee reads that global at 0x004e35ab -- so the store has to have
    // happened already, and nothing else in the body would ever notice if it had not.
    struct rebalance_call {
        int32_t player;
        void   *out_yield;
        int32_t min_count_seen;
    };
    std::vector<rebalance_call> rebalances;
    // ---- batch B layer 4/5, the mine portfolio (2026-08-05) ----
    struct yield_estimate_call {
        int32_t  building_id;
        uint32_t tile_x, tile_y;
        void    *out_yield;
        void    *out_quality;
    };
    std::vector<yield_estimate_call> yield_estimates;
    std::vector<int32_t>             train_flushes;
    // What the estimator stub WRITES, per call, into out_yield[1..4] and out_quality. It has to
    // write rather than merely record: every retire decision the rebalance makes is scored off this
    // table, so a stub that only counted calls would leave the body deciding over zeros and the
    // pass-2 / pass-3 assertions would be measuring the fixture instead of the body.
    int32_t yield_by_call[8][8]{};
    int32_t qual_by_call[8][3]{};
    // What the stub WRITES into out_yield[1..4]. Set it different from whatever the fixture's array
    // already holds and the per-resource flag assertions then prove the body re-reads the array
    // after the call rather than caching it before.
    int32_t rebalance_yield[8]{};
    bool    rebalance_writes = false;

    // ---- batch B layer 3, the turret / worker planners (2026-08-03) --------------------------
    //
    // llm_strat_bldg_connectivity_flood_fill(player, exclude_bldg_idx, flag_array). It records the
    // PLANE POINTER, not just the exclude index, because the turret planner's whole decision is a
    // comparison between two planes and a body that handed the same plane to both calls would make
    // that comparison trivially equal -- and would still pass a recorder that only counted calls.
    struct flood_call {
        uint32_t player;
        int32_t  exclude;
        void    *plane;
    };
    std::vector<flood_call> floods;
    // What the stub WRITES into the plane it is handed, indices 1..99. Two scripted answers rather
    // than one, keyed by whether `exclude` is 0 (the BASE pass) or not (the TRIAL pass), which is how
    // a cut vertex is simulated without a graph.
    uint8_t flood_base_ans[BUILDINGS_PER_PLAYER]{};
    uint8_t flood_trial_ans[BUILDINGS_PER_PLAYER]{};
    bool    flood_writes = false;

    struct nearest_call {
        int32_t player, x, y;
    };
    std::vector<nearest_call> nearests;
    int32_t                   nearest_ans = -1;

    // llm_strat_tile_midpoint_wrapped. The stub records its four inputs and writes the scripted
    // answer through the two out-pointers, so a translation that swapped out_x and out_y -- which the
    // push order at 0x004e501a makes an easy error -- moves the spiral scan to the wrong axis.
    struct midpoint_call {
        int32_t x0, y0, x1, y1;
    };
    std::vector<midpoint_call> midpoints;
    int32_t                    midpoint_out_x = 0, midpoint_out_y = 0;

    // The four IMMEDIATE-lane building orders. Each records its FULL tuple, for the reason the
    // dispatch arms' recorders give: these bodies are mostly argument marshalling.
    struct simple_bldg_order {
        uint32_t player;
        int32_t  building_index;
    };
    struct worker_order {
        bool     unassign;
        uint16_t player;
        uint16_t bldg_idx;
        uint32_t worker_count;
    };
    std::vector<simple_bldg_order> restart_orders;
    std::vector<simple_bldg_order> activate_orders;
    std::vector<worker_order>      worker_orders;

    // The two per-building predicates the worker rebalance gates on, answered PER BUILDING INDEX
    // rather than by one scalar: both walks visit several buildings in one call and the interesting
    // cases are the ones where the answers differ between them. Both default to ZERO, which is the
    // skip direction, so a case must opt each building in explicitly.
    uint8_t              uses_workers_ans[BUILDINGS_PER_PLAYER]{};
    uint8_t              worker_priority_ans[BUILDINGS_PER_PLAYER]{};
    std::vector<int32_t> uses_workers_queries;
    std::vector<int32_t> worker_priority_queries;

    // ---- batch B layer 4: the site-scanner dispatch ----
    // Which scanner ran, and with what. `kind` is 1 = resource-site, 2 = build-site, 3 = grid, so a
    // single ordered vector answers both "which branch" and "in what order relative to the sort",
    // which two separate counters could not. The scanners APPEND to the candidate count, exactly as
    // the originals do (INC, never a store) -- so a body that forgot its own reset is visible as an
    // accumulating count rather than as a missing call.
    struct scan_call {
        int     kind;
        long    player;
        int32_t building;
    };
    std::vector<scan_call> site_scans;
    int                    site_sorts = 0;
    // Where the stub scanners append. Set by the test to the fixture's count cell; null means the
    // stubs only record.
    int32_t *site_count_cell = nullptr;
    // ---- batch B layer 4, the storage-launch adapter ----
    // The four arguments the adapter forwards. Recorded rather than counted because the adapter's
    // ENTIRE observable behaviour is the marshalling: the mask on the first, and the other three
    // arriving in the right order and untouched.
    struct launch_call {
        uint32_t player;
        int32_t  unit_id;
        uint32_t x, y;
    };
    std::vector<launch_call> launches;

    // ---- batch C layer 0: the object-removed notification hook (2026-08-05) ----
    // Every one of the five outward calls is RECORDED WITH ITS ARGUMENTS rather than counted.
    // This body's whole observable behaviour is which calls it makes, in what order, with what
    // -- it writes only three scalars itself -- so a counter would leave the interesting errors
    // (the grid belonging to the wrong player, the seed on the wrong arm, the owner nibble
    // passed where the packed ref belongs) invisible.
    struct seed_stamp {
        void   *grid;
        int32_t w, h;
        void   *stencil;
        int32_t span_x, span_y, x, y, seed;
    };
    std::vector<seed_stamp> seed_stamps;
    struct target_remove {
        int32_t  player;
        uint32_t ref;
        int32_t  index;
    };
    std::vector<target_remove> target_removes;
    struct bldg_reconcile {
        uint32_t player, bldg_index;
    };
    std::vector<bldg_reconcile> bldg_reconciles;
    struct group_unlink {
        int32_t player, group, unit_index;
    };
    std::vector<group_unlink> unlinks;
    struct group_removal {
        int32_t  player;
        uint32_t group;
    };
    std::vector<group_removal> group_removes;

    // ---- batch C layer 1: the unit-group task machine driver (2026-08-05) ----
    // Recorded with the group index, not counted, because the whole point of llm_strat_ai_unit_group_tick
    // is WHICH group it touches and HOW MANY TIMES -- a per-call count would pass for a driver that
    // ticked group 3 twice and group 4 never.
    struct group_task_call {
        int32_t  player, group;
        uint32_t result; // steps only; 0 for activate calls
    };
    // ---- batch C layer 2, the group TASK MACHINE (activate + step) ----
    // ONE recorder for all nineteen activation arms, carrying the arm's own TASK CODE as `result`.
    // Nineteen separate stubs would need nineteen assertions to say what a transposed dispatch-table
    // entry does; one list lets a single loop over codes 0..0x18 check the whole table.
    std::vector<group_task_call> task_arms;
    // The step machine's callees. `dequeues` is the shared tail; `preds` carries which predicate ran
    // (its `result` is 1 arrival / 2 near-centroid / 3 settled / the owner_mask for a hostile scan)
    // and `docks` the dock-slot query, whose call must happen even on the arm that discards it.
    std::vector<group_task_call> dequeues;
    std::vector<group_task_call> preds;
    std::vector<group_task_call> docks;
    int32_t                      pred_result = 0; // what every progress predicate answers
    int32_t                      dock_busy   = 0; // what dock_slot_is_busy answers
    std::vector<group_task_call> activates;
    std::vector<group_task_call> steps;
    // What the STEP stub answers, consumed front-to-back; the tail value repeats once exhausted.
    // Non-zero means "run me again on the same group", which is the re-entry edge under test.
    std::vector<uint32_t> step_results;
    // How many activate calls leave active_flag CLEAR before one sets it. Models a task handler
    // that clears the flag again -- the only faithful way to reach the post-activate re-entry
    // edge, since llm_strat_ai_group_task_activate always sets the flag itself (0x004eb14d) and a
    // stub that never set it would hang the body exactly as the original would.
    int activate_clear_count = 0;

    // ---- batch C layer 2: group formation, hold, target seeding (2026-08-05) ----
    // The nine arguments of llm_strat_ai_group_task_enqueue / _preempt, recorded WHOLE. Six of the
    // nine differ between the three formation functions and two of them are the anchor pair, so a
    // recorder that kept only the task code would pass for a body that enqueued the right task with
    // the wrong anchors -- which is precisely the transposition ai_group_form.h's table exists to
    // make visible.
    struct group_task_args {
        bool    preempt; // false = enqueue (append), true = preempt (front-insert)
        int32_t player, group;
        int32_t task_code;
        int32_t param_4;
        int32_t param_5, param_6, param_7, param_8;
        int32_t param_9;
    };
    std::vector<group_task_args> task_pushes;
    // llm_strat_ai_group_create: what it answers, and who asked. The answer is a QUEUE so a test
    // can hand back -1 on a chosen call; the tail value repeats, like step_results above.
    std::vector<int32_t> create_players;
    std::vector<int32_t> create_results;
    // llm_strat_ai_scan_target_list_add -- the GLOBAL scratch adder, not player_data's.
    struct scan_target_add {
        int32_t  player;
        uint32_t ref;
        int32_t  index;
    };
    std::vector<scan_target_add> scan_target_adds;
    // llm_strat_ai_group_member_move, recorded WHOLE for the same reason task_pushes is: its four
    // arguments are (player, SRC, DST, unit) and the src/dst pair is the thing a translation
    // transposes. Recorded in call order, so a drain loop's sequence of head units is visible too.
    struct member_move {
        uint32_t player;
        int32_t  src, dst, unit_id;
    };
    std::vector<member_move> member_moves;
    // ---- batch C layer 3 (2026-08-06), the two harvest bodies' movers ----
    // Both are recorded WHOLE rather than by argument count: the point of these two bodies is that
    // they publish a member list and then make exactly ONE call, so "which arguments, in which
    // order" is the entire behaviour under test.
    struct formation_move {
        uint32_t player;
        int32_t  centroid_x, centroid_y, target_x, target_y;
    };
    std::vector<formation_move> formation_moves;
    struct scatter_call {
        uint32_t player;
        int32_t  x, y;
    };
    std::vector<scatter_call> scatter_calls;
    // ---- batch C layer 2, llm_strat_ai_group_redistribute_units' other four callees ----
    // The centroid the stub HANDS BACK through its out-pointers, and every call recorded whole. The
    // handed-back pair matters as much as the recording: the body forwards it to unit_flag_and_move,
    // so a translation that swapped the two out-pointers is only visible if the two values differ.
    int32_t centroid_out_x = 0, centroid_out_y = 0;
    struct centroid_call {
        int32_t player, group;
    };
    std::vector<centroid_call> centroid_calls;
    struct flag_and_move {
        uint32_t player;
        int32_t  unit_index;
        uint32_t x, y;
    };
    std::vector<flag_and_move> flag_moves;
    struct split_off_call {
        uint32_t player;
        int32_t  centroid_x, centroid_y, source_group_idx;
        uint32_t member_count;
    };
    std::vector<split_off_call> split_offs;
    struct split_excess_call {
        uint32_t player;
        int32_t  group_idx;
    };
    std::vector<split_excess_call> split_excess;

    // ---- RI-AI batch E, 2026-08-07: start_hq_attack_scenario's three callees ----
    struct create_soldier_call {
        uint32_t x, y;
        uint16_t a2, param_4;
        char     param_5;
    };
    std::vector<create_soldier_call> create_soldier_calls;
    uint32_t                         create_soldier_ans = 0;
    struct diplomacy_call {
        int32_t player_a, player_b;
        uint8_t relation;
    };
    std::vector<diplomacy_call> diplomacy_calls;
    int                         commit_attack_on_enemy_hq_calls = 0;

    // ---- RI-AI batch E, 2026-08-07 -- player_tick / active_unit_tick / unit_commit_attack_on_
    // enemy_hq / invasion_spawn_reinforcements / create_reinforcement_unit. Both player_tick and
    // active_unit_tick are ALMOST ENTIRELY a call SEQUENCE (see their own .h banners), so a single
    // ordered log of compact strings is what proves the sequence AND the per-call arguments at once --
    // cheaper than one struct+vector per callee for ~30 call sites whose own body is a one-line
    // marshal. Cleared by clear() along with everything else.
    std::vector<std::string> slice2_log;
    void                     log2(const char *fmt, ...) {
        char    buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        slice2_log.push_back(buf);
    }
    // Configurable answers for the slice's pure/query stubs.
    int32_t                     starting_unit_ans         = 42;
    uint32_t                    unit_create_ans           = 777;
    uint32_t                    defense_radius_sq_ans     = 100; // a perfect square by default; see the x87 test for why
    uint8_t                     select_weapon_ans         = 0;   // 0x64 = "no weapon mounted" (the move branch)
    int32_t                     building_weapon_range_ans = 0;
    std::map<int32_t, uint32_t> pending_by_unit; // unit_is_order_pending answer, default 0 (idle)
    std::map<int32_t, int32_t>  abandon_by_unit; // unit_should_abandon_target answer, default 0
    // OFFLINE-ORACLE, 2026-08-07 -- unit_is_idle_or_parked's answer, keyed by unit_index, default 0
    // (not idle). group_compute_centroid @0x004d5c17 is the first translated body to call it, so
    // this was previously unwired in stub_calls().
    std::map<int32_t, int32_t> idle_by_unit;
    // update_opponent_relations: which (assessed_player, out-pointer) pairs were handed to it, in
    // call order -- the out-pointer is what proves it targeted THIS player's own
    // ai_opponent_assessments[assessed], not some other player's slot or array.
    std::vector<std::pair<uint32_t, void *>> opponent_relation_calls;
    // unit_order_attack_building_reposition / unit_order_move: recorded WHOLE (see the ~line-706
    // comment on why for the build-queue dispatch arms) -- unit_commit_attack_on_enemy_hq is nearly
    // nothing BUT this argument marshalling, so the tuple IS the behaviour under test.
    struct attack_reposition_call {
        // `player` widened with the prototype 2026-08-24: the original homes the full DWORD of EAX
        // at 0x0046bfb1, and the high half carries the order flags of the packed
        // `player & 0xffff | 0x40` the attack-order family marshals. Recording it as uint16_t
        // would have thrown those flags away in the one tuple that IS the behaviour under test.
        uint32_t player;
        int32_t  unit_index;
        uint32_t target_ref;
        int32_t  target_index;
        uint32_t weapon_id;
    };
    std::vector<attack_reposition_call> attack_reposition_calls;
    struct order_move_call {
        uint32_t player;
        int32_t  unit_index;
        uint32_t x, y, arg5;
    };
    std::vector<order_move_call> order_move_calls;
    // score_reinforcement_unit / create_reinforcement_unit, as invasion_spawn_reinforcements sees
    // them THROUGH gc -- a controllable per-proto score and a recorded spawn-call tuple, so the
    // spawner's SELECTION and ROUND-ROBIN can be asserted independently of what the two leaves
    // (tested directly elsewhere in this file) actually compute.
    std::map<int32_t, uint32_t>              score_by_proto;
    std::vector<std::pair<int32_t, int32_t>> score_calls;       // (player, unit_proto_id), call order preserved
    uint32_t                                 score_default = 1; // score_by_proto's fallback for an unlisted proto
    struct spawn_call {
        uint32_t x, y, proto;
        uint16_t player;
    };
    std::vector<spawn_call> reinforcement_spawns;
    // gc.unit_create's full argument tuple (player_tick's one-shot spawn edge AND
    // create_reinforcement_unit's type>0xe branch both call it) -- recorded numerically rather than
    // only through slice2_log so a test can assert on the values without parsing a format string.
    struct create_unit_call {
        uint32_t x, y;
        uint16_t unit, player;
        uint8_t  is_ship;
    };
    std::vector<create_unit_call> create_unit_calls;

    void clear() { *this = recorder{}; }
};

recorder g_rec;

// The mine planner's scratch global, as the STUB sees it. It is a pointer rather than a value
// because what the stub has to capture is the cell the body just wrote, and the stub has no access
// to the fixture -- see recorder::rebalance_call::min_count_seen for why that capture matters.
int32_t  g_mine_min_dummy = 0;
int32_t *g_mine_min_cell  = &g_mine_min_dummy;

void st_add(uint32_t ref, int32_t idx) { g_rec.adds.push_back({ref, idx}); }
// The UNIT-ref weapon pair. They RECORD as well as answer (codes 4 and 5) because batch A layer 4's
// two ref predicates are almost nothing BUT a hand-off to them, and the thing that can be wrong there
// is which callee ran and what it was handed -- see test_target_ref_predicates.
int32_t st_ground(uint32_t ref, int32_t idx) {
    g_rec.weapon_preds.push_back({4, ref, idx});
    return g_rec.ground;
}
int32_t st_aa(uint32_t ref, int32_t idx) {
    g_rec.weapon_preds.push_back({5, ref, idx});
    return g_rec.aa;
}
int32_t st_alive(uint32_t ref, int32_t idx) {
    return (ref == g_rec.dead_ref && idx == g_rec.dead_index) ? 0 : 1;
}
void st_sort(uint32_t ref, int32_t idx, int32_t inherited_sorted_flag) {
    ++g_rec.sorts;
    g_rec.last_sort_ref   = ref;
    g_rec.last_sort_index = idx;
    g_rec.last_sort_flag  = inherited_sorted_flag;
}
void st_partition() { ++g_rec.partitions; }
void st_commit(uint32_t p, int32_t u, uint32_t tr, int32_t ti) {
    g_rec.commits.push_back({p, u, tr, ti, false});
}
void st_commit_alt(uint32_t p, int32_t u, uint32_t tr, int32_t ti) {
    g_rec.commits.push_back({p, u, tr, ti, true});
}
void st_enq_bldg(uint32_t p, int32_t u, uint32_t to, int32_t ti, uint32_t w) {
    g_rec.enqueues.push_back({p, u, to, ti, w, true});
}
void st_enq_unit(uint32_t p, int32_t u, uint32_t to, int32_t ti, uint32_t w) {
    g_rec.enqueues.push_back({p, u, to, ti, w, false});
}
uint32_t st_find(int32_t player, uint32_t ai_build, uint32_t type) {
    g_rec.finds.push_back({player, ai_build, type});
    return g_rec.next_find++;
}

// ---- batch A layer 1 stubs ----
//
// The two grid helpers do NOT merely record: they perform the real bit operation on the caller's
// buffer, because what the rescan is FOR is the resulting grid, and a recorder that only counts
// calls would pass a body that stamped the wrong tile.
void st_grid_clear(uint8_t *grid, int32_t w, int32_t h) {
    g_rec.grid_clears.push_back({grid, w, h});
    for (int32_t x = 0; x < w; ++x)
        for (int32_t y = 0; y < h; ++y) grid[(x << 8) | y] &= (uint8_t)~TILE_FLAG_TURRET_THREAT;
}
void st_grid_stamp(uint8_t *grid, int32_t w, int32_t h, int32_t x, int32_t y, int32_t radius) {
    g_rec.stamps.push_back({grid, w, h, x, y, radius});
    grid[(x << 8) | y] |= TILE_FLAG_TURRET_THREAT; // the centre tile is enough to see
}
int32_t st_bldg_is_alive(int32_t player, int32_t index) {
    return (player == g_rec.dead_bldg_owner && index == g_rec.dead_bldg_index) ? 0 : 1;
}
uint32_t st_group_index(int32_t player, int32_t unit_index) {
    g_rec.group_lookups.push_back({player, unit_index});
    return g_rec.group_index;
}
uint32_t st_sight(uint32_t, int32_t) { return g_rec.sight; }
uint32_t st_weapon_range(uint32_t, int32_t) { return g_rec.weapon_range; }
int32_t  st_spiral(int32_t player, int32_t x, int32_t y, int32_t ring_index, uint32_t mask) {
    g_rec.spirals.push_back({player, x, y, ring_index, mask});
    return 0;
}
// The REAL toroidal metric, not a placeholder: the sort's whole observable output is the ordering
// this produces, so a stub returning a constant would make every ordering assertion vacuous. It also
// RECORDS its arguments, because target_dist_sq's entire contract is the (a.x, a.y, b.x, b.y) order
// it hands over -- a metric assertion alone cannot tell that order from its reverse when the inputs
// are symmetric, and the four coordinates are the only thing the function produces.
uint32_t toroidal_metric(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w, int32_t h) {
    uint32_t dx = (uint32_t)(x1 > x2 ? x1 - x2 : x2 - x1);
    if (dx > (uint32_t)w / 2) dx = (uint32_t)w - dx;
    uint32_t dy = (uint32_t)(y1 > y2 ? y1 - y2 : y2 - y1);
    if (dy > (uint32_t)h / 2) dy = (uint32_t)h - dy;
    return dx * dx + dy * dy;
}
uint32_t st_toroidal(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_rec.toroidals.push_back({x1, y1, x2, y2});
    return toroidal_metric(x1, y1, x2, y2, g_rec.map_w, g_rec.map_h);
}
// The two PACKED-REF weapon predicates (batch A layer 3's ai_calls members, distinct from st_ground /
// st_aa above, which are the UNIT-ref pair). They record the call ORDER because
// target_ref_has_engageable_weapon must call both unconditionally, ground first -- a short-circuit
// would be invisible to a return-value assertion and visible to the shadow site as a call-count
// divergence.
int32_t st_ref_ground(uint32_t ref, int32_t idx) {
    g_rec.weapon_preds.push_back({1, ref, idx});
    return (uint8_t)g_rec.ref_ground;
}
int32_t st_ref_aa(uint32_t ref, int32_t idx) {
    g_rec.weapon_preds.push_back({2, ref, idx});
    return (uint8_t)g_rec.ref_aa;
}
// Batch A layer 4. The BUILDING half of target_ref_has_aa_weapon is not that function's own code at
// all -- it falls through into llm_strat_bldg_has_aa_weapon. So the ONLY thing the translation can
// get wrong there is the routing, and the only way to see the routing is to record it: `which`
// distinguishes it from the two UNIT-ref predicates above, and the recorded `ref` is what says
// whether the owner nibble was masked before the hand-off.
int32_t st_bldg_aa(uint32_t player, int32_t idx) {
    g_rec.weapon_preds.push_back({3, player, idx});
    return g_rec.bldg_aa;
}

// ---- batch B layer 1 stubs ----------------------------------------------------------------------
void st_grid_fill_below(uint8_t *grid, int32_t w, int32_t h, int32_t threshold, int32_t fill) {
    g_rec.grid_calls.push_back({false, grid, w, h, threshold, fill});
}
void st_grid_flood(uint8_t *grid, int32_t w, int32_t h, int32_t source_level, int32_t fill) {
    g_rec.grid_calls.push_back({true, grid, w, h, source_level, fill});
    // The re-read probe: rewrite the fixture's width from under the driver mid-pass. A body that
    // hoisted *v.map_width into a local keeps handing us the stale value from here on.
    if (g_rec.grid_width_change_after > 0 && g_rec.grid_width_cell != nullptr &&
        (int32_t)g_rec.grid_calls.size() == g_rec.grid_width_change_after)
        *g_rec.grid_width_cell = g_rec.grid_width_new;
}
void    st_queue_flush(int32_t player) { g_rec.flushes.push_back(player); }
int32_t st_total_cost(int32_t building_type) {
    g_rec.cost_queries.push_back(building_type);
    return g_rec.cost_answer;
}
int32_t st_queue_construction(int32_t player, int32_t building_type, int16_t x, uint16_t y) {
    g_rec.constructions.push_back({player, building_type, x, y});
    return 0;
}

// ---- batch B layer 3 stubs: the mine planner's two remaining callees -----------------------------
void st_queue_rotate(int32_t player) { g_rec.rotates.push_back(player); }

// The yield refresh. In the original this is an OUT-POINTER helper that zeroes indices 0..4 and
// fills them, so the stub does the same when asked -- suppressing the write instead would make the
// body score its shortage flags off stale input and the test would be measuring the fixture.
void st_mine_rebalance(uint32_t player, int32_t *out_yield) {
    g_rec.rebalances.push_back({(int32_t)player, out_yield, *g_mine_min_cell});
    if (g_rec.rebalance_writes && out_yield) {
        for (int i = 0; i <= RESOURCE_ID_LAST; ++i) out_yield[i] = g_rec.rebalance_yield[i];
    }
}

// ---- batch B layer 4/5 stubs: the rebalance's two callees ---------------------------------------
//
// The estimator FILLS the row it is handed, from a per-call table, for the same reason
// st_mine_rebalance does. It also reproduces the contract the estimator's header calls out: it does
// NOT touch out_yield[0]. A stub that zeroed slot 0 would hide a body that relied on the callee to
// do it, which is precisely the divergence the header warns about.
void st_calc_mine_yield(int32_t building_id, uint32_t tile_x, uint32_t tile_y, int32_t *out_yield,
                        int32_t *out_quality) {
    const size_t n = g_rec.yield_estimates.size();
    g_rec.yield_estimates.push_back({building_id, tile_x, tile_y, out_yield, out_quality});
    if (n >= 8) return; // the per-call table's extent; tests below never need more
    if (out_yield != nullptr) {
        for (int r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) out_yield[r] = g_rec.yield_by_call[n][r];
    }
    if (out_quality != nullptr) {
        // committed llm_strat_ai_calc_mine_yield_estimate spells this out-param int32_t * (TACT1-P
        // C6, 2026-09-04); the mock's payload is really mh::ai::mine_quality, so re-interpret it.
        mh::ai::mine_quality *q = reinterpret_cast<mh::ai::mine_quality *>(out_quality);
        q->near_count           = g_rec.qual_by_call[n][0];
        q->mid_count            = g_rec.qual_by_call[n][1];
        q->far_count            = g_rec.qual_by_call[n][2];
    }
}
void st_train_flush(int32_t player) { g_rec.train_flushes.push_back(player); }

// ---- batch B layer 2 stubs (the rest of it) -----------------------------------------------------
//
// count_unit_build_sources FILLS the caller's buffer rather than merely recording, because what
// plan_unit_training does with it is the whole of passes 2 and 4 -- a recorder that only counted
// calls would pass a body that never read the table. It writes indices 1..99 and NOT index 0,
// mirroring the real callee's 1..G_UNIT_COUNT_TOTAL inclusive loop.
void st_count_sources(int32_t player, int32_t *out_counts) {
    g_rec.source_queries.push_back(player);
    for (int i = 1; i < 100; ++i) out_counts[i] = g_rec.build_sources[(size_t)i];
}
int32_t st_queue_train(int32_t player, uint32_t unit_id) {
    g_rec.trains.push_back({player, unit_id});
    return 0;
}
void st_grant_resource(uint16_t player, uint32_t res_type, uint32_t amount) {
    g_rec.grants.push_back({player, res_type, amount});
}
// ---- batch B layer 3 stubs: the four dispatch arms' own callees ---------------------------------
int32_t st_find_producer(int32_t player, int32_t unit_id) {
    g_rec.producer_queries.push_back({player, unit_id});
    return g_rec.producer_answer;
}
int32_t st_recruit_order(uint32_t unit_id, uint32_t player) {
    g_rec.recruit_orders.push_back({unit_id, player});
    return 1; // the real one returns a constant 1
}
void st_production_add(uint16_t player, int32_t producer, int32_t unit_type) {
    g_rec.production_adds.push_back({player, producer, unit_type});
}
void st_econ_track(int32_t player, int32_t unit) { g_rec.econ_tracks.push_back({player, unit}); }
// A PRODUCER, not a query: it publishes into the shared site-candidate scratch, and the body under
// test reads the count back two instructions later to decide whether a tile was found. A recording
// stub alone would pin stage A to its empty branch forever.
void st_type_dispatch(uint32_t player, int32_t building_id) {
    g_rec.type_dispatches.push_back({player, building_id});
    if (g_rec.publish_site_count != nullptr) {
        auto *list = static_cast<mh::ai::site_candidate *>(g_rec.publish_site_list);
        if (list != nullptr && g_rec.publish_count > 0) {
            list[0].tile_x = g_rec.publish_x;
            list[0].tile_y = g_rec.publish_y;
        }
        *g_rec.publish_site_count = g_rec.publish_count;
    }
}
void st_notify_map(int32_t p, int32_t type, int32_t x, int32_t y) {
    g_rec.map_notifies.push_back({false, p, type, x, y});
}
void st_notify_map_2(int32_t p, int32_t type, int32_t x, int32_t y) {
    g_rec.map_notifies.push_back({true, p, type, x, y});
}
int32_t st_footprint(uint8_t *grid, int32_t w, int32_t h, uint8_t *mask, int32_t sx, int32_t sy, int32_t x,
                     int32_t y) {
    g_rec.footprints.push_back({grid, w, h, mask, sx, sy, x, y});
    return g_rec.footprint_answer;
}
uint32_t st_construction_order(uint32_t x, uint32_t y, uint32_t type, uint16_t player) {
    g_rec.construction_orders.push_back({x, y, type, player});
    return 0;
}
int32_t st_instant_order(uint32_t x, uint32_t y, int32_t type, uint32_t p4, uint16_t player) {
    g_rec.instant_orders.push_back({x, y, type, p4, player});
    return 0;
}
void st_expenditure(uint32_t player, int32_t building_id) {
    g_rec.expenditures.push_back({player, building_id});
}
void st_population_delta(uint32_t player, int32_t delta) {
    g_rec.population_deltas.push_back({player, delta});
}
void st_upgrade_order(uint32_t player, int32_t index) {
    g_rec.upgrade_orders.push_back({false, player, index});
}
void st_cancel_order(uint32_t player, int32_t index) {
    g_rec.upgrade_orders.push_back({true, player, index});
}

void st_q_recruit(uint32_t p, int32_t i) { g_rec.dispatches.push_back({0, p, i}); }
void st_q_process_entry(uint32_t p, int32_t i) {
    g_rec.dispatches.push_back({1, p, i});
    if (g_rec.append_on_process_entry > 0 && g_rec.append_target_count != nullptr) {
        --g_rec.append_on_process_entry;
        auto *q                              = static_cast<mh::game::mh_llm_strat_ai_bldg_queue_entry *>(g_rec.append_target_queue);
        q[*g_rec.append_target_count].status = g_rec.append_status;
        ++*g_rec.append_target_count;
    }
}
void st_q_state2() { g_rec.dispatches.push_back({2, 0, -1}); }
void st_q_upgrade(uint32_t p, int32_t i) { g_rec.dispatches.push_back({3, p, i}); }

// MEMBER-ASSIGNED, not a positional aggregate initialiser. It used to be positional, which meant a
// member inserted into the middle of ai_calls would have silently re-bound every stub after it to
// the wrong slot -- the same hazard calls_are_complete() exists to catch in the two PRODUCTION sets,
// except that a null is detectable and a shifted binding is not. Only the slots the offline tests
// actually reach are bound; the rest stay null deliberately, so an unstubbed call fails loudly.
// ---- batch B layer 3 stubs: the build-category / housing planners' callees -----------------------
//
// All seven are pure QUERIES in the original (confirmed twice: no store outside their own frames in
// the listings, and zero write/rw/movs cells for all of them in tmp/state_matrix.json), so a stub
// that records and answers is a faithful stand-in rather than a suppression.
int32_t q_idx(int32_t t) { return (t >= 0 && t < 256) ? t : 256; }

int32_t st_count_by_id(int32_t player, int32_t building_id) {
    g_rec.type_queries.push_back({0, player, building_id});
    return g_rec.count_by_id_ans[q_idx(building_id)];
}
int32_t st_count_by_type(int32_t player, int32_t bldg_type) {
    g_rec.type_queries.push_back({1, player, bldg_type});
    return g_rec.count_by_type_ans[q_idx(bldg_type)];
}
int32_t st_count_by_category(int32_t player, uint32_t category) {
    g_rec.type_queries.push_back({2, player, (int32_t)category});
    return g_rec.count_by_cat_ans[q_idx((int32_t)category)];
}
int32_t st_already_queued(int32_t player, uint32_t building_type) {
    g_rec.type_queries.push_back({3, player, (int32_t)building_type});
    return g_rec.already_queued_ans[q_idx((int32_t)building_type)];
}
int32_t st_queue_has_pending(int32_t player, uint32_t building_type) {
    g_rec.type_queries.push_back({4, player, (int32_t)building_type});
    return g_rec.queue_pending_ans[q_idx((int32_t)building_type)];
}
int32_t st_has_heli_unit(int32_t player) {
    g_rec.type_queries.push_back({5, player, 0});
    return g_rec.has_heli_ans;
}
int32_t st_has_aircraft_producer(int32_t player) {
    g_rec.type_queries.push_back({6, player, 0});
    return g_rec.has_aircraft_ans;
}

// ---- batch B layer 3 stubs: the turret / worker planners' callees --------------------------------
//
// The flood fill is the one stub here that PRODUCES rather than answers, and it has to: the turret
// planner reads back the plane it just filled, so a stub that only recorded would leave every branch
// past the first connectivity test unreachable. It writes indices 1..99 and DELIBERATELY LEAVES
// INDEX 0 ALONE, which is what the original does (its loop counter starts at 1, 0x004d77ec).
void st_flood_fill(uint32_t player, int32_t exclude_bldg_idx, uint8_t *flag_array) {
    g_rec.floods.push_back({player, exclude_bldg_idx, flag_array});
    if (!g_rec.flood_writes) return;
    const uint8_t *src = exclude_bldg_idx == 0 ? g_rec.flood_base_ans : g_rec.flood_trial_ans;
    for (int32_t i = 1; i < BUILDINGS_PER_PLAYER; ++i) flag_array[i] = src[i];
}
int32_t st_find_nearest_flagged(int32_t player, int32_t x, int32_t y) {
    g_rec.nearests.push_back({player, x, y});
    return g_rec.nearest_ans;
}
void st_tile_midpoint(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t *out_x, int32_t *out_y) {
    g_rec.midpoints.push_back({x0, y0, x1, y1});
    *out_x = g_rec.midpoint_out_x;
    *out_y = g_rec.midpoint_out_y;
}
void st_restart_construction(uint32_t player_id, int32_t building_index) {
    g_rec.restart_orders.push_back({player_id, building_index});
}
int32_t st_uses_workers(uint32_t player, int32_t building_index) {
    g_rec.uses_workers_queries.push_back(building_index);
    (void)player;
    return (building_index >= 0 && building_index < BUILDINGS_PER_PLAYER)
               ? g_rec.uses_workers_ans[building_index]
               : 0;
}
int32_t st_worker_priority(int32_t player, int32_t site_index) {
    g_rec.worker_priority_queries.push_back(site_index);
    (void)player;
    return (site_index >= 0 && site_index < BUILDINGS_PER_PLAYER)
               ? g_rec.worker_priority_ans[site_index]
               : 0;
}
void st_activate_order(uint32_t player_id, int32_t building_index) {
    g_rec.activate_orders.push_back({player_id, building_index});
}
void st_assign_workers(uint16_t player, uint16_t bldg_idx, uint32_t worker_count) {
    g_rec.worker_orders.push_back({false, player, bldg_idx, worker_count});
}
void st_unassign_workers(uint16_t player, uint16_t bldg_idx, uint32_t worker_count) {
    g_rec.worker_orders.push_back({true, player, bldg_idx, worker_count});
}

// ---- batch B layer 4 stubs: the three site scanners and the sort ---------------------------------
//
// They APPEND to the count and never store it, which is what the originals do (their only write to
// _G_LLM_STRAT_AI_SITE_CANDIDATE_COUNT is an INC at 0x004e6502 / 0x004e6673 / 0x004e682a). That is
// deliberate: it is what lets a test seed the count non-zero and then assert the dispatcher's own
// reset happened, rather than assert it by reading the dispatcher's source.
void st_scan_append(int kind, long player, int32_t building) {
    g_rec.site_scans.push_back({kind, player, building});
    if (g_rec.site_count_cell) ++*g_rec.site_count_cell;
}
void st_scan_resource_sites(uint32_t player, int32_t building_type) {
    st_scan_append(1, (long)player, building_type);
}
void st_scan_build_sites(int32_t player_idx, int32_t building_idx) {
    st_scan_append(2, (long)player_idx, building_idx);
}
void st_scan_grid_sites(uint32_t player, int32_t building_idx) {
    st_scan_append(3, (long)player, building_idx);
}
void st_sort_site_candidates() { ++g_rec.site_sorts; }
// llm_strat_unit_order_auto_launch_from_storage_enqueue. In the real shadow arm this runs FOR REAL
// (its three order regions are declared by the site); here it only records, because what the offline
// test can check is the marshalling and nothing else.
void st_auto_launch(uint32_t player, int32_t unit_idx, uint32_t x, uint32_t y) {
    g_rec.launches.push_back({player, unit_idx, x, y});
}

// ---- batch C layer 0: the object-removed notification hook ------------------------------
//
// The group record st_group_member_unlink DECREMENTS, or null for record-only. Same pattern as
// g_mine_min_cell above: the stub has no access to the fixture, and what makes the ORDER of the
// unlink and the member_count read testable is that the count really moves.
mh::ai::unit_group *g_unlink_group_cell = nullptr;

void st_grid_stamp_seeds(uint8_t *grid, int32_t w, int32_t h, uint8_t *stencil, int32_t span_x,
                         int32_t span_y, int32_t x, int32_t y, int32_t seed) {
    g_rec.seed_stamps.push_back({grid, w, h, stencil, span_x, span_y, x, y, seed});
}
void st_target_list_remove(int32_t player, uint32_t ref, int32_t index) {
    g_rec.target_removes.push_back({player, ref, index});
}
void st_queue_reconcile_bldg_change(uint32_t player, uint32_t bldg_index) {
    g_rec.bldg_reconciles.push_back({player, bldg_index});
}
// FAITHFUL TO THE ORIGINAL, and deliberately so: llm_strat_ai_group_member_unlink is what
// DECREMENTS ai_groups[group].member_count (the DEC at 0x004d4a22) --
// llm_strat_ai_notify_object_removed does not, whatever its plate said before 2026-08-05. The
// body under test reads member_count AFTER this call, so a translation that read it BEFORE
// would see a value one higher and disband a different set of groups. Without this decrement
// that error is untestable offline.
void st_group_member_unlink(uint32_t player, int32_t group, uint32_t unit_index) {
    g_rec.unlinks.push_back({(int32_t)player, group, (int32_t)unit_index});
    if (g_unlink_group_cell) --g_unlink_group_cell->member_count;
}
// COMPACTS, when a cell is bound. llm_strat_ai_group_remove moves the LAST group into the freed
// slot and decrements ai_group_count, and llm_strat_ai_unit_group_tick's disband pass depends on
// exactly that: it restarts the sweep after every removal *because* the indices shift. A
// record-only stub would leave the reaped group in place, and the pass would remove it forever --
// so this stub is what makes the restart testable AND what stops the test hanging.
player_data *g_group_remove_owner = nullptr;

void st_group_remove(int32_t player, uint32_t group) {
    g_rec.group_removes.push_back({player, group});
    // Also logged into slice2_log (RI-AI batch E) so player_tick's group-loop test can see
    // this call INTERLEAVED with group_redistribute_units in the order the loop actually made them --
    // the double-increment bug is a property of that interleaving, not of either call alone.
    g_rec.log2("group_remove(player=%d,group=%u)", player, group);
    if (g_group_remove_owner) {
        player_data &pd = *g_group_remove_owner;
        if (pd.ai_group_count > 0) {
            const int32_t last = pd.ai_group_count - 1;
            if ((int32_t)group != last) pd.ai_groups[group] = pd.ai_groups[last];
            std::memset(&pd.ai_groups[last], 0, sizeof(unit_group));
            --pd.ai_group_count;
        }
    }
}

// ---- batch C layer 1: the unit-group task machine ------------------------------------------
//
// The group record whose active_flag the ACTIVATE stub sets, or null for record-only. Same
// arrangement as g_unlink_group_cell above and for the same reason: the stub cannot see the
// fixture, and what makes the re-entry edges testable is that the flag really moves.
player_data *g_task_owner = nullptr;

void st_group_task_activate(int32_t player, int32_t group) {
    g_rec.activates.push_back({player, group, 0u});
    if (g_rec.activate_clear_count > 0) {
        --g_rec.activate_clear_count; // leave the flag clear -- the post-activate re-entry edge
        return;
    }
    if (g_task_owner) g_task_owner->ai_groups[group].active_flag = 1;
}

uint32_t st_group_task_step(uint32_t player, int32_t group) {
    uint32_t r = 0;
    if (!g_rec.step_results.empty()) {
        r = g_rec.step_results.front();
        // The tail value repeats, so a test that wants "always 0" needs one entry, not N.
        if (g_rec.step_results.size() > 1) g_rec.step_results.erase(g_rec.step_results.begin());
    }
    g_rec.steps.push_back({(int32_t)player, group, r});
    return r;
}

// ---- batch C layer 2: the group TASK MACHINE's own seams -------------------------------------
//
// The nineteen activation arms are indistinguishable except by which dispatch-table slot reached
// them, so they share one recording stub parameterised by the arm's CANONICAL TASK CODE. That is
// what makes the 25-entry table testable as a table: one loop over codes 0..0x18 asserting the id.
// Three arms are shared by more than one code (3/0x0c -> advance_to_anchor, 0x0f/0x10/0x11 ->
// muster_from_pool), and the id is the lowest code that reaches them.
template <int CODE>
void st_task_arm(int32_t player, int32_t group) {
    g_rec.task_arms.push_back({player, group, (uint32_t)CODE});
}
template <int CODE>
void st_task_arm_u(uint32_t player, int32_t group) {
    g_rec.task_arms.push_back({(int32_t)player, group, (uint32_t)CODE});
}
// The two genuine no-op stubs in the image take no arguments at all, so they cannot record a
// player/group -- -1 marks that, and it is also the assertion that the argument-less thunk was used.
template <int CODE>
void st_task_arm_void() {
    g_rec.task_arms.push_back({-1, -1, (uint32_t)CODE});
}

void st_group_task_dequeue(int32_t player, int32_t group) {
    g_rec.dequeues.push_back({player, group, 0u});
}
int32_t st_dock_slot_is_busy(int32_t player, int32_t slot) {
    g_rec.docks.push_back({player, slot, (uint32_t)g_rec.dock_busy});
    return g_rec.dock_busy;
}
int32_t st_check_arrival_status(uint32_t player, int32_t group) {
    g_rec.preds.push_back({(int32_t)player, group, 1u});
    return g_rec.pred_result;
}
int32_t st_check_unit_near_centroid(int32_t player, int32_t group) {
    g_rec.preds.push_back({player, group, 2u});
    return g_rec.pred_result;
}
int32_t st_all_units_settled(int32_t player, int32_t group) {
    g_rec.preds.push_back({player, group, 3u});
    return (uint8_t)g_rec.pred_result;
}
int32_t st_area_scan_hostile(uint32_t player, int32_t group, uint8_t owner_mask) {
    // The owner_mask IS the recorded value: codes 6 and 7 differ only in it (0x40 vs 0xc0), so a
    // swap between them is invisible to anything that records only "a hostile scan happened".
    g_rec.preds.push_back({(int32_t)player, group, (uint32_t)owner_mask});
    return g_rec.pred_result;
}

// ---- batch C layer 2: group formation, hold, target seeding ---------------------------------
//
// group_create ANSWERS FROM A QUEUE and does not touch the fixture. The originals stamp the
// returned index themselves (goal, active_member_count) and the two pool formers do so WITHOUT
// checking for -1, so a stub that allocated a real slot would hide exactly the behaviour under
// test; what the tests assert is where the stamp landed, which needs the answer to be chosen.
int32_t st_group_create(int32_t player_id) {
    g_rec.create_players.push_back(player_id);
    int32_t r = 0;
    if (!g_rec.create_results.empty()) {
        r = g_rec.create_results.front();
        if (g_rec.create_results.size() > 1) g_rec.create_results.erase(g_rec.create_results.begin());
    }
    return r;
}

void st_group_task_enqueue(int32_t player, int32_t group, uint16_t task_code, uint32_t p4,
                           uint32_t p5, uint32_t p6, uint32_t p7, uint32_t p8, uint16_t p9) {
    g_rec.task_pushes.push_back({false, player, group, (int32_t)task_code, (int32_t)p4, (int32_t)p5,
                                 (int32_t)p6, (int32_t)p7, (int32_t)p8, (int32_t)p9});
}

// The FRONT-INSERT sibling. Recorded into the same list with `preempt` set, so a test that expects
// a preempt and gets an append fails on the flag rather than passing on the arguments.
void st_group_task_preempt(int32_t player, int32_t group, int16_t task_code, int32_t p4, int32_t p5,
                           int32_t p6, int32_t p7, int32_t p8, int16_t p9) {
    g_rec.task_pushes.push_back(
        {true, player, group, (int32_t)task_code, p4, p5, p6, p7, p8, (int32_t)p9});
}

void st_scan_target_list_add(int32_t player, uint32_t target_ref, int32_t target_index) {
    g_rec.scan_target_adds.push_back({player, target_ref, target_index});
}

// ---- llm_strat_ai_group_member_move -------------------------------------------------------------
//
// THIS STUB HAS TO REALLY MOVE THE UNIT, and that is not a nicety: both callers that need it drain a
// group with `while (src.member_count != 0) move(src.head_unit)`, RE-READING the head every
// iteration. A record-only stub does not make those tests weak, it makes them HANG -- the loop's
// only exit is the state this call is supposed to change. (Same argument as st_group_task_activate's
// active_flag and st_grid_clear's real bit clear.)
//
// What it models is the HEAD case only, because that is the only case either original uses: every
// call site passes the source group's own head_unit. A caller that unlinked from the middle would
// silently get the head unlinked instead -- acceptable while no such caller exists, and asserted
// below rather than left implicit.
player_data *g_member_move_owner = nullptr; // the player row whose groups really change
unit        *g_member_move_units = nullptr; // that player's unit ROW (units + p*100), or null

void st_group_member_move(uint32_t player, int32_t src, int32_t dst, int32_t unit_id) {
    g_rec.member_moves.push_back({player, src, dst, unit_id});
    // A NULL owner IS A TEST-SETUP ERROR, NOT A "record-only" MODE, and it has to be reported here
    // rather than tolerated. Both callers drain with `while (src.member_count != 0)`, so a stub that
    // changed nothing would spin forever appending to the vector -- which is not a hang that reads
    // as one: it exits 0xC0000409 with NO output at all, because stdout is still buffered. (Measured
    // 2026-08-05: the first draft of test_invasion_launch did exactly this and the whole aitest mode
    // vanished with zero lines printed.) Exiting names the cause in the one place that knows it.
    if (!g_member_move_owner) {
        printf("FAIL st_group_member_move: g_member_move_owner is null -- the caller's drain loop "
               "has no exit. Set it (and g_member_move_units) before invoking a body that moves "
               "group members.\n");
        fflush(stdout);
        std::exit(2);
    }
    // THE RUNAWAY NET, and it earns its place: a drain loop whose source count never falls does not
    // fail, it consumes memory until /GS kills the process at 0xC0000409 with the ENTIRE mode's
    // output still sitting in the stdout buffer -- so the symptom is "aitest printed nothing",
    // which names neither the test nor the reason. Two distinct fixture mistakes produced exactly
    // that on 2026-08-05 (a null owner, above; and a create_results queue left empty, so
    // st_group_create answered 0 and the body drained group 0 INTO group 0, where the decrement and
    // the increment cancel). Both are now a named failure instead of a silent kill.
    if (g_rec.member_moves.size() > 100000) {
        printf("FAIL st_group_member_move: %zu moves -- the caller's drain loop is not converging "
               "(src=%d dst=%d; identical src/dst makes member_count stationary).\n",
               g_rec.member_moves.size(), src, dst);
        fflush(stdout);
        std::exit(2);
    }
    player_data &pd = *g_member_move_owner;
    unit_group  &s  = pd.ai_groups[src];
    unit_group  &d  = pd.ai_groups[dst];
    if (g_member_move_units && s.head_unit == (uint16_t)unit_id)
        s.head_unit = g_member_move_units[unit_id].ai_group_next;
    else if (s.head_unit == (uint16_t)unit_id)
        s.head_unit = 0;
    if (s.member_count > 0) --s.member_count;
    ++d.member_count;
    if (g_member_move_units) {
        g_member_move_units[unit_id].ai_group_next  = d.head_unit;
        g_member_move_units[unit_id].ai_group_index = (uint16_t)dst;
        d.head_unit                                 = (uint16_t)unit_id;
    }
}

// ---- batch C layer 2: llm_strat_ai_group_redistribute_units' other four callees ----------------
//
// The centroid stub WRITES THROUGH ITS OUT-POINTERS rather than only recording, for the same reason
// st_grid_clear does the real bit clear: the body forwards what it receives straight into
// unit_flag_and_move, so a test whose stub left the locals at 0 could not tell an x/y swap from a
// correct pass-through. g_rec.centroid_out_x/_y are deliberately DIFFERENT values in every test.
// ---- batch C layer 3 (2026-08-06) ------------------------------------------------------
// Neither stub touches the scratch list. That is deliberate: the real callees READ it, and a stub
// that also cleared or rewrote it would hide a harvest that published the wrong contents.
void st_group_move_formation_rotating(uint32_t player, int32_t centroid_x, int32_t centroid_y,
                                      int32_t target_x, int32_t target_y) {
    g_rec.formation_moves.push_back({player, centroid_x, centroid_y, target_x, target_y});
}

void st_group_scatter_to_passable_tile(uint32_t player, int32_t x, int32_t y) {
    g_rec.scatter_calls.push_back({player, x, y});
}

void st_group_compute_centroid(int32_t player, int32_t group, uint32_t *out_x, uint32_t *out_y) {
    g_rec.centroid_calls.push_back({player, group});
    if (out_x) *out_x = (uint32_t)g_rec.centroid_out_x;
    if (out_y) *out_y = (uint32_t)g_rec.centroid_out_y;
}
void st_unit_flag_and_move(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y) {
    g_rec.flag_moves.push_back({player, unit_index, x, y});
}
void st_group_split_off_create(uint32_t player, int32_t cx, int32_t cy, int32_t src_group,
                               uint32_t member_count) {
    g_rec.split_offs.push_back({player, cx, cy, src_group, member_count});
}
void st_group_split_excess_members(uint32_t player, int32_t group_idx) {
    g_rec.split_excess.push_back({player, group_idx});
}

// ---- RI-AI batch B/C, 2026-08-07: pick_owned_tile_or_home / random_point_near's RNG+trig callees --
//
// Deterministic stand-ins, not real math -- what these two functions need tested is the WIRING (the
// channel-2 draw happens, the angle reaches both trig calls unchanged, the result is masked/added
// correctly), not sin/cos accuracy. Plain globals rather than g_rec fields: nothing else in this file
// calls any of the four.
int32_t g_rand_below_ai_ans               = 0;
double  g_rand_state_advance_ans          = 0.0;
double  g_fsin_ans                        = 0.0;
double  g_cos_ans                         = 0.0;
double  g_fsin_last_angle                 = 0.0;
double  g_cos_last_angle                  = 0.0;
int32_t g_rand_below_ai_last_range        = -1;
int32_t g_rand_state_advance_last_channel = -1;

int32_t st_rand_below_ai(uint32_t range) {
    g_rand_below_ai_last_range = (int32_t)range;
    return g_rand_below_ai_ans;
}
double st_rand_state_advance(int32_t channel) {
    g_rand_state_advance_last_channel = channel;
    return g_rand_state_advance_ans;
}
double st_math_fsin_reduce_loop(double angle) {
    g_fsin_last_angle = angle;
    return g_fsin_ans;
}
double st_math_cos_impl(double angle) {
    g_cos_last_angle = angle;
    return g_cos_ans;
}

// ---- RI-AI batch E, 2026-08-07: start_hq_attack_scenario's three callees + scr_parse's file/keyword
// callees ---------------------------------------------------------------------------------------
uint32_t st_unit_create_soldier(uint32_t x, uint32_t y, uint16_t a2, uint16_t param_4,
                                char param_5) {
    g_rec.create_soldier_calls.push_back({x, y, a2, param_4, param_5});
    return g_rec.create_soldier_ans;
}
void st_diplomacy_set_relation(int32_t player_a, int32_t player_b, uint8_t relation) {
    g_rec.diplomacy_calls.push_back({player_a, player_b, relation});
}
void st_unit_commit_attack_on_enemy_hq() { ++g_rec.commit_attack_on_enemy_hq_calls; }

// ---- RI-AI batch E, 2026-08-07: player_tick / active_unit_tick / unit_commit_attack_on_
// enemy_hq / invasion_spawn_reinforcements / create_reinforcement_unit's outward calls. Every one
// of these logs into g_rec.slice2_log (see the recorder member comment) because the SEQUENCE across
// different callees is what these two dispatchers' tests are actually checking.
int32_t st2_get_starting_unit(uint32_t race) {
    g_rec.log2("GetStartingUnit(race=%u)", race);
    return g_rec.starting_unit_ans;
}
uint32_t st2_unit_create(uint32_t x, uint32_t y, uint16_t unit, uint16_t player, uint8_t is_ship) {
    g_rec.create_unit_calls.push_back({x, y, unit, player, is_ship});
    g_rec.log2("unit_create(x=%u,y=%u,unit=%u,player=%u,is_ship=%u)", x, y, unit, player, is_ship);
    return g_rec.unit_create_ans;
}
uint32_t st2_bldg_max_defense_radius_sq(int32_t player, int32_t x, int32_t y) {
    g_rec.log2("bldg_max_defense_radius_sq(player=%d,x=%d,y=%d)", player, x, y);
    return g_rec.defense_radius_sq_ans;
}
void st2_resource_spend_rate_update(int32_t player) { g_rec.log2("resource_spend_rate_update(%d)", player); }
void st2_update_opponent_relations(uint32_t                                       assessed_player,
                                   mh::game::mh_llm_strat_ai_opponent_assessment *out) {
    g_rec.opponent_relation_calls.push_back({assessed_player, (void *)out});
    g_rec.log2("update_opponent_relations(assessed=%u)", assessed_player);
}
void st2_recompute_shortage_state(uint32_t player) { g_rec.log2("recompute_shortage_state(%u)", player); }
void st2_plan_unit_training(int32_t player) { g_rec.log2("plan_unit_training(%d)", player); }
void st2_plan_construction(uint32_t player) { g_rec.log2("plan_construction(%u)", player); }
void st2_scan_bldg_repair_upgrade(int32_t player) { g_rec.log2("scan_bldg_repair_upgrade(%d)", player); }
void st2_order_collect_available_projects_thunk(int32_t player) {
    g_rec.log2("order_collect_available_projects_thunk(%d)", player);
}
void st2_bldg_queue_process(uint32_t player) { g_rec.log2("bldg_queue_process(%u)", player); }
void st2_group_home_guard_replenish(int32_t player) { g_rec.log2("group_home_guard_replenish(%d)", player); }
void st2_group_form_patrol(int32_t player) { g_rec.log2("group_form_patrol(%d)", player); }
void st2_group_form_surplus_from_pool4(int32_t player) {
    g_rec.log2("group_form_surplus_from_pool4(%d)", player);
}
void st2_group_form_standby_from_pool3(int32_t player) {
    g_rec.log2("group_form_standby_from_pool3(%d)", player);
}
void st2_group_expansion_form_or_repurpose(uint32_t player) {
    g_rec.log2("group_expansion_form_or_repurpose(%u)", player);
}
void st2_army_milestone_advance_or_attack(uint32_t player) {
    g_rec.log2("army_milestone_advance_or_attack(%u)", player);
}
void st2_group_redistribute_units(uint32_t player, int32_t group_index) {
    g_rec.log2("group_redistribute_units(player=%u,group=%d)", player, group_index);
}
void st2_invasion_launch_attack_group(uint32_t player) {
    g_rec.log2("invasion_launch_attack_group(%u)", player);
}
void st2_unit_order_move_with_bump(uint32_t player, int32_t unit_idx, uint32_t order_arg0,
                                   uint32_t order_arg1) {
    g_rec.log2("unit_order_move_with_bump(player=%u,unit=%d,x=%u,y=%u)", player, unit_idx, order_arg0,
               order_arg1);
}
int32_t st2_invasion_spawn_reinforcements(int32_t player) {
    g_rec.log2("invasion_spawn_reinforcements(%d)", player);
    return 0;
}
void st2_score_build_categories(int32_t player) { g_rec.log2("score_build_categories(%d)", player); }

// ---- active_unit_tick's new callees ----
int32_t st2_holding_pen_scan_targets(int32_t player, uint32_t pen_index) {
    g_rec.log2("holding_pen_scan_targets(pen=%u)", pen_index);
    return 0;
}
int32_t st2_target_list_invalidate_by_id(int32_t player, int32_t target_id) {
    g_rec.log2("target_list_invalidate_by_id(id=%d)", target_id);
    return 0;
}
int32_t st2_target_list_refresh_mothers(int32_t player) {
    g_rec.log2("target_list_refresh_mothers()");
    return 0;
}
int32_t st2_target_list_scan_visible_enemies(uint32_t player) {
    g_rec.log2("target_list_scan_visible_enemies()");
    return 0;
}
uint8_t st2_group_seed_resolved_target(int32_t player, int32_t group_index) {
    g_rec.log2("group_seed_resolved_target(group=%d)", group_index);
    return 0;
}
void st2_attack_candidate_add(int32_t player, int32_t unit_index) {
    g_rec.log2("attack_candidate_add(unit=%d)", unit_index);
}
int32_t st2_building_defense_weapon_range(int32_t player, int32_t building_index) {
    g_rec.log2("building_defense_weapon_range(bldg=%d)", building_index);
    return g_rec.building_weapon_range_ans;
}
void st2_scan_target_list_sort(void *base, uint32_t count) {
    g_rec.log2("scan_target_list_sort(count=%u)", count);
}
void st2_group_enter_hold(int32_t player, int32_t group_index) {
    g_rec.log2("group_enter_hold(group=%d)", group_index);
}
int32_t st2_unit_should_abandon_target(uint32_t player, int32_t unit_index) {
    g_rec.log2("unit_should_abandon_target(unit=%d)", unit_index);
    auto it = g_rec.abandon_by_unit.find(unit_index);
    return it != g_rec.abandon_by_unit.end() ? it->second : 0;
}
uint32_t st2_unit_is_order_pending(uint32_t player, uint32_t unit_id) {
    g_rec.log2("unit_is_order_pending(unit=%u)", unit_id);
    auto it = g_rec.pending_by_unit.find((int32_t)unit_id);
    return it != g_rec.pending_by_unit.end() ? it->second : 0u;
}
// OFFLINE-ORACLE, 2026-08-07 -- group_compute_centroid's per-member gate (CALL @0x004d5cb7). A
// controllable stub, not a logged one: the test that drives it needs to place idle members exactly
// where they separate a correct reading from a wrong one, not merely see that it was called.
int32_t st3_unit_is_idle_or_parked(int32_t player, int32_t unit_index) {
    auto it = g_rec.idle_by_unit.find(unit_index);
    return it != g_rec.idle_by_unit.end() ? it->second : 0;
}
void st2_unit_issue_default_order(uint32_t player, int32_t unit_index) {
    g_rec.log2("unit_issue_default_order(unit=%d)", unit_index);
}

// ---- unit_commit_attack_on_enemy_hq's three callees ----
uint8_t st2_unit_select_weapon(uint16_t player, int32_t unit_index, uint32_t target_mask) {
    g_rec.log2("unit_select_weapon(player=%u,unit=%d,mask=%u)", player, unit_index, target_mask);
    return g_rec.select_weapon_ans;
}
void st2_unit_order_attack_building_reposition(uint32_t player, int32_t unit_index, uint32_t target_ref,
                                               int32_t target_index, uint32_t weapon_id) {
    g_rec.attack_reposition_calls.push_back({player, unit_index, target_ref, target_index, weapon_id});
}
void st2_unit_order_move(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y, uint32_t arg5) {
    g_rec.order_move_calls.push_back({player, unit_index, x, y, arg5});
}

// ---- the reinforcement family's gc-routed leaves (invasion_spawn_reinforcements's view of them) ----
uint32_t st2_score_reinforcement_unit(int32_t player, int32_t unit_proto_id) {
    g_rec.score_calls.push_back({player, unit_proto_id});
    auto it = g_rec.score_by_proto.find(unit_proto_id);
    return it != g_rec.score_by_proto.end() ? it->second : g_rec.score_default;
}
void st2_create_reinforcement_unit(uint32_t x, uint32_t y, uint32_t unit_proto_id, uint16_t player) {
    g_rec.reinforcement_spawns.push_back({x, y, unit_proto_id, player});
}
// scr_parse's file-access + CRT-adjacent callees. Backed by REAL semantics (a fixed in-memory
// "resource file", real malloc/free, real case-insensitive strcmp) rather than g_rec playback --
// the point of testing scr_parse is the PARSE ALGORITHM (tokenizing, keyword dispatch, type-1/2
// conversion), which only exercises meaningfully against a real byte stream.
const char *g_scr_file_contents = nullptr;
size_t      g_scr_file_size     = 0;
int32_t     st_asset_size(const char *name) {
    (void)name;
    return (int32_t)g_scr_file_size;
}
// SIMABI-VFS: `asset_read` COPIES into the caller's buffer instead of handing back the host's
// allocation, so this stub no longer has to pretend a static buffer is heap the callee may free.
int32_t st_asset_read(const char *name, void *dst, uint32_t dst_cap) {
    (void)name;
    const uint32_t len = (uint32_t)g_scr_file_size;
    const uint32_t n   = len < dst_cap ? len : dst_cap;
    if (dst_cap != 0 && n != 0) std::memcpy(dst, g_scr_file_contents, n);
    return (int32_t)len;
}
void   *st_utils_malloc(uint32_t size) { return std::malloc(size); }
void    st_utils_free(void *p) { std::free(p); }
int32_t st_utils_str_cmp_ci(char *a, char *b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

// spiral_table_init's gc.qsort. The passed `compare` is a raw, never-decompiled game address (see
// the ai_calls::spiral_offset_sort_cmp comment) that this offline harness cannot execute, so the
// stub ignores it and sorts with a REAL comparator matching the documented behaviour (ascending by
// squared radius, ties broken by original order for a deterministic test) -- exercising the SAME
// downstream ring-boundary logic real qsort output would, without depending on undecompiled bytes.
void st_qsort(void *base, uint32_t num, uint32_t width, void *compare) {
    (void)compare;
    if (width != 2) return; // only the spiral_offset (dx,dy int8 pair) shape is exercised here
    auto *arr = static_cast<mh::ai::spiral_offset *>(base);
    std::stable_sort(arr, arr + num, [](const mh::ai::spiral_offset &a, const mh::ai::spiral_offset &b) {
        return (int32_t)a.dx * a.dx + (int32_t)a.dy * a.dy < (int32_t)b.dx * b.dx + (int32_t)b.dy * b.dy;
    });
}

const ai_calls &stub_calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.engage_candidate_add                   = &st_add;
        t.unit_has_ground_weapon                 = &st_ground;
        t.unit_has_aa_weapon                     = &st_aa;
        t.target_ref_is_alive                    = &st_alive;
        t.engage_sort_candidates_by_dist         = &st_sort;
        t.engage_partition_turret_candidates     = &st_partition;
        t.commit_attack_order                    = &st_commit;
        t.commit_attack_order_alt                = &st_commit_alt;
        t.order_attack_building_enqueue          = &st_enq_bldg;
        t.order_attack_unit_enqueue              = &st_enq_unit;
        t.bldg_find_by_ai_build_and_type         = &st_find;
        t.grid_clear_threat_bit                  = &st_grid_clear;
        t.grid_stamp_threat_ring                 = &st_grid_stamp;
        t.bldg_is_alive                          = &st_bldg_is_alive;
        t.unit_get_ai_group_index                = &st_group_index;
        t.unit_get_sight                         = &st_sight;
        t.unit_max_weapon_range                  = &st_weapon_range;
        t.scan_spiral_ring_for_engage_candidates = &st_spiral;
        t.toroidal_dist_sq                       = &st_toroidal;
        // ---- batch A layer 3's two packed-ref predicates ----
        t.target_ref_has_ground_weapon = &st_ref_ground;
        t.target_ref_has_aa_weapon     = &st_ref_aa;
        // ---- batch A layer 4 ----
        t.bldg_has_aa_weapon = &st_bldg_aa;
        // ---- batch B layer 1 ----
        t.grid_fill_below_threshold        = &st_grid_fill_below;
        t.grid_flood_step                  = &st_grid_flood;
        t.queue_flush_unit_train_entries_2 = &st_queue_flush;
        t.bldg_total_resource_cost         = &st_total_cost;
        t.bldg_queue_construction          = &st_queue_construction;
        // ---- batch B layer 2 (the rest of it) ----
        t.count_unit_build_sources            = &st_count_sources;
        t.queue_train_unit                    = &st_queue_train;
        t.order_grant_resource_raw            = &st_grant_resource;
        t.bldg_queue_handle_recruit_state     = &st_q_recruit;
        t.bldg_queue_process_entry            = &st_q_process_entry;
        t.bldg_queue_handle_state2_empty      = &st_q_state2;
        t.bldg_queue_handle_upgrade_or_cancel = &st_q_upgrade;
        // ---- batch B layer 3, the four dispatch arms ----
        t.bldg_find_idle_producer_for_unit         = &st_find_producer;
        t.order_recruit_unit_enqueue               = &st_recruit_order;
        t.bldg_order_production_add_enqueue        = &st_production_add;
        t.econ_track_unit_resource_spend           = &st_econ_track;
        t.bldg_production_type_dispatch            = &st_type_dispatch;
        t.notify_map_changed                       = &st_notify_map;
        t.notify_map_changed_2                     = &st_notify_map_2;
        t.footprint_scan_for_blocked_cell          = &st_footprint;
        t.order_queue_construction_enqueue         = &st_construction_order;
        t.bldg_instant_construct_find_slot_enqueue = &st_instant_order;
        t.bldg_record_resource_expenditure_stats   = &st_expenditure;
        t.order_population_delta_enqueue           = &st_population_delta;
        t.bldg_order_upgrade_enqueue               = &st_upgrade_order;
        t.bldg_order_repair_cycle_start_enqueue    = &st_cancel_order;
        // ---- batch B layer 3, the build-category / housing planners ----
        t.bldg_count_by_id                = &st_count_by_id;
        t.bldg_count_by_type              = &st_count_by_type;
        t.bldg_count_by_category          = &st_count_by_category;
        t.bldg_type_already_queued        = &st_already_queued;
        t.bldg_type_queue_has_pending     = &st_queue_has_pending;
        t.bldg_has_heli_unit              = &st_has_heli_unit;
        t.bldg_side_has_aircraft_producer = &st_has_aircraft_producer;
        // ---- batch B layer 3, the mine planner ----
        t.queue_rotate_newest_to_front = &st_queue_rotate;
        t.mine_portfolio_rebalance     = &st_mine_rebalance;
        // ---- batch B layer 3, the turret / worker planners ----
        t.bldg_connectivity_flood_fill            = &st_flood_fill;
        t.find_nearest_flagged_building           = &st_find_nearest_flagged;
        t.tile_midpoint_wrapped                   = &st_tile_midpoint;
        t.bldg_order_restart_construction_enqueue = &st_restart_construction;
        t.bldg_uses_workers                       = &st_uses_workers;
        t.is_worker_priority_candidate            = &st_worker_priority;
        t.bldg_order_activate_enqueue             = &st_activate_order;
        t.bldg_order_assign_workers_enqueue       = &st_assign_workers;
        t.bldg_order_unassign_workers_enqueue     = &st_unassign_workers;
        // ---- batch B layer 4 ----
        t.bldg_scan_resource_site_candidates          = &st_scan_resource_sites;
        t.scan_build_site_candidates                  = &st_scan_build_sites;
        t.bldg_scan_grid_candidates                   = &st_scan_grid_sites;
        t.sort_site_candidates_by_dist                = &st_sort_site_candidates;
        t.unit_order_auto_launch_from_storage_enqueue = &st_auto_launch;
        // ---- batch B layer 4/5, the mine portfolio ----
        t.calc_mine_yield_estimate = &st_calc_mine_yield;
        // ---- batch C layer 0, the object-removed notification hook ----
        t.grid_stamp_seeds               = &st_grid_stamp_seeds;
        t.target_list_remove             = &st_target_list_remove;
        t.queue_reconcile_bldg_change    = &st_queue_reconcile_bldg_change;
        t.group_member_unlink            = &st_group_member_unlink;
        t.group_remove                   = &st_group_remove;
        t.queue_flush_unit_train_entries = &st_train_flush;
        // ---- batch C layer 1, the unit-group task machine ----
        t.group_task_activate = &st_group_task_activate;
        t.group_task_step     = &st_group_task_step;
        // ---- batch C layer 2, group formation / hold / target seeding ----
        t.group_create         = &st_group_create;
        t.group_task_enqueue   = &st_group_task_enqueue;
        t.group_task_preempt   = &st_group_task_preempt;
        t.scan_target_list_add = &st_scan_target_list_add;
        t.group_member_move    = &st_group_member_move;
        // ---- batch C layer 2, the redistribute closure ----
        t.group_split_off_create     = &st_group_split_off_create;
        t.group_compute_centroid     = &st_group_compute_centroid;
        t.unit_flag_and_move         = &st_unit_flag_and_move;
        t.group_split_excess_members = &st_group_split_excess_members;
        // ---- batch C layer 2, the task machine ----
        t.group_task_rally_formup            = &st_task_arm<0x02>;
        t.group_task_advance_to_anchor       = &st_task_arm<0x03>;
        t.group_task_disperse_passable       = &st_task_arm_u<0x04>;
        t.group_task_patrol_shuttle          = &st_task_arm<0x05>;
        t.group_task_nudge_stragglers        = &st_task_arm_u<0x06>;
        t.group_task_wait                    = &st_task_arm_void<0x07>;
        t.group_task_recall_home             = &st_task_arm<0x08>;
        t.group_task_recruit_from_storage    = &st_task_arm_u<0x09>;
        t.group_task_hold                    = &st_task_arm_void<0x0b>;
        t.group_task_scatter_random          = &st_task_arm<0x0d>;
        t.group_task_loiter_wander           = &st_task_arm<0x0e>;
        t.group_task_muster_from_pool        = &st_task_arm_u<0x0f>;
        t.group_task_recruit_from_pool3      = &st_task_arm_u<0x12>;
        t.group_task_recruit_from_pool4      = &st_task_arm_u<0x13>;
        t.group_task_disband                 = &st_task_arm_u<0x14>;
        t.group_task_attack_nearest_defended = &st_task_arm<0x15>;
        t.group_task_engage_target           = &st_task_arm<0x16>;
        t.group_task_drain_reserve_attack    = &st_task_arm_u<0x17>;
        t.group_task_attack_random_target    = &st_task_arm<0x18>;
        t.group_task_dequeue                 = &st_group_task_dequeue;
        t.dock_slot_is_busy                  = &st_dock_slot_is_busy;
        t.group_check_arrival_status         = &st_check_arrival_status;
        t.group_check_unit_near_centroid     = &st_check_unit_near_centroid;
        t.group_area_scan_hostile            = &st_area_scan_hostile;
        t.group_all_units_settled            = &st_all_units_settled;
        // ---- batch C layer 3 (2026-08-06) ----
        t.group_move_formation_rotating  = &st_group_move_formation_rotating;
        t.group_scatter_to_passable_tile = &st_group_scatter_to_passable_tile;
        // ---- RI-AI batch B/C, 2026-08-07 ----
        t.rand_below_ai         = &st_rand_below_ai;
        t.rand_state_advance    = &st_rand_state_advance;
        t.math_fsin_reduce_loop = &st_math_fsin_reduce_loop;
        t.math_cos_impl         = &st_math_cos_impl;
        // ---- RI-AI batch E, 2026-08-07 ----
        t.unit_create_soldier            = &st_unit_create_soldier;
        t.diplomacy_set_relation         = &st_diplomacy_set_relation;
        t.unit_commit_attack_on_enemy_hq = &st_unit_commit_attack_on_enemy_hq;
        t.asset_size                     = &st_asset_size;
        t.asset_read                     = &st_asset_read;
        t.utils_malloc                   = &st_utils_malloc;
        t.utils_free                     = &st_utils_free;
        t.utils_str_cmp_ci               = &st_utils_str_cmp_ci;
        t.spiral_offset_sort_cmp         = (void *)1; // never dereferenced by our code -- see qsort stub
        t.qsort                          = &st_qsort;
        // ---- RI-AI batch E, 2026-08-07 ----
        t.GetStartingUnit                        = &st2_get_starting_unit;
        t.unit_create                            = &st2_unit_create;
        t.bldg_max_defense_radius_sq             = &st2_bldg_max_defense_radius_sq;
        t.resource_spend_rate_update             = &st2_resource_spend_rate_update;
        t.update_opponent_relations              = &st2_update_opponent_relations;
        t.recompute_shortage_state               = &st2_recompute_shortage_state;
        t.plan_unit_training                     = &st2_plan_unit_training;
        t.plan_construction                      = &st2_plan_construction;
        t.scan_bldg_repair_upgrade               = &st2_scan_bldg_repair_upgrade;
        t.order_collect_available_projects_thunk = &st2_order_collect_available_projects_thunk;
        t.bldg_queue_process                     = &st2_bldg_queue_process;
        t.group_home_guard_replenish             = &st2_group_home_guard_replenish;
        t.group_form_patrol                      = &st2_group_form_patrol;
        t.group_form_surplus_from_pool4          = &st2_group_form_surplus_from_pool4;
        t.group_form_standby_from_pool3          = &st2_group_form_standby_from_pool3;
        t.group_expansion_form_or_repurpose      = &st2_group_expansion_form_or_repurpose;
        t.army_milestone_advance_or_attack       = &st2_army_milestone_advance_or_attack;
        t.group_redistribute_units               = &st2_group_redistribute_units;
        t.invasion_launch_attack_group           = &st2_invasion_launch_attack_group;
        t.unit_order_move_with_bump              = &st2_unit_order_move_with_bump;
        t.holding_pen_scan_targets               = &st2_holding_pen_scan_targets;
        t.target_list_invalidate_by_id           = &st2_target_list_invalidate_by_id;
        t.target_list_refresh_mothers            = &st2_target_list_refresh_mothers;
        t.target_list_scan_visible_enemies       = &st2_target_list_scan_visible_enemies;
        t.group_seed_resolved_target             = &st2_group_seed_resolved_target;
        t.attack_candidate_add                   = &st2_attack_candidate_add;
        t.building_defense_weapon_range          = &st2_building_defense_weapon_range;
        t.scan_target_list_sort                  = &st2_scan_target_list_sort;
        t.group_enter_hold                       = &st2_group_enter_hold;
        t.unit_select_weapon                     = &st2_unit_select_weapon;
        t.unit_order_attack_building_reposition  = &st2_unit_order_attack_building_reposition;
        t.unit_order_move                        = &st2_unit_order_move;
        t.score_reinforcement_unit               = &st2_score_reinforcement_unit;
        t.create_reinforcement_unit              = &st2_create_reinforcement_unit;
        t.invasion_spawn_reinforcements          = &st2_invasion_spawn_reinforcements;
        t.unit_should_abandon_target             = &st2_unit_should_abandon_target;
        t.unit_is_order_pending                  = &st2_unit_is_order_pending;
        t.unit_issue_default_order               = &st2_unit_issue_default_order;
        t.score_build_categories                 = &st2_score_build_categories;
        // OFFLINE-ORACLE, 2026-08-07 -- previously unwired (nullptr), so any translated body that
        // called it would have crashed rather than been silently misjudged.
        t.unit_is_idle_or_parked = &st3_unit_is_idle_or_parked;
        return t;
    }();
    return c;
}

// Index a recorder list SAFELY, returning a zeroed record when the index is out of range.
//
// Most assertions here guard their own index with a `size() == N &&` first conjunct, but where a
// size check and the field checks are separate `ck` calls the index is unguarded -- and a mutation
// that SUPPRESSES a call leaves the list empty, so the unguarded `[0]` faults and takes the whole
// transcript down with it. tools/mutate.py can only score that as MISSED, which reads as "the
// assertion was too weak" when in fact several assertions had already failed by name. That is
// exactly what happened to the "footprint result read at face value" mutation on 2026-08-02.
template <class T>
const T &at(const std::vector<T> &v, size_t i) {
    static const T none{};
    return i < v.size() ? v[i] : none;
}

// Packed refs used throughout. Class nibble 2 => unit (0x20), class nibble 4 => building (0x40).
uint32_t unit_ref(uint32_t owner) { return 0x20u | owner; }
uint32_t bldg_ref(uint32_t owner) { return 0x40u | owner; }

// ---- 1. the target-list producer ----------------------------------------------------------------
void test_scan_target_list() {
    fixture f;
    g_rec.clear();
    const int me = 1;

    player_data &pd = f.players[me];
    // relations: hostile to 3, friendly to 2, self-friendly.
    for (int i = 0; i < MAX_PLAYERS; ++i) pd.ai_player_relation[i] = (i == 3) ? -1 : 1;

    auto entry = [&](int slot, int32_t target_id, uint32_t kind, int32_t aggressor_ref, int32_t pos) {
        pd.ai_target_list[slot].victim_index    = target_id;
        pd.ai_target_list[slot].victim_ref      = kind;
        pd.ai_target_list[slot].aggressor_ref   = aggressor_ref;
        pd.ai_target_list[slot].aggressor_index = pos;
    };
    // 0: everything right -> offered
    entry(0, 10, 0x20, 3, 1111);
    f.u(me, 10).ai_group_index = 0x0004;
    // 1: victim_ref misses 0xa0 -> rejected
    entry(1, 11, 0x10, 3, 2222);
    f.u(me, 11).ai_group_index = 0x0004;
    // 2: group mask does not match -> rejected
    entry(2, 12, 0x80, 3, 3333);
    f.u(me, 12).ai_group_index = 0x0001;
    // 3: owner is FRIENDLY (relation +1) -> rejected by the sign test
    entry(3, 13, 0x80, 2, 4444);
    f.u(me, 13).ai_group_index = 0x0004;
    // 4: relation exactly 0 -> also rejected; the test is `<= -1`, not `<= 0`
    entry(4, 14, 0x80, 5, 5555);
    f.u(me, 14).ai_group_index = 0x0004;
    pd.ai_player_relation[5]   = 0;
    pd.ai_target_list_count    = 5;

    const ai_view v = f.view();
    const int32_t n = detail::scan_target_list_for_engage_candidates(v, stub_calls(), me, 0x0004);

    ck(n == 1, "scan: exactly one of the five entries is offered");
    ck(g_rec.adds.size() == 1, "scan: exactly one candidate_add call");
    if (g_rec.adds.size() == 1) {
        // MUTATION-CHECKED: swapping aggressor_ref and position at the call site (an easy slip, since
        // both are int32 fields of the same record) fails exactly these two.
        ck(g_rec.adds[0].ref == 3, "scan: the offered ref is aggressor_ref, not the target id");
        ck(g_rec.adds[0].index == 1111, "scan: the offered index is `position`");
    }

    // The relation test is a SIGN test, and 0 is not hostile -- checked above via entry 4. Now the
    // other side: make it -1 and the same entry IS offered, so the previous rejection was the
    // relation and not some other filter silently doing the work.
    g_rec.clear();
    pd.ai_player_relation[5] = -1;
    ck(detail::scan_target_list_for_engage_candidates(v, stub_calls(), me, 0x0004) == 2,
       "scan CONTROL: flipping relation[5] from 0 to -1 admits entry 4 -- so it really was the "
       "hostility test that rejected it");

    // A zero count offers nothing and must not read entry 0 anyway.
    g_rec.clear();
    pd.ai_target_list_count = 0;
    ck(detail::scan_target_list_for_engage_candidates(v, stub_calls(), me, 0xffff) == 0 &&
           g_rec.adds.empty(),
       "scan: an empty target list offers nothing");
}

// ---- 2. the filter + swap-remove ----------------------------------------------------------------
void test_filter_and_swap_remove() {
    fixture   f;
    const int me = 2;

    // Four candidates; make every one survivable so the pick loop stops at index 0 and this case is
    // about the FILTER alone.
    auto build = [&]() {
        f.scratch_count = 0;
        f.push(unit_ref(3), 10); // 0x23: 0x20 set -> needs AA
        f.push(bldg_ref(4), 11); // 0x44: 0x40 set -> needs a ground weapon (0xc0 test)
        f.push(unit_ref(5), 12);
        f.push(bldg_ref(6), 13);
        for (int i = 0; i < MAX_PLAYERS; ++i)
            for (int j = 0; j < 20; ++j) {
                f.u(i, j).energy = 100.0;
                f.b(i, j).energy = 100.0;
            }
    };

    // (a) no AA weapon -> every 0x20-flagged candidate is removed.
    g_rec.clear();
    build();
    g_rec.aa = 0;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 0, 0);
    ck(f.scratch_count == 2, "filter: without an AA weapon both 0x20 candidates are dropped");
    ck(f.scratch[0].target_ref == bldg_ref(4) || f.scratch[1].target_ref == bldg_ref(4),
       "filter: the building candidates survive");

    // (b) no ground weapon -> every 0xc0-flagged candidate is removed. Note 0x40 alone satisfies
    // the 0xc0 test, which is why the building refs are the ones that go.
    g_rec.clear();
    build();
    g_rec.ground = 0;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 0, 0);
    ck(f.scratch_count == 2, "filter: without a ground weapon both 0x40 candidates are dropped");

    // (c) liveness removes the LAST entry -- the branch where the swap-remove must NOT copy, since
    // the victim already is the last. A naive `scratch[i] = scratch[count-1]` before decrementing is
    // still correct here, but a naive `scratch[i] = scratch[count]` is not, and neither is a version
    // that copies after decrementing. This is the case that separates them.
    g_rec.clear();
    build();
    g_rec.dead_ref   = bldg_ref(6);
    g_rec.dead_index = 13;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 0, 0);
    ck(f.scratch_count == 3, "filter: a dead last entry is removed");
    ck(f.scratch[0].target_ref == unit_ref(3) && f.scratch[1].target_ref == bldg_ref(4) &&
           f.scratch[2].target_ref == unit_ref(5),
       "filter: removing the LAST entry leaves the other three in their original order -- the "
       "swap-remove must not copy anything when the victim is already last");

    // (d) liveness removes a MIDDLE entry -> the last is swapped into its slot, so the order
    // changes. This is the observable difference between swap-remove and a shifting erase.
    g_rec.clear();
    build();
    g_rec.dead_ref   = unit_ref(3);
    g_rec.dead_index = 10;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 0, 0);
    ck(f.scratch_count == 3, "filter: a dead first entry is removed");
    ck(f.scratch[0].target_ref == bldg_ref(6),
       "filter: the LAST entry is swapped into the hole -- not a shifting erase, which would have "
       "left bldg_ref(4) at slot 0");

    // (e) everything dead -> the list empties and nothing is committed.
    g_rec.clear();
    build();
    f.scratch_count  = 1;
    g_rec.dead_ref   = unit_ref(3);
    g_rec.dead_index = 10;
    const bool r     = detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 0, 0);
    ck(!r && f.scratch_count == 0 && g_rec.commits.empty(),
       "filter: with every candidate dead the list empties and no order is committed");
    ck(g_rec.sorts == 0 && g_rec.partitions == 0,
       "filter: an emptied list short-circuits BEFORE the sort -- the original re-tests the count");
}

// ---- 3. the survivability pick, and its two traps ------------------------------------------------
void test_pick_survivable() {
    fixture   f;
    const int me = 1;

    // One unit candidate whose incoming threat EXCEEDS its energy -> not survivable -> skipped.
    // One building candidate that is fine -> picked.
    g_rec.clear();
    f.push(unit_ref(3), 10);
    f.push(bldg_ref(4), 11);
    f.u(3, 10).energy                 = 10.0;
    f.u(3, 10).incoming_threat_damage = 50;
    f.b(4, 11).energy                 = 100.0;
    f.b(4, 11).incoming_damage_tally  = 5;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 7, 0);
    ck(g_rec.commits.size() == 1 && g_rec.commits[0].target_ref == bldg_ref(4),
       "pick: a unit already taking more damage than it has energy is skipped");

    // THE MOVZX TRAP. A NEGATIVE int16 tally is loaded zero-extended, so -1 reads as 65535 and the
    // candidate is REJECTED. Casting the signed field straight to double would make it -1.0, which
    // is below any positive energy, and the candidate would be picked -- the opposite decision.
    g_rec.clear();
    f.reset();
    f.push(unit_ref(3), 10);
    f.push(bldg_ref(4), 11);
    f.u(3, 10).energy                 = 100.0;
    f.u(3, 10).incoming_threat_damage = (int16_t)-1;
    f.b(4, 11).energy                 = 100.0;
    f.b(4, 11).incoming_damage_tally  = 5;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 7, 0);
    ck(g_rec.commits.size() == 1 && g_rec.commits[0].target_ref == bldg_ref(4),
       "pick: a NEGATIVE damage tally is read UNSIGNED (65535) and rejects the candidate -- a "
       "signed cast would have picked it instead");

    // THE POLARITY TRAP, and the input that actually separates the two predicates.
    //
    // The pick loop chooses the roster with (ref & 0x40) != 0 -> BUILDING; the liveness helper, the
    // sort and the commit branch use (ref & 0xa0) == 0 -> BUILDING. THE OBVIOUS TEST INPUT DOES NOT
    // DISCRIMINATE: for a plain class-4 ref (0x4X) both answer "building", so swapping one predicate
    // for the other changes nothing and the mutation passes. Measured 2026-08-01 -- the first version
    // of this check used bldg_ref() and was GREEN against exactly that mutant, which is the
    // "assertion that cannot fail for the reason it names" anti-pattern in the reimpl-loop skill.
    //
    // The two disagree on class nibble 0 and on 0xc0. 0xc0 is the honest choice: it is a real value
    // (it is the mask the weapon filter tests) and it survives the filter with a ground weapon.
    //   (0xc0 & 0x40) != 0 -> BUILDING   <- correct
    //   (0xc0 & 0xa0) == 0 is FALSE      -> unit
    // So the building slot is made survivable and the unit slot at the same index is not: reading
    // the wrong roster commits nothing.
    g_rec.clear();
    f.reset();
    const uint32_t both_bits = 0xc0u | 2u;
    f.push(both_bits, 9);
    f.b(2, 9).energy                 = 100.0;
    f.b(2, 9).incoming_damage_tally  = 1;
    f.u(2, 9).energy                 = 1.0;
    f.u(2, 9).incoming_threat_damage = 999;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 7, 0);
    ck(g_rec.commits.size() == 1,
       "pick: a 0xc0 candidate is scored against BUILDINGS by the (ref & 0x40) test -- using the "
       "pipeline's other predicate (ref & 0xa0) would score it against UNITS, find it unsurvivable "
       "and commit nothing");

    // Nothing survivable at all -> no commit, and the count is UNCHANGED (the pick loop does not
    // remove anything, unlike the filter above).
    g_rec.clear();
    f.reset();
    f.push(unit_ref(3), 10);
    f.u(3, 10).energy                 = 1.0;
    f.u(3, 10).incoming_threat_damage = 999;
    const bool r                      = detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), me, 7, 0);
    ck(!r && g_rec.commits.empty() && f.scratch_count == 1,
       "pick: with nothing survivable no order is committed and the list is left intact");
}

// ---- 4. the two consumers' differences ----------------------------------------------------------
void test_consumer_differences() {
    fixture f;

    auto one_candidate = [&](uint32_t ref, int32_t idx) {
        f.reset();
        f.push(ref, idx);
        f.u(ref & 0xf, idx).energy = 100.0;
        f.b(ref & 0xf, idx).energy = 100.0;
    };

    // select_and_commit partitions turrets; filter_and_commit_target does NOT.
    g_rec.clear();
    one_candidate(unit_ref(3), 10);
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), 1, 7, 0);
    ck(g_rec.sorts == 1 && g_rec.partitions == 1,
       "select_and_commit sorts AND partitions turret candidates");

    // ---- THE CALL-SITE CONTRACT (REBIND-AI-ESI, 2026-09-10) --------------------------------------
    // The sort helper's third argument stands in for the ORIGINAL's ambient ESI at 0x004edd8e. It is
    // NOT free: llm_strat_unit_passive_engage_tick keeps the unit-slot index there (`MOV ESI,0x1`
    // 0x004ee559, `INC ESI` 0x004ee724, `MOV EDX,ESI` at each call), so the flag is the unit index
    // and is >= 1 at every real site. A zero would make the callee run the bubble sort the shipped
    // game never runs. Asserted on the VALUE, not merely on non-zeroness, so a caller that
    // hardcodes some other constant is also caught.
    ck(g_rec.last_sort_flag == 7,
       "select_and_commit forwards its unit_index as the inherited-ESI sort flag (0x004ee50e keeps "
       "the unit-slot index in ESI across the call at 0x004edd8e)");
    ck(g_rec.last_sort_flag != 0,
       "the inherited-ESI flag select_and_commit passes is NON-ZERO -- the shipped game never sorts "
       "the engage-candidate list at this site");

    g_rec.clear();
    one_candidate(unit_ref(3), 10);
    detail::engage_filter_and_commit_target(f.view(), f.store(), stub_calls(), 0x25, 7);
    ck(g_rec.sorts == 1 && g_rec.partitions == 0,
       "filter_and_commit_target sorts but does NOT partition -- adding the partition would change "
       "which target is picked whenever a turret is in range");

    // The sort is handed the attacker ref with bit 0x80 OR'd in ("the source is a unit").
    ck(g_rec.last_sort_ref == (0x25u | 0x80u) && g_rec.last_sort_index == 7,
       "the sort helper receives attacker_ref | 0x80");

    // Same call-site contract on the other site (original 0x004edf50, `MOV EDX,ESI` at 0x004ee71f).
    ck(g_rec.last_sort_flag == 7,
       "filter_and_commit_target forwards its context as the inherited-ESI sort flag");
    ck(g_rec.last_sort_flag != 0,
       "the inherited-ESI flag filter_and_commit_target passes is NON-ZERO -- no sort at this site "
       "either");

    // use_alt_commit routes to the alternate commit.
    g_rec.clear();
    one_candidate(unit_ref(3), 10);
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), 1, 7, 1);
    ck(g_rec.commits.size() == 1 && g_rec.commits[0].alt,
       "use_alt_commit != 0 routes to commit_attack_order_alt");
    g_rec.clear();
    one_candidate(unit_ref(3), 10);
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), 1, 7, 0);
    ck(g_rec.commits.size() == 1 && !g_rec.commits[0].alt, "...and 0 routes to the plain one");

    // filter_and_commit_target ENQUEUES an order instead, choosing the lane by (ref & 0xa0) -- the
    // OTHER predicate, on the same value the pick loop just tested with 0x40.
    g_rec.clear();
    one_candidate(unit_ref(3), 10);
    const int32_t rv = detail::engage_filter_and_commit_target(f.view(), f.store(), stub_calls(), 0x1234, 7);
    ck(rv == 1 && g_rec.enqueues.size() == 1, "filter_and_commit_target enqueues one order");
    if (g_rec.enqueues.size() == 1) {
        const auto &e = g_rec.enqueues[0];
        ck(!e.building, "a 0x20 candidate takes the attack-UNIT lane ((ref & 0xa0) != 0)");
        // MUTATION-CHECKED against the decompile's version of this argument, which renders the two
        // branches differently ((ref & 0xff) & 0xffffff0f vs ref & 0xf) although the disassembly
        // emits identical code. With a class nibble present, `ref & 0xff` would be 0x23, not 3.
        ck(e.target_owner == 3,
           "the enqueued target owner is the OWNER NIBBLE (ref & 0xf), both branches -- the "
           "decompile's asymmetric rendering of this argument is an artifact");
        ck(e.player == 0x1234, "the attacker is passed as the low 16 bits of attacker_ref");
        ck(e.unit_index == 7, "...and `context` is passed through unchanged");
        ck(e.weapon_id == 4, "weapon id 4 (`PUSH 0x4`), both branches");
    }

    g_rec.clear();
    one_candidate(bldg_ref(5), 11);
    detail::engage_filter_and_commit_target(f.view(), f.store(), stub_calls(), 0x1234, 7);
    ck(g_rec.enqueues.size() == 1 && g_rec.enqueues[0].building && g_rec.enqueues[0].target_owner == 5,
       "a 0x40 candidate takes the attack-BUILDING lane, with the same owner-nibble argument");

    // An empty list is a no-op for both, and must not call the sort.
    g_rec.clear();
    f.reset();
    ck(detail::engage_filter_and_commit_target(f.view(), f.store(), stub_calls(), 0x1234, 7) == 0 &&
           g_rec.sorts == 0,
       "an empty candidate list returns 0 without sorting");
}

// ---- 5. the build-candidate slots, and the overrun ----------------------------------------------
void test_build_candidates() {
    fixture   f;
    const int me = 3;

    g_rec.clear();
    f.players[me].is_alien_race = 0; // human
    detail::init_build_candidate_priorities(f.view(), f.store(), stub_calls(), me);

    ck(g_rec.finds.size() == 21, "build: exactly 21 lookups");
    if (g_rec.finds.size() == 21) {
        // The (category, race-selected type) pairs, in the original's order. Written out rather than
        // spot-checked: a table this mechanical is exactly where a transcription slip hides, and
        // every one of the 21 is a different building the AI would or would not construct.
        static const uint32_t CAT[21]   = {1, 2, 3, 0x40, 0x41, 0x42, 0x43, 0x22, 0x23, 0x21, 0x20,
                                           5, 4, 0x31, 0x30, 0x50, 0x51, 0x10, 0x11, 0x12, 0x13};
        static const uint32_t HUMAN[21] = {0x1a, 0x18, 0x17, 0x19, 0x19, 0x19, 0x19, 0x1e, 0x1d,
                                           0x1b, 0x1c, 0x23, 0x24, 0x1f, 0x1f, 0x16, 0x16, 0x15,
                                           0x15, 0x15, 0x15};
        int                   bad       = 0;
        for (int i = 0; i < 21; ++i) {
            if (g_rec.finds[i].ai_build != CAT[i] || g_rec.finds[i].type != HUMAN[i] ||
                g_rec.finds[i].player != me)
                ++bad;
        }
        ck(bad == 0, "build: all 21 (category, human type) pairs match, in order");
    }

    // Slot destinations. The stub returns 1000, 1001, ... in call order, so each slot's value names
    // the lookup it came from and a swapped pair is visible instead of plausible.
    const player_data &me_rec = f.players[me];
    ck(me_rec.ai_mother_building_type == 1000, "build: slot 0 -> ai_mother_building_type");
    ck(me_rec.ai_build_candidate_primary == 1001, "build: slot 1 -> ai_build_candidate_primary");
    ck(me_rec.ai_build_candidate_secondary == 1002, "build: slot 2 -> ai_build_candidate_secondary");
    ck(me_rec.ai_mine_alt_candidates[0] == 1003 && me_rec.ai_mine_alt_candidates[3] == 1006,
       "build: slots 3-6 -> ai_mine_alt_candidates[0..3]");
    ck(me_rec.ai_turret_candidate == 1011, "build: slot 11 -> ai_turret_candidate");
    ck(me_rec.ai_build_candidate_shortage == 1012, "build: slot 12 -> ai_build_candidate_shortage");
    ck(me_rec.ai_build_candidate_cat_0x31 == 1013 && me_rec.ai_build_candidate_cat_0x30 == 1014,
       "build: slots 13-14 -> the two cat_0x3x candidates, 0x31 BEFORE 0x30");
    ck(me_rec.ai_mine_candidate_tier1 == 1015 && me_rec.ai_mine_candidate_tier2 == 1016,
       "build: slots 15-16 -> the two mine tiers");
    ck(me_rec.ai_resource_shortage_candidates[0] == 1017 &&
           me_rec.ai_resource_shortage_candidates[3] == 1020,
       "build: slots 17-20 -> ai_resource_shortage_candidates[0..3]");

    // THE OVERRUN, which is the whole reason this function is in the pilot. The four housing slots
    // land in the NEXT player's record, and the current player's own four are untouched.
    const player_data &next = f.players[me + 1];
    ck(next.ai_housing_candidate_heli == 1007 && next.ai_housing_candidate_plane == 1008 &&
           next.ai_housing_candidate_vehicle == 1009 && next.ai_housing_candidate_soldier == 1010,
       "build: the four housing slots land in players[player + 1] -- PRESERVED, not fixed");
    ck(me_rec.ai_housing_candidate_heli == 0 && me_rec.ai_housing_candidate_plane == 0 &&
           me_rec.ai_housing_candidate_vehicle == 0 && me_rec.ai_housing_candidate_soldier == 0,
       "build: ...and the acting player's OWN housing slots are left untouched, which is what makes "
       "the aliasing observable rather than merely harmless");

    // Player 7 writes past the end of the real 8-element array. The fixture's ninth record is that
    // memory; the assertion is that the write still happens rather than being clamped away.
    g_rec.clear();
    f.reset();
    f.players[7].is_alien_race = 0;
    detail::init_build_candidate_priorities(f.view(), f.store(), stub_calls(), 7);
    ck(f.players[8].ai_housing_candidate_heli == 1007,
       "build: for player 7 the overrun lands OUTSIDE the 8-element array, exactly as the original "
       "does (scan_raw_pointers reports that address TOTAL-ORPHAN .bss)");

    // The race switch. Every one of the 21 type constants must change.
    g_rec.clear();
    f.reset();
    f.players[me].is_alien_race = 1;
    detail::init_build_candidate_priorities(f.view(), f.store(), stub_calls(), me);
    static const uint32_t ALIEN[21] = {6, 4, 3, 5, 5, 5, 5, 10, 9, 7, 8,
                                       0xf, 0x10, 0xb, 0xb, 2, 2, 1, 1, 1, 1};
    int                   bad       = 0;
    for (int i = 0; i < 21 && i < (int)g_rec.finds.size(); ++i)
        if (g_rec.finds[i].type != ALIEN[i]) ++bad;
    ck(g_rec.finds.size() == 21 && bad == 0,
       "build: is_alien_race != 0 selects the alien type constant at all 21 lookups");
}

// ---- 6. the view binds through the region registry -----------------------------------------------
//
// The rest of this file drives `detail::` over heap buffers, which proves the LOGIC and says nothing
// about the binding. This case is about state(): that it resolves through mh::state and re-resolves
// per call, so a rebased region moves what the view returns. The companion assertion lives in
// `statetest`, which owns the rebase fixture.
void test_view_binding() {
    using namespace mh::state;
    const ai_state before = state();
    ck((uint32_t)(uintptr_t)before.read.players == base_of(RID_PLAYER_DATA) &&
           (uint32_t)(uintptr_t)before.read.units == base_of(RID_UNITS) &&
           (uint32_t)(uintptr_t)before.read.buildings == base_of(RID_BUILDINGS),
       "binding: on an unrebased build the view returns each region's stock base");
    ck((void *)before.read.players == (void *)before.own.players,
       "binding: the read view and the store address the SAME player_data -- the split is about "
       "what may be written, not about two copies of the state");
}

// ---- 7. the target-list pair (batch A layer 1) ---------------------------------------------------
void test_scan_targets_for_engage() {
    fixture f;
    g_rec.clear();
    const int me = 1;

    player_data &pd = f.players[me];
    for (int i = 0; i < MAX_PLAYERS; ++i) pd.ai_player_relation[i] = (i == 3) ? -1 : 1;
    auto entry = [&](int slot, int32_t target_id, uint32_t kind, int32_t aggressor_ref, int32_t pos) {
        pd.ai_target_list[slot].victim_index    = target_id;
        pd.ai_target_list[slot].victim_ref      = kind;
        pd.ai_target_list[slot].aggressor_ref   = aggressor_ref;
        pd.ai_target_list[slot].aggressor_index = pos;
    };
    entry(0, 10, 0x20, 3, 1111); // matches on every axis -> offered
    entry(1, 10, 0x10, 3, 2222); // victim_ref misses 0xa0 -> rejected
    entry(2, 99, 0x80, 3, 3333); // a DIFFERENT target id -> rejected
    entry(3, 10, 0x80, 2, 4444); // owner friendly (relation +1) -> rejected
    pd.ai_target_list_count = 4;

    const ai_view v = f.view();
    const int32_t n = detail::scan_targets_for_engage(v, stub_calls(), me, 10);
    ck(n == 1, "scan_targets: exactly one of four entries is offered");
    ck(g_rec.adds.size() == 1, "scan_targets: exactly one candidate_add");
    if (g_rec.adds.size() == 1) {
        // MUTATION-CHECKED: swapping aggressor_ref and position at the call site fails these two.
        ck(g_rec.adds[0].ref == 3, "scan_targets: the offered ref is aggressor_ref");
        ck(g_rec.adds[0].index == 1111, "scan_targets: the offered index is `position`");
    }
    // CONTROL for the id filter: ask for 99 instead and a DIFFERENT entry is the one offered, so
    // the rejection above really was the id test and not some other filter doing the work.
    g_rec.clear();
    ck(detail::scan_targets_for_engage(v, stub_calls(), me, 99) == 1 && g_rec.adds.size() == 1 &&
           g_rec.adds[0].index == 3333,
       "scan_targets CONTROL: selecting id 99 offers entry 2, not entry 0");

    g_rec.clear();
    pd.ai_target_list_count = 0;
    ck(detail::scan_targets_for_engage(v, stub_calls(), me, 10) == 0 && g_rec.adds.empty(),
       "scan_targets: an empty list offers nothing");
}

void test_target_list_add() {
    fixture f;
    g_rec.clear();
    const int me = 1;

    ai_store     own = f.store();
    player_data &pd  = f.players[me];

    // (a) an object OWNED BY ME is never tracked -- the low nibble of aggressor_ref is the owner.
    detail::target_list_add(own, stub_calls(), me, 0x20, 5, 0x40u | (uint32_t)me, 777);
    ck(pd.ai_target_list_count == 0, "target_add: an object I own is not tracked");

    // (b) the ordinary append, with this player's AI DISABLED -> ai_group_index is written 0, not
    // left alone. Poison the slot first so "written 0" and "never written" are distinguishable.
    pd.ai_enabled                       = 0;
    pd.ai_target_list[0].ai_group_index = 0x5a5a5a5a;
    detail::target_list_add(own, stub_calls(), me, 0x20, 5, 0x43u, 777);
    ck(pd.ai_target_list_count == 1, "target_add: the count is bumped");
    ck(g_rec.group_lookups.empty(),
       "target_add: ai_enabled == 0 does NOT call unit_get_ai_group_index");
    if (pd.ai_target_list_count == 1) {
        const target_entry &e = pd.ai_target_list[0];
        // MUTATION-CHECKED: every one of these five is a distinct value, so a field written from the
        // wrong argument fails the specific assertion that names it rather than a generic one.
        ck(e.victim_index == 5, "target_add: target_id");
        ck(e.victim_ref == 0x20u, "target_add: the kind ARGUMENT lands in victim_ref");
        ck(e.ai_group_index == 0, "target_add: ai_group_index is explicitly ZEROED, not left stale");
        ck(e.aggressor_index == 777, "target_add: position");
        ck(e.aggressor_ref == 0x43u,
           "target_add: aggressor_ref is stored WHOLE, not masked to the nibble");
    }

    // (c) the dedup key is BOTH position and aggressor_ref -- neither alone.
    detail::target_list_add(own, stub_calls(), me, 0x20, 5, 0x43u, 777);
    ck(pd.ai_target_list_count == 1,
       "target_add: an exact (position, aggressor_ref) repeat is dropped");
    detail::target_list_add(own, stub_calls(), me, 0x20, 5, 0x43u, 778);
    ck(pd.ai_target_list_count == 2, "target_add: same aggressor_ref, DIFFERENT position -> appended");
    detail::target_list_add(own, stub_calls(), me, 0x20, 5, 0x53u, 777);
    ck(pd.ai_target_list_count == 3, "target_add: same position, DIFFERENT aggressor_ref -> appended");

    // (d) ai_enabled != 0 takes the lookup branch, and it is asked about (player, target_id).
    g_rec.clear();
    g_rec.group_index = 9;
    pd.ai_enabled     = 1;
    detail::target_list_add(own, stub_calls(), me, 0x20, 42, 0x43u, 900);
    ck(g_rec.group_lookups.size() == 1 && g_rec.group_lookups[0].player == me &&
           g_rec.group_lookups[0].unit_index == 42,
       "target_add: ai_enabled != 0 looks the group up by (player, target_id)");
    ck(pd.ai_target_list[3].ai_group_index == 9, "target_add: the looked-up group index is stored");

    // (e) THE CAP IS AN EQUALITY TEST at 64. Fill to exactly 64 and the next add is refused.
    pd.ai_target_list_count = 64;
    detail::target_list_add(own, stub_calls(), me, 0x20, 1, 0x43u, 5000);
    ck(pd.ai_target_list_count == 64, "target_add: a full (== 64) list refuses the append");
    // ... and at 63 it still appends, so the refusal is the cap and not an off-by-one earlier.
    pd.ai_target_list_count = 63;
    detail::target_list_add(own, stub_calls(), me, 0x20, 1, 0x43u, 5001);
    ck(pd.ai_target_list_count == 64, "target_add CONTROL: at 63 the append still happens");
}

// ---- 8. the turret threat rescan ------------------------------------------------------------------
void test_turret_threat_rescan() {
    fixture f;
    g_rec.clear();
    const uint32_t me = 1;

    f.active_players = 4;
    g_rec.map_w      = f.map_w;
    g_rec.map_h      = f.map_h;

    // Two weapon types with DISTINCT per-player ranges. This is the trap the site's own comment
    // names: range_max is subscripted by the SCANNED player, not the ticking one, and a fixture
    // with one range per weapon could not tell the two apart.
    const int W = 3;
    for (int p = 0; p < 9; ++p) f.cfg_weapons[W].range_max[p] = 10 + p;

    f.cfg_buildings[7].type      = BLDG_TYPE_A_TURRET;
    f.cfg_buildings[7].weapon_id = W;
    f.cfg_buildings[8].type      = BLDG_TYPE_H_TURRET;
    f.cfg_buildings[8].weapon_id = W;
    f.cfg_buildings[9].type      = BLDG_TYPE_A_TURRET;
    f.cfg_buildings[9].weapon_id = 0; // a turret with NO weapon -> no stamp
    f.cfg_buildings[5].type      = 2; // not a turret at all
    f.cfg_buildings[5].weapon_id = W;

    // Player 2's roster: slot 1 EMPTY, slot 2 a turret, slot 3 a non-turret, slot 4 a turret. The
    // live count is 3, so a count-driven walk must reach slot 4 by skipping the hole -- an
    // index-bounded walk over [1, count] would stop at 3 and never see it.
    auto put = [&](int p, int idx, uint16_t id, uint8_t x, uint8_t y) {
        f.b(p, idx).building_id = id;
        f.b(p, idx).x           = x;
        f.b(p, idx).y           = y;
    };
    f.b(2, 0).index = 3;
    put(2, 2, 7, 11, 12);
    put(2, 3, 5, 20, 21);
    put(2, 4, 8, 30, 31);
    // Player 3: one turret, and it is DEAD.
    f.b(3, 0).index = 1;
    put(3, 1, 7, 40, 41);
    g_rec.dead_bldg_owner = 3;
    g_rec.dead_bldg_index = 1;
    // Me: a turret of my own, which must be skipped because the scan excludes the ticking player.
    f.b(me, 0).index = 1;
    put(me, 1, 7, 50, 51);

    f.players[me].ai_turret_rescan_pending          = 1;
    f.players[me].ai_tile_flags_grid[(11 << 8) | 9] = TILE_FLAG_TURRET_THREAT; // stale, must clear

    detail::turret_threat_rescan(f.view(), f.store(), stub_calls(), me);

    ck(f.players[me].ai_turret_rescan_pending == 0, "rescan: the pending flag is consumed");
    ck(g_rec.grid_clears.size() == 1, "rescan: the grid is cleared exactly once");
    if (g_rec.grid_clears.size() == 1) {
        ck(g_rec.grid_clears[0].grid == (void *)&f.players[me].ai_tile_flags_grid[0],
           "rescan: the grid cleared is the TICKING player's");
        // MUTATION-CHECKED: a width/height swap fails here, which a square fixture could not see.
        ck(g_rec.grid_clears[0].w == f.map_w && g_rec.grid_clears[0].h == f.map_h,
           "rescan: clear is called (grid, width, height) in that order");
    }
    ck(f.players[me].ai_tile_flags_grid[(11 << 8) | 9] == 0,
       "rescan: the stale threat bit really was cleared");

    ck(g_rec.stamps.size() == 2, "rescan: exactly the two live enemy turrets are stamped");
    if (g_rec.stamps.size() == 2) {
        ck(g_rec.stamps[0].x == 11 && g_rec.stamps[0].y == 12 && g_rec.stamps[1].x == 30 &&
               g_rec.stamps[1].y == 31,
           "rescan: the sparse roster is walked by COUNT, so slot 4 is reached past the hole at 1");
        // THE SUBSCRIPT TRAP. Both turrets belong to player 2, so both radii must be
        // range_max[2] == 12 -- range_max[me] would be 11 and range_max[0] would be 10.
        ck(g_rec.stamps[0].radius == 12 && g_rec.stamps[1].radius == 12,
           "rescan: the radius is Weapon[].range_max[the TURRET OWNER], not [the ticking player]");
        ck(g_rec.stamps[0].grid == (void *)&f.players[me].ai_tile_flags_grid[0],
           "rescan: the grid stamped is the ticking player's, not the scanned player's");
    }
    ck(f.players[me].ai_tile_flags_grid[(11 << 8) | 12] == TILE_FLAG_TURRET_THREAT,
       "rescan: the threat bit lands at (x << 8) | y");
    // Everything that must NOT have been stamped: my own turret, the dead one, the unarmed one,
    // and the non-turret. Two of those live in player 2's roster, so this also proves the type and
    // weapon_id tests are doing work rather than the count happening to come out right.
    ck(g_rec.stamps.size() == 2 && f.players[me].ai_tile_flags_grid[(50 << 8) | 51] == 0 &&
           f.players[me].ai_tile_flags_grid[(40 << 8) | 41] == 0 &&
           f.players[me].ai_tile_flags_grid[(20 << 8) | 21] == 0,
       "rescan: own turret, dead turret and non-turret are all skipped");

    // active_player_count is the bound, NOT MAX_PLAYERS: put a turret at player 5 and it stays
    // invisible while the count says 4.
    // NB: g_rec.clear() also forgets which building is dead, so re-arm it -- otherwise player 3's
    // corpse comes back to life and the count moves for a reason that has nothing to do with the
    // bound under test. (Cost one confusing failure while writing this.)
    g_rec.clear();
    g_rec.map_w           = f.map_w;
    g_rec.map_h           = f.map_h;
    g_rec.dead_bldg_owner = 3;
    g_rec.dead_bldg_index = 1;
    f.b(5, 0).index       = 1;
    put(5, 1, 7, 60, 61);
    detail::turret_threat_rescan(f.view(), f.store(), stub_calls(), me);
    ck(g_rec.stamps.size() == 2, "rescan: players at or past active_player_count are not scanned");
    f.active_players = 6;
    g_rec.clear();
    g_rec.map_w           = f.map_w;
    g_rec.map_h           = f.map_h;
    g_rec.dead_bldg_owner = 3;
    g_rec.dead_bldg_index = 1;
    detail::turret_threat_rescan(f.view(), f.store(), stub_calls(), me);
    ck(g_rec.stamps.size() == 3,
       "rescan CONTROL: raising active_player_count to 6 admits player 5's turret");
}

// ---- 9. the engage-scan trio ----------------------------------------------------------------------
void test_engage_partition() {
    fixture f;
    g_rec.clear();

    f.cfg_buildings[7].type = BLDG_TYPE_H_TURRET;
    f.cfg_buildings[5].type = 2;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        f.b(p, 1).building_id = 7; // a turret
        f.b(p, 2).building_id = 5; // an ordinary building
    }
    // Order: non-turret building, unit, turret, turret. Only the two turrets move to the front, in
    // their original relative order.
    f.push(bldg_ref(1), 2);
    f.push(unit_ref(1), 3);
    f.push(bldg_ref(2), 1);
    f.push(bldg_ref(3), 1);

    detail::engage_partition_turret_candidates(f.view(), f.store());
    ck(f.scratch_count == 4, "partition: nothing is added or removed");
    ck(f.scratch[0].target_ref == bldg_ref(2) && f.scratch[1].target_ref == bldg_ref(3),
       "partition: both turrets are moved to the front, keeping their relative order");
    ck(f.scratch[2].target_ref == bldg_ref(1) || f.scratch[3].target_ref == bldg_ref(1),
       "partition: the displaced entries are still present");

    // THE POLARITY. Class nibble 0 is where the two live roster tests DISAGREE: (0 & 0xa0) == 0
    // says BUILDING, while (0 & 0x40) == 0 says NOT a building. This function tests 0x40, so a
    // class-0 ref must NOT be partitioned to the front -- a body that reached for
    // ref_is_building_by_a0 instead would put it first.
    f.reset();
    g_rec.clear();
    f.cfg_buildings[7].type = BLDG_TYPE_H_TURRET;
    f.b(1, 1).building_id   = 7;
    f.push(0x01u, 1);
    f.push(bldg_ref(1), 1);
    detail::engage_partition_turret_candidates(f.view(), f.store());
    ck(f.scratch[0].target_ref == bldg_ref(1),
       "partition: a class-0 ref is NOT a building here -- this function tests 0x40, not 0xa0");

    f.reset();
    g_rec.clear();
    detail::engage_partition_turret_candidates(f.view(), f.store());
    ck(f.scratch_count == 0, "partition: an empty scratch is a no-op");
}

void test_scan_in_range() {
    fixture f;
    g_rec.clear();
    const int me = 1, unit_id = 4;
    f.u(me, unit_id).x = 21;
    f.u(me, unit_id).y = 34;

    // sight larger than weapon range -> sight wins.
    g_rec.sight        = 9;
    g_rec.weapon_range = 3;
    detail::unit_scan_engage_candidates_in_range(f.view(), stub_calls(), me, unit_id, 0xdeadbeefu);
    ck(g_rec.spirals.size() == 1, "scan_in_range: the spiral scanner is called once");
    if (g_rec.spirals.size() == 1) {
        ck(g_rec.spirals[0].ring_index == 9, "scan_in_range: radius = max(sight, weapon_range)");
        // MUTATION-CHECKED: x and y are different values on purpose, so a swap fails here.
        ck(g_rec.spirals[0].x == 21 && g_rec.spirals[0].y == 34,
           "scan_in_range: the unit's x goes to the x slot and y to the y slot");
        // The mask must survive as a full DWORD. The committed Ghidra prototype said `byte` until
        // 2026-08-01, and a truncating translation would deliver 0xef here.
        ck(g_rec.spirals[0].target_mask == 0xdeadbeefu,
           "scan_in_range: target_mask is passed whole, not truncated to a byte");
        ck(g_rec.spirals[0].player == me, "scan_in_range: the player is forwarded");
    }

    g_rec.clear();
    g_rec.sight        = 3;
    g_rec.weapon_range = 9;
    detail::unit_scan_engage_candidates_in_range(f.view(), stub_calls(), me, unit_id, 1);
    ck(g_rec.spirals.size() == 1 && g_rec.spirals[0].ring_index == 9,
       "scan_in_range: a larger weapon range wins");
}

void test_sort_by_dist() {
    fixture f;
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;

    // Source: a unit of player 1 at (0, 0). Three candidates at increasing distance, PUSHED IN
    // REVERSE so a body that never sorts leaves them provably out of order.
    f.u(1, 1).x = 0;
    f.u(1, 1).y = 0;
    auto tgt    = [&](int idx, uint8_t x, uint8_t y) {
        f.u(2, idx).x = x;
        f.u(2, idx).y = y;
    };
    tgt(1, 9, 0); // 81
    tgt(2, 5, 0); // 25
    tgt(3, 1, 0); // 1
    f.push(unit_ref(2), 1);
    f.push(unit_ref(2), 2);
    f.push(unit_ref(2), 3);

    // (a) THE BUG BRANCH: a NON-ZERO inherited flag means the sort phase never runs. Distances are
    // still written -- that phase is unconditional -- but the order is untouched.
    detail::engage_sort_candidates_by_dist(f.view(), f.store(), stub_calls(), unit_ref(1), 1, 1);
    ck(f.scratch[0].dist_sq == 81 && f.scratch[1].dist_sq == 25 && f.scratch[2].dist_sq == 1,
       "sort: the DISTANCE phase runs regardless of the inherited flag");
    ck(f.scratch[0].target_index == 1 && f.scratch[2].target_index == 3,
       "sort: with a NON-ZERO inherited ESI the sort phase is skipped entirely -- the original's "
       "uninitialised-register bug, reproduced deliberately");

    detail::engage_sort_candidates_by_dist(f.view(), f.store(), stub_calls(), unit_ref(1), 1, 0);
    ck(f.scratch[0].dist_sq == 1 && f.scratch[1].dist_sq == 25 && f.scratch[2].dist_sq == 81,
       "sort: with a ZERO inherited ESI the scratch is sorted ascending by dist_sq");
    ck(f.scratch[0].target_index == 3 && f.scratch[2].target_index == 1,
       "sort: the whole record travels with its distance, not just the dist_sq column");

    // (c) the SOURCE roster is selected by the 0xa0 test. Put a BUILDING of player 1 somewhere else
    // and address it with a class-4 ref: the distance must be measured from the BUILDING.
    f.reset();
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;
    f.b(1, 1).x = 20;
    f.b(1, 1).y = 0;
    f.u(2, 1).x = 24;
    f.u(2, 1).y = 0;
    f.push(unit_ref(2), 1);
    detail::engage_sort_candidates_by_dist(f.view(), f.store(), stub_calls(), bldg_ref(1), 1, 0);
    ck(f.scratch[0].dist_sq == 16,
       "sort: a class-4 source ref reads the BUILDING roster (0xa0 == 0), not units");

    // (d) the torus wraps: two tiles 62 apart on a 64-wide map are really 2 apart.
    f.reset();
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;
    f.u(1, 1).x = 1;
    f.u(1, 1).y = 0;
    f.u(2, 1).x = 63;
    f.u(2, 1).y = 0;
    f.push(unit_ref(2), 1);
    detail::engage_sort_candidates_by_dist(f.view(), f.store(), stub_calls(), unit_ref(1), 1, 0);
    ck(f.scratch[0].dist_sq == 4, "sort: distance is measured across the torus seam");
}

// The COMPOSITION of the two halves above (REBIND-AI-ESI, 2026-09-10). test_sort_by_dist proves the
// callee obeys whichever flag it is handed; test_engage_commit proves each caller hands over its own
// unit index. Neither alone answers the question the tracker item was opened for -- "does the list a
// real call site produces come out sorted or not" -- because the two are asserted in different
// fixtures with different stubs. This runs the caller to CAPTURE its flag and then feeds that exact
// captured value to the real callee over a reverse-ordered seeded list, so the end-to-end ordering
// is the thing under test rather than an inference across two arms.
//
// It is also the arm that reddens if anyone "fixes" the uninitialised-register bug by passing 0.
void test_sort_call_site_flag_composes() {
    fixture f;

    // ---- capture: what does the caller actually pass? -------------------------------------------
    g_rec.clear();
    f.reset();
    f.push(unit_ref(3), 10);
    f.u(3, 10).energy = 100.0;
    f.b(3, 10).energy = 100.0;
    detail::engage_select_and_commit(f.view(), f.store(), stub_calls(), 1, 7, 0);
    const int32_t site_flag = g_rec.last_sort_flag;
    ck(g_rec.sorts == 1 && site_flag == 7,
       "compose: the engage-commit site passes unit_index (7) as the inherited-ESI flag");

    // ---- replay: three candidates pushed in REVERSE distance order ------------------------------
    auto seed = [&]() {
        f.reset();
        g_rec.clear();
        g_rec.map_w = f.map_w;
        g_rec.map_h = f.map_h;
        f.u(1, 1).x = 0;
        f.u(1, 1).y = 0;
        for (int k = 1; k <= 3; ++k) {
            f.u(2, k).x = (uint8_t)(13 - 4 * k); // 9, 5, 1 -> dist_sq 81, 25, 1: DESCENDING
            f.u(2, k).y = 0;
            f.push(unit_ref(2), k);
        }
    };

    // (a) THE SHIPPED BEHAVIOUR: the captured call-site flag is non-zero, so zero passes run and the
    // producer's order survives untouched, farthest candidate still first.
    seed();
    detail::engage_sort_candidates_by_dist(f.view(), f.store(), stub_calls(), unit_ref(1), 1,
                                           site_flag);
    ck(f.scratch[0].target_index == 1 && f.scratch[1].target_index == 2 &&
           f.scratch[2].target_index == 3,
       "compose: with the REAL call site's flag the engage-candidate order is left EXACTLY as the "
       "producer built it -- zero bubble-sort passes, which is what the shipped game does");
    ck(f.scratch[0].dist_sq > f.scratch[2].dist_sq,
       "compose: and the distances prove it is genuinely unsorted, not accidentally in order");

    // (b) the counterfactual, so (a) cannot pass by the fixture being degenerate: the SAME list with
    // a zero flag does sort. This is the branch a wrong constant would silently switch the game to.
    seed();
    detail::engage_sort_candidates_by_dist(f.view(), f.store(), stub_calls(), unit_ref(1), 1, 0);
    ck(f.scratch[0].target_index == 3 && f.scratch[2].target_index == 1,
       "compose: the same seeded list with a ZERO flag sorts ascending -- so (a) is a real branch, "
       "not an inert fixture");
}

// ---- 10. the two thin layer-3 sites, paid off offline --------------------------------------------
//
// WHY THESE TWO ARE HERE. The layer-3 rig run reached target_ref_has_engageable_weapon exactly ONCE
// (flags 0x80 -> mode 1) and target_dist_sq ZERO times, which is a T2-in-name-only and a T3. Both are
// PURE and carry their whole verdict in the return value, so the honest cover is exhaustive input
// enumeration here rather than another scenario hunt: no game, no rig, milliseconds.
//
// Both assertion sets are derived from the DISASSEMBLY, not from the C++ under test
// (tmp/decomp_a3/*.asm) -- a fixture read off the implementation would prove consistency, not
// correctness.
void test_has_engageable_weapon() {
    // The mode selector, read off 0x004d3d00-0x004d3d16: `TEST BL,0x40 / JNZ mode1 ; TEST BL,0x20 /
    // JZ mode1 ; mode = 2`. Mode 2 requires 0x20 SET and 0x40 CLEAR; every other combination --
    // including NEITHER bit -- is mode 1, which is the same value as the first case and not a third
    // outcome. Bit 0 of the answer mask is ground, bit 1 is aa; the return is (mode & bits) != 0.
    struct mode_case {
        uint32_t    flags;
        uint32_t    mode;
        const char *why;
    };
    const mode_case cases[] = {
        {0x00u, 1u, "neither bit -> mode 1"},
        {0x20u, 2u, "0x20 alone -> mode 2"},
        {0x40u, 1u, "0x40 alone -> mode 1"},
        {0x60u, 1u, "0x40 WINS over 0x20 -- the test order is not symmetric"},
        {0x80u, 1u, "the one combination the rig actually reached"},
        {0xffu, 1u, "0x40 present among everything else -> still mode 1"},
    };

    for (const mode_case &m : cases) {
        for (int g = 0; g < 2; ++g) {
            for (int a = 0; a < 2; ++a) {
                g_rec.clear();
                g_rec.ref_ground = g;
                g_rec.ref_aa     = a;

                const uint8_t got =
                    detail::target_ref_has_engageable_weapon(stub_calls(), (int32_t)bldg_ref(3), 11,
                                                             m.flags);

                const uint32_t bits = (g ? 1u : 0u) | (a ? 2u : 0u);
                const uint8_t  want = (m.mode & bits) != 0 ? 1u : 0u;
                ck(got == want, m.why);

                // The call COUNT and ORDER are observable to the shadow oracle, so they are part of
                // the contract: BOTH predicates run, ground first, whatever `mode` is.
                ck(g_rec.weapon_preds.size() == 2 && g_rec.weapon_preds[0].which == 1 &&
                       g_rec.weapon_preds[1].which == 2,
                   "engageable: both weapon predicates are called unconditionally, ground first");
                ck(g_rec.weapon_preds[0].ref == bldg_ref(3) && g_rec.weapon_preds[0].index == 11 &&
                       g_rec.weapon_preds[1].ref == bldg_ref(3) &&
                       g_rec.weapon_preds[1].index == 11,
                   "engageable: the ref and index are forwarded UNCHANGED to both predicates");
            }
        }
    }

    // The pair that separates mode 1 from mode 2 on the same answers: ground-only capability is
    // engageable under mode 1 and NOT under mode 2. A test that only ever set both predicates the
    // same way could not tell the two modes apart at all.
    g_rec.clear();
    g_rec.ref_ground = 1;
    g_rec.ref_aa     = 0;
    ck(detail::target_ref_has_engageable_weapon(stub_calls(), 0, 0, 0x00u) == 1,
       "engageable: mode 1 (no bits) accepts a ground-only target");
    g_rec.clear();
    g_rec.ref_ground = 1;
    g_rec.ref_aa     = 0;
    ck(detail::target_ref_has_engageable_weapon(stub_calls(), 0, 0, 0x20u) == 0,
       "engageable: mode 2 REJECTS a ground-only target -- the mode bit is what selects");
}

void test_target_dist_sq() {
    fixture f;
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;

    // Sixteen distinct coordinates, none equal to another and none with x == y, over the 64 x 48
    // RECTANGULAR map: an argument-order slip, an x/y swap or a width/height swap all have to change
    // a recorded value rather than cancel out. (The campaign maps are 128 x 128 and could not
    // separate the last of those, which is why the fixture is not square.)
    f.b(3, 5).x  = 10;
    f.b(3, 5).y  = 21;
    f.u(2, 7).x  = 32;
    f.u(2, 7).y  = 43;
    f.b(1, 9).x  = 4;
    f.b(1, 9).y  = 15;
    f.u(6, 13).x = 26;
    f.u(6, 13).y = 37;

    struct combo {
        uint32_t    ref_a;
        int32_t     idx_a;
        uint32_t    ref_b;
        int32_t     idx_b;
        int32_t     ax, ay, bx, by;
        const char *what;
    };
    const combo combos[] = {
        {bldg_ref(3), 5, bldg_ref(1), 9, 10, 21, 4, 15, "dist_sq: building/building"},
        {bldg_ref(3), 5, unit_ref(2), 7, 10, 21, 32, 43, "dist_sq: building/unit"},
        {unit_ref(2), 7, bldg_ref(1), 9, 32, 43, 4, 15, "dist_sq: unit/building"},
        {unit_ref(6), 13, unit_ref(2), 7, 26, 37, 32, 43, "dist_sq: unit/unit"},
    };
    for (const combo &c : combos) {
        g_rec.clear();
        g_rec.map_w = f.map_w;
        g_rec.map_h = f.map_h;
        const uint32_t got =
            detail::target_dist_sq(f.view(), stub_calls(), c.ref_a, c.idx_a, c.ref_b, c.idx_b);
        ck(g_rec.toroidals.size() == 1 && g_rec.toroidals[0].x1 == c.ax &&
               g_rec.toroidals[0].y1 == c.ay && g_rec.toroidals[0].x2 == c.bx &&
               g_rec.toroidals[0].y2 == c.by,
           c.what);
        // The metric is the stub's, so this only pins that the RESULT is passed through rather than
        // recomputed or dropped -- the coordinates above are what the assertion above is for.
        ck(got == toroidal_metric(c.ax, c.ay, c.bx, c.by, f.map_w, f.map_h),
           "dist_sq: the callee's value is the return value");
    }

    // EACH OPERAND SELECTS ITS ROSTER INDEPENDENTLY, and by the 0x40 test -- NOT the 0xa0 one the
    // rest of batch A uses. Class nibble 0 is the only input that separates them: (0x03 & 0x40) == 0
    // says UNIT, (0x03 & 0xa0) == 0 says BUILDING. The rig would never have distinguished this.
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;
    f.u(3, 5).x = 51;
    f.u(3, 5).y = 7;
    detail::target_dist_sq(f.view(), stub_calls(), 0x03u, 5, bldg_ref(1), 9);
    ck(g_rec.toroidals.size() == 1 && g_rec.toroidals[0].x1 == 51 && g_rec.toroidals[0].y1 == 7,
       "dist_sq: a CLASS-NIBBLE-0 ref reads the UNIT roster (the 0x40 test), not the building one");

    // The owner nibble is read PER OPERAND, so two refs with different owners must reach two
    // different roster rows -- a body that resolved both through one owner would still look right on
    // every same-owner input.
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;
    detail::target_dist_sq(f.view(), stub_calls(), unit_ref(6), 13, unit_ref(2), 7);
    ck(g_rec.toroidals.size() == 1 && g_rec.toroidals[0].x1 == 26 && g_rec.toroidals[0].x2 == 32,
       "dist_sq: each ref supplies its OWN owner nibble");

    // The coordinate fields are `byte` in both rosters and the original reads them with MOVZX. A
    // sign-extending translation turns 200 into -56 and the torus wrap hides it in the distance --
    // which is why the assertion is on the RECORDED ARGUMENT and not on the result.
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;
    f.u(2, 7).x = 200;
    f.u(2, 7).y = 130;
    detail::target_dist_sq(f.view(), stub_calls(), unit_ref(2), 7, unit_ref(6), 13);
    ck(g_rec.toroidals.size() == 1 && g_rec.toroidals[0].x1 == 200 && g_rec.toroidals[0].y1 == 130,
       "dist_sq: coordinates above 127 are ZERO-extended, as the original's MOVZX does");
}

// ---- 11. batch A layer 4: the two holes the rig run left --------------------------------------
//
// The layer-4 arming run (12000 steps on campaign save 4-saibel-4, 0 divergences) reached every one
// of its five sites, and the DISTRIBUTIONS said the counts were lying about two of them:
//
//   target_ref_is_alive        86000 calls, 3362 building / 638 unit in the traced prefix -- T1
//   target_ref_has_ground_wpn    400+ calls, 471 building / 4 unit                        -- T1
//   target_ref_has_aa_weapon        1 call,  UNIT only; the fall-through half never ran   -- T2
//   is_worker_priority_cand     21800 calls, the four-way match hit ZERO times, so the
//                                     production-queue scan -- most of the function -- never ran
//   target_dist_sq                  1 call                                               -- T2
//
// Both holes are cheap to close here and expensive to close on the rig (they need a specific mid-game
// world), so this is where they get closed. Everything below is derived from the .asm, not from the
// C++: the inclusive scan bound, the inverted else-path polarity and the masked-vs-unmasked hand-off
// are the three things a plausible-looking translation gets wrong.
void test_target_ref_predicates() {
    fixture f;

    // ---- is_alive: the empty-slot sentinel and the float compare, per roster ----
    // The two rosters test DIFFERENT fields for "slot empty" (building_id vs unit_proto_id), which is
    // the pairing a copy-paste between the branches would break.
    g_rec.clear();
    ck(detail::target_ref_is_alive(f.view(), bldg_ref(3), 5) == 0,
       "is_alive: an empty BUILDING slot (building_id == 0) is dead");
    ck(detail::target_ref_is_alive(f.view(), unit_ref(3), 5) == 0,
       "is_alive: an empty UNIT slot (unit_proto_id == 0) is dead");

    f.b(3, 5).building_id    = 7;
    f.b(3, 5).energy         = 100.0;
    f.b(3, 5).pending_damage = 40.0;
    ck(detail::target_ref_is_alive(f.view(), bldg_ref(3), 5) == 1,
       "is_alive: a BUILDING with energy above its pending damage is alive");
    f.b(3, 5).pending_damage = 100.0;
    ck(detail::target_ref_is_alive(f.view(), bldg_ref(3), 5) == 0,
       "is_alive: energy EQUAL to pending damage is dead -- the compare is strict (JNC, not JBE)");
    f.b(3, 5).pending_damage = 140.0;
    ck(detail::target_ref_is_alive(f.view(), bldg_ref(3), 5) == 0,
       "is_alive: energy below pending damage is dead");

    // THE TRAP, and the only input class that separates the two natural spellings. The original
    // branches on CF alone, and an x87 UNORDERED compare sets CF -- so a NaN energy reads as ALIVE.
    // `!(diff > 0.0)` would return dead here; `diff <= 0.0` returns alive, which is the machine.
    f.b(3, 5).energy         = std::numeric_limits<double>::quiet_NaN();
    f.b(3, 5).pending_damage = 0.0;
    ck(detail::target_ref_is_alive(f.view(), bldg_ref(3), 5) == 1,
       "is_alive: a NaN energy reads ALIVE -- the original's unordered compare sets CF");

    f.u(3, 5).unit_proto_id  = 9;
    f.u(3, 5).energy         = 1.0;
    f.u(3, 5).pending_damage = 0.5;
    ck(detail::target_ref_is_alive(f.view(), unit_ref(3), 5) == 1,
       "is_alive: the UNIT branch reads the unit roster, not the building one");
    // The owner nibble selects the roster ROW: player 3's unit is alive, player 4's slot is empty.
    ck(detail::target_ref_is_alive(f.view(), unit_ref(4), 5) == 0,
       "is_alive: the owner nibble picks the roster row");

    // ---- has_ground_weapon: the building path is this function's OWN code ----
    f.reset();
    g_rec.clear();
    f.b(2, 4).building_id         = 11;
    f.cfg_buildings[11].weapon_id = 0;
    ck(detail::target_ref_has_ground_weapon(f.view(), stub_calls(), bldg_ref(2), 4) == 0,
       "has_ground_weapon: weapon_id 0 is the sentinel -- no Weapon[] lookup, answer false");
    f.cfg_buildings[11].weapon_id = 6;
    f.cfg_weapons[6].target       = 2; // AIR only
    ck(detail::target_ref_has_ground_weapon(f.view(), stub_calls(), bldg_ref(2), 4) == 0,
       "has_ground_weapon: mask is 1, so an AIR-only weapon (target == 2) answers false");
    f.cfg_weapons[6].target = 1;
    ck(detail::target_ref_has_ground_weapon(f.view(), stub_calls(), bldg_ref(2), 4) == 1,
       "has_ground_weapon: target bit 1 set answers true");
    f.cfg_weapons[6].target = 3;
    ck(detail::target_ref_has_ground_weapon(f.view(), stub_calls(), bldg_ref(2), 4) == 1,
       "has_ground_weapon: the test is a BIT test, not an equality");
    ck(g_rec.weapon_preds.empty(),
       "has_ground_weapon: the BUILDING path calls nothing -- it is this function's own body");

    // The unit path is a TAIL JUMP, and what it hands over is the UNMASKED packed ref.
    g_rec.clear();
    g_rec.ground = 1;
    ck(detail::target_ref_has_ground_weapon(f.view(), stub_calls(), unit_ref(2), 4) == 1,
       "has_ground_weapon: the UNIT path returns the callee's verdict");
    ck(g_rec.weapon_preds.size() == 1 && g_rec.weapon_preds[0].which == 4 &&
           g_rec.weapon_preds[0].ref == unit_ref(2) && g_rec.weapon_preds[0].index == 4,
       "has_ground_weapon: the unit half is handed the UNMASKED packed ref (0x22)");
    g_rec.clear();
    g_rec.ground = 0;
    ck(detail::target_ref_has_ground_weapon(f.view(), stub_calls(), unit_ref(2), 4) == 0,
       "has_ground_weapon: and its negative verdict too");

    // ---- has_aa_weapon: routing is the WHOLE function ----
    // 21 bytes: a roster test, a tail jump one way and a fall-through the other. Nothing else. So the
    // assertions are about WHICH callee ran and WHAT it was handed -- the masked owner nibble for the
    // building half (because llm_strat_bldg_has_aa_weapon does not mask again) and the untouched
    // packed ref for the unit half.
    g_rec.clear();
    g_rec.bldg_aa = 1;
    ck(detail::target_ref_has_aa_weapon(stub_calls(), bldg_ref(5), 12) == 1,
       "has_aa_weapon: the BUILDING half returns bldg_has_aa_weapon's verdict");
    ck(g_rec.weapon_preds.size() == 1 && g_rec.weapon_preds[0].which == 3 &&
           g_rec.weapon_preds[0].ref == 5u && g_rec.weapon_preds[0].index == 12,
       "has_aa_weapon: the building half is handed the MASKED owner nibble (5), not the ref (0x45)");

    g_rec.clear();
    g_rec.aa = 1;
    ck(detail::target_ref_has_aa_weapon(stub_calls(), unit_ref(5), 12) == 1,
       "has_aa_weapon: the UNIT half returns unit_has_aa_weapon's verdict");
    ck(g_rec.weapon_preds.size() == 1 && g_rec.weapon_preds[0].which == 5 &&
           g_rec.weapon_preds[0].ref == unit_ref(5) && g_rec.weapon_preds[0].index == 12,
       "has_aa_weapon: the unit half is handed the UNMASKED packed ref (0x25)");
}

void test_worker_priority() {
    fixture   f;
    const int me = 1;

    // The four shortage-priority slots, and a fifth DIFFERENT field for the else path. Distinct
    // values throughout so a body that read the wrong slot picks up a wrong answer rather than the
    // right one by accident.
    player_data &pd                       = f.players[me];
    pd.ai_resource_shortage_candidates[0] = 28;
    pd.ai_resource_shortage_candidates[1] = 29;
    pd.ai_resource_shortage_candidates[2] = 30;
    pd.ai_resource_shortage_candidates[3] = 32;
    pd.ai_build_candidate_cat_0x30        = 42;
    f.unit_sec.total                      = 55;

    // The site the AI is asking about: a building whose PRODUCTION record lives at slot sub_id == 6,
    // deliberately different from the site index so an implementation that indexed `productions` by
    // site_index would read a record this test never touches.
    const int site            = 3;
    f.b(me, site).sub_id      = 6;
    f.b(me, site).building_id = 29; // matches candidate slot 1

    // ---- the MATCHED path: the production-queue scan the rig never reached ----
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == false,
       "worker_priority: matched, but nothing in production -> false");

    f.prod(me, 6).active_unit_type = 4;
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == true,
       "worker_priority: an ACTIVE production answers true without scanning the queue");
    f.prod(me, 6).active_unit_type = 0;

    // Slot 0 is NEVER inspected (the scan starts at 1) and slot `total` IS (the bound is <=). Those
    // two are the whole content of `for (i = 1; i <= total; ++i)` and both are off-by-one traps.
    f.prod(me, 6).queued_count[0] = 7;
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == false,
       "worker_priority: queue slot 0 is never inspected -- the scan starts at 1");
    f.prod(me, 6).queued_count[0] = 0;

    f.prod(me, 6).queued_count[1] = 7;
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == true,
       "worker_priority: a queued unit at slot 1 answers true");
    f.prod(me, 6).queued_count[1] = 0;

    f.prod(me, 6).queued_count[55] = 7; // == total
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == true,
       "worker_priority: the scan bound is INCLUSIVE -- slot `total` is inspected");
    f.prod(me, 6).queued_count[55] = 0;

    f.prod(me, 6).queued_count[56] = 7; // == total + 1
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == false,
       "worker_priority: and it stops there -- slot total + 1 is not inspected");
    f.prod(me, 6).queued_count[56] = 0;

    // The record is selected by sub_id. Load the SITE-INDEXED record instead and nothing may change.
    f.prod(me, site).queued_count[2] = 7;
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == false,
       "worker_priority: productions is indexed by sub_id, NOT by the site index");
    f.prod(me, site).queued_count[2] = 0;

    // Each of the four candidate slots must be live, not just the first.
    for (int s = 0; s < 4; ++s) {
        f.b(me, site).building_id      = (uint16_t)pd.ai_resource_shortage_candidates[s];
        f.prod(me, 6).active_unit_type = 4;
        ck(detail::is_worker_priority_candidate(f.view(), me, site) == true,
           "worker_priority: all four shortage-candidate slots are compared");
        f.prod(me, 6).active_unit_type = 0;
    }

    // ---- the ELSE path, whose polarity is INVERTED relative to the match above ----
    f.b(me, site).building_id      = 42; // == ai_build_candidate_cat_0x30, matches none of the four
    f.prod(me, 6).active_unit_type = 4;  // must be ignored on this path
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == false,
       "worker_priority: no shortage match AND equal to the build-candidate category -> FALSE");
    f.b(me, site).building_id = 43;
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == true,
       "worker_priority: no shortage match and DIFFERENT from that category -> TRUE (inverted)");

    // The two paths read two different fields: with the else path taken, the production record is
    // never consulted at all, which is what makes the inversion above safe to assert.
    f.prod(me, 6).active_unit_type = 0;
    ck(detail::is_worker_priority_candidate(f.view(), me, site) == true,
       "worker_priority: the else path does not consult the production record");
}

// ---- 12. the site sort's EDGE cases, which the rig covered only in the middle -------------------
//
// WHY THIS IS HERE. The layer-5 rig runs called this site 41 times and diverged zero times, and 31 of
// those calls were substantive (lists of 107..755 candidates with 72..339 adjacent pairs out of
// order), so the swap path, the multi-pass termination and the whole-record move ARE differentially
// verified. What the rig never produced is the cheap end of the input space: an EMPTY list, an
// already-sorted list of more than one element, and EQUAL keys. The first two are the function's two
// early exits and the third is the only place the strictly-greater comparison can be told from a
// greater-or-equal one -- a difference no distance distribution from a real game is likely to show.
//
// These assertions come from the DISASSEMBLY (tmp/decomp_a5/..._004e6228.asm), not from reading the
// C++ under test: a fixture written off the implementation proves consistency, not correctness. And
// an offline test is NOT a differential result -- it is recorded beside this site's tier, never
// folded into it.
void test_site_sort_edges() {
    fixture f;

    // (a) count == 0, the first check (0x004e623a `CMP [count],0 ; JZ epilogue`). This one is a
    // LIVENESS case, not a value case, and it is worth saying so rather than dressing it up: with a
    // signed bound `i < count - 1` the empty list is already inert, so deleting the early return
    // does NOT make this assertion fail. What it does catch is a body that reached the array at all
    // on an empty list -- and what it catches for free is the unsigned-bound spelling, which would
    // run four billion iterations and never return.
    detail::sort_site_candidates_by_dist(f.view(), f.store());
    ck(f.site_count == 0 && f.site_cands[0].dist_sq == 0 && f.site_cands[0].tile_x == 0,
       "site sort: an empty list is returned untouched (and returns at all)");

    // (b) count == 1. The inner bound is count - 1 == 0, so the inner loop body never executes and
    // the pass loop leaves on its SECOND visit to its own top. The single record must be unchanged
    // in every column, not just in its key.
    f.reset();
    f.push_site(7, 9, SITE_KIND_RESOURCE, 500);
    detail::sort_site_candidates_by_dist(f.view(), f.store());
    ck(f.site_count == 1 && f.site_cands[0].tile_x == 7 && f.site_cands[0].tile_y == 9 &&
           f.site_cands[0].kind == SITE_KIND_RESOURCE && f.site_cands[0].dist_sq == 500,
       "site sort: a one-element list is returned byte-identical");

    // (c) ALREADY SORTED, more than one element. One pass runs, makes no swap, and the loop exits on
    // the next visit to its top. This is the case that separates the original's flag polarity from
    // its mirror image: a body that exited when a swap DID happen would also leave this input
    // untouched, so (c) alone proves nothing -- it is (c) together with (d) that pins the polarity.
    f.reset();
    for (int32_t i = 0; i < 5; ++i) f.push_site(i, 0, SITE_KIND_GRID_FIT, (uint32_t)(10 * i));
    detail::sort_site_candidates_by_dist(f.view(), f.store());
    for (int32_t i = 0; i < 5; ++i)
        ck(f.site_cands[i].dist_sq == (uint32_t)(10 * i) && f.site_cands[i].tile_x == i,
           "site sort: an already-sorted list is left in place");

    // (d) FULLY REVERSED, which needs n-1 passes to settle. A single-pass body (the polarity error)
    // gets this wrong at every position but the last, so this is the assertion that actually fails
    // when the flag is inverted. Mutation-checked by flipping `no_swap`'s initial value.
    f.reset();
    for (int32_t i = 0; i < 6; ++i) f.push_site(i, i, SITE_KIND_GRID_FIT, (uint32_t)(100 - 10 * i));
    detail::sort_site_candidates_by_dist(f.view(), f.store());
    bool ordered = true, travelled = true;
    for (int32_t i = 0; i < 6; ++i) {
        if (f.site_cands[i].dist_sq != (uint32_t)(50 + 10 * i)) ordered = false;
        // The record that carried dist_sq 100-10k started at tile (k, k); after the sort the entry
        // holding key 50+10i must still carry ITS own tile, which is the whole-record-move check.
        const int32_t k = (100 - (int32_t)f.site_cands[i].dist_sq) / 10;
        if (f.site_cands[i].tile_x != k || f.site_cands[i].tile_y != k) travelled = false;
    }
    ck(ordered, "site sort: a fully reversed list settles ascending -- the pass loop runs to a fixed "
                "point rather than once");
    ck(travelled, "site sort: every column travels with its key, not the key alone");

    // (e) EQUAL KEYS. `JBE skip` means swap only on strictly greater, so two records with the same
    // dist_sq must come out in their original relative order. A `>=` spelling would swap them on
    // every pass and, worse, would never let `no_swap` stay set -- so a body with that error does
    // not merely reorder here, it fails to terminate. The distinct tile_x values are what makes the
    // ordering observable at all; the keys alone cannot show it.
    f.reset();
    f.push_site(1, 0, SITE_KIND_GRID_FIT, 42);
    f.push_site(2, 0, SITE_KIND_GRID_FIT, 42);
    f.push_site(3, 0, SITE_KIND_GRID_FIT, 7);
    detail::sort_site_candidates_by_dist(f.view(), f.store());
    ck(f.site_cands[0].dist_sq == 7 && f.site_cands[0].tile_x == 3,
       "site sort: the smaller key moves to the front");
    ck(f.site_cands[1].tile_x == 1 && f.site_cands[2].tile_x == 2,
       "site sort: EQUAL keys keep their relative order (strictly-greater, not >=)");
}

// ---- 19. the layer-6 window tests (batch A's last layer) -----------------------------------------
//
// THIS IS THE ONLY COVER THE TORUS-SEAM BRANCH IS GOING TO GET. The two stencils are called once per
// map tile by the layer-5 scanners, so the rig reaches them millions of times -- but every one of
// those calls has span 10x10 and target_byte 2, and whether any of them crosses the map seam is luck.
// Here the seam is driven deliberately, and so is the mask's contiguity, which no live scenario
// distinguishes.
//
// THIS CASE OVERRIDES THE FIXTURE'S MAP SIZE, and the reason is a property of the original worth
// stating once: it wraps by ANDing with (extent - 1), so the wrap is a torus ONLY for a POWER-OF-TWO
// extent. The shared fixture is 64 x 48, and 48 - 1 == 0x2f has a hole at bit 4, so y = 20 masks to 4
// and every coordinate above 15 lands somewhere arbitrary. (That is not a bug in either the original
// or this translation -- real maps are 128x128 or 256x256, which is also why the game keeps
// map_width_mask / map_height_mask as AND masks at all. It cost one confusing round of debugging
// while writing this, so: 64 x 32 here, rectangular so an axis swap still cannot cancel out, and
// both powers of two so the wrap means what it says.) The resulting mask is
// ((64-1) << 8) | (32-1) == 0x3f1f.
void test_grid_stencil() {
    fixture f;
    f.map_w             = 64;
    f.map_h             = 32;
    const uint32_t me   = 1;
    uint8_t       *grid = &f.players[me].ai_tile_flags_grid[0];
    auto           at   = [&](int32_t x, int32_t y) -> uint8_t             &{ return grid[((uint32_t)x << 8) | (uint32_t)y]; };

    // (a) THE SIDE EFFECT, and it is the only write either function makes. Byte 0 is height-1 and
    // byte 1 is width-1, and bits 16-31 are LEFT ALONE -- the original stores two bytes, not a dword.
    // MUTATION-CHECKED three ways: swapping the two operands, storing a flat 32-bit value, and
    // dropping the -1 each fail this and only this.
    f.wrap_mask      = 0xdead0000u;
    uint8_t care1[1] = {1};
    detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care1, 1, 1, 0, 0, 0);
    ck(f.wrap_mask == 0xdead3f1fu,
       "stencil: the wrap mask is (width-1) in byte 1, (height-1) in byte 0, upper half preserved");
    f.wrap_mask = 0xdead0000u;
    detail::grid_stencil_all_near_unthreatened(f.view(), f.store(), grid, f.map_w, f.map_h, care1, 1,
                                               1, 0, 0);
    ck(f.wrap_mask == 0xdead3f1fu, "stencil: the PREDICATE sibling stores the identical wrap mask");

    // (b) THE INDEX IS (x << 8) | y, with x the OUTER byte. A 1x1 window at (5, 7) must read the
    // tile at 0x0507 and not the one at 0x0705. The control at the transposed address is what makes
    // this an axis test rather than a "does it read anything" test.
    at(5, 7) = 2;
    at(7, 5) = 99;
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care1, 1, 1, 5, 7,
                                  2) == 1,
       "stencil: a 1x1 window at (5,7) reads grid[(5<<8)|7]");
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care1, 1, 1, 5, 7,
                                  99) == 0,
       "stencil: ...and it is a byte EQUALITY, so the transposed tile's value does not satisfy it");

    // (c) THE FOOTPRINT MASK SELECTS. A zero mask byte means "do not test this cell" -- so a cell
    // that would fail is invisible when its mask byte is 0, and fails when it is not. Same grid,
    // same window, only the mask differs: that is what makes this a mask test.
    at(10, 10)           = 2;
    at(10, 11)           = 2;
    at(11, 10)           = 2;
    at(11, 11)           = 77; // the offender, at window position (1,1)
    uint8_t all_care[4]  = {1, 1, 1, 1};
    uint8_t skip_last[4] = {1, 1, 1, 0};
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, all_care, 2, 2, 10, 10,
                                  2) == 0,
       "stencil: one failing CARE cell fails the whole window");
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, skip_last, 2, 2, 10,
                                  10, 2) == 1,
       "stencil: the same failing cell is IGNORED when its footprint byte is 0");
    // And a window whose mask is entirely zero tests nothing at all and returns 1 -- the vacuous
    // input class the shadow arm's `zero_care` counter exists to detect in the rig.
    uint8_t no_care[4] = {0, 0, 0, 0};
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, no_care, 2, 2, 10, 10,
                                  2) == 1,
       "stencil: an all-zero footprint returns 1 without testing anything");

    // (d) THE MASK IS WALKED CONTIGUOUSLY AND IS NOT RESET PER ROW: its index is
    // outer * span_y + inner, so mask[1] belongs to (start_x + 0, start_y + 1) ONLY. A body that
    // reset the mask pointer at the top of each row would ALSO test (start_x + 1, start_y + 1),
    // where the offender sits. Both cells are otherwise identical, so this separates the two
    // implementations and nothing else does.
    at(20, 20)           = 2;
    at(20, 21)           = 2; // the one real care cell
    at(20, 22)           = 2;
    at(21, 20)           = 2;
    at(21, 21)           = 55; // only reachable by a per-row-reset bug
    at(21, 22)           = 2;
    uint8_t row_probe[6] = {0, 1, 0, 0, 0, 0}; // span_x = 2, span_y = 3
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, row_probe, 2, 3, 20,
                                  20, 2) == 1,
       "stencil: the footprint index is outer*span_y+inner -- the mask is NOT restarted per row");

    // (e) BL IS RE-READ FROM THE ARGUMENT EVERY ROW, it does not carry over from the previous row's
    // final value. A carrying body would walk (30,30),(30,31) and then bl = 32,33 -- which the wrap
    // ANDs down to 0 and 1 -- instead of the 2x2 block, so the offenders are parked at (31,0) and
    // (31,1) where only that body looks.
    at(30, 30) = 2;
    at(30, 31) = 2;
    at(31, 30) = 2;
    at(31, 31) = 2;
    at(31, 0)  = 44;
    at(31, 1)  = 44;
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, all_care, 2, 2, 30, 30,
                                  2) == 1,
       "stencil: start_y is re-read per row, so row 1 is (start_y, start_y+1) again");

    // (f) THE TORUS WRAP ON X. width is 64, so a window starting at x = 63 with span 2 reads column
    // 64, which the wrap mask ANDs down to column 0. The distinguishing setup: the value a WRAPPING
    // body reads (column 0) fails, and the value a NON-wrapping body would read (column 64, a
    // perfectly valid index into the 64 KB plane) passes. Without that pairing an unwrapped read of
    // a zeroed plane would look like a wrapped read of a zeroed plane.
    at(63, 12)       = 2;
    at(0, 12)        = 66; // wrapping body sees this -> 0
    at(64, 12)       = 2;  // non-wrapping body sees this -> 1
    uint8_t care2[2] = {1, 1};
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care2, 2, 1, 63, 12,
                                  2) == 0,
       "stencil: the X axis wraps at width, so column 64 is read as column 0");
    at(0, 12) = 2;
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care2, 2, 1, 63, 12,
                                  2) == 1,
       "stencil CONTROL: making the WRAPPED tile pass makes the same window pass");

    // (g) THE TORUS WRAP ON Y, which is a different mask byte and a different extent (32, not 64) --
    // a body that used one extent for both axes gets this wrong while passing (f).
    at(50, 31) = 2;
    at(50, 0)  = 88; // wrapping body sees this -> 0
    at(50, 32) = 2;  // non-wrapping body sees this -> 1
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care2, 1, 2, 50, 31,
                                  2) == 0,
       "stencil: the Y axis wraps at HEIGHT (32), not at width");
    at(50, 0) = 2;
    ck(detail::grid_match_stencil(f.view(), f.store(), grid, f.map_w, f.map_h, care2, 1, 2, 50, 31,
                                  2) == 1,
       "stencil CONTROL: making the Y-wrapped tile pass makes the same window pass");

    // (h) THE PREDICATE SIBLING'S TWO-PART TEST, one assertion per bit pattern that separates a
    // spelling from its plausible neighbours:
    //   0x03  dist 3, no threat            -> pass  (the boundary, `<= 3` not `< 3`)
    //   0x04  dist 4                       -> fail
    //   0x20  bit 5 set, dist 0            -> pass  (the mask is 0x1f, NOT 0x3f)
    //   0x80  bit 7 set, dist 0            -> pass  (the threat test is TEST 0x40, not a sign test)
    //   0x43  threat bit set, dist 3       -> fail  (0x40 is checked BEFORE the distance passes it)
    //   0x9f  dist 31 with the high bit    -> fail  (the compare is unsigned after masking)
    struct {
        uint8_t     cell;
        int32_t     want;
        const char *what;
    } cases[] = {
        {0x00, 1, "stencil_near: an empty cell passes"},
        {0x03, 1, "stencil_near: distance 3 passes -- the bound is <= 3"},
        {0x04, 0, "stencil_near: distance 4 fails"},
        {0x20, 1, "stencil_near: bit 0x20 is NOT part of the distance field (mask is 0x1f)"},
        {0x80, 1, "stencil_near: bit 0x80 is ignored -- the threat test is TEST 0x40, not a sign bit"},
        {0x40, 0, "stencil_near: the turret-threat bit alone fails the cell"},
        {0x43, 0, "stencil_near: threat + an acceptable distance still fails"},
        {0x9f, 0, "stencil_near: distance 31 fails, and the compare is unsigned after masking"},
    };
    for (const auto &c : cases) {
        at(4, 4) = c.cell;
        ck(detail::grid_stencil_all_near_unthreatened(f.view(), f.store(), grid, f.map_w, f.map_h,
                                                      care1, 1, 1, 4, 4) == c.want,
           c.what);
    }

    // (i) And the predicate sibling shares the walk, so it must wrap too. Same construction as (f).
    at(63, 13) = 0x00;
    at(0, 13)  = 0x40; // wrapping body sees the threat bit -> 0
    at(64, 13) = 0x00; // non-wrapping body sees a clean tile -> 1
    ck(detail::grid_stencil_all_near_unthreatened(f.view(), f.store(), grid, f.map_w, f.map_h, care2,
                                                  2, 1, 63, 13) == 0,
       "stencil_near: shares the wrapping walk, not just the predicate");

    // NOT TESTED, ON PURPOSE: span_x == 0 or span_y == 0. The original's loops are do-while over a
    // DEC'd counter, so a zero span runs 2^32 passes rather than none -- reproduced faithfully, and
    // calling it here would hang the suite. Both call sites pass FOOTPRINT_SPAN (10), a compile-time
    // constant, so the input cannot occur.
}

// ---- RI-AI batch B, antichain layer 0 (2) -------------------------------------------------------
//
// llm_strat_ai_bldg_register_visible_building -- the DAMAGE-REPORT hook. See ai_attacker_intel.h for
// why neither its Ghidra name nor its committed parameter names describe what it does.
//
// THE OFFLINE ORACLE CARRIES MOST OF THIS FUNCTION, and specifically the parts the rig cannot ask
// for: an aircraft aggressor, an alien-race victim, a victim whose building type is a mine / turret
// / mother, the foreign-change flag set, a relation that is already non-zero, and the -1 sentinel.
// The rig supplies whatever a skirmish happens to emit, which on a human-vs-AI map is mostly one
// race and mostly unit victims.
//
// Every literal is picked to SEPARATE implementations rather than merely to be plausible: the three
// race-paired building types are distinct constants so a body that tests the wrong pair lands on
// none of them, home tile and building position are asymmetric so an (x,y) swap in the toroidal call
// shows up, and the radius is chosen so the squared comparison is strictly-less rather than
// less-or-equal at the boundary.
namespace intel_stub {

int32_t  g_aircraft_answer = 0;
int      g_aircraft_calls  = 0;
uint32_t g_aircraft_ref    = 0;
int32_t  g_aircraft_index  = -1;

// Mirrors llm_strat_ai_target_list_add's five parameters, so it carries that function's committed
// parameter vocabulary (renamed with llm_strat_ai_target_entry on 2026-08-03, finding -1731-3).
struct tl_call {
    uint32_t player;
    int32_t  victim_ref;
    int32_t  victim_index;
    uint32_t aggressor_ref;
    int32_t  aggressor_index;
};
std::vector<tl_call> g_tl;

int32_t st_is_aircraft(uint32_t unit_ref, int32_t unit_index) {
    ++g_aircraft_calls;
    g_aircraft_ref   = unit_ref;
    g_aircraft_index = unit_index;
    return g_aircraft_answer;
}
void st_target_list_add(uint32_t player, int32_t victim_ref, int32_t victim_index,
                        uint32_t aggressor_ref, int32_t aggressor_index) {
    g_tl.push_back({player, victim_ref, victim_index, aggressor_ref, aggressor_index});
}

void clear() {
    g_aircraft_answer = 0;
    g_aircraft_calls  = 0;
    g_aircraft_ref    = 0;
    g_aircraft_index  = -1;
    g_tl.clear();
}

// A calls set with the three members this body reaches bound and everything else deliberately null,
// so an unstubbed call fails loudly rather than returning zero.
const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.unit_is_aircraft = &st_is_aircraft;
        t.target_list_add  = &st_target_list_add;
        t.toroidal_dist_sq = &st_toroidal;
        return t;
    }();
    return c;
}

} // namespace intel_stub

void test_attacker_intel() {
    using path = register_report::path;
    fixture f;

    const uint32_t VICTIM = 1, AGGRESSOR = 3;
    const int32_t  BLDG = 12; // the victim's roster slot AND, per the original, its cfg index
    const int32_t  UNIT = 44; // the aggressor's unit index

    auto vref_bldg = [&](uint32_t owner) { return REF_KIND_BUILDING | owner; };
    auto vref_unit = [&](uint32_t owner) { return REF_KIND_UNIT | owner; };
    auto aref_unit = [&](uint32_t owner) { return REF_KIND_UNIT | owner; };

    auto arm = [&]() {
        f.reset();
        intel_stub::clear();
        // A HUMAN-race victim by default, so the human members of the three race pairs are the live
        // ones and a body that reads the alien members silently sets nothing.
        f.players[VICTIM].is_alien_race        = 0;
        f.players[VICTIM].ai_home_tile_x       = 10;
        f.players[VICTIM].ai_home_tile_y       = 20;
        f.players[VICTIM].ai_expand_gate_value = 5;
        f.b(VICTIM, BLDG).x                    = 12; // dx 2, dy 1 -> dist_sq 5 < 25
        f.b(VICTIM, BLDG).y                    = 21;
        f.cfg_buildings[BLDG].type             = 99; // matches none of the three pairs
    };
    auto seen  = [&](uint32_t col) { return f.players[VICTIM].ai_intel_seen_count[col]; };
    auto flags = [&](uint32_t col) { return f.players[VICTIM].ai_intel_flags[col]; };
    auto rel   = [&](uint32_t col) { return f.players[VICTIM].ai_player_relation[col]; };
    auto run   = [&](int32_t victim_index, uint32_t victim_ref, uint32_t aggressor_ref) {
        return detail::register_attacker_damage(f.view(), f.store(), intel_stub::calls(),
                                                  victim_index, victim_ref, (uint32_t)UNIT,
                                                  aggressor_ref, 1);
    };

    // (a) THE SAME-OWNER EARLY-OUT. Friendly fire writes nothing at all -- not even the hit counter,
    // which every other path bumps. This is what keeps the diagonal of all three per-opponent tables
    // at zero, and a body that moved the early-out after the INC would fail here and nowhere else.
    arm();
    ck(run(BLDG, vref_bldg(VICTIM), aref_unit(VICTIM)).taken == path::same_owner,
       "attacker intel: matching owner nibbles return before anything is written");
    ck(seen(VICTIM) == 0 && flags(VICTIM) == 0 && rel(VICTIM) == 0,
       "attacker intel: the self diagonal of all three tables stays zero");
    ck(intel_stub::g_tl.empty(), "attacker intel: the early-out skips the target-list append");

    // (b) A UNIT VICTIM. The hit counter and the hostility stamp still run; the whole intel-flag
    // block does not. The gate is on the VICTIM's ref, which is the half the committed parameter
    // names get backwards -- a body that tested the aggressor's ref instead would take the building
    // path here, because the aggressor ref carries 0x80 and not 0x40 either way.
    arm();
    register_report r = run(BLDG, vref_unit(VICTIM), aref_unit(AGGRESSOR));
    ck(r.taken == path::unit_victim, "attacker intel: a unit victim skips the intel-flag block");
    ck(seen(AGGRESSOR) == 1, "attacker intel: the hit counter runs OUTSIDE the building gate");
    ck(flags(AGGRESSOR) == 0, "attacker intel: no intel flag is set for a unit victim");
    ck(rel(AGGRESSOR) == -1, "attacker intel: the aggressor is marked hostile for a unit victim too");
    ck(intel_stub::g_tl.size() == 1, "attacker intel: a unit victim still reaches the target list");

    // (c) A BUILDING VICTIM whose type matches none of the three pairs: exactly two bits, the
    // unconditional 0x1 and the in-range 0x8. If this ever reports 0x1 alone the toroidal comparison
    // has been read as strict-less inverted, or the arguments have been swapped.
    arm();
    r = run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(r.taken == path::building_victim, "attacker intel: a building victim takes the full path");
    ck(flags(AGGRESSOR) == (INTEL_FLAG_BUILDING_HIT | INTEL_FLAG_NEAR_HOME),
       "attacker intel: an unremarkable building inside the home radius sets 0x1|0x8");
    ck(seen(AGGRESSOR) == 1, "attacker intel: one call increments the hit counter exactly once");

    // (d) THE ROW/COLUMN DIRECTION, and it is the claim the whole translation rests on. Row = the
    // DAMAGED player, column = the AGGRESSOR. A transposed body passes (c) -- both tables are
    // symmetric under a single call with everything else zero -- and fails here, because the two
    // rows now hold different values.
    arm();
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(f.players[VICTIM].ai_intel_seen_count[AGGRESSOR] == 1,
       "attacker intel: the row is the damaged player and the column is the aggressor");
    ck(f.players[AGGRESSOR].ai_intel_seen_count[VICTIM] == 0,
       "attacker intel: the aggressor's own row is not touched");

    // (e) THE THREE RACE-PAIRED KIND BITS, human side. Each is checked on its own so a body that
    // ORs the wrong constant lands on no bit rather than on a neighbouring one.
    const std::pair<uint8_t, int32_t> human_kinds[3] = {
        {BLDG_TYPE_H_MINE, INTEL_FLAG_MINE_HIT},
        {BLDG_TYPE_H_TURRET, INTEL_FLAG_TURRET_HIT},
        {BLDG_TYPE_H_MOTHER, INTEL_FLAG_MOTHER_HIT},
    };
    for (const auto &kase : human_kinds) {
        arm();
        f.cfg_buildings[BLDG].type = kase.first;
        run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
        ck(flags(AGGRESSOR) == (INTEL_FLAG_BUILDING_HIT | INTEL_FLAG_NEAR_HOME | kase.second),
           "attacker intel: the human building-kind bit is selected by cfg type");
    }

    // (f) THE RACE SWITCH. The SAME cfg type id now has to select the ALIEN member, so a body with
    // the race test inverted sets no kind bit at all here and the assertion names the pair.
    arm();
    f.players[VICTIM].is_alien_race = 1;
    f.cfg_buildings[BLDG].type      = BLDG_TYPE_A_TURRET;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(flags(AGGRESSOR) == (INTEL_FLAG_BUILDING_HIT | INTEL_FLAG_NEAR_HOME | INTEL_FLAG_TURRET_HIT),
       "attacker intel: is_alien_race != 0 selects the alien member of each type pair");
    // ... and the human constant must now MISS.
    arm();
    f.players[VICTIM].is_alien_race = 1;
    f.cfg_buildings[BLDG].type      = BLDG_TYPE_H_TURRET;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck((flags(AGGRESSOR) & INTEL_FLAG_TURRET_HIT) == 0,
       "attacker intel: an alien victim does not match the human turret constant");

    // (g) THE HOME-RADIUS COMPARISON IS STRICT AND SQUARED. dist_sq is exactly 25 with the building
    // moved to (15,20) and the gate at 5, so strict-less clears the bit and less-or-equal would set
    // it -- the one input that separates the two readings of CMP EAX,[EBP-0x18] / JNC.
    arm();
    f.b(VICTIM, BLDG).x = 15;
    f.b(VICTIM, BLDG).y = 20;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck((flags(AGGRESSOR) & INTEL_FLAG_NEAR_HOME) == 0,
       "attacker intel: dist_sq == radius^2 is NOT within the home radius (strict <)");
    arm();
    f.b(VICTIM, BLDG).x = 14; // dist_sq 16 < 25
    f.b(VICTIM, BLDG).y = 20;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck((flags(AGGRESSOR) & INTEL_FLAG_NEAR_HOME) != 0,
       "attacker intel: dist_sq < radius^2 is within the home radius");

    // (h) THE RELATION STAMP IS CONDITIONAL. With the foreign-change flag clear, an ALREADY non-zero
    // relation is left alone -- including a friendly +1, which is what makes this a real branch
    // rather than an idempotent write.
    arm();
    f.players[VICTIM].ai_player_relation[AGGRESSOR] = 1;
    r                                               = run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(!r.relation_stamps && rel(AGGRESSOR) == 1,
       "attacker intel: a non-zero relation survives when the foreign-change flag is clear");

    // (i) ... and the flag overrides it. Same input, flag set: the +1 is overwritten with -1. A body
    // that dropped the global entirely passes (h) and fails here, which is why the global is worth a
    // region of its own.
    arm();
    f.players[VICTIM].ai_player_relation[AGGRESSOR] = 1;
    f.foreign_flag                                  = 1;
    r                                               = run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(r.relation_stamps && rel(AGGRESSOR) == -1,
       "attacker intel: a set foreign-change flag re-stamps hostility unconditionally");

    // (j) THE AIRCRAFT RE-TAG, and its ONLY observable is the fourth argument of target_list_add.
    // The aggressor ref goes in as 0x80|owner and comes out as 0x20|owner; nothing else in the body
    // can see the difference, because every other consumer masks it to the owner nibble.
    arm();
    intel_stub::g_aircraft_answer = 1;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(intel_stub::g_aircraft_calls == 1 && intel_stub::g_aircraft_ref == aref_unit(AGGRESSOR) &&
           intel_stub::g_aircraft_index == UNIT,
       "attacker intel: the aircraft test is asked about the AGGRESSOR's ref and unit index");
    ck(intel_stub::g_tl.size() == 1 &&
           intel_stub::g_tl[0].aggressor_ref == (REF_KIND_AIRCRAFT | AGGRESSOR),
       "attacker intel: an aircraft aggressor reaches the target list as 0x20|owner");
    // ... and with the same input answered "no", the ref passes through untouched.
    arm();
    intel_stub::g_aircraft_answer = 0;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(intel_stub::g_tl.size() == 1 && intel_stub::g_tl[0].aggressor_ref == aref_unit(AGGRESSOR),
       "attacker intel: a non-aircraft aggressor's ref is passed through unchanged");

    // (k) A BUILDING AGGRESSOR (a turret shooting back) never reaches the aircraft test at all --
    // the 0x80 bit is what gates it, not "is it a unit index".
    arm();
    intel_stub::g_aircraft_answer = 1;
    run(BLDG, vref_bldg(VICTIM), REF_KIND_BUILDING | AGGRESSOR);
    ck(intel_stub::g_aircraft_calls == 0,
       "attacker intel: a non-unit aggressor ref skips the aircraft test");

    // (l) THE ARGUMENT MAP OF target_list_add -- now also its committed parameter naming (finding
    // 2026-08-03-1549-1, applied 2026-08-21): the row player, the VICTIM's full ref, the VICTIM's
    // roster index, the aggressor's ref, the aggressor's roster index. Recorded rather than asserted
    // structurally because the callee is an outward call and this order is the entire contract with
    // it.
    arm();
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(intel_stub::g_tl.size() == 1 && intel_stub::g_tl[0].player == VICTIM &&
           intel_stub::g_tl[0].victim_ref == (int32_t)vref_bldg(VICTIM) &&
           intel_stub::g_tl[0].victim_index == BLDG && intel_stub::g_tl[0].aggressor_index == UNIT,
       "attacker intel: target_list_add gets (victim owner, victim ref, victim index, aggressor "
       "ref, aggressor unit index)");

    // (m) THE -1 SENTINEL suppresses ONLY the target-list append. Everything above it still runs,
    // which is the ordering the original commits to by testing the sentinel last.
    arm();
    r = run(VICTIM_INDEX_NONE, vref_unit(VICTIM), aref_unit(AGGRESSOR));
    ck(!r.target_added && intel_stub::g_tl.empty(),
       "attacker intel: victim_index == -1 suppresses the target-list append");
    ck(seen(AGGRESSOR) == 1 && rel(AGGRESSOR) == -1,
       "attacker intel: the -1 sentinel is tested LAST -- the counter and the relation still ran");

    // (n) REPEATED HITS ACCUMULATE. The counter is an INC, not a set, and the flags OR rather than
    // assign -- so two calls with different building kinds leave both kind bits standing.
    arm();
    f.cfg_buildings[BLDG].type = BLDG_TYPE_H_MINE;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    f.cfg_buildings[BLDG].type = BLDG_TYPE_H_TURRET;
    run(BLDG, vref_bldg(VICTIM), aref_unit(AGGRESSOR));
    ck(seen(AGGRESSOR) == 2, "attacker intel: the hit counter accumulates across calls");
    ck(flags(AGGRESSOR) == (INTEL_FLAG_BUILDING_HIT | INTEL_FLAG_NEAR_HOME | INTEL_FLAG_MINE_HIT |
                            INTEL_FLAG_TURRET_HIT),
       "attacker intel: intel flags OR rather than overwrite");
}

void test_queue_release() {
    fixture       f;
    const int32_t P = 2, BLDG = 7;

    // Arm one queue entry and a known resource ledger. `spent` is deliberately not round: each index
    // gets a distinct base so a body that refunds into the wrong slot cannot land on the right value.
    auto arm = [&](uint8_t status, int32_t bldg, uint8_t tick) {
        f.reset();
        f.players[P].ai_enabled          = 1;
        f.players[P].ai_bldg_queue_count = 1;
        auto &e                          = f.players[P].ai_bldg_queue[0];
        e.status                         = status;
        e.tick_or_unit_id                = tick;
        e.building_index                 = bldg;
        // index 0 is NOT a cost (the construction path reuses it as a cached tile Y) and must be
        // left alone; 1..4 are the four the original refunds.
        e.resource_reserved[0] = 900;
        e.resource_reserved[1] = 11;
        e.resource_reserved[2] = 13;
        e.resource_reserved[3] = 17;
        e.resource_reserved[4] = 19;
        // resource_spent is int[4] over ids 1..4 since the 2026-08-03 reshape. The scalar that used
        // to be its slot [0] is ai_patrol_quadrant_cursor, seeded here as a SENTINEL: a refund loop
        // that ran off the front of the array would land on it.
        f.players[P].ai_patrol_quadrant_cursor = 1000;
        for (int32_t j = 0; j < 4; ++j) f.players[P].resource_spent[j] = 1100 + 100 * j;
    };
    // Indexed by RESOURCE ID, so every assertion below keeps speaking the original's vocabulary.
    auto spent = [&](int32_t id) { return f.players[P].resource_spent[id - RESOURCE_ID_FIRST]; };
    auto entry = [&](int32_t i) -> mh::game::mh_llm_strat_ai_bldg_queue_entry & {
        return f.players[P].ai_bldg_queue[i];
    };

    // (a) THE MASTER GATE. ai_enabled == 0 with an otherwise perfectly matching entry: nothing at
    // all happens. Deleting the gate makes this the loudest failure in the file, because the same
    // input is the mode-0 refund case below.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
    f.players[P].ai_enabled = 0;
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 0) == release_outcome::not_ai,
       "queue release: ai_enabled == 0 short-circuits before the scan");
    ck(spent(1) == 1100 && entry(0).tick_or_unit_id == 5 &&
           entry(0).status == QUEUE_STATUS_COMMITTED_REPAIR,
       "queue release: a non-AI player's queue and ledger are untouched");

    // (b) MODE 0 on a committed REPAIR entry -- the tick decrement and the four-resource refund.
    // Note what is asserted NOT to move: ai_patrol_quadrant_cursor, the scalar immediately BELOW
    // resource_spent (the refund starts at id 1, so it must never reach back past element 0), and
    // the status byte (mode 0 does not stamp 0x40 -- that is what separates it from mode 1).
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 0) == release_outcome::refunded,
       "queue release: mode 0 matches the committed repair entry");
    ck(entry(0).tick_or_unit_id == 4, "queue release: mode 0 decrements the tick counter");
    ck(f.players[P].ai_patrol_quadrant_cursor == 1000,
       "queue release: the refund starts at resource id 1 -- the ai_patrol_quadrant_cursor scalar "
       "that sits immediately below resource_spent[0] is untouched");
    ck(spent(1) == 1100 - 11 && spent(2) == 1200 - 13 && spent(3) == 1300 - 17 &&
           spent(4) == 1400 - 19,
       "queue release: mode 0 refunds resource_reserved[1..4] into resource_spent[1..4], index for "
       "index");
    ck(entry(0).status == QUEUE_STATUS_COMMITTED_REPAIR,
       "queue release: mode 0 does NOT stamp the removed bit");

    // (c) THE GUARDED DECREMENT. A tick already at 0 stays 0; an unguarded `--` would wrap it to
    // 0xff, and the refund must still happen.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 0);
    detail::queue_release_order(f.view(), f.store(), P, BLDG, 0);
    ck(entry(0).tick_or_unit_id == 0, "queue release: a zero tick counter does not wrap to 0xff");
    ck(spent(1) == 1100 - 11, "queue release: the refund happens whether or not the tick moved");

    // (d) MOVZX, NOT MOVSX. resource_reserved is int16_t in the struct but the original ZERO-extends
    // each halfword, so 0xffff subtracts 65535. A sign-extending body would ADD 1 and land on 1101 --
    // an input where the two implementations cannot agree.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 3);
    entry(0).resource_reserved[1] = (int16_t)0xffff;
    detail::queue_release_order(f.view(), f.store(), P, BLDG, 0);
    ck(spent(1) == 1100 - 65535,
       "queue release: reserved costs are ZERO-extended (0xffff subtracts 65535, it does not add 1)");

    // (e) THE UPGRADE EARLY-OUT, and the reason this test exists at all. With a committed UPGRADE
    // entry for the same building sitting in FRONT of a committed repair entry, mode 0 stops at the
    // upgrade and does nothing -- the repair behind it is never reached. A body that treated the
    // upgrade branch as `continue` would refund here, so this is the one input that separates the
    // original's early-out from the obvious misreading of it.
    arm(QUEUE_STATUS_COMMITTED_UPGRADE, BLDG, 5);
    f.players[P].ai_bldg_queue_count = 2;
    entry(1).status                  = QUEUE_STATUS_COMMITTED_REPAIR;
    entry(1).building_index          = BLDG;
    entry(1).tick_or_unit_id         = 8;
    entry(1).resource_reserved[1]    = 40;
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 0) ==
           release_outcome::upgrade_blocked,
       "queue release: mode 0 stops on a matching committed UPGRADE entry");
    ck(spent(1) == 1100 && entry(1).tick_or_unit_id == 8 && entry(0).tick_or_unit_id == 5,
       "queue release: the upgrade early-out refunds nothing and never reaches the repair entry "
       "behind it");

    // (f) MODE 1 -- cancel a repair. Stamps 0x40 on top of the existing status (it ORs, it does not
    // assign) and refunds nothing.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 1) ==
           release_outcome::stamped_removed,
       "queue release: mode 1 matches the committed repair entry");
    ck(entry(0).status == (uint8_t)(QUEUE_STATUS_COMMITTED_REPAIR | QUEUE_STATUS_REMOVED),
       "queue release: mode 1 ORs 0x40 into status rather than overwriting it");
    ck(spent(1) == 1100 && entry(0).tick_or_unit_id == 5,
       "queue release: mode 1 refunds nothing and leaves the tick alone");

    // (g) MODE 1 AND MODE 2 ARE NOT INTERCHANGEABLE. Mode 1 looks only for 0x83, mode 2 only for
    // 0x84; each must MISS the other's entry. A body that shared one literal passes (f) and fails
    // here.
    arm(QUEUE_STATUS_COMMITTED_UPGRADE, BLDG, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 1) == release_outcome::no_match &&
           entry(0).status == QUEUE_STATUS_COMMITTED_UPGRADE,
       "queue release: mode 1 does not match a committed UPGRADE entry");
    arm(QUEUE_STATUS_COMMITTED_UPGRADE, BLDG, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 2) ==
               release_outcome::stamped_removed &&
           entry(0).status == (uint8_t)(QUEUE_STATUS_COMMITTED_UPGRADE | QUEUE_STATUS_REMOVED),
       "queue release: mode 2 stamps the committed UPGRADE entry");
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 2) == release_outcome::no_match &&
           entry(0).status == QUEUE_STATUS_COMMITTED_REPAIR,
       "queue release: mode 2 does not match a committed REPAIR entry");

    // (h) THE STATUS TEST IS THE WHOLE BYTE, not the kind nibble. 0x93 is kind 3 with an extra bit
    // set; the original's `CMP byte,0x83` rejects it. A body written as `(status & 0xf) == 3` --
    // which is how the sibling scan_bldg_repair_upgrade legitimately tests it -- would match.
    arm(0x93, BLDG, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 1) == release_outcome::no_match &&
           entry(0).status == 0x93,
       "queue release: status is compared as a whole byte, not as a kind nibble");

    // (i) THE BUILDING INDEX MUST MATCH TOO.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG + 1, 5);
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 0) == release_outcome::no_match &&
           spent(1) == 1100,
       "queue release: an entry for a different building does not match");

    // (j) THE SCAN IS COUNT-DRIVEN. The entry is present in memory but sits at or past
    // ai_bldg_queue_count, so it is not there as far as this function is concerned. A body bounded
    // by the array's compile-time extent (64) instead of the live count would find it.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
    f.players[P].ai_bldg_queue_count = 0;
    ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, 0) == release_outcome::no_match &&
           spent(1) == 1100 && entry(0).tick_or_unit_id == 5,
       "queue release: the scan is bounded by ai_bldg_queue_count, not by the array extent");

    // (k) FIRST MATCH WINS. Two identical matching entries: only slot 0 is touched.
    arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
    f.players[P].ai_bldg_queue_count = 2;
    entry(1)                         = entry(0);
    detail::queue_release_order(f.view(), f.store(), P, BLDG, 1);
    ck(entry(0).status == (uint8_t)(QUEUE_STATUS_COMMITTED_REPAIR | QUEUE_STATUS_REMOVED) &&
           entry(1).status == QUEUE_STATUS_COMMITTED_REPAIR,
       "queue release: the scan returns on the first match and never reaches a second");

    // (l) THE MODE DISPATCH IS UNSIGNED and covers exactly {0, 1, 2}. Mode 3 is the obvious
    // out-of-range case; mode -1 is the one that separates the original's `CMP/JC` from a signed
    // `if (mode < 1)`, which would send it down the mode-0 refund path.
    for (int32_t bad : {3, -1, (int32_t)0x80000000}) {
        arm(QUEUE_STATUS_COMMITTED_REPAIR, BLDG, 5);
        ck(detail::queue_release_order(f.view(), f.store(), P, BLDG, bad) ==
                   release_outcome::bad_mode &&
               spent(1) == 1100 && entry(0).status == QUEUE_STATUS_COMMITTED_REPAIR &&
               entry(0).tick_or_unit_id == 5,
           "queue release: an out-of-range mode (including a negative one) does nothing");
    }
}

void test_calls_complete() {
    ck(mh::ai::calls_are_complete(),
       "wiring: every ai_calls slot is bound in BOTH live_calls() and shadow_calls()");
}

} // namespace

// ---- batch B layer 1a: the influence-map driver -------------------------------------------------
//
// llm_strat_ai_recompute_map_influence is a PURE DRIVER: it clears one dword and then emits an
// unconditional, branch-free sequence of 28 __cdecl calls. So unlike every other body in this file
// the offline test can be essentially exhaustive -- the ordered argument sequence IS the function --
// and there is no branch left over for the rig to reach that this cannot.
void test_map_influence() {
    fixture       f;
    const int32_t P = 3; // NOT 0: a stride bug would land on the right row for player 0

    // The expected sequence, straight off the listing. Kept as a table here and as straight-line
    // code in the translation deliberately: if the two ever agree by construction the test proves
    // nothing.
    struct step {
        bool    flood;
        int32_t level, fill;
    };
    std::vector<step> want;
    want.push_back({false, 3, 0});                             // 0x004d7a74
    want.push_back({true, 4, 3});                              // 0x004d7a8d
    for (int i = 0; i < 1; ++i) want.push_back({true, 3, 3});  // 0x004d7acb
    want.push_back({true, 3, 2});                              // 0x004d7aea
    for (int i = 0; i < 14; ++i) want.push_back({true, 2, 2}); // 0x004d7b28
    want.push_back({true, 2, 1});                              // 0x004d7b47
    for (int i = 0; i < 9; ++i) want.push_back({true, 1, 1});  // 0x004d7b83

    auto run = [&]() {
        g_rec.clear();
        g_rec.map_w = f.map_w;
        g_rec.map_h = f.map_h;
        return mh::ai::detail::recompute_map_influence(f.view(), f.store(), stub_calls(), P);
    };

    // (a) the trigger is consumed, and the pass runs regardless of whether it was set.
    f.players[P].ai_map_changed_pending = 1;
    mh::ai::detail::influence_report r  = run();
    ck(f.players[P].ai_map_changed_pending == 0, "influence: ai_map_changed_pending is cleared");
    ck(r.was_pending, "influence: the report says the trigger was set on entry");
    const size_t n_set = g_rec.grid_calls.size();

    f.players[P].ai_map_changed_pending = 0;
    r                                   = run();
    ck(!r.was_pending, "influence: the report says the trigger was clear on entry");
    ck(g_rec.grid_calls.size() == n_set,
       "influence: the pass is unconditional -- same call count with the trigger already clear");

    // (b) the sequence, exactly: one fill then 27 floods, each with its own (level, fill) pair.
    ck(r.fill_calls == 1, "influence: exactly one grid_fill_below_threshold call");
    ck(r.flood_calls == 27, "influence: exactly 27 grid_flood_step calls (3 unlooped + 1 + 14 + 9)");
    ck(g_rec.grid_calls.size() == want.size(), "influence: 28 outward calls in total");
    if (g_rec.grid_calls.size() == want.size()) {
        bool kind_ok = true, args_ok = true;
        for (size_t i = 0; i < want.size(); ++i) {
            if (g_rec.grid_calls[i].flood != want[i].flood) kind_ok = false;
            if (g_rec.grid_calls[i].level != want[i].level ||
                g_rec.grid_calls[i].fill != want[i].fill)
                args_ok = false;
        }
        ck(kind_ok, "influence: fill-vs-flood in the right order (the fill is first and alone)");
        ck(args_ok, "influence: every (source_level, fill_value) pair matches the listing");
    }

    // (c) the grid pointer is THIS player's tile-flag grid, not player 0's.
    bool ptr_ok = true;
    for (const auto &g : g_rec.grid_calls)
        if (g.grid != (void *)f.players[P].ai_tile_flags_grid) ptr_ok = false;
    ck(ptr_ok, "influence: every call gets &player_data[P].ai_tile_flags_grid");
    ck((void *)f.players[P].ai_tile_flags_grid != (void *)f.players[0].ai_tile_flags_grid,
       "influence: the fixture can actually tell the two rows apart");

    // (d) WIDTH THEN HEIGHT, and the fixture is 64 x 48 so a swap cannot cancel out. This is the
    // check that the axis order committed in Ghidra is what the translation passes.
    bool dims_ok = true;
    for (const auto &g : g_rec.grid_calls)
        if (g.width != 64 || g.height != 48) dims_ok = false;
    ck(dims_ok, "influence: args are (width=64, height=48) in that order, not swapped");

    // (e) THE EXTENTS ARE RE-READ PER CALL. Rewrite map_width from inside the 5th flood stub; every
    // later call must see the new value. A translation that hoisted them reports 64 forever.
    f.map_w = 64;
    g_rec.clear();
    g_rec.grid_width_cell         = &f.map_w;
    g_rec.grid_width_change_after = 5;
    g_rec.grid_width_new          = 40;
    mh::ai::detail::recompute_map_influence(f.view(), f.store(), stub_calls(), P);
    bool saw_old = false, saw_new = false;
    for (size_t i = 0; i < g_rec.grid_calls.size(); ++i) {
        if (i < 5 && g_rec.grid_calls[i].width == 64) saw_old = true;
        if (i >= 5 && g_rec.grid_calls[i].width == 40) saw_new = true;
        if (i >= 5 && g_rec.grid_calls[i].width == 64) saw_new = false;
    }
    ck(saw_old, "influence: calls before the change still see the old width");
    ck(saw_new, "influence: the extents are RE-READ per call, not hoisted across the pass");
    f.map_w = 64;
}

// ---- batch B layer 1b: the build-queue reconcile -------------------------------------------------
void test_queue_reconcile() {
    using namespace mh::ai;
    using mh::ai::detail::reconcile_outcome;
    const uint32_t P = 2, BLDG = 7;

    fixture f;
    // The roster record the tail reads: a type id distinguishable from the roster slot, and a tile
    // whose x and y differ so a swap shows.
    auto arm_roster = [&](uint16_t type, uint8_t x, uint8_t y) {
        f.b((int)P, (int)BLDG).building_id = type;
        f.b((int)P, (int)BLDG).x           = x;
        f.b((int)P, (int)BLDG).y           = y;
    };
    auto run = [&](int32_t cost) {
        g_rec.clear();
        g_rec.cost_answer = cost;
        return detail::queue_reconcile_bldg_change(f.view(), f.store(), stub_calls(), P, BLDG);
    };
    auto entry = [&](int slot) -> mh::game::mh_llm_strat_ai_bldg_queue_entry & {
        return f.players[P].ai_bldg_queue[slot];
    };

    // (a) the flush is unconditional and takes the PLAYER, even on an empty queue.
    f.reset();
    arm_roster(41, 9, 17);
    detail::reconcile_report r = run(100);
    ck(g_rec.flushes.size() == 1, "reconcile: the train-entry flush runs exactly once");
    ck(!g_rec.flushes.empty() && g_rec.flushes[0] == (int32_t)P,
       "reconcile: the flush gets the player index");
    ck(r.scanned == 0, "reconcile: an empty queue is scanned as zero entries");

    // (b) the tail: type comes from the ROSTER RECORD's building_id, and x/y from the record.
    ck(g_rec.cost_queries.size() == 1 && g_rec.cost_queries[0] == 41,
       "reconcile: total_resource_cost is asked about building_id (41), not the roster slot (7)");
    ck(g_rec.constructions.size() == 1, "reconcile: a non-zero cost queues one construction");
    if (g_rec.constructions.size() == 1) {
        const auto &c = g_rec.constructions[0];
        ck(c.player == (int32_t)P, "reconcile: queue_construction gets the player");
        ck(c.building_type == 41, "reconcile: queue_construction gets building_id, not the slot");
        ck(c.x == 9 && c.y == 17, "reconcile: x and y are passed in that order, not swapped");
    }
    ck(r.outcome == reconcile_outcome::requeued, "reconcile: the outcome is `requeued`");

    // (c) a ZERO total cost queues nothing.
    f.reset();
    arm_roster(41, 9, 17);
    r = run(0);
    ck(g_rec.constructions.empty(), "reconcile: a zero-cost type queues NO construction");
    ck(r.outcome == reconcile_outcome::cost_zero, "reconcile: the outcome is `cost_zero`");

    // (d) PHASE 1 MATCHES THE KIND NIBBLE, not the whole byte -- the difference from its sibling
    // llm_strat_ai_queue_release_order, which compares 0x83/0x84 whole. 0x23 is kind 3 with the
    // affordability-waived bit set and it MUST match.
    for (uint8_t status : {(uint8_t)0x03, (uint8_t)0x23, (uint8_t)0x83, (uint8_t)0x04,
                           (uint8_t)0x84}) {
        f.reset();
        arm_roster(41, 9, 17);
        f.players[P].ai_bldg_queue_count = 1;
        entry(0).status                  = status;
        entry(0).building_index          = (int32_t)BLDG;
        r                                = run(100);
        ck(r.stamped, "reconcile: a repair/upgrade entry matches on the NIBBLE whatever its flags");
        ck(entry(0).status == (uint8_t)(status | 0xc0),
           "reconcile: the stamp ORs 0xc0 -- kind nibble and other bits survive");
    }

    // (e) a non-repair/upgrade kind, and a matching kind for a DIFFERENT building, are both skipped.
    for (int variant = 0; variant < 2; ++variant) {
        f.reset();
        arm_roster(41, 9, 17);
        f.players[P].ai_bldg_queue_count = 1;
        entry(0).status                  = (variant == 0) ? 0x02 : 0x03;
        entry(0).building_index          = (variant == 0) ? (int32_t)BLDG : (int32_t)BLDG + 1;
        r                                = run(100);
        ck(!r.stamped, variant == 0 ? "reconcile: kind 2 is not a repair/upgrade and is skipped"
                                    : "reconcile: a repair entry for another building is skipped");
        ck(entry(0).status == (uint8_t)((variant == 0) ? 0x02 : 0x03),
           "reconcile: a non-matching entry is left untouched");
    }

    // (f) phase 1 is FIRST-MATCH-WINS: the second matching entry is not stamped.
    f.reset();
    arm_roster(41, 9, 17);
    f.players[P].ai_bldg_queue_count = 2;
    entry(0).status                  = 0x03;
    entry(0).building_index          = (int32_t)BLDG;
    entry(1).status                  = 0x04;
    entry(1).building_index          = (int32_t)BLDG;
    r                                = run(100);
    ck(entry(0).status == 0xc3 && entry(1).status == 0x04,
       "reconcile: phase 1 stamps the FIRST match only and breaks");
    ck(r.slot == 0, "reconcile: the report names the slot phase 1 matched");

    // (g) PHASE 2 COMPARES THE WHOLE BYTE. 0x81 revives; 0xa1 (same nibble, extra bit) does not.
    for (int variant = 0; variant < 2; ++variant) {
        const uint8_t status = (variant == 0) ? 0x81 : 0xa1;
        f.reset();
        arm_roster(41, 9, 17);
        f.players[P].ai_bldg_queue_count = 1;
        entry(0).status                  = status;
        entry(0).building_index          = (int32_t)BLDG;
        r                                = run(100);
        if (variant == 0) {
            ck(entry(0).status == 0x01,
               "reconcile: a committed construction is ASSIGNED 0x01, not OR'd or masked");
            ck(r.outcome == reconcile_outcome::revived, "reconcile: the outcome is `revived`");
            ck(g_rec.constructions.empty() && g_rec.cost_queries.empty(),
               "reconcile: reviving RETURNS -- the cost query and re-queue never happen");
        } else {
            ck(entry(0).status == 0xa1,
               "reconcile: 0xa1 has the same nibble but is not the whole-byte 0x81 -- no revive");
            ck(r.outcome == reconcile_outcome::requeued,
               "reconcile: with no revive the tail runs and re-queues");
        }
    }

    // (h) A PHASE-1 STAMP DOES NOT RETURN. It breaks INTO phase 2, so a stamp and a revive can both
    // happen in one call -- the single most losable bit of this body's control flow.
    f.reset();
    arm_roster(41, 9, 17);
    f.players[P].ai_bldg_queue_count = 2;
    entry(0).status                  = 0x03;
    entry(0).building_index          = (int32_t)BLDG;
    entry(1).status                  = 0x81;
    entry(1).building_index          = (int32_t)BLDG;
    r                                = run(100);
    ck(entry(0).status == 0xc3, "reconcile: phase 1 still stamped");
    ck(entry(1).status == 0x01,
       "reconcile: and phase 2 still ran -- the phase-1 stamp is a BREAK, not a return");
    ck(r.outcome == reconcile_outcome::revived, "reconcile: the call ends as a revive");

    // (i) the count is the bound: an entry past ai_bldg_queue_count is invisible to both scans.
    f.reset();
    arm_roster(41, 9, 17);
    f.players[P].ai_bldg_queue_count = 1;
    entry(0).status                  = 0x00;
    entry(0).building_index          = -1;
    entry(1).status                  = 0x81; // parked beyond the count
    entry(1).building_index          = (int32_t)BLDG;
    r                                = run(100);
    ck(entry(1).status == 0x81, "reconcile: entries past ai_bldg_queue_count are not scanned");
    ck(r.outcome == reconcile_outcome::requeued, "reconcile: so the tail runs instead of reviving");

    // (j) the tail reads THIS player's roster row, not player 0's.
    f.reset();
    f.b(0, (int)BLDG).building_id = 77;
    f.b(0, (int)BLDG).x           = 1;
    f.b(0, (int)BLDG).y           = 2;
    arm_roster(41, 9, 17);
    r = run(100);
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 41,
       "reconcile: the roster row is player P's, not player 0's");
}

// ---- batch B layer 2a: the building weapon-range lookup -----------------------------------------
//
// Three hops and one early-out, so this is close to exhaustive. What it exists to pin is the
// SUBSCRIPT on Weapon::range_max, which is the first parameter (a player index) and not any of the
// three other indices in flight -- a mistake no rig scenario can catch, because the AI ticks each
// player over its own row and the wrong subscript agrees with the right one whenever they coincide.
void test_bldg_weapon_range() {
    using namespace mh::ai;
    fixture f;

    const int32_t P = 3, Q = 5, B = 11;
    f.b(P, B).building_id         = 21;
    f.b(Q, B).building_id         = 22; // a different type on another player's row
    f.cfg_buildings[21].weapon_id = 4;
    f.cfg_buildings[22].weapon_id = 4;
    for (int i = 0; i < 9; ++i) f.cfg_weapons[4].range_max[i] = 100 + i; // distinct per player

    ck(detail::building_defense_weapon_range(f.view(), (uint32_t)P, B) == 100 + P,
       "weapon_range: range_max is subscripted by the PLAYER argument");
    ck(detail::building_defense_weapon_range(f.view(), (uint32_t)Q, B) == 100 + Q,
       "weapon_range: and it follows the player, so it is not a fixed slot");
    // The roster row must be the player's too -- with two different type ids on the two rows, a
    // row-stride bug would pick up the wrong weapon entirely.
    f.cfg_buildings[22].weapon_id = 7;
    for (int i = 0; i < 9; ++i) f.cfg_weapons[7].range_max[i] = 500 + i;
    ck(detail::building_defense_weapon_range(f.view(), (uint32_t)Q, B) == 500 + Q,
       "weapon_range: the roster row read is the player's own");

    // The early-out: an unarmed building type returns 0 without reading Weapon[] at all. Point
    // Weapon[0] at a value that would be returned if the lookup ran anyway.
    for (int i = 0; i < 9; ++i) f.cfg_weapons[0].range_max[i] = 999;
    f.cfg_buildings[21].weapon_id = 0;
    ck(detail::building_defense_weapon_range(f.view(), (uint32_t)P, B) == 0,
       "weapon_range: weapon_id == 0 returns 0, it does not index Weapon[0]");

    // building_id is a u16 field read with MOVZX; a high value must not sign-extend.
    f.b(P, B).building_id         = 90;
    f.cfg_buildings[90].weapon_id = 4;
    ck(detail::building_defense_weapon_range(f.view(), (uint32_t)P, B) == 100 + P,
       "weapon_range: building_id is zero-extended, not sign-extended");
}

// ---- batch B layer 2b: the pending-training flush ------------------------------------------------
//
// The rig cannot be relied on to reach the write path here (the caller fires on a soft building
// removal and usually finds an empty queue), so the branch coverage lives here.
void test_train_flush() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P     = 2;
    auto          entry = [&](int i) -> auto          &{ return f.players[P].ai_bldg_queue[i]; };

    // Unit type 7 maps to AI role 3; type 8 to role 3 as well, so the role tally accumulates.
    f.cfg_units[7].ai_unit = 3;
    f.cfg_units[8].ai_unit = 3;
    f.cfg_units[9].ai_unit = 5;

    f.players[P].ai_bldg_queue_count = 5;
    entry(0).status                  = 0x00;
    entry(0).tick_or_unit_id         = 7; // flushed
    entry(1).status                  = 0x01;
    entry(1).tick_or_unit_id         = 7; // bit 0 set -> skipped
    entry(2).status                  = 0x80;
    entry(2).tick_or_unit_id         = 8; // non-zero -> skipped
    entry(3).status                  = 0x00;
    entry(3).tick_or_unit_id         = 9; // flushed
    entry(4).status                  = 0x20;
    entry(4).tick_or_unit_id         = 9; // non-zero, bit 0 clear -> skipped
    for (int i = 0; i < 100; ++i) f.players[P].ai_train_queued_by_unit_type[i] = 50;
    for (int i = 0; i < 12; ++i) f.players[P].ai_train_queued_by_ai_unit[i] = 50;

    detail::train_flush_report r =
        detail::queue_flush_unit_train_entries_2(f.view(), f.store(), P);
    ck(r.scanned == 5, "train_flush: the report records the queue count it scanned");
    ck(r.flushed == 2, "train_flush: exactly the two status == 0 entries are flushed");
    ck(entry(0).status == 0xc0 && entry(3).status == 0xc0,
       "train_flush: a matched entry is stamped 0xc0");
    ck(entry(1).status == 0x01 && entry(2).status == 0x80 && entry(4).status == 0x20,
       "train_flush: every other entry is left exactly as it was");
    ck(f.players[P].ai_train_queued_by_unit_type[7] == 49,
       "train_flush: the per-TYPE tally is decremented for the flushed type");
    ck(f.players[P].ai_train_queued_by_unit_type[9] == 49,
       "train_flush: and for the second one");
    ck(f.players[P].ai_train_queued_by_unit_type[8] == 50,
       "train_flush: a skipped entry's type tally is untouched");
    ck(f.players[P].ai_train_queued_by_ai_unit[3] == 49,
       "train_flush: the per-ROLE tally is indexed by Unit[type].ai_unit, not by the type");
    ck(f.players[P].ai_train_queued_by_ai_unit[5] == 49,
       "train_flush: the second flush hits its own role");
    ck(f.players[P].ai_train_queued_by_ai_unit[7] == 50,
       "train_flush: role 7 is untouched -- the role index is NOT the unit id");

    // The count is the bound, not the array extent.
    f.reset();
    f.cfg_units[7].ai_unit           = 3;
    f.players[P].ai_bldg_queue_count = 1;
    entry(0).status                  = 0x00;
    entry(0).tick_or_unit_id         = 7;
    entry(1).status                  = 0x00;
    entry(1).tick_or_unit_id         = 7; // parked past the count
    r                                = detail::queue_flush_unit_train_entries_2(f.view(), f.store(), P);
    ck(r.flushed == 1 && entry(1).status == 0x00,
       "train_flush: entries past ai_bldg_queue_count are not scanned");

    // An empty queue writes nothing at all -- the state a green rig run is most likely to have seen.
    f.reset();
    f.players[P].ai_bldg_queue_count = 0;
    entry(0).status                  = 0x00;
    r                                = detail::queue_flush_unit_train_entries_2(f.view(), f.store(), P);
    ck(r.scanned == 0 && r.flushed == 0 && entry(0).status == 0x00,
       "train_flush: a zero count is a no-op");

    // The row is the player's own.
    f.reset();
    f.cfg_units[7].ai_unit               = 3;
    f.players[P].ai_bldg_queue_count     = 1;
    entry(0).status                      = 0x00;
    entry(0).tick_or_unit_id             = 7;
    f.players[0].ai_bldg_queue_count     = 1;
    f.players[0].ai_bldg_queue[0].status = 0x00;
    (void)detail::queue_flush_unit_train_entries_2(f.view(), f.store(), P);
    ck(f.players[0].ai_bldg_queue[0].status == 0x00,
       "train_flush: player 0's queue is untouched when P != 0");
}

// ---- batch B layer 2c: the weighted spend-rate ratio ---------------------------------------------
//
// Two things here are invisible to any rig run and rest entirely on this test: the UNSIGNED
// widening of a wrapped 32-bit accumulator (the rings would have to overflow in a real match), and
// the ring-vs-accumulator pairing (numer ring -> numer, denom ring -> denom), which a rig can only
// separate if the two rings ever hold different values.
void test_spend_rate() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 4;

    // Weights are {1, 10, 100, 1000}; put a single 1 in one cell at a time so the resulting sum
    // NAMES the cell that produced it.
    f.players[P].ai_spend_rate_numer_ring[3 * 4 + 2] = 1; // slot 3, resource column 2 -> weight 100
    f.players[P].ai_spend_rate_denom_ring[7 * 4 + 1] = 1; // slot 7, column 1        -> weight 10
    detail::spend_rate_report r                      = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.numer == 100, "spend_rate: the numerator term uses weights[column], not weights[slot]");
    ck(r.denom == 10, "spend_rate: and the denominator ring feeds the denominator");
    ck(r.nonzero_terms == 2, "spend_rate: both non-zero ring cells were summed");
    ck(f.players[P].ai_spend_rate == 10.0, "spend_rate: ai_spend_rate == numer / denom");

    // The zero-denominator sentinel, and it is the branch a fresh base always takes.
    f.reset();
    f.players[P].ai_spend_rate_numer_ring[0] = 5;
    r                                        = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.denom_is_zero, "spend_rate: a zero denominator is reported as such");
    ck(f.players[P].ai_spend_rate == SPEND_RATE_NO_DENOM,
       "spend_rate: and writes the 1e13 sentinel rather than dividing");
    ck(r.numer == 5, "spend_rate: the numerator is still summed on the sentinel path");

    // ALL 32 SLOTS ARE SUMMED, not just the ones up to the cursor.
    f.reset();
    for (int s = 0; s < SPEND_RING_SLOTS; ++s) {
        f.players[P].ai_spend_rate_numer_ring[s * 4] = 1;
        f.players[P].ai_spend_rate_denom_ring[s * 4] = 1;
    }
    f.players[P].ai_spend_ring_cursor = 0;
    r                                 = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.numer == 32 && r.denom == 32,
       "spend_rate: every one of the 32 slots contributes regardless of the cursor");
    // nonzero_terms counts (slot, column) PAIRS that contributed, not ring entries -- both rings
    // are non-zero at the same 32 pairs here, so it is 32 and not 64.
    ck(r.nonzero_terms == 32, "spend_rate: 32 contributing (slot, column) pairs");

    // THE UNSIGNED WIDENING. A numerator whose accumulated int32 is negative must divide as a large
    // POSITIVE value, because the original zero-fills the high dword before FILD. A signed widening
    // would give a negative ratio here; a saturating one would give something else again.
    f.reset();
    f.players[P].ai_spend_rate_numer_ring[0] = (int32_t)0x80000000; // weight 1 -> INT32_MIN
    f.players[P].ai_spend_rate_denom_ring[0] = 2;
    r                                        = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.numer == (int32_t)0x80000000, "spend_rate: the accumulator itself is plain 32-bit");
    ck(f.players[P].ai_spend_rate == 2147483648.0 / 2.0,
       "spend_rate: the operands are ZERO-extended to 64 bits before the divide, not sign-extended");
    ck(f.players[P].ai_spend_rate > 0.0, "spend_rate: so a wrapped numerator stays positive");

    // The x87 helper on its own, including the same widening.
    ck(detail::x87_ratio(1, 3) == 1.0 / 3.0, "spend_rate: x87_ratio matches on an exact case");
    ck(detail::x87_ratio(0xffffffffu, 1) == 4294967295.0,
       "spend_rate: x87_ratio widens 0xffffffff to 4294967295, not to -1");

    // The cursor advances and wraps at 32, with an UNSIGNED compare.
    f.reset();
    f.players[P].ai_spend_ring_cursor = 30;
    r                                 = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.cursor == 31 && !r.wrapped, "spend_rate: 30 -> 31 without wrapping");
    r = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.cursor == 0 && r.wrapped, "spend_rate: 31 -> 0, the wrap");
    f.players[P].ai_spend_ring_cursor = -5;
    r                                 = detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(r.cursor == 0 && r.wrapped,
       "spend_rate: the wrap test is UNSIGNED, so a negative cursor resets too");

    // The row is the player's own: player 0 must be untouched.
    f.reset();
    (void)detail::resource_spend_rate_update(f.view(), f.store(), P);
    ck(f.players[0].ai_spend_rate == 0.0 && f.players[0].ai_spend_ring_cursor == 0,
       "spend_rate: player 0's row is untouched when P != 0");
}

// ---- batch B layer 2d: the unit-training plan ----------------------------------------------------
//
// Almost everything here is cheaper and sharper offline than on the rig, and three properties are
// effectively rig-INVISIBLE: the two INCLUSIVE loop bounds (a rig cfg's last row is not special),
// the clear loop's use of the BUILDING count for a UNIT-indexed array (harmless while the shipped
// cfg's building count covers the unit range), and the UNSIGNED role compare (a real queued tally
// is never negative).
void test_train_plan() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 3;

    // A minimal world where the plan gate is OPEN and one unit type is trainable.
    //   invention rows 1..3, all .type == 2
    //   row 1 -> unit type 10 (role 2), row 2 -> unit type 11 (role 2), row 3 -> unit type 12 (role 5)
    auto build_world = [&]() {
        f.reset();
        f.players[P].ai_build_plan_len_and_flag = 2;
        f.players[P].ai_build_plan_cursor       = 2; // 2 > 2 is false -> the gate opens
        f.bldg_sec.total                        = 60;
        f.prog_sec.total                        = 3;
        f.unit_sec.total                        = 20;
        for (int r = 1; r <= 3; ++r) {
            f.inventions[r].type   = INVENTION_TYPE_UNIT;
            f.prog(P, r).available = 1;
        }
        f.inventions[1].index    = 10;
        f.inventions[2].index    = 11;
        f.inventions[3].index    = 12;
        f.cfg_units[10].ai_unit  = 2;
        f.cfg_units[11].ai_unit  = 2;
        f.cfg_units[12].ai_unit  = 5;
        f.cfg_units[10].ai_level = 1;
        f.cfg_units[11].ai_level = 7;
        f.cfg_units[12].ai_level = 9;
        g_rec.build_sources.assign(100, 0);
        g_rec.build_sources[10] = 1;
        g_rec.build_sources[11] = 1;
        g_rec.build_sources[12] = 1;
    };
    auto run = [&]() {
        g_rec.trains.clear();
        g_rec.source_queries.clear();
        return detail::plan_unit_training(f.view(), f.store(), stub_calls(), P);
    };

    // ---- the build-plan gate ----
    build_world();
    f.players[P].ai_build_plan_len_and_flag = 3; // 3 > 2 -> still working through the plan
    detail::train_plan_report r             = run();
    ck(!r.ran && g_rec.source_queries.empty() && g_rec.trains.empty(),
       "train_plan: a player still consuming its opening build plan trains nothing at all");

    build_world();
    f.players[P].ai_build_plan_len_and_flag = 0x80000002u; // the high bit is a MODE flag, not length
    r                                       = run();
    ck(r.ran, "train_plan: the length field's 0x80000000 flag bit is masked off before the gate "
              "compare -- unmasked it would read as ~2.1 billion and close the gate forever");

    // ---- the clear loop: BUILDING count, INCLUSIVE ----
    build_world();
    f.bldg_sec.total = 5;
    for (int i = 0; i < 10; ++i) f.players[P].ai_train_source_state[i] = 9;
    r = run();
    ck(f.players[P].ai_train_source_state[5] == 0,
       "train_plan: the source-state clear is INCLUSIVE of the count (index 5 of 0..5 is cleared)");
    ck(f.players[P].ai_train_source_state[6] == 9 && f.players[P].ai_train_source_state[9] == 9,
       "train_plan: and it stops there -- the bound is G_BUILDING_COUNT_TOTAL, not the array extent");

    // ---- the invention walk ----
    build_world();
    f.inventions[0].type   = INVENTION_TYPE_UNIT; // row 0 exists and must be SKIPPED
    f.inventions[0].index  = 40;
    f.prog(P, 0).available = 1;
    r                      = run();
    ck(f.players[P].ai_train_source_state[40] == 0,
       "train_plan: the invention scan starts at row 1 -- row 0 is never looked at");
    ck(r.marked == 3, "train_plan: the scan is INCLUSIVE of G_PROGRESS_COUNT_TOTAL, so rows 1..3 "
                      "are all marked");
    ck(f.players[P].ai_train_source_state[10] == TRAIN_SOURCE_READY &&
           f.players[P].ai_train_source_state[12] == TRAIN_SOURCE_READY,
       "train_plan: an available row with f3 clear marks its unit type 1");

    build_world();
    f.inventions[2].type = 1; // not a unit row
    r                    = run();
    ck(f.players[P].ai_train_source_state[11] == 0 && r.marked == 2,
       "train_plan: a row whose .type is not 2 is skipped outright");

    build_world();
    f.prog(P, 2).available = 0;
    r                      = run();
    ck(f.players[P].ai_train_source_state[11] == 0 && r.marked == 2,
       "train_plan: a row the PLAYER does not have available is skipped");

    build_world();
    f.prog(P, 1).f3 = 1;
    r               = run();
    ck(f.players[P].ai_train_source_state[10] == TRAIN_SOURCE_F3_SET,
       "train_plan: f3 set marks the type 2 instead of 1");
    ck(r.roles_live == 2,
       "train_plan: and it `continue`s BEFORE the role tally, so type 10's role is not counted -- "
       "role 2 survives only through type 11");

    build_world();
    g_rec.build_sources[11] = 0; // type 11 has no production source
    g_rec.build_sources[10] = 0;
    r                       = run();
    ck(r.roles_live == 1 && r.best_role == 5,
       "train_plan: a marked type with NO build source does not contribute to its role tally");

    // ---- the role pick ----
    build_world();
    f.players[P].ai_train_queued_by_ai_unit[2] = 4;
    f.players[P].ai_train_queued_by_ai_unit[5] = 1;
    r                                          = run();
    ck(r.best_role == 5, "train_plan: the role with the FEWEST already queued wins");
    f.players[P].ai_train_queued_by_ai_unit[2] = 1;
    r                                          = run();
    ck(r.best_role == 2, "train_plan: a tie keeps the LOWER role index (strictly-less wins)");
    f.players[P].ai_train_queued_by_ai_unit[2] = -1; // 0xffffffff read unsigned
    f.players[P].ai_train_queued_by_ai_unit[5] = 3;
    r                                          = run();
    ck(r.best_role == 5,
       "train_plan: the queued-count compare is UNSIGNED, so a negative tally reads as ~4.29 "
       "billion and LOSES -- signed it would win every time");

    // ---- the unit pick ----
    build_world();
    f.players[P].ai_train_queued_by_ai_unit[5] = 99; // force role 2, which has types 10 and 11
    r                                          = run();
    ck(r.best_role == 2 && r.best_unit == 11,
       "train_plan: within the role, the HIGHEST cfg ai_level wins (11 at level 7 over 10 at 1)");
    f.cfg_units[11].ai_level = 1; // now tied with type 10
    r                        = run();
    ck(r.best_unit == 10,
       "train_plan: a level tie keeps the LOWER unit type (the compare is >=, so equal loses)");
    f.cfg_units[10].ai_level = -3;
    f.cfg_units[11].ai_level = -2;
    r                        = run();
    ck(r.best_unit == 0,
       "train_plan: the level seed is -1 and the compare is >=, so a role whose every type has a "
       "NEGATIVE ai_level yields nothing at all -- an unsigned compare, or a seed of 0 or INT_MIN, "
       "would each pick one of them");
    f.cfg_units[11].ai_level = 0; // the boundary: -1 >= 0 is false, so 0 is accepted
    r                        = run();
    ck(r.best_unit == 11,
       "train_plan: ...and level 0 IS accepted, which is what makes the previous case a real "
       "rejection rather than an empty role");

    build_world();
    f.unit_sec.total                           = 11;
    f.players[P].ai_train_queued_by_ai_unit[5] = 99;
    r                                          = run();
    ck(r.best_unit == 11,
       "train_plan: the unit scan is INCLUSIVE of G_UNIT_COUNT_TOTAL, so type 11 of 1..11 is seen");
    f.unit_sec.total = 10;
    r                = run();
    ck(r.best_unit == 10, "train_plan: and lowering the bound to 10 really does drop type 11");

    // ---- nothing to train ----
    build_world();
    g_rec.build_sources.assign(100, 0);
    r = run();
    ck(r.best_role == -1 && r.best_unit == 0 && g_rec.trains.empty(),
       "train_plan: with no build sources anywhere nothing is queued");

    // ---- the tail: the scripted starting-army drain ----
    build_world();
    f.players[P].ai_start_units_remaining = 0;
    r                                     = run();
    ck(r.queued == 1 && g_rec.trains.size() == 1,
       "train_plan: with no starting units left the pick is queued exactly once");
    // Role 2 and role 5 are tied at 0 queued, so the tie-break picks role 2, and within role 2 the
    // higher ai_level picks type 11 -- not type 12, which has the highest level of all three but
    // lives in the role the tie-break discarded. That ordering IS the funnel: role first, level
    // second.
    ck(g_rec.trains[0].player == P && g_rec.trains[0].unit_id == 11,
       "train_plan: it is queued for the player and type the passes chose -- role BEFORE level, so "
       "type 12 at level 9 loses to type 11 at level 7 because role 5 lost the role tie-break");

    build_world();
    f.players[P].ai_start_units_remaining = 1;
    r                                     = run();
    ck(r.queued == 1 && f.players[P].ai_start_units_remaining == 0,
       "train_plan: remaining == 1 gives ONE queue call and drains the counter -- the first "
       "decrement happens before the loop test, so it is not two");

    build_world();
    f.players[P].ai_start_units_remaining = 4;
    r                                     = run();
    ck(r.queued == 4 && f.players[P].ai_start_units_remaining == 0,
       "train_plan: ONE planning pass drains the whole starting-unit counter (4 -> 4 calls, 0 "
       "left), it is not one per tick");
    bool same = g_rec.trains.size() == 4;
    for (const auto &t : g_rec.trains) same = same && t.unit_id == 11;
    ck(same, "train_plan: every call of the drain queues the SAME unit type");

    // ---- the row is the player's own ----
    build_world();
    r = run();
    ck(f.players[0].ai_train_source_state[10] == 0 && f.players[0].ai_start_units_remaining == 0,
       "train_plan: player 0's row is untouched when P != 0");
    ck(!g_rec.source_queries.empty() && g_rec.source_queries[0] == P,
       "train_plan: and the build-source query asks for P");
}

// ---- batch B layer 2e: the build-queue pump ------------------------------------------------------
//
// Phase 1's re-test-the-same-slot behaviour and the unaffordable path's stop-the-whole-queue return
// are the two things most likely to be mistranslated here, and neither is reliably reachable on the
// rig (the queue usually holds committed entries only). Both are pinned below.
void test_bldg_queue() {
    using namespace mh::ai;
    fixture        f;
    const uint32_t P     = 2;
    auto           entry = [&](int i) -> auto           &{ return f.players[P].ai_bldg_queue[i]; };
    auto           run   = [&]() {
        g_rec.dispatches.clear();
        g_rec.grants.clear();
        return detail::bldg_queue_process(f.view(), f.store(), stub_calls(), P);
    };

    // ---- phase 1: compaction ----
    f.reset();
    f.players[P].ai_bldg_queue_count = 4;
    entry(0).status                  = QUEUE_STATUS_REMOVED;
    entry(1).status                  = 0x81; // committed, so phase 2 skips it
    entry(2).status                  = QUEUE_STATUS_REMOVED;
    entry(3).status                  = 0x82;
    detail::bldg_queue_report r      = run();
    ck(r.compacted == 2 && f.players[P].ai_bldg_queue_count == 2,
       "bldg_queue: both removed entries are compacted out and the count follows");
    ck(entry(0).status == 0x81 && entry(1).status == 0x82,
       "bldg_queue: the survivors keep their order");

    // THE RE-TEST. Two ADJACENT removed entries at the head. If the loop did not re-examine slot i
    // after a removal, the second one would slide into slot 0, be skipped, and survive.
    f.reset();
    f.players[P].ai_bldg_queue_count = 3;
    entry(0).status                  = QUEUE_STATUS_REMOVED;
    entry(1).status                  = QUEUE_STATUS_REMOVED;
    entry(2).status                  = 0x83;
    r                                = run();
    ck(r.compacted == 2 && f.players[P].ai_bldg_queue_count == 1 && entry(0).status == 0x83,
       "bldg_queue: a run of ADJACENT removed entries is fully drained -- the slot is re-tested "
       "after each removal (the DEC falls through into the loop head), it is not skipped");

    // The shift moves the WHOLE 18-byte entry, not just the status byte.
    f.reset();
    f.players[P].ai_bldg_queue_count = 2;
    entry(0).status                  = QUEUE_STATUS_REMOVED;
    entry(1).status                  = 0x84;
    entry(1).tick_or_unit_id         = 77;
    entry(1).build_tile_x            = 1234;
    entry(1).resource_reserved[3]    = 555;
    entry(1).building_index          = 0x11223344;
    r                                = run();
    ck(entry(0).status == 0x84 && entry(0).tick_or_unit_id == 77 && entry(0).build_tile_x == 1234 &&
           entry(0).resource_reserved[3] == 555 && entry(0).building_index == 0x11223344,
       "bldg_queue: the compaction shift copies all 18 bytes of the entry");

    // ---- phase 2: the committed skip ----
    f.reset();
    f.players[P].ai_bldg_queue_count = 2;
    entry(0).status                  = QUEUE_STATUS_COMMITTED | 3;
    entry(1).status                  = 1;
    r                                = run();
    ck(r.skipped_done == 1 && r.dispatched == 1 && g_rec.dispatches.size() == 1 &&
           g_rec.dispatches[0].kind == 1,
       "bldg_queue: a committed (0x80) entry is skipped and does not reach the jump table");

    // ---- the affordability test ----
    auto one_entry = [&](uint8_t status, int16_t cost_res1, int32_t spent_res1) {
        f.reset();
        f.players[P].ai_bldg_queue_count = 1;
        entry(0).status                  = status;
        entry(0).resource_reserved[1]    = cost_res1;
        f.players[P].resource_spent[0]   = spent_res1; // id 1 -> element 0 since the reshape
    };

    one_entry(1, 10, 10);
    r = run();
    ck(r.dispatched == 1 && !r.stopped_short,
       "bldg_queue: cost EQUAL to the spend is affordable (the test is >, not >=)");

    one_entry(1, 11, 10);
    r = run();
    ck(r.dispatched == 0 && r.stopped_short && r.granted == 1,
       "bldg_queue: cost above the spend is unaffordable -- it grants and stops");
    ck(g_rec.grants.size() == 1 && g_rec.grants[0].player == (uint16_t)P &&
           g_rec.grants[0].res_type == 1 && g_rec.grants[0].amount == 11,
       "bldg_queue: the grant asks for (player, resource id j+1, the reserved cost)");

    // THE ZERO-EXTENSION. -1 as an int16 must read as 65535, not as -1.
    one_entry(1, (int16_t)-1, 10);
    r = run();
    ck(r.stopped_short && g_rec.grants.size() == 1 && g_rec.grants[0].amount == 65535,
       "bldg_queue: the reserved cost is a short read ZERO-extended -- sign-extended it would be "
       "-1, compare as affordable, and dispatch instead");

    // A mine yield for the shorted resource suppresses the grant but NOT the stop.
    one_entry(1, 11, 10);
    f.players[P].ai_mine_yield_by_resource[1] = 5;
    r                                         = run();
    ck(r.stopped_short && r.granted == 0,
       "bldg_queue: a non-zero mine yield for that resource suppresses the grant, and the queue "
       "still stops");

    // The waiver is applied AFTER the affordability loop, so it rescues a failure.
    one_entry(QUEUE_STATUS_AFFORD_WAIVED | 1, 999, 0);
    r = run();
    ck(r.dispatched == 1 && r.waived == 1 && !r.stopped_short && g_rec.grants.empty(),
       "bldg_queue: the 0x20 waiver overrides an affordability FAILURE (it is applied after the "
       "loop, not as a skip of it)");

    // The stop is the whole function: a later, perfectly affordable entry is not reached.
    f.reset();
    f.players[P].ai_bldg_queue_count = 2;
    entry(0).status                  = 1;
    entry(0).resource_reserved[1]    = 5;
    entry(1).status                  = 1;
    f.players[P].resource_spent[0]   = 0; // id 1
    r                                = run();
    ck(r.stopped_short && r.dispatched == 0 && r.scanned == 1,
       "bldg_queue: an unaffordable entry stops the WHOLE pass -- the table at 0x004e8283 is five "
       "copies of the epilogue, so nothing after it is looked at this tick");

    // All four resource columns are tested, and each names its own id.
    f.reset();
    f.players[P].ai_bldg_queue_count = 1;
    entry(0).status                  = 1;
    for (int j = 0; j < 4; ++j) {
        entry(0).resource_reserved[1 + j] = (int16_t)(7 + j);
        f.players[P].resource_spent[j]    = 0;
    }
    r = run();
    ck(r.granted == 4 && g_rec.grants.size() == 4,
       "bldg_queue: all four cost columns are checked on the grant path");
    bool ids_ok = true;
    for (int j = 0; j < 4; ++j)
        ids_ok = ids_ok && g_rec.grants[(size_t)j].res_type == (uint32_t)(j + 1) &&
                 g_rec.grants[(size_t)j].amount == (uint32_t)(7 + j);
    ck(ids_ok, "bldg_queue: column j maps to resource id j+1 -- reserved[1+j] against spent[1+j], "
               "so slot 0 of either array is never a cost");

    // ---- the dispatch table ----
    f.reset();
    f.players[P].ai_bldg_queue_count = 7;
    for (int i = 0; i < 7; ++i) entry(i).status = (uint8_t)i; // nibbles 0..6
    r = run();
    ck(r.dispatched == 5 && g_rec.dispatches.size() == 5,
       "bldg_queue: nibbles 0..4 dispatch and 5/6 are skipped (CMP AL,4 / JA)");
    ck(r.by_kind[0] == 1 && r.by_kind[1] == 1 && r.by_kind[2] == 1 && r.by_kind[3] == 1 &&
           r.by_kind[4] == 1,
       "bldg_queue: each of the five live nibbles is taken exactly once");
    ck(g_rec.dispatches[0].kind == 0 && g_rec.dispatches[1].kind == 1 &&
           g_rec.dispatches[2].kind == 2,
       "bldg_queue: nibble 0 -> recruit_state, 1 -> process_entry, 2 -> the empty handler");
    ck(g_rec.dispatches[3].kind == 3 && g_rec.dispatches[4].kind == 3,
       "bldg_queue: nibbles 3 AND 4 both route to handle_upgrade_or_cancel -- table slots 3 and 4 "
       "hold the same target");
    ck(g_rec.dispatches[0].player == P && g_rec.dispatches[0].index == 0 &&
           g_rec.dispatches[4].index == 4,
       "bldg_queue: each handler is passed (player, the entry's SLOT index)");

    // ---- the count is re-read at the loop head ----
    f.reset();
    f.players[P].ai_bldg_queue_count = 1;
    entry(0).status                  = 1; // process_entry, whose stub appends one more
    g_rec.append_on_process_entry    = 1;
    g_rec.append_target_queue        = f.players[P].ai_bldg_queue;
    g_rec.append_target_count        = &f.players[P].ai_bldg_queue_count;
    g_rec.append_status              = 0; // nibble 0 -> recruit_state
    r                                = run();
    g_rec.append_target_count        = nullptr;
    ck(r.dispatched == 2 && g_rec.dispatches.size() == 2 && g_rec.dispatches[1].kind == 0,
       "bldg_queue: the count is re-read at every loop head, so an entry a handler appended IS "
       "dispatched -- a hoisted count would stop at the original one");

    // ---- the row is the player's own ----
    f.reset();
    f.players[P].ai_bldg_queue_count     = 1;
    entry(0).status                      = QUEUE_STATUS_REMOVED;
    f.players[0].ai_bldg_queue_count     = 1;
    f.players[0].ai_bldg_queue[0].status = QUEUE_STATUS_REMOVED;
    r                                    = run();
    ck(f.players[0].ai_bldg_queue_count == 1 && f.players[0].ai_bldg_queue[0].status ==
                                                    QUEUE_STATUS_REMOVED,
       "bldg_queue: player 0's queue is untouched when P != 0");

    // An empty queue is a no-op -- the state a green rig run is most likely to have seen.
    f.reset();
    r = run();
    ck(r.compacted == 0 && r.scanned == 0 && g_rec.dispatches.empty(),
       "bldg_queue: a zero count is a no-op in both phases");
}

void test_build_sources() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 2;
    int32_t       out[256];

    auto run = [&]() {
        for (int i = 0; i < 256; ++i) out[i] = 0x5a5a;
        return detail::count_unit_build_sources(f.view(), P, out);
    };
    // A world where player P owns two buildings, both of which can build unit types 1 and 3, and
    // one of which can also build type 2. Housing is wide open unless a test closes it.
    auto build_world = [&]() {
        f.reset();
        f.unit_sec.total                 = 4;
        f.b(P, 0).index                  = 2; // the header slot's live-building count
        f.b(P, 1).building_id            = 7;
        f.b(P, 2).building_id            = 9;
        f.cfg_buildings[7].unit_quant[1] = 1.0;
        f.cfg_buildings[7].unit_quant[3] = 1.0;
        f.cfg_buildings[9].unit_quant[1] = 1.0;
        f.cfg_buildings[9].unit_quant[2] = 1.0;
        f.cfg_buildings[9].unit_quant[3] = 1.0;
        for (int t = 1; t <= 4; ++t) f.cfg_units[t].ai_unit = 2; // the vehicles class
        f.housing[P].used_vehicles     = 0;
        f.housing[P].cap_prev_vehicles = 99;
        f.housing[P].used_soldiers     = 0;
        f.housing[P].cap_prev_soldiers = 99;
        f.housing[P].used_planes       = 0;
        f.housing[P].cap_prev_planes   = 99;
        f.housing[P].used_helis        = 0;
        f.housing[P].cap_prev_helis    = 99;
    };

    // ---- the census itself ----
    build_world();
    build_sources_report r = run();
    ck(out[1] == 2 && out[2] == 1 && out[3] == 2,
       "build_sources: each occupied building contributes 1 to every unit type its cfg production "
       "menu lists, so two buildings that both list type 1 give a count of 2");
    ck(out[4] == 0, "build_sources: a unit type no building lists stays 0");
    ck(r.scanned == 2 && r.empty_slots == 0, "build_sources: both occupied slots were visited");

    // ---- the clear loop starts at 1 and is INCLUSIVE of `total` ----
    build_world();
    r = run();
    ck(out[0] == 0x5a5a,
       "build_sources: index 0 is NEVER written -- every loop starts at 1 (0x004e21fa), so "
       "out_counts[0] keeps whatever the caller left there");
    ck(out[4] == 0, "build_sources: the clear is INCLUSIVE of total (index 4 of 1..4 is cleared, "
                    "JBE @0x004e220b)");
    ck(out[5] == 0x5a5a, "build_sources: and it stops there -- index total+1 is untouched");

    // ---- the float test is STRICTLY greater than zero ----
    build_world();
    f.cfg_buildings[7].unit_quant[4] = 0.0;
    f.cfg_buildings[9].unit_quant[4] = -1.0;
    r                                = run();
    ck(out[4] == 0,
       "build_sources: unit_quant of exactly 0.0 and of a NEGATIVE value both read as 'cannot "
       "build' -- the test is 0.0 < quant (FLDZ/FCOMP/JNC @0x004e2283), not != 0");
    build_world();
    f.cfg_buildings[7].unit_quant[4] = 0.25; // a fraction, not a whole number
    r                                = run();
    ck(out[4] == 1, "build_sources: any strictly positive quant counts as one source, including a "
                    "fractional one -- the count is of BUILDINGS, not of the quant value");

    // ---- the roster walk is COUNT-DRIVEN, and empty slots do not consume the count ----
    build_world();
    f.b(P, 1).building_id = 0; // slot 1 empty; the two real buildings move outward
    f.b(P, 2).building_id = 7;
    f.b(P, 5).building_id = 9;
    r                     = run();
    ck(r.scanned == 2 && r.empty_slots == 3,
       "build_sources: the walk is driven by buildings[player][0].index, not by a slot bound -- it "
       "keeps stepping the cursor over empty slots until it has SEEN that many occupied ones");
    ck(out[1] == 2, "build_sources: and it finds both of them wherever they sit");

    // ---- and NOTHING bounds the cursor ----
    // The header claims three occupied slots and only two exist in the row, so the walk keeps
    // going. The third is planted in the NEXT player's row, which is where the original finds it
    // too -- the rosters are one contiguous [8][100] block. Without that terminator the loop does
    // not end: the original relies on eventually meeting a non-zero word somewhere in .bss, and
    // this test crashed with an access violation the first time it was written without one, which
    // is a fair demonstration of how unbounded the walk really is.
    build_world();
    f.b(P, 0).index           = 3;
    f.b(P + 1, 4).building_id = 7;
    r                         = run();
    ck(r.scanned == 3 && r.max_cursor > BUILDINGS_PER_PLAYER,
       "build_sources: a header count larger than the number of occupied slots in the row walks "
       "the cursor straight into the NEXT player's roster and counts a building there -- "
       "reproduced verbatim, the original has no bound either");
    ck(out[1] == 3, "build_sources: and the foreign building's production menu is counted as if it "
                    "were the ticking player's own");

    // ---- a zero header count means the census is the clear loop and nothing else ----
    build_world();
    f.b(P, 0).index = 0;
    r               = run();
    ck(r.scanned == 0 && out[1] == 0 && out[3] == 0,
       "build_sources: a player with no live buildings gets an all-zero table, not a stale one");

    // ---- the housing veto: four classes, each paired with its OWN latched capacity ----
    // Each sub-case fills exactly one class and checks the buildable types of that class only,
    // which is what a crossed pairing fails.
    struct classcase {
        uint32_t role;
        int32_t housing_stats::*used;
        int32_t housing_stats::*cap;
    };
    const classcase cases[] = {
        {2, &housing_stats::used_vehicles, &housing_stats::cap_prev_vehicles},
        {3, &housing_stats::used_vehicles, &housing_stats::cap_prev_vehicles},
        {4, &housing_stats::used_vehicles, &housing_stats::cap_prev_vehicles},
        {5, &housing_stats::used_vehicles, &housing_stats::cap_prev_vehicles},
        {1, &housing_stats::used_soldiers, &housing_stats::cap_prev_soldiers},
        {6, &housing_stats::used_helis, &housing_stats::cap_prev_helis},
        {7, &housing_stats::used_planes, &housing_stats::cap_prev_planes},
        {8, &housing_stats::used_planes, &housing_stats::cap_prev_planes},
    };
    for (const classcase &c : cases) {
        build_world();
        for (int t = 1; t <= 4; ++t) f.cfg_units[t].ai_unit = c.role;
        f.housing[P].*c.used = 5;
        f.housing[P].*c.cap  = 5; // used == cap -- the boundary, not a comfortable margin
        r                    = run();
        ck(out[1] == 0 && out[3] == 0 && r.vetoed == 4,
           "build_sources: at exactly used == cap the whole class is vetoed to zero -- the compare "
           "is >=, so the boundary case vetoes");

        build_world();
        for (int t = 1; t <= 4; ++t) f.cfg_units[t].ai_unit = c.role;
        f.housing[P].*c.used = 4;
        f.housing[P].*c.cap  = 5;
        r                    = run();
        ck(out[1] == 2 && r.vetoed == 0, "build_sources: one below capacity vetoes nothing");
    }

    // ---- the veto reads the LATCHED column, not the accumulator ----
    build_world();
    f.housing[P].used_vehicles      = 9;
    f.housing[P].cap_prev_vehicles  = 99; // latched: plenty of room
    f.housing[P].cap_accum_vehicles = 1;  // this tick's accumulator: would veto if it were read
    r                               = run();
    ck(out[1] == 2 && r.vetoed == 0,
       "build_sources: the veto compares used_* against cap_prev_* (the LATCHED capacity), NOT "
       "cap_accum_* -- reading the accumulator would make the answer depend on where in the tick "
       "the AI happened to run");

    // ---- a full class does not veto a DIFFERENT class ----
    build_world();
    f.cfg_units[1].ai_unit         = 1; // soldier
    f.cfg_units[3].ai_unit         = 6; // heli
    f.housing[P].used_soldiers     = 5;
    f.housing[P].cap_prev_soldiers = 5;
    r                              = run();
    ck(out[1] == 0 && out[3] == 2,
       "build_sources: a full soldier bay vetoes only role-1 types; the heli stays buildable");

    // ---- a role outside 1..8 is never vetoed ----
    build_world();
    for (int t = 1; t <= 4; ++t) f.cfg_units[t].ai_unit = 0;
    f.housing[P].used_vehicles     = 99;
    f.housing[P].cap_prev_vehicles = 0;
    f.housing[P].used_soldiers     = 99;
    f.housing[P].cap_prev_soldiers = 0;
    f.housing[P].used_planes       = 99;
    f.housing[P].cap_prev_planes   = 0;
    f.housing[P].used_helis        = 99;
    f.housing[P].cap_prev_helis    = 0;
    r                              = run();
    ck(out[1] == 2 && r.vetoed == 0,
       "build_sources: ai_unit 0 matches none of the four class tests, so it survives every bay "
       "being full -- the original has no default arm");

    // ---- the player index really selects the row ----
    build_world();
    f.housing[0].used_vehicles     = 99;
    f.housing[0].cap_prev_vehicles = 0; // player 0's bay is full
    r                              = run();
    ck(out[1] == 2 && r.vetoed == 0,
       "build_sources: the housing row read is the TICKING player's, not player 0's");
    // Player 0's header must name a count that is REACHABLE in player 0's row (1, with one building
    // planted). A larger fake count would send a translation that read row 0 off hunting for
    // buildings that do not exist, and it would crash or hang instead of failing this assertion --
    // which is a detection, but not one that NAMES the defect. Chosen so the mutant terminates and
    // reports a wrong roster_count.
    build_world();
    f.b(0, 0).index       = 1;
    f.b(0, 1).building_id = 7;
    r                     = run();
    ck(r.roster_count == 2, "build_sources: the roster row read is the ticking player's too");
}

// ---- ai/ai_queue_enqueue.cpp (AI1B layer 3): the three APPEND sides of the AI build queue --------
//
// Aimed at plausible-but-wrong readings rather than at the happy path, because the happy path here
// is four field stores that any transcription gets right. The readings under test are: which of the
// two cfg cost lists each function walks, how far a cost walk goes, which fields each one does NOT
// clear, that the per-unit cap masks the kind NIBBLE rather than the whole status byte, and that the
// repair tick loop is a strict `<` over doubles.
void test_queue_enqueue() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P     = 2;
    auto          entry = [&](int i) -> auto          &{ return f.players[P].ai_bldg_queue[i]; };

    // ---- the shared full-queue guard ----
    // 0x40 EXACTLY, and `>=` not `>`. A translation using `> 0x40` appends a 65th entry, one past
    // the array, which is the failure this pair exists to name.
    f.reset();
    f.players[P].ai_bldg_queue_count = 0x3f;
    // A DEFAULT fixture rejects here, and not for the reason the guard names: with train_cap 0 and
    // no scripted start units left, `matching >= cap` is `0 >= 0` and every train enqueue is refused.
    // That is the original's behaviour (JNC at 0x004e2717, an unsigned >=) and it is worth its own
    // check, but it also means the guard test has to step around the cap branch.
    ck(detail::queue_train_unit(f.view(), f.store(), P, 5).rc == 1,
       "train: a per-unit cap of 0 refuses EVERY train enqueue once start units are exhausted");
    f.players[P].ai_start_units_remaining = 1;
    ck(detail::queue_train_unit(f.view(), f.store(), P, 5).rc == 0 &&
           f.players[P].ai_bldg_queue_count == 0x40,
       "enqueue: count 0x3f still appends -- the guard is on 0x40, not 0x3f");
    ck(detail::queue_train_unit(f.view(), f.store(), P, 5).rc == 1 &&
           f.players[P].ai_bldg_queue_count == 0x40,
       "enqueue: count 0x40 rejects with 1 and does NOT bump the count");
    f.players[P].ai_bldg_queue_count = 0x40;
    // The count is re-asserted after EACH sibling, not just the rc: the guard's whole job is that
    // nothing is written, and a body that returned 1 and still bumped the count would satisfy an
    // rc-only check while overflowing the array. (Found by mutation -- the rc-only form missed it.)
    const bool rej_repair = detail::queue_bldg_repair(f.view(), f.store(), P, 0).rc == 1 &&
                            f.players[P].ai_bldg_queue_count == 0x40;
    const bool rej_upgrade =
        detail::queue_bldg_upgrade(f.view(), f.store(), stub_calls(), P, 0).rc == 1 &&
        f.players[P].ai_bldg_queue_count == 0x40;
    ck(rej_repair && rej_upgrade,
       "enqueue: all three siblings share the 0x40 guard, return 1, and leave the count alone");

    // ---- queue_train_unit @0x004e268c ----
    f.reset();
    f.cfg_units[7].ai_unit = 3; // the ROLE tally index, deliberately != the type id
    // Seven cost entries. Entries 4, 5 and 6 are the ones that only exist because cfg Unit.resource
    // is [7] -- with the [4] the DB carried until 2026-08-02 they would be unreachable, so these
    // three lines are the offline half of ghidra_findings 2026-08-02-1830-1.
    for (int d = 0; d < 7; ++d) {
        f.cfg_units[7].resource[d].id  = (uint32_t)(1 + (d % 4));
        f.cfg_units[7].resource[d].val = 10 + d;
    }
    // resource_2 is loaded with values that must NOT appear: train walks `resource` only.
    for (int d = 0; d < 4; ++d) {
        f.cfg_units[7].resource_2[d].id  = 1;
        f.cfg_units[7].resource_2[d].val = 1000;
    }
    f.players[P].ai_start_units_remaining = 1; // skip the cap branch for this one
    entry(0).build_tile_x                 = 0x1234;
    entry(0).resource_reserved[0]         = 0x5678;
    detail::enqueue_report r              = detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(r.rc == 0 && r.slot == 0 && entry(0).status == 0 && entry(0).tick_or_unit_id == 7,
       "train: a fresh entry is status 0 with the unit id in tick_or_unit_id");
    ck(entry(0).build_tile_x == 0 && entry(0).resource_reserved[0] == 0,
       "train: build_tile_x AND resource_reserved[0] are cleared -- unlike its two siblings");
    ck(r.cost_ids == 7, "train: the cost walk consumes SEVEN entries, not four");
    // ids cycle 1,2,3,4,1,2,3 over vals 10..16 -> [1]=10+14, [2]=11+15, [3]=12+16, [4]=13.
    ck(entry(0).resource_reserved[1] == 24 && entry(0).resource_reserved[2] == 26 &&
           entry(0).resource_reserved[3] == 28 && entry(0).resource_reserved[4] == 13,
       "train: costs accrue by the entry's .id, and entries 4..6 land on top of 0..2");
    ck(f.players[P].ai_train_queued_by_unit_type[7] == 1 &&
           f.players[P].ai_train_queued_by_ai_unit[3] == 1 &&
           f.players[P].ai_train_queued_by_ai_unit[7] == 0,
       "train: one tally is indexed by the TYPE, the other by Unit[type].ai_unit");
    ck(f.players[P].ai_bldg_queue_count == 1, "train: the count is bumped exactly once");

    // The walk stops at the first zero id and does NOT skip past it.
    f.reset();
    f.players[P].ai_start_units_remaining = 1;
    f.cfg_units[7].resource[0].id         = 2;
    f.cfg_units[7].resource[0].val        = 5;
    f.cfg_units[7].resource[1].id         = 0; // terminator
    f.cfg_units[7].resource[2].id         = 3;
    f.cfg_units[7].resource[2].val        = 99;
    r                                     = detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(r.cost_ids == 1 && entry(0).resource_reserved[2] == 5 && entry(0).resource_reserved[3] == 0,
       "train: a zero id ENDS the walk -- it is not skipped over");

    // `val` is read as a WORD, so the high half is dropped.
    f.reset();
    f.players[P].ai_start_units_remaining = 1;
    f.cfg_units[7].resource[0].id         = 1;
    f.cfg_units[7].resource[0].val        = 0x00010005;
    detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(entry(0).resource_reserved[1] == 5,
       "train: a cost is accrued from the LOW 16 bits of .val, not all 32");

    // The per-unit cap: reached only once the scripted start units are gone.
    f.reset();
    f.train_cap                           = 2;
    f.players[P].ai_bldg_queue_count      = 2;
    entry(0).status                       = 0x00;
    entry(0).tick_or_unit_id              = 7;
    entry(1).status                       = 0xc0; // kind nibble still 0 -- it COUNTS
    entry(1).tick_or_unit_id              = 7;
    f.players[P].ai_start_units_remaining = 3;
    r                                     = detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(r.rc == 0 && !r.cap_check,
       "train: a non-zero ai_start_units_remaining skips the cap check entirely");
    f.players[P].ai_bldg_queue_count      = 2; // undo the append above
    f.players[P].ai_start_units_remaining = 0;
    r                                     = detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(r.rc == 1 && r.cap_check && r.cap_hits == 2 && f.players[P].ai_bldg_queue_count == 2,
       "train: the cap masks the kind NIBBLE, so a 0xc0 train entry still counts against it");
    f.train_cap = 3;
    r           = detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(r.rc == 0 && r.cap_hits == 2, "train: the cap test is >=, so 2 matches under a cap of 3 pass");
    // A DIFFERENT type is not counted, and a non-train kind is not counted.
    f.reset();
    f.train_cap                      = 1;
    f.players[P].ai_bldg_queue_count = 2;
    entry(0).status                  = 0x00;
    entry(0).tick_or_unit_id         = 8; // other type
    entry(1).status                  = 0x03;
    entry(1).tick_or_unit_id         = 7; // right type, wrong kind
    r                                = detail::queue_train_unit(f.view(), f.store(), P, 7);
    ck(r.rc == 0 && r.cap_hits == 0,
       "train: the cap counts only kind-0 entries whose unit id matches");

    // ---- queue_bldg_repair @0x004e284d ----
    // The decisive one: repair walks resource_2, upgrade walks resource. Load them differently so a
    // translation that picks the wrong list cannot pass.
    f.reset();
    const int32_t BT                      = 11;
    f.b(P, 4).building_id                 = (uint16_t)BT;
    f.b(P, 4).energy                      = 40.0;
    f.cfg_buildings[BT].energy            = 100.0;
    f.cfg_buildings[BT].energy_d          = 25.0;
    f.cfg_buildings[BT].resource[0].id    = 1;
    f.cfg_buildings[BT].resource[0].val   = 777; // the UPGRADE list -- must not appear here
    f.cfg_buildings[BT].resource_2[0].id  = 2;
    f.cfg_buildings[BT].resource_2[0].val = 30;
    f.cfg_buildings[BT].resource_2[1].id  = 4;
    f.cfg_buildings[BT].resource_2[1].val = 7;
    entry(0).resource_reserved[0]         = 0x0abc; // NOT cleared by repair
    entry(0).build_tile_x                 = 0x0def; // NOT cleared by repair either
    entry(0).tick_or_unit_id              = 99;     // IS cleared, but only after the cost walk
    r                                     = detail::queue_bldg_repair(f.view(), f.store(), P, 4);
    ck(r.rc == 0 && entry(0).status == 3 && entry(0).building_index == 4,
       "repair: kind 3 with the ROSTER slot in building_index");
    ck(entry(0).resource_reserved[2] == 30 && entry(0).resource_reserved[4] == 7 &&
           entry(0).resource_reserved[1] == 0,
       "repair: costs come from Building.resource_2, NOT Building.resource");
    ck(entry(0).resource_reserved[0] == 0x0abc && entry(0).build_tile_x == 0x0def,
       "repair: resource_reserved[0] and build_tile_x are LEFT AS FOUND -- only [1..4] are cleared");
    // 40 -> 65 -> 90 -> 115; the third crosses 100, so three ticks.
    ck(r.ticks == 3 && entry(0).tick_or_unit_id == 3,
       "repair: the tick count is the number of energy_d steps needed to reach the type's energy");
    ck(f.players[P].ai_bldg_queue_count == 1, "repair: the count is bumped once");

    // Already at full charge: the compare is strictly-less, so zero ticks, not one.
    f.reset();
    f.b(P, 4).building_id        = (uint16_t)BT;
    f.b(P, 4).energy             = 100.0;
    f.cfg_buildings[BT].energy   = 100.0;
    f.cfg_buildings[BT].energy_d = 25.0;
    r                            = detail::queue_bldg_repair(f.view(), f.store(), P, 4);
    ck(r.ticks == 0 && entry(0).tick_or_unit_id == 0,
       "repair: an already-full building costs ZERO ticks -- FCOMP/JC is a strict <");

    // The tick field is a BYTE and the original INCs it, so a long repair wraps rather than saturating.
    f.reset();
    f.b(P, 4).building_id        = (uint16_t)BT;
    f.b(P, 4).energy             = 0.0;
    f.cfg_buildings[BT].energy   = 300.0;
    f.cfg_buildings[BT].energy_d = 1.0;
    r                            = detail::queue_bldg_repair(f.view(), f.store(), P, 4);
    ck(r.ticks == 300 && entry(0).tick_or_unit_id == (uint8_t)300,
       "repair: 300 ticks wrap the byte field to 44 -- the original INCs a byte and does not clamp");

    // ---- queue_bldg_upgrade @0x004e2a2e ----
    f.reset();
    f.b(P, 6).building_id                 = (uint16_t)BT;
    f.cfg_buildings[BT].resource[0].id    = 1;
    f.cfg_buildings[BT].resource[0].val   = 11;
    f.cfg_buildings[BT].resource[1].id    = 3;
    f.cfg_buildings[BT].resource[1].val   = 13;
    f.cfg_buildings[BT].resource_2[0].id  = 2;
    f.cfg_buildings[BT].resource_2[0].val = 999; // the REPAIR list -- must not appear here
    entry(0).resource_reserved[0]         = 0x0abc;
    g_rec.grants.clear();
    r = detail::queue_bldg_upgrade(f.view(), f.store(), stub_calls(), P, 6);
    ck(r.rc == 0 && entry(0).status == 4 && entry(0).building_index == 6,
       "upgrade: kind 4 with the roster slot in building_index");
    ck(entry(0).resource_reserved[1] == 11 && entry(0).resource_reserved[3] == 13 &&
           entry(0).resource_reserved[2] == 0,
       "upgrade: costs come from Building.resource, NOT Building.resource_2");
    ck(entry(0).resource_reserved[0] == 0x0abc,
       "upgrade: resource_reserved[0] is left as found, like repair and unlike train");
    ck(g_rec.grants.size() == 4, "upgrade: FOUR grants are issued, one per resource id 1..4");
    bool           grants_ok = true;
    const uint32_t want[4]   = {11, 0, 13, 0};
    for (size_t j = 0; j < g_rec.grants.size(); ++j)
        grants_ok = grants_ok && g_rec.grants[j].player == (uint16_t)P &&
                    g_rec.grants[j].res_type == (uint32_t)(j + 1) &&
                    g_rec.grants[j].amount == want[j];
    ck(grants_ok,
       "upgrade: the grants are ids 1,2,3,4 IN ORDER carrying resource_reserved[1..4], zeros "
       "included");

    // A negative accrued cost reaches the grant as an unsigned 16-bit value, because the original
    // MOVZXes the field (0x004e2b6d). A translation that sign-extended would pass 0xffffffff.
    f.reset();
    f.b(P, 6).building_id               = (uint16_t)BT;
    f.cfg_buildings[BT].resource[0].id  = 1;
    f.cfg_buildings[BT].resource[0].val = -1;
    g_rec.grants.clear();
    detail::queue_bldg_upgrade(f.view(), f.store(), stub_calls(), P, 6);
    ck(entry(0).resource_reserved[1] == -1 && g_rec.grants.size() == 4 &&
           g_rec.grants[0].amount == 0xffffu,
       "upgrade: a negative reserve is MOVZX'd to 65535 for the grant, not sign-extended");

    // The upgrade cost walk is also SEVEN long -- the Building sibling of the train check above.
    f.reset();
    f.b(P, 6).building_id = (uint16_t)BT;
    for (int d = 0; d < 7; ++d) {
        f.cfg_buildings[BT].resource[d].id  = 4;
        f.cfg_buildings[BT].resource[d].val = 1;
    }
    g_rec.grants.clear();
    r = detail::queue_bldg_upgrade(f.view(), f.store(), stub_calls(), P, 6);
    ck(r.cost_ids == 7 && entry(0).resource_reserved[4] == 7,
       "upgrade: the cost walk consumes SEVEN entries and they all accrue into the same slot");

    // The DELIBERATE divergence: an out-of-range cost id. The original would write past the entry;
    // this reimplementation counts it and leaves the neighbour alone. Asserted so the difference is
    // a checked property rather than a comment.
    f.reset();
    f.b(P, 6).building_id               = (uint16_t)BT;
    f.cfg_buildings[BT].resource[0].id  = 5; // one past resource_reserved[4]
    f.cfg_buildings[BT].resource[0].val = 42;
    entry(1).status                     = 0x77;
    r                                   = detail::queue_bldg_upgrade(f.view(), f.store(), stub_calls(), P, 6);
    ck(r.oob_cost == 1 && entry(1).status == 0x77,
       "upgrade: a cost id past resource_reserved[4] is counted, not written into the next slot");
}

// ---- batch B layer 3: the four build-queue dispatch arms -----------------------------------------
//
// The other side of llm_strat_ai_bldg_queue_process's jump table -- test_bldg_queue above covers the
// table (which nibble routes where), this covers what the four slots DO.
//
// THESE CHECKS ARE THE WHOLE OF THE EVIDENCE FOR ALL FOUR. Three of them have shadow sites in
// the shadow manifest with measured region sets, but no ini fragment arms them and
// no rig run has ever reached this family: the 2026-08-02 run armed the three enqueue helpers
// one step UPSTREAM over 24000 steps on a developed save and every one logged zero calls. So this is
// not a supplement to a differential run, and it is written accordingly -- against the readings that
// are plausible AND wrong rather than along the happy path:
//
//   * the SIGNED credit test (unsigned, a negative credit takes the FREE path);
//   * the two AI.SCR scalars whose arithmetic is INVERTED relative to their names, given distinct
//     values here so a swap cannot pass;
//   * the cost read that is a MOVZX, not a sign-extension (a -1 reserve would ADD one back);
//   * WHICH of the two map notifiers runs, since they are different functions with one shape;
//   * the tile re-read AFTER stage D invalidates it, which a cached-tile translation gets wrong;
//   * the literal 10x10 footprint span, which is NOT the type's cfg width/height;
//   * the race-paired retire pair, checked in both directions rather than only the matching one;
//   * the 0x80 stamp that happens on BOTH halves of the upgrade/cancel split.
// ---- the shadow arms' aggregate cadence ---------------------------------------------------------
//
// This covers INSTRUMENTATION, not a translated body, and it exists because the instrumentation was
// wrong in a way that silently capped what a rig run could prove. ai_bldg_queue_dispatch.cpp shared
// ONE `next_report` across its three arms until 2026-08-02, so `queue_entry`'s 1st and 10th call
// pushed the threshold to 100 and `queue_upgrade`'s 4 calls in the 20:25 run printed no aggregate at
// all. Nothing could go red: a suppressed histogram and a quiet site log identically.
//
// So the property under test is not "the cadence is 1/10/100/1000" -- it is that TWO SITES DO NOT
// SHARE A THRESHOLD. The last check below is the one that fails against the old shape.
// ================================================================================================
// RI-AI batch B, antichain layer 3 -- the three planners llm_strat_ai_plan_construction runs in
// sequence: llm_strat_ai_score_build_categories @0x004e54cc, llm_strat_ai_react_resource_shortage
// @0x004e571d and llm_strat_ai_maintain_unit_housing @0x004e58d1.
//
// WHAT THIS COVERS THAT NO RIG RUN CAN. All three bodies are gated on state a differential run
// happens to be in, and the gates are the interesting part: score_build_categories' two RACE-PAIRED
// building-type selections need an alien AND a human player in the same run; react_resource_shortage
// queues AT MOST ONE of four candidate slots and never runs the later ones once an earlier one
// fires, so a run that always fires slot 1 says nothing about slots 2-4; and maintain_unit_housing's
// four blocks each carry a different gate, the last of them (helis) requiring a heli-producing
// building to exist at all. Everything below is set directly.
//
// AND ONE PROPERTY HERE IS THE OFFLINE HALF OF A WITHDRAWN FINDING. Until 2026-08-02 the ledger's
// `preserve_bug` field and this cluster's plates all recorded that
// maintain_unit_housing's heli branch queues construction for a HARDCODED player 0 rather than the
// ticking player. It does not -- that was Ghidra's `.c` inventing an argument for a thunk with no
// prototype (ghidra_findings 2026-08-02-2103-1/-2; the Watcom fake-return trap). The last check of
// test_maintain_unit_housing runs the heli branch as player 2 and asserts the construction order
// carries player 2, so a reimplementation that "preserved" the withdrawn bug goes red here rather
// than waiting for a rig run to reach the most heavily gated of four blocks.
void test_score_build_categories() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 2; // NOT 0: a body that ignored the parameter would pass every check against 0

    // The ten category answers, all DISTINCT, so a store into the wrong field is visible rather
    // than cancelling out. 0x23 and 0x22 are the pair whose STORE order is inverted relative to
    // their numeric order.
    auto setup = [&](int32_t alien) {
        f.reset();
        g_rec.clear();
        f.players[P].is_alien_race   = alien;
        g_rec.count_by_cat_ans[0x01] = 11;
        g_rec.count_by_cat_ans[0x20] = 12;
        g_rec.count_by_cat_ans[0x21] = 13;
        g_rec.count_by_cat_ans[0x23] = 14;
        g_rec.count_by_cat_ans[0x22] = 15;
        g_rec.count_by_cat_ans[0x10] = 16;
        g_rec.count_by_cat_ans[0x11] = 17;
        g_rec.count_by_cat_ans[0x12] = 18;
        g_rec.count_by_cat_ans[0x13] = 19;
        g_rec.count_by_cat_ans[0x30] = 20;
        g_rec.count_by_type_ans[5]   = 100; // A_TURRET
        g_rec.count_by_type_ans[25]  = 200; // H_TURRET
        g_rec.count_by_type_ans[2]   = 300; // A_MINE
        g_rec.count_by_type_ans[22]  = 400; // H_MINE
        g_rec.queue_pending_ans[5]   = 1;
        g_rec.queue_pending_ans[25]  = 2;
        g_rec.queue_pending_ans[2]   = 3;
        g_rec.queue_pending_ans[22]  = 4;
    };

    // ---- the ALIEN side ----------------------------------------------------------------------
    setup(/*alien=*/1);
    detail::score_build_categories(f.view(), f.store(), stub_calls(), P);
    const player_data &a = f.players[P];
    ck(a.ai_score_cat_1 == 11 && a.ai_score_cat_0x20 == 12 && a.ai_score_cat_0x21 == 13 &&
           a.ai_score_cat_0x23 == 14 && a.ai_score_cat_0x22 == 15 && a.ai_score_cat_0x10 == 16 &&
           a.ai_score_cat_0x11 == 17 && a.ai_score_cat_0x12 == 18 && a.ai_score_cat_0x13 == 19 &&
           a.ai_score_cat_0x30 == 20,
       "score: each of the ten category counts lands in ITS OWN field -- including the 0x23/0x22 "
       "pair, whose fields sit in the reverse of their numeric order");
    ck(a.ai_score_bldg_type_a == 101,
       "score: ai_score_bldg_type_a = count_by_type(A_TURRET) + queue_has_pending(A_TURRET)");
    ck(a.ai_score_bldg_type_b == 303,
       "score: ai_score_bldg_type_b = count_by_type(A_MINE) + queue_has_pending(A_MINE)");

    // The CALL SEQUENCE, which is a separate claim from the stored values: the original interleaves
    // the tenth category query (0x30) BETWEEN the turret pair and the mine pair, and issues the nine
    // others in a non-numeric order.
    {
        const int32_t want[] = {0x01, 0x20, 0x21, 0x23, 0x22, 0x10, 0x11, 0x12, 0x13, 0x30};
        int           n      = 0;
        bool          seq_ok = true;
        for (const auto &q : g_rec.type_queries)
            if (q.kind == 2) {
                if (n >= 10 || q.arg != want[n]) seq_ok = false;
                ++n;
            }
        ck(n == 10 && seq_ok,
           "score: exactly ten count_by_category queries, in the listing's order "
           "(1, 0x20, 0x21, 0x23, 0x22, 0x10-0x13, then 0x30) -- not sorted");
    }
    {
        // The 0x30 query sits between the two count_by_type queries. A translation that hoisted all
        // ten category queries to the top would still store the right values.
        int idx_turret_count = -1, idx_cat30 = -1, idx_mine_count = -1;
        for (size_t i = 0; i < g_rec.type_queries.size(); ++i) {
            const auto &q = g_rec.type_queries[i];
            if (q.kind == 1 && q.arg == 5) idx_turret_count = (int)i;
            if (q.kind == 2 && q.arg == 0x30) idx_cat30 = (int)i;
            if (q.kind == 1 && q.arg == 2) idx_mine_count = (int)i;
        }
        ck(idx_turret_count >= 0 && idx_cat30 > idx_turret_count && idx_mine_count > idx_cat30,
           "score: the 0x30 category query happens BETWEEN the turret pair and the mine pair");
    }
    ck(f.players[P + 1].ai_score_cat_1 == 0 && f.players[0].ai_score_cat_1 == 0,
       "score: every store lands in players[player] -- not players[player + 1] and not players[0]");
    {
        bool right_player = true;
        for (const auto &q : g_rec.type_queries) right_player = right_player && q.player == P;
        ck(right_player, "score: every query is asked about the ticking player");
    }

    // ---- the HUMAN side: only the two race-paired selections change -----------------------------
    setup(/*alien=*/0);
    detail::score_build_categories(f.view(), f.store(), stub_calls(), P);
    const player_data &h = f.players[P];
    ck(h.ai_score_bldg_type_a == 202 && h.ai_score_bldg_type_b == 404,
       "score: is_alien_race == 0 selects H_TURRET (25) and H_MINE (22) instead -- the ONLY thing "
       "the race flag changes here");
    ck(h.ai_score_cat_1 == 11 && h.ai_score_cat_0x30 == 20,
       "score: the ten category queries are race-independent");
    {
        bool asked_alien_types = false;
        for (const auto &q : g_rec.type_queries)
            if (q.kind == 1 && (q.arg == 5 || q.arg == 2)) asked_alien_types = true;
        ck(!asked_alien_types,
           "score: the human run never asks about the ALIEN type ids -- the pair is selected, not "
           "both computed");
    }
}

void test_react_resource_shortage() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 2;

    // Four DISTINCT candidate types in the four shortage slots, all marked available, all with a
    // zero category counter and none already queued -- i.e. slot 1 is eligible. Each test below
    // spoils exactly one thing about that baseline.
    auto setup = [&](int32_t alien) {
        f.reset();
        g_rec.clear();
        player_data &p  = f.players[P];
        p.is_alien_race = alien;
        for (int i = 0; i < 4; ++i) {
            p.ai_resource_shortage_candidates[i] = 40 + i;
            p.ai_building_type_available[40 + i] = 1;
        }
        p.ai_build_candidate_cat_0x30    = 60;
        p.ai_building_type_available[60] = 1;
        // The tail's five gates, all set to their PASSING value; the chain's gates are all zero.
        p.ai_score_cat_0x10 = 0;
        p.ai_score_cat_0x20 = 0;
        p.ai_score_cat_0x30 = 0;
    };
    auto run = [&]() { detail::react_resource_shortage(f.view(), stub_calls(), P); };

    // -- the EARLY EXIT, which is also this body's vacuous shape on the rig.
    setup(/*alien=*/1);
    g_rec.queue_pending_ans[1] = 1;
    run();
    ck(g_rec.constructions.empty(),
       "react: a pending build of the race-paired probe type returns having queued NOTHING -- and "
       "skips the tail too, not just the chain");
    {
        int probes = 0;
        for (const auto &q : g_rec.type_queries)
            if (q.kind == 4) ++probes;
        ck(probes == 1 && g_rec.type_queries.size() == 1,
           "react: the early exit asks exactly one question and then stops");
    }
    setup(/*alien=*/0);
    g_rec.queue_pending_ans[1] = 1; // the ALIEN type -- a human player must not probe it
    run();
    ck(g_rec.constructions.size() >= 1,
       "react: is_alien_race == 0 probes type 0x15, not 1 -- the alien probe answering `pending` "
       "does not stop a human player");

    // -- the chain: slot 1 eligible, and nothing further is even asked.
    setup(1);
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 40,
       "react: slot 1 eligible queues candidate[0] -- and ONLY that, because slot 1 firing implies "
       "ai_score_cat_0x10 == 0, which is the tail's first gate inverted (see below)");
    ck(g_rec.constructions[0].player == P && g_rec.constructions[0].x == -1 &&
           g_rec.constructions[0].y == 0,
       "react: the queue call is (player, type, x = -1, y = 0) -- a SIGNED -1 in x");
    {
        bool asked_later_slot = false;
        for (const auto &q : g_rec.type_queries)
            if (q.kind == 3 && (q.arg == 41 || q.arg == 42 || q.arg == 43)) asked_later_slot = true;
        ck(!asked_later_slot,
           "react: once a slot fires the chain STOPS -- slots 2-4 are not even asked, because the "
           "original jumps straight to the shared queue site");
    }

    // -- SLOT 1 AND THE TAIL CANNOT BOTH FIRE, and that is a structural property of the original
    //    rather than a fact about this fixture: slot 1 needs `already_queued(cand[0]) +
    //    ai_score_cat_0x10 == 0` (0x004e5789/0x004e578f) and the tail needs `ai_score_cat_0x10 != 0`
    //    (0x004e5880/0x004e5887). The same counter, opposite senses, ~250 bytes apart. Slots 2-4
    //    carry no such implication, so THEY are how the fall-through into the tail gets tested.
    setup(1);
    g_rec.already_queued_ans[40]   = 1; // block slot 1
    f.players[P].ai_score_cat_0x10 = 5; // open the tail's first gate
    f.players[P].ai_score_cat_0x20 = 1; // and its second
    run();
    ck(g_rec.constructions.size() == 2 && g_rec.constructions[0].building_type == 41 &&
           g_rec.constructions[1].building_type == 60,
       "react: the tail runs even AFTER the chain queued -- the original falls through the queue "
       "call at 0x004e5865 into LAB_004e586a rather than returning");

    // -- `sum != 0` blocks from EITHER term, and the two terms are different quantities.
    setup(1);
    g_rec.already_queued_ans[40] = 1;
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 41,
       "react: already_queued(candidate[0]) blocks slot 1 and slot 2 wins");
    setup(1);
    f.players[P].ai_score_cat_0x10 = 3;
    f.players[P].ai_score_cat_0x11 = 0;
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 41,
       "react: a NON-ZERO ai_score_cat_0x10 blocks slot 1 by itself -- the test is on the SUM of "
       "already_queued and the counter, not on already_queued alone (the tail stays shut here too, "
       "on its ai_score_cat_0x20 gate)");

    // -- availability is `== 1`, not `!= 0`.
    setup(1);
    f.players[P].ai_building_type_available[40] = 2;
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 41,
       "react: ai_building_type_available == 2 does NOT count -- the comparison is `== 1`");

    // -- the last slot, reached only when the first three are all blocked.
    setup(1);
    for (int i = 0; i < 3; ++i) g_rec.already_queued_ans[40 + i] = 1;
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 43,
       "react: slot 4 fires when slots 1-3 are blocked -- its branch ENCODING is inverted in the "
       "assembly (JNZ-to-tail rather than JZ-to-queue) and its meaning is not");
    setup(1);
    for (int i = 0; i < 4; ++i) g_rec.already_queued_ans[40 + i] = 1;
    run();
    ck(g_rec.constructions.empty(),
       "react: all four slots blocked queues nothing at all -- the tail is shut too, by the "
       "ai_score_cat_0x10 == 0 this baseline shares with slot 1's eligibility");

    // -- the tail's five gates, one at a time.
    //
    // THE BASELINE HAS TO OPEN EVERY GATE, and the first version of this block did not: it left
    // ai_score_cat_0x10 at the 0 `setup` gives it, so the tail was ALREADY shut on gate 1 and every
    // "the tail needs X" check below was passing for a reason that had nothing to do with X. The
    // mutation campaign found it -- "the tail's already_queued gate is dropped" scored MISSED, the
    // probe indicting itself rather than the assertion, exactly as the shadow-cadence probe did on
    // 2026-08-02. The chain stays fully blocked so the only construction that can land is the tail's.
    auto tail_only = [&]() {
        setup(1);
        for (int i = 0; i < 4; ++i) g_rec.already_queued_ans[40 + i] = 1;
        f.players[P].ai_score_cat_0x10 = 1; // gate 1
        f.players[P].ai_score_cat_0x20 = 1; // gate 2
        // gates 3-5 (already_queued(cand30) == 0, cat_0x30 == 0, available[cand30] == 1) are what
        // `setup` already leaves open.
    };
    tail_only();
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 60,
       "react: the tail queues ai_build_candidate_cat_0x30 when all five of its gates pass");
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].player == P &&
           g_rec.constructions[0].x == -1 && g_rec.constructions[0].y == 0,
       "react: the TAIL's queue call carries the same (player, x = -1, y = 0) as the chain's -- "
       "checked separately because it is a second call site, not the same one reached twice");
    tail_only();
    f.players[P].ai_score_cat_0x10 = 0;
    run();
    ck(g_rec.constructions.empty(), "react: the tail needs ai_score_cat_0x10 != 0");
    tail_only();
    f.players[P].ai_score_cat_0x20 = 0;
    run();
    ck(g_rec.constructions.empty(), "react: the tail needs ai_score_cat_0x20 != 0");
    tail_only();
    f.players[P].ai_score_cat_0x30 = 1;
    run();
    ck(g_rec.constructions.empty(),
       "react: the tail needs ai_score_cat_0x30 == ZERO -- the opposite sense to cat_0x10/0x20 two "
       "lines above it");
    tail_only();
    g_rec.already_queued_ans[60] = 1;
    run();
    ck(g_rec.constructions.empty(), "react: the tail needs its candidate not already queued");
    tail_only();
    f.players[P].ai_building_type_available[60] = 0;
    run();
    ck(g_rec.constructions.empty(), "react: the tail needs its candidate AVAILABLE");
}

void test_maintain_unit_housing() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 2;

    // Distinct candidate ids in the FOUR aliased slots of players[player + 1], distinct used_*
    // columns, and every gate passing -- so a crossed pairing shows up as the wrong type queued
    // rather than as no queue at all.
    auto setup = [&]() {
        f.reset();
        g_rec.clear();
        player_data &p                    = f.players[P];
        player_data &next                 = f.players[P + 1];
        next.ai_housing_candidate_soldier = 70;
        next.ai_housing_candidate_vehicle = 71;
        next.ai_housing_candidate_plane   = 72;
        next.ai_housing_candidate_heli    = 73;
        f.housing[P].used_soldiers        = 100; // -> 3
        f.housing[P].used_vehicles        = 150; // -> 4
        f.housing[P].used_planes          = 200; // -> 5
        f.housing[P].used_helis           = 250; // -> 6
        // block 1's gate, and the counters every block compares against
        p.ai_resource_shortage_candidates[0] = 44;
        g_rec.count_by_id_ans[44]            = 1;
        p.ai_score_cat_0x20                  = 0;
        p.ai_score_cat_0x21                  = 0;
        p.ai_score_cat_0x23                  = 0;
        p.ai_score_cat_0x22                  = 0;
        // block 2's ||-triple, block 3's and block 4's predicates
        p.ai_score_cat_0x11    = 1;
        g_rec.has_aircraft_ans = 1;
        g_rec.has_heli_ans     = 1;
    };
    auto run    = [&]() { detail::maintain_unit_housing(f.view(), f.store(), stub_calls(), P); };
    auto queued = [&](int32_t type) {
        for (const auto &c : g_rec.constructions)
            if (c.building_type == type) return true;
        return false;
    };

    // -- all four blocks fire, each with ITS OWN candidate. Blocks 2/3/4 additionally need the
    //    counter the PREVIOUS block would have raised, so the passing configuration is not uniform:
    //    vehicles needs cat_0x20 != 0, planes needs cat_0x21 != 0, helis needs cat_0x21 and cat_0x23.
    setup();
    f.players[P].ai_score_cat_0x20 = 1; // < needed_soldiers 3, and non-zero for block 2
    f.players[P].ai_score_cat_0x21 = 1; // < needed_vehicles 4, and non-zero for blocks 3 and 4
    f.players[P].ai_score_cat_0x23 = 1; // < needed_planes 5,  and non-zero for block 4
    f.players[P].ai_score_cat_0x22 = 1; // < needed_helis 6
    run();
    ck(g_rec.constructions.size() == 4 && queued(70) && queued(71) && queued(72) && queued(73),
       "housing: all four blocks queue, each its OWN candidate from players[player + 1] -- soldier "
       "70, vehicle 71, plane 72, heli 73");
    {
        bool right_player = true;
        for (const auto &c : g_rec.constructions)
            right_player = right_player && c.player == P && c.x == -1 && c.y == 0;
        ck(right_player, "housing: every queue call is (player, type, x = -1, y = 0)");
    }

    // -- `used / 50 + 1` and the STRICT comparison, at the boundary in both directions.
    setup();
    f.housing[P].used_soldiers     = 0;
    f.players[P].ai_score_cat_0x20 = 0; // 0 < 1 -> queue
    run();
    ck(queued(70), "housing: used == 0 still needs ONE -- the formula is used / 50 + 1");
    setup();
    f.housing[P].used_soldiers     = 0;
    f.players[P].ai_score_cat_0x20 = 1; // 1 < 1 is false
    run();
    ck(!queued(70),
       "housing: the comparison is STRICT (`built < needed`) -- built == needed queues nothing");
    setup();
    f.housing[P].used_soldiers     = 49;
    f.players[P].ai_score_cat_0x20 = 1;
    run();
    ck(!queued(70), "housing: 49 / 50 + 1 == 1, so 49 units still need only one housing building");
    setup();
    f.housing[P].used_soldiers     = 50;
    f.players[P].ai_score_cat_0x20 = 1;
    run();
    ck(queued(70), "housing: 50 / 50 + 1 == 2 -- the boundary is at the 50th unit, not the 51st");

    // -- THE PAIRINGS. Raise one counter at a time and watch exactly one block fall silent.
    setup();
    f.players[P].ai_score_cat_0x20 = 99; // >= needed_soldiers 3
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    run();
    ck(!queued(70) && queued(71) && queued(72) && queued(73),
       "housing: ai_score_cat_0x20 is the SOLDIER counter -- raising it silences the soldier block "
       "and nothing else (it is still non-zero, so block 2's gate still passes)");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 99; // >= needed_vehicles 4
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    run();
    ck(queued(70) && !queued(71) && queued(72) && queued(73),
       "housing: ai_score_cat_0x21 is the VEHICLE counter");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 99; // >= needed_planes 5
    f.players[P].ai_score_cat_0x22 = 1;
    run();
    ck(queued(70) && queued(71) && !queued(72) && queued(73),
       "housing: ai_score_cat_0x23 is the PLANE counter -- 0x23 and 0x22 are the pair whose "
       "numbering runs opposite to the order their fields sit in");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 99; // >= needed_helis 6
    run();
    ck(queued(70) && queued(71) && queued(72) && !queued(73),
       "housing: ai_score_cat_0x22 is the HELI counter");

    // -- the four gates, which are all different.
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    g_rec.count_by_id_ans[44]      = 0;
    run();
    ck(!queued(70) && queued(71),
       "housing: the soldier block is gated on count_by_id(ai_resource_shortage_candidates[0]) != 0");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    f.players[P].ai_score_cat_0x11 = 0; // and 0x12 / 0x13 are already 0
    run();
    ck(queued(70) && !queued(71),
       "housing: the vehicle block needs at least one of ai_score_cat_0x11/0x12/0x13 -- an OR, not "
       "an AND");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    f.players[P].ai_score_cat_0x12 = 1;
    f.players[P].ai_score_cat_0x11 = 0;
    run();
    ck(queued(71), "housing: ai_score_cat_0x12 alone satisfies the vehicle block's OR");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    g_rec.has_aircraft_ans         = 0;
    run();
    ck(!queued(72) && queued(73),
       "housing: the plane block is gated on side_has_aircraft_producer -- and the HELI block is "
       "not, so they are two different predicates over the same roster");
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    g_rec.has_heli_ans             = 0;
    run();
    ck(queued(72) && !queued(73), "housing: the heli block is gated on bldg_has_heli_unit");

    // -- already_queued suppresses each block individually.
    setup();
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    g_rec.already_queued_ans[72]   = 1;
    run();
    ck(queued(70) && queued(71) && !queued(72) && queued(73),
       "housing: already_queued suppresses exactly the block whose candidate it names");

    // -- THE WITHDRAWN BUG. The heli branch's tail is `JMP llm_strat_bldg_queue_construction_thunk`,
    //    whose `MOV EAX,ESI` feeds it this function's own player. The plate and the
    //    ledger's preserve_bug all said "hardcoded player 0" until 2026-08-02; a reimplementation
    //    that preserved that reading fails HERE rather than on a rig run that has to reach the most
    //    heavily gated of four blocks first.
    setup();
    f.players[P].ai_score_cat_0x20 = 99; // silence the other three
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    g_rec.count_by_id_ans[44]      = 0;
    g_rec.has_aircraft_ans         = 0;
    run();
    ck(g_rec.constructions.size() == 2,
       "housing: with the soldier and plane gates shut, only the vehicle and heli blocks queue");
    {
        bool heli_ok = false;
        for (const auto &c : g_rec.constructions)
            if (c.building_type == 73) heli_ok = (c.player == P);
        ck(heli_ok,
           "housing: THE HELI BLOCK QUEUES FOR THE TICKING PLAYER, NOT PLAYER 0 -- the withdrawn "
           "'latent bug' of 2026-08-02 (ghidra_findings 2026-08-02-2103-1/-2)");
    }
}


void test_bldg_queue_dispatch() {
    using namespace mh::ai;
    fixture        f;
    const uint32_t P = 2;
    // NOT slot 0. Every arm indexes the entry by queue_index, and a body that ignored the parameter
    // entirely would still pass every assertion made against slot 0.
    const int32_t                 QI    = 1;
    auto                          entry = [&](int i) -> auto                          &{ return f.players[P].ai_bldg_queue[i]; };
    detail::queue_dispatch_report rep{};

    // ================================ nibble 0: handle_recruit_state ==============================
    auto run_recruit = [&]() {
        rep = detail::queue_dispatch_report{};
        detail::bldg_queue_handle_recruit_state(f.view(), f.store(), stub_calls(), P, QI, rep);
    };
    // A kind-0 entry with four DISTINCT reserved costs and a distinct starting ledger, so a debit
    // that used the wrong column (or the wrong row) lands somewhere visible.
    auto set_recruit = [&](int32_t credit, int32_t producer) {
        f.reset();
        g_rec.clear();
        entry(QI).status               = 0;
        entry(QI).tick_or_unit_id      = 42;
        entry(QI).resource_reserved[0] = 500; // the dead cost slot -- must never be debited
        for (int j = 0; j < 4; ++j) {
            entry(QI).resource_reserved[1 + j] = (int16_t)(10 + j);
            f.players[P].resource_spent[j]     = 1000; // id 1+j -> element j since the reshape
        }
        // The sentinel below resource_spent[0]: a debit loop that under-ran would land here.
        f.players[P].ai_patrol_quadrant_cursor = 777;
        f.players[P].ai_promo_credit           = credit;
        g_rec.producer_answer                  = producer;
    };

    // -- the early return. TEST EAX,EAX / JZ @0x004e8130.
    set_recruit(/*credit=*/5, /*producer=*/0);
    run_recruit();
    ck(rep.no_producer == 1 && g_rec.producer_queries.size() == 1 &&
           g_rec.producer_queries[0].player == (int32_t)P &&
           g_rec.producer_queries[0].unit_id == 42,
       "recruit: the producer query is asked (player, the entry's unit TYPE id)");
    bool recruit_clean = entry(QI).status == 0 && f.players[P].ai_promo_credit == 5 &&
                         g_rec.recruit_orders.empty() && g_rec.production_adds.empty() &&
                         g_rec.econ_tracks.empty();
    for (int j = 0; j < 4; ++j)
        recruit_clean = recruit_clean && f.players[P].resource_spent[j] == 1000;
    ck(recruit_clean,
       "recruit: a ZERO producer returns having written NOTHING -- the entry is left unstamped for "
       "the next tick to retry, which is also this arm's vacuous shape on the rig");

    // -- the FREE path. CMP dword[ai_promo_credit],0 / JLE @0x004e8136, so strictly greater.
    set_recruit(/*credit=*/1, /*producer=*/9);
    run_recruit();
    ck(rep.promo_spent == 1 && rep.paid == 0 && g_rec.recruit_orders.size() == 1 &&
           g_rec.recruit_orders[0].unit_id == 42 && g_rec.recruit_orders[0].player == P,
       "recruit: credit > 0 takes the FREE path -- order_recruit_unit_enqueue(unit_id, player), "
       "whose argument order is the REVERSE of order_population_delta_enqueue's");
    ck(f.players[P].ai_promo_credit == 1 - 3,
       "recruit: the free path SUBTRACTS promo_add (SUB @0x004e8153) -- the AI.SCR key is nPromoAdd "
       "and the arithmetic is inverted relative to the name");
    ck(entry(QI).status == 0xc0,
       "recruit: the free path stamps 0xc0 = COMMITTED | REMOVED, not 0x80");
    bool free_moved_nothing = g_rec.production_adds.empty() && g_rec.econ_tracks.empty() &&
                              f.players[P].ai_patrol_quadrant_cursor == 777;
    for (int j = 0; j < 4; ++j)
        free_moved_nothing = free_moved_nothing && f.players[P].resource_spent[j] == 1000;
    ck(free_moved_nothing,
       "recruit: NO resources move on the free path and neither paying callee runs -- that is what "
       "makes it the free one");

    // -- ZERO credit PAYS. The boundary: JLE, not JL.
    set_recruit(/*credit=*/0, /*producer=*/9);
    run_recruit();
    ck(rep.paid == 1 && rep.promo_spent == 0,
       "recruit: credit == 0 takes the PAYING path -- the test is `> 0` (JLE @0x004e813d), not "
       "`>= 0`");
    ck(g_rec.production_adds.size() == 1 && g_rec.production_adds[0].player == (uint16_t)P &&
           g_rec.production_adds[0].producer == 9 && g_rec.production_adds[0].unit_type == 42,
       "recruit: the paying path hands over (player, THE PRODUCER the query returned, unit type) -- "
       "the third argument is the unit type, which is what makes the callee's name suspect");
    ck(g_rec.econ_tracks.size() == 1 && g_rec.econ_tracks[0].player == (int32_t)P &&
           g_rec.econ_tracks[0].unit == 42,
       "recruit: econ_track_unit_resource_spend(player, unit type) runs on the paying path only");
    bool paid_debited = true;
    for (int j = 0; j < 4; ++j)
        paid_debited = paid_debited && f.players[P].resource_spent[j] == 1000 - (10 + j);
    ck(paid_debited,
       "recruit: the paying path debits resource_spent[j] by resource_reserved[1+j] for j = 0..3, "
       "column by column");
    ck(f.players[P].ai_patrol_quadrant_cursor == 777,
       "recruit: the debit loop starts at resource id 1 -- ai_patrol_quadrant_cursor, the scalar below resource_spent[0], is untouched");
    ck(f.players[P].ai_promo_credit == 7,
       "recruit: the paying path ADDS promo_sub (ADD @0x004e81cd) -- inverted again, and distinct "
       "from promo_add here so a swap of the two cannot pass");
    ck(entry(QI).status == 0x80,
       "recruit: the paying path stamps 0x80 ALONE -- the entry is committed but not removed");

    // -- a NEGATIVE credit pays. This is the signedness check the field's type turns on.
    set_recruit(/*credit=*/-1, /*producer=*/9);
    run_recruit();
    ck(rep.paid == 1 && rep.promo_spent == 0,
       "recruit: a NEGATIVE credit takes the PAYING path -- the compare is SIGNED; read unsigned, "
       "-1 is 0xffffffff and would take the free path on every tick");

    // -- the cost read is a MOVZX. -1 reserved must debit 65535, not credit one back.
    set_recruit(/*credit=*/0, /*producer=*/9);
    entry(QI).resource_reserved[1]          = (int16_t)-1;
    f.players[P].resource_spent[0] /*id 1*/ = 0;
    run_recruit();
    ck(f.players[P].resource_spent[0] /*id 1*/ == -65535,
       "recruit: the reserved cost is a SHORT read ZERO-extended (MOVZX @0x004e8197) then subtracted "
       "from an int -- sign-extended it would be -1 and the debit would ADD one");

    // -- the row is the ticking player's own.
    set_recruit(/*credit=*/0, /*producer=*/9);
    f.players[0].ai_promo_credit            = 4242;
    f.players[0].resource_spent[0] /*id 1*/ = 55;
    run_recruit();
    ck(f.players[0].ai_promo_credit == 4242 && f.players[0].resource_spent[0] /*id 1*/ == 55 &&
           f.players[0].ai_bldg_queue[QI].status == 0,
       "recruit: player 0's record is untouched when P != 0");

    // ================================ nibble 1: process_entry ====================================
    auto run_entry = [&]() {
        rep = detail::queue_dispatch_report{};
        detail::bldg_queue_process_entry(f.view(), f.store(), stub_calls(), P, QI, rep);
    };
    // An ordinary building type: neither race's turret (5 / 25) nor relay (15 / 35), so stage D
    // re-queues it unless a case says otherwise. Its cfg width/height are set to something OTHER
    // than 10 so the literal-span assertion below can actually go red.
    const uint8_t BT        = 7;
    auto          set_entry = [&](int16_t tile_x, int16_t tile_y, uint8_t status) {
        f.reset();
        g_rec.clear();
        entry(QI).status                        = status;
        entry(QI).tick_or_unit_id               = BT;
        entry(QI).build_tile_x                  = tile_x;
        entry(QI).resource_reserved[0]          = tile_y;
        entry(QI).building_index                = 0x0badf00d;
        f.cfg_buildings[BT].type                = 99; // not a race-paired turret or relay either way
        f.cfg_buildings[BT].width               = 4;
        f.cfg_buildings[BT].height              = 3;
        f.cfg_buildings[BT].worker_count        = 13;
        f.players[P].ai_build_candidate_primary = 99; // not BT, unless a case says so
    };
    // Point the type-dispatch stub at the fixture's shared site scratch, the way the real callee
    // writes it.
    auto publish = [&](int32_t count, int32_t x, int32_t y) {
        g_rec.publish_site_count = &f.site_count;
        g_rec.publish_site_list  = f.site_cands.data();
        g_rec.publish_count      = count;
        g_rec.publish_x          = x;
        g_rec.publish_y          = y;
    };

    // -- stage A does NOT run when a tile is already cached (CMP word,-1 / JNZ @0x004e7e12).
    set_entry(20, 30, 1);
    g_rec.footprint_answer = 0; // land in stage D; stage C has its own cases
    run_entry();
    ck(g_rec.type_dispatches.empty() && rep.tile_resolved == 0 && rep.tile_scan_empty == 0,
       "process_entry: a cached tile skips stage A entirely -- the site scan is NOT re-run, which "
       "matters because that callee is a producer and re-running it would refill the shared scratch");

    // -- stage A with a candidate: cache it and tell the other grids.
    //    The footprint is made to FIT here on purpose. Stage D's re-queue arm invalidates
    //    build_tile_x back to 0xffff, so a run that lands there cannot see what stage A cached --
    //    stage C is the only exit that leaves the cache observable.
    set_entry((int16_t)-1, 0, 1);
    publish(/*count=*/1, /*x=*/20, /*y=*/30);
    g_rec.footprint_answer = 1;
    run_entry();
    ck(g_rec.type_dispatches.size() == 1 && g_rec.type_dispatches[0].player == P &&
           g_rec.type_dispatches[0].building_id == BT,
       "process_entry: stage A runs bldg_production_type_dispatch(player, the entry's BUILDING type)");
    ck(rep.tile_resolved == 1 && entry(QI).build_tile_x == 20 &&
           entry(QI).resource_reserved[0] == 30,
       "process_entry: candidate [0] is cached into build_tile_x and into resource_reserved[0] -- "
       "the cost slot that is dead as a cost");
    ck(g_rec.map_notifies.size() == 1 && !g_rec.map_notifies[0].second &&
           g_rec.map_notifies[0].player == (int32_t)P &&
           g_rec.map_notifies[0].building_type == BT && g_rec.map_notifies[0].tile_x == 20 &&
           g_rec.map_notifies[0].tile_y == 30,
       "process_entry: stage A announces the cached tile through notify_map_changed -- the FIRST "
       "notifier, and with (player, type, x, y) in that order");
    ck(g_rec.footprints.size() == 1 && g_rec.footprints[0].x == 20 && g_rec.footprints[0].y == 30,
       "process_entry: stage B then tests the tile stage A just cached, in the SAME call -- the "
       "resolve and the footprint test are not one-per-tick");

    // -- stage A with NO candidate: retire outright, and stage B then returns.
    set_entry((int16_t)-1, 0, 1);
    publish(/*count=*/0, 0, 0);
    g_rec.footprint_answer = 1; // would fit -- proving the return happens BEFORE the test
    run_entry();
    ck(rep.tile_scan_empty == 1 && entry(QI).status == (uint8_t)(0xc0 | 1),
       "process_entry: a scan that produced NOTHING stamps 0xc0 and abandons the entry "
       "(@0x004e7e61)");
    ck(rep.no_tile == 1 && g_rec.footprints.empty() && g_rec.map_notifies.empty(),
       "process_entry: ... and stage B then returns on the still-unresolved tile -- no footprint "
       "test and no notifier, even though the footprint would have passed");

    // -- stage B RE-READS the entry rather than trusting that stage A succeeded.
    set_entry((int16_t)-1, 0, 1);
    publish(/*count=*/1, /*x=*/0xffff, /*y=*/5);
    g_rec.footprint_answer = 1;
    run_entry();
    ck(rep.tile_resolved == 1 && rep.no_tile == 1 && g_rec.footprints.empty(),
       "process_entry: stage B re-reads build_tile_x from the entry (@0x004e7e87), so a candidate "
       "whose low word IS the 0xffff sentinel returns instead of testing a footprint at it");

    // -- the footprint call's shape.
    set_entry(20, 30, 1);
    g_rec.footprint_answer = 0;
    run_entry();
    ck(g_rec.footprints.size() == 1 && g_rec.footprints[0].grid == f.passable.data() &&
           g_rec.footprints[0].w == f.map_w && g_rec.footprints[0].h == f.map_h,
       "process_entry: the footprint test walks `passable` with (width, height) in that order -- "
       "the fixture map is 64x48, so a swap does not cancel out");
    ck(at(g_rec.footprints, 0).mask == (void *)&f.cfg_buildings[BT].area[0][0],
       "process_entry: the mask is cfg Building[type].area (+0xb), not area_2 or area_3");
    ck(at(g_rec.footprints, 0).span_x == 10 && at(g_rec.footprints, 0).span_y == 10,
       "process_entry: the span is the literal 10x10 box (PUSH 0xa twice @0x004e7ea5) -- NOT the "
       "type's cfg width/height, which are 4 and 3 here");
    ck(at(g_rec.footprints, 0).x == 20 && at(g_rec.footprints, 0).y == 30,
       "process_entry: ... anchored at the cached tile");

    // -- the tile travels ZERO-extended everywhere below the sentinel test.
    set_entry((int16_t)-2, (int16_t)-3, 1);
    g_rec.footprint_answer = 0;
    run_entry();
    ck(g_rec.footprints.size() == 1 && g_rec.footprints[0].x == 65534 &&
           g_rec.footprints[0].y == 65533,
       "process_entry: every downstream use of the cached tile is a MOVZX (@0x004e7e95/0x004e7e9d) "
       "-- only the -1 sentinel test is a signed word compare");

    // -- stage C, the WAIVED half.
    set_entry(20, 30, (uint8_t)(1 | QUEUE_STATUS_AFFORD_WAIVED));
    for (int j = 0; j < 4; ++j) {
        entry(QI).resource_reserved[1 + j] = (int16_t)(10 + j);
        f.players[P].resource_spent[j]     = 1000;
    }
    g_rec.footprint_answer = 1;
    run_entry();
    ck(rep.fits == 1 && rep.built_waived == 1 && rep.built_instant == 0,
       "process_entry: a NONZERO footprint result means the box FITS -- the callee's polarity is "
       "inverted from its name (llm_scan_masked_table_for_empty_cell)");
    ck((entry(QI).status & QUEUE_STATUS_AFFORD_WAIVED) == 0,
       "process_entry: the waiver bit is CLEARED by the path it selects (AND 0xdf @0x004e7ee9)");
    ck(g_rec.construction_orders.size() == 1 && g_rec.construction_orders[0].x == 20 &&
           g_rec.construction_orders[0].y == 30 && g_rec.construction_orders[0].type == BT &&
           g_rec.construction_orders[0].player == (uint16_t)P,
       "process_entry: the waived path issues order_queue_construction_enqueue(x, y, type, player)");
    bool waived_kept_resources = g_rec.instant_orders.empty() && g_rec.expenditures.empty();
    for (int j = 0; j < 4; ++j)
        waived_kept_resources = waived_kept_resources && f.players[P].resource_spent[j] == 1000;
    ck(waived_kept_resources,
       "process_entry: the waived path debits NO resources and does not instant-construct -- the "
       "waiver is exactly why");
    ck(g_rec.map_notifies.size() == 1 && !g_rec.map_notifies[0].second,
       "process_entry: the build paths use notify_map_changed; notify_map_changed_2 belongs to "
       "stage D alone");
    ck(rep.population_delta == 0 && g_rec.population_deltas.empty(),
       "process_entry: no population delta when the queued type is not ai_build_candidate_primary");
    ck((entry(QI).status & QUEUE_STATUS_COMMITTED) != 0 && entry(QI).building_index == -1,
       "process_entry: the shared tail stamps 0x80 and clears building_index to -1 "
       "(@0x004e7fd9-0x004e7fe1)");

    // -- the population-delta match.
    set_entry(20, 30, (uint8_t)(1 | QUEUE_STATUS_AFFORD_WAIVED));
    f.players[P].ai_build_candidate_primary = BT;
    g_rec.footprint_answer                  = 1;
    run_entry();
    ck(rep.population_delta == 1 && g_rec.population_deltas.size() == 1 &&
           g_rec.population_deltas[0].player == P && g_rec.population_deltas[0].delta == 13,
       "process_entry: a queued type equal to ai_build_candidate_primary also enqueues a population "
       "delta of that type's cfg worker_count, as (player, delta) -- the reverse of recruit's order");

    // -- stage C, the INSTANT half.
    set_entry(20, 30, 1); // no waiver bit
    for (int j = 0; j < 4; ++j) {
        entry(QI).resource_reserved[1 + j] = (int16_t)(10 + j);
        f.players[P].resource_spent[j]     = 1000;
    }
    f.players[P].ai_patrol_quadrant_cursor  = 777;
    f.players[P].ai_build_candidate_primary = BT; // deliberately a MATCH -- see the assertion
    g_rec.footprint_answer                  = 1;
    run_entry();
    ck(rep.built_instant == 1 && g_rec.instant_orders.size() == 1 &&
           g_rec.instant_orders[0].x == 20 && g_rec.instant_orders[0].y == 30 &&
           g_rec.instant_orders[0].type == BT && g_rec.instant_orders[0].player == (uint16_t)P,
       "process_entry: without the waiver it instant-constructs at the cached tile");
    ck(at(g_rec.instant_orders, 0).param_4 == 0,
       "process_entry: the instant-construct call's fourth argument is a literal 0 (XOR ECX,ECX "
       "@0x004e7f6d), not the player or the type");
    ck(g_rec.expenditures.size() == 1 && g_rec.expenditures[0].player == P &&
           g_rec.expenditures[0].building_id == BT,
       "process_entry: ... followed by bldg_record_resource_expenditure_stats(player, type)");
    bool instant_debited = f.players[P].ai_patrol_quadrant_cursor == 777;
    for (int j = 0; j < 4; ++j)
        instant_debited = instant_debited && f.players[P].resource_spent[j] == 1000 - (10 + j);
    ck(instant_debited,
       "process_entry: the instant path is the one that DEBITS resource_spent[1..4], and it leaves "
       "slot 0 alone");
    ck(g_rec.construction_orders.empty() && g_rec.population_deltas.empty(),
       "process_entry: the instant path issues no construction order and NEVER reaches the "
       "population-delta test -- that test lives inside the waived branch, and the primary type "
       "matches here precisely so a mis-scoped test would fire");

    // -- stage D: retire on the race-paired TURRET, keeping the cached tile.
    set_entry(20, 30, 1);
    f.players[P].is_alien_race = 0;
    f.cfg_buildings[BT].type   = BLDG_TYPE_H_TURRET;
    g_rec.footprint_answer     = 0;
    run_entry();
    ck(rep.blocked_retired == 1 && (entry(QI).status & QUEUE_STATUS_REMOVED) != 0 &&
           entry(QI).build_tile_x == 20,
       "process_entry: a human TURRET that does not fit is RETIRED (OR 0x40 @0x004e8083) and its "
       "cached tile is left alone");
    ck(g_rec.map_notifies.size() == 1 && g_rec.map_notifies[0].second &&
           g_rec.map_notifies[0].player == (int32_t)P &&
           g_rec.map_notifies[0].building_type == BT && g_rec.map_notifies[0].tile_x == 20 &&
           g_rec.map_notifies[0].tile_y == 30,
       "process_entry: stage D finishes with notify_map_changed_2 -- the SECOND notifier, a "
       "different function with the same four-argument shape");

    // -- and on the race-paired RELAY.
    set_entry(20, 30, 1);
    f.players[P].is_alien_race = 0;
    f.cfg_buildings[BT].type   = BLDG_TYPE_H_RELAY;
    g_rec.footprint_answer     = 0;
    run_entry();
    ck(rep.blocked_retired == 1,
       "process_entry: the human RELAY is the second retire type (the CMP against 0x23 @0x004e8065)");

    // -- the pairing is selected by is_alien_race, checked in BOTH directions. A translation that
    //    tested all four constants unconditionally passes the two cases above and fails these.
    set_entry(20, 30, 1);
    f.players[P].is_alien_race = 0;
    f.cfg_buildings[BT].type   = BLDG_TYPE_A_TURRET;
    g_rec.footprint_answer     = 0;
    run_entry();
    ck(rep.blocked_requeued == 1 && rep.blocked_retired == 0,
       "process_entry: the ALIEN turret type is an ORDINARY type for a human player -- it is "
       "re-queued, not retired");

    set_entry(20, 30, 1);
    f.players[P].is_alien_race = 1;
    f.cfg_buildings[BT].type   = BLDG_TYPE_H_TURRET;
    g_rec.footprint_answer     = 0;
    run_entry();
    ck(rep.blocked_requeued == 1 && rep.blocked_retired == 0,
       "process_entry: ... and symmetrically, the HUMAN turret type is ordinary for an alien player");

    set_entry(20, 30, 1);
    f.players[P].is_alien_race = 1;
    f.cfg_buildings[BT].type   = BLDG_TYPE_A_TURRET;
    g_rec.footprint_answer     = 0;
    run_entry();
    ck(rep.blocked_retired == 1,
       "process_entry: an alien player DOES retire the alien turret type (0x5 @0x004e8011)");

    // -- the requeue, and the tile re-read that follows it.
    set_entry(20, 30, 1);
    f.players[P].is_alien_race = 0;
    f.cfg_buildings[BT].type   = 99;
    g_rec.footprint_answer     = 0;
    run_entry();
    ck(rep.blocked_requeued == 1 && entry(QI).build_tile_x == (int16_t)0xffff &&
           (entry(QI).status & QUEUE_STATUS_REMOVED) == 0 && entry(QI).status == 1,
       "process_entry: any other type has its cached tile invalidated to 0xffff and keeps its "
       "status, so stage A resolves it again next tick");
    ck(g_rec.map_notifies.size() == 1 && g_rec.map_notifies[0].second &&
           g_rec.map_notifies[0].tile_x == 65535 && g_rec.map_notifies[0].tile_y == 30,
       "process_entry: notify_map_changed_2 RE-READS the tile after the invalidation "
       "(@0x004e80cb-0x004e80d3), so it is told 0xffff -- a translation that kept the tile in a "
       "local across the write reports the old one, and the retire case above proves the read is "
       "not simply hardcoded");

    // -- the row is the ticking player's own.
    set_entry(20, 30, 1);
    g_rec.footprint_answer                      = 0;
    f.players[0].ai_bldg_queue[QI].status       = 1;
    f.players[0].ai_bldg_queue[QI].build_tile_x = 20;
    run_entry();
    ck(f.players[0].ai_bldg_queue[QI].status == 1 &&
           f.players[0].ai_bldg_queue[QI].build_tile_x == 20,
       "process_entry: player 0's queue entry is untouched when P != 0");

    // ================================ nibbles 3 and 4: upgrade_or_cancel =========================
    auto run_upg = [&]() {
        rep = detail::queue_dispatch_report{};
        detail::bldg_queue_handle_upgrade_or_cancel(f.view(), f.store(), stub_calls(), P, QI, rep);
    };
    auto set_upg = [&](uint8_t nibble) {
        f.reset();
        g_rec.clear();
        entry(QI).status               = nibble;
        entry(QI).building_index       = 0x11223344;
        entry(QI).resource_reserved[0] = 500;
        for (int j = 0; j < 4; ++j) {
            entry(QI).resource_reserved[1 + j] = (int16_t)(10 + j);
            f.players[P].resource_spent[j]     = 1000; // id 1+j -> element j since the reshape
        }
        // The sentinel below resource_spent[0]: a debit loop that under-ran would land here.
        f.players[P].ai_patrol_quadrant_cursor = 777;
    };

    set_upg(4);
    run_upg();
    ck(rep.upgraded == 1 && rep.cancelled == 0 && g_rec.upgrade_orders.size() == 1 &&
           !g_rec.upgrade_orders[0].cancel && g_rec.upgrade_orders[0].player == P &&
           g_rec.upgrade_orders[0].building_index == 0x11223344,
       "upgrade_or_cancel: nibble 4 issues the UPGRADE order, and building_index travels as the "
       "full int32 it is");
    bool upg_debited = f.players[P].ai_patrol_quadrant_cursor == 777;
    for (int j = 0; j < 4; ++j)
        upg_debited = upg_debited && f.players[P].resource_spent[j] == 1000 - (10 + j);
    ck(upg_debited,
       "upgrade_or_cancel: nibble 4 debits resource_spent[1..4] by resource_reserved[1..4] -- four "
       "unrolled copies at 0x004e822a-0x004e8258, not a loop, and slot 0 is not one of them");
    ck(entry(QI).status == (uint8_t)(0x80 | 4),
       "upgrade_or_cancel: the entry is stamped COMMITTED");

    set_upg(3);
    run_upg();
    ck(rep.cancelled == 1 && rep.upgraded == 0 && g_rec.upgrade_orders.size() == 1 &&
           g_rec.upgrade_orders[0].cancel && g_rec.upgrade_orders[0].player == P &&
           g_rec.upgrade_orders[0].building_index == 0x11223344,
       "upgrade_or_cancel: nibble 3 issues the CANCEL order instead");
    bool cancel_moved_nothing = f.players[P].ai_patrol_quadrant_cursor == 777;
    for (int j = 0; j < 4; ++j)
        cancel_moved_nothing = cancel_moved_nothing && f.players[P].resource_spent[j] == 1000;
    ck(cancel_moved_nothing,
       "upgrade_or_cancel: the cancel arm moves NO resources -- the debit is inside the nibble-4 "
       "half only");
    ck(entry(QI).status == (uint8_t)(0x80 | 3),
       "upgrade_or_cancel: the 0x80 stamp is made BEFORE the nibble branch (@0x004e8215), so the "
       "CANCEL arm carries it too -- a translation that stamped inside the upgrade half alone "
       "leaves this entry at 0x03 and the queue never skips it");

    // -- the test is `!= 4`, not `== 3`. The parent's table routes only 3 and 4 here, but the
    //    original's own branch is the weaker one and is reproduced as such.
    set_upg(0);
    run_upg();
    ck(rep.cancelled == 1 && g_rec.upgrade_orders.size() == 1 && g_rec.upgrade_orders[0].cancel,
       "upgrade_or_cancel: the original tests only `!= 4` (CMP CL,4 / JNZ @0x004e8225), so any "
       "other nibble reaching this arm takes the CANCEL half");

    // -- the same zero-extended short read as the other two arms.
    set_upg(4);
    entry(QI).resource_reserved[1]          = (int16_t)-1;
    f.players[P].resource_spent[0] /*id 1*/ = 0;
    run_upg();
    ck(f.players[P].resource_spent[0] /*id 1*/ == -65535,
       "upgrade_or_cancel: its debit is the same MOVZX-then-SUB shape (@0x004e822a) as the recruit "
       "and construction arms");

    // ================================ nibble 2: the 11-byte no-op ================================
    //
    // This arm CANNOT write player state -- its signature takes neither the view nor the store, so
    // the guarantee is structural and this check is what makes that structure load-bearing rather
    // than incidental: it goes red if a later edit hands the arm state and a side effect. Whether
    // the slot is ever REACHED is a different question, answered by test_bldg_queue's by_kind[2]
    // and, on the rig, by the parent site's kinds[2] counter -- not here.
    f.reset();
    g_rec.clear();
    std::memset(&f.players[P], 0x5a, sizeof(player_data));
    std::vector<uint8_t> before(sizeof(player_data));
    std::memcpy(before.data(), &f.players[P], sizeof(player_data));
    rep = detail::queue_dispatch_report{};
    detail::bldg_queue_handle_state2_empty(rep);
    ck(rep.noop_calls == 1 &&
           std::memcmp(before.data(), &f.players[P], sizeof(player_data)) == 0,
       "state2_empty: the nibble-2 arm writes nothing at all -- the player record is byte-identical "
       "afterwards, and the arm takes no state to write through");
    f.reset();
}

// ---- batch B layer 3: the SILO-capacity gate ----------------------------------------------------
//
// The rig can only ever prove that our return matched the original's; everything about WHICH arm
// produced that return is offline-only, and the ratio's direction -- the thing the original's plate
// had backwards until EN v204 -- is what these fixtures separate.
void test_resource_shortage_gate() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 3;

    auto res = [&](int32_t d) -> int32_t & {
        return f.resources[(size_t)P * RESOURCE_SLOTS_PER_PLAYER + d];
    };
    // The baseline: every resource comfortably NOT short (capacity 100 against holdings 10 is a
    // ratio of 10, far above fSiloRatio 1.1), the cap test set so that a shortage WOULD return 1.
    auto setup = [&]() {
        f.reset();
        g_rec.clear();
        for (int32_t d = RESOURCE_ID_FIRST; d <= RESOURCE_ID_LAST; ++d) {
            f.storage[P].cap_prev[d] = 100;
            res(d)                   = 10;
        }
        f.players[P].ai_build_candidate_shortage = 44;
        f.players[P].ai_score_bldg_type_b        = 5;
        g_rec.count_by_id_ans[44]                = 2; // 2 < 5 -> the shortage arm returns 1
    };
    auto run = [&]() { return detail::storage_capacity_short_and_cap_check(f.view(), stub_calls(), P); };

    setup();
    ck(run() == 0 && g_rec.type_queries.empty(),
       "silo_gate: with capacity far above holdings NOTHING is short -- it returns 0 and never even "
       "asks the count question");

    // -- THE DIRECTION. Capacity BELOW holdings * fSiloRatio is the shortage; the inverse is not.
    setup();
    f.storage[P].cap_prev[2] = 10;
    res(2)                   = 100; // ratio 0.1 < 1.1
    ck(run() == 1,
       "silo_gate: capacity 10 against holdings 100 IS short -- the CAPACITY is the numerator and "
       "the HOLDINGS the divisor, which is the way round the function's old plate had backwards");
    setup();
    f.storage[P].cap_prev[2] = 100;
    res(2)                   = 10; // the same pair swapped: ratio 10, not short
    ck(run() == 0,
       "silo_gate: the same two numbers the other way round are NOT short -- a translation that "
       "divided holdings by capacity passes the previous check and fails this one");

    // -- the threshold is STRICTLY greater, and it is fSiloRatio and not a literal.
    setup();
    f.storage[P].cap_prev[1] = 11;
    res(1)                   = 10; // ratio exactly 1.1 in decimal, but 1.1f is 1.10000002384...
    f.silo                   = 1.1f;
    ck(run() == 1,
       "silo_gate: a ratio of 11/10 is SHORT against fSiloRatio 1.1f -- the float constant is "
       "1.10000002384185791, strictly above the exact quotient, and the compare is strict");
    f.silo = 1.0f;
    ck(run() == 0,
       "silo_gate: the same state against fSiloRatio 1.0 is not short -- the threshold is read from "
       "the view, not baked in");
    // EQUALITY is the case the two halves of the JBE separate: C0 (below) and C3 (equal) both leave
    // the loop running, so a translation that only tested C0 calls an exactly-equal ratio short.
    setup();
    f.storage[P].cap_prev[1] = 2;
    res(1)                   = 1;
    f.silo                   = 2.0f;
    ck(run() == 0,
       "silo_gate: a ratio EXACTLY equal to fSiloRatio is not short -- the original's JBE leaves on "
       "C0 or C3, so equality continues the scan");

    // -- the DIVIDE-BY-ZERO flush, and which side it protects.
    // The flush replaces a +-0.0 DIVISOR with 1.0, and which operand it lands on is observable
    // exactly here: with both sides zero the correct reading gives 0/1 = 0, which is below the
    // threshold and therefore SHORT, while a translation that flushed the NUMERATOR instead gives
    // 1/0 = +inf and reports not-short. (An earlier draft of this assertion expected 0 on the
    // reasoning that "a ratio of zero is harmless" -- it is not; zero is the smallest ratio there
    // is, and no capacity at all for holdings you have is the most short a resource can be.)
    setup();
    f.storage[P].cap_prev[3] = 0;
    res(3)                   = 0;
    ck(run() == 1,
       "silo_gate: zero on BOTH sides is short -- the divisor is flushed to 1.0, giving 0/1 = 0, "
       "where flushing the numerator would give 1/0 = +inf and report not-short");
    setup();
    f.storage[P].cap_prev[3] = 0;
    res(3)                   = 5;
    ck(run() == 1,
       "silo_gate: zero CAPACITY against real holdings is short too -- no flush is involved on this "
       "one, so it separates the flush from the ordinary zero-numerator case above");
    setup();
    f.storage[P].cap_prev[3] = 100;
    res(3)                   = 0;
    ck(run() == 0,
       "silo_gate: real capacity against ZERO holdings is NOT short -- 100/1 after the flush, which "
       "is the case the flush exists to keep finite");

    // -- resource 0 is never examined, and neither is 5.
    setup();
    f.storage[P].cap_prev[0] = 0;
    res(0)                   = 1000;
    f.storage[P].cap_prev[5] = 0;
    res(5)                   = 1000;
    ck(run() == 0,
       "silo_gate: the loop is d = 1..4 INCLUSIVE -- a short resource at index 0 or 5 is invisible "
       "to it");

    // -- it STOPS at the first short resource.
    setup();
    f.storage[P].cap_prev[1] = 0;
    f.storage[P].cap_prev[4] = 0;
    ck(run() == 1 && g_rec.type_queries.size() == 1,
       "silo_gate: two short resources still ask the count question exactly ONCE -- the first "
       "shortage returns rather than continuing the scan");

    // -- the count/cap comparison itself, including its UNSIGNEDNESS.
    setup();
    f.storage[P].cap_prev[1]          = 0;
    g_rec.count_by_id_ans[44]         = 5;
    f.players[P].ai_score_bldg_type_b = 5;
    ck(run() == 0, "silo_gate: the cap test is STRICT -- count == cap returns 0, not 1");
    ck(g_rec.type_queries.size() == 1 && g_rec.type_queries[0].kind == 0 &&
           g_rec.type_queries[0].player == P && g_rec.type_queries[0].arg == 44,
       "silo_gate: the count is asked by BUILDING ID (kind 0 = count_by_id) about the ticking "
       "player and ai_build_candidate_shortage");
    // ... and a FAILED cap test returns THERE. With a second short resource behind it, a
    // translation that treated the failure as "try the next resource" would ask twice.
    setup();
    f.storage[P].cap_prev[1]          = 0;
    f.storage[P].cap_prev[4]          = 0;
    g_rec.count_by_id_ans[44]         = 5;
    f.players[P].ai_score_bldg_type_b = 5;
    ck(run() == 0 && g_rec.type_queries.size() == 1,
       "silo_gate: a failed cap test returns 0 rather than moving on to the next short resource -- "
       "the count question is asked once even with two resources short");
    setup();
    f.storage[P].cap_prev[1]          = 0;
    g_rec.count_by_id_ans[44]         = 1;
    f.players[P].ai_score_bldg_type_b = -1; // 0xffffffff unsigned -- everything is below it
    ck(run() == 1,
       "silo_gate: the compare is UNSIGNED (SETC) -- a NEGATIVE ai_score_bldg_type_b reads as "
       "0xffffffff and the test passes, where a signed reading would fail it");
    setup();
    f.storage[P].cap_prev[1]          = 0;
    g_rec.count_by_id_ans[44]         = -1; // 0xffffffff -- above every non-negative cap
    f.players[P].ai_score_bldg_type_b = 5;
    ck(run() == 0, "silo_gate: and unsigned on the other operand too");

    // -- per-player. A shortage on the WRONG player must not leak in.
    setup();
    f.storage[P - 1].cap_prev[1]                                 = 0;
    f.resources[(size_t)(P - 1) * RESOURCE_SLOTS_PER_PLAYER + 1] = 500;
    ck(run() == 0,
       "silo_gate: both arrays are indexed by the ticking player -- storage stride 0x58, resources "
       "stride 40, and neither is the other's");
}

// ---- batch B layer 3: the mine-construction planner ----------------------------------------------
void test_plan_mine_construction() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P = 2;

    auto setup = [&]() {
        f.reset();
        g_rec.clear();
        g_mine_min_cell                 = &f.mine_min;
        g_rec.rebalance_writes          = true;
        player_data &p                  = f.players[P];
        p.ai_mine_candidate_tier1       = 7;
        p.ai_mine_candidate_tier2       = 9;
        p.ai_building_type_available[7] = 1;
        p.ai_building_type_available[9] = 1;
        // base = 1 + 2 + 3 + 4 = 10  ->  min_count 21, ceiling 30
        p.ai_score_cat_0x10 = 1;
        p.ai_score_cat_0x11 = 2;
        p.ai_score_cat_0x12 = 3;
        p.ai_score_cat_0x13 = 4;
        // 25: above the 21 min_count (so the accumulator is NOT force-set) and below the 30 ceiling
        // (so the second gate does not veto). Both gates therefore ride on the shortage flags.
        p.ai_score_bldg_type_b = 25;
        p.ai_clock             = 1.0f;
        for (int32_t d = RESOURCE_ID_FIRST; d <= RESOURCE_ID_LAST; ++d) {
            p.resource_spend_total[d] = 1;
            g_rec.rebalance_yield[d]  = 100; // yield 100 >> rate 1 -> no shortage anywhere
        }
    };
    auto run = [&]() { detail::plan_mine_construction(f.view(), f.store(), stub_calls(), P); };

    // -- TYPE SELECTION, and which tier wins.
    setup();
    run();
    ck(g_rec.rebalances.size() == 1 && g_rec.constructions.empty(),
       "mine_plan: with a type chosen but nothing short, the rebalance still runs and no build is "
       "queued -- the yield refresh is unconditional once past the type gate");
    setup();
    g_rec.rebalance_yield[2] = 0; // a zero yield forces that resource short
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 9,
       "mine_plan: tier2 is tested SECOND and overwrites, so it wins when both slots are available");
    setup();
    f.players[P].ai_building_type_available[9] = 0;
    g_rec.rebalance_yield[2]                   = 0;
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == 7,
       "mine_plan: tier2 unavailable falls back to tier1 -- availability is tested per slot, not "
       "once");
    setup();
    f.players[P].ai_mine_candidate_tier1 = -1;
    f.players[P].ai_mine_candidate_tier2 = -1;
    run();
    ck(g_rec.rebalances.empty() && f.mine_min == -12345,
       "mine_plan: both slots -1 returns BEFORE the scratch global is written -- the poison value "
       "survives, which is how a body that hoisted the store above the type gate is caught");

    // -- THE SCRATCH GLOBAL, and the ordering that makes it load-bearing.
    setup();
    g_rec.rebalance_yield[2] = 0;
    run();
    ck(f.mine_min == 21,
       "mine_plan: _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT ends at 2*base + 1 = 21, not base and "
       "not 2*base -- the first two stores at 0x004e537a/86 are dead");
    ck(g_rec.rebalances.size() == 1 && g_rec.rebalances[0].min_count_seen == 21,
       "mine_plan: the global already holds 2*base + 1 WHEN the rebalance is called -- the real "
       "callee reads it at 0x004e35ab, so a store made after the call would be invisible here and "
       "wrong in the game");
    ck(g_rec.rebalances[0].player == P &&
           g_rec.rebalances[0].out_yield == (void *)&f.players[P].ai_mine_yield_by_resource[0],
       "mine_plan: the out-pointer is &player_data[player].ai_mine_yield_by_resource[0] -- the LEA "
       "at 0x004e53a1 over the TICKING player, not player 0 and not player + 1");

    // -- THE ALREADY-QUEUED GATE runs AFTER the rebalance, not before it.
    setup();
    g_rec.already_queued_ans[9] = 1;
    g_rec.rebalance_yield[2]    = 0;
    run();
    ck(g_rec.rebalances.size() == 1 && g_rec.constructions.empty() && f.mine_min == 21,
       "mine_plan: an already-queued mine type still runs the rebalance and still writes the global "
       "-- the gate is at 0x004e53bb, downstream of both");

    // -- THE SHORTAGE FLAGS.
    // The zero-yield override, and the input has to SEPARATE it from the rate test or it proves
    // nothing: with any positive spend total, a zero yield is already below the rate and both
    // readings agree. spend_total 0 makes the rate 0, so `0 < 0` is false and ONLY the override can
    // make the resource short. (spend_total is initialised to 1 by the game's own spawn paths and
    // only accumulates, so this is a synthetic input -- it is here to separate two implementations,
    // not to model a reachable state.)
    setup();
    f.players[P].resource_spend_total[3] = 0;
    g_rec.rebalance_yield[3]             = 0;
    run();
    ck(g_rec.constructions.size() == 1,
       "mine_plan: a ZERO yield estimate forces that resource short even when the rate test says "
       "otherwise (@0x004e5434) -- with spend_total 0 the rate is 0 and `yield < rate` is false");
    setup();
    f.players[P].resource_spend_total[3] = 500;
    f.players[P].ai_clock                = 1.0f;
    g_rec.rebalance_yield[3]             = 100; // 100 < 500/1
    run();
    ck(g_rec.constructions.size() == 1,
       "mine_plan: yield below spend_total / ai_clock is short");
    setup();
    f.players[P].resource_spend_total[3] = 500;
    f.players[P].ai_clock                = 100.0f; // rate 5, below the yield of 100
    run();
    ck(g_rec.constructions.empty(),
       "mine_plan: the SAME spend total against a larger ai_clock is not short -- the clock is the "
       "divisor, so a translation that multiplied by it queues here");
    // The flags are read from what the REBALANCE wrote, not from what was there before it.
    setup();
    for (int32_t d = RESOURCE_ID_FIRST; d <= RESOURCE_ID_LAST; ++d)
        f.players[P].ai_mine_yield_by_resource[d] = 0; // pre-call: everything looks short
    run();
    ck(g_rec.constructions.empty(),
       "mine_plan: the shortage flags are scored on the POST-rebalance yields -- a body that read "
       "the array before the call would see four zeros here and queue");

    // -- THE TWO GATES.
    setup();
    f.players[P].ai_score_bldg_type_b = 21; // == 2*base + 1, i.e. NOT above it
    run();
    ck(g_rec.constructions.size() == 1,
       "mine_plan: a mine count at or below 2*base + 1 FORCES the accumulator to 1, so a build is "
       "queued even with no resource short at all");
    setup();
    f.players[P].ai_score_bldg_type_b = 22; // just above the min_count
    run();
    ck(g_rec.constructions.empty(),
       "mine_plan: one above it and the force does not happen -- the compare is JA, strictly above");
    setup();
    g_rec.rebalance_yield[2]          = 0;  // a genuine shortage
    f.players[P].ai_score_bldg_type_b = 30; // == 3*base, the ceiling
    run();
    ck(g_rec.constructions.empty(),
       "mine_plan: the SECOND gate vetoes at mine count >= 3*base even with a shortage flag set -- "
       "this is the gate the old plate omitted entirely");
    setup();
    g_rec.rebalance_yield[2]          = 0;
    f.players[P].ai_score_bldg_type_b = 29;
    run();
    ck(g_rec.constructions.size() == 1, "mine_plan: one below the ceiling and it queues");

    // -- THE QUEUE CALL and the rotate that follows it.
    setup();
    g_rec.rebalance_yield[2] = 0;
    run();
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].player == P &&
           g_rec.constructions[0].x == -1 && g_rec.constructions[0].y == 0,
       "mine_plan: the queue call is (player, type, x = -1, y = 0) -- a SIGNED -1 in x, the "
       "auto-placement sentinel");
    ck(g_rec.rotates.size() == 1 && g_rec.rotates[0] == P,
       "mine_plan: the newest entry is rotated to the FRONT afterwards, for the ticking player");
    setup();
    run();
    ck(g_rec.rotates.empty(),
       "mine_plan: no queue, no rotate -- the rotate is inside the same tail, not unconditional");

    g_mine_min_cell = &g_mine_min_dummy;
}

// ---- batch B layer 3: the turret-upgrade planner -------------------------------------------------
//
// WHY THIS EXISTS RATHER THAN LEANING ON THE SHADOW SITE. Every branch below is scenario-bound in a
// way no step count fixes: the gate needs a researched turret type, pass 1 needs a building CUT OFF
// from the mother base, and the cut-vertex arm needs the excluded building to be an articulation
// point of the connectivity graph -- a compact base never has one. The site is armed anyway, but the
// evidence for these arms is here.
void test_plan_turret_upgrade() {
    using namespace mh::ai;
    fixture        f;
    const uint32_t P    = 2;
    const int32_t  CAND = 11; // the cached candidate turret type

    // Two buildings: slot 1 is DISCONNECTED (base flag 0) and slot 2 is connected. Slot 0 carries
    // the roster count, as the original reads it.
    auto setup = [&]() {
        f.reset();
        g_rec.clear();
        f.players[P].ai_turret_candidate = CAND;
        f.b(P, 0).index                  = 2;
        f.b(P, 1).building_id            = 5;
        f.b(P, 1).state                  = 4;
        f.b(P, 1).x                      = 20;
        f.b(P, 1).y                      = 30;
        f.b(P, 2).building_id            = (uint16_t)CAND;
        f.b(P, 2).state                  = 4;
        f.b(P, 2).x                      = 40;
        f.b(P, 2).y                      = 50;
        g_rec.flood_writes               = true;
        g_rec.flood_base_ans[1]          = 0; // slot 1 is NOT reachable -> pass 1 acts on it
        g_rec.flood_base_ans[2]          = 1;
        g_rec.flood_trial_ans[1]         = 0; // by default the trial fill agrees -> not a cut vertex
        g_rec.flood_trial_ans[2]         = 1;
        g_rec.nearest_ans                = 2;
        g_rec.midpoint_out_x             = 30;
        g_rec.midpoint_out_y             = 40;
        g_rec.footprint_answer           = 1; // nonzero == the footprint FITS
        f.ring_counts[15]                = 3; // three spiral cells to try
        f.spiral[0].dx                   = 0;
        f.spiral[0].dy                   = 0;
        f.spiral[1].dx                   = 1;
        f.spiral[1].dy                   = 0;
        f.spiral[2].dx                   = 0;
        f.spiral[2].dy                   = 1;
        g_rec.already_queued_ans[CAND]   = 0;
    };
    auto run = [&]() { detail::plan_turret_upgrade(f.view(), f.store(), stub_calls(), P); };

    // -- THE GATE. -1 returns before ANYTHING, including the first flood fill.
    setup();
    f.players[P].ai_turret_candidate = -1;
    run();
    ck(g_rec.floods.empty() && g_rec.constructions.empty() && g_rec.restart_orders.empty(),
       "turret_plan: ai_turret_candidate == -1 returns before the first flood fill -- the gate is "
       "the very first thing, not a filter applied later");
    setup();
    run();
    ck(!g_rec.floods.empty() && g_rec.floods[0].exclude == 0 &&
           g_rec.floods[0].plane == (void *)f.conn_base.data(),
       "turret_plan: the FIRST fill excludes nothing (exclude_bldg_idx = 0) and fills the BASE "
       "plane -- the full reachability graph");

    // -- PASS 1 acts on the DISCONNECTED building, not the connected one.
    setup();
    run();
    ck(g_rec.nearests.size() == 1 && g_rec.nearests[0].x == 20 && g_rec.nearests[0].y == 30,
       "turret_plan: pass 1 runs on slot 1 (base flag CLEAR) and asks for the nearest FLAGGED "
       "building from ITS coordinates -- slot 2, whose flag is set, is skipped");
    ck(g_rec.midpoints.size() == 1 && g_rec.midpoints[0].x0 == 20 && g_rec.midpoints[0].y0 == 30 &&
           g_rec.midpoints[0].x1 == 40 && g_rec.midpoints[0].y1 == 50,
       "turret_plan: the midpoint is taken between the DISCONNECTED building (x0, y0) and the "
       "nearest flagged one (x1, y1), in that order -- the push sequence read bottom-up");
    ck(g_rec.constructions.size() == 1 && g_rec.constructions[0].building_type == CAND &&
           g_rec.constructions[0].x == 30 && g_rec.constructions[0].y == 40,
       "turret_plan: the first spiral cell that fits queues the CANDIDATE type at the midpoint "
       "(offset 0, 0) -- and the queue call takes the tile, not the building");
    ck(g_rec.map_notifies.size() == 1 && !g_rec.map_notifies[0].second &&
           g_rec.map_notifies[0].tile_x == 30 && g_rec.map_notifies[0].tile_y == 40,
       "turret_plan: the queue is followed by notify_map_changed (NOT notify_map_changed_2) with "
       "the same tile");

    // -- THE FLAG SENSE. Setting slot 1's base flag makes pass 1 skip everything.
    setup();
    g_rec.flood_base_ans[1] = 1;
    run();
    ck(g_rec.nearests.empty() && g_rec.constructions.empty(),
       "turret_plan CONTROL: with EVERY building reachable, pass 1 does nothing -- the test is "
       "`flag == 0`, so a translation with the sense inverted would act on the connected ones");

    // -- THE THREE SKIP STATES.
    for (int32_t st : {0x6b, 2, 3}) {
        setup();
        f.b(P, 1).state = (uint16_t)st;
        run();
        ck(g_rec.nearests.empty(),
           "turret_plan: states 0x6b / 2 / 3 skip the building in pass 1");
    }

    // -- THE OWNED-BUILDING TILE ABANDONS THE WHOLE BUILDING, not just that spiral cell.
    setup();
    {
        // Cell 0 is (30, 40): make it an owned BUILDING tile. Cells 1 and 2 stay clear and would
        // both fit, so a translation that only skipped THIS cell would still queue a construction.
        const int32_t x = (30 + 0) & (int32_t)f.map_wm, y = (40 + 0) & (int32_t)f.map_hm;
        f.tiles[(x << 8) | y].class_owner = (uint8_t)(0x40 | P);
        run();
        ck(g_rec.constructions.empty() && g_rec.footprints.empty(),
           "turret_plan: an OWNED BUILDING tile in the spiral abandons the whole building -- the "
           "jump target is the OUTER loop's increment, so cells 1 and 2 are never tried and the "
           "footprint test never runs");
    }
    // The same tile owned by SOMEONE ELSE does not abandon: the owner nibble must match.
    setup();
    {
        const int32_t x = (30 + 0) & (int32_t)f.map_wm, y = (40 + 0) & (int32_t)f.map_hm;
        f.tiles[(x << 8) | y].class_owner = (uint8_t)(0x40 | (P + 1));
        run();
        ck(g_rec.constructions.size() == 1,
           "turret_plan CONTROL: the same 0x40 tile owned by a DIFFERENT player does not abandon -- "
           "both halves of the test must hold");
    }
    // And an owned tile that is NOT a building (0x40 clear) does not abandon either.
    setup();
    {
        const int32_t x = (30 + 0) & (int32_t)f.map_wm, y = (40 + 0) & (int32_t)f.map_hm;
        f.tiles[(x << 8) | y].class_owner = (uint8_t)(0x80 | P);
        run();
        ck(g_rec.constructions.size() == 1,
           "turret_plan CONTROL: an owned NON-building tile (0x80, a unit) does not abandon");
    }

    // -- THE SPIRAL WALKS UNTIL SOMETHING FITS.
    setup();
    g_rec.footprint_answer = 0;
    run();
    ck(g_rec.footprints.size() == 3 && g_rec.constructions.empty(),
       "turret_plan: with nothing fitting, all three spiral cells are tried and nothing is queued -- "
       "and the count comes from spiral_ring_cell_counts[15], the radius-15 element");

    // -- PASS 2. It only runs when the type is not already queued.
    setup();
    g_rec.already_queued_ans[CAND] = 1;
    run();
    ck(g_rec.floods.size() == 1 && g_rec.restart_orders.empty(),
       "turret_plan: an already-queued candidate type returns BETWEEN the passes -- one fill only, "
       "no trial fill and no restart order");

    setup();
    run();
    ck(g_rec.floods.size() == 2 && g_rec.floods[1].exclude == 2 &&
           g_rec.floods[1].plane == (void *)f.conn_trial.data(),
       "turret_plan: pass 2's fill EXCLUDES the candidate building and fills the TRIAL plane -- a "
       "body that reused the base plane would make the cut-vertex diff trivially empty");
    ck(g_rec.restart_orders.size() == 1 && g_rec.restart_orders[0].player == P &&
           g_rec.restart_orders[0].building_index == 2,
       "turret_plan: with the two planes agreeing, building 2 is NOT a cut vertex and its "
       "construction is restarted");

    // -- THE CUT-VERTEX BRANCH. Make the trial fill disagree on a live slot.
    setup();
    g_rec.flood_trial_ans[1] = 1; // base says 0, trial says 1 -> the planes disagree at slot 1
    run();
    ck(g_rec.restart_orders.empty(),
       "turret_plan: a slot where the trial and base planes DISAGREE marks the building blocked and "
       "no restart order is issued -- the cut-vertex veto");

    // -- The disagreement must be on a LIVE slot: an empty slot is skipped before the comparison.
    //
    // THE ROSTER IS DELIBERATELY HOLED. A first attempt put the disagreement at slot 3 of a
    // contiguous two-building roster, and it could not fail: the inner walk's budget runs out at
    // slot 2 whether or not the empty-slot test is there, so slot 3 is never visited under EITHER
    // reading. It takes an EMPTY slot BETWEEN two live ones -- slot 2 here -- for the two readings
    // to visit different indices at all. (Caught by mutation 5 of
    // tools/oneoff/2026-08-03-mutate-turret-worker-planners.py, which the first version MISSED.)
    setup();
    f.b(P, 1).building_id    = (uint16_t)CAND; // the restart target moves to slot 1
    f.b(P, 1).state          = 4;
    f.b(P, 2).building_id    = 0; // slot 2 is now EMPTY
    f.b(P, 3).building_id    = 7;
    f.b(P, 3).state          = 4;
    f.b(P, 0).index          = 2; // two live buildings, at slots 1 and 3
    g_rec.flood_base_ans[1]  = 1; // both reachable, so pass 1 does nothing at all
    g_rec.flood_base_ans[3]  = 1;
    g_rec.flood_trial_ans[1] = 1;
    g_rec.flood_trial_ans[3] = 1;
    g_rec.flood_base_ans[2]  = 0; // and the planes DISAGREE at the empty slot -- setup() leaves both
    g_rec.flood_trial_ans[2] = 1; // of these at 1, which would have made the case vacuous
    run();
    ck(g_rec.restart_orders.size() == 1 && g_rec.restart_orders[0].building_index == 1,
       "turret_plan CONTROL: a disagreement on an EMPTY roster slot is never seen -- the inner walk "
       "tests building_id first and only then the planes, so a hole between two live slots is "
       "stepped over rather than compared");

    // -- PASS 2 ONLY LOOKS AT THE CANDIDATE TYPE.
    setup();
    f.b(P, 2).building_id = (uint16_t)(CAND + 1);
    run();
    ck(g_rec.floods.size() == 1 && g_rec.restart_orders.empty(),
       "turret_plan: pass 2 matches building_id against the candidate type exactly -- no other "
       "building is a restart target, and with no match there is no trial fill at all");
}

// ---- batch B layer 3: the worker/builder rebalance -----------------------------------------------
//
// WHY THIS EXISTS RATHER THAN LEANING ON THE SHADOW SITE. The function has no vacuity gate, so the
// rig will exercise its common path -- but three of its arms are not reachable by asking for more
// steps: the `pop_total == housing_prev` reserve zeroing, the `1.0 <= util` early return, and above
// all the UNGUARDED DIVIDE at 0x004e3d68, which needs a player whose worker demand AND locked-builder
// total are both zero. The x87 branch senses around that divide are the part of this body a
// plausible-but-wrong translation gets wrong, and they are testable here in microseconds.
// "was one of the recorded worker orders exactly this?" -- the roster walk issues one per building
// and the ORDER between buildings is not what these cases are about, so they assert membership.
bool has_worker_order(uint16_t bldg_idx, bool unassign, uint32_t count) {
    for (const auto &o : g_rec.worker_orders)
        if (o.bldg_idx == bldg_idx && o.unassign == unassign && o.worker_count == count) return true;
    return false;
}

void test_rebalance_building_workers() {
    using namespace mh::ai;
    fixture        f;
    const uint32_t P = 2;

    // Slot 1: a LOCKED building (state 0x64) contributing builder_count.
    // Slot 2: a worker-demand building contributing worker_count.
    auto setup = [&]() {
        f.reset();
        g_rec.clear();
        f.pop[P].pop_total      = 1000;
        f.pop[P].human_in_field = 200;
        f.pop[P].housing_prev   = 900;
        f.b(P, 0).index         = 2;
        f.b(P, 1).building_id   = 5;
        f.b(P, 1).state         = 0x64;
        // Seeded AT its want (builder_count, since the default utilisation is non-zero) so the
        // LOCKED building issues NO order of its own and the worker-compare cases below isolate slot
        // 2. The cases that are ABOUT the locked branch override this.
        f.b(P, 1).current_workers = 100;
        f.b(P, 2).building_id     = 6;
        f.b(P, 2).state           = 4;
        f.b(P, 2).built_flags     = 2; // already staffed -> no activate order
        // ADJACENT cfg columns, given DIFFERENT values so a translation that swapped them fails.
        f.cfg_buildings[5].builder_count = 100;
        f.cfg_buildings[5].worker_count  = 7;
        f.cfg_buildings[6].builder_count = 9;
        f.cfg_buildings[6].worker_count  = 300;
        g_rec.uses_workers_ans[2]        = 1;
        g_rec.worker_priority_ans[2]     = 1;
    };
    auto run = [&]() {
        return detail::rebalance_building_workers(f.view(), f.store(), stub_calls(), P);
    };

    // -- PHASE 0: the reserve. floor((1000 - 200) * 0.2) = 160, above the 50 floor.
    // -- PHASE 2: util = (1000 - 200 - 160) / (300 + 100) = 640 / 400 = 1.6 -> clamped to 1.0.
    setup();
    run();
    ck(f.players[P].ai_labor_utilization == 1.0,
       "rebalance: a ratio above 1 is clamped to exactly 1.0 -- the second clamp writes the two "
       "halves 0 / 0x3ff00000");

    // The reserve FLOOR bites when the ratio-derived figure is smaller.
    setup();
    f.pop[P].pop_total      = 100;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 90;
    // floor(100 * 0.2) = 20 -> below unemployed_min 50, so the reserve is 50.
    // util = (100 - 0 - 50) / 400 = 0.125, which is BELOW max_fuck_ratio 0.3 -> flushed to 0.
    run();
    ck(f.players[P].ai_labor_utilization == 0.0,
       "rebalance: a ratio below fMaxFuckRatio is flushed to 0.0 -- the key is a FLOOR despite its "
       "name, and the reserve took the nUnemployedMin floor to get there");
    setup();
    f.pop[P].pop_total      = 100;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 90;
    f.unemployed_min        = 0; // reserve becomes floor(20) = 20; util = 80 / 400 = 0.2, still < 0.3
    run();
    ck(f.players[P].ai_labor_utilization == 0.0,
       "rebalance CONTROL: with the floor removed the reserve really is floor(pop * ratio) -- 0.2 is "
       "still below the flush threshold, so this pins the reserve arithmetic, not the clamp");
    setup();
    f.pop[P].pop_total      = 100;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 90;
    f.unemployed_min        = 0;
    f.max_fuck_ratio        = 0.1f; // now 0.2 survives the flush
    run();
    ck(f.players[P].ai_labor_utilization > 0.19 && f.players[P].ai_labor_utilization < 0.21,
       "rebalance CONTROL: lowering fMaxFuckRatio below the ratio lets it through unmodified -- the "
       "comparison is strictly `key > ratio`");

    // -- THE RESERVE IS FORCED TO ZERO when pop_total == housing_prev.
    //
    // The denominator is widened to 700 ON PURPOSE so the two readings land on OPPOSITE sides of the
    // 1.0 clamp: with the reserve zeroed the ratio is 800/700 = 1.143 -> clamped to 1.0, and without
    // it 640/700 = 0.914 -> left alone. At the default denominator of 400 BOTH readings clamp and the
    // assertion could not have failed for the reason it names.
    setup();
    f.cfg_buildings[6].worker_count = 600; // 600 demand + 100 locked = 700
    f.pop[P].pop_total              = 1000;
    f.pop[P].human_in_field         = 200;
    f.pop[P].housing_prev           = 1000; // == pop_total
    f.max_fuck_ratio                = 0.0f;
    run();
    ck(f.players[P].ai_labor_utilization == 1.0,
       "rebalance: pop_total == housing_prev zeroes the reserve -- 800/700 clamps to 1.0, where the "
       "un-zeroed 640/700 = 0.914 would not have");
    setup();
    f.cfg_buildings[6].worker_count = 600;
    f.pop[P].pop_total              = 1000;
    f.pop[P].human_in_field         = 200;
    f.pop[P].housing_prev           = 999; // one off pop_total: the rule does NOT fire
    f.max_fuck_ratio                = 0.0f;
    run();
    ck(f.players[P].ai_labor_utilization > 0.91 && f.players[P].ai_labor_utilization < 0.92,
       "rebalance CONTROL: one off pop_total and the reserve survives -- 640/700 = 0.914. This is "
       "the reading the case above had to be able to distinguish");

    // -- THE UNGUARDED DIVIDE, all three infinities, and the x87 branch senses that catch them.
    setup();
    f.b(P, 0).index         = 0; // an empty roster: both accumulators stay 0
    f.pop[P].pop_total      = 1000;
    f.pop[P].human_in_field = 200;
    f.pop[P].housing_prev   = 900;
    run(); // reserve 160, avail 640, denominator 0 -> +inf
    ck(f.players[P].ai_labor_utilization == 1.0,
       "rebalance: a POSITIVE numerator over an empty roster is +inf, which the fMaxFuckRatio flush "
       "does NOT catch (0.3 > inf is false) and the 1.0 clamp DOES");
    setup();
    f.b(P, 0).index         = 0;
    f.unemployed_min        = 0; // so the reserve is floor(0 * 0.2) = 0 and the numerator is exactly 0
    f.pop[P].pop_total      = 0;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 7; // != pop_total, so the reserve is not zeroed by that rule either
    run();
    ck(f.players[P].ai_labor_utilization == 1.0,
       "rebalance: 0 / 0 is a NaN, and BOTH x87 compares are then UNORDERED -- JBE is taken (no "
       "flush) and JNC is not (the 1.0 clamp fires). A translation written with C++ relationals "
       "would leave the NaN in place, because every C++ relational on a NaN is false");
    setup();
    f.b(P, 0).index         = 0;
    f.pop[P].pop_total      = 0;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 7; // reserve takes the 50 floor -> numerator -50 -> -inf
    run();
    ck(f.players[P].ai_labor_utilization == 0.0,
       "rebalance: a NEGATIVE numerator over an empty roster is -inf, which the flush DOES catch "
       "(0.3 > -inf) -- so the three divide-by-zero cases end at 1.0, 1.0 and 0.0 respectively");

    // -- PHASE 3: the three-way worker compare.
    setup();
    f.b(P, 2).current_workers = 0;
    run();
    // util clamps to 1.0, so want = floor(worker_count * 1.0) = 300.
    ck(g_rec.worker_orders.size() == 1 && !g_rec.worker_orders[0].unassign &&
           g_rec.worker_orders[0].bldg_idx == 2 && g_rec.worker_orders[0].worker_count == 300,
       "rebalance: cur < want issues ASSIGN for exactly the shortfall");
    setup();
    f.b(P, 2).current_workers = 300;
    run();
    ck(g_rec.worker_orders.empty(),
       "rebalance: cur == want issues NOTHING -- the JGE/JLE pair reading one CMP is a three-way "
       "branch, not two two-way ones");
    setup();
    f.b(P, 2).current_workers = 350;
    run();
    ck(g_rec.worker_orders.size() == 1 && g_rec.worker_orders[0].unassign &&
           g_rec.worker_orders[0].worker_count == 50,
       "rebalance: cur > want issues UNASSIGN for exactly the surplus");

    // -- THE LOCKED BRANCH takes builder_count, and it is gated on the utilisation being NON-ZERO.
    setup();
    f.b(P, 1).current_workers = 0;
    run();
    ck(g_rec.worker_orders.size() == 2 && has_worker_order(1, false, 100),
       "rebalance: a LOCKED building (state 0x64) wants cfg builder_count (100), not worker_count "
       "(7) -- the two cfg columns are adjacent and swapping them is the likely error");
    setup();
    f.b(P, 1).current_workers = 0;
    f.b(P, 0).index           = 1;   // only the locked building; demand 0, locked 100
    f.pop[P].pop_total        = 100; // util = floor: (100 - 0 - 50) / 100 = 0.5
    f.pop[P].human_in_field   = 0;
    f.pop[P].housing_prev     = 90;
    run();
    ck(f.players[P].ai_labor_utilization > 0.49 && f.players[P].ai_labor_utilization < 0.51 &&
           g_rec.worker_orders.size() == 1 && g_rec.worker_orders[0].worker_count == 100,
       "rebalance: the LOCKED branch's want is builder_count UNSCALED -- the utilisation ratio "
       "multiplies only the worker-demand branch");
    setup();
    f.b(P, 1).current_workers = 40;
    f.b(P, 0).index           = 1;
    f.pop[P].pop_total        = 100;
    f.pop[P].human_in_field   = 0;
    f.pop[P].housing_prev     = 90;
    f.max_fuck_ratio          = 0.9f; // 0.5 < 0.9 -> util flushed to 0.0
    run();
    ck(f.players[P].ai_labor_utilization == 0.0 && g_rec.worker_orders.size() == 1 &&
           g_rec.worker_orders[0].unassign && g_rec.worker_orders[0].worker_count == 40,
       "rebalance: with the utilisation flushed to zero a LOCKED building wants ZERO workers, so "
       "its 40 are unassigned -- the +-0.0 test at 0x004e3ee1 is what routes this");

    // -- THE ACTIVATE ORDER, and the two conditions that suppress it.
    setup();
    f.b(P, 2).built_flags = 0; // not staffed, and state 4 is none of {0x64, 0x6b, 0x82}
    run();
    ck(g_rec.activate_orders.size() == 1 && g_rec.activate_orders[0].building_index == 2,
       "rebalance: an unstaffed building in an ordinary state gets an ACTIVATE order");
    setup();
    f.b(P, 2).built_flags = 2;
    run();
    ck(g_rec.activate_orders.empty(),
       "rebalance: built_flags bit 0x2 (staffed) suppresses the activate order");
    setup();
    f.b(P, 2).built_flags = 0;
    f.b(P, 2).state       = 0x6b;
    run();
    ck(g_rec.activate_orders.empty(),
       "rebalance: a building in state 0x64 / 0x6b / 0x82 gets no activate order however unstaffed");

    // -- THE TWO PREDICATES GATE THE DEMAND BRANCH, and both must pass.
    setup();
    g_rec.uses_workers_ans[2] = 0;
    run();
    ck(g_rec.worker_orders.empty() && g_rec.worker_priority_queries.empty(),
       "rebalance: bldg_uses_workers == 0 skips the building entirely -- and short-circuits before "
       "is_worker_priority_candidate is even asked");
    setup();
    g_rec.worker_priority_ans[2] = 0;
    f.b(P, 2).current_workers    = 5;
    run();
    ck(g_rec.worker_orders.size() == 1 && g_rec.worker_orders[0].unassign &&
           g_rec.worker_orders[0].worker_count == 5,
       "rebalance: a NON-priority building still reaches the worker compare, with want = 0 -- the "
       "predicate's own zero return IS the want, so its workers are unassigned");

    // -- PHASE 4: the return value is a housing DECISION.
    setup(); // util clamps to 1.0
    ck(run() == 0,
       "rebalance: 1.0 <= ai_labor_utilization returns 0 before the housing test is reached");
    setup();
    f.pop[P].pop_total      = 100;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 90; // 0.9 ratio, below fExtraSpace 1.1 -> housing IS short
    f.unemployed_min        = 0;
    f.max_fuck_ratio        = 0.0f; // util = 80/400 = 0.2, so the early return does not fire
    ck(run() == 1,
       "rebalance: housing_prev / pop_total below fExtraSpace returns 1 -- `housing is short`");
    setup();
    f.pop[P].pop_total      = 100;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 200; // ratio 2.0, above 1.1
    f.unemployed_min        = 0;
    f.max_fuck_ratio        = 0.0f;
    ck(run() == 0, "rebalance: plenty of housing returns 0");
    // THE BOUNDARY, and it is the only case that separates JA from JAE. Exactly-equal is NOT short:
    // the original's JA @0x004e40a5 needs CF=0 AND ZF=0, so `(sw & 0x4100) == 0` and not
    // `(sw & 0x0100) == 0`. Mutation 16 of the campaign is that one-bit change and nothing else in
    // this test caught it.
    setup();
    f.pop[P].pop_total      = 100;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 100; // ratio EXACTLY 1.0, and 100/100 is exact in binary
    f.extra_space           = 1.0f;
    f.max_fuck_ratio        = 0.0f;
    ck(run() == 0,
       "rebalance: fExtraSpace EXACTLY equal to housing_prev / pop_total is NOT short -- the "
       "comparison is strictly greater (JA, not JAE)");
    setup();
    f.pop[P].pop_total      = 0;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 0;
    f.max_fuck_ratio        = 0.0f;
    // The roster is deliberately LEFT POPULATED here. With an empty one the denominator is 0 too, so
    // Phase 2's 0/0 NaN gets clamped to exactly 1.0 and Phase 4's FIRST test returns 0 before this
    // branch is ever reached -- the assertion would then have been checking a path it never took.
    ck(run() == 1,
       "rebalance: pop_total == 0 AND housing_prev == 0 returns 1 without dividing -- the special "
       "case that exists precisely because the ratio would be 0/0");
    setup();
    f.pop[P].pop_total      = 0;
    f.pop[P].human_in_field = 0;
    f.pop[P].housing_prev   = 5; // pop_total 0 but housing non-zero -- so the reserve is NOT zeroed
    f.max_fuck_ratio        = 0.0f;
    ck(run() == 0,
       "rebalance: pop_total == 0 with housing_prev != 0 returns 0 -- the second guard, and it is "
       "the OPPOSITE answer from the first, so a body that merged the two is caught here");
}


// ---- batch B layer 4a: the two build-queue membership predicates ---------------------------------
//
// llm_strat_ai_bldg_type_queue_has_pending @0x004d3a27 and llm_strat_ai_bldg_type_already_queued
// @0x004d3a93. NEITHER IS SHADOWABLE for state -- the matrix measures zero write cells, direct and
// transitive -- so the shadow site can only compare the return, and everything below is the real
// branch coverage.
//
// THE ONE CASE THAT MATTERS is the third: a queue entry whose id and whose cfg TYPE are different
// numbers, queried with each. It is the only input that separates the two bodies, and with the usual
// fixture habit of `building_id == type` a swapped pair passes every other assertion here.
void test_queue_type_query() {
    using namespace mh::ai;
    fixture f;

    const int32_t P = 4;
    // Entry ids and their cfg classes deliberately DISJOINT as number sets: ids 30/31, classes 5/6.
    f.cfg_buildings[30].type = 5;
    f.cfg_buildings[31].type = 6;

    auto entry = [&](int i) -> auto & { return f.players[P].ai_bldg_queue[i]; };

    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 0 &&
           detail::bldg_type_already_queued(f.view(), P, 30) == 0,
       "queue_type_query: an EMPTY queue matches nothing -- count 0 is the loop bound, not a "
       "sentinel");

    f.players[P].ai_bldg_queue_count = 1;
    entry(0).status                  = 1; // nibble 1 = construction
    entry(0).tick_or_unit_id         = 30;

    ck(detail::bldg_type_already_queued(f.view(), P, 30) == 1,
       "already_queued: matches the entry's OWN building id");
    ck(detail::bldg_type_already_queued(f.view(), P, 5) == 0,
       "already_queued: does NOT match the cfg TYPE of that id -- this is the whole difference from "
       "its sibling, and it is the assertion a swapped pair fails");
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 1,
       "queue_has_pending: matches Building[entry id].type, i.e. the E_BUILDING class");
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 30) == 0,
       "queue_has_pending: does NOT match the raw id -- the other half of the same separation");

    // The status nibble. 0x21 = construction with the affordability waiver set, which must still
    // match; 0x81 = construction already committed, which must ALSO still match, because the test
    // is on the low nibble alone. 2 (empty) and 4 (upgrade) must not.
    entry(0).status = 0x21;
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 1 &&
           detail::bldg_type_already_queued(f.view(), P, 30) == 1,
       "queue_type_query: the HIGH bits of status are ignored -- 0x21 (waived) still matches");
    entry(0).status = 0x81;
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 1 &&
           detail::bldg_type_already_queued(f.view(), P, 30) == 1,
       "queue_type_query: 0x81 (committed) still matches -- AND 0xf, not a whole-byte compare");
    entry(0).status = 4;
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 0 &&
           detail::bldg_type_already_queued(f.view(), P, 30) == 0,
       "queue_type_query: nibble 4 (upgrade/cancel) does not match -- only nibble 1 does");
    entry(0).status = 0;
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 0 &&
           detail::bldg_type_already_queued(f.view(), P, 30) == 0,
       "queue_type_query: nibble 0 (train/recruit) does not match either");

    // Several entries, with the match at the END: a body that returned after the first entry, or
    // that used the wrong entry stride, stops here.
    f.players[P].ai_bldg_queue_count = 3;
    entry(0).status                  = 1;
    entry(0).tick_or_unit_id         = 31; // class 6
    entry(1).status                  = 4;
    entry(1).tick_or_unit_id         = 30; // right id, WRONG nibble
    entry(2).status                  = 1;
    entry(2).tick_or_unit_id         = 30; // the real match, last
    ck(detail::bldg_type_already_queued(f.view(), P, 30) == 1,
       "already_queued: keeps walking past a right-id/wrong-nibble entry to a later real match");
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 5) == 1,
       "queue_has_pending: same, through the cfg hop");
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 6) == 1,
       "queue_has_pending: and the FIRST entry's class matches too, so the walk really is a scan");

    // The count is the bound and nothing else: an entry past it is invisible however well it matches.
    f.players[P].ai_bldg_queue_count = 2;
    ck(detail::bldg_type_already_queued(f.view(), P, 30) == 0,
       "already_queued: entries at or past ai_bldg_queue_count are not scanned");

    // The argument is a FULL dword compared against a zero-extended byte, so 0x100 | id can never
    // match. Mutation target: a body that truncated the argument to a byte would return 1 here.
    f.players[P].ai_bldg_queue_count = 1;
    entry(0).status                  = 1;
    entry(0).tick_or_unit_id         = 30;
    ck(detail::bldg_type_already_queued(f.view(), P, 0x100u | 30u) == 0,
       "already_queued: the match key is zero-extended from a byte and compared against the WHOLE "
       "argument, so 0x11e never matches id 30");
    ck(detail::bldg_type_queue_has_pending(f.view(), P, 0x100u | 5u) == 0,
       "queue_has_pending: same width rule on the cfg type");

    // The row is the player's own.
    ck(detail::bldg_type_already_queued(f.view(), P + 1, 30) == 0,
       "already_queued: reads the queried player's queue, not a neighbour's");
}

// ---- batch B layer 4b: promoting the newest queue entry to the front ----------------------------
//
// llm_strat_ai_queue_rotate_newest_to_front @0x004e2cac. The rig reaches it (three planners call it
// right after enqueuing), but only ever with whatever queue depth the scenario produced -- so the
// boundary cases and the whole-entry copy live here.
void test_queue_rotate() {
    using namespace mh::ai;
    fixture f;

    const int32_t P     = 2;
    auto          entry = [&](int i) -> auto          &{ return f.players[P].ai_bldg_queue[i]; };
    // Fill every one of the 18 bytes with a per-entry pattern, so a copy that moved only the first
    // dword -- or the 16 bytes a MOVSD.REP-only reading would give -- is visible.
    auto stamp = [&](int i, uint8_t tag) {
        entry(i).status          = tag;
        entry(i).tick_or_unit_id = (uint8_t)(tag + 1);
        entry(i).build_tile_x    = (int16_t)(tag * 0x101);
        for (int r = 0; r < 5; ++r) entry(i).resource_reserved[r] = (int16_t)(tag * 0x100 + r);
        entry(i).building_index = (int32_t)(tag * 0x10001);
    };
    auto same = [&](int i, uint8_t tag) {
        if (entry(i).status != tag || entry(i).tick_or_unit_id != (uint8_t)(tag + 1)) return false;
        if (entry(i).build_tile_x != (int16_t)(tag * 0x101)) return false;
        for (int r = 0; r < 5; ++r)
            if (entry(i).resource_reserved[r] != (int16_t)(tag * 0x100 + r)) return false;
        return entry(i).building_index == (int32_t)(tag * 0x10001);
    };

    // count 0 and count 1: the unsigned early-out, and it must not touch anything.
    stamp(0, 0x11);
    f.players[P].ai_bldg_queue_count = 0;
    detail::queue_rotate_newest_to_front(f.store(), P);
    ck(same(0, 0x11) && f.players[P].ai_bldg_queue_count == 0,
       "queue_rotate: count 0 is a no-op");
    f.players[P].ai_bldg_queue_count = 1;
    detail::queue_rotate_newest_to_front(f.store(), P);
    ck(same(0, 0x11) && f.players[P].ai_bldg_queue_count == 1,
       "queue_rotate: count 1 is a no-op -- the guard is <= 1, not < 1");

    // count 2: the smallest real rotation, i.e. a swap.
    stamp(0, 0x21);
    stamp(1, 0x22);
    f.players[P].ai_bldg_queue_count = 2;
    detail::queue_rotate_newest_to_front(f.store(), P);
    ck(same(0, 0x22) && same(1, 0x21),
       "queue_rotate: count 2 swaps, and BOTH entries survive whole -- all 18 bytes, not just the "
       "first dword");
    ck(f.players[P].ai_bldg_queue_count == 2,
       "queue_rotate: the count is NOT decremented -- this is a rotate, not a pop");

    // count 4 with a distinct tag per slot: the full shift, and the direction.
    for (int i = 0; i < 4; ++i) stamp(i, (uint8_t)(0x30 + i));
    stamp(4, 0x99); // one past the count -- must be left alone
    f.players[P].ai_bldg_queue_count = 4;
    detail::queue_rotate_newest_to_front(f.store(), P);
    ck(same(0, 0x33) && same(1, 0x30) && same(2, 0x31) && same(3, 0x32),
       "queue_rotate: the last entry goes to slot 0 and the rest shift UP by one -- a walk in the "
       "wrong direction smears slot 0 across the whole row instead");
    ck(same(4, 0x99),
       "queue_rotate: nothing past ai_bldg_queue_count is touched");

    // The row is the player's own.
    stamp(0, 0x41);
    stamp(1, 0x42);
    f.players[P].ai_bldg_queue_count     = 2;
    f.players[P + 1].ai_bldg_queue_count = 2;
    f.players[P + 1].ai_bldg_queue[0]    = f.players[P].ai_bldg_queue[0];
    f.players[P + 1].ai_bldg_queue[1]    = f.players[P].ai_bldg_queue[1];
    detail::queue_rotate_newest_to_front(f.store(), P);
    ck(f.players[P + 1].ai_bldg_queue[0].status == 0x41,
       "queue_rotate: rotates only the queried player's row");
}

// ---- batch B layer 4c: the nearest CONNECTED building ------------------------------------------
//
// llm_strat_ai_find_nearest_flagged_building @0x004e4e54. Not shadowable for state either, so the
// branch evidence is here -- and the branch that matters is the RETURN GATE, which reads the LAST
// evaluated candidate's distance rather than the winner's. That behaviour is unreachable by
// inspection of any single scenario and is exactly what a "sensible" reimplementation would fix.
void test_nearest_flagged() {
    using namespace mh::ai;
    fixture f;

    const int32_t P = 1;
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;

    auto place = [&](int i, uint8_t x, uint8_t y, uint16_t id, uint8_t flags) {
        f.b(P, i).x           = x;
        f.b(P, i).y           = y;
        f.b(P, i).building_id = (uint16_t)id;
        f.b(P, i).built_flags = flags;
    };
    auto count = [&](int n) { f.b(P, 0).index = (int16_t)n; };

    // No live buildings at all: the walk evaluates nothing, and BOTH branches of the gate return -1
    // in the original because the index slot still holds its -1. This is the case whose C++ stands in
    // for an uninitialised stack read; if it ever returned anything else the substitution is wrong.
    count(0);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == -1,
       "nearest_flagged: an empty roster returns -1 -- the case that stands in for the original's "
       "uninitialised last-distance slot");

    // One flagged building far away (dx 20 -> 400 > 0xf): found.
    place(1, 30, 10, 7, 1);
    count(1);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == 1,
       "nearest_flagged: a single flagged building beyond the 0xf gate is returned");

    // Same building, flag CLEAR: skipped, nothing evaluated, -1.
    place(1, 30, 10, 7, 0);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == -1,
       "nearest_flagged: built_flags bit 0x1 is required -- an unflagged building is not a candidate");
    place(1, 30, 10, 7, 2); // bit 0x2 (staffed) alone is not the bit
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == -1,
       "nearest_flagged: it is bit 0x1 specifically, not any nonzero built_flags");

    // Distance 0 -- the query point IS the building -- is rejected, which is how the caller avoids
    // pairing a building with itself.
    //
    // IT TAKES A SECOND BUILDING TO SEE THIS, and the single-building version of the case was the
    // mutation campaign's instrument error rather than a finding: with only the zero-distance
    // building present, a body that ACCEPTED it still answers -1, because the last-candidate gate
    // then sees a distance of 0 and throws the winner away. The two readings agree, so the case
    // proves nothing. With a far building AFTER it the gate passes on 400 and the two readings
    // disagree: the original answers 2, an accepting body answers 1.
    place(1, 10, 10, 7, 1); // dist 0, evaluated first
    place(2, 30, 10, 7, 1); // dist 400, evaluated second
    count(2);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == 2,
       "nearest_flagged: a zero distance is skipped, not accepted as the nearest -- with a farther "
       "building after it, so the gate cannot hide the difference");

    // THE GATE. Two flagged buildings: a good one at distance 400 (slot 1) and a NEAR one at
    // distance 4 (slot 2). The winner is slot 1, but the LAST evaluated candidate is slot 2 at 4,
    // which is <= 0xf -- so the original throws the winner away and answers -1.
    place(1, 30, 10, 7, 1); // dist 400
    place(2, 12, 10, 7, 1); // dist 4, evaluated second
    count(2);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == -1,
       "nearest_flagged: THE LAST-CANDIDATE GATE -- a valid nearest match is discarded because the "
       "LAST building examined was within 0xf. A body that gated on the best distance returns 1");
    // Reverse the roster order and the same two buildings now answer 2: the near one is evaluated
    // FIRST, so the last distance is 400 and the gate passes -- and the winner is still the near one.
    place(1, 12, 10, 7, 1); // dist 4, evaluated first, and it WINS
    place(2, 30, 10, 7, 1); // dist 400, evaluated last, so the gate sees 400
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == 1,
       "nearest_flagged: the SAME two buildings in the other roster order answer 1 -- which is the "
       "proof that the gate reads the last candidate and not the winner");

    // Ties keep the FIRST candidate (the compare is a >=-skip).
    place(1, 30, 10, 7, 1);
    place(2, 10, 30, 7, 1); // same squared distance, 400
    count(2);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == 1,
       "nearest_flagged: an equal distance does NOT replace the winner -- first wins");

    // An empty slot does not consume the live-count budget. Slot 1 is a hole, so with a budget of 1
    // the walk must continue to slot 2 and find it. A body that decremented on the hole answers -1.
    place(1, 0, 0, 0, 1); // building_id 0 = hole (flag set, to make the wrong body's answer wrong)
    place(2, 30, 10, 7, 1);
    count(1);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == 2,
       "nearest_flagged: a building_id == 0 hole does not consume the live-count budget");
    // ... whereas a live-but-unflagged building DOES consume it: with the same budget of 1 and slot
    // 1 live-but-unflagged, the walk stops before slot 2.
    place(1, 20, 10, 7, 0);
    ck(detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 10, 10) == -1,
       "nearest_flagged: a LIVE but unflagged building DOES consume the budget -- the two skips are "
       "not the same skip");

    // The row is the player's own, and the arguments reach the distance helper in the documented
    // order (query point first, building second).
    g_rec.clear();
    g_rec.map_w = f.map_w;
    g_rec.map_h = f.map_h;
    place(1, 30, 40, 7, 1);
    count(1);
    detail::find_nearest_flagged_building(f.view(), stub_calls(), P, 11, 12);
    ck(g_rec.toroidals.size() == 1 && g_rec.toroidals[0].x1 == 11 && g_rec.toroidals[0].y1 == 12 &&
           g_rec.toroidals[0].x2 == 30 && g_rec.toroidals[0].y2 == 40,
       "nearest_flagged: toroidal_dist_sq is called (query x, query y, building x, building y)");
}

// ---- batch B layer 4d: the site-scanner dispatch ------------------------------------------------
//
// llm_strat_ai_bldg_production_type_dispatch @0x004e7cec. The rig can reach this, but only along
// whichever branch the loaded base happens to build -- and its RESET is the thing most easily lost in
// translation, because the plate credited it to the callees until 2026-08-03.
void test_site_dispatch() {
    using namespace mh::ai;
    fixture f;

    const uint32_t P   = 3;
    auto           run = [&](int32_t bldg_id) {
        g_rec.clear();
        g_rec.site_count_cell = &f.site_count;
        detail::bldg_production_type_dispatch(f.view(), f.store(), stub_calls(), P, bldg_id);
    };

    // THE RESET. Seed the count non-zero and give the type a branch whose stub appends exactly one:
    // a body that dropped the store leaves 8, a body that kept it leaves 1. The stubs append rather
    // than store precisely so this assertion can tell the two apart.
    f.players[P].is_alien_race = 0;
    f.cfg_buildings[40].type   = 99; // no pair matches -> the grid scanner
    f.site_count               = 7;
    run(40);
    ck(f.site_count == 1,
       "site_dispatch: the candidate count is RESET by this body before the scanner appends -- with "
       "the store dropped it would be 8, since the scanners only ever INC");

    // The three human branches.
    f.players[P].is_alien_race = 0;
    f.cfg_buildings[41].type   = BLDG_TYPE_H_MINE;
    run(41);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 1 && g_rec.site_sorts == 1,
       "site_dispatch: H_MINE routes to the RESOURCE-site scanner, then sorts");
    f.cfg_buildings[42].type = BLDG_TYPE_H_GARAGE;
    run(42);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 2 && g_rec.site_sorts == 1,
       "site_dispatch: H_GARAGE routes to the BUILD-site scanner");
    f.cfg_buildings[43].type = BLDG_TYPE_H_BARRACKS;
    run(43);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 2,
       "site_dispatch: H_BARRACKS routes to the SAME build-site scanner -- both branches converge");
    f.cfg_buildings[44].type = BLDG_TYPE_H_TURRET;
    run(44);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 3,
       "site_dispatch: anything else falls through to the GRID scanner");

    // The alien half, with the SAME cfg ids: the race flag alone must move every branch. The human
    // constants must now MISS, which is what catches a body that hardcoded one race's numbers.
    f.players[P].is_alien_race = 1;
    f.cfg_buildings[45].type   = BLDG_TYPE_A_MINE;
    run(45);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 1,
       "site_dispatch: A_MINE routes to the resource-site scanner for an alien player");
    f.cfg_buildings[46].type = BLDG_TYPE_A_GARAGE;
    run(46);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 2,
       "site_dispatch: A_GARAGE routes to the build-site scanner");
    f.cfg_buildings[47].type = BLDG_TYPE_A_BARRACKS;
    run(47);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 2,
       "site_dispatch: A_BARRACKS routes to the build-site scanner");
    run(41); // H_MINE, but the player is alien now
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].kind == 3,
       "site_dispatch: H_MINE on an ALIEN player matches nothing and falls to the grid scanner -- "
       "the race flag selects the constant, it does not merely add a second one");

    // The arguments each scanner receives. The second one is the dispatcher's own building_id, which
    // in the original arrives implicitly in EDX -- so a translation that passed anything else (the
    // type, say) is caught here and nowhere else.
    f.players[P].is_alien_race = 0;
    run(41);
    ck(g_rec.site_scans.size() == 1 && g_rec.site_scans[0].player == (long)P &&
           g_rec.site_scans[0].building == 41,
       "site_dispatch: the scanner is handed (player, building_id) -- the ID, not the cfg type");

    // The sort runs on every path, including the default one.
    f.cfg_buildings[48].type = 200;
    run(48);
    ck(g_rec.site_sorts == 1,
       "site_dispatch: the sort follows every branch, the grid default included");
}

// ---- batch B layer 4: the storage-launch adapter -------------------------------------------------
//
// Twenty-two bytes, no branch, no state -- so the offline test IS the whole specification of what
// can go wrong: the nibble mask, and the order of the three pass-through arguments.
//
// THIS IS THE ONLY PLACE THE MASK IS EXERCISED AT ALL. Every live call site passes a player index in
// 0..7, so `player & 0xf` is an identity at runtime and the rig can never distinguish a body that
// applies it from one that does not. That half of the evidence has to be here or nowhere.
void test_launch_storage() {
    using namespace mh::ai;

    // The mask. 0..15 must pass through untouched -- the nibble is four bits, not three -- and
    // anything above must wrap.
    for (uint32_t p = 0; p < 16u; ++p)
        ck(detail::launch_player_nibble((uint8_t)p) == p,
           "launch_storage: the low nibble passes through unchanged for every player 0..15");
    ck(detail::launch_player_nibble(0x13) == 3,
       "launch_storage: AND AL,0xf keeps only the low four bits -- 0x13 forwards as 3");
    ck(detail::launch_player_nibble(0xff) == 0x0f,
       "launch_storage: 0xff forwards as 0xf, not as 0xff -- a body that dropped the AND would "
       "hand the order path a player index of 255");
    ck(detail::launch_player_nibble(0x80) == 0,
       "launch_storage: the high bit is masked away too, so 0x80 forwards as player 0");

    // The forwarding. Four DISTINCT values, none a plausible coincidence of another, so a swapped
    // pair cannot pass. x and y are the likely transposition: they arrive in EBX and ECX and the
    // callee takes them in that order.
    g_rec.clear();
    detail::unit_launch_from_storage_enqueue(stub_calls(), (uint8_t)5, 37, 111u, 222u);
    ck(g_rec.launches.size() == 1,
       "launch_storage: the adapter tail-calls the order enqueue exactly once");
    ck(at(g_rec.launches, 0).player == 5u && at(g_rec.launches, 0).unit_id == 37 &&
           at(g_rec.launches, 0).x == 111u && at(g_rec.launches, 0).y == 222u,
       "launch_storage: (player & 0xf, unit_id, target_x, target_y) reach the callee in that order "
       "-- EAX/EDX/EBX/ECX, with only the first transformed");

    // The masked player is what the CALLEE sees, not merely what a local holds. A separate assertion
    // from the mask test above because a body could compute the nibble and forward the raw byte.
    g_rec.clear();
    detail::unit_launch_from_storage_enqueue(stub_calls(), (uint8_t)0x27, 1, 2u, 3u);
    ck(at(g_rec.launches, 0).player == 7u,
       "launch_storage: the MASKED player is the one forwarded, not the raw argument byte");
}


// ---- batch B layer 5: the per-tile mine yield estimator ------------------------------------------
//
// llm_strat_ai_calc_mine_yield_estimate. The three things worth separating here are (a) the
// truncation is toward ZERO and not round-to-nearest, (b) the kernel walk stops at the FIRST hit so
// the nearest ring wins, and (c) out_yield[0] is the CALLER's and is never zeroed.
void test_calc_mine_yield() {
    using namespace mh::ai;
    fixture       f;
    const int32_t T = 11; // a cfg Building TYPE id

    // map 64 x 48 -> coarse 16 x 12. Rectangular on purpose: a body that swapped the two moduli
    // would still wrap "correctly" on a square map.
    int32_t      out[8]{};
    mine_quality q{};
    auto         setup = [&](int32_t val) {
        f.reset();
        f.cfg_buildings[T]                = cfg_building{};
        f.cfg_buildings[T].extract_id[0]  = 2; // a TERMINATED list: [1] stays 0
        f.cfg_buildings[T].extract_val[0] = val;
        for (int i = 0; i < 8; ++i) out[i] = 0;
        q = mine_quality{7, 7, 7}; // poison, so "zeroed by the callee" is observable
    };
    // resources[cx][cy].value[r], with the plane's real 64-wide row stride.
    auto cell = [&](int32_t cx, int32_t cy) -> map_resources & { return f.res_plane[cx * 64 + cy]; };
    auto run  = [&](uint32_t tx, uint32_t ty) {
        return detail::calc_mine_yield_estimate(f.view(), T, tx, ty, out, &q);
    };

    // -- nothing anywhere in the 5x5 window.
    setup(33);
    out[0]                      = 777; // the caller's carry
    detail::mine_yield_report r = run(20, 20);
    ck(out[1] == 0 && out[2] == 0 && out[3] == 0 && out[4] == 0,
       "mine_yield: an empty neighbourhood leaves every per-resource estimate at zero");
    ck(q.near_count == 0 && q.mid_count == 0 && q.far_count == 0,
       "mine_yield: out_quality is ZEROED by the callee (0x004e3274-0x004e3281), so the poison is "
       "gone even when nothing is found");
    ck(out[0] == 777,
       "mine_yield: out_yield[0] is NOT zeroed -- the init loop runs 1..4 and the total pass only "
       "ADDs, so the caller's carry survives (this is the divergence a zeroing body produces on its "
       "very first call)");
    ck(r.extract_slots == 1 && r.resources_hit == 0,
       "mine_yield: the extract walk stops at the first zero id -- one slot, not four");

    // -- ring 0, and TRUNCATION toward zero. 33 * 0.5 = 16.5: round-to-nearest gives 17.
    setup(33);
    cell(5, 5).value[2] = 1;
    run(20, 20); // 20 >> 2 == 5, so this IS the centre cell
    ck(out[2] == 33 && out[0] == 33,
       "mine_yield: a deposit on the mine's OWN coarse cell scores the full extract_val (weight "
       "1.0), and out_yield[0] accumulates it");
    ck(q.near_count == 1 && q.mid_count == 0 && q.far_count == 0,
       "mine_yield: weight 1.0 lands in the ring-0 bucket (>= 0x3f7d70a4)");

    setup(33);
    cell(4, 5).value[2] = 1; // dx -1, dy 0 -> ring 1, weight 0.5
    run(20, 20);
    ck(out[2] == 16,
       "mine_yield: 33 * 0.5 TRUNCATES to 16, not 17 -- utils_math_trunc sets RC = round-toward-zero "
       "before the FRNDINT, so a body that used the default rounding is off by one here");
    ck(q.mid_count == 1 && q.near_count == 0 && q.far_count == 0,
       "mine_yield: weight 0.5 lands in the ring-1 bucket (>= 0x3eff7cee, < 0x3f7d70a4)");

    setup(33);
    cell(3, 5).value[2] = 1; // dx -2 -> ring 2, weight 0.1
    run(20, 20);
    ck(out[2] == 3,
       "mine_yield: 33 * 0.1 truncates to 3 -- the x87 product is 3.3000000000000003, so this also "
       "separates a body that rounded");
    ck(q.far_count == 1 && q.near_count == 0 && q.mid_count == 0,
       "mine_yield: weight 0.1 lands in the ring-2 bucket (>= 0x3dcac083)");

    // -- NEAREST WINS: a ring-2 deposit and a ring-1 deposit, both present.
    setup(33);
    cell(3, 5).value[2] = 1; // ring 2
    cell(4, 5).value[2] = 1; // ring 1
    run(20, 20);
    ck(out[2] == 16 && q.mid_count == 1 && q.far_count == 0,
       "mine_yield: the kernel is walked NEAREST-FIRST and the scan BREAKS at the first hit, so the "
       "ring-1 cell wins over the ring-2 one -- a body that kept scanning for a maximum, or that "
       "walked the table backwards, reports the ring-2 weight here");

    // -- THE TORUS. Mine on coarse (0,0); deposit on coarse (15, 11) is kernel offset (-1,-1).
    setup(33);
    cell(15, 11).value[2] = 1;
    run(0, 0);
    ck(out[2] == 16 && q.mid_count == 1,
       "mine_yield: the coarse coords wrap on width/4 and height/4 (16 x 12 here) -- offset (-1,-1) "
       "from cell (0,0) is cell (15,11), and a body that clamped instead of wrapping finds nothing");

    // THE WRAP IS A MODULUS, NOT AN AND-MASK, and the default fixture cannot tell: 64/4 == 16 is a
    // power of two, so `& (extent-1)` gives the same answer for every offset and the assertion above
    // passed against a masking body (mutation campaign, 2026-08-05). A 48-wide map makes the coarse
    // extent 12, where the two disagree: (5 + 2 + 12) % 12 == 7 but (5 + 2) & 11 == 3.
    setup(33);
    f.map_w             = 48; // coarse width 12, deliberately NOT a power of two
    cell(7, 5).value[2] = 1;  // dx +2 from coarse (5,5) -> ring 2
    run(20, 20);
    ck(out[2] == 3 && q.far_count == 1,
       "mine_yield: the coarse wrap is an unsigned MODULUS by width/4, which on a non-power-of-two "
       "extent is not an AND-mask -- a masking body looks at cell 3 and finds nothing here");

    // -- THE TERMINATED LIST. {2, 0, 3, 0}: resource 3 must never be looked at.
    setup(33);
    f.cfg_buildings[T].extract_id[1]  = 0;
    f.cfg_buildings[T].extract_id[2]  = 3;
    f.cfg_buildings[T].extract_val[2] = 90;
    cell(5, 5).value[2]               = 1;
    cell(5, 5).value[3]               = 1;
    r                                 = run(20, 20);
    ck(out[2] == 33 && out[3] == 0 && r.extract_slots == 1,
       "mine_yield: the extract table is a TERMINATED list, not a fixed four -- a zero id ENDS the "
       "walk (@0x004e318a) and everything after it is invisible, so resource 3 scores nothing");

    // -- two resources at different rings, and the total.
    setup(33);
    f.cfg_buildings[T].extract_id[1]  = 4;
    f.cfg_buildings[T].extract_val[1] = 10;
    cell(5, 5).value[2]               = 1; // ring 0 for id 2
    cell(3, 5).value[4]               = 1; // ring 2 for id 4
    run(20, 20);
    ck(out[2] == 33 && out[4] == 1 && out[0] == 34,
       "mine_yield: each resource gets its OWN nearest ring, and out_yield[0] is their sum "
       "(10 * 0.1 truncates to 1)");
    ck(q.near_count == 1 && q.far_count == 1 && q.mid_count == 0,
       "mine_yield: the histogram counts RESOURCES per ring, so one at ring 0 and one at ring 2 "
       "gives 1/0/1 -- a body that bucketed by cell instead would give a different shape");

    // -- x87 helper, pinned directly. The three shipped weights against a value that truncates.
    const double w1 = 1.0, w2 = 0.5, w3 = 0.1;
    ck(detail::x87_scale_and_trunc(33, &w1) == 33 && detail::x87_scale_and_trunc(33, &w2) == 16 &&
           detail::x87_scale_and_trunc(33, &w3) == 3,
       "mine_yield: x87_scale_and_trunc reproduces FILD/FMUL/trunc/FISTP for the three shipped "
       "weights");
    // THE INPUTS THAT SEPARATE TRUNCATE FROM ROUND. FISTP's default mode is round-to-nearest-EVEN,
    // so 16.5 -> 16 and 3.3 -> 3 give the SAME answer either way; the assertion above passed against
    // a body with the FRNDINT deleted (mutation campaign, 2026-08-05). 35 * 0.5 = 17.5 rounds to 18,
    // and 36 * 0.1 = 3.6 rounds to 4 -- both differ from the truncation the original performs.
    ck(detail::x87_scale_and_trunc(35, &w2) == 17,
       "mine_yield: 35 * 0.5 = 17.5 TRUNCATES to 17 -- round-to-nearest-even gives 18, so this is "
       "the input that proves utils_math_trunc's RC = round-toward-zero is reproduced");
    ck(detail::x87_scale_and_trunc(36, &w3) == 3,
       "mine_yield: 36 * 0.1 = 3.6 truncates to 3, not 4");
    ck(detail::x87_scale_and_trunc(0, &w1) == 0,
       "mine_yield: a zero extract_val scores zero at every weight");
}

// ---- batch B layer 4: the mine portfolio rebalance -----------------------------------------------
void test_mine_portfolio_rebalance() {
    using namespace mh::ai;
    fixture        f;
    const uint32_t P     = 2;
    const int32_t  MINE  = 5; // a cfg Building TYPE whose .type is BLDG_TYPE_A_MINE
    const int32_t  OTHER = 6; // a non-mine type
    int32_t        out[8]{};

    // Lay out `n` mines in roster slots 1..n and set the budget to exactly the number of occupied
    // slots, which is what buildings[player][0].index holds in the game.
    auto place = [&](int n, int extra_occupied) {
        f.reset();
        g_rec.clear();
        f.cfg_buildings[MINE]       = cfg_building{};
        f.cfg_buildings[MINE].type  = BLDG_TYPE_A_MINE;
        f.cfg_buildings[OTHER]      = cfg_building{};
        f.cfg_buildings[OTHER].type = 40; // neither 2 nor 0x16
        for (int i = 0; i < n; ++i) {
            f.b(P, 1 + i).building_id = (uint16_t)MINE;
            f.b(P, 1 + i).x           = (uint8_t)(10 + i);
            f.b(P, 1 + i).y           = (uint8_t)(20 + i);
        }
        for (int i = 0; i < extra_occupied; ++i) f.b(P, 1 + n + i).building_id = (uint16_t)OTHER;
        f.b(P, 0).index = (int16_t)(n + extra_occupied);
        f.mine_min      = 0;
        for (int i = 0; i < 8; ++i) out[i] = 0;
    };
    auto yields = [&](int call, int32_t a, int32_t b, int32_t c, int32_t d) {
        g_rec.yield_by_call[call][1] = a;
        g_rec.yield_by_call[call][2] = b;
        g_rec.yield_by_call[call][3] = c;
        g_rec.yield_by_call[call][4] = d;
    };
    auto run = [&]() {
        return detail::mine_portfolio_rebalance(f.view(), f.store(), stub_calls(), P, out);
    };

    // -- NO MINES. Five zeros and nothing else.
    place(0, 3);
    out[0]                          = 999;
    detail::mine_rebalance_report r = run();
    ck(r.collected == 0 && g_rec.yield_estimates.empty() && g_rec.restart_orders.empty(),
       "mine_rebal: a player with no mine collects nothing, estimates nothing and orders nothing");
    ck(out[0] == 0 && out[4] == 0,
       "mine_rebal: out_yield[0..4] are ALL zeroed on entry (0x004e3433, r = 0..4) -- unlike the "
       "estimator's init loop, which starts at 1");

    // -- THE BUDGET, and that an EMPTY slot does not spend it.
    place(2, 0);
    f.b(P, 2).building_id = 0;              // punch a hole between the two mines
    f.b(P, 3).building_id = (uint16_t)MINE; // the second mine now lives past the hole
    f.b(P, 3).x           = 30;
    f.b(P, 3).y           = 31;
    yields(0, 10, 0, 0, 0);
    yields(1, 20, 0, 0, 0);
    // Park at the min-count gate so these are pure COLLECT assertions. Without it this fixture is a
    // single-resource portfolio, pass 3 finds resource 2 starved (weighted total 0) and retires the
    // smaller mine -- correct behaviour that would make the roster assertions below read as bugs.
    f.mine_min = 99;
    r          = run();
    ck(r.collected == 2,
       "mine_rebal: an EMPTY roster slot does not consume the budget (the JZ at 0x004e34af skips "
       "the DEC), so both mines are found across a hole -- a body that decremented unconditionally "
       "stops after the first");
    ck(g_rec.yield_estimates.size() == 2 && at(g_rec.yield_estimates, 1).tile_x == 30 &&
           at(g_rec.yield_estimates, 1).tile_y == 31,
       "mine_rebal: the estimator is called with the BUILDING's own x/y, not the roster index");
    ck(at(g_rec.yield_estimates, 1).out_yield == (void *)&f.mine_yields[1].by_resource[0] &&
           at(g_rec.yield_estimates, 1).out_quality == (void *)&f.mine_qual[1],
       "mine_rebal: the two out-pointers are row `collected` of the two PARALLEL tables (strides 32 "
       "and 12) -- a body that used the roster SLOT as the row index writes row 3 here");
    ck(f.mine_roster[0] == 1 && f.mine_roster[1] == 3,
       "mine_rebal: the roster-index table records the SLOT, in collect order");
    ck(out[1] == 30 && out[0] == 30,
       "mine_rebal: out_yield[r] is the SUM over collected mines, and [0] is re-summed from [1..4]");

    // -- A NON-MINE occupied slot spends the budget and is not collected.
    place(1, 1);
    yields(0, 10, 0, 0, 0);
    r = run();
    ck(r.collected == 1 && g_rec.yield_estimates.size() == 1,
       "mine_rebal: an occupied NON-mine slot spends a budget unit and is skipped -- only cfg type "
       "2 / 0x16 collects");

    // -- THE MIN-COUNT GATE, and that it is the ARGUMENT-CHANNEL global.
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    f.mine_min = 3; // live == 2 < 3
    r          = run();
    ck(r.below_min_count == 1 && g_rec.restart_orders.empty() && r.retired_pass2 == 0,
       "mine_rebal: live < _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT returns before either retire "
       "pass -- the caller's store into that global is what decides this");
    ck(out[0] == 404 && r.reached_pass3 == 0,
       "mine_rebal: the min-count early exit STILL re-sums out_yield[0] (the shared exit at "
       "0x004e3853), and does not fall into pass 3");

    //    The compare is UNSIGNED (JC @0x004e35b1). A negative threshold is not a state the caller
    //    can produce today -- it stores 2*(four counters)+1 -- but signed-vs-unsigned is exactly the
    //    class of error no scenario separates, and the whole point of an offline oracle.
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    f.mine_min = -1; // 0xffffffff unsigned: live == 2 is BELOW it
    r          = run();
    ck(r.below_min_count == 1 && g_rec.restart_orders.empty(),
       "mine_rebal: the min-count compare is UNSIGNED -- a -1 threshold is 0xffffffff and gates "
       "everything off, where a signed read would sail past it and retire the runt");

    // -- THE STATE-0x6b GATE.
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    f.b(P, 2).state = BLDG_STATE_RESTART_PENDING;
    r               = run();
    ck(r.hit_state_gate == 1 && g_rec.restart_orders.empty() && out[0] == 404,
       "mine_rebal: a collected mine already in state 0x6b aborts both passes, and the exit still "
       "re-sums the total");

    // -- PASS 2 RETIRES THE UNDERPERFORMER, and only it.
    place(2, 0);
    yields(0, 1, 1, 1, 1);         // the runt
    yields(1, 100, 100, 100, 100); // the workhorse
    r = run();
    ck(r.retired_pass2 == 1 && g_rec.restart_orders.size() == 1 &&
           at(g_rec.restart_orders, 0).building_index == 1,
       "mine_rebal: the runt fails all five keep tests and is retired by roster SLOT (1), not by "
       "collect row");
    ck(at(g_rec.restart_orders, 0).player == P,
       "mine_rebal: the retire order carries the ticking player");
    ck(g_rec.train_flushes.size() == 1 && at(g_rec.train_flushes, 0) == (int32_t)P,
       "mine_rebal: every retire is followed by the pending-training flush -- a body that dropped "
       "the call leaves the AI's train tallies overcounted");
    ck(f.mine_roster[0] == 0 && f.mine_roster[1] == 2,
       "mine_rebal: the retired mine's roster slot is CLEARED to 0 and the survivor's is untouched");
    ck(out[1] == 100 && out[0] == 400,
       "mine_rebal: the retired mine's estimate is subtracted back out of out_yield[1..4] and the "
       "total re-summed");
    ck(r.reached_pass3 == 0,
       "mine_rebal: pass 3 does NOT run when pass 2 retired something -- the two are mutually "
       "exclusive (JNZ @0x004e374f)");

    // -- EACH OF THE FIVE KEEP TESTS, one at a time, on the same runt that was just retired.
    //    THE QUALITY SCORE: 10*near + 5*mid + far >= 15.
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    g_rec.qual_by_call[0][0] = 1; // near 1 -> score 10, still under 15
    r                        = run();
    ck(r.retired_pass2 == 1,
       "mine_rebal: a quality score of 10 (one ring-0 resource) is UNDER the 15 gate and does not "
       "save the mine");
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    g_rec.qual_by_call[0][0] = 1; // near 1  -> 10
    g_rec.qual_by_call[0][1] = 1; // mid 1   -> +5  = 15
    r                        = run();
    ck(r.retired_pass2 == 0 && g_rec.restart_orders.empty(),
       "mine_rebal: 10*near + 5*mid + far == 15 KEEPS the mine -- the gate is >= and the "
       "multipliers are 10 / 5 / 1, so a body that summed the three counts unweighted (score 2) "
       "retires here");
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    g_rec.qual_by_call[0][2] = 14; // far 14 -> score 14, one under
    r                        = run();
    ck(r.retired_pass2 == 1,
       "mine_rebal: score 14 is under the gate -- the boundary is exactly 15");

    //    THE SHARE TEST. Two IDENTICAL mines: each holds 50% of every resource, so the summed
    //    percentage share is 4 * 50 = 200, far over the threshold of 10.
    place(2, 0);
    yields(0, 10, 10, 10, 10);
    yields(1, 10, 10, 10, 10);
    r = run();
    ck(r.retired_pass2 == 0,
       "mine_rebal: a mine holding half of every resource has a summed share of 200 and is kept by "
       "the FIRST test");
    //    Separate the threshold from zero: one resource only, share 100/101 -> 0 with integer
    //    division, and raise the threshold's own value to prove it is READ and not folded.
    place(2, 0);
    yields(0, 1, 1, 1, 1);
    yields(1, 100, 100, 100, 100);
    f.mine_low_share = 0; // an unsigned `share >= 0` is true for every input
    r                = run();
    ck(r.retired_pass2 == 0,
       "mine_rebal: the low-share threshold is READ from _G_LLM_STRAT_AI_MINE_LOW_SHARE_PCT_"
       "THRESHOLD, not folded -- at 0 the first keep test fires on every mine and nothing retires");

    //    THE FOUR PER-RESOURCE TESTS: 2 * this mine's estimate >= the portfolio total keeps it.
    //    The runt is out-produced 100:1 on three resources but holds HALF of resource 3.
    place(2, 0);
    yields(0, 1, 1, 50, 1);
    yields(1, 100, 100, 100, 100);
    r = run();
    ck(r.retired_pass2 == 0,
       "mine_rebal: ANY ONE of the four per-resource tests passing keeps the mine -- 2*50 >= 150 on "
       "resource 3 alone, even though it loses 100:1 on the other three");

    // -- PASS 3. Both mines survive pass 2; resource 1 is starved; the mine that makes NONE of it
    //    and has the smaller total is retired.
    place(2, 0);
    yields(0, 0, 50, 50, 50); // total 150, produces no resource 1
    yields(1, 1, 50, 50, 50); // total 151, produces some
    r = run();
    ck(r.retired_pass2 == 0 && r.reached_pass3 == 1,
       "mine_rebal: with nothing retired in pass 2 the body falls into pass 3");
    ck(r.scarce_res == 1,
       "mine_rebal: resource 1 is the scarce one -- weighted 1*12/4 = 3 against a weighted total of "
       "2003, and 3*8 < 2004");
    ck(r.retired_pass3 == 1 && g_rec.restart_orders.size() == 1 &&
           at(g_rec.restart_orders, 0).building_index == 1,
       "mine_rebal: pass 3 retires the mine that produces NONE of the scarce resource");
    ck(out[1] == 1 && out[2] == 50 && out[0] == 151,
       "mine_rebal: pass 3's retire subtracts the victim's estimate and re-sums, exactly as pass 2 "
       "does");
    ck(g_rec.train_flushes.size() == 1,
       "mine_rebal: pass 3's retire also flushes -- the same five-step sequence");

    //    THE RE-WEIGHTING DIVISORS. Resource 1 divides by FOUR and resources 2/3 by three; the
    //    case above cannot separate 4 from 3 because both leave resource 1 far under the threshold.
    //    Here the choice decides WHICH resource is scarce: with /4, w1 = 30 and 30*8 = 240 < 271;
    //    with /3, w1 = 40 and 40*8 = 320 >= 281, so resource 1 is skipped and resource 2 wins.
    place(2, 0);
    yields(0, 0, 0, 0, 10);  // total 10, produces no resource 1 -> the victim
    yields(1, 10, 0, 0, 10); // total 20
    r = run();
    ck(r.reached_pass3 == 1 && r.scarce_res == 1,
       "mine_rebal: pass 3 divides resource 1's weighted total by FOUR (a SHR 2) and resources 2/3 "
       "by three -- with a /3 on resource 1 this portfolio names resource 2 as the scarce one");
    ck(r.retired_pass3 == 1 && at(g_rec.restart_orders, 0).building_index == 1,
       "mine_rebal: and the victim is the mine that makes none of resource 1");

    //    Every mine produces the scarce resource -> no victim, nothing retired, but pass 3 RAN.
    place(2, 0);
    yields(0, 1, 50, 50, 50);
    yields(1, 1, 50, 50, 50);
    r = run();
    ck(r.reached_pass3 == 1 && r.retired_pass3 == 0 && g_rec.restart_orders.empty(),
       "mine_rebal: a scarce resource that EVERY mine produces leaves no victim -- pass 3 ran and "
       "chose nobody, which is why `reached_pass3` is reported separately from `retired_pass3`");

    //    AT MOST ONE MINE PER CALL, even when TWO resources are under the threshold. The single
    //    -scarce-resource case above cannot separate this: the loop has nothing left to find.
    place(3, 0);
    yields(0, 0, 0, 30, 30); // total 60 -- produces neither scarce resource
    yields(1, 0, 0, 35, 35); // total 70 -- ditto, and would be pass 2 of the scan
    yields(2, 1, 1, 35, 35); // total 72 -- produces both, never eligible
    r = run();
    ck(r.retired_pass2 == 0 && r.reached_pass3 == 1 && r.scarce_res == 1,
       "mine_rebal: with resources 1 AND 2 both weighted under an eighth, the scan stops at the "
       "FIRST of them");
    ck(r.retired_pass3 == 1 && g_rec.restart_orders.size() == 1 && g_rec.train_flushes.size() == 1,
       "mine_rebal: pass 3 BREAKS after retiring (JMP to the shared exit @0x004e3847) -- a body that "
       "kept scanning retires a second mine for resource 2 in the same call");

    //    SMALLEST TOTAL WINS among the eligible.
    place(3, 0);
    yields(0, 0, 80, 80, 80); // total 240, eligible
    yields(1, 0, 20, 20, 20); // total  60, eligible AND smaller -> the victim
    yields(2, 1, 50, 50, 50); // produces resource 1, ineligible
    r = run();
    ck(r.retired_pass3 == 1 && g_rec.restart_orders.size() == 1 &&
           at(g_rec.restart_orders, 0).building_index == 2,
       "mine_rebal: among the mines producing none of the scarce resource, the SMALLEST by_resource"
       "[0] total is retired -- slot 2 (collect row 1), not the first eligible one");
    ck(r.retired_pass3 == 1 && f.mine_roster[1] == 0 && f.mine_roster[0] == 1 &&
           f.mine_roster[2] == 3,
       "mine_rebal: only the victim's roster slot is cleared");
}

// ---- batch B layer 6: the mine-worth gate --------------------------------------------------------
//
// The rig reaches this function easily -- it is on the resource-site scanner's hot path -- but on a
// developed base its answer is `true` almost every time, so a clean shadow run is weak evidence by
// itself. What the rig CANNOT reach reliably is the fourth pass (the veto), a below-threshold answer,
// and an extract_id list that breaks early. Those are covered here.
void test_site_worth() {
    using namespace mh::ai;
    fixture f;

    const int32_t P    = 3;
    const int32_t TYPE = 12;
    // Fine coordinates whose coarse cell is (5, 9) -- deliberately unequal, so an [x][y] / [y][x]
    // transposition lands on a different cell.
    const int32_t FX = 5 * 4 + 2, FY = 9 * 4 + 2;

    auto cell = [&](int32_t cx, int32_t cy) -> map_resources & {
        return f.res_plane[(size_t)cx * 64 + (size_t)cy];
    };
    auto run     = [&]() { return detail::resource_site_meets_threshold(f.view(), P, TYPE, FX, FY); };
    auto no_veto = [&]() {
        for (int32_t id = RESOURCE_ID_FIRST; id <= RESOURCE_ID_LAST; ++id)
            f.players[P].ai_mine_yield_by_resource[id] = 1;
    };

    // THE CELL INDEX. Put the value at [x][y] and a decoy at [y][x]; a transposed body reads 0 here
    // and 9999 there, so the two assertions below cannot both pass on one body.
    no_veto();
    f.cfg_buildings[TYPE].extract_id[0] = 1;
    f.cfg_buildings[TYPE].extract_id[1] = 0;
    cell(5, 9).value[1]                 = 2000; // 2000 * 1 * weight 4 = 8000 >= 5000
    cell(9, 5).value[1]                 = 9999; // the decoy at the transposed cell
    ck(run(), "site_worth: the cell is resources[x >> 2][y >> 2] -- X selects the 1024-byte row");
    cell(5, 9).value[1] = 1000; // 1000 * 4 = 4000 < 5000
    ck(!run(),
       "site_worth: below nMineWorth answers false -- and the decoy at the transposed cell (9999) "
       "is still there, so a body reading [y][x] would answer TRUE here");

    // THE WEIGHT, and that it is keyed by resource id. id 4's weight is 1, so the same raw amount
    // that clears the bar as id 1 (weight 4) must fail as id 4.
    cell(5, 9).value[1]                 = 0;
    cell(5, 9).value[4]                 = 2000;
    f.cfg_buildings[TYPE].extract_id[0] = 4;
    ck(!run(),
       "site_worth: the weight is keyed by RESOURCE ID -- 2000 of id 4 is worth 2000, not 8000. A "
       "body that read weights[id] off the ACTIVE_PLAYER_COUNT base would pick up weight 2 here");
    f.mine_worth = 2000;
    ck(run(), "site_worth: ... and 2000 exactly meets a threshold of 2000 -- the compare is >=");
    f.mine_worth = 2001;
    ck(!run(), "site_worth: ... while 2000 does not meet 2001, so the >= is not a >");

    // THE UNSIGNED COMPARE (CMP / SETNC @0x004e6383). A threshold of -1 read SIGNED would let
    // everything through; read UNSIGNED it is 0xffffffff and nothing clears it.
    f.mine_worth        = -1;
    cell(5, 9).value[4] = 30000;
    ck(!run(),
       "site_worth: the threshold compare is UNSIGNED -- a threshold of -1 is 0xffffffff and no "
       "site clears it. Signed, every site would");
    f.mine_worth = 5000;

    // THE extract_id BREAK. {0, 3} must mask NOTHING: the first zero ends the walk, it does not skip.
    cell(5, 9).value[4]                 = 0;
    cell(5, 9).value[3]                 = 30000;
    f.cfg_buildings[TYPE].extract_id[0] = 0;
    f.cfg_buildings[TYPE].extract_id[1] = 3;
    ck(!run(),
       "site_worth: a zero extract_id BREAKS the walk -- {0, 3} extracts nothing, so a rich id-3 "
       "deposit is worth zero. A body that skipped instead of breaking would answer true");
    f.cfg_buildings[TYPE].extract_id[0] = 3;
    f.cfg_buildings[TYPE].extract_id[1] = 0;
    ck(run(), "site_worth: ... and {3, 0} does extract id 3, so the same deposit now clears");

    // Only the masked ids contribute: id 2 is rich but not extracted.
    cell(5, 9).value[3] = 0;
    cell(5, 9).value[2] = 30000;
    ck(!run(),
       "site_worth: a resource the building type does not extract contributes nothing, however "
       "rich -- extract_mask[id] is a multiplier, not a filter applied afterwards");

    // THE VETO -- the fourth pass, the one the function's plate omitted until 2026-08-03. A site that
    // comfortably clears the bar on the EXTRACTED resource, with the player's yield zeroed for a
    // resource this mine type could never produce.
    for (int32_t id = 0; id < 8; ++id) cell(5, 9).value[id] = 0;
    f.cfg_buildings[TYPE].extract_id[0] = 1;
    f.cfg_buildings[TYPE].extract_id[1] = 0;
    cell(5, 9).value[1]                 = 30000; // 30000 * 4 -- far above 5000
    no_veto();
    ck(run(), "site_worth: control -- with yield on every id the veto does not fire");
    f.players[P].ai_mine_yield_by_resource[2] = 0;
    ck(!run(),
       "site_worth: THE VETO. Zero existing yield on id 2 AND zero contribution from this site on "
       "id 2 forces the total to 0, even though the extracted id 1 is worth 120000. Note id 2 is "
       "not even in this type's extract list -- the veto is not 'the site must supply what I lack'");
    cell(5, 9).value[2] = 1; // still not extracted, so contrib stays 0 -> still vetoed
    ck(!run(),
       "site_worth: a rich cell does not lift the veto on an id the type cannot extract -- contrib "
       "is value * MASK * weight, and the mask is 0");
    f.cfg_buildings[TYPE].extract_id[1] = 2; // now it IS extracted, so contrib is nonzero
    ck(run(),
       "site_worth: ... and extracting id 2 does lift it, because contrib[2] becomes nonzero. That "
       "pair is what separates the veto's two conjuncts");

    // The player row. A body reading player 0's yield instead of P's would pass everything above
    // (only P's row is seeded), so give player 0 the opposite state and re-check.
    f.cfg_buildings[TYPE].extract_id[1] = 0; // back to extracting id 1 only
    no_veto();
    f.players[P].ai_mine_yield_by_resource[3] = 0;
    for (int32_t id = RESOURCE_ID_FIRST; id <= RESOURCE_ID_LAST; ++id)
        f.players[0].ai_mine_yield_by_resource[id] = 99;
    ck(!run(),
       "site_worth: the veto reads player_idx's own ai_mine_yield_by_resource row -- player 0 has "
       "yield on every id, and a body indexing the wrong row would answer true");
}

// ---- batch C layer 0: the object-removed notification hook --------------------------------------
//
// llm_strat_ai_notify_object_removed writes only three scalars itself; everything else it does is
// the ARGUMENTS of five outward calls. So every assertion below is over recorded calls rather than
// over the fixture, and the fixture is deliberately asymmetric (four active players of which one is
// human, a non-zero owner, a rectangular map, a building whose x != y) so that a body which swapped
// any pair cannot pass by coincidence.
//
// MUTATION-CHECKED (2026-08-05, tools/oneoff/2026-08-05-mutate-notify-removed.py). SEVEN deliberate
// breaks, each watched to fail the assertion that NAMES it and then reverted, with a clean
// 1448-checks/0-failures rebuild afterwards:
//   M1 seed the owner's grid with 2 even for a structural type -> the "ALL SIX structural types"
//      check plus case A's per-argument check (2 failures)
//   M2 set the turret-rescan flag on the owner as well          -> "the turret's OWN owner" (1)
//   M3 pass the owner nibble where the packed ref belongs       -> "the whole PACKED ref" (1)
//   M4 read member_count BEFORE the unlink                      -> "reaches 0 AFTER the unlink" (2)
//   M5 drop the group >= 5 disband guard                        -> "groups 0..4 are the spawn
//      seeds" (1)
//   M6 continue the site scan past the first match              -> "returns at the FIRST match" (2)
//   M7 store 1 into reinforce_pending instead of incrementing   -> "INCREMENTED (not stored)" (3)
void test_notify_object_removed() {
    const uint32_t OWNER = 3; // NOT 0: a stride bug lands on the right row for player 0
    const int32_t  IDX   = 7; // roster slot of the removed object
    const int32_t  BID   = 12;

    // ---- A. BUILDING removal: the influence re-seed loop, structural (turret) type -------------
    {
        fixture f;
        f.active_players            = 4;
        f.players[0].ai_enabled     = 1;
        f.players[1].ai_enabled     = 0; // human -- neither flagged nor re-seeded
        f.players[2].ai_enabled     = 1;
        f.players[OWNER].ai_enabled = 1;
        f.players[4].ai_enabled     = 1; // past active_player_count -- must stay untouched
        f.map_w                     = 64;
        f.map_h                     = 48;
        building &b                 = f.buildings[OWNER * BUILDINGS_PER_PLAYER + IDX];
        b.building_id               = (uint16_t)BID;
        b.x                         = 20;
        b.y                         = 9; // x != y, so an axis swap is visible
        f.cfg_buildings[BID].type   = BLDG_TYPE_A_TURRET;

        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 0);

        ck(g_rec.seed_stamps.size() == 3,
           "notify_removed: the re-seed runs once per AI player and skips the human one");
        bool           all_ok      = g_rec.seed_stamps.size() == 3;
        const uint32_t expect_p[3] = {0, 2, OWNER};
        for (size_t i = 0; i < g_rec.seed_stamps.size() && i < 3; ++i) {
            const recorder::seed_stamp &st = g_rec.seed_stamps[i];
            all_ok &= st.grid == (void *)f.players[expect_p[i]].ai_tile_flags_grid;
            all_ok &= st.w == 64 && st.h == 48;
            all_ok &= st.stencil == (void *)&f.cfg_buildings[BID].area[0][0];
            all_ok &= st.span_x == FOOTPRINT_SPAN && st.span_y == FOOTPRINT_SPAN;
            all_ok &= st.x == 20 && st.y == 9;
            all_ok &= st.seed == 0;
        }
        ck(all_ok, "notify_removed: each re-seed gets THAT player's grid, (width, height), the cfg "
                   "row's 10x10 area stencil, the building's (x, y) and seed 0");
        ck(f.players[0].ai_turret_rescan_pending == 1 &&
               f.players[2].ai_turret_rescan_pending == 1,
           "notify_removed: a removed turret flags every OTHER AI player for a threat rescan");
        ck(f.players[OWNER].ai_turret_rescan_pending == 0,
           "notify_removed: the turret's OWN owner is not flagged for a rescan");
        ck(f.players[1].ai_turret_rescan_pending == 0 &&
               f.players[4].ai_turret_rescan_pending == 0,
           "notify_removed: neither the human player nor one past active_player_count is flagged");
    }

    // ---- B. the seed is 2 only on the OWNER's grid and only for a NON-structural type ----------
    {
        fixture f;
        f.active_players            = 4;
        f.players[0].ai_enabled     = 1;
        f.players[OWNER].ai_enabled = 1;
        building &b                 = f.buildings[OWNER * BUILDINGS_PER_PLAYER + IDX];
        b.building_id               = (uint16_t)BID;
        f.cfg_buildings[BID].type   = BLDG_TYPE_A_BARRACKS; // not turret / mine / relay

        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 0);
        ck(g_rec.seed_stamps.size() == 2 && at(g_rec.seed_stamps, 0).seed == 0 &&
               at(g_rec.seed_stamps, 1).seed == 2,
           "notify_removed: a plain building re-seeds OTHER players' grids with 0 and the OWNER's "
           "with 2");
        ck(f.players[0].ai_turret_rescan_pending == 0,
           "notify_removed: a non-turret removal flags nobody for a rescan");

        // ...and every one of the six structural types puts the owner back on seed 0. Tested as a
        // set rather than on one member because the original spells out all six comparisons and a
        // translation that dropped one would still pass a single-type case.
        const uint8_t structural[6] = {BLDG_TYPE_H_TURRET, BLDG_TYPE_A_TURRET, BLDG_TYPE_H_MINE,
                                       BLDG_TYPE_A_MINE, BLDG_TYPE_H_RELAY, BLDG_TYPE_A_RELAY};
        bool          owner_zero    = true;
        for (int i = 0; i < 6; ++i) {
            f.cfg_buildings[BID].type = structural[i];
            g_rec.clear();
            detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                          (uint32_t)IDX, 0);
            owner_zero &= g_rec.seed_stamps.size() == 2 && at(g_rec.seed_stamps, 1).seed == 0;
        }
        ck(owner_zero, "notify_removed: ALL SIX structural types (both races' turret, mine and "
                       "relay) put the owner's own re-seed back on 0");
    }

    // ---- C. a UNIT-class ref never reaches phase 1 at all --------------------------------------
    {
        fixture f;
        f.active_players            = 2;
        f.players[0].ai_enabled     = 1;
        f.players[OWNER].ai_enabled = 1;
        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), unit_ref(0), (uint32_t)IDX,
                                      0);
        ck(g_rec.seed_stamps.empty(),
           "notify_removed: the influence re-seed is gated on the BUILDING bit (0x40), so a unit "
           "removal stamps nothing");
    }

    // ---- D. the heli-mother early return abandons the whole notification -----------------------
    {
        fixture f;
        f.active_players                                      = 3;
        f.players[OWNER].ai_enabled                           = 1;
        f.units[OWNER * UNITS_PER_PLAYER + IDX].unit_proto_id = 5;
        g_rec.group_index                                     = 9;

        for (int variant = 0; variant < 2; ++variant) {
            f.cfg_units[5].type =
                variant == 0 ? UNIT_TYPE_A_HELI_MOTHER : UNIT_TYPE_H_HELI_MOTHER;
            g_rec.clear();
            g_rec.group_index = 9;
            detail::notify_object_removed(f.view(), f.store(), stub_calls(), unit_ref(OWNER),
                                          (uint32_t)IDX, 0);
            ck(g_rec.target_removes.empty() && g_rec.unlinks.empty() &&
                   g_rec.group_lookups.empty(),
               variant == 0 ? "notify_removed: an A_HELI_MOTHER removal returns before the target "
                              "purge and before all owner bookkeeping"
                            : "notify_removed: an H_HELI_MOTHER removal does the same");
        }

        // A DIFFERENT unit type must go through -- otherwise the case above proves nothing about
        // the type test, only that units are ignored.
        f.cfg_units[5].type = 0x12;
        g_rec.clear();
        g_rec.group_index = 0xffffu; // stop after the purge; the group arm is case G
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), unit_ref(OWNER),
                                      (uint32_t)IDX, 0);
        ck(g_rec.target_removes.size() == 3,
           "notify_removed: a unit that is NOT a heli mother proceeds to the target purge");
    }

    // ---- E. the target purge covers every active player, with the PACKED ref ------------------
    {
        fixture f;
        f.active_players                     = 4;
        f.players[OWNER].ai_enabled          = 0; // human owner: phase 3 must be skipped, phase 2 must not
        f.players[0].ai_enabled              = 1;
        building &b                          = f.buildings[OWNER * BUILDINGS_PER_PLAYER + IDX];
        b.building_id                        = (uint16_t)BID;
        f.cfg_buildings[BID].type            = BLDG_TYPE_A_MINE;
        f.players[OWNER].ai_turret_candidate = 99;

        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 0);
        bool purge_ok = g_rec.target_removes.size() == 4;
        for (size_t i = 0; i < g_rec.target_removes.size(); ++i)
            purge_ok &= g_rec.target_removes[i].player == (int32_t)i &&
                        g_rec.target_removes[i].ref == bldg_ref(OWNER) &&
                        g_rec.target_removes[i].index == IDX;
        ck(purge_ok, "notify_removed: the purge runs for players 0..active-1 and forwards the whole "
                     "PACKED ref, not the owner nibble");
        ck(g_rec.bldg_reconciles.empty(),
           "notify_removed: a HUMAN owner skips all of phase 3 -- no queue reconcile even though "
           "the type and hard_remove would otherwise call for one");
        ck(!g_rec.seed_stamps.empty(),
           "notify_removed: phase 1 is NOT gated on the owner being an AI -- player 0's grid is "
           "still re-seeded for a human's building");
    }

    // ---- F. the build-queue reconcile and its two suppressors ---------------------------------
    {
        fixture f;
        f.active_players            = 2;
        f.players[OWNER].ai_enabled = 1;
        building &b                 = f.buildings[OWNER * BUILDINGS_PER_PLAYER + IDX];
        b.building_id               = (uint16_t)BID;
        f.cfg_buildings[BID].type   = BLDG_TYPE_A_BARRACKS; // not a mine: stops before the sites

        f.players[OWNER].ai_turret_candidate = 99;
        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 0);
        ck(g_rec.bldg_reconciles.size() == 1 && at(g_rec.bldg_reconciles, 0).player == OWNER &&
               at(g_rec.bldg_reconciles, 0).bldg_index == (uint32_t)IDX,
           "notify_removed: a soft remove of a building whose id is not the cached turret candidate "
           "reconciles the build queue, with (owner, roster index)");

        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 1);
        ck(g_rec.bldg_reconciles.empty(),
           "notify_removed: a HARD remove does not reconcile the build queue");

        f.players[OWNER].ai_turret_candidate = BID;
        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 0);
        ck(g_rec.bldg_reconciles.empty(),
           "notify_removed: a building whose id EQUALS ai_turret_candidate does not reconcile -- "
           "and the comparison is against the cfg id, not the roster index");
    }

    // ---- G. the mine's resource site: first match only, and only below the count ---------------
    {
        for (int hard = 0; hard < 2; ++hard) {
            fixture f;
            f.active_players                     = 2;
            f.players[OWNER].ai_enabled          = 1;
            f.players[OWNER].ai_turret_candidate = 99;
            building &b                          = f.buildings[OWNER * BUILDINGS_PER_PLAYER + IDX];
            b.building_id                        = (uint16_t)BID;
            f.cfg_buildings[BID].type            = BLDG_TYPE_H_MINE;

            player_data &pd                = f.players[OWNER];
            pd.ai_resource_site_count      = 4;
            pd.ai_resource_sites[0].status = 0;
            pd.ai_resource_sites[1].status = (int16_t)IDX; // the match
            pd.ai_resource_sites[2].status = (int16_t)IDX; // a SECOND match, must stay
            pd.ai_resource_sites[3].status = (int16_t)0xffff;
            pd.ai_resource_sites[5].status = (int16_t)IDX; // past the count, must stay

            g_rec.clear();
            detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                          (uint32_t)IDX, hard);

            const int16_t want = hard ? (int16_t)0xffff : (int16_t)0;
            ck(pd.ai_resource_sites[1].status == want,
               hard ? "notify_removed: a HARD mine removal INVALIDATES its resource site (0xffff)"
                    : "notify_removed: a SOFT mine removal REOPENS its resource site (0)");
            ck(pd.ai_resource_sites[2].status == (int16_t)IDX,
               "notify_removed: the site scan returns at the FIRST match -- a second site holding "
               "the same roster index is left alone");
            ck(pd.ai_resource_sites[5].status == (int16_t)IDX,
               "notify_removed: the scan is bounded by ai_resource_site_count, so a site past it "
               "is never examined");
            ck(pd.ai_resource_sites[0].status == 0 && pd.ai_resource_sites[3].status == (int16_t)0xffff,
               "notify_removed: sites whose status does not hold this building's index are "
               "untouched, including the already-invalid one");
        }

        // A NON-mine building of the same shape must not touch any site at all -- otherwise the
        // cases above would also pass a body that skipped the type gate.
        fixture f;
        f.active_players                             = 2;
        f.players[OWNER].ai_enabled                  = 1;
        f.players[OWNER].ai_turret_candidate         = 99;
        building &b                                  = f.buildings[OWNER * BUILDINGS_PER_PLAYER + IDX];
        b.building_id                                = (uint16_t)BID;
        f.cfg_buildings[BID].type                    = BLDG_TYPE_A_RELAY; // structural, but not a mine
        f.players[OWNER].ai_resource_site_count      = 2;
        f.players[OWNER].ai_resource_sites[0].status = (int16_t)IDX;
        g_rec.clear();
        detail::notify_object_removed(f.view(), f.store(), stub_calls(), bldg_ref(OWNER),
                                      (uint32_t)IDX, 1);
        ck(f.players[OWNER].ai_resource_sites[0].status == (int16_t)IDX,
           "notify_removed: only a MINE releases a resource site -- a relay with a matching site "
           "leaves it alone");
    }

    // ---- H. the unit's group: unlink, reinforce_pending, and the disband guard -----------------
    {
        // H1: the two "not in a group" sentinels, tested separately because the original tests
        // them separately and a body that folded them into one 16-bit compare would pass only one.
        const uint32_t sentinels[2] = {0xffffffffu, 0x0000ffffu};
        for (int i = 0; i < 2; ++i) {
            fixture f;
            f.active_players            = 2;
            f.players[OWNER].ai_enabled = 1;
            g_rec.clear();
            g_rec.group_index   = sentinels[i];
            g_unlink_group_cell = nullptr;
            detail::notify_object_removed(f.view(), f.store(), stub_calls(), unit_ref(OWNER),
                                          (uint32_t)IDX, 0);
            ck(g_rec.group_lookups.size() == 1 && g_rec.unlinks.empty() &&
                   g_rec.group_removes.empty(),
               i == 0 ? "notify_removed: group index -1 means 'no group' -- looked up, then nothing"
                      : "notify_removed: group index 0xffff means the same");
        }

        // H2/H3/H4: a real group. The unlink stub decrements member_count exactly as the original
        // callee does, so `member_count == 0` is a property of the state AFTER the unlink -- which
        // is what makes the read ORDER testable.
        struct case_row {
            uint32_t    group;
            int16_t     count_before;
            bool        expect_remove;
            const char *why;
        };
        const case_row rows[3] = {
            {7, 2, false,
             "notify_removed: a group that still has members after the unlink is not disbanded"},
            {7, 1, true,
             "notify_removed: a group whose member_count reaches 0 AFTER the unlink is disbanded -- "
             "reading the count before the unlink would disband nothing"},
            {4, 1, false,
             "notify_removed: groups 0..4 are the spawn seeds and are never disbanded, even empty"},
        };
        for (int i = 0; i < 3; ++i) {
            fixture f;
            f.active_players            = 2;
            f.players[OWNER].ai_enabled = 1;
            unit_group &g               = f.players[OWNER].ai_groups[rows[i].group];
            g.member_count              = rows[i].count_before;
            g.reinforce_pending         = 4; // non-zero, so an INC is distinguishable from a store

            g_rec.clear();
            g_rec.group_index   = rows[i].group;
            g_unlink_group_cell = &g;
            detail::notify_object_removed(f.view(), f.store(), stub_calls(), unit_ref(OWNER),
                                          (uint32_t)IDX, 0);
            g_unlink_group_cell = nullptr;

            ck(g_rec.unlinks.size() == 1 && at(g_rec.unlinks, 0).player == (int32_t)OWNER &&
                   at(g_rec.unlinks, 0).group == (int32_t)rows[i].group &&
                   at(g_rec.unlinks, 0).unit_index == IDX,
               "notify_removed: the member is unlinked with (owner, group, unit index)");
            ck(g.reinforce_pending == 5,
               "notify_removed: reinforce_pending is INCREMENTED (not stored) once per lost member");
            ck((g_rec.group_removes.size() == 1) == rows[i].expect_remove, rows[i].why);
            if (rows[i].expect_remove)
                ck(at(g_rec.group_removes, 0).player == (int32_t)OWNER &&
                       at(g_rec.group_removes, 0).group == rows[i].group,
                   "notify_removed: the disband names the same (owner, group)");
        }
    }
}

// ---- 47. batch C layer 1: the target-list REMOVER --------------------------------------------
//
// llm_strat_ai_target_list_remove is 165 bytes of swap-remove, and three of its properties are the
// kind a "sensible" rewrite silently improves. Each has its own case below:
//   * it keys on the AGGRESSOR pair, never the victim pair;
//   * it does not stop at the first match;
//   * it ADVANCES over the entry it swapped in from the tail, so a second matching entry sitting
//     last SURVIVES the call.
// MUTATION-CHECKED 2026-08-05 (tools/oneoff/2026-08-05-mutate-c1.py):
//   M1 key on victim_index/victim_ref instead of the aggressor pair -> "the AGGRESSOR half" (6)
//   M2 stop at the first match                                      -> "does NOT stop" (2)
//   M3 re-test the swapped-in entry (`--i` after the swap)          -> "SURVIVES" (2)
//   M4 always copy, even when the match is last                     -> NOT CAUGHT, and correctly so
//   M5 match on the ref alone                                       -> "BOTH columns" (1)
//
// M4 IS A NON-MUTATION, which is worth stating rather than leaving as a hole in the campaign. The
// original's `JZ 0x004eb2d2` skips the REP MOVSD when the match is already the last live entry, and
// removing that guard makes the copy `list[i] = list[i]` -- a self-assignment of a trivially
// copyable record, i.e. a no-op in both arms and invisible to the shadow site's byte compare of
// player_data as well. The guard is reproduced because it mirrors the original's control flow and
// costs nothing, NOT because anything can observe it. Case D below therefore pins what IS
// observable: the tail record and the count, not the branch.
void test_target_list_remove() {
    const int32_t  ME  = 3;
    const uint32_t REF = 0x25u; // packed: unit class + owner 5
    const int32_t  IDX = 11;

    auto put = [](fixture &f, int32_t player, int slot, int32_t agg_idx, uint32_t agg_ref,
                  int32_t victim_idx, uint32_t victim_ref) {
        target_entry &e   = f.players[player].ai_target_list[slot];
        e.aggressor_index = agg_idx;
        e.aggressor_ref   = agg_ref;
        e.victim_index    = victim_idx;
        e.victim_ref      = victim_ref;
        e.ai_group_index  = 1000 + slot; // a per-slot fingerprint, so a swap is traceable
    };

    // ---- A. the aggressor pair is the key, and BOTH columns are tested ------------------------
    {
        fixture f;
        put(f, ME, 0, IDX, REF, 0, 0);      // match
        put(f, ME, 1, IDX, REF ^ 1u, 0, 0); // same index, different ref -- no match
        put(f, ME, 2, IDX + 1, REF, 0, 0);  // same ref, different index -- no match
        put(f, ME, 3, 0, 0, IDX, REF);      // the VICTIM half matches -- must survive
        f.players[ME].ai_target_list_count = 4;

        const int32_t removed = detail::target_list_remove(f.store(), ME, REF, IDX);

        ck(removed == 1 && f.players[ME].ai_target_list_count == 3,
           "target_list_remove: exactly the one entry whose AGGRESSOR pair matches is removed");
        // Slot 0 now holds what was slot 3 -- the victim-half entry, moved down, still present.
        ck(f.players[ME].ai_target_list[0].ai_group_index == 1003 &&
               f.players[ME].ai_target_list[0].victim_index == IDX,
           "target_list_remove: it matches the AGGRESSOR half, so a victim-half entry survives");
        ck(f.players[ME].ai_target_list[1].aggressor_ref == (REF ^ 1u) &&
               f.players[ME].ai_target_list[2].aggressor_index == IDX + 1,
           "target_list_remove: BOTH columns must match -- one alone is not enough");
    }

    // ---- B. it does not stop at the first match ------------------------------------------------
    //
    // THE ARRANGEMENT IS THE TEST, and the first draft of this case got it wrong: with the matches
    // interleaved among the fillers, each removal swaps a TAIL entry down and the scan walks past
    // it, so a matching tail entry survives and the case measures property 3 instead of this one.
    // Matches at the HEAD and fillers at the tail is the arrangement in which the swap-in is always
    // a non-match, which isolates "the scan runs to the end" from "the swapped-in entry is skipped".
    {
        fixture f;
        put(f, ME, 0, IDX, REF, 0, 0);  // match
        put(f, ME, 1, IDX, REF, 0, 0);  // match
        put(f, ME, 2, IDX, REF, 0, 0);  // match, and it becomes the last live entry in turn
        put(f, ME, 3, 99, 0x11u, 0, 0); // filler
        put(f, ME, 4, 98, 0x12u, 0, 0); // filler
        f.players[ME].ai_target_list_count = 5;

        const int32_t removed = detail::target_list_remove(f.store(), ME, REF, IDX);

        ck(removed == 3 && f.players[ME].ai_target_list_count == 2,
           "target_list_remove: the scan does NOT stop at the first match");
        bool none_left = true;
        for (int i = 0; i < f.players[ME].ai_target_list_count; ++i)
            none_left &= !(f.players[ME].ai_target_list[i].aggressor_index == IDX &&
                           f.players[ME].ai_target_list[i].aggressor_ref == REF);
        ck(none_left, "target_list_remove: no matching entry is left behind in this arrangement");
    }

    // ---- C. THE SURVIVOR. Two matches, the second one LAST ------------------------------------
    //
    // Removing slot 0 swaps the LAST entry -- itself a match -- down into slot 0, and the index
    // then advances past it. The original never re-tests it, so it survives with the count now
    // pointing one short. This is the behaviour the header calls property 3, and it is the single
    // most likely thing for a translation to "fix".
    {
        fixture f;
        put(f, ME, 0, IDX, REF, 0, 0);  // match
        put(f, ME, 1, 99, 0x11u, 0, 0); // filler
        put(f, ME, 2, IDX, REF, 0, 0);  // match, and LAST
        f.players[ME].ai_target_list_count = 3;

        const int32_t removed = detail::target_list_remove(f.store(), ME, REF, IDX);

        ck(removed == 1 && f.players[ME].ai_target_list_count == 2,
           "target_list_remove: only ONE of two matches is removed when the second is last");
        ck(f.players[ME].ai_target_list[0].aggressor_index == IDX &&
               f.players[ME].ai_target_list[0].aggressor_ref == REF &&
               f.players[ME].ai_target_list[0].ai_group_index == 1002,
           "target_list_remove: the entry swapped in from the tail is never re-tested, so a "
           "matching one SURVIVES");
    }

    // ---- D. the swap is SKIPPED when the match is already the last live entry ------------------
    {
        fixture f;
        put(f, ME, 0, 99, 0x11u, 0, 0); // filler
        put(f, ME, 1, IDX, REF, 0, 0);  // match, and last
        f.players[ME].ai_target_list_count = 2;

        const int32_t removed = detail::target_list_remove(f.store(), ME, REF, IDX);

        ck(removed == 1 && f.players[ME].ai_target_list_count == 1,
           "target_list_remove: a match that is already last is removed by the count alone");
        ck(f.players[ME].ai_target_list[1].ai_group_index == 1001 &&
               f.players[ME].ai_target_list[1].aggressor_index == IDX,
           "target_list_remove: the record past the new count is left as it was");
        ck(f.players[ME].ai_target_list[0].ai_group_index == 1000,
           "target_list_remove: the surviving head entry is untouched");
    }

    // ---- E. the empty and no-match cases write nothing -----------------------------------------
    {
        fixture f;
        ck(detail::target_list_remove(f.store(), ME, REF, IDX) == 0 &&
               f.players[ME].ai_target_list_count == 0,
           "target_list_remove: an empty roster is a no-op");

        put(f, ME, 0, 1, 0x11u, 0, 0);
        f.players[ME].ai_target_list_count = 1;
        ck(detail::target_list_remove(f.store(), ME, REF, IDX) == 0 &&
               f.players[ME].ai_target_list_count == 1 &&
               f.players[ME].ai_target_list[0].ai_group_index == 1000,
           "target_list_remove: a roster with no match is left exactly as it was");
        // The stride check: player ME's roster moved, nobody else's did.
        ck(f.players[ME - 1].ai_target_list_count == 0 &&
               f.players[ME + 1].ai_target_list_count == 0,
           "target_list_remove: it writes only the addressed player's row");
    }
}

// ---- 48. batch C layer 1: the unit-group task machine DRIVER ----------------------------------
//
// llm_strat_ai_unit_group_tick delegates everything, so what is under test is control flow: two
// gates, a restart-on-removal reaper with a floor of 5, and a per-group body with two re-entry
// edges. Every case below asserts the SEQUENCE of recorded calls, because a count would pass for a
// driver that ticked the wrong groups the right number of times.
//
// MUTATION-CHECKED 2026-08-05 (tools/oneoff/2026-08-05-mutate-c1.py):
//   M6  drop the ALIVE gate                              -> "returns before touching anything" (1)
//   M7  drop the ai_phase_flags gate                     -> "the phase bit gates the body" (1)
//   M8  start the reaper at 0 instead of 5               -> "never reaps a seed group" (2)
//   M9  continue the reaper instead of restarting        -> "restarts from the floor" (2)
//   M10 reap on member_count alone (ignore task_code)    -> "only under task_code 9 or 0x14" (1)
//   M11 advance to the next group after a non-zero step  -> "re-enters the SAME group" (2)
//   M12 skip the post-activate re-test of active_flag    -> "re-enters until the flag is set" (2)
//   M13 apply the floor of 5 to pass 2 as well           -> "pass 2 has NO floor" (1)
void test_unit_group_tick() {
    const int32_t ME = 2; // not 0: a player stride bug lands on the right row for player 0

    // Arm both gates and give the fixture a task-machine owner. Returns the player_data row.
    auto armed = [](fixture &f, int32_t player) -> player_data & {
        f.profiles[player].status_flags  = PLAYER_STATUS_ALIVE;
        f.players[player].ai_phase_flags = AI_PHASE_UNIT_GROUPS;
        g_group_remove_owner             = &f.players[player];
        g_task_owner                     = &f.players[player];
        return f.players[player];
    };

    // ---- A. the two gates ----------------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd   = armed(f, ME);
        pd.ai_group_count = 8;
        for (int g = 0; g < 8; ++g) pd.ai_groups[g].task_queue_count = 1;

        f.profiles[ME].status_flags = 0; // ALIVE clear
        detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);
        ck(!r.alive_gate && r.bodies == 0 && g_rec.activates.empty(),
           "unit_group_tick: a player without the ALIVE bit returns before touching anything");

        f.profiles[ME].status_flags = PLAYER_STATUS_ALIVE;
        pd.ai_phase_flags           = AI_PHASE_UNIT_GROUPS ^ 0xffu; // every OTHER phase bit set
        r                           = detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);
        ck(r.alive_gate && !r.phase_gate && r.bodies == 0 && g_rec.activates.empty(),
           "unit_group_tick: the phase bit gates the body, and it is bit 0x4 specifically");

        // The neighbour rows must be untouched by any of the above.
        ck(f.players[ME - 1].ai_group_count == 0 && f.players[ME + 1].ai_group_count == 0,
           "unit_group_tick: the gates read only the addressed player's rows");
    }

    // ---- B. the reaper: floor of 5, task_code filter, restart after every removal --------------
    //
    // Groups 0..7. The reapable ones are 3 (below the floor -- must survive) and 6, 7. Removal
    // compacts, so reaping 6 moves 7 down into 6 and the restart is what finds it there. A driver
    // that continued from index 7 after removing 6 would leave one behind, which is M9.
    {
        fixture f;
        g_rec.clear();
        g_rec.step_results = {0u};
        player_data &pd    = armed(f, ME);
        pd.ai_group_count  = 8;
        for (int g = 0; g < 8; ++g) {
            pd.ai_groups[g].member_count     = 4;
            pd.ai_groups[g].task_code        = AI_GROUP_TASK_DISBAND;
            pd.ai_groups[g].serial_id        = 100 + g; // fingerprint, so compaction is traceable
            pd.ai_groups[g].task_queue_count = 0;       // keep pass 2 out of this case
        }
        pd.ai_groups[3].member_count = 0; // reapable but BELOW the floor
        pd.ai_groups[5].member_count = 0;
        pd.ai_groups[5].task_code    = 7; // empty, but not one of the two accepted codes
        pd.ai_groups[6].member_count = 0;
        pd.ai_groups[6].task_code    = AI_GROUP_TASK_RECRUIT_FROM_STORAGE;
        pd.ai_groups[7].member_count = 0;
        pd.ai_groups[7].task_code    = AI_GROUP_TASK_DISBAND;

        const detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);

        ck(r.disbands == 2 && g_rec.group_removes.size() == 2,
           "unit_group_tick: it reaps both empty groups at or above the floor");
        ck(g_rec.group_removes.size() == 2 && g_rec.group_removes[0].group == 6 &&
               g_rec.group_removes[1].group == 6,
           "unit_group_tick: it restarts from the floor after a removal -- the compacted-in group "
           "is found at the SAME index, not skipped");
        ck(pd.ai_group_count == 6 && pd.ai_groups[3].serial_id == 103 &&
               pd.ai_groups[3].member_count == 0,
           "unit_group_tick: the reaper never reaps a seed group (index < 5), however empty");
        ck(pd.ai_groups[5].serial_id == 105,
           "unit_group_tick: an empty group is reaped only under task_code 9 or 0x14");
        // sweeps == disbands + 1: one sweep per removal plus the final clean one.
        ck(r.disband_sweeps == 3,
           "unit_group_tick: the reaper ends only when a whole sweep removes nothing");
    }

    // ---- C. pass 2 has NO floor, and an idle group ends its turn at the first test -------------
    {
        fixture f;
        g_rec.clear();
        g_rec.step_results               = {0u};
        player_data &pd                  = armed(f, ME);
        pd.ai_group_count                = 4;
        pd.ai_groups[0].task_queue_count = 1; // a SEED group with work
        pd.ai_groups[1].task_queue_count = 0; // idle
        pd.ai_groups[2].task_queue_count = 2;
        pd.ai_groups[3].task_queue_count = 0; // idle

        const detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);

        ck(r.groups_ticked == 2 && r.idle_groups == 2,
           "unit_group_tick: pass 2 has NO floor -- the seed groups are ticked, and an idle group "
           "ends its turn at the task_queue_count test");
        ck(g_rec.activates.size() == 2 && g_rec.activates[0].group == 0 &&
               g_rec.activates[1].group == 2,
           "unit_group_tick: the groups activated are 0 and 2, in that order");
        ck(g_rec.steps.size() == 2 && g_rec.steps[0].group == 0 && g_rec.steps[1].group == 2,
           "unit_group_tick: each activated group is stepped exactly once when the step returns 0");
        ck(pd.ai_groups[0].active_flag == 1 && pd.ai_groups[1].active_flag == 0,
           "unit_group_tick: activation is what sets active_flag, and only for a group with work");
    }

    // ---- D. a group already ACTIVE is stepped without being re-activated ------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.step_results               = {0u};
        player_data &pd                  = armed(f, ME);
        pd.ai_group_count                = 2;
        pd.ai_groups[0].task_queue_count = 1;
        pd.ai_groups[0].active_flag      = 1;
        pd.ai_groups[1].task_queue_count = 1;

        detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);

        ck(g_rec.activates.size() == 1 && g_rec.activates[0].group == 1,
           "unit_group_tick: an already-active group skips activation");
        ck(g_rec.steps.size() == 2 && g_rec.steps[0].group == 0 && g_rec.steps[1].group == 1,
           "unit_group_tick: both groups are stepped regardless");
    }

    // ---- E. THE STEP RE-ENTRY EDGE. A non-zero result re-enters the SAME group -----------------
    {
        fixture f;
        g_rec.clear();
        // Group 0's step asks to run again twice, then stops; group 1 stops immediately.
        g_rec.step_results               = {1u, 1u, 0u, 0u};
        player_data &pd                  = armed(f, ME);
        pd.ai_group_count                = 2;
        pd.ai_groups[0].task_queue_count = 1;
        pd.ai_groups[1].task_queue_count = 1;

        const detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);

        ck(r.step_reentries == 2 && g_rec.steps.size() == 4,
           "unit_group_tick: a non-zero step result re-enters the SAME group rather than advancing");
        ck(g_rec.steps.size() == 4 && g_rec.steps[0].group == 0 && g_rec.steps[1].group == 0 &&
               g_rec.steps[2].group == 0 && g_rec.steps[3].group == 1,
           "unit_group_tick: only a ZERO step result advances to the next group");
        ck(g_rec.activates.size() == 2,
           "unit_group_tick: a re-entry does not re-activate an already-active group");
    }

    // ---- F. THE POST-ACTIVATE RE-ENTRY EDGE, and the queue re-test that goes with it -----------
    //
    // Two activate calls leave the flag clear, so the body re-enters from the top twice -- which
    // means task_queue_count is re-read each time. The third activation sets it and the group is
    // stepped once.
    {
        fixture f;
        g_rec.clear();
        g_rec.step_results               = {0u};
        g_rec.activate_clear_count       = 2;
        player_data &pd                  = armed(f, ME);
        pd.ai_group_count                = 1;
        pd.ai_groups[0].task_queue_count = 1;

        const detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);

        ck(r.reactivations == 2 && g_rec.activates.size() == 3,
           "unit_group_tick: the body re-enters until activation actually sets the flag");
        ck(r.bodies == 3 && g_rec.steps.size() == 1,
           "unit_group_tick: every re-entry goes to the TOP of the body, so task_queue_count is "
           "re-tested each time");
    }

    // ---- G. a group whose queue drains mid-turn ends its turn on the re-entry -------------------
    //
    // The step asks to run again, but by then the handler has emptied the queue. Because the
    // re-entry re-reads task_queue_count, the group's turn ends there rather than stepping again --
    // which is why the two re-entry edges both target the body TOP and not the step.
    {
        fixture f;
        g_rec.clear();
        player_data &pd                  = armed(f, ME);
        pd.ai_group_count                = 1;
        pd.ai_groups[0].task_queue_count = 1;

        // A step that empties the queue AND asks to be run again. The recorder's canned-result
        // stub cannot express "and the state changed", so this one case binds its own.
        struct drain_step {
            static uint32_t call(uint32_t player, int32_t group) {
                g_rec.steps.push_back({(int32_t)player, group, 1u});
                if (g_task_owner) g_task_owner->ai_groups[group].task_queue_count = 0;
                return 1u;
            }
        };
        ai_calls draining        = stub_calls();
        draining.group_task_step = &drain_step::call;

        const detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), draining, ME);

        ck(g_rec.steps.size() == 1 && r.step_reentries == 1 && r.idle_groups == 1,
           "unit_group_tick: a queue drained by the handler ends the group's turn at the re-entry, "
           "without the step result having to say so");
    }

    // ---- H. no groups at all -------------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd   = armed(f, ME);
        pd.ai_group_count = 0;

        const detail::group_tick_report r =
            detail::unit_group_tick(f.view(), f.store(), stub_calls(), ME);
        ck(r.phase_gate && r.disband_sweeps == 1 && r.bodies == 0 && g_rec.activates.empty() &&
               g_rec.group_removes.empty(),
           "unit_group_tick: with no groups the reaper sweeps once and pass 2 does nothing");
    }

    g_group_remove_owner = nullptr;
    g_task_owner         = nullptr;
}

// ---- 49. batch C layer 2: the three group-FORMATION functions ---------------------------------
//
// Three near-identical bodies, so every case here is written to separate them: the goal value, the
// pool group index, the threshold comparison and the two-task payload. A test that only asserted
// "a group was formed" would pass for a body that formed a PATROL group out of pool 3.
//
// MUTATION-CHECKED 2026-08-05 (tools/oneoff/2026-08-05-mutate-c2.py) -- see that driver for the
// mutation-to-assertion map.
void test_group_form() {
    const int32_t ME = 2; // not 0: a player-stride bug lands on the right row for player 0
    const int32_t HX = 41, HY = 23;

    auto seed = [&](fixture &f) -> player_data & {
        g_rec.clear();
        player_data &pd   = f.players[ME];
        pd.ai_home_tile_x = HX;
        pd.ai_home_tile_y = HY;
        pd.ai_group_count = 4;
        return pd;
    };

    // ---- A. form_standby_from_pool3: goal 0xa, pool group 3, "not empty", tasks 0x12 + 0x17 ----
    {
        fixture      f;
        player_data &pd              = seed(f);
        pd.ai_groups[1].goal         = 0x0a; // one already exists
        pd.ai_groups[3].member_count = 7;
        detail::group_form_report r =
            detail::form_standby_from_pool3(f.view(), f.store(), stub_calls(), ME);
        ck(r.census_blocked && !r.formed && g_rec.create_players.empty() &&
               g_rec.task_pushes.empty(),
           "form_standby_from_pool3: an existing goal-0xa group blocks the whole body");
        ck(r.census_scanned == 2,
           "form_standby_from_pool3: the census STOPS at the first match rather than counting");

        // The census bound is ai_group_count, not the array extent: a matching group ABOVE the
        // count must be invisible. (M1 raises the bound to 32 and this is what fails.)
        f.reset();
        player_data &pd2              = seed(f);
        pd2.ai_groups[6].goal         = 0x0a;
        pd2.ai_groups[3].member_count = 7;
        g_rec.create_results          = {9};
        r                             = detail::form_standby_from_pool3(f.view(), f.store(), stub_calls(), ME);
        ck(!r.census_blocked && r.formed && r.census_scanned == 4,
           "form_standby_from_pool3: the census is bounded by ai_group_count, not by the extent");
        ck(pd2.ai_groups[9].goal == 0x0a && pd2.ai_groups[9].active_member_count == 7,
           "form_standby_from_pool3: it stamps goal 0xa and the POOL headcount into the new group");
        ck(g_rec.task_pushes.size() == 2, "form_standby_from_pool3: exactly two tasks are enqueued");
        {
            const auto &t0 = at(g_rec.task_pushes, 0);
            ck(!t0.preempt && t0.player == ME && t0.group == 9 && t0.task_code == 0x12 &&
                   t0.param_4 == 3 && t0.param_5 == HX && t0.param_6 == HY && t0.param_7 == 0 &&
                   t0.param_8 == 0 && t0.param_9 == 7,
               "form_standby_from_pool3: task 0x12 carries pending_param 3, the AI HOME TILE as the "
               "anchor pair, and the pool headcount as sub_code");
            const auto &t1 = at(g_rec.task_pushes, 1);
            ck(!t1.preempt && t1.group == 9 && t1.task_code == 0x17 && t1.param_4 == 0 &&
                   t1.param_5 == 0 && t1.param_6 == 0 && t1.param_9 == 0,
               "form_standby_from_pool3: the second task is 0x17 with every argument zero");
        }
        ck(f.players[ME - 1].ai_group_count == 0 && f.players[ME + 1].ai_group_count == 0 &&
               f.players[ME + 1].ai_groups[9].goal == 0,
           "form_standby_from_pool3: it writes only the addressed player's row");

        // An EMPTY pool blocks it, and the pool is group 3 specifically.
        f.reset();
        player_data &pd3              = seed(f);
        pd3.ai_groups[2].member_count = 99; // patrol's pool
        pd3.ai_groups[4].member_count = 99; // surplus's pool
        pd3.ai_groups[3].member_count = 0;
        r                             = detail::form_standby_from_pool3(f.view(), f.store(), stub_calls(), ME);
        ck(r.pool_blocked && !r.formed && g_rec.create_players.empty(),
           "form_standby_from_pool3: the pool it reads is group 3 -- a full group 2 or 4 does not "
           "unblock it");
        // ONE member is enough: the test is != 0, not >= some floor.
        pd3.ai_groups[3].member_count = 1;
        g_rec.create_results          = {5};
        r                             = detail::form_standby_from_pool3(f.view(), f.store(), stub_calls(), ME);
        ck(r.formed && pd3.ai_groups[5].active_member_count == 1,
           "form_standby_from_pool3: a single pool member is enough -- the gate is != 0");
    }

    // ---- B. form_standby_from_pool3 does NOT check group_create's -1 -------------------------
    //
    // The behaviour form_patrol has and these two do not. A -1 addresses ai_groups[-1], which is
    // 0xa66 bytes before the array and still inside player_data, so the write lands in the record
    // rather than out of bounds -- which is exactly why nothing has ever noticed.
    {
        fixture      f;
        player_data &pd              = seed(f);
        pd.ai_groups[3].member_count = 4;
        g_rec.create_results         = {-1};
        const detail::group_form_report r =
            detail::form_standby_from_pool3(f.view(), f.store(), stub_calls(), ME);
        ck(!r.create_failed && r.formed && r.group_index == -1,
           "form_standby_from_pool3: a failed group_create is NOT checked -- the body carries on");
        ck(g_rec.task_pushes.size() == 2 && at(g_rec.task_pushes, 0).group == -1,
           "form_standby_from_pool3: and it enqueues onto group -1");
        ck(pd.ai_groups[0].goal == 0,
           "form_standby_from_pool3: the -1 stamp lands BEFORE the array, not on group 0");
    }

    // ---- C. form_surplus_from_pool4: goal 8, pool group 4, ">= MIN", tasks 0x13 + 0x15 --------
    {
        fixture      f;
        player_data &pd      = seed(f);
        pd.ai_groups[0].goal = 0x08;
        detail::group_form_report r =
            detail::form_surplus_from_pool4(f.view(), f.store(), stub_calls(), ME);
        ck(r.census_blocked && g_rec.create_players.empty(),
           "form_surplus_from_pool4: an existing goal-8 group blocks the body");

        // THE THRESHOLD BOUNDARY. surplus_min is 5, and the original's test is `< min` -> return,
        // so 4 blocks and 5 proceeds. (M6 flips it to <= and the second half of this fails.)
        f.reset();
        player_data &pd2              = seed(f);
        pd2.ai_groups[4].member_count = 4;
        r                             = detail::form_surplus_from_pool4(f.view(), f.store(), stub_calls(), ME);
        ck(r.pool_blocked && !r.formed,
           "form_surplus_from_pool4: a pool one BELOW the minimum blocks it");
        pd2.ai_groups[4].member_count = 5;
        g_rec.create_results          = {11};
        r                             = detail::form_surplus_from_pool4(f.view(), f.store(), stub_calls(), ME);
        ck(r.formed && !r.pool_blocked,
           "form_surplus_from_pool4: a pool EQUAL to the minimum proceeds -- the gate is `<`");
        ck(pd2.ai_groups[11].goal == 0x08 && pd2.ai_groups[11].active_member_count == 5,
           "form_surplus_from_pool4: it stamps goal 8 and the pool headcount");
        ck(g_rec.task_pushes.size() == 2, "form_surplus_from_pool4: exactly two tasks");
        {
            const auto &t0 = at(g_rec.task_pushes, 0);
            ck(!t0.preempt && t0.task_code == 0x13 && t0.param_4 == 3 && t0.param_5 == HX &&
                   t0.param_6 == HY && t0.param_9 == 5,
               "form_surplus_from_pool4: task 0x13 carries pending_param 3, the home tile and the "
               "pool headcount");
            const auto &t1 = at(g_rec.task_pushes, 1);
            ck(!t1.preempt && t1.task_code == 0x15 && t1.param_4 == 0x183 && t1.param_5 == 0 &&
                   t1.param_9 == 0,
               "form_surplus_from_pool4: the second task is 0x15 with pending_param 0x183 and no "
               "anchors -- NOT the 0x17/0 pair its standby sibling uses");
        }

        // The pool is group 4, not group 3.
        f.reset();
        player_data &pd3              = seed(f);
        pd3.ai_groups[3].member_count = 99;
        pd3.ai_groups[4].member_count = 0;
        r                             = detail::form_surplus_from_pool4(f.view(), f.store(), stub_calls(), ME);
        ck(r.pool_blocked, "form_surplus_from_pool4: the pool it reads is group 4, not group 3");
    }

    // ---- D. form_patrol: goal 5, pool group 2, COUNTS, "> 3 * SIZE", checks -1 ----------------
    {
        fixture      f;
        player_data &pd              = seed(f);
        pd.ai_groups[0].goal         = 0x05;
        pd.ai_groups[2].member_count = 100;
        detail::group_form_report r  = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(!r.census_blocked && r.formed && r.census_matches == 1 && r.census_scanned == 4,
           "form_patrol: it COUNTS goal-5 groups over the whole range -- one is below the max of 2");

        f.reset();
        player_data &pd2              = seed(f);
        pd2.ai_groups[0].goal         = 0x05;
        pd2.ai_groups[3].goal         = 0x05;
        pd2.ai_groups[2].member_count = 100;
        r                             = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(r.census_blocked && r.census_matches == 2 && g_rec.create_players.empty(),
           "form_patrol: TWO goal-5 groups reach the max and block it -- the census does not stop "
           "at the first match");

        // THE CAP GATE IS UNSIGNED (JNC @0x004e7b25 is JAE). With the cap at -1 a SIGNED `>=` blocks
        // every call -- 0 >= -1 -- while the original, comparing unsigned, sees 0 >= 0xffffffff as
        // false and forms the group. The shipped cap is 2, so this input is only reachable in a
        // modded image; it is here because three reimpl-verify reviewers found the draft signed on
        // 2026-08-05 and a check that cannot separate the two readings would not have.
        f.reset();
        player_data &pdu              = seed(f);
        pdu.ai_groups[2].member_count = 100;
        f.patrol_max                  = -1;
        g_rec.create_results          = {8};
        r                             = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(!r.census_blocked && r.formed,
           "form_patrol: the patrol-group cap is compared UNSIGNED -- a cap of -1 does not block");
        f.patrol_max = 2;

        // THE SUPPLY GATE IS STRICTLY GREATER THAN 3 * SIZE (size 3 -> 9 blocks, 10 proceeds).
        f.reset();
        player_data &pd3              = seed(f);
        pd3.ai_groups[2].member_count = 9;
        r                             = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(r.pool_blocked && !r.formed,
           "form_patrol: a pool of exactly 3 * PATROL_GROUP_SIZE is NOT enough -- the gate is `>`");
        pd3.ai_groups[2].member_count = 10;
        g_rec.create_results          = {-1};
        r                             = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(r.create_failed && !r.formed && g_rec.task_pushes.empty(),
           "form_patrol: it CHECKS group_create's -1 and returns without stamping or enqueuing");
        ck(pd3.ai_groups[0].goal == 0 && pd3.ai_groups[1].goal == 0,
           "form_patrol: nothing is written on the create-failed arm");

        g_rec.clear();
        g_rec.create_results = {6};
        r                    = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(r.formed && pd3.ai_groups[6].goal == 0x05,
           "form_patrol: it stamps goal 5 into the created group");
        ck(pd3.ai_groups[6].active_member_count == 3,
           "form_patrol: active_member_count is PATROL_GROUP_SIZE, not the pool headcount");
        ck(g_rec.task_pushes.size() == 2, "form_patrol: exactly two tasks");
        {
            const auto &t0 = at(g_rec.task_pushes, 0);
            ck(!t0.preempt && t0.task_code == 0x10 && t0.param_4 == 0x183 && t0.param_5 == -1 &&
                   t0.param_6 == -1 && t0.param_7 == 0 && t0.param_8 == 0 && t0.param_9 == 3,
               "form_patrol: task 0x10 anchors at (-1, -1), NOT at the home tile, and its sub_code "
               "is PATROL_GROUP_SIZE");
            const auto &t1 = at(g_rec.task_pushes, 1);
            ck(!t1.preempt && t1.task_code == 0x0e && t1.param_4 == 0x193 && t1.param_5 == -1 &&
                   t1.param_6 == -1,
               "form_patrol: the second task is 0x0e with pending_param 0x193 and the same anchors");
            // 100000 & 0xffff == 34464 (as int16, -31072). The truncation happens inside
            // llm_strat_ai_group_task_enqueue, which stores only BX -- see ai_group_form.cpp.
            ck(t1.param_9 == 34464,
               "form_patrol: the wander bounce count is TRUNCATED to 16 bits on the way into the "
               "task record -- 100000 arrives as 34464");
        }

        // The pool is group 2.
        f.reset();
        player_data &pd4              = seed(f);
        pd4.ai_groups[3].member_count = 100;
        pd4.ai_groups[4].member_count = 100;
        pd4.ai_groups[2].member_count = 0;
        r                             = detail::form_patrol(f.view(), f.store(), stub_calls(), ME);
        ck(r.pool_blocked, "form_patrol: the pool it reads is group 2");
    }
}

// ---- 50. batch C layer 2: enter_hold and seed_resolved_target ---------------------------------
//
// Both are a guard over one delegated call, so every case here asserts WHICH call was made with
// WHICH arguments -- a count would pass for a body that enqueued where it should preempt.
void test_group_hold() {
    const int32_t ME = 2, G = 4;

    // ---- A. enter_hold -------------------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd               = f.players[ME];
        pd.ai_groups[G].task_code     = 0x0b;
        pd.ai_groups[G].current_param = 1234;
        detail::group_hold_report r =
            detail::group_enter_hold(f.view(), f.store(), stub_calls(), ME, G);
        ck(r.already_holding && !r.preempted && g_rec.task_pushes.empty(),
           "enter_hold: a group whose LIVE task_code is already 0xb is left alone");

        pd.ai_groups[G].task_code     = 0x07; // wait -- some other live task
        pd.ai_groups[G].pending_param = 5678; // the field it must NOT read
        r                             = detail::group_enter_hold(f.view(), f.store(), stub_calls(), ME, G);
        ck(r.preempted && g_rec.task_pushes.size() == 1,
           "enter_hold: any other live task_code preempts");
        {
            const auto &t = at(g_rec.task_pushes, 0);
            ck(t.preempt, "enter_hold: it FRONT-INSERTS (preempt), it does not append");
            ck(t.player == ME && t.group == G && t.task_code == 0x0b,
               "enter_hold: the preempted task is 0xb on the addressed group");
            ck(t.param_4 == 1234,
               "enter_hold: pending_param comes from the group's CURRENT_PARAM (+0x0e), not from "
               "its pending_param (+0x26)");
            ck(t.param_5 == 0 && t.param_6 == 0 && t.param_7 == 0 && t.param_8 == 0 &&
                   t.param_9 == 0,
               "enter_hold: all five stack arguments are zero");
        }

        // It reads the ADDRESSED group, not group 0 and not a neighbour.
        g_rec.clear();
        pd.ai_groups[0].task_code = 0x0b;
        pd.ai_groups[G].task_code = 0x07;
        r                         = detail::group_enter_hold(f.view(), f.store(), stub_calls(), ME, G);
        ck(r.preempted, "enter_hold: a HOLD on group 0 does not suppress a preempt on group 4");
        ck(f.players[ME + 1].ai_groups[G].task_code == 0,
           "enter_hold: it touches only the addressed player's row");
    }

    // ---- B. seed_resolved_target ---------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd                       = f.players[ME];
        pd.ai_groups[G].resolved_target_ref   = 0;
        pd.ai_groups[G].resolved_target_index = 77; // a live index behind a zero ref
        detail::group_hold_report r =
            detail::group_seed_resolved_target(f.view(), f.store(), stub_calls(), ME, G);
        ck(r.no_target && !r.seeded && g_rec.scan_target_adds.empty(),
           "seed_resolved_target: a zero resolved_target_ref returns false and adds nothing");

        pd.ai_groups[G].resolved_target_ref = 0x42;
        r                                   = detail::group_seed_resolved_target(f.view(), f.store(), stub_calls(), ME, G);
        ck(r.seeded && g_rec.scan_target_adds.size() == 1,
           "seed_resolved_target: a live ref seeds the GLOBAL scan-target scratch and returns true");
        {
            const auto &a = at(g_rec.scan_target_adds, 0);
            ck(a.player == ME && a.ref == 0x42u && a.index == 77,
               "seed_resolved_target: the arguments are (player, REF, INDEX) in that order -- the "
               "ref is the second, not the third");
        }

        // The group index is honoured, and a sibling group's ref does not stand in for it.
        g_rec.clear();
        pd.ai_groups[0].resolved_target_ref = 0x99;
        pd.ai_groups[G].resolved_target_ref = 0;
        r                                   = detail::group_seed_resolved_target(f.view(), f.store(), stub_calls(), ME, G);
        ck(r.no_target && g_rec.scan_target_adds.empty(),
           "seed_resolved_target: it reads the addressed group, not group 0");
    }
}


// ---- batch C layer 2: the group TASK MACHINE ---------------------------------------------------
//
// The one thing worth saying about the shape of these tests: the DISPATCH TABLES are checked as
// TABLES, by looping over every code 0..0x18 and asserting which arm ran. A transposed entry is the
// most likely error here and the least visible one -- it typechecks, it is not a divergence on any
// scenario that never queues that task code, and a per-case test would only cover the cases someone
// thought to write.
void test_group_task_machine() {
    const int32_t ME = 3, G = 6;

    // The expected activation arm per task code, expressed INDEPENDENTLY of the translation's own
    // table: 0 means "no arm runs". Written out from the jump table read at 0x004eb0cd, which is
    // the spec; if this array and ACTIVATE_ARMS were derived from each other the test would prove
    // only that the file is self-consistent.
    static const int ACT_EXPECT[0x19] = {
        0,
        0,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x07,
        0x08,
        0x09,
        0,
        0x0b,
        0x03 /* 0x0c shares 0x03's arm */,
        0x0d,
        0x0e,
        0x0f,
        0x0f,
        0x0f,
        0x12,
        0x13,
        0x14,
        0x15,
        0x16,
        0x17,
        0x18,
    };

    // ---- A. activate: the whole dispatch table -------------------------------------------------
    {
        fixture f;
        for (int code = 0; code <= 0x18; ++code) {
            g_rec.clear();
            g_rec.task_arms.clear();
            player_data &pd           = f.players[ME];
            pd.ai_groups[G].task_code = (int16_t)code;
            detail::group_task_report r =
                detail::group_task_activate(f.view(), f.store(), stub_calls(), ME, G);
            const int want = ACT_EXPECT[code];
            if (want == 0) {
                ck(r.table_no_op && !r.dispatched && g_rec.task_arms.empty(),
                   "task_activate: an in-range code whose table slot is the epilogue runs no arm");
            } else {
                ck(r.dispatched && g_rec.task_arms.size() == 1 &&
                       (int)g_rec.task_arms[0].result == want,
                   "task_activate: every task code reaches the arm its jump table names");
                // The two argument-less no-op stubs are the only arms that cannot carry a
                // player/group, and that is itself the assertion that the right thunk was used.
                const bool argless = (want == 0x07 || want == 0x0b);
                ck(g_rec.task_arms.size() == 1 &&
                       (argless ? (g_rec.task_arms[0].player == -1)
                                : (g_rec.task_arms[0].player == ME &&
                                   g_rec.task_arms[0].group == G)),
                   "task_activate: the arm is called with the addressed player and group");
            }
        }
    }

    // ---- B. activate: the stamps, and that they precede the bound test --------------------------
    {
        fixture      f;
        player_data &pd = f.players[ME];
        pd.ai_clock     = 1234.5f; // exactly representable, so a float/double mix-up shows as noise
        for (int32_t code : {(int32_t)0x02, (int32_t)0x19, (int32_t)0xffff}) {
            g_rec.clear();
            g_rec.task_arms.clear();
            pd.ai_groups[G].task_code       = (int16_t)code;
            pd.ai_groups[G].pending_param   = 4242;
            pd.ai_groups[G].current_param   = 0;
            pd.ai_groups[G].active_flag     = 0;
            pd.ai_groups[G].task_start_time = 0.0;
            detail::group_task_report r =
                detail::group_task_activate(f.view(), f.store(), stub_calls(), ME, G);
            ck(r.stamped && pd.ai_groups[G].current_param == 4242 &&
                   pd.ai_groups[G].active_flag == 1 &&
                   pd.ai_groups[G].task_start_time == 1234.5,
               "task_activate: the three stamps run on EVERY call, out-of-range codes included");
            ck(pd.ai_groups[G].pending_param == 4242,
               "task_activate: the copy is pending_param -> current_param, not the reverse");
            // 0x19 and 0xffff are both ABOVE 0x18 read as a uint16_t. 0xffff stored into the int16_t
            // field is -1: a SIGNED bound test would let it index the table at a negative offset.
            if (code != 0x02)
                ck(r.out_of_range && !r.dispatched && g_rec.task_arms.empty(),
                   "task_activate: the 0x18 bound is UNSIGNED over 16 bits, so -1 is out of range");
        }
        ck(f.players[ME + 1].ai_groups[G].active_flag == 0 &&
               f.players[ME].ai_groups[G + 1].active_flag == 0,
           "task_activate: it stamps only the addressed player's addressed group");
    }

    // ---- C. step: the early return and the indeterminate arm ------------------------------------
    {
        fixture      f;
        player_data &pd = f.players[ME];
        g_rec.clear();
        pd.ai_groups[G].task_queue_count = 0;
        pd.ai_groups[G].task_code        = 0x02; // a code that WOULD call a predicate
        detail::group_task_report r =
            detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
        ck(r.idle && r.ret == 1 && g_rec.preds.empty() && g_rec.dequeues.empty(),
           "task_step: task_queue_count == 0 returns 1 before reading task_code at all");

        for (int32_t code : {(int32_t)1, (int32_t)0x19, (int32_t)0xffff}) {
            g_rec.clear();
            pd.ai_groups[G].task_queue_count = 1;
            pd.ai_groups[G].task_code        = (int16_t)code;
            r                                = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0xdeadbeefu);
            ck(r.indeterminate && r.ret == 0xdeadbeefu && g_rec.dequeues.empty(),
               "task_step: task code 1 and anything above 0x18 return the caller's EBX and do "
               "nothing else");
        }
    }

    // ---- D. step: the return-zero codes and the bare dequeue ------------------------------------
    {
        fixture      f;
        player_data &pd                  = f.players[ME];
        pd.ai_groups[G].task_queue_count = 1;
        for (int code : {0x00, 0x05, 0x08, 0x09, 0x0e, 0x14, 0x15, 0x17, 0x18}) {
            g_rec.clear();
            pd.ai_groups[G].task_code = (int16_t)code;
            detail::group_task_report r =
                detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.ret == 0 && !r.dequeued && g_rec.preds.empty() && g_rec.docks.empty(),
               "task_step: the nine return-zero codes call nothing and leave the task running");
        }
        g_rec.clear();
        pd.ai_groups[G].task_code = 0x0a;
        detail::group_task_report r =
            detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
        ck(r.ret == 1 && r.dequeued && g_rec.dequeues.size() == 1 &&
               g_rec.dequeues[0].player == ME && g_rec.dequeues[0].group == G,
           "task_step: code 0x0a dequeues unconditionally and returns 1");
    }

    // ---- E. step: the four progress predicates --------------------------------------------------
    {
        fixture      f;
        player_data &pd                  = f.players[ME];
        pd.ai_groups[G].task_queue_count = 1;
        // code -> the predicate id the st_* stub records (owner_mask for the two hostile scans)
        const int codes[] = {0x02, 0x03, 0x04, 0x0c, 0x06, 0x07, 0x0b, 0x0d};
        const int want[]  = {1, 1, 1, 1, 0x40, 0xc0, 3, 2};
        for (int i = 0; i < 8; ++i) {
            g_rec.clear();
            g_rec.pred_result         = 0;
            pd.ai_groups[G].task_code = (int16_t)codes[i];
            detail::group_task_report r =
                detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(g_rec.preds.size() == 1 && (int)g_rec.preds[0].result == want[i],
               "task_step: each polling code reaches the predicate its jump table names, and the "
               "two hostile scans differ only in owner_mask (0x40 vs 0xc0)");
            ck(r.ret == 0 && !r.dequeued,
               "task_step: a predicate answering 0 leaves the task running and returns 0");

            g_rec.clear();
            g_rec.pred_result = 7; // NOT 1 -- the original returns the predicate's own value
            r                 = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.ret == 7u && r.dequeued && g_rec.dequeues.size() == 1,
               "task_step: a non-zero predicate dequeues and RETURNS THE PREDICATE'S VALUE, not 1");
        }
    }

    // ---- F. step: the two dock arms ------------------------------------------------------------
    {
        fixture      f;
        player_data &pd                  = f.players[ME];
        pd.ai_groups[G].task_queue_count = 1;

        // F1. The DISCARD arm (0x0f/0x10/0x11): it still calls dock_slot_is_busy, and it returns 1
        //     whatever the answer.
        for (int code : {0x0f, 0x10, 0x11}) {
            for (int busy : {0, 1}) {
                g_rec.clear();
                g_rec.dock_busy                   = busy;
                pd.ai_groups[G].task_code         = (int16_t)code;
                pd.ai_groups[G].reinforce_pending = 0;
                pd.ai_groups[G].active_sub_code   = 0;
                pd.ai_groups[G].active_flag       = 1;
                detail::group_task_report r =
                    detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
                ck(g_rec.docks.size() == 1,
                   "task_step: the discard arm still CALLS dock_slot_is_busy -- the callee is not "
                   "pure, so dropping the call because its result is unused would be wrong");
                ck(r.ret == 1 && r.dequeued && pd.ai_groups[G].active_flag == 1,
                   "task_step: with nothing pending the discard arm dequeues and returns 1 whatever "
                   "the dock says");
            }
        }
        // F2. The USE arm (0x12/0x13): the dequeue AND the return both follow (busy == 0).
        for (int code : {0x12, 0x13}) {
            g_rec.clear();
            g_rec.dock_busy                   = 0;
            pd.ai_groups[G].task_code         = (int16_t)code;
            pd.ai_groups[G].reinforce_pending = 0;
            pd.ai_groups[G].active_sub_code   = 0;
            pd.ai_groups[G].active_flag       = 1;
            detail::group_task_report r =
                detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.ret == 1 && r.dequeued, "task_step: dock free + nothing pending -> dequeue, ret 1");

            g_rec.clear();
            g_rec.dock_busy = 1;
            r               = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.ret == 0 && !r.dequeued && pd.ai_groups[G].active_flag == 1,
               "task_step: a BUSY dock leaves the task alone and returns 0 -- this is the one place "
               "the two dock arms disagree");
        }
        // F3. The pending sum clears active_flag INSTEAD of dequeuing, on both arms, and it is two
        //     16-bit fields zero-extended: active_sub_code == -1 is 0xffff, i.e. pending.
        for (int code : {0x0f, 0x12}) {
            g_rec.clear();
            g_rec.dock_busy                   = 0;
            pd.ai_groups[G].task_code         = (int16_t)code;
            pd.ai_groups[G].reinforce_pending = 0;
            pd.ai_groups[G].active_sub_code   = -1;
            pd.ai_groups[G].active_flag       = 1;
            detail::group_task_report r =
                detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.cleared_flag && pd.ai_groups[G].active_flag == 0 && !r.dequeued,
               "task_step: a non-zero reinforce_pending + active_sub_code clears active_flag and "
               "does NOT dequeue -- and the sum is UNSIGNED 16-bit, so -1 counts as pending");
            ck(r.ret == 1, "task_step: the pending path still returns (busy == 0)");

            g_rec.clear();
            pd.ai_groups[G].reinforce_pending = 2;
            pd.ai_groups[G].active_sub_code   = 0;
            pd.ai_groups[G].active_flag       = 1;
            r                                 = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.cleared_flag && !r.dequeued,
               "task_step: reinforce_pending alone is enough to take the clear-flag path");

            // THE SEPARATING INPUT FOR THE WIDTH QUESTION. 1 + (-1) is 0 read as two signed ints
            // and 0x10000 read as two zero-extended 16-bit ones. The original MOVZXes both
            // (0x004e9740/0x004e974a, 0x004e97a4/0x004e97ab), so this is the PENDING path; a signed
            // sum would fall through to the dequeue and every other case in this block would still
            // pass.
            g_rec.clear();
            pd.ai_groups[G].reinforce_pending = 1;
            pd.ai_groups[G].active_sub_code   = -1;
            pd.ai_groups[G].active_flag       = 1;
            r                                 = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
            ck(r.cleared_flag && !r.dequeued && pd.ai_groups[G].active_flag == 0,
               "task_step: the pending sum is two ZERO-EXTENDED 16-bit fields -- 1 + (-1) is "
               "0x10000, not 0, so it is still pending");
        }
    }

    // ---- G. step: the engage arm (code 0x16) ----------------------------------------------------
    {
        fixture      f;
        player_data &pd                  = f.players[ME];
        pd.ai_groups[G].task_queue_count = 1;
        pd.ai_groups[G].task_code        = 0x16;

        // G1. NO TARGET INDEX -> treated as finished: clear both fields, dequeue, return 1. Note the
        //     gate is resolved_target_INDEX, not the ref.
        g_rec.clear();
        pd.ai_groups[G].resolved_target_ref   = 0x55;
        pd.ai_groups[G].resolved_target_index = 0;
        detail::group_task_report r =
            detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
        ck(r.ret == 1 && r.dequeued && r.cleared_target &&
               pd.ai_groups[G].resolved_target_ref == 0 &&
               pd.ai_groups[G].resolved_target_index == 0,
           "task_step: the engage arm gates on resolved_target_INDEX, and a zero index finishes the "
           "task even with a non-zero ref still set");

        // G2. A LIVE target -> the task keeps running and nothing is cleared. THE ALIVENESS CALL
        //     TAKES active_param_a/_b, not resolved_target_ref/_index: the fixture makes the
        //     resolved_* pair the DEAD one, so a translation that passed those would report done.
        g_rec.clear();
        g_rec.dead_ref                        = 0x99;
        g_rec.dead_index                      = 11;
        pd.ai_groups[G].resolved_target_ref   = 0x99;
        pd.ai_groups[G].resolved_target_index = 11;
        pd.ai_groups[G].active_param_a        = 0x11;
        pd.ai_groups[G].active_param_b        = 22;
        r                                     = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
        ck(r.ret == 0 && !r.dequeued && !r.cleared_target &&
               pd.ai_groups[G].resolved_target_ref == 0x99,
           "task_step: the engage arm's aliveness test reads active_param_a/_b (+0x2f/+0x33), NOT "
           "resolved_target_ref/_index (+0x1e/+0x22)");

        // G3. A DEAD target -> finished.
        g_rec.clear();
        g_rec.dead_ref                        = 0x11;
        g_rec.dead_index                      = 22;
        pd.ai_groups[G].resolved_target_ref   = 0x99;
        pd.ai_groups[G].resolved_target_index = 11;
        pd.ai_groups[G].active_param_a        = 0x11;
        pd.ai_groups[G].active_param_b        = 22;
        r                                     = detail::group_task_step(f.view(), f.store(), stub_calls(), ME, G, 0u);
        ck(r.ret == 1 && r.dequeued && r.cleared_target &&
               pd.ai_groups[G].resolved_target_ref == 0 &&
               pd.ai_groups[G].resolved_target_index == 0,
           "task_step: a dead target clears BOTH resolved_target fields, then dequeues");
    }
}

// ---- batch C layer 2: the INVASION LAUNCH ------------------------------------------------------
//
// WHY THIS TEST EXISTS AT ALL. llm_strat_ai_invasion_launch_attack_group was armed in the
// 2026-08-05 all-AI soak and REACHED ZERO TIMES -- correctly, since the scenario has no
// invasion-force player (ai_invasion_force != 0 is set only by llm_strat_spawn_invasion_force,
// which nothing in the soak's start path calls). It was recorded T3 with that measurement written
// down. The body is pure over player_data plus a stubbed ai_calls -- no map, no render, no clock --
// so the cheap move is to cover it here rather than to keep hunting a scenario. This is the
// reimpl-loop's "prefer the offline oracle" step, and it is what takes the function off T3.
//
// The two things the rig could never have separated even if it HAD reached the site are the ones
// with the most weight below: the UNSIGNED weakest-opponent compare, and the FULL-DWORD
// target_player_id write. Both agree with the wrong reading on ordinary data.
void test_invasion_launch() {
    const int32_t ME = 2;
    // The index st_group_create hands back. It MUST NOT be 0: the launch path drains
    // ai_groups[0] into the created group, so group 0 as the destination makes the
    // decrement and the increment cancel and the loop never ends (see the runaway net in
    // st_group_member_move). The empty-queue default of that stub IS 0, so every
    // g_rec.clear() in this test re-arms the queue.
    const int32_t NEW_GROUP = 6;

    // ---- A. the three guards -------------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.create_results = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        player_data &pd      = f.players[ME];
        // Several sub-cases below fall through to the LAUNCH path, which drains ai_groups[0]; the
        // stub can only end that loop if it can really move members. See st_group_member_move.
        g_member_move_owner = &pd;
        g_member_move_units = &f.units[(size_t)ME * UNITS_PER_PLAYER];

        // Guard 1: still reinforcing. Set up an OTHERWISE-LAUNCHABLE state so the guard is the only
        // reason nothing happens -- a test whose other preconditions are also unmet would pass for a
        // body that had no guard 1 at all.
        f.active_players             = 4;
        pd.ai_groups[0].member_count = 3;
        pd.ai_invasion_points        = 1;
        detail::invasion_launch_report r =
            detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(r.invasion_points_pending && !r.launched && g_rec.create_players.empty(),
           "invasion_launch: a non-zero ai_invasion_points returns before group_create");
        ck(r.census_scanned == 0,
           "invasion_launch: guard 1 returns BEFORE the census, not after it");

        // Guard 2: nothing staged. Same launchable state minus the members.
        g_rec.clear();
        g_rec.create_results         = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        pd.ai_invasion_points        = 0;
        pd.ai_groups[0].member_count = 0;
        r                            = detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(r.home_group_empty && !r.launched && g_rec.create_players.empty(),
           "invasion_launch: an empty ai_groups[0] returns before group_create");

        // Guard 3: the census. It scans 0 .. ai_group_count and bails on goal 3 or 0xb.
        g_rec.clear();
        g_rec.create_results         = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        pd.ai_groups[0].member_count = 3;
        pd.ai_group_count            = 0;
        r                            = detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(!r.attack_already_underway && r.census_scanned == 0 && r.launched,
           "invasion_launch: ai_group_count 0 scans nothing and proceeds");

        for (const int16_t goal : {(int16_t)3, (int16_t)0x0b}) {
            g_rec.clear();
            g_rec.create_results = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
            std::memset(&pd.ai_groups[0], 0, sizeof(unit_group) * 4);
            pd.ai_groups[0].member_count = 3;
            pd.ai_group_count            = 3;
            pd.ai_groups[2].goal         = goal;
            r                            = detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
            ck(r.attack_already_underway && !r.launched && g_rec.create_players.empty(),
               goal == 3 ? "invasion_launch: an existing goal-3 group aborts the census"
                         : "invasion_launch: an existing goal-0xb group aborts the census");
        }

        // The bound is EXCLUSIVE: a marked group AT ai_group_count is out of range and must not be
        // seen. This is the assertion that separates `g < count` from `g <= count`.
        g_rec.clear();
        g_rec.create_results = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        std::memset(&pd.ai_groups[0], 0, sizeof(unit_group) * 4);
        pd.ai_groups[0].member_count = 3;
        pd.ai_group_count            = 2;
        pd.ai_groups[2].goal         = 3; // one past the end
        r                            = detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(!r.attack_already_underway && r.census_scanned == 2,
           "invasion_launch: the census bound is EXCLUSIVE -- a goal-3 group AT ai_group_count is "
           "not scanned");

        // And a goal that is neither 3 nor 0xb does not abort -- e.g. 0xa, which is adjacent to 0xb
        // and is what a `>= 3` or a `goal & 3` reading would wrongly catch.
        g_rec.clear();
        g_rec.create_results = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        std::memset(&pd.ai_groups[0], 0, sizeof(unit_group) * 4);
        pd.ai_groups[0].member_count = 3;
        pd.ai_group_count            = 3;
        pd.ai_groups[1].goal         = 0x0a;
        pd.ai_groups[2].goal         = 4;
        r                            = detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(!r.attack_already_underway && r.launched,
           "invasion_launch: goals 0xa and 4 are not attack goals and do not abort the census");
        g_member_move_owner = nullptr;
        g_member_move_units = nullptr;
    }

    // ---- B. the weakest-opponent pick ----------------------------------------------------------
    //
    // THE COMPARE IS UNSIGNED (CMP/JNC @0x004e8ae6). total_unit_power is a signed field, so a
    // negative value -- which a damaged/underflowed assessment can hold -- reads as ~4.29e9 and is
    // the STRONGEST candidate, not the weakest. A signed reading picks it. That is the whole point
    // of slot 3 below.
    {
        fixture f;
        g_rec.clear();
        g_rec.create_results                           = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        player_data &pd                                = f.players[ME];
        g_member_move_owner                            = &pd; // both sub-cases here reach the drain -- see st_group_member_move
        g_member_move_units                            = &f.units[(size_t)ME * UNITS_PER_PLAYER];
        pd.ai_groups[0].member_count                   = 1;
        pd.ai_group_count                              = 0;
        f.active_players                               = 5;
        pd.ai_opponent_assessments[0].total_unit_power = 500;
        pd.ai_opponent_assessments[1].total_unit_power = 300; // the true unsigned minimum
        pd.ai_opponent_assessments[2].total_unit_power = 1;   // SELF -- must be skipped
        pd.ai_opponent_assessments[3].total_unit_power = -1;  // 0xffffffff: the unsigned MAXIMUM
        pd.ai_opponent_assessments[4].total_unit_power = 900;
        // No hostility filter: mark the winner FRIENDLY. The sibling army_milestone scan skips on
        // this field; this one has no such test in the bytes, so a translation that copied the
        // sibling would pick 0 instead of 1.
        pd.ai_player_relation[1] = 1;
        detail::invasion_launch_report r =
            detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(r.target_player_id == 1,
           "invasion_launch: the weakest opponent is picked UNSIGNED (a negative total_unit_power "
           "is the strongest, not the weakest) and with NO ai_player_relation filter");

        // Self is skipped even when it is the outright minimum -- covered by slot 2 above, asserted
        // separately so a failure names the right rule.
        ck(r.target_player_id != ME, "invasion_launch: the scan skips the acting player");

        // Nobody else active -> -1, and the body still proceeds (there is no early return for it).
        //
        // THE ACTOR HAS TO BE SLOT 0 FOR THIS CASE, not ME. active_player_count bounds the scan from
        // ZERO UP, so with the actor at slot 2 an active count of 1 still leaves slot 0 in range and
        // eligible: the first draft of this check asserted -1 with (ME=2, count=1) and correctly
        // FAILED, because the body picked player 0. The only honest "no other candidate" shape is an
        // actor that is itself the whole of the active range.
        const int32_t SOLO = 0;
        g_rec.clear();
        g_rec.create_results           = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        player_data &solo              = f.players[SOLO];
        g_member_move_owner            = &solo;
        g_member_move_units            = &f.units[(size_t)SOLO * UNITS_PER_PLAYER];
        f.active_players               = 1; // slot 0 only -- and slot 0 is the actor
        solo.ai_groups[0].member_count = 1;
        r                              = detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)SOLO);
        ck(r.launched && r.target_player_id == -1,
           "invasion_launch: with no other candidate the target stays -1 AND the launch proceeds "
           "(unlike army_milestone, this function has no bail-out for that case)");
        g_member_move_owner = nullptr;
        g_member_move_units = nullptr;
    }

    // ---- C. group_create == -1 -- the one real early return past the census --------------------
    {
        fixture f;
        g_rec.clear();
        g_rec.create_results         = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        player_data &pd              = f.players[ME];
        pd.ai_groups[0].member_count = 2;
        f.active_players             = 3;
        g_rec.create_results         = {-1};
        pd.ai_groups[5].goal         = 0x77; // poison: a -1 index must not be written through
        detail::invasion_launch_report r =
            detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(r.create_failed && !r.launched && g_rec.create_players.size() == 1,
           "invasion_launch: group_create returning -1 returns without stamping or draining");
        ck(g_rec.member_moves.empty() && g_rec.task_pushes.empty(),
           "invasion_launch: the -1 return happens BEFORE the drain and the two enqueues");
        ck(pd.ai_groups[0].member_count == 2,
           "invasion_launch: the -1 return leaves ai_groups[0] untouched");
    }

    // ---- D. the full launch --------------------------------------------------------------------
    {
        const int32_t G = 6;
        fixture       f;
        g_rec.clear();
        g_rec.create_results = {NEW_GROUP}; // clear() forgets it, and 0 == the drain SOURCE
        player_data &pd      = f.players[ME];
        g_rec.create_results = {G};
        // Make the stub really move units, so the drain loop's `while (member_count != 0)` can end
        // and its ORDER is observable. Three members, linked 11 -> 12 -> 13.
        g_member_move_owner                   = &pd;
        g_member_move_units                   = &f.units[(size_t)ME * UNITS_PER_PLAYER];
        pd.ai_groups[0].member_count          = 3;
        pd.ai_groups[0].head_unit             = 11;
        g_member_move_units[11].ai_group_next = 12;
        g_member_move_units[12].ai_group_next = 13;
        g_member_move_units[13].ai_group_next = 0;

        // Slots 0 and 1 are both eligible (the actor is slot 2), and slot 0 is made the weaker of
        // the two so the expected target is 0 -- which is what makes the poison below discriminate:
        // a WORD-width store of 0 leaves 0x11110000, a DWORD store leaves 0.
        f.active_players                               = 2;
        pd.ai_opponent_assessments[0].total_unit_power = 7;
        pd.ai_opponent_assessments[1].total_unit_power = 9;
        // POISON the whole dword: target_player_id is an int32 and a translation that wrote a WORD
        // (which is what its Ghidra type said until 2026-08-01) would leave the top half standing.
        pd.ai_groups[G].target_player_id = 0x11112222;

        detail::invasion_launch_report r =
            detail::invasion_launch_attack_group(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        g_member_move_owner = nullptr;
        g_member_move_units = nullptr;

        ck(r.launched && r.group_index == G && r.units_moved == 3 && r.tasks_enqueued == 2,
           "invasion_launch: the full path creates, drains three members and enqueues twice");
        ck(pd.ai_groups[G].goal == 0x0b, "invasion_launch: the new group's goal is 0xb");
        ck(pd.ai_groups[G].target_player_id == 0,
           "invasion_launch: target_player_id is a FULL DWORD write -- a word-width store would "
           "leave 0x11110000 in this field");
        ck(pd.ai_groups[0].member_count == 0,
           "invasion_launch: ai_groups[0] is drained COMPLETELY, not partially");

        ck(g_rec.member_moves.size() == 3, "invasion_launch: exactly three moves");
        if (g_rec.member_moves.size() == 3) {
            for (size_t i = 0; i < 3; ++i) {
                const auto &m = at(g_rec.member_moves, i);
                ck(m.player == (uint32_t)ME && m.src == 0 && m.dst == G,
                   "invasion_launch: each move is (player, src=0, dst=new group) -- not transposed");
            }
            ck(at(g_rec.member_moves, 0).unit_id == 11 && at(g_rec.member_moves, 1).unit_id == 12 &&
                   at(g_rec.member_moves, 2).unit_id == 13,
               "invasion_launch: the drain re-reads head_unit every iteration, so it walks 11, 12, "
               "13 rather than moving the same unit three times");
        }

        ck(g_rec.task_pushes.size() == 2, "invasion_launch: exactly two task enqueues");
        if (g_rec.task_pushes.size() == 2) {
            const auto &t0 = at(g_rec.task_pushes, 0);
            const auto &t1 = at(g_rec.task_pushes, 1);
            ck(!t0.preempt && !t1.preempt,
               "invasion_launch: both are APPENDS (enqueue), not front-inserts");
            ck(t0.task_code == 0x18 && t1.task_code == 0,
               "invasion_launch: the codes are 0x18 (attack_random) then 0 -- the second is the "
               "park code and is not a typo");
            ck(t0.param_4 == 0x183 && t1.param_4 == 0x183,
               "invasion_launch: both carry pending_param 0x183");
            ck(t0.group == G && t1.group == G && t0.player == ME && t1.player == ME,
               "invasion_launch: both address the NEW group, not group 0");
            ck(t0.param_5 == 0 && t0.param_6 == 0 && t0.param_7 == 0 && t0.param_8 == 0 &&
                   t0.param_9 == 0 && t1.param_5 == 0 && t1.param_6 == 0 && t1.param_7 == 0 &&
                   t1.param_8 == 0 && t1.param_9 == 0,
               "invasion_launch: all ten stack arguments across the two calls are zero");
        }

        // It touches only the acting player's row.
        ck(f.players[ME + 1].ai_groups[G].goal == 0 &&
               f.players[ME + 1].ai_groups[0].member_count == 0,
           "invasion_launch: the neighbouring player_data row is untouched");
    }
}

// ---- batch C layer 2: REDISTRIBUTE -- and this one exists because the RIG COULD NOT REACH IT ----
//
// The 2026-08-05 all-AI soak armed llm_strat_ai_group_redistribute_units and reached it 100-199
// times over 14000 steps with zero divergence -- and that verdict was nearly worthless, because of
// those calls 25 returned at guard 1 and the other 75 went down the excess-split path and were
// rejected by its guards. NOT ONE writing branch executed. A shadow site compares region bytes, so
// a run in which neither arm writes anything is clean by construction: exactly the vacuous-green
// class this suite exists to refuse.
//
// So the MERGE half -- the centroid call, the transfer cap, the move loop, the type predicate and
// the reinforce_pending clear, i.e. everything this function actually does -- is covered here
// instead. The kind==0 arm's call into group_split_off_create stays un-shadowable either way (three
// uninitialised original frame words); what this test CAN pin is that the standins land in the
// right ARGUMENT POSITIONS, which is a real transposition risk the rig cannot see.
void test_group_redistribute() {
    const int32_t ME = 3, G = 5;

    auto arm = [&](fixture &f) {
        g_member_move_owner = &f.players[ME];
        g_member_move_units = &f.units[(size_t)ME * UNITS_PER_PLAYER];
    };
    auto disarm = [] {
        g_member_move_owner = nullptr;
        g_member_move_units = nullptr;
    };

    // ---- A. the two guards ---------------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        arm(f);
        player_data &pd = f.players[ME];

        pd.ai_groups[G].reinforce_pending = 0;
        detail::redistribute_report r     = detail::group_redistribute_units(
            f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0, 0, 0);
        ck(r.no_reinforce_pending && g_rec.split_offs.empty() && g_rec.centroid_calls.empty(),
           "redistribute: reinforce_pending == 0 returns before everything");

        g_rec.clear();
        pd.ai_groups[G].reinforce_pending = 4;
        pd.ai_groups[G].goal              = 0x0b;
        r                                 = detail::group_redistribute_units(f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0,
                                                                             0, 0);
        ck(r.already_attacking && g_rec.split_offs.empty(),
           "redistribute: goal 0xb returns before the kind ladder");
        disarm();
    }

    // ---- B. the goal -> (kind, pool) ladder, ENUMERATED --------------------------------------
    //
    // Written out from the CMP/JC/JBE chain at 0x004e6adb-0x004e6b1f rather than from the draft, so
    // agreement is evidence rather than tautology. 0xb is excluded -- guard 2 above eats it.
    {
        struct
        {
            int16_t goal;
            int32_t kind, pool;
        } const EXPECT[] = {
            {0, 0, 2},
            {1, 0, 2},
            {2, 0, 2},
            {3, 0, 2},
            {4, 0, 2},
            {5, 0, 2},
            {6, 0, 2},
            {7, 1, 3},
            {8, 2, 4},
            {9, 0, 2},
            {0x0a, 1, 3},
            {0x0c, 0, 2},
            {0x10, 0, 2},
            {0x7f, 0, 2},
        };
        for (const auto &e : EXPECT) {
            fixture f;
            g_rec.clear();
            arm(f);
            player_data &pd                   = f.players[ME];
            pd.ai_groups[G].reinforce_pending = 1;
            pd.ai_groups[G].goal              = e.goal;
            // Keep the pool emptier than pending so the call stops on the excess path -- this block
            // is about the ladder, not about what follows it.
            pd.ai_groups[e.pool].member_count   = 0;
            const detail::redistribute_report r = detail::group_redistribute_units(
                f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0, 0, 0);
            ck(r.kind == e.kind && r.pool_group_index == e.pool,
               "redistribute: the goal -> (kind, pool) ladder matches the original's CMP/JC/JBE "
               "chain for every goal value");
            ck((r.kind0_split_off_reached ? 1 : 0) == (e.kind == 0 ? 1 : 0),
               "redistribute: group_split_off_create is called on EXACTLY the kind == 0 arm");
            disarm();
        }
    }

    // ---- C. the kind == 0 standins land in the right ARGUMENT POSITIONS ------------------------
    //
    // The values are unknowable (three uninitialised original frame words) but their POSITIONS are
    // not, and a transposition there is invisible to the rig. Three distinct sentinels separate all
    // six orderings.
    {
        fixture f;
        g_rec.clear();
        arm(f);
        player_data &pd                   = f.players[ME];
        pd.ai_groups[G].reinforce_pending = 1;
        pd.ai_groups[G].goal              = 4; // -> kind 0
        pd.ai_groups[2].member_count      = 0;
        detail::group_redistribute_units(f.view(), f.store(), stub_calls(), (uint32_t)ME, G,
                                         /*member_count=*/111, /*centroid_x=*/222,
                                         /*centroid_y=*/333);
        ck(g_rec.split_offs.size() == 1, "redistribute: one split_off_create on the kind == 0 arm");
        if (g_rec.split_offs.size() == 1) {
            const auto &s = at(g_rec.split_offs, 0);
            ck(s.player == (uint32_t)ME && s.source_group_idx == G,
               "redistribute: split_off_create gets (player, ..., THIS group index)");
            ck(s.member_count == 111u && s.centroid_x == 222 && s.centroid_y == 333,
               "redistribute: the three indeterminate standins land in (member_count, centroid_x, "
               "centroid_y) -- not transposed");
        }
        disarm();
    }

    // ---- D. the excess-split path and its three guards ------------------------------------------
    {
        fixture f;
        g_rec.clear();
        arm(f);
        player_data &pd              = f.players[ME];
        pd.ai_groups[2].member_count = 1; // pool < pending -> excess path
        pd.ai_groups[G].goal         = 4;

        // Guard: reinforce_pending must be STRICTLY greater than this group's member_count.
        pd.ai_groups[G].reinforce_pending = 5;
        pd.ai_groups[G].member_count      = 5;
        detail::redistribute_report r     = detail::group_redistribute_units(
            f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0, 0, 0);
        ck(r.excess_split_path && !r.excess_split_called && g_rec.split_excess.empty(),
           "redistribute: excess path with reinforce_pending == member_count does NOT split (the "
           "original's JBE is <=, not <)");

        pd.ai_groups[G].member_count = 4;
        g_rec.clear();
        r = detail::group_redistribute_units(f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0,
                                             0, 0);
        ck(r.excess_split_called && g_rec.split_excess.size() == 1,
           "redistribute: excess path with reinforce_pending > member_count splits");
        if (g_rec.split_excess.size() == 1) {
            const auto &s = at(g_rec.split_excess, 0);
            ck(s.player == (uint32_t)ME && s.group_idx == G,
               "redistribute: split_excess_members addresses this group");
        }

        // ...unless the goal is 3 or 0xb. (0xb cannot get here -- guard 2 -- so only 3 is testable.)
        for (const int16_t goal : {(int16_t)3}) {
            g_rec.clear();
            pd.ai_groups[G].goal = goal;
            r                    = detail::group_redistribute_units(f.view(), f.store(), stub_calls(), (uint32_t)ME, G,
                                                                    0, 0, 0);
            ck(r.excess_split_path && !r.excess_split_called && g_rec.split_excess.empty(),
               "redistribute: goal 3 suppresses the excess split");
        }
        disarm();
    }

    // ---- E. THE MERGE PATH -- the half the rig has never executed -------------------------------
    {
        fixture f;
        g_rec.clear();
        arm(f);
        player_data &pd      = f.players[ME];
        g_rec.centroid_out_x = 4242; // distinct, so an out-pointer swap is visible downstream
        g_rec.centroid_out_y = 8484;

        pd.ai_groups[G].goal              = 4; // kind 0, pool 2
        pd.ai_groups[G].reinforce_pending = 2; // transfer_cap starts at 2 * 2 == 4
        pd.ai_groups[2].member_count      = 9; // >= pending, so the MERGE path
        pd.ai_groups[2].head_unit         = 21;

        unit *row = &f.units[(size_t)ME * UNITS_PER_PLAYER];
        // A five-deep pool list. Types alternate so the predicate is exercised both ways: on kind 0
        // ONLY an ai_unit in {1, 2, 4, 5} moves.
        const int     ids[]   = {21, 22, 23, 24, 25};
        const int32_t roles[] = {1, 9, 4, 7, 5}; // eligible, NOT, eligible, NOT, eligible
        for (int i = 0; i < 5; ++i) {
            row[ids[i]].unit_proto_id   = (uint16_t)(30 + i);
            row[ids[i]].ai_group_next   = (uint16_t)(i + 1 < 5 ? ids[i + 1] : 0);
            f.cfg_units[30 + i].ai_unit = roles[i];
        }

        const detail::redistribute_report r = detail::group_redistribute_units(
            f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0, 0, 0);

        ck(r.merge_path && !r.excess_split_path,
           "redistribute: pool.member_count >= reinforce_pending takes the MERGE path");
        ck(g_rec.centroid_calls.size() == 1 && at(g_rec.centroid_calls, 0).group == G,
           "redistribute: the centroid is computed once, for THIS group (not the pool)");
        // transfer_cap = min(pending * 2, pool.member_count) = min(4, 9) = 4, and three of the five
        // walked units are eligible, so THREE move.
        ck(r.units_moved == 3,
           "redistribute: on kind 0 only ai_unit 1/2/4/5 members move -- three of these five");
        ck(g_rec.flag_moves.size() == 3 && g_rec.member_moves.size() == 3,
           "redistribute: each moved member gets exactly one flag_and_move and one member_move");
        if (g_rec.flag_moves.size() == 3) {
            const auto &m = at(g_rec.flag_moves, 0);
            ck(m.x == 4242u && m.y == 8484u,
               "redistribute: unit_flag_and_move receives the centroid in (x, y) order -- the two "
               "out-pointers are not swapped");
        }
        if (g_rec.member_moves.size() == 3) {
            for (size_t i = 0; i < 3; ++i) {
                const auto &m = at(g_rec.member_moves, i);
                ck(m.src == 2 && m.dst == G,
                   "redistribute: members move FROM the pool INTO this group, not the reverse");
            }
        }
        // pool.reinforce_pending += transfer_cap, taken BEFORE the loop and with the CAPPED value.
        ck(pd.ai_groups[2].reinforce_pending == 4,
           "redistribute: the pool's reinforce_pending gains the CAPPED transfer count (4), not the "
           "uncapped pending*2 and not the number actually moved");
        ck(pd.ai_groups[G].reinforce_pending == 0,
           "redistribute: this group's reinforce_pending is cleared on the merge tail");

        // The cap really binds: with pool.member_count below pending*2 the cap is the pool count.
        g_rec.clear();
        g_rec.centroid_out_x              = 1;
        g_rec.centroid_out_y              = 2;
        pd.ai_groups[G].reinforce_pending = 3; // pending*2 == 6
        pd.ai_groups[2].member_count      = 3; // >= pending, so still the merge path, but cap -> 3
        pd.ai_groups[2].reinforce_pending = 0;
        pd.ai_groups[2].head_unit         = 21;
        for (int i = 0; i < 5; ++i) {
            row[ids[i]].ai_group_next   = (uint16_t)(i + 1 < 5 ? ids[i + 1] : 0);
            f.cfg_units[30 + i].ai_unit = 1; // ALL eligible now, so only the cap can stop it
        }
        detail::group_redistribute_units(f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0, 0,
                                         0);
        ck(pd.ai_groups[2].reinforce_pending == 3,
           "redistribute: transfer_cap is min(pending * 2, pool.member_count) -- the pool count "
           "wins here");

        // kind != 0 moves EVERY member regardless of type.
        g_rec.clear();
        pd.ai_groups[G].goal              = 7; // -> kind 1, pool 3
        pd.ai_groups[G].reinforce_pending = 2;
        pd.ai_groups[3].member_count      = 9;
        pd.ai_groups[3].head_unit         = 21;
        for (int i = 0; i < 5; ++i) {
            row[ids[i]].ai_group_next   = (uint16_t)(i + 1 < 5 ? ids[i + 1] : 0);
            f.cfg_units[30 + i].ai_unit = 9; // NONE of them is an eligible type
        }
        const detail::redistribute_report r2 = detail::group_redistribute_units(
            f.view(), f.store(), stub_calls(), (uint32_t)ME, G, 0, 0, 0);
        ck(r2.units_moved == 4,
           "redistribute: on kind != 0 the type filter does not apply -- every walked member moves, "
           "bounded only by transfer_cap");
        disarm();
    }
}

// ---- batch C layer 3 (2026-08-06): the layer's last four ------------------------------
//
// THESE ARE NOT REDUNDANT WITH THE SHADOW SITES, and the 30000-step all-AI soak that armed them is
// what proves it. Measured on that run:
//   * group_task_wait        -- UN-SHADOWABLE. Empty body, empty region set, so compare() over zero
//                               regions returns true unconditionally. No site exists.
//   * group_task_disperse_passable -- armed and reached ZERO times (task_code 0x04 never fired in an
//                               8-way all-AI match from the default start). A zero-call site is not
//                               a pass; these checks are its only behavioural evidence.
//   * group1_drain_to_group0 -- 3200 calls, 0 divergences, and `group1_members=0` on ALL 400 traced
//                               calls. The rig therefore verified the NO-OP arm 3200 times over and
//                               the DRAIN LOOP not once. Case E below is where the loop is covered.
//   * group_task_advance_to_anchor -- 138 real calls over 110 distinct anchors; the one of the four
//                               with genuine rig evidence. Kept here anyway for the argument ORDER,
//                               which a divergence-free run cannot distinguish (both arms would have
//                               to pass the same wrong pair for the state to match).
// llm_strat_ai_unit_squad_firepower_value @0x004d3437 (batch A layer 3, 2026-08-06).
//
// THE OFFLINE ORACLE IS THE POINT HERE, not a supplement. Its three call sites all sit downstream of
// llm_strat_ai_army_milestone_advance_or_attack's goal-conflict scan, which returned on every one of
// 998 traced rig calls -- so the rig has never executed a single byte of this function, and until a
// scenario reaches it these assertions are the ONLY evidence its shadow site can be judged against.
void test_squad_firepower() {
    using namespace mh::ai;
    fixture f;

    const int32_t P = 3, U = 5;
    auto          run = [&]() { return detail::unit_squad_firepower_value(f.view(), P, U); };
    // Arm one weapon slot. `power` is filled with DISTINCT per-player values and only power[P] is
    // set to the number a test expects, so a body reading power[0] (or any fixed row) sees 100.0
    // and every assertion below misses by an order of magnitude.
    auto arm = [&](int32_t slot, uint8_t wid, uint8_t target, double power_for_P) {
        f.u(P, U).weapons[slot].weapon_id = wid;
        f.cfg_weapons[wid].target         = target;
        for (int i = 0; i < 9; ++i) f.cfg_weapons[wid].power[i] = 100.0 + i;
        f.cfg_weapons[wid].power[P] = power_for_P;
    };

    // ---- THE SLOT LOOP STOPS AT THE FIRST EMPTY SLOT ----
    // This is the assertion that separates this function from its byte-similar sibling
    // llm_strat_ai_group_pick_best_weapon_unit, whose identical-looking loop CONTINUES instead
    // (0x004d6cf1 -> the INC, vs 0x004d346c -> past the loop). Copy the sibling's shape into this
    // body and this pair of checks goes red.
    f.reset();
    arm(1, 4, 1, 9.0); // slot 0 left empty
    ck(run() == 0,
       "squad_firepower: an EMPTY slot 0 ends the scan -- the armed slot 1 is never reached");
    f.reset();
    arm(0, 4, 1, 9.0);
    ck(run() == 9, "squad_firepower: ... and the same weapon in slot 0 IS counted");

    // ---- BUT A NON-ANTI-PERSONNEL WEAPON ONLY SKIPS THAT SLOT ----
    // The two exits from one loop body are different addresses; a body that treats both as `break`
    // (or both as `continue`) fails exactly one of these two blocks.
    f.reset();
    arm(0, 4, 2, 50.0); // target bit 0 clear -> not anti-personnel
    arm(1, 5, 1, 7.0);
    ck(run() == 7,
       "squad_firepower: a weapon failing the anti-personnel test skips only ITS slot -- slot 1 "
       "still counts");

    // ---- THE ANTI-PERSONNEL TEST IS A BIT TEST ----
    f.reset();
    arm(0, 4, 3, 6.0); // bit 0 set alongside bit 1
    ck(run() == 6, "squad_firepower: target 3 has bit 0 set, so it counts -- TEST, not a compare");
    f.reset();
    arm(0, 4, 4, 6.0);
    ck(run() == 0, "squad_firepower: target 4 leaves bit 0 clear, so it does not");

    // ---- THE ACCUMULATOR TRUNCATES ONCE PER WEAPON, NOT ONCE AT THE END ----
    // trunc(0 + 2.7) = 2, then trunc(2 + 2.7) = 4. A body that sums in floating point and truncates
    // at the end answers 5, and one that rounds instead of truncating answers 6.
    f.reset();
    arm(0, 4, 1, 2.7);
    arm(1, 5, 1, 2.7);
    ck(run() == 4,
       "squad_firepower: each weapon is added and TRUNCATED in turn (2.7 + 2.7 -> 4, not 5 or 6)");

    // All four slots are scanned when none is empty.
    f.reset();
    arm(0, 4, 1, 1.0);
    arm(1, 5, 1, 2.0);
    arm(2, 6, 1, 4.0);
    arm(3, 7, 1, 8.0);
    ck(run() == 15, "squad_firepower: all four slots are counted");

    // ---- THE TYPE MULTIPLIER LADDER ----
    // Base of 3 so every multiplier lands on a distinct total (3/6/9/12/15/0) and an off-by-one in
    // the ladder cannot alias onto its neighbour.
    f.reset();
    arm(0, 4, 1, 3.0);
    auto with_type = [&](int32_t type) {
        f.u(P, U).unit_proto_id = 11;
        f.cfg_units[11].type    = (uint8_t)type;
        return run();
    };
    ck(with_type(UNIT_TYPE_A_INFANTRY_2) == 6, "squad_firepower: A_INFANTRY_2 (2) scales x2");
    ck(with_type(UNIT_TYPE_A_INFANTRY_3) == 9, "squad_firepower: A_INFANTRY_3 (3) scales x3");
    ck(with_type(UNIT_TYPE_A_INFANTRY_4) == 12, "squad_firepower: A_INFANTRY_4 (4) scales x4");
    ck(with_type(UNIT_TYPE_A_INFANTRY_5) == 15, "squad_firepower: A_INFANTRY_5 (5) scales x5");
    ck(with_type(UNIT_TYPE_H_INFANTRY_2) == 6, "squad_firepower: H_INFANTRY_2 (7) scales x2");
    ck(with_type(UNIT_TYPE_H_INFANTRY_3) == 9, "squad_firepower: H_INFANTRY_3 (8) scales x3");
    ck(with_type(UNIT_TYPE_H_INFANTRY_4) == 12, "squad_firepower: H_INFANTRY_4 (9) scales x4");
    ck(with_type(UNIT_TYPE_H_INFANTRY_5) == 15, "squad_firepower: H_INFANTRY_5 (0xa) scales x5");
    // 6 sits BETWEEN the two infantry runs and is not in either -- the gap a ladder written as two
    // ranges instead of two runs would swallow.
    ck(with_type(6) == 3, "squad_firepower: type 6 is in NEITHER infantry run and stays x1");
    ck(with_type(0) == 3, "squad_firepower: type 0 is x1");
    // The heli band is tested as a RANGE by the original, so both ENDS and the interior must zero,
    // while the types either side of it must not.
    ck(with_type(UNIT_TYPE_A_HELI_MOTHER) == 0, "squad_firepower: A_HELI_MOTHER (0x13) scores zero");
    ck(with_type(UNIT_TYPE_H_HELI_MOTHER) == 0, "squad_firepower: ... and 0x14, inside the band");
    ck(with_type(UNIT_TYPE_H_HELI_CARGO) == 0,
       "squad_firepower: ... and H_HELI_CARGO (0x18), the INCLUSIVE far end");
    ck(with_type(UNIT_TYPE_A_HELI_MOTHER - 1) == 3,
       "squad_firepower: 0x12, one below the band, is x1 -- the low end is inclusive but not wider");
    ck(with_type(UNIT_TYPE_H_HELI_CARGO + 1) == 3,
       "squad_firepower: 0x19, one above the band, is x1 -- so is every type past it");

    // The multiplier applies to the SUMMED total, not per weapon.
    f.reset();
    arm(0, 4, 1, 3.0);
    arm(1, 5, 1, 4.0);
    ck(with_type(UNIT_TYPE_A_INFANTRY_3) == 21, "squad_firepower: the multiplier scales the SUM");
}

void test_player_score_tier() {
    using namespace mh::ai;
    fixture       f;
    const int32_t P   = 4;
    auto          run = [&]() { return detail::player_score_tier(f.view(), P); };

    f.reset();
    ck(run() == 0, "score_tier: an all-zero cache is tier 0");

    // ---- THE OFF-BY-ONE FIELD TRAP ----
    // The gate for tier N is [tier N-1's gate] PLUS one more field, and that field is NOT the one
    // syntactically adjacent to it in Ghidra's own .c rendering -- see the header banner on
    // detail::player_score_tier for the full derivation from the disassembly's five MOV EDX,N
    // immediates. Each block below arms exactly the tier-N gate and STOPS, leaving every field the
    // (wrong, naively-grouped) reading would additionally require UNSET -- so a translation with the
    // fields shifted by one returns a tier ONE LOWER than these checks expect.
    f.reset();
    f.players[P].ai_score_cat_0x10 = 1;
    f.players[P].ai_score_cat_0x20 = 1;
    ck(run() == 1, "score_tier: 0x10 && 0x20 alone reaches tier 1 (0x21/0x11/0x30/... all unset)");

    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x11 = 1;
    // 0x30 DELIBERATELY LEFT UNSET: a translation that bundles 0x30 into tier 2's own gate (the
    // naive reading of the .c's nesting) returns 1 here instead of 2.
    ck(run() == 2,
       "score_tier: + 0x21 && 0x11 reaches tier 2 -- WITHOUT 0x30, which gates tier 3, not tier 2");

    f.players[P].ai_score_cat_0x30 = 1;
    // 0x12 DELIBERATELY LEFT UNSET.
    ck(run() == 3, "score_tier: + 0x30 reaches tier 3 -- WITHOUT 0x12, which gates tier 4");

    f.players[P].ai_score_cat_0x12 = 1;
    // 0x13/0x23/0x22 DELIBERATELY LEFT UNSET: a translation that bundles 0x13&&0x23 into tier 3's
    // gate (as if tier 3 needed them) would already have diverged one step earlier; this checks tier
    // 4 does not ALSO demand them early.
    ck(run() == 4, "score_tier: + 0x12 reaches tier 4 -- WITHOUT 0x13/0x23/0x22, which gate tier 5");

    f.players[P].ai_score_cat_0x13 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    ck(run() == 5, "score_tier: + 0x13 && 0x23 && 0x22 -- all nine fields -- reaches tier 5");

    // A field near the TOP of the chain missing caps the tier there even with every field below it
    // set (so a bit-count implementation, or one that scans in a different order, would over-report).
    f.reset();
    f.players[P].ai_score_cat_0x10 = 1;
    f.players[P].ai_score_cat_0x20 = 1;
    f.players[P].ai_score_cat_0x21 = 1;
    f.players[P].ai_score_cat_0x11 = 1;
    // 0x30 unset -> stops at tier 2, regardless of 0x12/0x13/0x23/0x22 below.
    f.players[P].ai_score_cat_0x12 = 1;
    f.players[P].ai_score_cat_0x13 = 1;
    f.players[P].ai_score_cat_0x23 = 1;
    f.players[P].ai_score_cat_0x22 = 1;
    ck(run() == 2,
       "score_tier: 0x30 unset caps the result at tier 2 even though every LATER field is set -- "
       "this is NOT a popcount over nine independent flags");

    ck(detail::player_score_tier(f.view(), P + 1) == 0,
       "score_tier: reads player_data[player_idx], not a fixed slot");
}

void test_unit_weapon_power() {
    using namespace mh::ai;
    fixture f;

    const int32_t P = 3, U = 5;
    auto          run = [&]() { return detail::unit_weapon_power(f.view(), P, U); };
    auto          arm = [&](int32_t slot, uint8_t wid, uint8_t enabled_2, double power_for_P) {
        f.u(P, U).weapons[slot].weapon_id = wid;
        f.u(P, U).weapons[slot].enabled_2 = enabled_2;
        for (int i = 0; i < 9; ++i) f.cfg_weapons[wid].power[i] = 100.0 + i; // DISTINCT per-player rows
        f.cfg_weapons[wid].power[P] = power_for_P;
    };

    // ---- GATED ON enabled_2, NOT weapon_id != 0 and NOT Weapon.target's ground/air bit ----
    // This is the assertion that separates this function from its two byte-similar lookalikes
    // (llm_strat_ai_unit_squad_firepower_value gates on weapon_id!=0 with an EARLY BREAK on an empty
    // slot; llm_strat_ai_group_pick_best_weapon_unit additionally gates on Weapon.target&1). A weapon
    // present but DISABLED (enabled_2==0) contributes nothing, regardless of its target bits.
    f.reset();
    arm(0, 4, /*enabled_2=*/0, 9.0);
    f.cfg_weapons[4].target = 3; // would pass squad_firepower's anti-personnel test if it mattered here
    ck(run() == 0, "unit_weapon_power: enabled_2==0 contributes nothing, even with weapon_id set and "
                   "a ground-capable target bit");
    f.reset();
    arm(0, 4, /*enabled_2=*/1, 9.0);
    f.cfg_weapons[4].target = 0; // NOT ground-capable -- irrelevant to this function
    ck(run() == 9, "unit_weapon_power: enabled_2!=0 counts the slot regardless of Weapon.target");

    // ---- NO EARLY-OUT: every slot is visited even when an earlier one is empty/disabled ----
    // squad_firepower_value's sibling loop BREAKS on an empty slot 0; this one does not.
    f.reset();
    arm(1, 4, 1, 9.0); // slot 0 left untouched (weapon_id 0, enabled_2 0)
    ck(run() == 9, "unit_weapon_power: slot 0 empty does NOT stop the scan -- slot 1 still counts "
                   "(unlike squad_firepower_value)");

    // ---- THE ACCUMULATOR TRUNCATES ONCE AT THE END, NOT PER-SLOT ----
    // trunc(2.7 + 2.7) = trunc(5.4) = 5. squad_firepower_value's sibling helper
    // (weapon_power_add_and_trunc) truncates PER ADDITION and would answer 4 for the same inputs
    // (test_squad_firepower asserts exactly that on the identical 2.7+2.7 case) -- this is the
    // assertion that would catch a translation that reused that helper here instead of the
    // once-at-the-end trunc_float_to_int32 sequence.
    f.reset();
    arm(0, 4, 1, 2.7);
    arm(1, 5, 1, 2.7);
    ck(run() == 5,
       "unit_weapon_power: the sum truncates ONCE at the end (2.7 + 2.7 -> 5, not squad_firepower's "
       "per-slot-truncated 4)");

    // All four slots visited when every one is armed.
    f.reset();
    arm(0, 4, 1, 1.0);
    arm(1, 5, 1, 2.0);
    arm(2, 6, 1, 4.0);
    arm(3, 7, 1, 8.0);
    ck(run() == 15, "unit_weapon_power: all four slots are counted");

    // Reads cfg_weapons[weapon_id].power[player] -- the SAME per-player row group_pick_best_weapon_unit
    // reads -- not a fixed row.
    f.reset();
    arm(0, 4, 1, 9.0);
    ck(detail::unit_weapon_power(f.view(), P + 1, U) != 9,
       "unit_weapon_power: reads power[player], not a fixed row (a different player sees the "
       "OTHER distinct per-row value armed by the fixture, not this player's 9.0)");
}

void test_calc_power_supply_ratio() {
    using namespace mh::ai;
    fixture f;

    const int32_t P     = 2;
    auto          run   = [&]() { return detail::calc_power_supply_ratio(f.view(), P); };
    auto          place = [&](int32_t idx, int32_t building_id, uint8_t type, int32_t power) {
        f.b(P, idx).building_id                     = building_id;
        f.cfg_buildings[building_id].type           = type;
        f.cfg_buildings[building_id].electric_power = power;
    };

    // ---- empty roster: both sums are zero -> 0.0 ----
    f.reset();
    f.b(P, 0).index = 0;
    ck(run() == 0.0, "power_ratio: an empty roster is 0.0, not a divide-by-zero NaN/inf");

    // ---- generators only: consumers zero -> the FIXED SENTINEL 1.1, not a computed value ----
    f.reset();
    f.b(P, 0).index = 1;
    place(1, 10, BLDG_TYPE_A_PLANT, 500);
    ck(run() == 1.1, "power_ratio: generators nonzero, consumers zero -> the fixed sentinel 1.1");

    // ---- the four generator types, tested UNCONDITIONALLY (no is_alien_race gate) ----
    // Each case below pairs ONE generator-type building with ONE consumer so the ratio is exactly
    // generator_power / consumer_power, isolating which type landed on which side.
    auto ratio_for = [&](uint8_t gen_type) {
        f.reset();
        f.b(P, 0).index = 2;
        place(1, 10, gen_type, 300);
        place(2, 11, /*some non-generator type*/ 9, 100); // 9 is not A/H_PLANT/A/H_MOTHER
        return run();
    };
    ck(ratio_for(BLDG_TYPE_A_PLANT) == 3.0, "power_ratio: A_PLANT (3) is a generator");
    ck(ratio_for(BLDG_TYPE_H_PLANT) == 3.0, "power_ratio: H_PLANT (0x17) is a generator");
    ck(ratio_for(BLDG_TYPE_A_MOTHER) == 3.0, "power_ratio: A_MOTHER (6) is a generator");
    ck(ratio_for(BLDG_TYPE_H_MOTHER) == 3.0, "power_ratio: H_MOTHER (0x1a) is a generator");
    // is_alien_race is NOT consulted: a HUMAN-race player with an A_PLANT building still counts it as
    // a generator, and vice versa -- unlike the race_*_type() helpers elsewhere in this file.
    f.reset();
    f.players[P].is_alien_race = 0; // human
    f.b(P, 0).index            = 2;
    place(1, 10, BLDG_TYPE_A_PLANT, 300); // the ALIEN plant type, on a human player
    place(2, 11, 9, 100);
    ck(run() == 3.0,
       "power_ratio: A_PLANT counts as a generator even for a human-race player -- unconditional, "
       "no is_alien_race gate (unlike race_turret_type/race_mine_type/race_mother_type)");

    // ---- everything else falls to the consumer side ----
    f.reset();
    f.b(P, 0).index = 1;
    place(1, 12, /*not a generator type*/ 9, 50);
    ck(run() == 0.0, "power_ratio: a non-generator building with no generators present is 0.0 (both "
                     "sums route to the SAME zero-check as the empty-roster case)");

    // ---- empty roster SLOTS (building_id == 0) are skipped, count-driven walk over `remaining` ----
    f.reset();
    f.b(P, 0).index       = 1;            // ONE live building somewhere in the roster
    f.b(P, 1).building_id = 0;            // slot 1 empty
    place(2, 13, BLDG_TYPE_A_PLANT, 700); // the one live building is at slot 2
    ck(run() == 1.1, "power_ratio: the walk is COUNT-driven (skips empty slot 1, still finds the live "
                     "building at slot 2, decrementing `remaining` only on occupied slots)");
}

void test_group_task_formation() {
    const int32_t ME = 3;
    const int32_t G  = 6;

    // Link a member chain head -> ... -> 0 for player p's group g.
    auto chain = [](fixture &f, int32_t p, int32_t g, std::vector<uint16_t> ids) {
        f.players[p].ai_groups[g].head_unit    = ids.empty() ? (uint16_t)0 : ids[0];
        f.players[p].ai_groups[g].member_count = (int16_t)ids.size();
        for (size_t i = 0; i < ids.size(); ++i)
            f.u(p, ids[i]).ai_group_next = (i + 1 < ids.size()) ? ids[i + 1] : (uint16_t)0;
    };

    // ---- A. group_task_wait does NOTHING -------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        f.group_scratch_count = 17; // a value nothing in a correct body may touch
        detail::group_task_wait();
        ck(f.group_scratch_count == 17 && g_rec.member_moves.empty() &&
               g_rec.formation_moves.empty() && g_rec.scatter_calls.empty() && g_rec.dequeues.empty(),
           "task_wait: the no-op arm calls nothing and leaves the scratch count alone");
    }

    // ---- B. the harvest, shared by both movers --------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        chain(f, ME, G, {5, 9, 2});
        f.players[ME].ai_groups[G].active_param_a = 41; // distinct from _b so a swap is visible
        f.players[ME].ai_groups[G].active_param_b = 17;
        f.group_scratch_count                     = 4; // inherited garbage the body must RESET
        f.group_scratch[0]                        = -1;
        g_rec.centroid_out_x                      = 33; // distinct from each other AND from the
        g_rec.centroid_out_y                      = 44; // two anchor params above

        detail::group_task_advance_to_anchor(f.view(), f.store(), stub_calls(), ME, G);

        ck(f.group_scratch_count == 3, "advance_to_anchor: harvest resets the count and publishes 3");
        ck(f.group_scratch[0] == 5 && f.group_scratch[1] == 9 && f.group_scratch[2] == 2,
           "advance_to_anchor: harvest publishes the members in LINK order, head first");

        ck(g_rec.centroid_calls.size() == 1, "advance_to_anchor: exactly one centroid call");
        ck(g_rec.formation_moves.size() == 1, "advance_to_anchor: exactly one formation move");
        if (g_rec.formation_moves.size() == 1) {
            const auto &m = g_rec.formation_moves[0];
            ck(m.player == (uint32_t)ME, "advance_to_anchor: the move carries the player");
            ck(m.centroid_x == 33 && m.centroid_y == 44,
               "advance_to_anchor: the centroid pair reaches the mover in x,y order (not swapped)");
            ck(m.target_x == 41 && m.target_y == 17,
               "advance_to_anchor: the target is active_param_a,_b in that order");
        }
        // The asymmetry the header calls out: this arm jumps PAST the shared tail's dequeue.
        ck(g_rec.dequeues.empty(), "advance_to_anchor: does NOT dequeue (unlike its two siblings)");
    }

    // ---- C. an EMPTY group still publishes an empty list, and still moves ----------------------
    {
        fixture f;
        g_rec.clear();
        chain(f, ME, G, {});
        f.group_scratch_count = 9;
        detail::group_task_advance_to_anchor(f.view(), f.store(), stub_calls(), ME, G);
        ck(f.group_scratch_count == 0, "advance_to_anchor: an empty group resets the count to 0");
        ck(g_rec.formation_moves.size() == 1,
           "advance_to_anchor: the mover is called unconditionally -- there is no empty-group guard");
    }

    // ---- D. disperse_passable: same harvest, different single call -----------------------------
    {
        fixture f;
        g_rec.clear();
        chain(f, ME, G, {7, 1});
        f.players[ME].ai_groups[G].active_param_a = 12;
        f.players[ME].ai_groups[G].active_param_b = 60;
        f.group_scratch_count                     = 5;

        detail::group_task_disperse_passable(f.view(), f.store(), stub_calls(), (uint32_t)ME, G);

        ck(f.group_scratch_count == 2 && f.group_scratch[0] == 7 && f.group_scratch[1] == 1,
           "disperse_passable: the same harvest, in link order");
        ck(g_rec.scatter_calls.size() == 1 && g_rec.formation_moves.empty() &&
               g_rec.centroid_calls.empty(),
           "disperse_passable: one scatter call, and NO centroid/formation call");
        if (g_rec.scatter_calls.size() == 1) {
            const auto &s = g_rec.scatter_calls[0];
            ck(s.player == (uint32_t)ME && s.x == 12 && s.y == 60,
               "disperse_passable: scatter carries player, active_param_a, active_param_b");
        }
        ck(g_rec.dequeues.empty(), "disperse_passable: does not dequeue either");
    }

    // ---- E. group1_drain_to_group0 -- THE ARM THE RIG NEVER REACHED ----------------------------
    {
        fixture f;
        g_rec.clear();
        player_data &pd     = f.players[ME];
        g_member_move_owner = &pd;
        g_member_move_units = &f.units[(size_t)ME * UNITS_PER_PLAYER];
        chain(f, ME, 1, {4, 8, 3});

        detail::group1_drain_to_group0(f.view(), f.store(), stub_calls(), (uint32_t)ME);

        ck(g_rec.member_moves.size() == 3, "drain_to_group0: one move per group-1 member");
        bool routed = !g_rec.member_moves.empty();
        for (size_t i = 0; i < g_rec.member_moves.size(); ++i) {
            const auto &m = g_rec.member_moves[i];
            routed &= (m.player == (uint32_t)ME && m.src == 1 && m.dst == 0);
        }
        ck(routed, "drain_to_group0: every move is player, src=1, dst=0");
        if (g_rec.member_moves.size() == 3)
            ck(g_rec.member_moves[0].unit_id == 4 && g_rec.member_moves[1].unit_id == 8 &&
                   g_rec.member_moves[2].unit_id == 3,
               "drain_to_group0: it re-reads head_unit each pass, so the chain drains in order");
        ck(pd.ai_groups[1].member_count == 0 && pd.ai_groups[0].member_count == 3,
           "drain_to_group0: group 1 ends empty and group 0 has them all");

        // The no-op case -- which, per the soak, is what EVERY rig call actually was.
        g_rec.clear();
        detail::group1_drain_to_group0(f.view(), f.store(), stub_calls(), (uint32_t)ME);
        ck(g_rec.member_moves.empty(), "drain_to_group0: an already-empty group 1 moves nothing");

        g_member_move_owner = nullptr;
        g_member_move_units = nullptr;
    }
}

// RI-AI batch B/C, 2026-08-07: grid_stamp_seeds, notify_map_changed / _2, bldg_count_by_category,
// bldg_has_heli_unit, pick_owned_tile_or_home, unit_is_order_pending, random_point_near.
void test_ai1b_2026_08_07_slice() {
    using namespace mh::ai;

    // ---- grid_stamp_seeds: wrap-mask packing, preserve-top-2-bits stamp, skip on a zero stencil ---
    {
        fixture  f;
        ai_store own  = f.store();
        uint8_t *grid = f.players[3].ai_tile_flags_grid;
        std::memset(grid, 0, 65536);
        grid[(7 << 8) | 3] = 0xAA; // top 2 bits = 0x80 -- must survive; low 6 must be replaced
        grid[(0 << 8) | 0] = 0x40; // top 2 bits = 0x40 -- same check at the WRAPPED cell

        uint8_t stencil[4] = {1, 0, 0, 1}; // X-outer(i)/Y-inner(j): [0][0]=1 [0][1]=0 [1][0]=0 [1][1]=1

        detail::grid_stamp_seeds(own, grid, /*map_width*/ 8, /*map_height*/ 4, stencil, /*span_x*/ 2,
                                 /*span_y*/ 2, /*origin_x*/ 7, /*origin_y*/ 3, /*seed*/ 0x15);

        ck(f.wrap_mask == 0x0703,
           "grid_stamp_seeds: packs own.grid_wrap_mask as ((map_width-1)<<8)|(map_height-1)");
        ck(grid[(7 << 8) | 3] == (uint8_t)(0x80 | 0x15),
           "grid_stamp_seeds: a nonzero-stencil cell is stamped, preserving the old top 2 bits");
        ck(grid[(0 << 8) | 0] == (uint8_t)(0x40 | 0x15),
           "grid_stamp_seeds: origin_x AND origin_y both wrap (7+1&7=0, 3+1&3=0), same preserve rule");
        ck(grid[(7 << 8) | 0] == 0 && grid[(0 << 8) | 3] == 0,
           "grid_stamp_seeds: the two zero-stencil cells are left untouched");
    }

    // ---- notify_map_changed: per-AI-player loop, dirty flag, 7/5/4 seed choice --------------------
    {
        fixture f;
        g_rec.clear();
        f.active_players                  = 3;
        f.players[0].ai_enabled           = 1; // the builder
        f.players[1].ai_enabled           = 1; // a foreign AI
        f.players[2].ai_enabled           = 0; // not AI -- must be skipped entirely
        const int32_t TURRET_TYPE         = 5;
        f.cfg_buildings[TURRET_TYPE].type = BLDG_TYPE_H_TURRET;

        detail::notify_map_changed(f.view(), f.store(), stub_calls(), /*builder*/ 0, TURRET_TYPE, 11,
                                   22);

        ck(g_rec.seed_stamps.size() == 2, "notify_map_changed: only AI-enabled players are stamped");
        if (g_rec.seed_stamps.size() == 2) {
            ck(g_rec.seed_stamps[0].grid == f.players[0].ai_tile_flags_grid &&
                   g_rec.seed_stamps[0].seed == 5,
               "notify_map_changed: the builder gets seed 5 for a turret/mine/relay type, on its OWN "
               "grid");
            ck(g_rec.seed_stamps[1].grid == f.players[1].ai_tile_flags_grid &&
                   g_rec.seed_stamps[1].seed == 7,
               "notify_map_changed: a foreign AI player always gets seed 7, on ITS OWN grid");
            ck(g_rec.seed_stamps[0].x == 11 && g_rec.seed_stamps[0].y == 22,
               "notify_map_changed: tile_x/tile_y pass through as origin_x/origin_y");
        }
        ck(f.players[0].ai_map_changed_pending == 1 && f.players[1].ai_map_changed_pending == 1,
           "notify_map_changed: sets the dirty flag on every AI-enabled player, including the builder");
        ck(f.players[2].ai_map_changed_pending == 0,
           "notify_map_changed: a non-AI player's dirty flag is untouched");

        g_rec.clear();
        const int32_t PLAIN_TYPE         = 6;
        f.cfg_buildings[PLAIN_TYPE].type = 99; // none of the six structural types
        detail::notify_map_changed(f.view(), f.store(), stub_calls(), 0, PLAIN_TYPE, 0, 0);
        ck(g_rec.seed_stamps.size() == 2 && g_rec.seed_stamps[0].seed == 4,
           "notify_map_changed: the builder gets seed 4 for a non-structural type");
    }

    // ---- notify_map_changed_2: the 0/2 "clear it again" pair, not 7/5/4 ----------------------------
    {
        fixture f;
        g_rec.clear();
        f.active_players                 = 2;
        f.players[0].ai_enabled          = 1;
        f.players[1].ai_enabled          = 1;
        const int32_t PLAIN_TYPE         = 6;
        f.cfg_buildings[PLAIN_TYPE].type = 99;

        detail::notify_map_changed_2(f.view(), f.store(), stub_calls(), 0, PLAIN_TYPE, 0, 0);
        ck(g_rec.seed_stamps.size() == 2, "notify_map_changed_2: stamps every AI-enabled player too");
        if (g_rec.seed_stamps.size() == 2) {
            ck(g_rec.seed_stamps[0].seed == 2,
               "notify_map_changed_2: the builder gets seed 2 for a non-structural type");
            ck(g_rec.seed_stamps[1].seed == 0,
               "notify_map_changed_2: a foreign player always gets seed 0 -- NOT 7 like the sibling "
               "above (the exported plate's 'byte-identical' claim was wrong)");
        }

        g_rec.clear();
        const int32_t TURRET_TYPE         = 5;
        f.cfg_buildings[TURRET_TYPE].type = BLDG_TYPE_A_MINE;
        detail::notify_map_changed_2(f.view(), f.store(), stub_calls(), 0, TURRET_TYPE, 0, 0);
        ck(g_rec.seed_stamps.size() == 2 && g_rec.seed_stamps[0].seed == 0,
           "notify_map_changed_2: the builder gets seed 0 (not 2) when the type IS structural");
    }

    // ---- bldg_count_by_category: roster walk starting at index 0 + the queue's status==1 half -----
    {
        fixture       f;
        const int32_t P = 2;

        f.b(P, 0).index              = 3;  // remaining-count sentinel; the WALK STARTS AT THIS SAME SLOT
        f.b(P, 0).building_id        = 10; // slot 0 IS scanned (unlike several sibling roster walks)
        f.b(P, 1).building_id        = 0;  // empty -- does not decrement `remaining`
        f.b(P, 2).building_id        = 11;
        f.b(P, 3).building_id        = 12;
        f.cfg_buildings[10].ai_build = 0x30;
        f.cfg_buildings[11].ai_build = 0x30;
        f.cfg_buildings[12].ai_build = 0x10; // a different category -- counted toward `remaining` but
                                             // NOT toward the 0x30 result

        f.players[P].ai_bldg_queue_count              = 2;
        f.players[P].ai_bldg_queue[0].status          = 1; // construction -- counts
        f.players[P].ai_bldg_queue[0].tick_or_unit_id = 13;
        f.cfg_buildings[13].ai_build                  = 0x30;
        f.players[P].ai_bldg_queue[1].status          = 2;  // NOT construction -- must not count even though
        f.players[P].ai_bldg_queue[1].tick_or_unit_id = 13; // it names the same matching type

        ck(detail::bldg_count_by_category(f.view(), P, 0x30) == 3,
           "bldg_count_by_category: 2 roster matches (index 0 AND 2, not index 3) + 1 queued "
           "construction match = 3");

        // The category mask: a stray high bit above 0xfff on the cfg field must not change the match.
        f.reset();
        f.b(P, 0).index              = 1;
        f.b(P, 0).building_id        = 20;
        f.cfg_buildings[20].ai_build = 0x1030; // 0x30 with bit 0x1000 set
        ck(detail::bldg_count_by_category(f.view(), P, 0x30) == 1,
           "bldg_count_by_category: matches on the low 12 bits only (& 0xfff), a stray high bit does "
           "not break the match");
    }

    // ---- bldg_has_heli_unit: roster walk starting at index 1, nested cfg Unit scan for ai_unit==6 -
    {
        fixture       f;
        const int32_t P  = 4;
        f.unit_sec.total = 3; // scans unit types 1..3 inclusive

        // Negative case first: a building with SOME unit_quant but no HELI-classed consumer.
        f.b(P, 0).index                   = 1;
        f.b(P, 1).building_id             = 22;
        f.cfg_buildings[22].unit_quant[1] = 5.0;
        f.cfg_units[1].ai_unit            = 1; // SOLDIER, not HELI
        ck(detail::bldg_has_heli_unit(f.view(), P) == 0,
           "bldg_has_heli_unit: a nonzero unit_quant for a NON-heli unit type does not match");

        // Positive case: the SECOND building (index 2, since the walk starts at 1) carries it.
        f.reset();
        f.unit_sec.total                  = 3;
        f.b(P, 0).index                   = 2;
        f.b(P, 1).building_id             = 22;
        f.b(P, 2).building_id             = 23;
        f.cfg_buildings[22].unit_quant[1] = 0.0;
        f.cfg_buildings[23].unit_quant[2] = 1.0;
        f.cfg_units[2].ai_unit            = 6; // HELI, confirmed via the disassembly literal
        ck(detail::bldg_has_heli_unit(f.view(), P) == 1,
           "bldg_has_heli_unit: finds a match on the SECOND scanned building, over unit type 2");
    }

    // ---- unit_is_order_pending: bit 0x80 of order_status_flags, returned as bit 0x8000 -------------
    {
        fixture f;
        f.u(1, 5).order_status_flags = 0x80;
        ck(detail::unit_is_order_pending(f.view(), 1, 5) == 0x8000,
           "unit_is_order_pending: bit 0x80 set -> returns 0x8000, not 1");
        f.u(1, 5).order_status_flags = 0x40;
        ck(detail::unit_is_order_pending(f.view(), 1, 5) == 0,
           "unit_is_order_pending: bit 0x80 clear -> returns 0, even with other bits set");
        f.u(1, 5).order_status_flags = 0xff;
        ck(detail::unit_is_order_pending(f.view(), 1, 5) == 0x8000,
           "unit_is_order_pending: every other bit set does not change the result");
    }

    // ---- pick_owned_tile_or_home: rank-th match in scan order, home-tile fallback -----------------
    {
        fixture       f;
        const int32_t P = 1;
        f.map_w         = 4;
        f.map_h         = 3;
        uint8_t *grid   = f.players[P].ai_tile_flags_grid;
        std::memset(grid, 0, 65536);
        grid[(1 << 8) | 1] = 1; // rank 0 in (x outer, y inner) scan order
        grid[(3 << 8) | 2] = 1; // rank 1

        uint32_t ox = 999, oy = 999;
        g_rand_below_ai_ans = 0;
        detail::pick_owned_tile_or_home(f.view(), stub_calls(), P, &ox, &oy);
        ck(ox == 1 && oy == 1, "pick_owned_tile_or_home: rank 0 is the FIRST owned tile in scan order");
        ck(g_rand_below_ai_last_range == 2,
           "pick_owned_tile_or_home: draws rand_below_ai(owned_count), i.e. 2 here");

        g_rand_below_ai_ans = 1;
        detail::pick_owned_tile_or_home(f.view(), stub_calls(), P, &ox, &oy);
        ck(ox == 3 && oy == 2, "pick_owned_tile_or_home: rank 1 is the SECOND owned tile in scan order");

        // No owned tile anywhere -> falls back to the player's home tile.
        std::memset(grid, 0, 65536);
        f.players[P].ai_home_tile_x = 77;
        f.players[P].ai_home_tile_y = 88;
        g_rand_below_ai_ans         = 0;
        detail::pick_owned_tile_or_home(f.view(), stub_calls(), P, &ox, &oy);
        ck(ox == 77 && oy == 88,
           "pick_owned_tile_or_home: with no owned tile at all, falls back to ai_home_tile_x/y");
    }

    // ---- random_point_near: channel-2 draw feeds BOTH trig calls with the SAME angle; mask+add -----
    {
        fixture      f;
        const double CHANNEL2_ANGLE = 3.0;
        g_rand_state_advance_ans    = CHANNEL2_ANGLE;
        g_fsin_ans                  = 0.5;
        g_cos_ans                   = -0.5;

        int32_t ox = 0, oy = 0;
        detail::random_point_near(f.view(), stub_calls(), /*x*/ 100, /*y*/ 100, /*radius*/ 10, &ox,
                                  &oy);

        ck(g_rand_state_advance_last_channel == 2,
           "random_point_near: draws AI PRNG channel 2, not any other channel");
        ck(g_fsin_last_angle == g_cos_last_angle,
           "random_point_near: sin and cos are computed over the IDENTICAL angle (one channel-2 draw, "
           "not two)");
        ck(g_fsin_last_angle != CHANNEL2_ANGLE,
           "random_point_near: the raw channel-2 draw is SCALED before use, not passed to sin/cos "
           "unscaled");
        // dx = floor(0.5*10) = 5, dy = floor(-0.5*10) = floor(-5.0) = -5; masked by map_width_mask/
        // map_height_mask (the fixture's defaults, 127/63 -- see the member comment on why they are
        // NOT the same as map_w/map_h).
        ck(ox == (int32_t)(((uint32_t)(100 + 5)) & 127u), "random_point_near: x = (x+dx) & width_mask");
        ck(oy == (int32_t)(((uint32_t)(100 - 5)) & 63u), "random_point_near: y = (y+dy) & height_mask");
    }
}

void test_ai1e_2026_08_07_hq_attack_scenario() {
    g_rec.clear();
    ai_view  v{};
    ai_store own{};

    std::vector<player_profile> profiles(1);
    profiles[0].landing_x[0x1f] = 100;
    profiles[0].landing_y[0x1f] = 50;
    v.strat_players             = profiles.data();
    int32_t map_w = 64, map_h = 48;
    v.map_width  = &map_w;
    v.map_height = &map_h;

    int32_t  ai_enabled_val              = 0;
    uint32_t attack_hq_id_val            = 0;
    bool     tutorial_done               = false;
    own.ai_enabled                       = &ai_enabled_val;
    own.ai_attack_hq_unit_id             = &attack_hq_id_val;
    own.tutorial_hq_attack_scenario_done = &tutorial_done;

    g_rec.create_soldier_ans = 4242;
    detail::start_hq_attack_scenario(v, own, stub_calls());

    ck(ai_enabled_val == 1, "start_hq_attack_scenario: sets the master AI-enabled latch");
    ck(tutorial_done == true, "start_hq_attack_scenario: sets the one-shot tutorial-done latch");
    ck(attack_hq_id_val == 4242,
       "start_hq_attack_scenario: stores unit_create_soldier's return as the attacker's unit id");

    ck(g_rec.create_soldier_calls.size() == 1, "start_hq_attack_scenario: exactly one soldier spawn");
    if (g_rec.create_soldier_calls.size() == 1) {
        const auto &c = g_rec.create_soldier_calls[0];
        ck(c.x == (uint32_t)((100 - 4) % 64), "start_hq_attack_scenario: x = (landing_x[0x1f]-4) % width");
        ck(c.y == (uint32_t)((50 + 0x14) % 48),
           "start_hq_attack_scenario: y = (landing_y[0x1f]+0x14) % height");
        ck(c.a2 == 2 && c.param_4 == 1 && c.param_5 == 1,
           "start_hq_attack_scenario: unit_create_soldier's fixed args (2, 1, 1)");
    }
    ck(g_rec.diplomacy_calls.size() == 4, "start_hq_attack_scenario: exactly four relation flips");
    if (g_rec.diplomacy_calls.size() == 4) {
        ck(g_rec.diplomacy_calls[0].player_a == 0 && g_rec.diplomacy_calls[0].player_b == 0 &&
               g_rec.diplomacy_calls[0].relation == 1,
           "start_hq_attack_scenario: relation flip 1 = (0,0,1)");
        ck(g_rec.diplomacy_calls[1].player_a == 1 && g_rec.diplomacy_calls[1].player_b == 1 &&
               g_rec.diplomacy_calls[1].relation == 1,
           "start_hq_attack_scenario: relation flip 2 = (1,1,1)");
        ck(g_rec.diplomacy_calls[2].player_a == 0 && g_rec.diplomacy_calls[2].player_b == 1 &&
               g_rec.diplomacy_calls[2].relation == 2,
           "start_hq_attack_scenario: relation flip 3 = (0,1,2)");
        ck(g_rec.diplomacy_calls[3].player_a == 1 && g_rec.diplomacy_calls[3].player_b == 0 &&
               g_rec.diplomacy_calls[3].relation == 2,
           "start_hq_attack_scenario: relation flip 4 = (1,0,2)");
    }
    ck(g_rec.commit_attack_on_enemy_hq_calls == 1,
       "start_hq_attack_scenario: commits the attack exactly once");

    // Mutation-style check: a swapped landing_x/landing_y read would still pass every ck() above by
    // symmetry-of-shape UNLESS the two operands are numerically distinct, which they are here
    // (100/-4/64 vs 50/+0x14/48) -- re-run with landing coordinates swapped to confirm the x/y
    // channels are not accidentally interchangeable.
    g_rec.clear();
    profiles[0].landing_x[0x1f] = 50;
    profiles[0].landing_y[0x1f] = 100;
    ai_enabled_val              = 0;
    attack_hq_id_val            = 0;
    tutorial_done               = false;
    detail::start_hq_attack_scenario(v, own, stub_calls());
    ck(g_rec.create_soldier_calls.size() == 1 &&
           g_rec.create_soldier_calls[0].x == (uint32_t)((50 - 4) % 64) &&
           g_rec.create_soldier_calls[0].y == (uint32_t)((100 + 0x14) % 48),
       "start_hq_attack_scenario: x reads landing_x specifically, y reads landing_y specifically "
       "(not swapped)");
}

// scr_parse itself frees the "resource file" pointer (gc.utils_free(res_ptr), ai_scr_parse.cpp:49) --
// exactly mirroring GetResourseFilePtr's contract in the original. So the stub's `g_scr_file_contents`
// MUST be a real malloc()-ed buffer, fresh per call: a stack array (freed once, or freed twice across
// two calls sharing one pointer) is a bad-free / double-free, which is exactly what ASan caught on the
// first version of this test (attempted free on a stack address at ai_scr_parse.cpp:49).
void set_scr_file(const char *text) {
    const size_t n    = std::strlen(text);
    char        *heap = static_cast<char *>(std::malloc(n));
    std::memcpy(heap, text, n);
    g_scr_file_contents = heap;
    g_scr_file_size     = n;
}

void test_ai1e_2026_08_07_scr_parse() {
    // Two process-global int/float tunables to stand in for real AI.SCR keyword targets.
    int32_t       g_int_target    = -1;
    float         g_float_target  = -1.0f;
    const int32_t keyword_table[] = {
        1, (int32_t)(intptr_t)&g_int_target, (int32_t)(intptr_t)"nTestInt",
        2, (int32_t)(intptr_t)&g_float_target, (int32_t)(intptr_t)"fTestFloat",
        0, 0, 0, // sentinel record
    };
    ai_view v{};
    v.ai_scr_keyword_table = keyword_table;

    {
        // Case 1: both keywords present, mixed case, CRLF line endings -- both should parse.
        set_scr_file("ntestint 42\r\nFTESTFLOAT 3.5\r\n");
        g_int_target   = -1;
        g_float_target = -1.0f;
        detail::scr_parse(v, stub_calls(), (char *)"AI.SCR");
        ck(g_int_target == 42, "scr_parse: type-1 keyword parses via atoi into its int target");
        ck(g_float_target == 3.5f, "scr_parse: type-2 keyword parses via strtod into its float target");
    }
    {
        // Case 2: an UNKNOWN keyword silently aborts the rest of the file -- the second (otherwise
        // valid) keyword must NOT be applied.
        set_scr_file("unknownkeyword 1\r\nntestint 99\r\n");
        g_int_target = -1;
        detail::scr_parse(v, stub_calls(), (char *)"AI.SCR");
        ck(g_int_target == -1,
           "scr_parse: an unmatched keyword silently aborts the rest of the file (no later keyword "
           "is applied)");
    }
    {
        // Case 3: no leak -- run through the allocator/free path twice with FRESH buffers each time
        // and trust ASan (this binary's gate build) to catch a missed/double free.
        set_scr_file("ntestint 7\r\n");
        detail::scr_parse(v, stub_calls(), (char *)"AI.SCR");
        set_scr_file("ntestint 8\r\n");
        detail::scr_parse(v, stub_calls(), (char *)"AI.SCR");
        ck(g_int_target == 8, "scr_parse: repeated calls with fresh buffers each parse correctly, no "
                              "leak/double-free (ASan's to catch if there were one)");
    }
}

void test_ai1e_2026_08_07_spiral_table_init() {
    using mh::ai::spiral_offset;
    // Sized to the REAL region extent (131072 bytes / 2 = 65536 entries), NOT the fixture's 4096 --
    // see the block comment above test_ai1e_2026_08_07_hq_attack_scenario for why this test does not
    // reuse `fixture`.
    std::vector<spiral_offset> offsets(65536);
    std::vector<uint32_t>      ring_counts(128, 0);
    int32_t                    cell_count = -1;

    ai_store own{};
    own.spiral_offsets            = offsets.data();
    own.spiral_ring_cell_counts   = ring_counts.data();
    own.ai_tile_spiral_cell_count = &cell_count;

    detail::spiral_table_init(own, stub_calls());

    // The exact disc population count for radius 127 (dx*dx+dy*dy < 0x3f02, dx/dy in [-127,127]) is
    // a deterministic function of the loop bounds -- computed independently here rather than
    // hardcoded, so the assertion is checking the TRANSLATION's loop bounds/comparison operator
    // against an oracle derived the same way a reader would, not against a magic number copied from
    // a prior run.
    int32_t expected = 0;
    for (int32_t dx = -127; dx <= 127; ++dx)
        for (int32_t dy = -127; dy <= 127; ++dy)
            if ((uint32_t)(dx * dx + dy * dy) < 0x3f02u) ++expected;
    ck(cell_count == expected,
       "spiral_table_init: cell_count matches an independently-computed disc population for radius "
       "127");
    ck(cell_count > 0 && cell_count < 65536, "spiral_table_init: cell_count is in the real region's range");

    // Ring 0 is exactly the origin cell (dist_sq 0 <= 0*0): count 1.
    ck(ring_counts[0] == 1, "spiral_table_init: ring 0 contains only the origin cell");
    // Ring 1 adds the 4 orthogonal neighbours (dist_sq 1): cumulative count 5.
    ck(ring_counts[1] == 5, "spiral_table_init: ring 1's cumulative count includes the origin + 4 "
                            "orthogonal neighbours");
    // The table is sorted ascending by squared radius -- every entry's dist_sq at the ring 0 boundary
    // must be <= every entry's dist_sq past it.
    bool sorted = true;
    for (int32_t i = 1; i < cell_count; ++i) {
        const int32_t prev = (int32_t)offsets[i - 1].dx * offsets[i - 1].dx +
                             (int32_t)offsets[i - 1].dy * offsets[i - 1].dy;
        const int32_t cur = (int32_t)offsets[i].dx * offsets[i].dx + (int32_t)offsets[i].dy * offsets[i].dy;
        if (cur < prev) {
            sorted = false;
            break;
        }
    }
    ck(sorted, "spiral_table_init: the offset table is sorted ascending by squared radius after qsort");
}


// ---- score_reinforcement_unit ----------------------------------------------------------------------
//
// Pure: no ai_store, no ai_calls at all (its one CALL, utils_math_trunc, is folded into the same x87
// chain ai_mine_yield.h/ai_group_muster_pick.h use, so nothing to stub). Off 0x004d352c: a 4-slot
// weapon loop (skip a non-ground weapon and keep scanning; BREAK outright on an empty slot) whose
// running total is FISTP-truncated after every slot, then a multiplier ladder keyed on
// Unit[proto].type, walked from every jump target at 0x004d35b1-0x004d3601.
void test_ai1e_2026_08_07_slice2_score_reinforcement_unit() {
    fixture       f;
    const int32_t PROTO = 5; // arbitrary, within the 100-entry cfg_units table

    auto set_slot = [&](int32_t slot, uint8_t weapon_id) {
        f.cfg_units[PROTO].weapons[slot * 2]     = weapon_id;
        f.cfg_units[PROTO].weapons[slot * 2 + 1] = 0;
    };
    auto reset_weapons = [&] {
        for (int i = 0; i < 4; ++i) set_slot(i, 0);
    };

    // ---- the multiplier ladder, every arm, base score fixed at 10 (slot 0 = one ground weapon
    // worth 10, slot 1 empty -> BREAK) so only Unit[PROTO].type changes between cases.
    reset_weapons();
    set_slot(0, 1);
    f.cfg_weapons[1].target   = 1; // bit 0 -> ground-capable (0x004d356e TEST ..,0x1)
    f.cfg_weapons[1].power[2] = 10.0;
    const int32_t P           = 2;

    struct {
        int32_t     type;
        uint32_t    expect;
        const char *why;
    } cases[] = {
        {0, 10, "type 0 -> below every ladder arm -> default x1"},
        {1, 10, "type 1 -> default x1"},
        {UNIT_TYPE_A_INFANTRY_2, 20, "type 2 (A_INFANTRY_2) -> x2 (0x004d35e8)"},
        {UNIT_TYPE_A_INFANTRY_3, 30, "type 3 (A_INFANTRY_3) -> x3 (0x004d35ec/ef)"},
        {UNIT_TYPE_A_INFANTRY_4, 40, "type 4 (A_INFANTRY_4) -> x4 (0x004d35f3)"},
        {UNIT_TYPE_A_INFANTRY_5, 50, "type 5 (A_INFANTRY_5) -> x5 (0x004d35f8)"},
        {6, 10, "type 6 -> the gap BETWEEN the two infantry runs -> default x1"},
        {UNIT_TYPE_H_INFANTRY_2, 20, "type 7 (H_INFANTRY_2) -> x2"},
        {UNIT_TYPE_H_INFANTRY_3, 30, "type 8 (H_INFANTRY_3) -> x3"},
        {UNIT_TYPE_H_INFANTRY_4, 40, "type 9 (H_INFANTRY_4) -> x4"},
        {UNIT_TYPE_H_INFANTRY_5, 50, "type 0xa (H_INFANTRY_5) -> x5"},
        {0xb, 10, "type 0xb -> just above the human infantry run -> default x1"},
        {0x12, 10, "type 0x12 -> just below the zeroed heli band -> default x1"},
        {UNIT_TYPE_A_HELI_MOTHER, 0, "type 0x13 (A_HELI_MOTHER) -> the heli band's LOW end -> x0"},
        {UNIT_TYPE_H_HELI_CARGO, 0, "type 0x18 (H_HELI_CARGO) -> the heli band's HIGH end -> x0"},
        {0x19, 10, "type 0x19 -> just above the heli band -> default x1"},
        {0xff, 10, "type 0xff -> far past every arm -> default x1"},
    };
    for (auto &c : cases) {
        f.cfg_units[PROTO].type = c.type;
        const uint32_t got      = detail::score_reinforcement_unit(f.view(), P, PROTO);
        char           msg[192];
        snprintf(msg, sizeof(msg), "score_reinforcement_unit: %s (got %u, want %u)", c.why, got,
                 c.expect);
        ck(got == c.expect, msg);
    }

    // ---- MUTATION ARGUMENT 1: skip-vs-break. A non-ground weapon_id!=0 is SKIPPED (the loop keeps
    // scanning); an empty (weapon_id==0) slot BREAKS outright. This input separates a translation
    // that breaks on ANY non-contributing slot from the correct one, which breaks ONLY on empty.
    reset_weapons();
    set_slot(0, 2);
    f.cfg_weapons[2].target = 0; // NOT ground-capable, but weapon_id != 0
    set_slot(1, 1);
    f.cfg_weapons[1].target   = 1;
    f.cfg_weapons[1].power[P] = 5.0;
    f.cfg_units[PROTO].type   = 0; // x1, so the raw accumulation passes straight through
    ck(detail::score_reinforcement_unit(f.view(), P, PROTO) == 5,
       "score_reinforcement_unit: a non-ground weapon_id!=0 slot is SKIPPED, not a break (0x004d3575 "
       "JZ continues the loop rather than exiting it) -- a wrong impl that breaks here would return 0");

    // ---- MUTATION ARGUMENT 2: the converse -- an empty slot really does break, so a LATER slot's
    // weapon (99, impossible to miss if it were added) is never reached.
    reset_weapons();
    set_slot(1, 1);
    f.cfg_weapons[1].target   = 1;
    f.cfg_weapons[1].power[P] = 99.0;
    ck(detail::score_reinforcement_unit(f.view(), P, PROTO) == 0,
       "score_reinforcement_unit: weapon_id==0 BREAKS the scan outright -- a wrong impl that only "
       "skips empty slots (never breaking) would see slot 1's weapon and return 99");

    // ---- MUTATION ARGUMENT 3: FISTP truncates PER SLOT, not once over the double-precision sum.
    // trunc(trunc(0+1.9)+1.9) = trunc(1+1.9) = trunc(2.9) = 2, vs trunc(1.9+1.9) = trunc(3.8) = 3 if
    // a translation summed in double first and truncated only the final total.
    reset_weapons();
    set_slot(0, 1);
    f.cfg_weapons[1].target   = 1;
    f.cfg_weapons[1].power[P] = 1.9;
    set_slot(1, 1); // same weapon id -- only the SLOT differs, so this is a second independent add
    set_slot(2, 0); // break after two slots
    f.cfg_units[PROTO].type = 0;
    ck(detail::score_reinforcement_unit(f.view(), P, PROTO) == 2,
       "score_reinforcement_unit: truncates after EVERY accumulation step (2), which a "
       "sum-then-truncate-once translation (3) would get wrong");

    // ---- MUTATION ARGUMENT 4: the `player` argument selects Weapon[].power[player], not a
    // hardcoded/ignored column.
    reset_weapons();
    set_slot(0, 1);
    f.cfg_weapons[1].target   = 1;
    f.cfg_weapons[1].power[3] = 30.0;
    f.cfg_weapons[1].power[5] = 50.0;
    set_slot(1, 0);
    f.cfg_units[PROTO].type = 0;
    ck(detail::score_reinforcement_unit(f.view(), 3, PROTO) == 30,
       "score_reinforcement_unit: player 3 reads Weapon[].power[3] (30), not player 5's column");
    ck(detail::score_reinforcement_unit(f.view(), 5, PROTO) == 50,
       "score_reinforcement_unit: player 5 reads its OWN column (50) -- rules out a translation that "
       "hardcoded or ignored the player argument (which the case above alone could not rule out, since "
       "30 could coincidentally be 'the' answer for a hardcoded-player-3 implementation)");
}

// ---- create_reinforcement_unit ----------------------------------------------------------------------
//
// A two-way branch and nothing else: Unit[proto].type > 0xe (SIGNED, JG) -> gc.unit_create; otherwise
// gc.unit_create_soldier. Both get the literal 1 as their fifth argument (0x0046d917 / 0x0046d932).
void test_ai1e_2026_08_07_slice2_create_reinforcement_unit() {
    fixture        f;
    const uint32_t PROTO_SOLDIER    = 7;
    const uint32_t PROTO_VEHICLE    = 8;
    f.cfg_units[PROTO_SOLDIER].type = 0xe; // boundary: <= 0xe -> soldier path
    f.cfg_units[PROTO_VEHICLE].type = 0xf; // boundary: > 0xe -> unit_create path

    g_rec.clear();
    detail::create_reinforcement_unit(f.view(), stub_calls(), /*x*/ 100u, /*y*/ 200u, PROTO_SOLDIER,
                                      /*player*/ 3u);
    ck(g_rec.create_soldier_calls.size() == 1 && g_rec.create_unit_calls.empty(),
       "create_reinforcement_unit: Unit[proto].type==0xe (<=0xe) routes to unit_create_soldier, not "
       "unit_create");
    if (g_rec.create_soldier_calls.size() == 1) {
        const auto &c = g_rec.create_soldier_calls[0];
        ck(c.x == 100 && c.y == 200, "create_reinforcement_unit: soldier path's x,y pass through unchanged");
        ck(c.a2 == PROTO_SOLDIER, "create_reinforcement_unit: soldier path's 3rd arg is unit_proto_id");
        ck(c.param_4 == 3, "create_reinforcement_unit: soldier path's 4th arg is player");
        ck(c.param_5 == 1, "create_reinforcement_unit: soldier path's literal 5th arg is 1 (0x0046d917)");
    }

    g_rec.clear();
    detail::create_reinforcement_unit(f.view(), stub_calls(), /*x*/ 300u, /*y*/ 400u, PROTO_VEHICLE,
                                      /*player*/ 5u);
    ck(g_rec.create_unit_calls.size() == 1 && g_rec.create_soldier_calls.empty(),
       "create_reinforcement_unit: Unit[proto].type==0xf (>0xe) routes to unit_create, not "
       "unit_create_soldier");
    if (g_rec.create_unit_calls.size() == 1) {
        const auto &c = g_rec.create_unit_calls[0];
        ck(c.x == 300 && c.y == 400, "create_reinforcement_unit: unit_create path's x,y pass through unchanged");
        ck(c.unit == PROTO_VEHICLE, "create_reinforcement_unit: unit_create path's 3rd arg is unit_proto_id");
        ck(c.player == 5, "create_reinforcement_unit: unit_create path's 4th arg is player");
        ck(c.is_ship == 1, "create_reinforcement_unit: unit_create path's literal 5th arg is 1 too "
                           "(0x0046d932)");
    }
    // MUTATION ARGUMENT 5: the two cases above sit on EITHER SIDE of the SIGNED > 0xe boundary
    // (0xe routes one way, 0xf the other) -- a translation using >= instead of >, or an unsigned
    // compare instead of signed, would flip type==0xe to the unit_create branch, which the first
    // case's ck() on create_unit_calls.empty() would catch.
}

void test_ai1e_2026_08_07_slice2_unit_commit_attack_on_enemy_hq() {
    fixture  f;
    uint32_t hq_unit_id      = 7;
    ai_store own             = f.store();
    own.ai_attack_hq_unit_id = &hq_unit_id;
    const ai_view v          = f.view();

    f.b(0, 1).x = 11; // buildings[0][1] is the hardcoded human-HQ target
    f.b(0, 1).y = 22;

    // ---- each of the five early-out states, individually ----
    const uint16_t bail_states[] = {4, 0xa, 0xf, 0x1c, 0x1a};
    for (uint16_t st : bail_states) {
        f.u(1, hq_unit_id).state = st;
        g_rec.clear();
        detail::unit_commit_attack_on_enemy_hq(v, own, stub_calls());
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "unit_commit_attack_on_enemy_hq: state 0x%x is an early-out -- no calls at all", st);
        ck(g_rec.attack_reposition_calls.empty() && g_rec.order_move_calls.empty(), msg);
    }

    // ---- a state that is none of the five, including values ADJACENT to excluded ones (0xb next to
    // the excluded 0xa; 9 just before it; 0x1b next to the excluded 0x1a) -- catches an off-by-one in
    // the exclusion list.
    const uint16_t live_states[] = {0, 9, 0xb, 0x1b};
    for (uint16_t st : live_states) {
        f.u(1, hq_unit_id).state = st;
        g_rec.clear();
        g_rec.select_weapon_ans = 5; // != 0x64 -> attack path, so exactly one call either way
        detail::unit_commit_attack_on_enemy_hq(v, own, stub_calls());
        char msg[96];
        snprintf(msg, sizeof(msg), "unit_commit_attack_on_enemy_hq: state 0x%x is NOT an early-out", st);
        ck(g_rec.attack_reposition_calls.size() == 1, msg);
    }

    // ---- weapon == 0x64 ("no weapon mounted") -> the MOVE branch ----
    f.u(1, hq_unit_id).state = 0;
    g_rec.clear();
    g_rec.select_weapon_ans = 0x64;
    detail::unit_commit_attack_on_enemy_hq(v, own, stub_calls());
    ck(g_rec.order_move_calls.size() == 1 && g_rec.attack_reposition_calls.empty(),
       "unit_commit_attack_on_enemy_hq: weapon==0x64 takes the move branch, not the attack branch");
    if (g_rec.order_move_calls.size() == 1) {
        const auto &c = g_rec.order_move_calls[0];
        ck(c.player == 1 && c.unit_index == (int32_t)hq_unit_id,
           "unit_commit_attack_on_enemy_hq: move order targets player 1's designated unit");
        ck(c.x == 11 && c.y == 22,
           "unit_commit_attack_on_enemy_hq: move order's (x,y) is buildings[0][1]'s own tile");
        ck(c.arg5 == 0, "unit_commit_attack_on_enemy_hq: move order's 5th arg is the literal 0 "
                        "(0x004ba7be)");
    }

    // ---- weapon != 0x64 -> the ATTACK branch, exact hardcoded target ----
    g_rec.clear();
    g_rec.select_weapon_ans = 3;
    detail::unit_commit_attack_on_enemy_hq(v, own, stub_calls());
    ck(g_rec.attack_reposition_calls.size() == 1 && g_rec.order_move_calls.empty(),
       "unit_commit_attack_on_enemy_hq: weapon!=0x64 takes the attack branch, not the move branch");
    if (g_rec.attack_reposition_calls.size() == 1) {
        const auto &c = g_rec.attack_reposition_calls[0];
        ck(c.player == 1 && c.unit_index == (int32_t)hq_unit_id,
           "unit_commit_attack_on_enemy_hq: attack order targets player 1's designated unit");
        ck(c.target_ref == 0 && c.target_index == 1,
           "unit_commit_attack_on_enemy_hq: the hardcoded target is (target_ref=0,target_index=1) == "
           "buildings[0][1], the human HQ (0x004ba7a5/0x004ba7aa)");
        ck(c.weapon_id == 3, "unit_commit_attack_on_enemy_hq: weapon_id is select_weapon's own return "
                             "value, passed straight through");
    }

    // MUTATION ARGUMENT 6: weapon values immediately adjacent to the 0x64 sentinel must BOTH still
    // take the attack branch -- separates the exact-equality test (JZ) the original makes from a
    // translation that used >= or <= against 0x64.
    for (uint8_t w : {(uint8_t)0x63, (uint8_t)0x65}) {
        g_rec.clear();
        g_rec.select_weapon_ans = w;
        detail::unit_commit_attack_on_enemy_hq(v, own, stub_calls());
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "unit_commit_attack_on_enemy_hq: weapon 0x%x (adjacent to the 0x64 sentinel) takes "
                 "the attack branch",
                 w);
        ck(g_rec.attack_reposition_calls.size() == 1 && g_rec.order_move_calls.empty(), msg);
    }
}

// ---- invasion_spawn_reinforcements ------------------------------------------------------------------
//
// NOTE ON THE FIXTURE'S SPIRAL TABLE: `fixture::spiral` is value-initialised, so every entry is
// (dx=0,dy=0) -- every spiral index resolves to the SAME tile, the player's own (masked) home tile.
// That is deliberately exploited below: seeding exactly that one cell as PASSABLE makes every spawn
// attempt succeed on its first probe, with no real spiral geometry needed for anything this test
// checks (selection / round-robin / trim / caps, not geometry). NOT covered here: the persisted,
// never-reset min-index in the trim loop when >4 candidates ALL score >= 1,000,000 (the acknowledged
// R3 finding) -- exercising it needs scores that large, which is a state I judged not worth
// constructing offline; the normal (score << 1e6) trim path below IS covered and passes through the
// same code.
void test_ai1e_2026_08_07_slice2_invasion_spawn_reinforcements() {
    // ---- the SIGNED >0 bail gate, and that it makes no calls at all ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 0;
        g_rec.clear();
        ck(detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P) == 0,
           "invasion_spawn_reinforcements: ai_invasion_points==0 bails, returning 0");
        ck(g_rec.score_calls.empty() && g_rec.reinforcement_spawns.empty(),
           "invasion_spawn_reinforcements: the bail makes NO score/create calls at all");
    }
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = -5;
        g_rec.clear();
        ck(detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P) == 0,
           // MUTATION ARGUMENT: a negative value also bails under the real SIGNED >0 test (JG); a
           // translation that used an unsigned or a !=0 test would treat -5 as a huge positive count
           // and proceed to spawn.
           "invasion_spawn_reinforcements: a NEGATIVE ai_invasion_points also bails (the gate is "
           "SIGNED >0, not an unsigned/nonzero test)");
    }

    // ---- zero-candidate path: zeroes points, spawns nothing (the original also calls the proven
    // no-op debug-log stub here; we no longer do -- SIMABI-HOOKS 2026-09-10) ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 10;
        f.prog_sec.total                = 0; // Progress[0] stays type 0 (not INVENTION_TYPE_UNIT) -> no candidate
        g_rec.clear();
        const int32_t r = detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        ck(r == 0, "invasion_spawn_reinforcements: no surviving candidate returns 0");
        ck(f.players[P].ai_invasion_points == 0,
           "invasion_spawn_reinforcements: the zero-candidate path ZEROES ai_invasion_points");
        ck(g_rec.reinforcement_spawns.empty(), "invasion_spawn_reinforcements: nothing spawned");
    }

    // ---- the scan bound is INCLUSIVE: the row AT index==cfg_progress_sec->total participates ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 100;
        f.prog_sec.total                = 3;
        f.inventions[3].type            = INVENTION_TYPE_UNIT;
        f.inventions[3].index           = 20;
        f.prog(P, 3).available          = 1;
        f.cfg_units[20].type            = 0;
        f.players[P].ai_home_tile_x     = 10;
        f.players[P].ai_home_tile_y     = 10;
        f.passable[(10 << 8) | 10]      = REINFORCEMENT_SPAWN_PASSABLE_A;
        g_rec.clear();
        g_rec.score_by_proto[20] = 5;
        const int32_t r          = detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        ck(r >= 1,
           "invasion_spawn_reinforcements: the row AT index==total (the INCLUSIVE bound, JBE not JC "
           "at 0x004e887a) is scanned and can spawn");
        ck(!g_rec.reinforcement_spawns.empty() && g_rec.reinforcement_spawns[0].proto == 20,
           "invasion_spawn_reinforcements: the spawned candidate is the boundary-row one");
    }

    // ---- the six excluded heli types, both ends of the range plus its immediate neighbours ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 100;
        f.prog_sec.total                = 3;
        const int32_t types[4]          = {0x12, (int32_t)UNIT_TYPE_H_HELI_CARGO /*0x18*/, 0x19,
                                           (int32_t)UNIT_TYPE_A_HELI_MOTHER /*0x13*/};
        for (int i = 0; i < 4; ++i) {
            f.inventions[i].type     = INVENTION_TYPE_UNIT;
            f.inventions[i].index    = 40 + i;
            f.prog(P, i).available   = 1;
            f.cfg_units[40 + i].type = types[i];
        }
        f.players[P].ai_home_tile_x = 10;
        f.players[P].ai_home_tile_y = 10;
        f.passable[(10 << 8) | 10]  = REINFORCEMENT_SPAWN_PASSABLE_A;
        g_rec.clear();
        g_rec.score_by_proto[40] = 5;
        g_rec.score_by_proto[41] = 5;
        g_rec.score_by_proto[42] = 5;
        g_rec.score_by_proto[43] = 5;
        detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        bool saw[4] = {};
        for (auto &s : g_rec.reinforcement_spawns)
            for (int i = 0; i < 4; ++i)
                if (s.proto == (uint32_t)(40 + i)) saw[i] = true;
        ck(saw[0], "invasion_spawn_reinforcements: type 0x12 (just below the excluded range) is eligible");
        ck(!saw[1], "invasion_spawn_reinforcements: type 0x18 (H_HELI_CARGO, the range's HIGH end) is "
                    "rejected even with a nonzero score");
        ck(saw[2], "invasion_spawn_reinforcements: type 0x19 (just above the excluded range) is eligible");
        ck(!saw[3], "invasion_spawn_reinforcements: type 0x13 (A_HELI_MOTHER, the range's LOW end) is "
                    "rejected -- together with the 0x18 case, this rules out a translation that tested "
                    "only one endpoint or the wrong pair");
    }

    // ---- the score filter: score_reinforcement_unit==0 excludes the candidate ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 10;
        f.prog_sec.total                = 0;
        f.inventions[0].type            = INVENTION_TYPE_UNIT;
        f.inventions[0].index           = 50;
        f.prog(P, 0).available          = 1;
        f.cfg_units[50].type            = 0;
        g_rec.clear();
        g_rec.score_by_proto[50] = 0;
        const int32_t r          = detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        ck(r == 0 && g_rec.reinforcement_spawns.empty(),
           "invasion_spawn_reinforcements: score_reinforcement_unit==0 excludes the candidate "
           "(0x004e8800 TEST/JZ)");
    }

    // ---- the AND chain: type filter, availability filter, and a row that clears both ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 10;
        f.prog_sec.total                = 2;
        f.inventions[0].type            = 0; // not INVENTION_TYPE_UNIT
        f.inventions[0].index           = 60;
        f.prog(P, 0).available          = 1;
        f.inventions[1].type            = INVENTION_TYPE_UNIT;
        f.inventions[1].index           = 61;
        f.prog(P, 1).available          = 0; // not available to this player
        f.inventions[2].type            = INVENTION_TYPE_UNIT;
        f.inventions[2].index           = 62;
        f.prog(P, 2).available          = 1;
        f.cfg_units[62].type            = 0;
        f.players[P].ai_home_tile_x     = 5;
        f.players[P].ai_home_tile_y     = 5;
        f.passable[(5 << 8) | 5]        = REINFORCEMENT_SPAWN_PASSABLE_B; // exercises the OTHER accepted value
        g_rec.clear();
        g_rec.score_by_proto[60] = 5;
        g_rec.score_by_proto[61] = 5;
        g_rec.score_by_proto[62] = 5;
        detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        bool saw60 = false, saw61 = false, saw62 = false;
        for (auto &s : g_rec.reinforcement_spawns) {
            if (s.proto == 60) saw60 = true;
            if (s.proto == 61) saw61 = true;
            if (s.proto == 62) saw62 = true;
        }
        ck(!saw60, "invasion_spawn_reinforcements: a non-INVENTION_TYPE_UNIT row is excluded");
        ck(!saw61, "invasion_spawn_reinforcements: an unavailable-to-this-player row is excluded");
        ck(saw62, "invasion_spawn_reinforcements: a row passing all three filters is eligible, and "
                  "PASSABLE value 5 (not just 0) is accepted as spawnable");
    }

    // ---- round-robin order and the ordinary points-reach-0 exit (no re-zeroing needed) ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 3;
        f.prog_sec.total                = 1;
        f.inventions[0].type            = INVENTION_TYPE_UNIT;
        f.inventions[0].index           = 70;
        f.prog(P, 0).available          = 1;
        f.cfg_units[70].type            = 0;
        f.inventions[1].type            = INVENTION_TYPE_UNIT;
        f.inventions[1].index           = 71;
        f.prog(P, 1).available          = 1;
        f.cfg_units[71].type            = 0;
        f.players[P].ai_home_tile_x     = 1;
        f.players[P].ai_home_tile_y     = 1;
        f.passable[(1 << 8) | 1]        = 0;
        g_rec.clear();
        g_rec.score_by_proto[70] = 5;
        g_rec.score_by_proto[71] = 7;
        const int32_t r          = detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        ck(r == 3, "invasion_spawn_reinforcements: spawns exactly ai_invasion_points units when never capped");
        ck(f.players[P].ai_invasion_points == 0,
           "invasion_spawn_reinforcements: the ordinary points-reach-0 exit decrements to 0 itself "
           "(it does not ALSO re-zero afterward, unlike the cap-100 exit)");
        ck(g_rec.reinforcement_spawns.size() == 3, "invasion_spawn_reinforcements: 3 create calls");
        if (g_rec.reinforcement_spawns.size() == 3) {
            ck(g_rec.reinforcement_spawns[0].proto == 70 && g_rec.reinforcement_spawns[1].proto == 71 &&
                   g_rec.reinforcement_spawns[2].proto == 70,
               "invasion_spawn_reinforcements: round-robins the candidate list in scan order, "
               "wrapping after the last (70,71,70 for a 2-candidate list and 3 spawns) -- a "
               "translation that re-scanned from the start each time, or never wrapped, would not "
               "reproduce this exact sequence");
            ck(g_rec.reinforcement_spawns[0].x == 1 && g_rec.reinforcement_spawns[0].y == 1,
               "invasion_spawn_reinforcements: the spawn tile is the player's own (masked) home tile");
        }
    }

    // ---- trim-to-4: removes the two LOWEST-scoring of six candidates ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 4;
        f.prog_sec.total                = 5;
        const int32_t  protos[6]        = {80, 81, 82, 83, 84, 85};
        const uint32_t scores[6]        = {50, 10, 40, 20, 60, 30}; // min two: idx1(10), idx3(20)
        for (int i = 0; i < 6; ++i) {
            f.inventions[i].type        = INVENTION_TYPE_UNIT;
            f.inventions[i].index       = protos[i];
            f.prog(P, i).available      = 1;
            f.cfg_units[protos[i]].type = 0;
        }
        f.players[P].ai_home_tile_x = 2;
        f.players[P].ai_home_tile_y = 2;
        f.passable[(2 << 8) | 2]    = 0;
        g_rec.clear(); // clear() wipes score_by_proto -- seed it AFTER, not before
        for (int i = 0; i < 6; ++i) g_rec.score_by_proto[protos[i]] = scores[i];
        detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        bool saw[6] = {};
        for (auto &s : g_rec.reinforcement_spawns)
            for (int i = 0; i < 6; ++i)
                if (s.proto == (uint32_t)protos[i]) saw[i] = true;
        ck(!saw[1] && !saw[3],
           "invasion_spawn_reinforcements: trim-to-4 removes the two LOWEST-scoring candidates "
           "(proto 81 score 10, proto 83 score 20) over its two outer passes (6->5->4)");
        ck(saw[0] && saw[2] && saw[4] && saw[5],
           "invasion_spawn_reinforcements: the four highest-scoring candidates all survive the trim");
    }

    // ---- the 100-spawn cap: stops even with ample points left, and explicitly zeroes them ----
    {
        fixture       f;
        const int32_t P                 = 1;
        f.players[P].ai_invasion_points = 1000;
        f.prog_sec.total                = 0;
        f.inventions[0].type            = INVENTION_TYPE_UNIT;
        f.inventions[0].index           = 90;
        f.prog(P, 0).available          = 1;
        f.cfg_units[90].type            = 0;
        f.players[P].ai_home_tile_x     = 3;
        f.players[P].ai_home_tile_y     = 3;
        f.passable[(3 << 8) | 3]        = 0;
        g_rec.clear();
        g_rec.score_by_proto[90] = 5;
        const int32_t r          = detail::invasion_spawn_reinforcements(f.view(), f.store(), stub_calls(), P);
        ck(r == REINFORCEMENT_SPAWN_CAP,
           "invasion_spawn_reinforcements: the 100-spawn cap stops the loop even with 1000 points left");
        ck(f.players[P].ai_invasion_points == 0,
           "invasion_spawn_reinforcements: the CAP exit explicitly zeroes ai_invasion_points "
           "(0x004e89e7), the same end state the ordinary exit reaches by simple decrement");
        ck(g_rec.reinforcement_spawns.size() == 100,
           "invasion_spawn_reinforcements: exactly 100 spawns, no more");
    }
}

// ---- player_tick ---------------------------------------------------------------------------------
//
// NO SHADOW SITE (778 reachable functions, 127 escape candidates -- see ai_player_tick.h). Its own
// behaviour IS the dispatch sequence, so these tests check the sequence and the handful of local
// computations (the expand-gate x87 chain, the one-shot spawn edge) rather than what its ~20 callees
// do internally (each is tested, or will be tested, on its own).
void test_ai1e_2026_08_07_slice2_player_tick() {
    // ---- x87_sqrt_and_trunc's OWN correctness, independent of player_tick's wiring: trunc(sqrt(n))
    // against a standard-library oracle, over perfect squares and their immediate neighbours.
    {
        auto           isqrt_trunc = [](uint32_t n) -> int32_t { return (int32_t)(uint32_t)std::sqrt((double)n); };
        const uint32_t probes[]    = {0, 1, 2, 3, 4, 15, 16,
                                      17, 99, 100, 101, 65535, 65536,
                                      65537, 1000000, 0xffffffffu};
        for (uint32_t n : probes) {
            const int32_t got  = detail::x87_sqrt_and_trunc(n);
            const int32_t want = isqrt_trunc(n);
            char          msg[96];
            snprintf(msg, sizeof(msg), "x87_sqrt_and_trunc(%u) = %d, want trunc(sqrt(n)) = %d", n, got,
                     want);
            ck(got == want, msg);
        }
    }

    // ---- not-alive: an early return that makes NO outward calls at all ----
    {
        fixture       f;
        const int32_t P            = 2;
        f.profiles[P].status_flags = 0; // ALIVE bit clear
        g_rec.clear();
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        ck(g_rec.slice2_log.empty(),
           "player_tick: not-alive is an early return (0x004e8bc1 JZ) -- no outward calls whatsoever");
    }

    // ---- the invasion-force branch: EXACTLY its two callees, in order, and nothing else ----
    {
        fixture       f;
        const int32_t P                = 2;
        f.profiles[P].status_flags     = PLAYER_STATUS_ALIVE;
        f.players[P].ai_invasion_force = 1;
        g_rec.clear();
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        const std::vector<std::string> want = {"invasion_spawn_reinforcements(2)",
                                               "invasion_launch_attack_group(2)"};
        ck(g_rec.slice2_log == want,
           "player_tick: ai_invasion_force!=0 calls EXACTLY invasion_spawn_reinforcements then "
           "invasion_launch_attack_group, then returns -- nothing else runs (0x004e8be8-0x004e8bf6)");
    }

    // ---- the one-shot spawn edge: fires once (ai_established==0), sets the latch, and the exact
    // GetStartingUnit / unit_create / unit_order_move_with_bump sequence + arguments ----
    {
        fixture       f;
        const int32_t P                = 2;
        f.profiles[P].status_flags     = PLAYER_STATUS_ALIVE;
        f.players[P].ai_invasion_force = 0;
        f.players[P].ai_established    = 0;
        f.players[P].is_alien_race     = 0; // human -> GetStartingUnit(race=1)
        f.players[P].ai_home_tile_x    = 15;
        f.players[P].ai_home_tile_y    = 25;
        f.players[P].ai_score_cat_1    = 0; // keep the expand-gate on the flat-5 arm for this case
        g_rec.clear();
        g_rec.starting_unit_ans = 88;
        g_rec.unit_create_ans   = 999;
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        ck(f.players[P].ai_established == 1, "player_tick: the one-shot edge sets ai_established");
        const std::vector<std::string> want = {
            "GetStartingUnit(race=1)",
            "unit_create(x=15,y=26,unit=88,player=2,is_ship=2)",
            "unit_order_move_with_bump(player=2,unit=999,x=15,y=25)",
            "resource_spend_rate_update(2)",
            "score_build_categories(2)",
            "update_opponent_relations(assessed=2)",
            "recompute_shortage_state(2)",
            "bldg_queue_process(2)",
        };
        ck(g_rec.slice2_log == want,
           "player_tick: the one-shot edge's exact call sequence -- unit_create's y is "
           "ai_home_tile_y+1 (26) but unit_order_move_with_bump's y is the UNINCREMENTED "
           "ai_home_tile_y (25), and is_ship is literal 2 (0x004e8c20), not 1");
        ck(g_rec.opponent_relation_calls.size() == 1 && g_rec.opponent_relation_calls[0].first == 2 &&
               g_rec.opponent_relation_calls[0].second ==
                   (void *)&f.players[P].ai_opponent_assessments[2],
           "player_tick: update_opponent_relations's out-pointer is THIS player's OWN "
           "ai_opponent_assessments[assessed] -- rules out a translation that indexed a different "
           "player's record or a different assessment slot");
    }

    // ---- the expand-gate, BOTH arms ----
    {
        fixture       f;
        const int32_t P             = 3;
        f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
        f.players[P].ai_established = 1; // skip the one-shot block -- keeps this case's log short
        f.players[P].ai_score_cat_1 = 7; // nonzero -> the x87 sqrt arm
        f.players[P].ai_home_tile_x = 30;
        f.players[P].ai_home_tile_y = 40;
        g_rec.clear();
        g_rec.defense_radius_sq_ans = 200; // not a perfect square -- real truncation, not a coincidence
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        const int32_t expected_gate = detail::x87_sqrt_and_trunc(200);
        ck(f.players[P].ai_expand_gate_value == expected_gate,
           "player_tick: score_cat_1!=0 sets ai_expand_gate_value = trunc(sqrt(bldg_max_defense_"
           "radius_sq(player,ai_home_tile_x,ai_home_tile_y)))");
        bool saw_radius_call = false;
        for (auto &l : g_rec.slice2_log)
            if (l == "bldg_max_defense_radius_sq(player=3,x=30,y=40)") saw_radius_call = true;
        ck(saw_radius_call,
           "player_tick: the radius query's arguments are (player,ai_home_tile_x,ai_home_tile_y)");
    }
    {
        fixture       f;
        const int32_t P             = 4;
        f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
        f.players[P].ai_established = 1;
        f.players[P].ai_score_cat_1 = 0; // -> flat 5, no radius/sqrt call at all
        g_rec.clear();
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        ck(f.players[P].ai_expand_gate_value == 5,
           "player_tick: score_cat_1==0 sets ai_expand_gate_value = 5 flat (0x004e8cd4)");
        bool called_radius = false;
        for (auto &l : g_rec.slice2_log)
            if (l.find("bldg_max_defense_radius_sq") != std::string::npos) called_radius = true;
        ck(!called_radius,
           "player_tick: the flat-5 arm never calls bldg_max_defense_radius_sq -- rules out a "
           "translation that always computes the sqrt and only conditionally uses it");
    }

    // ---- each phase-flag gate firing only under its own bit AND its own extra conditions ----
    {
        // AI_PHASE_UNIT_TRAINING (0x2): needs the bit, established, AND score_cat_1.
        auto fires_training = [&](uint8_t phase_flags, int32_t established, int32_t score_cat_1) {
            fixture       f;
            const int32_t P             = 5;
            f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
            f.players[P].ai_established = established;
            f.players[P].ai_phase_flags = phase_flags;
            f.players[P].ai_score_cat_1 = score_cat_1;
            g_rec.clear();
            detail::player_tick(f.view(), f.store(), stub_calls(), P);
            for (auto &l : g_rec.slice2_log)
                if (l == "plan_unit_training(5)") return true;
            return false;
        };
        ck(fires_training(AI_PHASE_UNIT_TRAINING, 1, 1),
           "player_tick: plan_unit_training fires when its bit + established + score_cat_1 all hold");
        ck(!fires_training(0, 1, 1),
           "player_tick: plan_unit_training does NOT fire without AI_PHASE_UNIT_TRAINING, even with "
           "established and score_cat_1 both true");
        // ENTERING WITH established==0 STILL FIRES IT, and that is not a loophole -- it is the
        // function's own ordering. The one-shot spawn edge sets ai_established = 1 at 0x004e8c6c,
        // and the gate does not read the field until 0x004e8d58, re-loading it from memory
        // (CMP dword ptr [EAX + 0xe6ded0],0x0). So no call of this function can ever reach the gate
        // with established still 0. This assertion was written the other way round by the test
        // author (working from the .asm, deliberately not from the translation) and the
        // implementation is what proved it wrong -- kept inverted rather than deleted, because it is
        // now a REGRESSION TEST FOR THE ORDERING: a translation that hoisted the `established` read
        // to function entry, or that cached it across the spawn edge, would not fire here.
        ck(fires_training(AI_PHASE_UNIT_TRAINING, 0, 1),
           "player_tick: entering with established==0, the spawn edge sets it (0x004e8c6c) BEFORE the "
           "gate re-reads it (0x004e8d58), so plan_unit_training still fires -- rules out a "
           "translation that reads ai_established once at entry");
        {
            // ...and prove it was the spawn edge that did it, not a missing gate: the same run must
            // contain the spawn calls, and the field must be 1 afterwards.
            fixture       f;
            const int32_t P             = 5;
            f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
            f.players[P].ai_established = 0;
            f.players[P].ai_phase_flags = AI_PHASE_UNIT_TRAINING;
            f.players[P].ai_score_cat_1 = 1;
            g_rec.clear();
            detail::player_tick(f.view(), f.store(), stub_calls(), P);
            bool saw_create = false;
            for (auto &l : g_rec.slice2_log)
                if (l.find("unit_create(") != std::string::npos) saw_create = true;
            ck(saw_create, "player_tick: entering with established==0 runs the one-shot spawn edge");
            ck(f.players[P].ai_established == 1,
               "player_tick: the spawn edge leaves ai_established == 1 (0x004e8c6c)");
        }
        ck(!fires_training(AI_PHASE_UNIT_TRAINING, 1, 0),
           "player_tick: plan_unit_training does NOT fire with score_cat_1==0, even with its bit set");
        ck(!fires_training(AI_PHASE_CONSTRUCTION | AI_PHASE_UNIT_GROUPS, 1, 1),
           "player_tick: the OTHER two phase bits set (but not 0x2) still do not fire "
           "plan_unit_training -- rules out a translation that OR'd in the wrong bit");
    }
    {
        // AI_PHASE_CONSTRUCTION (0x1): gates BOTH plan_construction and scan_bldg_repair_upgrade together.
        auto fires_construction = [&](uint8_t phase_flags) {
            fixture       f;
            const int32_t P             = 5;
            f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
            f.players[P].ai_established = 1;
            f.players[P].ai_phase_flags = phase_flags;
            f.players[P].ai_score_cat_1 = 1;
            g_rec.clear();
            detail::player_tick(f.view(), f.store(), stub_calls(), P);
            bool saw_plan = false, saw_scan = false;
            for (auto &l : g_rec.slice2_log) {
                if (l == "plan_construction(5)") saw_plan = true;
                if (l == "scan_bldg_repair_upgrade(5)") saw_scan = true;
            }
            return saw_plan && saw_scan;
        };
        ck(fires_construction(AI_PHASE_CONSTRUCTION),
           "player_tick: AI_PHASE_CONSTRUCTION fires BOTH plan_construction and scan_bldg_repair_"
           "upgrade");
        ck(!fires_construction(AI_PHASE_UNIT_TRAINING | AI_PHASE_UNIT_GROUPS),
           "player_tick: without AI_PHASE_CONSTRUCTION, neither call fires, even with the other two "
           "bits set");
    }
    {
        // AI_PHASE_UNIT_GROUPS (0x4): needs established alone -- deliberately NOT score_cat_1, unlike
        // the two gates above. score_cat_1==0 here is what proves the difference.
        fixture       f;
        const int32_t P             = 5;
        f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
        f.players[P].ai_established = 1;
        f.players[P].ai_phase_flags = AI_PHASE_UNIT_GROUPS;
        f.players[P].ai_score_cat_1 = 0;
        f.players[P].ai_group_count = 0; // keep this case focused on the gate, not the group loop
        g_rec.clear();
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        bool saw = false;
        for (auto &l : g_rec.slice2_log)
            if (l == "group_home_guard_replenish(5)") saw = true;
        ck(saw, "player_tick: AI_PHASE_UNIT_GROUPS fires the group-formation block on established "
                "ALONE -- unlike the other two phase gates, it does not also require score_cat_1");
        g_rec.clear();
        f.players[P].ai_phase_flags = 0;
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        saw = false;
        for (auto &l : g_rec.slice2_log)
            if (l == "group_home_guard_replenish(5)") saw = true;
        ck(!saw, "player_tick: without AI_PHASE_UNIT_GROUPS the group-formation block does not fire");
    }

    // ---- the group loop's DOUBLE INCREMENT: a redistributed group's successor is provably skipped ----
    {
        fixture       f;
        const int32_t P             = 6;
        f.profiles[P].status_flags  = PLAYER_STATUS_ALIVE;
        f.players[P].ai_established = 1;
        f.players[P].ai_phase_flags = AI_PHASE_UNIT_GROUPS;
        f.players[P].ai_score_cat_1 = 0;
        f.players[P].ai_group_count = 8;
        for (int g = 0; g < 8; ++g) f.players[P].ai_groups[g].member_count = 0;
        f.players[P].ai_groups[5].member_count = 3; // >0 -> REDISTRIBUTE (double-increment)
        f.players[P].ai_groups[6].member_count = 4; // >0 -> WOULD redistribute if ever visited
        f.players[P].ai_groups[7].member_count = 0; // <=0 -> REMOVE
        g_rec.clear();
        detail::player_tick(f.view(), f.store(), stub_calls(), P);
        bool saw5 = false, saw6 = false, saw7 = false;
        for (auto &l : g_rec.slice2_log) {
            if (l == "group_redistribute_units(player=6,group=5)") saw5 = true;
            if (l.find("group=6") != std::string::npos) saw6 = true;
            if (l == "group_remove(player=6,group=7)") saw7 = true;
        }
        ck(saw5, "player_tick: group 5 (member_count>0) gets group_redistribute_units");
        ck(!saw6,
           "player_tick: group 6 is NEVER VISITED -- the redistribute path's double increment "
           "(0x004e8e61 plus the shared 0x004e8e6d) jumps straight from group 5 to group 7. This is "
           "exactly the divergence a 'tidied' single-increment loop would silently introduce");
        ck(saw7,
           "player_tick: group 7 (member_count<=0) gets group_remove, proving the loop DID reach "
           "index 7 -- so group 6's absence above is the skip, not the loop stopping early");
    }
}

// ---- active_unit_tick -----------------------------------------------------------------------------
//
// SHADOW-ARMED in production (48 reachable fns, 0 escape candidates -- see ai_active_unit_tick.h), so
// this is a SECOND, independent arm on top of the rig's differential oracle, not the only one. Covers:
// the entry relation stamp (identical to player_tick's own tail), the four-way goal dispatch, the
// linked-list unit walk terminating at ai_group_next==0, and the energy>0.0 gate. NOT COVERED: the
// sort+scoring tail (turret-threat window, weapon-eligibility scoring, best-candidate selection,
// swap-remove, the incoming-damage tail) -- reaching it needs the GLOBAL scan-target scratch
// (_G_LLM_STRAT_AI_SCAN_TARGETS) bound in the fixture, which it is not (see ai_state.h and the
// fixture's scan_target_count_v comment); every case below deliberately keeps scan_target_count at 0
// so the tail is never entered. A future session wanting that coverage should extend `fixture` with
// `scan_targets`/`attack_candidates` array bindings first.
//
// ALSO NOT TESTED: the `player & 0xf` mask on the energy read (batch context section 2 flags it as
// confirmed-real, not a decompiler artifact). It is untestable within this fixture's bounds: the only
// values that would make player != (player&0xf) are >= MAX_PLAYERS (8), and MAX_PLAYERS is exactly
// this fixture's (and the game's) valid player range for `units`/`player_data`, so driving player=20
// to observe the mask would read/write out of bounds on the SAME call whose masking is under test.
void test_ai1e_2026_08_07_slice2_active_unit_tick() {
    // ---- entry relation stamp (mirrors player_tick's own tail, same 4 instructions) ----
    {
        fixture       f;
        const int32_t P             = 1;
        f.players[P].ai_group_count = 0; // empty loop -> isolates the stamp from everything else
        f.foreign_flag              = 0;
        g_rec.clear();
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);
        ck(f.players[P].ai_player_relation[P] == 1,
           "active_unit_tick: entry stamp sets ai_player_relation[player][player]=1 when "
           "foreign_bldg_change_flag==0");
        f.foreign_flag = 5;
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);
        ck(f.players[P].ai_player_relation[P] == -1,
           "active_unit_tick: entry stamp sets it to -1 when foreign_bldg_change_flag!=0");
    }

    // ---- goal dispatch: group index 0 ALWAYS scans, regardless of its own goal ----
    {
        fixture       f;
        const int32_t P                     = 2;
        f.players[P].ai_group_count         = 1;
        f.players[P].ai_groups[0].goal      = 999; // irrelevant -- index 0 wins regardless
        f.players[P].ai_groups[0].head_unit = 0;
        g_rec.clear();
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);
        const std::vector<std::string> want = {"holding_pen_scan_targets(pen=0)",
                                               "target_list_invalidate_by_id(id=0)",
                                               "target_list_refresh_mothers()",
                                               "target_list_scan_visible_enemies()"};
        ck(g_rec.slice2_log == want,
           "active_unit_tick: group index 0 always takes the holding-pen sequence regardless of its "
           "own goal field (0x004ecba4 TEST EDX,EDX runs BEFORE any goal comparison)");
    }

    // ---- goal in {3,8,10,0xb} ----
    {
        fixture       f;
        const int32_t P                     = 2;
        f.players[P].ai_group_count         = 2;
        f.players[P].ai_groups[0].goal      = 999;
        f.players[P].ai_groups[0].head_unit = 0;
        f.players[P].ai_groups[1].goal      = 8; // one member of {3,8,10,0xb}
        f.players[P].ai_groups[1].head_unit = 0;
        g_rec.clear();
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);
        const std::vector<std::string> want = {"holding_pen_scan_targets(pen=1)",
                                               "target_list_invalidate_by_id(id=1)",
                                               "group_seed_resolved_target(group=1)"};
        const bool                     found =
            std::search(g_rec.slice2_log.begin(), g_rec.slice2_log.end(), want.begin(), want.end()) !=
            g_rec.slice2_log.end();
        ck(found, "active_unit_tick: goal==8 (in {3,8,10,0xb}) dispatches holding_pen_scan_targets / "
                  "invalidate(self) / group_seed_resolved_target as a contiguous run");
    }

    // ---- goal == 7 ----
    {
        fixture       f;
        const int32_t P                             = 2;
        f.players[P].ai_group_count                 = 2;
        f.players[P].ai_groups[0].goal              = 999;
        f.players[P].ai_groups[0].head_unit         = 0;
        f.players[P].ai_groups[1].goal              = 7;
        f.players[P].ai_groups[1].head_unit         = 0;
        f.players[P].ai_groups[1].link_target_group = 42;
        g_rec.clear();
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);
        const std::vector<std::string> want = {"holding_pen_scan_targets(pen=1)",
                                               "target_list_invalidate_by_id(id=1)",
                                               "target_list_invalidate_by_id(id=42)"};
        const bool                     found =
            std::search(g_rec.slice2_log.begin(), g_rec.slice2_log.end(), want.begin(), want.end()) !=
            g_rec.slice2_log.end();
        ck(found, "active_unit_tick: goal==7 invalidates the group itself (id=1) AND its "
                  "link_target_group (id=42) -- two DIFFERENT ids, not the same id twice");
    }

    // ---- any OTHER goal: no scan producer runs, and the shared body is never entered at all ----
    {
        fixture       f;
        const int32_t P                     = 2;
        f.players[P].ai_group_count         = 2;
        f.players[P].ai_groups[0].goal      = 999;
        f.players[P].ai_groups[0].head_unit = 0;
        f.players[P].ai_groups[1].goal      = 42;  // none of {index0, 3,8,10,0xb, 7}
        f.players[P].ai_groups[1].head_unit = 123; // deliberately nonzero -- must never be read
        g_rec.clear();
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);
        const std::vector<std::string> want = {"holding_pen_scan_targets(pen=0)",
                                               "target_list_invalidate_by_id(id=0)",
                                               "target_list_refresh_mothers()",
                                               "target_list_scan_visible_enemies()"};
        ck(g_rec.slice2_log == want,
           "active_unit_tick: an unrecognised goal (42) runs no scan producer AND never enters the "
           "shared unit-walk body -- group 1's head_unit (123, deliberately nonzero) is never read, "
           "so the whole log is exactly group 0's own four calls");
    }

    // ---- the linked-list unit walk: idle/pending-abandon/pending-no-abandon/energy-gated, and
    // termination at ai_group_next==0 ----
    {
        fixture       f;
        const int32_t P                     = 2;
        f.players[P].ai_group_count         = 2;
        f.players[P].ai_groups[0].goal      = 999;
        f.players[P].ai_groups[0].head_unit = 0;
        f.players[P].ai_groups[1].goal      = 8; // dispatches, then the shared walk runs
        f.players[P].ai_groups[1].head_unit = 10;
        f.u(P, 10).energy                   = 5.0;
        f.u(P, 10).ai_group_next            = 11;
        f.u(P, 11).energy                   = 5.0;
        f.u(P, 11).ai_group_next            = 12;
        f.u(P, 12).energy                   = 5.0;
        f.u(P, 12).ai_group_next            = 13;
        f.u(P, 13).energy                   = 0.0; // <=0 -- the gate body is skipped, the walk still ADVANCES
        f.u(P, 13).ai_group_next            = 0;   // terminates the chain

        g_rec.clear();
        g_rec.pending_by_unit[10] = 0; // idle -> attack_candidate_add
        g_rec.pending_by_unit[11] = 1;
        g_rec.abandon_by_unit[11] = 1; // pending + should-abandon -> issue_default_order
        g_rec.pending_by_unit[12] = 1;
        g_rec.abandon_by_unit[12] = 0; // pending, should NOT abandon -> neither call
        detail::active_unit_tick(f.view(), f.store(), stub_calls(), P);

        auto count_of = [&](const char *needle) {
            int n = 0;
            for (auto &l : g_rec.slice2_log)
                if (l.find(needle) != std::string::npos) ++n;
            return n;
        };
        ck(count_of("attack_candidate_add(unit=10)") == 1,
           "active_unit_tick: an idle unit (energy>0, order not pending) is offered as an attack "
           "candidate");
        ck(count_of("unit_should_abandon_target(unit=11)") == 1 &&
               count_of("unit_issue_default_order(unit=11)") == 1,
           "active_unit_tick: an order-pending unit that SHOULD abandon gets issued the default order");
        ck(count_of("attack_candidate_add(unit=11)") == 0,
           "active_unit_tick: unit 11 is NOT also offered as a candidate -- the two outcomes are "
           "mutually exclusive");
        ck(count_of("unit_should_abandon_target(unit=12)") == 1 &&
               count_of("unit_issue_default_order(unit=12)") == 0 &&
               count_of("attack_candidate_add(unit=12)") == 0,
           "active_unit_tick: an order-pending unit that should NOT abandon gets neither call -- left "
           "alone this tick");
        ck(count_of("unit_is_order_pending(unit=13)") == 0 && count_of("attack_candidate_add(unit=13)") == 0,
           "active_unit_tick: energy<=0 (unit 13) skips the gate body entirely -- no "
           "is_order_pending / candidate_add call for it at all");
        ck(count_of("attack_candidate_add(unit=") == 1,
           "active_unit_tick: across the whole 4-unit chain exactly ONE candidate (unit 10) is "
           "offered -- proves the walk terminated at ai_group_next==0 after unit 13 rather than "
           "looping or reading past the chain's end");
    }
}

// ---- OFFLINE-ORACLE, 2026-08-07: the vacuous shadow site -------------------------------------------
//
// gen_dll_shadow.py reports llm_strat_ai_group_compute_centroid @0x004d5c17 and
// llm_strat_ai_group_task_hold @0x004eab33's shadow site as VACUOUS -- it declares no compared
// region and no compared return value, so the original and the translation agree by construction and
// the call count the site does report is not coverage of anything. These two aitest cases are what
// replace that missing runtime evidence; see ai_group_centroid.h / ai_group_task_lifecycle.h for the
// disassembly-derived behaviour each assertion below cites by address.
void test_ai1c_2026_08_07_vacuous_group_compute_centroid() {
    // ---- A. EMPTY GROUP (member_count == 0): returns the PLAYER's ai_home_tile_x/y, not (0,0) and
    // not left alone. 0x004d5c51 CMP word[..+0xe7e42c],0 / JNZ -- the not-equal arm (0x004d5c5b-69)
    // reads player_data.ai_home_tile_x/y (offsets 0x24/0x28, i.e. +0xe6dee4/+0xe6dee8 off the same
    // per-player base the member_count check used) straight into *out_x/*out_y and returns.
    //
    // MUTATION ARGUMENT: player 0's home tile is seeded to a DIFFERENT pair (999,888) than the
    // called player's (17,231) -- a translation that hardcoded/misindexed player 0 reads 999/888
    // instead. The out-pointers start poisoned (not 0, not the expected pair) so "left the outputs
    // untouched" is caught too, and a "return (0,0)" translation is caught because neither 17 nor
    // 231 is 0. All three plausible wrong answers are separated from the right one by this one call.
    {
        fixture       f;
        const int32_t P = 3, G = 5;
        f.players[0].ai_home_tile_x            = 999; // decoy: a different player, must never be read
        f.players[0].ai_home_tile_y            = 888;
        f.players[P].ai_home_tile_x            = 17;
        f.players[P].ai_home_tile_y            = 231;
        f.players[P].ai_groups[G].member_count = 0;
        uint32_t out_x = 0xdeadbeef, out_y = 0xdeadbeef;
        detail::group_compute_centroid(f.view(), stub_calls(), P, G, &out_x, &out_y);
        ck(out_x == 17 && out_y == 231,
           "group_compute_centroid: an empty group returns the CALLED player's ai_home_tile_x/y "
           "verbatim (0x004d5c5b-69), not (0,0), not left untouched, and not another player's tile");
    }

    // ---- B. Idle members are skipped from the delta SUM but the divisor stays member_count-1
    // (0x004d5cbe JNZ skips the ADD at 0x004d5d58/5b for an idle member but nothing downstream ever
    // decrements member_count; the DIVISOR read at 0x004d5da3 is the SAME field re-read whole).
    //
    // Chain: head(H, non-idle) -> M1(non-idle) -> M2(idle) -> M3(idle) -> 0, member_count = 4.
    // Head's own delta is always (0,0) (its position minus itself), so whether the head itself is
    // idle never shows up in the sum -- only M1..M3 matter here, and M2/M3 are the ones under test.
    // M1 sits at a small, no-wrap delta (dx=+9, dy=+6) so the wrap logic (tested separately below)
    // never engages and the divisor is the only thing this case exercises. M2/M3 are placed far
    // from the head (near the byte-range edges) precisely so that if a translation forgot to skip
    // them, their large deltas would make the result visibly wrong too -- this case is a belt-and-
    // suspenders check on the skip itself as well as the divisor.
    //
    // MUTATION ARGUMENT: sum = 0 + 9 (M1 only, M2/M3 skipped) = 9. The original's divisor is
    // member_count-1 = 3 (UNADJUSTED for the two skipped members) -> avg = 9/3 = 3 -> out_x =
    // head_x(40)+3 = 43 (out_y likewise: sum=6, avg=2, out_y=22). A "fixed" implementation that
    // divides by the COUNT OF CONTRIBUTING units instead gives a different divisor under EITHER
    // natural reading of that phrase: counting the head as a contributor gives divisor=2 (avg_x=4,
    // out_x=44); excluding the head gives divisor=1 (avg_x=9, out_x=49). Both 44 and 49 are
    // distinct from the original's 43, so this input separates the original from either candidate
    // "fix" -- the divisor really is the raw member_count-1, biased toward the head by design.
    {
        fixture        f;
        const int32_t  P = 1, G = 2;
        const uint16_t H = 5, M1 = 6, M2 = 7, M3 = 8;
        f.players[P].ai_groups[G].head_unit    = H;
        f.players[P].ai_groups[G].member_count = 4;
        f.u(P, H).ai_group_next                = M1;
        f.u(P, M1).ai_group_next               = M2;
        f.u(P, M2).ai_group_next               = M3;
        f.u(P, M3).ai_group_next               = 0;
        f.u(P, H).x                            = 40;
        f.u(P, H).y                            = 20;
        f.u(P, M1).x                           = 49;
        f.u(P, M1).y                           = 26; // dx=+9, dy=+6 off the head -- well inside +/-32,+/-24, no wrap
        f.u(P, M2).x                           = 5;
        f.u(P, M2).y                           = 5; // idle -- must be excluded, and its own raw delta would need a wrap
        f.u(P, M3).x                           = 250;
        f.u(P, M3).y                           = 250; // idle -- must be excluded, another large off-head delta
        g_rec.idle_by_unit[M2]                 = 1;
        g_rec.idle_by_unit[M3]                 = 1;
        uint32_t out_x = 0, out_y = 0;
        detail::group_compute_centroid(f.view(), stub_calls(), P, G, &out_x, &out_y);
        ck(out_x == 43 && out_y == 22,
           "group_compute_centroid: idle members (M2, M3) are excluded from the delta sum, but the "
           "divide still uses the RAW member_count-1 (3), not 'how many actually contributed' (which "
           "would give 43/22 a different value under either natural reading -- see comment above)");
    }

    // ---- C. Toroidal wrap: each axis is independently wrapped into +/-(width/2), +/-(height/2) via
    // a two-sided compare-and-adjust (0x004d5cdb-5d0e for X against map_width, 0x004d5d25-5d58 for Y
    // against map_height), NOT a modulo -- then the FINAL sum (head + avg) is masked with
    // width_m/height_m via bitwise AND (0x004d5dcb/dd), a third and separate operation. A
    // single-active-member group (member_count=2, divisor=1) isolates the wrap arithmetic from any
    // integer-division rounding.
    //
    // map defaults: width=64 (half=32), height=48 (half=24), width_m=127, height_m=63 (the
    // fixture's own defaults -- deliberately NOT width-1/height-1, matching the original's two
    // independent globals; see the fixture's map_wm/map_hm member comment).
    //
    // Head at (2,46), member at (60,2) -- chosen so the map's zero-seam sits BETWEEN them on both
    // axes: raw dx=60-2=58 (> half=32 -> wraps to 58-64=-6, i.e. "6 tiles the other way around"),
    // raw dy=2-46=-44 (< -half=-24 -> wraps to -44+48=4).
    //
    // MUTATION ARGUMENT: with the wrap, final = (2+(-6), 46+4) = (-4, 50), AND-masked to
    // (124, 50) (-4 & 127 == 124). A translation that used the RAW unwrapped delta instead gets
    // final = (2+58, 46-44) = (60, 2), masked to (60, 2) -- both coordinates differ from (124, 50),
    // so this input catches a wrap dropped on EITHER axis independently, not just both at once.
    {
        fixture        f;
        const int32_t  P = 2, G = 1;
        const uint16_t H = 10, M = 11;
        f.players[P].ai_groups[G].head_unit    = H;
        f.players[P].ai_groups[G].member_count = 2; // divisor = 1: avg == delta, exactly
        f.u(P, H).ai_group_next                = M;
        f.u(P, M).ai_group_next                = 0;
        f.u(P, H).x                            = 2;
        f.u(P, H).y                            = 46;
        f.u(P, M).x                            = 60;
        f.u(P, M).y                            = 2;
        uint32_t out_x = 0, out_y = 0;
        detail::group_compute_centroid(f.view(), stub_calls(), P, G, &out_x, &out_y);
        ck(out_x == 124 && out_y == 50,
           "group_compute_centroid: a member straddling the map seam wraps EACH axis independently "
           "into +/-(extent/2) before the final head+avg sum is AND-masked by width_m/height_m -- an "
           "unwrapped translation would land on (60,2) instead of (124,50)");
    }

    // ---- D. member_count == 1: the divide (member_count-1, which would be 0) is SKIPPED outright.
    // 0x004d5da1 CMP word[..+0xe7e42c],1 / JBE branches PAST both IDIVs straight to the AND-mask
    // step for member_count <= 1, leaving the raw (undivided) sum in place. For a single-member
    // group the loop runs exactly once, over the head itself, whose delta against itself is always
    // (0,0) regardless of its idle/parked status -- so the raw sum is (0,0) and the result is the
    // head's own position, unmasked-by-division and (here) unaffected by the width_m/height_m AND
    // since it is already in range.
    //
    // NOTE ON SEPARATING POWER: because the numerator is 0 either way, this input cannot distinguish
    // "the divide is skipped" from "the divide runs against max(member_count-1, 1)" -- both give
    // 0/anything == 0. What it DOES rule out is an UNGUARDED divide by the literal (member_count-1)
    // == 0, which is what 0x004d5da1's JBE exists to prevent (an x86 IDIV by zero traps; a C++
    // translation missing the guard divides by zero too). This test's own process crashing here
    // would BE the finding, not a false negative -- see the .h banner comment on trace_on() for why
    // that is an accepted, if loud, failure mode in this file.
    {
        fixture        f;
        const int32_t  P = 4, G = 1;
        const uint16_t H                       = 20;
        f.players[P].ai_groups[G].head_unit    = H;
        f.players[P].ai_groups[G].member_count = 1;
        f.u(P, H).ai_group_next                = 0;
        f.u(P, H).x                            = 88;
        f.u(P, H).y                            = 33;
        uint32_t out_x = 0xdeadbeef, out_y = 0xdeadbeef;
        detail::group_compute_centroid(f.view(), stub_calls(), P, G, &out_x, &out_y);
        ck(out_x == 88 && out_y == 33,
           "group_compute_centroid: a single-member group returns the head's own position verbatim "
           "-- the member_count<=1 guard at 0x004d5da1 skips the divide entirely rather than faulting "
           "on member_count-1 == 0");
    }
}

// llm_strat_ai_group_task_hold @0x004eab33 (11 bytes). The WHOLE body is `PUSH 4 / CALL
// utils_assert_stack_capacity / RET` -- there is no other instruction, so there is nothing for a
// translation to have gotten subtly wrong. This is a CONTRACT test in the same spirit as batch B's
// llm_strat_ai_bldg_queue_handle_state2_empty (see that test's own comment): it asserts the
// observable state is byte-identical across the call and that no outward call fires, which is a
// guarantee the function's SIGNATURE does not already make by itself (unlike state2_empty, this
// function DOES take the view/store/calls triple, so nothing here structurally prevents it from
// writing or calling something). The check exists so that "does nothing" stays load-bearing rather
// than incidental -- it can only ever catch a translation that STARTS doing something.
void test_ai1c_2026_08_07_vacuous_group_task_hold() {
    fixture f;
    g_rec.clear();
    // Stamp the WHOLE player_data table (every slot, not just one) with a recognizable fingerprint,
    // so a write anywhere in it -- not only at some player/group this test happened to pick -- shows
    // up. group_task_hold takes no player_id/group_index argument at all (see the .h: "NO
    // PARAMETERS"), so there is no single slot a correct call could legitimately touch either.
    std::memset(f.players.data(), 0x5a, f.players.size() * sizeof(player_data));
    std::vector<uint8_t> before(f.players.size() * sizeof(player_data));
    std::memcpy(before.data(), f.players.data(), before.size());

    detail::group_task_hold(f.view(), f.store(), stub_calls());

    ck(std::memcmp(before.data(), f.players.data(), before.size()) == 0,
       "group_task_hold: the whole player_data table is byte-identical across the call");
    ck(g_rec.task_pushes.empty() && g_rec.dequeues.empty() && g_rec.member_moves.empty() &&
           g_rec.slice2_log.empty(),
       "group_task_hold: issues no outward call -- a recording stub set that would notice any of its "
       "task-lifecycle siblings' typical calls (enqueue/preempt, dequeue, member_move) or any "
       "batch-E-style logged call fired instead");
}

int run_aitest() {
    printf("=== aitest (AI0: the strategic AI's logic over heap buffers) ===\n");
    test_calls_complete();
    test_scan_target_list();
    test_filter_and_swap_remove();
    test_pick_survivable();
    test_consumer_differences();
    test_build_candidates();
    test_view_binding();
    test_scan_targets_for_engage();
    test_target_list_add();
    test_turret_threat_rescan();
    test_engage_partition();
    test_scan_in_range();
    test_sort_by_dist();
    test_sort_call_site_flag_composes();
    test_has_engageable_weapon();
    test_target_dist_sq();
    test_target_ref_predicates();
    test_worker_priority();
    test_site_sort_edges();
    test_grid_stencil();
    test_queue_release();
    test_attacker_intel();
    test_map_influence();
    test_queue_reconcile();
    test_bldg_weapon_range();
    test_train_flush();
    test_spend_rate();
    test_train_plan();
    test_bldg_queue();
    test_build_sources();
    test_queue_enqueue();
    test_bldg_queue_dispatch();
    test_score_build_categories();
    test_react_resource_shortage();
    test_maintain_unit_housing();
    test_resource_shortage_gate();
    test_plan_mine_construction();
    test_plan_turret_upgrade();
    test_rebalance_building_workers();
    test_queue_type_query();
    test_queue_rotate();
    test_nearest_flagged();
    test_site_dispatch();
    test_launch_storage();
    // batch B layer 6
    test_calc_mine_yield();
    test_mine_portfolio_rebalance();
    test_site_worth();
    // batch C layer 0
    test_notify_object_removed();
    // batch C layer 1
    test_target_list_remove();
    test_unit_group_tick();
    // batch C layer 2
    test_group_form();
    test_group_hold();
    test_group_task_machine();
    test_invasion_launch();
    test_group_redistribute();
    // batch C layer 3
    test_group_task_formation();
    test_squad_firepower();
    // batch B layer 3 (2026-08-06)
    test_player_score_tier();
    test_unit_weapon_power();
    test_calc_power_supply_ratio();
    // RI-AI batch B/C, 2026-08-07
    test_ai1b_2026_08_07_slice();
    test_ai1e_2026_08_07_hq_attack_scenario();
    test_ai1e_2026_08_07_scr_parse();
    test_ai1e_2026_08_07_spiral_table_init();
    // RI-AI batch E, 2026-08-07
    test_ai1e_2026_08_07_slice2_score_reinforcement_unit();
    test_ai1e_2026_08_07_slice2_create_reinforcement_unit();
    test_ai1e_2026_08_07_slice2_unit_commit_attack_on_enemy_hq();
    test_ai1e_2026_08_07_slice2_invasion_spawn_reinforcements();
    test_ai1e_2026_08_07_slice2_player_tick();
    test_ai1e_2026_08_07_slice2_active_unit_tick();
    test_ai1c_2026_08_07_vacuous_group_compute_centroid();
    test_ai1c_2026_08_07_vacuous_group_task_hold();
    // the per-TU oracle files (RI-AI batch D / AI1D)
    run_group_member_list_tests();
    run_attack_commit_tests();
    run_group_remove_tests();
    run_players_tick_tests();
    run_notify_unit_lifecycle_tests();
    // the compensating oracles for batch C's zero-call T3 rows (AI1 close)
    run_queue_remove_tests();
    run_group_building_scan_tests();
    run_group_no_member_near_centroid_tests();
    run_group_task_workers_tests();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
