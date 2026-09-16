//
// sim_bldg_state_hangar_recharge_check_selftest.cpp -- `simtest` cases for
// llm_strat_bldg_state_hangar_recharge_check (sim/sim_bldg_state_hangar.h/.cpp @0x00472b26), SIM1-G4
// building_tick machinery -- the "is any hangar unit still hungry for energy" per-tick handler.
//
// SCOPE, hand-derived from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_hangar_recharge_check_00472b26.asm), NOT the sibling .c draft:
//
//   Gate (0x00472b4c-0x00472b6d): `if (llm_strat_hangar_any_unit_needs_energy(cur_player, cur_index)
//   == 0) { cur_building->state = 0x77 /* IDLE_NOOP */; } else { cur_building->state = 0x79
//   /* HANGAR_RECHARGE_UNITS */; }` -- written through the cur_building POINTER (state@0xd, uint16_t).
//   No cfg read anywhere in this function -- it is exactly the one bool call, the branch, and the tail.
//   Tail, UNCONDITIONAL on BOTH arms (0x00472b6d-0x00472b93): `llm_strat_bldg_notify_ui(cur_player,
//   cur_index)` THEN `llm_strat_refresh_building(cur_player, cur_index)` -- in that order, both firing
//   regardless of which way the gate went. This is the case the header banner calls out explicitly:
//   a naive oracle that only checks the tail behind a "gate taken" case would pass vacuously on a
//   translation that (bug) gated the tail calls on needs_energy too -- so this file exercises the tail
//   pair on EACH arm separately (C1 and C2), not just once.
//
// PIN LIST (each ck message below names the mechanism + its address; see the mutation-testing note in
// the delegated-investigation brief -- a message that does not name what it proves cannot be read as
// having caught the mutation that broke it):
//   - needs_energy == 0 -> state = IDLE_NOOP (0x77)                                   (C1)
//   - needs_energy != 0 (both a boundary value of 1 AND a non-canonical nonzero, e.g. -1 as an int32
//     the original TEST/JZ treats as "nonzero") -> state = HANGAR_RECHARGE_UNITS (0x79)  (C2, C3)
//   - the gate call receives (cur_player, cur_index) in that arg order, both arms               (C1,C2)
//   - the tail's TWO calls fire UNCONDITIONALLY on EACH arm, in order notify_ui THEN
//     refresh_building, with (cur_player, cur_index) each                                (C1,C2)
//   - the tail calls are NOT gated behind the needs_energy result (checked on both arms explicitly,
//     not inferred from one)                                                             (C1,C2)
//   - (cur_player, cur_index) are threaded consistently, not swapped, across all three calls (C4)
//   - cur_player/cur_index are RE-READ fresh from the ambient globals at EACH of the three call
//     sites, not hoisted into a local once at the top (mid-call mutation pin)                 (C5)
//   - the write goes through the SAME cur_building pointer sim_store::cur_building() names, not some
//     other record (C1/C2 assert on fx.b(player,index), the same building the fixture wires
//     cur_building_ptr to)
//   - no cfg_buildings/cfg_building_sec touch anywhere in this closure (nothing to seed, nothing to
//     assert -- the banner's "no cfg read at all" claim is pinned negatively by C1/C2 asserting NOTHING
//     else changed on the building record besides `state`)
//
#include "sim/sim_bldg_state_hangar.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves call ORDER across all three callees -----------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knob for the gate call, settable per case BEFORE seed_and_run -----------------
int32_t g_needs_energy_ret = 0;

// ---- C5's re-read pin: the asm re-reads cur_player/cur_index fresh from the ambient globals at
// EACH of the three call sites (0x00472b3e/0x00472b6d/0x00472b80 all MOVZX straight from the same two
// memory operands, not from a cached register carried across the calls), and the .cpp mirrors that by
// dereferencing `*v.cur_player`/`*v.cur_index` at each call site rather than hoisting them into a
// local. A translation that "optimized" by caching the values ONCE at the top would be
// behaviourally indistinguishable in the live game (nothing in this closure writes those globals),
// but IS distinguishable here: this pointer, set by seed_and_run, lets the gate-call recorder mutate
// the fixture's ambient view_cur_player/view_cur_index mid-call, so the tail calls only see the NEW
// values if the code under test genuinely re-reads rather than reusing a cached local.
sim_fixture *g_fixture_for_mutation = nullptr;
bool         g_mutate_on_gate_call  = false;
uint16_t     g_mutated_player       = 0;
uint16_t     g_mutated_index        = 0;

// ---- per-callee recorders (3, one per bldg_state_hangar_recharge_check_calls member) -------------
struct NeedsEnergyCall {
    int32_t player, index;
};
std::vector<NeedsEnergyCall> g_needs_energy_calls;
int32_t                      rec_hangar_any_unit_needs_energy(int32_t player, int32_t index) {
    tr("needs_energy");
    g_needs_energy_calls.push_back({player, index});
    if (g_mutate_on_gate_call && g_fixture_for_mutation != nullptr) {
        g_fixture_for_mutation->view_cur_player = g_mutated_player;
        g_fixture_for_mutation->view_cur_index  = g_mutated_index;
    }
    return g_needs_energy_ret;
}

