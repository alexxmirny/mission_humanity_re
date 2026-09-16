//
// sim_bldg_completion_dispatch_selftest.cpp -- `simtest` offline oracle for
// llm_strat_bldg_completion_dispatch (sim/sim_bldg_completion_dispatch.{h,cpp}, RI-SIM / SIM1-G4,
// the batch's largest function, 5288 bytes). A dispatcher over building `state` (six arms:
// CONSTRUCTION/CHARGE_GATE/PROD_WORKING/MINE_EXTRACTING/UPGRADING/RESEARCHING) plus a shared
// "invasion escalation accumulator" prologue+footer that runs regardless of which arm fired.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_completion_dispatch_004795dd.asm), not the .c draft -- per this
// project's own rule the .c has silently lied before. The ONE exception (per the .h's own banner)
// is the building-TYPE jump table's case targets, which the raw .asm export cannot show byte-for-
// byte; that piece is trusted from the .h's own independently-cross-checked account, not re-derived
// here.
//
// This closure's shadow site will NOT be armed (the callee closure reaches the notify_ui/
// game_SetEvent UI-event fan-out and AI order-staging globals) -- this offline oracle is the
// evidence path instead, so it is written to be as adversarial as a rig comparison would be.
//
// THREE REGRESSION LOCKS for the bugs a prior reimpl-verify pass found and fixed (see the .h's
// banner) are marked "REGRESSION LOCK" below -- each is phrased so reverting its fix fails the case:
//   1. MINE_EXTRACTING: a deposit value with bit 15 set must zero-extend (large positive), not
//      sign-extend (negative, which used to silently skip resource_add/depletion).
//   2. PROD_WORKING: the local-player accum+=1 bump must fire on BOTH the spawned==0 and spawned!=0
//      sub-paths (it used to be scoped inside the spawned==0 branch only).
//   3. The footer's SBB-based accum/4 quartering must add 3, not 4, for a negative accumulator.
//
#include "sim/sim_bldg_completion_dispatch.h"

#include "sim_test_support.h"

#include <cstring>

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- llm_strat_bldg_state values this function dispatches on (0x00479677) -- file-local copies,
// same values sim_bldg_completion_dispatch.cpp's own anonymous namespace uses (no committed enum
// exists yet).
constexpr uint16_t ST_CONSTRUCTION    = 0x64;
constexpr uint16_t ST_CHARGE_GATE     = 0x69;
constexpr uint16_t ST_PROD_WORKING    = 0x6d;
constexpr uint16_t ST_MINE_EXTRACTING = 0x74;
constexpr uint16_t ST_UPGRADING       = 0x82;
constexpr uint16_t ST_RESEARCHING     = 0x89;
constexpr uint16_t ST_UNMATCHED       = 0x50; // not one of the six -- exercises the outer default/no-op

// ---- cfg_enum_E_BUILDING members the CONSTRUCTION arm's type-switch compares (0x004798a6) --
// same values sim_bldg_completion_dispatch.cpp's own anonymous namespace uses.
constexpr uint8_t TY_A_MINE    = 2;
constexpr uint8_t TY_A_MOTHER  = 6;
constexpr uint8_t TY_H_MOTHER  = 0x1a;
constexpr uint8_t TY_A_PORT    = 0x0c;
constexpr uint8_t TY_H_PORT    = 0x20;
constexpr uint8_t TY_UNMATCHED = 0x63; // not MINE/MOTHER/PORT -- exercises the type-switch's own default

constexpr uint32_t STATUS_AI_CONTROLLED = 0x8u; // player_profile::status_flags bit3
constexpr uint32_t RACE_ALIEN           = 2u;
constexpr int32_t  SESSION_MODE_SP      = 1;
constexpr int32_t  SESSION_MODE_MP      = 3;

// param_3/param_4 are CONFIRMED DEAD (sim_bldg_completion_dispatch.h's "PARAM_3/PARAM_4 ARE DEAD"
// section, and the .cpp's own `uint32_t /*param_3*/, uint32_t /*param_4*/` signature) -- passed as
// non-zero, non-matching GARBAGE on every call below (not 0) so that if this were an oversight
// rather than a confirmed fact, some downstream effect would visibly depend on it and a case would
// fail. None do, by design.
constexpr uint32_t DEAD_PARAM_3 = 0xdeadbeefu;
constexpr uint32_t DEAD_PARAM_4 = 0xfeedfaceu;

// ---- recorders for every one of the 24 callees in bldg_completion_dispatch_calls, in the header's
// own declared order -----------------------------------------------------------------------------

struct MineScanCall {
    uint8_t  slot_index;
    uint32_t player;
    int32_t  bidx;
};
std::vector<MineScanCall> g_mine_scan;
bool                      g_mutate_slot_count_on_mine_scan = false;
sim_fixture              *g_fx                             = nullptr;

struct PopAddCall {
    uint16_t player;
    int32_t  count;
};
std::vector<PopAddCall> g_population_add;

struct ResourceAddCall {
    int32_t player, resource_index, amount;
};
std::vector<ResourceAddCall> g_resource_add;

struct LoadDmpCall {
    int32_t     player;
    const char *path;
};
std::vector<LoadDmpCall> g_load_dmp;

struct SprintfVssiiCall {
    const void *dst;
    const char *format, *a0, *a1;
    int32_t     a2, a3;
};
std::vector<SprintfVssiiCall> g_sprintf_vssii;

struct UsesWorkersCall {
    uint32_t player;
    int32_t  bidx;
};
std::vector<UsesWorkersCall> g_uses_workers_calls;
std::vector<int32_t>         g_uses_workers_queue; // per-call returns, in order
size_t                       g_uses_workers_qi  = 0;
int32_t                      g_uses_workers_ret = 0; // used once the queue is exhausted

struct UnassignCall {
    uint16_t player;
    uint32_t bidx, count;
};
std::vector<UnassignCall> g_unassign;

struct AssignCall {
    uint32_t player, bidx;
    int32_t  count;
};
std::vector<AssignCall> g_assign;

struct SetStaffCall {
    uint16_t player;
    int32_t  bidx;
};
std::vector<SetStaffCall> g_set_staffed;

struct ClearStaffCall {
    uint16_t player;
    uint32_t bidx;
};
std::vector<ClearStaffCall> g_clear_staffed;

struct UpdateProgressCall {
    uint16_t plr, inv;
};
std::vector<UpdateProgressCall> g_update_progress;

struct RegisterOnlineCall {
    int16_t  player;
    int32_t  bidx;
    uint32_t p3, p4;
    double   anim_dur;
};
std::vector<RegisterOnlineCall> g_register_online;

struct LinkNetworkCall {
    uint16_t player;
    uint32_t index;
};
std::vector<LinkNetworkCall> g_link_network;

int32_t g_prod_deliver_count = 0;

struct SpawnDockedCall {
    uint16_t unit_proto_id, player;
    uint32_t probe_slot;
};
std::vector<SpawnDockedCall> g_spawn_docked;
int32_t                      g_spawn_docked_ret = 0;

struct AddDockedCall {
    uint32_t unit_proto_id;
    uint16_t player;
    uint32_t probe_slot;
};
std::vector<AddDockedCall> g_add_docked;
int32_t                    g_add_docked_ret = 0;

std::vector<int32_t> g_group_index_calls;
int32_t              g_group_index_ret = 0;

struct HousingCall {
    uint32_t p1;
    int32_t  p2;
};
std::vector<HousingCall> g_housing_calls;
int32_t                  g_housing_ret = 0;

struct WSprintfVssCall {
    const void    *dst;
    const wchar_t *format, *a0, *a1;
};
std::vector<WSprintfVssCall> g_wsprintf_vss;

struct WSprintfVsssCall {
    const void    *dst;
    const wchar_t *format, *a0, *a1, *a2;
};
std::vector<WSprintfVsssCall> g_wsprintf_vsss;

int32_t g_print_text_count = 0;

struct ApplyProdCompletionCall {
    uint32_t player;
    int32_t  unit_proto_id;
};
std::vector<ApplyProdCompletionCall> g_apply_prod_completion;

struct NotifyLifecycleCall {
    uint16_t player, unit_type;
    uint32_t unit_id, param4;
};
std::vector<NotifyLifecycleCall> g_notify_lifecycle;

std::vector<int32_t> g_invasion_roll_calls;
int32_t              g_invasion_roll_ret = 0;

// ---- the 24 stubs, in the header's own declared order --------------------------------------------

