//
// map_zoom_selftest.cpp -- `net_selftest.exe mapzoomtest`: the strategic map zoom's pure parts
// (gfx/map_zoom.{h,cpp} arithmetic, gfx/zoom_scale.{h,cpp} resampler). RES2.
//
//   1. limits: the max zoom is 2*H/768 and never below the stock 2.0;
//   2. the crop is the stock trunc(win/z) & ~1;
//   3. the anchor: over randomised zoom-in / zoom-out sequences the map pixel under the cursor stays under it
//      (to the pixel) whenever the crop has slack, the camera absorb gives back exactly 32 px per tile, and the
//      origin always lands inside [0, slack]; zooming in from a valid state never needs a camera tile;
//   4. the camera absorb on its own (zoom out toward an edge);
//   5. the cursor translation equals the stock formula for the stock (centred) crop, and keeps the edge rule;
//   6. the wheel accumulator (120 per notch, fractions kept, a reversal drops the other direction's rest);
//   7. the resampler: for all four filters and both pixel formats the result is within TWO levels per channel of
//      cpu_scale::scale_image (point exactly; the mean error is under a level), independent of whatever lies
//      outside the crop, in place, on crops that do not start at the origin; then a timing of the two frame
//      sizes the player runs.
//   8. (M1WL v6) the heat map's tile -> screen geometry (gfx/heat_xf.h) on the user's ui_scale 150 layout: every tile of the view is drawn at every camera
//      position (the rc11 fold into [-mw/2, mw/2) dropped the tiles 32 or more right / below the camera), the scale survives the strip hiding the viewport's
//      last rows, and a view wider than the map repeats it.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "gfx/cpu_scale.h"
#include "gfx/heat_xf.h"
#include "gfx/map_zoom.h"
#include "gfx/zoom_scale.h"

