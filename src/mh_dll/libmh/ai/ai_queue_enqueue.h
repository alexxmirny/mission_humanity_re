//
// ai/ai_queue_enqueue.h -- the three APPEND-one-entry sides of the AI build queue
// (RI-AI / AI1B, antichain layer 3).
//
//   llm_strat_ai_queue_train_unit   @0x004e268c -- kind 0, a unit to train
//   llm_strat_ai_queue_bldg_repair  @0x004e284d -- kind 3, repair a roster building
//   llm_strat_ai_queue_bldg_upgrade @0x004e2a2e -- kind 4, upgrade a roster building
//
// One translation unit because they are the same function three times: guard
// `ai_bldg_queue_count < 0x40`, stamp the new tail entry, walk a cfg cost list into
// `resource_reserved[]`, bump the count, return 0 -- or return 1 without touching anything if the
// queue is full. Reading them side by side is also the only way to see that the three DIFFER in
// four places that look like they should not, all preserved below and each carrying its address:
//
//   * the cost list.   repair walks cfg Building `resource_2`, upgrade walks cfg Building
//                      `resource` (the OTHER list), train walks cfg Unit `resource`.
//   * the loop shape.  repair tests `.id == 0` BEFORE the `d < 7` bound (0x004e2968 then
//                      0x004e2971); upgrade and train test the bound first (0x004e2b4b,
//                      0x004e27fe). Same outcome, different read order -- see the note on
//                      cost_walk below.
//   * what is zeroed.  train clears `build_tile_x` and `resource_reserved[0..4]`; repair and
//                      upgrade clear only `resource_reserved[1..4]`, so slot [0] and (for repair)
//                      `build_tile_x` keep whatever the previous occupant of that slot left. Slot
//                      [0] is unreachable as a cost -- every walk STOPS on `.id == 0` -- so this
//                      is dead state, not a live bug, but it is state the shadow oracle compares.
//   * the tail.        train bumps two per-player training tallies; upgrade issues four resource
//                      orders; repair runs a floating-point loop instead.
//
// THE INDEX SPACES. `building_index` is a ROSTER slot (it indexes buildings[player][]) and
// `unit_id` is a cfg TYPE index (it indexes cfg_units[] with stride 0x23f). The queue entry's
// `building_index` field holds the roster slot; the cfg TYPE only ever enters as the roster
// record's own `building_id`, read MOVZX-word at 0x004e28d6 / 0x004e2af0.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The queue is a fixed 64-entry array and the guard is `JC` -- UNSIGNED (0x004e26bc, 0x004e287f,
// 0x004e2a60). A negative count would therefore read as huge and be rejected, not as "empty".
inline constexpr int32_t AI_BLDG_QUEUE_CAP = 0x40;

// The status byte's kind nibble, as the three enqueues stamp it. The shared vocabulary lives in
// ai_queue_reconcile.h (QUEUE_KIND_CONSTRUCTION / _REPAIR / _UPGRADE); these are the raw whole-byte
// values stored here, and they are stores, not ORs -- every flag bit is cleared with them.
inline constexpr uint8_t QUEUE_STATUS_NEW_TRAIN   = 0; // 0x004e2736
inline constexpr uint8_t QUEUE_STATUS_NEW_REPAIR  = 3; // 0x004e2892
inline constexpr uint8_t QUEUE_STATUS_NEW_UPGRADE = 4; // 0x004e2a73

// Every cost walk in this file is bounded at SEVEN, and so is every other consumer of these arrays
// in the binary -- which is what settled cfg Unit `resource`/`resource_2` being [7] rather than [4]
// (ghidra_findings 2026-08-02-1830-1). If a Ghidra retype ever shrank them again the walks below
// would silently stop at 4, hence the assert.
static_assert(sizeof(cfg_unit::resource) / sizeof(mh::game::mh_cfg_struct_resource) == 7,
              "cfg Unit.resource must stay 7 entries -- every cost walk is bounded at 7");
static_assert(sizeof(cfg_building::resource) / sizeof(mh::game::mh_cfg_struct_resource) == 7,
              "cfg Building.resource must stay 7 entries -- every cost walk is bounded at 7");
static_assert(sizeof(cfg_building::resource_2) / sizeof(mh::game::mh_cfg_struct_resource) == 7,
              "cfg Building.resource_2 must stay 7 entries -- queue_bldg_repair walks it to 7");
inline constexpr int32_t COST_WALK_MAX = 7;

// `resource_reserved` is indexed by the cost entry's `.id`, NOT by the walk position, and NOTHING
// in the original bounds that id against the array (the store is a bare `ADD word ptr
// [entry + id*2 + 4]` at 0x004e2952 / 0x004e2b42 / 0x004e27f5). An id above 4 would write past the
// entry into the next queue slot. The walks below reproduce the index but assert the bound, so a
// cfg file that could do it fails the offline oracle loudly instead of corrupting a neighbour.
inline constexpr int32_t RESOURCE_RESERVED_COUNT = 5;

namespace detail {

// Shared by all three: how a call ended. The original returns int (1 = queue full, 0 = appended);
// the extra fields are instrumentation, because "N calls, 0 divergences" over calls that all hit
// the full-queue guard would compare almost nothing.
struct enqueue_report {
    int32_t rc        = 0;     // what the original returns
    int32_t slot      = -1;    // the entry index written, -1 if the guard rejected
    int32_t cost_ids  = 0;     // cost entries the walk actually consumed
    int32_t oob_cost  = 0;     // cost entries whose id was outside resource_reserved -- see cost_walk
    int32_t ticks     = 0;     // repair only: iterations of the energy loop
    int32_t grants    = 0;     // upgrade only: grant_resource_raw calls (always 4 when it appends)
    int32_t cap_scan  = 0;     // train only: queue entries the per-unit cap scan looked at
    int32_t cap_hits  = 0;     // train only: matching train entries it counted
    bool    cap_check = false; // train only: whether the cap scan ran at all
};

// llm_strat_ai_queue_train_unit @0x004e268c. `unit_id` is a cfg Unit TYPE index.
enqueue_report queue_train_unit(const ai_view &v, const ai_store &own, uint32_t player,
                                uint32_t unit_id);

// llm_strat_ai_queue_bldg_repair @0x004e284d. `building_index` is a ROSTER slot.
enqueue_report queue_bldg_repair(const ai_view &v, const ai_store &own, uint32_t player,
                                 int32_t building_index);

// llm_strat_ai_queue_bldg_upgrade @0x004e2a2e. `building_index` is a ROSTER slot.
enqueue_report queue_bldg_upgrade(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  uint32_t player, uint32_t building_index);

} // namespace detail

// The three public wrappers take `int32_t player` because that is the committed storage in
// addr/mh_export.gen.h -- the drift gate rejects any other spelling.
int32_t queue_train_unit(int32_t player, uint32_t unit_id);
int32_t queue_bldg_repair(int32_t player, int32_t building_index);
int32_t queue_bldg_upgrade(int32_t player, uint32_t building_index);

} // namespace mh::ai
