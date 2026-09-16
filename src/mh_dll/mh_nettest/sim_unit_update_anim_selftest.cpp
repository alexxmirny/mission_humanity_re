//
// sim_unit_update_anim_selftest.cpp -- `simtest` cases for llm_strat_unit_update_anim
// (sim/sim_unit_update_anim.{h,cpp}), SIM1A.
//
// Zero callees (the only CALL in the body is the inert prologue, omitted per translator-brief rule
// 6), so unlike sim_unit_passive_engage_selftest.cpp there is no recording calls struct here -- every
// case drives detail:: directly over a fixture, same shape as sim_unit_predicates_selftest.cpp, with
// cur_unit_ptr REPOINTED at a specific roster slot (sim_test_support.h's fixture supports this: "a
// case that needs 'the current unit' to be a DIFFERENT record than units[0][0] repoints cur_unit_ptr
// itself before calling view()/store()").
//
#include "sim/sim_unit_update_anim.h"

#include <cmath>
#include <limits>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- gate: no-op when Unit[unit_proto_id].dmg_smoke_enabled == 0 (0x0047dd6e-0x0047dd84) -----------
void test_gate_off_is_a_no_op() {
    sim_fixture f;
    f.cur_unit_ptr = &f.u(2, 5);

    unit &cur                         = f.u(2, 5);
    cur.unit_proto_id                 = 40;
    f.cfg_units[40].dmg_smoke_enabled = 0;
    cur.dmg_smoke_anim_timer          = 12.5;
    cur.dmg_smoke_anim_id             = 7;

    sim_store      own = f.store();
    const sim_view v   = f.view();
    detail::unit_update_anim(v, own);

    ck(own.cur_unit().dmg_smoke_anim_timer == 12.5 && own.cur_unit().dmg_smoke_anim_id == 7,
       "update_anim: dmg_smoke_enabled == 0 -> the whole body is skipped, no field touched");
}

// ---- the CACHED (never-refreshed) anim_time, and the budget-fits-in-frame branch --------------------
void test_budget_fits_single_iteration() {
    sim_fixture f;
    f.cur_unit_ptr = &f.u(2, 5);

    unit &cur                         = f.u(2, 5);
    cur.unit_proto_id                 = 40;
    f.cfg_units[40].dmg_smoke_enabled = 1;
    cur.dmg_smoke_anim_id             = 3;
    cur.dmg_smoke_anim_timer          = 90.0;
    f.game_clock                      = 100.0; // local_time = 100 - 90 = 10
    f.anim_frames[4].time             = 20.0;  // Anim[dmg_smoke_anim_id(3)+1]; local_time(10)<=anim_time(20) -> fits

    sim_store      own = f.store();
    const sim_view v   = f.view();
    detail::unit_update_anim(v, own);

    ck(own.cur_unit().dmg_smoke_anim_id == 3, "update_anim: the fits branch never touches dmg_smoke_anim_id");
    ck_eq_d(own.cur_unit().dmg_smoke_anim_timer, 90.0,
            "update_anim: fits branch: timer = GAME_CLOCK(100) - local_time(10) = 90 (one iteration, then stop)");
}

// ---- advance-via-.next across multiple non-fit iterations, proving anim_time is CACHED ONCE ---------
void test_advance_via_next_multi_iteration_uses_cached_frame_time() {
    sim_fixture f;
    f.cur_unit_ptr = &f.u(1, 1);

    unit &cur                         = f.u(1, 1);
    cur.unit_proto_id                 = 40;
    f.cfg_units[40].dmg_smoke_enabled = 1;
    cur.dmg_smoke_anim_id             = 3;
    cur.dmg_smoke_anim_timer          = 55.0;
    f.game_clock                      = 100.0; // local_time = 45

    // anim_time is read ONCE from Anim[dmg_smoke_anim_id(3)+1] == Anim[4] and never refreshed, even
    // though dmg_smoke_anim_id changes below -- Anim[6]/Anim[10]'s .time are deliberately left at a
    // DIFFERENT value to prove the loop never re-reads them.
    f.anim_frames[4].time  = 20.0;
    f.anim_frames[4].next  = 2;     // iter 1: id 3 -> 5
    f.anim_frames[6].time  = 999.0; // must NOT be consulted
    f.anim_frames[6].next  = 4;     // iter 2: id 5 -> 9
    f.anim_frames[10].time = 999.0; // must NOT be consulted either

    sim_store      own = f.store();
    const sim_view v   = f.view();
    detail::unit_update_anim(v, own);

    // iter1: local_time(45) > anim_time(20) -> id += 2 (3->5), local_time = 25
    // iter2: local_time(25) > anim_time(20) -> id += 4 (5->9), local_time = 5
    // iter3: local_time(5) <= anim_time(20) -> fits: timer = 100 - 5 = 95, stop
    ck(own.cur_unit().dmg_smoke_anim_id == 9,
       "update_anim: two non-fit iterations advance dmg_smoke_anim_id by .next each time (3->5->9)");
    ck_eq_d(own.cur_unit().dmg_smoke_anim_timer, 95.0,
            "update_anim: the SAME cached anim_time(20) from Anim[4] -- not Anim[6]/Anim[10]'s 999.0 -- "
            "drives every iteration's comparison, proving it is never refreshed inside the loop");
}