uint32_t stub_mine_scan_deposit_slot(uint8_t slot_index, uint32_t player, int32_t bidx) {
    g_mine_scan.push_back({slot_index, player, bidx});
    if (g_mutate_slot_count_on_mine_scan && g_fx) g_fx->bldg_completion_slot_count = 10;
    return 0;
}
void stub_population_add(uint16_t player, int32_t count) { g_population_add.push_back({player, count}); }
void stub_resource_add(int32_t player, int32_t resource_index, int32_t amount) {
    g_resource_add.push_back({player, resource_index, amount});
}
void    stub_load_base_layout_dmp(int32_t player, char *dmp_path) { g_load_dmp.push_back({player, dmp_path}); }
int32_t stub_utils_sprintf__vssii(void *dst, const char *format, const char *a0, const char *a1, int32_t a2,
                                  int32_t a3) {
    g_sprintf_vssii.push_back({dst, format, a0, a1, a2, a3});
    return 0;
}
int32_t stub_bldg_uses_workers(uint32_t player, int32_t bidx) {
    g_uses_workers_calls.push_back({player, bidx});
    if (g_uses_workers_qi < g_uses_workers_queue.size()) return g_uses_workers_queue[g_uses_workers_qi++];
    return g_uses_workers_ret;
}
int32_t stub_bldg_unassign_workers(uint16_t player, uint32_t bidx, uint32_t count) {
    g_unassign.push_back({player, bidx, count});
    return 0;
}
int32_t stub_bldg_assign_workers(uint32_t player, uint32_t bidx, int32_t count) {
    g_assign.push_back({player, bidx, count});
    return 0;
}
void stub_bldg_set_staffed_flag(uint16_t player, int32_t bidx) { g_set_staffed.push_back({player, bidx}); }
void stub_bldg_clear_staffed_flag(uint16_t player, uint32_t bidx) { g_clear_staffed.push_back({player, bidx}); }
void stub_game_UpdateProgress(uint16_t plr, uint16_t inv) { g_update_progress.push_back({plr, inv}); }
void stub_bldg_register_online(int16_t player, int32_t bidx, uint32_t p3, uint32_t p4, double anim_dur) {
    g_register_online.push_back({player, bidx, p3, p4, anim_dur});
}
void stub_bldg_link_to_network_if_adjacent(uint16_t player, uint32_t index) {
    g_link_network.push_back({player, index});
}
void    stub_prod_deliver_arrivals() { ++g_prod_deliver_count; }
int32_t stub_unit_spawn_docked(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot) {
    g_spawn_docked.push_back({unit_proto_id, player, probe_slot});
    return g_spawn_docked_ret;
}
int32_t stub_unit_add_docked(uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot) {
    g_add_docked.push_back({unit_proto_id, player, probe_slot});
    return g_add_docked_ret;
}
int32_t stub_unit_type_group_index(int32_t unit_id) {
    g_group_index_calls.push_back(unit_id);
    return g_group_index_ret;
}
int32_t stub_reason_to_housing_bldg(uint32_t p1, int32_t p2) {
    g_housing_calls.push_back({p1, p2});
    return g_housing_ret;
}
int32_t stub_w_sprintf__vss(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1) {
    g_wsprintf_vss.push_back({dst, format, a0, a1});
    return 0;
}
int32_t stub_w_sprintf__vsss(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1,
                             const wchar_t *a2) {
    g_wsprintf_vsss.push_back({dst, format, a0, a1, a2});
    return 0;
}
uint32_t stub_game_ui_PrintTextMessage(void *) {
    ++g_print_text_count;
    return 0;
}
void stub_unit_apply_production_completion(uint32_t player, int32_t unit_proto_id) {
    g_apply_prod_completion.push_back({player, unit_proto_id});
}
void stub_ai_notify_unit_lifecycle(uint16_t player_, uint16_t unit_type, uint32_t unit_id, uint32_t param4) {
    g_notify_lifecycle.push_back({player_, unit_type, unit_id, param4});
}
int32_t stub_invasion_chance_roll(int32_t building_completed) {
    g_invasion_roll_calls.push_back(building_completed);
    return g_invasion_roll_ret;
}

const bldg_completion_dispatch_calls g_calls = {
    stub_mine_scan_deposit_slot,
    stub_population_add,
    stub_resource_add,
    stub_load_base_layout_dmp,
    stub_utils_sprintf__vssii,
    stub_bldg_uses_workers,
    stub_bldg_unassign_workers,
    stub_bldg_assign_workers,
    stub_bldg_set_staffed_flag,
    stub_bldg_clear_staffed_flag,
    stub_game_UpdateProgress,
    stub_bldg_register_online,
    stub_bldg_link_to_network_if_adjacent,
    stub_prod_deliver_arrivals,
    stub_unit_spawn_docked,
    stub_unit_add_docked,
    stub_unit_type_group_index,
    stub_reason_to_housing_bldg,
    stub_w_sprintf__vss,
    stub_w_sprintf__vsss,
    stub_game_ui_PrintTextMessage,
    stub_unit_apply_production_completion,
    stub_ai_notify_unit_lifecycle,
    stub_invasion_chance_roll,
};

// Clears only the CALL RECORDERS (what was called, with what args) -- never the CONTROL variables
// (g_*_ret / g_uses_workers_queue / g_mutate_slot_count_on_mine_scan). Those are set by each case
// BEFORE calling run() to script a stub's return value for THIS call, and run() calls this first
// thing; if it cleared the controls too it would wipe out the very value the case just set,
// before the dispatch under test ever ran. Each case is responsible for (re)setting the controls
// it cares about -- most default to 0/empty from the previous case's cleanup below, which is
// harmless for arms that don't inspect worker_mgmt's/the footer's outcome (see the comment on
// run() for why that is safe here).
void reset_calls() {
    g_mine_scan.clear();
    g_population_add.clear();
    g_resource_add.clear();
    g_load_dmp.clear();
    g_sprintf_vssii.clear();
    g_uses_workers_calls.clear();
    g_unassign.clear();
    g_assign.clear();
    g_set_staffed.clear();
    g_clear_staffed.clear();
    g_update_progress.clear();
    g_register_online.clear();
    g_link_network.clear();
    g_prod_deliver_count = 0;
    g_spawn_docked.clear();
    g_add_docked.clear();
    g_group_index_calls.clear();
    g_housing_calls.clear();
    g_wsprintf_vss.clear();
    g_wsprintf_vsss.clear();
    g_print_text_count = 0;
    g_apply_prod_completion.clear();
    g_notify_lifecycle.clear();
    g_invasion_roll_calls.clear();
}

// Resets EVERYTHING, including the controls -- call this (not reset_calls()) at the top of a case
// that relies on a control defaulting to 0/empty (e.g. every case that does NOT itself set
// g_uses_workers_ret still gets a deterministic uses==0 from a fresh case, rather than whatever the
// previous case left behind).
void reset_all() {
    reset_calls();
    g_mutate_slot_count_on_mine_scan = false;
    g_uses_workers_queue.clear();
    g_uses_workers_qi   = 0;
    g_uses_workers_ret  = 0;
    g_spawn_docked_ret  = 0;
    g_add_docked_ret    = 0;
    g_group_index_ret   = 0;
    g_housing_ret       = 0;
    g_invasion_roll_ret = 0;
}

// Calls detail::bldg_completion_dispatch directly (the offline detail overload, not the live
// wrapper -- same pattern every other sim selftest uses). param_3/param_4 always carry
// DEAD_PARAM_3/4 garbage -- see that constant's own comment.
void run(sim_fixture &fx, uint32_t player, int32_t bidx, double param_5 = 0.0) {
    reset_calls();
    g_fx          = &fx;
    sim_store own = fx.store();
    detail::bldg_completion_dispatch(fx.view(), own, g_calls, player, static_cast<uint32_t>(bidx), DEAD_PARAM_3,
                                     DEAD_PARAM_4, param_5);
}

} // namespace