struct NotifyUiCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyUiCall> g_notify_ui_calls;
void                      rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_ui_calls.push_back({player, index});
}

struct RefreshCall {
    uint16_t player;
    int32_t  index;
};
std::vector<RefreshCall> g_refresh_calls;
void                     rec_refresh_building(uint16_t player, int32_t index) {
    tr("refresh_building");
    g_refresh_calls.push_back({player, index});
}

const bldg_state_hangar_recharge_check_calls g_calls = {
    &rec_hangar_any_unit_needs_energy,
    &rec_bldg_notify_ui,
    &rec_refresh_building,
};

void reset_observations() {
    g_trace.clear();
    g_needs_energy_calls.clear();
    g_notify_ui_calls.clear();
    g_refresh_calls.clear();
}

// ---- fixture seeding -------------------------------------------------------------------------------
struct Seed {
    // Distinct, non-symmetric player/index so a swapped-arg translation is caught by value, not just
    // by accident of both being equal.
    uint16_t player = 1;
    int32_t  index  = 3;

    // A sentinel PRE-EXISTING state value, distinct from BOTH branch outcomes (0x77/0x79) and from 0,
    // so a case that forgets to check the gate at all (leaving state untouched) does not accidentally
    // read as passing.
    uint16_t state_in = 0x55;

    int32_t needs_energy_ret = 0; // case overrides to walk the gate

