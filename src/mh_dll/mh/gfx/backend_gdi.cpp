//
// backend_gdi.cpp -- PT-GFX1: the GDI presenter (gfx/present_backend.h).
//
// One StretchDIBits per frame from the game's 16-bit surface into the window's client area. It needs
// nothing but GDI, so it runs everywhere the game does -- including the GPU-less rig VMs, which is why
// it is the fallback every other backend drops to.
//
// The source is described as a top-down BI_BITFIELDS DIB with the surface's exact channel masks, so the
// 16-bit pixels go to the driver untouched (no conversion pass of ours). biWidth is the PITCH in pixels,
// not the visible width, so a surface whose rows are padded still reads correctly; the source rect then
// selects the visible width.
//
// Scaling: aspect-preserving, centred, black bars, POINT filter (COLORONCOLOR -- HALFTONE would blur
// the pixel art and costs ~10x). At exactly 1:1 it uses SetDIBitsToDevice, which skips the stretch
// path entirely. The window mode (windowed / borderless) is the owner's business (ddraw_own.cpp); this
// backend only fills whatever client rect it is given.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>

#include "gfx/present_backend.h"
#include "gfx/ui_layout.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace mh::gfx {
namespace {

struct dib_header {
    BITMAPINFOHEADER bmi;
    DWORD            masks[3];
};

class gdi_backend final : public present_backend {
public:
    const char *name() const override { return "gdi"; }

    bool attach(HWND hwnd, const backend_config &cfg) override {
        if (!hwnd || !IsWindow(hwnd)) return false;
        hwnd_ = hwnd;
        cfg_  = cfg;
        return true;
    }

    void on_mode(int, int, pixel_format) override { bars_valid_ = false; }

