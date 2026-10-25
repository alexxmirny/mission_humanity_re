//
// bg_scale_selftest.cpp -- `net_selftest.exe bgscaletest`: the mode-change crash guards' pure cores
// (seams/video_mode_guards.{h,cpp}), measured the way the crash happened.
//
// The rc8 resolution-picker crash was a full-screen background copy that read WindowWidth*WindowHeight
// words from an image smaller than that. So every scaling case here places the source image at the END
// of a committed page followed by a PAGE_NOACCESS guard page (header just before the pixels): a read one
// word past the image faults the suite instead of passing. The destination sits at the end of a guarded
// page too, so a write past W x H faults likewise.
//
//   1. pass-through: iw == W and ih >= W's rows -> byte-identical to the linear copy retail did.
//   2. 1024x768 -> 1280x768 and 2560x1440, all four filters: exact dims, point == the nearest-texel
//      reference, the others == the shared kernel (gfx/cpu_scale) pixel for pixel; cache hit/rebuild.
//   3. 800x576 -> 1280x768 (the observed crash geometry): completes under the guard page.
//   4. one-axis down-scale (1024x768 -> 1280x704): bilinear within 1 level of an independent reference.
//   5. NULL image / zero header -> nothing written.
//   6. RGB555: green stays 5 bits, bit 15 stays 0.
//   7. P1 cursor clamp and P2 galaxy index cores.
//   8. RES7: the grown side-panel list tables (panel_rows_selftest.cpp).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "gfx/cpu_scale.h"
#include "gfx/ddraw_own.h"
#include "seams/video_mode_guards.h"

namespace {

using namespace mh::video;
namespace cs = mh::gfx::cpu_scale;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// `bytes` of read/write memory that ends exactly at a PAGE_NOACCESS guard page.
struct guarded {
    uint8_t *base  = nullptr; // whole reservation
    uint8_t *start = nullptr; // `bytes` of usable memory, ending at the guard
    size_t   total = 0;
    explicit guarded(size_t bytes) {
        const size_t pages = (bytes + 4095) / 4096;
        total              = (pages + 1) * 4096;
        base               = (uint8_t *)VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!base) return;
        DWORD old;
        VirtualProtect(base + pages * 4096, 4096, PAGE_NOACCESS, &old);
        start = base + pages * 4096 - bytes;
    }
    ~guarded() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    guarded(const guarded &)            = delete;
    guarded &operator=(const guarded &) = delete;
};

// An image as the game stores it: u16 w, u16 h, then the pixels -- ending at the guard page.
struct image {
    guarded         mem;
    int             w, h;
    const uint16_t *px() const { return (const uint16_t *)(mem.start + 4); }
    uint16_t       *px() { return (uint16_t *)(mem.start + 4); }
    image(int w_, int h_, bool rgb555) : mem(4 + (size_t)w_ * h_ * 2), w(w_), h(h_) {
        uint16_t *hdr = (uint16_t *)mem.start;
        hdr[0]        = (uint16_t)w;
        hdr[1]        = (uint16_t)h;
        uint32_t seed = 0x2468ACEu ^ (uint32_t)(w * 31 + h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                seed       = seed * 1664525u + 1013904223u;
                uint16_t v = (x < w / 2) ? ((((x >> 3) ^ (y >> 3)) & 1) ? 0xFFFF : 0x0000) : (uint16_t)(seed >> 16);
                if (rgb555) v &= 0x7FFF;
                px()[(size_t)y * w + x] = v;
            }
    }
};

const char *const FN[] = {"point", "linear", "sharp", "area"};

// Independent nearest-texel reference: the texel containing the output pixel's centre.
uint16_t nearest(const image &im, int ow, int oh, int ox, int oy) {
    int sx = (int)floor((ox + 0.5) * im.w / ow), sy = (int)floor((oy + 0.5) * im.h / oh);
    if (sx >= im.w) sx = im.w - 1;
    if (sy >= im.h) sy = im.h - 1;
    return im.px()[(size_t)sy * im.w + sx];
}