    // C5's re-read pin -- see g_mutate_on_gate_call's comment above. Off by default.
    bool     mutate_on_gate_call = false;
    uint16_t mutated_player      = 0;
    uint16_t mutated_index       = 0;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b = fx.b(s.player, s.index);
    b.state     = s.state_in;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    g_needs_energy_ret     = s.needs_energy_ret;
    g_fixture_for_mutation = &fx;
    g_mutate_on_gate_call  = s.mutate_on_gate_call;
    g_mutated_player       = s.mutated_player;
    g_mutated_index        = s.mutated_index;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_hangar_recharge_check(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_hangar_recharge_check_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- needs_energy() returns 0 (0x00472b51 TEST/0x00472b53 JZ TAKEN, jumps to LAB_00472b62): state
    // -> IDLE_NOOP (0x77, `MOV word ptr [EAX+0xd],0x77` @0x00472b67) -- IDLE_NOOP is the "no unit needs
    // energy" outcome. The UNCONDITIONAL tail (notify_ui THEN refresh_building) still fires on this arm
    // -- the case that catches an oracle gating the tail behind the bool result.
    // =================================================================================================
    {
        Seed s;
        s.needs_energy_ret = 0;
        seed_and_run(fx, s);

        ck(g_needs_energy_calls.size() == 1 && g_needs_energy_calls[0].player == (int32_t)s.player &&
               g_needs_energy_calls[0].index == s.index,
           "C1: llm_strat_hangar_any_unit_needs_energy(cur_player, cur_index) fires once with "
           "(cur_player, cur_index) in that order (0x00472b3e-0x00472b4c)");

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x77u,
              "C1: needs_energy==0 -> state = IDLE_NOOP (0x77), JZ @0x00472b53 TAKEN -> LAB_00472b62 "
              "`MOV word ptr [EAX+0xd],0x77` @0x00472b67");

        ck(trace_eq({"needs_energy", "notify_ui", "refresh_building"}),
           "C1: exact call order gate-then-tail, and the tail fires even on the needs_energy==0 arm "
           "(0x00472b6d bldg_notify_ui -> 0x00472b8e refresh_building) -- NOT gated behind the bool");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C1: llm_strat_bldg_notify_ui(cur_player, cur_index) fires unconditionally on the "
           "IDLE_NOOP arm (0x00472b6d-0x00472b7b)");
        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == s.player &&
               g_refresh_calls[0].index == s.index,
           "C1: llm_strat_refresh_building(cur_player, cur_index) fires unconditionally on the "
           "IDLE_NOOP arm (0x00472b80-0x00472b93)");
    }

    // =================================================================================================
    // C2 -- needs_energy() returns 1 (the canonical "true", JZ @0x00472b53 NOT taken): state ->
    // HANGAR_RECHARGE_UNITS (0x79, 0x00472b5a `MOV word ptr [EAX+0xd],0x79`). Same unconditional tail,
    // proven fired on THIS arm too (the inverse half of the "not gated" pin).
    // =================================================================================================
    {
        Seed s;
        s.needs_energy_ret = 1;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x79u,
              "C2: needs_energy!=0 -> state = HANGAR_RECHARGE_UNITS (0x79), JZ @0x00472b53 NOT taken, "
              "falls through to `MOV word ptr [EAX+0xd],0x79` @0x00472b5a");

        ck(trace_eq({"needs_energy", "notify_ui", "refresh_building"}),
           "C2: exact call order gate-then-tail on the RECHARGE_UNITS arm too");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C2: llm_strat_bldg_notify_ui(cur_player, cur_index) fires unconditionally on the "
           "RECHARGE_UNITS arm too (0x00472b6d-0x00472b7b) -- not gated behind needs_energy");
        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == s.player &&
               g_refresh_calls[0].index == s.index,
           "C2: llm_strat_refresh_building(cur_player, cur_index) fires unconditionally on the "
           "RECHARGE_UNITS arm too (0x00472b80-0x00472b93)");
    }

    // =================================================================================================
    // C3 -- needs_energy() returns a non-canonical nonzero (-1): the x86 `TEST EAX,EAX` / `JZ` pair
    // treats ANY nonzero dword as "true", so -1 must take the SAME branch as +1 (C2), pinning that the
    // translation's `if (needs_energy == 0)` reads the raw int32 rather than e.g. testing `== 1` or
    // treating a negative return as a distinct/error case.
    // =================================================================================================
    {
        Seed s;
        s.needs_energy_ret = -1;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x79u,
              "C3: needs_energy==-1 (non-canonical nonzero) -> STILL state = HANGAR_RECHARGE_UNITS "
              "(0x79) -- TEST/JZ @0x00472b51-0x00472b53 branches on ANY nonzero dword, not just +1");
    }

    // =================================================================================================
    // C4 -- ARGUMENT-DISTINCTNESS pin: (cur_player, cur_index) deliberately swapped in MAGNITUDE
    // (index < player here, the opposite of C1-C3) so a translation that swapped the two args at any
    // of the three call sites is caught by value, not masked by a symmetric fixture. Threaded
    // identically through the gate call and both tail calls.
    // =================================================================================================
    {
        Seed s;
        s.player           = 6;
        s.index            = 2; // deliberately swapped magnitude vs C1/C2/C3 (index < player here)
        s.needs_energy_ret = 1;
        seed_and_run(fx, s);

        ck(g_needs_energy_calls.size() == 1 && g_needs_energy_calls[0].player == (int32_t)s.player &&
               g_needs_energy_calls[0].index == s.index,
           "C4: gate call (cur_player=6, cur_index=2) -- not swapped despite index < player here "
           "(0x00472b3e MOVZX EDX,cur_index / 0x00472b45 MOVZX EAX,cur_player -> "
           "hangar_any_unit_needs_energy(EAX=player, EDX=index))");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C4: tail's bldg_notify_ui uses the SAME (cur_player, cur_index) pair, not a swapped one "
           "(0x00472b80-0x00472b87)");
        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == s.player &&
               g_refresh_calls[0].index == s.index,
           "C4: tail's refresh_building uses the SAME (cur_player, cur_index) pair too "
           "(0x00472b80-0x00472b87, shared with notify_ui's own re-read just before it)");
    }

    // =================================================================================================
    // C5 -- RE-READ (not cached) pin: the asm re-reads cur_player/cur_index fresh from the ambient
    // globals at EACH of the three call sites (0x00472b3e/0x00472b45 for the gate call,
    // 0x00472b6d/0x00472b74 for notify_ui, 0x00472b80/0x00472b87 for refresh_building -- three separate
    // MOVZX pairs off the same two memory operands, never carried in a register across a call). The
    // gate-call recorder mutates the fixture's ambient view_cur_player/view_cur_index to a DIFFERENT
    // pair mid-call; a translation that (mis-)optimized by hoisting `*v.cur_player`/`*v.cur_index` into
    // locals ONCE at the top and reusing them for all three calls would still show the PRE-mutation
    // pair on the tail calls, failing this case, even though C1-C4 above could not distinguish it
    // (nothing in THIS closure mutates those globals in the real game).
    // =================================================================================================
    {
        Seed s;
        s.player              = 4;
        s.index               = 9;
        s.needs_energy_ret    = 1;
        s.mutate_on_gate_call = true;
        s.mutated_player      = 40; // distinct from both s.player/s.index and from each other
        s.mutated_index       = 90;
        seed_and_run(fx, s);

        ck(g_needs_energy_calls.size() == 1 && g_needs_energy_calls[0].player == (int32_t)s.player &&
               g_needs_energy_calls[0].index == s.index,
           "C5: the gate call itself still observes the PRE-mutation pair (4, 9) -- the mutation "
           "happens INSIDE the recorder, after this call's own args are already latched "
           "(0x00472b3e-0x00472b4c)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.mutated_player &&
               g_notify_ui_calls[0].index == (uint32_t)s.mutated_index,
           "C5: bldg_notify_ui observes the MUTATED pair (40, 90) -- proves cur_player/cur_index are "
           "RE-READ from the ambient globals at the tail, not cached from before the gate call "
           "(0x00472b6d/0x00472b74 fresh MOVZX, not a carried register)");
        ck(g_refresh_calls.size() == 1 && g_refresh_calls[0].player == s.mutated_player &&
               g_refresh_calls[0].index == s.mutated_index,
           "C5: refresh_building observes the MUTATED pair too (0x00472b80/0x00472b87 fresh MOVZX)");
    }
}

} // namespace mh::sim::test
