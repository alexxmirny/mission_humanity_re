//
// ai/ai_notify_map_changed.h -- the two "a building changed near me" AI broadcast hooks (RI-AI
// batch B, 2026-08-07).
//
// Callers: llm_strat_ai_bldg_queue_process_entry / llm_strat_ai_plan_turret_upgrade /
// llm_strat_ai_scan_construction_sites call notify_map_changed; llm_strat_ai_notify_bldg_constructed
// (batch E, not yet translated) calls notify_map_changed_2. Both loop every active AI player and
// re-stamp that player's own tile-ownership influence grid via grid_stamp_seeds, but they choose a
// DIFFERENT paint value: notify_map_changed is the "announce this to everyone" broadcast (7 for a
// foreign observer, 5 or 4 for the builder depending on building type); notify_map_changed_2 is the
// "clear/re-evaluate" pair with llm_strat_ai_notify_object_removed (0 for everyone but the builder,
// 2 for the builder on a non-structural type) -- NOT byte-identical bodies despite the generic export
// plate's claim to the contrary (checked instruction-by-instruction; that plate is wrong here).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_notify_map_changed @0x004d82ca.
//
// For every active player p (0-indexed, bound _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT): if p is
// AI-enabled, sets p's ai_map_changed_pending dirty flag, then calls grid_stamp_seeds over p's own
// ai_tile_flags_grid with the building's 10x10 area mask centred at (tile_x, tile_y) and a seed
// value of:
//   7                          if p != builder_player (a foreign observer)
//   5                          if p == builder_player AND the building is one of the six
//                              turret/mine/relay types (checked against BLDG_TYPE_{H,A}_{TURRET,
//                              MINE,RELAY}, same six as is_structural_building_type in
//                              ai_notify_removed.cpp -- duplicated locally per the translator brief's
//                              no-new-shared-helpers rule, not factored out)
//   4                          if p == builder_player and it is not one of those six types
// The original's own tail is a JMP into llm_strat_ai_commit_attack_order's shared epilogue
// (0x004d5701, offset +1 into that function) -- a Watcom code-folded RETURN, not a real call.
void notify_map_changed(const ai_view &v, const ai_store &own, const ai_calls &gc,
                        int32_t builder_player, int32_t building_type, int32_t tile_x, int32_t tile_y);

// llm_strat_ai_notify_map_changed_2 @0x004d83c7.
//
// Same active-player loop and dirty-flag stamp as notify_map_changed above, but the seed choice is
// the "clear it again" shape shared with llm_strat_ai_notify_object_removed: 2 if p == builder_player
// AND the building is NOT one of the six structural types, else 0.
void notify_map_changed_2(const ai_view &v, const ai_store &own, const ai_calls &gc,
                          int32_t builder_player, int32_t building_type, int32_t tile_x,
                          int32_t tile_y);

} // namespace detail

void notify_map_changed(int32_t builder_player, int32_t building_type, int32_t tile_x,
                        int32_t tile_y);
void notify_map_changed_2(int32_t builder_player, int32_t building_type, int32_t tile_x,
                          int32_t tile_y);

} // namespace mh::ai
