//
// ai/ai_queue_release.h -- the AI build-queue RELEASE hook (RI-AI / AI1B, antichain layer 0).
//
// One function. It is the hook the SIM calls when a building's repair or upgrade job ends -- both
// its call sites are in llm_strat_bldg_state_charge_gate / _upgrading, i.e. outside the AI cluster
// (callers_in_ai == 0 in tools/data/ai_migration.json) -- and it settles that job's entry in the
// player's AI build queue. It is the AI's own bookkeeping, so it is skipped entirely for a
// non-AI player.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The status byte values this function matches on. `status` packs a kind nibble with three flag
// bits (see the field comment in addr/mh_structs.gen.h): 0x80 = finished/committed, low nibble 3 =
// repair, 4 = upgrade. So 0x83 / 0x84 are the COMMITTED repair / upgrade forms, and the original
// compares the whole byte -- not the nibble -- so an entry carrying any other bit does not match.
inline constexpr uint8_t QUEUE_STATUS_COMMITTED_REPAIR  = 0x83;
inline constexpr uint8_t QUEUE_STATUS_COMMITTED_UPGRADE = 0x84;
// The bit both non-zero modes set: "entry removed, compacted out by the queue_process prologue".
inline constexpr uint8_t QUEUE_STATUS_REMOVED = 0x40;
// resource_reserved[] / resource_spent[] are indexed by RESOURCE ID, and only ids 1..4 are ever
// costs -- slot [0] is reused as a cached build-tile Y by the construction path. The original
// refunds exactly [1..4], four unrolled subtractions.
//
// RESOURCE_ID_FIRST / RESOURCE_ID_LAST were declared HERE until 2026-08-02, when the storage/mine
// planners turned out to walk the same 1..4 range over two entirely different arrays
// (player_resources and llm_strat_storage_stats::cap_prev). They now live in ai_state.h beside
// RESOURCE_SLOTS_PER_PLAYER, with the same values -- one range, one declaration.

// WHICH BRANCH A CALL TOOK. This is instrumentation, not behaviour -- the original returns nothing
// and the production wrapper discards it. It exists because a call COUNT is not coverage for this
// function: the master gate rejects every non-AI player and the scan usually finds nothing, so a
// site reporting "40000 calls, 0 divergences" can be entirely gated-off calls that never read a
// queue entry. The shadow arm aggregates these, so the run's own log says how many calls did work.
// It also lets `aitest` assert the branch taken rather than inferring it from the side effect.
enum class release_outcome {
    not_ai,          // players[player].ai_enabled == 0 -- the master gate rejected the call
    bad_mode,        // mode was not 0, 1 or 2 (unsigned), including any negative
    no_match,        // scanned the whole queue and nothing matched
    refunded,        // mode 0, committed REPAIR entry: tick decremented (if non-zero) and cost refunded
    upgrade_blocked, // mode 0, committed UPGRADE entry matched first: scan ends, NO effect
    stamped_removed, // mode 1 or 2: the matching entry got status |= 0x40
};

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_queue_release_order @0x004dbf36.
//
// No-op unless players[player].ai_enabled. Otherwise scans ai_bldg_queue[0 .. ai_bldg_queue_count)
// for the first entry whose building_index equals `building_index` and whose status matches, and
// acts per `mode`:
//
//   mode 0 -- the job COMPLETED. On a committed REPAIR entry (0x83): decrement its tick counter if
//             it is non-zero, then refund the entry's reserved cost out of the player's
//             resource_spent[1..4]. A committed UPGRADE entry (0x84) matching first instead ENDS
//             the scan with no effect at all -- an early-out, not a fall-through.
//   mode 1 -- CANCEL a repair: flag the matching committed REPAIR entry removed (status |= 0x40).
//   mode 2 -- CANCEL an upgrade: same, on the matching committed UPGRADE entry.
//   anything else -- returns immediately. The dispatch is UNSIGNED, so a negative mode also returns.
//
// The scan stops at the first match in every mode; a second matching entry is never reached.
release_outcome queue_release_order(const ai_view &v, const ai_store &own, int32_t player,
                                    int32_t building_index, int32_t mode);

} // namespace detail

void queue_release_order(int32_t player, int32_t building_index, int32_t mode);

} // namespace mh::ai