// ---- restart-from-base: .next == 0 walks the POP_STATS out-of-declared-bounds idiom -----------------
void test_restart_from_pop_stats_when_next_is_zero() {
    sim_fixture f;
    f.cur_unit_ptr = &f.u(4, 2);

    unit &cur                         = f.u(4, 2);
    cur.unit_proto_id                 = 40;
    f.cfg_units[40].dmg_smoke_enabled = 1;
    cur.dmg_smoke_anim_id             = 3;
    cur.dmg_smoke_anim_timer          = 70.0;
    cur.dmg_smoke_level               = 0;     // keeps the walk IN BOUNDS: (&population[7].layoff_cursor)[0]
    f.game_clock                      = 100.0; // local_time = 30

    f.anim_frames[4].time         = 20.0;
    f.anim_frames[4].next         = 0;  // triggers the restart path
    f.population[7].layoff_cursor = 42; // the "restart base" this out-of-declared-bounds walk reads

    sim_store      own = f.store();
    const sim_view v   = f.view();
    detail::unit_update_anim(v, own);

    // iter1: local_time(30) > anim_time(20) -> next==0 -> id = population[7].layoff_cursor(42);
    //        local_time = 30 - 20 = 10
    // iter2: local_time(10) <= anim_time(20) -> fits: timer = 100 - 10 = 90, stop
    ck(own.cur_unit().dmg_smoke_anim_id == 42,
       "update_anim: .next == 0 -> dmg_smoke_anim_id is re-seeded from the POP_STATS restart-base read "
       "(&population[7].layoff_cursor)[dmg_smoke_level], PRESERVED verbatim, not 'fixed' into a real table");
    ck_eq_d(own.cur_unit().dmg_smoke_anim_timer, 90.0,
            "update_anim: the restart iteration still drains the SAME cached anim_time before the next fits check");
}

// ---- NaN-aware OUTER gate: FLDZ/FCOMP local_time/FNSTSW/SAHF/JNC -- a NaN budget must NOT exit early
void test_outer_gate_nan_local_time_does_not_exit_early() {
    sim_fixture f;
    f.cur_unit_ptr = &f.u(2, 2);

    unit &cur                         = f.u(2, 2);
    cur.unit_proto_id                 = 40;
    f.cfg_units[40].dmg_smoke_enabled = 1;
    cur.dmg_smoke_anim_id             = 3;
    cur.dmg_smoke_anim_timer          = std::numeric_limits<double>::quiet_NaN(); // local_time = clock - NaN = NaN
    f.game_clock                      = 100.0;
    f.anim_frames[4].time             = 20.0;

    sim_store      own = f.store();
    const sim_view v   = f.view();
    detail::unit_update_anim(v, own);

    // THE FIX THIS CASE GUARDS: the naive `0.0 < local_time` translation is FALSE on NaN and would
    // skip the loop entirely, leaving dmg_smoke_anim_timer at the unconditional pre-loop
    // GAME_CLOCK(100) assignment. The correct `!(local_time <= 0.0)` idiom (matching x87 JNC on an
    // unordered compare) enters the loop, takes the fits branch (NaN also fails `local_time >
    // anim_time`), and computes timer -= local_time -- NaN minus anything is NaN, so a NaN RESULT is
    // the observable signature that the loop actually ran.
    ck(std::isnan(own.cur_unit().dmg_smoke_anim_timer),
       "update_anim: NaN local_time does NOT exit the outer loop gate (x87 unordered != ordered <= 0) "
       "-- the loop runs and leaves a NaN timer, not the pre-loop GAME_CLOCK(100) it would if skipped");
}

// ---- NaN-aware INNER gate: FLD local_time/FCOMP anim_time/FNSTSW/SAHF/JBE -- a NaN cached frame time
void test_inner_gate_nan_anim_time_takes_the_fits_branch() {
    sim_fixture f;
    f.cur_unit_ptr = &f.u(3, 3);

    unit &cur                         = f.u(3, 3);
    cur.unit_proto_id                 = 40;
    f.cfg_units[40].dmg_smoke_enabled = 1;
    cur.dmg_smoke_anim_id             = 3; // sentinel -- must stay 3 if the fits (not advance) branch is taken
    cur.dmg_smoke_anim_timer          = 150.0;
    f.game_clock                      = 200.0;                                    // local_time = 50, finite and > 0
    f.anim_frames[4].time             = std::numeric_limits<double>::quiet_NaN(); // the CACHED frame time is NaN

    sim_store      own = f.store();
    const sim_view v   = f.view();
    detail::unit_update_anim(v, own);

    // THE FIX THIS CASE GUARDS: the naive `local_time <= anim_time` translation is FALSE when
    // anim_time is NaN and would SKIP the fits branch (falsely taking the advance-via-.next branch
    // instead, mutating dmg_smoke_anim_id). The correct `!(local_time > anim_time)` idiom (matching
    // x87 JBE, which fires on CF|ZF, i.e. also on an unordered compare) takes the FITS branch on NaN:
    // this one-iteration case ends with dmg_smoke_anim_id UNCHANGED and only the timer touched.
    ck(own.cur_unit().dmg_smoke_anim_id == 3,
       "update_anim: NaN cached anim_time -> the fits branch is taken (x87 JBE fires on unordered), "
       "dmg_smoke_anim_id is NOT advanced");
    ck_eq_d(own.cur_unit().dmg_smoke_anim_timer, 150.0,
            "update_anim: fits branch: timer = GAME_CLOCK(200) - local_time(50) = 150");
}

} // namespace

void run_unit_update_anim_tests() {
    test_gate_off_is_a_no_op();
    test_budget_fits_single_iteration();
    test_advance_via_next_multi_iteration_uses_cached_frame_time();
    test_restart_from_pop_stats_when_next_is_zero();
    test_outer_gate_nan_local_time_does_not_exit_early();
    test_inner_gate_nan_anim_time_takes_the_fits_branch();
}

} // namespace mh::sim::test
