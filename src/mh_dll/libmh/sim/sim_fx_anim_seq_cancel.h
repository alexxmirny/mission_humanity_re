#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. See the header banner on why this is a one-member struct
// rather than a direct `mh::call::` inside `detail::` -- same precedent as sim_bldg_defense_cost.h.
struct fx_anim_seq_cancel_calls {
    int32_t (*chain_find_tail)(int32_t start_frame); // llm_fx_anim_chain_find_tail @0x0045583c
};

const fx_anim_seq_cancel_calls &live_fx_anim_seq_cancel_calls();

namespace detail {

// llm_fx_anim_seq_cancel @0x004539cc. See the header banner for the full derivation.
void fx_anim_seq_cancel(const sim_view &v, sim_store &own, const fx_anim_seq_cancel_calls &c,
                        int32_t anim_seq_start_frame);

} // namespace detail

// Live wrapper: the logic applied to state() and live_fx_anim_seq_cancel_calls(). Matches the
// committed prototype (mh_calls.gen.h's llm_fx_anim_seq_cancel) exactly.
void fx_anim_seq_cancel(int32_t anim_seq_start_frame);

namespace detail {
} // namespace detail

} // namespace mh::sim
