//
// sim_bldg_start_special_anim_selftest.cpp -- OFFLINE `simtest` oracle for
// llm_strat_bldg_start_special_anim @0x00475b75 (sim/sim_bldg_anim_trigger.h/.cpp, RI-SIM SIM1-G4
// batch).
//
// WHY OFFLINE, NOT SHADOW-VERIFIED: the site (install_shadow_bldg_anim_trigger) is armed, but a
// completed rig run logged it at 0 calls -- the running scenario never triggered a "special anim"
// on any building, so the rig produced no evidence (0 calls is untested, not passed). This oracle
// is this function's only evidence until a scenario actually exercises the call site.
//
// NO `_calls` struct: the header's own "CALLEES: NONE" note is confirmed by the .asm -- the only
// CALL in the whole body is the inert `utils_assert_stack_capacity` prologue check (0x00475b7d,
// rule 6, omitted). Every effect is a direct write into `own.building_at(player, b_index)`, so this
// oracle seeds/reads `fx.b(player, index)` and `fx.cfg_buildings[bid]` directly -- no recorder mocks.
//
// EXPECTED BEHAVIOUR from the .asm (tmp/decomp_sim/llm_strat_bldg_start_special_anim_00475b75.asm)
// and the header's derivation (sim_bldg_anim_trigger.h):
//   `player` is ALREADY a committed 16-bit parameter (storage=AX:2) -- the asm's repeated
//     `MOVZX EAX/EDX, word ptr [EBP + -0x14]` is just re-loading the spilled 16-bit value, NOT
//     masking a wider register's garbage upper bits (unlike the liftoff-anim siblings' param_1).
//     So there is no "garbage-high-word" case here -- the type itself is the guarantee.
//   `b_index` (EDX:4) is the FULL 32-bit index, never masked.
//   `unused_ebx`(EBX)/`unused_ecx`(ECX) are DEAD -- no read anywhere past the callee-save PUSH/POP
//     at 0x00475b82-0x00475b85 / 0x00475cdb-0x00475cde.
//   anim[0]=0 (write @0x00475ba5), anim[1]=0 (write @0x00475bc2), anim[2]=0 (write @0x00475bdf) --
//     UNCONDITIONAL, each a raw 4-byte store into the flattened uint8_t[48] field.
//   anim[3] = cfg_buildings[building_id].anim[2] (building_id read @0x00475bfc, cfg read
//     @0x00475c1c, write @0x00475c22) -- the cfg TYPE table's frame-index sub-index 2 (byte offset
//     8 into the flattened array).
//   anim_dur[0..3] ALL FOUR = `timestamp`, the SAME value stamped four times -- ALREADY a committed
//     `double` stack parameter (unlike the liftoff-anim siblings' dword-pair reconstruction), so the
//     asm just re-reads [EBP+0x8]/[EBP+0xc] (the double's own two halves) four times, not a
//     bit-reconstruction from two logically distinct dwords:
//       anim_dur[0] low/high write @0x00475c3e/0x00475c47
//       anim_dur[1] low/high write @0x00475c63/0x00475c6c
//       anim_dur[2] low/high write @0x00475c88/0x00475c91
//       anim_dur[3] low/high write @0x00475cad/0x00475cb6
//   online_state = 0xa UNCONDITIONALLY (write @0x00475ccf), regardless of every other input (there
//     are no conditionals at all -- this function is straight-line code).
//
#include "sim/sim_bldg_anim_trigger.h"

#include <cstdint>
#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Mirrors sim_bldg_anim_trigger.cpp's own SPECIAL_ANIM_CFG_SLOT (2) -- kept as the test's own
// literal (not #include-shared) so a regression in the production constant shows up as a test
// failure instead of both sides silently drifting together.
constexpr int32_t SPECIAL_CFG_SLOT = 2;

// Raw little-endian 4-byte accessors over the flattened uint8_t[48] anim arrays -- an INDEPENDENT
// copy of the production TU's own store_u32_le/load_u32_le (sim_bldg_anim_trigger.cpp's anonymous
// namespace); an oracle must not reach into the thing it is checking, and this project's own
// precedent for this exact pair of flattened fields (mh_map_object_building::anim[48] /
// mh_cfg_final_struct_Building::anim[48], both cfg_t_frame_index[12]) is to duplicate this helper
// per-TU rather than share it.
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

} // namespace

