//
// sim_unit_select_weapon_selftest.cpp -- `simtest` oracle for llm_strat_unit_select_weapon
// @0x0048ba9e (sim/sim_unit_select_weapon.h/.cpp).
//
// arm_ready:false -- this row was armed on its return value and made ZERO calls in a 15000-step
// all-AI soak (see the tracker ledger), so this offline oracle is the ONLY evidence this function
// will ever get. Written as the primary proof, not a supplement.
//
// THE SINGLE MOST IMPORTANT THING THIS FILE PINS: the return value is a FIXED STATUS BYTE (4 on a
// match, 0x64 on exhaustion), NOT the matched weapon slot index. The Ghidra .c draft's
// `return (byte)(unkfloat1)(float)CONCAT31((int3)(uVar2 >> 8),local_14);` is decompiler garbage
// reassembled from a stale register; reimpl-verify caught it and the real tail
// (0x0048bb4c/0x0048bb5d, both literal immediates) is what this file's translation
// (sim/sim_unit_select_weapon.cpp) implements. Every FOUND case below asserts == 4, never the slot
// position the match happened at, so a "helpful" restoration of the slot-index reading fails loudly.
//
// EXPECTED BEHAVIOUR from the asm (tmp/decomp_sim/llm_strat_unit_select_weapon_0048ba9e.asm):
//   Per iteration (LAB_0048bac1 top of loop): weapon_id==0 -> exit to NOT_FOUND (0x0048bad9/bae4).
//   Otherwise slot>=4 -> exit to NOT_FOUND too (0x0048bae6/baea, falls into LAB_0048baec). Only when
//   BOTH weapon_id!=0 AND slot<4 does the body run (LAB_0048baee).
//   Body: enabled_2==0 -> skip to next slot, no target test at all (0x0048bb0a/bb11).
//   enabled_2!=0 -> read weapon_id again, look up cfg_weapons[weapon_id].target (0x0048bb2f-bb40),
//   TEST target_mask & target (0x0048bb47); ==0 -> skip to next slot; !=0 -> status=4, return
//   immediately (0x0048bb4c/bb50) -- first match wins, no further slots examined.
//   Loop exhausted (either sentinel) -> status=0x64 (0x0048bb5d), then RET AL (0x0048bb61-6b).
//
#include "sim/sim_unit_select_weapon.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER     = 2;
constexpr int32_t  UNIT_INDEX = 5;

// Fills one weapon slot's instance fields (unit::weapons[slot]) and its cfg-side target mask
// (cfg_weapons[weapon_id].target) in one call, so each test case reads as a slot-by-slot table
// rather than four separate statements per slot.
void set_slot(sim_fixture &fx, unit &u, int32_t slot, uint8_t weapon_id, uint8_t enabled_2,
              uint8_t cfg_target) {
    u.weapons[slot].weapon_id        = weapon_id;
    u.weapons[slot].enabled_2        = enabled_2;
    fx.cfg_weapons[weapon_id].target = cfg_target;
}

} // namespace