namespace {

namespace mz = mh::gfx::mapzoom;
namespace cs = mh::gfx::cpu_scale;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

uint32_t g_rng = 12345;
uint32_t rnd() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

void test_limits() {
    check("max zoom: 768 -> 2.0", mz::max_zoom(768) == 2.0);
    check("max zoom: 1056 -> 2.75", fabs(mz::max_zoom(1056) - 2.75) < 1e-9);
    check("max zoom: 1440 -> 3.75", fabs(mz::max_zoom(1440) - 3.75) < 1e-9);
    check("max zoom: never below the stock 2.0 (480, 576)", mz::max_zoom(480) == 2.0 && mz::max_zoom(576) == 2.0);
}

void test_crop() {
    check("crop: 1760 at 2.0 = 880", mz::crop_dim(1760, 2.0) == 880);
    check("crop: odd result rounds down to even (1760 / 1.9 = 926.3 -> 926)", mz::crop_dim(1760, 1.9) == 926);
    check("crop: 1000 / 1.7 = 588.2 -> 588", mz::crop_dim(1000, 1.7) == 588);
    check("crop: 1x is the whole viewport", mz::crop_dim(1760, 1.0) == 1760);
    check("crop: below 1x clamps to 1x", mz::crop_dim(1760, 0.5) == 1760);
    check("crop: never below 2", mz::crop_dim(100, 1000.0) == 2);
    check("slack: viewport 1760, crop 880 -> 880", mz::slack(1760, 880) == 880);
    check("slack: never negative", mz::slack(100, 200) == 0);
}

// A model world: the camera tile and the crop origin; the map pixel under viewport pixel c.
struct axis_model {
    long long cam;  // camera, tiles
    int       left; // crop origin
    double    z;
    int       win;
    long long world(int c) const { return cam * mz::TILE_PX + left + (long long)c * mz::crop_dim(win, z) / win; }
};

bool step(axis_model &m, double z_new, int c, int *tiles_out) {
    int nl, tiles;
    mz::anchor_axis(m.left, m.win, m.z, z_new, c, &nl, &tiles);
    m.cam += tiles;
    m.left = nl;
    m.z    = z_new;
    if (tiles_out) *tiles_out = tiles;
    return nl >= 0 && nl <= mz::slack(m.win, mz::crop_dim(m.win, z_new));
}

void test_anchor_random() {
    printf("  [anchor: random zoom walks]\n");
    int bad_origin = 0, bad_anchor = 0, tiles_on_in = 0, big_err = 0, steps = 0, absorbed = 0;
    for (int run = 0; run < 400; ++run) {
        const int    win  = 640 + (int)(rnd() % 3000);
        const double zmin = 1.0;
        const double zmax = mz::max_zoom(768 + (int)(rnd() % 800));
        axis_model   m{1000, 0, zmin, win};
        // the model starts at 1x with the crop at the origin, as the hook leaves it
        double z = zmin;
        for (int i = 0; i < 40; ++i) {
            const bool   in = (rnd() % 100) < 60;
            const double zn = in ? z * 1.2 : z / 1.2;
            const double zc = zn < zmin ? zmin : zn > zmax ? zmax
                                                           : zn;
            if (zc == z) continue;
            const int       c      = (int)(rnd() % (unsigned)win);
            const long long before = m.world(c);
            int             tiles  = 0;
            const bool      ok     = step(m, zc, c, &tiles);
            const long long after  = m.world(c);
            ++steps;
            if (!ok) ++bad_origin;
            if (zc > z && tiles != 0) ++tiles_on_in;
            if (tiles != 0) ++absorbed;
            // the anchor holds to a pixel unless the crop was out of room (the origin sits on a clamp)
            const long long err      = after - before;
            const bool      at_clamp = m.left == 0 || m.left == mz::slack(win, mz::crop_dim(win, zc));
            if (llabs(err) > 2 && !at_clamp) ++bad_anchor;
            if (llabs(err) > 40) ++big_err;
            z = zc;
        }
    }
    printf("    %d steps, %d absorbed a camera tile\n", steps, absorbed);
    check("anchor: the origin always lands inside [0, slack]", bad_origin == 0);
    check("anchor: the map pixel under the cursor holds to 2 px whenever the origin is not on a clamp", bad_anchor == 0);
    check("anchor: no step loses more than a tile (40 px) of anchor", big_err == 0);
    check("anchor: zooming IN never needs a camera tile", tiles_on_in == 0);
}

void test_absorb() {
    // 1760 wide viewport. At 2x with the crop at the left edge, zoom out to 1.5 around the RIGHT edge.
    axis_model      m{100, 0, 2.0, 1760};
    int             tiles = 0;
    const int       c     = 1759;
    const long long w0    = m.world(c);
    step(m, 1.5, c, &tiles);
    const long long w1 = m.world(c);
    check("absorb: zooming out toward the right edge from the left edge asks the camera for tiles", tiles < 0);
    check("absorb: ...and the cursor's map pixel is held within a pixel", llabs(w1 - w0) <= 1);
    check("absorb: the origin is the tile give-back (a multiple of 32 plus the fraction), inside the slack",
          m.left >= 0 && m.left <= mz::slack(1760, mz::crop_dim(1760, 1.5)));
    // zoom back to 1x: everything the crop held is handed to the camera
    axis_model      k{50, 300, 2.0, 1760};
    const int       c2 = 900;
    const long long a0 = k.world(c2);
    step(k, 1.0, c2, &tiles);
    check("absorb: back to 1x leaves the origin at 0", k.left == 0);
    check("absorb: ...with the cursor's map pixel held to a tile fraction (< 32 px)", llabs(k.world(c2) - a0) < 32);
    // a small overshoot is clamped, not paid for with a camera tile
    axis_model s{10, 0, 2.0, 1760};
    step(s, 2.0 * 1.0005, 0, &tiles);
    check("absorb: a sub-pixel overshoot costs no camera tile", tiles == 0);
    check("clamp_origin pulls a stale origin back", mz::clamp_origin(5000, 1760, 2.0) == 880 && mz::clamp_origin(-5, 1760, 2.0) == 0);
}

void test_translate() {
    // The stock formula for a centred crop: x' = trunc(left + x / z), x' == left -> 0.
    const int W          = 1760;
    int       mismatches = 0;
    for (double z : {1.07, 1.5, 2.0, 2.5, 3.75}) {
        const int cw   = mz::crop_dim(W, z);
        const int left = (W - cw) / 2;
        for (int x = 0; x < W; x += 7) {
            int stock = (int)(left + x / z);
            if (stock == left) stock = 0;
            const int ours = mz::translate_axis(x, W, left, cw);
            if (abs(ours - stock) > 3) ++mismatches; // z vs the even-rounded crop: the crop is up to 2 px short of W / z, scaled by x / W
        }
    }
    check("translate: equals the stock formula for the stock centred crop (within the crop rounding)", mismatches == 0);
    check("translate: the first pixel is 0 (edge scroll) whatever the origin", mz::translate_axis(0, 1760, 300, 880) == 0);
    check("translate: the last pixel maps to the crop's far edge", mz::translate_axis(1759, 1760, 300, 880) == 300 + 879);
    check("translate: a shifted origin shifts the result", mz::translate_axis(800, 1760, 100, 880) - mz::translate_axis(800, 1760, 0, 880) == 100);
}

void test_wheel() {
    mz::wheel_acc a;
    check("wheel: one notch", mz::wheel_notches(a, 120) == 1);
    check("wheel: two notches in one frame", mz::wheel_notches(a, 240) == 2);
    check("wheel: a notch backward", mz::wheel_notches(a, -120) == -1);
    check("wheel: a fraction accumulates (3 x 40 = 1 notch)",
          mz::wheel_notches(a, 40) == 0 && mz::wheel_notches(a, 40) == 0 && mz::wheel_notches(a, 40) == 1);
    mz::wheel_acc b;
    mz::wheel_notches(b, 90);
    check("wheel: a reversal drops the rest of the other direction", mz::wheel_notches(b, -90) == 0 && b.residual == -90);
    check("wheel: the remainder is kept across a notch (150 -> 1 + 30)", [] {
        mz::wheel_acc c;
        return mz::wheel_notches(c, 150) == 1 && c.residual == 30;
    }());
}

// ---- the resampler -------------------------------------------------------------------------------------

uint16_t pack(unsigned r, unsigned g, unsigned b, bool rgb555) {
    return rgb555 ? (uint16_t)((r << 10) | (g << 5) | b) : (uint16_t)((r << 11) | (g << 5) | b);
}
void unpack(uint16_t p, bool rgb555, int &r, int &g, int &b) {
    if (rgb555) {
        r = (p >> 10) & 31;
        g = (p >> 5) & 31;
        b = p & 31;
    } else {
        r = p >> 11;
        g = (p >> 5) & 63;
        b = p & 31;
    }
}

void fill_random(std::vector<uint16_t> &v, bool rgb555) {
    for (auto &p : v) p = rgb555 ? (uint16_t)(rnd() & 0x7FFF) : (uint16_t)(rnd() & 0xFFFF);
}

// Returns the largest per-channel difference between ours and the reference over the whole output.
double g_err_sum = 0.0;
long   g_err_n   = 0;

int compare_case(int fw, int fh, int pitch, int cx, int cy, int cw, int ch, int dw, int dh, int filter, bool rgb555, bool *poison_ok) {
    std::vector<uint16_t> fb((size_t)pitch * fh);
    fill_random(fb, rgb555);
    std::vector<uint16_t> crop((size_t)cw * ch);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) crop[(size_t)y * cw + x] = fb[(size_t)(cy + y) * pitch + cx + x];
    std::vector<uint16_t> ref((size_t)dw * dh);
    cs::scale_image(crop.data(), cw, ch, ref.data(), dw, dh, filter, rgb555);

