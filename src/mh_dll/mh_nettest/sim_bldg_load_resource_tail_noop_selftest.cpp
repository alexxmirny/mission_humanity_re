//
// sim_bldg_load_resource_tail_noop_selftest.cpp -- `simtest` cases for llm_bldg_load_resource_tail_
// noop @0x0049f84a (sim/sim_bldg_load_resource_tail_noop.h/.cpp), CONFIRMED DEAD CODE per the ledger.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_bldg_load_resource_tail_noop_0049f84a.asm --
// NOT read off the .cpp. Re-verified instruction by instruction for this file: after the inert
// `utils_assert_stack_capacity` prologue call (0x0049f852), the body is
//   MOV [EBP-0x18],1                                              (0x0049f865, counter := 1)
//   LAB_0049f86c: CMP [EBP-0x18],0xa / JL .. / JMP 0x0049f896      (0x0049f86c-0x0049f872, loop test)
//   LAB_0049f87c: IMUL EDX,[EBP-0x1c],0x1f18                       (0x0049f87c)
//                 IMUL EAX,[EBP-0x18],0x31c                        (0x0049f883)
//                 ADD EAX,EDX ; CMP word ptr [EAX+0xbd2172],0      (0x0049f88a, 0x0049f88c -- READ, discarded)
//   LAB_0049f874: MOV EAX,[EBP-0x18] (dead) ; INC [EBP-0x18]       (0x0049f874, 0x0049f877, counter++)
//   LAB_0049f896: pop the five saved regs, LEA ESP, RET            (0x0049f896-0x0049f89f, no EAX load)
// i.e. NINE iterations (counter 1..9) of "compute an address from a caller-left, never-actually-a-
// parameter EAX value and an over-9-iterating counter, CMP one word there, discard the flags" and
// nothing else. There is no MOV/store to memory anywhere in the body, no CALL besides the inert stack
// probe at 0x0049f852, and the RET at 0x0049f89f has no preceding `MOV EAX,*` -- the function is void,
// not an int-returner. Agrees with the header/cpp derivation and the ledger's CONFIRMED DEAD CODE
// verdict: no write, no (non-inert) call, void return -- there is no discrepancy to report.
//
// WHY THIS TEST LOOKS DIFFERENT FROM THE sim_unit_state_budget_noop_selftest.cpp EXEMPLAR: that
// closure's two noop bodies still take a `sim_store &own` because they DO write one real region
// (TICK_BUDGET). This function's detail:: body AND its public wrapper both take zero parameters
// (sim_bldg_load_resource_tail_noop.h) and neither calls `mh::sim::state()` (confirmed by reading
// sim_bldg_load_resource_tail_noop.cpp: the public wrapper is a bare forward to detail::, no `state()`
// anywhere in the TU) -- there is no sim_store/sim_view handle to route a fixture through at all. So
// "pin the nothing" here cannot mean "pass the fixture in and check it back out" the way the exemplar
// does; it means "make ANY future edit that reaches for state -- the only way this signature COULD
// acquire behaviour without changing its own prototype -- land somewhere this test can see it."
// `mh::sim::state()` needs no arguments, so a future edit could add `sim_state st = state();` inside
// this exact void(void) body with no signature change at all, exactly the shape every other noop
// sibling in this file's neighbourhood already uses to reach real state.
//
// SCOPE (honest, not exhaustive): there are no branches to cover -- the loop always runs exactly nine
// times regardless of input, and every iteration is the identical discarded-read shape. Coverage here
// is: (1) a `state_guard` traps the specific way this function COULD start doing something -- calling
// `state()` -- by redirecting the four pointer-valued globals `state()` ALWAYS dereferences
// (CUR_UNIT/CUR_BUILDING/CUR_PROJECTILE/CUR_FX_ANIM, unconditionally, regardless of what the caller
// needs -- see sim_weapon_damage_selftest.cpp's live_regions_on_fixture for the same rationale) plus
// BUILDINGS and TICK_BUDGET -- the two regions this test actually asserts on -- onto the fixture's own
// storage, so a stray `state()` call lands in observable fixture memory instead of dereferencing an
// unmapped address from the live game's layout and crashing the process with no named failure; (2) the
// building record this function is nominally about (cur_building_ptr's target, "building_tick
// machinery" per the header) keeps every seeded field; (3) a neighbouring building slot in the SAME
// array keeps every seeded field (catches a write landing one slot off, e.g. from a mistaken re-read
// of the discarded loop's counter as a real index); (4) the ambient tick-budget scratch double --
// named explicitly in the task brief and the field this closure's sibling noop functions zero -- is
// untouched. The `.asm`'s own discarded-read address (EAX_at_entry*0x1f18 + counter*0x31c + 0xbd2172)
// is NOT itself seeded/asserted: EAX_at_entry is documented (header banner, re-confirmed above) as
// whatever the caller happened to leave in the register, not a real parameter or index, so there is no
// single named global that address resolves to -- pinning BUILDINGS/TICK_BUDGET/the CUR_* targets
// covers every region a plausible "restore some behaviour" edit could reach through `state()`, which
// is the only path this signature has to reach anything at all.
//
#include "sim/sim_bldg_load_resource_tail_noop.h"

