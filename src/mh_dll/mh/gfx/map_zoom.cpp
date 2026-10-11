//
// gfx/map_zoom.cpp -- see map_zoom.h.
//
#include "map_zoom.h"

#include <math.h>

namespace mh::gfx::mapzoom {

double max_zoom(int render_h) {
    const double m = 2.0 * (double)render_h / 768.0;
    return m < 2.0 ? 2.0 : m;
}

int crop_dim(int win, double z) {
    if (z < 1.0) z = 1.0;
    int c = (int)((double)win / z) & ~1;
    if (c < 2) c = 2;
    return c > win ? win : c;
}

int slack(int win, int crop) {
    const int s = win - crop;
    return s < 0 ? 0 : s;
}

int clamp_origin(int left, int win, double z) {
    const int s = slack(win, crop_dim(win, z));
    return left < 0 ? 0 : left > s ? s
                                   : left;
}

void anchor_axis(int left, int win, double z_old, double z_new, int c, int *out_left, int *cam_tiles) {
    const int    cw_old = crop_dim(win, z_old), cw_new = crop_dim(win, z_new);
    const double u     = (double)left + (double)c * cw_old / win; // the source pixel under the cursor
    const double want  = u - (double)c * cw_new / win;            // where the new crop must start to keep it there
    int          L     = (int)floor(want + 0.5);
    const int    sl    = slack(win, cw_new);
    int          tiles = 0;
    if (L < -ABSORB_SLACK_PX) {
        const int k = (-L + TILE_PX - 1) / TILE_PX; // camera back k tiles, give 32 px each back to the crop
        tiles       = -k;
        L += k * TILE_PX;
    } else if (L > sl + ABSORB_SLACK_PX) {
        const int k = (L - sl + TILE_PX - 1) / TILE_PX;
        tiles       = k;
        L -= k * TILE_PX;
    }
    *out_left  = L < 0 ? 0 : L > sl ? sl
                                    : L;
    *cam_tiles = tiles;
}

int translate_axis(int x, int win, int left, int crop) {
    if (win <= 0) return x;
    const int xp = left + (int)((long long)x * crop / win);
    return xp == left ? 0 : xp;
}

int wheel_notches(wheel_acc &acc, int delta) {
    if ((delta > 0 && acc.residual < 0) || (delta < 0 && acc.residual > 0)) acc.residual = 0;
    acc.residual += delta;
    const int n = acc.residual / WHEEL_NOTCH;
    acc.residual -= n * WHEEL_NOTCH;
    return n;
}

} // namespace mh::gfx::mapzoom