    std::vector<uint16_t> work = fb;
    const bool            ok   = mh::gfx::zoom::stretch_crop(work.data(), pitch, cx, cy, cw, ch, dw, dh, filter, rgb555);
    if (!ok) return 999;
    // independence of the pixels outside the crop: a second run with different outside content
    std::vector<uint16_t> w2 = fb;
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x)
            if (x < cx || x >= cx + cw || y < cy || y >= cy + ch) w2[(size_t)y * pitch + x] = (uint16_t)(rnd() & (rgb555 ? 0x7FFF : 0xFFFF));
    mh::gfx::zoom::stretch_crop(w2.data(), pitch, cx, cy, cw, ch, dw, dh, filter, rgb555);
    bool same = true;
    for (int y = 0; y < dh; ++y)
        if (memcmp(&work[(size_t)y * pitch], &w2[(size_t)y * pitch], (size_t)dw * 2) != 0) same = false;
    *poison_ok = same;
    // the area outside the output rectangle must be untouched
    bool untouched = true;
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x)
            if ((x >= dw || y >= dh) && work[(size_t)y * pitch + x] != fb[(size_t)y * pitch + x]) untouched = false;
    if (!untouched) *poison_ok = false;

    int worst = 0;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            int r0, g0, b0, r1, g1, b1;
            unpack(work[(size_t)y * pitch + x], rgb555, r0, g0, b0);
            unpack(ref[(size_t)y * dw + x], rgb555, r1, g1, b1);
            const int d = abs(r0 - r1) > abs(g0 - g1) ? abs(r0 - r1) : abs(g0 - g1);
            const int e = d > abs(b0 - b1) ? d : abs(b0 - b1);
            if (e > worst) worst = e;
            g_err_sum += d > e ? d : e;
            ++g_err_n;
        }
    return worst;
}

