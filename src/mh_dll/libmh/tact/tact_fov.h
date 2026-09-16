#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

namespace detail {

void fov_update_nearest_target(tact_store &own, const tact_view &v,
                               const mh::state::mode_planes &planes, int32_t col, int32_t row);

// llm_tact_fov_raycast_stencil @0x00436f30. `v` is passed through only for fov_candidate_dist,
// consumed inside fov_update_nearest_target -- every other scratch input this function reads/writes
// is a tact_store-only member (see header banner), read via `own`.
void fov_raycast_stencil(const tact_view &v, tact_store &own, const mh::state::mode_planes &planes);

// llm_tact_vision_cone_setup @0x0042e1c9. dead_outptr0..3 are accepted for signature parity and
// never dereferenced (see header banner).
void vision_cone_setup(const tact_view &v, tact_store &own, const mh::state::mode_planes &planes,
                       int32_t col, int32_t row, int32_t angle_base, int32_t angle_width,
                       int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                       void *dead_outptr2, void *dead_outptr3);


} // namespace detail

void vision_cone_setup(int32_t col, int32_t row, int32_t angle_base, int32_t angle_width,
                       int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                       void *dead_outptr2, void *dead_outptr3);

// DECLARED HERE so another TU's rebind can name it: the committed row spells `angle_width`
// unsigned, the wrapper above spells it int32_t.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {
void vision_cone_setup(int32_t col, int32_t row, int32_t angle_base, uint32_t angle_width,
                       int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                       void *dead_outptr2, void *dead_outptr3);
} // namespace rebind_arm

void fov_raycast_stencil();

} // namespace mh::tact
