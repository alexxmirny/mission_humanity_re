//
// sim_unit_state_group_step_selftest.cpp -- `simtest` cases for llm_strat_unit_state_group_step
// @0x00482f7f (sim/sim_unit_state_group_step.h/.cpp).
//
// SCOPE: this function is a PURE two-way DISPATCHER with NO WRITE of its own (per the .h banner) --
// it reads units[cur_player][cur_index].unit_proto_id, looks up cfg_units[proto].type, and calls
// exactly one of two ORIGINAL __watcall(void) callees:
//   type == A_PLANE(0x11) || type == H_PLANE(0x12)  -> group_step_plane()
//   otherwise                                        -> group_step_ground()
// This oracle covers: the A_PLANE exact-match arm (first CMP/JZ), the H_PLANE arm (falls through the
// first compare and takes the second compare's JNZ-NOT-taken path to the SAME plane call target --
// i.e. it does NOT independently re-derive a third branch, it reuses LAB_00482ffb), the boundary
// just BELOW A_PLANE (0x10) and just ABOVE H_PLANE (0x13) both landing on the ground arm, a generic
// non-plane ground type, and that the roster-arithmetic (cur_player/cur_index -> unit_proto_id ->
// cfg_units[proto].type) is read from the CURRENT unit only -- a differently-typed unit at another
// roster slot does not influence the dispatch. Exactly one callee fires per case, with a call-count
// assertion on both sides so a translation that fired both (or neither) is caught.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_state_group_step_00482f7f.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
//   0x00482f97-0x00482fb3: EAX = units[cur_player][cur_index].unit_proto_id (first read, row/col
//     arithmetic: cur_player*0x5b04 + cur_index*0xe9, then MOVZX word at +0xdd8c4a).
//   0x00482fba-0x00482fc0: EAX *= 0x23f (sizeof(cfg_unit)); CMP [EAX+0xe4a176] (Unit[proto].type), 0x11.
//   0x00482fc7: JZ 0x00482ffb (LAB_00482ffb, the plane call) -- taken when type == A_PLANE(0x11).
//   0x00482fc9-0x00482fe5: NOT taken -- cur_player/cur_index/unit_proto_id are RE-DERIVED from
//     scratch (byte-identical second row/col computation, the asm does not cache EAX across the
//     first check).
//   0x00482fec-0x00482ff2: EAX *= 0x23f again; CMP [EAX+0xe4a176] (Unit[proto].type), 0x12.
//   0x00482ff9: JNZ 0x00483002 (LAB_00483002, the ground call) -- taken when type != H_PLANE(0x12).
//     NOT taken (i.e. type == H_PLANE) falls through into LAB_00482ffb -- the SAME plane-call target
//     the first branch uses, not a third call site.
//   LAB_00482ffb (0x00482ffb): CALL 0x00484b4a (llm_strat_unit_group_step_plane); JMP epilogue.
//   LAB_00483002 (0x00483002): CALL 0x00483011 (llm_strat_unit_group_step_ground); falls into epilogue.
//
#include "sim/sim_unit_state_group_step.h"

#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves exactly one callee fired, and which one -------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- the two callees, recorded (both are void(void) -- nothing to capture but the call itself) --
int  g_plane_calls = 0;
void rec_group_step_plane() {
    tr("group_step_plane");
    ++g_plane_calls;
}

int  g_ground_calls = 0;
void rec_group_step_ground() {
    tr("group_step_ground");
    ++g_ground_calls;
}

const unit_state_group_step_calls g_calls = {
    &rec_group_step_plane,
    &rec_group_step_ground,
};

void reset_observations() {
    g_trace.clear();
    g_plane_calls  = 0;
    g_ground_calls = 0;
}

// Fixed "guard" slot no test's own (player,index) ever touches -- carries the OPPOSITE-arm type from
// whatever the case under test is exercising, at a DIFFERENT cfg row, so a dispatch that accidentally
// reads the wrong unit/cfg row would flip the observed arm. Kept outside every case's own
// player(0..3)/index(0..4)/cfg_row ranges below.
constexpr uint16_t GUARD_PLAYER  = 6;
constexpr int32_t  GUARD_INDEX   = 9;
constexpr uint16_t GUARD_CFG_ROW = 77;

void seed_guard_slot(sim_fixture &fx) {
    unit &g                          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id                  = GUARD_CFG_ROW;
    fx.cfg_units[GUARD_CFG_ROW].type = 0x1234; // neither A_PLANE nor H_PLANE nor any test's own type
}

// ---- fixture seeding ------------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10; // unit_proto_id -- distinct per case so the roster->cfg lookup is exercised
    uint32_t type    = 0;  // cfg_units[cfg_row].type -- the field the dispatch branches on
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u         = fx.u(s.player, s.index);
    u.unit_proto_id = s.cfg_row;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    fx.cfg_units[s.cfg_row].type = s.type;

    reset_observations();

    detail::unit_state_group_step(fx.view(), g_calls);
}

} // namespace

