//
// ai/ai_bldg_queue_dispatch.h -- the FOUR arms of the AI build-queue dispatch table (RI-AI / AI1B
// layer 3). Their caller, llm_strat_ai_bldg_queue_process @0x004e82ab, is ai_bldg_queue.{h,cpp};
// this file is the jump table's other side, and the four are an ANTICHAIN (none reaches another),
// so they can be armed together.
//
//   nibble 0 -> llm_strat_ai_bldg_queue_handle_recruit_state       @0x004e80ef  (0xf0 bytes)
//   nibble 1 -> llm_strat_ai_bldg_queue_process_entry              @0x004e7dd7  (0x318 bytes)
//   nibble 2 -> llm_strat_ai_bldg_queue_handle_state2_empty        @0x004e81df  (0x0b bytes)
//   nibble 3/4 -> llm_strat_ai_bldg_queue_handle_upgrade_or_cancel @0x004e81ea  (0x96 bytes)
//
// All four take (player in EAX, queue_index in EDX) except the nibble-2 no-op, which takes nothing.
// EVERY absolute is re-derived against addr/mh_structs.gen.h with the 166140 = 0x288fc player stride
// (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD, e.g. 0x004e7df0-0x004e7e02):
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0].status         (+0x12 per slot)
//   0xe935f1 =                       = ai_bldg_queue[0].tick_or_unit_id
//   0xe935f2 =                       = ai_bldg_queue[0].build_tile_x
//   0xe935f4 =                       = ai_bldg_queue[0].resource_reserved[0]   (REUSED as tile Y)
//   0xe935f6 =                       = ai_bldg_queue[0].resource_reserved[1]
//   0xe935fe =                       = ai_bldg_queue[0].building_index
//   0xe7df14 = player_data + 0x10054 = resource_spent[1]               (+4 per resource id)
//   0xe7e380 = player_data + 0x104c0 = is_alien_race
//   0xe7e414 = player_data + 0x10554 = ai_promo_credit
//   0xe9677c = player_data + 0x288bc = ai_build_candidate_primary
//   0xd9ec80 = the cfg Building table (stride 0x842): +0x8 type, +0xb area, +0x258 worker_count
// so no byte offset and no literal VA appears in the .cpp (Law 1).
//
// ---- handle_recruit_state (nibble 0) ------------------------------------------------------------
//
// "The queue wants unit type U built; find somewhere to build it and pay for it -- unless the
// promo budget can cover it for free."
//   1. llm_strat_bldg_find_idle_producer_for_unit(player, entry.tick_or_unit_id) @0x004e8129.
//      ZERO means no idle producer can make that unit type: RETURN, leaving the entry untouched so
//      the next tick retries it (JZ to the shared epilogue @0x004e8130).
//   2. player_data::ai_promo_credit > 0 (CMP/JLE @0x004e8136 -- SIGNED, and the field is signed):
//      spend one promo. llm_strat_order_recruit_unit_enqueue(unit_id, player) @0x004e8149, then
//      ai_promo_credit -= _G_LLM_STRAT_AI_CFG_PROMO_ADD and status |= 0xc0. NO RESOURCES MOVE ON
//      THIS PATH -- that is what makes it the free one.
//   3. otherwise pay: llm_strat_bldg_order_production_add_enqueue(player, producer, unit_id)
//      @0x004e8177 (its name is under doubt -- ghidra_findings 2026-08-02-1922-10 -- but its third
//      argument is unmistakably the unit type, loaded at 0x004e8165), then
//      llm_strat_econ_track_unit_resource_spend(player, unit_id) @0x004e8185, then debit
//      resource_spent[1..4] by resource_reserved[1..4], then ai_promo_credit +=
//      _G_LLM_STRAT_AI_CFG_PROMO_SUB and status |= 0x80.
//
// THE TWO SCALARS ARE INVERTED RELATIVE TO THEIR AI.SCR NAMES: nPromoAdd (0x00669390) is
// SUBTRACTED, nPromoSub (0x00669394) is ADDED. Both default to 1, so the net effect is one free
// promo per paid production. Reproduced as-is; see the addr manifest entries.
//
// ---- process_entry (nibble 1) -------------------------------------------------------------------
//
// The construction arm, and the only one of the four with real control flow. THREE stages:
//   A. RESOLVE A TILE, but only if none is cached (build_tile_x == -1 @0x004e7e0a). It runs
//      llm_strat_ai_bldg_production_type_dispatch(player, type) -- which is what FILLS the shared
//      site-candidate scratch -- and then takes candidate [0] if the count is non-zero, caching x
//      into build_tile_x and y into resource_reserved[0], and telling the other players' influence
//      grids via llm_strat_ai_notify_map_changed. If the scan produced NOTHING the entry is stamped
//      0xc0 (committed + removed) and abandoned @0x004e7e61.
//      NOTE the cached x/y are read back as UNSIGNED 16-bit everywhere downstream (MOVZX), while
//      the -1 test is a SIGNED word compare -- so 0xffff is the sentinel and 0..0xfffe are tiles.
//   B. RE-TEST THE FOOTPRINT every tick, cached tile or not: llm_scan_masked_table_for_empty_cell
//      over `passable` with the type's 10x10 cfg Building::area mask @0x004e7ed0. The span is the
//      literal 10x10 box, NOT Building::width/height -- do not "fix" that (ai_state.h FOOTPRINT_SPAN).
//   C. IF IT FITS, build it, one of two ways:
//      * status & 0x20 (affordability WAIVED) -> clear the bit and issue the ordinary construction
//        order llm_strat_order_queue_construction_enqueue @0x004e7f08 + notify_map_changed. THEN, if
//        the queued type equals player_data::ai_build_candidate_primary (CMP @0x004e7f33), also
//        issue llm_strat_order_population_delta_enqueue(player, Building[type].worker_count).
//        NO RESOURCES ARE DEBITED on this path -- the waiver is why.
//      * otherwise -> llm_strat_bldg_instant_construct_find_slot_enqueue @0x004e7f6f (its 4th
//        argument is a literal 0, XOR ECX,ECX @0x004e7f6d), then
//        llm_strat_bldg_record_resource_expenditure_stats, then debit resource_spent[1..4].
//      Both paths converge on status |= 0x80 and building_index = -1 @0x004e7fd9-0x004e7fe1.
//   D. IF IT DOES NOT FIT, the entry is either RETIRED or RE-QUEUED depending on the building type:
//      a TURRET or a RELAY (the two race-paired types, selected by is_alien_race exactly as
//      ai_state.h's race_* helpers do) gets status |= 0x40 -- removed, the queue compaction will
//      drop it. Anything else has its cached tile invalidated back to 0xffff so stage A runs again
//      next tick. Either way it finishes with llm_strat_ai_notify_map_changed_2.
//      The type is RE-READ from the entry for the second comparison (0x004e803b) rather than kept in
//      a register; that is a no-op here and is written as one read.
//
// ---- handle_state2_empty (nibble 2) -------------------------------------------------------------
//
// Eleven bytes: PUSH 0x4 / CALL assert_stack_capacity / RET. It is a REAL dispatch slot with a real
// (empty) body, not an unused table entry, so it is reproduced as an empty function rather than
// elided -- eliding a dispatch arm is how a table slot silently stops being reproduced.
//
// ---- handle_upgrade_or_cancel (nibble 3 and 4) --------------------------------------------------
//
// Stamps 0x80 FIRST (@0x004e8215), then re-reads the byte it just wrote and masks the nibble to
// decide which order to issue (@0x004e821c-0x004e8228):
//   nibble 4 -> debit resource_spent[1..4] by resource_reserved[1..4], then
//               llm_strat_bldg_order_upgrade_enqueue(player, entry.building_index).
//   nibble 3 -> llm_strat_bldg_order_repair_cycle_start_enqueue(player, entry.building_index),
//               with NO resource movement.
// The OR-then-re-read matters: it is why the nibble test cannot be hoisted above the stamp in a
// translation that also wanted to skip the write.
//
// ---- what a shadow site here can and cannot see -------------------------------------------------
//
// All four write player_data, so unlike the layer's out-pointer helpers they are shadowable in
// principle. Their outward calls all reach llm_strat_order_enqueue, whose write set is the three
// order-container regions -- the same set ai_bldg_queue's own site already declares -- so they run
// for real in the second arm and the restore un-issues the duplicate order. process_entry
// additionally writes the site-candidate scratch, its count, the grid wrap mask and the building
// enclosure scratch, through llm_strat_ai_bldg_production_type_dispatch and the footprint test.
//
// WHAT A VACUOUS GREEN LOOKS LIKE, and on this family it is the likely case rather than the exotic
// one: the 2026-08-02 run armed the three enqueue helpers over 24000 steps on a developed save
// and every one of them logged ZERO calls, i.e. that AI never edited its build queue at all. These
// four sit one step further down the same path (they need an entry to exist AND be affordable AND
// be uncommitted), so read the per-site call count before reading anything into a clean run.
//
#pragma once
#include <cstdint>

