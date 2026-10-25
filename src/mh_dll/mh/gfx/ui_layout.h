//
// gfx/ui_layout.h -- mods:RES4 stages 1-2: the UI-scale layout, as pure arithmetic.
//
// THE DESIGN (user, 2026-10-09 / 2026-10-19). `[video] ui_scale` makes the HUD PHYSICALLY bigger on any
// resolution (150% = the HUD looks 1.5x bigger) while the map stays 1:1 with the screen. The game renders
// one frame of W x H pixels; the presenter shows it as SEVERAL QUADS, each a source rectangle of that frame
// drawn into a destination rectangle of the screen:
//
//   map    the world, source (0,0)-(Mw,Mh) of the frame, drawn 1:1 at the screen's top left;
//   strip  the 32-row bottom strip (frame rows H-32..H-1), scaled by s, under the map;
//   panel  the 160-column side panel (frame columns W-160..W-1), scaled by s, down the right edge.
//
// Later stages append dialog and cursor quads (kind::dialog / kind::cursor); the list is ordered back to
// front and a point is hit-tested front to back.
//
// SCREEN. Dv = the presenter's client area. s = ui_scale in percent (100 = off). The screen-side geometry:
//   pw = round(160 s), ds = round(32 s)             the panel's width and the strip's height on screen
//   Mw = Dv_w - pw, Mh = Dv_h - ds                  the visible map
//   s is clamped to  min(Dv_h / 480, Dv_w / 640)    so a 640x480 game still fits (logged by the caller)
// FRAME (what stage 3 will make the game render; `ideal_render`):
//   W = ceil32(Mw) + 160, H = ceil32(Mh) + 32       up to 31 map pixels stay hidden under the quads
//   W <= 2048 and H <= 1600 (the RES3 render-size caps); a screen larger than that shows the composition
//   centred with black bars, never a bigger frame.
//
// RES21 (composition smaller than the room; RES17's map enlargement is gone). A composition SMALLER than the room the screen leaves on both axes (an explicit resolution=WxH, or a frame the render caps cut) is
// not shown as a small 1:1 box and its map is not shrunk beside the scaled panel: the WHOLE frame is fitted to the screen like the 100 layout (one uniform scale, the same map zoom at every
// percentage, no bars when the aspects match) and the side panel and the bottom strip are drawn OVER it at the Interface size, anchored to the composition's right / bottom edge
// (layout::overlay; the last two quads before any dialog). A percentage that does not exceed the fit zoom is the 100 layout. `make_overlay`.
//
// RES18 (the CROP). A session size need not be a multiple of 32: the game renders ceil32(W) x ceil32(H) and the presenter shows only W x H of it.
// `build`'s crop_w x crop_h is that size (0 = the frame is the composition). The composition is the frame with its map region cut to
// (crop_w - 160) x (crop_h - 32): the map quad is the top-left of the frame's map region, the strip quad is the frame's LAST 32 rows (so the strip
// sits on the composition's last row) and the panel quad is the frame's LAST 160 columns, its first crop_h rows. At ui_scale 100 this is the
// uniform 1:1 (or fitted) composition, layout::crop; above 100 the visible map is also limited to the crop (the room still applies).
//
// s = 100 IS THE OLD PRESENTER: one quad, the whole frame, fit_image_rect (present_backend.h) -- so the
// single-rect path every backend has today is the 1-quad case, byte for byte, and its mouse map stays the
// legacy formula (unless a crop makes the frame bigger than its composition). The multi-quad mapping below uses the PIXEL-CENTRE rule: screen pixel x shows source
// pixel floor((2(x - dst.x0) + 1) * sw / (2 dw)), which is exactly what a GPU point sampler picks.
//
// MOUSE (stage 4 wires it; the arithmetic is here and tested). screen_to_frame: a point outside the
// composition clamps to its edge; the EDGE RULE then maps the composition's last column / row to the
// frame's W-1 / H-1 (the game scrolls the map when its cursor reaches the frame edge, and a scaled panel
// does not otherwise show its hidden rows' edge). frame_to_screen: the middle of the source pixel's
// screen span, so screen_to_frame(frame_to_screen(p)) == p for every visible pixel. (One exception, by the edge
// rule: the side panel's LAST source row answers H-1 when its only screen row is the composition's last, so a
// click there reads as the frame's bottom edge.)
//
// Pure: no Windows, no game memory. `net_selftest.exe uilayouttest` drives all of it.
//
#pragma once
#include <stdint.h>

