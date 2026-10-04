//
// present_backend.h -- PT-GFX: the presenter seam behind the owned DirectDraw.
//
// The game software-renders every frame into a 16-bit back buffer; DirectDraw
// only ever moves those pixels to the window. With [video] backend=own, mh.dll's fake IDirectDraw hands
// each "put this on screen" event (the per-frame Blt to the primary, a Flip, the movie tick's blits) to
// ONE present_backend, which owns the window-side resources. Backends:
//
//   gdi    StretchDIBits into the client rect. Works everywhere, including GPU-less rig VMs.
//   null   composes nothing, shows nothing -- headless lanes (PT-GFX2).
//   d3d11  texture upload + scaled draw; falls back to gdi if device creation fails (PT-GFX3).
//
// Threading: every call comes from the game's main (render) thread. No backend may block for longer than
// one present; frame pacing is the limiter's job, not the backend's (except d3d11 with vsync=1).
//
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

namespace mh::gfx {

enum class pixel_format : uint8_t {
    rgb565, // R 0xF800, G 0x07E0, B 0x001F
    rgb555, // R 0x7C00, G 0x03E0, B 0x001F
};

// The d3d11 presenter's scaling filter ([video] filter). gdi is always point. Numbering is the ini /
// ImGui-panel order and must stay append-only (owned_set_filter takes the integer).
//   point   nearest texel: crisp, but at a non-integer scale texel widths alternate (n / n+1 px).
//   linear  plain bilinear: even, but soft at every scale (dgVoodoo's `Resampling = bilinear`).
//   sharp   libretro "sharp-bilinear-simple": integer prescale floor(scale) (min 1), bilinear for the
//           remainder. Blend band = scale / floor(scale) output px, so BELOW 2x it IS plain bilinear.
//   area    exact box filter (pixel-art AA): each output pixel averages the texels its footprint
//           covers, so every texel edge is a ramp exactly 1 output px wide at ANY scale >= 1.
enum class scale_filter : uint8_t { point,
                                    linear,
                                    sharp,
                                    area };
enum class window_mode : uint8_t { windowed,
                                   borderless };

// One frame as the game composed it. `pixels` is valid only for the duration of present().
struct frame_view {
    const void  *pixels;
    int          width;  // in pixels
    int          height; // in rows
    int          pitch;  // in BYTES (may exceed width * 2)
    pixel_format format;
};

struct backend_config {
    scale_filter filter = scale_filter::point;
    window_mode  window = window_mode::windowed;
    bool         vsync  = false;
    // [video] scale=integer: the largest whole multiple of the frame that fits, black bars around it
    // (falls back to the aspect fit when not even 1x fits). The image rect -- and so the mouse
    // mapping, which reads the same fit_image_rect -- follows it.
    bool integer_scale = false;
};

// Where a fw x fh frame lands inside a cw x ch client area: the largest rect with the frame's aspect
// that fits, centred, in whole pixels (the rest is black bars). ONE formula, shared by the gdi and
// d3d11 presenters AND the owned device's mouse mapping (ddraw_own.cpp owned_client_to_game), so a
// click is mapped through exactly the rect the player is looking at. An empty rect for a degenerate
// input. `integer`: [video] scale=integer (backend_config::integer_scale).
inline RECT fit_image_rect(int cw, int ch, int fw, int fh, bool integer = false) {
    RECT r = {0, 0, 0, 0};
    if (cw <= 0 || ch <= 0 || fw <= 0 || fh <= 0) return r;
    int dw = cw, dh = (int)((long long)cw * fh / fw);
    if (dh > ch) {
        dh = ch;
        dw = (int)((long long)ch * fw / fh);
    }
    if (integer) {
        const int kx = cw / fw, ky = ch / fh, k = kx < ky ? kx : ky;
        if (k >= 1) {
            dw = fw * k;
            dh = fh * k;
        }
    }
    r.left   = (cw - dw) / 2;
    r.top    = (ch - dh) / 2;
    r.right  = r.left + dw;
    r.bottom = r.top + dh;
    return r;
}

class present_backend {
public:
    virtual ~present_backend() = default;

    // Short stable name for logs ("gdi", "null", "d3d11").
    virtual const char *name() const = 0;

    // Bind to the game window. Called once when the game's cooperative level is set, and again if the
    // game hands over a different HWND. Returns false if the backend cannot run on this machine (the
    // caller then falls back to gdi and logs it).
    virtual bool attach(HWND hwnd, const backend_config &cfg) = 0;

    // The game changed display mode (SetDisplayMode / a new back buffer size). Backends resize their
    // staging resources; the window itself is sized by the game (MoveWindow) or by the owner.
    virtual void on_mode(int width, int height, pixel_format format) = 0;

    // Put one frame into the window's client area, scaled to fit. Returns false on a transient failure
    // (device lost, minimized window) -- the caller treats that like a dropped frame, never an error.
    virtual bool present(const frame_view &frame) = 0;

    // Change filter / vsync / integer_scale while attached, without tearing the device down (a runtime options toggle).
    // The window mode is NOT live: restyling the window is the owner's job, then re-attach.
    virtual void set_config(const backend_config &) {}

    // Window lost or regained focus / was resized by the user.
    virtual void on_window_changed() {}

    // Release everything. After detach() the object may be deleted or attach()ed again.
    virtual void detach() = 0;
};

// Factories. Each returns nullptr only when the backend is not compiled into this build.
present_backend *make_gdi_backend();
present_backend *make_null_backend();
present_backend *make_d3d11_backend();

// Test seam (backend_d3d11.cpp): the next d3d11 present copies its filtered game image -- before the
// ImGui overlay -- into `buf` (w*h BGRA8, top row first). State: 0 pending, 1 done, -1 refused/failed.
// Used by net_selftest.exe scalefiltertest (and its --dump mode, for an outside reference).
void d3d11_request_readback(uint32_t *buf, int w, int h);
int  d3d11_readback_state();

} // namespace mh::gfx