void test_resampler() {
    const char *names[] = {"point", "linear", "sharp", "area"};
    for (int fmt555 = 0; fmt555 < 2; ++fmt555)
        for (int f = 0; f < 4; ++f) {
            int  worst  = 0;
            bool poison = true, any = true;
            g_err_sum = 0.0;
            g_err_n   = 0;
            struct c {
                int fw, fh, pitch, cx, cy, cw, ch, dw, dh;
            };
            const c cases[] = {
                {96, 64, 96, 0, 0, 48, 32, 96, 64},      // exact 2x, origin crop
                {96, 64, 100, 20, 11, 48, 32, 96, 64},   // exact 2x, offset crop, padded pitch
                {96, 64, 96, 13, 9, 62, 44, 96, 64},     // 1.55x, offset crop
                {120, 80, 120, 31, 7, 88, 70, 120, 80},  // 1.36x
                {128, 96, 128, 40, 30, 36, 28, 128, 96}, // 3.5x
                {96, 64, 96, 0, 0, 94, 62, 96, 64},      // barely zoomed
            };
            for (const c &t : cases) {
                bool      p = true;
                const int w = compare_case(t.fw, t.fh, t.pitch, t.cx, t.cy, t.cw, t.ch, t.dw, t.dh, f, fmt555 != 0, &p);
                if (w > worst) worst = w;
                poison = poison && p;
                any    = any && w != 999;
            }
            char         what[160];
            const double mean = g_err_n ? g_err_sum / g_err_n : 0.0;
            snprintf(what, sizeof(what), "resample %s %s: within 2 levels of cpu_scale::scale_image (worst %d, mean %.2f)", fmt555 ? "rgb555" : "rgb565", names[f], worst, mean);
            check(what, any && (f == 0 ? worst == 0 : worst <= 2 && mean < 1.0));
            snprintf(what, sizeof(what), "resample %s %s: independent of pixels outside the crop, rest of the frame untouched", fmt555 ? "rgb555" : "rgb565", names[f]);
            check(what, poison);
        }
    std::vector<uint16_t> tiny(16 * 16);
    check("resample: a crop larger than the output is refused", !mh::gfx::zoom::stretch_crop(tiny.data(), 16, 0, 0, 16, 16, 8, 8, 0, false));
    check("resample: a null frame is refused", !mh::gfx::zoom::stretch_crop(nullptr, 16, 0, 0, 4, 4, 8, 8, 0, false));
}

