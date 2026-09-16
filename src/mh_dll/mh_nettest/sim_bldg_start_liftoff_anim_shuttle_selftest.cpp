//
// sim_bldg_start_liftoff_anim_shuttle_selftest.cpp -- `simtest` oracle for
// llm_strat_bldg_start_liftoff_anim_shuttle @0x00475ce3 (sim/sim_bldg_liftoff_anim.h/.cpp,
// RI-SIM / SIM1-G4 batch).
//
// NO `_calls` struct: the header's own "CALLEES: NONE" note is confirmed by the .asm -- the only
// CALL in the whole body is the inert `utils_assert_stack_capacity` prologue check (0x00475ceb,
// rule 6, omitted). Every effect is a direct write into `own.building_at(player, index)`, so this
// oracle seeds/reads `fx.b(player, index)` and `fx.cfg_buildings[bid]` directly -- no recorder mocks.
//
// EXPECTED BEHAVIOUR from the header's derivation (sim_bldg_liftoff_anim.h):
//   param_1's LOW 16 BITS are the player (MOVZX word ptr masks it before every one of the 9 address
//     computations, e.g. 0x00475d00/0x00475d1d/0x00475d3a/0x00475d57/0x00475d77/0x00475d96/
//     0x00475dbb/0x00475e05/0x00475e2a) -- garbage in the upper 16 bits must not affect which
//     building record gets written.
//   param_2 is the FULL 32-bit index, never masked (e.g. 0x00475d0a's dword read).
//   param_3(EBX)/param_4(ECX) are DEAD -- no read anywhere past the callee-save PUSH/POP.
//   anim[0]=0 (0x00475d13), anim[1]=0 (0x00475d30), anim[2]=0 (0x00475d4d) -- UNCONDITIONAL, in that
//     order, each a raw 4-byte store into the flattened uint8_t[48] field.
//   anim[3] = cfg_buildings[building_id].anim[10] (0x00475d90) -- the cfg TYPE table's frame-index
//     sub-index 10 (byte offset 40 into the flattened array), NOT sub-index 5 (that is the MOTHER
//     sibling's slot, sim_bldg_liftoff_anim.h's own contrast).
//   anim_dur[0..3] ALL FOUR = combine_dword_pair(param_5=lo, param_6=hi) -- ONE value stamped four
//     times (header's CORRECTION note: this is anim_dur, not pip_frame/pip_timer; the batch-context
//     doc's 0xd3/0xd7/0xdb/... reading was the raw VA's own low bytes, not struct-relative offsets):
//       anim_dur[0] lo/hi @ 0x00475dac/0x00475db5
//       anim_dur[1] lo/hi @ 0x00475dd1/0x00475dda
//       anim_dur[2] lo/hi @ 0x00475df6/0x00475dff
//       anim_dur[3] lo/hi @ 0x00475e1b/0x00475e24
//     The reconstruction must depend on the ARGUMENTS (param_5/param_6), NOT a second independent
//     read of the ambient GAME_CLOCK global -- the function only ever sees its own two dword params.
//   online_state = 0xb UNCONDITIONALLY (0x00475e3d), regardless of every other branch above (there
//     are none -- this function is straight-line code, no conditionals at all).
//
#include "sim/sim_bldg_liftoff_anim.h"

#include <cstdint>
#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Mirrors sim_bldg_liftoff_anim.cpp's own SHUTTLE_LIFTOFF_ANIM_CFG_SLOT (10) -- kept as the test's own
// literal (not #include-shared) so a regression in the production constant shows up as a test failure
// instead of both sides silently drifting together.
constexpr int32_t SHUTTLE_CFG_SLOT = 10;

