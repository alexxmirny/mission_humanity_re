#include "tact/tact_facing_to_delta.h"

namespace mh::tact {
namespace detail {

void facing_to_delta(const tact_view &v, int32_t facing_dir, int32_t *out_dx, int32_t *out_dy) {
    // @0x004311de-0x004311ee: (facing_dir - 1) / 3, IDIV truncates toward zero -- matches C++'s
    // signed `/` exactly.
    const int32_t idx = (facing_dir - 1) / 3;

    // @0x004311f1-0x00431213: write both deltas out of the fixed 8-entry octant table.
    *out_dx = v.dir8_delta_table[idx].dx;
    *out_dy = v.dir8_delta_table[idx].dy;

    // @0x00431216-0x0043121e: a dead CMP pair, both landing on the same fallthrough regardless of
    // the result -- no Jcc reads either flag. Not reproduced.
}

} // namespace detail

void facing_to_delta(int32_t facing_dir, int32_t *out_dx, int32_t *out_dy) {
    const tact_view v = state().read;
    detail::facing_to_delta(v, facing_dir, out_dx, out_dy);
}

} // namespace mh::tact