void time_case(int w, int h, double z, int filter) {
    std::vector<uint16_t> fb((size_t)w * h);
    fill_random(fb, false);
    const int cw = mz::crop_dim(w, z), ch = mz::crop_dim(h, z);
    mh::gfx::zoom::stretch_crop(fb.data(), w, (w - cw) / 3, (h - ch) / 3, cw, ch, w, h, filter, false); // warm the buffers
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    const int N = 10;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < N; ++i) mh::gfx::zoom::stretch_crop(fb.data(), w, (w - cw) / 3, (h - ch) / 3, cw, ch, w, h, filter, false);
    QueryPerformanceCounter(&b);
    const char *names[] = {"point", "linear", "sharp", "area"};
    printf("    timing %dx%d from a %dx%d crop (z=%.2f) %s: %.2f ms per frame\n", w, h, cw, ch, z, names[filter],
           (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / N);
}

// ---- 8. the heat map geometry (M1WL v6) ---------------------------------------------------------------------------------------------------------------------
struct fpt {
    float x = 0, y = 0;
    fpt() {}
    fpt(float a, float b) : x(a), y(b) {}
};

// The user's layout (2026-10-25): 2560x1440 client, 1920x1440 frame at ui_scale 147%: the map quad is 1:1 at client x = 282, 1760 wide and 1393 high (the
// strip takes the other 47 rows), so the viewport (1760 x 1408) has its last 15 rows under the strip. The presenter's game -> client answer is x + 282 and y
// clamped to the last visible row.
void layout_points(int vw, int vh, int map_rows, int *p0x, int *p0y, int *p1x, int *p1y, int *pcx, int *pcy) {
    auto cy = [&](int y) { return y < map_rows ? y : map_rows - 1; };
    *p0x    = 282;
    *p0y    = cy(0);
    *p1x    = 282 + vw / 2;
    *p1y    = cy(vh / 2);
    *pcx    = 282 + vw - 1;
    *pcy    = cy(vh - 1);
}