uint16_t pack565(const cs::rgbf &c) {
    return (uint16_t)(((unsigned)(c.r * 31.0f + 0.5f) << 11) | ((unsigned)(c.g * 63.0f + 0.5f) << 5) |
                      (unsigned)(c.b * 31.0f + 0.5f));
}

void test_passthrough() {
    printf("  [pass-through]\n");
    {
        image     im(640, 480, false);
        guarded   out(640 * 480 * 2);
        bg_cache  c = {};
        bg_result r = bg_restore_core(c, (uint16_t *)out.start, im.px(), 640 * 480, 640, 480, 640 * 2, 3, 565);
        check("640x480 art in a 640x480 window is retail's own copy", r == bg_result::passthrough);
        check("... byte-identical", memcmp(out.start, im.px(), 640 * 480 * 2) == 0);
        check("... and allocated nothing", c.buf == nullptr);
        bg_cache_release(c);
    }
    {
        image     im(640, 600, false); // taller art: retail cropped to the top rows
        guarded   out(640 * 480 * 2);
        bg_cache  c = {};
        bg_result r = bg_restore_core(c, (uint16_t *)out.start, im.px(), 640 * 480, 640, 480, 640 * 2, 1, 565);
        check("taller art is a pass-through", r == bg_result::passthrough);
        check("... top 480 rows exactly", memcmp(out.start, im.px(), 640 * 480 * 2) == 0);
        bg_cache_release(c);
    }
}

void test_scale_up(int iw, int ih, int ow, int oh, int stride) {
    for (int f = 0; f < 4; ++f) {
        image     im(iw, ih, false);
        guarded   out((size_t)ow * oh * 2);
        uint16_t *dst = (uint16_t *)out.start;
        bg_cache  c   = {};
        char      what[160];
        wsprintfA(what, "%dx%d -> %dx%d %s: scaled (new)", iw, ih, ow, oh, FN[f]);
        bg_result r = bg_restore_core(c, dst, im.px(), (uint32_t)ow * oh, ow, oh, ow * 2, f, 565);
        check(what, r == bg_result::scaled_new);
        // Every sampled output pixel equals the shared kernel (and, for point, the independent reference).
        const double sx = (double)ow / iw, sy = (double)oh / ih;
        long         bad = 0, badp = 0;
        for (int oy = 0; oy < oh; oy += (oy % 9 == 0 ? 1 : stride)) {
            for (int ox = 0; ox < ow; ox += stride) {
                const cs::rgbf c0 = cs::sample(im.px(), iw, ih, false, f, (ox + 0.5) / sx, (oy + 0.5) / sy, sx, sy);
                if (dst[(size_t)oy * ow + ox] != pack565(c0)) ++bad;
                if (f == 0 && dst[(size_t)oy * ow + ox] != nearest(im, ow, oh, ox, oy)) ++badp;
            }
        }
        wsprintfA(what, "%dx%d -> %dx%d %s: output == shared kernel (%ld bad)", iw, ih, ow, oh, FN[f], bad);
        check(what, bad == 0);
        if (f == 0) {
            wsprintfA(what, "%dx%d -> %dx%d point: output == nearest-texel reference (%ld bad)", iw, ih, ow, oh, badp);
            check(what, badp == 0);
        }
        // A second identical call is a cache hit; a different filter rebuilds.
        r = bg_restore_core(c, dst, im.px(), (uint32_t)ow * oh, ow, oh, ow * 2, f, 565);
        check("second identical call is a cache hit", r == bg_result::scaled_cached);
        r = bg_restore_core(c, dst, im.px(), (uint32_t)ow * oh, ow, oh, ow * 2, (f + 1) & 3, 565);
        check("a filter change rebuilds", r == bg_result::scaled_new);
        bg_cache_release(c);
    }
}

