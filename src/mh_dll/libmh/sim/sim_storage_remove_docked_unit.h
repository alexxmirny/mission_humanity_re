#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (llm_strat_bldg_notify_ui @0x00470bdd, already
// committed in mh_calls.gen.h -- not part of this migration slice, stays original), indirected for
// offline testability like every other sim/ TU's `_calls` struct.
struct storage_remove_docked_unit_calls {
    // llm_strat_bldg_notify_ui @0x00470bdd. EAX=player (uint16_t), EDX=b_Index (uint32_t) -- matches
    // mh_calls.gen.h's own signature for this callee exactly.
    void (*notify_ui)(uint16_t player, uint32_t b_index);
};

const storage_remove_docked_unit_calls &live_storage_remove_docked_unit_calls();

namespace detail {

// llm_strat_storage_remove_docked_unit @0x00489dc4. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation.
void storage_remove_docked_unit(const sim_view &v, sim_store &own,
                                const storage_remove_docked_unit_calls &c, uint16_t player,
                                int32_t unit_index, int32_t storage_slot);

} // namespace detail

// Live wrapper: the logic applied to state() and live_storage_remove_docked_unit_calls(). Matches the
// committed prototype (sig_llm_strat_storage_remove_docked_unit) exactly.
void storage_remove_docked_unit(uint16_t player, int32_t unit_index, int32_t storage_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
