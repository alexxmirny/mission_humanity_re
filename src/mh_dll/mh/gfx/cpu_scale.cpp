//
// gfx/cpu_scale.cpp -- see cpu_scale.h. Moved here from net_selftest's scale_filter_selftest.cpp so the
// background scaler in seams/video_mode_guards.cpp and the selftest's reference are ONE implementation.
//
#include "cpu_scale.h"

#include <math.h>

namespace mh::gfx::cpu_scale {

void axis_weights(int f, double p, double scale, int *k0, double *w1) {
    if (f == f_point) {
        *k0 = (int)floor(p);
        *w1 = 0.0;
        return;
    }
    double t = scale; // the ramp slope in output px per texel
    if (f == f_linear) t = 1.0;
    if (f == f_sharp) t = floor(scale) < 1.0 ? 1.0 : floor(scale);
    if (f == f_area && t < 1.0) t = 1.0;
    // Nearest texel edge e = round(p). The next texel's weight ramps linearly across the edge:
    // w = clamp((p - e) * t + 0.5, 0, 1), with texel e-1 on the left and e on the right. For t = 1 that
    // is exactly bilinear between centres e-0.5 and e+0.5; for the true scale it is the box filter.
    const double e = floor(p + 0.5);
    double       w = (p - e) * t + 0.5;
    w              = w < 0.0 ? 0.0 : w > 1.0 ? 1.0
                                             : w;
    *k0            = (int)e - 1;
    *w1            = w;
}

rgbf texel(const uint16_t *s, int fw, int fh, bool rgb555, int x, int y) {
    x                = x < 0 ? 0 : x >= fw ? fw - 1
                                           : x;
    y                = y < 0 ? 0 : y >= fh ? fh - 1
                                           : y;
    const uint16_t p = s[(size_t)y * fw + x];
    if (rgb555) return {(float)((p >> 10) & 31) / 31.0f, (float)((p >> 5) & 31) / 31.0f, (float)(p & 31) / 31.0f};
    // B5G6R5_UNORM reads as v / (2^n - 1); the 8-bit render target stores round(c * 255).
    return {(float)(p >> 11) / 31.0f, (float)((p >> 5) & 63) / 63.0f, (float)(p & 31) / 31.0f};
}

namespace {

rgbf blend(const uint16_t *s, int fw, int fh, bool rgb555, int filter, int x0, double wx, int y0, double wy) {
    if (filter == f_point) return texel(s, fw, fh, rgb555, x0, y0);
    const rgbf a = texel(s, fw, fh, rgb555, x0, y0), b = texel(s, fw, fh, rgb555, x0 + 1, y0);
    const rgbf c = texel(s, fw, fh, rgb555, x0, y0 + 1), d = texel(s, fw, fh, rgb555, x0 + 1, y0 + 1);
    auto       mix = [&](float u, float v, float z, float q) {
        return (float)((1 - wy) * ((1 - wx) * u + wx * v) + wy * ((1 - wx) * z + wx * q));
    };
    return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b)};
}

} // namespace

rgbf sample(const uint16_t *s, int fw, int fh, bool rgb555, int filter, double px, double py, double sx, double sy) {
    int    x0, y0;
    double wx, wy;
    axis_weights(filter, px, sx, &x0, &wx);
    axis_weights(filter, py, sy, &y0, &wy);
    return blend(s, fw, fh, rgb555, filter, x0, wx, y0, wy);
}

bool scale_image(const uint16_t *src, int iw, int ih, uint16_t *dst, int ow, int oh, int filter, bool rgb555) {
    if (!src || !dst || iw <= 0 || ih <= 0 || ow <= 0 || oh <= 0) return false;
    const double sx = (double)ow / iw, sy = (double)oh / ih;
    // The per-axis weights depend on one coordinate each: tabulate them once instead of per pixel.
    int    *x0 = new int[(size_t)ow];
    double *wx = new double[(size_t)ow];
    for (int ox = 0; ox < ow; ++ox) axis_weights(filter, (ox + 0.5) / sx, sx, &x0[ox], &wx[ox]);
    for (int oy = 0; oy < oh; ++oy) {
        int    y0;
        double wy;
        axis_weights(filter, (oy + 0.5) / sy, sy, &y0, &wy);
        uint16_t *row = dst + (size_t)oy * ow;
        for (int ox = 0; ox < ow; ++ox) {
            const rgbf c = blend(src, iw, ih, rgb555, filter, x0[ox], wx[ox], y0, wy);
            if (rgb555) {
                const unsigned r = (unsigned)(c.r * 31.0f + 0.5f), g = (unsigned)(c.g * 31.0f + 0.5f),
                               b = (unsigned)(c.b * 31.0f + 0.5f);
                row[ox]          = (uint16_t)((r << 10) | (g << 5) | b);
            } else {
                const unsigned r = (unsigned)(c.r * 31.0f + 0.5f), g = (unsigned)(c.g * 63.0f + 0.5f),
                               b = (unsigned)(c.b * 31.0f + 0.5f);
                row[ox]          = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
    delete[] x0;
    delete[] wx;
    return true;
}

} // namespace mh::gfx::cpu_scale
