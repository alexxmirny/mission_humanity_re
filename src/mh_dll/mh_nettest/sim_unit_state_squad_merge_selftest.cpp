//
// sim_unit_state_squad_merge_selftest.cpp -- `simtest` oracle for llm_strat_unit_state_squad_merge
// @0x0047ea62 (sim/sim_unit_state_squad_merge.h/.cpp, RI-SIM / SIM1-G3 batch).
//
// EXPECTED BEHAVIOUR read directly from
// tmp/decomp_sim/llm_strat_unit_state_squad_merge_0047ea62.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .c draft (per the batch instructions -- the .c has
// lied in this project before) and not read off the .cpp body either.
//
// *** T7's DIVERGENCE WAS REAL AND IS NOW FIXED (2026-08-21) -- the note below is kept as history ***
// The .cpp and its header banner have both carried the asm's `max(dx,dy)` selection since then, so T7
// PASSES. Read what follows as the record of a finding, not as a live failure.
//
// The adjacency "near" pick at 0x0047ec86-0x0047ecb1 is, per the asm:
//   near = (dx <= dy) ? wrap_delta_y(x,y,goal_x,goal_y) : wrap_delta_x(x,y,goal_x)     -- i.e. max(dx,dy)
// (JLE 0x0047ec88 jumps to LAB_0047eca0's wrap_delta_y call when dx<=dy; the dx<=dy-FALSE fallthrough
// at 0x0047ec8a calls wrap_delta_x a second time instead). sim_unit_state_squad_merge.cpp instead has:
//   near = (dx <= dy) ? c.wrap_delta_x(x,y,goal_x) : c.wrap_delta_y(x,y,goal_x,goal_y)  -- i.e. min(dx,dy)
// the OPPOSITE selection (the header banner's own prose -- "the SMALLER of the two axis deltas is
// recomputed" -- is the same inversion, so the .cpp's mistranslation and its own header agree with each
// other but not with the asm). T7 was written to the asm ground truth and therefore FAILED against
// the .cpp of the day; that was the intended, reported finding, not a test to weaken -- and the .cpp
// was corrected to match it.
//
#include "sim/sim_unit_state_squad_merge.h"

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT (shared there)

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across the dir_from_to..unit_change_proto_and_energy span.
// wrap_delta_x/y are deliberately NOT traced here -- their own call identity/order is T7's whole
// point, and tracing them here would make every other case assert on that same disputed branch.
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
bool                      trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- llm_map_wrap_delta_x / _y (0x0047ec62-0x0047ecb1) ---------------------------------------------
int32_t g_wrap_dx_calls = 0, g_wrap_dy_calls = 0;
int32_t g_wrap_dx_result = 0, g_wrap_dy_result = 0;
int32_t rec_wrap_delta_x(int32_t pos_a, uint32_t unused_param, int32_t pos_b) {
    (void)pos_a;
    (void)unused_param;
    (void)pos_b;
    ++g_wrap_dx_calls;
    return g_wrap_dx_result;
}
int32_t rec_wrap_delta_y(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    ++g_wrap_dy_calls;
    return g_wrap_dy_result;
}

// ---- llm_strat_dir_from_to (0x0047ecc9) -------------------------------------------------------------
int32_t g_dir_calls  = 0;
int32_t g_dir_result = 0;
struct dir_call {
    int32_t x, y, gx, gy;
};
std::vector<dir_call> g_dir_args;
int32_t               rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    ++g_dir_calls;
    g_dir_args.push_back({x1, y1, x2, y2});
    return g_dir_result;
}

// ---- llm_ui_cursor_apply_anim_frame_offset (0x0047ee12, once per loop-2 soldier) --------------------
struct cursor_call {
    int8_t  wrote_x, wrote_y;
    int32_t facing;
};
std::vector<cursor_call> g_cursor_calls;
void                     rec_cursor_apply_anim_frame_offset(char *out_x, char *out_y, int32_t cursor_state_index) {
    tr("cursor_apply_anim_frame_offset");
    const int8_t vx = static_cast<int8_t>(10 + static_cast<int32_t>(g_cursor_calls.size()));
    const int8_t vy = static_cast<int8_t>(-(10 + static_cast<int32_t>(g_cursor_calls.size())));
    *out_x          = static_cast<char>(vx);
    *out_y          = static_cast<char>(vy);
    g_cursor_calls.push_back({vx, vy, cursor_state_index});
}

// ---- llm_strat_squad_pick_free_formation_anchor (0x0047ee54, once per loop-2 soldier) ---------------
struct pick_call {
    int32_t scratch_count;
    int8_t  wrote_end_x, wrote_end_y;
};
std::vector<pick_call> g_pick_calls;
void                   rec_squad_pick_free_formation_anchor(int32_t scratch_count, char *out_end_x, char *out_end_y) {
    tr("squad_pick_free_formation_anchor");
    const int8_t vx = static_cast<int8_t>(20 + static_cast<int32_t>(g_pick_calls.size()));
    const int8_t vy = static_cast<int8_t>(-(20 + static_cast<int32_t>(g_pick_calls.size())));
    *out_end_x      = static_cast<char>(vx);
    *out_end_y      = static_cast<char>(vy);
    g_pick_calls.push_back({scratch_count, vx, vy});
}

