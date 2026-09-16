//
// tact_cmd_queue_wrap_selftest.cpp -- offline oracle closing TACT1B's two outstanding acceptance
// clauses over the tactical unit COMMAND QUEUE (mh_tact_unit_record::cmd_queue, 128 entries x 0xb
// bytes, based at +0x54; entry shape interrupt_flag +0, op +1, arg0 +3, arg1 +5, arg2 +7, arg3 +9 --
// mh_structs.gen.h's mh_llm_tact_unit_cmd_entry):
//
//   (1) drive the queue through a wrap at index 0x80 and assert the WHOLE queue region byte-for-byte
//       against the original's post-state, including the STALE interrupt_flag a dequeue leaves
//       behind (a reimplementation that zero-fills the entry must be caught).
//   (2) reproduce (not fix) the stall trap: enqueue an op with no case in the weapons-tick dispatch
//       and assert the queue head is still wedged.
//
// THE WRAP BOUND, DERIVED FROM THE .ASM (not assumed from the clause text): the free-slot scan in
// llm_tact_unit_enqueue_command's default append arm increments its probe index and wraps it to 0
// at exactly 0x80 -- `INC [idx] / CMP [idx],0x80 / JL / MOV [idx],0` at
// 0x0042b91d/0x0042b920/0x0042b927/0x0042b929 (mirrored at three other op-specific scan sites in the
// same function: 0x0042b49d-0x0042b4a9, 0x0042b51d-0x0042b529, 0x0042b619-0x0042b625). The HEAD
// pointer itself (unit.cmd_index) is advanced+wrapped the same way, independently, inside
// llm_tact_unit_cmd_advance: `INC byte[cmd_index] / CMP ...,0x80 / JC / MOV 0` at
// 0x0043133c/0x00431349/0x00431350/0x00431352. Both bounds are 0x80, confirmed structurally (not
// merely by the clause's own wording), and match mh_structs.gen.h's cmd_queue[128] extent exactly.
//
// THE STALE interrupt_flag, DERIVED FROM THE .ASM: llm_tact_unit_cmd_advance's dequeue clears
// op/arg0/arg1/arg2/arg3 (0x004312b4-0x0043130c, five MOV-0 stores) but never touches
// interrupt_flag -- the struct's own field comment (mh_structs.gen.h) independently names this exact
// mechanism ("NOT cleared by llm_tact_unit_cmd_advance ... this byte is STALE whenever op == 0").
//
// OFFLINE SAFETY: llm_tact_unit_cmd_advance's detail:: entry point (tact_unit_cmd_advance.h) takes NO
// calls-struct -- it calls mh::call::time_GetCurrentTime() directly (a frontier/real-VA call, unsafe
// in net_selftest.exe) but ONLY when the unit's *new* cmd_index slot's op == 0. Every case below seeds
// the whole queue with a nonzero-op formula, so that branch never fires; D1/D2 additionally pin
// wander_check_time to a sentinel and assert it is untouched, which is the direct proof the frontier
// call was skipped rather than merely "didn't crash". Likewise llm_tact_unit_enqueue_command's
// detail:: entry only reaches a frontier call on op==9/0x1f/0x1d -- every case here uses an ordinary
// queued op (0x40, 8, 1) outside that set, so the default append arm is offline-safe throughout.
//
// THE STALL (clause 2): llm_tact_unit_weapons_tick's op dispatch (tact_unit_weapons_tick.h step 12)
// is an explicit compare chain with real no-op gaps -- {2,3,6,7,0xc..0x1b,0x1d,0x1f..0x3f,0x41..0x7e,
// 0x80..} -- confirmed structurally: no branch of the .cpp's if-chain reaches any of these values, so
// dispatch does nothing and neither cmd_index nor the head slot's op is ever touched for them. This IS
// a real trap in the original game (reproduced literally, not "fixed"): every case below enqueues via
// the real llm_tact_unit_enqueue_command, ticks via the real llm_tact_unit_weapons_tick dispatcher
// (mocking only its 15 outward callees, exactly as tact_unit_weapons_tick_selftest.cpp's own oracle
// does for the SAME reason -- see that file's banner), and asserts cmd_index/head-op are BOTH
// unchanged afterward.
//
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_enqueue_command.h"
#include "tact/tact_unit_weapons_tick.h"

