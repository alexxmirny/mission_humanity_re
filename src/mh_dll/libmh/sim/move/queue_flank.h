//
// sim/move/queue_flank.h -- mp:U75: [net] move_queue_flank_guard, the pure rules.
//
// WHY. The "make room" shuffle (llm_strat_unit_queue_advance_search 0x004d7f3a, run when a unit cannot leave a storage because the exit
// tile is held by a friendly idle unit) and the idle-unit nudge (move_nudge_idle, pick_nudge_tile) both pick a DIAGONAL neighbour tile
// and test only that tile (passable, unoccupied). Neither tests the two orthogonal flank tiles, so a unit steps between two wall tiles
// through the corner of a cliff line, and can end sealed in a one-tile notch no route can lead out of. The route planner forbids that
// step: llm_map_region_route_search lets a diagonal through only when at least one flank is open.
//
// RULE (the planner's own). A diagonal step (dx != 0 && dy != 0) from (x, y) is legal only when at least one of the flank tiles
// (x + dx, y) and (x, y + dy) is OPEN. An orthogonal step is always legal here (its own tile test is unchanged). OPEN = not terrain-
// blocked: passable != 0, OR the tile holds a unit claim (class_owner bit 0x80; its passable 0 is occupancy, not terrain). Trees (class
// 0x10) and buildings (0x40) have passable 0 and no 0x80 bit, so they stay blocked.
//
// DEPTH CAP. _G_LLM_UNITQ_FRONTIER is typed [128] nodes (1024 bytes) and _G_LLM_UNITQ_CLOSED [1024] nodes (8192 bytes) and nothing in the
// original bounds the search depth: with raised unit caps a chain longer than 128 writes past the frontier array. The real extent of
// FRONTIER is NOT proven (Ghidra's size is a guess, the BSS-sizing trap; the next named global is far above), so the cap is the typed
// 128. Closed nodes are bounded by the CLOSED array, which ends exactly where FRONTIER begins (an overflow lands in FRONTIER[0..]).
//
// Shared by the retail splice (mh/seams/move_proto_nudge.cpp) and the libmh twins (sim_unit_queue_advance.cpp, move/nudge.h). Integer only.
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

constexpr int QF_FRONTIER_NODES = 128;  // _G_LLM_UNITQ_FRONTIER typed extent (unproven beyond it)
constexpr int QF_CLOSED_NODES   = 1024; // _G_LLM_UNITQ_CLOSED extent

// One tile's openness for the flank rule (see above).
inline bool qf_tile_open(uint8_t passable, uint8_t class_owner) { return passable != 0 || (class_owner & 0x80u) != 0; }

// A step (dx, dy) is acceptable under the guard: orthogonal always, diagonal only with an open flank.
inline bool qf_step_ok(bool guard, int dx, int dy, bool flank_x_open, bool flank_y_open) {
    if (!guard || dx == 0 || dy == 0) return true;
    return flank_x_open || flank_y_open;
}

// May the search descend one more node? stage_idx = the frontier slot the current node is staged at; closed_count = nodes already closed.
// A descent stages the next node at stage_idx + 1 (needs < 128) and every live node may later be closed (needs <= 1024 in all).
inline bool qf_may_descend(bool guard, int stage_idx, int closed_count) {
    if (!guard) return true;
    return stage_idx + 1 < QF_FRONTIER_NODES && closed_count + stage_idx + 2 <= QF_CLOSED_NODES;
}

} // namespace mh::sim::move
