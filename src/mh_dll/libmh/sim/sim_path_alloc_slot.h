#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct path_alloc_slot_calls {
    void (*path_attach_slot)(int32_t path_group_idx, int32_t entity_id, int32_t slot); // 0x00496a7b
};

const path_alloc_slot_calls &live_path_alloc_slot_calls();

namespace detail {

// llm_strat_path_alloc_slot @0x0049a1ce. See the header banner above for the full derivation. Returns
// the allocated slot index [0,100) on success, or -1 if every slot for `path_group_idx` is busy.
int32_t path_alloc_slot(sim_store &own, const path_alloc_slot_calls &c, int32_t path_group_idx,
                        int32_t entity_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_path_alloc_slot_calls(). Matches the committed
// prototype (sig_llm_strat_path_alloc_slot) exactly.
int32_t path_alloc_slot(int32_t path_group_idx, int32_t entity_id);


} // namespace mh::sim
