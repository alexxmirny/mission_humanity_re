//
// tact/tact_map_bounds.h -- TACT1A: recompute the active-map bounds cache.
//
//   llm_tact_map_compute_bounds @0x0043a06f (0x16a)
//
// Scans the mission's 128x128 tile-height-sprite table and grows _G_LLM_TACT_MAP_WIDTH_CACHE /
// _G_LLM_TACT_MAP_HEIGHT_CACHE to the highest column/row that has ANY nonzero height-sprite slot,
// then converts each running max into a COUNT (+1) and clears a one-tile boundary strip on the
// shared `passable` plane at the new edge.
//
// THE TWO CACHES ARE NEVER RESET HERE -- the original has no store to either global before the
// scan, so this function only ever GROWS them (llm_tact_map_reset, not yet translated, is the
// reset). Preserved as-is: a translation that zeroed them first would silently change behaviour on
// a second call within the same mission.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_map_compute_bounds @0x0043a06f.
//
// Outer/inner/innermost loop order is the ORIGINAL's, not relabelled for readability: the outer
// variable (0x0043a087, growing HEIGHT_CACHE) is the slow-varying one and the middle variable
// (0x0043a0a4, growing WIDTH_CACHE) is reset every outer iteration -- reproduced literally because
// swapping the nesting would not change the RESULT here (both caches end at the same max either
// way) but would change which one is "outer" if anyone ever adds a side effect to the loop body.
//
// `sprites` is *v.map_tile_height_sprites -- a POINTER VARIABLE (Ghidra: size 4, "undefined4"), set
// once by llm_tact_mission_start, not the array itself. Indexed in uint16_t units: byte offset
// b_idx*2048 + a_idx*16 + k*2 (0x0043a0e2/0x0043a0ef/0x0043a0f7) is exactly
// sprites_u16[b_idx*1024 + a_idx*8 + k].
//
// The two boundary-clear blocks (0x0043a156-0x0043a193) are gated by `< 0x80`, NOT by the
// [256][256] plane's real extent -- reproduced as the literal 0x80 the disassembly uses, not as
// TACT_MAP_DIM, because here it is a bound-check constant rather than a restatement of the map's
// declared dimension (the two happen to share a value, not a meaning).
void map_compute_bounds(tact_view &v, tact_store &own);

} // namespace detail

void map_compute_bounds();

// Called from tact_state.cpp's mh::tact::install_shadow aggregator (one ini entry point per
// domain, owned by the state pair).

} // namespace mh::tact