void test_heat_geometry() {
    namespace hx = mh::gfx::heat;
    const int vw = 1760, vh = 1408;
    int       a[6];
    layout_points(vw, vh, 1393, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
    hx::heat_xf x;
    x.mw = x.mh = 64;
    x.vw = x.zcw = vw;
    x.vh = x.zch = vh;
    check("H1: the scale is 1:1 from points inside the map", x.set_map_points(a[0], a[1], a[2], a[3], a[4], a[5]) && x.sx == 1.0f && x.sy == 1.0f);
    check("H2: origin at the map quad (client x 282 - the centre half pixel)", x.ox == 281.5f && x.oy == -0.5f);
    check("H3: the clip ends at the last visible row (the 1393rd, edge 1392.5 in the centre convention), not at the hidden viewport rows", x.cy1 == 1392.5f && x.cx1 == 281.5f + (float)vw);
    {
        // the rc11 slope, from the (vw - 1, vh - 1) corner, drifts: 1392 rows over 1407 pixels
        const float old_sy = (float)(a[5] - a[1]) / (float)(vh - 1);
        check("H4: the old corner slope was off (regression anchor)", old_sy < 0.995f && (float)vh * (1.0f - old_sy) > 5.0f);
    }

    const int want_cols = (vw + 31) / 32, want_rows = (vh + 31) / 32; // 55 x 44 tiles touch the viewport
    int       bad_new = 0, bad_old = 0, far_ok = 1;
    for (int cc = 0; cc < 64; ++cc)
        for (int cr = 0; cr < 64; cr += 3) { // every column, every third row: the fold depends on each axis alone
            x.camc    = cc;
            x.camr    = cr;
            int n_new = 0, n_old = 0;
            for (int c = 0; c < 64; ++c)
                for (int r = 0; r < 64; ++r) {
                    x.each_copy(c, r, [&](int di, int dj) {
                        fpt p, q;
                        if (x.tile_rect(di, dj, &p, &q)) ++n_new;
                    });
                    int di, dj;
                    fpt p, q;
                    x.offsets(c, r, &di, &dj);
                    if (x.tile_rect(di, dj, &p, &q)) ++n_old;
                }
            bad_new += n_new != want_cols * want_rows;
            bad_old += n_old != want_cols * want_rows;
            // a tile 40 columns right and 35 rows below the camera is where the old fold lost it
            int hit = 0;
            x.each_copy((cc + 40) & 63, (cr + 35) & 63, [&](int di, int dj) {
                fpt p, q;
                if (x.tile_rect(di, dj, &p, &q) && di == 40 && dj == 35 && p.x == 281.5f + 40.0f * 32.0f && p.y == -0.5f + 35.0f * 32.0f) ++hit;
            });
            far_ok &= hit == 1;
        }
    check("H5: all 55 x 44 tiles of the view are drawn at every camera position", bad_new == 0);
    check("H6: (regression anchor) the old centred fold missed tiles at most camera positions", bad_old > 1000);
    check("H7: a tile 40 columns / 35 rows from the camera is drawn exactly once, on its pixels", far_ok == 1);

    // a zoomed crop (zoom 2: the crop is half the viewport, starting inside the world pixel row / column)
    x.camc  = 7;
    x.camr  = 60;
    x.zl    = 300;
    x.zt    = 130;
    x.zcw   = 880;
    x.zch   = 704;
    int bad = 0;
    for (int c = 0; c < 64; ++c)
        for (int r = 0; r < 64; ++r) {
            int n = 0;
            x.each_copy(c, r, [&](int di, int dj) {
                fpt p, q;
                if (x.tile_rect(di, dj, &p, &q)) ++n;
            });
            // brute force: the tile's world pixel box against the crop (torus of 64 * 32 px)
            const int ox = (((c - x.camc) & 63) * 32 - x.zl), oy = (((r - x.camr) & 63) * 32 - x.zt);
            auto      hit = [&](int o, int crop) { // does a 32-px tile at o (or o - 2048) touch [0, crop)?
                return (o < crop && o + 32 > 0) || (o - 2048 < crop && o - 2048 + 32 > 0);
            };
            bad += n != ((hit(ox, x.zcw) && hit(oy, x.zch)) ? 1 : 0);
        }
    check("H8: a zoomed crop draws exactly the tiles that touch it", bad == 0);

    // a view WIDER than the map: a 16 x 16 map under the same 1760 x 1408 viewport repeats (55 x 44 tile slots over 256 tiles)
    x.mw = x.mh = 16;
    x.zl = x.zt = 0;
    x.zcw       = vw;
    x.zch       = vh;
    x.camc = x.camr = 3;
    int total       = 0;
    for (int c = 0; c < 16; ++c)
        for (int r = 0; r < 16; ++r)
            x.each_copy(c, r, [&](int di, int dj) {
                fpt p, q;
                if (x.tile_rect(di, dj, &p, &q)) ++total;
            });
    check("H9: a view wider than the map draws every slot (the wrapped copies too)", total == want_cols * want_rows);
}

} // namespace

int run_mapzoomtest() {
    printf("=== mapzoomtest (map zoom: limits, cursor anchor + camera absorb, cursor map, wheel, resampler) ===\n");
    test_limits();
    test_crop();
    test_anchor_random();
    test_absorb();
    test_translate();
    test_wheel();
    test_resampler();
    test_heat_geometry();
    printf("  [timing, informational]\n");
    time_case(1760, 1024, 2.0, 3);
    time_case(1760, 1024, 2.0, 0);
    time_case(2400, 1408, 2.0, 3);
    time_case(2400, 1408, 1.2, 3);
    mh::gfx::zoom::release_buffers();
    printf("mapzoomtest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
