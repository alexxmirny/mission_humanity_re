//
// sim_unit_path_store_selftest.cpp -- `simtest` oracle for the three per-unit path-slot WRITERS
// (sim/resid/sim_unit_path_store.h/.cpp, RI-SIM / sim_resid batch D):
//   llm_strat_unit_assign_path_from_job_result @0x0049508a
//   llm_strat_unit_path_store_result           @0x00495aa0
//   llm_strat_unit_assign_shared_path           @0x00495f87
//
// NO SHADOW SITE (sim_resid rule 1 -- see the header's own banner): this offline oracle
// (net_selftest simtest) is the ONLY verification. The three .asm listings under
// tmp/decomp_sim_resid/ are the spec, never the .c beside them (the .c has silently lied
// elsewhere in this project).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY:
//   All three scan the player's 100-slot _G_LLM_STRAT_PATH_SLOT_FLAGS table for a free slot, but
//   with DIFFERENT predicates: assign_path_from_job_result skips a slot when flag==1
//   (CMP ...,0x1 / JZ @0x0049510c), assign_shared_path skips when flag!=0 (CMP ...,0x0 / JNZ
//   @0x00495fdf) -- so a flag value of 2 (neither 0 nor 1) is FREE to one and BUSY to the other.
//   path_store_result does not search at all -- its slot is caller-supplied.
//   assign_path_from_job_result reads from the async job-result table (byte0+1 = heading,
//   byte1 verbatim = run_length, terminator heading=0x00 @0x004951de, entry bound 299/0x12b
//   @0x00495127). path_store_result and assign_shared_path both read from the live pathtrace
//   dirs buffer, ONE BYTE ADVANCED before the first store (dirs[1], not dirs[0] --
//   0x00495ad0-0x00495ad3 / 0x00495fec-0x00495fef), raw byte heading (no +1), terminator
//   heading=0xff (0x00495b33 / 0x0049609f), entry bound 300/0x12c (@0x00495b10 / @0x0049607b).
//   assign_shared_path ADDITIONALLY writes a SECOND terminator 0xff at the FIRST byte of the
//   NEXT slot's buffer (@0x004960bc) -- an unconditional extra write, the "shared" companion
//   buffer -- and a genuine out-of-region alias if the loop runs the full 300 entries
//   (PRESERVE-BUG, reproduced literally per Law 2, not exercised at the last-slot corner here).
//   All three (well: both search-based ones plus path_store_result) reset unit.path_cursor=0
//   and call path_attach_slot(player, unit_index, <the slot actually used>).
//   assign_path_from_job_result/assign_shared_path return 1 on success, 0 if no free slot found
//   in 100 tries (@0x004951f4/0x00495202, @0x004960f0/0x004960fe).
//
#include "sim/resid/sim_unit_path_store.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct free_slot_call {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<free_slot_call> g_free_slot_calls;
void                        rec_path_free_slot(uint16_t player, int32_t unit_index) {
    g_free_slot_calls.push_back({player, unit_index});
}

struct attach_call {
    int32_t player;
    int32_t unit_index;
    int32_t slot;
};
std::vector<attach_call> g_attach_calls;
void                     rec_path_attach_slot(int32_t player, int32_t unit_index, int32_t slot) {
    g_attach_calls.push_back({player, unit_index, slot});
}

// Settable by each case before the call -- the mock stands in for llm_strat_pathtrace_dirs_get.
const void *g_dirs_ptr = nullptr;
// committed llm_strat_pathtrace_dirs_get returns uint8_t * -- TACT1-P C6, 2026-09-04.
uint8_t *rec_pathtrace_dirs_get() { return const_cast<uint8_t *>(static_cast<const uint8_t *>(g_dirs_ptr)); }

const unit_path_store_calls g_calls = {
    &rec_path_free_slot,
    &rec_path_attach_slot,
    &rec_pathtrace_dirs_get,
};

void reset_recorders() {
    g_free_slot_calls.clear();
    g_attach_calls.clear();
    g_dirs_ptr = nullptr;
}

} // namespace