#include "tact_test_support.h"

#include <cstdio>
#include <vector>

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_IDX = 44; // distinctive, mid-range TACT_UNIT_SLOTS (129) index

// ---- the seed formula: every slot/field gets a DISTINCT, non-symmetric, always-nonzero value so
// any zero-fill, wrong-offset, or wrong-slot write is directly observable. Ranges are kept far apart
// (0x10.., 0x1000.., 0x2000.., 0x3000.., 0x4000.., 0x5000..) so a family swap (e.g. arg1<->arg2) is
// also directly observable, and none of them collides with any of the small sentinel values the
// individual cases write in (all < 2100 decimal). --------------------------------------------------
uint8_t  seed_if(int i) { return (uint8_t)(0x10 + i); }
uint16_t seed_op(int i) { return (uint16_t)(0x1001 + i); }
uint16_t seed_a0(int i) { return (uint16_t)(0x2001 + i); }
uint16_t seed_a1(int i) { return (uint16_t)(0x3001 + i); }
uint16_t seed_a2(int i) { return (uint16_t)(0x4001 + i); }
uint16_t seed_a3(int i) { return (uint16_t)(0x5001 + i); }

void seed_queue(tact_unit &u) {
    for (int i = 0; i < 128; ++i) {
        u.cmd_queue[i].interrupt_flag = seed_if(i);
        u.cmd_queue[i].op             = seed_op(i);
        u.cmd_queue[i].arg0           = seed_a0(i);
        u.cmd_queue[i].arg1           = seed_a1(i);
        u.cmd_queue[i].arg2           = seed_a2(i);
        u.cmd_queue[i].arg3           = seed_a3(i);
    }
}