    bool target_size(int &w, int &h) override {
        RECT cr;
        if (!hwnd_ || !GetClientRect(hwnd_, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
        w = cr.right;
        h = cr.bottom;
        return true;
    }

    bool present(const frame_view &f) override {
        if (!hwnd_ || !f.pixels || f.width <= 0 || f.height <= 0) return false;
        RECT cr;
        if (!GetClientRect(hwnd_, &cr) || cr.right <= 0 || cr.bottom <= 0) return false; // minimised
        const int cw = cr.right, ch = cr.bottom;

        // RES4: ui_scale above 100 = several quads (the single-rect path below is the 1-quad case and is untouched).
        // (RES23-S1: the layout is the present's view, built once in present_surface; RES18: a crop is a several-quad layout at 100 too)
        if (f.view && f.view->valid && f.view->multi) return present_quads(f, f.view->layout);

        // Aspect fit (present_backend.h fit_image_rect -- the same rect the mouse mapping uses).
        const RECT ir = fit_image_rect(cw, ch, f.width, f.height, cfg_.integer_scale);
        const int  dw = ir.right - ir.left, dh = ir.bottom - ir.top;
        const int  dx = ir.left, dy = ir.top;

        dib_header h;
        ZeroMemory(&h, sizeof(h));
        h.bmi.biSize        = sizeof(BITMAPINFOHEADER);
        h.bmi.biWidth       = f.pitch / 2;
        h.bmi.biHeight      = -f.height; // top-down
        h.bmi.biPlanes      = 1;
        h.bmi.biBitCount    = 16;
        h.bmi.biCompression = BI_BITFIELDS;
        if (f.format == pixel_format::rgb555) {
            h.masks[0] = 0x7C00;
            h.masks[1] = 0x03E0;
        } else {
            h.masks[0] = 0xF800;
            h.masks[1] = 0x07E0;
        }
        h.masks[2] = 0x001F;

        HDC dc = GetDC(hwnd_);
        if (!dc) return false;
        // Bars only when the layout changed (or the window was reshaped) -- repainting them every
        // frame would flicker on some drivers and is wasted work in the common 1:1 case (no bars).
        if (!bars_valid_ || cw != last_cw_ || ch != last_ch_ || dw != last_dw_ || dh != last_dh_) {
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            RECT   r;
            if (dy > 0) {
                r = {0, 0, cw, dy};
                FillRect(dc, &r, black);
                r = {0, dy + dh, cw, ch};
                FillRect(dc, &r, black);
            }
            if (dx > 0) {
                r = {0, dy, dx, dy + dh};
                FillRect(dc, &r, black);
                r = {dx + dw, dy, cw, dy + dh};
                FillRect(dc, &r, black);
            }
            last_cw_ = cw, last_ch_ = ch, last_dw_ = dw, last_dh_ = dh;
            bars_valid_ = true;
        }
        auto blit = [&](HDC d) -> int {
            if (dw == f.width && dh == f.height)
                return SetDIBitsToDevice(d, dx, dy, (DWORD)f.width, (DWORD)f.height, 0, 0, 0, (UINT)f.height, f.pixels, (const BITMAPINFO *)&h,
                                         DIB_RGB_COLORS);
            SetStretchBltMode(d, COLORONCOLOR);
            return StretchDIBits(d, dx, dy, dw, dh, 0, 0, f.width, f.height, f.pixels, (const BITMAPINFO *)&h, DIB_RGB_COLORS, SRCCOPY);
        };
        const int lines = blit(dc);
        capture_pass(dc, cw, ch, blit); // RES23-S0: a no-op unless `bcapture` armed it
        ReleaseDC(hwnd_, dc);
        return lines > 0;
    }

    void on_window_changed() override { bars_valid_ = false; }

    void set_config(const backend_config &cfg) override {
        if (cfg.ui_scale_pct != cfg_.ui_scale_pct || cfg.integer_scale != cfg_.integer_scale) bars_valid_ = false;
        cfg_.ui_scale_pct  = cfg.ui_scale_pct;
        cfg_.integer_scale = cfg.integer_scale;
    }

    void detach() override { hwnd_ = nullptr; }

    // RES23-S0 bcapture. The gdi presenter has no back buffer to read: it draws straight into the window DC, and a read of the
    // window (BitBlt / PrintWindow) is clipped to the monitor (the rig's 2560x1440 borderless window sits on a 1920x1080 screen) and
    // PrintWindow made the game process fail fast. So the armed present runs the SAME draw calls a second time into a client-sized
    // memory DIB (black to start, as the bars are) -- the pixels GDI produces for this frame, whatever the monitor shows of them.
    void arm_output_capture() override {
        out_armed_ = true;
        out_ok_    = false;
    }
    bool take_output_capture(std::vector<uint32_t> &bgra, int &w, int &h) override {
        if (!out_ok_) return false;
        out_ok_ = false;
        bgra.swap(out_px_);
        w = out_w_;
        h = out_h_;
        return true;
    }

private:
    // Run `draw(dc)` against a cw x ch memory DIB when a capture is armed and keep the result (BGRA, alpha 255).
    template <class F>
    void capture_pass(HDC like, int cw, int ch, F draw) {
        if (!out_armed_) return;
        out_armed_ = false;
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = cw;
        bi.bmiHeader.biHeight      = -ch; // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void   *bits               = nullptr;
        HBITMAP bmp                = CreateDIBSection(like, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            return;
        }
        HDC     mem = CreateCompatibleDC(like);
        HGDIOBJ old = SelectObject(mem, bmp);
        draw(mem);
        GdiFlush();
        out_px_.resize((size_t)cw * ch);
        memcpy(out_px_.data(), bits, (size_t)cw * ch * 4);
        for (uint32_t &p : out_px_) p |= 0xff000000u;
        out_w_  = cw;
        out_h_  = ch;
        out_ok_ = true;
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }

    // RES4: one StretchDIBits per quad (map 1:1, panel and strip scaled), each from a sub-rectangle of the frame.
    // The bars around the composition are painted when the layout changes, as the single-rect path does.
    bool present_quads(const frame_view &f, const uilayout::layout &lay) {
        dib_header h;
        ZeroMemory(&h, sizeof(h));
        h.bmi.biSize        = sizeof(BITMAPINFOHEADER);
        h.bmi.biWidth       = f.pitch / 2;
        h.bmi.biHeight      = -f.height; // top-down
        h.bmi.biPlanes      = 1;
        h.bmi.biBitCount    = 16;
        h.bmi.biCompression = BI_BITFIELDS;
        h.masks[0]          = f.format == pixel_format::rgb555 ? 0x7C00 : 0xF800;
        h.masks[1]          = f.format == pixel_format::rgb555 ? 0x03E0 : 0x07E0;
        h.masks[2]          = 0x001F;

        HDC dc = GetDC(hwnd_);
        if (!dc) return false;
        const uilayout::irect &u = lay.used;
        if (!bars_valid_ || lay.screen_w != last_cw_ || lay.screen_h != last_ch_ || u.x0 != last_dx_ || u.y0 != last_dy_ ||
            u.w() != last_dw_ || u.h() != last_dh_) {
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            RECT   r     = {0, 0, lay.screen_w, u.y0};
            if (u.y0 > 0) FillRect(dc, &r, black);
            r = {0, u.y1, lay.screen_w, lay.screen_h};
            if (u.y1 < lay.screen_h) FillRect(dc, &r, black);
            r = {0, u.y0, u.x0, u.y1};
            if (u.x0 > 0) FillRect(dc, &r, black);
            r = {u.x1, u.y0, lay.screen_w, u.y1};
            if (u.x1 < lay.screen_w) FillRect(dc, &r, black);
            last_cw_ = lay.screen_w, last_ch_ = lay.screen_h, last_dx_ = u.x0, last_dy_ = u.y0, last_dw_ = u.w(), last_dh_ = u.h();
            bars_valid_ = true;
        }
        auto paint = [&](HDC d) -> int {
            SetStretchBltMode(d, COLORONCOLOR);
            int lines = 0;
            for (int i = 0; i < lay.nquads; ++i) {
                const uilayout::quad &q = lay.q[i];
                if (lay.quad_is_layered(i)) continue; // 6b: drawn below as an alpha overlay
                // The source rectangle is described by the DIB itself (a pointer to its first pixel, a height of its rows,
                // the surface pitch as the stride) and drawn from (0,0): StretchDIBits reads ySrc of a TOP-DOWN DIB from the
                // bottom when it stretches (measured: the panel came out 128 rows low), so a nonzero ySrc is never passed.
                h.bmi.biHeight   = -q.src.h();
                const uint8_t *p = (const uint8_t *)f.pixels + (size_t)q.src.y0 * f.pitch + (size_t)q.src.x0 * 2;
                lines += StretchDIBits(d, q.dst.x0, q.dst.y0, q.dst.w(), q.dst.h(), 0, 0, q.src.w(), q.src.h(), p, (const BITMAPINFO *)&h,
                                       DIB_RGB_COLORS, SRCCOPY);
            }
            for (int i = 0; i < f.overlays.n; ++i) draw_cursor(d, f.overlays.item[i].ov); // RES23-S5: the overlay list, bottom to top
            return lines;
        };
        const int lines = paint(dc);
        capture_pass(dc, lay.screen_w, lay.screen_h, paint); // RES23-S0: a no-op unless `bcapture` armed it
        ReleaseDC(hwnd_, dc);
        return lines > 0;
    }

    // RES4 stage 5: the owned cursor over the quads -- AlphaBlend (msimg32, loaded on first use) of a premultiplied 32-bit DIB.
    void draw_cursor(HDC dc, const uilayout::cursor_overlay &c) {
        typedef BOOL(WINAPI * alpha_blend_fn)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
        static alpha_blend_fn fn    = nullptr;
        static bool           tried = false;
        if (!tried) {
            tried = true;
            if (HMODULE m = LoadLibraryA("msimg32.dll")) fn = (alpha_blend_fn)GetProcAddress(m, "AlphaBlend");
        }
        if (!fn || c.w <= 0 || c.h <= 0) return;
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = c.w;
        bi.bmiHeader.biHeight      = -c.h; // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void   *bits               = nullptr;
        HBITMAP bmp                = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            return;
        }
        memcpy(bits, c.px, (size_t)c.w * c.h * 4);
        HDC           mem = CreateCompatibleDC(dc);
        HGDIOBJ       old = SelectObject(mem, bmp);
        BLENDFUNCTION bf  = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        fn(dc, c.x, c.y, c.w, c.h, mem, 0, 0, c.w, c.h, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    int last_dx_ = 0, last_dy_ = 0;

    HWND                  hwnd_ = nullptr;
    backend_config        cfg_;
    bool                  out_armed_ = false, out_ok_ = false; // RES23-S0
    int                   out_w_ = 0, out_h_ = 0;
    std::vector<uint32_t> out_px_;
    bool                  bars_valid_ = false;
    int                   last_cw_ = 0, last_ch_ = 0, last_dw_ = 0, last_dh_ = 0;
};

} // namespace

present_backend *make_gdi_backend() { return new gdi_backend(); }

} // namespace mh::gfx