void run_bldg_completion_dispatch_tests() {
    sim_fixture fx;

    // ==================================================================================================
    // ESCALATION BONUS prologue (0x004795fa-0x00479663): local-player-only, adds trunc((clock-lost)/
    // interval)*5 to escalation_bonus and sets did_something -- gated on planet_mother_lost_time>0.0.
    // ==================================================================================================

    // ---- local player, lost_time>0: bonus computed and did_something forced true even with an
    // UNMATCHED state (the outer switch's own default has nothing to contribute) -----------------------
    fx.reset();
    reset_all();
    fx.player_side                     = 2;
    fx.planet_index                    = 3;
    fx.mother_lost_escalation_interval = 60.0;
    fx.planet_mother_lost_time[3]      = 10.0;
    fx.game_clock                      = 70.0; // (70-10)/60 = 1.0 -> trunc 1 -> bonus 5
    fx.bldg_completion_accum           = 0;
    fx.tutorial_step                   = 1; // block the footer so we can inspect accum in isolation
    fx.b(2, 5).state                   = ST_UNMATCHED;
    run(fx, 2, 5);
    // did_something alone can't be read directly; observe it through the footer's magnitude gate
    // instead in the FOOTER section below. Here just confirm the arm truly did nothing on its own.
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "PROLOGUE @0x00479663: unmatched state, no arm bump");

    // ---- other player: no bonus regardless of lost_time -----------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                = 2;
    fx.planet_index               = 3;
    fx.planet_mother_lost_time[3] = 10.0;
    fx.game_clock                 = 70.0;
    fx.b(5, 5).state              = ST_UNMATCHED; // player 5 != PlayerSide(2)
    run(fx, 5, 5);
    ck_eq((uint32_t)fx.planet_mother_lost_time[3], 10u, "PROLOGUE @0x00479612: other player, mother_lost_time untouched");

    // ---- lost_time<=0.0: no bonus even for the local player --------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                = 4;
    fx.planet_index               = 1;
    fx.planet_mother_lost_time[1] = 0.0;
    fx.b(4, 0).state              = ST_UNMATCHED;
    run(fx, 4, 0);
    ck_eq((uint32_t)fx.planet_mother_lost_time[1], 0u, "PROLOGUE @0x0047961b: lost_time<=0, no bonus computed");

    // ==================================================================================================
    // BLDG_STATE_CONSTRUCTION (0x64) [0x004796eb-0x00479dd8]
    // ==================================================================================================

    // ---- invention-acquired accum+=10, local player only (0x00479716-0x0047972b) ---------------------
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    fx.planet_index                                   = 2;
    fx.cfg_planets[2].invention_index                 = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired = 1;
    fx.bldg_completion_accum                          = 0;
    fx.tutorial_step                                  = 1; // keep the footer from further mutating accum
    building &bc1                                     = fx.b(1, 7);
    bc1.state                                         = ST_CONSTRUCTION;
    bc1.building_id                                   = 30;
    fx.cfg_buildings[30].type                         = TY_UNMATCHED; // isolate: no type-switch side effects
    fx.cfg_buildings[30].state_transition_ids[1]      = 0;
    fx.cfg_buildings[30].invention                    = 90;
    fx.progress[1 * PROGRESS_ROW_COUNT + 90].acquired = 1; // suppress the tail's game_UpdateProgress too
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 10u, "CONSTRUCTION@0x00479724: accum+=10 (local+acquired)");

    // acquired==0 -> no bump
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    fx.planet_index                                   = 2;
    fx.cfg_planets[2].invention_index                 = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired = 0;
    building &bc2                                     = fx.b(1, 7);
    bc2.state                                         = ST_CONSTRUCTION;
    bc2.building_id                                   = 30;
    fx.cfg_buildings[30].type                         = TY_UNMATCHED;
    fx.cfg_buildings[30].invention                    = 90;
    fx.progress[1 * PROGRESS_ROW_COUNT + 90].acquired = 1;
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "CONSTRUCTION@0x00479716: not acquired -> no bump");

    // other player -> no bump even if acquired
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    fx.planet_index                                   = 2;
    fx.cfg_planets[2].invention_index                 = 50;
    fx.progress[5 * PROGRESS_ROW_COUNT + 50].acquired = 1;
    building &bc3                                     = fx.b(5, 7);
    bc3.state                                         = ST_CONSTRUCTION;
    bc3.building_id                                   = 30;
    fx.cfg_buildings[30].type                         = TY_UNMATCHED;
    fx.cfg_buildings[30].invention                    = 90;
    fx.progress[5 * PROGRESS_ROW_COUNT + 90].acquired = 1;
    run(fx, 5, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "CONSTRUCTION@0x004796ee: other player -> no bump");

    // ---- state advance: b.state = LOW WORD of Building[building_id].state_transition_ids[1]
    // (0x004797d2-0x0047980c) --------------------------------------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                               = 1;
    fx.progress[1 * PROGRESS_ROW_COUNT].acquired = 1; // suppress accum/update_progress noise
    building &bc4                                = fx.b(1, 7);
    bc4.state                                    = ST_CONSTRUCTION;
    bc4.building_id                              = 30;
    fx.cfg_buildings[30].type                    = TY_UNMATCHED;
    fx.cfg_buildings[30].state_transition_ids[1] = 0x00125678; // low word 0x5678
    fx.cfg_buildings[30].invention               = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.b(1, 7).state, 0x5678u, "CONSTRUCTION@0x0047980c: state = state_transition_ids[1] & 0xffff");

    // ---- type-switch A_MINE: 4x mine_scan_deposit_slot(i, player, bidx), i=0..3 (0x004798ad-0x004798d6)
    fx.reset();
    reset_all();
    fx.player_side            = 1;
    building &bc5             = fx.b(1, 7);
    bc5.state                 = ST_CONSTRUCTION;
    bc5.building_id           = 30;
    fx.cfg_buildings[30].type = TY_A_MINE;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_mine_scan.size(), 4u, "CONSTRUCTION type A_MINE@switchD 0x004798a6: 4 mine_scan calls");
    for (int32_t i = 0; i < 4; ++i) {
        ck_eq((uint32_t)g_mine_scan[(size_t)i].slot_index, (uint32_t)i, "CONSTRUCTION A_MINE: slot index in order");
        ck_eq((uint32_t)g_mine_scan[(size_t)i].bidx, 7u, "CONSTRUCTION A_MINE: bidx passed through");
    }
    ck((uint32_t)g_population_add.size() == 0 && g_resource_add.size() == 0,
       "CONSTRUCTION A_MINE: no MOTHER-arm calls fire");

    // ---- type-switch default (unmatched type): no mine_scan, no MOTHER calls (0x00479ddd fallthrough
    // to the shared join point) --------------------------------------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side            = 1;
    building &bc6             = fx.b(1, 7);
    bc6.state                 = ST_CONSTRUCTION;
    bc6.building_id           = 30;
    fx.cfg_buildings[30].type = TY_UNMATCHED;
    run(fx, 1, 7);
    ck((uint32_t)(g_mine_scan.size() + g_population_add.size() + g_resource_add.size()) == 0,
       "CONSTRUCTION type default: type-switch is a true no-op");

    // ---- type-switch A_MOTHER, mother_established==0, AI_CONTROLLED set: DMP sprintf+load, established
    // set to 1, population_add (human_transport!=0), resource_add capacity[1..9] NOT [0]
    // (0x0047994b-0x00479a3c) -----------------------------------------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                       = 1;
    fx.planet_index                      = 4;
    building &bm1                        = fx.b(1, 7);
    bm1.state                            = ST_CONSTRUCTION;
    bm1.building_id                      = 30;
    fx.cfg_buildings[30].type            = TY_A_MOTHER;
    fx.cfg_buildings[30].human_transport = 6;
    fx.cfg_buildings[30].capacity[0]     = 999; // sentinel: must NOT be read/added
    for (int32_t i = 1; i < 10; ++i) fx.cfg_buildings[30].capacity[i] = 100 + i;
    fx.profiles[1].mother_established     = 0;
    fx.profiles[1].status_flags           = STATUS_AI_CONTROLLED;
    fx.profiles[1].race                   = RACE_ALIEN;
    fx.profiles[1].landing_spot_index[4]  = 11;
    fx.profiles[1].primary_mother_bldg[4] = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.profiles[1].mother_established, 1u, "MOTHER@0x00479943: mother_established set to 1");
    ck_eq((uint32_t)g_sprintf_vssii.size(), 1u, "MOTHER@0x00479969: DMP-path sprintf fired (AI_CONTROLLED)");
    if (!g_sprintf_vssii.empty()) {
        ck(strcmp(g_sprintf_vssii[0].format, "%s%s_%02d%02d.DMP") == 0, "MOTHER: sprintf format literal");
        ck(strcmp(g_sprintf_vssii[0].a0, "init\\") == 0, "MOTHER: sprintf init\\ literal");
        ck(strcmp(g_sprintf_vssii[0].a1, "A") == 0, "MOTHER@0x00479963: race==ALIEN -> \"A\" (not \"H\")");
        ck_eq((uint32_t)g_sprintf_vssii[0].a2, 4u, "MOTHER: sprintf planet arg");
        ck_eq((uint32_t)g_sprintf_vssii[0].a3, 11u, "MOTHER: sprintf landing_spot_index[planet] arg");
    }
    ck_eq((uint32_t)g_load_dmp.size(), 1u, "MOTHER@0x0047997a: load_base_layout_dmp fired");
    ck(!g_load_dmp.empty() && !g_sprintf_vssii.empty() && g_load_dmp[0].path == g_sprintf_vssii[0].dst,
       "MOTHER: load_base_layout_dmp path == sprintf dst (same dmp_path_scratch buffer)");
    ck_eq((uint32_t)g_population_add.size(), 1u, "MOTHER@0x004799e6: population_add fired (human_transport!=0)");
    if (!g_population_add.empty())
        ck_eq((uint32_t)g_population_add[0].count, 6u, "MOTHER: population_add count == human_transport");
    ck_eq((uint32_t)g_resource_add.size(), 9u, "MOTHER@0x00479a37: 9 resource_add calls, ids 1..9 not 0");
    for (size_t k = 0; k < g_resource_add.size(); ++k) {
        ck_eq((uint32_t)g_resource_add[k].resource_index, (uint32_t)(k + 1), "MOTHER: resource_add id order 1..9");
        ck_eq((uint32_t)g_resource_add[k].amount, (uint32_t)(100 + (k + 1)), "MOTHER: resource_add amount == capacity[id]");
    }
    ck_eq((uint32_t)fx.profiles[1].primary_mother_bldg[4], 7u, "MOTHER@0x00479a74: primary_mother_bldg claimed = bidx");

    // ---- type-switch A_MOTHER, AI_CONTROLLED bit CLEAR: population_add/resource_add still fire
    // (they are OUTSIDE the AI-controlled gate), but sprintf/load_dmp do NOT ---------------------------
    fx.reset();
    reset_all();
    fx.player_side                       = 1;
    fx.planet_index                      = 4;
    building &bm2                        = fx.b(1, 7);
    bm2.state                            = ST_CONSTRUCTION;
    bm2.building_id                      = 30;
    fx.cfg_buildings[30].type            = TY_A_MOTHER;
    fx.cfg_buildings[30].human_transport = 6;
    for (int32_t i = 1; i < 10; ++i) fx.cfg_buildings[30].capacity[i] = 1;
    fx.profiles[1].mother_established = 0;
    fx.profiles[1].status_flags       = 0; // NOT AI-controlled
    run(fx, 1, 7);
    ck_eq((uint32_t)g_sprintf_vssii.size(), 0u, "MOTHER@0x00479908: status bit3 clear -> no DMP sprintf");
    ck_eq((uint32_t)g_load_dmp.size(), 0u, "MOTHER: status bit3 clear -> no load_base_layout_dmp");
    ck_eq((uint32_t)g_population_add.size(), 1u, "MOTHER: population_add STILL fires (outside the AI gate)");
    ck_eq((uint32_t)g_resource_add.size(), 9u, "MOTHER: resource_add STILL fires (outside the AI gate)");

    // ---- type-switch A_MOTHER, mother_established==1 AND is_local_player: block skipped entirely -----
    fx.reset();
    reset_all();
    fx.player_side                       = 1;
    fx.planet_index                      = 4;
    building &bm3                        = fx.b(1, 7);
    bm3.state                            = ST_CONSTRUCTION;
    bm3.building_id                      = 30;
    fx.cfg_buildings[30].type            = TY_A_MOTHER;
    fx.cfg_buildings[30].human_transport = 6;
    fx.profiles[1].mother_established    = 1;
    fx.profiles[1].status_flags          = STATUS_AI_CONTROLLED;
    run(fx, 1, 7);
    ck((uint32_t)(g_sprintf_vssii.size() + g_load_dmp.size() + g_population_add.size() + g_resource_add.size()) == 0,
       "MOTHER@0x00479716: established==1 && local -> whole block skipped");

    // ---- type-switch A_MOTHER, mother_established==1 but OTHER player: block runs anyway (OR gate) ---
    fx.reset();
    reset_all();
    fx.player_side                       = 1; // PlayerSide = 1
    fx.planet_index                      = 4;
    building &bm4                        = fx.b(5, 7); // player 5 != PlayerSide
    bm4.state                            = ST_CONSTRUCTION;
    bm4.building_id                      = 30;
    fx.cfg_buildings[30].type            = TY_A_MOTHER;
    fx.cfg_buildings[30].human_transport = 0; // 0 -> population_add gated off separately
    for (int32_t i = 1; i < 10; ++i) fx.cfg_buildings[30].capacity[i] = 2;
    fx.profiles[5].mother_established = 1; // already established...
    fx.profiles[5].status_flags       = 0;
    run(fx, 5, 7);
    ck_eq((uint32_t)g_resource_add.size(), 9u,
          "MOTHER@0x00479716: established==1 but !local -> block STILL runs (OR, not AND)");
    ck_eq((uint32_t)g_population_add.size(), 0u, "MOTHER: human_transport==0 -> no population_add");
    ck_eq((uint32_t)fx.b(5, 7).x, 0u, "MOTHER: other-player arrival never stamps the UI marker (checked below too)");

    // ---- type-switch A_MOTHER, primary_mother_bldg already claimed: NOT overwritten -------------------
    fx.reset();
    reset_all();
    fx.player_side                        = 1;
    fx.planet_index                       = 4;
    building &bm5                         = fx.b(1, 7);
    bm5.state                             = ST_CONSTRUCTION;
    bm5.building_id                       = 30;
    fx.cfg_buildings[30].type             = TY_A_MOTHER;
    fx.profiles[1].mother_established     = 1;
    fx.profiles[1].primary_mother_bldg[4] = 42;
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.profiles[1].primary_mother_bldg[4], 42u, "MOTHER@0x00479a5a: already-claimed mother NOT overwritten");

    // ---- type-switch A_MOTHER, is_local_player: UI base-marker stamped from x/y + half width/height,
    // torus-wrapped (0x00479a8a-0x00479b08). geom.width_mask=0xff, height_mask=0x3f (fixture reset()). -
    fx.reset();
    reset_all();
    fx.player_side                    = 1;
    fx.planet_index                   = 4;
    building &bm6                     = fx.b(1, 7);
    bm6.state                         = ST_CONSTRUCTION;
    bm6.building_id                   = 30;
    bm6.x                             = 10;
    bm6.y                             = 20;
    fx.cfg_buildings[30].type         = TY_A_MOTHER;
    fx.cfg_buildings[30].width        = 4; // half = 2
    fx.cfg_buildings[30].height       = 6; // half = 3
    fx.profiles[1].mother_established = 1; // skip the DMP block, isolate the marker stamp
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.ui_base_marker_coords[0].cam_col, (uint32_t)((10 + 2) & 0xff),
          "MOTHER@0x00479ac6: cam_col = (x + width/2) & width_mask");
    ck_eq((uint32_t)fx.ui_base_marker_coords[0].cam_row, (uint32_t)((20 + 3) & 0x3f),
          "MOTHER@0x00479b08: cam_row = (y + height/2) & height_mask");

    // ---- type-switch A_MOTHER, OTHER player: UI marker NOT stamped (local-player-only) ----------------
    fx.reset();
    reset_all();
    fx.player_side              = 1;
    fx.planet_index             = 4;
    building &bm7               = fx.b(5, 7);
    bm7.state                   = ST_CONSTRUCTION;
    bm7.building_id             = 30;
    bm7.x                       = 50;
    bm7.y                       = 50;
    fx.cfg_buildings[30].type   = TY_A_MOTHER;
    fx.cfg_buildings[30].width  = 4;
    fx.cfg_buildings[30].height = 4;
    run(fx, 5, 7);
    ck_eq((uint32_t)fx.ui_base_marker_coords[0].cam_col, 0u, "MOTHER@0x00479a7d: other player -> marker untouched");

    // ---- PORT gate: prod_deliver_arrivals fires only for A_PORT/H_PORT (0x00479d6f-0x00479da1) --------
    fx.reset();
    reset_all();
    fx.player_side            = 1;
    building &bp1             = fx.b(1, 7);
    bp1.state                 = ST_CONSTRUCTION;
    bp1.building_id           = 30;
    fx.cfg_buildings[30].type = TY_A_PORT;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_prod_deliver_count, 1u, "CONSTRUCTION@0x00479da1: A_PORT -> prod_deliver_arrivals fires");
    fx.reset();
    reset_all();
    fx.player_side            = 1;
    building &bp2             = fx.b(1, 7);
    bp2.state                 = ST_CONSTRUCTION;
    bp2.building_id           = 30;
    fx.cfg_buildings[30].type = TY_H_PORT;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_prod_deliver_count, 1u, "CONSTRUCTION: H_PORT -> prod_deliver_arrivals fires");
    fx.reset();
    reset_all();
    fx.player_side            = 1;
    building &bp3             = fx.b(1, 7);
    bp3.state                 = ST_CONSTRUCTION;
    bp3.building_id           = 30;
    fx.cfg_buildings[30].type = TY_UNMATCHED;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_prod_deliver_count, 0u, "CONSTRUCTION: non-PORT type -> prod_deliver_arrivals NOT called");

    // ---- game_UpdateProgress gate: fires when NEW building's invention is NOT acquired
    // (0x00479cbf-0x00479d2e) -- UNCONDITIONAL relative to the type-switch/worker outcome ----------------
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    building &bg1                                     = fx.b(1, 7);
    bg1.state                                         = ST_CONSTRUCTION;
    bg1.building_id                                   = 30;
    fx.cfg_buildings[30].type                         = TY_UNMATCHED;
    fx.cfg_buildings[30].invention                    = 80;
    fx.progress[1 * PROGRESS_ROW_COUNT + 80].acquired = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_update_progress.size(), 1u, "CONSTRUCTION@0x00479d29: not-acquired -> game_UpdateProgress fires");
    if (!g_update_progress.empty()) {
        ck_eq((uint32_t)g_update_progress[0].plr, 1u, "CONSTRUCTION: game_UpdateProgress player arg");
        ck_eq((uint32_t)g_update_progress[0].inv, 80u, "CONSTRUCTION: game_UpdateProgress invention arg");
    }
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    building &bg2                                     = fx.b(1, 7);
    bg2.state                                         = ST_CONSTRUCTION;
    bg2.building_id                                   = 30;
    fx.cfg_buildings[30].type                         = TY_UNMATCHED;
    fx.cfg_buildings[30].invention                    = 80;
    fx.progress[1 * PROGRESS_ROW_COUNT + 80].acquired = 1;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_update_progress.size(), 0u, "CONSTRUCTION@0x00479cf5: already acquired -> no game_UpdateProgress");

    // ---- register_online: ALWAYS fires, param_3/param_4 hardcoded 0 regardless of this function's own
    // (dead) param_3/param_4, param_5 passed through unchanged (0x00479d2e-0x00479d43) -------------------
    fx.reset();
    reset_all();
    fx.player_side            = 1;
    building &bro             = fx.b(1, 7);
    bro.state                 = ST_CONSTRUCTION;
    bro.building_id           = 30;
    fx.cfg_buildings[30].type = TY_UNMATCHED;
    run(fx, 1, 7, 3.5);
    ck_eq((uint32_t)g_register_online.size(), 1u, "CONSTRUCTION@0x00479d3e: register_online always fires");
    if (!g_register_online.empty()) {
        ck_eq((uint32_t)g_register_online[0].bidx, 7u, "register_online: bidx arg");
        ck_eq(g_register_online[0].p3, 0u, "register_online: param_3 hardcoded 0 (REGISTER_ONLINE_PARAM3_UNRESOLVED)");
        ck_eq(g_register_online[0].p4, 0u, "register_online: param_4 hardcoded 0 (REGISTER_ONLINE_PARAM4_UNRESOLVED)");
        ck_eq_d(g_register_online[0].anim_dur, 3.5, "register_online: param_5 (anim_dur) passed through unchanged");
    }
    // link_to_network_if_adjacent: ALWAYS fires too (0x00479d4a)
    ck_eq((uint32_t)g_link_network.size(), 1u, "CONSTRUCTION@0x00479d4a: link_to_network_if_adjacent always fires");

    // ---- foreign-building-event relay, OTHER player only (0x00479da6-0x00479dd8) ----------------------
    fx.reset();
    reset_all();
    fx.player_side                = 1;          // local is 1
    building &bf1                 = fx.b(5, 7); // player 5 -> not local
    bf1.state                     = ST_CONSTRUCTION;
    bf1.building_id               = 30;
    fx.cfg_buildings[30].type     = TY_UNMATCHED;
    fx.foreign_bldg_event_pending = 1;
    run(fx, 5, 7);
    ck_eq((uint32_t)fx.foreign_bldg_change_flag, 1u, "CONSTRUCTION@0x00479dbb: other-player relay sets change_flag");
    ck_eq((uint32_t)fx.foreign_bldg_event_pending, 0u, "CONSTRUCTION@0x00479dc5: relay clears event_pending");
    fx.reset();
    reset_all();
    fx.player_side                = 1;
    building &bf2                 = fx.b(5, 7);
    bf2.state                     = ST_CONSTRUCTION;
    bf2.building_id               = 30;
    fx.cfg_buildings[30].type     = TY_UNMATCHED;
    fx.foreign_bldg_event_pending = 0; // nothing pending
    run(fx, 5, 7);
    ck_eq((uint32_t)fx.foreign_bldg_change_flag, 0u, "CONSTRUCTION@0x00479db9: other-player, nothing pending -> no relay");
    fx.reset();
    reset_all();
    fx.player_side                = 1;
    building &bf3                 = fx.b(1, 7); // LOCAL player this time
    bf3.state                     = ST_CONSTRUCTION;
    bf3.building_id               = 30;
    fx.cfg_buildings[30].type     = TY_UNMATCHED;
    fx.foreign_bldg_event_pending = 1; // would relay if reached -- but it's the dead subtree for local
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.foreign_bldg_change_flag, 0u,
          "CONSTRUCTION@0x00479db0: local player -> dead subtree, no relay even with event pending");

    // ==================================================================================================
    // BLDG_STATE_CHARGE_GATE (0x69) [0x0047a0cb-0x0047a0c6]: WORKER_MGMT only. Used here to exercise
    // WORKER_MGMT exhaustively (same helper CONSTRUCTION's tail and UPGRADING's tail call).
    // ==================================================================================================

    // ---- uses==0, current_workers!=0 -> unassign ALL; recheck stays nonzero (stub is side-effect-free)
    // -> set_staffed_flag, NOT clear (0x0047a0cb-0x0047a0c6 / the uses_workers==0 sub-branch) -----------
    fx.reset();
    reset_all();
    fx.player_side      = 1;
    building &wc1       = fx.b(1, 7);
    wc1.state           = ST_CHARGE_GATE;
    wc1.building_id     = 30;
    wc1.current_workers = 3;
    g_uses_workers_ret  = 0; // uses==0 for both potential calls
    run(fx, 1, 7);
    ck_eq((uint32_t)g_unassign.size(), 1u, "WORKER_MGMT: uses==0 && current_workers!=0 -> unassign fires");
    if (!g_unassign.empty()) ck_eq(g_unassign[0].count, 3u, "WORKER_MGMT: unassign count == current_workers");
    ck_eq((uint32_t)g_uses_workers_calls.size(), 1u,
          "WORKER_MGMT: only ONE uses_workers call (current_workers!=0 short-circuits the recheck's &&)");
    ck_eq((uint32_t)g_set_staffed.size(), 1u, "WORKER_MGMT: recheck false (workers still 3) -> set_staffed_flag");
    ck_eq((uint32_t)g_clear_staffed.size(), 0u, "WORKER_MGMT: clear_staffed_flag NOT called");

    // ---- FRESH-recheck: uses==0 with current_workers ALREADY 0 -> no unassign; the SECOND uses_workers
    // call is fresh (not cached from the first) -- first call returns 0, second returns nonzero ---------
    fx.reset();
    reset_all();
    fx.player_side       = 1;
    building &wc2        = fx.b(1, 7);
    wc2.state            = ST_CHARGE_GATE;
    wc2.building_id      = 30;
    wc2.current_workers  = 0;
    g_uses_workers_queue = {0, 5}; // call#1 -> 0 (skip unassign), call#2 -> 5 (fresh, not cached)
    run(fx, 1, 7);
    ck_eq((uint32_t)g_unassign.size(), 0u, "WORKER_MGMT: current_workers already 0 -> no unassign");
    ck_eq((uint32_t)g_uses_workers_calls.size(), 2u, "WORKER_MGMT: TWO uses_workers calls (current_workers==0 path)");
    ck_eq((uint32_t)g_clear_staffed.size(), 1u,
          "WORKER_MGMT: recheck's fresh 2nd call (5!=0) -> clear_staffed_flag, NOT cached from the 1st call's 0");
    ck_eq((uint32_t)g_set_staffed.size(), 0u, "WORKER_MGMT: set_staffed_flag NOT called on the fresh-nonzero path");

    // ---- worker_count < current_workers -> unassign the EXCESS (worker_count - current_workers) -------
    fx.reset();
    reset_all();
    fx.player_side                    = 1;
    building &wc3                     = fx.b(1, 7);
    wc3.state                         = ST_CHARGE_GATE;
    wc3.building_id                   = 30;
    wc3.current_workers               = 7;
    fx.cfg_buildings[30].worker_count = 3;
    g_uses_workers_ret                = 1; // uses!=0
    run(fx, 1, 7);
    ck_eq((uint32_t)g_unassign.size(), 1u, "WORKER_MGMT@excess: unassign fires");
    if (!g_unassign.empty()) ck_eq(g_unassign[0].count, 4u, "WORKER_MGMT@excess: count == current_workers-worker_count (7-3=4)");
    ck_eq((uint32_t)g_assign.size(), 0u, "WORKER_MGMT@excess: assign_workers NOT called");

    // ---- current_workers < worker_count && human!=0 -> assign round-toward-zero-half(human) -----------
    fx.reset();
    reset_all();
    fx.player_side                    = 1;
    building &wc4                     = fx.b(1, 7);
    wc4.state                         = ST_CHARGE_GATE;
    wc4.building_id                   = 30;
    wc4.current_workers               = 2;
    fx.cfg_buildings[30].worker_count = 10;
    fx.population[1].human            = 7; // (7 - (7>>31))>>1 = (7-0)>>1 = 3
    g_uses_workers_ret                = 1;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_assign.size(), 1u, "WORKER_MGMT@shortage: assign_workers fires");
    if (!g_assign.empty()) ck_eq((uint32_t)g_assign[0].count, 3u, "WORKER_MGMT@shortage: count == round-toward-zero-half(human) = 3");
    ck_eq((uint32_t)g_unassign.size(), 0u, "WORKER_MGMT@shortage: unassign_workers NOT called");

    // ---- current_workers < worker_count but human==0 -> neither assign nor unassign --------------------
    fx.reset();
    reset_all();
    fx.player_side                    = 1;
    building &wc5                     = fx.b(1, 7);
    wc5.state                         = ST_CHARGE_GATE;
    wc5.building_id                   = 30;
    wc5.current_workers               = 2;
    fx.cfg_buildings[30].worker_count = 10;
    fx.population[1].human            = 0;
    g_uses_workers_ret                = 1;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_assign.size(), 0u, "WORKER_MGMT@shortage,human==0: no assign");
    ck_eq((uint32_t)g_unassign.size(), 0u, "WORKER_MGMT@shortage,human==0: no unassign either");

    // ---- worker_count == current_workers -> neither branch fires ---------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                    = 1;
    building &wc6                     = fx.b(1, 7);
    wc6.state                         = ST_CHARGE_GATE;
    wc6.building_id                   = 30;
    wc6.current_workers               = 5;
    fx.cfg_buildings[30].worker_count = 5;
    fx.population[1].human            = 9;
    g_uses_workers_ret                = 1;
    run(fx, 1, 7);
    ck_eq((uint32_t)(g_assign.size() + g_unassign.size()), 0u, "WORKER_MGMT@equal: neither assign nor unassign");

    // ==================================================================================================
    // BLDG_STATE_PROD_WORKING (0x6d) [0x0047a281-0x0047a7b3]
    // ==================================================================================================

    // ---- move_op_code==0xf (ground) -> unit_spawn_docked (NOT add_docked); success (0)+local player+
    // reason_to_housing_bldg<1 -> 2-string w_sprintf__vss form (0x0047a294-0x0047a356) --------------------
    fx.reset();
    reset_all();
    fx.player_side                                                  = 1;
    building &pw1                                                   = fx.b(1, 7);
    pw1.state                                                       = ST_PROD_WORKING;
    pw1.building_id                                                 = 40;
    pw1.sub_id                                                      = 2;
    fx.productions[1 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x0f;
    fx.cfg_buildings[40].id                                         = 20;
    fx.text_ptrs[20]                                                = L"BLDNAME";
    fx.text_ptrs[3]                                                 = L"GROUPNAME"; // group_idx return value below
    g_spawn_docked_ret                                              = 0;            // success
    g_group_index_ret                                               = 3;
    g_housing_ret                                                   = 0; // <1 -> 2-string form
    run(fx, 1, 7);
    ck_eq((uint32_t)g_spawn_docked.size(), 1u, "PROD_WORKING@0x0047a304: move_op_code==0xf -> unit_spawn_docked");
    ck_eq((uint32_t)g_add_docked.size(), 0u, "PROD_WORKING: unit_add_docked NOT called on the 0xf path");
    if (!g_spawn_docked.empty()) ck_eq((uint32_t)g_spawn_docked[0].unit_proto_id, 55u, "PROD_WORKING: spawn_docked unit_type arg");
    ck_eq((uint32_t)g_wsprintf_vss.size(), 1u, "PROD_WORKING@0x0047a3e3: housing_bid<1 -> 2-string w_sprintf__vss");
    ck_eq((uint32_t)g_wsprintf_vsss.size(), 0u, "PROD_WORKING: 3-string form NOT used");
    if (!g_wsprintf_vss.empty()) {
        ck(g_wsprintf_vss[0].a0 == fx.text_ptrs[20], "PROD_WORKING: vss a0 == text_ptrs[cfg_buildings[bid].id]");
        ck(g_wsprintf_vss[0].a1 == fx.text_ptrs[3], "PROD_WORKING: vss a1 == text_ptrs[group_idx]");
    }
    ck_eq((uint32_t)g_print_text_count, 1u, "PROD_WORKING@0x00496508: game_ui_PrintTextMessage fires");
    ck_eq((uint32_t)g_apply_prod_completion.size(), 1u, "PROD_WORKING@0x0047a6ea: unit_apply_production_completion fires");
    ck_eq((uint32_t)g_notify_lifecycle.size(), 1u, "PROD_WORKING@0x0047a6f6-ish: ai_notify_unit_lifecycle fires");
    if (!g_notify_lifecycle.empty()) {
        ck_eq((uint32_t)g_notify_lifecycle[0].unit_id, 0u, "PROD_WORKING success: notify unit_id arg == 0");
        ck_eq((uint32_t)g_notify_lifecycle[0].param4, 2u, "PROD_WORKING success: notify param4 == 2");
    }

    // ---- move_op_code!=0xf -> unit_add_docked; success but is_local_player==false: no message printed,
    // but apply_production_completion/notify STILL fire (message gate is local-player-only, not these) --
    fx.reset();
    reset_all();
    fx.player_side                                                  = 1;          // local is 1
    building &pw2                                                   = fx.b(5, 7); // player 5 -> not local
    pw2.state                                                       = ST_PROD_WORKING;
    pw2.building_id                                                 = 40;
    pw2.sub_id                                                      = 2;
    fx.productions[5 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x00; // NOT 0xf
    g_add_docked_ret                                                = 0;    // success
    g_housing_ret                                                   = 3;    // >=1 -> 3-string form (irrelevant here, gated off by !local)
    run(fx, 5, 7);
    ck_eq((uint32_t)g_add_docked.size(), 1u, "PROD_WORKING@0x0047a53a: move_op_code!=0xf -> unit_add_docked");
    ck_eq((uint32_t)g_spawn_docked.size(), 0u, "PROD_WORKING: unit_spawn_docked NOT called on the non-0xf path");
    ck_eq((uint32_t)(g_wsprintf_vss.size() + g_wsprintf_vsss.size() + g_print_text_count), 0u,
          "PROD_WORKING: !is_local_player -> no housing message printed");
    ck_eq((uint32_t)g_apply_prod_completion.size(), 1u, "PROD_WORKING: apply_production_completion fires regardless of is_local_player");
    ck_eq((uint32_t)g_notify_lifecycle.size(), 1u, "PROD_WORKING: notify_unit_lifecycle fires regardless of is_local_player");

    // ---- reason_to_housing_bldg>=1 -> 3-string w_sprintf__vsss form ------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                                                  = 1;
    building &pw3                                                   = fx.b(1, 7);
    pw3.state                                                       = ST_PROD_WORKING;
    pw3.building_id                                                 = 40;
    pw3.sub_id                                                      = 2;
    fx.productions[1 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x0f;
    g_spawn_docked_ret                                              = 0;
    g_housing_ret                                                   = 4; // >=1 -> 3-string form
    fx.cfg_buildings[40].id                                         = 20;
    fx.cfg_buildings[4].id                                          = 21; // housing_bid==4 -> cfg_buildings[4].id
    run(fx, 1, 7);
    ck_eq((uint32_t)g_wsprintf_vsss.size(), 1u, "PROD_WORKING@0x0047a650: housing_bid>=1 -> 3-string w_sprintf__vsss");
    ck_eq((uint32_t)g_wsprintf_vss.size(), 0u, "PROD_WORKING: 2-string form NOT used");

    // ---- failure path (spawned!=0): notify_unit_lifecycle(..., spawned, 1); NO apply_production_
    // completion, NO housing message regardless of is_local_player (0x0047a356-0x0047a4fc) --------------
    fx.reset();
    reset_all();
    fx.player_side                                                  = 1;
    building &pw4                                                   = fx.b(1, 7);
    pw4.state                                                       = ST_PROD_WORKING;
    pw4.building_id                                                 = 40;
    pw4.sub_id                                                      = 2;
    fx.productions[1 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x0f;
    g_spawn_docked_ret                                              = 7; // failure -- error code 7
    run(fx, 1, 7);
    ck_eq((uint32_t)g_apply_prod_completion.size(), 0u, "PROD_WORKING failure: apply_production_completion NOT called");
    ck_eq((uint32_t)(g_wsprintf_vss.size() + g_wsprintf_vsss.size()), 0u, "PROD_WORKING failure: no housing message");
    ck_eq((uint32_t)g_notify_lifecycle.size(), 1u, "PROD_WORKING failure: notify_unit_lifecycle fires");
    if (!g_notify_lifecycle.empty()) {
        ck_eq((uint32_t)g_notify_lifecycle[0].unit_id, 7u, "PROD_WORKING failure: notify unit_id == spawned (error code)");
        ck_eq((uint32_t)g_notify_lifecycle[0].param4, 1u, "PROD_WORKING failure: notify param4 == 1");
    }

    // ---- REGRESSION LOCK #2: accum+=1 is UNCONDITIONAL across BOTH the spawned==0 and spawned!=0
    // sub-paths (0x0047a732-0x0047a773) -- exercised in TWO separate cases, each asserting the SAME
    // +1 bump. Reverting the fix (scoping the bump inside `if (spawned==0)` only) would make the
    // spawned!=0 case below fail (accum would stay 0, not become 1). --------------------------------
    fx.reset();
    reset_all();
    fx.player_side                                                  = 1;
    fx.planet_index                                                 = 2;
    fx.cfg_planets[2].invention_index                               = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired               = 1;
    fx.bldg_completion_accum                                        = 0;
    fx.tutorial_step                                                = 1; // keep the footer from mutating accum further
    building &pwr1                                                  = fx.b(1, 7);
    pwr1.state                                                      = ST_PROD_WORKING;
    pwr1.building_id                                                = 40;
    pwr1.sub_id                                                     = 2;
    fx.productions[1 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x0f;
    g_spawn_docked_ret                                              = 0; // spawned==0 sub-path
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 1u, "REGRESSION LOCK#2 @0x0047a732 (spawned==0 sub-path): accum+=1");

    fx.reset();
    reset_all();
    fx.player_side                                                  = 1;
    fx.planet_index                                                 = 2;
    fx.cfg_planets[2].invention_index                               = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired               = 1;
    fx.bldg_completion_accum                                        = 0;
    fx.tutorial_step                                                = 1;
    building &pwr2                                                  = fx.b(1, 7);
    pwr2.state                                                      = ST_PROD_WORKING;
    pwr2.building_id                                                = 40;
    pwr2.sub_id                                                     = 2;
    fx.productions[1 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x0f;
    g_spawn_docked_ret                                              = 9; // spawned!=0 sub-path (the OTHER branch)
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 1u,
          "REGRESSION LOCK#2 @0x0047a732 (spawned!=0 sub-path): accum+=1 too -- SAME bump, proving it is "
          "unconditional across both branches, not scoped to spawned==0 only");

    // is_local_player-gated: accum stays 0 for a non-local player regardless of spawned outcome
    fx.reset();
    reset_all();
    fx.player_side                                                  = 1; // local is 1
    fx.planet_index                                                 = 2;
    fx.cfg_planets[2].invention_index                               = 50;
    fx.progress[5 * PROGRESS_ROW_COUNT + 50].acquired               = 1;
    fx.bldg_completion_accum                                        = 0;
    fx.tutorial_step                                                = 1;
    building &pwr3                                                  = fx.b(5, 7); // player 5 -> not local
    pwr3.state                                                      = ST_PROD_WORKING;
    pwr3.building_id                                                = 40;
    pwr3.sub_id                                                     = 2;
    fx.productions[5 * PRODUCTIONS_PER_PLAYER + 2].active_unit_type = 55;
    fx.cfg_units[55].move_op_code                                   = 0x0f;
    g_spawn_docked_ret                                              = 0;
    run(fx, 5, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "PROD_WORKING@0x0047a73c: other player -> accum bump gated off");

    // ==================================================================================================
    // BLDG_STATE_MINE_EXTRACTING (0x74) [0x0047a7b8-0x0047aa08]
    // ==================================================================================================

    // ---- basic: neither cap binds -> resource_add(amount==extract_rate), no re-scan -------------------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.bldg_completion_slot_count                      = 4;
    building &me1                                      = fx.b(1, 7);
    me1.state                                          = ST_MINE_EXTRACTING;
    me1.building_id                                    = 40;
    me1.sub_id                                         = 0;
    mine &m1                                           = fx.mines[1 * MINES_PER_PLAYER + 0];
    m1.deposit_slot[0].resource_id                     = 3;
    m1.deposit_slot[0].tile_x_q4                       = 2;
    m1.deposit_slot[0].tile_y_q4                       = 5;
    m1.deposit_slot[0].extract_rate                    = 50;
    fx.resources[2 * 64 + 5].value[3]                  = 1000; // deposit plenty
    fx.storage_stats_rows[1].cap_prev[3]               = 1000;
    fx.player_resources[1 * PLAYER_RESOURCE_SLOTS + 3] = 100; // cap_room = 900, plenty
    run(fx, 1, 7);
    ck_eq((uint32_t)g_resource_add.size(), 1u, "MINE@0x0047a947: basic slot -> resource_add fires once");
    if (!g_resource_add.empty()) {
        ck_eq((uint32_t)g_resource_add[0].resource_index, 3u, "MINE: resource_add id == slot.resource_id");
        ck_eq((uint32_t)g_resource_add[0].amount, 50u, "MINE: uncapped amount == extract_rate");
    }
    ck_eq((uint32_t)(uint16_t)fx.resources[2 * 64 + 5].value[3], (uint32_t)(uint16_t)950,
          "MINE@0x0047a965: deposit cell decremented by amount (1000-50=950)");
    ck_eq((uint32_t)g_mine_scan.size(), 0u, "MINE@0x0047a98a: neither cap bound -> NO re-scan");

    // ---- REGRESSION LOCK #1: a deposit value with bit 15 set must ZERO-extend (large positive), not
    // sign-extend (negative) -- 0x0047a898/0x0047a8ba MOVZX. Deposit cell = 0x8000 (bit15 set);
    // extract_rate = 40000 (> 32768) so the deposit caps the extract. Correct (zero-extend): deposit_u
    // = 32768 (positive) -> capped_by_deposit=true -> amount=32768 -> resource_add(...,32768) fires and
    // the cell (32768-32768=0) triggers the OR-merge reset. If a future edit reverted to
    // static_cast<int32_t>(int16_t) sign-extension, deposit_u would read as -32768, amount would go
    // negative, and `if (amount>0)` would SILENTLY SKIP resource_add entirely -- this case's resource_
    // add-fires assertion is exactly what would fail under that regression. --------------------------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.bldg_completion_slot_count                      = 1;
    building &me2                                      = fx.b(1, 7);
    me2.state                                          = ST_MINE_EXTRACTING;
    me2.building_id                                    = 40;
    me2.sub_id                                         = 1;
    mine &m2                                           = fx.mines[1 * MINES_PER_PLAYER + 1];
    m2.deposit_slot[0].resource_id                     = 1;
    m2.deposit_slot[0].tile_x_q4                       = 6;
    m2.deposit_slot[0].tile_y_q4                       = 9;
    m2.deposit_slot[0].extract_rate                    = 40000;
    fx.resources[6 * 64 + 9].value[1]                  = (int16_t)0x8000; // bit15 set -- the trap bit pattern
    fx.storage_stats_rows[1].cap_prev[1]               = 100000;          // plenty, storage does NOT cap
    fx.player_resources[1 * PLAYER_RESOURCE_SLOTS + 1] = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_resource_add.size(), 1u,
          "REGRESSION LOCK#1 @0x0047a898: bit15-set deposit -> resource_add MUST fire (zero-extended, not "
          "silently skipped by a negative-amount misread)");
    if (!g_resource_add.empty())
        ck_eq((uint32_t)g_resource_add[0].amount, 32768u,
              "REGRESSION LOCK#1: amount == 32768 (zero-extended 0x8000), not a negative sign-extended value");
    ck_eq((uint32_t)(uint16_t)fx.resources[6 * 64 + 9].value[1], 0u, "REGRESSION LOCK#1: deposit cell drained to 0 (32768-32768)");
    ck_eq((uint32_t)(uint16_t)fx.resources[6 * 64 + 9].value[0], 0u,
          "MINE@0x0047a9dd: OR-merge reset fired (cell[0]=0, all other slots also 0)");
    ck_eq((uint32_t)g_mine_scan.size(), 1u, "REGRESSION LOCK#1: capped_by_deposit -> re-scan fires");

    // ---- storage-capped (not deposit-capped): amount reduced to cap_room, re-scan still fires ---------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.bldg_completion_slot_count                      = 1;
    building &me3                                      = fx.b(1, 7);
    me3.state                                          = ST_MINE_EXTRACTING;
    me3.building_id                                    = 40;
    me3.sub_id                                         = 2;
    mine &m3                                           = fx.mines[1 * MINES_PER_PLAYER + 2];
    m3.deposit_slot[0].resource_id                     = 2;
    m3.deposit_slot[0].tile_x_q4                       = 1;
    m3.deposit_slot[0].tile_y_q4                       = 1;
    m3.deposit_slot[0].extract_rate                    = 1000;
    fx.resources[1 * 64 + 1].value[2]                  = 5000; // plenty, NOT the binding cap
    fx.storage_stats_rows[1].cap_prev[2]               = 300;
    fx.player_resources[1 * PLAYER_RESOURCE_SLOTS + 2] = 250; // cap_room = 50 -- the binding cap
    run(fx, 1, 7);
    ck_eq((uint32_t)g_resource_add.size(), 1u, "MINE@storage-cap: resource_add fires");
    if (!g_resource_add.empty()) ck_eq((uint32_t)g_resource_add[0].amount, 50u, "MINE@storage-cap: amount == cap_room (50), not extract_rate");
    ck_eq((uint32_t)g_mine_scan.size(), 1u, "MINE@storage-cap: capped_by_storage -> re-scan fires");

    // ---- resource_id==0 slots are skipped (continue), iteration still reaches later slots -------------
    fx.reset();
    reset_all();
    fx.player_side                       = 1;
    fx.bldg_completion_slot_count        = 2;
    building &me4                        = fx.b(1, 7);
    me4.state                            = ST_MINE_EXTRACTING;
    me4.building_id                      = 40;
    me4.sub_id                           = 3;
    mine &m4                             = fx.mines[1 * MINES_PER_PLAYER + 3];
    m4.deposit_slot[0].resource_id       = 0; // skip
    m4.deposit_slot[1].resource_id       = 4;
    m4.deposit_slot[1].tile_x_q4         = 3;
    m4.deposit_slot[1].tile_y_q4         = 3;
    m4.deposit_slot[1].extract_rate      = 10;
    fx.resources[3 * 64 + 3].value[4]    = 1000;
    fx.storage_stats_rows[1].cap_prev[4] = 1000;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_resource_add.size(), 1u, "MINE@0x0047a814: resource_id==0 slot skipped, slot[1] still processed");
    if (!g_resource_add.empty()) ck_eq((uint32_t)g_resource_add[0].resource_index, 4u, "MINE: the one resource_add is for slot[1]'s id");

    // ---- DEFENSIVE: bldg_completion_slot_count is CACHED once per call, not re-read per iteration.
    // slot_count=2 at entry; slot[0] triggers a re-scan whose stub also mutates the fixture's
    // bldg_completion_slot_count to 10 (simulating a hypothetical future "re-read every iteration"
    // regression). slot[2]/slot[3] are seeded to ALSO trigger a resource_add/re-scan if the loop
    // incorrectly extended past the cached bound of 2 (and would be a genuine out-of-bounds read past
    // mh_map_object_mine::deposit_slot[4] if the mutated value of 10 were honoured). --------------------
    fx.reset();
    reset_all();
    fx.player_side                       = 1;
    fx.bldg_completion_slot_count        = 2;
    building &me5                        = fx.b(1, 7);
    me5.state                            = ST_MINE_EXTRACTING;
    me5.building_id                      = 40;
    me5.sub_id                           = 4;
    mine &m5                             = fx.mines[1 * MINES_PER_PLAYER + 4];
    m5.deposit_slot[0].resource_id       = 7;
    m5.deposit_slot[0].tile_x_q4         = 4;
    m5.deposit_slot[0].tile_y_q4         = 4;
    m5.deposit_slot[0].extract_rate      = 99999; // force capped_by_deposit -> re-scan fires -> mutates slot_count
    fx.resources[4 * 64 + 4].value[7]    = 10;    // small deposit -> caps, triggers re-scan
    fx.storage_stats_rows[1].cap_prev[7] = 1000000;
    m5.deposit_slot[1].resource_id       = 0; // skip (still within the cached bound of 2)
    m5.deposit_slot[2].resource_id       = 6; // would fire if the loop wrongly extended past the cached bound
    m5.deposit_slot[2].tile_x_q4         = 8;
    m5.deposit_slot[2].tile_y_q4         = 8;
    m5.deposit_slot[2].extract_rate      = 5;
    fx.resources[8 * 64 + 8].value[6]    = 1000; // mh_map_resources::value[8] -- 6 is in-bounds, 9 was not
    fx.storage_stats_rows[1].cap_prev[6] = 1000;
    g_mutate_slot_count_on_mine_scan     = true;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_resource_add.size(), 1u,
          "DEFENSIVE @cached slot_count: only slot[0] processed -- slot[2] (index >= cached bound 2) untouched "
          "even though the mine_scan_deposit_slot stub mutated bldg_completion_slot_count to 10 mid-call");
    ck_eq((uint32_t)fx.bldg_completion_slot_count, 10u,
          "DEFENSIVE: the underlying storage WAS mutated (proving the stub really fired) -- the loop simply "
          "never re-read it, because the .cpp caches slot_count in a local before the loop starts");

    // ==================================================================================================
    // BLDG_STATE_UPGRADING (0x82) [0x00479ddd..0x00479f21ish, WORKER_MGMT tail]
    // ==================================================================================================

    // ---- energy = energy * upgrade.energy / before.energy (FLD/FMUL/FDIV order); building_id becomes
    // upgrade_index; game_UpdateProgress gated on the NEW building's invention -----------------------
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    building &up1                                     = fx.b(1, 7);
    up1.state                                         = ST_UPGRADING;
    up1.building_id                                   = 30;
    up1.energy                                        = 100.0;
    fx.cfg_buildings[30].upgrade_index                = 8;
    fx.cfg_buildings[30].energy                       = 3.0; // before.energy (divisor)
    fx.cfg_buildings[8].energy                        = 7.0; // upgrade.energy (multiplier)
    fx.cfg_buildings[8].invention                     = 95;
    fx.progress[1 * PROGRESS_ROW_COUNT + 95].acquired = 0; // not acquired -> game_UpdateProgress fires
    g_uses_workers_ret                                = 0;
    run(fx, 1, 7);
    ck_eq_d(fx.b(1, 7).energy, (100.0 * 7.0) / 3.0,
            "UPGRADING@0x00479e1a-0x00479e5f: energy = energy*upgrade.energy/before.energy, in THAT order "
            "(FLD energy; FMUL upgrade.energy; FDIV before.energy)");
    ck_eq((uint32_t)fx.b(1, 7).building_id, 8u, "UPGRADING@0x00479e9f: building_id becomes upgrade_index");
    ck_eq((uint32_t)g_update_progress.size(), 1u, "UPGRADING@0x00479f10: NEW building's invention not acquired -> update fires");
    if (!g_update_progress.empty()) ck_eq((uint32_t)g_update_progress[0].inv, 95u, "UPGRADING: update_progress uses the NEW building_id's invention");
    ck_eq((uint32_t)g_set_staffed.size() + g_clear_staffed.size(), 1u, "UPGRADING: worker_mgmt tail runs (staffed flag touched)");

    // acquired -> no game_UpdateProgress
    fx.reset();
    reset_all();
    fx.player_side                                    = 1;
    building &up2                                     = fx.b(1, 7);
    up2.state                                         = ST_UPGRADING;
    up2.building_id                                   = 30;
    up2.energy                                        = 50.0;
    fx.cfg_buildings[30].upgrade_index                = 8;
    fx.cfg_buildings[30].energy                       = 5.0;
    fx.cfg_buildings[8].energy                        = 5.0;
    fx.cfg_buildings[8].invention                     = 95;
    fx.progress[1 * PROGRESS_ROW_COUNT + 95].acquired = 1;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_update_progress.size(), 0u, "UPGRADING@0x00479f15-gate: NEW building's invention acquired -> no update");

    // ==================================================================================================
    // BLDG_STATE_RESEARCHING (0x89) [0x0047aa0d-0x0047aa94]
    // ==================================================================================================

    // ---- local player + acquired -> accum+=15 (NOT +1 or +10 -- the THIRD distinct delta); game_
    // UpdateProgress fires UNCONDITIONALLY (0x0047aa43 / 0x0047aa8f) --------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.planet_index                                    = 2;
    fx.cfg_planets[2].invention_index                  = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired  = 1;
    fx.bldg_completion_accum                           = 0;
    fx.tutorial_step                                   = 1; // isolate from the footer
    building &rs1                                      = fx.b(1, 7);
    rs1.state                                          = ST_RESEARCHING;
    rs1.sub_id                                         = 5;
    fx.labs[1 * LABS_PER_PLAYER + 5].active_project_id = 12;
    fx.cfg_projects[12].invention                      = 77;
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 15u, "RESEARCHING@0x0047aa43: accum+=15 (the THIRD, distinct delta)");
    ck_eq((uint32_t)g_update_progress.size(), 1u, "RESEARCHING@0x0047aa8f: game_UpdateProgress fires");
    if (!g_update_progress.empty()) ck_eq((uint32_t)g_update_progress[0].inv, 77u, "RESEARCHING: update_progress uses cfg_projects[active_project_id].invention");

    // acquired==0 -> no accum bump, but game_UpdateProgress STILL fires (unconditional)
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.planet_index                                    = 2;
    fx.cfg_planets[2].invention_index                  = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired  = 0;
    fx.bldg_completion_accum                           = 0;
    building &rs2                                      = fx.b(1, 7);
    rs2.state                                          = ST_RESEARCHING;
    rs2.sub_id                                         = 5;
    fx.labs[1 * LABS_PER_PLAYER + 5].active_project_id = 12;
    fx.cfg_projects[12].invention                      = 77;
    run(fx, 1, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "RESEARCHING@0x0047aa38: not acquired -> no accum bump");
    ck_eq((uint32_t)g_update_progress.size(), 1u, "RESEARCHING: game_UpdateProgress fires REGARDLESS of the accum gate");

    // other player -> no accum bump, game_UpdateProgress STILL fires (unconditional, no local-player gate)
    fx.reset();
    reset_all();
    fx.player_side                                     = 1; // local is 1
    fx.planet_index                                    = 2;
    fx.cfg_planets[2].invention_index                  = 50;
    fx.progress[5 * PROGRESS_ROW_COUNT + 50].acquired  = 1;
    fx.bldg_completion_accum                           = 0;
    building &rs3                                      = fx.b(5, 7); // player 5 -> not local
    rs3.state                                          = ST_RESEARCHING;
    rs3.sub_id                                         = 5;
    fx.labs[5 * LABS_PER_PLAYER + 5].active_project_id = 12;
    fx.cfg_projects[12].invention                      = 77;
    run(fx, 5, 7);
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "RESEARCHING@0x0047aa17: other player -> accum bump gated off");
    ck_eq((uint32_t)g_update_progress.size(), 1u, "RESEARCHING: game_UpdateProgress has NO local-player gate at all");

    // ==================================================================================================
    // default / no-matching-state -- the outer CMP/JC/JBE ladder's fall-through (0x004796e6/0x004796e1/
    // 0x004796c3/0x0047aa94 join points), re-encoded as a switch's `default: break`
    // ==================================================================================================
    fx.reset();
    reset_all();
    fx.player_side           = 1;
    building &def1           = fx.b(1, 7);
    def1.state               = ST_UNMATCHED;
    fx.bldg_completion_accum = 0;
    run(fx, 1, 7);
    ck((uint32_t)(g_mine_scan.size() + g_resource_add.size() + g_population_add.size() + g_assign.size() +
                  g_unassign.size() + g_register_online.size() + g_link_network.size() + g_prod_deliver_count +
                  g_spawn_docked.size() + g_add_docked.size() + g_apply_prod_completion.size() +
                  g_notify_lifecycle.size() + g_update_progress.size()) == 0,
       "DEFAULT@outer ladder: unmatched state -> no arm-specific callee fires");
    ck_eq((uint32_t)fx.bldg_completion_accum, 0u, "DEFAULT: accum untouched");

    // ==================================================================================================
    // SHARED FOOTER (0x0047aa94-0x0047ab15): did_something && (accum+escalation_bonus)>=0x46 &&
    // tutorial_step==0 && session_mode==SINGLE_PLAYER(1) gates an invasion_chance_roll(1); roll==0
    // halves the accumulator, roll!=0 quarters it (REGRESSION LOCK #3); either way stamps
    // planet_mother_lost_time[planet] = game_clock.
    // ==================================================================================================

    // ---- gate satisfied, roll==0 -> HALVE: (accum-(accum>>31))>>1, positive accum -> plain /2 ----------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.planet_index                                    = 6;
    fx.cfg_planets[6].invention_index                  = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired  = 1;  // drives did_something via the RESEARCHING bump
    fx.bldg_completion_accum                           = 60; // + arm's own +15 = 75 >= 0x46(70)
    fx.tutorial_step                                   = 0;
    fx.session_mode                                    = SESSION_MODE_SP;
    fx.game_clock                                      = 500.0;
    building &f1                                       = fx.b(1, 7);
    f1.state                                           = ST_RESEARCHING;
    f1.sub_id                                          = 0;
    fx.labs[1 * LABS_PER_PLAYER + 0].active_project_id = 0;
    g_invasion_roll_ret                                = 0; // -> halve
    run(fx, 1, 7);
    ck_eq((uint32_t)g_invasion_roll_calls.size(), 1u, "FOOTER@0x0047aac4: gate satisfied -> invasion_chance_roll(1) fires");
    if (!g_invasion_roll_calls.empty()) ck_eq((uint32_t)g_invasion_roll_calls[0], 1u, "FOOTER: invasion_chance_roll arg == 1 (building_completed)");
    ck_eq((uint32_t)fx.bldg_completion_accum, 37u, "FOOTER@0x0047aaf8: roll==0 -> halve: (75-0)>>1 = 37");
    ck_eq_d(fx.planet_mother_lost_time[6], 500.0, "FOOTER@0x0047ab0f: planet_mother_lost_time stamped to game_clock");

    // ---- REGRESSION LOCK #3: gate satisfied via a LARGE escalation_bonus while accum itself is
    // NEGATIVE (-4); roll!=0 -> QUARTER. Correct: bias=(accum>>31)&3=3 -> (accum+3)>>2=(-4+3)>>2=(-1)>>2
    // = -1. The prior buggy SBB-misread formula computed accum-((accum>>31)<<2) = accum-(-4) =
    // accum+4 -> (-4+4)>>2 = 0>>2 = 0 -- a DIFFERENT result (0, not -1), so this case's exact-value
    // assertion below distinguishes the two formulas (accum=-4 was chosen precisely because it is a
    // multiple of 4, which is where (accum+3)>>2 and (accum+4)>>2 diverge -- accum=-1 or -5 do NOT
    // diverge, both formulas agree there). ---------------------------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                     = 1;
    fx.planet_index                    = 6;
    fx.mother_lost_escalation_interval = 60.0;
    fx.planet_mother_lost_time[6]      = 1.0;
    fx.game_clock                      = 1201.0; // (1201-1)/60 = 20.0 -> trunc 20 -> bonus 100
    fx.bldg_completion_accum           = -4;     // total = -4+100 = 96 >= 0x46(70)
    fx.tutorial_step                   = 0;
    fx.session_mode                    = SESSION_MODE_SP;
    building &f2                       = fx.b(1, 7);
    f2.state                           = ST_UNMATCHED; // isolate: no arm bump, did_something
                                                       // comes purely from the escalation bonus
    g_invasion_roll_ret = 1;                           // nonzero -> quarter
    run(fx, 1, 7);
    ck_eq((uint32_t)g_invasion_roll_calls.size(), 1u, "REGRESSION LOCK#3: gate satisfied via escalation_bonus alone");
    ck_eq((uint32_t)(int32_t)fx.bldg_completion_accum, (uint32_t)-1,
          "REGRESSION LOCK#3 @0x0047aad2-0x0047aae3: quarter(-4) == -1 (bias=+3), NOT 0 (the old buggy +4 bias)");
    ck_eq_d(fx.planet_mother_lost_time[6], 1201.0, "REGRESSION LOCK#3: mother_lost_time stamped on the quarter path too");

    // ---- gate fails: tutorial_step!=0 -> no roll, no stamp, accum left at the post-arm value -----------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.planet_index                                    = 6;
    fx.cfg_planets[6].invention_index                  = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired  = 1;
    fx.bldg_completion_accum                           = 60;
    fx.tutorial_step                                   = 1; // GATE FAILS here
    fx.session_mode                                    = SESSION_MODE_SP;
    fx.planet_mother_lost_time[6]                      = 0.0;
    building &f3                                       = fx.b(1, 7);
    f3.state                                           = ST_RESEARCHING;
    f3.sub_id                                          = 0;
    fx.labs[1 * LABS_PER_PLAYER + 0].active_project_id = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_invasion_roll_calls.size(), 0u, "FOOTER@0x0047aaa0: tutorial_step!=0 -> gate fails, no roll");
    ck_eq((uint32_t)fx.bldg_completion_accum, 75u, "FOOTER: gate-fail leaves accum at the post-arm value (60+15)");
    ck_eq_d(fx.planet_mother_lost_time[6], 0.0, "FOOTER: gate-fail -> no stamp");

    // ---- gate fails: session_mode != SINGLE_PLAYER (MP lockstep) ---------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.planet_index                                    = 6;
    fx.cfg_planets[6].invention_index                  = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired  = 1;
    fx.bldg_completion_accum                           = 60;
    fx.tutorial_step                                   = 0;
    fx.session_mode                                    = SESSION_MODE_MP; // GATE FAILS here
    building &f4                                       = fx.b(1, 7);
    f4.state                                           = ST_RESEARCHING;
    f4.sub_id                                          = 0;
    fx.labs[1 * LABS_PER_PLAYER + 0].active_project_id = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_invasion_roll_calls.size(), 0u, "FOOTER@0x0047aab4: session_mode!=1(MP) -> gate fails, no roll");

    // ---- gate fails: total < 0x46 --------------------------------------------------------------------
    fx.reset();
    reset_all();
    fx.player_side                                     = 1;
    fx.planet_index                                    = 6;
    fx.cfg_planets[6].invention_index                  = 50;
    fx.progress[1 * PROGRESS_ROW_COUNT + 50].acquired  = 1;
    fx.bldg_completion_accum                           = 0; // + arm's +15 = 15, well under 0x46(70)
    fx.tutorial_step                                   = 0;
    fx.session_mode                                    = SESSION_MODE_SP;
    building &f5                                       = fx.b(1, 7);
    f5.state                                           = ST_RESEARCHING;
    f5.sub_id                                          = 0;
    fx.labs[1 * LABS_PER_PLAYER + 0].active_project_id = 0;
    run(fx, 1, 7);
    ck_eq((uint32_t)g_invasion_roll_calls.size(), 0u, "FOOTER@0x0047aaa2: total<0x46 -> gate fails, no roll");
    ck_eq((uint32_t)fx.bldg_completion_accum, 15u, "FOOTER: gate-fail (magnitude) leaves accum at the post-arm value");

    // ---- gate fails: did_something==false (no arm bumped, no escalation bonus) -- even with accum
    // pre-seeded past the threshold from a previous call, the footer must not touch it --------------
    fx.reset();
    reset_all();
    fx.player_side                = 1;
    fx.planet_index               = 6;
    fx.bldg_completion_accum      = 200; // already "past threshold", but did_something is false
    fx.tutorial_step              = 0;
    fx.session_mode               = SESSION_MODE_SP;
    fx.planet_mother_lost_time[6] = 0.0;
    building &f6                  = fx.b(1, 7);
    f6.state                      = ST_UNMATCHED; // no arm bump; mother_lost_time<=0 -> no escalation bonus either
    run(fx, 1, 7);
    ck_eq((uint32_t)g_invasion_roll_calls.size(), 0u, "FOOTER@0x0047aa98: did_something==false -> gate fails regardless of accum's own value");
    ck_eq((uint32_t)fx.bldg_completion_accum, 200u, "FOOTER: did_something==false -> accum left completely untouched");
    ck_eq_d(fx.planet_mother_lost_time[6], 0.0, "FOOTER: did_something==false -> no stamp");
}

} // namespace mh::sim::test
