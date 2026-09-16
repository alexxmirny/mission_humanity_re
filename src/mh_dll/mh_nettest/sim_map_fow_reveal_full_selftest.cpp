//
// sim_map_fow_reveal_full_selftest.cpp -- `simtest` oracle for llm_map_fow_reveal_full @0x0049ab26
// (sim/sim_map_fow_reveal_full.h/.cpp, RI-SIM SIM1D / SIM1E opening).
//
// order-code 0xf9 (high/special command range), single arg (player). shadow_region_closure.py
// --depth 4 converges at 4 functions / 2 regions (_G_LLM_STRAT_G_TMP_PLAYER,
// _G_LLM_STRAT_G_TMP_SIGHT), both owned and written by the SOLE callee map_fow_UpdateFoWPlus
// (@0x0049681a) -- so this oracle MOCKS that callee and asserts the arguments + call COUNT it
// receives, per the ledger, rather than modelling what it does. The site was armed and made ZERO
// calls in a 15000-step all-AI soak (no AI/skirmish path issues this
// order code), so this offline oracle is the only evidence this function will ever have.
//
// EXPECTED BEHAVIOUR from the .asm (tmp/decomp_sim/llm_map_fow_reveal_full_0049ab26.asm), which the
// .cpp matches exactly (no divergence found):
//   0x0049ab41: outer counter x = 0.
//   0x0049ab48-0x0049ab53: outer check -- x < width (0x00825084, sim_view::map_width, SIGNED `JL`);
//     false -> exit the whole function (0x0049ab8c).
//   0x0049ab5b: inner counter y = 0 (entered only when the outer check passed).
//   0x0049ab62-0x0049ab6d: inner check -- y < height (0x00825064, sim_view::map_height, SIGNED
//     `JL`); false -> fall through to the outer increment (0x0049ab55) without ever calling out for
//     this x.
//   0x0049ab75-0x0049ab83: body -- map_fow_UpdateFoWPlus(EAX=player, EDX=x, EBX=y, ECX=0xa) i.e.
//     (player, x, y, sight=10 LITERAL, unconditional every call).
//   0x0049ab6f: inner increment, y += 6, back to the inner check (0x0049ab62).
//   0x0049ab55: outer increment, x += 6, back to the outer check (0x0049ab48).
//   Net shape: for (x = 0; x < width; x += 6) for (y = 0; y < height; y += 6)
//                  map_fow_UpdateFoWPlus(player, x, y, 10);
//   No sim state is written by this closure itself -- its only effect is the outward call.
//
#include "sim/sim_map_fow_reveal_full.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct fow_call {
    uint32_t player;
    uint32_t x;
    uint32_t y;
    uint8_t  sight;
};
std::vector<fow_call> g_fow_calls;

void rec_fow_update_fow_plus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    g_fow_calls.push_back({player, x, y, sight});
}

const map_fow_reveal_full_calls g_calls = {
    &rec_fow_update_fow_plus,
};

void reset_recorders() { g_fow_calls.clear(); }

} // namespace

