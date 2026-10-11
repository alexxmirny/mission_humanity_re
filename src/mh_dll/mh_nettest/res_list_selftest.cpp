//
// res_list_selftest.cpp -- `net_selftest.exe reslisttest`: the generated resolution list (gfx/res_list.{h,cpp}),
// pure arithmetic. The launcher carries a Rust copy of the generator and asserts the
// SAME vectors; a change to either side must change both.
//
//   1. the list for 1920x1080, 2560x1440, 3840x2160, 1920x1200, 1366x768, 1280x1024 desktops: exact entries
//      (stock three first, then every 4:3 size, every 16:9 size, the rest; ascending by area within a group), every entry valid, none above the desktop, none above the cap;
//   2. Auto for the same desktops at the display scales that matter (100/150/200%): exact sizes, never below
//      1024x768, never above the desktop or the 2048x1600 cap (the view must fit the sprite window of the 64-tile
//      smallest map), never wider than 16:9 (ultrawide desktops: 3440x1440, 5120x1440);
//   3. the integer-scale rule (1920x1056 on 1080p: whole; 1280x704: fractional; 960x512: 2x; stock 640x480 on 1440p: 3x);
//   4. aspect_group (the 4:3 / 16:9 / other split), parse_size, insert_sorted (a requested size lands in its group's area order, an existing one is not duplicated),
//      and the label text the launcher shows;
//   5. the biased sprite window (gfx/wrap_window): stock vs biased on both axes, negative d, the boundaries,
//      the 64/128/256-tile maps, and that the cap follows from K.
//
#include <stdio.h>
#include <string.h>

#include "gfx/res_list.h"
#include "gfx/wrap_window.h"

