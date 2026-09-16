//
// tact_unit_stand_tick_selftest.cpp -- offline oracle for
//   llm_tact_unit_stand_tick @0x0043047d (libmh/tact/tact_unit_stand_tick.h)
//
// WHY OFFLINE, NOT RIG: the manifest classifies this function RIG-armable (2 regions:
// _G_LLM_TACT_UNITS + _G_LLM_STRAT_PATH_SLOT_FLAGS via unit_cmd_advance), and gen_shadow_ini.py
// does arm it -- but the only scenario that reaches this site at all is --tact-synth (the default
// save/steps combination reads 0 calls, per migration_sweep.py), and --tact-synth is EXACTLY the
// scenario in which a shadow-armed field fed by time_GetCurrentTime diverges 100% of
// calls under --tact-synth" entry already spent three hypotheses on without resolving:
// llm_tact_unit_cmd_advance (this function's own callee on both dequeue arms) writes
// `wander_check_time`, one of the two fields already known to be in that unresolved class. Per
// that entry's own instruction ("do not treat a clean shadow verdict... as proof the hazard is
// absent" / a fourth hypothesis needs direct instrumentation of the two reads) this function is
// routed to the SAME offline treatment already used for llm_tact_unit_cmd_stance_on/_off: the two
// outward callees are mocked via the calls struct, so the real wander_check_time write is never
// reached -- this oracle proves stand_tick's OWN branching (kneel/turn dequeue-watch vs. the
// progress-driven anim cycle, the 0xf threshold, the re-read-after-call discipline shared with
// kneel_tick's sibling shape) and says nothing about unit_cmd_advance's body, which is verified
// separately.
//
#include "tact/tact_unit_stand_tick.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct stand_recorder {
    std::vector<int32_t> set_anim_states; // the uint8_t state arg per set_anim_state call
    std::vector<int32_t> set_anim_units;
    std::vector<int32_t> cmd_advance_units;
    std::vector<int32_t> cmd_advance_slots;
    void                 reset() { *this = stand_recorder{}; }
    int32_t              total() { return (int32_t)(set_anim_states.size() + cmd_advance_units.size()); }
};
stand_recorder g_rec;

// T6's set_anim_state mock writes back through this -- the real callee is what moves anim_state,
// and a multi-tick case that never advances it would just re-enter the same arm forever.
tact_unit *g_anim_subject = nullptr;

const unit_stand_tick_calls &rec_calls() {
    static const unit_stand_tick_calls c = {
        [](int32_t unit_id, uint8_t state) {
            g_rec.set_anim_units.push_back(unit_id);
            g_rec.set_anim_states.push_back((int32_t)state);
        },
        [](int32_t unit_id, int32_t cmd_index) {
            g_rec.cmd_advance_units.push_back(unit_id);
            g_rec.cmd_advance_slots.push_back(cmd_index);
        },
    };
    return c;
}

} // namespace

