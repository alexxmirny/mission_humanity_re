//
// ai/ai_bldg_queue.h -- compact the AI build queue, then dispatch it (RI-AI / AI1B layer 2).
//
// llm_strat_ai_bldg_queue_process @0x004e82ab, `void __watcall f(uint player_id)`, player in EAX.
// The pump the AI's whole construction/training economy runs through, and it is two phases over
// player_data[player].ai_bldg_queue.
//
// PHASE 1 -- COMPACTION (0x004e82c1-0x004e8377). Walk i upward; while entry i carries
// QUEUE_STATUS_REMOVED (0x40), shift every later entry down one slot (an 18-byte copy, spelled
// REP MOVSD x4 + MOVSW) and decrement ai_bldg_queue_count. THE SLOT IS RE-TESTED AFTER EACH
// REMOVAL: the `DEC count` at 0x004e8312 falls through into the loop head at 0x004e8318 with i
// unchanged, so a run of consecutive removed entries is fully drained before i advances. (Session
// 0907's handoff read this the other way -- "i is not rewound, so the entry that slid in is
// skipped". It is rewound, by fallthrough rather than by a jump, which is why it is easy to miss.)
//
// PHASE 2 -- DISPATCH (0x004e8379-0x004e8520). For each surviving entry, in order:
//   * skip it entirely if QUEUE_STATUS_COMMITTED (0x80) is set;
//   * AFFORDABILITY: for resource ids 1..4, if the entry's reserved cost EXCEEDS the player's
//     resource_spent for that id, the entry is unaffordable. The reserved cost is a short read
//     ZERO-extended (MOVZX @0x004e83c6) and then compared SIGNED against the int (JLE @0x004e83d8);
//   * QUEUE_STATUS_AFFORD_WAIVED (0x20) forces affordable regardless (0x004e8404) -- and it is
//     applied AFTER the loop, so it overrides a failure rather than skipping the test;
//   * if UNAFFORDABLE: for each of the same four ids where the cost exceeds the spend AND
//     ai_mine_yield_by_resource for that id is zero, ask the order layer to grant the shortfall --
//     then RETURN FROM THE WHOLE FUNCTION. The return is not an early-out written as such: the
//     path falls into a five-entry jump table at 0x004e8283 whose slots ALL hold 0x004e792e, the
// function's shared Watcom epilogue (read out of the image, both tables). So no
//     entry after an unaffordable one is looked at on this tick.
//   * if AFFORDABLE: dispatch on the status LOW NIBBLE through the table at 0x004e8297 --
//         0 -> handle_recruit_state   1 -> process_entry   2 -> handle_state2_empty (a no-op)
//         3 -> handle_upgrade_or_cancel   4 -> handle_upgrade_or_cancel  (SLOTS 3 AND 4 ARE THE
//         SAME TARGET, 0x004e84f0 twice -- not a transcription slip)
//     and a nibble above 4 is skipped (JA @0x004e84bc).
//
// THE COUNT IS RE-READ FROM MEMORY AT EVERY LOOP HEAD, in both phases, and that is load-bearing in
// phase 2: three of the four handlers write player_data and one of them (process_entry) can change
// the queue under the loop. A translation that hoisted the count into a local would keep walking
// slots the game has since dropped.
//
// WHY THIS SITE CAN BE SHADOWED AT ALL, since its handlers issue real game orders. Measured rather
// than assumed (2026-08-02): the transitive callee closure is 34 functions, and its whole write set
// is EIGHT regions -- player_data, the three order-container regions (queue, count, scratch args),
// the two site-candidate regions, the grid wrap mask and the building-enclosure scratch. All eight
// are declared as the site's regions, so the restore between the two arms un-issues the duplicate
// orders and nothing needs stubbing. Nothing in the closure reaches _G_LLM_STRAT_RNG_STATE, which
// is the one shared side channel that could not have been handled by declaring a region. The
// residual risk is the state matrix's known blindness to base+displacement accesses
// -- an undeclared writer would corrupt the comparison quietly rather than fail.
//
#pragma once
#include <cstdint>

#include "ai/ai_queue_reconcile.h" // QUEUE_KIND_MASK / QUEUE_STATUS_COMMITTED -- one definition
#include "ai/ai_queue_release.h"   // QUEUE_STATUS_REMOVED -- likewise
#include "ai/ai_state.h"

namespace mh::ai {

// The one status bit no sibling module had a name for yet; the other three this function tests
// (0x0f, 0x40, 0x80) come from the two headers above rather than being respelled here, since a
// second definition of a wire-format constant is a place for the two to drift apart.
//   QUEUE_KIND_MASK         0x0f  0x004e847f / 0x004e84b8
//   QUEUE_STATUS_REMOVED    0x40  0x004e833f
//   QUEUE_STATUS_COMMITTED  0x80  0x004e8388
inline constexpr uint8_t QUEUE_STATUS_AFFORD_WAIVED = 0x20; // 0x004e8404
// The highest dispatchable nibble; above it the entry is skipped (CMP AL,0x4 / JA).
inline constexpr uint8_t QUEUE_KIND_MAX = 4;
// The four resource ids an entry can reserve a cost for. The loops run j = 0..3 and index
// resource_reserved[1 + j] / resource_spent[1 + j] / ai_mine_yield_by_resource[1 + j]; id 0 is
// never a cost (see the resource_reserved field comment).
inline constexpr int32_t QUEUE_COST_RESOURCE_COUNT = 4;

namespace detail {

struct bldg_queue_report {
    int32_t compacted     = 0;     // entries phase 1 removed
    int32_t scanned       = 0;     // entries phase 2 looked at (before any early return)
    int32_t skipped_done  = 0;     // skipped for QUEUE_STATUS_COMMITTED
    int32_t dispatched    = 0;     // entries that reached the jump table
    int32_t waived        = 0;     // entries the 0x20 bit rescued from an affordability failure
    int32_t granted       = 0;     // order_grant_resource_raw calls made
    bool    stopped_short = false; // true == returned on the unaffordable path
    // One counter per dispatch arm, indexed by nibble. [4] is real: nibble 4 shares nibble 3's
    // handler but they are different entries, and collapsing them here would hide the fact that
    // slot 4 was ever taken.
    int32_t by_kind[QUEUE_KIND_MAX + 1] = {};
};

// The logic over an EXPLICIT state and an INJECTED call set, so `net_selftest.exe aitest` can drive
// it over heap buffers with recording stubs and no game.
bldg_queue_report bldg_queue_process(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     uint32_t player);

} // namespace detail

void bldg_queue_process(uint32_t player);

} // namespace mh::ai
