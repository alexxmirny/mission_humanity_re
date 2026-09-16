//
// ai_queue_remove_selftest.cpp -- `aitest` cases for llm_strat_ai_queue_remove_at @0x004e5da2
// (RI-AI batch C / AI1C, closed at T3).
//
// WHY THIS FILE EXISTS. The row was closed on reimpl-verify + lint alone: its shadow site
// (shadow_ai_c*) was ARMED and reached ZERO times over 65000 combined soak steps, because its only
// caller path -- an AI build-queue cancel/remove-by-index out of llm_strat_ai_scan_bldg_repair_
// upgrade -- never fired from an all-AI default start, and its own ledger row says in as many words
// that the offline oracle had no case for it either. AI1's done_when requires every T3 to carry its
// written reason PLUS a compensating test; this is that test, written at AI1 close (2026-08-30).
// The reason is unchanged and still true -- the zero-call site is not evidence and is not counted
// as any.
//
// WHAT IT PINS: the left-compact shift (`for (i = slot; i < count - 1; ++i) q[i] = q[i+1]`) moving
// the FULL 0x12-byte entry rather than a prefix of it; the freed tail slot left ABANDONED, not
// cleared; the unconditional count decrement, which happens on every path including the one where
// the loop body never runs; and the UNSIGNED `i < (uint32_t)(count - 1)` bound, which the original
// guards with nothing at all.
//
#include "ai_test_support.h"

#include "ai/ai_queue_remove.h"

