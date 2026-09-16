//
// sim_storage_release_door_held_by_unit_selftest.cpp -- `simtest` oracle for
// llm_strat_storage_release_door_held_by_unit @0x00485466 (sim/sim_storage_release_door_held_by_unit.h/.cpp,
// RI-SIM / SIM1D batch D).
//
// THIS IS THE ONLY EVIDENCE THIS FUNCTION HAS: a 15000-step all-AI soak (real combat, 8 players down to
// 7 by step 11700) made ZERO calls into it -- no sim/ TU in the migration set calls it yet, so its only
// caller is still-original code and shadow_region_closure.py cannot arm it for real (depth 4, bounded,
// 1 region, but no live call site to trigger). Written as primary proof, not a supplement.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_storage_release_door_held_by_unit.h):
//   0x00485483, 0x0048548a-0x00485490: for (slot = 1; slot < 0x19; ++slot) -- slots 1..24 INCLUSIVE.
//     Slot 0 of every player's storage array is deliberately never scanned.
//   0x004854aa-0x004854b1: unit_storage[player][slot].b_index != 0 (JZ skips both remaining tests).
//   0x004854c3-0x004854cc: unit_storage[player][slot].door_mutex_unit == unit_idx (JNZ skips the write).
//   0x004854de: on both guards passing, door_mutex_unit = 0. Nothing else written (b_index untouched).
//   No outward calls anywhere in the body besides the inert Watcom stack-capacity probe -- so there are
//   no callee mocks/recorders to wire up here, unlike the taxi/dock exemplar this file otherwise imitates
//   in shape (fx.reset() then seed then call then check, one braced block per case).
//   Return value is DETERMINISTICALLY ALWAYS 24 (0x18): the loop range is compile-time-fixed [1,25), it
//   always runs to completion (no early exit anywhere in the body), and EAX is unconditionally
//   overwritten every iteration by the loop tail (0x00485492, the CURRENT slot before increment) before
//   RET -- so the exported .c draft's "iVar1 = player" fallback path is provably dead code.
//
#include "sim/sim_storage_release_door_held_by_unit.h"

#include "sim_test_support.h"

namespace mh::sim::test {

// No anonymous-namespace recorders/`*_calls` struct here (unlike the taxi/dock exemplar this file
// otherwise imitates in shape): the function under test makes NO outward calls at all besides the inert
// Watcom stack-capacity probe -- see the header banner and sim_storage_release_door_held_by_unit.h's own
// "WHAT THIS DOES NOT TOUCH" note. There is nothing to mock.
using namespace mh::sim;

void run_storage_release_door_held_by_unit_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- slot 0 is NEVER scanned (loop counter inits to 1, 0x00485483): a slot-0 record that would
    // match on both guards must be left completely untouched.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 2;
        constexpr int32_t UNIT_IDX = 41;