void run_bldg_start_special_anim_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the full write set, seeded so every distinct mechanism the header/.asm derive is
    // independently pinned: the three unconditional zero-writes, the cfg-slot-2 re-seed (against a
    // cfg.anim table where EVERY slot holds a distinct value, so a wrong sub-index reads a
    // neighbor's value instead of accidentally matching), the four-way anim_dur stamp from the
    // ARGUMENT `timestamp` (not the ambient game_clock global, seeded to a different value),
    // online_state, neighbor slots (anim[4], anim_dur[4]) proving the writes don't overrun, and
    // sibling building rows (a different player, a different index) proving the row-address
    // arithmetic lands on exactly (PLAYER, B_INDEX).
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER        = 5;
        constexpr int32_t  B_INDEX       = 9;
        constexpr uint16_t OTHER_PLAYER  = 2;
        constexpr int32_t  SIBLING_INDEX = 11;
        constexpr uint16_t BUILDING_ID   = 13;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = BUILDING_ID;

        // anim[0..3] seeded with distinct nonzero garbage -- proves the first three become EXACTLY 0
        // (not "already zero", not partially overwritten) and that anim[3] gets OVERWRITTEN by the
        // cfg read rather than left at its seeded value.
        set_anim_slot_u32(b.anim, 0, 0xAAAA0000u);
        set_anim_slot_u32(b.anim, 1, 0xBBBB0001u);
        set_anim_slot_u32(b.anim, 2, 0xCCCC0002u);
        set_anim_slot_u32(b.anim, 3, 0xDDDD0003u);
        // Neighbor slot -- must survive untouched, pinning that the three zero-writes + the slot-3
        // re-seed don't overrun into slot 4.
        set_anim_slot_u32(b.anim, 4, 0xE5E5E5E5u);

        // cfg.anim: every one of the 12 frame-index slots gets ITS OWN distinct value, so reading the
        // wrong sub-index lands on a recognizably wrong number instead of coincidentally matching.
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x30000000u + (uint32_t)slot);
        set_anim_slot_u32(cfg.anim, SPECIAL_CFG_SLOT, 0x5EED0002u); // the slot this function reads

        // anim_dur[0..3] seeded with distinct garbage (not the expected stamp, not each other) so
        // "all four become the SAME argument value" is a real observation, not a coincidence;
        // anim_dur[4] is the neighbor sentinel proving the four-write span is bounded to [0..3].
        b.anim_dur[0] = 111.0;
        b.anim_dur[1] = 222.0;
        b.anim_dur[2] = 333.0;
        b.anim_dur[3] = 444.0;
        b.anim_dur[4] = -999.5;

        // online_state seeded with a sentinel distinct from both 0 and the expected 0xa.
        b.online_state = 0x7777;

        // The ambient GAME_CLOCK global is seeded to a value DIFFERENT from `timestamp` below -- a
        // translation that read *v.game_clock directly instead of using its own `timestamp` argument
        // would stamp anim_dur[] with 55.5, not the expected 424242.125.
        fx.game_clock = 55.5;

        // Sibling rows the write must NOT touch -- proves building_at(player, b_index) resolution
        // lands exactly on (PLAYER, B_INDEX), not a neighbour player or a neighbour index.
        building &other_player_row    = fx.b(OTHER_PLAYER, B_INDEX);
        other_player_row.online_state = 0x1111;
        set_anim_slot_u32(other_player_row.anim, 0, 0xC0FFEE00u);

        building &sibling_index_row    = fx.b(PLAYER, SIBLING_INDEX);
        sibling_index_row.online_state = 0x2222;
        set_anim_slot_u32(sibling_index_row.anim, 0, 0xFACEB00Cu);

        const double timestamp = 424242.125; // exactly representable, distinct from game_clock decoy
        // unused_ebx/unused_ecx (EBX/ECX): DEAD -- sentinel values with no plausible legitimate
        // meaning if they were accidentally read as an address, index, or count.
        const uint32_t unused_ebx = 0xCAFEBABEu;
        const uint32_t unused_ecx = 0xFEEDFACEu;

        sim_store own = fx.store();
        detail::bldg_start_special_anim(fx.view(), own, PLAYER, B_INDEX, unused_ebx, unused_ecx,
                                        timestamp);

        ck_eq(anim_slot_u32(b.anim, 0), 0u, "T1: anim[0] = 0 unconditionally, 0x00475ba5");
        ck_eq(anim_slot_u32(b.anim, 1), 0u, "T1: anim[1] = 0 unconditionally, 0x00475bc2");
        ck_eq(anim_slot_u32(b.anim, 2), 0u, "T1: anim[2] = 0 unconditionally, 0x00475bdf");
        ck_eq(anim_slot_u32(b.anim, 3), 0x5EED0002u,
              "T1: anim[3] = cfg_buildings[building_id].anim[2], 0x00475bfc (building_id read)/"
              "0x00475c1c (cfg read)/0x00475c22 (write)");
        ck_eq(anim_slot_u32(b.anim, 4), 0xE5E5E5E5u,
              "T1: anim[4] (neighbor slot) untouched -- writes don't overrun past slot 3");

        ck_eq_d(b.anim_dur[0], timestamp,
                "T1: anim_dur[0] = the `timestamp` ARGUMENT, not *v.game_clock, "
                "0x00475c3e/0x00475c47");
        ck_eq_d(b.anim_dur[1], timestamp, "T1: anim_dur[1] = the SAME stamp, 0x00475c63/0x00475c6c");
        ck_eq_d(b.anim_dur[2], timestamp, "T1: anim_dur[2] = the SAME stamp, 0x00475c88/0x00475c91");
        ck_eq_d(b.anim_dur[3], timestamp, "T1: anim_dur[3] = the SAME stamp, 0x00475cad/0x00475cb6");
        ck(b.anim_dur[0] != 55.5,
           "T1: anim_dur[0] must NOT be the ambient game_clock decoy -- the function only ever sees "
           "its own `timestamp` argument");
        ck_eq_d(b.anim_dur[4], -999.5,
                "T1: anim_dur[4] (neighbor slot) untouched -- the stamp span is bounded to [0..3]");

        ck_eq((uint32_t)(uint16_t)b.online_state, 0xau, "T1: online_state = 0xa unconditionally, 0x00475ccf");

        ck_eq((uint32_t)(uint16_t)other_player_row.online_state, 0x1111u,
              "T1: a DIFFERENT player's row at the SAME index is untouched -- building_at resolved "
              "the right player");
        ck_eq(anim_slot_u32(other_player_row.anim, 0), 0xC0FFEE00u,
              "T1: same row -- the other player's anim[0] is untouched");
        ck_eq((uint32_t)(uint16_t)sibling_index_row.online_state, 0x2222u,
              "T1: a DIFFERENT index for the SAME player is untouched -- b_index resolved the right "
              "row");
        ck_eq(anim_slot_u32(sibling_index_row.anim, 0), 0xFACEB00Cu,
              "T1: same row -- the sibling index's anim[0] is untouched");
    }

    // =================================================================================================
    // T2 -- unused_ebx(EBX)/unused_ecx(ECX) are DEAD: run the identical call twice on two separately-
    // seeded (but identically-seeded) building records, differing ONLY in unused_ebx/unused_ecx's
    // value, and confirm the two resulting records are byte-identical. If either register were
    // secretly read anywhere past the callee-save PUSH/POP, this would diverge.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 1;
        constexpr int32_t  INDEX_A     = 20;
        constexpr int32_t  INDEX_B     = 21;
        constexpr uint16_t BUILDING_ID = 4;

        auto seed = [](building &b) {
            b.building_id = BUILDING_ID;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x77770000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 5.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba);
        seed(bb);

        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, SPECIAL_CFG_SLOT, 0x0BADF00Du);

        const double timestamp = 42.5;

        sim_store own = fx.store();
        // unused_ebx/unused_ecx = (0, 0) on the first call, (0xFFFFFFFF, 0xFFFFFFFF) on the second --
        // the widest possible divergence for a "no observable effect" claim.
        detail::bldg_start_special_anim(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, timestamp);
        detail::bldg_start_special_anim(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                        timestamp);

        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2: unused_ebx/unused_ecx dead -- anim[] identical regardless of their value (no read "
           "past the PUSH/POP pair, 0x00475b82-0x00475b85/0x00475cdb-0x00475cde)");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2: unused_ebx/unused_ecx dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2: unused_ebx/unused_ecx dead -- online_state identical regardless of their value");
    }
}

} // namespace mh::sim::test
