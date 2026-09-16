//
// sim_bldg_register_online_selftest.cpp -- OFFLINE `simtest` oracle for TWO of the six functions in
// sim/sim_bldg_register_online.h/.cpp (RI-SIM building_tick "online animation" family):
//   llm_strat_bldg_online_barracks_garage_a @0x00474bfc
//   llm_strat_bldg_online_helipad_a         @0x004750e5
//
// SCOPE -- READ THIS BEFORE ADDING A CASE. The other four functions in the sibling .cpp
// (llm_strat_bldg_register_online the dispatcher @0x004747d6, llm_strat_bldg_online_default
// @0x004749ed, llm_strat_bldg_online_vehicles_h @0x00474d4a, llm_strat_bldg_online_soldiers_h
// @0x00474f33) were ALREADY rig-verified live against the original binary earlier. They
// are OUT OF SCOPE for this file -- do NOT add cases for them here. This file exists ONLY because
// the rig run logged barracks_garage_a and helipad_a at ZERO calls (the AI scenario never happened
// to construct either building type), so they have no rig evidence; this offline oracle is their
// only evidence until a scenario exercises them.
//
// NO `_calls` struct: both functions' only outward CALL is the inert `utils_assert_stack_capacity`
// prologue check (0x00474c04 / 0x004750ed) -- confirmed by direct read of
// tmp/decomp_sim/llm_strat_bldg_online_barracks_garage_a_00474bfc.asm and
// _online_helipad_a_004750e5.asm (NOT the sibling .c drafts -- see this project's house rule that the
// .c is a draft that has already lied). Every effect is a direct write into
// `own.building_at(player, building_index)`, so this oracle seeds/reads `fx.b(player, index)` and
// `fx.cfg_buildings[building_id]` directly, same shape as sim_bldg_start_special_anim_selftest.cpp
// (the sibling family sim_bldg_anim_trigger.cpp's own offline oracle).
//
// CROSS-CHECKED address-by-address against sim/sim_bldg_register_online.h's "PER-FUNCTION SLOT
// COUNTS" table and the .cpp's own per-line address citations, and independently re-derived from the
// raw .asm below -- the .cpp/header and the .asm AGREE EXACTLY on every address and every immediate
// operand for both functions; no divergence found (see the "verification" notes per case below for
// the specific address arithmetic that was re-walked by hand).
//
// EXPECTED BEHAVIOUR, both functions (same shape, different slot sources -- see
// sim_bldg_register_online.h's header banner for the full family derivation):
//   bldg_online_barracks_garage_a: anim[0]=cfg.anim[2] (0x00474c4c-0x00474c52), anim[1]=RAW SENTINEL
//     0xffffffff (0x00474c6b, NOT a cfg lookup), anim[2]=cfg.anim[1] (0x00474ca8-0x00474cae);
//     anim_dur[0..2] all = the `anim_dur` argument (0x00474cc7-0x00474d1d, THREE stamps);
//     online_state = 1 UNCONDITIONALLY, LAST (0x00474d36).
//   bldg_online_helipad_a: anim[0]=cfg.anim[1] (0x00475135-0x0047513b), anim[1]=RAW SENTINEL
//     0xffffffff (0x00475154, NOT a cfg lookup), anim[2]=cfg.anim[3] (0x00475191-0x00475197);
//     anim_dur[0..2] all = the `anim_dur` argument (0x004751b0-0x00475206, THREE stamps);
//     online_state = 2 UNCONDITIONALLY, LAST (0x0047521f) -- the OUTLIER of the five-handler family
//     (every sibling writes 1). Re-decoded directly from the immediate operand bytes
//     (`66 c7 80 b7 d2 c3 00 02 00`: disp=0x00c3d2b7, imm16=0x0002), matching the header banner.
//   Both: `param_3`(EBX)/`param_4`(ECX) are DEAD -- no read anywhere in either body past the
//     callee-save PUSH/POP pair.
//
#include "sim/sim_bldg_register_online.h"

#include <cstdint>
#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Raw little-endian 4-byte accessors over the flattened uint8_t[48] anim arrays -- an INDEPENDENT
// copy of the production TU's own store_u32_le/load_u32_le (sim_bldg_register_online.cpp's anonymous
// namespace); an oracle must not reach into the thing it is checking. Same duplicate-per-TU
// precedent every other sim/ selftest with this pair of flattened fields already follows
// (sim_bldg_start_special_anim_selftest.cpp, sim_bldg_start_liftoff_anim_*_selftest.cpp).
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

constexpr uint32_t RAW_SENTINEL = 0xffffffffu; // cfg_t_frame_index "no frame"

} // namespace

