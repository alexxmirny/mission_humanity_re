//
// sim_game_player_set_side_selftest.cpp -- `simtest` offline oracle for llm_game_player_set_ai
// (sim/sim_game_player_set_side.{h,cpp}, RI-SIM / SIM1F). The AI-side arm of the human/AI mirror
// pair: AND the player's bit OUT of _G_LLM_GAME_HUMAN_PLAYER_MASK (own.game_human_player_mask()),
// re-read that SAME byte and zero-extend it into is_human (own.is_human_mut(), a second independent
// write per the header's derivation), then unconditionally call llm_map_fog_of_war_recompute through
// the game_player_set_side_calls indirection.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_game_player_set_ai_0049e79c.asm):
//   0x0049e7b7-0x0049e7be: AL = 1 SHL CL (CL = player, x86 masks the count to &0x1f internally);
//   0x0049e7be:            AL = NOT AL;
//   0x0049e7c0:             mask &= AL                    (clears exactly bit `player & 0x1f`)
//   0x0049e7c6-0x0049e7cd: is_human = zero-extend(mask)   (re-read the SAME byte, unconditional)
//   0x0049e7d2:             call llm_map_fog_of_war_recompute (unconditional, no gate anywhere)
// There is no branch in this function -- every case below is the same straight-line path with
// different bit positions / starting masks, which is what actually exercises "correct bit index".
//
#include "sim/sim_game_player_set_side.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

int32_t g_fog_calls = 0;
void    stub_fog_recompute() { ++g_fog_calls; }

const game_player_set_side_calls g_calls = {stub_fog_recompute};

void call_set_ai(sim_fixture &fx, uint8_t player) {
    sim_store own = fx.store();
    detail::game_player_set_ai(own, g_calls, player);
}

} // namespace

void run_player_set_ai_tests() {
    sim_fixture fx;

    // ---- A1: default (all-human, 0xff) mask -- clearing player 3's bit ----------------------------
    fx.reset();
    fx.game_human_player_mask = 0xff;
    fx.is_human               = 0xdeadbeef; // garbage, so the mirror-write must overwrite it fully
    g_fog_calls               = 0;
    call_set_ai(fx, /*player*/ 3);
    ck_eq((uint32_t)fx.game_human_player_mask, 0xf7u, "A1: mask 0xff with bit 3 cleared -> 0xf7");
    ck_eq((uint32_t)fx.is_human, 0xf7u, "A1: is_human overwritten with the zero-extended mask (0xf7)");
    ck_eq((uint32_t)g_fog_calls, 1u, "A1: fog_of_war_recompute fires exactly once");

    // ---- A2: bit-index boundary LOW -- player 0 clears the low bit, nothing else ------------------
    fx.reset();
    fx.game_human_player_mask = 0xff;
    g_fog_calls               = 0;
    call_set_ai(fx, /*player*/ 0);
    ck_eq((uint32_t)fx.game_human_player_mask, 0xfeu, "A2: mask 0xff with bit 0 cleared -> 0xfe");
    ck_eq((uint32_t)fx.is_human, 0xfeu, "A2: is_human mirrors 0xfe");

    // ---- A3: bit-index boundary HIGH -- player 7 clears the top bit, nothing else ------------------
    fx.reset();
    fx.game_human_player_mask = 0xff;
    g_fog_calls               = 0;
    call_set_ai(fx, /*player*/ 7);
    ck_eq((uint32_t)fx.game_human_player_mask, 0x7fu, "A3: mask 0xff with bit 7 cleared -> 0x7f");
    ck_eq((uint32_t)fx.is_human, 0x7fu, "A3: is_human mirrors 0x7f");

    // ---- A4: a MIXED starting mask -- proves the exact bit position, not just "something changed",
    // and that every OTHER bit survives untouched. mask=0xb5 (1011_0101), clear bit 2 (value 0x04):
    // 0xb5 & ~0x04 = 0xb1 (1011_0001) -- bits 0,4,5,7 stay set, bit 2 (and only bit 2) drops. -------
    fx.reset();
    fx.game_human_player_mask = 0xb5;
    g_fog_calls               = 0;
    call_set_ai(fx, /*player*/ 2);
    ck_eq((uint32_t)fx.game_human_player_mask, 0xb1u,
          "A4: mixed mask 0xb5 with ONLY bit 2 cleared -> 0xb1 (a wrong bit index changes this value)");
    ck_eq((uint32_t)fx.is_human, 0xb1u, "A4: is_human mirrors the same 0xb1");

    // ---- A5: idempotent -- the bit is ALREADY clear (player already AI). Mask must stay unchanged,
    // but the mirror-write and the fog call are UNCONDITIONAL and still both happen. -----------------
    fx.reset();
    fx.game_human_player_mask = 0xf7; // bit 3 already 0
    fx.is_human               = 0;    // stale/zero, must still be overwritten to 0xf7
    g_fog_calls               = 0;
    call_set_ai(fx, /*player*/ 3);
    ck_eq((uint32_t)fx.game_human_player_mask, 0xf7u, "A5: mask unchanged when the bit was already 0");
    ck_eq((uint32_t)fx.is_human, 0xf7u, "A5: is_human still (re)written to 0xf7, not left at 0");
    ck_eq((uint32_t)g_fog_calls, 1u, "A5: fog_of_war_recompute fires even when the mask didn't change");

    // ---- A6: two calls in a row both fire the fog callback (not batched/deduped), and the two bit
    // clears compose: 0xff -> (clear bit1, 0x02) -> 0xfd -> (clear bit5, 0x20) -> 0xdd. ---------------
    fx.reset();
    fx.game_human_player_mask = 0xff;
    g_fog_calls               = 0;
    call_set_ai(fx, /*player*/ 1);
    call_set_ai(fx, /*player*/ 5);
    ck_eq((uint32_t)fx.game_human_player_mask, 0xddu,
          "A6: two sequential clears (bit1 then bit5) compose: 0xff -> 0xfd -> 0xdd");
    ck_eq((uint32_t)g_fog_calls, 2u, "A6: fog_of_war_recompute fires once per call (2 calls -> 2)");
}

} // namespace mh::sim::test

// ---- mutation notes (spot-verifiable by breaking the translation) --------------------------------
// - A1/A5's is_human check: replacing `own.is_human_mut() = (uint32_t)own.game_human_player_mask()`
//   with an `|=` (a cached-copy bug) fails A1 (garbage 0xdeadbeef would survive as extra high bits)
//   and fails A5's zero-seeded case identically.
// - A2/A3/A4: using `1u << player` without narrowing back to uint8_t before complementing, or
//   computing the bit from the wrong operand (e.g. `~(1u << player)` at 32-bit width instead of
//   narrowing THEN complementing), changes which byte-bits survive -- A4's 0xb5->0xb1 is the
//   sharpest catch since it has bits set on both sides of bit 2.
// - A5/A6: dropping the `c.fog_of_war_recompute()` call (or gating it on "mask changed") fails A5's
//   and A6's call-count checks while every mask/is_human check still passes -- this is the ONLY check
//   in the file that would catch that particular bug.
