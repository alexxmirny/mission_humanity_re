//
// scale_filter_selftest.cpp -- `net_selftest.exe scalefiltertest`: PT-GFX6, the d3d11 presenter's
// scaling filters measured on the REAL pipeline (gfx/backend_d3d11.cpp: the same device, shaders,
// samplers, viewport and cbuffer code the game presents with), read back from the back buffer and
// compared with a CPU reference of each filter written from its definition, not from the shader:
//
//   point   nearest texel of the output pixel's centre;
//   linear  bilinear at the output pixel's centre (texel centres at k + 0.5);
//   sharp   libretro sharp-bilinear-simple: prescale k = max(floor(scale), 1), bilinear on the
//           k-times nearest-upscaled image -- i.e. the next texel's weight is the ramp
//           clamp((p - edge) * k / (edge-band), ...) as that shader defines it;
//   area    the box filter: an output pixel's weight on each texel is the fraction of its footprint
//           (1/scale texels wide) that texel covers.
//
// Two non-integer scales on a 1440x1080 target: 640x480 (2.25x) and 1024x768 (1.40625x), plus the
// integer-scale rect (640x480 -> 1280x960, centred), over a random RGB565 frame with a hard-edged
// checker so every filter's edge behaviour is exercised. Pass = PSNR >= 45 dB and max channel error
// <= 3 levels (the hardware's 8-bit sub-texel weights) for the filtered kinds; for point, a pixel may
// differ only where the output centre sits within 1e-3 texel of a texel edge (a tie the rasteriser
// may break either way), and nowhere else.
//
// `scalefiltertest --dump <in.rgb565> <fw> <fh> <ow> <oh> <point|linear|sharp|area> <out.bgra>
// [integer]` renders one raw frame and writes the raw read-back -- the numpy side of the same check
// (and the side-by-side screenshots) run from that.
//
// Needs a D3D11 device; WARP is accepted exactly as the presenter accepts it. No device at all is a
// SKIP line and a pass, the presenter's own fallback contract (it drops to gdi).
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
#include "gfx/present_backend.h"

namespace {

namespace gfx = mh::gfx;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

HWND make_window(int w, int h) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc     = {};
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.lpszClassName = "mh_scalefiltertest";
        RegisterClassA(&wc);
        reg = true;
    }
    // WS_POPUP: the client rect IS the window rect. Never shown -- the back buffer is read, not the screen.
    return CreateWindowExA(0, "mh_scalefiltertest", "scalefiltertest", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                           GetModuleHandleA(nullptr), nullptr);
}

// Render one frame through the real backend and read it back. false = no device (or a refused readback).
bool render(const std::vector<uint16_t> &src, int fw, int fh, int ow, int oh, gfx::scale_filter f, bool integer,
            std::vector<uint32_t> &out) {
    HWND hw = make_window(ow, oh);
    if (!hw) return false;
    gfx::present_backend *b = gfx::make_d3d11_backend();
    gfx::backend_config   cfg;
    cfg.filter        = f;
    cfg.integer_scale = integer;
    bool ok           = b && b->attach(hw, cfg);
    if (ok) {
        out.assign((size_t)ow * oh, 0);
        gfx::d3d11_request_readback(out.data(), ow, oh);
        gfx::frame_view fv = {src.data(), fw, fh, fw * 2, gfx::pixel_format::rgb565};
        b->present(fv);
        ok = gfx::d3d11_readback_state() == 1;
        b->detach();
    }
    delete b;
    DestroyWindow(hw);
    return ok;
}

