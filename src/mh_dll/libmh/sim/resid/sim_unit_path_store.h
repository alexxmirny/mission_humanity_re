#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// three are frontier originals (none of the twelve sim_resid siblings), so they are reached through
// mh::call:: in live_unit_path_store_calls(), never called directly -- translator-brief 3b.
struct unit_path_store_calls {
    void (*path_free_slot)(uint16_t player, int32_t unit_index);                // llm_strat_path_free_slot @0x004969e8
    void (*path_attach_slot)(int32_t player, int32_t unit_index, int32_t slot); // llm_strat_path_attach_slot @0x00496a7b
    uint8_t *(*pathtrace_dirs_get)();                                           // llm_strat_pathtrace_dirs_get @0x0066b415
};

const unit_path_store_calls &live_unit_path_store_calls();

namespace detail {

// llm_strat_unit_assign_path_from_job_result @0x0049508a. Reads path_slot_id/path_job_result through
// `v`/`own`, writes the path buffers/slot flags (via calls)/unit.path_slot_id-adjacent state through
// `own`, reaches path_free_slot/path_attach_slot through `c`. Returns 1 on success, 0 if no free slot.
int32_t assign_path_from_job_result(const sim_view &v, sim_store &own, const unit_path_store_calls &c,
                                    uint16_t player, int32_t unit_index, uint32_t job_result_idx);

// llm_strat_unit_path_store_result @0x00495aa0. `path_slot` is caller-owned (not searched here).
void path_store_result(const sim_view &v, sim_store &own, const unit_path_store_calls &c,
                       uint32_t player, int32_t unit_idx, uint32_t unused1, uint32_t unused2,
                       int32_t path_slot);

// llm_strat_unit_assign_shared_path @0x00495f87. Returns 1 on success, 0 if no free slot.
int32_t assign_shared_path(const sim_view &v, sim_store &own, const unit_path_store_calls &c,
                           uint32_t player, int32_t unit_index, uint8_t start_col, uint8_t start_row);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_path_store_calls().
int32_t unit_assign_path_from_job_result(uint16_t player, int32_t unit_index, uint32_t job_result_idx);
void    unit_path_store_result(uint32_t player, int32_t unit_idx, uint32_t unused1, uint32_t unused2,
                               int32_t path_slot);
int32_t unit_assign_shared_path(uint32_t player, int32_t unit_index, uint8_t start_col,
                                uint8_t start_row);

} // namespace mh::sim
