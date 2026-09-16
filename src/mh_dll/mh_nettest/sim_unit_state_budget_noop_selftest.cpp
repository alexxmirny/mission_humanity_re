//
// sim_unit_state_budget_noop_selftest.cpp -- `simtest` cases for the SIM1-G1 unit `budget_noop`:
// llm_strat_unit_state_parked_noop @0x0047e276 and llm_strat_unit_state_default_noop @0x0047e2ac
// (sim/sim_unit_state_budget_noop.h/.cpp). Both `void __watcall <name>(void)`, byte-for-byte
// identical 0x36-byte bodies, no _calls struct (nothing outward to stub).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_state_parked_noop_0047e276.asm and
// tmp/decomp/llm_strat_unit_state_default_noop_0047e2ac.asm -- NOT read off the .cpp. Both bodies,
// after the inert `utils_assert_stack_capacity` prologue call, are exactly:
//   parked_noop:  MOV dword ptr [0x00ae3738],0x0 (0x0047e28e) ; MOV dword ptr [0x00ae373c],0x0 (0x0047e298)
//   default_noop: MOV dword ptr [0x00ae3738],0x0 (0x0047e2c4) ; MOV dword ptr [0x00ae373c],0x0 (0x0047e2ce)
// i.e. the whole 8-byte double at _G_LLM_STRAT_TICK_BUDGET (0x00ae3738) is zeroed via its low dword
// then its high dword, and nothing else happens: no branch, no read, no outward call besides the
// inert stack probe, no roster/unit touch of any kind.
//
// SCOPE (honest, not exhaustive): there are no branches to cover -- each body is a straight-line
// unconditional double-zero. Coverage here is: (1) the write itself lands on the SAME ambient
// tick-budget scratch double both dword stores target (fx.tick_budget after the call == 0.0, seeded
// from a distinct nonzero value beforehand so the zeroing is actually observable), and (2) NOTHING
// else is written -- the current unit (own.cur_unit(), fx.cur_unit_ptr's target) and an unrelated
// neighbouring roster slot both keep every seeded sentinel field, proving the body touches no unit
// record at all. Both functions are exercised via their own `detail::` entry point (not inferred
// from one call's result), since each is a separate original address with its own shadow site (see
// the header's rationale for two detail:: bodies despite the identical machine code).
//
#include "sim/sim_unit_state_budget_noop.h"

#include <cstdint>
#include <cstdio>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Two distinct, non-adjacent, sentinel-seeded roster slots this function never has any business
// touching -- one is the "current unit" (own.cur_unit()/fx.cur_unit_ptr's target), the other an
// unrelated neighbour. Neither .asm body contains a single instruction that addresses
// _G_LLM_STRAT_CUR_UNIT or any unit-record field, so BOTH must come back byte-identical.
constexpr uint16_t CUR_PLAYER   = 0;
constexpr int32_t  CUR_INDEX    = 1;
constexpr uint16_t GUARD_PLAYER = 2;
constexpr int32_t  GUARD_INDEX  = 3;

void seed_sentinel(unit &u, uint16_t proto, int32_t x, int32_t y, int16_t tref, int16_t tidx,
                   int16_t t2ref, int16_t t2idx, uint8_t path_slot, int32_t microstep,
                   double activity_clock) {
    u.unit_proto_id  = proto;
    u.x              = (uint8_t)x;
    u.y              = (uint8_t)y;
    u.target_ref     = tref;
    u.target_index   = tidx;
    u.target2_ref    = t2ref;
    u.target2_index  = t2idx;
    u.path_slot_id   = path_slot;
    u.move_microstep = microstep;
    u.activity_clock = activity_clock;
}

bool sentinel_intact(const unit &u, uint16_t proto, int32_t x, int32_t y, int16_t tref, int16_t tidx,
                     int16_t t2ref, int16_t t2idx, uint8_t path_slot, int32_t microstep,
                     double activity_clock) {
    return u.unit_proto_id == proto && u.x == (uint8_t)x && u.y == (uint8_t)y && u.target_ref == tref &&
           u.target_index == tidx && u.target2_ref == t2ref && u.target2_index == t2idx &&
           u.path_slot_id == path_slot && u.move_microstep == microstep &&
           u.activity_clock == activity_clock;
}

