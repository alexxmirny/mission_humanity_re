//
// sim/libtrans/sim_lt_bldg_cell_grid.h -- the per-building-TYPE 8x8 footprint+halo cell-grid
// rebuild.
//
//   void __watcall llm_strat_bldg_recompute_cell_grid(void)   @ 0x004dc25f (size 743 B)
//
// lib_trans batch C, unit c4 -- the row units_C.json deliberately DROPPED pending its dead-probe
// (context_C.md sect. 6); translated 2026-09-02 after the runtime probe, on the "conductor prefers
// closure over evidence" arm that section reserved. The probe record lives in the ledger row and
// tracker LT1C.
//
// WHAT IT COMPUTES. Called once per planet-map session init (llm_strat_planet_map_session_init
// @0x004dc65a, its ONLY caller -- map load / land path). For every cfg building TYPE b in
// 1..cfg_building_sec->total INCLUSIVE (JBE @0x004dc53a, unsigned), it derives an 8x8 byte grid
// _G_LLM_STRAT_BLDG_CELL_GRID[b] plus a row shift _G_LLM_STRAT_BLDG_CELL_GRID_ROW_SHIFT[b]:
//
//   1. clear grid[b][0..7][0..7] = 0                                   (0x004dc278..0x004dc29e)
//   2. row_shift[b] = 0; if Building[b].type == 0x1c -> row_shift[b] = -1
//                                                                      (0x004dc2a0..0x004dc2c6)
//   3. stamp the 5x5 cfg footprint: for dy,dx in 0..4, if Building[b].area[dy][dx] != 0 ->
//      grid_flat[b*64 + (dy + 1 - row_shift[b])*8 + (dx + 1)] = 1      (0x004dc2c8..0x004dc30e)
//      (base literal 0xfb34b1 = grid+1: the +1,+1 offset centres the footprint with a 1-cell
//      halo margin; a -1 row shift moves the stamp DOWN one further row)
//   4. halo: for dy,dx in 0..7 with grid[cell]==0, if any 8-connected neighbour == 1 -> cell = 2.
//      The neighbours are FLAT byte deltas off the running index -- -9,-1,+7,-8,+8,-7,+1,+9 in
//      that ASM order -- each behind its own dy/dx edge guards (so flat == 2D exactly)
//                                                                      (0x004dc310..0x004dc43e)
//   5. type 7 or 0x1b -> stamp rows 4..7 x cols 4..7 = 1               (0x004dc444..0x004dc488)
//   6. type 8         -> stamp rows 4..7 x cols 4..7 = 1               (0x004dc48a..0x004dc4c5)
//   7. type 0x1c      -> stamp rows 0..3 x cols 4..7 = 1               (0x004dc4c7..0x004dc4ff)
//   8. normalize: any nonzero cell -> 1 (the halo's transient 2s collapse)
//                                                                      (0x004dc501..0x004dc531)
//
// GHIDRA GAP (context_C.md G2) -- THE .c DRAFT IS WRONG ABOUT PHASE 4 AND MUST NOT BE FOLLOWED.
// Ghidra renders four of the eight halo probes (0x004dc341/0x004dc3a3/0x004dc3e3/0x004dc360) as
// reads of _G_LLM_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS + 0x1f7.. because the negative literal
// displacements (grid base - 9..-1) fall inside the ADJACENT spiral array's extent. All eight
// probes share base 0xfb34b0 (the grid) at the flat deltas above; at runtime b >= 1 so every
// probe lands inside the grid and there is no out-of-bounds read. Translated from the DISASSEMBLY.
//
// INDEX SLOT 0 IS NEVER TOUCHED: b starts at 1, grid[0..63] and row_shift[0] keep their prior
// bytes. The bound is INCLUSIVE and UNSIGNED (JBE against cfg_building_sec->total) -- total == N
// writes slots 1..N; the arrays are sized [100] and the cfg caps types below that.
//
// NO READER. LT-PREP's 4-method sweep (find-constant-uses, find-constants-in-range, full-decompile
// regex, find-cross-references) found ZERO readers of either output; the decomposition's "AI
// construction scanners consume it" claim was REFUTED (those consume passable[] + Building[].area
// via llm_scan_masked_table_for_empty_cell). The 2026-09-02 runtime probe (ini-gated writer
// poison, [probe] bldg_cell_grid_poison -- net_seams.cpp) is the runtime leg of that negative
// claim. The cache is a DEAD STORE in the shipped game; the body is still owned here because the
// function EXECUTES on every planet load (a `state: dead` ledger row would arm an X-TOMB trap on
// a live entry -- the build_target_list false-fire precedent, net_seams.cpp ~1980).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// Pure leaf: reads v.cfg_buildings / v.cfg_building_sec->total, writes own.bldg_cell_grid_byte()
// / own.bldg_cell_grid_row_shift_at(). No outward call, no `_calls` struct.
void bldg_recompute_cell_grid(const sim_view &v, sim_store &own);


} // namespace detail

// Public wrapper matching the committed __watcall(void) export shape.
void bldg_recompute_cell_grid();

} // namespace mh::sim