void run_bldg_register_online_tests() {
    sim_fixture fx;

    // =================================================================================================
    // bldg_online_barracks_garage_a -- T1: the full write set, seeded so every distinct mechanism the
    // header/.asm derive is independently pinned. cfg.anim[1] and cfg.anim[2] (the two REAL sources)
    // get distinct, recognisable, non-adjacent values -- distinct from each other AND from
    // RAW_SENTINEL -- so a mixed-up slot mapping (e.g. dest0<-cfg[1] instead of cfg[2]) would read a
    // recognisably wrong number, and cfg.anim[1]'s distinctive value (which is NOT 0xffffffff) sitting
    // right next to dest1's RAW_SENTINEL write proves dest1 is truly a raw store, not an accidental
    // cfg[1] lookup that happened to land there.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER        = 4;
        constexpr int32_t  B_INDEX       = 7;
        constexpr uint16_t OTHER_PLAYER  = 1;
        constexpr int32_t  SIBLING_INDEX = 12;
        constexpr uint16_t BUILDING_ID   = 21;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = BUILDING_ID;

        // anim[0..2] (the write span) seeded with distinct garbage, distinct from every expected
        // outcome below; anim[3] is the neighbor sentinel proving the 3-slot write doesn't overrun.
        set_anim_slot_u32(b.anim, 0, 0xAAAA0000u);
        set_anim_slot_u32(b.anim, 1, 0xBBBB0001u);
        set_anim_slot_u32(b.anim, 2, 0xCCCC0002u);
        set_anim_slot_u32(b.anim, 3, 0xDDDD0003u);

        // cfg.anim: every one of the 12 frame-index slots gets its OWN distinct, non-sequential value
        // (not adjacent integers, so byte-swapped/off-by-one reads don't coincidentally match), with
        // the two REAL sources (k=1, k=2) pinned to specific recognisable literals.
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x60000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 1, 0x11113333u); // dest 2's source, 0x00474ca8-0x00474cae
        set_anim_slot_u32(cfg.anim, 2, 0x22224444u); // dest 0's source, 0x00474c4c-0x00474c52

        // anim_dur[0..2] (the write span) seeded with distinct garbage; anim_dur[3] is the neighbor
        // sentinel proving the 3-stamp write doesn't overrun into the building record's other 9 slots.
        b.anim_dur[0] = 111.0;
        b.anim_dur[1] = 222.0;
        b.anim_dur[2] = 333.0;
        b.anim_dur[3] = -999.0;

        // online_state seeded with a sentinel distinct from both 0 and the expected 1.
        b.online_state = 0x7777;

        // Sibling rows the write must NOT touch -- proves building_at(player, building_index)
        // resolution lands exactly on (PLAYER, B_INDEX), not a neighbour player or a neighbour index.
        building &other_player_row    = fx.b(OTHER_PLAYER, B_INDEX);
        other_player_row.online_state = 0x1111;
        set_anim_slot_u32(other_player_row.anim, 0, 0xC0FFEE00u);

        building &sibling_index_row    = fx.b(PLAYER, SIBLING_INDEX);
        sibling_index_row.online_state = 0x2222;
        set_anim_slot_u32(sibling_index_row.anim, 0, 0xFACEB00Cu);

        const double anim_dur = 3.75; // exactly representable, distinct from every seeded garbage value
        // param_3(EBX)/param_4(ECX): DEAD -- distinctive nonzero sentinels with no plausible
        // legitimate meaning if accidentally read as an address, index, or count. See T2 below for the
        // explicit dual-call proof; this case alone would already fail if either were secretly used as
        // an index/offset feeding into the writes checked here.
        const uint32_t param_3 = 0xCAFEBABEu;
        const uint32_t param_4 = 0xFEEDFACEu;

        sim_store own = fx.store();
        detail::bldg_online_barracks_garage_a(fx.view(), own, PLAYER, B_INDEX, param_3, param_4,
                                              anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x22224444u,
              "T1(barracks_garage_a): anim[0] = cfg.anim[2], building_id read 0x00474c2c, cfg read "
              "0x00474c4c, write 0x00474c52");
        ck_eq(anim_slot_u32(b.anim, 1), RAW_SENTINEL,
              "T1(barracks_garage_a): anim[1] = RAW SENTINEL 0xffffffff, NOT a cfg lookup, write "
              "0x00474c6b");
        ck_eq(anim_slot_u32(b.anim, 2), 0x11113333u,
              "T1(barracks_garage_a): anim[2] = cfg.anim[1], building_id read 0x00474c88, cfg read "
              "0x00474ca8, write 0x00474cae");
        ck_eq(anim_slot_u32(b.anim, 3), 0xDDDD0003u,
              "T1(barracks_garage_a): anim[3] (neighbor slot) untouched -- the 3-slot write doesn't "
              "overrun");

        ck_eq_d(b.anim_dur[0], anim_dur,
                "T1(barracks_garage_a): anim_dur[0] = the anim_dur argument, write 0x00474cc7/"
                "0x00474cd3");
        ck_eq_d(b.anim_dur[1], anim_dur,
                "T1(barracks_garage_a): anim_dur[1] = the SAME stamp, write 0x00474cec/0x00474cf8");
        ck_eq_d(b.anim_dur[2], anim_dur,
                "T1(barracks_garage_a): anim_dur[2] = the SAME stamp, write 0x00474d11/0x00474d1d");
        ck_eq_d(b.anim_dur[3], -999.0,
                "T1(barracks_garage_a): anim_dur[3] (neighbor slot) untouched -- the 3-stamp span is "
                "bounded to [0..2]");

        ck_eq((uint32_t)(uint16_t)b.online_state, 1u,
              "T1(barracks_garage_a): online_state = 1 unconditionally, written LAST, 0x00474d36");

        ck_eq((uint32_t)(uint16_t)other_player_row.online_state, 0x1111u,
              "T1(barracks_garage_a): a DIFFERENT player's row at the SAME index is untouched");
        ck_eq(anim_slot_u32(other_player_row.anim, 0), 0xC0FFEE00u,
              "T1(barracks_garage_a): same row -- the other player's anim[0] is untouched");
        ck_eq((uint32_t)(uint16_t)sibling_index_row.online_state, 0x2222u,
              "T1(barracks_garage_a): a DIFFERENT index for the SAME player is untouched");
        ck_eq(anim_slot_u32(sibling_index_row.anim, 0), 0xFACEB00Cu,
              "T1(barracks_garage_a): same row -- the sibling index's anim[0] is untouched");
    }

    // =================================================================================================
    // bldg_online_barracks_garage_a -- T2: param_3(EBX)/param_4(ECX) are DEAD. Run the identical call
    // twice on two separately-seeded (but identically-seeded) building records, differing ONLY in
    // param_3/param_4's value across the widest possible span (0 vs 0xffffffff), and confirm the two
    // resulting records are byte-identical.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 2;
        constexpr int32_t  INDEX_A     = 30;
        constexpr int32_t  INDEX_B     = 31;
        constexpr uint16_t BUILDING_ID = 5;

        auto seed = [](building &b) {
            b.building_id = BUILDING_ID;
            for (int slot = 0; slot < 4; ++slot)
                set_anim_slot_u32(b.anim, slot, 0x77770000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 5.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba);
        seed(bb);

        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 1, 0x0BADF00Du);
        set_anim_slot_u32(cfg.anim, 2, 0xF00DBEEFu);

        const double anim_dur = 8.5;

        sim_store own = fx.store();
        detail::bldg_online_barracks_garage_a(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_barracks_garage_a(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                              anim_dur);

        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(barracks_garage_a): param_3/param_4 dead -- anim[] identical regardless of their value "
           "(no read past the callee-save PUSH/POP pair, 0x00474c09-0x00474c0d/0x00474d42-0x00474d45)");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(barracks_garage_a): param_3/param_4 dead -- anim_dur[] identical regardless of their "
           "value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(barracks_garage_a): param_3/param_4 dead -- online_state identical regardless of "
              "their value");
    }

    // =================================================================================================
    // bldg_online_helipad_a -- T1: same shape as barracks_garage_a's T1 above, DIFFERENT cfg sources
    // (dest0<-cfg[1], dest2<-cfg[3]) and the OUTLIER online_state=2. cfg.anim[1] and cfg.anim[3] get
    // distinct, non-adjacent, recognisable literals so a mixed-up slot mapping reads a wrong number.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER        = 6;
        constexpr int32_t  B_INDEX       = 15;
        constexpr uint16_t OTHER_PLAYER  = 3;
        constexpr int32_t  SIBLING_INDEX = 22;
        constexpr uint16_t BUILDING_ID   = 33;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = BUILDING_ID;

        set_anim_slot_u32(b.anim, 0, 0x11110000u);
        set_anim_slot_u32(b.anim, 1, 0x22220001u);
        set_anim_slot_u32(b.anim, 2, 0x33330002u);
        set_anim_slot_u32(b.anim, 3, 0x44440003u); // neighbor sentinel

        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x70000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 1, 0x55559999u); // dest 0's source, 0x00475135-0x0047513b
        set_anim_slot_u32(cfg.anim, 3, 0x6666AAAAu); // dest 2's source, 0x00475191-0x00475197

        b.anim_dur[0] = 444.0;
        b.anim_dur[1] = 555.0;
        b.anim_dur[2] = 666.0;
        b.anim_dur[3] = -999.0; // neighbor sentinel

        b.online_state = (int16_t)0x8888; // distinct from both 0 and the expected 2

        building &other_player_row    = fx.b(OTHER_PLAYER, B_INDEX);
        other_player_row.online_state = 0x1111;
        set_anim_slot_u32(other_player_row.anim, 0, 0xC0FFEE00u);

        building &sibling_index_row    = fx.b(PLAYER, SIBLING_INDEX);
        sibling_index_row.online_state = 0x2222;
        set_anim_slot_u32(sibling_index_row.anim, 0, 0xFACEB00Cu);

        const double   anim_dur = 6.25;
        const uint32_t param_3  = 0xDEADBEEFu;
        const uint32_t param_4  = 0x0BADC0DEu;

        sim_store own = fx.store();
        detail::bldg_online_helipad_a(fx.view(), own, PLAYER, B_INDEX, param_3, param_4, anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x55559999u,
              "T1(helipad_a): anim[0] = cfg.anim[1], building_id read 0x00475115, cfg read "
              "0x00475135, write 0x0047513b");
        ck_eq(anim_slot_u32(b.anim, 1), RAW_SENTINEL,
              "T1(helipad_a): anim[1] = RAW SENTINEL 0xffffffff, NOT a cfg lookup, write 0x00475154");
        ck_eq(anim_slot_u32(b.anim, 2), 0x6666AAAAu,
              "T1(helipad_a): anim[2] = cfg.anim[3], building_id read 0x00475171, cfg read "
              "0x00475191, write 0x00475197");
        ck_eq(anim_slot_u32(b.anim, 3), 0x44440003u,
              "T1(helipad_a): anim[3] (neighbor slot) untouched -- the 3-slot write doesn't overrun");

        ck_eq_d(b.anim_dur[0], anim_dur,
                "T1(helipad_a): anim_dur[0] = the anim_dur argument, write 0x004751b0/0x004751b9");
        ck_eq_d(b.anim_dur[1], anim_dur,
                "T1(helipad_a): anim_dur[1] = the SAME stamp, write 0x004751d5/0x004751de");
        ck_eq_d(b.anim_dur[2], anim_dur,
                "T1(helipad_a): anim_dur[2] = the SAME stamp, write 0x004751fa/0x00475203");
        ck_eq_d(b.anim_dur[3], -999.0,
                "T1(helipad_a): anim_dur[3] (neighbor slot) untouched -- the 3-stamp span is bounded "
                "to [0..2]");

        ck_eq((uint32_t)(uint16_t)b.online_state, 2u,
              "T1(helipad_a): online_state = 2 (the OUTLIER, every sibling writes 1), written LAST, "
              "0x0047521f -- re-decoded from immediate bytes 66 c7 80 b7 d2 c3 00 02 00");

        ck_eq((uint32_t)(uint16_t)other_player_row.online_state, 0x1111u,
              "T1(helipad_a): a DIFFERENT player's row at the SAME index is untouched");
        ck_eq(anim_slot_u32(other_player_row.anim, 0), 0xC0FFEE00u,
              "T1(helipad_a): same row -- the other player's anim[0] is untouched");
        ck_eq((uint32_t)(uint16_t)sibling_index_row.online_state, 0x2222u,
              "T1(helipad_a): a DIFFERENT index for the SAME player is untouched");
        ck_eq(anim_slot_u32(sibling_index_row.anim, 0), 0xFACEB00Cu,
              "T1(helipad_a): same row -- the sibling index's anim[0] is untouched");
    }

    // =================================================================================================
    // bldg_online_helipad_a -- T2: param_3(EBX)/param_4(ECX) are DEAD, same dual-call proof as
    // barracks_garage_a's T2 above.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 0;
        constexpr int32_t  INDEX_A     = 40;
        constexpr int32_t  INDEX_B     = 41;
        constexpr uint16_t BUILDING_ID = 8;

        auto seed = [](building &b) {
            b.building_id = BUILDING_ID;
            for (int slot = 0; slot < 4; ++slot)
                set_anim_slot_u32(b.anim, slot, 0x88880000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 9.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba);
        seed(bb);

        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 1, 0xABCD1234u);
        set_anim_slot_u32(cfg.anim, 3, 0x4321DCBAu);

        const double anim_dur = 1.5;

        sim_store own = fx.store();
        detail::bldg_online_helipad_a(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_helipad_a(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                      anim_dur);

        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(helipad_a): param_3/param_4 dead -- anim[] identical regardless of their value (no "
           "read past the callee-save PUSH/POP pair, 0x004750f2-0x004750f6/0x0047522b-0x0047522e)");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(helipad_a): param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(helipad_a): param_3/param_4 dead -- online_state identical regardless of their "
              "value");
    }
}

} // namespace mh::sim::test
