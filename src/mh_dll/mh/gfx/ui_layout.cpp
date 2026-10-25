// gfx/ui_layout.cpp -- mods:RES4 stages 1-2: see ui_layout.h.
#include "gfx/ui_layout.h"

#include <string.h>

namespace mh::gfx::uilayout {
namespace {

int ceil32(int v) { return (v + ALIGN - 1) & ~(ALIGN - 1); }

int64_t ceil_div(int64_t a, int64_t b) { // b > 0, a may be negative
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

int round_pct(int base, int pct) { return (int)(((int64_t)base * pct + 50) / 100); }

// Screen pixel (relative to dst, 0..dw-1) -> source pixel (relative to src, 0..sw-1): the pixel-centre rule.
int centre_src(int rel, int dw, int sw) { return (int)(((int64_t)(2 * rel + 1) * sw) / (2LL * dw)); }

// The middle of the span of screen pixels (relative to dst) that centre_src maps to `r`.
int span_mid(int r, int dw, int sw) {
    int64_t lo = ceil_div((int64_t)2 * r * dw - sw, 2LL * sw);
    int64_t hi = ceil_div((int64_t)2 * (r + 1) * dw - sw, 2LL * sw) - 1;
    if (lo < 0) lo = 0;
    if (hi > dw - 1) hi = dw - 1;
    if (hi < lo) hi = lo;
    return (int)((lo + hi) / 2);
}

void add_quad(layout &l, quad_kind k, irect src, irect dst) {
    if (l.nquads >= MAX_QUADS) return;
    quad &q = l.q[l.nquads++];
    q.kind  = k;
    q.src   = src;
    q.dst   = dst;
}

void make_single(layout &l, bool integer) {
    l.single        = true;
    l.pct           = 100;
    l.integer_scale = integer;
    l.nquads        = 0;
    irect src{0, 0, l.frame_w, l.frame_h};
    irect dst = fit_rect(l.screen_w, l.screen_h, l.frame_w, l.frame_h, integer);
    add_quad(l, quad_kind::frame, src, dst);
    l.used = dst;
}

// RES18: ui_scale 100 with a crop. The composition (crop_w x crop_h frame pixels) is fitted into the screen like the single quad (one uniform
// scale, centred, whole multiples when `integer`), cut into the three quads. Every boundary is computed once, so the quads tile exactly.
void make_crop(layout &l, bool integer) {
    const int   lw = l.crop_w, lh = l.crop_h;
    const irect D = fit_rect(l.screen_w, l.screen_h, lw, lh, integer);
    if (D.w() <= 0 || D.h() <= 0) {
        make_single(l, integer);
        return;
    }
    auto      X    = [&](int c) { return D.x0 + (int)(((int64_t)c * D.w() + lw / 2) / lw); };
    auto      Y    = [&](int c) { return D.y0 + (int)(((int64_t)c * D.h() + lh / 2) / lh); };
    const int mapw = lw - PANEL_W, maph = lh - STRIP_H;
    l.single        = false;
    l.crop          = true;
    l.pct           = 100;
    l.integer_scale = integer;
    l.nquads        = 0;
    const irect md{X(0), Y(0), X(mapw), Y(maph)};
    add_quad(l, quad_kind::map, irect{0, 0, mapw, maph}, md);
    add_quad(l, quad_kind::strip, irect{0, l.frame_h - STRIP_H, mapw, l.frame_h}, irect{X(0), Y(maph), X(mapw), Y(lh)});
    add_quad(l, quad_kind::panel, irect{l.frame_w - PANEL_W, 0, l.frame_w, lh}, irect{X(mapw), Y(0), X(lw), Y(lh)});
    l.map_src_w  = mapw;
    l.map_src_h  = maph;
    l.map_w      = md.w();
    l.map_h      = md.h();
    l.panel_w    = X(lw) - X(mapw);
    l.strip_h    = Y(lh) - Y(maph);
    l.panel_rows = lh;
    l.used       = D;
}

// RES21: an explicit frame smaller than the room the screen leaves, at Interface size above 100. The WHOLE frame (or its crop composition) is shown exactly as
// at 100 -- one uniform fit, the same map zoom at every percentage -- and the side panel and the bottom strip are drawn OVER it at the Interface size, anchored to
// the composition's right / bottom edge, hiding the map behind them. No bars at any percentage (the 100 layout has none when the aspects match). The overlays
// are the last quads (strip, then panel on top), so the inverse map sends a point over them to the panel / strip and any other point to the map under.
void make_overlay(layout &l, int pct, bool integer) {
    make_crop(l, integer); // the base: map / strip / panel at the uniform fit z (the crop, or the whole frame when there is none)
    const int base_pw = l.panel_w, base_ds = l.strip_h;
    int       pw = scaled_panel_w(pct), ds = scaled_strip_h(pct);
    if (pw <= base_pw && ds <= base_ds) { // the percentage does not exceed the fit: the base IS the layout (the 100 layout)
        l.pct = MIN_PCT;
        return;
    }
    if (pw < base_pw) pw = base_pw;
    if (ds < base_ds) ds = base_ds;
    const irect &D = l.used;
    if (pw >= D.w() || ds >= D.h()) { // no room for an overlay: the base alone
        l.pct = MIN_PCT;
        return;
    }
    const int lw = l.crop_w, lh = l.crop_h;
    int64_t   rows = ceil_div((int64_t)D.h() * 100, pct);
    if (rows > lh) rows = lh;
    int64_t cols = ceil_div((int64_t)(D.w() - pw) * 100, pct);
    if (cols > lw - PANEL_W) cols = lw - PANEL_W;
    l.pct  = pct;
    l.crop = false;
    add_quad(l, quad_kind::strip, irect{0, l.frame_h - STRIP_H, (int)cols, l.frame_h}, irect{D.x0, D.y1 - ds, D.x1 - pw, D.y1});
    add_quad(l, quad_kind::panel, irect{l.frame_w - PANEL_W, 0, l.frame_w, (int)rows}, irect{D.x1 - pw, D.y0, D.x1, D.y1});
    l.overlay    = true;
    l.map_w      = D.w() - pw;
    l.map_h      = D.h() - ds;
    l.panel_w    = pw;
    l.strip_h    = ds;
    l.panel_rows = (int)rows;
}

} // namespace

int max_scale_pct(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return MIN_PCT;
    int64_t by_h = (int64_t)screen_h * 100 / 480;
    int64_t by_w = (int64_t)screen_w * 100 / 640;
    int64_t m    = by_h < by_w ? by_h : by_w;
    if (m > MAX_PCT) m = MAX_PCT;
    return m < MIN_PCT ? MIN_PCT : (int)m;
}

int clamp_scale_pct(int req_pct, int screen_w, int screen_h, bool *clamped) {
    if (clamped) *clamped = false;
    if (req_pct < MIN_PCT) return MIN_PCT;
    if (req_pct > MAX_PCT) req_pct = MAX_PCT;
    const int mx = max_scale_pct(screen_w, screen_h);
    if (req_pct > mx) {
        if (clamped) *clamped = true;
        return mx;
    }
    return req_pct;
}

int main_list_rows(int screen_h, int pct) {
    if (pct <= MIN_PCT || screen_h <= 0) return -1;
    const int ds = round_pct(STRIP_H, pct);
    int       mh = screen_h - ds;
    if (mh > MAX_H - STRIP_H) mh = MAX_H - STRIP_H;
    if (mh < 1) return -1;
    const int hp = (int)(((int64_t)(mh + ds) * 100) / pct);
    return hp >= LIST_TOP ? (hp - LIST_TOP) / LIST_ROW : -1;
}

int snap_scale_pct(int, int screen_h, int pct, bool *snapped) {
    if (snapped) *snapped = false;
    if (pct <= MIN_PCT) return pct;
    const int k = main_list_rows(screen_h, pct);
    if (k < 0) return pct;
    const int ds = round_pct(STRIP_H, pct);
    int       mh = screen_h - ds;
    if (mh > MAX_H - STRIP_H) mh = MAX_H - STRIP_H;
    const int hp  = (int)(((int64_t)(mh + ds) * 100) / pct);
    const int gap = (hp - LIST_TOP) - k * LIST_ROW;
    if (gap * 2 < LIST_ROW) return pct;
    const int lo = (pct * 95 + 99) / 100;
    for (int p = pct - 1; p >= lo && p > MIN_PCT; --p)
        if (main_list_rows(screen_h, p) >= k + 1) {
            if (snapped) *snapped = true;
            return p;
        }
    return pct;
}

int effective_scale_pct(int req_pct, int screen_w, int screen_h, bool *clamped, bool *snapped) {
    const int c = clamp_scale_pct(req_pct, screen_w, screen_h, clamped);
    return snap_scale_pct(screen_w, screen_h, c, snapped);
}

int auto_scale_pct(int display_scale_pct) {
    static const int STEPS[] = {100, 125, 150, 175, 200};
    int              best    = STEPS[0];
    for (int s : STEPS) {
        const int d = display_scale_pct > s ? display_scale_pct - s : s - display_scale_pct;
        const int b = display_scale_pct > best ? display_scale_pct - best : best - display_scale_pct;
        if (d <= b) best = s; // ascending: a tie goes to the larger step
    }
    return best;
}

int whole_panel_rows(const layout &l) {
    if (l.single || l.pct <= 0 || l.nquads <= 0) return 0;
    if (l.crop) return l.panel_rows; // RES18: every cropped row shows, at one uniform scale
    int64_t rows = (int64_t)l.used.h() * 100 / l.pct;
    return (int)(rows > l.frame_h ? l.frame_h : rows);
}

int scaled_panel_w(int pct) { return round_pct(PANEL_W, pct); }
int scaled_strip_h(int pct) { return round_pct(STRIP_H, pct); }

render_size ideal_render(int screen_w, int screen_h, int req_pct) {
    render_size r;
    r.pct     = effective_scale_pct(req_pct, screen_w, screen_h, &r.clamped, &r.snapped);
    r.panel_w = scaled_panel_w(r.pct);
    r.strip_h = scaled_strip_h(r.pct);
    int mw    = screen_w - r.panel_w;
    int mh    = screen_h - r.strip_h;
    if (mw < 1) mw = 1;
    if (mh < 1) mh = 1;
    if (mw > MAX_W - PANEL_W) {
        mw       = MAX_W - PANEL_W;
        r.capped = true;
    }
    if (mh > MAX_H - STRIP_H) {
        mh       = MAX_H - STRIP_H;
        r.capped = true;
    }
    r.map_w            = mw;
    r.map_h            = mh;
    r.w                = ceil32(mw) + PANEL_W;
    r.h                = ceil32(mh) + STRIP_H;
    const int64_t rows = ceil_div((int64_t)(mh + r.strip_h) * 100, r.pct);
    r.panel_rows       = rows > r.h ? r.h : (int)rows;
    return r;
}

irect fit_rect(int cw, int ch, int fw, int fh, bool integer) {
    irect r;
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
    r.x0 = (cw - dw) / 2;
    r.y0 = (ch - dh) / 2;
    r.x1 = r.x0 + dw;
    r.y1 = r.y0 + dh;
    return r;
}

static void add_dialogs(layout &l, const dialog_set *dialogs) {
    for (int i = 0; dialogs && i < dialogs->n && i < MAX_DIALOGS; ++i) {
        irect dd;
        if (!place_dialog(l, dialogs->r[i], dd)) continue;
        add_quad(l, quad_kind::dialog, dialogs->r[i], dd);
        l.dialog_src[l.ndialogs]     = dialogs->r[i];
        l.dialog_dst[l.ndialogs]     = dd;
        l.dialog_layered[l.ndialogs] = dialogs->layered[i];
        l.has_dialog                 = true;
        ++l.ndialogs;
    }
}

void build(layout &l, int screen_w, int screen_h, int frame_w, int frame_h, int req_pct, bool integer, const dialog_set *dialogs,
           int crop_w, int crop_h) {
    l               = layout();
    l.screen_w      = screen_w;
    l.screen_h      = screen_h;
    l.frame_w       = frame_w;
    l.frame_h       = frame_h;
    l.req_pct       = req_pct;
    l.integer_scale = integer;
    if (screen_w <= 0 || screen_h <= 0 || frame_w <= 0 || frame_h <= 0) {
        l.single = true;
        return;
    }
    // RES18: the composition inside a frame that was rounded up to the tile grid (0 / a size that is not smaller = none)
    const bool has_crop = crop_w > PANEL_W && crop_h > STRIP_H && crop_w <= frame_w && crop_h <= frame_h && (crop_w < frame_w || crop_h < frame_h);
    l.crop_w            = has_crop ? crop_w : frame_w;
    l.crop_h            = has_crop ? crop_h : frame_h;
    l.pct               = effective_scale_pct(req_pct, screen_w, screen_h, &l.clamped, &l.snapped);
    if (l.pct <= MIN_PCT || frame_w <= PANEL_W || frame_h <= STRIP_H) {
        if (has_crop && frame_w > PANEL_W && frame_h > STRIP_H)
            make_crop(l, integer);
        else
            make_single(l, integer);
        return;
    }
    const int pw = scaled_panel_w(l.pct), ds = scaled_strip_h(l.pct);
    int       mw = screen_w - pw, mh = screen_h - ds;
    if (mw > l.crop_w - PANEL_W) mw = l.crop_w - PANEL_W; // (the frame's map region unless a crop cut it)
    if (mh > l.crop_h - STRIP_H) mh = l.crop_h - STRIP_H;
    if (mw < 1 || mh < 1) {
        make_single(l, integer);
        return;
    }
    // RES21: a composition SMALLER than the room the screen leaves on both axes (an explicit resolution=WxH, or a frame the render caps cut) is shown by the
    // overlay layout (the whole composition fitted like the 100 layout, panel / strip drawn over it). Otherwise the composition is at least the room: the map is
    // shown 1:1 at the top left and whatever the composition holds beyond the room (the ceil32 margin of the ideal frame) stays hidden under the quads.
    const int fmw = l.crop_w - PANEL_W, fmh = l.crop_h - STRIP_H; // the composition's map region
    const int rw = screen_w - pw, rh = screen_h - ds;             // the room the screen leaves for the map
    if (rw > fmw && rh > fmh) {
        make_overlay(l, l.pct, integer);
        add_dialogs(l, dialogs);
        return;
    }
    const int tw = mw + pw, th = mh + ds;
    const int ox = (screen_w - tw) / 2, oy = (screen_h - th) / 2;

    int64_t rows = ceil_div((int64_t)th * 100, l.pct);
    if (rows > l.crop_h) rows = l.crop_h;
    int64_t scols = ceil_div((int64_t)mw * 100, l.pct);
    if (scols > l.crop_w - PANEL_W) scols = l.crop_w - PANEL_W;

    l.single     = false;
    l.map_src_w  = mw;
    l.map_src_h  = mh;
    l.map_w      = mw;
    l.map_h      = mh;
    l.panel_w    = pw;
    l.strip_h    = ds;
    l.panel_rows = (int)rows;
    add_quad(l, quad_kind::map, irect{0, 0, mw, mh}, irect{ox, oy, ox + mw, oy + mh});
    add_quad(l, quad_kind::strip, irect{0, frame_h - STRIP_H, (int)scols, frame_h}, irect{ox, oy + mh, ox + mw, oy + th});
    add_quad(l, quad_kind::panel, irect{frame_w - PANEL_W, 0, frame_w, (int)rows}, irect{ox + mw, oy, ox + tw, oy + th});
    l.used = irect{ox, oy, ox + tw, oy + th};
    add_dialogs(l, dialogs);
}

bool place_dialog(const layout &l, const irect &src, irect &dst) {
    if (l.single || l.nquads < 1 || l.pct <= MIN_PCT) return false;
    const int w = src.w(), h = src.h();
    if (w <= 0 || h <= 0) return false;
    const irect &map = l.q[0].src; // the visible map, frame pixels (1:1 with the screen, or the overlay / crop layouts' uniform fit)
    if (src.x0 < map.x0 || src.y0 < map.y0 || src.x1 > map.x1 || src.y1 > map.y1) return false;
    const irect &u  = l.used;
    const irect &md = l.q[0].dst;
    // The 1x copy on screen: the map quad's image of the dialog's frame rectangle (exactly `src` when the quad is 1:1; larger in the overlay and crop layouts).
    const int cx0 = md.x0 + (int)((int64_t)(src.x0 - map.x0) * md.w() / map.w());
    const int cy0 = md.y0 + (int)((int64_t)(src.y0 - map.y0) * md.h() / map.h());
    const int cx1 = md.x0 + (int)(((int64_t)(src.x1 - map.x0) * md.w() + map.w() - 1) / map.w());
    const int cy1 = md.y0 + (int)(((int64_t)(src.y1 - map.y0) * md.h() + map.h() - 1) / map.h());
    const int cw = cx1 - cx0, ch = cy1 - cy0;
    int64_t   p = l.pct;
    if ((int64_t)u.w() * 100 / w < p) p = (int64_t)u.w() * 100 / w;
    if ((int64_t)u.h() * 100 / h < p) p = (int64_t)u.h() * 100 / h;
    if (p <= MIN_PCT) return false;
    const int dw = round_pct(w, (int)p), dh = round_pct(h, (int)p);
    if (dw > u.w() || dh > u.h() || dw < cw || dh < ch) return false;
    auto place = [](int centre, int len, int cover0, int cover1, int lo, int hi) {
        int x0 = centre - len / 2;
        if (x0 > cover0) x0 = cover0; // the copy must be inside [x0, x0 + len)
        if (x0 + len < cover1) x0 = cover1 - len;
        if (x0 + len > hi) x0 = hi - len;
        if (x0 < lo) x0 = lo;
        return x0;
    };
    const int x0 = place(md.x0 + md.w() / 2, dw, cx0, cx1, u.x0, u.x1);
    const int y0 = place(md.y0 + md.h() / 2, dh, cy0, cy1, u.y0, u.y1);
    dst          = irect{x0, y0, x0 + dw, y0 + dh};
    return true;
}

int quad_at(const layout &l, int x, int y) {
    for (int i = l.nquads - 1; i >= 0; --i)
        if (l.q[i].dst.contains(x, y)) return i;
    return -1;
}

bool screen_to_frame(const layout &l, int &x, int &y) {
    if (l.nquads <= 0 || l.frame_w <= 0 || l.frame_h <= 0) return false;
    if (l.single) { // the legacy mapping (present_backend.h image_client_to_game), unchanged
        const irect &r  = l.q[0].dst;
        const int    rw = r.w(), rh = r.h();
        if (rw <= 0 || rh <= 0) return false;
        const int cx = x < r.x0 ? r.x0 : x >= r.x1 ? r.x1 - 1
                                                   : x;
        const int cy = y < r.y0 ? r.y0 : y >= r.y1 ? r.y1 - 1
                                                   : y;
        x            = (int)((long long)(cx - r.x0) * l.frame_w / rw);
        y            = (int)((long long)(cy - r.y0) * l.frame_h / rh);
        return true;
    }
    const irect &u = l.used;
    if (x < u.x0) x = u.x0;
    if (x >= u.x1) x = u.x1 - 1;
    if (y < u.y0) y = u.y0;
    if (y >= u.y1) y = u.y1 - 1;
    const int qi = quad_at(l, x, y);
    if (qi < 0) return false;
    const quad &q   = l.q[qi];
    int         fx  = q.src.x0 + centre_src(x - q.dst.x0, q.dst.w(), q.src.w());
    int         fy  = q.src.y0 + centre_src(y - q.dst.y0, q.dst.h(), q.src.h());
    const bool  hud = q.kind == quad_kind::map || q.kind == quad_kind::strip || q.kind == quad_kind::panel;
    if (hud && x == u.x1 - 1 && q.dst.x1 == u.x1) fx = l.frame_w - 1; // the edge rule
    if (hud && y == u.y1 - 1 && q.dst.y1 == u.y1) fy = l.frame_h - 1;
    x = fx;
    y = fy;
    return true;
}

bool frame_to_screen(const layout &l, int &x, int &y) {
    if (l.nquads <= 0 || l.frame_w <= 0 || l.frame_h <= 0) return false;
    if (l.single) { // the legacy mapping (image_game_to_client): the centre of the game pixel
        const irect &r  = l.q[0].dst;
        const int    rw = r.w(), rh = r.h();
        if (rw <= 0 || rh <= 0) return false;
        const int gx = x < 0 ? 0 : x >= l.frame_w ? l.frame_w - 1
                                                  : x;
        const int gy = y < 0 ? 0 : y >= l.frame_h ? l.frame_h - 1
                                                  : y;
        x            = r.x0 + (int)(((long long)gx * 2 + 1) * rw / (2LL * l.frame_w));
        y            = r.y0 + (int)(((long long)gy * 2 + 1) * rh / (2LL * l.frame_h));
        return true;
    }
    int best = -1;
    for (int i = l.nquads - 1; i >= 0; --i)
        if (l.q[i].src.contains(x, y)) {
            best = i;
            break;
        }
    bool visible = best >= 0;
    int  px = x, py = y;
    if (!visible) { // hidden: the nearest visible pixel of the nearest quad
        int64_t bd = -1;
        for (int i = l.nquads - 1; i >= 0; --i) {
            const irect  &s  = l.q[i].src;
            const int     cx = x < s.x0 ? s.x0 : x >= s.x1 ? s.x1 - 1
                                                           : x;
            const int     cy = y < s.y0 ? s.y0 : y >= s.y1 ? s.y1 - 1
                                                           : y;
            const int64_t d  = (int64_t)(cx - x) * (cx - x) + (int64_t)(cy - y) * (cy - y);
            if (bd < 0 || d < bd) {
                bd   = d;
                best = i;
                px   = cx;
                py   = cy;
            }
        }
        if (best < 0) return false;
    }
    const quad &q = l.q[best];
    x             = q.dst.x0 + span_mid(px - q.src.x0, q.dst.w(), q.src.w());
    y             = q.dst.y0 + span_mid(py - q.src.y0, q.dst.h(), q.src.h());
    if (visible && l.overlay) // RES21: a base pixel whose screen position is under the panel / strip overlay is hidden
        for (int i = l.nquads - 1; i > best; --i)
            if ((l.q[i].kind == quad_kind::panel || l.q[i].kind == quad_kind::strip) && l.q[i].dst.contains(x, y)) return false;
    return visible;
}

void compose_bgr24(const layout &l, const uint16_t *frame, int frame_pitch, uint8_t *out, int out_stride) {
    if (!out || l.screen_w <= 0 || l.screen_h <= 0) return;
    for (int y = 0; y < l.screen_h; ++y) memset(out + (size_t)y * out_stride, 0, (size_t)l.screen_w * 3);
    if (!frame) return;
    for (int i = 0; i < l.nquads; ++i) {
        const quad &q  = l.q[i];
        const int   dw = q.dst.w(), dh = q.dst.h(), sw = q.src.w(), sh = q.src.h();
        if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || l.quad_is_layered(i)) continue; // 6b: a layered dialog is an overlay
        for (int y = q.dst.y0; y < q.dst.y1; ++y) {
            if (y < 0 || y >= l.screen_h) continue;
            const int       sy   = q.src.y0 + centre_src(y - q.dst.y0, dh, sh);
            const uint16_t *srow = (const uint16_t *)((const uint8_t *)frame + (size_t)sy * frame_pitch);
            uint8_t        *drow = out + (size_t)y * out_stride;
            for (int x = q.dst.x0; x < q.dst.x1; ++x) {
                if (x < 0 || x >= l.screen_w) continue;
                const uint16_t p  = srow[q.src.x0 + centre_src(x - q.dst.x0, dw, sw)];
                const int      r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f;
                uint8_t       *d = drow + (size_t)x * 3;
                d[0]             = (uint8_t)((b5 << 3) | (b5 >> 2));
                d[1]             = (uint8_t)((g6 << 2) | (g6 >> 4));
                d[2]             = (uint8_t)((r5 << 3) | (r5 >> 2));
            }
        }
    }
}

namespace {
int chan8(uint16_t p, int c) {
    if (c == 0) {
        const int r5 = (p >> 11) & 0x1f;
        return (r5 << 3) | (r5 >> 2);
    }
    if (c == 1) {
        const int g6 = (p >> 5) & 0x3f;
        return (g6 << 2) | (g6 >> 4);
    }
    const int b5 = p & 0x1f;
    return (b5 << 3) | (b5 >> 2);
}
} // namespace

uint32_t layer_pixel_from_passes(uint16_t on_black, uint16_t on_white, uint16_t on_grey, bool *used_grey) {
    if (used_grey) *used_grey = false;
    int            bk[3], dw = 0, dg = 0, gv = 0;
    const uint16_t grey = 0x8410;
    for (int c = 0; c < 3; ++c) {
        bk[c] = chan8(on_black, c);
        dw += chan8(on_white, c) - bk[c];
        dg += chan8(on_grey, c) - bk[c];
        gv += chan8(grey, c);
    }
    // s in 1/255 units: from white  = sum(white - black) / 3 ; from grey = sum(grey - black) * 255 / sum(grey value)
    int sw = (dw + 1) / 3;
    int sg = gv > 0 ? (dg * 255 + gv / 2) / gv : sw;
    int s  = sw;
    if (sw - sg > 12 || sg - sw > 12) { // 12 = 1.5 LSB of a 5-bit channel at 8 bits (truncation in the game's own LUTs)
        // the model (out = C + s dst) disagrees between the two: clamp or overflow over white
        s = sg;
        if (used_grey) *used_grey = true;
    }
    if (s < 0) s = 0;
    if (s > 255) s = 255;
    const int a = 255 - s;
    if (a == 0) return 0;
    for (int c = 0; c < 3; ++c)
        if (bk[c] > a) bk[c] = a;
    return ((uint32_t)a << 24) | ((uint32_t)bk[0] << 16) | ((uint32_t)bk[1] << 8) | (uint32_t)bk[2];
}

bool place_dialog_layer(const layout &l, int i, const uint32_t *px, uint32_t *buf, int buf_px, cursor_overlay &out) {
    out = cursor_overlay();
    if (l.single || !px || !buf || i < 0 || i >= l.ndialogs || l.screen_w <= 0 || l.screen_h <= 0) return false;
    const irect &sr = l.dialog_src[i], &d = l.dialog_dst[i];
    const int    sw = sr.w(), sh = sr.h(), dw = d.w(), dh = d.h();
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    const int cx0 = d.x0 < 0 ? 0 : d.x0, cy0 = d.y0 < 0 ? 0 : d.y0;
    const int cx1 = d.x1 > l.screen_w ? l.screen_w : d.x1, cy1 = d.y1 > l.screen_h ? l.screen_h : d.y1;
    const int w = cx1 - cx0, h = cy1 - cy0;
    if (w <= 0 || h <= 0 || (int64_t)w * h > buf_px) return false;
    for (int y = 0; y < h; ++y) {
        const int sy = centre_src(cy0 + y - d.y0, dh, sh);
        for (int x = 0; x < w; ++x) buf[(size_t)y * w + x] = px[(size_t)sy * sw + centre_src(cx0 + x - d.x0, dw, sw)];
    }
    out.px = buf;
    out.w  = w;
    out.h  = h;
    out.x  = cx0;
    out.y  = cy0;
    return true;
}

uint32_t cursor_pixel_from_passes(uint16_t on_black, uint16_t on_white) {
    auto ch = [](uint16_t p, int c) {
        if (c == 0) {
            const int r5 = (p >> 11) & 0x1f;
            return (r5 << 3) | (r5 >> 2);
        }
        if (c == 1) {
            const int g6 = (p >> 5) & 0x3f;
            return (g6 << 2) | (g6 >> 4);
        }
        const int b5 = p & 0x1f;
        return (b5 << 3) | (b5 >> 2);
    };
    int bk[3], d = 0;
    for (int c = 0; c < 3; ++c) {
        bk[c] = ch(on_black, c);
        d += ch(on_white, c) - bk[c];
    }
    d = (d + 1) / 3;
    if (d < 0) d = 0;
    if (d > 255) d = 255;
    const int a = 255 - d;
    if (a == 0) return 0;
    for (int c = 0; c < 3; ++c)
        if (bk[c] > a) bk[c] = a;
    return ((uint32_t)a << 24) | ((uint32_t)bk[0] << 16) | ((uint32_t)bk[1] << 8) | (uint32_t)bk[2];
}

bool place_cursor(const layout &l, const uint32_t *src, int fx, int fy, uint32_t *buf, int buf_px, cursor_overlay &out) {
    out = cursor_overlay();
    if (l.single || !src || !buf || l.pct <= MIN_PCT || l.screen_w <= 0 || l.screen_h <= 0) return false;
    int sx = fx, sy = fy;
    frame_to_screen(l, sx, sy);
    const int full = (int)(((int64_t)CURSOR_SRC * l.pct + 99) / 100);      // the scaled side
    const int hot  = (int)(((int64_t)(2 * CURSOR_HOT + 1) * l.pct) / 200); // floor(80.5 s): the hotspot pixel's middle
    const int x0 = sx - hot, y0 = sy - hot;
    const int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0;
    const int cx1 = x0 + full > l.screen_w ? l.screen_w : x0 + full;
    const int cy1 = y0 + full > l.screen_h ? l.screen_h : y0 + full;
    const int w = cx1 - cx0, h = cy1 - cy0;
    if (w <= 0 || h <= 0 || (int64_t)w * h > buf_px) return false;
    for (int y = 0; y < h; ++y) {
        int srow = (int)(((int64_t)(2 * (cy0 + y - y0) + 1) * 100) / (2LL * l.pct));
        if (srow >= CURSOR_SRC) srow = CURSOR_SRC - 1;
        for (int x = 0; x < w; ++x) {
            int scol = (int)(((int64_t)(2 * (cx0 + x - x0) + 1) * 100) / (2LL * l.pct));
            if (scol >= CURSOR_SRC) scol = CURSOR_SRC - 1;
            buf[(size_t)y * w + x] = src[(size_t)srow * CURSOR_SRC + scol];
        }
    }
    out.px = buf;
    out.w  = w;
    out.h  = h;
    out.x  = cx0;
    out.y  = cy0;
    return true;
}

bool place_scaled_layer(const layout &l, const uint32_t *src, int sw, int sh, int fx, int fy, uint32_t *buf, int buf_px, cursor_overlay &out) {
    out = cursor_overlay();
    if (l.single || l.crop || !src || !buf || sw <= 0 || sh <= 0 || l.pct <= MIN_PCT || l.nquads < 1 || l.screen_w <= 0 || l.screen_h <= 0) return false;
    const irect &md = l.q[0].dst; // the map quad: its top-left is the frame's (0,0)
    const int    x0 = md.x0 + (int)((int64_t)fx * l.pct / 100), y0 = md.y0 + (int)((int64_t)fy * l.pct / 100);
    const int    dw = (int)(((int64_t)sw * l.pct + 99) / 100), dh = (int)(((int64_t)sh * l.pct + 99) / 100);
    const int    cx0 = x0 < l.used.x0 ? l.used.x0 : x0, cy0 = y0 < l.used.y0 ? l.used.y0 : y0;
    const int    mx1 = md.x0 + l.map_w, my1 = md.y0 + l.map_h;                             // the VISIBLE map (RES21: the panel / strip overlays hide part of the base map quad)
    const int    cx1 = x0 + dw > mx1 ? mx1 : x0 + dw, cy1 = y0 + dh > my1 ? my1 : y0 + dh; // only over the map, never the panel / strip
    const int    w = cx1 - cx0, h = cy1 - cy0;
    if (w <= 0 || h <= 0 || (int64_t)w * h > buf_px) return false;
    for (int y = 0; y < h; ++y) {
        int srow = (int)(((int64_t)(2 * (cy0 + y - y0) + 1) * 100) / (2LL * l.pct));
        if (srow >= sh) srow = sh - 1;
        for (int x = 0; x < w; ++x) {
            int scol = (int)(((int64_t)(2 * (cx0 + x - x0) + 1) * 100) / (2LL * l.pct));
            if (scol >= sw) scol = sw - 1;
            buf[(size_t)y * w + x] = src[(size_t)srow * sw + scol];
        }
    }
    out.px = buf;
    out.w  = w;
    out.h  = h;
    out.x  = cx0;
    out.y  = cy0;
    return true;
}

void blend_cursor_bgr24(const cursor_overlay &c, uint8_t *out, int out_stride, int screen_w, int screen_h) {
    if (!c.px || !out) return;
    for (int y = 0; y < c.h; ++y) {
        const int dy = c.y + y;
        if (dy < 0 || dy >= screen_h) continue;
        for (int x = 0; x < c.w; ++x) {
            const int dx = c.x + x;
            if (dx < 0 || dx >= screen_w) continue;
            const uint32_t p = c.px[(size_t)y * c.w + x];
            const int      a = (int)(p >> 24);
            if (a == 0) continue;
            uint8_t *d = out + (size_t)dy * out_stride + (size_t)dx * 3;
            d[0]       = (uint8_t)((int)(p & 0xff) + (d[0] * (255 - a) + 127) / 255);
            d[1]       = (uint8_t)((int)((p >> 8) & 0xff) + (d[1] * (255 - a) + 127) / 255);
            d[2]       = (uint8_t)((int)((p >> 16) & 0xff) + (d[2] * (255 - a) + 127) / 255);
        }
    }
}

} // namespace mh::gfx::uilayout
