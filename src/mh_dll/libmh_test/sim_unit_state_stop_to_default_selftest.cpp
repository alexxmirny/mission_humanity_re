//
// sim_unit_state_stop_to_default_selftest.cpp -- `simtest` cases for
// llm_strat_unit_state_stop_to_default @0x0047e1e1 (sim/sim_unit_state_stop_to_default.h/.cpp,
// SIM1-G1).
//
// SCOPE: the path_slot_id!=0xff gate (0x0047e1fe CMP / 0x0047e205 JZ) that guards
// llm_strat_path_free_slot(cur_player, cur_index) -- both sides, exact args; the
// `cfg_units[proto].default_op_code != cur_unit->state` gate (0x0047e239 CMP / 0x0047e23b JZ) that
// guards llm_strat_unit_set_state(default_op_code) -- both sides, exact arg (the PROTO's
// default_op_code, not the unit's own current state -- the two reads are of different fields and a
// translation that fed `state` into unit_set_state instead would still "look" plausible); the
// unconditional tick_budget zero-store (0x0047e258/0x0047e262, both dwords) across every combination
// of the two gates; the exact CALL ORDER when both fire (path_free_slot before unit_set_state,
// matching the asm's straight-line block layout -- the path_slot_id block at 0x0047e1fe precedes the
// default_op_code block at 0x0047e21a); and non-corruption -- this function writes NOTHING on
// cur_unit itself (only own.tick_budget()), so the unit's own path_slot_id/state/unit_proto_id/order
// fields must read back exactly as seeded (the real writes belong to the two ORIGINAL callees, which
// are stubbed no-ops here), and a neighbouring roster slot this call never addresses stays untouched.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp/llm_strat_unit_state_stop_to_default_0047e1e1.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_stop_to_default.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across both unit_state_stop_to_default_calls members --------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (2, one per unit_state_stop_to_default_calls member) --------------------
struct PathFreeCall {
    uint16_t player;
    int32_t  index;
};
std::vector<PathFreeCall> g_path_free_calls;
void                      rec_path_free_slot(uint16_t player, int32_t unit_index) {
    tr("path_free_slot");
    g_path_free_calls.push_back({player, unit_index});
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

const unit_state_stop_to_default_calls g_calls = {
    &rec_path_free_slot,
    &rec_unit_set_state,
};

void reset_observations() {
    g_trace.clear();
    g_path_free_calls.clear();
    g_set_state_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// each run so a wrong-index write (there should be none from THIS function, but the stubbed callees
// are the honest check for "did the fixture wiring accidentally alias two slots") lands somewhere
// observable.
constexpr uint16_t GUARD_PLAYER = 6;
constexpr int32_t  GUARD_INDEX  = 9;

void seed_guard_slot(sim_fixture &fx) {
    unit &g         = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id = 88;
    g.state         = 0x33;
    g.path_slot_id  = 0x44;
    g.order         = 0x55;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    uint8_t  path_slot_id    = 0xff; // 0xff == none
    uint16_t state           = 3;    // cur_unit->state
    uint8_t  default_op_code = 3;    // cfg_units[proto].default_op_code -- equal to `state` by
                                     // default so a case that only wants to isolate the FIRST gate
                                     // doesn't have to also fight the second one open.

    double tick_budget = 12.5; // nonzero seed -- must read back 0.0 after every run
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u         = fx.u(s.player, s.index);
    u.unit_proto_id = s.cfg_row;
    u.path_slot_id  = s.path_slot_id;
    u.state         = s.state;
    u.order         = 0x77; // sentinel -- this function never touches Order; must read back unchanged

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    cfg_unit &cu       = fx.cfg_units[s.cfg_row];
    cu.default_op_code = s.default_op_code;

    fx.tick_budget = s.tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_stop_to_default(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_stop_to_default_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- path_slot_id gate (0x0047e1fe CMP / 0x0047e205 JZ): 0xff (none) vs an assigned slot, plus
    // the exact args (cur_player, cur_index) when it fires. The second gate is held closed
    // (default_op_code == state) so this case isolates the FIRST gate only.
    // =================================================================================================
    {
        Seed s;
        s.player       = 2;
        s.index        = 3;
        s.path_slot_id = 0xff;
        seed_and_run(fx, s);
        ck(g_path_free_calls.empty(), "T1a: path_slot_id==0xff -- path_free_slot does NOT fire (0x0047e205 JZ taken)");

        s.path_slot_id = 12;
        seed_and_run(fx, s);
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == 2 && g_path_free_calls[0].index == 3,
           "T1b: path_slot_id!=0xff -- path_free_slot(cur_player=2, cur_index=3) fires with the RIGHT "
           "args, not swapped (0x0047e207 MOVZX EDX,CUR_INDEX / 0x0047e20e MOVZX EAX,CUR_PLAYER / "
           "0x0047e215 CALL)");
    }

    // =================================================================================================
    // T2 -- default_op_code != state gate (0x0047e239 CMP / 0x0047e23b JZ): equal vs unequal, plus
    // the exact arg when it fires -- the PROTO's default_op_code, NOT the unit's own current state
    // (a translation that passed `state` instead would still compile and still call unit_set_state,
    // just with the wrong value). The first gate is held closed (path_slot_id==0xff) so this case
    // isolates the SECOND gate only.
    // =================================================================================================
    {
        Seed s;
        s.player          = 0;
        s.index           = 1;
        s.path_slot_id    = 0xff;
        s.state           = 9;
        s.default_op_code = 9; // equal -- gate closed
        seed_and_run(fx, s);
        ck(g_set_state_calls.empty(), "T2a: default_op_code == state -- unit_set_state does NOT fire (0x0047e23b JZ taken)");

        s.default_op_code = 4; // distinct from state=9 -- gate open
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 4,
           "T2b: default_op_code(4) != state(9) -- unit_set_state(default_op_code=4) fires, NOT the "
           "unit's own state=9 (0x0047e242 MOVZX EAX,unit_proto_id / 0x0047e246 IMUL EAX,0x23f / "
           "0x0047e24c MOVZX EAX,[EAX+0xe4a185]=proto.default_op_code / 0x0047e253 CALL)");
    }

    // =================================================================================================
    // T3 -- tick_budget is zeroed UNCONDITIONALLY (0x0047e258/0x0047e262, both dwords -- not gated by
    // either CMP), in EVERY combination of the two gates.
    // =================================================================================================
    {
        struct Combo {
            uint8_t  path_slot_id;
            uint16_t state, default_op_code;
        };
        const Combo combos[4] = {
            {0xff, 1, 1}, // neither gate open
            {5, 1, 1},    // path gate only
            {0xff, 1, 2}, // state gate only
            {5, 1, 2},    // both gates open
        };
        for (const Combo &c : combos) {
            Seed s;
            s.player          = 0;
            s.index           = 1;
            s.path_slot_id    = c.path_slot_id;
            s.state           = c.state;
            s.default_op_code = c.default_op_code;
            s.tick_budget     = 77.25; // nonzero sentinel every run
            seed_and_run(fx, s);
            ck_eq_d(fx.tick_budget, 0.0,
                    "T3: tick_budget == 0.0 after every combo of the two gates (0x0047e258/0x0047e262)");
        }
    }

    // =================================================================================================
    // T4 -- CALL ORDER when both gates fire: path_free_slot BEFORE unit_set_state, matching the asm's
    // straight-line block layout (the path_slot_id block at 0x0047e1fe precedes the default_op_code
    // block at 0x0047e21a -- there is no branch that could reorder them).
    // =================================================================================================
    {
        Seed s;
        s.player          = 1;
        s.index           = 2;
        s.path_slot_id    = 7;
        s.state           = 3;
        s.default_op_code = 8;
        seed_and_run(fx, s);
        ck(trace_eq({"path_free_slot", "unit_set_state"}),
           "T4: path_free_slot fires BEFORE unit_set_state when both gates are open (asm block order, "
           "0x0047e1fe-block precedes 0x0047e21a-block)");
    }

    // =================================================================================================
    // T5 -- non-corruption: this function writes NOTHING on cur_unit itself (only own.tick_budget()) --
    // the header's own derivation says so, and the asm has no other MOV-to-[CUR_UNIT+...] anywhere.
    // The unit's own path_slot_id/state/unit_proto_id/order must read back exactly as seeded (the real
    // writes belong to path_free_slot/unit_set_state, both ORIGINAL callees stubbed as no-ops here),
    // and a neighbouring roster slot this call never addresses stays untouched.
    // =================================================================================================
    {
        Seed s;
        s.player       = 0;
        s.index        = 1;
        s.path_slot_id = 3; // path gate open -- the REAL path_free_slot would clear this in the
                            // live game; the STUB here does not, so it must read back UNCHANGED
        s.state           = 5;
        s.default_op_code = 9; // state gate open too
        seed_and_run(fx, s);

        const unit &u = fx.u(0, 1);
        ck(u.path_slot_id == 3,
           "T5: cur_unit->path_slot_id unchanged (owned by path_free_slot, stubbed as a no-op here)");
        ck(u.state == 5, "T5: cur_unit->state unchanged (owned by unit_set_state, stubbed as a no-op here)");
        ck(u.unit_proto_id == s.cfg_row, "T5: cur_unit->unit_proto_id untouched");
        ck(u.order == 0x77, "T5: cur_unit->order untouched (this function never reaches Order)");

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 88 && g.state == 0x33 && g.path_slot_id == 0x44 && g.order == 0x55,
           "T5: neighbouring guard slot untouched");
    }
}

} // namespace mh::sim::test
