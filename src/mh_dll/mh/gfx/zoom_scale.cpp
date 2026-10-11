//
// gfx/zoom_scale.cpp -- see zoom_scale.h.
//
#include "zoom_scale.h"

#include "cpu_scale.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace mh::gfx::zoom {

namespace {

// ---- the packed blend ------------------------------------------------------------------------------
//
// expand(p) puts the three channels in disjoint lanes of a 32-bit word with 5+ spare bits above each,
// so one multiply by a 0..32 weight scales all three without a carry into its neighbour:
//   RGB565: b bits 0-4, r bits 11-15, g (copied up by 16) bits 21-26;   mask 0x07E0F81F
//   RGB555: b bits 0-4, r bits 10-14, g (copied up by 16) bits 21-25;   mask 0x03E07C1F
// A lane after `a*(32-w) + b*w` holds at most 31*32 (63*32 for g): 10 bits (11 for g), inside its gap.
// +16 per lane rounds to nearest; >>5 returns to channel scale; the mask drops the fractional bits that
// the shift left in the gaps; (x | x >> 16) puts green back.
struct fmt {
    uint32_t mask;
    uint32_t round;
};
constexpr fmt F565 = {0x07E0F81Fu, 16u | (16u << 11) | (16u << 21)};
constexpr fmt F555 = {0x03E07C1Fu, 16u | (16u << 10) | (16u << 21)};

inline uint32_t expand(uint16_t p, uint32_t mask) {
    return ((uint32_t)p | ((uint32_t)p << 16)) & mask;
}

// w in [0, 32]: 0 = a, 32 = b.
inline uint16_t lerp(uint16_t a, uint16_t b, uint32_t w, const fmt &f) {
    const uint32_t x = ((expand(a, f.mask) * (32u - w) + expand(b, f.mask) * w + f.round) >> 5) & f.mask;
    return (uint16_t)(x | (x >> 16));
}

// ---- process-lifetime buffers ----------------------------------------------------------------------

struct axis_tab {
    int     *k0 = nullptr, *k1 = nullptr;
    uint8_t *w   = nullptr;
    int      cap = 0;
};
struct buffers {
    axis_tab  x, y;
    uint16_t *rows     = nullptr; // the horizontally blended crop: ch rows of dw pixels
    size_t    cap_rows = 0;
};
buffers g;

bool ensure(axis_tab &t, int need) {
    if (need <= t.cap) return true;
    int *a = (int *)realloc(t.k0, (size_t)need * sizeof(int));
    if (a) t.k0 = a;
    int *b = a ? (int *)realloc(t.k1, (size_t)need * sizeof(int)) : nullptr;
    if (b) t.k1 = b;
    uint8_t *c = b ? (uint8_t *)realloc(t.w, (size_t)need) : nullptr;
    if (c) t.w = c;
    if (!c) return false;
    t.cap = need;
    return true;
}

// One axis: for each output index o in [0, n_out), the two clamped source taps and the second tap's
// weight in 1/32 -- the table cpu_scale::scale_image builds in doubles.
void build_axis(int filter, int n_out, int n_in, int *k0, int *k1, uint8_t *w) {
    const double scale = (double)n_out / n_in; // output pixels per source texel
    for (int o = 0; o < n_out; ++o) {
        int    k;
        double w1;
        cpu_scale::axis_weights(filter, (o + 0.5) / scale, scale, &k, &w1);
        int a = k < 0 ? 0 : k >= n_in ? n_in - 1
                                      : k;
        int b = k + 1 < 0 ? 0 : k + 1 >= n_in ? n_in - 1
                                              : k + 1;
        k0[o] = a;
        k1[o] = b;
        int q = (int)(w1 * 32.0 + 0.5);
        w[o]  = (uint8_t)(q < 0 ? 0 : q > 32 ? 32
                                             : q);
    }
}

} // namespace

bool stretch_crop(uint16_t *fb, int pitch_px, int crop_x, int crop_y, int cw, int ch, int dw, int dh, int filter,
                  bool rgb555) {
    if (!fb || cw <= 0 || ch <= 0 || dw < cw || dh < ch || pitch_px < dw || crop_x < 0 || crop_y < 0) return false;
    if (filter < 0 || filter > 3) filter = 0;
    if (!ensure(g.x, dw) || !ensure(g.y, dh)) return false;
    const size_t need_rows = (size_t)ch * (size_t)dw;
    if (need_rows > g.cap_rows) {
        uint16_t *n = (uint16_t *)realloc(g.rows, need_rows * sizeof(uint16_t));
        if (!n) return false;
        g.rows     = n;
        g.cap_rows = need_rows;
    }
    build_axis(filter, dw, cw, g.x.k0, g.x.k1, g.x.w);
    build_axis(filter, dh, ch, g.y.k0, g.y.k1, g.y.w);
    const fmt &f = rgb555 ? F555 : F565;

    // pass 1: the crop's rows, blended horizontally to the output width.
    for (int sy = 0; sy < ch; ++sy) {
        const uint16_t *src = fb + (size_t)(crop_y + sy) * pitch_px + crop_x;
        uint16_t       *out = g.rows + (size_t)sy * dw;
        for (int ox = 0; ox < dw; ++ox) {
            const uint32_t w = g.x.w[ox];
            out[ox]          = w == 0 ? src[g.x.k0[ox]] : lerp(src[g.x.k0[ox]], src[g.x.k1[ox]], w, f);
        }
    }
    // pass 2: the output rows (the crop is consumed, so writing over it is safe).
    for (int oy = 0; oy < dh; ++oy) {
        const uint16_t *ra  = g.rows + (size_t)g.y.k0[oy] * dw;
        const uint16_t *rb  = g.rows + (size_t)g.y.k1[oy] * dw;
        uint16_t       *dst = fb + (size_t)oy * pitch_px;
        const uint32_t  w   = g.y.w[oy];
        if (w == 0) {
            memcpy(dst, ra, (size_t)dw * sizeof(uint16_t));
        } else {
            for (int ox = 0; ox < dw; ++ox) dst[ox] = lerp(ra[ox], rb[ox], w, f);
        }
    }
    return true;
}

void release_buffers() {
    free(g.x.k0);
    free(g.x.k1);
    free(g.x.w);
    free(g.y.k0);
    free(g.y.k1);
    free(g.y.w);
    free(g.rows);
    g = buffers();
}

} // namespace mh::gfx::zoom
