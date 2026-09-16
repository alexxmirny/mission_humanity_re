#include "sim/sim_unit_state_flight.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 5 callees --------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (5, one per unit_state_flight_calls member) ---------------------------
std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct UnbindCall {
    int32_t player, planet_slot;
};
std::vector<UnbindCall> g_unbind_calls;
void                    rec_prod_unbind_planet(int32_t player, int32_t planet_slot) {
    tr("prod_unbind_planet");
    g_unbind_calls.push_back({player, planet_slot});
}

struct WStrCopyCall {
    void *src, *dst;
};
std::vector<WStrCopyCall> g_wstrcopy_calls;
void                     *rec_w_str_copy(void *src, void *dst) {
    tr("w_str_copy");
    g_wstrcopy_calls.push_back({src, dst});
    return dst;
}

struct ConcatCall {
    void *dst, *src;
};
std::vector<ConcatCall> g_concat_calls;
void                   *rec_concat(void *dst, void *src) {
    tr("concat");
    g_concat_calls.push_back({dst, src});
    return dst;
}

std::vector<void *> g_print_calls;
uint32_t            rec_print_text_message(void *text) {
    tr("print_text_message");
    g_print_calls.push_back(text);
    return 0;
}

const unit_state_flight_calls g_calls = {
    &rec_unit_set_state,
    &rec_prod_unbind_planet,
    &rec_w_str_copy,
    &rec_concat,
    &rec_print_text_message,
};

