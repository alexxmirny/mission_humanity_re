//
// tact_unit_kneel_tick_selftest.cpp -- offline oracle for
//   llm_tact_unit_kneel_tick @0x00430363 (libmh/tact/tact_unit_kneel_tick.h)
//
// FULL COVERAGE (2026-08-26, second revision). The first revision could only exercise the two pure
// prefix arms: the TU then called `mh::call::llm_tact_unit_set_anim_state` / `llm_tact_unit_cmd_
// advance` DIRECTLY, and its shadow site was expected to be the primary instrument. The shadow
// generator then derived an EMPTY region set for the site (gen_dll_shadow.py classifies it VACUOUS;
// gen_shadow_ini.py refuses to arm it), so the rig can produce no evidence here at all -- the TU was
// refactored to the `unit_kneel_tick_calls` struct and this oracle now mocks both callees and covers
// every arm, including the two re-read-after-call disciplines.
//
// DERIVATION (tmp/decomp_tact/llm_tact_unit_kneel_tick_00430363.asm):
//   1. @0x00430380-0x0043038e: anim_state==3 -> return, zero writes, zero calls.
//   2. @0x00430394-0x004303b2: anim_state==2 AND progress==0 -> step 3; anything else -> step 4.
//   3. @0x004303b6-0x004303f0: cmd_queue[cmd_index].op==4 -> cmd_advance(unit_id, cmd_index); return
//      either way.
//   4. @0x004303f5-0x00430474: progress+=1 (byte); set_anim_state(unit_id,2); RE-READ progress after
//      the call; <=0xf -> return; else progress=0; set_anim_state(unit_id,3); RE-READ cmd_index/op
//      after the second call; op==4 -> cmd_advance(unit_id, cmd_index).
//
#include "tact/tact_unit_kneel_tick.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct kneel_recorder {
    std::vector<int32_t> set_anim_states;   // the uint8_t state arg per set_anim_state call
    std::vector<int32_t> set_anim_units;    // the unit arg per set_anim_state call
    std::vector<int32_t> cmd_advance_units; // args per cmd_advance call
    std::vector<int32_t> cmd_advance_slots;
    // Optional call-time fixture mutations, to pin the two re-read-after-call disciplines.
    tact_fixture *fx            = nullptr;
    int32_t       mutate_unit   = -1;
    int32_t       set2_progress = -1; // if >=0: set_anim_state(...,2) writes this into u.progress
    int32_t       set3_cmdidx   = -1; // if >=0: set_anim_state(...,3) repoints u.cmd_index here
    int32_t       set3_op       = -1; //         ...and writes this op at the NEW head
    void          reset() { *this = kneel_recorder{}; }
    int32_t       total() { return (int32_t)(set_anim_states.size() + cmd_advance_units.size()); }
};
kneel_recorder g_rec;

const unit_kneel_tick_calls &rec_calls() {
    static const unit_kneel_tick_calls c = {
        [](int32_t unit_id, uint8_t state) {
            g_rec.set_anim_units.push_back(unit_id);
            g_rec.set_anim_states.push_back((int32_t)state);
            if (g_rec.fx != nullptr && g_rec.mutate_unit >= 0) {
                tact_unit &u = g_rec.fx->units[(size_t)g_rec.mutate_unit];
                if (state == 2 && g_rec.set2_progress >= 0)
                    u.progress = (uint8_t)g_rec.set2_progress;
                if (state == 3 && g_rec.set3_cmdidx >= 0) {
                    u.cmd_index                               = (uint8_t)g_rec.set3_cmdidx;
                    u.cmd_queue[(size_t)g_rec.set3_cmdidx].op = (uint8_t)g_rec.set3_op;
                }
            }
        },
        [](int32_t unit_id, int32_t cmd_index) {
            g_rec.cmd_advance_units.push_back(unit_id);
            g_rec.cmd_advance_slots.push_back(cmd_index);
        },
    };
    return c;
}

} // namespace