// ---- CPU reference --------------------------------------------------------------------------------
// The kernels live in gfx/cpu_scale.{h,cpp} (shared with the background scaler in
// seams/video_mode_guards.cpp, which `bgscaletest` measures); this suite only places them on the
// presenter's image rect and converts to the back buffer's 8-bit channels.
void reference(const std::vector<uint16_t> &s, int fw, int fh, int ow, int oh, gfx::scale_filter f, bool integer,
               std::vector<uint32_t> &out, std::vector<uint8_t> *tie_mask) {
    const RECT   ir = gfx::fit_image_rect(ow, oh, fw, fh, integer);
    const int    rw = ir.right - ir.left, rh = ir.bottom - ir.top;
    const double sx = (double)rw / fw, sy = (double)rh / fh;
    out.assign((size_t)ow * oh, 0xFF000000u);
    if (tie_mask) tie_mask->assign((size_t)ow * oh, 0);
    for (int oy = ir.top; oy < ir.bottom; ++oy) {
        const double py = (oy - ir.top + 0.5) / sy;
        for (int ox = ir.left; ox < ir.right; ++ox) {
            const double               px = (ox - ir.left + 0.5) / sx;
            const gfx::cpu_scale::rgbf c  = gfx::cpu_scale::sample(s.data(), fw, fh, false, (int)f, px, py, sx, sy);
            const uint32_t             r = (uint32_t)lround(c.r * 255.0f), g = (uint32_t)lround(c.g * 255.0f),
                           b          = (uint32_t)lround(c.b * 255.0f);
            out[(size_t)oy * ow + ox] = 0xFF000000u | (r << 16) | (g << 8) | b;
            if (tie_mask) {
                const double fx = px - floor(px), fy = py - floor(py);
                if (fx < 1e-3 || fx > 1 - 1e-3 || fy < 1e-3 || fy > 1 - 1e-3) (*tie_mask)[(size_t)oy * ow + ox] = 1;
            }
        }
    }
}

struct diff_stats {
    int    max_diff   = 0;
    double psnr       = 0.0;
    long   mismatched = 0; // pixels with any channel differing
    long   untied     = 0; // ... outside the tie mask (point only)
};

diff_stats compare(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, const std::vector<uint8_t> *tie) {
    diff_stats st;
    double     se = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        bool any = false;
        for (int sh = 0; sh < 24; sh += 8) {
            const int d = abs((int)((a[i] >> sh) & 0xFF) - (int)((b[i] >> sh) & 0xFF));
            if (d > st.max_diff) st.max_diff = d;
            se += (double)d * d;
            any = any || d != 0;
        }
        if (any) {
            ++st.mismatched;
            if (tie && !(*tie)[i]) ++st.untied;
        }
    }
    const double mse = se / (a.size() * 3.0);
    st.psnr          = mse <= 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
    return st;
}

bool parse_filter(const char *s, gfx::scale_filter *f) {
    static const char *const N[] = {"point", "linear", "sharp", "area"};
    for (int i = 0; i < 4; ++i)
        if (strcmp(s, N[i]) == 0) {
            *f = (gfx::scale_filter)i;
            return true;
        }
    return false;
}

int run_dump(int argc, char **argv) {
    // argv: [0]exe [1]scalefiltertest [2]--dump [3]in [4]fw [5]fh [6]ow [7]oh [8]filter [9]out [10]integer?
    if (argc < 10) {
        printf("usage: scalefiltertest --dump <in.rgb565> <fw> <fh> <ow> <oh> <filter> <out.bgra> [integer]\n");
        return 2;
    }
    const int         fw = atoi(argv[4]), fh = atoi(argv[5]), ow = atoi(argv[6]), oh = atoi(argv[7]);
    gfx::scale_filter f;
    if (fw <= 0 || fh <= 0 || ow <= 0 || oh <= 0 || !parse_filter(argv[8], &f)) {
        printf("scalefiltertest --dump: bad size or filter\n");
        return 2;
    }
    const bool            integer = argc > 10 && strcmp(argv[10], "integer") == 0;
    std::vector<uint16_t> src((size_t)fw * fh);
    FILE                 *in = fopen(argv[3], "rb");
    if (!in || fread(src.data(), 2, src.size(), in) != src.size()) {
        printf("scalefiltertest --dump: cannot read %s\n", argv[3]);
        if (in) fclose(in);
        return 2;
    }
    fclose(in);
    std::vector<uint32_t> out;
    if (!render(src, fw, fh, ow, oh, f, integer, out)) {
        printf("scalefiltertest --dump: no D3D11 device / readback failed\n");
        return 1;
    }
    FILE *o = fopen(argv[9], "wb");
    if (!o) return 2;
    fwrite(out.data(), 4, out.size(), o);
    fclose(o);
    printf("scalefiltertest --dump: %dx%d -> %dx%d %s%s written to %s\n", fw, fh, ow, oh, argv[8],
           integer ? " integer" : "", argv[9]);
    return 0;
}

} // namespace

