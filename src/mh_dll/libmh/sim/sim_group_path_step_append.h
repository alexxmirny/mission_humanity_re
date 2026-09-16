//
// sim/sim_group_path_step_append.h -- one function (RI-SIM / SIM1-G2):
//
//   llm_strat_group_path_step_append @0x0041fdda (0xcd bytes)
//
// `void __watcall llm_strat_group_path_step_append(int order_idx, uint heading)`, matching the
// committed prototype exactly. Already called by an EARLIER slice's `group_path_step_record`
// (sim_path_group_steps.cpp) via `mh::call::` -- that TU is unchanged by this one (Law 4 / the
// established "cross-TU calls always go through mh::call::/a _calls struct" convention; this
// function's own translation does not retrofit its caller).
//
// Appends (heading, run_length=1) to the group's path build cursor, or -- if the cursor already
// points at an entry with the SAME heading -- extends that entry's run_length by 1 instead (a simple
// run-length-encode step, the sibling operation to `group_path_step_record`'s own richer coalescing
// logic in the same PATH_BUFFERS array). `order_idx` is the group's PATH SLOT id (not a "path build
// slot" abstraction -- passed straight into `path_buffer_at`'s `slot_id` parameter, same indexing
// `group_path_step_record` uses for its own identically-named parameter), `owner` is
// `*v.group_order_owner`, and the write cursor is `own.group_path_build_idx_mut()`.
//
// THE PREVIOUS-ENTRY READ IS GATED (0x0041fdf7-0x0041fdfe: `build_idx <= 0` skips straight to the
// append path) -- UNLIKE its sibling `group_path_step_record`, which reads/writes entry
// `build_idx-1` UNCONDITIONALLY and underflows into the adjacent `Anim` region when `build_idx==0`.
// Here `build_idx-1` is only ever read/written once `build_idx>0` is already established, so the
// offset can never actually resolve into `Anim` at runtime -- see the .cpp for why `Anim` is still
// declared as an extra_region (the mechanical write-closure tool flags it from the raw instruction
// operand alone, not path-sensitively).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_group_path_step_append @0x0041fdda. Writes _G_LLM_STRAT_PATH_BUFFERS and
// _G_LLM_STRAT_GROUP_PATH_BUILD_IDX -- both already-bound sim_store accessors. No callees.
void group_path_step_append(const sim_view &v, sim_store &own, int32_t order_idx, uint32_t heading);


} // namespace detail

// Public wrapper. Matches the committed prototype (mh::call::llm_strat_group_path_step_append)
// exactly.
void group_path_step_append(int32_t order_idx, uint32_t heading);

} // namespace mh::sim
