//
// sim_clock_resync_selftest.cpp -- `simtest` oracle for llm_strat_clock_resync_units_and_buildings
// @0x00499a3a (sim/resid/sim_clock_resync.h/.cpp, RI-SIM sim_resid batch).
//
// NO SHADOW SITE, NO `_calls` STRUCT: this unit has zero outward calls (see the header's own
// derivation) -- only the inert `PUSH 0x2c / CALL utils_assert_stack_capacity` prologue probe. This
// offline oracle (net_selftest simtest) is the ONLY verification (proof:OFFLINE).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY (tmp/decomp_sim_resid/
// llm_strat_clock_resync_units_and_buildings_00499a3a.asm) -- the .asm is the spec, never the .c
// beside it (the .c has silently lied elsewhere in this project):
//   0x00499a73: TEST byte[status_flags],0x2 -- a not-ALIVE player's units and buildings are left
//     completely untouched; an alive one's are rewritten.
//   0x00499a83: units[player][0].unit_above reinterpreted as a 16-bit word = the unit live-count
//     header.
//   0x00499a94-0x00499b08: scan slot 1, 2, 3, ... -- UNBOUNDED (no compare against UNITS_PER_PLAYER
//     anywhere in this loop) -- decrementing the live count only when unit_proto_id != 0
//     (0x00499ab4/0x00499abc), so an empty slot is SKIPPED without consuming the count.
//   0x00499ad1 / 0x00499af3: BOTH activity_clock and rotation_clock get the raw 8-byte bit-copy of
//     new_time (two independent stores).
//   0x00499b11: buildings[player][0].index reinterpreted as a 16-bit word = the building live-count
//     header, same shape as the unit header.
//   0x00499b45/0x00499b4d: same skip-empty shape for buildings -- building_id != 0 gates
//     last_tick_time (0x00499b5f) + all 12 anim_dur slots (0x00499b78-0x00499bb2, bound 0xc).
//   0x00499ace-0x00499bb1: every store is two raw 32-bit MOVs of the `new_time` parameter's two stack
//     dwords, with NOT ONE x87 instruction in the whole body (the header's FLOAT HANDLING note) --
//     so a negative/fractional new_time whose low and high dwords differ is the case that would catch
//     a half-copy or a reordered pair.
//
#include "sim/resid/sim_clock_resync.h"

#include "sim_test_support.h"

namespace mh::sim::test {

void run_clock_resync_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- ALIVE gate, NEGATIVE arm (0x00499a73 TEST .. JZ): a not-alive player's live unit and
    // building records are left COMPLETELY untouched, even though their live-count headers and
    // unit_proto_id/building_id say they are live.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t P         = 2;
        fx.profiles[P].status_flags = 0; // NOT alive (bit1 clear)

        fx.u(P, 0).unit_above[0]  = 1;
        fx.u(P, 0).unit_above[1]  = 0; // unit live-count header = 1
        fx.u(P, 1).unit_proto_id  = 5;
        fx.u(P, 1).activity_clock = 11.0;
        fx.u(P, 1).rotation_clock = 22.0;

        fx.b(P, 0).index          = 1; // building live-count header = 1
        fx.b(P, 1).building_id    = 6;
        fx.b(P, 1).last_tick_time = 33.0;
        fx.b(P, 1).anim_dur[0]    = 44.0;
        fx.b(P, 1).anim_dur[11]   = 55.0;

        sim_store own = fx.store();
        detail::clock_resync_units_and_buildings(fx.view(), own, 999.0);