void reset_observations() {
    g_trace.clear();
    g_set_state_calls.clear();
    g_unbind_calls.clear();
    g_wstrcopy_calls.clear();
    g_concat_calls.clear();
    g_print_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever touches -- sentinel nonzero data each run so a
// wrong-index write lands somewhere observable. Well outside every test's own player (0..3) /
// index (1..2) ranges below.
constexpr uint16_t GUARD_PLAYER = 6;
constexpr int32_t  GUARD_INDEX  = 9;

void seed_guard_slot(sim_fixture &fx) {
    unit &g          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.order          = 0x4444;
    g.elevation      = 424242;
    g.activity_clock = 9191.0;
    g.shuttle_slot   = 5;
}

void check_guard_slot_untouched(sim_fixture &fx, const char *what) {
    const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
    ck(g.order == 0x4444 && g.elevation == 424242 && g.activity_clock == 9191.0 && g.shuttle_slot == 5, what);
}

// A DIFFERENT roster slot for the SAME player, adjacent to the one the test drives, and never touched
// by seed_and_run_*/the function under test -- stays all-zero (fx.reset()'s memset) unless a wrong
// index write lands there.
void check_neighbor_untouched(sim_fixture &fx, uint16_t player, int32_t neighbor_index, const char *what) {
    const unit &n = fx.u(player, neighbor_index);
    ck(n.order == 0 && n.elevation == 0 && n.activity_clock == 0.0 && n.shuttle_slot == 0, what);
}

// =====================================================================================================
// climb_vertical @0x00481960 -- multiplier: 1.5 if order==ASCEND_TO_ORBIT(0x31) else 2.0 (loaded as
// inline hi-dword immediates 0x3ff80000/0x40000000 @0x0048198b/0x0048199b, gated by CMP @0x0048197d /
// JNZ @0x00481982); cost = step_speed[cur_player]*mult (0x004819a2-0x004819c6); budget gate FCOMP/
// JNC @0x004819c9-0x004819d5; step = +1 (INC @0x00481a12); cap test: elevation >= Unit[proto].elevation
// (JL @0x00481a33 skips the clamp when elevation<cap); new state = unit.order, the PENDING order, not
// a literal (0x00481a53-0x00481a5c). No tail effects.
// =====================================================================================================
struct SeedClimb {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    uint16_t order          = UNIT_STATE_ASCEND_TO_ORBIT; // 0x31 -> 1.5 multiplier by default
    double   step_speed     = 4.0;
    double   tick_budget    = 10.0;
    int32_t  elevation      = 10;
    int32_t  cfg_elevation  = 50; // cap, approached from BELOW
    double   activity_clock = 100.0;
};

void seed_and_run_climb(sim_fixture &fx, const SeedClimb &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.cfg_row;
    u.order          = s.order;
    u.elevation      = s.elevation;
    u.activity_clock = s.activity_clock;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    cfg_unit &cu            = fx.cfg_units[s.cfg_row];
    cu.step_speed[s.player] = s.step_speed;
    cu.elevation            = s.cfg_elevation;

    fx.tick_budget = s.tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_climb_vertical(fx.view(), own, g_calls);
}

void run_climb_vertical_tests(sim_fixture &fx) {
    // ---- C1: multiplier selection, proven by a budget that spends under 1.5 but STALLS under 2.0,
    // with the IDENTICAL step_speed on both sides; also the not-yet-capped spend case. -----------------
    {
        SeedClimb s;
        s.player        = 0;
        s.index         = 1;
        s.step_speed    = 4.0; // cost@1.5==6.0, cost@2.0==8.0
        s.tick_budget   = 7.0; // >=6.0 but <8.0 -- separates the two multipliers
        s.elevation     = 10;
        s.cfg_elevation = 50;                         // far from cap
        s.order         = UNIT_STATE_ASCEND_TO_ORBIT; // 0x31 -> multiplier 1.5

        seed_and_run_climb(fx, s);
        ck_eq_d(fx.tick_budget, 1.0,
                "climb C1a: order==ASCEND_TO_ORBIT -> cost=step_speed*1.5=6.0, budget 7.0>=6.0 spends "
                "(0x0048197d CMP order,0x31 / 0x00481982 JNZ NOT taken when equal, falls into the 1.5 "
                "immediate @0x0048198b; 0x004819a2-0x004819c6 cost=step_speed*mult; 0x004819d5 JNC "
                "takes the spend arm; 0x004819fe-0x00481a07 tick_budget-=cost)");
        ck_eq((uint32_t)fx.u(0, 1).elevation, 11u,
              "climb C1a: elevation += 1 on the spend arm (0x00481a0d-0x00481a12 INC)");
        ck(g_set_state_calls.empty(),
           "climb C1a: elevation 11 < cap 50 -- no clamp/set_state (0x00481a33 JL taken, not-yet-capped)");
        ck(g_trace.empty(), "climb C1a: no outward calls at all when not capped");

        s.order = 0x99; // NOT ASCEND_TO_ORBIT -> multiplier 2.0, SAME step_speed+budget as C1a
        seed_and_run_climb(fx, s);
        ck_eq_d(fx.tick_budget, 0.0,
                "climb C1b: order!=0x31 -> cost=step_speed*2.0=8.0 (0x00481994-0x0048199b selects the "
                "2.0 immediate), budget 7.0<8.0 -- STALLS where C1a spent (same step_speed+budget) "
                "-- pins the multiplier selection, not merely 'some multiplier'");
        ck_eq_d(fx.u(0, 1).activity_clock, 100.0 - 7.0,
                "climb C1b: stall arm -- activity_clock -= tick_budget (0x004819d7-0x004819e5 FSUBR form)");
        ck_eq((uint32_t)fx.u(0, 1).elevation, 10u, "climb C1b: elevation UNCHANGED on the stall arm");
        ck(g_trace.empty(), "climb C1b: no outward calls on the stall arm");
    }

    // ---- C2: cap boundary (elevation-after-step exactly == cfg cap) + new-state argument is the
    // PENDING order, under BOTH cost-multiplier code paths. ---------------------------------------------
    {
        SeedClimb s;
        s.player        = 1;
        s.index         = 1;
        s.step_speed    = 2.0; // cost@2.0==4.0
        s.tick_budget   = 10.0;
        s.elevation     = 19; // +1 == 20 == cap
        s.cfg_elevation = 20;
        s.order         = 0x2b5; // sentinel, NOT ASCEND_TO_ORBIT -> multiplier 2.0

        seed_and_run_climb(fx, s);
        ck_eq_d(fx.tick_budget, 6.0, "climb C2a: cost=step_speed*2.0=4.0, tick_budget 10.0-4.0=6.0");
        ck_eq((uint32_t)fx.u(1, 1).elevation, 20u,
              "climb C2a: elevation clamped to Unit[proto].elevation=20 (0x00481a35-0x00481a50), "
              "boundary case elevation(19)+1==cap(20) (0x00481a2d CMP/0x00481a33 JL NOT taken)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x2b5,
           "climb C2a: unit_set_state(unit.order=0x2b5) -- the PENDING order, not a literal "
           "(0x00481a58 MOVZX order/0x00481a5c CALL)");
        ck(trace_eq({"unit_set_state"}), "climb C2a: exactly one outward call, on the capped path");

        s.order      = UNIT_STATE_ASCEND_TO_ORBIT; // 0x31 -> multiplier 1.5, SAME cap boundary shape
        s.step_speed = 2.0;                        // cost@1.5==3.0
        seed_and_run_climb(fx, s);
        ck_eq_d(fx.tick_budget, 7.0, "climb C2b: cost=step_speed*1.5=3.0, tick_budget 10.0-3.0=7.0");
        ck_eq((uint32_t)fx.u(1, 1).elevation, 20u, "climb C2b: elevation clamped to cap=20 again, other multiplier path");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_ASCEND_TO_ORBIT,
           "climb C2b: unit_set_state(unit.order=0x31) -- pass-through holds under the 1.5 path too");

        check_neighbor_untouched(fx, 1, 2, "climb C2b: neighbouring index (player 1, index 2) untouched");
        check_guard_slot_untouched(fx, "climb C2b: guard slot (player 6, index 9) untouched");
    }
}