namespace mh::ai::test {
namespace {

using namespace mh::ai;

// Seed one queue entry with values that are distinct ACROSS FIELDS and across entries, so a copy
// that moved the right number of bytes from the wrong entry -- or the wrong number of bytes from
// the right one -- changes an assertion. `tag` separates the entries; the per-field spreads
// separate the fields.
void seed_entry(player_data &pd, int slot, int tag) {
    auto &e                = pd.ai_bldg_queue[slot];
    e.status               = (uint8_t)(0x80 + tag);
    e.tick_or_unit_id      = (uint8_t)(0x10 + tag);
    e.build_tile_x         = (int16_t)(1000 + tag);
    e.resource_reserved[0] = (int16_t)(2000 + tag);
    e.resource_reserved[1] = (int16_t)(2100 + tag);
    e.resource_reserved[2] = (int16_t)(2200 + tag);
    e.resource_reserved[3] = (int16_t)(2300 + tag);
    e.resource_reserved[4] = (int16_t)(2400 + tag);
    e.building_index       = 30000 + tag;
}

// True when `slot` holds exactly the entry seeded with `tag` -- ALL nine fields, i.e. the whole
// 0x12 bytes. A partial copy (the MOVSD.REP-x4 without the trailing MOVSW, which is precisely what
// Ghidra's decompile mis-renders) leaves building_index behind and fails here.
bool entry_is(const player_data &pd, int slot, int tag) {
    const auto &e = pd.ai_bldg_queue[slot];
    return e.status == (uint8_t)(0x80 + tag) && e.tick_or_unit_id == (uint8_t)(0x10 + tag) &&
           e.build_tile_x == (int16_t)(1000 + tag) &&
           e.resource_reserved[0] == (int16_t)(2000 + tag) &&
           e.resource_reserved[1] == (int16_t)(2100 + tag) &&
           e.resource_reserved[2] == (int16_t)(2200 + tag) &&
           e.resource_reserved[3] == (int16_t)(2300 + tag) &&
           e.resource_reserved[4] == (int16_t)(2400 + tag) && e.building_index == 30000 + tag;
}

} // namespace

void run_queue_remove_tests() {
    printf("-- ai_queue_remove (llm_strat_ai_queue_remove_at) --\n");

    const int32_t P = 4; // not 0/1: separates a player-row bug from a slot-index bug

    // ---- Q1: remove from the MIDDLE -- every entry above slot_index shifts down by one, whole
    // struct at a time, and the count drops by one. --------------------------------------------
    {
        fixture      f;
        player_data &pd        = f.players[P];
        pd.ai_bldg_queue_count = 4;
        for (int i = 0; i < 4; ++i) seed_entry(pd, i, i + 1); // tags 1,2,3,4

        detail::queue_remove_at(f.store(), P, 1u);

        ck(pd.ai_bldg_queue_count == 3, "Q1: the count decrements unconditionally, DEC @0x004e5df9");
        ck(entry_is(pd, 0, 1), "Q1: entries BELOW the removed slot are untouched");
        ck(entry_is(pd, 1, 3) && entry_is(pd, 2, 4),
           "Q1: entries above shift down one slot, whole 0x12-byte entry each (MOVSD.REP x4 + MOVSW)");
        ck(entry_is(pd, 3, 4),
           "Q1: the vacated TAIL slot keeps its stale copy -- the original never clears it");
    }

    // ---- Q2: remove the LAST live entry -- slot_index == count - 1, so `i < count - 1` is false
    // on the first test and the loop body never runs. Only the count moves. --------------------
    {
        fixture      f;
        player_data &pd        = f.players[P];
        pd.ai_bldg_queue_count = 3;
        for (int i = 0; i < 3; ++i) seed_entry(pd, i, i + 1);

        detail::queue_remove_at(f.store(), P, 2u);

        ck(pd.ai_bldg_queue_count == 2, "Q2: removing the last entry still decrements the count");
        ck(entry_is(pd, 0, 1) && entry_is(pd, 1, 2) && entry_is(pd, 2, 3),
           "Q2: no entry moves -- the shift loop's first comparison already fails");
    }

    // ---- Q3: the single-entry queue empties. `count - 1` is 0 and slot 0 is not < 0. -----------
    {
        fixture      f;
        player_data &pd        = f.players[P];
        pd.ai_bldg_queue_count = 1;
        seed_entry(pd, 0, 9);

        detail::queue_remove_at(f.store(), P, 0u);

        ck(pd.ai_bldg_queue_count == 0, "Q3: the queue empties");
        ck(entry_is(pd, 0, 9), "Q3: the emptied slot is abandoned, not cleared");
    }

    // ---- Q4: an OUT-OF-RANGE slot_index above the live region -- the original has no guard, and
    // the unsigned bound simply fails immediately. It still decrements. This is the documented
    // no-guard behaviour, reproduced rather than hardened; the test pins that it is a NO-OP shift
    // rather than a wild one. -------------------------------------------------------------------
    {
        fixture      f;
        player_data &pd        = f.players[P];
        pd.ai_bldg_queue_count = 3;
        for (int i = 0; i < 5; ++i) seed_entry(pd, i, i + 1);

        detail::queue_remove_at(f.store(), P, 5u);

        ck(pd.ai_bldg_queue_count == 2,
           "Q4: an out-of-range slot still decrements -- the original guards nothing");
        ck(entry_is(pd, 0, 1) && entry_is(pd, 1, 2) && entry_is(pd, 2, 3) && entry_is(pd, 3, 4) &&
               entry_is(pd, 4, 5),
           "Q4: 5 < (uint32_t)(3 - 1) is false, so nothing shifts");
    }

    // ---- Q5: the shift is driven by the count as it stands ON ENTRY, and reaches the top of the
    // real 64-entry array -- the compile-time cap every enqueue site guards. --------------------
    {
        fixture      f;
        player_data &pd        = f.players[P];
        pd.ai_bldg_queue_count = 64; // the cap
        seed_entry(pd, 62, 62);
        seed_entry(pd, 63, 63);

        detail::queue_remove_at(f.store(), P, 62u);

        ck(pd.ai_bldg_queue_count == 63, "Q5: decrements from the 0x40 cap");
        ck(entry_is(pd, 62, 63),
           "Q5: the top entry shifts down into the removed slot at the array's last index");
    }

    // ---- Q6: the player row is selected by `player`, not by a hardcoded slot. -----------------
    {
        fixture f;
        f.players[P].ai_bldg_queue_count = 3;
        f.players[0].ai_bldg_queue_count = 3;
        for (int i = 0; i < 3; ++i) seed_entry(f.players[0], i, i + 1);

        detail::queue_remove_at(f.store(), P, 0u);

        ck(f.players[0].ai_bldg_queue_count == 3 && entry_is(f.players[0], 1, 2),
           "Q6: another player's queue is untouched");
    }
}

} // namespace mh::ai::test
