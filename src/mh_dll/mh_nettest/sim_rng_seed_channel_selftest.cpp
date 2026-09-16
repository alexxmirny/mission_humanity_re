//
// sim_rng_seed_channel_selftest.cpp -- `simtest` offline oracle for llm_strat_rng_seed_channel
// @0x004b4ce6 (sim/resid/sim_rng_seed_channel.h/.cpp, RI-SIM sim_resid batch E/F).
//
// NO SHADOW SITE (sim_resid rule 1, see the header banner) -- this file is the ONLY verification
// (proof:OFFLINE).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_rng_seed_channel_004b4ce6.asm) -- the .asm is the spec, never the
// .c beside it (the .c has silently lied elsewhere in this project: it once rendered
// `status |= 0x40` as a pointer into a struct field). The whole body is four instructions:
//   0x004b4ceb: EBX = ch (param 1, [EBP+0x8])
//   0x004b4cee-0x004b4cf1: EAX = value (param 2, [EBP+0xc]) & 0xffff -- PRESERVE-BUG: the high 16
//     bits of `value` are unconditionally discarded, regardless of what they hold.
//   0x004b4cf6: _G_LLM_STRAT_RNG_STATE[EBX] = EAX -- PRESERVE-BUG: `ch` is used as a bare scaled
//     index (EBX*0x4 + base) with NO bounds check anywhere in the body.
// Two load-bearing facts, both preserve-bugs, both pinned below from both sides: the mask (a value
// that loses real high bits, and a value already under 0x10000 that must pass through untouched)
// and the index (all 4 in-range channels each land in their own slot, and a write to one channel
// never disturbs any other).
//
#include "sim/resid/sim_rng_seed_channel.h"

#include "sim_test_support.h"

namespace mh::sim::test {

void run_rng_seed_channel_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- masking side A: a value whose high half is non-zero AND distinct from its low half loses
    // the high half (0xDEAD1234 -> 0x1234). All four slots seeded with distinct sentinels first, so
    // the untouched-slot checks prove the store hit ONLY channel 1, not the whole array.
    // =================================================================================================
    {
        fx.reset();
        fx.rng_state[0] = 0x11111111u;
        fx.rng_state[1] = 0x22222222u;
        fx.rng_state[2] = 0x33333333u;
        fx.rng_state[3] = 0x44444444u;

        sim_store own = fx.store();
        detail::rng_seed_channel(own, /*ch=*/1, /*value=*/0xdead1234u);

        ck_eq(fx.rng_state[1], 0x1234u,
              "T1: value & 0xffff strips the high half (0xdead1234 -> 0x1234), AND EAX,0xffff @0x004b4cf1");
        ck_eq(fx.rng_state[0], 0x11111111u, "T1: channel 0 untouched by a ch=1 write, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[2], 0x33333333u, "T1: channel 2 untouched by a ch=1 write, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[3], 0x44444444u, "T1: channel 3 untouched by a ch=1 write, indexed store @0x004b4cf6");
    }

    // =================================================================================================
    // T2 -- masking side B: a value already under 0x10000 passes through the mask completely
    // unchanged (0x0000beef -> 0xbeef) -- proves the mask is a no-op here, not a narrowing that
    // happens to look right on a small number. Distinct fresh sentinels, distinct channel (2).
    // =================================================================================================
    {
        fx.reset();
        fx.rng_state[0] = 0xaaaaaaaau;
        fx.rng_state[1] = 0xbbbbbbbbu;
        fx.rng_state[2] = 0xccccccccu;
        fx.rng_state[3] = 0xddddddddu;

        sim_store own = fx.store();
        detail::rng_seed_channel(own, /*ch=*/2, /*value=*/0x0000beefu);

        ck_eq(fx.rng_state[2], 0x0000beefu,
              "T2: value already < 0x10000 passes through AND EAX,0xffff unchanged @0x004b4cf1");
        ck_eq(fx.rng_state[0], 0xaaaaaaaau, "T2: channel 0 untouched by a ch=2 write, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[1], 0xbbbbbbbbu, "T2: channel 1 untouched by a ch=2 write, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[3], 0xddddddddu, "T2: channel 3 untouched by a ch=2 write, indexed store @0x004b4cf6");
    }

    // =================================================================================================
    // T3 -- indexing: all 4 in-range channels (0..3), each seeded/written with a DISTINCT value, one
    // call per channel, checked incrementally after EACH call. This is what catches a wrong stride
    // (EBX*0x4) or an off-by-one index: a bug there would either land a write in the wrong slot or
    // clobber a slot a previous call in this same sequence already set.
    // =================================================================================================
    {
        fx.reset();
        fx.rng_state[0] = 0x100u;
        fx.rng_state[1] = 0x200u;
        fx.rng_state[2] = 0x300u;
        fx.rng_state[3] = 0x400u;

        sim_store own = fx.store();

        detail::rng_seed_channel(own, /*ch=*/0, /*value=*/0x9001u);
        ck_eq(fx.rng_state[0], 0x9001u, "T3a: ch=0 write lands in slot 0, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[1], 0x200u, "T3a: slot 1 untouched by a ch=0 write");
        ck_eq(fx.rng_state[2], 0x300u, "T3a: slot 2 untouched by a ch=0 write");
        ck_eq(fx.rng_state[3], 0x400u, "T3a: slot 3 untouched by a ch=0 write");

        detail::rng_seed_channel(own, /*ch=*/1, /*value=*/0x9002u);
        ck_eq(fx.rng_state[0], 0x9001u, "T3b: slot 0 retains ch=0's earlier write, not disturbed by ch=1");
        ck_eq(fx.rng_state[1], 0x9002u, "T3b: ch=1 write lands in slot 1, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[2], 0x300u, "T3b: slot 2 untouched by a ch=1 write");
        ck_eq(fx.rng_state[3], 0x400u, "T3b: slot 3 untouched by a ch=1 write");

        detail::rng_seed_channel(own, /*ch=*/2, /*value=*/0x9003u);
        ck_eq(fx.rng_state[0], 0x9001u, "T3c: slot 0 still holds ch=0's write, not disturbed by ch=2");
        ck_eq(fx.rng_state[1], 0x9002u, "T3c: slot 1 still holds ch=1's write, not disturbed by ch=2");
        ck_eq(fx.rng_state[2], 0x9003u, "T3c: ch=2 write lands in slot 2, indexed store @0x004b4cf6");
        ck_eq(fx.rng_state[3], 0x400u, "T3c: slot 3 untouched by a ch=2 write");

        detail::rng_seed_channel(own, /*ch=*/3, /*value=*/0x9004u);
        ck_eq(fx.rng_state[0], 0x9001u, "T3d: slot 0 still holds ch=0's write, not disturbed by ch=3");
        ck_eq(fx.rng_state[1], 0x9002u, "T3d: slot 1 still holds ch=1's write, not disturbed by ch=3");
        ck_eq(fx.rng_state[2], 0x9003u, "T3d: slot 2 still holds ch=2's write, not disturbed by ch=3");
        ck_eq(fx.rng_state[3], 0x9004u, "T3d: ch=3 write lands in slot 3, indexed store @0x004b4cf6");
    }
}

} // namespace mh::sim::test
