//
// sim_bldg_refund_resources_scaled_by_energy_selftest.cpp -- `simtest` cases for the BODY of
// llm_strat_bldg_refund_resources_scaled_by_energy @0x00479493 (sim/sim_bldg_refund_resources_scaled_by_
// energy.h/.cpp, RI-SIM / SIM1-G4).
//
// The function grants the target building's CFG TYPE'S configured resource list back to the player,
// scaled by how charged/complete the building INSTANCE currently is (its own `.energy`, the HP/charge
// stat -- NOT the POWER resource; see docs/conventions.md#energy-is-not-power), by calling llm_resource_add once per non-empty cfg
// resource slot. It performs NO sim-state writes (a pure read over buildings / cfg_buildings plus the
// one outward call), so this file's assertions are entirely on the RECORDED CALL SEQUENCE: how many
// refunds, in what order, and the exact (player, resource_index, amount) each carries. Modeled directly
// on sim_unit_refund_selftest.cpp's run_unit_refund_tests() -- same ratio-scaled-by-a-stat-then-loop-
// over-resource-slots idiom, one level up (building instance + its cfg TYPE table instead of unit
// instance + its cfg TYPE table).
//
// EVERY EXPECTED VALUE IS DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_refund_resources_scaled_by_energy_00479493.asm), not from the C++
// translation. The formula, instruction by instruction:
//
//   bid   = (uint16_t)buildings[player][index].building_id            @0x004794c0 MOVZX word
//   ratio = (buildings[player][index].energy * REFUND_ENERGY_FACTOR)   @0x004794da FLD /
//           / cfg_buildings[bid].energy                                 0x004794e0 FMUL DAT /
//                                                                        0x004794ed FDIV /
//                                                                        0x004794f3 FSTP -> 64-bit local
//           computed EXACTLY ONCE before the loop, reloaded (not recomputed) every iteration
//           @0x00479538 FMUL double ptr [EBP + -0x2c].
//   for (i = 0; ; ++i):
//       resource_id = (int32)cfg_buildings[bid].resource[i].id         @0x0047950c MOV dword
//       if (resource_id == 0) break;                                   @0x00479515/19 CMP,0 / JZ
//       if (!(i < 7)) break;                                           @0x0047951b/1f CMP,7 / JL
//       amount = trunc_toward_zero(resource[i].val * ratio)            @0x00479532 FILD dword val /
//                                                                        0x00479538 FMUL ratio /
//                                                                        0x0047953b CALL utils_math_trunc
//                                                                                   (RC=11 truncate) /
//                                                                        0x00479540 FISTP dword
//       llm_resource_add(player, resource_id, amount)                  @0x00479543 EDX=id (from -0x18) /
//                                                                        0x00479546 EAX=player /
//                                                                        0x00479549 EBX=amount /
//                                                                        0x0047954c CALL
//
// THE CONJUNCTION IS id-FIRST: the id==0 check (0x00479515/19) runs BEFORE the i<7 bound check
// (0x0047951b/1f), so the id READ at index i always happens on loop-body entry, including i==7 -- one
// slot past cfg_building::resource's declared 7-entry extent. UNLIKE the sibling cfg_unit table (whose
// resource[7] lands exactly on resource_2[0], see sim_unit_refund_selftest.cpp's own i==7 case), THIS
// struct's layout does NOT put resource_2 right after resource: per addr/mh_structs.gen.h,
// mh_cfg_final_struct_Building::resource is at 0x6f6 and resource_2 is at 0x736, a 0x40-byte gap --
// while resource[7] (0x6f6 + 7*8 = 0x72e) lands exactly on `build_time_2` (a plain `double` at 0x72e,
// confirmed via the struct's own static_assert). So the OOB id read at i==7 actually reads the low 4
// bytes of `build_time_2`'s bit pattern, not a resource_2 entry -- independently re-derived here per the
// header's own "not independently re-derived for cfg_building" flag, and it changes the fixture-seeding
// target from the sibling's `resource_2[0]` to a bit-punned `build_time_2`. It does NOT change the
// user-visible behavior: id!=0 at i==7 still fails the i<7 bound check and exits WITHOUT a resource_add
// call (0x0047951b/1f JL not taken -> falls into LAB_00479521 -> unconditional JMP 0x00479559, the
// function's exit), so the observable effect is identical to the sibling -- exactly 7 refunds fire for a
// full 7-slot table, never an 8th, and the bound (not a lucky zero) is what stops it.
//
// FP EXACTNESS: every fixture ratio below is exactly representable (1.0, 0.375, 0.5, or an Infinity) and
// every resource val is a small integer, so `val * ratio` is either an exact double or unambiguously far
// from an integer boundary (the -2.7 / 6.3 pair). No tolerance is needed or wanted (ck_eq is exact).
//
#include "sim/sim_bldg_refund_resources_scaled_by_energy.h"