void run_unit_kneel_tick_tests() {
    // T1: anim_state==3 -- early return, zero writes, zero calls. 0x00430387/0x0043038e.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 11;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 3;
        u.progress                = 200;
        u.cmd_index               = 9;
        u.cmd_queue[9].op         = 4; // trap: would be dequeued by step 3 if arm (1) fell through
        u.status                  = 0x55;

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).anim_state, 3u,
              "T1: anim_state==3 unchanged, 0x00430387/0x0043038e");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 200u, "T1: progress untouched");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).cmd_queue[9].op, 4u, "T1: queue head left QUEUED");
        ck_eq((uint32_t)g_rec.total(), 0u, "T1: zero outward calls");
    }

    // T2: anim_state==2, progress==0, head op==0 -- op!=4 sub-arm, no call; slot-0 decoy proves
    // cmd_index indexing. 0x004303c4-0x004303d8/0x004303f0.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 22;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 2;
        u.progress                = 0;
        u.cmd_index               = 6;
        u.cmd_queue[6].op         = 0;
        u.cmd_queue[0].op         = 4; // DECOY

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T2: progress untouched (op!=4 sub-arm)");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).cmd_queue[0].op, 4u,
              "T2: decoy at slot 0 untouched -- indexing uses cmd_index (6)");
        ck_eq((uint32_t)g_rec.total(), 0u, "T2: zero outward calls");
    }

    // T3/T4: the op==4 EQUALITY pinned from both sides (3 and 5), at cmd_index 127 and 1.
    // 0x004303d0/0x004303d8.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 33;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 2;
        u.progress                = 0;
        u.cmd_index               = 127;
        u.cmd_queue[127].op       = 3;
        u.cmd_queue[0].op         = 4;
        tact_store own            = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);
        ck_eq((uint32_t)g_rec.total(), 0u, "T3: op==3 (below KNEEL) -- no dequeue, no calls");
    }
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 44;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 2;
        u.progress                = 0;
        u.cmd_index               = 1;
        u.cmd_queue[1].op         = 5;
        u.cmd_queue[0].op         = 4;
        tact_store own            = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);
        ck_eq((uint32_t)g_rec.total(), 0u, "T4: op==5 (above KNEEL) -- no dequeue, no calls");
    }

    // T5: the step-3 dequeue -- anim_state==2, progress==0, head op==4 -> exactly one
    // cmd_advance(unit_id, cmd_index), no set_anim_state, no writes. 0x004303eb.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 12;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 2;
        u.progress                = 0;
        u.cmd_index               = 5;
        u.cmd_queue[5].op         = 4;

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)g_rec.cmd_advance_units.size(), 1u, "T5: exactly one dequeue, 0x004303eb");
        if (g_rec.cmd_advance_slots.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance_slots[0], 5u, "T5: dequeue got cmd_index (5)");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 0u, "T5: no set_anim_state on this arm");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T5: progress untouched");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).anim_state, 2u, "T5: anim_state untouched");
    }

    // T6: step 4, below the threshold -- anim_state==0, progress 5 -> 6 (<=0xf), one
    // set_anim_state(2), return. 0x004303f5-0x00430416.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 13;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 0;
        u.progress                = 5;
        u.cmd_index               = 2;
        u.cmd_queue[2].op         = 4; // must NOT be dequeued on the early-return path

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 6u, "T6: progress incremented, 0x004303f5");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 1u, "T6: one set_anim_state");
        if (g_rec.set_anim_states.size() == 1)
            ck_eq((uint32_t)g_rec.set_anim_states[0], 2u, "T6: ...with state 2, 0x0043040a");
        ck_eq((uint32_t)g_rec.cmd_advance_units.size(), 0u, "T6: no dequeue below the threshold");
    }

    // T6b: the 0xf threshold is INCLUSIVE -- progress 14 -> 15 (== 0xf) still returns early.
    // 0x00430416 (JLE-shape per the .asm).
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 14;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 0;
        u.progress                = 14;

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 15u, "T6b: 15 == 0xf stays below-or-equal");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 1u, "T6b: only the state-2 call");
    }

    // T7: the full 2->3 transition -- progress 15 -> 16 (>0xf): reset to 0, set_anim_state(3), then
    // the tail dequeue when the head is op==4. Order [2,3] asserted. 0x00430420-0x00430474.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 15;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 0;
        u.progress                = 15;
        u.cmd_index               = 3;
        u.cmd_queue[3].op         = 4;

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T7: progress reset, 0x00430420");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 2u, "T7: two set_anim_state calls");
        if (g_rec.set_anim_states.size() == 2) {
            ck_eq((uint32_t)g_rec.set_anim_states[0], 2u, "T7: first with 2, 0x0043040a");
            ck_eq((uint32_t)g_rec.set_anim_states[1], 3u, "T7: then with 3, 0x0043042e");
        }
        ck_eq((uint32_t)g_rec.cmd_advance_slots.size(), 1u, "T7: tail dequeue fired, 0x0043046f");
        if (g_rec.cmd_advance_slots.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance_slots[0], 3u, "T7: ...with cmd_index (3)");
    }

    // T8: the RE-READ-AFTER-CALL disciplines, both of them, each pinned by a mock that flips the
    // branch the cached pre-call value would have taken (0x0043040f/0x00430416 and
    // 0x0043043a/0x00430441):
    // (a) progress 3 -> 4 would return early, but set_anim_state(2)'s mock writes progress=0x40 --
    //     the re-read must see 0x40 and PROCEED to the 2->3 transition.
    // (b) the head at the original cmd_index is op==0, but set_anim_state(3)'s mock repoints
    //     cmd_index to a fresh slot holding op==4 -- the tail dequeue must fire with the NEW index.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 16;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 0;
        u.progress                = 3;
        u.cmd_index               = 8;
        u.cmd_queue[8].op         = 0; // pre-mutation head: would NOT dequeue
        g_rec.fx                  = &fx;
        g_rec.mutate_unit         = UNIT_ID;
        g_rec.set2_progress       = 0x40; // flips the threshold branch
        g_rec.set3_cmdidx         = 20;   // repoints the head...
        g_rec.set3_op             = 4;    // ...to a dequeueable op

        tact_store own = fx.store();
        detail::unit_kneel_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)g_rec.set_anim_states.size(), 2u,
              "T8: mutated progress (0x40) re-read after the state-2 call forced the transition");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T8: progress reset on the forced path");
        ck_eq((uint32_t)g_rec.cmd_advance_slots.size(), 1u,
              "T8: tail dequeue fired off the RE-READ head, 0x0043043a/0x00430441");
        if (g_rec.cmd_advance_slots.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance_slots[0], 20u,
                  "T8: ...with the MUTATED cmd_index (20), not the cached 8");
        // (b2) the mirror: pre-call progress 15 would transition, but the mock caps it back low.
        g_rec.reset();
        tact_fixture fx2;
        tact_unit   &u2     = fx2.units[UNIT_ID];
        u2.anim_state       = 0;
        u2.progress         = 15;
        g_rec.fx            = &fx2;
        g_rec.mutate_unit   = UNIT_ID;
        g_rec.set2_progress = 1; // flips the branch the other way
        tact_store own2     = fx2.store();
        detail::unit_kneel_tick(own2, rec_calls(), UNIT_ID);
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 1u,
              "T8b: mutated progress (1) re-read after the state-2 call suppressed the transition");
        ck_eq((uint32_t)own2.unit_at(UNIT_ID).progress, 1u, "T8b: the mock's value survives");
    }
    g_rec.reset();
}

} // namespace mh::tact::test
