//
// sim/sim_pathfind_route.h -- two strategic-map routing primitives (RI-SIM / SIM1-G2):
// a greedy, region-value-descending tile tracer with a corner-cut-guarded straight-line finish
// (pathfind_trace_route), and a region-crossing validity walker that plans a group's route between
// two tiles (pathfind_plan_group_route). Translated from the DISASSEMBLY, not the Ghidra .c drafts:
//   tmp/decomp_sim/llm_strat_pathfind_trace_route_0041ef3e.asm
//   tmp/decomp_sim/llm_strat_pathfind_plan_group_route_0042039c.asm
//
// ---- llm_strat_pathfind_trace_route @0x0041ef3e (0x8b1 B) --------------------------------------------
// `int __mh_watcall_ecx_ebx_volatile llm_strat_pathfind_trace_route(byte src_col, byte src_row,
// byte dst_col, byte dst_row, void *step_ctx)`, matching the committed prototype in
// addr/mh_calls.gen.h exactly. Sole caller is llm_strat_group_move_order_pathfind (its deviation-
// search's corner-cut success arm, see sim_group_move_order_pathfind.cpp's own call site) -- NOT AI
// code despite the pre-2026-08-07 llm_strat_ai_ name; zero AI callers over the whole graph. 0 DIRECT
// writes to tracked state -- every write happens inside the frontier callee
// llm_strat_group_path_step_record(step_ctx, heading, col, row), already committed in
// mh_calls.gen.h.
//
// SHAPE, two phases:
//   Phase 1 (0x0041efe2-0x0041f658): greedy region-value descent. While (col,row) != (dst_col,
//   dst_row) AND the destination cell's own value is still <= the best value seen so far, read the
//   current cell's region value (a per-cell scalar read via terrain_flags>>8 -- see the "REGION
//   VALUE" note below); 0 means blocked (return 1 immediately), 1 breaks straight into phase 2. Then
//   scan the 4 cardinal neighbours (south=row+1, west=col-1, north=row-1, east=col+1; each gated on
//   being < 0xffffff and != 0) and the 4 diagonals (each gated on at least one of its two flanking
//   cardinals being open, per the `cardinal_open` bitmask -- south|west -> NW-diagonal heading 4,
//   west|north -> heading 0xa, north|east -> heading 0x10, south|east -> heading 0x16), picking the
//   lowest-valued neighbour as the candidate heading (ties only override an ALREADY-diverged
//   candidate -- see the .cpp's `consider()` note on why Ghidra's 8 slightly different-looking
//   comma-expressions all collapse to one rule). The committed heading only actually changes when a
//   straight run of >0 steps has already happened AND the candidate differs -- this is what makes the
//   walk prefer continuing straight over greedily re-routing every single tile. Records the step
//   (llm_strat_group_path_step_record) BEFORE advancing (col,row) by the committed heading's
//   (dx,dy) from map_dir_step_deltas.
//
//   Phase 2 (0x0041f65d-0x0041f7ec): once phase 1 exits with (col,row) != (dst_col,dst_row) (region
//   value floor reached, i.e. we are now "close"), walk a straight Bresenham-ish diagonal-preferring
//   line toward the destination, one tile at a time, stopping (return 1, blocked) the moment BOTH
//   flanking tiles of a diagonal step are impassable (the corner-cut guard) -- reaching the
//   destination exactly returns 0.
//
// ---- REGION VALUE: terrain_flags>>8, NOT the struct field's documented low-byte semantics --------
// Every region-grid read in this function is `region_cell_at(col,row).terrain_flags >> 8` -- the
// UPPER 24 bits of the dword, not the LOW byte mh_structs.gen.h's field comment describes ("low byte
// from g::passable; bits 2/3/4 = obstacle..."). The struct evidently overloads this one dword: low
// byte = terrain obstacle flags (documented), upper 24 bits = a per-cell "value" this tracer treats
// as a cost to minimize (its own Ghidra plate: "greedily steps into the lowest-valued neighboring
// region"). This function only ever reads the upper bits; nothing here re-derives what populates
// them. `own.region_cell_at(...)` (sim_store's existing MUTABLE accessor) is used for this READ-ONLY
// purpose, matching sim_group_move_order_pathfind.cpp's identical precedent (its own
// `terrain_ahead`/`terrain_here`/`best_terrain` locals) -- there is no separate const sim_view
// accessor for the region grid, and none is needed given that precedent.
//
// ---- A CAUGHT GHIDRA DRAFT BUG: the destination cell's value is read ONCE, not shifted twice ------
// The Ghidra .c draft's `uVar1 = _G_LLM_MAP_REGION_GRID[dst_col][dst_row].terrain_flags;` (no shift)
// followed later by `uVar1 >> 8 <= local_34` in the while-guard READS as if the shift happens at the
// point of use. The raw .asm shows exactly ONE `MOV`+`XOR AL,AL`+`SHR EAX,8` sequence for this cell
// (0x0041ef92-0x0041efaf, storing the ALREADY-shifted value once into a stack slot), and the sole
// later use (0x0041eff5) is a bare `CMP` against that stored value with no further shift. Applying a
// second `>>8` at the comparison (as the draft's text implies) would compare against a value up to
// 256x too small whenever the destination's raw region value has any of bits 8-15 set --
// materially changes when phase 1 stops early. Translated here as a single read into `dst_value`,
// compared directly. See uncertainties[].
//
// ---- llm_strat_pathfind_plan_group_route @0x0042039c (0x27a B) ---------------------------------------
// `int __mh_watcall_ebx_volatile llm_strat_pathfind_plan_group_route(uint start_col, uint start_row,
// uint dest_col, uint dest_row, uint *out_col, uint *out_row)`, matching the committed prototype
// exactly. 0 DIRECT writes to tracked state besides the two OUT PARAMETERS (caller-owned stack
// storage, not sim state) -- every other write happens inside the frontier callees
// llm_map_region_find_route / _flood_reachable / _route_search, all already committed.
//
// SHAPE: an outer do-while over successive region crossings, each iteration re-snapshotting the
// CURRENT cell's region pointer, then an inner while stepping (col,row) one wrap-corrected tile at a
// time toward (dest_col,dest_row) using the SAME halving/wrap-correction idiom
// sim_group_move_order_pathfind.cpp's centroid computation and
// sim_pathfind_route_leg_group_and_sort.cpp already use (bit-identical to plain C `/` for every int32
// input here, per those files' own notes) -- masked into range with path_wrap_mask after each step.
// Every time the stepped-to cell's region differs from the snapshot AND is non-null,
// llm_map_region_find_route(dest_packed, current_packed) is called (packed = (col<<8)|row, matching
// this codebase's other packed-position idiom); a nonzero (blocked) result returns 0 immediately.
//
// ---- REACHABILITY OF THE TRAILING BLOCK -- RE-DERIVED, NOT TAKEN ON EITHER PRIOR CLAIM ------------
// The Ghidra plate flags the block writing *out_col/*out_row + calling llm_map_region_flood_reachable
// + llm_map_region_route_search (0x00420595-0x00420602) as reached only through a region its own CFG
// analysis calls unreachable ("inferred, not confirmed"); a prior conductor note claimed the OPPOSITE,
// that it IS reached via the outer loop's blocked-exit path. Independently re-traced against the raw
// JMP/Jcc targets for THIS translation (not taking either claim on faith): the inner while loop's ONLY
// live exit (0x004203fc, the dead 0x00420414 exit is discussed next) fires exactly when `col==dest_col
// && row==dest_row`; the block immediately after (0x00420585-0x00420593) RE-CHECKS that identical
// condition, and since 0x00420418/0x00420585 has no other predecessor, the re-check is always true --
// so the JNZ into the trailing block (0x00420595) can never execute. THE GHIDRA PLATE'S CONCLUSION IS
// CORRECT (the block is genuinely unreachable in the compiled binary), though its own address citation
// (0x00420404) actually names a DIFFERENT dead block (see next paragraph), not the trailing one. See
// uncertainties[] -- translated anyway per house rule (Law 2): a provably-dead block is still
// transcribed, not dropped.
//
// A SECOND, separate dead block exists in the SAME function: the inner while's passable-check arm at
// 0x00420404-0x00420414 (`CMP dword ptr [EBP+-0x14],0`) is gated on a stack slot that is written to 0
// exactly once per outer-loop entry and never modified before that check -- so the check's else-branch
// (a `passable[col][row]` read) never executes either. This is almost certainly what the Ghidra
// draft's own "WARNING: Removing unreachable block (ram,0x00420404)" comment refers to (its .c has no
// passable check at all in the inner while), NOT the trailing out_col/route_search block -- two
// different dead blocks, one citation. Neither is reproduced as a live branch: the passable-check dead
// branch has no OBSERVABLE effect to transcribe (it never runs, and its own body has no side effect
// besides a conditional jump), so it is simply omitted (matching the Ghidra .c); the trailing block
// IS transcribed (per Law 2) since it contains real writes/calls, just placed after the always-true
// early return so it is provably (not merely apparently) dead code.
//
#pragma once
#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE / the entry-thunk shapes
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_pathfind_trace_route @0x0041ef3e. See the header banner above.
int32_t pathfind_trace_route(const sim_view &v, sim_store &own, uint8_t src_col, uint8_t src_row,
                             uint8_t dst_col, uint8_t dst_row, void *step_ctx);

// llm_strat_pathfind_plan_group_route @0x0042039c. See the header banner above.
int32_t pathfind_plan_group_route(const sim_view &v, sim_store &own, uint32_t start_col,
                                  uint32_t start_row, uint32_t dest_col, uint32_t dest_row,
                                  uint32_t *out_col, uint32_t *out_row);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes / mh::exp::sig_llm_strat_pathfind_*
// exactly (out_col/out_row carry the committed `uint32_t *` pointee -- TACT1-P C6, 2026-09-04;
// step_ctx stays `void *`, matching the export signature).
int32_t pathfind_trace_route(uint8_t src_col, uint8_t src_row, uint8_t dst_col, uint8_t dst_row,
                             void *step_ctx);
int32_t pathfind_plan_group_route(uint32_t start_col, uint32_t start_row, uint32_t dest_col,
                                  uint32_t dest_row, uint32_t *out_col, uint32_t *out_row);

namespace detail {
} // namespace detail

} // namespace mh::sim
