//
// overlay_list.h -- RES23-S5: THE overlay list of a present, the stamps that keep it fresh, and THE scratch-render helper.
//
// Three things the game draws INTO its frame are shown by mh.dll as presenter overlays instead (at ui_scale above 100): the floating
// notices, the translucent dialog layers and the cursor. Each is captured on the game thread while the frame is drawn (the stock routine is
// run into a scratch, see scratch_scope) and PLACED at present time (ddraw_own.cpp overlay_collect). Two rules:
//
//   * every captured source carries an overlay_stamp -- the number of the present it was captured for and the HUD epoch it was captured
//     under. The presenter takes only sources stamped for the present being shown (and the geometry in force) and DROPS the rest: a notice
//     captured in the last strategic frame never floats over a mode-3 modal, a layer captured under an old geometry never meets a new one;
//   * the result is ONE overlay_list (bottom to top: notice, dialog layers, cursor) that gdi, d3d11 and pcapture / bcapture all consume.
//
// Presentation only: nothing here touches simulation state.
//
#pragma once

#include <stdint.h>

#include "gfx/ui_layout.h"
#include "addr/mh_addrs.gen.h"

namespace mh::gfx {

// ---- the list ---------------------------------------------------------------------------------------------------------------------------
// Slots are stable per kind (the d3d11 presenter keeps one texture per slot).
namespace overlay_slot {
constexpr int notice  = 0;
constexpr int dialog0 = 1; // dialog layers: dialog0 .. dialog0 + MAX_DIALOGS - 1
constexpr int cursor  = dialog0 + uilayout::MAX_DIALOGS;
constexpr int count   = cursor + 1;
} // namespace overlay_slot

struct overlay_item {
    uilayout::cursor_overlay ov;        // screen pixels, premultiplied B8G8R8A8 (gfx/ui_layout.h)
    int                      slot = -1; // overlay_slot::*
};

// Bottom to top. Valid only for the duration of the present() it is handed to (the pixels live in static buffers).
struct overlay_list {
    overlay_item item[overlay_slot::count];
    int          n = 0;
    void         add(const uilayout::cursor_overlay &ov, int slot) {
        if (n >= overlay_slot::count || !ov.px || ov.w <= 0 || ov.h <= 0) return;
        item[n].ov   = ov;
        item[n].slot = slot;
        ++n;
    }
    const overlay_item *find(int slot) const {
        for (int i = 0; i < n; ++i)
            if (item[i].slot == slot) return &item[i];
        return nullptr;
    }
};

// ---- the stamp ----------------------------------------------------------------------------------------------------------------------------
struct overlay_window {
    unsigned seq_lo = 0, seq_hi = 0; // the present numbers an entry may be stamped for
    unsigned epoch = 0;              // the HUD epoch in force
};

struct overlay_stamp {
    unsigned seq   = 0; // the present this capture is for (0 = nothing captured)
    unsigned epoch = 0;
    void     set(unsigned for_seq, unsigned at_epoch) { seq = for_seq, epoch = at_epoch; }
    void     clear() { seq = 0; }
    bool     fresh(const overlay_window &w) const { return seq != 0 && seq >= w.seq_lo && seq <= w.seq_hi && epoch == w.epoch; }
};

// ---- the scratch-render helper ----------------------------------------------------------------------------------------------------------
//
// "Run the stock routine into a scratch with the frame globals swapped": the save / restore set lives HERE, once. A target is REGISTERED by
// a scratch_target that names the routine and records why running it more than once per frame (and into a buffer that is not the frame) is
// harmless -- REPORT.md 3.5: any new target must be checked for idempotence first. scratch_scope refuses a target with no such note.
struct scratch_target {
    const char *name;
    uintptr_t   addr;       // the stock routine's entry
    const char *idempotent; // the evidence it only reads state / stamps what is idempotent (never empty)
};

namespace scratch_addr {
constexpr uintptr_t tile_drawn_map = 0x00824ffcu; // _G_LLM_TILE_DRAWN_MAP_PTR
constexpr uintptr_t ui_clip_top    = 0x005d8b50u; // _G_LLM_GFX_UI_CLIP_TOP
constexpr uintptr_t ui_clip_bottom = 0x005d8b54u; // _G_LLM_GFX_UI_CLIP_BOTTOM
constexpr uintptr_t ui_surf_ptr    = 0x005d7190u; // the UI surface the routines leave on the frame they drew into
constexpr uintptr_t ui_surf_pitch  = 0x005d7194u;
constexpr uintptr_t cursor_menu_x  = 0x00e58cccu; // _G_LLM_CURSOR_MENU_X
constexpr uintptr_t cursor_menu_y  = 0x00e58cc8u; // _G_LLM_CURSOR_MENU_Y
} // namespace scratch_addr

// What the pass swaps besides the frame (surface + pitch, always).
struct scratch_cfg {
    int      win_w = 0, win_h = 0; // > 0: WindowWidth / WindowHeight during the passes (the routine clips to them)
    bool     game_win = false;     // also G_WIN_W / G_WIN_H = win_w / win_h
    bool     cursor   = false;     // the cursor position (the hotspot) during the passes
    int      cur_x = 0, cur_y = 0;
    uint8_t *vis = nullptr, *drawn = nullptr; // dummy dirty-tile maps (both or neither): no real tile is stamped
    bool     clip = false;                    // the UI clip rows 0 .. win_h
};

class scratch_scope {
public:
    scratch_scope(const scratch_target &t, const scratch_cfg &c) : cfg_(c), ok_(t.addr != 0 && t.idempotent && t.idempotent[0]) {
        if (!ok_) return;
        fb_    = ref(mh::addr::_G_LLM_FRAMEBUFFER);
        pitch_ = ref(mh::addr::_G_LLM_FB_PITCH);
        if (c.win_w > 0) {
            ww_ = ref(mh::addr::WindowWidth), wh_ = ref(mh::addr::WindowHeight);
            ref(mh::addr::WindowWidth)  = c.win_w;
            ref(mh::addr::WindowHeight) = c.win_h;
        }
        if (c.game_win) {
            gw_ = ref(mh::addr::G_WIN_W), gh_ = ref(mh::addr::G_WIN_H);
            ref(mh::addr::G_WIN_W) = c.win_w;
            ref(mh::addr::G_WIN_H) = c.win_h;
        }
        if (c.cursor) {
            mx_ = ref(scratch_addr::cursor_menu_x), my_ = ref(scratch_addr::cursor_menu_y);
            ref(scratch_addr::cursor_menu_x) = c.cur_x;
            ref(scratch_addr::cursor_menu_y) = c.cur_y;
        }
        if (c.vis) {
            vis_ = ref(mh::addr::_G_LLM_TILE_VIS_MAP_PTR), drawn_ = ref(scratch_addr::tile_drawn_map);
            ref(mh::addr::_G_LLM_TILE_VIS_MAP_PTR) = (int)(intptr_t)c.vis;
            ref(scratch_addr::tile_drawn_map)      = (int)(intptr_t)c.drawn;
        }
        if (c.clip) {
            ctop_ = ref(scratch_addr::ui_clip_top), cbot_ = ref(scratch_addr::ui_clip_bottom);
            ref(scratch_addr::ui_clip_top)    = 0;
            ref(scratch_addr::ui_clip_bottom) = c.win_h;
        }
    }
    ~scratch_scope() {
        if (!ok_) return;
        ref(mh::addr::_G_LLM_FRAMEBUFFER) = fb_;
        ref(mh::addr::_G_LLM_FB_PITCH)    = pitch_;
        if (cfg_.win_w > 0) ref(mh::addr::WindowWidth) = ww_, ref(mh::addr::WindowHeight) = wh_;
        if (cfg_.game_win) ref(mh::addr::G_WIN_W) = gw_, ref(mh::addr::G_WIN_H) = gh_;
        if (cfg_.cursor) ref(scratch_addr::cursor_menu_x) = mx_, ref(scratch_addr::cursor_menu_y) = my_;
        if (cfg_.vis) ref(mh::addr::_G_LLM_TILE_VIS_MAP_PTR) = vis_, ref(scratch_addr::tile_drawn_map) = drawn_;
        if (cfg_.clip) ref(scratch_addr::ui_clip_top) = ctop_, ref(scratch_addr::ui_clip_bottom) = cbot_;
        // The stock routines leave the UI surface on the frame they drew into; the stock path leaves it on the real frame.
        ref(scratch_addr::ui_surf_ptr)   = fb_;
        ref(scratch_addr::ui_surf_pitch) = pitch_;
    }
    scratch_scope(const scratch_scope &)            = delete;
    scratch_scope &operator=(const scratch_scope &) = delete;