// Runs ONE of the two byte-identical detail:: bodies (both take `sim_store &own` only) and checks
// its ONLY effect (the tick-budget zero-write) plus non-corruption of the current unit and a
// neighbouring guard slot. `fn_name` is folded into every message so a failure names which of the
// two original addresses it came from.
void check_noop(void (*fn)(sim_store &), const char *fn_name) {
    sim_fixture fx;
    fx.reset();

    // DISTINCT, non-symmetric sentinel values for the two roster slots -- a swapped-index write
    // (e.g. landing on the guard slot instead of nowhere) would be caught by either failing.
    // activity_clock is included deliberately: it's a `double` field in the SAME "tick budget"
    // domain as _G_LLM_STRAT_TICK_BUDGET (sim_state.h documents TICK_BUDGET = GAME_CLOCK -
    // activity_clock), so it's the most plausible field a mistaken translation might touch instead
    // of (or in addition to) the ambient scratch double -- neither .asm body references it at all.
    seed_sentinel(fx.u(CUR_PLAYER, CUR_INDEX), /*proto*/ 66, /*x*/ 21, /*y*/ 22, /*tref*/ 1, /*tidx*/ 2,
                  /*t2ref*/ 3, /*t2idx*/ 4, /*path_slot*/ 55, /*microstep*/ 888,
                  /*activity_clock*/ 5555.25);
    seed_sentinel(fx.u(GUARD_PLAYER, GUARD_INDEX), /*proto*/ 77, /*x*/ 41, /*y*/ 59, /*tref*/ 5,
                  /*tidx*/ 6, /*t2ref*/ 7, /*t2idx*/ 8, /*path_slot*/ 33, /*microstep*/ 444,
                  /*activity_clock*/ 6666.75);

    fx.cur_unit_ptr    = &fx.u(CUR_PLAYER, CUR_INDEX);
    fx.view_cur_player = CUR_PLAYER;
    fx.view_cur_index  = (uint16_t)CUR_INDEX;

    // A distinct, NONZERO double (exactly representable, so ck_eq_d's exact compare is meaningful)
    // so the post-call 0.0 is an actual observation, not "it was already zero".
    fx.tick_budget = 12345.6875;

    sim_store own = fx.store();
    fn(own);

    char what[192];
    std::snprintf(what, sizeof(what),
                  "%s: the whole 8-byte double at _G_LLM_STRAT_TICK_BUDGET is zeroed via its low "
                  "dword then its high dword -- own.tick_budget() = 0.0",
                  fn_name);
    ck_eq_d(fx.tick_budget, 0.0, what);

    const unit &cur = fx.u(CUR_PLAYER, CUR_INDEX);
    std::snprintf(what, sizeof(what),
                  "%s: the current unit (own.cur_unit()) is untouched -- no field-write instruction "
                  "of any kind besides the two TICK_BUDGET dword stores",
                  fn_name);
    ck(sentinel_intact(cur, 66, 21, 22, 1, 2, 3, 4, 55, 888, 5555.25), what);

    const unit &guard = fx.u(GUARD_PLAYER, GUARD_INDEX);
    std::snprintf(what, sizeof(what),
                  "%s: a neighbouring, unaddressed roster slot is untouched", fn_name);
    ck(sentinel_intact(guard, 77, 41, 59, 5, 6, 7, 8, 33, 444, 6666.75), what);
}

} // namespace

void run_unit_state_budget_noop_tests() {
    // T1 -- llm_strat_unit_state_parked_noop @0x0047e276: MOV [TICK_BUDGET],0 (0x0047e28e) / MOV
    // [TICK_BUDGET+4],0 (0x0047e298), no branch, no callee besides the inert stack probe.
    check_noop(&mh::sim::detail::unit_state_parked_noop, "unit_state_parked_noop@0x0047e276");

    // T2 -- llm_strat_unit_state_default_noop @0x0047e2ac: byte-identical body, own address -- MOV
    // [TICK_BUDGET],0 (0x0047e2c4) / MOV [TICK_BUDGET+4],0 (0x0047e2ce). A SEPARATE detail::
    // function/shadow site per the header's rationale (two distinct original addresses), so it gets
    // its own call and its own checks, not the first call's result reused.
    check_noop(&mh::sim::detail::unit_state_default_noop, "unit_state_default_noop@0x0047e2ac");
}

} // namespace mh::sim::test