// ---- llm_strat_unit_teardown_mapped (0x0047ef4a) ----------------------------------------------------
struct teardown_call {
    uint32_t player, unit_index;
};
std::vector<teardown_call> g_teardown_calls;
// A case that needs to prove step 6's own_energy snapshot happens BEFORE this call while step 8's
// u.experience read happens AFTER it (LIVE) points these at the SELF unit; this mutates it as a side
// effect of the call itself -- see the header banner's step 6/8 ordering note.
unit   *g_teardown_touch_unit             = nullptr;
double  g_teardown_touch_energy_after     = 0.0;
int32_t g_teardown_touch_experience_after = 0;
void    rec_unit_teardown_mapped(uint32_t player, uint32_t unit_index) {
    tr("teardown_mapped");
    g_teardown_calls.push_back({player, unit_index});
    if (g_teardown_touch_unit != nullptr) {
        g_teardown_touch_unit->energy     = g_teardown_touch_energy_after;
        g_teardown_touch_unit->experience = g_teardown_touch_experience_after;
    }
}

// ---- llm_strat_unit_change_proto_and_energy (0x0047efa3) --------------------------------------------
struct change_call {
    uint16_t player;
    int32_t  unit_idx;
    int16_t  proto_delta;
    int32_t  unused;
    double   energy_delta;
};
std::vector<change_call> g_change_calls;
void                     rec_unit_change_proto_and_energy(uint16_t player, int32_t unit_idx, int16_t proto_delta, int32_t unused,
                                                          double energy_delta) {
    tr("unit_change_proto_and_energy");
    g_change_calls.push_back({player, unit_idx, proto_delta, unused, energy_delta});
}

// ---- llm_strat_unit_set_state (the shared BAIL sink, e.g. 0x0047ecbf) -------------------------------
std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

const unit_state_squad_merge_calls g_calls = {
    &rec_wrap_delta_x,
    &rec_wrap_delta_y,
    &rec_dir_from_to,
    &rec_cursor_apply_anim_frame_offset,
    &rec_squad_pick_free_formation_anchor,
    &rec_unit_teardown_mapped,
    &rec_unit_change_proto_and_energy,
    &rec_unit_set_state,
};

constexpr uint16_t PLAYER       = 3;
constexpr int32_t  UNIT_INDEX   = 9;
constexpr int32_t  TARGET_IDX   = 17;
constexpr int32_t  SELF_PROTO   = 30;
constexpr int32_t  TARGET_PROTO = 40;
constexpr int32_t  X            = 5;
constexpr int32_t  Y            = 7;
constexpr int32_t  GOAL_X       = 12;
constexpr int32_t  GOAL_Y       = 20;

void reset_recorders() {
    g_trace.clear();
    g_wrap_dx_calls = g_wrap_dy_calls = 0;
    g_wrap_dx_result = g_wrap_dy_result = 0;
    g_dir_calls                         = 0;
    g_dir_result                        = 0;
    g_dir_args.clear();
    g_cursor_calls.clear();
    g_pick_calls.clear();
    g_teardown_calls.clear();
    g_teardown_touch_unit             = nullptr;
    g_teardown_touch_energy_after     = 0.0;
    g_teardown_touch_experience_after = 0;
    g_change_calls.clear();
    g_set_state_calls.clear();
}

soldier &sold(sim_fixture &fx, int32_t index) {
    return fx.soldiers[(size_t)PLAYER * (size_t)SOLDIERS_PER_PLAYER + (size_t)index];
}

// Step 0/1 (0x0047ea7a-0x0047eb32): self_proto=30, cfgA=cfg_units[30].soldier_count=1,
// family_base=30-1+1=30, cfgB=cfg_units[30].soldier_type=55. The scan walks idx 31,32,33 -- 31 and 32
// extend the run (soldier_count 2,3 matching the ALREADY-INCREMENTED `expected`, soldier_type 55
// matching cfgB), 33 breaks it (soldier_count 999 != expected 4) -- family_count settles at 3.
//
// ---- THIS FIXTURE WAS BUILT AROUND A TRANSLATION BUG, and encoded it (2026-09-07) --------------
// It used to read soldier_count 1,1,2,999 across idx 30..33, which only makes family_count 3 if
// `expected` is compared BEFORE being incremented. The original increments first (INC [EBP-0x50] at
// 0x0047eb05, CMP at 0x0047eb1b reads the incremented slot), the header banner's step-1 derivation
// says so, and the C++ had drifted -- so the fixture was fitted to the drifted code and locked it
// in. Two consequences worth keeping in view:
//   * A cfg family is a run of consecutive protos whose soldier_count is 1,2,3,... -- the "1-man,
//     2-man, 3-man" variants of one soldier type. The old fixture's 1,1,2 is not a shape the game's
//     own data can have, which is the tell a fixture author has a predicate backwards: it had to
//     invent impossible data to make the run come out at 3.
//   * Nothing offline could catch it, because THIS is the offline oracle. It took the SPCAMP A/B/C
//     scenario -- a recorded human session where the player really merges soldiers into a squad --
//     and it showed up as the promoted body refusing every merge the original performed.
void configure_family_cfg(sim_fixture &fx) {
    fx.cfg_units[30].soldier_count = 1;
    fx.cfg_units[30].soldier_type  = 55;
    fx.cfg_units[31].soldier_count = 2;
    fx.cfg_units[31].soldier_type  = 55;
    fx.cfg_units[32].soldier_count = 3;
    fx.cfg_units[32].soldier_type  = 55;
    fx.cfg_units[33].soldier_count = 999; // breaks the scan
    fx.cfg_units[33].soldier_type  = 55;
}

