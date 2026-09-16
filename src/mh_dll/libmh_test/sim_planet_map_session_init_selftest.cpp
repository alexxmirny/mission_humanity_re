//
// sim_planet_map_session_init_selftest.cpp -- `simtest` offline oracle for
// llm_strat_planet_map_session_init @0x004dc65a (sim/resid/sim_planet_map_session_init.h/.cpp,
// RI-SIM sim_resid batch).
//
// NO SHADOW SITE -- session-entry-only writer (see the header banner's "sim_resid rule 1" note).
// This file is the ONLY verification (proof:OFFLINE).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_planet_map_session_init_004dc65a.asm) -- the .asm is the spec,
// never the .c beside it (the .c has silently lied elsewhere in this project: it once rendered
// `status |= 0x40` as a pointer into a struct field):
//   0x004dc666-0x004dc66c: width_m = map_width - 1 (unsigned store -- a map_width of 0 wraps to
//     0xffffffff, DEC EAX on zero).
//   0x004dc671-0x004dc677: height_m = map_height - 1, the SAME shape, a SEPARATE region.
//   0x004dc67c: llm_strat_bldg_recompute_cell_grid(), no args, unconditional, no gate anywhere in
//     this body.
//   0x004dc681/0x004dc683/0x004dc685: llm_strat_rng_seed_channel(channel=2, value=0) -- PUSH 0x0
//     (value) then PUSH 0x2 (channel), so channel is the FIRST argument, value the second.
//   0x004dc68d: _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT = 0 (a plain store, not a decrement/compare).
//   0x004dc697: llm_strat_ai_spiral_table_init(), no args, unconditional.
//   0x004dc69c-0x004dc744: nine llm_gfx_pack_rgb16 calls filling the two small palettes. THESE ARE
//     NO LONGER LIBMH'S (LIFT-TABLE S4, 2026-09-09): the block moved host-side with its nine cells
//     -- MF_VIEW/OWN_ISLAND presentation that migration had left in the sim store -- and
//     llm_gfx_pack_rgb16 left the host table with it, this having been its only libmh caller. What
//     this oracle can still hold is the INSTANT and the ARITY: exactly one
//     LIBMH_EVK_INV_PLANET_MAP_PALETTE record, emitted after the spiral-table init, once per call.
//     The nine triples, their order and the (red, blue, green) argument order are pinned where they
//     now live, in the sink's own comment (seams/host_event_sink.cpp).
//
//     What was lost with them is worth naming rather than glossing: the old arms distinguished
//     calls [2]/[7] and [4]/[6], the two argument-identical magenta/black pairs, by returning a
//     per-index value. No libmh-side test can hold that any more -- the block is not libmh's -- and
//     nothing on the host side substitutes for it. The nine cells are write-only image-wide (one
//     WRITE xref each, no reader anywhere), so there is no oracle that could have; that is the same
//     measurement that made section 6's UI-capture oracle for S4 vacuous.
//
#include "sim/resid/sim_planet_map_session_init.h"

#include <cstdio>

#include "sim_test_support.h"
#include "state/host_events.h" // LIFT-TABLE S4: the palette block is a record now, drained here

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Own tiny recorder -- three callees, not worth generated-stub machinery. (It had a fourth,
// pack_rgb16, with a per-call-index return so the two argument-identical colour pairs stayed
// distinguishable; that callee left libmh at LIFT-TABLE S4 together with the block that called it.)
struct planet_map_session_init_log_t {
    int bldg_recompute_cell_grid_calls = 0;
    int ai_spiral_table_init_calls     = 0;

    int      rng_seed_channel_calls   = 0;
    int32_t  rng_seed_channel_channel = -1;
    uint32_t rng_seed_channel_value   = 0xdeadbeef;

    void reset() { *this = planet_map_session_init_log_t{}; }
};
planet_map_session_init_log_t g_log;

const planet_map_session_init_calls &recording_calls() {
    static const planet_map_session_init_calls c = {
        [](int32_t channel, uint32_t value) {
            ++g_log.rng_seed_channel_calls;
            g_log.rng_seed_channel_channel = channel;
            g_log.rng_seed_channel_value   = value;
        },
        []() { ++g_log.bldg_recompute_cell_grid_calls; },
        []() { ++g_log.ai_spiral_table_init_calls; },
    };
    return c;
}

} // namespace