#include <cstdint>
#include <cstdio>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Redirects the four pointer-valued globals `mh::sim::state()` ALWAYS dereferences, plus BUILDINGS
// and TICK_BUDGET (the two regions this test asserts on), onto the fixture's own storage for the
// scope's lifetime. bldg_load_resource_tail_noop() does not call state() today -- this guard exists
// purely as a trap for a future edit that starts calling it, so that edit's write is observed by this
// test's named assertions instead of corrupting (or crashing on) an address from the live game's
// layout that has no meaning in an offline test process.
class state_guard {
public:
    explicit state_guard(sim_fixture &f)
        : cur_unit_(f.cur_unit_ptr), cur_building_(f.cur_building_ptr),
          cur_projectile_(f.cur_projectile_ptr), cur_fx_anim_(f.cur_fx_anim_ptr) {
        bind(mh::state::RID_STRAT_CUR_UNIT, &cur_unit_, sizeof(cur_unit_));
        bind(mh::state::RID_STRAT_CUR_BUILDING, &cur_building_, sizeof(cur_building_));
        bind(mh::state::RID_STRAT_CUR_PROJECTILE, &cur_projectile_, sizeof(cur_projectile_));
        bind(mh::state::RID_STRAT_CUR_FX_ANIM, &cur_fx_anim_, sizeof(cur_fx_anim_));
        bind(mh::state::RID_BUILDINGS, f.buildings.data(), f.buildings.size() * sizeof(building));
        bind(mh::state::RID_STRAT_TICK_BUDGET, &f.tick_budget, sizeof(double));
    }
    ~state_guard() {
        for (mh::state::region_id r : kRedirected) mh::state::unrebase(r);
    }
    state_guard(const state_guard &)            = delete;
    state_guard &operator=(const state_guard &) = delete;

private:
    static void bind(mh::state::region_id r, void *p, size_t n) {
        mh::state::rebase(r, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)),
                          static_cast<uint32_t>(n));
    }

    static constexpr mh::state::region_id kRedirected[] = {
        mh::state::RID_STRAT_CUR_UNIT,
        mh::state::RID_STRAT_CUR_BUILDING,
        mh::state::RID_STRAT_CUR_PROJECTILE,
        mh::state::RID_STRAT_CUR_FX_ANIM,
        mh::state::RID_BUILDINGS,
        mh::state::RID_STRAT_TICK_BUDGET,
    };

    unit       *cur_unit_;
    building   *cur_building_;
    projectile *cur_projectile_;
    fx_anim    *cur_fx_anim_;
};

// A building's sentinel snapshot -- DISTINCT, non-symmetric field values (index/building_id/state
// numerically different from the neighbour's, x/y swapped-and-different, energy/cycle_progress
// non-integral so a truncation or a wrong-field write is visible).
struct bldg_sentinel {
    int16_t  index;
    uint16_t building_id;
    uint16_t state;
    uint8_t  x, y;
    double   energy;
    double   cycle_progress;
};

void seed_building(building &b, const bldg_sentinel &s) {
    b.index          = s.index;
    b.building_id    = s.building_id;
    b.state          = s.state;
    b.x              = s.x;
    b.y              = s.y;
    b.energy         = s.energy;
    b.cycle_progress = s.cycle_progress;
}