void test_down_one_axis() {
    printf("  [1024x768 -> 1280x704: down in Y]\n");
    const int iw = 1024, ih = 768, ow = 1280, oh = 704;
    image     im(iw, ih, false);
    for (int f = 0; f < 4; ++f) {
        guarded   out((size_t)ow * oh * 2);
        uint16_t *dst = (uint16_t *)out.start;
        bg_cache  c   = {};
        bg_restore_core(c, dst, im.px(), (uint32_t)ow * oh, ow, oh, ow * 2, f, 565);
        const double sx = (double)ow / iw, sy = (double)oh / ih;
        int          maxd = 0;
        for (int oy = 0; oy < oh; oy += 3)
            for (int ox = 0; ox < ow; ox += 3) {
                const cs::rgbf r0   = cs::sample(im.px(), iw, ih, false, f, (ox + 0.5) / sx, (oy + 0.5) / sy, sx, sy);
                const uint16_t want = pack565(r0), got = dst[(size_t)oy * ow + ox];
                const int      d = abs((int)(got >> 11) - (int)(want >> 11));
                if (d > maxd) maxd = d;
                const int dg = abs((int)((got >> 5) & 63) - (int)((want >> 5) & 63)), db = abs((int)(got & 31) - (int)(want & 31));
                if (dg > maxd) maxd = dg;
                if (db > maxd) maxd = db;
            }
        char what[120];
        wsprintfA(what, "down-Y %s: within the shared kernel (max channel error %d)", FN[f], maxd);
        check(what, maxd == 0);
        if (f == 1) {
            // Independent bilinear (centres at k+0.5) for the linear filter, tolerance 1 level.
            int worst = 0;
            for (int oy = 0; oy < oh; oy += 11)
                for (int ox = 0; ox < ow; ox += 11) {
                    const double px = (ox + 0.5) / sx - 0.5, py = (oy + 0.5) / sy - 0.5;
                    int          x0 = (int)floor(px), y0 = (int)floor(py);
                    const double fx = px - x0, fy = py - y0;
                    auto         T = [&](int x, int y) {
                        x = x < 0 ? 0 : x >= iw ? iw - 1
                                                        : x;
                        y = y < 0 ? 0 : y >= ih ? ih - 1
                                                        : y;
                        return im.px()[(size_t)y * iw + x];
                    };
                    double         g    = 0;
                    const uint16_t t[4] = {T(x0, y0), T(x0 + 1, y0), T(x0, y0 + 1), T(x0 + 1, y0 + 1)};
                    const double   w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                    for (int k = 0; k < 4; ++k) g += w[k] * ((t[k] >> 5) & 63);
                    const int d = abs((int)lround(g) - (int)((dst[(size_t)oy * ow + ox] >> 5) & 63));
                    if (d > worst) worst = d;
                }
            wsprintfA(what, "down-Y linear vs an independent bilinear: green within 1 level (worst %d)", worst);
            check(what, worst <= 1);
        }
        bg_cache_release(c);
    }
}

void test_nothing_drawn() {
    printf("  [nothing drawn]\n");
    guarded   out(64 * 64 * 2);
    uint16_t *dst = (uint16_t *)out.start;
    for (int i = 0; i < 64 * 64; ++i) dst[i] = 0xABCD;
    bg_cache  c = {};
    bg_result r = bg_restore_core(c, dst, (const uint16_t *)4, 64 * 64, 64, 64, 128, 3, 565); // NULL slot + 4
    check("a NULL table slot (src == 4) is skipped", r == bg_result::skipped_noimage);
    {
        image im(8, 8, false);
        ((uint16_t *)im.mem.start)[0] = 0; // iw == 0
        r                             = bg_restore_core(c, dst, im.px(), 64 * 64, 64, 64, 128, 3, 565);
        check("a zero-width header is skipped", r == bg_result::skipped_bad);
    }
    bool untouched = true;
    for (int i = 0; i < 64 * 64; ++i) untouched = untouched && dst[i] == 0xABCD;
    check("... and the destination is untouched", untouched);
    bg_cache_release(c);
}