// =====================================================================================================
// descend_cruise @0x00481f7c -- multiplier UNCONDITIONALLY 1.5 (0x00481f94-0x00481f9b, no order-code
// branch); step = -1 (DEC @0x00482015); cap test: elevation <= Unit[proto].elevation (JG @0x00482036
// skips the clamp+tail when elevation>cap, i.e. the floor test, approached from ABOVE); new state =
// unit.order (0x00482059-0x00482062). TAIL (only on the capped path): re-derives unit_proto_id via
// roster arithmetic and tests Unit[proto].type against FOUR values in sequence -- A_HELI_MOTHER(0x13)
// @0x00482090, H_HELI_MOTHER(0x14) @0x004820c2, A_HELI_SHUTTLE(0x15) @0x004820f6,
// H_HELI_SHUTTLE(0x16) @0x00482128 -- any match falls into llm_strat_prod_unbind_planet(cur_player,
// G_PLANET_INDEX) @0x00482131-0x0048213e; none matching skips it entirely (0x0048212f JNZ to the tail).
// =====================================================================================================
struct SeedDescend {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    double   step_speed     = 4.0;
    double   tick_budget    = 10.0;
    int32_t  elevation      = 10;
    int32_t  cfg_elevation  = 0; // cap/floor, approached from ABOVE
    double   activity_clock = 100.0;
    uint16_t order          = 0x55;             // sentinel pending order, passed through on the capped path
    uint32_t cfg_type       = UNIT_TYPE_A_HELI; // 0x0f -- non-mother/shuttle by default
    int32_t  planet_index   = 4;
};

void seed_and_run_descend(sim_fixture &fx, const SeedDescend &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.cfg_row;
    u.order          = s.order;
    u.elevation      = s.elevation;
    u.activity_clock = s.activity_clock;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    cfg_unit &cu            = fx.cfg_units[s.cfg_row];
    cu.step_speed[s.player] = s.step_speed;
    cu.elevation            = s.cfg_elevation;
    cu.type                 = s.cfg_type;

    fx.tick_budget  = s.tick_budget;
    fx.planet_index = s.planet_index;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_descend_cruise(fx.view(), own, g_calls);
}