void run_unit_path_store_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- assign_path_from_job_result, the success path: stride into a NON-ZERO player/slot,
    // heading = job-result byte0+1 (0x00495147 INC DL), run_length = byte1 VERBATIM
    // (0x0049518c-0x0049518f), destination terminator heading==0x00 (NOT 0xff, 0x004951de),
    // path_attach_slot(player, unit_index, slot), return 1. Also pins that job_result_idx is
    // masked to its low 16 bits (0x004950b0 MOVZX) by handing in an index with garbage upper bits.
    // path_slot_id starts 0xff so path_free_slot must NOT be called (that is T2's case).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t PLAYER             = 3;
        constexpr int32_t  UNIT_INDEX         = 5;
        constexpr int32_t  FREE_SLOT          = 4;           // slots 0-3 marked busy below
        constexpr uint32_t JOB_RESULT_IDX_RAW = 0x00170009u; // low16=9, garbage in the high half

        for (int32_t s = 0; s < FREE_SLOT; ++s) {
            fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1; // busy
        }
        fx.u(PLAYER, UNIT_INDEX).path_slot_id = 0xff; // no existing path

        // job_result table entry 9 (== JOB_RESULT_IDX_RAW & 0xffff): two real steps then the
        // source terminator (0xff). 3 bytes/step: [dir code][aux/run_length][pad, unused].
        std::vector<uint8_t> steps = {
            2, 5, 0,    // entry0: heading = 2+1 = 3, run_length = 5
            9, 12, 0,   // entry1: heading = 9+1 = 10, run_length = 12
            0xff, 0, 0, // source terminator
        };
        fx.path_job_result[9].path_steps = steps.data();

        sim_store     own = fx.store();
        const int32_t rc =
            detail::assign_path_from_job_result(fx.view(), own, g_calls, PLAYER, UNIT_INDEX,
                                                JOB_RESULT_IDX_RAW);

        ck_eq((uint32_t)rc, 1u, "T1: return 1 on success, 0x004951f4");
        ck_eq((uint32_t)g_free_slot_calls.size(), 0u, "T1: path_slot_id was 0xff -- path_free_slot NOT called");
        ck((g_attach_calls.size() == 1 && g_attach_calls[0].player == PLAYER &&
            g_attach_calls[0].unit_index == UNIT_INDEX && g_attach_calls[0].slot == FREE_SLOT),
           "T1: path_attach_slot(player=3, unit_index=5, slot=4) called once, 0x004951ef");

        const path_waypoint &e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        const path_waypoint &e1 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 1];
        const path_waypoint &e2 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 2];
        ck_eq(e0.heading, 3u, "T1: entry0.heading = job-result byte0(2)+1, 0x00495145-0x00495163");
        ck_eq(e0.run_length, 5u, "T1: entry0.run_length = job-result byte1(5) VERBATIM, 0x0049518c-0x0049518f");
        ck_eq(e1.heading, 10u, "T1: entry1.heading = job-result byte0(9)+1 -- masked idx (low16=9) picked the right row");
        ck_eq(e1.run_length, 12u, "T1: entry1.run_length = 12 verbatim");
        ck_eq(e2.heading, 0u, "T1: destination terminator heading==0x00 (NOT 0xff), 0x004951c4-0x004951de");

        // Stride negative arm: neighbouring slot and neighbouring player must be untouched.
        const path_waypoint &neigh_slot =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + (FREE_SLOT + 1) * PATH_WAYPOINTS_PER_SLOT + 0];
        const path_waypoint &neigh_player =
            fx.path_buffers[(PLAYER + 1) * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(neigh_slot.heading, 0u, "T1: neighbouring slot (player*0xea60 + (slot+1)*0x258) untouched");
        ck_eq(neigh_player.heading, 0u, "T1: neighbouring player ((player+1)*0xea60 + slot*0x258) untouched");
    }

    // =================================================================================================
    // T2 -- assign_path_from_job_result: the unit already carries a path (path_slot_id != 0xff) ->
    // path_free_slot(player, unit_index) is called FIRST, 0x004950d3/0x004950e3. Also pins the
    // sentinel-immediate-stop case (source byte0==0xff at entry0): terminator lands at entry0 too.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t PLAYER     = 4;
        constexpr int32_t  UNIT_INDEX = 6;
        constexpr int32_t  FREE_SLOT  = 2; // slots 0-1 marked busy below

        for (int32_t s = 0; s < FREE_SLOT; ++s) {
            fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1;
        }
        fx.u(PLAYER, UNIT_INDEX).path_slot_id = 5; // existing path -> release first

        std::vector<uint8_t> steps       = {0xff, 0, 0}; // immediate source terminator
        fx.path_job_result[0].path_steps = steps.data();

        sim_store     own = fx.store();
        const int32_t rc  = detail::assign_path_from_job_result(fx.view(), own, g_calls, PLAYER,
                                                                UNIT_INDEX, /*job_result_idx=*/0u);

        ck_eq((uint32_t)rc, 1u, "T2: still returns 1 -- a free slot exists after the release");
        ck((g_free_slot_calls.size() == 1 && g_free_slot_calls[0].player == PLAYER &&
            g_free_slot_calls[0].unit_index == UNIT_INDEX),
           "T2: path_slot_id!=0xff -> path_free_slot(player=4, unit_index=6) called, 0x004950e3");
        ck((g_attach_calls.size() == 1 && g_attach_calls[0].slot == FREE_SLOT),
           "T2: path_attach_slot slot=2 (first free after the busy 0/1), 0x004951ef");
        const path_waypoint &e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(e0.heading, 0u, "T2: immediate source sentinel -> terminator at entry0, 0x00495132/0x004951de");
    }

    // =================================================================================================
    // T3 -- assign_path_from_job_result: every one of the player's 100 slots busy -> return 0, no
    // path_attach_slot call, no buffer write. A mock that goes uncalled on both arms passes
    // vacuously, so this asserts the NEGATIVE call count too.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t PLAYER     = 5;
        constexpr int32_t  UNIT_INDEX = 1;

        for (int32_t s = 0; s < 100; ++s) fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1;
        fx.u(PLAYER, UNIT_INDEX).path_slot_id = 0xff;

        sim_store     own = fx.store();
        const int32_t rc  = detail::assign_path_from_job_result(fx.view(), own, g_calls, PLAYER,
                                                                UNIT_INDEX, /*job_result_idx=*/0u);

        ck_eq((uint32_t)rc, 0u, "T3: no free slot in 100 tries -> return 0, 0x00495202");
        ck_eq((uint32_t)g_attach_calls.size(), 0u, "T3: path_attach_slot NOT called on the exhausted path");
        const path_waypoint &e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + 0 * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(e0.heading, 0u, "T3: nothing written anywhere -- slot0 entry0 still the reset default");
    }

    // =================================================================================================
    // T4 -- assign_path_from_job_result's entry-copy bound is 299 (0x12b), NOT 300: 299 real,
    // non-terminator steps exhaust the loop by COUNT alone (steps[299] is never read), and the
    // terminator lands at entry 299 -- the buffer's last valid index, NOT an overflow.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint16_t PLAYER     = 6;
        constexpr int32_t  UNIT_INDEX = 2;
        constexpr int32_t  FREE_SLOT  = 3; // slots 0-2 marked busy below

        for (int32_t s = 0; s < FREE_SLOT; ++s) {
            fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1;
        }
        fx.u(PLAYER, UNIT_INDEX).path_slot_id = 0xff;

        // Exactly 299 entries (897 bytes), all non-terminator (dir code 1) -- the loop body never
        // reads index 299 (0x12b), so this array is never indexed out of its own bounds either.
        std::vector<uint8_t> steps(299u * 3u);
        for (size_t i = 0; i < 299; ++i) {
            steps[i * 3 + 0] = 1; // dir code
            steps[i * 3 + 1] = 2; // run_length
            steps[i * 3 + 2] = 0;
        }
        fx.path_job_result[7].path_steps = steps.data();

        sim_store     own = fx.store();
        const int32_t rc  = detail::assign_path_from_job_result(fx.view(), own, g_calls, PLAYER,
                                                                UNIT_INDEX, /*job_result_idx=*/7u);

        ck_eq((uint32_t)rc, 1u, "T4: 299-entry copy still succeeds");
        const path_waypoint &e298 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 298];
        const path_waypoint &e299 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 299];
        ck_eq(e298.heading, 2u, "T4: last COPIED entry is index 298 (0x00495127 bound=0x12b=299)");
        ck_eq(e299.heading, 0u,
              "T4: terminator lands at entry 299 -- the slot's own last valid index, no overflow");
    }

    // =================================================================================================
    // T5 -- path_store_result, the success path: the source pointer is advanced ONE BYTE before the
    // first store (0x00495ad0-0x00495ad3), so the first heading WRITTEN is dirs[1], never dirs[0];
    // destination terminator heading==0xff (0x00495b33, the OPPOSITE convention from
    // assign_path_from_job_result's 0x00); path_slot is CALLER-OWNED (no slot search at all --
    // path_attach_slot gets exactly the value passed in); unit.path_cursor reset to 0.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER    = 2;
        constexpr int32_t  UNIT_IDX  = 8;
        constexpr int32_t  PATH_SLOT = 7; // caller-supplied, NOT searched

        fx.u(PLAYER, UNIT_IDX).path_cursor = 42; // must become 0

        // dirs[0] must never be read; dirs[1..3] are distinct so the shift is visible; dirs[4] is
        // the source sentinel.
        std::vector<uint8_t> dirs = {0xaa, 0x11, 0x22, 0x33, 0xff};
        g_dirs_ptr                = dirs.data();

        sim_store own = fx.store();
        detail::path_store_result(fx.view(), own, g_calls, PLAYER, UNIT_IDX, /*unused1=*/0xdeadu,
                                  /*unused2=*/0xbeefu, PATH_SLOT);

        const path_waypoint &e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + PATH_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        const path_waypoint &e1 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + PATH_SLOT * PATH_WAYPOINTS_PER_SLOT + 1];
        const path_waypoint &e2 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + PATH_SLOT * PATH_WAYPOINTS_PER_SLOT + 2];
        const path_waypoint &e3 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + PATH_SLOT * PATH_WAYPOINTS_PER_SLOT + 3];
        ck_eq(e0.heading, 0x11u, "T5: first stored heading is dirs[1] (0x11), NOT dirs[0] (0xaa), 0x00495ad3");
        ck_eq(e1.heading, 0x22u, "T5: second stored heading is dirs[2]");
        ck_eq(e2.heading, 0x33u, "T5: third stored heading is dirs[3], then dirs[4]==0xff stops the copy");
        ck_eq(e3.heading, 0xffu, "T5: destination terminator heading==0xff (NOT 0x00), 0x00495b33");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_IDX).path_cursor, 0u, "T5: unit.path_cursor reset to 0, 0x00495b4d");
        ck((g_attach_calls.size() == 1 && g_attach_calls[0].player == (int32_t)PLAYER &&
            g_attach_calls[0].unit_index == UNIT_IDX && g_attach_calls[0].slot == PATH_SLOT),
           "T5: path_attach_slot(player, unit_idx, PATH_SLOT) -- the CALLER's slot, not a searched one, 0x00495b61");

        const path_waypoint &neigh_slot =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + (PATH_SLOT + 1) * PATH_WAYPOINTS_PER_SLOT + 0];
        const path_waypoint &neigh_player =
            fx.path_buffers[(PLAYER + 1) * PATH_WAYPOINTS_PER_PLAYER + PATH_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(neigh_slot.heading, 0u, "T5: neighbouring slot (slot+1) untouched");
        ck_eq(neigh_player.heading, 0u, "T5: neighbouring player (player+1) untouched");
    }

    // =================================================================================================
    // T6 -- path_store_result's entry-copy bound is 300 (0x12c), and running the FULL 300 without
    // ever hitting the source's own 0xff produces a GENUINE OVERFLOW: the terminator write lands at
    // entry 300, which is entry 0 of the NEXT slot (PRESERVE-BUG, reproduced literally per Law 2).
    // An INTERIOR slot only -- the last-slot-of-last-player corner would run past this fixture's own
    // path_buffers vector and trip ASan, per the brief.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER    = 6;
        constexpr int32_t  UNIT_IDX  = 3;
        constexpr int32_t  PATH_SLOT = 10; // interior: PATH_SLOT+1 (11) is still this player's own slot

        fx.u(PLAYER, UNIT_IDX).path_cursor = 99;

        // 305 bytes, all non-0xff -- dirs[1..300] (300 bytes) are all consumed by the copy loop
        // without ever seeing a source sentinel, so termination is by COUNT alone.
        std::vector<uint8_t> dirs(305, 0x07);
        g_dirs_ptr = dirs.data();

        sim_store own = fx.store();
        detail::path_store_result(fx.view(), own, g_calls, PLAYER, UNIT_IDX, 0u, 0u, PATH_SLOT);

        const path_waypoint &e299 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + PATH_SLOT * PATH_WAYPOINTS_PER_SLOT + 299];
        const path_waypoint &overflow_e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + (PATH_SLOT + 1) * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(e299.heading, 0x07u, "T6: entry 299 is the last COPIED (real) waypoint, count bound 0x12c=300");
        ck_eq(overflow_e0.heading, 0xffu,
              "T6: PRESERVE-BUG -- terminator overflows to (slot+1).entry0 when the loop runs the full "
              "300 entries, 0x00495b33 with cursor+1==300");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_IDX).path_cursor, 0u, "T6: unit.path_cursor still reset to 0");
        ck((g_attach_calls.size() == 1 && g_attach_calls[0].slot == PATH_SLOT),
           "T6: path_attach_slot still gets the ORIGINAL slot (10), not the aliased one (11)");
    }

    // =================================================================================================
    // T7 -- assign_shared_path, the success path: same dirs[1]-skip convention as path_store_result,
    // PRIMARY terminator heading==0xff at the found slot, PLUS a SECOND, unconditional terminator
    // 0xff at entry 0 of the NEXT slot (@0x004960bc -- the "shared" companion buffer,
    // PRESERVE-BUG, not clamped in the original), unit.path_cursor reset, path_attach_slot gets the
    // FOUND slot only (never slot+1).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER     = 3;
        constexpr int32_t  UNIT_INDEX = 9;
        constexpr int32_t  FREE_SLOT  = 4; // slots 0-3 marked busy (any nonzero value) below

        for (int32_t s = 0; s < FREE_SLOT; ++s) {
            fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1;
        }
        fx.u(PLAYER, UNIT_INDEX).path_cursor = 88;

        // Dir codes kept small (<40 == DIR_STEP_OFFSET_COUNT): this function ALSO indexes
        // v.dir_step_offsets[*dirs] for the dead position-walk (see the header's uncertainty note),
        // so an arbitrary byte here would be an out-of-bounds read under ASan.
        std::vector<uint8_t> dirs = {0x00, 3, 5, 0xff}; // dirs[0] unused, dirs[1]=3, dirs[2]=5, sentinel
        g_dirs_ptr                = dirs.data();

        sim_store     own = fx.store();
        const int32_t rc =
            detail::assign_shared_path(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, /*start_col=*/10,
                                       /*start_row=*/20);

        ck_eq((uint32_t)rc, 1u, "T7: return 1 on success, 0x004960f0");
        const path_waypoint &e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        const path_waypoint &e1 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 1];
        const path_waypoint &e2 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 2];
        ck_eq(e0.heading, 3u, "T7: first stored heading is dirs[1] (3), same off-by-one skip as path_store_result");
        ck_eq(e1.heading, 5u, "T7: second stored heading is dirs[2] (5), then dirs[3]==0xff stops the copy");
        ck_eq(e2.heading, 0xffu, "T7: PRIMARY terminator heading==0xff at the found slot, 0x0049609f");

        const path_waypoint &second_term =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + (FREE_SLOT + 1) * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(second_term.heading, 0xffu,
              "T7: PRESERVE-BUG -- SECOND terminator 0xff at entry0 of slot+1 (the shared companion "
              "buffer), unconditional, 0x004960bc");

        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).path_cursor, 0u, "T7: unit.path_cursor reset to 0, 0x004960d6");
        ck((g_attach_calls.size() == 1 && g_attach_calls[0].player == (int32_t)PLAYER &&
            g_attach_calls[0].unit_index == UNIT_INDEX && g_attach_calls[0].slot == FREE_SLOT),
           "T7: path_attach_slot gets the FOUND slot (4), NOT slot+1, 0x004960eb/0x004960e0");

        // Stride negative arm: slot+2 (past the shared companion) and the neighbouring player are
        // untouched.
        const path_waypoint &neigh_slot2 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + (FREE_SLOT + 2) * PATH_WAYPOINTS_PER_SLOT + 0];
        const path_waypoint &neigh_player =
            fx.path_buffers[(PLAYER + 1) * PATH_WAYPOINTS_PER_PLAYER + FREE_SLOT * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(neigh_slot2.heading, 0u, "T7: slot+2 (past the companion write) untouched");
        ck_eq(neigh_player.heading, 0u, "T7: neighbouring player untouched");
    }

    // =================================================================================================
    // T8 -- assign_shared_path: every one of the player's 100 slots busy (flag != 0) -> return 0, no
    // path_attach_slot call, no buffer write (neither terminator).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER     = 5;
        constexpr int32_t  UNIT_INDEX = 4;

        for (int32_t s = 0; s < 100; ++s) fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1;

        std::vector<uint8_t> dirs = {0x00, 1, 0xff};
        g_dirs_ptr                = dirs.data();

        sim_store     own = fx.store();
        const int32_t rc =
            detail::assign_shared_path(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, 0, 0);

        ck_eq((uint32_t)rc, 0u, "T8: no free slot in 100 tries -> return 0, 0x004960fe");
        ck_eq((uint32_t)g_attach_calls.size(), 0u, "T8: path_attach_slot NOT called on the exhausted path");
        const path_waypoint &e0 =
            fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + 0 * PATH_WAYPOINTS_PER_SLOT + 0];
        ck_eq(e0.heading, 0u, "T8: nothing written anywhere");
    }

    // =================================================================================================
    // T9 -- THE slot-search predicate differentiator. path_slot_flags[player][9] is set to 2 --
    // neither 0 nor 1. assign_path_from_job_result skips only when flag==1 (0x0049510c), so a 2
    // reads as FREE and is picked immediately. assign_shared_path skips whenever flag!=0
    // (0x00495fdf), so the SAME 2 reads as BUSY and it falls through to the next real free slot.
    // This is the only case that can tell the two predicates apart.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PLAYER         = 7;
        constexpr int32_t  UNIT_INDEX_JR  = 1;
        constexpr int32_t  UNIT_INDEX_SP  = 2;
        constexpr int32_t  AMBIGUOUS_SLOT = 9;  // flag==2
        constexpr int32_t  NEXT_FREE_SLOT = 10; // flag==0

        for (int32_t s = 0; s < AMBIGUOUS_SLOT; ++s) {
            fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + s] = 1; // genuinely busy to both
        }
        fx.path_slot_flags[PLAYER * PATH_SLOTS_PER_PLAYER + AMBIGUOUS_SLOT] = 2; // the differentiator
        // slot 10 stays 0 (free) from fx.reset().

        fx.u(PLAYER, UNIT_INDEX_JR).path_slot_id = 0xff;
        std::vector<uint8_t> jr_steps            = {0xff, 0, 0};
        fx.path_job_result[0].path_steps         = jr_steps.data();

        {
            sim_store     own = fx.store();
            const int32_t rc  = detail::assign_path_from_job_result(
                fx.view(), own, g_calls, (uint16_t)PLAYER, UNIT_INDEX_JR, /*job_result_idx=*/0u);
            ck_eq((uint32_t)rc, 1u, "T9a: assign_path_from_job_result succeeds");
        }

        std::vector<uint8_t> sp_dirs = {0x00, 1, 0xff};
        g_dirs_ptr                   = sp_dirs.data();
        {
            sim_store     own = fx.store();
            const int32_t rc =
                detail::assign_shared_path(fx.view(), own, g_calls, PLAYER, UNIT_INDEX_SP, 0, 0);
            ck_eq((uint32_t)rc, 1u, "T9b: assign_shared_path succeeds");
        }

        ck((g_attach_calls.size() == 2), "T9: exactly two path_attach_slot calls recorded (one per function)");
        if (g_attach_calls.size() == 2) {
            ck_eq((uint32_t)g_attach_calls[0].slot, (uint32_t)AMBIGUOUS_SLOT,
                  "T9: assign_path_from_job_result treats flag==2 as FREE and picks slot 9, 0x0049510c "
                  "(CMP ...,0x1 / JZ -- only ==1 is 'busy')");
            ck_eq((uint32_t)g_attach_calls[1].slot, (uint32_t)NEXT_FREE_SLOT,
                  "T9: assign_shared_path treats the SAME flag==2 as BUSY and falls through to slot 10, "
                  "0x00495fdf (CMP ...,0x0 / JNZ -- any nonzero is 'busy')");
        }
    }
}

} // namespace mh::sim::test