void run_planet_map_session_init_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the whole happy path in one shot (the body has no branches: every call fires exactly
    // once, unconditionally). Seeds every one of the nine palette cells + ai_active_player_count to
    // a DISTINCT, non-target value first, so "already had the right value" cannot masquerade as a
    // real write; and pins the CALL ORDER + the (r,g,b) argument at each of the nine call sites.
    // =================================================================================================
    {
        fx.reset();
        fx.map_width  = 57; // distinct, non-power-of-two, unrelated to map_height
        fx.map_height = 33;

        fx.ai_active_player_count = 77; // nonzero sentinel -- must become exactly 0, not decremented

        g_log.reset();
        sim_store own = fx.store();
        detail::planet_map_session_init(fx.view(), own, recording_calls());

        ck_eq(fx.width_m, 56u, "T1: width_m = map_width(57) - 1 = 56, 0x004dc666-0x004dc66c");
        ck_eq(fx.height_m, 32u, "T1: height_m = map_height(33) - 1 = 32, 0x004dc671-0x004dc677 (SEPARATE "
                                "region from width_m -- an axis swap would fail this against T1's own "
                                "distinct 57/33 inputs)");

        ck(g_log.bldg_recompute_cell_grid_calls == 1,
           "T1: llm_strat_bldg_recompute_cell_grid() called exactly once, unconditionally, 0x004dc67c");
        ck(g_log.ai_spiral_table_init_calls == 1,
           "T1: llm_strat_ai_spiral_table_init() called exactly once, unconditionally, 0x004dc697");

        ck(g_log.rng_seed_channel_calls == 1,
           "T1: llm_strat_rng_seed_channel() called exactly once, 0x004dc685");
        ck((int32_t)g_log.rng_seed_channel_channel == 2,
           "T1: rng_seed_channel channel arg = 2 (PUSH 0x2 is the LAST push, i.e. the FIRST "
           "argument), 0x004dc683");
        ck(g_log.rng_seed_channel_value == 0u,
           "T1: rng_seed_channel value arg = 0 (PUSH 0x0 is the FIRST push, i.e. the SECOND "
           "argument) -- a channel/value swap would read (0, 2) instead, 0x004dc681");

        ck_eq(fx.ai_active_player_count, 0u,
              "T1: _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT set to 0 by a plain store (seeded 77 "
              "beforehand, so this proves an overwrite, not a coincidental default), 0x004dc68d");

        // The palette block is host-side now: what libmh still owns is the emit, and the two
        // things a translation could get wrong about it are the COUNT and the KIND.
        {
            libmh_event    d[8];
            const uint32_t n = libmh_poll_events(d, 8);
            ck(n == 1 && d[0].channel == LIBMH_EVC_INVALIDATE &&
                   d[0].kind == LIBMH_EVK_INV_PLANET_MAP_PALETTE,
               "T1: exactly ONE LIBMH_EVK_INV_PLANET_MAP_PALETTE record is emitted, and nothing "
               "else -- the nine pack_rgb16 calls became this instant, 0x004dc69c-0x004dc744");
        }
    }

    // =================================================================================================
    // T2 -- width_m's unsigned-wrap sentinel: map_width=0 makes DEC EAX wrap to 0xffffffff, not to a
    // signed -1 that a uint32_t comparison would treat as huge-but-different. map_height is a distinct,
    // ordinary value on the OTHER side of the boundary so a translation that swapped which axis reads
    // map_width vs map_height disagrees with one of T2/T3.
    // =================================================================================================
    {
        fx.reset();
        fx.map_width  = 0;  // DEC EAX on 0 -> 0xffffffff
        fx.map_height = 10; // ordinary value, on the far side of the boundary from map_width here

        g_log.reset();
        sim_store own = fx.store();
        detail::planet_map_session_init(fx.view(), own, recording_calls());

        ck_eq(fx.width_m, 0xffffffffu,
              "T2: map_width=0 -> width_m wraps to 0xffffffff (unsigned DEC-of-zero), 0x004dc66b-0x004dc66c");
        ck_eq(fx.height_m, 9u,
              "T2: map_height=10 -> height_m=9 on the SAME call -- proves the wrap is confined to "
              "width_m's own axis, 0x004dc676-0x004dc677");
    }

    // =================================================================================================
    // T3 -- the mirror of T2: height_m's unsigned-wrap sentinel, map_width on the ordinary side. This
    // pair (T2+T3) is what catches an axis swap: a translation reading map_height for width_m and vice
    // versa would pass T1 (57/33, both ordinary) but fail T2 or T3.
    // =================================================================================================
    {
        fx.reset();
        fx.map_width  = 10; // ordinary value
        fx.map_height = 0;  // DEC EAX on 0 -> 0xffffffff

        g_log.reset();
        sim_store own = fx.store();
        detail::planet_map_session_init(fx.view(), own, recording_calls());

        ck_eq(fx.width_m, 9u, "T3: map_width=10 -> width_m=9, 0x004dc66b-0x004dc66c");
        ck_eq(fx.height_m, 0xffffffffu,
              "T3: map_height=0 -> height_m wraps to 0xffffffff, confined to height_m's own axis, "
              "0x004dc676-0x004dc677");
    }
}

} // namespace mh::sim::test
