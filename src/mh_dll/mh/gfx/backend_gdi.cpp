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

#include "gfx/present_backend.h"

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

    bool present(const frame_view &f) override {
        if (!hwnd_ || !f.pixels || f.width <= 0 || f.height <= 0) return false;
        RECT cr;
        if (!GetClientRect(hwnd_, &cr) || cr.right <= 0 || cr.bottom <= 0) return false; // minimised
        const int cw = cr.right, ch = cr.bottom;

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
        int lines;
        if (dw == f.width && dh == f.height) {
            lines = SetDIBitsToDevice(dc, dx, dy, (DWORD)f.width, (DWORD)f.height, 0, 0, 0, (UINT)f.height, f.pixels,
                                      (const BITMAPINFO *)&h, DIB_RGB_COLORS);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);
            lines = StretchDIBits(dc, dx, dy, dw, dh, 0, 0, f.width, f.height, f.pixels, (const BITMAPINFO *)&h,
                                  DIB_RGB_COLORS, SRCCOPY);
        }
        ReleaseDC(hwnd_, dc);
        return lines > 0;
    }

    void on_window_changed() override { bars_valid_ = false; }

    void detach() override { hwnd_ = nullptr; }

private:
    HWND           hwnd_ = nullptr;
    backend_config cfg_;
    bool           bars_valid_ = false;
    int            last_cw_ = 0, last_ch_ = 0, last_dw_ = 0, last_dh_ = 0;
};

} // namespace

present_backend *make_gdi_backend() { return new gdi_backend(); }

} // namespace mh::gfx
