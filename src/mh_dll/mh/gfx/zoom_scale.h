//
// gfx/zoom_scale.h -- the map-zoom resampler: stretch a crop of the framebuffer's viewport over the whole
// viewport, in place, with one of the four [video] filters.
//
// The stock zoom (llm_gfx_bitmap_resample_center_out, 0x004a82b0) blends neighbours with an RGB565 halve
// mask and can only expand the CENTRED crop. This is the same job for an arbitrary crop origin, with the
// same kernels the presenter and the background scaler use (gfx/cpu_scale.h): point, linear, sharp
// (sharp-bilinear-simple) and area (box). The kernels are applied the way cpu_scale::scale_image defines
// them, but in fixed point so a 2560x1440 frame is a few milliseconds instead of a few hundred:
//
//   * per-axis taps and weights come from cpu_scale::axis_weights, once per output column / row, the
//     weight quantised to 1/32;
//   * two separable passes -- the crop's rows are blended horizontally into a scratch image first, then
//     the output rows blend two scratch rows -- so the crop's pixels are read once and the output
//     (the large side) pays one lerp per pass;
//   * a lerp is SWAR on the packed pixel: green is moved to its own lane (p | p << 16) & mask, all three
//     channels blend in one 32-bit multiply, and the result is re-packed. 5 bit weights, rounded.
//
// The result is within two levels of cpu_scale::scale_image in every channel, under one on average, and
// exact for point (the `mapzoomtest` suite measures it, all four filters, both pixel formats): the weights
// are 1/32 steps and the blended crop is held in 16 bits between the passes. Pure: no Windows, no game
// memory.
//
#pragma once
#include <stdint.h>

namespace mh::gfx::zoom {

// Stretch the crop (crop_x, crop_y, cw, ch) of the 16-bit image at `fb` (row pitch `pitch_px` PIXELS) over
// its top-left dw x dh area, IN PLACE. The crop and the output area may overlap: the crop is consumed into
// a scratch image before the first output row is written. Clamp addressing inside the crop. `filter` is
// cpu_scale's numbering (0 point, 1 linear, 2 sharp, 3 area). Returns false -- touching nothing -- on a
// non-positive size, a crop wider/taller than the output, or a null buffer.
bool stretch_crop(uint16_t *fb, int pitch_px, int crop_x, int crop_y, int cw, int ch, int dw, int dh, int filter,
                  bool rgb555);

// Release the scratch and table buffers (selftest teardown; the game keeps them for the process).
void release_buffers();

} // namespace mh::gfx::zoom
