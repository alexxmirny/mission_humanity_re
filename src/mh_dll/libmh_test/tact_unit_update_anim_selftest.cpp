//
// tact_unit_update_anim_selftest.cpp -- offline oracle for
//   llm_tact_unit_update_anim @0x0042c547 (libmh/tact/tact_unit_update_anim.cpp)
//
// WHY OFFLINE, NOT RIG: see tact_unit_update_anim.h's PROOF note -- two of this function's paths call
// llm_rand DIRECTLY (@0x0042c878 state 0's jitter draw, @0x0042cc31 state 1's twin), one of TACT-CUT2's
// ungated *effectful* shared callees -- a PRNG-advance double-fired under a shadow window desyncs
// silently. So ALL THREE outward calls (time_GetCurrentTime @0x00427616, llm_rand @0x004da98b,
// llm_tact_quantize_facing_dir @0x0042d0cf) are mocked here via unit_update_anim_calls, with SCRIPTED
// per-call sequences -- this function reads its own mocked clock/rand MULTIPLE times per invocation,
// and the exact call order/count is part of what the header derivation pins (its "TWO SEPARATE calls"
// and "CALL ORDER is rand() then time_now()" notes).
//
// DERIVATION: tmp/decomp_tact/llm_tact_unit_update_anim_0042c547.asm, cross-checked branch by branch
// against tact_unit_update_anim.h's banner and tact_unit_update_anim.cpp. Addresses cited per case.
//
#include "tact/tact_unit_update_anim.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_ID = 5;

// Feeds SCRIPTED, per-call sequences to the three mocked outward calls and RECORDS every
// quantize_facing_dir argument pair. Each vector is indexed by its own call counter and clamped to
// the last element once exhausted (never wraps, never indexes an empty vector) -- a case that
// undercounts its own script re-reads a stale value, which still fails loudly on the resulting wrong
// output instead of reading out-of-bounds memory.
struct anim_mock {
    std::vector<double>  clock_seq;
    size_t               clock_calls = 0;
    std::vector<int32_t> rand_seq;
    size_t               rand_calls      = 0;
    int32_t              quantize_return = 0;
    struct qcall {
        int32_t notch_span;
        int32_t facing_dir;
    };
    std::vector<qcall> quantize_calls;

    void reset() { *this = anim_mock{}; }
};
anim_mock g_m;

double mock_time_now() {
    const size_t i =
        g_m.clock_calls < g_m.clock_seq.size() ? g_m.clock_calls : g_m.clock_seq.size() - 1;
    ++g_m.clock_calls;
    return g_m.clock_seq[i];
}

int32_t mock_rand() {
    const size_t i = g_m.rand_calls < g_m.rand_seq.size() ? g_m.rand_calls : g_m.rand_seq.size() - 1;
    ++g_m.rand_calls;
    return g_m.rand_seq[i];
}

int32_t mock_quantize_facing_dir(int32_t notch_span, int32_t facing_dir) {
    g_m.quantize_calls.push_back({notch_span, facing_dir});
    return g_m.quantize_return;
}

const unit_update_anim_calls &mock_calls() {
    static const unit_update_anim_calls c = {mock_time_now, mock_rand, mock_quantize_facing_dir};
    return c;
}

} // namespace

