#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_path_write_from_solver @0x0049581d. See the header banner above for the full derivation.
// `player` is the caller's `param_1 & 0xffff`; `unit_index`/`path_slot_id` are `param_2`/`param_5`
// unmodified; `param_3`/`param_4` are dead (see banner) and kept only to match the committed
// __watcall register layout.
void path_write_from_solver(const sim_view &v, sim_store &own, uint32_t param_1, int32_t unit_index,
                            uint32_t param_3, uint32_t param_4, int32_t path_slot_id);

// llm_strat_path_find_free_slot @0x00495f1b. See the header banner above: a pure query over
// `own.path_slot_flag_at(player, slot)`, returning the first free (0) slot in [0, PATH_SLOTS_PER_PLAYER)
// or -1 if all 100 are occupied.
int32_t path_find_free_slot(sim_store &own, int32_t player);

} // namespace detail

// Live wrappers: the logic applied to state(). Each matches its committed prototype exactly
// (sig_llm_strat_path_write_from_solver / sig_llm_strat_path_find_free_slot).
void    path_write_from_solver(uint32_t param_1, int32_t unit_index, uint32_t param_3, uint32_t param_4,
                               int32_t path_slot_id);
int32_t path_find_free_slot(int32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
