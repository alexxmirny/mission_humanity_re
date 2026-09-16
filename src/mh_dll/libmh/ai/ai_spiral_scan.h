//
// ai/ai_spiral_scan.h -- the spiral-ring engage-candidate scanner (RI-AI / AI1A batch A layer 2).
//
// The callee unit_scan_engage_candidates_in_range (ai_engage_scan.h) hands off to: walk every tile
// within `ring_index` of (x, y) (the precomputed disc in v.spiral_offsets, wrapped through the torus
// masks) and, at each tile, offer up to TWO engage candidates -- the tile's BUILDING and every UNIT
// stacked on the tile -- to gc.engage_candidate_add, gated on target_mask, fog-of-war visibility, and
// hostility (player_data::ai_player_relation, a SIGN test).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_scan_spiral_ring_for_engage_candidates @0x004ed495.
//
// For i in [0, v.spiral_ring_cell_counts[ring_index]) (UNSIGNED compare against the count):
//   tx = (x + spiral_offsets[i].dx) & *map_width_mask, ty = (y + spiral_offsets[i].dy) & *map_height_mask
//   tile = tile_at(v, tx, ty); fog = fog_visible_count_at(v, tx, ty, player)      -- computed ONCE,
//   shared by both offers below.
//
//   OFFER 1 (the tile's BUILDING, if any): tile.building != 0, AND
//   (target_mask & (tile.class_owner & 0xf0)) != 0 (a FULL-DWORD TEST against target_mask -- the
//   Ghidra prototype called it `byte` until 2026-08-01), AND
//   player_data[player].ai_player_relation[tile.class_owner & 0xf] <= -1 (hostile -- the owner
//   indexed is the TILE's owner, not `player`), AND fog != 0 ->
//   gc.engage_candidate_add(tile.class_owner, tile.building); ++added. No extra bits OR'd into the ref.
//
//   OFFER 2 (every UNIT stacked on the tile, if (target_mask & 0x20) != 0 -- a BYTE-WIDTH test this
//   time, functionally identical to the dword test above since 0x20 lives entirely in the low byte --
//   AND fog != 0): walk the chain starting at tile.unit (a packed map::t::unit_full_id, high nibble =
//   owner, low 12 bits = index) and stepping via each unit's OWN `unit_above` field (offset 0x0,
//   verified against the disassembly's 0xdd8c48-relative load, NOT `ai_group_next` at 0xd4) until the
//   link is 0. At each link: owner = (link & 0xf000) >> 12 (NOTE: extracted from bits 12-15 here,
//   unlike offer 1's `& 0xf` on class_owner's low nibble -- two different extractions of "owner" over
//   two different packed fields), index = link & 0xfff. If
//   player_data[player].ai_player_relation[owner] <= -1 (hostile) ->
//   gc.engage_candidate_add(owner | 0x20, index); ++added -- this branch OR's 0x20 into the ref
//   (offer 1 does not).
//
// Returns the number of candidates added.
int32_t scan_spiral_ring_for_engage_candidates(const ai_view &v, const ai_calls &gc, int32_t player,
                                               int32_t x, int32_t y, int32_t ring_index,
                                               uint32_t target_mask);

} // namespace detail

int32_t scan_spiral_ring_for_engage_candidates(int32_t player, int32_t x, int32_t y,
                                               int32_t ring_index, uint32_t target_mask);

} // namespace mh::ai
