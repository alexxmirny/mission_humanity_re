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
#include <vector>

#include "gfx/ui_layout.h"    // RES4 stage 5: cursor_overlay
#include "gfx/overlay_list.h" // RES23-S5: overlay_list

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

// RES23-S1: the view of ONE present -- everything a presenter, the mouse maps, the clip, the cursor, the dialogs and pcapture need to agree on,
// built once per present (ddraw_own.cpp present_surface) and handed to the backend inside frame_view. `layout` is uilayout::build for
// (screen, frame, pct, integer, dialogs, crop), always (at 100 and no crop it is the single fitted rect); `multi` says it holds several
// quads (the presenter then draws layout.q[]; otherwise it draws the single fitted rect itself). `screen` is the presenter's
// real target size (the d3d11 back buffer after its resize, the gdi client rect), or the dev override [video] ui_screen.
// THE ONLY uilayout::build CALL OUTSIDE THE SELFTESTS IS build_present_view() below.
struct present_view {
    bool                 valid    = false;
    bool                 multi    = false;
    int                  screen_w = 0, screen_h = 0;
    int                  frame_w = 0, frame_h = 0;
    int                  pct     = 100; // the percentage in force for this present
    bool                 integer = false;
    int                  crop_w = 0, crop_h = 0; // RES18 composition inside the frame; 0 = none
    uilayout::dialog_set dialogs;
    uilayout::layout     layout;
};

inline void build_present_view(present_view &v, int screen_w, int screen_h, int frame_w, int frame_h, int pct, bool integer, const uilayout::dialog_set *dialogs,
                               int crop_w, int crop_h) {
    v          = present_view();
    v.valid    = true;
    v.screen_w = screen_w, v.screen_h = screen_h;
    v.frame_w = frame_w, v.frame_h = frame_h;
    v.pct     = pct;
    v.integer = integer;
    v.crop_w = crop_w, v.crop_h = crop_h;
    if (dialogs && dialogs->n > 0) v.dialogs = *dialogs;
    uilayout::build(v.layout, screen_w, screen_h, frame_w, frame_h, pct, integer, v.dialogs.n > 0 ? &v.dialogs : nullptr, crop_w, crop_h);
    v.multi = !v.layout.single;
}

// One frame as the game composed it. `pixels` is valid only for the duration of present().
struct frame_view {
    const void  *pixels;
    int          width;  // in pixels
    int          height; // in rows
    int          pitch;  // in BYTES (may exceed width * 2)
    pixel_format format;
    // RES4 stage 6: the frame rectangles of the gameplay-size dialogs up (the ESC menu, a confirmation over it, ...), bottom to top; n = 0
    // when none. At ui_scale above 100 the quad list gains a scaled dialog quad for each (uilayout::build's `dialogs`).
    uilayout::dialog_set dialogs;
    // RES23-S5: THE overlay list -- the scaled notice, the translucent layers of the dialogs whose dialogs.layered[] is set (stage 6b), the owned cursor
    // (stage 5), bottom to top, already scaled and clipped to the screen (premultiplied B8G8R8A8), drawn over the quads. Empty = the game's own cursor and
    // notices are in the frame. Valid only for the duration of present(), like `pixels`.
    overlay_list overlays;
    // RES18: the composition inside this frame when the session size is not a multiple of the 32 px tile (the game rendered
    // ceil32(W) x ceil32(H)); 0 = the frame is the whole picture. The presenters build the crop layout from it (uilayout::build).
    int crop_w = 0, crop_h = 0;
    // RES23-S1: this present's view (null = none, e.g. a selftest that only wants the single fitted rect). The presenters take their quad
    // layout from it and never call uilayout::build themselves.
    const present_view *view = nullptr;
};