#include "ai/ai_bldg_queue.h" // QUEUE_STATUS_AFFORD_WAIVED / QUEUE_KIND_MASK / the cost-loop width
#include "ai/ai_state.h"

namespace mh::ai {

// The two bits the recruit/construction arms stamp together. 0xc0 == COMMITTED | REMOVED: the entry
// is finished AND is to be compacted out, which is what both "this is over" exits use
// (0x004e8159, 0x004e7e61). Spelled as the OR of the two named bits rather than as 0xc0 so it stays
// tied to their definitions.
inline constexpr uint8_t QUEUE_STATUS_DONE_AND_REMOVED =
    uint8_t(QUEUE_STATUS_COMMITTED | QUEUE_STATUS_REMOVED);

// build_tile_x's "not resolved yet" sentinel. The original writes it as the word 0xffff
// (0x004e80a7) and tests it as the SIGNED word -1 (0x004e7e0a / 0x004e7e87), which is the same
// bit pattern; the two spellings are kept apart here for exactly that reason.
inline constexpr int16_t QUEUE_TILE_UNRESOLVED = int16_t(-1);
// building_index's "none" value, stamped on every completed construction (0x004e7fe1).
inline constexpr int32_t QUEUE_BUILDING_INDEX_NONE = -1;

namespace detail {

struct queue_dispatch_report {
    // recruit_state
    int32_t recruit_calls = 0;
    int32_t no_producer   = 0; // returned early: no idle producer for the unit type
    int32_t promo_spent   = 0; // took the FREE path (ai_promo_credit > 0)
    int32_t paid          = 0; // took the paying path
    // process_entry
    int32_t entry_calls      = 0;
    int32_t tile_resolved    = 0; // stage A cached a fresh candidate tile
    int32_t tile_scan_empty  = 0; // stage A found no candidate at all -> entry abandoned
    int32_t no_tile          = 0; // returned at stage B with no tile to test
    int32_t fits             = 0; // the footprint test passed
    int32_t built_waived     = 0; //   ... via the 0x20 waived construction-order path
    int32_t built_instant    = 0; //   ... via the instant-construct + debit path
    int32_t population_delta = 0; // the ai_build_candidate_primary match fired
    int32_t blocked_retired  = 0; // did not fit AND was a turret/relay -> 0x40
    int32_t blocked_requeued = 0; // did not fit -> tile invalidated, retry next tick
    // upgrade_or_cancel
    int32_t upgrade_calls = 0;
    int32_t upgraded      = 0; // nibble 4
    int32_t cancelled     = 0; // nibble 3 (or any other nibble that reaches this arm)
    // state2_empty
    int32_t noop_calls = 0;
};

// The logic over an EXPLICIT state and an INJECTED call set, so `net_selftest.exe aitest` can drive
// it over heap buffers with recording stubs and no game.
void bldg_queue_handle_recruit_state(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     uint32_t player, int32_t queue_index,
                                     queue_dispatch_report &rep);
void bldg_queue_process_entry(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              uint32_t player, int32_t queue_index, queue_dispatch_report &rep);
void bldg_queue_handle_state2_empty(queue_dispatch_report &rep);
void bldg_queue_handle_upgrade_or_cancel(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         uint32_t player, int32_t queue_index,
                                         queue_dispatch_report &rep);

} // namespace detail

void bldg_queue_handle_recruit_state(uint32_t player, int32_t queue_index);
void bldg_queue_process_entry(uint32_t player, int32_t queue_index);
void bldg_queue_handle_state2_empty();
void bldg_queue_handle_upgrade_or_cancel(uint32_t player, int32_t queue_index);

} // namespace mh::ai