// Every field named separately, per the task brief: "a single 'nothing changed' assertion over one
// field is not enough". `tag` distinguishes the current-building case from the neighbour case in the
// failure message; `fn_name` distinguishes detail:: from the public wrapper.
void check_building_intact(const building &b, const bldg_sentinel &want, const char *tag,
                           const char *fn_name) {
    char what[224];

    std::snprintf(what, sizeof(what),
                  "%s/%s: .index untouched -- the body (0x0049f84a-0x0049f89f) has no memory-writing "
                  "instruction of any kind, only the discarded CMP read at 0x0049f88c",
                  fn_name, tag);
    ck_eq((uint32_t)(uint16_t)b.index, (uint32_t)(uint16_t)want.index, what);

    std::snprintf(what, sizeof(what),
                  "%s/%s: .building_id untouched -- same evidence, no store anywhere in the loop "
                  "(0x0049f86c-0x0049f894) or at the 0x0049f89f RET",
                  fn_name, tag);
    ck_eq((uint32_t)want.building_id, (uint32_t)b.building_id, what);

    std::snprintf(what, sizeof(what),
                  "%s/%s: .state untouched -- the RET at 0x0049f89f has no preceding EAX load and "
                  "the loop body's only memory access is the read-and-discard CMP at 0x0049f88c",
                  fn_name, tag);
    ck_eq((uint32_t)want.state, (uint32_t)b.state, what);

    std::snprintf(what, sizeof(what),
                  "%s/%s: .x untouched -- no store in the 9-iteration loop at 0x0049f86c-0x0049f894",
                  fn_name, tag);
    ck_eq((uint32_t)want.x, (uint32_t)b.x, what);

    std::snprintf(what, sizeof(what),
                  "%s/%s: .y untouched -- no store in the 9-iteration loop at 0x0049f86c-0x0049f894",
                  fn_name, tag);
    ck_eq((uint32_t)want.y, (uint32_t)b.y, what);

    std::snprintf(what, sizeof(what),
                  "%s/%s: .energy untouched -- the loop's only memory traffic is the word-sized read "
                  "at 0x0049f88c, discarded (no branch/store consumes it)",
                  fn_name, tag);
    ck_eq_d(b.energy, want.energy, what);

    std::snprintf(what, sizeof(what),
                  "%s/%s: .cycle_progress untouched -- same evidence as .energy above",
                  fn_name, tag);
    ck_eq_d(b.cycle_progress, want.cycle_progress, what);
}

// Runs ONE of the two call surfaces (detail:: body, or the public wrapper) and checks every seeded
// field survives: the current building (cur_building_ptr's target), a neighbouring building slot in
// the SAME array, and the ambient tick-budget scratch double.
void check_pins_nothing(void (*fn)(), const char *fn_name) {
    sim_fixture fx;
    fx.reset();

    const bldg_sentinel cur_want = {
        /*index*/ 11,
        /*building_id*/ 222,
        /*state*/ 7,
        /*x*/ 33,
        /*y*/ 44,
        /*energy*/ 555.5,
        /*cycle_progress*/ 66.25,
    };
    const bldg_sentinel neighbour_want = {
        /*index*/ 12,
        /*building_id*/ 333,
        /*state*/ 9,
        /*x*/ 77,
        /*y*/ 88,
        /*energy*/ 999.75,
        /*cycle_progress*/ 11.125,
    };

    // cur_building_ptr defaults to buildings.data() == b(0, 0) -- see sim_test_support.h's ctor.
    seed_building(*fx.cur_building_ptr, cur_want);
    // A distinct player/index in the SAME buildings array -- not adjacent to (0,0) by flat index
    // coincidence, so a swapped or off-by-one write is caught here rather than silently landing on
    // an unseeded neighbour.
    seed_building(fx.b(1, 2), neighbour_want);

    // Distinct, nonzero, exactly-representable double so the post-call value is an actual
    // observation, not "it was already whatever the mutation set it to".
    fx.tick_budget = 12345.6875;

    {
        state_guard guard(fx); // see the class banner -- traps a future state() call, if one appears
        fn();
    } // guard destructs (unrebase) before the checks below, matching live_regions_on_fixture's shape

    check_building_intact(*fx.cur_building_ptr, cur_want, "cur_building", fn_name);
    check_building_intact(fx.b(1, 2), neighbour_want, "neighbour_building(1,2)", fn_name);

    char what[224];
    std::snprintf(what, sizeof(what),
                  "%s: tick_budget untouched -- 0x0049f84a-0x0049f89f writes nothing anywhere "
                  "(unlike this closure's sibling budget_noop functions, which DO zero this same "
                  "field at their own addresses)",
                  fn_name);
    ck_eq_d(fx.tick_budget, 12345.6875, what);
}

} // namespace

void run_bldg_load_resource_tail_noop_tests() {
    // T1 -- the detail:: body directly, exactly as sim_bldg_load_resource_tail_noop.cpp's public
    // wrapper calls it (no arguments, matching the header's `void bldg_load_resource_tail_noop();`).
    check_pins_nothing(&mh::sim::detail::bldg_load_resource_tail_noop,
                       "detail::bldg_load_resource_tail_noop@0x0049f84a");

    // T2 -- the public wrapper (mh::sim::bldg_load_resource_tail_noop.cpp forwards straight to
    // detail:: with no state() call of its own, per this file's header banner) -- exercised
    // separately so a future edit that adds behaviour ONLY at the wrapper layer is caught too.
    check_pins_nothing(&mh::sim::bldg_load_resource_tail_noop,
                       "mh::sim::bldg_load_resource_tail_noop@0x0049f84a");
}

} // namespace mh::sim::test
