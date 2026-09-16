#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- (3) llm_strat_dir_step_toroidal_dist's outward calls ------------------------------------------
struct dir_step_toroidal_dist_calls {
    double (*sqrt_fn)(double x); // llm_sqrt @0x004da9c0
};

const dir_step_toroidal_dist_calls &live_dir_step_toroidal_dist_calls();

namespace detail {

void pathfind_mark_group_member_regions(const sim_view &v, sim_store &own);

// llm_strat_pathfind_target_hook_stub @0x004219dd. See the header banner (2) above. Genuine no-op --
// both parameters are stored to locals that are never read again.
void pathfind_target_hook_stub(int32_t target_col, int32_t target_row);

// llm_strat_dir_step_toroidal_dist @0x004cbc9b. See the header banner (3) above. Pure query; `own` is
// non-const only because the two half-extent scratch globals have no const accessor (see banner).
int32_t dir_step_toroidal_dist(const sim_view &v, sim_store &own, const dir_step_toroidal_dist_calls &c,
                               int32_t a_index, int32_t b_index);

} // namespace detail

// Live wrappers: the logic applied to state() and (for (3)) live_dir_step_toroidal_dist_calls(). Match
// the committed prototypes (sig_llm_strat_pathfind_mark_group_member_regions /
// sig_llm_strat_pathfind_target_hook_stub / sig_llm_strat_dir_step_toroidal_dist) exactly.
void    pathfind_mark_group_member_regions();
void    pathfind_target_hook_stub(int32_t target_col, int32_t target_row);
int32_t dir_step_toroidal_dist(int32_t a_index, int32_t b_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
