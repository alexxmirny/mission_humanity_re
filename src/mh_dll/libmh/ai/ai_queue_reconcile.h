//
// ai/ai_queue_reconcile.h -- reconcile the AI build queue after one of its buildings changed
// (RI-AI / AI1B layer 1).
//
// llm_strat_ai_queue_reconcile_bldg_change @0x004e2d91. Its only caller is
// llm_strat_ai_notify_object_removed @0x004db7ff, on the branch where a building of this player is
// going away, it is NOT that player's ai_turret_candidate, and the removal is SOFT
// (`hard_remove == 0`). So the whole function reads as: "one of my buildings just went; settle
// whatever the queue still thinks about it, and put it back in the build plan."
//
// Three phases, and the middle one can end the function early:
//   1. kill any pending REPAIR or UPGRADE entry for that building (status |= 0xc0),
//   2. if a COMMITTED CONSTRUCTION entry (status == 0x81) for it exists, un-commit it back to 0x01
//      and RETURN -- the plan already covers rebuilding it,
//   3. otherwise queue a fresh construction at the building's own tile, provided its type costs
//      anything at all.
//
#pragma once
#include <cstdint>

#include "ai/ai_queue_release.h" // QUEUE_STATUS_REMOVED -- one definition, not two
#include "ai/ai_state.h"

namespace mh::ai {

// The status byte packs a kind NIBBLE with three flag bits (field comment in
// addr/mh_structs.gen.h). This function is not consistent about which it tests, and the
// inconsistency is the original's, so it is spelled out rather than smoothed:
//   phase 1 masks to the NIBBLE (AND BL,0xf @0x004e2db6 and @0x004e2df0) -- so it matches a repair
//           or upgrade entry whatever its flag bits, committed or not;
//   phase 2 compares the WHOLE BYTE against 0x81 (CMP byte ptr ..,0x81 @0x004e2e28) -- so only an
//           exactly-committed construction entry with no other bit set matches.
// Its sibling llm_strat_ai_queue_release_order uses whole-byte tests in both places.
inline constexpr uint8_t QUEUE_KIND_MASK         = 0x0f;
inline constexpr uint8_t QUEUE_KIND_CONSTRUCTION = 1;
inline constexpr uint8_t QUEUE_KIND_REPAIR       = 3;
inline constexpr uint8_t QUEUE_KIND_UPGRADE      = 4;
// "finished/committed, skipped by queue_process".
inline constexpr uint8_t QUEUE_STATUS_COMMITTED = 0x80;
// What phase 1 stamps: committed AND removed at once, in one OR (0x004e2dc6).
inline constexpr uint8_t QUEUE_STATUS_SETTLED = QUEUE_STATUS_COMMITTED | QUEUE_STATUS_REMOVED;
// What phase 2 matches, and what it writes back: a committed construction entry becomes a plain
// live one (MOV byte ptr ..,0x1 @0x004e2e39 -- an ASSIGNMENT, not an AND-NOT, so every other flag
// bit is cleared with it).
inline constexpr uint8_t QUEUE_STATUS_COMMITTED_CONSTRUCTION =
    QUEUE_STATUS_COMMITTED | QUEUE_KIND_CONSTRUCTION;
inline constexpr uint8_t QUEUE_STATUS_LIVE_CONSTRUCTION = QUEUE_KIND_CONSTRUCTION;

namespace detail {

// How a call ended. Instrumentation, not behaviour -- the original returns void. It exists because
// a call count is not coverage here: on a queue that is empty (or holds nothing about this
// building) both scans walk zero entries and the whole call is the flush plus the tail.
enum class reconcile_outcome {
    revived,   // phase 2 matched: a committed construction entry went 0x81 -> 0x01, early return
    requeued,  // phase 3: the type has a non-zero total cost, so construction was queued afresh
    cost_zero, // phase 3: llm_strat_bldg_total_resource_cost returned 0, nothing was queued
};

struct reconcile_report {
    reconcile_outcome outcome = reconcile_outcome::cost_zero;
    bool              stamped = false; // phase 1 settled a repair/upgrade entry
    int32_t           scanned = 0;     // ai_bldg_queue_count as phase 1 saw it
    int32_t           cost    = 0;     // what total_resource_cost returned (0 unless phase 3 ran)
    int32_t           slot    = -1;    // the entry phase 1 or phase 2 matched, -1 for none
};

// The logic over an EXPLICIT state + call set, so `net_selftest.exe aitest` can drive it over heap
// buffers with recording stubs and no game.
reconcile_report queue_reconcile_bldg_change(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, uint32_t player,
                                             uint32_t building_index);

} // namespace detail

void queue_reconcile_bldg_change(uint32_t player, uint32_t building_index);

} // namespace mh::ai
