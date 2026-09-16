//
// tact_unit_cmd_teleport_jump_tick_selftest.cpp -- offline oracle for
//   llm_tact_unit_cmd_teleport_jump_tick @0x00433464 (libmh/tact/tact_unit_cmd_teleport_jump_tick.cpp)
//
// WHY OFFLINE, NOT RIG: llm_tact_teleport_cmdqueue_jump calls llm_tact_unit_teleport directly on
// success, whose measured write closure reaches two TACT-CUT2 SHARED
// callees tools/data/tact_shared_callees.json already classifies `effectful` -- llm_tact_fx_play_sound
// (starts real DirectSound playback) and llm_tact_ui_sidebar_roster_refresh (blits into the live UI
// surface). Both are ungated (the open, non-autonomous TACT-CUT2 item). Arming this function's entry
// would risk a real double-fired sound and a real double blit under shadow's snapshot/restore. So
// both outward calls are mocked via the file's own `unit_cmd_teleport_jump_tick_calls` table --
// proving the dispatch logic without ever executing the real teleport or anything beneath it.
//
#include "tact/tact_unit_cmd_teleport_jump_tick.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_IDX = 4;
constexpr int32_t SLOT_IDX = 6;

struct jump_recorder {
    std::vector<std::tuple<int32_t, int32_t, int32_t>> teleport_calls; // (unit_id, dest_x, dest_y)
    std::vector<std::tuple<int32_t, int32_t>>          advance_calls;  // (unit_idx, cmd_slot_index)
    int32_t                                            teleport_result = 0;
    void                                               reset() { *this = jump_recorder{}; }
};
jump_recorder g_rec;

const unit_cmd_teleport_jump_tick_calls &rec_calls() {
    static const unit_cmd_teleport_jump_tick_calls c = {
        [](int32_t unit_id, int32_t dest_x, int32_t dest_y) -> int32_t {
            g_rec.teleport_calls.push_back({unit_id, dest_x, dest_y});
            return g_rec.teleport_result;
        },
        [](int32_t unit_idx, int32_t cmd_slot_index) {
            g_rec.advance_calls.push_back({unit_idx, cmd_slot_index});
        },
    };
    return c;
}

} // namespace

void run_unit_cmd_teleport_jump_tick_tests() {
    // T1: dest_x/dest_y are read from cmd_queue[slot].arg0/.arg1 and forwarded to
    // teleport_cmdqueue_jump as (unit_idx, dest_x, dest_y), 0x0043348e-0x004334b8. Deliberately
    // distinct, non-symmetric values so a swapped-argument translation fails.
    {
        tact_fixture fx;
        g_rec.reset();
        g_rec.teleport_result                       = 0;
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg0 = 11;
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg1 = 22;
        tact_store own                              = fx.store();
        detail::unit_cmd_teleport_jump_tick(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)g_rec.teleport_calls.size(), 1u,
              "T1: llm_tact_teleport_cmdqueue_jump fires exactly once");
        if (g_rec.teleport_calls.size() == 1) {
            ck_eq((uint32_t)std::get<0>(g_rec.teleport_calls[0]), (uint32_t)UNIT_IDX,
                  "T1: unit_id forwarded unchanged, 0x004334b5");
            ck_eq((uint32_t)std::get<1>(g_rec.teleport_calls[0]), 11u,
                  "T1: dest_x == cmd_queue[slot].arg0, 0x0042348e/0x00433495");
            ck_eq((uint32_t)std::get<2>(g_rec.teleport_calls[0]), 22u,
                  "T1: dest_y == cmd_queue[slot].arg1, 0x004334a5/0x004334ac");
        }
    }

    // T2: arg0/arg1 are ZERO-EXTENDED (MOVZX word reads, 0x0042348e/0x004334a5) -- a value with the
    // top bit of the 16-bit field set must NOT come through sign-extended negative.
    {
        tact_fixture fx;
        g_rec.reset();
        g_rec.teleport_result                       = 0;
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg0 = 0x8001;
        fx.units[UNIT_IDX].cmd_queue[SLOT_IDX].arg1 = 0xffff;
        tact_store own                              = fx.store();
        detail::unit_cmd_teleport_jump_tick(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)g_rec.teleport_calls.size(), 1u, "T2: fires once");
        if (g_rec.teleport_calls.size() == 1) {
            ck_eq((uint32_t)std::get<1>(g_rec.teleport_calls[0]), 0x8001u,
                  "T2: arg0 zero-extended, not sign-extended");
            ck_eq((uint32_t)std::get<2>(g_rec.teleport_calls[0]), 0xffffu,
                  "T2: arg1 zero-extended, not sign-extended");
        }
    }

    // T3: result == 0 -> unit_cmd_advance(unit_idx, cmd_slot_index) dispatched, 0x004334c0-0x004334cc.
    {
        tact_fixture fx;
        g_rec.reset();
        g_rec.teleport_result = 0;
        tact_store own        = fx.store();
        detail::unit_cmd_teleport_jump_tick(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)g_rec.advance_calls.size(), 1u,
              "T3: result==0 dequeues via unit_cmd_advance exactly once");
        if (g_rec.advance_calls.size() == 1) {
            ck_eq((uint32_t)std::get<0>(g_rec.advance_calls[0]), (uint32_t)UNIT_IDX,
                  "T3: unit_idx forwarded unchanged, 0x004334c9");
            ck_eq((uint32_t)std::get<1>(g_rec.advance_calls[0]), (uint32_t)SLOT_IDX,
                  "T3: cmd_slot_index forwarded unchanged, 0x004334c6");
        }
    }

    // T4: result != 0 -> unit_cmd_advance is NOT called; the command stays queued for a retry.
    {
        tact_fixture fx;
        g_rec.reset();
        g_rec.teleport_result = 1;
        tact_store own        = fx.store();
        detail::unit_cmd_teleport_jump_tick(own, rec_calls(), UNIT_IDX, SLOT_IDX);
        ck_eq((uint32_t)g_rec.advance_calls.size(), 0u,
              "T4: result!=0 -- 0x004334c4 skips the dequeue, command retried next tick");
    }
}

} // namespace mh::tact::test
