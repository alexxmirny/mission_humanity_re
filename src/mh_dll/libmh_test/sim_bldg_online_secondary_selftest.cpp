//
// sim_bldg_online_secondary_selftest.cpp -- OFFLINE `simtest` oracle for FIVE of the seven functions
// in sim/sim_bldg_online_secondary.h/.cpp (RI-SIM building_tick "online animation" family, the
// register_online callees left out of scope until the):
//   llm_strat_bldg_online_airfield_a @0x004753e5
//   llm_strat_bldg_online_shuttle_a  @0x00475811
//   llm_strat_bldg_online_shuttle_h  @0x004759c3
//   llm_strat_bldg_online_port_a     @0x00475fa1
//   llm_strat_bldg_online_port_h     @0x00476153
//
// SCOPE -- READ THIS BEFORE ADDING A CASE. The other two functions in the sibling .cpp
// (llm_strat_bldg_online_helipad_h_or_misc @0x00475233, llm_strat_bldg_online_airfield_h
// @0x00475597) were ALREADY rig-verified live against the original binary (a
// 60000-step all-AI soak at game_speed_pct=1000 logged 3 and 6 calls respectively, 0 divergences).
// They are OUT OF SCOPE for this file -- do NOT add cases for them here. This file exists ONLY
// because that same soak logged the other five at ZERO calls (the AI scenario never happened to
// construct any of those five building types across 60000 steps), so they have no rig evidence;
// this offline oracle is their only evidence until a scenario exercises them.
//
// NO `_calls` struct: every one of the five functions' only outward CALL is the inert
// `utils_assert_stack_capacity` prologue check -- confirmed by direct read of each function's own
// `tmp/decomp_sim/llm_strat_bldg_online_*.asm` (NOT the `.c` drafts -- house rule). Every effect is
// a direct write into `own.building_at(player, building_index)`, same shape as
// sim_bldg_register_online_selftest.cpp (this family's own sibling oracle, whose fixture idiom this
// file duplicates: set_anim_slot_u32/anim_slot_u32 over the flattened uint8_t[48] arrays).
//
// EXPECTED BEHAVIOUR, all five (see sim_bldg_online_secondary.h's header banner for the full
// re-derivation -- every address below is independently re-confirmed against the raw .asm here,
// not copied from the header):
//   airfield_a: anim[0]=cfg.anim[1] (read 0x00475435, write 0x0047543b), anim[1]=RAW SENTINEL (write
//     0x00475454), anim[2]=cfg.anim[2] (read 0x00475491, write 0x00475497), anim[3]=cfg.anim[3]
//     (read 0x004754d0, write 0x004754d6); anim_dur[0..3]=anim_dur (0x004754ef-0x0047556a);
//     online_state=1 (write 0x00475583).
//   shuttle_a: anim[0]=cfg.anim[11] (read 0x00475861, write 0x00475867) -- the ONLY slot-11 source
//     in this family; anim[1]=RAW SENTINEL (write 0x00475880); anim[2]=cfg.anim[1] (read 0x004758bd,
//     write 0x004758c3); anim[3]=cfg.anim[3] (read 0x004758fc, write 0x00475902);
//     anim_dur[0..3]=anim_dur (0x0047591b-0x00475996); online_state=1 (write 0x004759af).
//   shuttle_h: anim[0]=cfg.anim[11] (read 0x00475a13, write 0x00475a19); anim[1]=RAW SENTINEL (write
//     0x00475a32); anim[2]=cfg.anim[3] (read 0x00475a6f, write 0x00475a75); anim[3]=cfg.anim[1] (read
//     0x00475aae, write 0x00475ab4) -- NOTE slots 2/3 are the MIRROR of shuttle_a's slots 2/3 (which
//     read cfg[1] then cfg[3]; this one reads cfg[3] then cfg[1]) -- confirmed independently from
//     THIS function's own displacement bytes, not assumed symmetric; anim_dur[0..3]=anim_dur
//     (0x00475acd-0x00475b48); online_state=1 (write 0x00475b61).
//   port_a: anim[0]=cfg.anim[1] (read 0x00475ff1, write 0x00475ff7); anim[1]=cfg.anim[6] (read
//     0x00476030, write 0x00476036); anim[2]=RAW SENTINEL (write 0x0047604f); anim[3]=cfg.anim[2]
//     (read 0x0047608c, write 0x00476092); anim_dur[0..3]=anim_dur (0x004760ab-0x00476126);
//     online_state=1 (write 0x0047613f).
//   port_h: anim[0]=cfg.anim[1] (read 0x004761a3, write 0x004761a9); anim[1]=cfg.anim[2] (read
//     0x004761e2, write 0x004761e8); anim[2]=RAW SENTINEL (write 0x00476201); anim[3]=cfg.anim[3]
//     (read 0x0047623e, write 0x00476244); anim_dur[0..3]=anim_dur (0x0047625d-0x004762d8);
//     online_state=2 (write 0x004762f1) -- the OUTLIER of this file's five (every sibling writes 1),
//     matching the family's established online_helipad_a/online_helipad_h_or_misc outlier pattern.
//   All five: `param_3`(EBX)/`param_4`(ECX) are DEAD -- no read anywhere in any body past the
//   callee-save PUSH/POP pair.
//
#include "sim/sim_bldg_online_secondary.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Independent copy of the production TU's own store_u32_le/load_u32_le -- an oracle must not reach
// into the thing it is checking. Same precedent as sim_bldg_register_online_selftest.cpp.
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