// The ticking unit + the goal tile it's parked on, 0x0047eb32-0x0047eb7d.
unit &make_self_unit(sim_fixture &fx) {
    configure_family_cfg(fx);
    unit &u            = fx.u(PLAYER, UNIT_INDEX);
    u.unit_proto_id    = (uint16_t)SELF_PROTO;
    u.x                = (uint8_t)X;
    u.y                = (uint8_t)Y;
    u.goal_x           = (uint8_t)GOAL_X;
    u.goal_y           = (uint8_t)GOAL_Y;
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    tile_object &tile  = fx.t(GOAL_X, GOAL_Y);
    tile.class_owner   = 0x80u | (uint8_t)PLAYER; // owner==player AND bit 0x80 (a UNIT stands there)
    tile.building      = (uint16_t)TARGET_IDX;    // FIELD OVERLOAD -- .building holds a unit index here
    tile.unit[0] = tile.unit[1] = 0xEE;           // deliberately WRONG/unused -- proves .building, not
                                                  // .unit, is what the function reads (0x0047eb7d)
    return u;
}

// A target unit that passes every 3a-3d eligibility check by default -- each bail test overrides
// exactly ONE field below to fail exactly the check it targets.
unit &make_eligible_target(sim_fixture &fx) {
    unit &t                                  = fx.u(PLAYER, TARGET_IDX);
    t.unit_proto_id                          = (uint16_t)TARGET_PROTO;
    t.state                                  = UNIT_STATE_STOP_TO_DEFAULT; // 3b, 0x0047ebe9
    fx.cfg_units[TARGET_PROTO].soldier_count = 2;                          // 3a (>0) and 3c (2+cfgA(1)=3<=family_count(3))
    fx.cfg_units[TARGET_PROTO].soldier_type  = 55;                         // 3d, == cfgB
    return t;
}

} // namespace