// Raw little-endian 4-byte accessors over the flattened uint8_t[48] anim arrays -- same pattern the
// production TU's own store_u32_le/load_u32_le establish, duplicated here so this test file has no
// dependency on the anonymous-namespace helpers in sim_bldg_liftoff_anim.cpp (which are not visible
// across translation units by design).
void set_anim_slot_u32(uint8_t *arr, int slot, uint32_t value) {
    uint8_t *p = arr + slot * 4;
    p[0]       = (uint8_t)(value);
    p[1]       = (uint8_t)(value >> 8);
    p[2]       = (uint8_t)(value >> 16);
    p[3]       = (uint8_t)(value >> 24);
}
uint32_t anim_slot_u32(const uint8_t *arr, int slot) {
    const uint8_t *p = arr + slot * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Splits a double into its raw low/high dwords -- the SAME bit-for-bit split the caller
// (llm_strat_bldg_state_deploy_start) performs before this function's param_5/param_6, and the
// INVERSE of the production TU's own combine_dword_pair(). memcpy, not a union/reinterpret_cast.
void split_double_for_test(double value, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    lo = (uint32_t)(bits & 0xffffffffu);
    hi = (uint32_t)(bits >> 32);
}

} // namespace

void run_bldg_start_liftoff_anim_shuttle_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the full write set, seeded so every distinct mechanism the header derives is independently
    // pinned: param_1's player-masking, the three unconditional zero-writes, the cfg-slot-10 re-seed
    // (against a cfg.anim table where EVERY slot holds a distinct value, so a wrong sub-index reads a
    // neighbor's value instead of accidentally matching), the four-way anim_dur stamp reconstructed
    // from the ARGUMENTS (not the ambient game_clock global, which is seeded to a different value),
    // online_state, and two neighbor slots (anim[4], anim_dur[4]) proving the writes don't overrun.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 3;
        constexpr int32_t  INDEX       = 12;
        constexpr uint16_t BUILDING_ID = 7;

        building &b   = fx.b(PLAYER, INDEX);
        b.building_id = BUILDING_ID;

        // anim[0..3] seeded with distinct nonzero garbage -- proves the first three become EXACTLY 0
        // (not "already zero", not partially overwritten) and that anim[3] gets OVERWRITTEN by the cfg
        // read rather than left at its seeded value.
        set_anim_slot_u32(b.anim, 0, 0xAAAAAAAAu);
        set_anim_slot_u32(b.anim, 1, 0xBBBBBBBBu);
        set_anim_slot_u32(b.anim, 2, 0xCCCCCCCCu);
        set_anim_slot_u32(b.anim, 3, 0xDDDDDDDDu);
        // Neighbor slot -- must survive untouched, pinning that the three zero-writes + the slot-3
        // re-seed don't overrun into slot 4.
        set_anim_slot_u32(b.anim, 4, 0xE5E5E5E5u);

        // cfg.anim: every one of the 12 frame-index slots gets ITS OWN distinct value, so reading the
        // wrong sub-index (e.g. the MOTHER sibling's slot 5, or an off-by-one at 9/11) lands on a
        // recognizably wrong number instead of coincidentally matching.
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot) set_anim_slot_u32(cfg.anim, slot, 0x10000000u + (uint32_t)slot);
        set_anim_slot_u32(cfg.anim, SHUTTLE_CFG_SLOT, 0x600DF00Du); // the slot this function actually reads

        // anim_dur[0..3] seeded with distinct garbage (not the expected stamp, not each other) so "all
        // four become the SAME reconstructed value" is a real observation, not a coincidence; anim_dur[4]
        // is the neighbor sentinel proving the four-write loop is bounded to [0..3].
        for (int i = 0; i < 4; ++i) b.anim_dur[i] = 1000.0 + i;
        b.anim_dur[4] = -777.25;

        // online_state seeded with a sentinel distinct from both 0 and the expected 0xb.
        b.online_state = 0x1234;

        // The ambient GAME_CLOCK global is seeded to a value DIFFERENT from the (param_5,param_6) pair
        // below -- a translation that read *v.game_clock directly instead of reconstructing from its
        // own two dword arguments would stamp anim_dur[] with 999.0, not the expected 12345.6789.
        fx.game_clock = 999.0;

        const double stamp = 12345.6789;
        uint32_t     lo, hi;
        split_double_for_test(stamp, lo, hi);

        // param_1: PLAYER in the low 16 bits, garbage (0xBEEF) in the high 16 bits -- pins the MOVZX
        // mask applied before every address computation (0x00475d00 and its eight siblings). If the
        // mask were dropped, building_at() would resolve a completely different (and, for this
        // garbage value, out-of-bounds) address, and every check below on `b` would see its untouched
        // seed value instead of the expected result.
        const uint32_t param_1 = (0xBEEFu << 16) | PLAYER;
        // param_2: the FULL 32-bit index, read as a dword with no MOVZX/mask (0x00475d0a etc.).
        const int32_t param_2 = INDEX;
        // param_3/param_4 (EBX/ECX): DEAD -- sentinel values with no plausible legitimate meaning if
        // they were accidentally read as an address, index, or count.
        const uint32_t param_3 = 0xCAFEBABEu;
        const uint32_t param_4 = 0xFEEDFACEu;

        sim_store own = fx.store();
        detail::bldg_start_liftoff_anim_shuttle(fx.view(), own, param_1, param_2, param_3, param_4, lo, hi);

        ck_eq(anim_slot_u32(b.anim, 0), 0u, "T1: anim[0] = 0 unconditionally, 0x00475d13");
        ck_eq(anim_slot_u32(b.anim, 1), 0u, "T1: anim[1] = 0 unconditionally, 0x00475d30");
        ck_eq(anim_slot_u32(b.anim, 2), 0u, "T1: anim[2] = 0 unconditionally, 0x00475d4d");
        ck_eq(anim_slot_u32(b.anim, 3), 0x600DF00Du,
              "T1: anim[3] = cfg_buildings[building_id].anim[10] (SHUTTLE slot, not MOTHER's slot 5), 0x00475d90");
        ck_eq(anim_slot_u32(b.anim, 4), 0xE5E5E5E5u, "T1: anim[4] (neighbor slot) untouched -- writes don't overrun");

        ck_eq_d(b.anim_dur[0], stamp,
                "T1: anim_dur[0] = GAME_CLOCK reconstructed from (param_5,param_6), not the ambient global, "
                "0x00475dac/0x00475db5");
        ck_eq_d(b.anim_dur[1], stamp, "T1: anim_dur[1] = the SAME stamp (second of four writes), 0x00475dd1/0x00475dda");
        ck_eq_d(b.anim_dur[2], stamp, "T1: anim_dur[2] = the SAME stamp (third of four writes), 0x00475df6/0x00475dff");
        ck_eq_d(b.anim_dur[3], stamp, "T1: anim_dur[3] = the SAME stamp (fourth of four writes), 0x00475e1b/0x00475e24");
        ck_eq_d(b.anim_dur[4], -777.25, "T1: anim_dur[4] (neighbor slot) untouched -- the stamp loop is bounded to [0..3]");

        ck_eq((uint32_t)b.online_state, 0xbu, "T1: online_state = 0xb unconditionally, 0x00475e3d");
    }

    // =================================================================================================
    // T2 -- param_3(EBX)/param_4(ECX) are DEAD: run the identical call twice on two separately-seeded
    // (but identically-seeded) building records, differing ONLY in param_3/param_4's value, and confirm
    // the two resulting records are byte-identical. If either register were secretly read anywhere past
    // the callee-save PUSH/POP, this would diverge.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 1;
        constexpr int32_t  INDEX_A     = 20;
        constexpr int32_t  INDEX_B     = 21;
        constexpr uint16_t BUILDING_ID = 4;

        auto seed = [](building &b) {
            b.building_id = BUILDING_ID;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x77777700u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 5.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba);
        seed(bb);

        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, SHUTTLE_CFG_SLOT, 0x0BADF00Du);

        const double stamp = 42.5;
        uint32_t     lo, hi;
        split_double_for_test(stamp, lo, hi);

        sim_store own = fx.store();
        // param_3/param_4 = (0, 0) on the first call, (0xFFFFFFFF, 0xFFFFFFFF) on the second -- the
        // widest possible divergence for a "no observable effect" claim.
        detail::bldg_start_liftoff_anim_shuttle(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, lo, hi);
        detail::bldg_start_liftoff_anim_shuttle(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu, lo, hi);

        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2: param_3/param_4 dead -- anim[] identical regardless of their value (no read past the PUSH/POP pair)");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2: param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)ba.online_state, (uint32_t)bb.online_state,
              "T2: param_3/param_4 dead -- online_state identical regardless of their value");
    }
}

} // namespace mh::sim::test