void run_descend_cruise_tests(sim_fixture &fx) {
    // ---- D1: spend arm, not-yet-capped -- no clamp/set_state/unbind at all. ---------------------------
    {
        SeedDescend s;
        s.player        = 0;
        s.index         = 1;
        s.step_speed    = 4.0; // cost==6.0 (unconditional 1.5)
        s.tick_budget   = 10.0;
        s.elevation     = 10;
        s.cfg_elevation = 0; // floor far below

        seed_and_run_descend(fx, s);
        ck_eq_d(fx.tick_budget, 4.0,
                "descend D1: cost=step_speed*1.5=6.0 unconditionally (0x00481f94-0x00481f9b, no "
                "order-code branch), tick_budget 10.0-6.0=4.0 (0x00482001-0x0048200a)");
        ck_eq((uint32_t)fx.u(0, 1).elevation, 9u, "descend D1: elevation -= 1 (0x00482010-0x00482015 DEC)");
        ck(g_trace.empty(),
           "descend D1: elevation 9 > cap 0 -- NOT capped (0x00482030 CMP/0x00482036 JG taken), no "
           "set_state/unbind at all");
    }

    // ---- D2: stall arm. ---------------------------------------------------------------------------------
    {
        SeedDescend s;
        s.player         = 0;
        s.index          = 1;
        s.step_speed     = 4.0; // cost==6.0
        s.tick_budget    = 5.0; // < 6.0
        s.elevation      = 10;
        s.activity_clock = 100.0;

        seed_and_run_descend(fx, s);
        ck_eq_d(fx.tick_budget, 0.0, "descend D2: stall arm -- tick_budget=0.0 (0x00481fe8-0x00481ff2)");
        ck_eq_d(fx.u(0, 1).activity_clock, 100.0 - 5.0,
                "descend D2: activity_clock -= tick_budget (0x00481fd7-0x00481fe5 FSUBR form)");
        ck_eq((uint32_t)fx.u(0, 1).elevation, 10u, "descend D2: elevation UNCHANGED on the stall arm");
        ck(g_trace.empty(), "descend D2: no outward calls on the stall arm");
    }

    // ---- D3: capped boundary (elevation-1 == cfg cap) -- order pass-through + the 4-way type gate. -----
    {
        SeedDescend s;
        s.player        = 0;
        s.index         = 1;
        s.step_speed    = 4.0; // cost==6.0
        s.tick_budget   = 10.0;
        s.elevation     = 10; // -1 == 9 == cap
        s.cfg_elevation = 9;
        s.order         = 0x55;
        s.planet_index  = 4;

        // D3a: A_HELI_MOTHER(0x13) -- first check @0x00482090/0x00482097 JZ taken -> unbind.
        s.cfg_type = UNIT_TYPE_A_HELI_MOTHER;
        seed_and_run_descend(fx, s);
        ck_eq((uint32_t)fx.u(0, 1).elevation, 9u,
              "descend D3a: elevation clamped to cap=9 (0x0048203c-0x00482056), boundary "
              "elevation(10)-1==cap(9) (0x00482030 CMP/0x00482036 JG NOT taken)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x55,
           "descend D3a: unit_set_state(unit.order=0x55) on the capped path (0x00482059-0x00482062)");
        ck(g_unbind_calls.size() == 1 && g_unbind_calls[0].player == 0 && g_unbind_calls[0].planet_slot == 4,
           "descend D3a: type==A_HELI_MOTHER(0x13) -- prod_unbind_planet(cur_player, G_PLANET_INDEX) "
           "fires (0x00482090 CMP/0x00482097 JZ, 0x00482131-0x0048213e EDX=planet_index,EAX=cur_player)");
        ck(trace_eq({"unit_set_state", "prod_unbind_planet"}), "descend D3a: exact call order");

        // D3b: H_HELI_MOTHER(0x14) -- second check @0x004820c2/0x004820c9 JNZ NOT taken -> unbind.
        s.cfg_type = UNIT_TYPE_H_HELI_MOTHER;
        seed_and_run_descend(fx, s);
        ck(g_unbind_calls.size() == 1,
           "descend D3b: type==H_HELI_MOTHER(0x14) -- unbind fires via the SECOND type check "
           "(0x004820c2 CMP/0x004820c9 JNZ not taken, falls through to the unbind block)");

        // D3c: A_HELI_SHUTTLE(0x15) -- third check @0x004820f6/0x004820fd JZ taken -> unbind.
        s.cfg_type = UNIT_TYPE_A_HELI_SHUTTLE;
        seed_and_run_descend(fx, s);
        ck(g_unbind_calls.size() == 1,
           "descend D3c: type==A_HELI_SHUTTLE(0x15) -- unbind fires via the THIRD type check "
           "(0x004820f6 CMP/0x004820fd JZ taken)");

        // D3d: H_HELI_SHUTTLE(0x16) -- fourth check @0x00482128/0x0048212f JNZ NOT taken -> unbind.
        s.cfg_type = UNIT_TYPE_H_HELI_SHUTTLE;
        seed_and_run_descend(fx, s);
        ck(g_unbind_calls.size() == 1,
           "descend D3d: type==H_HELI_SHUTTLE(0x16) -- unbind fires via the FOURTH type check "
           "(0x00482128 CMP/0x0048212f JNZ not taken)");

        // D3e: A_HELI(0x0f) -- none of the four checks match -> unbind does NOT fire.
        s.cfg_type = UNIT_TYPE_A_HELI;
        seed_and_run_descend(fx, s);
        ck(g_unbind_calls.empty(),
           "descend D3e: type==A_HELI(0x0f) matches NONE of the four heli-mother/shuttle values -- "
           "prod_unbind_planet does NOT fire (0x0048212f JNZ taken to the tail)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x55,
           "descend D3e: unit_set_state still fires (the clamp/set_state and the type gate are "
           "independent -- clamp always runs once capped, unbind is conditional)");
        ck(trace_eq({"unit_set_state"}), "descend D3e: exact call order -- no unbind in the trace");

        check_neighbor_untouched(fx, 0, 2, "descend D3e: neighbouring index (player 0, index 2) untouched");
        check_guard_slot_untouched(fx, "descend D3e: guard slot (player 6, index 9) untouched");
    }
}