        ck_eq_d(fx.u(P, 1).activity_clock, 11.0,
                "T1: not-alive player -- unit.activity_clock untouched, ALIVE gate 0x00499a73/0x00499a7a");
        ck_eq_d(fx.u(P, 1).rotation_clock, 22.0,
                "T1: not-alive player -- unit.rotation_clock untouched, same gate");
        ck_eq_d(fx.b(P, 1).last_tick_time, 33.0,
                "T1: not-alive player -- building.last_tick_time untouched, same gate");
        ck_eq_d(fx.b(P, 1).anim_dur[0], 44.0,
                "T1: not-alive player -- building.anim_dur[0] untouched, same gate");
        ck_eq_d(fx.b(P, 1).anim_dur[11], 55.0,
                "T1: not-alive player -- building.anim_dur[11] untouched, same gate");
    }

    // =================================================================================================
    // T2 -- ALIVE gate, POSITIVE arm: rewrite happens, BOTH unit clocks are written (independent
    // writes, so distinct pre-seed values catch a "only one of the two" bug), and all 12 anim_dur
    // slots are rewritten for a live building.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t P         = 1;
        fx.profiles[P].status_flags = 0x2u; // ALIVE (E_STRAT_PLAYER_STATUS bit1)

        fx.u(P, 0).unit_above[0]  = 1;
        fx.u(P, 0).unit_above[1]  = 0; // unit live-count header = 1
        fx.u(P, 1).unit_proto_id  = 7;
        fx.u(P, 1).activity_clock = 1.0; // distinct from rotation_clock's seed
        fx.u(P, 1).rotation_clock = 2.0; // -- a swap or a "wrote only one" bug disagrees here

        fx.b(P, 0).index          = 1; // building live-count header = 1
        fx.b(P, 1).building_id    = 8;
        fx.b(P, 1).last_tick_time = 3.0;
        for (int32_t k = 0; k < 12; ++k) fx.b(P, 1).anim_dur[k] = 100.0 + (double)k; // 12 distinct seeds

        const double new_time = 42.5;
        sim_store    own      = fx.store();
        detail::clock_resync_units_and_buildings(fx.view(), own, new_time);

        ck_eq_d(fx.u(P, 1).activity_clock, new_time, "T2: activity_clock = new_time, 0x00499ace-0x00499adf");
        ck_eq_d(fx.u(P, 1).rotation_clock, new_time,
                "T2: rotation_clock = new_time too -- independent write, 0x00499af0-0x00499aff");
        ck_eq_d(fx.b(P, 1).last_tick_time, new_time,
                "T2: building.last_tick_time = new_time, 0x00499b5f-0x00499b6f");
        for (int32_t k = 0; k < 12; ++k) {
            ck_eq_d(fx.b(P, 1).anim_dur[k], new_time,
                    "T2: anim_dur[k] = new_time for all 12 slots, 0x00499b78-0x00499bb2 (loop bound 0xc)");
        }
    }

    // =================================================================================================
    // T3 -- live-count headers of 0: an alive player whose slot-0 header says zero live records has
    // BOTH scan loops never execute their body, even though slot 1 carries a nonzero
    // unit_proto_id/building_id that would otherwise look live.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t P         = 3;
        fx.profiles[P].status_flags = 0x2u; // ALIVE

        fx.u(P, 0).unit_above[0]  = 0;
        fx.u(P, 0).unit_above[1]  = 0; // unit live-count header = 0
        fx.u(P, 1).unit_proto_id  = 9; // looks live, but the header says there are none
        fx.u(P, 1).activity_clock = 5.5;
        fx.u(P, 1).rotation_clock = 6.5;

        fx.b(P, 0).index          = 0; // building live-count header = 0
        fx.b(P, 1).building_id    = 10;
        fx.b(P, 1).last_tick_time = 7.5;
        fx.b(P, 1).anim_dur[0]    = 8.5;

        sim_store own = fx.store();
        detail::clock_resync_units_and_buildings(fx.view(), own, 999.25);

        ck_eq_d(fx.u(P, 1).activity_clock, 5.5,
                "T3: unit live_count=0 -> loop body never runs despite proto_id!=0, 0x00499a94/0x00499a9a");
        ck_eq_d(fx.u(P, 1).rotation_clock, 6.5, "T3: same case, rotation_clock also untouched");
        ck_eq_d(fx.b(P, 1).last_tick_time, 7.5,
                "T3: building live_count=0 -> loop body never runs despite building_id!=0, 0x00499b22/0x00499b28");
        ck_eq_d(fx.b(P, 1).anim_dur[0], 8.5, "T3: same case, anim_dur[0] also untouched");
    }

    // =================================================================================================
    // T4 -- the SCAN's subtle part, units side: live/empty/live at slots 1/2/3 with live_count=2. The
    // index starts at 1 and only DECREMENTS the remaining count when unit_proto_id != 0, so the empty
    // slot 2 must be walked over WITHOUT being consumed -- both live slots 1 and 3 get rewritten and
    // the empty slot 2 is left alone. (The scan itself is unbounded past slot 3 -- no compare against
    // UNITS_PER_PLAYER anywhere in this loop -- but live_count=2 is fully satisfied by slots 1 and 3,
    // so this case never walks past this fixture's own roster; the unbounded-scan behaviour is
    // preserved deliberately, not exercised here.)
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t P         = 4;
        fx.profiles[P].status_flags = 0x2u; // ALIVE

        fx.u(P, 0).unit_above[0]  = 2;
        fx.u(P, 0).unit_above[1]  = 0; // unit live-count header = 2
        fx.u(P, 1).unit_proto_id  = 1; // live
        fx.u(P, 1).activity_clock = 10.0;
        fx.u(P, 1).rotation_clock = 20.0;
        fx.u(P, 2).unit_proto_id  = 0; // EMPTY -- must be skipped without consuming live_count
        fx.u(P, 2).activity_clock = 30.0;
        fx.u(P, 2).rotation_clock = 40.0;
        fx.u(P, 3).unit_proto_id  = 2; // live
        fx.u(P, 3).activity_clock = 50.0;
        fx.u(P, 3).rotation_clock = 60.0;

        fx.b(P, 0).index = 0; // no building work in this case

        const double new_time = 111.5;
        sim_store    own      = fx.store();
        detail::clock_resync_units_and_buildings(fx.view(), own, new_time);

        ck_eq_d(fx.u(P, 1).activity_clock, new_time,
                "T4: slot1 (first live) rewritten, unit_proto_id!=0 gate 0x00499ab4/0x00499abc");
        ck_eq_d(fx.u(P, 1).rotation_clock, new_time, "T4: slot1 rotation_clock rewritten too");
        ck_eq_d(fx.u(P, 2).activity_clock, 30.0,
                "T4: EMPTY slot2 (unit_proto_id==0) left untouched, JZ 0x00499abc -- does not consume live_count");
        ck_eq_d(fx.u(P, 2).rotation_clock, 40.0, "T4: EMPTY slot2 rotation_clock also left untouched");
        ck_eq_d(fx.u(P, 3).activity_clock, new_time,
                "T4: slot3 (second live, reached by walking PAST the empty slot) rewritten -- live_count "
                "decremented only by non-empty records, 0x00499a94-0x00499b08");
        ck_eq_d(fx.u(P, 3).rotation_clock, new_time, "T4: slot3 rotation_clock rewritten too");
    }

    // =================================================================================================
    // T5 -- the same skip-empty scan shape, buildings side, PLUS: anim_dur is only written for a
    // non-empty building record (the 12-slot inner loop lives inside the building_id!=0 gate).
    // live/empty/live at slots 1/2/3, live_count=2.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t P         = 5;
        fx.profiles[P].status_flags = 0x2u; // ALIVE

        fx.u(P, 0).unit_above[0] = 0;
        fx.u(P, 0).unit_above[1] = 0; // no unit work in this case

        fx.b(P, 0).index          = 2; // building live-count header = 2
        fx.b(P, 1).building_id    = 1; // live
        fx.b(P, 1).last_tick_time = 10.0;
        for (int32_t k = 0; k < 12; ++k) fx.b(P, 1).anim_dur[k] = 100.0 + (double)k;
        fx.b(P, 2).building_id    = 0; // EMPTY -- must be skipped without consuming live_count
        fx.b(P, 2).last_tick_time = 99.0;
        fx.b(P, 2).anim_dur[0]    = 199.0;
        fx.b(P, 3).building_id    = 3; // live
        fx.b(P, 3).last_tick_time = 20.0;
        for (int32_t k = 0; k < 12; ++k) fx.b(P, 3).anim_dur[k] = 200.0 + (double)k;

        const double new_time = 77.75;
        sim_store    own      = fx.store();
        detail::clock_resync_units_and_buildings(fx.view(), own, new_time);

        ck_eq_d(fx.b(P, 1).last_tick_time, new_time,
                "T5: slot1 (first live) last_tick_time rewritten, building_id!=0 gate 0x00499b45/0x00499b4d");
        for (int32_t k = 0; k < 12; ++k) {
            ck_eq_d(fx.b(P, 1).anim_dur[k], new_time,
                    "T5: slot1 anim_dur[k] rewritten, all 12 slots, 0x00499b78-0x00499bb2 (bound 0xc)");
        }
        ck_eq_d(fx.b(P, 2).last_tick_time, 99.0,
                "T5: EMPTY slot2 (building_id==0) last_tick_time untouched, JZ 0x00499b4d -- does not consume "
                "live_count");
        ck_eq_d(fx.b(P, 2).anim_dur[0], 199.0,
                "T5: EMPTY slot2 anim_dur NOT written either -- the 12-slot loop is inside the building_id!=0 "
                "gate, 0x00499b71-0x00499bb4");
        ck_eq_d(fx.b(P, 3).last_tick_time, new_time,
                "T5: slot3 (second live, reached by walking PAST the empty slot) rewritten -- live_count "
                "decremented only by non-empty records, 0x00499b0a-0x00499bba");
        for (int32_t k = 0; k < 12; ++k) {
            ck_eq_d(fx.b(P, 3).anim_dur[k], new_time, "T5: slot3 anim_dur[k] rewritten, all 12 slots too");
        }
    }

    // =================================================================================================
    // T6 -- a NEGATIVE, FRACTIONAL new_time. Every store in this body is two raw 32-bit MOVs of the
    // parameter's stack dwords ([EBP+0x8]/[EBP+0xc]), with NOT ONE x87 instruction anywhere (the
    // header's FLOAT HANDLING note) -- -12345.5 is exactly representable and its IEEE-754 low and high
    // dwords are both nonzero and differ from each other, so a translation that copied only one half,
    // or swapped the two MOVs, disagrees here.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t P         = 6;
        fx.profiles[P].status_flags = 0x2u; // ALIVE

        fx.u(P, 0).unit_above[0]  = 1;
        fx.u(P, 0).unit_above[1]  = 0;
        fx.u(P, 1).unit_proto_id  = 4;
        fx.u(P, 1).activity_clock = 0.0;
        fx.u(P, 1).rotation_clock = 0.0;

        fx.b(P, 0).index          = 1;
        fx.b(P, 1).building_id    = 5;
        fx.b(P, 1).last_tick_time = 0.0;
        fx.b(P, 1).anim_dur[0]    = 0.0;
        fx.b(P, 1).anim_dur[11]   = 0.0;

        const double new_time = -12345.5;
        sim_store    own      = fx.store();
        detail::clock_resync_units_and_buildings(fx.view(), own, new_time);

        ck_eq_d(fx.u(P, 1).activity_clock, new_time,
                "T6: activity_clock = new_time verbatim for a negative fractional value, "
                "raw dword copy no x87, 0x00499ace-0x00499adf");
        ck_eq_d(fx.u(P, 1).rotation_clock, new_time,
                "T6: rotation_clock = new_time verbatim too, 0x00499af0-0x00499aff");
        ck_eq_d(fx.b(P, 1).last_tick_time, new_time,
                "T6: building.last_tick_time = new_time verbatim, 0x00499b5f-0x00499b6f");
        ck_eq_d(fx.b(P, 1).anim_dur[0], new_time, "T6: anim_dur[0] = new_time verbatim, 0x00499ba0-0x00499bb1");
        ck_eq_d(fx.b(P, 1).anim_dur[11], new_time, "T6: anim_dur[11] (last slot) = new_time verbatim too");
    }
}

} // namespace mh::sim::test