void test_rgb555() {
    printf("  [RGB555]\n");
    const uint16_t solid = (uint16_t)((3u << 10) | (17u << 5) | 9u); // r3 g17 b9, bit 15 clear
    for (int f = 0; f < 4; ++f) {
        image im(64, 48, true);
        for (int i = 0; i < 64 * 48; ++i) im.px()[i] = solid;
        guarded   out(128 * 96 * 2);
        uint16_t *dst = (uint16_t *)out.start;
        bg_cache  c   = {};
        bg_restore_core(c, dst, im.px(), 128 * 96, 128, 96, 256, f, 555);
        bool same = true, clear = true;
        for (int i = 0; i < 128 * 96; ++i) {
            same  = same && dst[i] == solid;
            clear = clear && (dst[i] & 0x8000) == 0;
        }
        char what[100];
        wsprintfA(what, "555 %s: a solid colour scales to itself (green kept 5-bit)", FN[f]);
        check(what, same);
        wsprintfA(what, "555 %s: bit 15 stays 0", FN[f]);
        check(what, clear);
        bg_cache_release(c);
    }
    { // random 555 art never gains bit 15 either
        image     im(64, 48, true);
        guarded   out(100 * 70 * 2);
        uint16_t *dst = (uint16_t *)out.start;
        bg_cache  c   = {};
        bg_restore_core(c, dst, im.px(), 100 * 70, 100, 70, 200, 1, 555);
        bool clear = true;
        for (int i = 0; i < 100 * 70; ++i) clear = clear && (dst[i] & 0x8000) == 0;
        check("555 random art, linear: bit 15 stays 0", clear);
        bg_cache_release(c);
    }
}

void test_pitch() {
    printf("  [pitch]\n");
    image     im(320, 240, false);
    const int W = 400, H = 300, pitch = W * 2 + 96;
    guarded   out((size_t)pitch * H);
    memset(out.start, 0x5A, (size_t)pitch * H);
    bg_cache  c = {};
    bg_result r = bg_restore_core(c, (uint16_t *)out.start, im.px(), (uint32_t)W * H, W, H, pitch, 0, 565);
    check("a padded pitch scales", r == bg_result::scaled_new);
    bool pad_ok = true, row_ok = true;
    for (int y = 0; y < H; ++y) {
        const uint8_t *row = out.start + (size_t)y * pitch;
        for (int i = W * 2; i < pitch; ++i) pad_ok = pad_ok && row[i] == 0x5A;
        row_ok = row_ok && ((const uint16_t *)row)[0] == nearest(im, W, H, 0, y);
    }
    check("... the row padding is left alone", pad_ok);
    check("... each row starts at its own pitch offset", row_ok);
    r = bg_restore_core(c, (uint16_t *)out.start, im.px(), (uint32_t)W * H, W, H, W * 2 - 2, 0, 565);
    check("a pitch narrower than the row is refused", r == bg_result::skipped_bad);
    bg_cache_release(c);
}