void run_unit_stand_tick_tests() {
    // THE POLARITY THESE FIVE CASES PIN WAS INVERTED UNTIL 2026-09-04, in the body AND here: they
    // asserted the dequeue-only arm for anim_state 2/3 and the animation arm for everything else,
    // which is backwards (see the header's jump-target derivation). An oracle written from the same
    // misreading as the translation cannot falsify it -- the bug shipped and was found by PLAYING.
    // T6 below is the end-to-end assertion whose absence let it through.

    // T1: anim_state==0 (NOT mid kneel/turn), head op==5 (a STAND with nothing to animate) ->
    // exactly one cmd_advance(unit_id, cmd_index), NO set_anim_state, progress untouched.
    // 0x004304bc-0x004304f1.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 11;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 0;
        u.progress                = 200; // must survive untouched -- proves this arm never reaches step 2/3
        u.cmd_index               = 9;
        u.cmd_queue[9].op         = 5;
        u.cmd_queue[0].op         = 5; // decoy: indexing must use cmd_index (9), not slot 0

        tact_store own = fx.store();
        detail::unit_stand_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)g_rec.cmd_advance_units.size(), 1u, "T1: exactly one dequeue, 0x004304f1");
        if (g_rec.cmd_advance_slots.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance_slots[0], 9u, "T1: dequeue got cmd_index (9), not the decoy slot");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 0u, "T1: no set_anim_state on the dequeue-only arm");
        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 200u, "T1: progress untouched, 0x004304f6 early return");
    }

    // T2: anim_state==1 (another value outside the kneel/turn pair) takes the same dequeue-only
    // arm. 0x004304a8-0x004304b8.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 22;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 1;
        u.cmd_index               = 4;
        u.cmd_queue[4].op         = 0; // op != 5 -> no dequeue, but still an early return, zero calls

        tact_store own = fx.store();
        detail::unit_stand_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)g_rec.total(), 0u, "T2: anim_state==1, op!=5 -- early return, zero calls, 0x004304de");
    }

    // T3: anim_state==2 (mid kneel/turn), progress starts BELOW the threshold -- increments by
    // one, one set_anim_state(unit,3), returns before the 0xf check's fall-through path.
    // 0x004304fb-0x00430523.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 13;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 2;
        u.progress                = 5;
        u.cmd_index               = 2;
        u.cmd_queue[2].op         = 5; // must NOT be dequeued -- this arm never reaches the tail check

        tact_store own = fx.store();
        detail::unit_stand_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 6u, "T3: progress incremented, 0x00430502");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 1u, "T3: exactly one set_anim_state");
        if (g_rec.set_anim_states.size() == 1)
            ck_eq((uint32_t)g_rec.set_anim_states[0], 3u, "T3: ...with state 3, 0x00430510");
        ck_eq((uint32_t)g_rec.cmd_advance_units.size(), 0u, "T3: no dequeue below the threshold");
    }

    // T3b: the 0xf threshold is INCLUSIVE from the low side -- progress 14 -> 15 (== 0xf) still
    // takes the early-return (JBE), not the reset arm. 0x0043051c/0x00430523.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 14;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 3; // the other kneel/turn value takes the same arm
        u.progress                = 14;

        tact_store own = fx.store();
        detail::unit_stand_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 15u, "T3b: 15 == 0xf stays on the early-return side");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 1u, "T3b: only the state-3 call, no reset");
    }

    // T4: progress 15 -> 16 (> 0xf): reset to 0, second set_anim_state(0), then the tail dequeue
    // fires because the head op is 5. Order [3, 0] asserted. 0x00430525-0x00430572.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 15;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 3;
        u.progress                = 15;
        u.cmd_index               = 3;
        u.cmd_queue[3].op         = 5;

        tact_store own = fx.store();
        detail::unit_stand_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T4: progress reset, 0x0043052c");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 2u, "T4: two set_anim_state calls");
        if (g_rec.set_anim_states.size() == 2) {
            ck_eq((uint32_t)g_rec.set_anim_states[0], 3u, "T4: first with 3, 0x00430510");
            ck_eq((uint32_t)g_rec.set_anim_states[1], 0u, "T4: then with 0, 0x00430538");
        }
        ck_eq((uint32_t)g_rec.cmd_advance_slots.size(), 1u, "T4: tail dequeue fired, 0x00430572");
        if (g_rec.cmd_advance_slots.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance_slots[0], 3u, "T4: ...with cmd_index (3)");
    }

    // T5: same reset arm, but the head op is NOT 5 -- no tail dequeue. Proves the tail check is a
    // real gate, not an unconditional dequeue once the reset fires. 0x0043055f.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t UNIT_ID = 16;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 2;
        u.progress                = 15;
        u.cmd_index               = 7;
        u.cmd_queue[7].op         = 1; // != 5

        tact_store own = fx.store();
        detail::unit_stand_tick(own, rec_calls(), UNIT_ID);

        ck_eq((uint32_t)own.unit_at(UNIT_ID).progress, 0u, "T5: progress still reset");
        ck_eq((uint32_t)g_rec.set_anim_states.size(), 2u, "T5: both set_anim_state calls still fire");
        ck_eq((uint32_t)g_rec.cmd_advance_units.size(), 0u, "T5: no dequeue -- head op != 5, 0x0043055f");
    }

    // T6: THE BEHAVIOUR, not the branch -- A KNEELING UNIT STANDS BACK UP. Every case above is a
    // single call asserting one arm, and five of them passed while the two arms were swapped,
    // because each was written from the same misreading as the body. This one asserts the mechanism
    // the player sees: a unit parked in the kneeled state (anim_state 3, progress 0, which is what
    // kneel_tick leaves behind) driven the way the frame pump drives it must reach anim_state 0
    // within the animation's own length, and must dequeue its STAND when it gets there. Under the
    // inverted polarity it sat in anim_state 3 forever and never called set_anim_state at all.
    //
    // The mock feeds set_anim_state's writes back into the unit, since the real callee is what
    // moves anim_state -- without that the loop would re-enter the same arm on a stale value and
    // prove nothing about progression.
    {
        tact_fixture      fx;
        constexpr int32_t UNIT_ID = 17;
        tact_unit        &u       = fx.units[UNIT_ID];
        u.anim_state              = 3; // kneeled: kneel_tick's terminal state
        u.progress                = 0;
        u.cmd_index               = 6;
        u.cmd_queue[6].op         = 5; // the STAND order, still at the head

        tact_store own = fx.store();
        g_anim_subject = &own.unit_at(UNIT_ID); // the mock's only route back to the unit
        g_rec.reset();
        const unit_stand_tick_calls live = {
            [](int32_t unit_id, uint8_t state) {
                g_rec.set_anim_units.push_back(unit_id);
                g_rec.set_anim_states.push_back((int32_t)state);
                g_anim_subject->anim_state = state;
            },
            [](int32_t unit_id, int32_t cmd_index) {
                g_rec.cmd_advance_units.push_back(unit_id);
                g_rec.cmd_advance_slots.push_back(cmd_index);
            },
        };

        int32_t stood_at = -1;
        for (int32_t frame = 0; frame < 64 && stood_at < 0; ++frame) {
            detail::unit_stand_tick(own, live, UNIT_ID);
            if (own.unit_at(UNIT_ID).anim_state == 0) stood_at = frame;
        }

        ck(stood_at >= 0, "T6: a kneeling unit reaches anim_state 0 -- it STANDS UP");
        ck_eq((uint32_t)stood_at, 15u, "T6: ...after 16 ticks (progress 0 -> 16 crosses 0xf)");
        ck_eq((uint32_t)g_rec.cmd_advance_slots.size(), 1u, "T6: and the STAND order is dequeued once");
        if (g_rec.cmd_advance_slots.size() == 1)
            ck_eq((uint32_t)g_rec.cmd_advance_slots[0], 6u, "T6: ...at its own cmd_index (6)");
    }
}

} // namespace mh::tact::test
