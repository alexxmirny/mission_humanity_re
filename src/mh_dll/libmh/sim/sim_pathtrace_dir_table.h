#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_pathtrace_dirs_get @0x0066b415. Returns the raw address of _G_LLM_STRAT_PATHTRACE_DIRS[0]
// through the tracked store, matching trace_greedy_path's own `&own.pathtrace_dir_at(0)` idiom
// (sim_pathtrace_greedy.cpp:277) rather than a literal VA. No sim_view read at all, so (matching
// sim_pathfind_grid_geometry.h's dir_step_factor precedent for a state-free helper) this takes only the
// mutable store.
uint8_t *pathtrace_dirs_get(sim_store &own);

// llm_strat_pathtrace_normalize_repeat_dir_table @0x0066b4a1. See the header banner above for the full
// derivation. Splices repeated-diagonal-direction runs in place; writes into pathtrace_dirs/
// pathtrace_pos only (never pathtrace_len).
void pathtrace_normalize_repeat_dir_table(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state(). pathtrace_dirs_get matches addr/mh_calls.gen.h's own
// committed `uint8_t *llm_strat_pathtrace_dirs_get()` signature (TACT1-P C6, 2026-09-04; no separate
// sig_ typedef exists -- MH_EXPORT_REPLACE_llm_strat_pathtrace_dirs_get is a hard static_assert(false,
// "...too small to host a detour") in addr/mh_export.gen.h, see the header banner).
// pathtrace_normalize_repeat_dir_table matches sig_llm_strat_pathtrace_normalize_repeat_dir_table
// (void(__cdecl*)(void)) exactly.
uint8_t *pathtrace_dirs_get();
void     pathtrace_normalize_repeat_dir_table();

namespace detail {
} // namespace detail

} // namespace mh::sim
