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
struct rgbf {
    float r, g, b;
};

rgbf texel(const std::vector<uint16_t> &s, int fw, int fh, int x, int y) { // clamp addressing
    x                = x < 0 ? 0 : x >= fw ? fw - 1
                                           : x;
    y                = y < 0 ? 0 : y >= fh ? fh - 1
                                           : y;
    const uint16_t p = s[(size_t)y * fw + x];
    // B5G6R5_UNORM reads as v / (2^n - 1); the 8-bit render target stores round(c * 255).
    return {(float)(p >> 11) / 31.0f, (float)((p >> 5) & 63) / 63.0f, (float)(p & 31) / 31.0f};
}

// Per axis: the two texels to blend and the second one's weight, for output pixel centre p (texels).
void axis_weights(gfx::scale_filter f, double p, double scale, int *k0, double *w1) {
    if (f == gfx::scale_filter::point) {
        *k0 = (int)floor(p);
        *w1 = 0.0;
        return;
    }
    double t = scale; // the ramp slope in output px per texel
    if (f == gfx::scale_filter::linear) t = 1.0;
    if (f == gfx::scale_filter::sharp) t = floor(scale) < 1.0 ? 1.0 : floor(scale);
    if (f == gfx::scale_filter::area && t < 1.0) t = 1.0;
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

void reference(const std::vector<uint16_t> &s, int fw, int fh, int ow, int oh, gfx::scale_filter f, bool integer,
               std::vector<uint32_t> &out, std::vector<uint8_t> *tie_mask) {
    const RECT   ir = gfx::fit_image_rect(ow, oh, fw, fh, integer);
    const int    rw = ir.right - ir.left, rh = ir.bottom - ir.top;
    const double sx = (double)rw / fw, sy = (double)rh / fh;
    out.assign((size_t)ow * oh, 0xFF000000u);
    if (tie_mask) tie_mask->assign((size_t)ow * oh, 0);
    for (int oy = ir.top; oy < ir.bottom; ++oy) {
        const double py = (oy - ir.top + 0.5) / sy;
        int          y0;
        double       wy;
        axis_weights(f, py, sy, &y0, &wy);
        for (int ox = ir.left; ox < ir.right; ++ox) {
            const double px = (ox - ir.left + 0.5) / sx;
            int          x0;
            double       wx;
            axis_weights(f, px, sx, &x0, &wx);
            rgbf c;
            if (f == gfx::scale_filter::point) {
                c = texel(s, fw, fh, x0, y0);
            } else {
                const rgbf a = texel(s, fw, fh, x0, y0), bb = texel(s, fw, fh, x0 + 1, y0);
                const rgbf cc = texel(s, fw, fh, x0, y0 + 1), d = texel(s, fw, fh, x0 + 1, y0 + 1);
                auto       mix = [&](float u, float v, float z, float q) {
                    return (float)((1 - wy) * ((1 - wx) * u + wx * v) + wy * ((1 - wx) * z + wx * q));
                };
                c = {mix(a.r, bb.r, cc.r, d.r), mix(a.g, bb.g, cc.g, d.g), mix(a.b, bb.b, cc.b, d.b)};
            }
            const uint32_t r = (uint32_t)lround(c.r * 255.0f), g = (uint32_t)lround(c.g * 255.0f),
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