struct backend_config {
    scale_filter filter = scale_filter::point;
    window_mode  window = window_mode::windowed;
    bool         vsync  = false;
    // [video] scale=integer: the largest whole multiple of the frame that fits, black bars around it
    // (falls back to the aspect fit when not even 1x fits). The image rect -- and so the mouse
    // mapping, which reads the same fit_image_rect -- follows it.
    bool integer_scale = false;
    // RES4 [video] ui_scale in percent (100 = off = the single-rect presenter above, unchanged). Above 100 the
    // frame is drawn as the quad list gfx/ui_layout.h builds (map 1:1, side panel and bottom strip scaled).
    // Live (set_config). The mouse mapping follows it from stage 4.
    int ui_scale_pct = 100;
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

// One point through an image rect `r` showing an mw x mh frame (the rect fit_image_rect returns). client -> game: a
// client point in the bars clamps to the image edge, the result is a pixel of the frame. game -> client: the CENTRE
// of the game pixel, so client_to_game(game_to_client(p)) == p. The owned device's mouse mapping
// (ddraw_own.cpp) and the RES6 cursor carry (remap_cursor_across_modes) both use these two.
inline void image_client_to_game(const RECT &r, int mw, int mh, int &x, int &y) {
    const int rw = (int)(r.right - r.left), rh = (int)(r.bottom - r.top);
    if (rw <= 0 || rh <= 0 || mw <= 0 || mh <= 0) return;
    const int cx = x < r.left ? (int)r.left : x >= r.right ? (int)r.right - 1
                                                           : x;
    const int cy = y < r.top ? (int)r.top : y >= r.bottom ? (int)r.bottom - 1
                                                          : y;
    x            = (int)((long long)(cx - r.left) * mw / rw);
    y            = (int)((long long)(cy - r.top) * mh / rh);
}

inline void image_game_to_client(const RECT &r, int mw, int mh, int &x, int &y) {
    const int rw = (int)(r.right - r.left), rh = (int)(r.bottom - r.top);
    if (rw <= 0 || rh <= 0 || mw <= 0 || mh <= 0) return;
    const int gx = x < 0 ? 0 : x >= mw ? mw - 1
                                       : x;
    const int gy = y < 0 ? 0 : y >= mh ? mh - 1
                                       : y;
    x            = (int)r.left + (int)(((long long)gx * 2 + 1) * rw / (2LL * mw));
    y            = (int)r.top + (int)(((long long)gy * 2 + 1) * rh / (2LL * mh));
}

// RES6: a game cursor (x,y) of an old_w x old_h frame shown in old_r, carried across a mode change to the new_w x
// new_h frame shown in new_r (both rects in the same client space) so it stays on the same screen point: old game
// pixel -> its client point -> the new frame's pixel there, clamped to the new image (a point that falls in the new
// mode's bars lands on the nearest image edge).
inline POINT remap_cursor_across_modes(const RECT &old_r, int old_w, int old_h, const RECT &new_r, int new_w, int new_h,
                                       int x, int y) {
    image_game_to_client(old_r, old_w, old_h, x, y);
    image_client_to_game(new_r, new_w, new_h, x, y);
    POINT p = {x, y};
    return p;
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

    // RES23-S1: the size of the surface this presenter draws into RIGHT NOW (gdi: the client rect; d3d11: the swap chain's back buffer, which it
    // resizes to the client rect first). Called by present_surface just before it builds the present_view. False = nothing to draw into
    // (minimised, no device) or a presenter with no target (null).
    virtual bool target_size(int &w, int &h) {
        (void)w, (void)h;
        return false;
    }

    // Window lost or regained focus / was resized by the user.
    virtual void on_window_changed() {}

    // RES23-S0 `bcapture`: what the presenter really put on screen (cursor, dialog layers and any overlay included).
    // arm_output_capture() is called just BEFORE present(); take_output_capture() just AFTER it and fills `bgra`
    // (w*h, top row first, alpha forced to 255). False = this backend shows nothing (null) or the read failed.
    virtual void arm_output_capture() {}
    virtual bool take_output_capture(std::vector<uint32_t> &bgra, int &w, int &h) {
        (void)bgra, (void)w, (void)h;
        return false;
    }

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