// Seeds b.anim[0..3]/anim_dur[0..3]/online_state with distinct garbage (anim[i] and anim_dur[i]
// carry index-coded literals so a shifted/off-by-one write reads a recognisably wrong number), and
// seeds a DIFFERENT player's row at the same index plus a DIFFERENT index for the same player as
// untouched-sentinel witnesses.
struct rows {
    building &b;
    building &other_player_row;
    building &sibling_index_row;
};
rows seed_rows(sim_fixture &fx, uint16_t player, int32_t index, uint16_t other_player,
               int32_t sibling_index, uint32_t garbage_tag) {
    building &b = fx.b(player, index);
    for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, garbage_tag + (uint32_t)slot);
    for (int i = 0; i < 4; ++i) b.anim_dur[i] = 100.0 + i;
    b.online_state = (int16_t)0x5555;

    building &other_player_row    = fx.b(other_player, index);
    other_player_row.online_state = 0x1111;
    set_anim_slot_u32(other_player_row.anim, 0, 0xC0FFEE00u);

    building &sibling_index_row    = fx.b(player, sibling_index);
    sibling_index_row.online_state = 0x2222;
    set_anim_slot_u32(sibling_index_row.anim, 0, 0xFACEB00Cu);

    return rows{b, other_player_row, sibling_index_row};
}

void check_untouched_witnesses(const rows &r, const char *fn) {
    ck_eq((uint32_t)(uint16_t)r.other_player_row.online_state, 0x1111u,
          (std::string(fn) + ": a DIFFERENT player's row at the SAME index is untouched").c_str());
    ck_eq(anim_slot_u32(r.other_player_row.anim, 0), 0xC0FFEE00u,
          (std::string(fn) + ": same row -- the other player's anim[0] is untouched").c_str());
    ck_eq((uint32_t)(uint16_t)r.sibling_index_row.online_state, 0x2222u,
          (std::string(fn) + ": a DIFFERENT index for the SAME player is untouched").c_str());
    ck_eq(anim_slot_u32(r.sibling_index_row.anim, 0), 0xFACEB00Cu,
          (std::string(fn) + ": same row -- the sibling index's anim[0] is untouched").c_str());
}

} // namespace

