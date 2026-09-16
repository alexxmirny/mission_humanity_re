#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. Indirected for the same reason as every other module
// here: a direct mh::call:: inside a detail:: body reaches into the live game image, which makes the
// body untestable by net_selftest.exe simtest.
struct cfg_apply_project_resources_calls {
    void (*resource_add)(int32_t player, int32_t resource_id, int32_t amount); // llm_resource_add @0x00497f4a
};

const cfg_apply_project_resources_calls &live_cfg_apply_project_resources_calls();

namespace detail {

// llm_cfg_apply_project_resources @0x00492e35. See the header banner above for the full derivation.
void cfg_apply_project_resources(const sim_view &v, const cfg_apply_project_resources_calls &c,
                                 uint32_t player_id, uint32_t project_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (sig_llm_cfg_apply_project_resources) exactly.

void cfg_apply_project_resources(uint32_t player_id, uint32_t project_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