// =====================================================================================================
// ascend_to_orbit @0x0048214d -- multiplier UNCONDITIONALLY 1.5 (0x00482165-0x0048216c); step = +3
// (ADD ...,0x3 @0x004821e6); cap = Unit[proto].elevation + 300 (0x12c) (0x004821f9-0x00482205); cap
// test: cap <= elevation (JG @0x0048220d skips the tail when cap>elevation, i.e. not-yet-capped); new
// state = the LITERAL UNIT_STATE_PRODUCTION_READY(0xcd) (0x00482236 MOV EAX,0xcd / 0x0048223b CALL),
// NOT unit.order, unlike the other two. TAIL (unconditional once capped): prod_unbind_planet(cur_player,
// G_PLANET_INDEX) @0x00482240-0x0048224d, then the SIM-CUT UI message: w_str_copy(text_ptrs[0x1c],
// text_scratch) @0x0048225e-0x00482269, concat(text_scratch," (") @0x0048226e-0x00482278, concat(
// text_scratch, cfg_planets[dest_planet].name -> text_ptrs[name]) @0x0048227d-0x004822b4 (dest_planet
// from _G_LLM_PROD_SHUTTLE_SLOTS[cur_player*10+shuttle_slot].dest_planet), concat(text_scratch,")")
// @0x004822b9-0x004822c3, print_text_message(text_scratch) @0x004822c8-0x004822cd.
// =====================================================================================================
struct SeedAscend {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    double   step_speed     = 4.0;
    double   tick_budget    = 10.0;
    int32_t  elevation      = 10;
    int32_t  cfg_elevation  = 100; // cap = cfg_elevation + 300
    double   activity_clock = 100.0;
    uint16_t order          = UNIT_STATE_ASCEND_TO_ORBIT; // sentinel: must NOT reach unit_set_state
    int32_t  planet_index   = 4;
    uint8_t  shuttle_slot   = 3;
    int16_t  dest_planet    = 7;
    int32_t  planet_name_id = 42;
};

// Distinct wide-string content for the two text_ptrs entries the UI message reads, so a swapped
// pointer (base message vs planet name) disagrees with the fixture.
const wchar_t BASE_MSG_TEXT[]    = L"BASE_SIM_CUT_MSG";
const wchar_t PLANET_NAME_TEXT[] = L"TEST_PLANET_NAME";

