#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls, shared by pathfind_build_steps and group_path_step_record --------------------
// unit_path_queue_splice makes no outward calls and needs none of this (see sim_diplomacy_ai_relation_
// swap.cpp's identical precedent for a view-less, calls-less write-only detail:: body).
struct path_group_steps_calls {
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);    // 0x0049404e
    int32_t (*pathfind_dir_code_from_delta)(int32_t d_col_sign, int32_t d_row_sign); // 0x0041e790
    void (*group_path_step_append)(int32_t order_idx, uint32_t heading);             // 0x0041fdda
};

const path_group_steps_calls &live_path_group_steps_calls();

namespace detail {

// llm_strat_unit_path_queue_splice @0x00421758. `unit_index` is the PATH SLOT ID (see header banner) --
// kept named to match the committed prototype (sig_llm_strat_unit_path_queue_splice) exactly, same
// precedent as sim_unit_path_queue_count.h's own "unit_index" parameter.
int32_t unit_path_queue_splice(sim_store &own, int32_t owner_index, int32_t unit_index, int32_t slot,
                               int32_t count);

// llm_strat_pathfind_build_steps @0x0041e836. See the header banner for the full derivation.
int32_t pathfind_build_steps(const sim_view &v, sim_store &own, const path_group_steps_calls &c,
                             int32_t start_x, int32_t start_y, int32_t target_range,
                             uint32_t unused_reserved, int32_t path_slot_index);

// llm_strat_group_path_step_record @0x0041f7ef. `order_idx` is the PATH SLOT ID (see header banner).
void group_path_step_record(const sim_view &v, sim_store &own, const path_group_steps_calls &c,
                            int32_t order_idx, uint32_t heading, int32_t col, int32_t row);

} // namespace detail

// Live wrappers: the logic applied to state() and live_path_group_steps_calls(). Signatures match the
// committed prototypes exactly (sig_llm_strat_unit_path_queue_splice / sig_llm_strat_pathfind_build_steps
// / sig_llm_strat_group_path_step_record, addr/mh_export.gen.h).
int32_t unit_path_queue_splice(int32_t owner_index, int32_t unit_index, int32_t slot, int32_t count);
int32_t pathfind_build_steps(int32_t start_x, int32_t start_y, int32_t target_range,
                             uint32_t unused_reserved, int32_t path_slot_index);
void    group_path_step_record(int32_t order_idx, uint32_t heading, int32_t col, int32_t row);

namespace detail {
} // namespace detail

} // namespace mh::sim
