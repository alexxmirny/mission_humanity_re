//
// gfx/cpu_scale.h -- CPU reference kernels for the four [video] filters, on RGB565 / RGB555 pixels.
//
// Pure: no Windows, no game memory. The kernels are the ones net_selftest's `scalefiltertest` measures the
// d3d11 presenter against (written from each filter's definition, not from the shader):
//
//   point   nearest texel of the output pixel's centre;
//   linear  bilinear at the output pixel's centre (texel centres at k + 0.5);
//   sharp   sharp-bilinear-simple: per axis prescale k = max(floor(scale), 1), bilinear on the k-times
//           nearest-upscaled image;
//   area    the box filter: an output pixel's weight on each texel is the fraction of its footprint
//           (1/scale texels wide) that texel covers.
//
// X and Y scale independently and work both up and down, so a 1024x768 image can fill a 1280x704 target.
// The filter numbering is `present_backend.h` scale_filter / `ddraw_own.h` owned_filter().
//
#pragma once
#include <stdint.h>

namespace mh::gfx::cpu_scale {

enum filter : int { f_point  = 0,
                    f_linear = 1,
                    f_sharp  = 2,
                    f_area   = 3 };

struct rgbf {
    float r, g, b; // 0..1
};

// Per axis: the two texels to blend (k0 and k0+1) and the second one's weight, for output pixel centre `p`
// in source texels and `scale` = output pixels per source texel on this axis.
void axis_weights(int filter, double p, double scale, int *k0, double *w1);

// One source texel as 0..1 floats; coordinates clamp to the image (clamp addressing).
rgbf texel(const uint16_t *s, int fw, int fh, bool rgb555, int x, int y);

// The filtered colour at source-space centre (px, py) for scale (sx, sy).
rgbf sample(const uint16_t *s, int fw, int fh, bool rgb555, int filter, double px, double py, double sx, double sy);

// Scale the iw x ih image to exactly ow x oh (non-uniform), writing packed pixels in the SAME format as the
// input, tightly packed (pitch = ow * 2). Never reads outside iw*ih pixels. false on a zero dimension.
bool scale_image(const uint16_t *src, int iw, int ih, uint16_t *dst, int ow, int oh, int filter, bool rgb555);

} // namespace mh::gfx::cpu_scale