int run_scalefiltertest(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[2], "--dump") == 0) return run_dump(argc, argv);
    printf("=== scalefiltertest (PT-GFX6: d3d11 scaling filters vs a CPU reference) ===\n");

    // RES6 cursor carry (pure, no device): a cursor kept on its screen point across the galaxy screen's switch from
    // a 1696x960 picker size to 1024x768 and back, on the 2560x1440 desktop of the user's report (fit: the 1696x960
    // image is (8,0)-(2552,1440), the 1024x768 one (320,0)-(2240,1440) with 320 px side bars).
    {
        const RECT big = gfx::fit_image_rect(2560, 1440, 1696, 960), nat = gfx::fit_image_rect(2560, 1440, 1024, 768);
        check("carry: 1696x960 fit rect on 2560x1440", big.left == 8 && big.top == 0 && big.right == 2552 && big.bottom == 1440);
        check("carry: 1024x768 fit rect on 2560x1440", nat.left == 320 && nat.top == 0 && nat.right == 2240 && nat.bottom == 1440);
        // The report's enter (1607,205): its screen point is in the new mode's right bar -> the image's right edge.
        POINT p = gfx::remap_cursor_across_modes(big, 1696, 960, nat, 1024, 768, 1607, 205);
        check("carry: enter (1607,205) -> (1023,164), clamped to the edge", p.x == 1023 && p.y == 164);
        // The report's exit (729,710) -> the same screen point in 1696x960.
        p = gfx::remap_cursor_across_modes(nat, 1024, 768, big, 1696, 960, 729, 710);
        check("carry: exit (729,710) -> (1119,888)", p.x == 1119 && p.y == 888);
        // A point inside both images survives the round trip within one pixel.
        p             = gfx::remap_cursor_across_modes(big, 1696, 960, nat, 1024, 768, 600, 500);
        const POINT b = gfx::remap_cursor_across_modes(nat, 1024, 768, big, 1696, 960, p.x, p.y);
        check("carry: (600,500) round trip within 1 px", b.x >= 599 && b.x <= 601 && b.y >= 499 && b.y <= 501);
        // The centre maps to the centre (what the stock re-centre gives, so a centred cursor does not move).
        p = gfx::remap_cursor_across_modes(big, 1696, 960, nat, 1024, 768, 848, 480);
        check("carry: centre -> centre", p.x == 512 && p.y == 384);
        // Integer scale on the same desktop: 1024x768 x1 = (768,336)-(1792,1104); a point keeps its screen pixel.
        const RECT ni = gfx::fit_image_rect(2560, 1440, 1024, 768, true);
        p             = gfx::remap_cursor_across_modes(big, 1696, 960, ni, 1024, 768, 848, 480);
        check("carry: integer-scale target keeps the screen point", p.x == 512 && p.y == 384);
        // Stretch (the image fills the client): the map is the plain mode ratio.
        const RECT full = {0, 0, 2560, 1440};
        p               = gfx::remap_cursor_across_modes(full, 1696, 960, full, 1024, 768, 1272, 720);
        check("carry: stretch is the mode ratio", p.x == 768 && p.y == 576);
    }

    struct shape {
        int  fw, fh, ow, oh;
        bool integer;
    };
    const shape              shapes[] = {{640, 480, 1440, 1080, false}, {1024, 768, 1440, 1080, false}, {640, 480, 1440, 1080, true}};
    static const char *const NAMES[]  = {"point", "linear", "sharp", "area"};

    for (const shape &sh : shapes) {
        // Random 565 noise with a hard 8x8 black/white checker over the left half: noise tests the
        // weights everywhere, the checker tests the edge band the filters differ on.
        std::vector<uint16_t> src((size_t)sh.fw * sh.fh);
        uint32_t              seed = 0x1234567u;
        for (int y = 0; y < sh.fh; ++y)
            for (int x = 0; x < sh.fw; ++x) {
                seed                       = seed * 1664525u + 1013904223u;
                src[(size_t)y * sh.fw + x] = (x < sh.fw / 2) ? ((((x >> 3) ^ (y >> 3)) & 1) ? 0xFFFF : 0x0000)
                                                             : (uint16_t)(seed >> 16);
            }
        for (int fi = 0; fi < 4; ++fi) {
            const gfx::scale_filter f = (gfx::scale_filter)fi;
            std::vector<uint32_t>   gpu, ref;
            std::vector<uint8_t>    tie;
            if (!render(src, sh.fw, sh.fh, sh.ow, sh.oh, f, sh.integer, gpu)) {
                printf("  SKIP: no D3D11 device (the presenter would fall back to gdi) -- nothing to measure\n");
                printf("scalefiltertest: %d checks, %d failures\n", g_checks, g_fails);
                return g_fails ? 1 : 0;
            }
            reference(src, sh.fw, sh.fh, sh.ow, sh.oh, f, sh.integer, ref, f == gfx::scale_filter::point ? &tie : nullptr);
            const diff_stats st = compare(gpu, ref, f == gfx::scale_filter::point ? &tie : nullptr);
            printf("  %4dx%-4d -> %dx%d%s %-6s PSNR %6.2f dB  max %3d  mismatched px %ld%s\n", sh.fw, sh.fh, sh.ow, sh.oh,
                   sh.integer ? " integer" : "", NAMES[fi], st.psnr, st.max_diff, st.mismatched,
                   f == gfx::scale_filter::point ? (st.untied ? "  (UNTIED!)" : "  (all at texel-edge ties)") : "");
            char what[160];
            if (f == gfx::scale_filter::point) {
                wsprintfA(what, "%dx%d->%dx%d%s point: every mismatch is a texel-edge tie (%ld untied)", sh.fw, sh.fh,
                          sh.ow, sh.oh, sh.integer ? " int" : "", st.untied);
                check(what, st.untied == 0);
            } else {
                wsprintfA(what, "%dx%d->%dx%d%s %s: max channel error %d <= 3", sh.fw, sh.fh, sh.ow, sh.oh,
                          sh.integer ? " int" : "", NAMES[fi], st.max_diff);
                check(what, st.max_diff <= 3);
                wsprintfA(what, "%dx%d->%dx%d%s %s: PSNR >= 45 dB", sh.fw, sh.fh, sh.ow, sh.oh, sh.integer ? " int" : "",
                          NAMES[fi]);
                check(what, st.psnr >= 45.0);
            }
        }
        if (sh.integer) {
            // The integer rect: 2x, centred, black bars (1440-1280)/2 = 80 and (1080-960)/2 = 60.
            const RECT r = gfx::fit_image_rect(sh.ow, sh.oh, sh.fw, sh.fh, true);
            check("integer rect is 2x centred (80,60)-(1360,1020)", r.left == 80 && r.top == 60 && r.right == 1360 && r.bottom == 1020);
        }
    }
    // The integer option never shrinks below 1x: a client smaller than the frame keeps the aspect fit.
    const RECT small = gfx::fit_image_rect(600, 400, 640, 480, true);
    check("integer below 1x falls back to the aspect fit", small.right - small.left == 533 && small.bottom - small.top == 400);

    printf("scalefiltertest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
