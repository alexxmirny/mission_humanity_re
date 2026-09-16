#include "sim/sim_bldg_liftoff_anim.h"

#include <cstdint>
#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- local byte-level helpers, an INDEPENDENT copy of the production TU's own store_u32_le/
// load_u32_le (sim_bldg_liftoff_anim.cpp's anonymous namespace) -- an oracle must not reach into the
// thing it is checking, and this project's own precedent for this exact pair of flattened fields
// (mh_map_object_building::anim[48] / mh_cfg_final_struct_Building::anim[48], both cfg_t_frame_index[12])
// is to duplicate this helper per-TU rather than share it.
void store_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value);
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}
uint32_t load_u32_le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

// Splits a double into its raw low/high dwords -- the SAME direction the real caller
// (llm_strat_bldg_state_deploy_start) splits GAME_CLOCK before this call (see the header's
// derivation). Lets a case pick a RECOGNIZABLE double and feed it in as (param_5, param_6).
void split_double_le(double d, uint32_t &lo, uint32_t &hi) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    lo = (uint32_t)(bits & 0xffffffffu);
    hi = (uint32_t)(bits >> 32);
}

} // namespace

void run_bldg_start_liftoff_anim_mother_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- happy path, all mechanisms pinned in one row: player resolved from param_1's LOW 16 BITS
    // (garbage upper bits), anim[0] pinned to cfg slot 5 (NOT the shuttle sibling's slot 10 -- both
    // seeded with DIFFERENT values so a copy-pasted shuttle read is caught), anim_dur[0] reconstructed
    // bit-for-bit from (param_5,param_6) and NOT from *v.game_clock (seeded to a different value),
    // online_state unconditionally overwritten to 0xb, and -- the shape difference from the shuttle
    // sibling -- anim[1..3]/anim_dur[1..3] and two sibling building rows are all left untouched (no
    // loop, no stray write).
    // =================================================================================================
    {
        fx.reset();

        constexpr uint16_t PLAYER       = 2;
        constexpr uint16_t OTHER_PLAYER = 5;
        constexpr int32_t  INDEX        = 6;
        constexpr int32_t  SIBLING_IDX  = 7;
        constexpr uint16_t BID          = 42;

        constexpr uint32_t ANIM_SLOT5_VAL  = 0x11115555u; // MOTHER's own slot (cfg byte offset 0x14)
        constexpr uint32_t ANIM_SLOT10_VAL = 0x99998888u; // SHUTTLE's slot (cfg byte offset 0x28) --
                                                          // DIFFERENT value: catches a copy-pasted
                                                          // shuttle-index read
        constexpr uint32_t ANIM0_SEED = 0xDEAD0000u;      // target's own slot 0 -- must be overwritten
        constexpr uint32_t ANIM1_SEED = 0xAAAA1111u;      // must SURVIVE -- mother has no loop
        constexpr uint32_t ANIM2_SEED = 0xAAAA2222u;      // must SURVIVE -- mother has no loop
        constexpr uint32_t ANIM3_SEED = 0xAAAA3333u;      // the SHUTTLE sibling's own write target --
                                                          // must SURVIVE here

        const double GAME_CLOCK_DECOY = 13.0;     // must NOT end up in anim_dur[0]
        const double STAMP_DOUBLE     = 424242.5; // the recognizable value carried by (param_5,param_6)
        const double ANIM_DUR0_SEED   = 777.75;   // sentinel, must be overwritten
        const double ANIM_DUR1_SEED   = 111.25;   // must SURVIVE -- no 4-way stamp here
        const double ANIM_DUR2_SEED   = 222.25;   // must SURVIVE
        const double ANIM_DUR3_SEED   = 333.25;   // must SURVIVE

        fx.game_clock = GAME_CLOCK_DECOY;

        store_u32_le(&fx.cfg_buildings[BID].anim[5 * 4], ANIM_SLOT5_VAL);
        store_u32_le(&fx.cfg_buildings[BID].anim[10 * 4], ANIM_SLOT10_VAL);

        building &b    = fx.b(PLAYER, INDEX);
        b.building_id  = BID;
        b.online_state = 0x1234; // sentinel, must become 0xb
        store_u32_le(&b.anim[0 * 4], ANIM0_SEED);
        store_u32_le(&b.anim[1 * 4], ANIM1_SEED);
        store_u32_le(&b.anim[2 * 4], ANIM2_SEED);
        store_u32_le(&b.anim[3 * 4], ANIM3_SEED);
        b.anim_dur[0] = ANIM_DUR0_SEED;
        b.anim_dur[1] = ANIM_DUR1_SEED;
        b.anim_dur[2] = ANIM_DUR2_SEED;
        b.anim_dur[3] = ANIM_DUR3_SEED;

        // Sibling rows the write must NOT touch -- proves the building_at(player, index) resolution
        // lands exactly on (PLAYER, INDEX), not a neighbour player or a neighbour index.
        building &other_player_row    = fx.b(OTHER_PLAYER, INDEX);
        other_player_row.online_state = 0x4321;
        store_u32_le(&other_player_row.anim[0 * 4], 0xC0FFEE00u);

        building &sibling_index_row    = fx.b(PLAYER, SIBLING_IDX);
        sibling_index_row.online_state = 0x5678;
        store_u32_le(&sibling_index_row.anim[0 * 4], 0xFACEB00Cu);

        uint32_t lo, hi;
        split_double_le(STAMP_DOUBLE, lo, hi);

        // param_1: PLAYER in the low 16 bits, garbage (0xBEEF) in the upper 16 -- must still resolve to
        // PLAYER, not a masked/garbage row (0x00475f16: MOVZX EDX, word ptr [EBP + -0x14]).
        const uint32_t param_1 = 0xBEEF0000u | PLAYER;
        const int32_t  param_2 = INDEX;
        const uint32_t param_3 = 0x11111111u; // EBX -- DEAD, no read anywhere in the body
        const uint32_t param_4 = 0x22222222u; // ECX -- DEAD, no read anywhere in the body

        sim_store own = fx.store();
        detail::bldg_start_liftoff_anim_mother(fx.view(), own, param_1, param_2, param_3, param_4, lo,
                                               hi);

        ck_eq(load_u32_le(&b.anim[0 * 4]), ANIM_SLOT5_VAL,
              "T1: anim[0] <- cfg_buildings[bid].anim[5] (cfg byte offset 0x14), 0x00475f29-0x00475f4f");
        ck(load_u32_le(&b.anim[0 * 4]) != ANIM_SLOT10_VAL,
           "T1: anim[0] must NOT be the shuttle sibling's slot-10 value (copy-paste guard)");

        ck_eq_d(b.anim_dur[0], STAMP_DOUBLE,
                "T1: anim_dur[0] <- memcpy(param_5=lo, param_6=hi), bit-for-bit, 0x00475f68-0x00475f74");
        ck(b.anim_dur[0] != GAME_CLOCK_DECOY,
           "T1: anim_dur[0] must NOT be *v.game_clock -- header's correction of the wrong "
           "pip_frame/pip_timer/ambient-global-read hazard");

        ck_eq(load_u32_le(&b.anim[1 * 4]), ANIM1_SEED,
              "T1: anim[1] left untouched -- mother has NO loop (unlike the shuttle sibling)");
        ck_eq(load_u32_le(&b.anim[2 * 4]), ANIM2_SEED,
              "T1: anim[2] left untouched -- mother has NO loop");
        ck_eq(load_u32_le(&b.anim[3 * 4]), ANIM3_SEED,
              "T1: anim[3] (the SHUTTLE sibling's own write target) left untouched here");

        ck_eq_d(b.anim_dur[1], ANIM_DUR1_SEED,
                "T1: anim_dur[1] left untouched -- no 4-way anim_dur stamp here (unlike shuttle)");
        ck_eq_d(b.anim_dur[2], ANIM_DUR2_SEED, "T1: anim_dur[2] left untouched");
        ck_eq_d(b.anim_dur[3], ANIM_DUR3_SEED, "T1: anim_dur[3] left untouched");

        ck_eq((uint32_t)(uint16_t)b.online_state, 0xbu,
              "T1: online_state <- 0xb unconditionally, 0x00475f8d-0x00475f96");

        ck_eq((uint32_t)(uint16_t)other_player_row.online_state, 0x4321u,
              "T1: a DIFFERENT player's row at the SAME index is untouched -- param_1&0xffff resolved "
              "the right player, 0x00475f16");
        ck_eq(load_u32_le(&other_player_row.anim[0 * 4]), 0xC0FFEE00u,
              "T1: same row -- the other player's anim[0] is untouched");
        ck_eq((uint32_t)(uint16_t)sibling_index_row.online_state, 0x5678u,
              "T1: a DIFFERENT index for the SAME player is untouched -- param_2 resolved the right row");
        ck_eq(load_u32_le(&sibling_index_row.anim[0 * 4]), 0xFACEB00Cu,
              "T1: same row -- the sibling index's anim[0] is untouched");
    }

    // =================================================================================================
    // T2 -- param_1's upper 16 bits are pure garbage: two calls with the SAME real (player, index) but
    // DIFFERENT garbage in the high word must land on the SAME row and produce IDENTICAL results
    // (0x00475f16: MOVZX EDX, word ptr [EBP + -0x14] -- only the low word is ever read).
    // =================================================================================================
    {
        constexpr uint16_t PLAYER = 4;
        constexpr int32_t  INDEX  = 8;
        constexpr uint16_t BID    = 17;

        uint32_t lo, hi;
        split_double_le(2468.5, lo, hi);

        auto run_with_garbage_high_word = [&](uint32_t garbage_high) -> building {
            fx.reset();
            fx.game_clock = 5.5;
            store_u32_le(&fx.cfg_buildings[BID].anim[5 * 4], 0x77776666u);

            building &b    = fx.b(PLAYER, INDEX);
            b.building_id  = BID;
            b.online_state = 0x2222;

            sim_store      own     = fx.store();
            const uint32_t param_1 = (garbage_high << 16) | PLAYER;
            detail::bldg_start_liftoff_anim_mother(fx.view(), own, param_1, INDEX, 0, 0, lo, hi);
            return b;
        };

        building result_a = run_with_garbage_high_word(0xBEEFu);
        building result_b = run_with_garbage_high_word(0x0000u);

        ck_eq(load_u32_le(&result_a.anim[0 * 4]), load_u32_le(&result_b.anim[0 * 4]),
              "T2: param_1's high-word garbage (0xBEEF vs 0x0000) does not change anim[0]");
        ck_eq_d(result_a.anim_dur[0], result_b.anim_dur[0],
                "T2: param_1's high-word garbage does not change anim_dur[0]");
        ck_eq((uint32_t)(uint16_t)result_a.online_state, (uint32_t)(uint16_t)result_b.online_state,
              "T2: param_1's high-word garbage does not change online_state");
        ck_eq((uint32_t)(uint16_t)result_a.online_state, 0xbu,
              "T2: both garbage variants resolve to the SAME real row (PLAYER=4,INDEX=8), not a "
              "garbage-derived one");
    }

    // =================================================================================================
    // T3 -- param_3(EBX)/param_4(ECX) are DEAD: two calls on the same (player, index) with wildly
    // different garbage in those two registers must produce IDENTICAL results (no read of EBX/ECX
    // anywhere in the body past the mechanical callee-save PUSH/POP pair).
    // =================================================================================================
    {
        constexpr uint16_t PLAYER = 1;
        constexpr int32_t  INDEX  = 3;
        constexpr uint16_t BID    = 9;

        uint32_t lo, hi;
        split_double_le(90909.25, lo, hi);

        auto run_with_dead_params = [&](uint32_t p3, uint32_t p4) -> building {
            fx.reset();
            fx.game_clock = 1.5;
            store_u32_le(&fx.cfg_buildings[BID].anim[5 * 4], 0x13135757u);

            building &b    = fx.b(PLAYER, INDEX);
            b.building_id  = BID;
            b.online_state = 0x3333;

            sim_store own = fx.store();
            detail::bldg_start_liftoff_anim_mother(fx.view(), own, PLAYER, INDEX, p3, p4, lo, hi);
            return b;
        };

        building result_a = run_with_dead_params(0xCAFEBABEu, 0xFEEDFACEu);
        building result_b = run_with_dead_params(0x00000000u, 0xFFFFFFFFu);

        ck_eq(load_u32_le(&result_a.anim[0 * 4]), load_u32_le(&result_b.anim[0 * 4]),
              "T3: param_3/param_4 garbage does not change anim[0] -- both are DEAD");
        ck_eq_d(result_a.anim_dur[0], result_b.anim_dur[0],
                "T3: param_3/param_4 garbage does not change anim_dur[0] -- both are DEAD");
        ck_eq((uint32_t)(uint16_t)result_a.online_state, (uint32_t)(uint16_t)result_b.online_state,
              "T3: param_3/param_4 garbage does not change online_state -- both are DEAD");
    }
}

} // namespace mh::sim::test