#include "sim_test_support.h"

#include <cstdint>
#include <cstring>

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// player < MAX_PLAYERS (8), building_index < BUILDINGS_PER_PLAYER; distinct and non-symmetric so a
// swapped-IMUL translation lands in a different record. Reused across cases -- each case builds its own
// fresh sim_fixture, so there is no cross-case state to collide.
constexpr int32_t PLAYER = 3;
constexpr int32_t BLDG   = 5;

struct refund_log {
    struct ev {
        int32_t player;
        int32_t resource_index;
        int32_t amount;
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
refund_log g_log;

const bldg_refund_resources_scaled_by_energy_calls &recording_calls() {
    static const bldg_refund_resources_scaled_by_energy_calls c = {
        [](int32_t player, int32_t resource_index, int32_t amount) -> void {
            g_log.events.push_back(refund_log::ev{player, resource_index, amount});
        },
    };
    return c;
}

// Seed one cfg resource slot on cfg_buildings[bid]. `id` is the E_RESOURCE key (0 = the UNDEFINED
// sentinel that stops the walk); `val` is the per-slot amount the refund scales.
void put_resource(sim_fixture &f, uint32_t bid, int32_t slot, uint32_t id, int32_t val) {
    f.cfg_buildings[bid].resource[slot].id  = id;
    f.cfg_buildings[bid].resource[slot].val = val;
}

// Assert the recorded refund at `n` is exactly (player, resource_index, amount).
void ck_ev(int32_t n, int32_t player, int32_t resource_index, int32_t amount, const char *what) {
    if ((size_t)n >= g_log.events.size()) {
        ck(false, what);
        return;
    }
    const refund_log::ev &e = g_log.events[(size_t)n];
    ck(e.player == player && e.resource_index == resource_index && e.amount == amount, what);
}

// ---- case 1: easy sanity, ratio == 1.0 exactly ----------------------------------------------------
// instance energy 40 * REFUND_ENERGY_FACTOR (0.5, the fixture's real read-memory-confirmed default,
// @0x0050138a) / cfg energy 20 -> ratio = 20.0 / 20.0 = 1.0 (@0x004794da-0x004794f3). Two slots with
// distinct, non-symmetric (id, val) pairs prove the ratio is genuinely applied (amount == val when
// ratio == 1.0) rather than e.g. always emitting 0 or val unconditionally; the third slot's id == 0
// stops the walk (@0x00479515/19).
void test_ratio_one_sanity_case() {
    sim_fixture        f;
    constexpr uint32_t BID        = 21;
    f.b(PLAYER, BLDG).building_id = static_cast<uint16_t>(BID); // @0x004794aa-0x004794c7 MOVZX word
    f.b(PLAYER, BLDG).energy      = 40.0;                       // instance energy (HP/charge, not POWER)
    f.cfg_buildings[BID].energy   = 20.0;                       // cfg TYPE max energy, the divisor
    put_resource(f, BID, 0, /*id=*/11, /*val=*/7);              // -> trunc(7*1.0)=7
    put_resource(f, BID, 1, /*id=*/22, /*val=*/-3);             // -> trunc(-3*1.0)=-3
    put_resource(f, BID, 2, /*id=*/0, /*val=*/999);             // id 0: STOP (val must not be read)

    const sim_view v = f.view();
    g_log.reset();
    detail::bldg_refund_resources_scaled_by_energy(v, recording_calls(), PLAYER, BLDG);

    ck_eq((uint32_t)g_log.events.size(), 2u,
          "ratio==1.0: two non-empty slots -> two refunds (@0x00479515/19 id-zero stop)");
    ck_ev(0, PLAYER, 11, 7, "refund[0]: (player, id=11, trunc(7*1.0)=7) @0x00479540/4c");
    ck_ev(1, PLAYER, 22, -3, "refund[1]: (player, id=22, trunc(-3*1.0)=-3) @0x00479540/4c");
}

// ---- case 2: non-round ratio, reused verbatim across iterations -----------------------------------
// instance energy 33 * 0.5 / cfg energy 44 -> ratio = 16.5 / 44 = 0.375 exactly (3/8, exactly
// representable). If a mistranslation hardcoded ratio to 1.0 (plausible after case 1) or recomputed it
// per-iteration from mutated state, this case's non-round, non-1.0 value catches it: 8*0.375=3.0,
// 17*0.375=6.375->trunc 6, -8*0.375=-3.0 -- all exact, and the SAME ratio must apply to all three slots
// since the original computes it once at @0x004794f3 (FSTP double ptr [EBP+-0x2c]) and only ever RELOADS
// it inside the loop @0x00479538, never recomputing the FLD/FMUL/FDIV chain per-iteration.
void test_non_round_ratio_reused_every_iteration_FINDING() {
    sim_fixture        f;
    constexpr uint32_t BID        = 33;
    f.b(PLAYER, BLDG).building_id = static_cast<uint16_t>(BID);
    f.b(PLAYER, BLDG).energy      = 33.0;
    f.cfg_buildings[BID].energy   = 44.0;
    put_resource(f, BID, 0, /*id=*/1, /*val=*/8);  // -> trunc(8*0.375)=trunc(3.0)=3
    put_resource(f, BID, 1, /*id=*/2, /*val=*/17); // -> trunc(17*0.375)=trunc(6.375)=6
    put_resource(f, BID, 2, /*id=*/3, /*val=*/-8); // -> trunc(-8*0.375)=trunc(-3.0)=-3
    put_resource(f, BID, 3, /*id=*/0, /*val=*/0);  // STOP

    const sim_view v = f.view();
    g_log.reset();
    detail::bldg_refund_resources_scaled_by_energy(v, recording_calls(), PLAYER, BLDG);

    ck_eq((uint32_t)g_log.events.size(), 3u,
          "ratio==0.375: three non-empty slots -> three refunds, same ratio reused @0x00479538");
    ck_ev(0, PLAYER, 1, 3, "refund[0]: (player, id=1, trunc(8*0.375)=3)");
    ck_ev(1, PLAYER, 2, 6, "refund[1]: (player, id=2, trunc(17*0.375)=6, fraction dropped)");
    ck_ev(2, PLAYER, 3, -3, "refund[2]: (player, id=3, trunc(-8*0.375)=-3, same 0.375 ratio applied)");
}

// ---- case 3: zero-id sentinel in the MIDDLE of the array (slot 2 of 7) ----------------------------
// resource[2].id == 0 must stop the walk right there (@0x00479515/19), even though slots 3-6 are
// deliberately seeded with non-zero ids that would otherwise refund if the walk kept going -- proving
// the id-zero check is what stops it mid-array, not merely "ran out of seeded slots".
void test_stop_at_zero_id_in_middle_of_array() {
    sim_fixture        f;
    constexpr uint32_t BID        = 45;
    f.b(PLAYER, BLDG).building_id = static_cast<uint16_t>(BID);
    f.b(PLAYER, BLDG).energy      = 20.0;
    f.cfg_buildings[BID].energy   = 20.0;           // ratio = (20*0.5)/20 = 0.5
    put_resource(f, BID, 0, /*id=*/51, /*val=*/10); // -> trunc(10*0.5)=5
    put_resource(f, BID, 1, /*id=*/52, /*val=*/20); // -> trunc(20*0.5)=10
    put_resource(f, BID, 2, /*id=*/0, /*val=*/777); // STOP (mid-array sentinel; val must not be read)
    // Slots 3-6: non-zero ids that must NEVER be reached, because slot 2 stopped the walk first.
    put_resource(f, BID, 3, /*id=*/53, /*val=*/30);
    put_resource(f, BID, 4, /*id=*/54, /*val=*/40);
    put_resource(f, BID, 5, /*id=*/55, /*val=*/50);
    put_resource(f, BID, 6, /*id=*/56, /*val=*/60);

    const sim_view v = f.view();
    g_log.reset();
    detail::bldg_refund_resources_scaled_by_energy(v, recording_calls(), PLAYER, BLDG);

    ck_eq((uint32_t)g_log.events.size(), 2u,
          "mid-array zero id (slot 2 of 7) stops the walk -> exactly two refunds, slots 3-6 never fire");
    ck_ev(0, PLAYER, 51, 5, "refund[0]: (player, id=51, trunc(10*0.5)=5)");
    ck_ev(1, PLAYER, 52, 10, "refund[1]: (player, id=52, trunc(20*0.5)=10)");
}

// ---- case 4: truncation is toward ZERO, not floor --------------------------------------------------
// instance energy 180 * 0.5 / cfg energy 100 -> ratio = 90/100 = 0.9. val -3 -> -2.7 -> trunc toward
// zero -> -2 (a FLOOR would give -3). val 7 -> 6.3 -> trunc 6 (both directions agree with floor, so the
// negative slot is what actually pins the rounding MODE). Same FRNDINT RC=11 (truncate) control-word
// swap @0x004d059f/a7 (utils_math_trunc, inlined via refund_amount()'s __asm block) that the sibling
// sim_unit_refund_selftest.cpp's negative-slot case exercises.
void test_truncation_toward_zero_negative_FINDING() {
    sim_fixture        f;
    constexpr uint32_t BID        = 12;
    f.b(PLAYER, BLDG).building_id = static_cast<uint16_t>(BID);
    f.b(PLAYER, BLDG).energy      = 180.0;
    f.cfg_buildings[BID].energy   = 100.0;          // ratio = (180*0.5)/100 = 0.9
    put_resource(f, BID, 0, /*id=*/61, /*val=*/7);  // -> trunc(7*0.9)=trunc(6.3)=6
    put_resource(f, BID, 1, /*id=*/62, /*val=*/-3); // -> trunc(-3*0.9)=trunc(-2.7)=-2, NOT -3
    put_resource(f, BID, 2, /*id=*/0, /*val=*/0);   // STOP

    const sim_view v = f.view();
    g_log.reset();
    detail::bldg_refund_resources_scaled_by_energy(v, recording_calls(), PLAYER, BLDG);

    ck_eq((uint32_t)g_log.events.size(), 2u, "ratio==0.9: two non-empty slots -> two refunds");
    ck_ev(0, PLAYER, 61, 6, "refund[0]: (player, id=61, trunc(7*0.9)=trunc(6.3)=6)");
    ck_ev(1, PLAYER, 62, -2,
          "refund[1] FINDING: (player, id=62, trunc(-3*0.9)=trunc(-2.7)=-2, toward ZERO not floor(-3)) "
          "@0x004d059f/a7 FRNDINT RC=11");
}

// ---- case 5: UNGUARDED DIVISOR -- cfg_buildings[bid].energy == 0.0 -> INT_MIN, not a skip/guard ----
// cfg energy 0.0 is never checked (@0x004794e6-0x004794ed): instance energy 10 (nonzero) * 0.5 / 0.0 ->
// ratio = +Infinity. refund_amount(4, +Inf): FMUL propagates the Infinity, FRNDINT raises the masked
// (non-trapping) invalid-operation exception on the non-finite ST0 and leaves it unconverted, and FISTP
// of a non-finite operand stores the x87 "integer indefinite" pattern 0x80000000 == INT32_MIN --
// reproduced literally per the header's own derivation, not guarded or skipped. Only one resource slot
// is seeded so the single recorded call isolates the assertion.
void test_zero_divisor_unguarded_yields_int_min_FINDING() {
    sim_fixture        f;
    constexpr uint32_t BID        = 60;
    f.b(PLAYER, BLDG).building_id = static_cast<uint16_t>(BID);
    f.b(PLAYER, BLDG).energy      = 10.0; // nonzero numerator -> ratio is +Infinity, not NaN
    f.cfg_buildings[BID].energy   = 0.0;  // the UNGUARDED divisor @0x004794e6-0x004794ed
    put_resource(f, BID, 0, /*id=*/99, /*val=*/4);
    put_resource(f, BID, 1, /*id=*/0, /*val=*/0); // STOP after the one call

    const sim_view v = f.view();
    g_log.reset();
    detail::bldg_refund_resources_scaled_by_energy(v, recording_calls(), PLAYER, BLDG);

    ck_eq((uint32_t)g_log.events.size(), 1u, "0-energy cfg type: exactly one refund call fires (not a skip)");
    ck(g_log.events.size() >= 1 && g_log.events[0].player == PLAYER && g_log.events[0].resource_index == 99,
       "refund[0]: (player, id=99) still fires despite the +Inf ratio");
    if (!g_log.events.empty()) {
        ck_eq((uint32_t)g_log.events[0].amount, (uint32_t)INT32_MIN,
              "refund[0] FINDING: amount == INT32_MIN (0x80000000, the x87 'integer indefinite' pattern "
              "from FISTP of a non-finite operand) @0x00479540, NOT a guard/skip/exception");
    }
}

// ---- case 6: i==7 -- the id READ happens before the i<7 bound check, but it never reaches a call ---
// All seven resource[] slots carry a non-zero id, so the id-zero check (@0x00479515/19) never breaks
// early. The walk processes i = 0..6 (seven refunds) and at i == 7 reads resource[7].id BEFORE testing
// i < 7 (@0x0047950c happens unconditionally on loop-body entry) -- but UNLIKE the sibling cfg_unit
// table, cfg_building's resource[7] does NOT land on resource_2[0]: per addr/mh_structs.gen.h,
// resource is at 0x6f6 and resource_2 is at 0x736 (a 0x40-byte gap), while resource[7] == 0x6f6+7*8 ==
// 0x72e lands exactly on `build_time_2` (a plain double at 0x72e). This case bit-puns build_time_2's
// low 4 bytes to a deliberately NON-ZERO id (0x9999) -- proving the walk is stopped by the i<7 BOUND
// (@0x0047951b/1f JL not taken -> LAB_00479521 -> unconditional JMP 0x00479559 exit), not by luckily
// landing on a zero sentinel at i==7. Regardless of what the OOB read returns, this control path can
// NEVER reach the resource_add call (that requires JL taken, i.e. i<7) -- so an 8th refund must not
// fire either way; the bit-pun only makes the "not a lucky zero" half of the claim checkable too.
// ratio = (100*0.5)/100 = 0.5 exactly.
void test_seven_full_slots_stop_at_bound_not_oob_read_FINDING() {
    sim_fixture        f;
    constexpr uint32_t BID        = 77;
    f.b(PLAYER, BLDG).building_id = static_cast<uint16_t>(BID);
    f.b(PLAYER, BLDG).energy      = 100.0;
    f.cfg_buildings[BID].energy   = 100.0; // ratio = (100*0.5)/100 = 0.5
    // ids 201..207, vals 8,16,24,32,40,48,56 -> amounts *0.5 = 4,8,12,16,20,24,28.
    put_resource(f, BID, 0, 201, 8);
    put_resource(f, BID, 1, 202, 16);
    put_resource(f, BID, 2, 203, 24);
    put_resource(f, BID, 3, 204, 32);
    put_resource(f, BID, 4, 205, 40);
    put_resource(f, BID, 5, 206, 48);
    put_resource(f, BID, 6, 207, 56);
    // resource[7] over-read target == build_time_2 (offset 0x72e, see the case comment above), bit-punned
    // so the low 4 bytes (would-be resource[7].id) are a deliberately NON-ZERO sentinel; the high 4 bytes
    // (would-be resource[7].val) are never read on this control path (the i<7 bound breaks first), so
    // they are left 0.
    {
        uint32_t oob_id_low   = 0x9999;
        int32_t  oob_val_high = 0;
        double   build_time_2_bits{};
        std::memcpy(&build_time_2_bits, &oob_id_low, sizeof(oob_id_low));
        std::memcpy(reinterpret_cast<uint8_t *>(&build_time_2_bits) + sizeof(oob_id_low), &oob_val_high,
                    sizeof(oob_val_high));
        f.cfg_buildings[BID].build_time_2 = build_time_2_bits;
    }

    const sim_view v = f.view();
    g_log.reset();
    detail::bldg_refund_resources_scaled_by_energy(v, recording_calls(), PLAYER, BLDG);

    ck_eq((uint32_t)g_log.events.size(), 7u,
          "seven full slots -> exactly seven refunds (the i<7 bound @0x0047951b/1f stops the walk, NOT "
          "a zero sentinel, and NOT an 8th call from the non-zero OOB id read at i==7)");
    ck_ev(0, PLAYER, 201, 4, "refund[0]: (player, id=201, trunc(8*0.5)=4)");
    ck_ev(1, PLAYER, 202, 8, "refund[1]: (player, id=202, trunc(16*0.5)=8)");
    ck_ev(2, PLAYER, 203, 12, "refund[2]: (player, id=203, trunc(24*0.5)=12)");
    ck_ev(3, PLAYER, 204, 16, "refund[3]: (player, id=204, trunc(32*0.5)=16)");
    ck_ev(4, PLAYER, 205, 20, "refund[4]: (player, id=205, trunc(40*0.5)=20)");
    ck_ev(5, PLAYER, 206, 24, "refund[5]: (player, id=206, trunc(48*0.5)=24)");
    ck_ev(6, PLAYER, 207, 28, "refund[6]: (player, id=207, trunc(56*0.5)=28)");
}

} // namespace

void run_bldg_refund_resources_scaled_by_energy_tests() {
    test_ratio_one_sanity_case();
    test_non_round_ratio_reused_every_iteration_FINDING();
    test_stop_at_zero_id_in_middle_of_array();
    test_truncation_toward_zero_negative_FINDING();
    test_zero_divisor_unguarded_yields_int_min_FINDING();
    test_seven_full_slots_stop_at_bound_not_oob_read_FINDING();
}

} // namespace mh::sim::test
