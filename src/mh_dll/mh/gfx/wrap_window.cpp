//
// gfx/wrap_window.cpp -- see wrap_window.h. Pure.
//
#include "gfx/wrap_window.h"

namespace mh::gfx::wrapwin {

int stock_axis(int from, int to, int big) {
    int sign, delta;
    if (from > to) { // the asm: JLE not taken
        sign  = -1;
        delta = from - to;
    } else {
        sign  = 1;
        delta = to - from;
    }
    if (big / 2 < delta) {
        delta = big - delta;
        sign  = -sign;
    }
    return sign * delta;
}

int bias_for(int big, int viewport, int floor_k) {
    if (big <= 0 || viewport <= 0) return floor_k;
    const int k = big - viewport;
    return k > floor_k ? k : floor_k;
}

int biased_axis(int from, int to, int big, int k) {
    if (big <= 0) return to - from;
    const long long d = (long long)to - (long long)from + k;
    long long       m = d % big;
    if (m < 0) m += big;
    return (int)(m - k);
}

int span_axis(int from, int to, int big) {
    if (big <= 0) return to - from;
    long long m = ((long long)to - (long long)from) % big;
    if (m < 0) m += big;
    return (int)m;
}

int drag_near_axis(int cam, int left_w, int right_w, int big, int k) {
    if (big <= 0) return left_w - cam;
    const int stock = stock_axis(cam, left_w, big);
    if (stock < -k && stock + span_axis(left_w, right_w, big) > 0) return stock;
    return biased_axis(cam, left_w, big, k);
}

int drag_dir_screen(int sa, int sc, int viewport, int big) {
    if (viewport <= 0 || big <= 0 || sc < 0 || sc >= viewport) return 0;
    const long long span = (long long)sc - (long long)sa;
    if (span >= big || -span >= big) return 0;
    return sa <= sc ? 1 : -1;
}

int drag_track_anchor(drag_axis_track &t, int cam, int anchor, int big, int k) {
    if (!t.valid || t.anchor != anchor || big <= 0) {
        t.valid  = true;
        t.anchor = anchor;
        t.screen = biased_axis(cam, anchor, big, k);
    } else {
        int step = ((cam - t.cam) % big + big) % big; // the camera's wrapped step, in [-big/2, big/2)
        if (step >= big / 2) step -= big;
        t.screen -= step;
    }
    t.cam = cam;
    return t.screen;
}

bool tile_in_span_stock(int tile, int cam, int tiles, int map_tiles) {
    const int  left  = cam == 0 ? map_tiles : cam - 1;
    const int  right = (cam + tiles + 1) & (map_tiles - 1);
    const bool out   = (left < right && (tile < left || right < tile)) || (right < left && (tile <= left && right <= tile));
    return !out;
}

bool tile_in_span(int tile, int cam, int tiles, int map_tiles) {
    if (map_tiles > 0 && tiles + 3 >= map_tiles) return true; // the span covers the whole axis
    return tile_in_span_stock(tile, cam, tiles, map_tiles);
}

} // namespace mh::gfx::wrapwin
