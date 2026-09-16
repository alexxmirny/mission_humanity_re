//
// tact_unit_cmd_advance_with_defstat_selftest.cpp -- offline oracle for
//   llm_tact_unit_cmd_advance_with_defstat @0x0042f8e4 (libmh/tact/tact_unit_cmd_advance_with_defstat.cpp)
//
// WHY OFFLINE, NOT RIG: op 0xb is genuinely unreachable by migration_sweep.py's TACT-SYNTH workload
// -- re-derived from the listings this slice (see the header banner in
// tact_unit_cmd_advance_with_defstat.h for the full call-site trace): every PLAYER-command enqueue
// site passes a different hardcoded op or one of the UI button set's ops, and the one remaining site
// (llm_tact_mission_load) sources its op from the mission SCRIPT's own data, not from any player
// action or synthesizable input. Same shape as llm_tact_squad_sync_hp
// (tact_squad_status_selftest.cpp): armable, but no available scenario reaches it, so this proves the
// function's own logic offline instead, mocking the one outward call
// (llm_tact_unit_cmd_advance, frontier, Law 4) via the file's own `unit_cmd_advance_with_defstat_calls`
// table.
//
#include "tact/tact_unit_cmd_advance_with_defstat.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

using namespace mh::tact;

constexpr int32_t UNIT_IDX = 3;
constexpr int32_t SLOT_IDX = 5;

struct ev2 {
    int32_t unit_idx, cmd_slot_index;
};

struct advance_recorder {
    std::vector<ev2> calls;
    void             reset() { *this = advance_recorder{}; }
};
advance_recorder g_rec;

const unit_cmd_advance_with_defstat_calls &rec_calls() {
    static const unit_cmd_advance_with_defstat_calls c = {
        [](int32_t unit_idx, int32_t cmd_slot_index) {
            g_rec.calls.push_back({unit_idx, cmd_slot_index});
        },
    };
    return c;
}

} // namespace

void run_unit_cmd_advance_with_defstat_tests() {
    // T1: def_stat := LOW BYTE of cmd_queue[slot].arg0, 0x0042f915-0x0042f91b -- a full-word read
    // would carry the high byte through too, so arg0 is chosen non-trivial in both halves.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg0 = 0x1234;
        fx.units[UNIT_IDX].def_stat                 = 0xff; // sentinel, must be overwritten
        tact_store own                              = fx.store();
        detail::unit_cmd_advance_with_defstat(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].def_stat, 0x34u,
              "T1: def_stat = (uint8_t)arg0, truncates the high byte, 0x0042f915-0x0042f91b");
    }

    // T2: def_stat truncation from the OTHER direction -- a low byte of 0 must not be read as
    // "unchanged" (catches a translation that skips the write when the truncated value is falsy).
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg0 = 0x0700;
        fx.units[UNIT_IDX].def_stat                 = 0xaa;
        tact_store own                              = fx.store();
        detail::unit_cmd_advance_with_defstat(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)fx.units[UNIT_IDX].def_stat, 0x00u,
              "T2: low byte 0 is still WRITTEN, not skipped, 0x0042f915-0x0042f91b");
    }

    // T3: llm_tact_unit_cmd_advance is dispatched exactly once with (unit_idx, cmd_slot_index)
    // forwarded UNCHANGED, 0x0042f921-0x0042f927 -- deliberately different unit/slot values so a
    // swapped-argument translation fails.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg0 = 1;
        tact_store own                              = fx.store();
        detail::unit_cmd_advance_with_defstat(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)g_rec.calls.size(), 1u, "T3: llm_tact_unit_cmd_advance fires exactly once");
        if (g_rec.calls.size() == 1) {
            ck_eq((uint32_t)g_rec.calls[0].unit_idx, (uint32_t)UNIT_IDX,
                  "T3: unit_idx forwarded unchanged, 0x0042f921");
            ck_eq((uint32_t)g_rec.calls[0].cmd_slot_index, (uint32_t)SLOT_IDX,
                  "T3: cmd_slot_index forwarded unchanged, 0x0042f927");
        }
    }

    // T4: the dispatch happens AFTER the def_stat write (order matters -- if a translation swapped
    // the two, a callee reading def_stat mid-call would see the stale value). The mock cannot observe
    // program order directly, so this checks the ONLY externally-visible consequence of order here:
    // def_stat must already hold the NEW value by the time the call fires.
    {
        tact_fixture fx;
        g_rec.reset();
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg0                = 0x42;
        fx.units[UNIT_IDX].def_stat                                = 0;
        static tact_unit                         *observed_unit    = &fx.units[UNIT_IDX];
        static uint8_t                            def_stat_at_call = 0xff;
        const unit_cmd_advance_with_defstat_calls order_calls      = {
            [](int32_t, int32_t) { def_stat_at_call = observed_unit->def_stat; },
        };
        tact_store own = fx.store();
        detail::unit_cmd_advance_with_defstat(own, order_calls, UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)def_stat_at_call, 0x42u,
              "T4: def_stat write @0x0042f91b happens BEFORE the dispatch @0x0042f921");
    }
}

} // namespace mh::tact::test