namespace {

namespace rl = mh::gfx::reslist;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

struct sz {
    int w, h;
};

// The expected list (stock three first) must match entry for entry.
void expect_list(const char *name, int dw, int dh, const sz *want, int nwant) {
    rl::entry got[rl::MAX_ENTRIES];
    const int n = rl::generate(dw, dh, got, rl::MAX_ENTRIES);
    char      msg[160];
    sprintf(msg, "%s: %d entries (want %d)", name, n, nwant);
    check(msg, n == nwant);
    for (int i = 0; i < n && i < nwant; ++i) {
        sprintf(msg, "%s: [%d] %dx%d (want %dx%d)", name, i, got[i].w, got[i].h, want[i].w, want[i].h);
        check(msg, got[i].w == want[i].w && got[i].h == want[i].h);
    }
    for (int i = 3; i < n; ++i) { // the invariants, independent of the table above
        sprintf(msg, "%s: [%d] %dx%d is a valid size", name, i, got[i].w, got[i].h);
        check(msg, rl::valid_size(got[i].w, got[i].h));
        sprintf(msg, "%s: [%d] %dx%d fits the desktop", name, i, got[i].w, got[i].h);
        check(msg, got[i].w <= dw && got[i].h <= dh);
        if (i > 3) {
            const int ga = rl::aspect_group(got[i - 1].w, got[i - 1].h), gb = rl::aspect_group(got[i].w, got[i].h);
            sprintf(msg, "%s: [%d] 4:3 first, then 16:9, then the rest", name, i);
            check(msg, gb >= ga);
            sprintf(msg, "%s: [%d] ascending by area within its group", name, i);
            check(msg, gb != ga || (long)got[i].w * got[i].h >= (long)got[i - 1].w * got[i - 1].h);
        }
    }
}

void expect_auto(const char *name, int dw, int dh, int pct, int w, int h) {
    const rl::entry a = rl::auto_size(dw, dh, pct);
    char            msg[160];
    sprintf(msg, "auto %s @%d%%: %dx%d (want %dx%d)", name, pct, a.w, a.h, w, h);
    check(msg, a.w == w && a.h == h);
    sprintf(msg, "auto %s @%d%%: tile-aligned, within the cap and the desktop", name, pct);
    check(msg, (a.w % 32) == 0 && (a.h % 32) == 0 && a.w <= rl::CAP_W && a.h <= rl::CAP_H && a.w <= dw && a.h <= dh);
}

// ---- the biased sprite window (gfx/wrap_window) --------------------------------------------------------------
void wrap_window_checks() {
    namespace ww = mh::gfx::wrapwin;
    char msg[200];

    // 1. Stock is the asm's function: the short way round, result within [-big/2, big/2].
    check("stock: 100 -> 300 on 2048 = +200", ww::stock_axis(100, 300, 2048) == 200);
    check("stock: 300 -> 100 = -200", ww::stock_axis(300, 100, 2048) == -200);
    check("stock: 0 -> 1024 = +1024 (the tie stays positive)", ww::stock_axis(0, 1024, 2048) == 1024);
    check("stock: 0 -> 1025 folds to -1023", ww::stock_axis(0, 1025, 2048) == -1023);
    check("stock: 1025 -> 0 folds to +1023", ww::stock_axis(1025, 0, 2048) == 1023);
    check("stock: the bug -- a sprite 1500 px right of the camera lands at -548", ww::stock_axis(0, 1500, 2048) == -548);

    // 2. Biased: [-K, big-K) on every map size.
    const int bigs[] = {2048, 4096, 8192}; // 64, 128, 256 tiles
    const int ks[]   = {ww::K_X, ww::K_Y};
    for (int big : bigs)
        for (int k : ks) {
            bool in_window = true, equal_to_modular = true, agrees_with_stock_where_it_must = true;
            int  samples = 0;
            for (int from = -big; from <= 2 * big; from += 97)
                for (int to = -big; to <= 2 * big; to += 131) {
                    const int d = ww::biased_axis(from, to, big, k);
                    // stock on the masked world positions: the game passes positions in [0, big)
                    const int st = ww::stock_axis(((from % big) + big) % big, ((to % big) + big) % big, big);
                    ++samples;
                    if (d < -k || d >= big - k) in_window = false;
                    if ((((to - from - d) % big) + big) % big != 0) equal_to_modular = false;
                    // wherever the stock answer lies inside the biased window the two agree
                    if (st >= -k && st < big - k && st != d) agrees_with_stock_where_it_must = false;
                }
            sprintf(msg, "biased big=%d k=%d: every result lies in [-k, big-k) (%d pairs)", big, k, samples);
            check(msg, in_window);
            sprintf(msg, "biased big=%d k=%d: congruent to the true difference mod big", big, k);
            check(msg, equal_to_modular);
            sprintf(msg, "biased big=%d k=%d: equals stock wherever stock lies inside the window", big, k);
            check(msg, agrees_with_stock_where_it_must);
        }

    // 3. The boundaries and the cases the patch exists for.
    const int B = 2048, KX = ww::K_X;
    check("biased: d = -K stays -K", ww::biased_axis(1000, 1000 - KX, B, KX) == -KX);
    check("biased: d = -K-1 files on the far side (big-K-1)", ww::biased_axis(1000, 1000 - KX - 1, B, KX) == B - KX - 1);
    check("biased: d = big-K-1 stays", ww::biased_axis(0, B - KX - 1, B, KX) == B - KX - 1);
    check("biased: d = big-K wraps to -K", ww::biased_axis(0, B - KX, B, KX) == -KX);
    check("biased: the bug case -- 1500 px right of the camera is +1500 (stock -548)",
          ww::biased_axis(0, 1500, B, KX) == 1500 && ww::stock_axis(0, 1500, B) == -548);
    check("biased: 1850 right is still +1850; 1900 is not (the window ends at 1888 on the 64-tile map)",
          ww::biased_axis(0, 1850, B, KX) == 1850 && ww::biased_axis(0, 1900, B, KX) == -148);
    check("biased: a sprite 200 px left of the edge (reach > K) is filed on the far side, 1848 -- the documented artifact",
          ww::biased_axis(500, 300, B, KX) == B - 200 && B - 200 < rl::CAP_W - rl::PANEL_W);
    check("biased: at 1920 wide the same sprite lands at 1848 >= the 1760 viewport, so it is not drawn (no artifact)",
          B - 200 >= 1920 - rl::PANEL_W);
    check("biased: 100 px left of the camera is -100 (a sprite partly in view)", ww::biased_axis(500, 400, B, KX) == -100);
    check("biased: across the seam: camera at 2000, sprite at 40 is +88", ww::biased_axis(2000, 40, B, KX) == 88);
    check("biased: across the seam the other way: camera at 40, sprite at 2000 is -88", ww::biased_axis(40, 2000, B, KX) == -88);
    check("biased: an unmasked coordinate (to beyond big) is the same point", ww::biased_axis(100, 100 + B + 50, B, KX) == 50);
    check("biased: negative to (elevation shifts) is the same point", ww::biased_axis(100, -40, B, KX) == -140);
    check("biased: big <= 0 degrades to the difference", ww::biased_axis(5, 9, 0, KX) == 4);

    // 4. What the cap promises: any sprite from K px left of the edge to the far edge of the widest allowed
    // viewport is placed exactly, on the smallest map.
    {
        const int vw = rl::CAP_W - rl::PANEL_W, vh = rl::CAP_H - rl::STRIP_H;
        bool      ok = true;
        for (int cam = 0; cam < B; cam += 61)
            for (int off = -ww::K_X; off < vw; off += 7) {
                const int to = ((cam + off) % B + B) % B;
                if (ww::biased_axis(cam, to, B, ww::K_X) != off) ok = false;
            }
        for (int cam = 0; cam < B; cam += 61)
            for (int off = -ww::K_Y; off < vh; off += 7) {
                const int to = ((cam + off) % B + B) % B;
                if (ww::biased_axis(cam, to, B, ww::K_Y) != off) ok = false;
            }
        check("cap: every offset in [-K, viewport) at the widest allowed size is placed exactly (64-tile map)", ok);
        check("cap: one more tile of width would break that (viewport + K > big)", vw + 32 + ww::K_X > B);
    }

    // 5. K at draw time: big - viewport, floored. Every sprite whose reach fits it is placed exactly and none ghosts.
    check("bias_for: 64-tile map, 2048 wide (viewport 1888) is the floor 160", ww::bias_for(2048, 1888, ww::K_X) == 160);
    check("bias_for: 64-tile map, 1920 wide (viewport 1760) is 288 -- above the deepest reach (280)", ww::bias_for(2048, 1760, ww::K_X) == 288);
    check("bias_for: 64-tile map, 1024 wide (viewport 864) is 1184", ww::bias_for(2048, 864, ww::K_X) == 1184);
    check("bias_for: 128-tile map, 2048 wide is 2208 (the ghost needs reach > K)", ww::bias_for(4096, 1888, ww::K_X) == 2208);
    check("bias_for: vertical, 64-tile map, 1152 tall (viewport 1120) is 928 and 1600 tall (1568) is 480",
          ww::bias_for(2048, 1120, ww::K_Y) == 928 && ww::bias_for(2048, 1568, ww::K_Y) == 480);
    check("bias_for: an unreadable viewport or map gives the floor", ww::bias_for(2048, 0, ww::K_X) == 160 && ww::bias_for(0, 800, ww::K_Y) == 256);
    check("bias_for: a viewport past the map (cannot happen under the cap) is floored, not negative", ww::bias_for(2048, 2100, ww::K_X) == 160);
    {
        // The ghost: a sprite of reach r whose offset v lies in [-r, -K) is filed at v + big. It is on screen iff that is < viewport.
        // Over every size the cap allows on the smallest map: widths up to 1920 are clean for every reach to 280; from 1952 up the\n        // reaches above K = 2048 - viewport ghost, the most at the 2048 cap (K = 160).
        bool clean_below_cap = true, ghost_at_cap = false;
        for (int w = 640; w <= rl::CAP_W; w += 32) {
            const int vw = w - rl::PANEL_W, k = ww::bias_for(2048, vw, ww::K_X);
            for (int reach = 1; reach <= 280; ++reach) {
                const int  v_lo   = -reach;
                const int  placed = ww::biased_axis(0, ((v_lo % 2048) + 2048) % 2048, 2048, k); // = v_lo while v_lo >= -k
                const bool ghost  = (v_lo < -k) && placed < vw;
                if (ghost && w <= 1920) clean_below_cap = false;
                if (ghost && w == rl::CAP_W) ghost_at_cap = true;
            }
        }
        check("ghost: no width up to 1920 ghosts any reach up to 280 on the 64-tile map", clean_below_cap);
        check("ghost: 2048 wide still ghosts reaches 161..280 (the known residue)", ghost_at_cap);
    }

    // the effect / projectile view gate (tile_in_span): equal to the stock test wherever the span is short of the
    // map, the whole axis in view where it is not (the 2048 cap on a 64-tile map: VIEW_TILES 64).
    {
        bool same = true, all_in = true, stock_rejects = false;
        for (int n = 64; n <= 256; n *= 2)
            for (int tiles = 20; tiles + 3 < n && tiles <= 64; ++tiles)
                for (int cam = 0; cam < n; ++cam)
                    for (int t = 0; t < n; ++t)
                        if (ww::tile_in_span(t, cam, tiles, n) != ww::tile_in_span_stock(t, cam, tiles, n)) same = false;
        for (int cam = 0; cam < 64; ++cam)
            for (int t = 0; t < 64; ++t) {
                if (!ww::tile_in_span(t, cam, 64, 64)) all_in = false;
                if (!ww::tile_in_span_stock(t, cam, 64, 64)) stock_rejects = true;
            }
        check("view gate: equal to the stock test while the span is short of the map", same);
        check("view gate: a 64-tile window on the 64-tile map shows every tile (stock rejects most of them)", all_in && stock_rejects);
        check("view gate: 1920 wide (60 tiles) on the 64-tile map: still the stock answer", ww::tile_in_span(5, 10, 60, 64) == ww::tile_in_span_stock(5, 10, 60, 64) &&
                                                                                                ww::tile_in_span(8, 10, 60, 64) == ww::tile_in_span_stock(8, 10, 60, 64));
    }
}

} // namespace

