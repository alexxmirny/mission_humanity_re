#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim::detail {

// llm_gfx_bldg_frame_center_offset @0x00450b40. Per-building-TYPE (not instance), pure -- writes only
// through the caller's two out-pointers. See the header banner above for the full derivation.
void gfx_bldg_frame_center_offset(const sim_view &v, uint16_t building_id, int32_t *out_dx,
                                  int32_t *out_dy);

} // namespace mh::sim::detail

namespace mh::sim {

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_gfx_bldg_frame_center_offset) exactly -- this IS the promotion seam (batch rule 2).
void gfx_bldg_frame_center_offset(uint16_t building_id, int32_t *out_dx, int32_t *out_dy);

} // namespace mh::sim