    bool ok() const { return ok_; }
    // Point the stock routine at the next pass buffer (stride in pixels).
    void retarget(uint16_t *pix, int stride_px) {
        ref(mh::addr::_G_LLM_FRAMEBUFFER) = (int)(intptr_t)pix;
        ref(mh::addr::_G_LLM_FB_PITCH)    = stride_px * 2;
    }
    // The real frame's surface / pitch as they were when the scope opened (the dialog check reads them).
    int saved_fb() const { return fb_; }
    int saved_pitch() const { return pitch_; }

private:
    static int &ref(uintptr_t a) { return *reinterpret_cast<int *>(a); }
    scratch_cfg cfg_;
    bool        ok_;
    int         fb_ = 0, pitch_ = 0, ww_ = 0, wh_ = 0, gw_ = 0, gh_ = 0, mx_ = 0, my_ = 0, vis_ = 0, drawn_ = 0, ctop_ = 0, cbot_ = 0;
};

// Fill the w x h rectangle at (x0, y0) of an RGB565 scratch of `stride_px` pixels per row.
inline void scratch_fill_rect(uint16_t *buf, int stride_px, int x0, int y0, int w, int h, uint16_t v) {
    for (int y = 0; y < h; ++y) {
        uint16_t *row = buf + (size_t)(y0 + y) * stride_px + x0;
        for (int x = 0; x < w; ++x) row[x] = v;
    }
}

// A sprite drawn by the stock routine into a w x h scratch, twice (over black, over white), premultiplied into `out` (w*h B8G8R8A8) through
// uilayout::cursor_pixel_from_passes. `stock` draws it into whatever the scope is pointed at. Returns the number of non-transparent
// pixels (0 = nothing was drawn, or the target was refused); bbox (optional, x0 y0 x1 y1 inclusive) gets their box.
template <class Stock>
inline long scratch_render_bw(const scratch_target &t, const scratch_cfg &c, int w, int h, uint16_t *black, uint16_t *white, uint32_t *out, Stock stock, int *bbox = nullptr) {
    const size_t n = (size_t)w * h;
    {
        scratch_scope scope(t, c);
        if (!scope.ok()) return 0;
        uint16_t *passes[2] = {black, white};
        for (int p = 0; p < 2; ++p) {
            const uint16_t fill = p == 0 ? 0x0000 : 0xFFFF;
            for (size_t i = 0; i < n; ++i) passes[p][i] = fill;
            scope.retarget(passes[p], w);
            stock();
        }
    }
    int  bx0 = w, by0 = h, bx1 = -1, by1 = -1;
    long any = 0;
    for (size_t i = 0; i < n; ++i) {
        out[i] = uilayout::cursor_pixel_from_passes(black[i], white[i]);
        if (out[i] >> 24) {
            ++any;
            const int x = (int)(i % w), y = (int)(i / w);
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    if (bbox) bbox[0] = bx0, bbox[1] = by0, bbox[2] = bx1, bbox[3] = by1;
    return any;
}

} // namespace mh::gfx