void run_map_fow_reveal_full_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- outer bound zero (width == 0): the very first outer check (0x0049ab4b CMP / 0x0049ab51
    // JL) fails before the inner loop is ever entered. ZERO calls, UNCONDITIONALLY -- a naive oracle
    // that only checks "does the mock get called on the happy path" would miss a mutant that guards
    // this call behind some other condition, since this arm never calls it either way; this case pins
    // that the count is exactly 0, not merely "not asserted".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = 0;
        fx.map_height = 9;

        detail::map_fow_reveal_full(fx.view(), g_calls, 77u);

        ck_eq((uint32_t)g_fow_calls.size(), 0u,
              "T1: width==0 -- outer check x<width fails at x=0, zero calls, 0x0049ab4b/0x0049ab51");
    }

    // =================================================================================================
    // T2 -- inner bound zero (height == 0), outer bound non-trivial: the outer loop DOES run (x takes
    // 0, then 6, since both are < width=9), but the inner check (0x0049ab65 CMP / 0x0049ab6b JL)
    // fails immediately every time, so the call site is never reached for ANY x. Distinguishes the
    // inner gate from the outer one in T1 -- a mutant that only checks the outer bound would pass T1
    // but call the mock here.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = 9;
        fx.map_height = 0;

        detail::map_fow_reveal_full(fx.view(), g_calls, 77u);

        ck_eq((uint32_t)g_fow_calls.size(), 0u,
              "T2: height==0 -- inner check y<height fails every x, zero calls, 0x0049ab65/0x0049ab6b");
    }

    // =================================================================================================
    // T3 -- outer bound NEGATIVE (width == -1), pinning the SIGNED comparison (`JL`, not `JB`/`JAE`)
    // at 0x0049ab51 from the sentinel's other side: x starts at 0, and 0 < -1 is false under a signed
    // compare, so this is zero calls too. A mutation that widened `x`/`width` to unsigned (or used an
    // unsigned jump) would instead see 0 < 0xffffffff as TRUE and call the mock a huge number of
    // times -- this case is what would catch that.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = -1;
        fx.map_height = 9;

        detail::map_fow_reveal_full(fx.view(), g_calls, 77u);

        ck_eq((uint32_t)g_fow_calls.size(), 0u,
              "T3: width==-1 -- signed 0<-1 is false, zero calls (pins SIGNED compare), 0x0049ab51");
    }

    // =================================================================================================
    // T4 -- inner bound NEGATIVE (height == -1), same sentinel pin as T3 but on the inner check
    // (0x0049ab6b). Outer loop still executes normally (x takes 0, then 6, since width=9), but at
    // EVERY outer iteration the inner check 0 < -1 is false so no call ever fires.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = 9;
        fx.map_height = -1;

        detail::map_fow_reveal_full(fx.view(), g_calls, 77u);

        ck_eq((uint32_t)g_fow_calls.size(), 0u,
              "T4: height==-1 -- signed 0<-1 is false, zero calls (pins SIGNED compare), 0x0049ab6b");
    }

    // =================================================================================================
    // T5 -- EXACT MULTIPLE-OF-6 dimensions (width=12, height=6): pins the TOP boundary of both loops
    // from the "included" side. x takes {0, 6} (12<12 is false, so x=12 is EXCLUDED -- an off-by-one
    // that included it would produce a 3rd call at x=12). y takes {0} only (6<6 is false, so y=6 is
    // EXCLUDED -- an off-by-one there would produce a 2nd call at y=6). Also pins the LITERAL sight
    // constant (10, 0x0049ab75 MOV ECX,0xa) and the player pass-through (0x0049ab80/0x0049ab83) on
    // both calls.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = 12;
        fx.map_height = 6;

        detail::map_fow_reveal_full(fx.view(), g_calls, 77u);

        ck_eq((uint32_t)g_fow_calls.size(), 2u,
              "T5: width=12,height=6 -- x in {0,6} (12 excluded), y in {0} (6 excluded), 2 calls total");
        if (g_fow_calls.size() == 2) {
            ck_eq(g_fow_calls[0].x, 0u, "T5: call 0 x=0 (first outer iteration), 0x0049ab7d");
            ck_eq(g_fow_calls[0].y, 0u, "T5: call 0 y=0 (first inner iteration), 0x0049ab7a");
            ck_eq((uint32_t)g_fow_calls[0].sight, 10u, "T5: call 0 sight=10 LITERAL, 0x0049ab75 MOV ECX,0xa");
            ck_eq(g_fow_calls[0].player, 77u, "T5: call 0 player passed through unchanged, 0x0049ab80/0x0049ab83");
            ck_eq(g_fow_calls[1].x, 6u, "T5: call 1 x=6 -- top-of-width boundary INCLUDED (6<12), 0x0049ab55");
            ck_eq(g_fow_calls[1].y, 0u, "T5: call 1 y=0 -- inner restarted at 0 for the new x, 0x0049ab5b");
            ck_eq((uint32_t)g_fow_calls[1].sight, 10u, "T5: call 1 sight=10 LITERAL too, unconditional every call");
            ck_eq(g_fow_calls[1].player, 77u, "T5: call 1 player unchanged across outer iterations");
        }
    }

    // =================================================================================================
    // T6 -- NON-multiple-of-6 dimensions (width=13, height=7), exhaustive call-by-call trace pinning
    // the full iteration order (outer=x/width, inner=y/height -- from the register assignment at the
    // call site: EDX<-[EBP-0x1c] is x, EBX<-[EBP-0x18] is y), the FIRST coordinate (0,0), the LAST
    // coordinate (12,6) -- an off-by-one at EITHER end must fail this -- and the exact call count
    // (ceil(13/6)*ceil(7/6) = 3*2 = 6). width and height are chosen UNEQUAL and NON-SYMMETRIC
    // (13 != 7) specifically so a translation that swapped the outer/inner roles produces the SAME
    // total count (3*2 == 2*3) but a DIFFERENT sequence and different last coordinate (last x would
    // become 6, last y would become 12) -- the per-call trace below is what catches that, not just
    // the size() check.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = 13;
        fx.map_height = 7;

        detail::map_fow_reveal_full(fx.view(), g_calls, 200u);

        const std::vector<fow_call> want = {
            {200u, 0u, 0u, 10u},
            {200u, 0u, 6u, 10u},
            {200u, 6u, 0u, 10u},
            {200u, 6u, 6u, 10u},
            {200u, 12u, 0u, 10u},
            {200u, 12u, 6u, 10u},
        };
        ck_eq((uint32_t)g_fow_calls.size(), (uint32_t)want.size(),
              "T6: width=13,height=7 -- ceil(13/6)*ceil(7/6) = 3*2 = 6 calls total, 0x0049ab48-0x0049ab8c");
        const size_t n = g_fow_calls.size() < want.size() ? g_fow_calls.size() : want.size();
        for (size_t i = 0; i < n; ++i) {
            char msg_x[96], msg_y[96], msg_s[96], msg_p[96];
            snprintf(msg_x, sizeof(msg_x), "T6: call %zu x, outer/x-bounded-by-width, 0x0049ab7d/0x0049ab55", i);
            snprintf(msg_y, sizeof(msg_y), "T6: call %zu y, inner/y-bounded-by-height, 0x0049ab7a/0x0049ab6f", i);
            snprintf(msg_s, sizeof(msg_s), "T6: call %zu sight=10 LITERAL, unconditional, 0x0049ab75", i);
            snprintf(msg_p, sizeof(msg_p), "T6: call %zu player pass-through, 0x0049ab80/0x0049ab83", i);
            ck_eq(g_fow_calls[i].x, want[i].x, msg_x);
            ck_eq(g_fow_calls[i].y, want[i].y, msg_y);
            ck_eq((uint32_t)g_fow_calls[i].sight, (uint32_t)want[i].sight, msg_s);
            ck_eq(g_fow_calls[i].player, want[i].player, msg_p);
        }
        if (!g_fow_calls.empty()) {
            ck_eq(g_fow_calls.front().x, 0u, "T6: FIRST call x=0 -- an off-by-one at the start must fail this");
            ck_eq(g_fow_calls.front().y, 0u, "T6: FIRST call y=0 -- an off-by-one at the start must fail this");
            ck_eq(g_fow_calls.back().x, 12u,
                  "T6: LAST call x=12 (13-1, last x<13 multiple of 6) -- an off-by-one at the end must fail this");
            ck_eq(g_fow_calls.back().y, 6u,
                  "T6: LAST call y=6 (7-1, last y<7 multiple of 6) -- an off-by-one at the end must fail this");
        }
    }
}

} // namespace mh::sim::test