namespace mh::gfx::uilayout {

constexpr int PANEL_W     = 160; // the side panel's width in frame pixels
constexpr int STRIP_H     = 32;  // the bottom strip's height in frame pixels
constexpr int ALIGN       = 32;  // render sizes are multiples of this (the tile)
constexpr int MAX_W       = 2048;
constexpr int MAX_H       = 1600;
constexpr int MIN_PCT     = 100;
constexpr int MAX_PCT     = 400;
constexpr int MAX_QUADS   = 8;
constexpr int MAX_DIALOGS = 2; // dialog quads: a modal list and the dialog opened over it
// The side panel's main list (the available / owned buildings): its first row's top y and its row height, in frame pixels. The scale snap
// below fits whole rows of THIS list (the others have other pitches but the same bottom edge).
constexpr int LIST_TOP = 248;
constexpr int LIST_ROW = 52;

struct irect {
    int  x0 = 0, y0 = 0, x1 = 0, y1 = 0; // half-open
    int  w() const { return x1 - x0; }
    int  h() const { return y1 - y0; }
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

enum class quad_kind : uint8_t { frame,
                                 map,
                                 strip,
                                 panel,
                                 dialog,
                                 cursor };

struct quad {
    quad_kind kind = quad_kind::frame;
    irect     src; // pixels of the game frame
    irect     dst; // pixels of the screen (the presenter's client area)
};

struct cursor_overlay {
    const uint32_t *px = nullptr; // premultiplied B8G8R8A8, top row first, w * h, tightly packed
    int             w = 0, h = 0; // the part of the scaled cursor that is on screen
    int             x = 0, y = 0; // its top-left on screen
};

struct layout {
    int   frame_w = 0, frame_h = 0;
    int   screen_w = 0, screen_h = 0;
    int   req_pct = 100;          // what was asked for
    int   pct     = 100;          // what is in force (clamped; 100 = the single-quad old presenter)
    bool  clamped = false;        // pct < req_pct because the screen is too small for it
    bool  snapped = false;        // pct was lowered (by at most 5%) so the main panel list fits one more whole row (snap_scale_pct)
    bool  single  = true;         // one quad = the whole frame fitted into the screen (pct == 100 and no crop, or no room)
    bool  overlay = false;        // RES21: the whole frame fitted like 100 and the panel / strip quads (the last two before any dialog) drawn OVER it at pct
    bool  crop    = false;        // RES18: pct == 100 with a crop -- map / strip / panel quads at ONE uniform scale (map_w / map_src_w); !single
    int   crop_w = 0, crop_h = 0; // the composition inside the frame, frame pixels (== the frame when there is no crop)
    int   nquads = 0;
    quad  q[MAX_QUADS];
    irect used;                         // the screen rectangle the quads cover (the rest is black bars)
    int   map_w = 0, map_h = 0;         // visible map, screen pixels (multi-quad only)
    int   map_src_w = 0, map_src_h = 0; // the map quad's source size, frame pixels; map_w / map_src_w is the map factor k (1 = 1:1; the crop / overlay fit otherwise)
    int   panel_w = 0, strip_h = 0;     // the scaled panel width / strip height, screen pixels
    int   panel_rows    = 0;            // frame rows of the side panel that show (Hp)
    bool  integer_scale = false;        // single quad: [video] scale=integer
    bool  has_dialog    = false;        // quad_kind::dialog quads are the last ndialogs of q[], bottom to top (stage 6)
    int   ndialogs      = 0;
    irect dialog_src[MAX_DIALOGS]; // each one's frame rectangle (the dialog's own 1x pixels)
    irect dialog_dst[MAX_DIALOGS]; // where it is drawn on screen
    // Stage 6b: the dialog's translucent layer was recovered (dialog_set::layered): its quad is NOT copied from the frame (the frame holds the
    // true background there); the presenter draws the layer as an alpha overlay instead (place_dialog_layer).
    bool dialog_layered[MAX_DIALOGS] = {};
    // Is quad `qi` a dialog quad whose pixels come from a layer overlay rather than the frame?
    bool quad_is_layered(int qi) const {
        const int j = qi - (nquads - ndialogs);
        return ndialogs > 0 && j >= 0 && j < ndialogs && dialog_layered[j];
    }
};

// The gameplay-size dialogs up in the frame, bottom to top (e.g. the ESC menu, then the confirmation over it): the `dialogs` of build().
struct dialog_set {
    irect r[MAX_DIALOGS];
    bool  layered[MAX_DIALOGS] = {}; // stage 6b: a premultiplied layer exists for it (the frame does not hold the dialog)
    int   n                    = 0;
};

// The largest s this screen allows: floor(100 * min(Dv_h / 480, Dv_w / 640)), never below 100.
int max_scale_pct(int screen_w, int screen_h);

// Clamp a requested percentage into [100, max_scale_pct]; *clamped is set when the screen forced it down.
int clamp_scale_pct(int req_pct, int screen_w, int screen_h, bool *clamped = nullptr);

// The frame rows of the side panel that show WHOLE on screen: floor(composition height / s), 0 for a single-quad layout. The panel lists
// size themselves to this (a row cut by the screen's bottom edge is never drawn); panel_rows (the quad's source height) is the ceil.
// A crop layout (RES18) shows crop_h rows, all of them whole.
int whole_panel_rows(const layout &l);

// The whole rows of the panel's main list that fit at percentage `pct` on a screen_h-tall screen (ideal frame): (Hp - LIST_TOP) / LIST_ROW,
// Hp = floor(composition height / s); -1 when not even one fits.
int main_list_rows(int screen_h, int pct);

// THE SNAP (user, 2026-10-19). After the clamp, when the gap left under the main list's last whole row is at least half a row, lower s to
// the largest percentage s' in [0.95 s, s] that fits one more whole row; otherwise keep s. 100 is never touched. Pure in (screen, pct), so the
// frame the game renders, the quads, Hp and the inverse map all take the same s'. *snapped is set when s' < s.
int snap_scale_pct(int screen_w, int screen_h, int pct, bool *snapped = nullptr);

// clamp_scale_pct, then snap_scale_pct: the percentage that is in force for a request.
int effective_scale_pct(int req_pct, int screen_w, int screen_h, bool *clamped = nullptr, bool *snapped = nullptr);

// The panel's / strip's size on screen for a percentage: round(160 s), round(32 s).
int scaled_panel_w(int pct);
int scaled_strip_h(int pct);

// [video] ui_scale=auto: the Windows display scale of the game's monitor in percent (96 dpi = 100) -> the nearest of 100/125/150/175/200
// (a tie goes up: 112 -> 125; anything above 200 is 200). The clamp to the screen is clamp_scale_pct's, as for an explicit value.
int auto_scale_pct(int display_scale_pct);

// What the game should render for a screen at a scale (stage 3 reads it; stage 2 only logs and tests it).
struct render_size {
    int  pct = 100;            // clamped
    int  w = 0, h = 0;         // the frame
    int  map_w = 0, map_h = 0; // visible map on screen (after the render-size caps)
    int  panel_w = 0, strip_h = 0;
    int  panel_rows = 0;     // Hp: frame rows of the side panel the screen shows, = min(h, ceil(composition height / s))
    bool capped     = false; // the caps cut the map: the composition is centred with bars
    bool clamped    = false;
    bool snapped    = false; // pct was snapped down to fit one more whole row (snap_scale_pct)
};
render_size ideal_render(int screen_w, int screen_h, int req_pct);

// Build the quad list for a frame of frame_w x frame_h shown on a screen_w x screen_h client area at req_pct.
// A frame that is not the ideal size is fine (stage 2 runs the stock frame): the visible map is simply
// min(screen room, frame room) and the composition is centred. pct == 100 (or a degenerate size) gives the
// single fitted quad; `integer` is the single quad's [video] scale=integer.
//
// `dialogs` (stage 6, null = none): the frame rectangles of the gameplay-size dialogs (the ESC menu, the options screen, a confirmation
// over it, ...), bottom to top. Each is appended as a quad above the HUD ones, scaled by s about the visible map's centre, never
// smaller than the 1x copy it hides (see place_dialog). A dialog that does not sit wholly in the visible map, or cannot be scaled to at
// least 101% on this screen, adds no quad: it stays 1x in the map quad, and the mouse maps through the plain map quad there.
//
// `crop_w` x `crop_h` (RES18, 0 = none): the composition inside the frame when the frame was rounded up to the tile grid. Honoured only when
// it is smaller than the frame on at least one axis and larger than the panel / strip.
void build(layout &out, int screen_w, int screen_h, int frame_w, int frame_h, int req_pct, bool integer = false,
           const dialog_set *dialogs = nullptr, int crop_w = 0, int crop_h = 0);

// ---- mods:RES4 stage 6: dialog quads ---------------------------------------------------------------------------------------------------
// The dialog's destination on screen for a layout (HUD quads only) and its frame rectangle `src`. False = no dialog quad.
// Size: round(w p/100) x round(h p/100), p = min(pct, what fits the composition) -- must exceed 100. Position: centred on the
// visible MAP (the game centres its dialogs in the map view, which the panel does not belong to), then moved the least that (1) covers the 1x copy in the map quad completely (no half-scaled ghost) and (2) stays
// inside the composition (this one wins when both cannot hold).
bool place_dialog(const layout &l, const irect &src, irect &dst);

// The single quad's rectangle: the same formula as present_backend.h fit_image_rect (kept pure here; the selftest
// asserts the two agree over a grid).
irect fit_rect(int cw, int ch, int fw, int fh, bool integer);

// A screen point -> the frame pixel under it. x,y are in/out. A point outside the composition clamps to its edge.
// Returns false only for a degenerate layout (nothing changes). Single layout: the legacy fit-rect formula.
bool screen_to_frame(const layout &l, int &x, int &y);

// A frame pixel -> the middle of its span on screen. Returns true when the pixel is visible (inside some quad's
// source); a hidden pixel (a map pixel under the panel, panel rows below Hp) still gets the nearest visible
// position, so a cursor parked on it lands somewhere sensible, and false is returned.
bool frame_to_screen(const layout &l, int &x, int &y);

// The topmost quad whose destination holds the screen point, or -1.
int quad_at(const layout &l, int x, int y);

// The presented image on the CPU: every quad nearest-sampled by the centre rule (what a GPU point sampler does,
// whatever [video] filter is set), bars black. `out` is screen_w x screen_h, 3 bytes per pixel B,G,R, top row
// first, `out_stride` bytes per row. `frame` is RGB565, `frame_pitch` bytes per row.
void compose_bgr24(const layout &l, const uint16_t *frame, int frame_pitch, uint8_t *out, int out_stride);

// ---- mods:RES4 stage 6b: translucent dialog layers ---------------------------------------------------------------------------------------
// A layered dialog (dialog_set::layered) is NOT in the frame: the game's list draw was run alone into scratch (over black, over white
// and over mid grey), and the premultiplied B8G8R8A8 layer (w x h = the dialog's frame rect, 1x) is shown by the presenter as an alpha
// overlay scaled to layout.dialog_dst[i], nearest by the centre rule, clipped to the screen, above the HUD quads and below the cursor.
// `px` is w*h premultiplied pixels. False when nothing of it is on screen or `buf` is too small.
bool place_dialog_layer(const layout &l, int i, const uint32_t *px, uint32_t *buf, int buf_px, cursor_overlay &out);

// One layer pixel from the scratch renderings of the same list: RGB565 over 0x0000, 0xFFFF and 0x8410 (mid grey). Exact (1 LSB per
// channel at 8 bits) for the blend model out = C + s*dst, s in [0,1], including the clamped op 0x60; the grey pass is used per pixel
// only when black and white disagree about the model (an unclamped op 0x40 overflowing over white). *used_grey is set then.
uint32_t layer_pixel_from_passes(uint16_t on_black, uint16_t on_white, uint16_t on_grey, bool *used_grey = nullptr);

// ---- mods:RES4 stage 5: the owned cursor ------------------------------------------------------------------------------------------------
// The game draws its cursor sprite INTO the frame, so a scaled panel would show it at the frame's size and the cursor would be
// the only thing not at ui_scale. In a scaled layout the DLL draws the stock cursor routine into a CURSOR_SRC x CURSOR_SRC scratch
// instead (hotspot pixel at CURSOR_HOT, twice: on black and on white, from which per-pixel coverage is recovered), keeps the result
// as a premultiplied B8G8R8A8 image, and the presenter draws it ON TOP of the quads, scaled by s (nearest) with its hotspot on the
// screen point of the game's cursor pixel (frame_to_screen: a click there reads back as that very pixel).
constexpr int CURSOR_SRC = 160; // the scratch's side, frame pixels
constexpr int CURSOR_HOT = 80;  // the scratch pixel the game's cursor position (the hotspot) is drawn at
constexpr int CURSOR_MAX = 640; // the largest scaled side (MAX_PCT / 100 * CURSOR_SRC)


// Coverage + premultiplied colour from the cursor routine's two renderings of the same sprite (RGB565 over 0x0000 and over 0xFFFF).
// Per pixel: a = 255 - mean(white - black) over the channels; colour = the black rendering (= a * sprite colour).
uint32_t cursor_pixel_from_passes(uint16_t on_black, uint16_t on_white);

// Scale a CURSOR_SRC^2 premultiplied cursor (hotspot pixel CURSOR_HOT) by l.pct, nearest, so its hotspot lands on the screen point of
// frame pixel (fx, fy) (frame_to_screen). Only the part inside the screen is produced, into `buf` (buf_px pixels). False when the layout is
// the single-quad one, nothing is visible, or `buf` is too small.
bool place_cursor(const layout &l, const uint32_t *src, int fx, int fy, uint32_t *buf, int buf_px, cursor_overlay &out);

// ---- RES20 / RES23-S5: the scaled floating messages (notices) ---------------------------------------------------------------------------
// The game prints its notices ("order refused", "????") into the frame's map area at the top left, in frame pixels, so at ui_scale they stayed 1x
// while the panel grew. The DLL renders the stock routine into a scratch over black and over white (cursor_pixel_from_passes), keeps the
// premultiplied layer `src` (sw x sh, its (0,0) = frame pixel (fx,fy)) and the presenter shows it ON the map quad scaled by the percentage,
// anchored at the map quad's image of (fx,fy), nearest, clipped to the VISIBLE map (never over the panel or the strip) and the composition.
// False when there is nothing to show (the single-quad / crop layouts, nothing visible, or `buf` too small).
bool place_scaled_layer(const layout &l, const uint32_t *src, int sw, int sh, int fx, int fy, uint32_t *buf, int buf_px, cursor_overlay &out);

// Blend an overlay into a B,G,R image of screen_w x screen_h (the pcapture compositor).
void blend_cursor_bgr24(const cursor_overlay &c, uint8_t *out, int out_stride, int screen_w, int screen_h);

} // namespace mh::gfx::uilayout