void test_cursor_and_index() {
    printf("  [P1 cursor clamp / P2 index]\n");
    int x = 595, y = 749, mx = 595, my = 749;
    int n = clamp_cursor_to_mode(&x, &y, &mx, &my, 640, 480);
    check("(595,749) into 640x480 -> (595,479)", x == 595 && y == 479 && mx == 595 && my == 479 && n == 2);
    x  = 100;
    y  = 100;
    mx = 7;
    my = 9;
    n  = clamp_cursor_to_mode(&x, &y, &mx, &my, 640, 480);
    check("an in-range cursor is untouched", x == 100 && y == 100 && mx == 7 && my == 9 && n == 0);
    x  = -5;
    y  = 480;
    mx = 640;
    my = -1;
    clamp_cursor_to_mode(&x, &y, &mx, &my, 640, 480);
    check("negative and one-past values clamp to the edges", x == 0 && y == 479 && mx == 639 && my == 0);
    x = 900;
    y = 900;
    n = clamp_cursor_to_mode(&x, &y, &mx, &my, 0, 480);
    check("a zero window changes nothing", n == 0 && x == 900 && y == 900);
    check("galaxy index: stock modes 0/1/2 unchanged", galaxy_bg_mode(0) == 0 && galaxy_bg_mode(1) == 1 && galaxy_bg_mode(2) == 2);
    check("galaxy index: picker entries clamp to 2", galaxy_bg_mode(3) == 2 && galaxy_bg_mode(4) == 2 && galaxy_bg_mode(40) == 2);
    {
        int from = 0, to = 0, skip = 0;
        // RES23-S3 / RES26: THE fill rule: teal from the game's rows end to the last whole slot of the frame (46 px rows, top 230), except the slot the visible edge cuts
        // (pink, user 2026-10-23); stock heights draw nothing.
        check("panel filler: tall frame, all visible -> teal from the capped end to the last whole slot, no skip",
              panel_fill_plan(1024, 1024, 230, 46, 750, from, to, skip) && from == 750 && to == 230 + ((1024 - 230) / 46) * 46 && skip == -1);
        check("panel filler: stock height (the rows already end on the last whole slot) draws nothing",
              !panel_fill_plan(768, 768, 230, 46, 230 + ((768 - 230) / 46) * 46, from, to, skip) && from == to && skip == -1);
        check("panel filler: a zero row height draws nothing", !panel_fill_plan(1152, 1152, 230, 0, 750, from, to, skip));
        check("panel filler: Hp 533 of 800, rows end at 508 -> teal 508..768 but the slot the visible edge cuts (508) is pink",
              panel_fill_plan(800, 533, 248, 52, 508, from, to, skip) && from == 508 && to == 768 && skip == 508);
        check("panel filler: the slot below the cut one is a whole slot -> teal (not skipped)", skip != 560);
        check("panel filler: visible edge on a slot boundary -> nothing is cut, no skip",
              panel_fill_plan(800, 560, 248, 52, 560, from, to, skip) && skip == -1);
        check("panel filler: the rows already reach the last whole slot -> nothing", !panel_fill_plan(800, 800, 248, 52, 768, from, to, skip));
        check("panel filler: a frame at or below the panel top draws nothing", !panel_fill_plan(240, 240, 248, 52, 248, from, to, skip));
    }
    check("panel rows: stock heights unchanged, taller capped at 768", panel_rows_height(480) == 480 && panel_rows_height(576) == 576 && panel_rows_height(704) == 704 && panel_rows_height(768) == 768 && panel_rows_height(1056) == 768 && panel_rows_height(1152) == 768);
}

// 8. PT-GFX9 frame reservation: the owned frame is allocated for the largest reachable mode. Pitch stays w*2.
void test_frame_reserve() {
    const SIZE_T big  = (SIZE_T)1280 * 2 * 1024;
    const SIZE_T bare = mh::gfx::owned_probe_frame_alloc(640, 480, true);
    check("no reservation: a 640x480 frame holds its own body (+slack) and no more than the 1280x1024 body",
          bare >= (SIZE_T)640 * 2 * 480 && bare < big);
    mh::gfx::owned_reserve_frame(1280, 1024);
    mh::gfx::owned_reserve_frame(800, 600); // the largest request wins
    const SIZE_T prim = mh::gfx::owned_probe_frame_alloc(640, 480, true);
    const SIZE_T tex  = mh::gfx::owned_probe_frame_alloc(64, 64, false);
    check("reserved: the primary 640x480 frame is allocated for 1280x1024 (the rows a stale menu frame writes)", prim >= big);
    check("reserved: an unrelated small offscreen surface is not inflated", tex > 0 && tex < big);
    // the bevel crash geometry: a write at row 579+ of a 640-pitch frame lies inside the allocation
    check("reserved: a stale 1280x1024 layout's last row (1023) at the 640-wide pitch is inside the 640x480 allocation",
          prim > (SIZE_T)1024 * 640 * 2);
}

} // namespace

int panel_rows_checks(int *checks); // panel_rows_selftest.cpp -- RES7

int run_bgscaletest() {
    printf("=== bgscaletest (mode-change guards: header-aware filtered background restore, cursor clamp) ===\n");
    test_passthrough();
    printf("  [1024x768 -> 1280x768]\n");
    test_scale_up(1024, 768, 1280, 768, 1);
    printf("  [1024x768 -> 2560x1440]\n");
    test_scale_up(1024, 768, 2560, 1440, 7);
    printf("  [800x576 -> 1280x768 (the observed crash geometry)]\n");
    test_scale_up(800, 576, 1280, 768, 5);
    test_down_one_axis();
    test_nothing_drawn();
    test_rgb555();
    test_pitch();
    test_cursor_and_index();
    test_frame_reserve();
    g_fails += panel_rows_checks(&g_checks);
    printf("bgscaletest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