void run_unit_state_squad_merge_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- goal tile's class_owner low nibble != cur_player: bail, 0x0047eb52-0x0047eb54 (JNZ
    // 0x0047eb6d -> the shared bail chain -> 0x0047ecba/ecbf unit_set_state(STOP_TO_DEFAULT)).
    // tick_budget is UNTOUCHED on every bail path (only the success path at 0x0047efb2 zeroes it).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        fx.t(GOAL_X, GOAL_Y).class_owner = 0x80u | (uint8_t)(PLAYER + 1); // wrong owner
        fx.tick_budget                   = 42.0;

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T1: wrong tile owner -- unit_set_state(STOP_TO_DEFAULT), 0x0047eb54/0x0047ecbf");
        ck_eq_d(fx.tick_budget, 42.0, "T1: tick_budget untouched on the bail path, 0x0047efb2 not reached");
        ck_eq((uint32_t)g_dir_calls, 0u, "T1: dir_from_to never called on this bail, 0x0047ecc9 not reached");
        ck_eq((uint32_t)(g_wrap_dx_calls + g_wrap_dy_calls), 0u,
              "T1: wrap_delta_x/y never called -- bails before 0x0047ec62");
        ck_eq((uint32_t)g_teardown_calls.size(), 0u, "T1: unit_teardown_mapped never called");
    }

    // =================================================================================================
    // T2 -- goal tile's class_owner bit 0x80 NOT set (a building/tree occupies it, not a unit): bail,
    // 0x0047eb64-0x0047eb6b (TEST/JNZ not taken) -> the same bail chain.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        fx.t(GOAL_X, GOAL_Y).class_owner = (uint8_t)PLAYER; // right owner, but no 0x80 bit
        fx.tick_budget                   = 7.0;

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T2: no unit standing on the goal tile -- unit_set_state(STOP_TO_DEFAULT), "
           "0x0047eb6b/0x0047ecbf");
        ck_eq_d(fx.tick_budget, 7.0, "T2: tick_budget untouched on the bail path");
    }

    // =================================================================================================
    // T3 -- target's cfg soldier_count <= 0 (not itself a "family" unit type): bail, 0x0047ebaa-0x0047ebb3.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        fx.cfg_units[TARGET_PROTO].soldier_count = 0; // 3a boundary: <=0 bails (the 0 side)

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T3: target cfg soldier_count<=0 -- unit_set_state(STOP_TO_DEFAULT), 0x0047ebb1/0x0047ecbf");
        ck_eq((uint32_t)g_dir_calls, 0u, "T3: never reaches dir_from_to");
    }

    // =================================================================================================
    // T4 -- target.state != UNIT_STATE_STOP_TO_DEFAULT (not idle): bail, 0x0047ebe1-0x0047ebe9.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        unit &t = make_eligible_target(fx);
        t.state = 5; // anything but STOP_TO_DEFAULT(1)

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T4: target not idle -- unit_set_state(STOP_TO_DEFAULT), 0x0047ebe9/0x0047ecbf");
    }

    // =================================================================================================
    // T5 -- combined crew (target soldier_count + cfgA) EXCEEDS family_count(3): bail, 0x0047ec16-
    // 0x0047ec1c (3 + cfgA(1) = 4 > 3). Boundary's BAIL side; T6/T8 exercise the <= (pass) side.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        fx.cfg_units[TARGET_PROTO].soldier_count = 3; // 3+1 = 4 > family_count(3)

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T5: combined crew exceeds family_count -- unit_set_state(STOP_TO_DEFAULT), "
           "0x0047ec1c/0x0047ecbf");
    }

    // =================================================================================================
    // T6 -- target soldier_type != self's soldier_type (different family): bail, 0x0047ec58-0x0047ec5e.
    // Passes 3a-3c (soldier_count=2, 2+1=3<=3) so ONLY 3d is under test here.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        fx.cfg_units[TARGET_PROTO].soldier_type = 777; // != cfg_units[SELF_PROTO].soldier_type (55)

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T6: soldier_type mismatch -- unit_set_state(STOP_TO_DEFAULT), 0x0047ec5e/0x0047ecbf");
        ck_eq((uint32_t)(g_wrap_dx_calls + g_wrap_dy_calls), 0u,
              "T6: never reaches the adjacency check, 0x0047ec62");
    }

    // =================================================================================================
    // T7 -- adjacency "near" pick, 0x0047ec86-0x0047ecb8: the asm computes
    // near = (dx<=dy) ? wrap_delta_y() : wrap_delta_x() (the SECOND, recomputed call -- JLE 0x0047ec88
    // -> LAB_0047eca0's wrap_delta_y call vs the fallthrough's wrap_delta_x call), i.e. near ==
    // max(dx,dy). Seeding dx=0 (small) and dy=5 (>1) with dx<=dy true means the CORRECT near is 5 (dy)
    // -> BAILS (0x0047ecb8 JLE not taken -> 0x0047ecba unit_set_state(1)).
    //
    // *** sim_unit_state_squad_merge.cpp currently computes near = (dx<=dy) ? wrap_delta_x() :
    // wrap_delta_y() -- the OPPOSITE selection (== min(dx,dy) == 0 here) -- so it does NOT bail and
    // instead falls through into step 4 onward. This case is written to the asm and is EXPECTED TO
    // FAIL against the current .cpp; see this file's header banner. Not weakened to match the .cpp. ***
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        g_wrap_dx_result = 0; // dx
        g_wrap_dy_result = 5; // dy, > 1

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T7 [ASM]: near = max(dx,dy) = dy(5) > 1 -- unit_set_state(STOP_TO_DEFAULT), "
           "0x0047ecb8/0x0047ecbf -- DIVERGES from current .cpp, see header banner");
        ck_eq((uint32_t)g_wrap_dx_calls, 1u,
              "T7 [ASM]: wrap_delta_x called ONCE (only the first, dx-computing call), 0x0047ec6e");
        ck_eq((uint32_t)g_wrap_dy_calls, 2u,
              "T7 [ASM]: wrap_delta_y called TWICE (dy, then again as the chosen 'near'), 0x0047ec81 "
              "and 0x0047ecac -- DIVERGES from current .cpp, see header banner");
        ck_eq((uint32_t)g_dir_calls, 0u, "T7: bails before step 4 -- dir_from_to never called");
    }

    // =================================================================================================
    // T8 -- full success path: adjacency passes (near=1, dx==dy=1 so both branches agree -- the order
    // dispute T7 pins is deliberately neutralized here so this case tests everything ELSE); the
    // CORRECTED facing_target write/read-back (0x0047ece1-0x0047ece7, NOT facing_current); the
    // target's pre-existing 2-node crew chain walked and snapshotted into the shared anchor scratch
    // (0x0047ed14-0x0047ed8d); the graft onto the chain's TAIL (0x0047ed8f-0x0047ed94); the energy
    // snapshot taken BEFORE unit_teardown_mapped vs the experience read taken LIVE (AFTER it,
    // 0x0047ef3c-0x0047ef8d -- see the header banner's step 6/8 ordering note); this unit's own
    // 2-node crew walked in loop 2, each soldier getting a cursor_apply_anim_frame_offset THEN a
    // squad_pick_free_formation_anchor call with the anchor_count carried over from loop 1 and
    // incrementing per soldier (0x0047edd5-0x0047ef36, proves ORDER); and the final
    // unit_change_proto_and_energy(merged_count=2)/tick_budget=0.0 finish (0x0047ef3c-0x0047efbc).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u = make_self_unit(fx);
        unit &t = make_eligible_target(fx);

        g_wrap_dx_result = 1;
        g_wrap_dy_result = 1; // dx==dy -- near is 1 either way, regardless of the T7 order dispute

        // facing: seed CURRENT and TARGET to DIFFERENT values so a regression back to facing_current
        // (the bug reimpl-verify already fixed once) is visible.
        u.facing_current = 9;
        u.facing_target  = 3;
        g_dir_result     = 15;

        // target's existing 2-node crew chain: head=50 -> 51 -> end.
        t.unit_above[0]           = 50;
        t.unit_above[1]           = 0;
        sold(fx, 50).next_soldier = 51;
        sold(fx, 50).end_x        = 11;
        sold(fx, 50).end_y        = -12;
        sold(fx, 51).next_soldier = 0;
        sold(fx, 51).end_x        = 13;
        sold(fx, 51).end_y        = -14;

        // this unit's own 2-node crew chain: head=60 -> 61 -> end.
        u.unit_above[0]           = 60;
        u.unit_above[1]           = 0;
        sold(fx, 60).next_soldier = 61;
        sold(fx, 61).next_soldier = 0;
        // cur_x/cur_y PRE-SEEDED to junk that is neither 0 nor the start_* the stubs will write, so
        // "cur == start afterwards" is a real observation rather than two zeroes agreeing. These pin
        // 0x0047eecb/0x0047eed1 and 0x0047eefd/0x0047ef03 -- the two copies loop 2's translation
        // omitted until 2026-09-07.
        sold(fx, 60).cur_x = 101;
        sold(fx, 60).cur_y = 102;
        sold(fx, 61).cur_x = 103;
        sold(fx, 61).cur_y = 104;

        // energy/experience ordering: teardown_mapped mutates the SELF unit as a side effect.
        u.energy                          = 77.5;
        u.experience                      = 8;
        t.experience                      = 1000;
        g_teardown_touch_unit             = &u;
        g_teardown_touch_energy_after     = 999.5; // must NOT leak into own_energy (snapshotted early)
        g_teardown_touch_experience_after = 555;   // MUST leak into target.experience (read live)

        fx.tick_budget = 321.0;

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 0u, "T8: success path -- unit_set_state never called");

        // ---- facing (0x0047ecc9-0x0047ed11) ----
        ck_eq((uint32_t)g_dir_calls, 1u, "T8: dir_from_to called once, 0x0047ecd5");
        ck(g_dir_args.size() == 1 && g_dir_args[0].x == X && g_dir_args[0].y == Y &&
               g_dir_args[0].gx == GOAL_X && g_dir_args[0].gy == GOAL_Y,
           "T8: dir_from_to(x,y,goal_x,goal_y), 0x0047ecc9-0x0047ecd5");
        ck_eq((uint32_t)u.facing_target, 15u,
              "T8 [CORRECTED]: facing_target (+0x2c) set from dir_from_to's result, 0x0047ece1");
        ck_eq((uint32_t)u.facing_current, 9u,
              "T8 [CORRECTED]: facing_current (+0x2d) UNCHANGED -- the fixed facing_current/"
              "facing_target swap bug would fail this");

        // ---- loop 1: target's existing chain snapshotted, tail tracked (0x0047ed14-0x0047ed8d) ----
        ck_eq((uint32_t)own.squad_anchor_scratch_at(0).x, (uint32_t)11,
              "T8: scratch[0] = soldier[50].end_x, 0x0047ed27");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(0).y, (uint32_t)-12,
              "T8: scratch[0].y = soldier[50].end_y sign-extended, 0x0047ed4d");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(1).x, (uint32_t)13,
              "T8: scratch[1] = soldier[51].end_x, second node of the target's chain");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(1).y, (uint32_t)-14, "T8: scratch[1].y");

        // ---- step 6: graft onto the TAIL (51, not the head 50); own_energy snapshotted here ----
        ck_eq((uint32_t)sold(fx, 51).next_soldier, 60u,
              "T8: soldiers[51].next_soldier = this unit's own chain head (60), 0x0047ed8f -- grafted "
              "onto the TAIL, not the head");
        ck_eq((uint32_t)sold(fx, 50).next_soldier, 51u,
              "T8: soldiers[50].next_soldier untouched by the graft (only the tail is rewritten)");

        // ---- loop 2: this unit's own (now-grafted) crew, cursor_apply THEN squad_pick per soldier,
        // anchor_count carried over from loop 1 (2) and incrementing (0x0047edd5-0x0047ef36) ----
        ck_eq((uint32_t)g_cursor_calls.size(), 2u, "T8: cursor_apply_anim_frame_offset called twice");
        ck_eq((uint32_t)g_pick_calls.size(), 2u, "T8: squad_pick_free_formation_anchor called twice");
        ck(g_cursor_calls.size() == 2 && g_cursor_calls[0].facing == 15 && g_cursor_calls[1].facing == 15,
           "T8: both cursor_apply calls get own_facing == facing_target's NEW value (15), not the old "
           "facing_current (9)");
        ck(g_pick_calls.size() == 2 && g_pick_calls[0].scratch_count == 2 &&
               g_pick_calls[1].scratch_count == 3,
           "T8: squad_pick_free_formation_anchor's scratch_count carries over from loop 1 (2) then "
           "increments per soldier (3) -- ORDER visible, 0x0047ee51-0x0047ee54");
        ck_eq((uint32_t)sold(fx, 60).start_x, (uint32_t)10,
              "T8: cursor_apply wrote into soldier[60]'s OWN start_x -- pointer identity, 0x0047ee12");
        ck_eq((uint32_t)sold(fx, 60).start_y, (uint32_t)-10, "T8: soldier[60].start_y");
        ck_eq((uint32_t)sold(fx, 61).start_x, (uint32_t)11,
              "T8: cursor_apply wrote into soldier[61]'s OWN start_x, second iteration");
        ck_eq((uint32_t)sold(fx, 61).start_y, (uint32_t)-11, "T8: soldier[61].start_y");
        ck_eq((uint32_t)sold(fx, 60).end_x, (uint32_t)20,
              "T8: squad_pick wrote into soldier[60]'s OWN end_x -- pointer identity, 0x0047ee54");
        ck_eq((uint32_t)sold(fx, 60).end_y, (uint32_t)-20, "T8: soldier[60].end_y");
        ck_eq((uint32_t)sold(fx, 61).end_x, (uint32_t)21, "T8: soldier[61].end_x, second iteration");
        ck_eq((uint32_t)sold(fx, 61).end_y, (uint32_t)-21, "T8: soldier[61].end_y");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(2).x, (uint32_t)20,
              "T8: scratch[2] re-read from the just-picked end_x, 0x0047ee6c-0x0047ee79");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(3).x, (uint32_t)21, "T8: scratch[3], second soldier");
        // The two cur_* copies at the END of each loop-2 iteration. Seeded to 101..104 above, so a
        // body that omits them (as this translation did) leaves the seeds and fails here.
        ck_eq((uint32_t)sold(fx, 60).cur_x, (uint32_t)10,
              "T8: soldier[60].cur_x = start_x (+0x7 -> +0x4), 0x0047eecb/0x0047eed1");
        ck_eq((uint32_t)sold(fx, 60).cur_y, (uint32_t)-10,
              "T8: soldier[60].cur_y = start_y (+0x8 -> +0x5), 0x0047eefd/0x0047ef03");
        ck_eq((uint32_t)sold(fx, 61).cur_x, (uint32_t)11, "T8: soldier[61].cur_x = start_x, 2nd iteration");
        ck_eq((uint32_t)sold(fx, 61).cur_y, (uint32_t)-11, "T8: soldier[61].cur_y = start_y, 2nd iteration");

        // ---- step 8: teardown BEFORE the live experience read; energy snapshotted BEFORE teardown ----
        ck(g_teardown_calls.size() == 1 && g_teardown_calls[0].player == PLAYER &&
               g_teardown_calls[0].unit_index == (uint32_t)UNIT_INDEX,
           "T8: unit_teardown_mapped(player, unit_index) called once, 0x0047ef4a");
        ck_eq((uint32_t)t.experience, 1555u,
              "T8 [ORDERING]: target.experience = 1000 + u.experience READ LIVE AFTER teardown (555, "
              "not the pre-teardown 8) -- 0x0047ef87 reads AFTER 0x0047ef4a");
        ck(g_change_calls.size() == 1 && g_change_calls[0].player == PLAYER &&
               g_change_calls[0].unit_idx == TARGET_IDX && g_change_calls[0].proto_delta == 2 &&
               g_change_calls[0].energy_delta == 77.5,
           "T8 [ORDERING]: unit_change_proto_and_energy(player, target_idx, merged_count=2, _, "
           "own_energy) -- own_energy is the PRE-teardown snapshot (77.5, not the post-teardown "
           "999.5), 0x0047ed8f-0x0047ef93 snapshots BEFORE 0x0047ef4a");
        ck_eq_d(fx.tick_budget, 0.0, "T8: tick_budget=0.0 -- success path only, 0x0047efa8-0x0047efb2");

        ck(trace_eq({"dir_from_to", "cursor_apply_anim_frame_offset", "squad_pick_free_formation_anchor",
                     "cursor_apply_anim_frame_offset", "squad_pick_free_formation_anchor",
                     "teardown_mapped", "unit_change_proto_and_energy"}),
           "T8 [ORDER]: dir_from_to, then per-soldier (cursor_apply, squad_pick) x2, then "
           "teardown_mapped, then unit_change_proto_and_energy -- the whole call sequence");
    }

    // =================================================================================================
    // T9 -- loop 1's DO-WHILE quirk: target.unit_above == 0 (chain "empty"), so the loop still runs
    // ONCE and reads soldier slot [0] -- the per-player used-slot-count SENTINEL record -- as if it
    // were real crew data (0x0047ed14-0x0047ed8d always runs at least once). tail ends up 0, so the
    // graft rewrites the SENTINEL record's own next_soldier.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u = make_self_unit(fx);
        unit &t = make_eligible_target(fx);

        g_wrap_dx_result = 1;
        g_wrap_dy_result = 1;
        g_dir_result     = 4;

        t.unit_above[0]   = 0;
        t.unit_above[1]   = 0; // chain == 0
        sold(fx, 0).end_x = 44;
        sold(fx, 0).end_y = -45; // slot [0]'s "data", read anyway per the quirk

        u.unit_above[0] = 70;
        u.unit_above[1] = 0; // this unit's own (real) single-node chain

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 0u, "T9: success path -- no bail");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(0).x, (uint32_t)44,
              "T9 [QUIRK]: loop 1 reads soldier slot [0] as data on an empty chain, 0x0047ed14 runs once");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(0).y, (uint32_t)-45, "T9: scratch[0].y from slot [0]");
        ck_eq((uint32_t)sold(fx, 0).next_soldier, 70u,
              "T9 [QUIRK]: the graft rewrites slot [0]'s OWN next_soldier (tail==0), 0x0047ed8f");
        ck(g_pick_calls.size() == 1 && g_pick_calls[0].scratch_count == 1,
           "T9: loop 2's squad_pick_free_formation_anchor gets anchor_count==1, carried from loop 1's "
           "single (quirk) contribution");
        ck(g_change_calls.size() == 1 && g_change_calls[0].proto_delta == 1,
           "T9: merged_count==1 (this unit's own chain had exactly one real node)");
    }

    // =================================================================================================
    // T10 -- loop 2's DO-WHILE quirk: this unit's own unit_above == 0 (own chain "empty"), so loop 2
    // still runs ONCE on soldier slot [0] (0x0047edd5-0x0047ef36 always runs at least once). The graft
    // (step 6) writes the literal word 0 (this unit's own empty unit_above) into the TARGET's real
    // chain tail's next_soldier.
    //
    // FIXED (build-time, 2026-08-21): the first draft pre-seeded the tail node's OWN next_soldier field
    // with a nonzero "sentinel" (55) to prove the graft overwrites it -- but that exact field is what
    // loop 1's own DO-WHILE reads to decide whether to keep walking (`chain = soldier[chain].
    // next_soldier; while (chain != 0)`), so seeding it nonzero silently turned the "real single-node
    // chain" into a 2-node chain (80->55->...), moving `tail` to 55 and making every downstream
    // assertion check the wrong soldier record -- 5 failures, not a production bug. A node's own
    // next_soldier field cannot simultaneously be "the loop's real terminator (0)" and "a nonzero
    // sentinel to detect an overwrite" -- they are the same field. Fixed the way T8 already does it
    // (0x0047ed8f-94, "graft onto the TAIL, not the head" + "the non-tail node is untouched"): a REAL
    // two-node target chain (80->81, 81 terminates with next_soldier=0) so `tail` resolves to 81, then
    // assert node 80's own link (81) is UNCHANGED (proves the graft touches only the tail) alongside
    // node 81's next_soldier reading 0 (the graft's actual write, weak alone since 0 was also the
    // fixture's default, but meaningful paired with the untouched-node check).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u = make_self_unit(fx);
        unit &t = make_eligible_target(fx);

        g_wrap_dx_result = 1;
        g_wrap_dy_result = 1;
        g_dir_result     = 4;

        t.unit_above[0]           = 80;
        t.unit_above[1]           = 0;  // target's real TWO-node chain: 80 -> 81 -> end
        sold(fx, 80).next_soldier = 81; // a real link, not a sentinel -- loop 1 must keep walking
        sold(fx, 80).end_x        = 33;
        sold(fx, 80).end_y        = -34;
        sold(fx, 81).next_soldier = 0; // the REAL terminator -- tail resolves here
        sold(fx, 81).end_x        = 17;
        sold(fx, 81).end_y        = -18;

        u.unit_above[0] = 0;
        u.unit_above[1] = 0; // this unit's own chain == 0

        sim_store own = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 0u, "T10: success path -- no bail");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(0).x, (uint32_t)33,
              "T10: loop 1 walks the target's real chain, scratch[0] = soldier[80].end_x");
        ck_eq((uint32_t)own.squad_anchor_scratch_at(1).x, (uint32_t)17,
              "T10: loop 1 continues past 80 to soldier[81].end_x, second (tail) node");
        ck_eq((uint32_t)sold(fx, 80).next_soldier, 81u,
              "T10: soldiers[80].next_soldier UNCHANGED -- the graft rewrites only the tail (81), not "
              "the whole chain (same pattern as T8's identical check)");
        ck_eq((uint32_t)sold(fx, 81).next_soldier, 0u,
              "T10 [QUIRK]: graft writes the literal word 0 (this unit's own EMPTY unit_above) into "
              "the target's real chain tail (81), 0x0047ed8f");
        ck(g_pick_calls.size() == 1 && g_pick_calls[0].scratch_count == 2,
           "T10 [QUIRK]: loop 2 still runs once on soldier slot [0] despite this unit's chain being "
           "empty, 0x0047edd5 runs at least once -- anchor_count carried from loop 1's real 2-node walk");
        ck_eq((uint32_t)sold(fx, 0).start_x, (uint32_t)10,
              "T10 [QUIRK]: cursor_apply_anim_frame_offset wrote into slot [0]'s OWN start_x -- proves "
              "loop 2 really operated on soldier [0], not skipped");
        ck_eq((uint32_t)sold(fx, 0).start_y, (uint32_t)-10, "T10: soldier[0].start_y");
        ck_eq((uint32_t)sold(fx, 0).end_x, (uint32_t)20, "T10: soldier[0].end_x from squad_pick");
        ck_eq((uint32_t)sold(fx, 0).end_y, (uint32_t)-20, "T10: soldier[0].end_y");
        ck(g_change_calls.size() == 1 && g_change_calls[0].proto_delta == 1,
           "T10: merged_count==1 (the quirk-processed slot [0] counts as one 'soldier')");
    }

    // =================================================================================================
    // T11 -- THE FAMILY-SCAN PREDICATE, pinned in BOTH directions (added 2026-09-07).
    //
    // Every other case here reads family_count only through step 3c's inequality, so a scan that is
    // off by one is invisible as long as the fixture's numbers happen to come out the same -- which is
    // exactly how the bug survived: configure_family_cfg had been fitted to the wrong predicate, and
    // then every test that used it agreed with it. This case measures family_count DIRECTLY, by
    // walking the 3c boundary, and does it for two cfg shapes that differ ONLY in whether `expected`
    // is compared before or after its increment.
    //
    // The scan advances while cfg_units[base+k].soldier_count == k+1 (0x0047eb05 INCs, 0x0047eb1b
    // compares the incremented value). configure_family_cfg gives 1,2,3,999 at idx 30..33, so
    // family_count == 3.
    //   * target soldier_count 2 -> 2 + cfgA(1) = 3 <= 3  -> SUCCESS
    //   * target soldier_count 3 -> 3 + cfgA(1) = 4 >  3  -> BAIL
    // Under the off-by-one the same fixture gives family_count == 1 and BOTH bail, so the pair of
    // cases below cannot both pass unless the predicate is right.
    // =================================================================================================
    {
        // The <= side: family_count must be at least 3 for this to reach the success path at all.
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        g_wrap_dx_result                         = 1;
        g_wrap_dy_result                         = 1;
        fx.cfg_units[TARGET_PROTO].soldier_count = 2; // 2 + cfgA(1) = 3 == family_count(3)
        sim_store own                            = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);
        ck(g_set_state_calls.size() == 0u,
           "T11: family_count==3 (scan 1,2,3,999) -- combined crew 3 <= 3 reaches the success path. "
           "An off-by-one `expected` makes family_count 1 and this bails, 0x0047eafc-0x0047eb30");
    }
    {
        // The > side, one apart: proves the number really is 3 and not merely 'large enough'.
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        g_wrap_dx_result                         = 1;
        g_wrap_dy_result                         = 1;
        fx.cfg_units[TARGET_PROTO].soldier_count = 3; // 3 + cfgA(1) = 4 > family_count(3)
        sim_store own                            = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);
        ck(g_set_state_calls.size() == 1u && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T11: combined crew 4 > family_count(3) BAILS -- family_count is exactly 3, not merely "
           ">= 3, 0x0047ec19-0x0047ec1c");
    }
    {
        // THE SHAPE THE OLD FIXTURE USED, now a negative: a run of 1,1,2 is not a family under the
        // real predicate -- cfg_units[31].soldier_count(1) != expected(2) breaks the scan on its FIRST
        // iteration, family_count == 1, and every merge bails. If someone re-introduces the off-by-one
        // this case is the one that goes green when it should be red.
        fx.reset();
        reset_recorders();
        make_self_unit(fx);
        make_eligible_target(fx);
        g_wrap_dx_result                         = 1;
        g_wrap_dy_result                         = 1;
        fx.cfg_units[31].soldier_count           = 1; // the impossible 1,1,2 shape
        fx.cfg_units[32].soldier_count           = 2;
        fx.cfg_units[TARGET_PROTO].soldier_count = 2; // 2 + cfgA(1) = 3 > family_count(1)
        sim_store own                            = fx.store();
        detail::unit_state_squad_merge(fx.view(), own, g_calls);
        ck(g_set_state_calls.size() == 1u && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "T11 [NEGATIVE]: a 1,1,2 cfg run breaks the scan immediately (family_count==1) and the "
           "merge bails -- the off-by-one would score this run 3 and succeed");
    }
}

} // namespace mh::sim::test
