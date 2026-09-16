//
// tact/tact_map_bounds.cpp -- see tact_map_bounds.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_map_bounds.h"


namespace mh::tact {
namespace detail {

void map_compute_bounds(tact_view &v, tact_store &own) {
    const uint16_t *sprites = *v.map_tile_height_sprites;

    // @0x0043a08e (outer, a_idx -> HEIGHT_CACHE) / @0x0043a0ab (middle, b_idx -> WIDTH_CACHE) /
    // @0x0043a0cf (innermost, k 0..7). Neither cache is reset first -- see the header banner.
    for (int32_t a_idx = 0; a_idx < TACT_MAP_DIM; ++a_idx) {
        for (int32_t b_idx = 0; b_idx < TACT_MAP_DIM; ++b_idx) {
            bool found = false;
            for (int32_t k = 0; k < 8; ++k) {
                // @0x0043a0df-0x0043a0fb: addr = sprites + b_idx*2048 + a_idx*16 + k*2 (bytes) ==
                // sprites_u16[b_idx*1024 + a_idx*8 + k].
                if (sprites[b_idx * 1024 + a_idx * 8 + k] != 0) {
                    found = true;
                }
            }
            if (found) {
                // @0x0043a10a-0x0043a11d: WIDTH_CACHE = max(WIDTH_CACHE, b_idx).
                if (b_idx > own.map_width()) {
                    own.map_width() = b_idx;
                }
                // @0x0043a125-0x0043a138: HEIGHT_CACHE = max(HEIGHT_CACHE, a_idx).
                if (a_idx > own.map_height()) {
                    own.map_height() = a_idx;
                }
            }
        }
    }

    // @0x0043a14a/0x0043a150: the running max becomes a COUNT.
    ++own.map_width();
    ++own.map_height();

    mh::state::mode_planes &planes = own.planes();

    // @0x0043a156-0x0043a193: clear a one-tile boundary strip on `passable` at the new edge, each
    // block gated by its OWN cache staying under the literal 0x80 the disassembly uses (not the
    // plane's [256][256] extent -- see the header banner).
    if (own.map_height() < 0x80) {
        for (int32_t i = 0; i < own.map_width(); ++i) {
            planes.passable_at(i, own.map_height()) = 0;
        }
    }
    if (own.map_width() < 0x80) {
        for (int32_t j = 0; j < own.map_height(); ++j) {
            planes.passable_at(own.map_width(), j) = 0;
        }
    }
}

} // namespace detail

void map_compute_bounds() {
    tact_state st = state();
    detail::map_compute_bounds(st.read, st.own);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