void seed_and_run_ascend(sim_fixture &fx, const SeedAscend &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.cfg_row;
    u.order          = s.order;
    u.elevation      = s.elevation;
    u.activity_clock = s.activity_clock;
    u.shuttle_slot   = s.shuttle_slot;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    cfg_unit &cu            = fx.cfg_units[s.cfg_row];
    cu.step_speed[s.player] = s.step_speed;
    cu.elevation            = s.cfg_elevation;

    fx.tick_budget  = s.tick_budget;
    fx.planet_index = s.planet_index;

    fx.text_ptrs[TEXT_ID_ASCEND_TO_ORBIT_BASE] = BASE_MSG_TEXT;
    fx.text_ptrs[(size_t)s.planet_name_id]     = PLANET_NAME_TEXT;
    fx.prod_shuttle_slots[(size_t)s.player * PROD_SHUTTLE_SLOTS_PER_PLAYER + s.shuttle_slot].dest_planet =
        s.dest_planet;
    fx.cfg_planets[(size_t)(uint16_t)s.dest_planet].name = s.planet_name_id;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_ascend_to_orbit(fx.view(), own, g_calls);
}

void run_ascend_to_orbit_tests(sim_fixture &fx) {
    // ---- A1: spend arm, not-yet-capped -- elevation steps by +3, NO tail at all. -----------------------
    {
        SeedAscend s;
        s.player        = 0;
        s.index         = 1;
        s.step_speed    = 4.0; // cost==6.0 (unconditional 1.5)
        s.tick_budget   = 10.0;
        s.elevation     = 10;
        s.cfg_elevation = 100; // cap = 400, far above

        seed_and_run_ascend(fx, s);
        ck_eq_d(fx.tick_budget, 4.0,
                "ascend A1: cost=step_speed*1.5=6.0 unconditionally (0x00482165-0x0048216c), "
                "tick_budget 10.0-6.0=4.0 (0x004821d2-0x004821db)");
        ck_eq((uint32_t)fx.u(0, 1).elevation, 13u, "ascend A1: elevation += 3 (0x004821e1-0x004821e6 ADD)");
        ck(g_trace.empty(),
           "ascend A1: elevation 13 < cap 400 -- NOT capped (0x0048220a CMP/0x0048220d JG taken), no "
           "set_state/unbind/UI calls at all");
    }

    // ---- A2: stall arm. -----------------------------------------------------------------------------
    {
        SeedAscend s;
        s.player         = 1;
        s.index          = 1;
        s.step_speed     = 4.0; // cost==6.0
        s.tick_budget    = 5.0; // < 6.0
        s.elevation      = 10;
        s.activity_clock = 100.0;

        seed_and_run_ascend(fx, s);
        ck_eq_d(fx.tick_budget, 0.0, "ascend A2: stall arm -- tick_budget=0.0 (0x004821b9-0x004821c3)");
        ck_eq_d(fx.u(1, 1).activity_clock, 100.0 - 5.0,
                "ascend A2: activity_clock -= tick_budget (0x004821a8-0x004821b6 FSUBR form)");
        ck_eq((uint32_t)fx.u(1, 1).elevation, 10u, "ascend A2: elevation UNCHANGED on the stall arm");
        ck(g_trace.empty(), "ascend A2: no outward calls on the stall arm");
    }

    // ---- A3: capped boundary (elevation+3 == cap) -- literal new-state, unconditional unbind, and the
    // full SIM-CUT UI message chain (argument tuples + exact call order). -------------------------------
    {
        SeedAscend s;
        s.player        = 0;
        s.index         = 1;
        s.step_speed    = 4.0; // cost==6.0
        s.tick_budget   = 10.0;
        s.elevation     = 307; // +3 == 310 == cap(10+300)
        s.cfg_elevation = 10;
        s.order         = UNIT_STATE_ASCEND_TO_ORBIT; // 0x31 -- if unit_set_state wrongly got unit.order
                                                      // instead of the literal, this would look
                                                      // plausible; the assertion below refutes that.
        s.planet_index   = 4;
        s.shuttle_slot   = 3;
        s.dest_planet    = 7;
        s.planet_name_id = 42;

        seed_and_run_ascend(fx, s);

        ck_eq_d(fx.tick_budget, 4.0, "ascend A3: cost=6.0, tick_budget 10.0-6.0=4.0");
        ck_eq((uint32_t)fx.u(0, 1).elevation, 310u,
              "ascend A3: elevation clamped to Unit[proto].elevation+300=310 (0x00482213-0x00482233), "
              "boundary elevation(307)+3==cap(310) (0x0048220a CMP/0x0048220d JG NOT taken)");

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_PRODUCTION_READY,
           "ascend A3: unit_set_state(LITERAL 0xcd), NOT unit.order(0x31) -- 0x00482236 MOV EAX,0xcd is "
           "an immediate, not a read of unit+0x4");

        ck(g_unbind_calls.size() == 1 && g_unbind_calls[0].player == 0 && g_unbind_calls[0].planet_slot == 4,
           "ascend A3: prod_unbind_planet(cur_player, G_PLANET_INDEX) fires UNCONDITIONALLY once "
           "capped, no type gate (0x00482240-0x0048224d)");

        ck(g_wstrcopy_calls.size() == 1 &&
               g_wstrcopy_calls[0].src == (const void *)BASE_MSG_TEXT &&
               g_wstrcopy_calls[0].dst == (void *)fx.text_scratch.data(),
           "ascend A3: w_str_copy(text_ptrs[TEXT_ID_ASCEND_TO_ORBIT_BASE=0x1c], text_scratch) -- the "
           "FIXED base message id, not any unit/proto-name lookup (0x0048225e-0x00482269)");

        ck(g_concat_calls.size() == 3, "ascend A3: exactly 3 concat calls");
        if (g_concat_calls.size() == 3) {
            ck(g_concat_calls[0].dst == (void *)fx.text_scratch.data() &&
                   std::wcscmp((const wchar_t *)g_concat_calls[0].src, L" (") == 0,
               "ascend A3: concat #1 appends \" (\" (0x0048226e-0x00482278)");
            ck(g_concat_calls[1].dst == (void *)fx.text_scratch.data() &&
                   g_concat_calls[1].src == (const void *)PLANET_NAME_TEXT,
               "ascend A3: concat #2 appends the dest planet's name -- "
               "PROD_SHUTTLE_SLOTS[cur_player*10+shuttle_slot].dest_planet(7) -> "
               "cfg_planets[7].name(42) -> text_ptrs[42] (0x0048227d-0x004822b4)");
            ck(g_concat_calls[2].dst == (void *)fx.text_scratch.data() &&
                   std::wcscmp((const wchar_t *)g_concat_calls[2].src, L")") == 0,
               "ascend A3: concat #3 appends \")\" (0x004822b9-0x004822c3)");
        }

        ck(g_print_calls.size() == 1 && g_print_calls[0] == (void *)fx.text_scratch.data(),
           "ascend A3: print_text_message(text_scratch), return value discarded (0x004822c8-0x004822cd)");

        ck(trace_eq({"unit_set_state", "prod_unbind_planet", "w_str_copy", "concat", "concat", "concat",
                     "print_text_message"}),
           "ascend A3: exact CALL ORDER across the whole capped tail");

        ck_eq((uint32_t)fx.u(0, 1).shuttle_slot, 3u,
              "ascend A3: unit.shuttle_slot is READ, never WRITTEN, by this function -- unchanged");

        check_neighbor_untouched(fx, 0, 2, "ascend A3: neighbouring index (player 0, index 2) untouched");
        check_guard_slot_untouched(fx, "ascend A3: guard slot (player 6, index 9) untouched");
    }
}

} // namespace

void run_unit_state_flight_tests() {
    sim_fixture fx;
    run_climb_vertical_tests(fx);
    run_descend_cruise_tests(fx);
    run_ascend_to_orbit_tests(fx);
}

} // namespace mh::sim::test
