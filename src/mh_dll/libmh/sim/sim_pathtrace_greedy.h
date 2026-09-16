#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one ORIGINAL outward call llm_strat_pathtrace_remove_loops makes. Indirected through a
// one-member struct for the same offline-testability reason every other sim TU indirects its callees
// (see sim_unit_path_detour.h's banner for the fullest statement of why this applies even to a single
// callee).
struct pathtrace_remove_loops_calls {
    void (*pathtrace_normalize_repeat_dir_table)(); // @0x0066b4a1
};

const pathtrace_remove_loops_calls &live_pathtrace_remove_loops_calls();

namespace detail {

// llm_strat_trace_greedy_path @0x0066a9ac. See the header banner for the full derivation. Returns a
// pointer to the trace's direction buffer (pathtrace_dirs[0]), matching the original's `byte *` return
// (the caller walks it via llm_strat_pathtrace_dirs_get, a SEPARATE frontier accessor over the same
// storage -- not reimplemented here).
uint8_t *trace_greedy_path(const sim_view &v, sim_store &own, int32_t start_col, int32_t start_row,
                           int32_t mode, int32_t goal_col, int32_t goal_row, int32_t heading);

// llm_strat_pathtrace_remove_loops @0x0066b376. See the header banner for the full derivation.
int32_t pathtrace_remove_loops(const sim_view &v, sim_store &own,
                               const pathtrace_remove_loops_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() (and live_pathtrace_remove_loops_calls() for the second
// one). Match the committed prototypes (sig_llm_strat_trace_greedy_path /
// sig_llm_strat_pathtrace_remove_loops in addr/mh_export.gen.h) exactly.
uint8_t *trace_greedy_path(int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                           int32_t goal_row, int32_t heading);
int32_t  pathtrace_remove_loops();

namespace detail {
} // namespace detail

} // namespace mh::sim
