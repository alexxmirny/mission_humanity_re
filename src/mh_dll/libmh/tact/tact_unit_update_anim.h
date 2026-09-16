#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The three outward calls this function makes, indirected for offline testability (see the PROOF
// note above -- `rand` is a TACT-CUT2 ungated-effectful callee, so ALL THREE go through this struct,
// not just it).
struct unit_update_anim_calls {
    double (*time_now)(); // time_GetCurrentTime @0x00427616
    int32_t (*rand)();    // llm_rand @0x004da98b
    int32_t (*quantize_facing_dir)(int32_t notch_span,
                                   int32_t facing_dir); // llm_tact_quantize_facing_dir @0x0042d0cf
};

const unit_update_anim_calls &live_unit_update_anim_calls();

namespace detail {

// llm_tact_unit_update_anim @0x0042c547. See the header banner above for the full derivation.
void unit_update_anim(const tact_view &tv, tact_store &own, const unit_update_anim_calls &c,
                      int32_t unit_idx);

} // namespace detail

void unit_update_anim(int32_t unit_idx);


} // namespace mh::tact