int run_reslisttest() {
    g_checks = g_fails = 0;
    printf("== reslisttest (the generated resolution list, the 16:9 cap, the biased sprite window) ==\n");

    {
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {672, 512}, {928, 704}, {1152, 864}, {1408, 1056}, {960, 512}, {1280, 704}, {1536, 864}, {1920, 1056}};
        expect_list("1920x1080", 1920, 1080, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    {
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {768, 576}, {928, 704}, {1280, 960}, {1536, 1152}, {1920, 1440}, {832, 480}, {1024, 576}, {1280, 704}, {1696, 960}, {2048, 1152}};
        expect_list("2560x1440", 2560, 1440, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    {
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {672, 512}, {928, 704}, {1152, 864}, {1408, 1056}, {1920, 1440}, {960, 512}, {1280, 704}, {1536, 864}, {1920, 1056}};
        expect_list("3840x2160", 3840, 2160, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    {
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {768, 576}, {1056, 800}, {1280, 960}, {1568, 1184}, {768, 480}, {960, 576}, {1280, 800}, {1536, 960}, {1920, 1184}};
        expect_list("1920x1200", 1920, 1200, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    {
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {672, 512}, {800, 608}, {896, 512}, {1088, 608}, {1344, 768}};
        expect_list("1366x768", 1366, 768, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    {
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {672, 512}, {896, 672}, {1056, 800}, {640, 512}, {832, 672}, {1024, 800}, {1280, 1024}};
        expect_list("1280x1024", 1280, 1024, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    { // 16:10 desktop: not wider than 16:9, so the cap alone limits it
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {672, 512}, {864, 640}, {1056, 800}, {1408, 1056}, {1696, 1280}, {832, 512}, {1024, 640}, {1280, 800}, {1696, 1056}, {2048, 1280}};
        expect_list("2560x1600", 2560, 1600, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    { // ultrawide: 16:9 sizes of the desktop's own height (side bars), the same list as 2560x1440
        const sz w[] = {{640, 480}, {800, 576}, {1024, 768}, {768, 576}, {928, 704}, {1280, 960}, {1536, 1152}, {1920, 1440}, {832, 480}, {1024, 576}, {1280, 704}, {1696, 960}, {2048, 1152}};
        expect_list("3440x1440", 3440, 1440, w, (int)(sizeof(w) / sizeof(w[0])));
        expect_list("5120x1440", 5120, 1440, w, (int)(sizeof(w) / sizeof(w[0])));
    }

    // Auto: desktop / display scale, floored to 32, then clamped.
    expect_auto("1920x1080", 1920, 1080, 100, 1920, 1056);
    expect_auto("1920x1080", 1920, 1080, 150, 1280, 768);  // 1280x704 would be below 1024x768 in height
    expect_auto("2560x1440", 2560, 1440, 100, 2048, 1152); // 2560 is over the cap: the largest generated entry
    expect_auto("2560x1440", 2560, 1440, 125, 2048, 1152); // 2560 / 1.25 = 2048: exactly the cap
    expect_auto("2560x1440", 2560, 1440, 200, 1280, 768);
    expect_auto("2560x1440", 2560, 1440, 150, 1696, 960);
    expect_auto("3840x2160", 3840, 2160, 100, 1920, 1056); // over the cap: the largest generated entry
    expect_auto("3840x2160", 3840, 2160, 150, 1920, 1056);
    expect_auto("2560x1600", 2560, 1600, 100, 2048, 1280);
    expect_auto("3440x1440", 3440, 1440, 100, 2048, 1152); // ultrawide: 16:9 of its own height
    expect_auto("5120x1440", 5120, 1440, 100, 2048, 1152);
    expect_auto("3840x2160", 3840, 2160, 200, 1920, 1056);
    expect_auto("1920x1200", 1920, 1200, 100, 1920, 1184);
    expect_auto("1366x768", 1366, 768, 100, 1344, 768);
    expect_auto("1280x1024", 1280, 1024, 100, 1280, 1024);
    expect_auto("1280x720", 1280, 720, 100, 1280, 704); // never above the desktop beats never below 1024x768
    expect_auto("5120x2880", 5120, 2880, 100, 2048, 1152);
    {
        const rl::entry a = rl::auto_size(1920, 1080, 0); // a bad scale reads as 100%
        check("auto: scale 0 reads as 100%", a.w == 1920 && a.h == 1056);
    }

    // The presenter's integer-scale rule.
    check("integer: 1920x1056 on 1920x1080 (1:1)", rl::integer_scale_ok(1920, 1080, 1920, 1056));
    check("integer: 960x512 on 1920x1080 (2x)", rl::integer_scale_ok(1920, 1080, 960, 512));
    check("integer: 1408x1056 on 1920x1080 (4:3 1:1, 2% slack)", rl::integer_scale_ok(1920, 1080, 1408, 1056));
    check("integer: 640x480 on 2560x1440 (3x)", rl::integer_scale_ok(2560, 1440, 640, 480));
    check("fit: 1280x704 on 1920x1080 (1.5x)", !rl::integer_scale_ok(1920, 1080, 1280, 704));
    check("fit: 1024x768 on 1920x1080 (1.4x)", !rl::integer_scale_ok(1920, 1080, 1024, 768));
    check("fit: 640x480 on 1920x1080 (2.25x)", !rl::integer_scale_ok(1920, 1080, 640, 480));
    check("fit: a picture larger than the desktop", !rl::integer_scale_ok(1280, 720, 1920, 1056));
    check("fit: nonsense sizes", !rl::integer_scale_ok(0, 1080, 640, 480) && !rl::integer_scale_ok(1920, 1080, 0, 480));

    // parse_size / valid_size.
    int w = 0, h = 0;
    check("parse 1920x1056", rl::parse_size("1920x1056", &w, &h) && w == 1920 && h == 1056);
    check("parse 1280X704", rl::parse_size("1280X704", &w, &h) && w == 1280 && h == 704);
    check("parse rejects auto / empty / trailing junk / missing side",
          !rl::parse_size("auto", &w, &h) && !rl::parse_size("", &w, &h) && !rl::parse_size("1920x1056 ", &w, &h) &&
              !rl::parse_size("1920x", &w, &h) && !rl::parse_size("x1056", &w, &h) && !rl::parse_size(nullptr, &w, &h));
    check("valid: 640x480 and 2048x1600 are the extremes", rl::valid_size(640, 480) && rl::valid_size(2048, 1600));
    check("invalid: 1920x1080 (height off the grid), 2144x1440, 2112x1184 and 2080x1152 (over the cap), 608x480 (narrow), 3840x2144",
          !rl::valid_size(1920, 1080) && !rl::valid_size(2144, 1440) && !rl::valid_size(2112, 1184) && !rl::valid_size(2080, 1152) &&
              !rl::valid_size(608, 480) && !rl::valid_size(3840, 2144));
    // The cap: the viewport (W-160 x H-32) plus the sprite window's bias must fit the 64-tile map (2048 px), and
    // the older tile window (viewport plus two tiles under 64 tiles) is looser than that.
    check("cap: 2048 wide is the last width whose view fits the sprite window of a 64-tile map (K_X = 160)",
          rl::CAP_W == 2048 && mh::gfx::wrapwin::K_X == 160 && (rl::CAP_W - 160) + mh::gfx::wrapwin::K_X == rl::MIN_MAP_TILES * 32 &&
              (rl::CAP_W + 32 - 160) + mh::gfx::wrapwin::K_X > rl::MIN_MAP_TILES * 32);
    check("cap: the tile-window limit (2112) is looser than the sprite-window limit",
          rl::TILE_WIN_W == 2112 && rl::TILE_WIN_W > rl::SPRITE_WIN_W);
    check("cap: the height cap (1600) is below both windows, which are 1984 (tiles) and 1824 (sprites)",
          rl::CAP_H == 1600 && rl::TILE_WIN_H == 1984 && rl::SPRITE_WIN_H == 1824 &&
              (rl::CAP_H - 32) + mh::gfx::wrapwin::K_Y <= rl::MIN_MAP_TILES * 32);
    check("cap: 2560x1440 and 2112x1184 outgrow a 64-tile map; 1920x1056 and 1024x768 do not",
          rl::view_wraps_smallest_map(2560, 1440) && rl::view_wraps_smallest_map(2112, 1184) &&
              !rl::view_wraps_smallest_map(1920, 1056) && !rl::view_wraps_smallest_map(1024, 768));
    check("16:9: 1920x1056 (1080 floored), 2048x1152 and the 4:3 sizes pass; 1696x704 and 2560x1080 do not",
          rl::within_16x9(1920, 1056) && rl::within_16x9(2048, 1152) && rl::within_16x9(1408, 1056) &&
              !rl::within_16x9(1696, 704) && !rl::within_16x9(2560, 1080));
    {
        bool      all_ok  = true; // no generated entry for any plausible desktop may wrap the smallest map
        bool      wide_ok = true; // ... and none is wider than 16:9 (the stock three and the 4:3 family are narrower)
        const int ds[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 2880}, {2560, 1600}, {3440, 1440}, {5120, 1440}, {7680, 4320}};
        for (const auto &d : ds) {
            rl::entry l[rl::MAX_ENTRIES];
            const int n = rl::generate(d[0], d[1], l, rl::MAX_ENTRIES);
            for (int i = 0; i < n; ++i)
                if (rl::view_wraps_smallest_map(l[i].w, l[i].h)) all_ok = false;
            for (int i = 0; i < n; ++i)
                if (!rl::within_16x9(l[i].w, l[i].h)) wide_ok = false;
            for (int pct = 100; pct <= 200; pct += 25) {
                const rl::entry a = rl::auto_size(d[0], d[1], pct);
                if (rl::view_wraps_smallest_map(a.w, a.h)) all_ok = false;
                if (!rl::within_16x9(a.w, a.h)) wide_ok = false;
            }
        }
        check("cap: no generated entry or Auto outgrows the smallest map on any tested desktop", all_ok);
        check("16:9: no generated entry or Auto is wider than 16:9 on any tested desktop", wide_ok);
    }

    wrap_window_checks();

    // insert_sorted: a requested size lands in area order after the stock three; a duplicate is not added.
    {
        rl::entry list[rl::MAX_ENTRIES];
        int       n = rl::generate(1920, 1080, list, rl::MAX_ENTRIES), cnt = 0;
        const int ix = rl::insert_sorted(list, n, rl::MAX_ENTRIES, rl::entry{1600, 896, rl::k_requested}, &cnt);
        check("insert 1600x896 (16:9) -> a new entry between 1536x864 and 1920x1056", cnt == n + 1 && ix >= 3 && list[ix].w == 1600 &&
                                                                                          list[ix - 1].w == 1536 && list[ix + 1].w == 1920);
        {
            rl::entry l2[rl::MAX_ENTRIES];
            int       n2 = rl::generate(1920, 1080, l2, rl::MAX_ENTRIES), c2 = 0;
            const int i43 = rl::insert_sorted(l2, n2, rl::MAX_ENTRIES, rl::entry{1280, 960, rl::k_requested}, &c2);
            check("insert 1280x960 (4:3) -> between 1152x864 and 1408x1056, ahead of every 16:9 size",
                  c2 == n2 + 1 && l2[i43 - 1].w == 1152 && l2[i43 + 1].w == 1408);
            const int io = rl::insert_sorted(l2, c2, rl::MAX_ENTRIES, rl::entry{1280, 800, rl::k_requested}, &c2);
            check("insert 1280x800 (16:10) -> last, after every 16:9 size", io == c2 - 1);
        }
        {
            check("aspect_group: 4:3 family and floored natives",
                  rl::aspect_group(1408, 1056) == rl::grp_4x3 && rl::aspect_group(672, 512) == rl::grp_4x3 &&
                      rl::aspect_group(1056, 800) == rl::grp_4x3 && rl::aspect_group(1568, 1184) == rl::grp_4x3);
            check("aspect_group: 16:9 incl. floored heights (1920x1056, 960x512, 1344x768)",
                  rl::aspect_group(1920, 1056) == rl::grp_16x9 && rl::aspect_group(960, 512) == rl::grp_16x9 &&
                      rl::aspect_group(1344, 768) == rl::grp_16x9 && rl::aspect_group(2048, 1152) == rl::grp_16x9);
            check("aspect_group: 16:10, 5:4 and the stock slot-1 shape are others",
                  rl::aspect_group(1280, 800) == rl::grp_other && rl::aspect_group(1280, 1024) == rl::grp_other &&
                      rl::aspect_group(640, 512) == rl::grp_other && rl::aspect_group(768, 480) == rl::grp_other);
        }
        n               = cnt;
        const int again = rl::insert_sorted(list, n, rl::MAX_ENTRIES, rl::entry{1600, 896, rl::k_requested}, &cnt);
        check("insert the same size again -> same index, count unchanged", again == ix && cnt == n);
        const int stock = rl::insert_sorted(list, n, rl::MAX_ENTRIES, rl::entry{1024, 768, rl::k_requested}, &cnt);
        check("insert 1024x768 -> the stock slot 2", stock == 2 && cnt == n);
        cnt = n;
        check("insert into a full list -> -1", rl::insert_sorted(list, n, n, rl::entry{1664, 928, rl::k_requested}, &cnt) == -1);
    }

    // The label text.
    {
        char lab[64];
        rl::describe(rl::entry{1920, 1056, rl::k_native}, 1920, 1080, lab, sizeof(lab));
        check("label 1920x1056", strcmp(lab, "1920x1056 (1:1, sharpest)") == 0);
        rl::describe(rl::entry{1280, 704, rl::k_native}, 1920, 1080, lab, sizeof(lab));
        check("label 1280x704", strcmp(lab, "1280x704 (1.5x)") == 0);
        rl::describe(rl::entry{960, 512, rl::k_native}, 1920, 1080, lab, sizeof(lab));
        check("label 960x512", strcmp(lab, "960x512 (2x)") == 0);
        rl::describe(rl::entry{1536, 864, rl::k_native}, 3840, 2160, lab, sizeof(lab));
        check("label 1536x864 on 4K", strcmp(lab, "1536x864 (2.5x)") == 0);
        rl::describe(rl::entry{2048, 1152, rl::k_native}, 2560, 1440, lab, sizeof(lab));
        check("label 2048x1152 on 1440p", strcmp(lab, "2048x1152 (1.25x)") == 0);
        rl::describe(rl::entry{1408, 1056, rl::k_4x3}, 1920, 1080, lab, sizeof(lab));
        check("label 4:3 1408x1056", strcmp(lab, "1408x1056 (4:3, 1:1)") == 0);
        rl::describe(rl::entry{640, 480, rl::k_stock}, 1920, 1080, lab, sizeof(lab));
        check("label stock", strcmp(lab, "640x480 (classic)") == 0);
        char tiny[8];
        rl::describe(rl::entry{1920, 1056, rl::k_native}, 1920, 1080, tiny, sizeof(tiny));
        check("label into a tiny buffer stays terminated", strlen(tiny) == 7);
    }

    printf("reslisttest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