void run_unit_state_group_step_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- type == A_PLANE(0x11) exactly: first CMP/JZ taken (0x00482fc0 CMP / 0x00482fc7 JZ) ->
    // straight to LAB_00482ffb, the plane call. Ground must NOT fire.
    // =================================================================================================
    {
        Seed s;
        s.player  = 0;
        s.index   = 1;
        s.cfg_row = 10;
        s.type    = UNIT_TYPE_A_PLANE; // 0x11
        seed_and_run(fx, s);

        ck(g_plane_calls == 1, "T1: type==A_PLANE(0x11) -- group_step_plane fires exactly once (0x00482fc7 JZ taken)");
        ck(g_ground_calls == 0, "T1: type==A_PLANE -- group_step_ground does NOT fire");
        ck(trace_eq({"group_step_plane"}), "T1: exactly one call, and it is the plane call");
    }

    // =================================================================================================
    // T2 -- type == H_PLANE(0x12): first compare fails (type != 0x11), so the asm RE-DERIVES
    // cur_player/cur_index/unit_proto_id from scratch (0x00482fc9-0x00482fe5, byte-identical to the
    // first read) and re-reads Unit[proto].type; second CMP (0x00482ff2) against 0x12, JNZ
    // (0x00482ff9) is NOT taken because type == 0x12, so control falls through into LAB_00482ffb --
    // the SAME plane-call target as T1, not a distinct third call site. Ground must NOT fire.
    // =================================================================================================
    {
        Seed s;
        s.player  = 1;
        s.index   = 2;
        s.cfg_row = 11;                // distinct row from T1, proving the lookup is not hardcoded to row 10
        s.type    = UNIT_TYPE_H_PLANE; // 0x12
        seed_and_run(fx, s);

        ck(g_plane_calls == 1,
           "T2: type==H_PLANE(0x12) -- group_step_plane fires exactly once (0x00482ff9 JNZ NOT taken, "
           "falls through to LAB_00482ffb, the same plane-call target the A_PLANE arm uses)");
        ck(g_ground_calls == 0, "T2: type==H_PLANE -- group_step_ground does NOT fire");
        ck(trace_eq({"group_step_plane"}), "T2: exactly one call, and it is the plane call");
    }

    // =================================================================================================
    // T3 -- type == 0x10, just BELOW A_PLANE: first compare fails, second compare (against H_PLANE
    // 0x12) also fails since 0x10 != 0x12, so JNZ (0x00482ff9) IS taken -> LAB_00483002, the ground
    // call. Plane must NOT fire.
    // =================================================================================================
    {
        Seed s;
        s.player  = 2;
        s.index   = 3;
        s.cfg_row = 12;
        s.type    = 0x10; // one below UNIT_TYPE_A_PLANE
        seed_and_run(fx, s);

        ck(g_ground_calls == 1,
           "T3: type==0x10 (one below A_PLANE) -- group_step_ground fires exactly once (0x00482fc7 JZ "
           "not taken, 0x00482ff9 JNZ taken to LAB_00483002)");
        ck(g_plane_calls == 0, "T3: type==0x10 -- group_step_plane does NOT fire");
        ck(trace_eq({"group_step_ground"}), "T3: exactly one call, and it is the ground call");
    }

    // =================================================================================================
    // T4 -- type == 0x13, just ABOVE H_PLANE: both compares fail (0x13 != 0x11, 0x13 != 0x12) -> the
    // ground arm. Confirms the dispatch is an exact two-value match, not a range test.
    // =================================================================================================
    {
        Seed s;
        s.player  = 3;
        s.index   = 4;
        s.cfg_row = 13;
        s.type    = 0x13; // one above UNIT_TYPE_H_PLANE
        seed_and_run(fx, s);

        ck(g_ground_calls == 1,
           "T4: type==0x13 (one above H_PLANE) -- group_step_ground fires exactly once (both CMPs "
           "fail -- an exact two-value match, not a <= range test)");
        ck(g_plane_calls == 0, "T4: type==0x13 -- group_step_plane does NOT fire");
        ck(trace_eq({"group_step_ground"}), "T4: exactly one call, and it is the ground call");
    }

    // =================================================================================================
    // T5 -- a generic non-plane ground type (unrelated to the 0x11/0x12 boundary), at a roster slot
    // and cfg row distinct from every prior case, PLUS the guard unit/cfg-row pair (a different
    // player/index/proto entirely, seeded with a type that would take the OPPOSITE arm) proving the
    // dispatch reads only the CURRENT unit's own proto -- a stray cross-read of the guard's row would
    // flip this result to the plane arm.
    // =================================================================================================
    {
        Seed s;
        s.player  = 0;
        s.index   = 4;  // distinct (player, index) from every case above
        s.cfg_row = 55; // distinct cfg row, far from GUARD_CFG_ROW(77) and every case above
        s.type    = 5;  // an ordinary ground-unit type, nowhere near the 0x11/0x12 boundary
        seed_and_run(fx, s);

        ck(g_ground_calls == 1, "T5: an ordinary non-plane type -- group_step_ground fires exactly once");
        ck(g_plane_calls == 0, "T5: an ordinary non-plane type -- group_step_plane does NOT fire");
        ck(trace_eq({"group_step_ground"}),
           "T5: exactly one call, and it is the ground call -- unaffected by the guard slot's "
           "opposite-arm cfg row (GUARD_CFG_ROW=77, type=0x1234) sitting elsewhere in cfg_units");

        // The guard unit's own fields are untouched -- this function writes nothing (per the .h
        // banner: "PURE DISPATCHER, NO WRITE"), so a corruption here would mean a translation added
        // a write the original does not have.
        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == GUARD_CFG_ROW, "T5: guard unit's unit_proto_id untouched (function writes nothing)");
        ck((uint32_t)fx.cfg_units[GUARD_CFG_ROW].type == 0x1234u, "T5: guard cfg row's type untouched");

        // The acted-on unit's own proto id is likewise untouched (nothing in this function writes it).
        ck((uint32_t)fx.u(s.player, s.index).unit_proto_id == s.cfg_row,
           "T5: the acted-on unit's own unit_proto_id is untouched post-call");
    }
}

} // namespace mh::sim::test
