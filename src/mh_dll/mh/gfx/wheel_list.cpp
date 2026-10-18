// gfx/wheel_list.cpp -- D15: see wheel_list.h.
#include "gfx/wheel_list.h"

#include "gfx/map_zoom.h" // wheel_acc / wheel_notches: the one accumulator the map zoom uses

namespace mh::gfx::wheel_list {

int feed_notches(feed &f, int total, unsigned now_ms, int *dropped) {
    if (dropped) *dropped = 0;
    const bool gap   = f.have && (now_ms - f.last_ms) > GAP_MS;
    const int  delta = f.have ? total - f.last : 0;
    f.have           = true;
    f.last           = total;
    f.last_ms        = now_ms;
    if (gap) {
        f.residual = 0;
        if (dropped) *dropped = delta;
        return 0;
    }
    if (delta == 0) return 0;
    mapzoom::wheel_acc acc;
    acc.residual = f.residual;
    const int n  = mapzoom::wheel_notches(acc, delta);
    f.residual   = acc.residual;
    return n;
}

int hud_rows_per_notch(int row_height) {
    if (row_height <= 0) return 1;
    const int k = HUD_PX_PER_NOTCH / row_height;
    return k < 1 ? 1 : k;
}

int scroll_top(int top, int notches, int rows_per_notch, int count, int visible) {
    int max_top = count - visible;
    if (max_top < 0) max_top = 0;
    long long t = (long long)top - (long long)notches * rows_per_notch;
    if (t < 0) t = 0;
    if (t > max_top) t = max_top;
    return (int)t;
}

int page_after(int index, int notches, int count) {
    if (count < 1) return index < 0 ? 0 : index;
    long long t = (long long)index - notches;
    if (t < 0) t = 0;
    if (t > count - 1) t = count - 1;
    return (int)t;
}

bool in_rect(int px, int py, int x, int y, int right, int bottom) {
    return px >= x && px < right && py >= y && py < bottom;
}

} // namespace mh::gfx::wheel_list
