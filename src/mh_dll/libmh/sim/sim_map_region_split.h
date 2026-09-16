//
// sim/sim_map_region_split.h -- llm_map_region_split (RI-SIM / SIM1F).
//
// One function: llm_map_region_split @0x00423fb8 (0x44e B), `llm_map_region *
// __mh_watcall_ebx_volatile llm_map_region_split(llm_map_region *old_region, int seed_col, int
// seed_row)`. Allocates ONE fresh llm_map_region node via map::block_8::GetNextBlock(), seeds it at
// (seed_col, seed_row), then 4-neighbor wrapped-BFS-floods outward through the private
// _G_LLM_MAP_BFS_QUEUE_REGIONSPLIT scratch, absorbing every cell still owned by `old_region` into the
// new node and growing its cell_count. Returns the new region.
//
// ---- THE REGION-POOL ARCHITECTURE (see tmp/decomp/_CONTEXT_SIM1F_4.md for the full derivation) ----
//
// `llm_map_region` is a HEAP object. The node this function allocates is dereferenced DIRECTLY
// (`new_region->x`, `->cell_count`, ...) -- there is no sim_store accessor for a region node's own
// fields, by design (same posture as any other pointer-chasing C++ over live, shared-process heap
// memory). What DOES go through sim_store: `region_cell_at(x, y)` (the fixed-VA 256x256 grid a cell's
// OWNING region pointer lives in) and `region_bfs_queue()` (this function's own private flood-fill
// scratch, `_G_LLM_MAP_BFS_QUEUE_REGIONSPLIT[2048]`, an ADDRESS ESCAPE like `text_scratch()`).
//
// ---- QUEUE-INDEX WRAP: FAITHFUL, NOT DEFENDED -------------------------------------------------
// The read/write queue indices wrap at `& 0x7ff` (2048 entries). The ORIGINAL never checks for queue
// overflow (a flood claiming more than 2048 live cells before the read index catches up would
// silently corrupt earlier, not-yet-processed entries) -- this translation reproduces that exactly
// and adds no bounds check. See the batch context doc's "Queue index wraps..." hazard note.
//
// ---- THE SEED CELL IS NOT WRAP-MASKED, UNLIKE EVERY BFS-DERIVED NEIGHBOR ------------------------
// 0x00423ffb-0x0042400c computes the seed cell's grid address straight from the full (unmasked)
// seed_col/seed_row parameters -- `(seed_col << 11) + (seed_row << 3)` over the region-cell stride,
// with NO `& width_m` / `& height_m` anywhere in that instruction range (unlike every one of the
// four BFS neighbor computations below it, which mask both axes before indexing). A caller passing an
// out-of-[0, MAP_GRID_DIM) seed produces the same out-of-bounds grid write here that the original's
// raw pointer arithmetic would -- not a bug to fix in this translation.
//
// ---- THE SHADOW-ARM ALLOCATION HAZARD (conductor's problem, not this translation's) --------------
// This function calls map::block_8::GetNextBlock() for a FRESH heap pointer on every call. Under a
// shadow arm BOTH arms call the real allocator for real (it is not stubbed), so the original arm gets
// pointer X and this translation's arm gets a DIFFERENT pointer Y from the advanced free-list/heap
// state -- comparing raw REGION_GRID cell bytes between arms would show X vs Y and read as a
// divergence even when the logic is identical. Translated faithfully regardless, per the batch
// context doc; the conductor decides separately whether this site gets a live rig run or an
// offline-fixture-only oracle.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other multi-callee TU in this batch: a direct mh::call::
// inside a detail:: body reaches into the live game image, which makes the body untestable by
// net_selftest.exe simtest. The one callee is the ORIGINAL allocator (Law 4 -- a shared allocator
// used by every region function, not reimplemented in this migration set; see the context doc).
struct map_region_split_calls {
    mh::game::mh_llm_map_region *(*get_next_block)(); // map::block_8::GetNextBlock @0x004222cf -- typed by the committed prototype (was void*; synced 2026-08-19)
};

const map_region_split_calls &live_map_region_split_calls();

namespace detail {

// llm_map_region_split @0x00423fb8. See the header hazards above for the seed-vs-neighbor masking
// asymmetry and the queue-wrap posture. `old_region` is never dereferenced by the original, only
// compared by raw pointer value against each candidate cell's owner -- reproduced identically here.
llm_map_region *region_split(const sim_view &v, sim_store &own, const map_region_split_calls &c,
                             llm_map_region *old_region, int32_t seed_col, int32_t seed_row);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// `__mh_watcall_ebx_volatile` shape (sig_llm_map_region_split, addr/mh_export.gen.h) up to the typed
// vs. `void *` pointer representation -- callers inside this migration set that already hold a typed
// `llm_map_region *` (llm_map_region_apply_area / map_ApplyAreaToMap, this slice's own siblings) call
// this overload directly, per the standing sibling-calls-through-`mh::call::`-only-for-UNTRANSLATED-
// originals rule; the shadow arm below is the one that must match the raw `void *` export signature.
llm_map_region *region_split(llm_map_region *old_region, int32_t seed_col, int32_t seed_row);

namespace detail {
} // namespace detail

} // namespace mh::sim