void run_unit_select_weapon_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- a single matching slot (slot 0): the FOUND status is the FIXED literal 4, NOT the matched
    // slot's index (which here is 0 -- a slot-index-returning mutant would fail this exact check).
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/7, /*enabled_2=*/1, /*cfg_target=*/0x02);

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0x02u);

        ck_eq(result, 4u,
              "T1: match at slot 0 returns the FIXED status 4, NOT the slot index 0, 0x0048bb4c");
    }

    // =================================================================================================
    // T2 -- all 4 slots enabled with distinct weapon ids, none of their targets intersect
    // target_mask: full-scan exhaustion returns the FIXED literal 0x64 (100), not 4 and not the slot
    // count (4) either.
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/2, 1, 0x01);
        set_slot(fx, u, 1, /*weapon_id=*/3, 1, 0x02);
        set_slot(fx, u, 2, /*weapon_id=*/5, 1, 0x04);
        set_slot(fx, u, 3, /*weapon_id=*/11, 1, 0x08);

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0x10u);

        ck_eq(result, 0x64u,
              "T2: all 4 slots enabled but none match target_mask -- exhaustion returns the FIXED "
              "status 0x64 (100), not 4 and not the slot count, 0x0048bb5d");
    }

    // =================================================================================================
    // T3 -- weapon_id==0 at slot 0 is the loop's own sentinel: exits to NOT_FOUND immediately, BEFORE
    // even the slot<4 check, and without ever consulting enabled_2 or target_mask (both set here to
    // values that WOULD match if the id==0 check were skipped).
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/0, /*enabled_2=*/1, /*cfg_target=*/0xff);

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0xffu);

        ck_eq(result, 0x64u,
              "T3: weapon_id==0 at slot 0 exits immediately to NOT_FOUND (0x64) -- the id==0 check "
              "gates BEFORE enabled_2/target are ever read, 0x0048bad9/0x0048bae4");
    }

    // =================================================================================================
    // T4 -- FIRST match wins / early exit, proven by observable difference: slot 0 matches and MUST
    // return immediately. slot 1 is the id==0 sentinel -- if a mutant kept scanning after a match
    // instead of returning right away, it would reach slot 1's sentinel and come back NOT_FOUND
    // (0x64) instead of the correct FOUND (4).
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/13, /*enabled_2=*/1, /*cfg_target=*/0x04);
        set_slot(fx, u, 1, /*weapon_id=*/0, /*enabled_2=*/1, /*cfg_target=*/0x04); // sentinel

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0x04u);

        ck_eq(result, 4u,
              "T4: slot 0 matches and returns FOUND (4) immediately -- proven by slot 1 being the "
              "id==0 sentinel, which would flip the result to 0x64 if the scan continued past the "
              "match, 0x0048bb4c/0x0048bb50");
    }

    // =================================================================================================
    // T5 -- the enabled_2 gate: slot 0 is DISABLED (enabled_2==0) even though its target WOULD match
    // target_mask; it must be skipped without consulting target at all. slot 1 is enabled but its
    // target does NOT overlap the mask, so the scan continues to slot 2 (weapon_id==0, untouched by
    // the fixture) and exhausts. If enabled_2 were ignored, slot 0's target would satisfy the mask
    // and this would wrongly return FOUND (4) instead of the correct NOT_FOUND (0x64).
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/17, /*enabled_2=*/0, /*cfg_target=*/0x08); // disabled
        set_slot(fx, u, 1, /*weapon_id=*/19, /*enabled_2=*/1, /*cfg_target=*/0x10); // no mask overlap

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0x08u);

        ck_eq(result, 0x64u,
              "T5: disabled slot 0 (enabled_2==0) is never consulted despite a target that would "
              "match -- correctly skipped, scan exhausts to 0x64, enabled_2 gate at 0x0048bb0a");
    }

    // =================================================================================================
    // T6 -- the target_mask & target test itself: slot 0 is enabled with a target that does NOT
    // intersect target_mask (must be skipped, not treated as a stop condition), slot 1 is enabled
    // with a target that DOES intersect -- scan continues past the non-match to find it. If a mutant
    // stopped the scan on the first enabled-but-non-matching slot, this would wrongly return
    // NOT_FOUND (0x64) instead of the correct FOUND (4).
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/23, /*enabled_2=*/1, /*cfg_target=*/0x20); // no overlap
        set_slot(fx, u, 1, /*weapon_id=*/29, /*enabled_2=*/1, /*cfg_target=*/0x40); // overlaps

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0x40u);

        ck_eq(result, 4u,
              "T6: an enabled slot whose target misses target_mask is skipped (not a stop "
              "condition) -- scan continues to slot 1's match, FOUND (4), TEST at 0x0048bb47");
    }

    // =================================================================================================
    // T7 -- the slot<4 boundary from the far side: slots 0-2 are enabled but non-matching, and the
    // match sits at slot 3 -- the LAST valid index (UNIT_WEAPON_SLOTS==4). Proves the scan reaches
    // and tests slot index 3 (does not stop one short at slot==3 mistaken for the exit bound).
    // =================================================================================================
    {
        fx.reset();
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        set_slot(fx, u, 0, /*weapon_id=*/2, 1, 0x01);
        set_slot(fx, u, 1, /*weapon_id=*/3, 1, 0x02);
        set_slot(fx, u, 2, /*weapon_id=*/5, 1, 0x04);
        set_slot(fx, u, 3, /*weapon_id=*/31, 1, 0x08); // last valid slot index

        const uint8_t result = detail::unit_select_weapon(fx.view(), PLAYER, UNIT_INDEX, 0x08u);

        ck_eq(result, 4u,
              "T7: a match at slot 3 (the LAST valid index, slot<4 boundary) is still found -- "
              "FOUND (4), slot<4 check at 0x0048bae6/0x0048baea");
    }
}

} // namespace mh::sim::test
