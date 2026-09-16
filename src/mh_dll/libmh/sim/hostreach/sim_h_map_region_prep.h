//
// sim/hostreach/sim_h_map_region_prep.h -- three of the nav-region pipeline's PREP steps (SIM1-H wave
// 2, tmp/decomp_sim/_CONTEXT_SIM1H_WAVE2.md): llm_map_compute_obstacle_proximity_flags @0x0042207b,
// llm_map_init_region_route_step_deltas @0x00423299, llm_map_compute_region_merge_threshold
// @0x0042308b. All three are LEAF functions -- none has any callee besides the inert
// utils_assert_stack_capacity prologue (translator-brief rule 6, omitted) -- so this TU needs no
// `_calls` struct at all, unlike every other hostreach sibling.
//
// llm_map_build_regions (sim_h_map_build_regions.cpp) is these three's only caller in this batch
// (0x00423383 / 0x00423388 / 0x0042341f); llm_map_init_region_route_step_deltas is ALSO called by
// llm_map_load_regions (0x00424aaf, not in this batch). Wiring build_regions' three `mh::call::`
// bindings for these names to the `detail::` bodies below is the CONDUCTOR's edit (per the wave-2
// context's own note that wave 1's `mh::call::` bindings "become direct calls once your bodies
// exist"), not done here.
//
// ---- llm_map_compute_obstacle_proximity_flags @0x0042207b (0x254 B) -------------------------------
// Two full width*height sweeps over `_G_LLM_MAP_REGION_GRID` (own.region_cell_at(x,y), x the
// WIDTH-bounded index / outer address term, y the HEIGHT-bounded index / inner address term -- same
// convention as every other GRID site in this closure, re-derived here from the address arithmetic at
// 0x004220d1-0x004220e9: `passable[(x<<8)|y]`, `GRID address = (x<<11)+(y<<3)`).
//   Pass 1 (0x0042209a-0x004220f3): GRID[x][y].terrain_flags = (uint)passable[x][y] -- a plain copy,
//     0 = impassable/obstacle, nonzero = passable. Matches mh_structs.gen.h's own field comment on
//     mh_llm_map_region_cell::terrain_flags ("low byte from g::passable").
//   Pass 2 (0x004220fa-0x004222c5): for every cell whose terrain_flags is STILL 0 (an obstacle) that
//     has at least one of its four torus-wrapped orthogonal neighbours (west/east/north/south, in
//     that exact order -- the asm is a short-circuit OR chain, each JZ/JNZ testing one neighbour
//     before falling through to the next) ALSO at terrain_flags==0 (an isolated single-tile obstacle
//     gets no stencil at all -- all four neighbours must be checked and found passable to skip),
//     stamp a 13x13 (0xa9=169) stencil centred 6 tiles up/left of the cell:
//       * the stencil's top-left corner (bx,by) is computed in BYTE-TRUNCATED arithmetic -- `(byte)x
//         - 6` and `(byte)y - 6`, ANDed with the BYTE-truncated low byte of width_m/height_m
//         (0x004221d3-0x004221f0: MOV AL/AH, byte-width SUB/AND) -- this is genuinely narrower than
//         the offset-accumulation step below and matters at the map's low edge (x<6 or y<6 wraps
//         through a BYTE, not an int32). Reproduced with explicit uint8_t ops, not int32.
//       * each of the 169 offsets i in 0..168 contributes (i%13, i/13), added to (bx,by) as a FULL
//         32-bit unsigned add, then masked by the FULL 32-bit width_m/height_m (0x00422226/
//         0x00422247) -- this part is NOT byte-truncated, unlike the corner above. Both IDIVs divide
//         the literal 13.
//       * if the target cell (sx,sy) is PASSABLE (terrain_flags != 0), OR in
//         `1 << (proximity_stencil[i].bit_index & 0x1f)` (the `&0x1f` reproduces x86 SHL's implicit
//         CL-mod-32 masking); an obstacle target (terrain_flags==0) is left untouched.
// Matches mh_structs.gen.h's own field comment on terrain_flags: "bits 2/3/4 = obstacle within
// ~5/~3/~1 tiles (13x13 stencil)" -- this function is what populates those bits. The pass mutates the
// SAME array it reads (a later obstacle-adjacency test in pass 2 can see bits set by an earlier
// iteration of the SAME pass) -- reproduced by reading `own.region_cell_at()` fresh at every use, per
// translator-brief rule 16 (no caching/snapshotting a value the same pass can still change); this is
// never observable as a 0<->nonzero transition since the stencil write only ORs bits into cells that
// were already nonzero (passable) to begin with.
//
// `_G_LLM_MAP_PROXIMITY_STENCIL` (0x0051deac, `llm_map_proximity_stencil_entry[169]`, 4-byte stride,
// only the first byte -- `.bit_index` -- read) is bound as `sim_view::proximity_stencil`. The
// translator raised it as a DECLARED NEED because no binding existed; the conductor registered the
// region and the struct and the binding landed with this batch. A 13x13 stencil: index i maps to the
// offset (i%13 - 6, i/13 - 6), the live values are 0/2/3/4, and `llm_map_region_cell::terrain_flags`'
// own field comment says "bits 2/3/4 = obstacle within ~5/~3/~1 tiles (13x13 stencil)" -- the data
// and the consuming field's documentation corroborate each other independently.
//
// ---- llm_map_init_region_route_step_deltas @0x00423299 (0x9c B) -----------------------------------
// 16 byte stores (0x004232b1-0x0042331a) filling the 8-direction, 4-byte-stride
// `_G_LLM_MAP_REGION_ROUTE_STEP_DELTAS` (own.region_route_step_deltas_mut(), byte[32]) two bytes at a
// time, then a dword copy (0x00423321/0x00423326) of entry 0 into the 4-byte WRAP sentinel
// (own.region_route_step_delta_wrap()) immediately past the array (0x00708af4+32==0x00708b14 exactly)
// -- a wrap slot, not a ninth direction, so `region_route_search` can read [i]/[i+1] for any i in 0..7
// without a bounds check. The wrap copy reads the FULL dword at entry 0, i.e. bytes [0]/[1] (the ones
// this function itself writes) AND bytes [2]/[3] (whatever the BSS-zero-initialised global already
// holds -- this function never touches them); the wrap accessor's four bytes are populated from
// exactly that dword, byte for byte.
//
// Decoded per-direction (byte+0, byte+1) pairs, re-derived directly from the store addresses/values
// (not from either committed comment -- see the SETTLED note below):
//   dir0=(1,0) dir1=(1,-1) dir2=(0,-1) dir3=(-1,-1) dir4=(-1,0) dir5=(-1,1) dir6=(0,1) dir7=(1,1)
// (0xff is -1, stored as the raw byte -- matches how sim_map_region_routing.cpp's
// `region_route_search` consumes it: `cur_row + row_delta` as a plain uint8_t add, which wraps
// exactly like x86's byte ADD would.)
//
// ---- SETTLED: +0 is the ROW/Y delta, +1 is the COL/X delta (mh_addrs.gen.h was RIGHT; this
// function's own Ghidra plate, "dx at +0, dy at +1", is WRONG) --------------------------------------
// The wave-2 context flagged this as an open contradiction to resolve. sim_state.h itself already
// commits to the answer at two independent sites -- `sim_view::region_route_step_deltas`'s own
// comment ("byte[32], 8 dirs x 4-byte stride: +0=row/y, +1=col/x", sim_state.h:934) and
// `sim_store::region_route_step_deltas_mut()`'s own comment (sim_state.h:2431, "+1=col/x ...
// llm_map_init_region_route_step_deltas is the sole writer; route_search reads it") -- and the
// CONSUMER, `mh::sim::detail::region_route_search` (sim_map_region_routing.cpp:44-53, already
// translated), independently re-derives the SAME layout from the raw instructions at its own call
// site: it reads `row_delta = region_route_step_deltas[dir*4+0]` and adds it to `cur_row` (the LOW
// byte of the packed cell, `current & 0xff`), and `col_delta = [dir*4+1]` added to `cur_col` (the HIGH
// byte, `(current>>8)&0xff`) -- self-consistently cross-checked three ways in that file's own header
// banner (source-byte read, destination slot, and the later word-reconstruction round-trip through
// `region_cell_at`). Three independent derivations (two conductor-written state-header comments plus
// the consumer's own from-scratch re-read of the DIFFERENT function that reads this table) all agree:
// +0 is row/y, +1 is col/x. This function's own plate is the one committed comment that disagrees, and
// it is the one that is wrong. Not changing the plate here (out of scope -- this TU only writes new
// files); flagging it as a finding for the conductor.
//
// This function's own compass labelling (E/NE/N/NW/...) is NOT re-asserted above for the same reason:
// it requires knowing which screen/world direction "row+1" and "col+1" each correspond to, which is
// not derivable from this function's bytes alone (see uncertainties[]).
//
// ---- llm_map_compute_region_merge_threshold @0x0042308b (0x6c B) ------------------------------------
// Walks the ENTIRE active region list (*v.region_list_head, via ->next -- own.region_cell_at is not
// involved; this is the same intrusive list build_regions/assign_remaining_tiles_to_regions both use),
// summing `cell_count` (offset 0x4, mh_structs.gen.h's own static_assert) and counting nodes, then
// stores `sum / count` (UNSIGNED `DIV`, not `IDIV`) into `own.region_merge_threshold_mut()`.
//
// ---- PRESERVE-BUG: the division is NOT guarded against an empty list --------------------------------
// `count` starts at 0 and the loop never runs if `*v.region_list_head == nullptr`; the store at
// 0x004230e0-0x004230e8 (`DIV [EBP-0x20]`) is unconditional -- an empty list is therefore a hardware
// divide-by-zero FAULT in the original, not a wrong value. Transcribed literally per translator-brief
// rule 10 (preserve documented original bugs); no guard added.
//   Reachability at this function's one call site in this batch (llm_map_build_regions, 0x0042341f,
//   immediately after `seed_regions_multires` at 0x0042341a): per the wave-2 context's own read of
//   `llm_map_try_seed_region_at`, a region gets allocated for ANY cell still carrying the
//   flood-fill-pending sentinel once `seed_regions_multires`'s decreasing `max_d` sequence reaches 0
//   (a 1x1 block trivially satisfies "every cell in the block is still pending"), so ANY planet with
//   at least one passable tile produces at least one region before this function runs. The fault is
//   therefore reachable only for a planet whose ENTIRE `passable` plane is zero (no walkable tile
//   anywhere) -- a degenerate map no normal game data produces, but nothing in this function OR its
//   caller's code proves it can't happen; not independently confirmed against real map files (this
//   translator has no game-data access). See uncertainties[].
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_compute_obstacle_proximity_flags @0x0042207b. See the header banner (GRID/passable
// indexing, the byte-truncated stencil corner vs. the full-width offset accumulation, and the
// proximity-stencil binding).
void compute_obstacle_proximity_flags(const sim_view &v, sim_store &own);

// llm_map_init_region_route_step_deltas @0x00423299. See the header banner (the SETTLED +0=row/y,
// +1=col/x layout; the wrap-sentinel dword copy).
void init_region_route_step_deltas(sim_store &own);

// llm_map_compute_region_merge_threshold @0x0042308b. See the header banner (PRESERVE-BUG: the
// unsigned division is not guarded against an empty region list).
void compute_region_merge_threshold(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state(). All three take no parameters and return void per the
// committed prototypes (the .asm headers' `void __watcall <name>(void)`).
void compute_obstacle_proximity_flags();
void init_region_route_step_deltas();
void compute_region_merge_threshold();

} // namespace mh::sim