void run_bldg_online_secondary_tests() {
    sim_fixture fx;

    // =================================================================================================
    // airfield_a -- T1: full write set. cfg.anim[1]/[2]/[3] (the three real sources) get distinct,
    // non-adjacent, recognisable literals; anim[1]'s destination gets RAW_SENTINEL, proven distinct
    // from every real cfg value seeded around it.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 1;
        constexpr int32_t  B_INDEX     = 3;
        constexpr uint16_t BUILDING_ID = 40;

        rows      r       = seed_rows(fx, PLAYER, B_INDEX, 5, 50, 0xA1A10000u);
        building &b       = r.b;
        b.building_id     = BUILDING_ID;
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x50000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 1, 0x11112222u); // dest 0's source, 0x00475435/0x0047543b
        set_anim_slot_u32(cfg.anim, 2, 0x33334444u); // dest 2's source, 0x00475491/0x00475497
        set_anim_slot_u32(cfg.anim, 3, 0x55556666u); // dest 3's source, 0x004754d0/0x004754d6

        const double anim_dur = 12.25;
        sim_store    own      = fx.store();
        detail::bldg_online_airfield_a(fx.view(), own, PLAYER, B_INDEX, 0xDEAD0001u, 0xDEAD0002u,
                                       anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x11112222u, "T1(airfield_a): anim[0]=cfg.anim[1], 0x00475435/0x0047543b");
        ck_eq(anim_slot_u32(b.anim, 1), RAW_SENTINEL, "T1(airfield_a): anim[1]=RAW SENTINEL, write 0x00475454");
        ck_eq(anim_slot_u32(b.anim, 2), 0x33334444u, "T1(airfield_a): anim[2]=cfg.anim[2], 0x00475491/0x00475497");
        ck_eq(anim_slot_u32(b.anim, 3), 0x55556666u, "T1(airfield_a): anim[3]=cfg.anim[3], 0x004754d0/0x004754d6");
        ck_eq_d(b.anim_dur[0], anim_dur, "T1(airfield_a): anim_dur[0]=anim_dur, 0x004754ef/0x004754f2");
        ck_eq_d(b.anim_dur[1], anim_dur, "T1(airfield_a): anim_dur[1]=anim_dur, 0x00475514/0x00475517");
        ck_eq_d(b.anim_dur[2], anim_dur, "T1(airfield_a): anim_dur[2]=anim_dur, 0x00475539/0x0047553c");
        ck_eq_d(b.anim_dur[3], anim_dur, "T1(airfield_a): anim_dur[3]=anim_dur, 0x0047555e/0x00475561");
        ck_eq((uint32_t)(uint16_t)b.online_state, 1u, "T1(airfield_a): online_state=1, write 0x00475583");
        check_untouched_witnesses(r, "T1(airfield_a)");
    }
    // T2: param_3/param_4 dead.
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 2;
        constexpr int32_t  INDEX_A     = 60;
        constexpr int32_t  INDEX_B     = 61;
        constexpr uint16_t BUILDING_ID = 41;
        auto               seed        = [](building &b, uint16_t bid) {
            b.building_id = bid;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x9A000000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 20.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba, BUILDING_ID);
        seed(bb, BUILDING_ID);
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 1, 0xA1B2C3D4u);
        set_anim_slot_u32(cfg.anim, 2, 0xD4C3B2A1u);
        set_anim_slot_u32(cfg.anim, 3, 0x11223344u);
        const double anim_dur = 3.5;
        sim_store    own      = fx.store();
        detail::bldg_online_airfield_a(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_airfield_a(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu, anim_dur);
        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(airfield_a): param_3/param_4 dead -- anim[] identical regardless of their value");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(airfield_a): param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(airfield_a): param_3/param_4 dead -- online_state identical regardless of their value");
    }

    // =================================================================================================
    // shuttle_a -- T1: the only function in this file whose FIRST slot sources cfg.anim[11] (the last
    // legal anim slot) -- given its own distinct literal, distinguishable from the slot-1/slot-2/
    // slot-3 sources every other sibling in this batch reads.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 3;
        constexpr int32_t  B_INDEX     = 7;
        constexpr uint16_t BUILDING_ID = 42;

        rows      r       = seed_rows(fx, PLAYER, B_INDEX, 6, 51, 0xB1B10000u);
        building &b       = r.b;
        b.building_id     = BUILDING_ID;
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x51000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 11, 0x77778888u); // dest 0's source, 0x00475861/0x00475867
        set_anim_slot_u32(cfg.anim, 1, 0x99991010u);  // dest 2's source, 0x004758bd/0x004758c3
        set_anim_slot_u32(cfg.anim, 3, 0x20203030u);  // dest 3's source, 0x004758fc/0x00475902

        const double anim_dur = 7.75;
        sim_store    own      = fx.store();
        detail::bldg_online_shuttle_a(fx.view(), own, PLAYER, B_INDEX, 0xBEEF0001u, 0xBEEF0002u, anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x77778888u, "T1(shuttle_a): anim[0]=cfg.anim[11], 0x00475861/0x00475867");
        ck_eq(anim_slot_u32(b.anim, 1), RAW_SENTINEL, "T1(shuttle_a): anim[1]=RAW SENTINEL, write 0x00475880");
        ck_eq(anim_slot_u32(b.anim, 2), 0x99991010u, "T1(shuttle_a): anim[2]=cfg.anim[1], 0x004758bd/0x004758c3");
        ck_eq(anim_slot_u32(b.anim, 3), 0x20203030u, "T1(shuttle_a): anim[3]=cfg.anim[3], 0x004758fc/0x00475902");
        ck_eq_d(b.anim_dur[0], anim_dur, "T1(shuttle_a): anim_dur[0]=anim_dur, 0x0047591b/0x0047591e");
        ck_eq_d(b.anim_dur[1], anim_dur, "T1(shuttle_a): anim_dur[1]=anim_dur, 0x0047592d/0x00475940");
        ck_eq_d(b.anim_dur[2], anim_dur, "T1(shuttle_a): anim_dur[2]=anim_dur, 0x00475952/0x00475965");
        ck_eq_d(b.anim_dur[3], anim_dur, "T1(shuttle_a): anim_dur[3]=anim_dur, 0x00475977/0x0047598a");
        ck_eq((uint32_t)(uint16_t)b.online_state, 1u, "T1(shuttle_a): online_state=1, write 0x004759af");
        check_untouched_witnesses(r, "T1(shuttle_a)");
    }
    // T2: param_3/param_4 dead.
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 4;
        constexpr int32_t  INDEX_A     = 62;
        constexpr int32_t  INDEX_B     = 63;
        constexpr uint16_t BUILDING_ID = 43;
        auto               seed        = [](building &b, uint16_t bid) {
            b.building_id = bid;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x9B000000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 21.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba, BUILDING_ID);
        seed(bb, BUILDING_ID);
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 11, 0x5A5A5A5Au);
        set_anim_slot_u32(cfg.anim, 1, 0x6B6B6B6Bu);
        set_anim_slot_u32(cfg.anim, 3, 0x7C7C7C7Cu);
        const double anim_dur = 4.5;
        sim_store    own      = fx.store();
        detail::bldg_online_shuttle_a(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_shuttle_a(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu, anim_dur);
        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(shuttle_a): param_3/param_4 dead -- anim[] identical regardless of their value");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(shuttle_a): param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(shuttle_a): param_3/param_4 dead -- online_state identical regardless of their value");
    }

    // =================================================================================================
    // shuttle_h -- T1: the MIRROR of shuttle_a's slots 2/3 (cfg[3] then cfg[1], not cfg[1] then
    // cfg[3]) -- cfg.anim[1] and cfg.anim[3] get DIFFERENT literals from shuttle_a's own T1 above so a
    // copy-paste of the wrong function's expectation would fail loudly.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 5;
        constexpr int32_t  B_INDEX     = 9;
        constexpr uint16_t BUILDING_ID = 44;

        rows      r       = seed_rows(fx, PLAYER, B_INDEX, 7, 52, 0xC1C10000u);
        building &b       = r.b;
        b.building_id     = BUILDING_ID;
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x52000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 11, 0x40405050u); // dest 0's source, 0x00475a13/0x00475a19
        set_anim_slot_u32(cfg.anim, 3, 0x60607070u);  // dest 2's source, 0x00475a6f/0x00475a75
        set_anim_slot_u32(cfg.anim, 1, 0x80809090u);  // dest 3's source, 0x00475aae/0x00475ab4

        const double anim_dur = 9.125;
        sim_store    own      = fx.store();
        detail::bldg_online_shuttle_h(fx.view(), own, PLAYER, B_INDEX, 0xFEED0001u, 0xFEED0002u, anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x40405050u, "T1(shuttle_h): anim[0]=cfg.anim[11], 0x00475a13/0x00475a19");
        ck_eq(anim_slot_u32(b.anim, 1), RAW_SENTINEL, "T1(shuttle_h): anim[1]=RAW SENTINEL, write 0x00475a32");
        ck_eq(anim_slot_u32(b.anim, 2), 0x60607070u,
              "T1(shuttle_h): anim[2]=cfg.anim[3] (MIRROR of shuttle_a's anim[2]=cfg[1]), 0x00475a6f/0x00475a75");
        ck_eq(anim_slot_u32(b.anim, 3), 0x80809090u,
              "T1(shuttle_h): anim[3]=cfg.anim[1] (MIRROR of shuttle_a's anim[3]=cfg[3]), 0x00475aae/0x00475ab4");
        ck_eq_d(b.anim_dur[0], anim_dur, "T1(shuttle_h): anim_dur[0]=anim_dur, 0x00475acd/0x00475ad0");
        ck_eq_d(b.anim_dur[1], anim_dur, "T1(shuttle_h): anim_dur[1]=anim_dur, 0x00475adf/0x00475af2");
        ck_eq_d(b.anim_dur[2], anim_dur, "T1(shuttle_h): anim_dur[2]=anim_dur, 0x00475b04/0x00475b17");
        ck_eq_d(b.anim_dur[3], anim_dur, "T1(shuttle_h): anim_dur[3]=anim_dur, 0x00475b29/0x00475b3c");
        ck_eq((uint32_t)(uint16_t)b.online_state, 1u, "T1(shuttle_h): online_state=1, write 0x00475b61");
        check_untouched_witnesses(r, "T1(shuttle_h)");
    }
    // T2: param_3/param_4 dead.
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 0;
        constexpr int32_t  INDEX_A     = 64;
        constexpr int32_t  INDEX_B     = 65;
        constexpr uint16_t BUILDING_ID = 45;
        auto               seed        = [](building &b, uint16_t bid) {
            b.building_id = bid;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x9C000000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 22.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba, BUILDING_ID);
        seed(bb, BUILDING_ID);
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 11, 0x1D1D1D1Du);
        set_anim_slot_u32(cfg.anim, 3, 0x2E2E2E2Eu);
        set_anim_slot_u32(cfg.anim, 1, 0x3F3F3F3Fu);
        const double anim_dur = 5.5;
        sim_store    own      = fx.store();
        detail::bldg_online_shuttle_h(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_shuttle_h(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu, anim_dur);
        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(shuttle_h): param_3/param_4 dead -- anim[] identical regardless of their value");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(shuttle_h): param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(shuttle_h): param_3/param_4 dead -- online_state identical regardless of their value");
    }

    // =================================================================================================
    // port_a -- T1: the only function in this file whose SECOND slot sources cfg.anim[6].
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 6;
        constexpr int32_t  B_INDEX     = 11;
        constexpr uint16_t BUILDING_ID = 46;

        rows      r       = seed_rows(fx, PLAYER, B_INDEX, 0, 53, 0xD1D10000u);
        building &b       = r.b;
        b.building_id     = BUILDING_ID;
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x53000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 1, 0x14142424u); // dest 0's source, 0x00475ff1/0x00475ff7
        set_anim_slot_u32(cfg.anim, 6, 0x34345454u); // dest 1's source, 0x00476030/0x00476036
        set_anim_slot_u32(cfg.anim, 2, 0x64648484u); // dest 3's source, 0x0047608c/0x00476092

        const double anim_dur = 15.5;
        sim_store    own      = fx.store();
        detail::bldg_online_port_a(fx.view(), own, PLAYER, B_INDEX, 0xC0DE0001u, 0xC0DE0002u, anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x14142424u, "T1(port_a): anim[0]=cfg.anim[1], 0x00475ff1/0x00475ff7");
        ck_eq(anim_slot_u32(b.anim, 1), 0x34345454u, "T1(port_a): anim[1]=cfg.anim[6], 0x00476030/0x00476036");
        ck_eq(anim_slot_u32(b.anim, 2), RAW_SENTINEL, "T1(port_a): anim[2]=RAW SENTINEL, write 0x0047604f");
        ck_eq(anim_slot_u32(b.anim, 3), 0x64648484u, "T1(port_a): anim[3]=cfg.anim[2], 0x0047608c/0x00476092");
        ck_eq_d(b.anim_dur[0], anim_dur, "T1(port_a): anim_dur[0]=anim_dur, 0x004760ab/0x004760ae");
        ck_eq_d(b.anim_dur[1], anim_dur, "T1(port_a): anim_dur[1]=anim_dur, 0x004760bd/0x004760d0");
        ck_eq_d(b.anim_dur[2], anim_dur, "T1(port_a): anim_dur[2]=anim_dur, 0x004760e2/0x004760f5");
        ck_eq_d(b.anim_dur[3], anim_dur, "T1(port_a): anim_dur[3]=anim_dur, 0x00476107/0x0047611a");
        ck_eq((uint32_t)(uint16_t)b.online_state, 1u, "T1(port_a): online_state=1, write 0x0047613f");
        check_untouched_witnesses(r, "T1(port_a)");
    }
    // T2: param_3/param_4 dead.
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 7;
        constexpr int32_t  INDEX_A     = 66;
        constexpr int32_t  INDEX_B     = 67;
        constexpr uint16_t BUILDING_ID = 47;
        auto               seed        = [](building &b, uint16_t bid) {
            b.building_id = bid;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x9D000000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 23.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba, BUILDING_ID);
        seed(bb, BUILDING_ID);
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 1, 0x4E4E4E4Eu);
        set_anim_slot_u32(cfg.anim, 6, 0x5F5F5F5Fu);
        set_anim_slot_u32(cfg.anim, 2, 0x60606060u);
        const double anim_dur = 6.5;
        sim_store    own      = fx.store();
        detail::bldg_online_port_a(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_port_a(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu, anim_dur);
        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(port_a): param_3/param_4 dead -- anim[] identical regardless of their value");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(port_a): param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(port_a): param_3/param_4 dead -- online_state identical regardless of their value");
    }

    // =================================================================================================
    // port_h -- T1: the online_state=2 OUTLIER of this file's five (every other sibling writes 1).
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 1;
        constexpr int32_t  B_INDEX     = 13;
        constexpr uint16_t BUILDING_ID = 48;

        rows      r       = seed_rows(fx, PLAYER, B_INDEX, 2, 54, 0xE1E10000u);
        building &b       = r.b;
        b.building_id     = BUILDING_ID;
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        for (int slot = 0; slot < 12; ++slot)
            set_anim_slot_u32(cfg.anim, slot, 0x54000000u + (uint32_t)(slot * 0x101));
        set_anim_slot_u32(cfg.anim, 1, 0x15152525u); // dest 0's source, 0x004761a3/0x004761a9
        set_anim_slot_u32(cfg.anim, 2, 0x35355555u); // dest 1's source, 0x004761e2/0x004761e8
        set_anim_slot_u32(cfg.anim, 3, 0x65658585u); // dest 3's source, 0x0047623e/0x00476244

        const double anim_dur = 18.75;
        sim_store    own      = fx.store();
        detail::bldg_online_port_h(fx.view(), own, PLAYER, B_INDEX, 0xF00D0001u, 0xF00D0002u, anim_dur);

        ck_eq(anim_slot_u32(b.anim, 0), 0x15152525u, "T1(port_h): anim[0]=cfg.anim[1], 0x004761a3/0x004761a9");
        ck_eq(anim_slot_u32(b.anim, 1), 0x35355555u, "T1(port_h): anim[1]=cfg.anim[2], 0x004761e2/0x004761e8");
        ck_eq(anim_slot_u32(b.anim, 2), RAW_SENTINEL, "T1(port_h): anim[2]=RAW SENTINEL, write 0x00476201");
        ck_eq(anim_slot_u32(b.anim, 3), 0x65658585u, "T1(port_h): anim[3]=cfg.anim[3], 0x0047623e/0x00476244");
        ck_eq_d(b.anim_dur[0], anim_dur, "T1(port_h): anim_dur[0]=anim_dur, 0x0047625d/0x00476260");
        ck_eq_d(b.anim_dur[1], anim_dur, "T1(port_h): anim_dur[1]=anim_dur, 0x0047626f/0x00476282");
        ck_eq_d(b.anim_dur[2], anim_dur, "T1(port_h): anim_dur[2]=anim_dur, 0x00476294/0x004762a7");
        ck_eq_d(b.anim_dur[3], anim_dur, "T1(port_h): anim_dur[3]=anim_dur, 0x004762b9/0x004762cc");
        ck_eq((uint32_t)(uint16_t)b.online_state, 2u,
              "T1(port_h): online_state=2 (the OUTLIER, every other sibling in this file writes 1), "
              "write 0x004762f1");
        check_untouched_witnesses(r, "T1(port_h)");
    }
    // T2: param_3/param_4 dead.
    {
        fx.reset();
        constexpr uint16_t PLAYER      = 2;
        constexpr int32_t  INDEX_A     = 68;
        constexpr int32_t  INDEX_B     = 69;
        constexpr uint16_t BUILDING_ID = 49;
        auto               seed        = [](building &b, uint16_t bid) {
            b.building_id = bid;
            for (int slot = 0; slot < 4; ++slot) set_anim_slot_u32(b.anim, slot, 0x9E000000u + (uint32_t)slot);
            for (int i = 0; i < 4; ++i) b.anim_dur[i] = 24.0 + i;
            b.online_state = 0;
        };
        building &ba = fx.b(PLAYER, INDEX_A);
        building &bb = fx.b(PLAYER, INDEX_B);
        seed(ba, BUILDING_ID);
        seed(bb, BUILDING_ID);
        cfg_building &cfg = fx.cfg_buildings[BUILDING_ID];
        set_anim_slot_u32(cfg.anim, 1, 0x71717171u);
        set_anim_slot_u32(cfg.anim, 2, 0x82828282u);
        set_anim_slot_u32(cfg.anim, 3, 0x93939393u);
        const double anim_dur = 7.5;
        sim_store    own      = fx.store();
        detail::bldg_online_port_h(fx.view(), own, PLAYER, INDEX_A, 0u, 0u, anim_dur);
        detail::bldg_online_port_h(fx.view(), own, PLAYER, INDEX_B, 0xFFFFFFFFu, 0xFFFFFFFFu, anim_dur);
        ck(memcmp(&ba.anim[0], &bb.anim[0], sizeof(ba.anim)) == 0,
           "T2(port_h): param_3/param_4 dead -- anim[] identical regardless of their value");
        ck(memcmp(&ba.anim_dur[0], &bb.anim_dur[0], sizeof(ba.anim_dur)) == 0,
           "T2(port_h): param_3/param_4 dead -- anim_dur[] identical regardless of their value");
        ck_eq((uint32_t)(uint16_t)ba.online_state, (uint32_t)(uint16_t)bb.online_state,
              "T2(port_h): param_3/param_4 dead -- online_state identical regardless of their value");
    }
}

} // namespace mh::sim::test