        unit_storage &slot0   = fx.storage[PLAYER * STORAGE_PER_PLAYER + 0];
        slot0.b_index         = 7;        // would pass the b_index!=0 guard
        slot0.door_mutex_unit = UNIT_IDX; // would pass the door_mutex_unit==unit_idx guard

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)slot0.door_mutex_unit, (uint32_t)UNIT_IDX,
              "T1: slot 0 is never scanned -- loop counter starts at 1, 0x00485483/0x0048548a, "
              "door_mutex_unit left at its matching pre-call value");
    }

    // =================================================================================================
    // T2 -- slot 24 (0x18) IS scanned: it is the LAST value the bound check (CMP local_18,0x19 / JL,
    // 0x0048548a-0x0048548e) admits into the loop body.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 5;
        constexpr int32_t UNIT_IDX = 13;

        unit_storage &slot24   = fx.storage[PLAYER * STORAGE_PER_PLAYER + 24];
        slot24.b_index         = 3;
        slot24.door_mutex_unit = UNIT_IDX;

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)slot24.door_mutex_unit, 0u,
              "T2: slot 24 (0x18) IS the last loop-admitted slot and gets cleared, 0x0048548a-0x0048548e");
    }

    // =================================================================================================
    // T3 -- the bound is EXCLUSIVE at 25: slot index 25 for `player` is the SAME flat storage element as
    // slot 0 of `player+1` (storage_at(player, slot) = storage_[player*STORAGE_PER_PLAYER + slot]).
    // A loop that ran one iteration too far (<=0x19 instead of <0x19) would corrupt the next player's
    // row; this proves it does not.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER      = 2;
        constexpr int32_t NEXT_PLAYER = 3;
        constexpr int32_t UNIT_IDX    = 88;

        unit_storage &next_row_slot0   = fx.storage[NEXT_PLAYER * STORAGE_PER_PLAYER + 0];
        next_row_slot0.b_index         = 9;
        next_row_slot0.door_mutex_unit = UNIT_IDX;

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)next_row_slot0.door_mutex_unit, (uint32_t)UNIT_IDX,
              "T3: the upper bound is EXCLUSIVE at 25 -- player+1's slot 0 (the flat element one past "
              "player's slot 24) is untouched, 0x0048548a-0x0048548e");
    }

    // =================================================================================================
    // T4 -- b_index == 0 gate short-circuits BOTH remaining tests (JZ 0x004854b1): even a slot whose
    // door_mutex_unit already equals unit_idx must NOT be cleared while b_index is 0.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 1;
        constexpr int32_t SLOT     = 6;
        constexpr int32_t UNIT_IDX = 17;

        unit_storage &s   = fx.storage[PLAYER * STORAGE_PER_PLAYER + SLOT];
        s.b_index         = 0; // gate fails here
        s.door_mutex_unit = UNIT_IDX;

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)s.door_mutex_unit, (uint32_t)UNIT_IDX,
              "T4: b_index==0 skips both remaining tests, JZ 0x004854b1 -- door_mutex_unit left untouched "
              "even though it equals unit_idx");
    }

    // =================================================================================================
    // T5 -- b_index != 0 but door_mutex_unit != unit_idx (JNZ 0x004854cc): no write.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 4;
        constexpr int32_t SLOT     = 12;
        constexpr int32_t UNIT_IDX = 21;

        unit_storage &s   = fx.storage[PLAYER * STORAGE_PER_PLAYER + SLOT];
        s.b_index         = 5;
        s.door_mutex_unit = 999; // distinct from UNIT_IDX

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)s.door_mutex_unit, 999u,
              "T5: door_mutex_unit != unit_idx -- JNZ 0x004854cc skips the write, value left at 999");
    }

    // =================================================================================================
    // T6 -- both guards pass: door_mutex_unit cleared to 0 (0x004854de) and NOTHING else on the record
    // is touched -- b_index (the field the write's own address literal does NOT target, 0xc72894 vs
    // 0xc727c0) must survive with its pre-call value.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 6;
        constexpr int32_t SLOT     = 18;
        constexpr int32_t UNIT_IDX = 30;
        constexpr int32_t B_INDEX  = 123; // distinct sentinel, must survive

        unit_storage &s   = fx.storage[PLAYER * STORAGE_PER_PLAYER + SLOT];
        s.b_index         = B_INDEX;
        s.door_mutex_unit = UNIT_IDX;

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)s.door_mutex_unit, 0u,
              "T6: matching slot's door_mutex_unit is cleared to 0, 0x004854de");
        ck_eq((uint32_t)s.b_index, (uint32_t)B_INDEX,
              "T6: b_index (offset 0x0, a DIFFERENT field from the write's 0xd4 target) is untouched");
    }

    // =================================================================================================
    // T7 -- multiple matching slots for the SAME player all get cleared: the loop does not stop after
    // the first match (no early exit anywhere in the body per the header's return-value derivation).
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 4;
        constexpr int32_t UNIT_IDX = 55;

        unit_storage &s_lo    = fx.storage[PLAYER * STORAGE_PER_PLAYER + 3];
        unit_storage &s_mid   = fx.storage[PLAYER * STORAGE_PER_PLAYER + 10];
        unit_storage &s_hi    = fx.storage[PLAYER * STORAGE_PER_PLAYER + 24];
        s_lo.b_index          = 9;
        s_lo.door_mutex_unit  = UNIT_IDX;
        s_mid.b_index         = 3;
        s_mid.door_mutex_unit = UNIT_IDX;
        s_hi.b_index          = 1;
        s_hi.door_mutex_unit  = UNIT_IDX;

        sim_store own = fx.store();
        int32_t   ret = detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)s_lo.door_mutex_unit, 0u, "T7: slot 3 cleared -- no early exit on the first match");
        ck_eq((uint32_t)s_mid.door_mutex_unit, 0u, "T7: slot 10 also cleared -- loop kept scanning");
        ck_eq((uint32_t)s_hi.door_mutex_unit, 0u, "T7: slot 24 also cleared -- loop ran to its true end");
        ck_eq((uint32_t)ret, 24u, "T7: return value is 24 even when matches occurred mid-loop, 0x00485492");
    }

    // =================================================================================================
    // T8 -- player isolation: the SAME slot index and the SAME unit_idx in a DIFFERENT player's row must
    // NOT be touched -- pins the player*0x17d4 + slot*0xf4 row/elem addressing (0x0048549a-0x004854a8)
    // against a translation that dropped or misapplied the player stride.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t TARGET_PLAYER = 0;
        constexpr int32_t OTHER_PLAYER  = 7;
        constexpr int32_t SLOT          = 8;
        constexpr int32_t UNIT_IDX      = 66;

        unit_storage &other   = fx.storage[OTHER_PLAYER * STORAGE_PER_PLAYER + SLOT];
        other.b_index         = 4;
        other.door_mutex_unit = UNIT_IDX;

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, TARGET_PLAYER, UNIT_IDX);

        ck_eq((uint32_t)other.door_mutex_unit, (uint32_t)UNIT_IDX,
              "T8: a different player's same-slot/same-unit_idx record is untouched -- row stride "
              "0x17d4, 0x0048549a");
    }

    // =================================================================================================
    // T9 -- the PRIMARY load-bearing claim: return value is deterministically 24 for EVERY (player,
    // unit_idx), never `player`. Chosen player (2) is non-zero and NOT 24, so if the exported .c draft's
    // dead "iVar1 = player" fallback were actually live, this would observe 2 instead of 24.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 2;
        constexpr int32_t UNIT_IDX = 77;
        // No slot in this player's row matches -- the loop makes zero writes, isolating the return path.

        sim_store own = fx.store();
        int32_t   ret = detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)ret, 24u,
              "T9: return is 24 (last slot visited), NOT `player`==2 -- the draft's 'iVar1=player' path "
              "is dead, 0x00485492/0x004854ea");
    }

    // =================================================================================================
    // T10 -- full 32-bit SIGNED equality, not a truncated/unsigned compare: a negative door_mutex_unit
    // sentinel (-1) matched against a negative unit_idx (-1) must still clear -- pins the CMP
    // EAX,dword ptr[EBP-0x18] at 0x004854c9 as an ordinary 32-bit compare.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t PLAYER   = 3;
        constexpr int32_t SLOT     = 15;
        constexpr int32_t UNIT_IDX = -1;

        unit_storage &s   = fx.storage[PLAYER * STORAGE_PER_PLAYER + SLOT];
        s.b_index         = 2;
        s.door_mutex_unit = -1;

        sim_store own = fx.store();
        detail::storage_release_door_held_by_unit(own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)s.door_mutex_unit, 0u,
              "T10: door_mutex_unit==-1 matched against unit_idx==-1 clears -- full 32-bit signed "
              "compare, 0x004854c9");
    }
}

} // namespace mh::sim::test
