//
// tact_unit_cmd_stance_selftest.cpp -- offline oracle for
//   llm_tact_unit_cmd_stance_on  @0x00430580 (libmh/tact/tact_unit_cmd_stance.cpp)
//   llm_tact_unit_cmd_stance_off @0x0043060f
//
// WHY OFFLINE, NOT THE RIG: both sites ARE armed and reachable, but every --tact-synth run shows a
// 100% divergence rate on _G_LLM_TACT_UNITS's wander_check_time field (reached through the
// dispatched llm_tact_unit_cmd_advance whenever the newly-advanced cmd_index slot is empty) -- a
// live-clock-read hazard, not a translation bug (see tact_unit_cmd_stance.h's header banner for the
// full derivation, including the llm_tact_unit_owner_tick control run that confirmed the shadow
// harness has a scenario-dependent hazard live in this domain independent of this function). This
// proves the STATUS BIT and the DISPATCH-SKIP CONDITION -- the function's actual logic -- by mocking
// the one outward call, deliberately asserting nothing about wander_check_time's value.
//
#include "tact/tact_unit_cmd_stance.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

using namespace mh::tact;

constexpr int32_t UNIT_IDX = 2;

struct ev2 {
    int32_t unit_idx, cmd_slot_index;
};

struct advance_recorder {
    std::vector<ev2> calls;
    void             reset() { *this = advance_recorder{}; }
};
advance_recorder g_rec;

const unit_cmd_stance_calls &rec_calls() {
    static const unit_cmd_stance_calls c = {
        [](int32_t unit_idx, int32_t cmd_slot_index) {
            g_rec.calls.push_back({unit_idx, cmd_slot_index});
        },
    };
    return c;
}

} // namespace

void run_unit_cmd_stance_tests() {
    // T1: stance_on sets status bit 0x4, 0x0043059d-0x004305b4 -- other bits untouched.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].status          = 0x11; // bits 0x10/0x1 set, 0x4 clear
        fx.units[UNIT_IDX].cmd_index       = 0;
        fx.units[UNIT_IDX].cmd_queue[0].op = 0x7; // not the 0x1e marker -> NO dispatch (see T3)
        fx.units[UNIT_IDX].progress        = 0;
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_on(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].status, 0x15u,
              "T1: status |= 0x4, other bits preserved, 0x0043059d-0x004305b4");
    }

    // T2: stance_off clears status bit 0x4, 0x0043062c-0x00430643 -- other bits untouched.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].status          = 0x15;
        fx.units[UNIT_IDX].cmd_index       = 0;
        fx.units[UNIT_IDX].cmd_queue[0].op = 0x7; // not the 0x1c marker -> NO dispatch (see T6)
        fx.units[UNIT_IDX].progress        = 0;
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_off(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].status, 0x11u,
              "T2: status &= ~0x4, other bits preserved, 0x0043062c-0x00430643");
    }

    // T3: stance_on does NOT dispatch when the head slot is not its own marker.
    //
    // THE WHOLE POLARITY OF T3-T7 WAS INVERTED UNTIL 2026-09-04 (TACT1-P C5), together with the
    // body they check -- which is why 8494 offline checks passed over it. Read the jumps to their
    // targets: `CMP [op],0x1e / JNZ 0x004305ee` sends op != 0x1e to LAB_004305ee, whose entire body
    // is `JMP 0x00430606`, and 0x00430606 is the EPILOGUE (`LEA ESP,[EBP-0x10]` + five POPs + RET).
    // Only `CMP [progress],0 / JZ 0x004305f0` reaches the CALL. So the dispatch fires on
    // (op == marker && progress == 0) and on nothing else: the marker means THIS stance command is
    // the one at the head and has not started, and the call is what advances the queue past it.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_index       = 5;
        fx.units[UNIT_IDX].cmd_queue[5].op = 0x40; // some other op
        fx.units[UNIT_IDX].progress        = 0;
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_on(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)g_rec.calls.size(), 0u,
              "T3: NO dispatch when head op != 0x1e (JNZ -> the epilogue), 0x004305d4-0x004305dc");
    }

    // T4: stance_on DISPATCHES when head op == 0x1e AND progress == 0 -- its own marker, not yet
    // started, so the queue is advanced past it. The forwarded arguments are checked here, on the
    // one path that actually calls.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_index       = 3;
        fx.units[UNIT_IDX].cmd_queue[3].op = 0x1e;
        fx.units[UNIT_IDX].progress        = 0;
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_on(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)g_rec.calls.size(), 1u,
              "T4: dispatch FIRES, op==0x1e && progress==0, 0x004305f0-0x00430601");
        if (g_rec.calls.size() == 1) {
            ck_eq((uint32_t)g_rec.calls[0].unit_idx, (uint32_t)UNIT_IDX, "T4: unit_idx forwarded");
            ck_eq((uint32_t)g_rec.calls[0].cmd_slot_index, 3u, "T4: cmd_index forwarded as cmd_slot_index");
        }
    }

    // T5: stance_on does NOT dispatch when head op == 0x1e but progress != 0 -- the marker's turn
    // has already started, so there is nothing to advance past. Catches an AND-vs-OR polarity bug in
    // the condition.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_index       = 3;
        fx.units[UNIT_IDX].cmd_queue[3].op = 0x1e;
        fx.units[UNIT_IDX].progress        = 7; // nonzero -- marker already in progress
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_on(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)g_rec.calls.size(), 0u,
              "T5: NO dispatch when op==0x1e but progress!=0, 0x004305e5-0x004305ec");
    }

    // T6: stance_off DISPATCHES when head op == 0x1c AND progress == 0 -- the mirror's own marker,
    // distinct from stance_on's 0x1e (catches a marker-constant mix-up between the two).
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_index       = 1;
        fx.units[UNIT_IDX].cmd_queue[1].op = 0x1c;
        fx.units[UNIT_IDX].progress        = 0;
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_off(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)g_rec.calls.size(), 1u,
              "T6: dispatch FIRES, op==0x1c && progress==0 (stance_off's own marker), 0x0043067f-0x00430690");
    }

    // T7: stance_off does NOT dispatch on stance_on's marker (0x1e) -- the two markers must not be
    // cross-recognised.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_index       = 1;
        fx.units[UNIT_IDX].cmd_queue[1].op = 0x1e; // the OTHER function's marker
        fx.units[UNIT_IDX].progress        = 0;
        tact_store own                     = fx.store();
        detail::unit_cmd_stance_off(own, rec_calls(), UNIT_IDX);
        ck_eq((uint32_t)g_rec.calls.size(), 0u,
              "T7: stance_off does not recognise stance_on's 0x1e marker, so it does NOT dispatch");
    }
}

} // namespace mh::tact::test