// One entry's worth of field-level checks, each naming the slot and its byte offset within the
// 0xb-byte record (base tact_unit_record+0x54, stride 0xb) so a mismatch pinpoints exactly which
// byte(s) diverged.
void ck_entry(const mh::game::mh_llm_tact_unit_cmd_entry &got, uint8_t exp_if, uint16_t exp_op,
              uint16_t exp_a0, uint16_t exp_a1, uint16_t exp_a2, uint16_t exp_a3, int slot,
              const char *tag) {
    char buf[192];
    std::snprintf(buf, sizeof buf, "%s: slot %d interrupt_flag (+0x54+0x%x*0xb+0)", tag, slot, slot);
    ck_eq(got.interrupt_flag, exp_if, buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d op (+0x54+0x%x*0xb+1)", tag, slot, slot);
    ck_eq(got.op, exp_op, buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg0 (+0x54+0x%x*0xb+3)", tag, slot, slot);
    ck_eq(got.arg0, exp_a0, buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg1 (+0x54+0x%x*0xb+5)", tag, slot, slot);
    ck_eq(got.arg1, exp_a1, buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg2 (+0x54+0x%x*0xb+7)", tag, slot, slot);
    ck_eq(got.arg2, exp_a2, buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg3 (+0x54+0x%x*0xb+9)", tag, slot, slot);
    ck_eq(got.arg3, exp_a3, buf);
}

// Every slot except `skip` must still hold its original seed -- proves the operation under test
// touched ONLY the slot(s) it was supposed to (0x0042b83f's scan is READ-ONLY on every slot it
// merely probes; llm_tact_unit_cmd_advance touches only its own cmd_slot_index).
void ck_region_untouched(const tact_unit &u, int skip, const char *tag) {
    for (int i = 0; i < 128; ++i) {
        if (i == skip) continue;
        ck_entry(u.cmd_queue[i], seed_if(i), seed_op(i), seed_a0(i), seed_a1(i), seed_a2(i),
                 seed_a3(i), i, tag);
    }
}

// ---- the weapons-tick mock: minimal (call-count only) since clause 2 only needs to prove the
// dispatch does NOT fire for a gap op -- tact_unit_weapons_tick_selftest.cpp's own oracle already
// covers per-arm argument forwarding for every HANDLED op; this file's job is the state (cmd_index /
// head op) either side of a stall, which that file's T-NOOP-GAPS block does not assert.
struct wt_clock {
    std::vector<double> q;
    size_t              pos = 0;
    double              next() { return pos < q.size() ? q[pos++] : -1.0e9; }
};
wt_clock g_wt_clock;

struct wt_recorder {
    int32_t total = 0; // every one of the 15 non-clock callees increments this
    void    reset() { *this = wt_recorder{}; }
};
wt_recorder g_wt_rec;

const unit_weapons_tick_calls &stall_calls() {
    static const unit_weapons_tick_calls c = {
        []() -> double { return g_wt_clock.next(); },
        [](int32_t, uint32_t) { ++g_wt_rec.total; },         // unit_cmd_queue_advance
        [](int32_t) -> int32_t { ++g_wt_rec.total; return 0; },                          // unit_death_tick
        [](int32_t, int32_t, int32_t) { ++g_wt_rec.total; }, // unit_fire_weapon
        [](int32_t, int32_t) { ++g_wt_rec.total; },          // unit_cmd_advance
        [](int32_t) { ++g_wt_rec.total; },                   // unit_rotate_tick
        [](int32_t, int32_t, double) { ++g_wt_rec.total; },  // unit_move_tick
        [](int32_t) { ++g_wt_rec.total; },                   // unit_kneel_tick
        [](int32_t) { ++g_wt_rec.total; },                   // unit_stand_tick
        [](int32_t) { ++g_wt_rec.total; },                   // unit_mine_arm_tick
        [](int32_t, uint8_t) { ++g_wt_rec.total; },          // unit_set_anim_state
        [](int32_t, int32_t) { ++g_wt_rec.total; },          // unit_cmd_teleport_jump_tick
        [](int32_t, int32_t) { ++g_wt_rec.total; },          // unit_cmd_advance_with_defstat
        [](int32_t) { ++g_wt_rec.total; },                   // unit_cmd_stance_off
        [](int32_t) { ++g_wt_rec.total; },                   // unit_cmd_stance_on
        [](int32_t, int32_t) { ++g_wt_rec.total; },          // unit_cmd_queue_resubmit_run
    };
    return c;
}

} // namespace

void run_cmd_queue_wrap_tests() {
    // =============================================================================================
    // CLAUSE (1a) -- ENQUEUE'S FREE-SLOT SCAN WRAPS AT 0x80 (0x0042b91d-0x0042b929).
    // =============================================================================================

    // E1: cmd_index=0x7f (occupied) forces the scan past the end -> wraps to 0 (empty) -> writes
    // there. Full-region byte-for-byte check that ONLY slot 0 changed.
    {
        tact_fixture fx;
        seed_queue(fx.units[UNIT_IDX]);
        fx.units[UNIT_IDX].cmd_queue[0].op = 0; // the only free slot, reached only via the wrap
        fx.units[UNIT_IDX].cmd_index       = 0x7f;
        tact_store own                     = fx.store();

        const int32_t ret = detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, 0x40 /*run,
                                                          not special-cased*/
                                                         ,
                                                         0xAB, 1001, 1002,
                                                         1003, 1004);
        ck_eq((uint32_t)ret, 1u, "E1: enqueue succeeds by finding the wrapped slot, 0x0042b911");

        tact_unit &u = fx.units[UNIT_IDX];
        ck_entry(u.cmd_queue[0], 0xAB, 0x40, 1001, 1002, 1003, 1004, 0,
                 "E1: WRAP-TARGET WRITE (idx wrapped 0x80->0 at 0x0042b929, written by the "
                 "six-store block 0x0042b888-0x0042b90a on the re-probed pass)");
        ck_region_untouched(u, /*skip=*/0, "E1: scan-probed slot NOT written (incl. head 0x7f)");
        ck_eq((uint32_t)u.cmd_index, 0x7fu,
              "E1: enqueue never advances cmd_index itself, only cmd_advance does (0x0042b83f note 9)");
    }

    // E2: boundary from the OTHER side -- cmd_index=0x7e (one below the wrap trigger) with slot
    // 0x7e itself free: the scan must hit it on the FIRST probe and never reach the wrap logic at
    // all. Proves 0x0042b920's `JL` (not the wrap) is what fires here.
    {
        tact_fixture fx;
        seed_queue(fx.units[UNIT_IDX]);
        fx.units[UNIT_IDX].cmd_queue[0x7e].op = 0;
        fx.units[UNIT_IDX].cmd_index          = 0x7e;
        tact_store own                        = fx.store();

        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, 8 /*wait, not special-cased*/,
                                         0xCD, 2001, 2002, 2003, 2004);
        ck_eq((uint32_t)ret, 1u, "E2: enqueue succeeds on the first probe, no wrap needed");

        tact_unit &u = fx.units[UNIT_IDX];
        ck_entry(u.cmd_queue[0x7e], 0xCD, 8, 2001, 2002, 2003, 2004, 0x7e,
                 "E2: NO-WRAP WRITE at the exact pre-boundary slot, six-store block 0x0042b888-0x0042b90a");
        ck_entry(u.cmd_queue[0x7f], seed_if(0x7f), seed_op(0x7f), seed_a0(0x7f), seed_a1(0x7f),
                 seed_a2(0x7f), seed_a3(0x7f), 0x7f, "E2: wrap-target slot 0x7f untouched (never probed)");
        ck_entry(u.cmd_queue[0], seed_if(0), seed_op(0), seed_a0(0), seed_a1(0), seed_a2(0),
                 seed_a3(0), 0, "E2: wrap-target slot 0 untouched (never probed)");
        ck_eq((uint32_t)u.cmd_index, 0x7eu, "E2: enqueue never advances cmd_index");
    }

    // =============================================================================================
    // CLAUSE (1b) -- DEQUEUE (llm_tact_unit_cmd_advance) WRAPS cmd_index AT 0x80 AND LEAVES A
    // STALE interrupt_flag ON THE DEQUEUED SLOT (0x00431335-0x00431360, 0x004312b4-0x0043130c).
    // =============================================================================================

    // D1: dequeue slot 0x7f -> cmd_index increments to 0x80 -> wraps to 0. Full-region byte-for-byte
    // check: every slot except 0x7f is untouched, and 0x7f keeps its STALE interrupt_flag while
    // op/arg0..arg3 are zeroed. MUTATION CHECK: a reimplementation that zero-fills the whole entry
    // on dequeue (interrupt_flag included) fails the interrupt_flag line below.
    {
        tact_fixture fx;
        seed_queue(fx.units[UNIT_IDX]);
        tact_unit &u           = fx.units[UNIT_IDX];
        u.cmd_index            = 0x7f;
        u.move_path_slot       = 0;    // skip the (offline-safe but out-of-scope) plane write
        u.move_retry_wait      = 0x11; // distinct non-symmetric sentinels, each must -> 0
        u.move_retry_attempts  = 0x22;
        u.move_stuck_countdown = 0x33;
        u.wander_check_time    = 54321.0; // sentinel: must stay untouched (proves the frontier
                                          // time_GetCurrentTime() branch was correctly SKIPPED,
                                          // since slot 0's seeded op is nonzero)
        tact_store own = fx.store();

        detail::unit_cmd_advance(own, UNIT_IDX, 0x7f);

        ck_eq((uint32_t)u.cmd_index, 0u,
              "D1: cmd_index wraps 0x80->0, INC/CMP/JC/MOV-0 at 0x0043133c-0x00431352");
        ck_eq((uint32_t)u.cmd_queue[0x7f].interrupt_flag, (uint32_t)seed_if(0x7f),
              "D1: STALE interrupt_flag survives the dequeue, NOT zeroed (0x004312b4 clears op+args "
              "only, no store to +0x54); a zero-filling reimplementation fails this line");
        ck_eq((uint32_t)u.cmd_queue[0x7f].op, 0u, "D1: dequeued slot op zeroed, 0x004312b4");
        ck_eq((uint32_t)u.cmd_queue[0x7f].arg0, 0u, "D1: dequeued slot arg0 zeroed, 0x004312ca");
        ck_eq((uint32_t)u.cmd_queue[0x7f].arg1, 0u, "D1: dequeued slot arg1 zeroed, 0x004312e0");
        ck_eq((uint32_t)u.cmd_queue[0x7f].arg2, 0u, "D1: dequeued slot arg2 zeroed, 0x004312f6");
        ck_eq((uint32_t)u.cmd_queue[0x7f].arg3, 0u, "D1: dequeued slot arg3 zeroed, 0x0043130c");
        ck_region_untouched(u, /*skip=*/0x7f, "D1: every other queue slot untouched by the dequeue");
        ck_eq((uint32_t)u.move_retry_wait, 0u, "D1: move_retry_wait reset, 0x0043139d");
        ck_eq((uint32_t)u.move_retry_attempts, 0u, "D1: move_retry_attempts reset, 0x004313ad");
        ck_eq((uint32_t)u.move_stuck_countdown, 0u, "D1: move_stuck_countdown reset, 0x004313bd");
        ck_eq_d(u.wander_check_time, 54321.0,
                "D1: wander_check_time untouched -- new cmd_index(0)'s op != 0, so the "
                "time_GetCurrentTime() frontier call at 0x00431384-0x00431390 is correctly SKIPPED");
    }

    // D2: boundary from the OTHER side -- dequeue slot 0x7e (one below the wrap trigger): cmd_index
    // must become a PLAIN 0x7f, not reset to 0. Distinguishes the increment-only path (0x00431350
    // JC taken) from D1's wrap path (JC not taken).
    {
        tact_fixture fx;
        seed_queue(fx.units[UNIT_IDX]);
        tact_unit &u        = fx.units[UNIT_IDX];
        u.cmd_index         = 0x7e;
        u.move_path_slot    = 0;
        u.wander_check_time = 13579.0; // sentinel: slot 0x7f's seeded op is nonzero -> must stay put
        tact_store own      = fx.store();

        detail::unit_cmd_advance(own, UNIT_IDX, 0x7e);

        ck_eq((uint32_t)u.cmd_index, 0x7fu,
              "D2: cmd_index does a PLAIN increment (0x7e->0x7f), JC at 0x00431350 taken, no wrap");
        ck_eq((uint32_t)u.cmd_queue[0x7e].interrupt_flag, (uint32_t)seed_if(0x7e),
              "D2: STALE interrupt_flag survives the dequeue at the pre-boundary slot too");
        ck_eq((uint32_t)u.cmd_queue[0x7e].op, 0u, "D2: dequeued slot 0x7e op zeroed");
        ck_entry(u.cmd_queue[0x7f], seed_if(0x7f), seed_op(0x7f), seed_a0(0x7f), seed_a1(0x7f),
                 seed_a2(0x7f), seed_a3(0x7f), 0x7f,
                 "D2: new cmd_index(0x7f)'s own slot untouched by the dequeue itself");
        ck_eq_d(u.wander_check_time, 13579.0,
                "D2: wander_check_time untouched -- new cmd_index(0x7f)'s op != 0, frontier call skipped");
    }

    // =============================================================================================
    // CLAUSE (2) -- THE STALL TRAP: an op with no case in llm_tact_unit_weapons_tick's dispatch
    // leaves the queue head exactly as it was (cmd_index AND the head slot's op both unchanged).
    // REPRODUCED, not fixed -- these cases assert the BUGGY (original) behaviour.
    // =============================================================================================

    // S1: op==3 -- the same "no-op gap value" tact_unit_weapons_tick_selftest.cpp's own T-NOOP-GAPS
    // block already uses, but that file asserts only a call COUNT; this asserts the actual QUEUE
    // STATE is wedged, which is what the clause requires.
    {
        tact_fixture fx;
        // Default-constructed queue is all-zero (op==0 everywhere), cmd_index==0 -- enqueue lands at
        // the head with nothing else in play.
        tact_store    own = fx.store();
        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, 3, 1, 0, 0, 0, 0);
        ck_eq((uint32_t)ret, 1u, "S1: enqueue of the no-case op succeeds (enqueue does not gate on op)");

        tact_unit &u = fx.units[UNIT_IDX];
        ck_eq((uint32_t)u.cmd_index, 0u, "S1 precondition: head is slot 0");
        ck_eq((uint32_t)u.cmd_queue[0].op, 3u, "S1 precondition: head slot holds the no-case op");

        g_wt_rec.reset();
        g_wt_clock.q   = {100.0, -1.0e9}; // pass the time gate once, then force the 2nd-iteration exit
        g_wt_clock.pos = 0;
        detail::unit_weapons_tick(fx.view(), own, stall_calls(), UNIT_IDX);

        ck_eq((uint32_t)u.cmd_index, 0u,
              "S1: STALL REPRODUCED -- cmd_index unchanged after the tick, op==3 has no dispatch arm "
              "(tact_unit_weapons_tick.cpp step 12, 'op in {2,3}: no-op')");
        ck_eq((uint32_t)u.cmd_queue[0].op, 3u,
              "S1: STALL REPRODUCED -- head slot's op unchanged, queue can never drain past it");
        ck_eq((uint32_t)g_wt_rec.total, 1u,
              "S1: only unit_cmd_queue_advance fires (step 4, unconditional every iteration); no "
              "dispatch-arm callee (cmd_advance et al.) is ever reached for a gap op");
    }

    // S2: op==0x41 -- one past the LAST handled arm below it (0x40, resubmit_run). Boundary from the
    // low side of the [0x41,0x7e] gap.
    {
        tact_fixture  fx;
        tact_store    own = fx.store();
        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, 0x41, 1, 0, 0, 0, 0);
        ck_eq((uint32_t)ret, 1u, "S2: enqueue succeeds");

        tact_unit &u = fx.units[UNIT_IDX];
        g_wt_rec.reset();
        g_wt_clock.q   = {100.0, -1.0e9};
        g_wt_clock.pos = 0;
        detail::unit_weapons_tick(fx.view(), own, stall_calls(), UNIT_IDX);

        ck_eq((uint32_t)u.cmd_index, 0u,
              "S2: STALL REPRODUCED at the low boundary of the gap (op==0x41, just above the "
              "handled 0x40 arm) -- cmd_index unchanged");
        ck_eq((uint32_t)u.cmd_queue[0].op, 0x41u, "S2: head slot's op unchanged (0x41)");
        ck_eq((uint32_t)g_wt_rec.total, 1u, "S2: only unit_cmd_queue_advance fires, no dispatch arm");
    }

    // S3: op==0x7e -- one before the LAST handled arm above it (0x7f, stop/interrupt). Boundary from
    // the high side of the [0x41,0x7e] gap.
    {
        tact_fixture  fx;
        tact_store    own = fx.store();
        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, 0x7e, 1, 0, 0, 0, 0);
        ck_eq((uint32_t)ret, 1u, "S3: enqueue succeeds");

        tact_unit &u = fx.units[UNIT_IDX];
        g_wt_rec.reset();
        g_wt_clock.q   = {100.0, -1.0e9};
        g_wt_clock.pos = 0;
        detail::unit_weapons_tick(fx.view(), own, stall_calls(), UNIT_IDX);

        ck_eq((uint32_t)u.cmd_index, 0u,
              "S3: STALL REPRODUCED at the high boundary of the gap (op==0x7e, just below the "
              "handled 0x7f arm) -- cmd_index unchanged");
        ck_eq((uint32_t)u.cmd_queue[0].op, 0x7eu, "S3: head slot's op unchanged (0x7e)");
        ck_eq((uint32_t)g_wt_rec.total, 1u, "S3: only unit_cmd_queue_advance fires, no dispatch arm");
    }
}

} // namespace mh::tact::test