void run_unit_update_anim_tests() {
    // T1: the type guard -- type > 0x80 returns IMMEDIATELY, before even the cmd_queue read, so
    // NOTHING is written and NONE of the three calls fire. 0x0042c562 (type=unit.type, IMUL*0x5f4
    // stride) / 0x0042c573 (CMP ...,0x80) / 0x0042c57a (JG -> straight to the 0x0042d0c5 epilogue).
    // cmd_queue[0].op is seeded NONZERO and anim_state=0 -- if the guard were broken (or off-by-one),
    // cmd_active would be true, do_reset would fire, and every sentinel below would visibly change.
    {
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq     = {-999.0}; // never touched if the guard holds
        g_m.rand_seq      = {-999};
        tact_unit &u      = fx.units[UNIT_ID];
        u.type            = 0x81; // 0x80+1: strictly greater than the guard's 0x80
        u.anim_state      = 0;
        u.cmd_queue[0].op = 7; // would force do_reset=true if the guard didn't return first
        u.attack_cmd_op   = 0;
        u.status          = 1;
        u.frame_index     = 0x77;
        u.anim_frame_time = 111111.0;
        u.anim_cycle_time = 222222.0;
        u.sprite_id       = 0xbeef;
        u.progress        = 0x30;
        u.facing_dir      = 9;

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        ck_eq_d(u.anim_frame_time, 111111.0, "T1: anim_frame_time untouched, 0x0042c57a return");
        ck_eq((uint32_t)u.frame_index, 0x77u, "T1: frame_index untouched, 0x0042c57a return");
        ck_eq_d(u.anim_cycle_time, 222222.0, "T1: anim_cycle_time untouched, 0x0042c57a return");
        ck_eq((uint32_t)u.sprite_id, 0xbeefu, "T1: sprite_id untouched, 0x0042c57a return");
        ck_eq((uint32_t)g_m.clock_calls, 0u, "T1: time_now not called");
        ck_eq((uint32_t)g_m.rand_calls, 0u, "T1: rand not called");
        ck_eq((uint32_t)g_m.quantize_calls.size(), 0u, "T1: quantize_facing_dir not called");
    }

    // T2: type == 0x80 is the BOUNDARY -- the guard is strict '>' (JG, not JGE/JAE), so 0x80 must
    // proceed past 0x0042c57a. anim_state=5 is a genuine no-op dispatch slot (falls through every
    // CMP/JC/JBE in 0x0042c648-0x0042c672 to the default JMP @0x0042c672), so it cannot touch
    // character_types -- keeping this purely a "did it return early or not" probe, safely inside the
    // fixture's 16-slot character_types table regardless. cmd_queue[0].op nonzero forces
    // cmd_active=true (0x0042c5a5/0x0042c5ad), so do_reset fires unconditionally
    // (0x0042c5ff-0x0042c638): two SEPARATE time_now() calls, order preserved.
    {
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq     = {111.0, 222.0};
        tact_unit &u      = fx.units[UNIT_ID];
        u.type            = 0x80;
        u.anim_state      = 5; // no-op dispatch slot
        u.cmd_queue[0].op = 9;
        u.attack_cmd_op   = 0;
        u.frame_index     = 0x55;
        u.anim_frame_time = 1.0;
        u.anim_cycle_time = 2.0;
        u.sprite_id       = 0xbeef; // must survive: state 5 never touches sprite_id

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        ck_eq_d(u.anim_frame_time, 111.0, "T2: type==0x80 proceeds -- anim_frame_time stamped 1st, 0x0042c60b");
        ck_eq((uint32_t)u.frame_index, 0u, "T2: frame_index zeroed by do_reset, 0x0042c618");
        ck_eq_d(u.anim_cycle_time, 222.0, "T2: anim_cycle_time stamped 2nd (SEPARATE call), 0x0042c62b");
        ck_eq((uint32_t)u.sprite_id, 0xbeefu, "T2: sprite_id untouched -- state 5 is a no-op, 0x0042c672");
        ck_eq((uint32_t)g_m.clock_calls, 2u, "T2: time_now called exactly twice (do_reset block)");
        ck_eq((uint32_t)g_m.quantize_calls.size(), 0u, "T2: quantize_facing_dir not called (no-op state)");
    }

    // T3: the anim_state dispatch domain {0,1,2,3,4,0x1f} -- one case per state the .asm actually
    // branches on (0x0042c648-0x0042c672). States 0/1 use their "just reset" (do_reset=true, hence
    // continuing_anim=false) static-pose sub-path here -- the CONTINUING sub-path is covered
    // separately by T4/T5/T6, which need the rand/clock mocks this dispatch check does not. States
    // 2/3/4/0x1f are UNCONDITIONAL (independent of do_reset/continuing_anim), so do_reset is left
    // false there and frame_index/anim_frame_time/anim_cycle_time are seeded as SENTINELS that must
    // SURVIVE untouched -- proving those states are pure sprite_id writes, unlike 0/1.
    {
        // T3.0 (state 0, not continuing): frames[2], divisor 0x40, NO extra offset. 0x0042c9ce-0x0042ca40.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {10.0, 20.0};
        g_m.quantize_return             = 2;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 7; // cmd_active -> do_reset=true -> continuing_anim=false
        u.progress                      = 0x30;
        u.facing_dir                    = 9;
        u.frame_index                   = 0x55;
        u.anim_frame_time               = 1.0;
        u.anim_cycle_time               = 2.0;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[2] = {300, 8}; // start, count

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 300 + facing(2)*count(8)=16 + progress(0x30=48)/(0x40/8=8)=6 -> 322.
        ck_eq((uint32_t)u.sprite_id, 322u, "T3.0: state 0 reset pose = frames[2] formula, 0x0042ca39");
        ck_eq((uint32_t)u.frame_index, 0u, "T3.0: frame_index zeroed by do_reset");
        ck_eq_d(u.anim_frame_time, 10.0, "T3.0: anim_frame_time stamped (do_reset)");
        ck_eq_d(u.anim_cycle_time, 20.0, "T3.0: anim_cycle_time stamped (do_reset)");
        ck_eq((uint32_t)g_m.quantize_calls.size(), 1u, "T3.0: quantize called once, 0x0042c9fa");
        if (!g_m.quantize_calls.empty())
            ck_eq((uint32_t)g_m.quantize_calls[0].notch_span, 3u, "T3.0: notch_span literal 3");
    }
    {
        // T3.1 (state 1, not continuing): SAME frames[2] shape as state 0's reset pose, PLUS an
        // extra +count/2 half-frame offset state 0 does NOT have. 0x0042cd87-0x0042ce09.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {30.0, 40.0};
        g_m.quantize_return             = 2;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 1;
        u.cmd_queue[0].op               = 7;
        u.progress                      = 0x20;
        u.facing_dir                    = 9;
        u.frame_index                   = 0x55;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[2] = {300, 8};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 300 + facing(2)*count(8)=16 + count/2(4) + progress(0x20=32)/(0x40/8=8)=4 -> 324.
        ck_eq((uint32_t)u.sprite_id, 324u,
              "T3.1: state 1 reset pose = frames[2] + count/2 offset, 0x0042cdbe-0x0042ce02");
        ck_eq((uint32_t)u.frame_index, 0u, "T3.1: frame_index zeroed by do_reset");
        ck_eq_d(u.anim_frame_time, 30.0, "T3.1: anim_frame_time stamped (do_reset)");
        ck_eq_d(u.anim_cycle_time, 40.0, "T3.1: anim_cycle_time stamped (do_reset)");
    }
    {
        // T3.2 (state 2): unconditional, frames[3], divisor 0x10, ASCENDING (no threshold).
        // 0x0042ce0e-0x0042d0c5.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {-777.0};
        g_m.rand_seq                    = {-777};
        g_m.quantize_return             = 2;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 2;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.progress                      = 8;
        u.facing_dir                    = 9;
        u.frame_index                   = 0x66; // sentinel: state 2 must not touch this
        u.anim_frame_time               = 3.0;  // sentinel
        u.anim_cycle_time               = 4.0;  // sentinel
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[3] = {400, 4};
        fx.key_rshift_held              = 0;
        fx.key_lshift_held              = 0;

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 400 + facing(2)*count(4)=8 + progress(8)/(0x10/4=4)=2 -> 410.
        ck_eq((uint32_t)u.sprite_id, 410u, "T3.2: state 2 ascending, 0x0042ce79");
        ck_eq((uint32_t)u.frame_index, 0x66u, "T3.2: frame_index UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_frame_time, 3.0, "T3.2: anim_frame_time UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_cycle_time, 4.0, "T3.2: anim_cycle_time UNTOUCHED (unconditional state)");
        ck_eq((uint32_t)g_m.clock_calls, 0u, "T3.2: time_now never called");
    }
    {
        // T3.3 (state 3): unconditional, SAME frames[3] slot as state 2, divisor 0x10, DESCENDING
        // with threshold 0 (no subtraction from progress at all -- genuinely different from every
        // other descending case). 0x0042ce85-0x0042ceff.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {-777.0};
        g_m.quantize_return             = 2;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 3;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.progress                      = 8;
        u.facing_dir                    = 9;
        u.frame_index                   = 0x66;
        u.anim_frame_time               = 3.0;
        u.anim_cycle_time               = 4.0;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[3] = {400, 4};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 400 + facing(2)*count(4)=8 + (count-1)=3 - progress(8)/(0x10/4=4)=2 -> 409.
        ck_eq((uint32_t)u.sprite_id, 409u, "T3.3: state 3 descending, threshold 0, 0x0042cef8");
        ck_eq((uint32_t)u.frame_index, 0x66u, "T3.3: frame_index UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_frame_time, 3.0, "T3.3: anim_frame_time UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_cycle_time, 4.0, "T3.3: anim_cycle_time UNTOUCHED (unconditional state)");
    }
    {
        // T3.4 (state 4): unconditional, frames[5], threshold on progress vs 0x20, divisor 0x20 --
        // BELOW-threshold (ascending) branch. 0x0042cf04-0x0042d003.
        tact_fixture fx;
        g_m.reset();
        g_m.quantize_return             = 2;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 4;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.progress                      = 0x10; // < 0x20: below-threshold branch
        u.facing_dir                    = 9;
        u.frame_index                   = 0x66;
        u.anim_frame_time               = 3.0;
        u.anim_cycle_time               = 4.0;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[5] = {600, 4};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 600 + facing(2)*count(4)=8 + progress(0x10=16)/(0x20/4=8)=2 -> 610.
        ck_eq((uint32_t)u.sprite_id, 610u, "T3.4: state 4 below-threshold, 0x0042cf7f");
        ck_eq((uint32_t)u.frame_index, 0x66u, "T3.4: frame_index UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_frame_time, 3.0, "T3.4: anim_frame_time UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_cycle_time, 4.0, "T3.4: anim_cycle_time UNTOUCHED (unconditional state)");
    }
    {
        // T3.1f (state 0x1f): unconditional, frames[4], threshold on progress vs 0x20 -- BELOW
        // threshold (ascending, same divisor-0x20 shape as state 4's below branch). The ABOVE branch
        // (a genuinely different FLAT formula) is covered by T6c below. 0x0042d008-0x0042d088.
        tact_fixture fx;
        g_m.reset();
        g_m.quantize_return             = 2;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0x1f;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.progress                      = 0x18; // < 0x20: below-threshold branch
        u.facing_dir                    = 9;
        u.frame_index                   = 0x66;
        u.anim_frame_time               = 3.0;
        u.anim_cycle_time               = 4.0;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[4] = {500, 8};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 500 + facing(2)*count(8)=16 + progress(0x18=24)/(0x20/8=4)=6 -> 522.
        ck_eq((uint32_t)u.sprite_id, 522u, "T3.1f: state 0x1f below-threshold, 0x0042d081");
        ck_eq((uint32_t)u.frame_index, 0x66u, "T3.1f: frame_index UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_frame_time, 3.0, "T3.1f: anim_frame_time UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_cycle_time, 4.0, "T3.1f: anim_cycle_time UNTOUCHED (unconditional state)");
    }

    // T4: the shift-held do_reset trigger, reached only when cmd_active is FALSE
    // (0x0042c5a5-0x0042c5be both zero). shift_held = (key_rshift_held&1)!=0 || (key_lshift_held&1)!=0
    // (0x0042c5c2-0x0042c5dd, TEST byte,0x1 -- a BIT test, not a truthiness test), ANDed with
    // (status&1)!=0 (0x0042c5ea-0x0042c5fb). When it fires, do_reset=true short-circuits the smooth
    // continuing_anim advance straight to the "just reset" static frames[2] pose; when it doesn't,
    // the unit falls into advance_running_anim's frames[1] "no wrap yet" pose instead -- two entirely
    // different formulas, which is the observable "speedup" this case pins.
    {
        // T4.A: key_rshift_held=1 (bit 0 set) -> do_reset fires.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {60.0, 70.0};
        g_m.quantize_return             = 3;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.status                        = 1;
        u.progress                      = 0x20;
        u.facing_dir                    = 9;
        u.frame_index                   = 0x55;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[2] = {700, 4};
        fx.key_rshift_held              = 1;
        fx.key_lshift_held              = 0;

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 700 + facing(3)*count(4)=12 + progress(0x20=32)/(0x40/4=16)=2 -> 714.
        ck_eq((uint32_t)u.sprite_id, 714u, "T4.A: rshift bit0 set -> reset pose, 0x0042c5c2/0x0042c5c9");
        ck_eq((uint32_t)u.frame_index, 0u, "T4.A: do_reset fired -- frame_index zeroed");
        ck_eq_d(u.anim_frame_time, 60.0, "T4.A: do_reset fired -- anim_frame_time stamped");
        ck_eq_d(u.anim_cycle_time, 70.0, "T4.A: do_reset fired -- anim_cycle_time stamped");
    }
    {
        // T4.B: both flags 0 -> do_reset does NOT fire; continuing_anim's frames[1] pose runs
        // instead. Clock is set so the cycle-window check is skipped (now1 > cycle_deadline,
        // 0x0042c6b8/0x0042c6be) and the step-deadline check does NOT fire either (step_deadline >=
        // now2, 0x0042c6df/0x0042c6e5), landing on the frames[1] "no wrap this tick" pose with
        // frame_index UNCHANGED -- 0x0042c77f-0x0042c86c.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {5.0, 50.0};
        g_m.quantize_return             = 3;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.status                        = 1;
        u.facing_dir                    = 9;
        u.anim_cycle_time               = 1.0;
        u.frame_interval                = 1.0;   // cycle_deadline = 2.0 < now1(5.0) -- skip jitter path
        u.anim_frame_time               = 100.0; // step_deadline = 100.25 (STEP_SEC_1 fixture default 0.25)
        u.frame_index                   = 5;     // < 0x10: ascending frames[1] sub-formula
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[1] = {800, 4};
        fx.key_rshift_held              = 0;
        fx.key_lshift_held              = 0;

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 800 + facing(3)*count(4)=12 + frame_index(5)/(0x10/4=4)=1 -> 813.
        ck_eq((uint32_t)u.sprite_id, 813u, "T4.B: no shift -> frames[1] pose, 0x0042c7f8");
        ck_eq((uint32_t)u.frame_index, 5u, "T4.B: no reset -- frame_index unchanged");
        ck_eq_d(u.anim_frame_time, 100.0, "T4.B: no reset -- anim_frame_time unchanged");
        ck_eq_d(u.anim_cycle_time, 1.0, "T4.B: no reset -- anim_cycle_time unchanged");
        ck_eq((uint32_t)g_m.rand_calls, 0u, "T4.B: jitter path not taken -- rand not called");
    }
    {
        // T4.C: key_lshift_held=1 (rshift=0) -> do_reset fires too (it's an OR of the two flags).
        // Same frames[2]/progress/facing as T4.A on purpose: identical inputs via the OTHER flag
        // must land on the identical reset pose.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {80.0, 90.0};
        g_m.quantize_return             = 3;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.status                        = 1;
        u.progress                      = 0x20;
        u.facing_dir                    = 9;
        u.frame_index                   = 0x55;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[2] = {700, 4};
        fx.key_rshift_held              = 0;
        fx.key_lshift_held              = 1;

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        ck_eq((uint32_t)u.sprite_id, 714u, "T4.C: lshift bit0 set -> reset pose (OR), 0x0042c5cb/0x0042c5d2");
        ck_eq((uint32_t)u.frame_index, 0u, "T4.C: do_reset fired -- frame_index zeroed");
        ck_eq_d(u.anim_frame_time, 80.0, "T4.C: do_reset fired -- anim_frame_time stamped");
        ck_eq_d(u.anim_cycle_time, 90.0, "T4.C: do_reset fired -- anim_cycle_time stamped");
    }
    {
        // T4.D: key_rshift_held=2 (bit 0 CLEAR, bit 1 set) -> do_reset must NOT fire. This is the
        // case that distinguishes a real `&1` bit test (0x0042c5c2: TEST byte,0x1) from a truthiness
        // test -- a naive `if(flag)` would treat 2 as true and wrongly reset. Same shape as T4.B
        // (frames[1] pose, unchanged timestamps) but a distinct frame_index so the two cases can't
        // pass on a coincidentally-shared expected value.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {15.0, 45.0};
        g_m.quantize_return             = 3;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.status                        = 1;
        u.facing_dir                    = 9;
        u.anim_cycle_time               = 1.0;
        u.frame_interval                = 1.0;
        u.anim_frame_time               = 100.0;
        u.frame_index                   = 9; // distinct from T4.B's 5
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[1] = {800, 4};
        fx.key_rshift_held              = 2; // bit 0 clear
        fx.key_lshift_held              = 0;

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 800 + facing(3)*count(4)=12 + frame_index(9)/(0x10/4=4)=2 -> 814.
        ck_eq((uint32_t)u.sprite_id, 814u, "T4.D: rshift=2 (&1 clear) -> no reset, frames[1] pose");
        ck_eq((uint32_t)u.frame_index, 9u, "T4.D: no reset -- frame_index unchanged (proves &1, not truthy)");
        ck_eq_d(u.anim_frame_time, 100.0, "T4.D: no reset -- anim_frame_time unchanged");
        ck_eq_d(u.anim_cycle_time, 1.0, "T4.D: no reset -- anim_cycle_time unchanged");
        ck_eq((uint32_t)g_m.rand_calls, 0u, "T4.D: jitter path not taken -- rand not called");
    }

    // T5: the jittered per-frame deadline's exact FP comparison. jittered_deadline =
    // rand()*jitter_scale_1 + anim_frame_time (0x0042c880-0x0042c88f); compared via
    // FCOMP/FNSTSW/SAHF/JBE @0x0042c897-0x0042c89d, which is `if (jittered_deadline < time_now())` in
    // the .cpp -- JBE (jittered<=now->skip) is taken on EQUALITY, so the increment fires only on a
    // STRICT '<', never on '=='. Both cases share rand=4, jitter_scale_1=0.5 (fixture default) and
    // anim_frame_time=10.0, so jittered_deadline=12.0 exactly (4*0.5 is exact in binary FP) in both --
    // only the SECOND time_now() draw differs across the boundary.
    {
        // T5.a: now(2nd call)=12.5 > 12.0 -> jittered_deadline < now -> FIRES.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {1.0, 12.5, 99.0};
        g_m.rand_seq                    = {4};
        g_m.quantize_return             = 3;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.facing_dir                    = 9;
        u.anim_cycle_time               = 0.0;
        u.frame_interval                = 100.0; // cycle_deadline=100.0 >= now1(1.0) -- ENTER the jitter path
        u.anim_frame_time               = 10.0;
        u.frame_index                   = 7;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[0] = {900, 4};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 900 + facing(3)*count(4)=12 + frame_index(8)/(0x10/4=4)=2 -> 914.
        ck_eq((uint32_t)u.sprite_id, 914u, "T5.a: jitter fires (12.0 < 12.5), 0x0042c95c");
        ck_eq((uint32_t)u.frame_index, 8u, "T5.a: frame_index incremented, 0x0042c8b1");
        ck_eq_d(u.anim_frame_time, 99.0, "T5.a: anim_frame_time stamped (3rd time_now call), 0x0042c8ab");
        ck_eq_d(u.anim_cycle_time, 0.0, "T5.a: anim_cycle_time untouched (no wrap on this path)");
        ck_eq((uint32_t)g_m.clock_calls, 3u, "T5.a: 3 time_now calls (cycle-check, jitter-compare, stamp)");
        ck_eq((uint32_t)g_m.rand_calls, 1u, "T5.a: rand called exactly once");
    }
    {
        // T5.b: now(2nd call)=12.0 == 12.0 -> jittered_deadline < now is FALSE at equality -- does
        // NOT fire. Identical rand/anim_frame_time/jitter_scale to T5.a; only now2 differs.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {1.0, 12.0};
        g_m.rand_seq                    = {4};
        g_m.quantize_return             = 3;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.facing_dir                    = 9;
        u.anim_cycle_time               = 0.0;
        u.frame_interval                = 100.0;
        u.anim_frame_time               = 10.0;
        u.frame_index                   = 7;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[0] = {900, 4};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // 900 + facing(3)*count(4)=12 + frame_index(7, UNCHANGED)/(0x10/4=4)=1 -> 913.
        ck_eq((uint32_t)u.sprite_id, 913u, "T5.b: jitter does NOT fire at equality (12.0==12.0)");
        ck_eq((uint32_t)u.frame_index, 7u, "T5.b: frame_index unchanged -- no increment on '=='");
        ck_eq_d(u.anim_frame_time, 10.0, "T5.b: anim_frame_time unchanged -- no stamp on '=='");
        ck_eq_d(u.anim_cycle_time, 0.0, "T5.b: anim_cycle_time unchanged");
        ck_eq((uint32_t)g_m.clock_calls, 2u,
              "T5.b: only 2 time_now calls -- the 3rd (stamp) is INSIDE the not-taken branch");
        ck_eq((uint32_t)g_m.rand_calls, 1u, "T5.b: rand still called once (drawn before the compare)");
    }

    // T6: the wrap-pose arithmetic -- when frame_index just crossed 0x1f, the pose comes from
    // frames[0] ALONE with NO division at all: sprite_id = (facing*count + start) mod 0x10000 (a
    // genuine 16-bit truncation, review-confirmed). facing/count/start below are chosen so
    // facing*count+start EXCEEDS 0xffff, making the truncation observable rather than coincidentally
    // in-range.
    {
        // T6.a (state 0's wrap, 0x0042c721-0x0042c773): reach it via the STEP-deadline branch (jitter
        // skipped: now1 > cycle_deadline), with frame_index pre-seeded at 0x1f so the increment
        // crosses to 0x20 and trips the wrap. This sub-path ALSO restamps anim_cycle_time and returns
        // EARLY (no frames[1] fallthrough) -- quantize_calls.size()==1 proves the early return, since
        // a fallthrough would add a second call and overwrite sprite_id with a frames[1]-derived value.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {5.0, 9.0, 77.0, 88.0};
        g_m.rand_seq                    = {-1}; // unused -- jitter path is skipped
        g_m.quantize_return             = 300;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.facing_dir                    = 9;
        u.anim_cycle_time               = 1.0;
        u.frame_interval                = 1.0;  // cycle_deadline=2.0 < now1(5.0) -- skip jitter
        u.anim_frame_time               = 1.0;  // step_deadline=1.25 (STEP_SEC_1=0.25) < now2(9.0) -- fires
        u.frame_index                   = 0x1f; // -> 0x20 on increment: wraps
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[0] = {40000, 250};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // (facing(300)*count(250) + start(40000)) mod 0x10000 = 115000 mod 65536 = 49464.
        ck_eq((uint32_t)u.sprite_id, 49464u, "T6.a: state 0 wrap pose truncates mod 0x10000, 0x0042c773");
        ck_eq((uint32_t)u.frame_index, 0u, "T6.a: frame_index reset to 0 on wrap, 0x0042c721");
        ck_eq_d(u.anim_frame_time, 77.0, "T6.a: anim_frame_time stamped (3rd time_now), 0x0042c6f7");
        ck_eq_d(u.anim_cycle_time, 88.0, "T6.a: anim_cycle_time restamped on wrap, 0x0042c734");
        ck_eq((uint32_t)g_m.clock_calls, 4u, "T6.a: 4 time_now calls total");
        ck_eq((uint32_t)g_m.rand_calls, 0u, "T6.a: jitter path skipped -- rand not called");
        ck_eq((uint32_t)g_m.quantize_calls.size(), 1u, "T6.a: single quantize call proves the EARLY return");
    }
    {
        // T6.b (state 1's twin wrap, 0x0042cad3-0x0042cb2c): identical shape, tuned by
        // STEP_SEC_2/JITTER_SCALE_2 (unused here since jitter is skipped too), SAME frames[0] slot as
        // state 0's wrap -- but with a DIFFERENT overflowing product so this case can't pass by
        // accidentally reusing T6.a's numbers.
        tact_fixture fx;
        g_m.reset();
        g_m.clock_seq                   = {5.0, 9.0, 177.0, 188.0};
        g_m.quantize_return             = 400;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 1;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.facing_dir                    = 9;
        u.anim_cycle_time               = 1.0;
        u.frame_interval                = 1.0;
        u.anim_frame_time               = 1.0; // step_deadline=1.125 (STEP_SEC_2=0.125) < now2(9.0) -- fires
        u.frame_index                   = 0x1f;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[0] = {50000, 200};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // (facing(400)*count(200) + start(50000)) mod 0x10000 = 130000 mod 65536 = 64464.
        ck_eq((uint32_t)u.sprite_id, 64464u, "T6.b: state 1 wrap pose truncates mod 0x10000, 0x0042cb2c");
        ck_eq((uint32_t)u.frame_index, 0u, "T6.b: frame_index reset to 0 on wrap, 0x0042cada");
        ck_eq_d(u.anim_frame_time, 177.0, "T6.b: anim_frame_time stamped, 0x0042caa9-0x0042cab0");
        ck_eq_d(u.anim_cycle_time, 188.0, "T6.b: anim_cycle_time restamped on wrap, 0x0042caed");
        ck_eq((uint32_t)g_m.clock_calls, 4u, "T6.b: 4 time_now calls total");
        ck_eq((uint32_t)g_m.quantize_calls.size(), 1u, "T6.b: single quantize call proves the EARLY return");
    }
    {
        // T6.c (state 0x1f's FLAT pose, progress>=0x20, 0x0042d08a-0x0042d0be): NO division at all --
        // sprite_id = facing*count + start + count-1, mod 0x10000. Distinct from state 4's
        // above-threshold branch, which DOES subtract a divided term (frame_sprite_descending); this
        // is the flat "last frame" shape unique to 0x1f. Unconditional dispatch, so do_reset is left
        // false (0 clock calls) and the timestamp fields are sentinels that must survive.
        tact_fixture fx;
        g_m.reset();
        g_m.quantize_return             = 1000;
        tact_unit &u                    = fx.units[UNIT_ID];
        u.type                          = 3;
        u.anim_state                    = 0x1f;
        u.cmd_queue[0].op               = 0;
        u.attack_cmd_op                 = 0;
        u.progress                      = 0x50; // >= 0x20: the flat branch
        u.facing_dir                    = 9;
        u.frame_index                   = 0x44;
        u.anim_frame_time               = 3.0;
        u.anim_cycle_time               = 4.0;
        u.sprite_id                     = 0xdead;
        fx.character_types[3].frames[4] = {20000, 100};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

        // (facing(1000)*count(100) + start(20000) + (count-1)=99) mod 0x10000
        //   = 120099 mod 65536 = 54563.
        ck_eq((uint32_t)u.sprite_id, 54563u, "T6.c: state 0x1f flat pose truncates mod 0x10000, 0x0042d0be");
        ck_eq((uint32_t)u.frame_index, 0x44u, "T6.c: frame_index UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_frame_time, 3.0, "T6.c: anim_frame_time UNTOUCHED (unconditional state)");
        ck_eq_d(u.anim_cycle_time, 4.0, "T6.c: anim_cycle_time UNTOUCHED (unconditional state)");
        ck_eq((uint32_t)g_m.clock_calls, 0u, "T6.c: time_now never called");
    }

    // T7: character_types indexing -- `type_idx = u.type` (0x0042c570-0x0042c573) selects the ROW of
    // the mission's character-type table (TACT_CHARACTER_TYPE_SLOTS=16 in this fixture, matching
    // RID_TACT_CHARACTER_TYPES's real 16-slot extent). All 16 slots get a DISTINCT frames[3] (start =
    // 1000+i*100) so a hardcoded-to-slot-0 (or off-by-one) bug produces an observably wrong sprite_id
    // rather than accidentally matching. type=1/8/0xf span low/mid/top of the valid range.
    //
    // NOT tested: type in [0x10,0x80]. tact_unit_update_anim.h's own UNCERTAINTY section documents
    // that range as UNCHECKED past the guard and read straight into character_types[type] with no
    // bound to 16 -- for a type that high this fixture's character_types vector (also sized exactly
    // 16) would be read out of bounds too, so exercising it here would be testing fixture-OOB
    // behaviour, not the function; it stays a documented open question, not a passing case.
    {
        tact_fixture fx;
        for (int32_t i = 0; i < mh::tact::TACT_CHARACTER_TYPE_SLOTS; ++i) {
            fx.character_types[(size_t)i].frames[3] = {(uint16_t)(1000 + i * 100), 4};
        }

        {
            g_m.reset();
            g_m.quantize_return = 2;
            tact_unit &u        = fx.units[UNIT_ID];
            u                   = tact_unit{};
            u.type              = 1;
            u.anim_state        = 2;
            u.progress          = 8;
            u.facing_dir        = 9;
            u.sprite_id         = 0xdead;

            tact_view  tv  = fx.view();
            tact_store own = fx.store();
            detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

            // 1100 + facing(2)*count(4)=8 + progress(8)/(0x10/4=4)=2 -> 1110.
            ck_eq((uint32_t)u.sprite_id, 1110u, "T7: type=1 hits character_types[1], not slot 0");
        }
        {
            g_m.reset();
            g_m.quantize_return = 2;
            tact_unit &u        = fx.units[UNIT_ID];
            u                   = tact_unit{};
            u.type              = 8;
            u.anim_state        = 2;
            u.progress          = 8;
            u.facing_dir        = 9;
            u.sprite_id         = 0xdead;

            tact_view  tv  = fx.view();
            tact_store own = fx.store();
            detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

            // 1800 + 8 + 2 -> 1810.
            ck_eq((uint32_t)u.sprite_id, 1810u, "T7: type=8 hits character_types[8], a distinct mid slot");
        }
        {
            g_m.reset();
            g_m.quantize_return = 2;
            tact_unit &u        = fx.units[UNIT_ID];
            u                   = tact_unit{};
            u.type              = 0xf;
            u.anim_state        = 2;
            u.progress          = 8;
            u.facing_dir        = 9;
            u.sprite_id         = 0xdead;

            tact_view  tv  = fx.view();
            tact_store own = fx.store();
            detail::unit_update_anim(tv, own, mock_calls(), UNIT_ID);

            // 2500 + 8 + 2 -> 2510.
            ck_eq((uint32_t)u.sprite_id, 2510u, "T7: type=0xf hits character_types[15], top of the table");
        }
    }
}

} // namespace mh::tact::test
