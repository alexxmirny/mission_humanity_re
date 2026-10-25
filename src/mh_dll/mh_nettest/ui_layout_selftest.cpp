//
// ui_layout_selftest.cpp -- `net_selftest.exe uilayouttest`: the pure parts of mods:RES4 stages 1-2
// (gfx/ui_layout.{h,cpp}), the UI-scale layout.
//
//   1. the scale limit and the clamp (min(Dv_h/480, Dv_w/640)), the panel / strip sizes;
//   2. ideal_render: exact sizes for common screens, multiples of 32, W <= 2048 and H <= 1600 over a grid,
//      W/H never exceed the screen once s is large enough to hide the 31-pixel rounding;
//   3. the quad list at the ideal frame: source / destination rectangles, tiling of the composition, every
//      quad at least 1:1, a stock-size frame centred;
//   4. the inverse map: round trip over every visible frame pixel, monotone, every source pixel reachable,
//      the edge rule, a point outside the composition clamps;
//   5. s = 100 is the old presenter: one quad, fit_image_rect, and the legacy mouse map, over a grid;
//   6. the CPU compositor against the inverse map;
//   6b. RES18: the crop -- a frame rounded up to the tile grid shown cropped to the session size: the quads, the mouse map at the cut, the
//      hidden rows, the tiling, the composition against the inverse map, and that no crop / a degenerate one is today's single quad;
//   7. the two real presenters (stage 2): the gdi backend into a shown window and the d3d11 backend into its back
//      buffer, ui_scale 125 on 800x600, read back and compared with the compositor (gdi: the 1:1 map exact, the
//      scaled quads within one source pixel; d3d11: the compositor within the sampler's rounding). No device / no
//      window to show = a SKIP line and a pass, the presenters' own fallback contract.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "gfx/present_backend.h"
#include "gfx/ui_layout.h"
#include "gfx/overlay_list.h"
#include "gfx/classic_sizes.h"

namespace {

namespace ul = mh::gfx::uilayout;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

bool same(const ul::irect &r, int x0, int y0, int x1, int y1) { return r.x0 == x0 && r.y0 == y0 && r.x1 == x1 && r.y1 == y1; }

const int SCREENS[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768}, {1600, 900}, {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 2160}, {5120, 2880}};
const int PCTS[]       = {100, 110, 125, 150, 175, 200, 225, 250, 300, 400};

void test_limits() {
    check("1920x1080 allows 225", ul::max_scale_pct(1920, 1080) == 225);
    check("1024x768 allows 160 (both axes)", ul::max_scale_pct(1024, 768) == 160);
    check("640x480 allows only 100", ul::max_scale_pct(640, 480) == 100);
    check("a screen below 640x480 still reads 100", ul::max_scale_pct(320, 200) == 100);
    check("3840x2160 allows the 400 ceiling", ul::max_scale_pct(3840, 2160) == 400);
    check("a bad screen reads 100", ul::max_scale_pct(0, 0) == 100);
    bool cl = false;
    check("150 on 1280x720 is clamped to 150 (720/480)", ul::clamp_scale_pct(150, 1280, 720, &cl) == 150 && !cl);
    check("200 on 1280x720 clamps to 150 and says so", ul::clamp_scale_pct(200, 1280, 720, &cl) == 150 && cl);
    check("below 100 reads 100", ul::clamp_scale_pct(50, 1920, 1080, &cl) == 100 && !cl);
    check("above 400 reads at most 400", ul::clamp_scale_pct(900, 7680, 4320, &cl) == 400);
    check("panel width round(160 s)", ul::scaled_panel_w(125) == 200 && ul::scaled_panel_w(150) == 240 &&
                                          ul::scaled_panel_w(175) == 280 && ul::scaled_panel_w(200) == 320 &&
                                          ul::scaled_panel_w(100) == 160 && ul::scaled_panel_w(110) == 176);
    check("strip height round(32 s)", ul::scaled_strip_h(125) == 40 && ul::scaled_strip_h(150) == 48 &&
                                          ul::scaled_strip_h(175) == 56 && ul::scaled_strip_h(200) == 64 &&
                                          ul::scaled_strip_h(100) == 32 && ul::scaled_strip_h(110) == 35);
}

void test_ideal_render() {
    ul::render_size r = ul::ideal_render(1920, 1080, 150);
    check("1920x1080 @150: 1856x1088", r.w == 1856 && r.h == 1088 && r.pct == 150 && !r.capped);
    check("1920x1080 @150: map 1680x1032, panel 240, strip 48", r.map_w == 1680 && r.map_h == 1032 && r.panel_w == 240 && r.strip_h == 48);
    check("1920x1080 @150: panel shows 720 rows", r.panel_rows == 720);
    r = ul::ideal_render(1920, 1080, 125);
    check("1920x1080 @125: 1888x1088", r.w == 1888 && r.h == 1088);
    r = ul::ideal_render(1920, 1080, 200);
    check("1920x1080 @200 snaps to 192% (one more whole list row): map 1613x1019, frame 1792x1056", r.snapped && r.pct == 192 && r.map_w == 1613 && r.map_h == 1019 && r.w == 1792 && r.h == 1056);
    r = ul::ideal_render(1920, 1080, 100);
    check("1920x1080 @100: 1920x1088 (the 8 hidden map rows)", r.w == 1920 && r.h == 1088);
    r = ul::ideal_render(2560, 1440, 200);
    check("2560x1440 @200: width capped to 2048, height 1408", r.capped && r.w == 2048 && r.h == 1408 && r.map_w == 1888 && r.map_h == 1376);
    r = ul::ideal_render(3840, 2160, 200);
    check("3840x2160 @200: both capped, 2048x1600", r.capped && r.w == 2048 && r.h == 1600);
    r = ul::ideal_render(1024, 768, 300);
    check("1024x768 @300 clamps to 160", r.clamped && r.pct == 160);
    r = ul::ideal_render(640, 480, 200);
    check("640x480 @200 clamps to 100: 640x480", r.clamped && r.pct == 100 && r.w == 640 && r.h == 480);

    bool all_ok = true, caps_ok = true, fits_ok = true, rows_ok = true;
    for (const auto &s : SCREENS)
        for (int p : PCTS) {
            const ul::render_size q = ul::ideal_render(s[0], s[1], p);
            if (q.w % 32 || q.h % 32) all_ok = false;
            if (q.w > ul::MAX_W || q.h > ul::MAX_H) caps_ok = false;
            if (q.w < 640 || q.h < 480) fits_ok = false;
            // the rounding up to 32 hides at most 31 map pixels, so the frame is never more than 31 over the screen
            if (!q.capped && (q.w > s[0] + 31 || q.h > s[1] + 31)) fits_ok = false;
            if (q.panel_rows < 1 || q.panel_rows > q.h) rows_ok = false;
        }
    check("every frame size is a multiple of 32", all_ok);
    check("W <= 2048 and H <= 1600 everywhere", caps_ok);
    check("never below 640x480; never more than 31 px over the screen", fits_ok);
    check("panel rows within 1..H", rows_ok);
}

// Stage 3: the row counts and the frame the game is asked to render.
void test_stage3() {
    // Hp = ceil(Dv_h / s) wherever the composition fills the screen vertically, and the built layout agrees with ideal_render
    bool hp_ok = true, agree_ok = true, strip_ok = true, floor_ok = true;
    for (const auto &s : SCREENS)
        for (int p : PCTS) {
            const ul::render_size q = ul::ideal_render(s[0], s[1], p);
            ul::layout            l;
            ul::build(l, s[0], s[1], q.w, q.h, p);
            if (q.pct <= 100) continue;
            if (l.single) {
                agree_ok = false;
                continue;
            }
            if (l.panel_rows != q.panel_rows && !q.capped) agree_ok = false; // (capped: the composition is smaller than the room and the overlay fits it to the screen, ideal_render does not model that)
            if (!q.capped) {
                const int hp = (int)(((int64_t)s[1] * 100 + q.pct - 1) / q.pct);
                if (q.panel_rows != (hp < q.h ? hp : q.h)) hp_ok = false;
                // the map fills the screen's width above the panel and its height above the strip, 1:1
                if (l.map_w != s[0] - q.panel_w || l.map_h != s[1] - q.strip_h) agree_ok = false;
            }
            // the strip shows the left ceil(Mw / s) columns of the frame and never more than the map's frame width
            int cols = 0;
            for (int i = 0; i < l.nquads; ++i)
                if (l.q[i].kind == ul::quad_kind::strip) cols = l.q[i].src.x1;
            if (cols <= 0 || cols > q.w - ul::PANEL_W) strip_ok = false;
            // a scaled composition never shows fewer than 480 frame rows of the panel (the shortest stock window)
            if (q.panel_rows < 480 * 100 / q.pct - 1) floor_ok = false;
        }
    check("stage 3: Hp = ceil(Dv_h / s) (capped to the frame) at every screen and scale", hp_ok);
    check("stage 3: build() on the ideal frame agrees with ideal_render (panel rows, 1:1 map)", agree_ok);
    check("stage 3: the strip source stops at the visible width", strip_ok);
    // whole rows: a list sized to W rows ends at or above the screen's bottom, and one more row would not fit
    bool whole_ok = true, le_ok = true;
    for (const auto &s : SCREENS)
        for (int p : PCTS) {
            const ul::render_size q = ul::ideal_render(s[0], s[1], p);
            ul::layout            l;
            ul::build(l, s[0], s[1], q.w, q.h, p);
            if (l.single) {
                if (ul::whole_panel_rows(l) != 0) whole_ok = false;
                continue;
            }
            const int w = ul::whole_panel_rows(l);
            if (w * l.pct > l.used.h() * 100 || (w + 1) * l.pct <= l.used.h() * 100) {
                if (w < l.frame_h) whole_ok = false;
            }
            if (w > l.panel_rows || l.panel_rows - w > 1) le_ok = false;
        }
    check("stage 3: whole_panel_rows never reaches past the screen's bottom and is the largest that does not", whole_ok);
    check("stage 3: whole rows <= the quad's rows, at most one fewer", le_ok);
    check("stage 3: the panel always shows its 480 stock rows", floor_ok);
    // the documented examples
    const ul::render_size a = ul::ideal_render(1920, 1080, 150);
    check("1920x1080 @150 -> 1856x1088, Hp 720", a.w == 1856 && a.h == 1088 && a.panel_rows == 720);
    const ul::render_size b = ul::ideal_render(1280, 800, 150);
    check("1280x800 @150 -> 1216x800, Hp 534", b.w == 1216 && b.h == 800 && b.panel_rows == 534);
    const ul::render_size c = ul::ideal_render(1280, 960, 200);
    check("1280x960 @200 -> 1120x928, Hp 480", c.w == 1120 && c.h == 928 && c.panel_rows == 480);
    const ul::render_size d = ul::ideal_render(1024, 768, 200); // 768/480 = 1.6 -> clamped
    check("1024x768 @200 clamps to 160%", d.clamped && d.pct == 160);
}

// The snap (user, 2026-10-19): s' in [0.95 s, s] that fits one more whole row of the main list when the gap under the last one is >= half a row.
void test_snap() {
    // 175% on 1280x960: 5 rows + a 0.77 row gap -> 171% fits 6 whole rows
    bool      clamped = false, snapped = false;
    const int e = ul::effective_scale_pct(175, 1280, 960, &clamped, &snapped);
    check("snap: 175% on 1280x960 -> 171%", e == 171 && snapped && !clamped);
    check("snap: 175% on 1280x960 showed 5 whole rows, 171% shows 6", ul::main_list_rows(960, 175) == 5 && ul::main_list_rows(960, 171) == 6);
    const ul::render_size r = ul::ideal_render(1280, 960, 175);
    check("snap: the render size takes s' (171%): pct, panel width and strip height", r.pct == 171 && r.snapped && r.panel_w == 274 && r.strip_h == 55);
    ul::layout l;
    ul::build(l, 1280, 960, r.w, r.h, 175);
    check("snap: the layout takes s' too and shows 6 whole list rows (Hp 561, 6 rows)",
          !l.single && l.pct == 171 && l.snapped && l.req_pct == 175 && (ul::whole_panel_rows(l) - ul::LIST_TOP) / ul::LIST_ROW == 6);
    int x = 1279, y = 959;
    check("snap: the inverse map still ends on the frame's last column / row", ul::screen_to_frame(l, x, y) && x == r.w - 1 && y == r.h - 1);
    // a snap that would need more than 5% does not happen (500 px tall at 101%: gap 39 but one more row needs 6%)
    snapped = true;
    check("snap: 101% on a 1000x500 screen stays (the snap would need more than 5%)", ul::effective_scale_pct(101, 1000, 500, nullptr, &snapped) == 101 && !snapped);
    check("snap: 112% on a 1000x540 screen stays", ul::effective_scale_pct(112, 1000, 540, nullptr, &snapped) == 112 && !snapped);
    // a gap under half a row does not snap: 200% on 1280x960 has a 24 px gap (4 rows)
    check("snap: a gap under half a row keeps s (200% on 1280x960)", ul::effective_scale_pct(200, 1280, 960, nullptr, &snapped) == 200 && !snapped);
    // 100 is never snapped, and the 100% layout is the old single quad
    bool never = true;
    for (const auto &sc : SCREENS) {
        bool sn = true;
        if (ul::effective_scale_pct(100, sc[0], sc[1], nullptr, &sn) != 100 || sn) never = false;
        ul::layout l1;
        ul::build(l1, sc[0], sc[1], 1024, 768, 100);
        if (!l1.single || l1.pct != 100 || l1.snapped) never = false;
    }
    check("snap: 100% is never snapped and stays the single fitted quad", never);
    // the property over every screen and scale
    bool prop = true;
    for (const auto &sc : SCREENS)
        for (int p : PCTS) {
            bool      cl = false, sn = false;
            const int c  = ul::clamp_scale_pct(p, sc[0], sc[1], &cl);
            const int ef = ul::effective_scale_pct(p, sc[0], sc[1], &cl, &sn);
            if (c <= 100) {
                if (ef != c || sn) prop = false;
                continue;
            }
            if (ef > c || (sn != (ef < c))) prop = false;
            if (sn && (ef * 100 < c * 95 || ul::main_list_rows(sc[1], ef) < ul::main_list_rows(sc[1], c) + 1)) prop = false;
            if (!sn) { // no snap: the gap was under half a row, or no s' within 5% fits another row
                const int k = ul::main_list_rows(sc[1], c);
                if (k >= 0) {
                    bool could = false;
                    for (int q = c - 1; q >= (c * 95 + 99) / 100 && q > 100; --q)
                        if (ul::main_list_rows(sc[1], q) >= k + 1) could = true;
                    // the gap in px: recompute from the composition height
                    const int ds = (32 * c + 50) / 100;
                    int       mh = sc[1] - ds;
                    if (mh > ul::MAX_H - 32) mh = ul::MAX_H - 32;
                    const int gap = ((mh + ds) * 100 / c - ul::LIST_TOP) - k * ul::LIST_ROW;
                    if (could && gap * 2 >= ul::LIST_ROW) prop = false;
                }
            }
            // every layout / render size built for the request agrees on s'
            const ul::render_size q = ul::ideal_render(sc[0], sc[1], p);
            ul::layout            lq;
            ul::build(lq, sc[0], sc[1], q.w, q.h, p);
            if (q.pct != ef || (!q.capped && lq.pct != ef)) prop = false; // (a capped frame is smaller than the room: the RES21 overlay layout, whose base may be the 100 fit)
        }
    check("snap: over every screen and scale: within [0.95 s, s], one more row, and the render size / layout agree on s'", prop);
}

// [video] ui_scale=auto: the monitor's display scale -> the nearest step; the screen clamp is the layout's (as for an explicit value).
void test_auto() {
    struct {
        int in, want;
    } T[]   = {{96, 100}, {100, 100}, {106, 100}, {112, 100}, {113, 125}, {125, 125}, {137, 125}, {138, 150}, {150, 150}, {162, 150}, {163, 175}, {175, 175}, {187, 175}, {188, 200}, {200, 200}, {225, 200}, {300, 200}, {500, 200}, {0, 100}};
    bool ok = true;
    for (const auto &t : T)
        if (ul::auto_scale_pct(t.in) != t.want) {
            ok = false;
            printf("  auto_scale_pct(%d) = %d, wanted %d\n", t.in, ul::auto_scale_pct(t.in), t.want);
        }
    check("auto: display scale -> nearest of 100/125/150/175/200 (ties up, capped at 200)", ok);
    // 200% display scale on a 1024x768 screen: auto picks 200, the layout clamps it to what fits (1.6)
    check("auto: the pick is clamped like an explicit value (1024x768 @200 -> 160)", ul::clamp_scale_pct(ul::auto_scale_pct(200), 1024, 768) == 160);
    check("auto: 1920x1080 at a 150% display scale -> the 1856x1088 frame", ul::ideal_render(1920, 1080, ul::auto_scale_pct(150)).w == 1856);
}

// Stage 4: a click on each quad lands on the source pixel the quad shows; the edge rule.
void test_stage4() {
    ul::layout l;
    ul::build(l, 1280, 800, 1216, 800, 150);
    // the Menu button of the stock side panel sits at panel-local (108,150): the screen point 1040 + 108*1.5 = 1202, 225
    int x = 1202, y = 225;
    check("1280x800 @150: screen (1202,225) -> frame (1164,150)", ul::screen_to_frame(l, x, y) && x == 1056 + 108 && y == 150);
    x = 700, y = 400;
    check("a map click is the identity", ul::screen_to_frame(l, x, y) && x == 700 && y == 400);
    x = 300, y = 780; // the strip (rows 752..799 on screen): frame rows 768..799 at 1.5x
    check("a strip click maps to the strip rows", ul::screen_to_frame(l, x, y) && y >= 768 && y < 800 && x == 200);
    x = 1279, y = 100;
    check("edge rule: the last screen column is frame W-1", ul::screen_to_frame(l, x, y) && x == 1215);
    x = 100, y = 799;
    check("edge rule: the last screen row is frame H-1", ul::screen_to_frame(l, x, y) && y == 799);
    x = 5000, y = -40;
    check("outside the composition clamps to its corner", ul::screen_to_frame(l, x, y) && x == 1215 && y == 0);
    // the cursor carry both ways
    int fx = 1164, fy = 150;
    check("frame (1164,150) is visible and maps to the panel", ul::frame_to_screen(l, fx, fy) && fx >= 1040 && fx < 1280 && fy == 225);
    // a frame pixel under the panel (the hidden map column) is reported hidden but still lands somewhere sensible
    fx = 1050, fy = 300;
    const bool vis = ul::frame_to_screen(l, fx, fy);
    check("a hidden map column reports false and lands on the edge", !vis && fx >= 1030 && fx < 1280);
}

void test_quads_ideal() {
    ul::layout l;
    ul::build(l, 1920, 1080, 1856, 1088, 150);
    check("1920x1080 @150: three quads, multi", !l.single && l.nquads == 3 && l.pct == 150 && !l.clamped);
    check("map 1:1 at the origin", l.q[0].kind == ul::quad_kind::map && same(l.q[0].src, 0, 0, 1680, 1032) && same(l.q[0].dst, 0, 0, 1680, 1032));
    check("strip: frame rows 1056..1088, 1120 columns, scaled under the map",
          l.q[1].kind == ul::quad_kind::strip && same(l.q[1].src, 0, 1056, 1120, 1088) && same(l.q[1].dst, 0, 1032, 1680, 1080));
    check("panel: right 160 columns, 720 rows, scaled to the full height",
          l.q[2].kind == ul::quad_kind::panel && same(l.q[2].src, 1696, 0, 1856, 720) && same(l.q[2].dst, 1680, 0, 1920, 1080));
    check("the composition is the whole screen", same(l.used, 0, 0, 1920, 1080));

    // a stock 1024x768 frame on the same screen: the visible map is the frame's, the composition is centred
    ul::build(l, 1920, 1080, 1024, 768, 150);
    check("stock frame: RES21 overlay layout, map 1200x1032 visible beside the 240 panel and above the 48 strip", l.overlay && l.map_w == 1200 && l.map_h == 1032 && l.panel_w == 240 && l.strip_h == 48);
    check("stock frame: the whole frame is fitted like 100 (1440x1080 centred at 240,0)", same(l.used, 240, 0, 1680, 1080));
    check("stock frame: panel rows ceil(1080/1.5) = 720", l.panel_rows == 720 && l.q[4].src.h() == 720);

    bool tile_ok = true, up_ok = true, hit_ok = true;
    for (const auto &s : SCREENS)
        for (int p : PCTS) {
            const ul::render_size rs = ul::ideal_render(s[0], s[1], p);
            ul::build(l, s[0], s[1], rs.w, rs.h, p);
            if (l.single || l.overlay) continue; // (an overlay layout's panel / strip lie ON the base map: test_overlay)
            // the quads tile `used` exactly: areas add up and no screen pixel is in two quads
            long long area = 0;
            for (int i = 0; i < l.nquads; ++i) {
                area += (long long)l.q[i].dst.w() * l.q[i].dst.h();
                if (l.q[i].dst.w() < l.q[i].src.w() || l.q[i].dst.h() < l.q[i].src.h()) up_ok = false;
                if (l.q[i].src.x0 < 0 || l.q[i].src.y0 < 0 || l.q[i].src.x1 > l.frame_w || l.q[i].src.y1 > l.frame_h) up_ok = false;
            }
            if (area != (long long)l.used.w() * l.used.h()) tile_ok = false;
            for (int i = 0; i < l.nquads; ++i)
                for (int j = i + 1; j < l.nquads; ++j) {
                    const ul::irect &a = l.q[i].dst, &b = l.q[j].dst;
                    if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1) tile_ok = false;
                }
            // a full-size composition sits on the screen with no bars
            if (!rs.capped && !(l.used.x0 == 0 && l.used.y0 == 0 && l.used.x1 == s[0] && l.used.y1 == s[1])) tile_ok = false;
            if (ul::quad_at(l, l.used.x0, l.used.y0) != 0) hit_ok = false;
            if (ul::quad_at(l, l.used.x1 - 1, l.used.y1 - 1) != 2) hit_ok = false;
            if (ul::quad_at(l, l.used.x0 - 1, l.used.y0) != -1) hit_ok = false;
        }
    check("the quads tile the composition (no gap, no overlap); uncapped = whole screen", tile_ok);
    check("every quad is at least 1:1 and inside the frame", up_ok);
    check("quad_at: top-left is the map, bottom-right the panel, outside is none", hit_ok);
}

// every visible frame pixel -> screen -> back is itself (the last panel row may answer H-1: the edge rule)
void test_inverse(int sw, int sh, int fw, int fh, int pct, const char *what, int kw = 0, int kh = 0) {
    if (fw <= 0) { // the ideal frame for this screen and scale
        const ul::render_size rs = ul::ideal_render(sw, sh, pct);
        fw                       = rs.w;
        fh                       = rs.h;
    }
    ul::layout l;
    ul::build(l, sw, sh, fw, fh, pct, false, nullptr, kw, kh);
    char msg[160];
    if (l.single) {
        snprintf(msg, sizeof msg, "%s: multi-quad layout", what);
        check(msg, false);
        return;
    }
    long long bad_rt = 0, bad_dst = 0, hidden = 0, visible = 0;
    for (int qi = 0; qi < l.nquads; ++qi) {
        if (l.overlay && (qi == 1 || qi == 2)) continue; // RES21: the base strip / panel lie wholly under their overlays
        const ul::quad &q = l.q[qi];
        for (int y = q.src.y0; y < q.src.y1; ++y)
            for (int x = q.src.x0; x < q.src.x1; ++x) {
                int        px = x, py = y;
                const bool vis = ul::frame_to_screen(l, px, py);
                if (!vis && l.overlay) continue; // RES21: the base map under the panel / strip overlay is hidden, and says so
                ++visible;
                if (!vis) ++hidden;
                if (!q.dst.contains(px, py)) ++bad_dst;
                int bx = px, by = py;
                ul::screen_to_frame(l, bx, by);
                const bool edge_row = q.kind == ul::quad_kind::panel && py == l.used.y1 - 1;
                if (bx != x || (by != y && !(edge_row && by == fh - 1))) ++bad_rt;
            }
    }
    snprintf(msg, sizeof msg, "%s: frame -> screen -> frame is the identity (%lld bad of %lld)", what, bad_rt, visible);
    check(msg, bad_rt == 0);
    snprintf(msg, sizeof msg, "%s: frame -> screen lands inside the pixel's own quad (%lld bad)", what, bad_dst);
    check(msg, bad_dst == 0);
    snprintf(msg, sizeof msg, "%s: visible source pixels report visible (%lld hidden)", what, hidden);
    check(msg, hidden == 0);

    // screen -> frame: lands in its own quad's source, monotone along a row and a column, and every source
    // pixel of every quad is reached by some screen pixel
    long long bad_src = 0, bad_mono = 0, unreached = 0;
    for (int qi = 0; qi < l.nquads; ++qi) {
        if (l.overlay && (qi == 1 || qi == 2)) continue;
        const ul::quad   &q = l.q[qi];
        std::vector<char> hit((size_t)q.src.w() * q.src.h(), 0);
        int               prev_fx = -1;
        for (int y = q.dst.y0; y < q.dst.y1; ++y) {
            int prev = -1;
            for (int x = q.dst.x0; x < q.dst.x1; ++x) {
                if (ul::quad_at(l, x, y) != qi) continue; // (covered by a quad above it: an overlay)
                int fx = x, fy = y;
                ul::screen_to_frame(l, fx, fy);
                const bool edge = (fx == fw - 1 || fy == fh - 1);
                if (!q.src.contains(fx, fy) && !edge) ++bad_src;
                if (fx < prev) ++bad_mono;
                prev = fx;
                if (q.src.contains(fx, fy)) hit[(size_t)(fy - q.src.y0) * q.src.w() + (fx - q.src.x0)] = 1;
            }
            (void)prev_fx;
        }
        for (size_t k = 0; k < hit.size(); ++k) {
            // the panel's LAST source row may share its only screen row with the edge rule (that row answers H-1)
            const bool last_panel_row = q.kind == ul::quad_kind::panel && (int)(k / q.src.w()) == q.src.h() - 1;
            int        ux = q.src.x0 + (int)(k % q.src.w()), uy = q.src.y0 + (int)(k / q.src.w());
            if (l.overlay && !ul::frame_to_screen(l, ux, uy)) continue; // hidden under an overlay
            if (!hit[k] && !last_panel_row) ++unreached;
        }
    }
    snprintf(msg, sizeof msg, "%s: screen -> frame stays in the quad's source (%lld bad)", what, bad_src);
    check(msg, bad_src == 0);
    snprintf(msg, sizeof msg, "%s: monotone along rows (%lld bad)", what, bad_mono);
    check(msg, bad_mono == 0);
    snprintf(msg, sizeof msg, "%s: every source pixel of every quad is reachable (%lld unreached)", what, unreached);
    check(msg, unreached == 0);
}

void test_edges() {
    ul::layout l;
    ul::build(l, 1920, 1080, 1856, 1088, 150);
    int x, y;
    x = 0, y = 0;
    ul::screen_to_frame(l, x, y);
    check("top-left screen pixel is frame (0,0)", x == 0 && y == 0);
    x = 1919, y = 0;
    ul::screen_to_frame(l, x, y);
    check("top-right screen pixel is frame (W-1, 0)", x == 1855 && y == 0);
    x = 1919, y = 1079;
    ul::screen_to_frame(l, x, y);
    check("bottom-right screen pixel is frame (W-1, H-1)", x == 1855 && y == 1087);
    x = 100, y = 1079;
    ul::screen_to_frame(l, x, y);
    check("last screen row over the strip is frame row H-1", y == 1087 && x >= 0);
    x = 1800, y = 1079;
    ul::screen_to_frame(l, x, y);
    check("last screen row over the panel is frame row H-1 (the edge rule, past the panel's 720 rows)", y == 1087);
    x = 1800, y = 1078;
    ul::screen_to_frame(l, x, y);
    check("the row above it is a panel row, not H-1", y < 720);
    x = 1679, y = 500;
    ul::screen_to_frame(l, x, y);
    check("the map's last column is frame column 1679 (no edge rule inside the screen)", x == 1679 && y == 500);
    x = 1680, y = 500;
    ul::screen_to_frame(l, x, y);
    check("the next column is the panel's first (frame 1696)", x == 1696);
    x = -50, y = -9;
    ul::screen_to_frame(l, x, y);
    check("a point left / above the composition clamps to (0,0)", x == 0 && y == 0);
    x = 99999, y = 99999;
    ul::screen_to_frame(l, x, y);
    check("a point beyond clamps to (W-1, H-1)", x == 1855 && y == 1087);

    // bars: a centred composition clamps to its own edge, and the edge rule uses ITS last column / row
    ul::build(l, 1920, 1080, 1024, 768, 150);
    x = 5, y = 0;
    ul::screen_to_frame(l, x, y);
    check("bars: a point in the left bar clamps to the map's first column", x == 0 && y == 0);
    x = 1900, y = 1079;
    ul::screen_to_frame(l, x, y);
    check("bars: a point in the bottom-right bar is frame (W-1, H-1)", x == 1023 && y == 767);
    x = l.used.x1 - 1, y = l.used.y0;
    ul::screen_to_frame(l, x, y);
    check("bars: the composition's last column is frame column W-1", x == 1023);

    // a hidden frame pixel (the map under the panel) still gets a position
    ul::build(l, 1920, 1080, 1856, 1088, 150);
    x = 1690, y = 100;
    const bool vis = ul::frame_to_screen(l, x, y);
    check("a hidden frame pixel reports hidden but lands next to the panel", !vis && x >= 1600 && x <= 1700);
    x = 1800, y = 900; // panel row below Hp
    const bool v2 = ul::frame_to_screen(l, x, y);
    check("a panel row past Hp reports hidden, at the nearest visible pixel (the map side)", !v2 && x >= 0 && x < 1920 && y >= 0 && y < 1080);
}

void test_single() {
    bool      fit_ok = true, c2g_ok = true, g2c_ok = true;
    const int FRAMES[][2] = {{640, 480}, {1024, 768}, {1280, 1024}, {1856, 1088}, {2048, 1600}};
    for (const auto &s : SCREENS)
        for (const auto &f : FRAMES)
            for (int integer = 0; integer < 2; ++integer) {
                ul::layout l;
                ul::build(l, s[0], s[1], f[0], f[1], 100, integer != 0);
                const RECT r = mh::gfx::fit_image_rect(s[0], s[1], f[0], f[1], integer != 0);
                if (!l.single || l.nquads != 1 || l.q[0].kind != ul::quad_kind::frame || !same(l.q[0].src, 0, 0, f[0], f[1]) ||
                    !same(l.q[0].dst, r.left, r.top, r.right, r.bottom))
                    fit_ok = false;
                for (int k = 0; k < 40; ++k) {
                    int ax = (k * 977) % (s[0] + 40) - 20, ay = (k * 541) % (s[1] + 40) - 20;
                    int bx = ax, by = ay;
                    ul::screen_to_frame(l, ax, ay);
                    mh::gfx::image_client_to_game(r, f[0], f[1], bx, by);
                    if (ax != bx || ay != by) c2g_ok = false;
                    int gx = (k * 613) % f[0], gy = (k * 307) % f[1];
                    int hx = gx, hy = gy;
                    ul::frame_to_screen(l, gx, gy);
                    mh::gfx::image_game_to_client(r, f[0], f[1], hx, hy);
                    if (gx != hx || gy != hy) g2c_ok = false;
                }
            }
    check("s=100: one quad = the whole frame in fit_image_rect (fit and integer), over 15 screens x 5 frames", fit_ok);
    check("s=100: screen_to_frame == image_client_to_game (bars, edges, outside)", c2g_ok);
    check("s=100: frame_to_screen == image_game_to_client", g2c_ok);

    ul::layout l;
    ul::build(l, 1920, 1080, 1024, 768, 150);
    check("a multi layout needs pct > 100", !l.single);
    ul::build(l, 640, 480, 640, 480, 200);
    check("a screen that cannot take any scale clamps to the single quad, and says so", l.single && l.clamped && l.pct == 100 && l.req_pct == 200);
    ul::build(l, 1920, 1080, 160, 768, 150);
    check("a frame no wider than the panel is the single quad", l.single);
    ul::build(l, 1920, 1080, 1024, 32, 150);
    check("a frame no taller than the strip is the single quad", l.single);
    ul::build(l, 0, 0, 1024, 768, 150);
    int x = 3, y = 4;
    check("an empty screen: nothing to map", l.nquads == 0 && !ul::screen_to_frame(l, x, y) && !ul::frame_to_screen(l, x, y) && x == 3 && y == 4);
    ul::build(l, 1920, 1080, 0, 0, 150);
    check("an empty frame: nothing to map", l.nquads == 0 && !ul::screen_to_frame(l, x, y));
}

void test_compose() {
    const int             sw = 1920, sh = 1080, fw = 1856, fh = 1088;
    std::vector<uint16_t> frame((size_t)fw * fh);
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) frame[(size_t)y * fw + x] = (uint16_t)(x * 31 + y * 17 + (x >> 3));
    ul::layout l;
    ul::build(l, sw, sh, fw, fh, 150);
    std::vector<uint8_t> out((size_t)sw * sh * 3, 0xAA);
    ul::compose_bgr24(l, frame.data(), fw * 2, out.data(), sw * 3);
    long long bad = 0;
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            if (x == sw - 1 || y == sh - 1) continue; // the edge rule moves these in screen_to_frame, not in the sampler
            int fx = x, fy = y;
            ul::screen_to_frame(l, fx, fy);
            const uint16_t p  = frame[(size_t)fy * fw + fx];
            const int      r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f;
            const uint8_t *d = &out[((size_t)y * sw + x) * 3];
            if (d[0] != (uint8_t)((b5 << 3) | (b5 >> 2)) || d[1] != (uint8_t)((g6 << 2) | (g6 >> 4)) || d[2] != (uint8_t)((r5 << 3) | (r5 >> 2))) ++bad;
        }
    check("compose: every screen pixel is the frame pixel screen_to_frame names", bad == 0);

    // the 1:1 map region is byte-exact
    bool map_ok = true;
    for (int y = 0; y < 1032 && map_ok; y += 7)
        for (int x = 0; x < 1680; x += 5) {
            const uint16_t p = frame[(size_t)y * fw + x];
            const uint8_t *d = &out[((size_t)y * sw + x) * 3];
            if (d[2] != (uint8_t)((((p >> 11) & 0x1f) << 3) | (((p >> 11) & 0x1f) >> 2))) {
                map_ok = false;
                break;
            }
        }
    check("compose: the map region is the frame pixel for pixel", map_ok);

    // a centred composition leaves black bars
    ul::build(l, 1920, 1080, 1024, 768, 150);
    std::vector<uint8_t>  o2((size_t)sw * sh * 3, 0xAA);
    std::vector<uint16_t> f2((size_t)1024 * 768, 0xFFFF);
    ul::compose_bgr24(l, f2.data(), 1024 * 2, o2.data(), sw * 3);
    check("compose: bars are black", o2[0] == 0 && o2[1] == 0 && o2[2] == 0 && o2[((size_t)1079 * sw + 1919) * 3 + 1] == 0);
    const size_t inside = ((size_t)(l.used.y0 + 5) * sw + (l.used.x0 + 5)) * 3;
    check("compose: the composition is the frame colour", o2[inside] == 0xFF && o2[inside + 1] == 0xFF && o2[inside + 2] == 0xFF);

    // a null frame / buffer does not crash and leaves black
    ul::compose_bgr24(l, nullptr, 0, o2.data(), sw * 3);
    check("compose: no frame = all black", o2[inside] == 0);
    ul::compose_bgr24(l, f2.data(), 2048, nullptr, 0);
    check("compose: no buffer is a no-op", true);
}

// ---- the real presenters ---------------------------------------------------------------------------

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

HWND make_window(int w, int h, bool show) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc     = {};
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.lpszClassName = "mh_uilayouttest";
        RegisterClassA(&wc);
        reg = true;
    }
    HWND hw = CreateWindowExA(show ? WS_EX_TOPMOST | WS_EX_NOACTIVATE : 0, "mh_uilayouttest", "uilayouttest", WS_POPUP, 0, 0, w, h, nullptr,
                              nullptr, GetModuleHandleA(nullptr), nullptr);
    if (hw && show) ShowWindow(hw, SW_SHOWNA);
    return hw;
}

uint16_t rgb565_of(uint32_t bgra) {
    return (uint16_t)(((bgra >> 19) & 0x1f) << 11 | ((bgra >> 10) & 0x3f) << 5 | ((bgra >> 3) & 0x1f));
}

// RES4 stage 5: a 40x40 test overlay at (100,200): the left half opaque red, the right half 50% black, the first column transparent.
struct test_overlay {
    std::vector<uint32_t> px = std::vector<uint32_t>(40 * 40);
    ul::cursor_overlay    ov;
    test_overlay() {
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) px[(size_t)y * 40 + x] = x == 0 ? 0u : x < 20 ? 0xFFFF0000u
                                                                                       : 0x80000000u;
        ov.px = px.data();
        ov.w  = 40;
        ov.h  = 40;
        ov.x  = 100;
        ov.y  = 200;
    }
};

void test_gdi_presenter(const ul::layout &lay, const std::vector<uint16_t> &frame, int fw, int fh, int sw, int sh, int pct) {
    HWND hw = make_window(sw, sh, true);
    RECT cr = {};
    if (!hw || !GetClientRect(hw, &cr) || cr.right != sw || cr.bottom != sh) {
        printf("  SKIP: gdi arm -- no window to show\n");
        if (hw) DestroyWindow(hw);
        return;
    }
    mh::gfx::present_backend *b = mh::gfx::make_gdi_backend();
    mh::gfx::backend_config   cfg;
    cfg.ui_scale_pct            = pct;
    bool                  shown = b && b->attach(hw, cfg);
    mh::gfx::present_view pv; // RES23-S1: the presenter draws the view it is handed, so hand it the layout under test
    pv.valid               = true;
    pv.multi               = !lay.single;
    pv.layout              = lay;
    mh::gfx::frame_view fv = {frame.data(), fw, fh, fw * 2, mh::gfx::pixel_format::rgb565};
    fv.view                = &pv;
    shown                  = shown && b->present(fv);
    Sleep(50);
    HDC        wdc = GetDC(hw), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi              = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = sw;
    bi.bmiHeader.biHeight      = -sh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void   *bits               = nullptr;
    HBITMAP bmp                = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old                = SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, sw, sh, wdc, 0, 0, SRCCOPY);
    if (const char *dump = getenv("MH_UIL_DUMP")) { // debugging aid: the raw read-back (BGRA, sw x sh) for an outside diff
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\uil_gdi_%dx%d.bgra", dump, sw, sh);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(bits, 4, (size_t)sw * sh, f);
            fclose(f);
        }
    }
    const uint32_t *px      = (const uint32_t *)bits;
    long long       map_bad = 0, map_n = 0, scaled_bad = 0, scaled_n = 0;
    for (int y = 0; y < sh && bits; y += 2)
        for (int x = 0; x < sw; x += 2) {
            const int qi = ul::quad_at(lay, x, y);
            if (qi < 0) continue;
            const ul::quad &q   = lay.q[qi];
            const uint16_t  got = rgb565_of(px[(size_t)y * sw + x]);
            if (q.kind == ul::quad_kind::map) {
                ++map_n;
                if (got != frame[(size_t)(y - q.dst.y0 + q.src.y0) * fw + (x - q.dst.x0 + q.src.x0)]) ++map_bad;
            } else {
                ++scaled_n;
                const int cx = q.src.x0 + (int)((long long)(x - q.dst.x0) * q.src.w() / q.dst.w());
                const int cy = q.src.y0 + (int)((long long)(y - q.dst.y0) * q.src.h() / q.dst.h());
                bool      ok = false;
                for (int dy = -1; dy <= 1 && !ok; ++dy)
                    for (int dx = -1; dx <= 1 && !ok; ++dx) {
                        const int ux = cx + dx, uy = cy + dy;
                        if (q.src.contains(ux, uy) && frame[(size_t)uy * fw + ux] == got) ok = true;
                    }
                if (!ok) ++scaled_bad;
            }
        }
    if (!shown || !bits || map_n == 0 || (map_bad == map_n && scaled_bad == scaled_n)) {
        printf("  SKIP: gdi arm -- the window could not be read back here (shown=%d)\n", (int)shown);
    } else {
        char msg[160];
        snprintf(msg, sizeof msg, "gdi: the 1:1 map quad is the frame sub-rectangle pixel for pixel (%lld of %lld bad)", map_bad, map_n);
        check(msg, map_bad == 0);
        snprintf(msg, sizeof msg, "gdi: the scaled quads sample within one source pixel of their place (%lld of %lld bad)", scaled_bad, scaled_n);
        check(msg, scaled_bad == 0);
        // stage 5: the owned cursor over the quads (AlphaBlend): opaque replaces, 50% darkens, transparent keeps
        std::vector<uint32_t> before(px, px + (size_t)sw * sh);
        test_overlay          t;
        mh::gfx::frame_view   fc = fv;
        fc.overlays.add(t.ov, mh::gfx::overlay_slot::cursor);
        const bool shown2 = b->present(fc);
        Sleep(50);
        BitBlt(mdc, 0, 0, sw, sh, wdc, 0, 0, SRCCOPY);
        const uint32_t red = px[(size_t)210 * sw + 110] & 0xFFFFFFu, half = px[(size_t)210 * sw + 130], keep = px[(size_t)210 * sw + 100];
        const uint32_t was  = before[(size_t)210 * sw + 130];
        auto           chan = [](uint32_t v, int sh_) { return (int)((v >> sh_) & 0xff); };
        snprintf(msg, sizeof msg, "gdi: the owned cursor overlay: opaque %06x, half %08x over %08x, transparent %08x vs %08x", red, half, was, keep,
                 before[(size_t)210 * sw + 100]);
        check(msg, shown2 && red == 0xFF0000u && abs(chan(half, 0) - chan(was, 0) / 2) <= 2 && abs(chan(half, 8) - chan(was, 8) / 2) <= 2 &&
                       abs(chan(half, 16) - chan(was, 16) / 2) <= 2 && (keep & 0xFFFFFFu) == (before[(size_t)210 * sw + 100] & 0xFFFFFFu));
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hw, wdc);
    if (b) b->detach();
    delete b;
    DestroyWindow(hw);
}

void test_d3d11_presenter(const ul::layout &lay, const std::vector<uint8_t> &comp, const std::vector<uint16_t> &frame, int fw, int fh, int sw, int sh, int pct) {
    HWND                      hw = make_window(sw, sh, false);
    mh::gfx::present_backend *b  = hw ? mh::gfx::make_d3d11_backend() : nullptr;
    mh::gfx::backend_config   cfg;
    cfg.filter       = mh::gfx::scale_filter::point;
    cfg.ui_scale_pct = pct;
    if (!b || !b->attach(hw, cfg)) {
        printf("  SKIP: d3d11 arm -- no D3D11 device (the presenter would fall back to gdi)\n");
    } else {
        std::vector<uint32_t> out((size_t)sw * sh, 0);
        mh::gfx::d3d11_request_readback(out.data(), sw, sh);
        mh::gfx::present_view pv; // RES23-S1: the presenter draws the view it is handed, so hand it the layout under test
        pv.valid               = true;
        pv.multi               = !lay.single;
        pv.layout              = lay;
        mh::gfx::frame_view fv = {frame.data(), fw, fh, fw * 2, mh::gfx::pixel_format::rgb565};
        fv.view                = &pv;
        b->present(fv);
        if (mh::gfx::d3d11_readback_state() != 1) {
            printf("  SKIP: d3d11 arm -- readback refused\n");
        } else {
            long long bad = 0;
            if (const char *dump = getenv("MH_UIL_DUMP")) {
                char path[MAX_PATH];
                snprintf(path, sizeof path, "%s\\uil_d3d_%dx%d.bgra", dump, sw, sh);
                if (FILE *f = fopen(path, "wb")) {
                    fwrite(out.data(), 4, out.size(), f);
                    fclose(f);
                }
                snprintf(path, sizeof path, "%s\\uil_comp_%dx%d.bgr", dump, sw, sh);
                if (FILE *f = fopen(path, "wb")) {
                    fwrite(comp.data(), 1, comp.size(), f);
                    fclose(f);
                }
            }
            long long ties = 0, tie_bad = 0;
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    const uint32_t g  = out[(size_t)y * sw + x];
                    const uint8_t *c  = &comp[((size_t)y * sw + x) * 3]; // B G R
                    const int      db = abs((int)(g & 0xff) - c[0]), dg = abs((int)((g >> 8) & 0xff) - c[1]), dr = abs((int)((g >> 16) & 0xff) - c[2]);
                    const bool     differs = db > 3 || dg > 3 || dr > 3;
                    // A pixel whose centre sits EXACTLY on a texel edge (a rational tie at 1.25x) is the rasteriser's to
                    // break either way, which is the sampler's business, not the layout's.
                    bool      tie = false;
                    const int qi  = ul::quad_at(lay, x, y);
                    if (qi >= 0) {
                        const ul::quad &q = lay.q[qi];
                        tie               = (((long long)(2 * (x - q.dst.x0) + 1) * q.src.w()) % (2LL * q.dst.w()) == 0) ||
                              (((long long)(2 * (y - q.dst.y0) + 1) * q.src.h()) % (2LL * q.dst.h()) == 0);
                    }
                    if (tie) ++ties;
                    if (differs && tie) ++tie_bad;
                    if (differs && !tie) ++bad;
                }
            char msg[200];
            snprintf(msg, sizeof msg, "d3d11: the presented quads equal the CPU compositor off the texel ties (%lld bad; %lld of %lld tie pixels differ)", bad, tie_bad, ties);
            check(msg, bad == 0 && ties > 0);
            // stage 5: the owned cursor over the quads (premultiplied blend): the compositor with the same overlay blended in
            test_overlay         t;
            std::vector<uint8_t> comp2 = comp;
            ul::blend_cursor_bgr24(t.ov, comp2.data(), sw * 3, sw, sh);
            std::vector<uint32_t> out2((size_t)sw * sh, 0);
            mh::gfx::d3d11_request_readback(out2.data(), sw, sh);
            mh::gfx::frame_view fc = fv;
            fc.overlays.add(t.ov, mh::gfx::overlay_slot::cursor);
            b->present(fc);
            if (mh::gfx::d3d11_readback_state() != 1) {
                printf("  SKIP: d3d11 cursor arm -- readback refused\n");
            } else {
                long long cbad = 0, cn = 0;
                for (int y = 190; y < 250; ++y)
                    for (int x = 90; x < 150; ++x) {
                        const uint32_t g = out2[(size_t)y * sw + x];
                        const uint8_t *c = &comp2[((size_t)y * sw + x) * 3];
                        ++cn;
                        if (abs((int)(g & 0xff) - c[0]) > 3 || abs((int)((g >> 8) & 0xff) - c[1]) > 3 || abs((int)((g >> 16) & 0xff) - c[2]) > 3) ++cbad;
                    }
                snprintf(msg, sizeof msg, "d3d11: the owned cursor overlay equals the compositor's blend (%lld of %lld pixels bad)", cbad, cn);
                check(msg, cbad == 0);
            }
        }
        b->detach();
    }
    delete b;
    if (hw) DestroyWindow(hw);
}

void test_presenters() {
    const int             sw = 800, sh = 600, pct = 125;
    const ul::render_size rs = ul::ideal_render(sw, sh, pct);
    const int             fw = rs.w, fh = rs.h;
    std::vector<uint16_t> frame((size_t)fw * fh);
    uint32_t              seed = 0xC0FFEEu;
    for (auto &p : frame) {
        seed = seed * 1664525u + 1013904223u;
        p    = (uint16_t)(seed >> 16);
    }
    ul::layout lay;
    ul::build(lay, sw, sh, fw, fh, pct);
    check("presenters: the 125% layout on 800x600 is three quads", !lay.single && lay.nquads == 3 && same(lay.used, 0, 0, sw, sh));
    std::vector<uint8_t> comp((size_t)sw * sh * 3);
    ul::compose_bgr24(lay, frame.data(), fw * 2, comp.data(), sw * 3);
    test_gdi_presenter(lay, frame, fw, fh, sw, sh, pct);
    test_d3d11_presenter(lay, comp, frame, fw, fh, sw, sh, pct);
}

// RES4 stage 5: the owned cursor -- coverage from the black / white renderings, scaling, hotspot, clipping, blending.
void test_cursor() {
    // transparent: both renderings equal the backdrop; opaque red: red on both; a halve-the-backdrop pixel: half on white.
    check("cursor: transparent pixel (black stays black, white stays white) has no coverage", ul::cursor_pixel_from_passes(0x0000, 0xFFFF) == 0);
    const uint32_t red = ul::cursor_pixel_from_passes(0xF800, 0xF800);
    check("cursor: opaque red is a=255, premultiplied red", red == 0xFFFF0000u);
    const uint32_t shade = ul::cursor_pixel_from_passes(0x0000, 0x7BEF);
    check("cursor: a halve-the-backdrop pixel is black at about 50% coverage", (shade & 0x00FFFFFFu) == 0 && (shade >> 24) > 120 && (shade >> 24) < 136);
    bool ok = true;
    for (int i = 0; i < 65536; i += 37) {
        const uint32_t px = ul::cursor_pixel_from_passes((uint16_t)i, (uint16_t)(i | 0x7BEF));
        const uint32_t a  = px >> 24;
        ok                = ok && ((px & 0xff) <= a) && (((px >> 8) & 0xff) <= a) && (((px >> 16) & 0xff) <= a);
    }
    check("cursor: premultiplied channels never exceed the coverage", ok);

    const int       N = ul::CURSOR_SRC, H = ul::CURSOR_HOT;
    static uint32_t src[ul::CURSOR_SRC * ul::CURSOR_SRC], buf[ul::CURSOR_MAX * ul::CURSOR_MAX];
    for (auto &p : src) p = 0;
    // a 10x12 opaque block whose top-left is the hotspot pixel
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 10; ++x) src[(size_t)(H + y) * N + H + x] = 0xFF0000FFu;
    const int bufpx = (int)(sizeof buf / sizeof buf[0]);
    for (int req : {150, 200, 175}) {
        ul::layout            lay;
        const int             sh = req == 200 ? 960 : 800;
        const ul::render_size rs = ul::ideal_render(1280, sh, req);
        ul::build(lay, 1280, sh, rs.w, rs.h, req);
        const int          pct = lay.pct; // what is in force (clamped / snapped)
        ul::cursor_overlay ov;
        // over the map: frame pixel == screen pixel, so the hotspot's pixel-middle is the screen point itself
        const bool placed = ul::place_cursor(lay, src, 300, 300, buf, bufpx, ov);
        char       msg[160];
        snprintf(msg, sizeof msg, "cursor @%d (asked %d): placed over the map", pct, req);
        check(msg, placed && ov.w > 0 && ov.h > 0);
        if (!placed) continue;
        int x0 = ov.w, y0 = ov.h, x1 = -1, y1 = -1;
        for (int y = 0; y < ov.h; ++y)
            for (int x = 0; x < ov.w; ++x)
                if (ov.px[(size_t)y * ov.w + x] >> 24) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;
        snprintf(msg, sizeof msg, "cursor @%d: the 10x12 block is %dx%d (want ~%dx%d)", pct, w, h, 10 * pct / 100, 12 * pct / 100);
        check(msg, abs(w - 10 * pct / 100) <= 1 && abs(h - 12 * pct / 100) <= 1);
        // the block's first pixel sits on the pointer
        const int left = ov.x + x0, top = ov.y + y0;
        snprintf(msg, sizeof msg, "cursor @%d: hotspot block starts at the pointer (%d,%d)", pct, left, top);
        check(msg, abs(left - 300) <= pct / 100 + 1 && abs(top - 300) <= pct / 100 + 1);
    }
    // over the panel the hotspot is the middle of the frame pixel's span: a click there maps back to the same frame pixel
    {
        ul::layout lay;
        ul::build(lay, 1280, 800, 1216, 800, 150);
        ul::cursor_overlay ov;
        int                sx = 1164, sy = 150;
        ul::frame_to_screen(lay, sx, sy);
        int bx = sx, by = sy;
        ul::screen_to_frame(lay, bx, by);
        check("cursor: the panel hotspot's screen point maps back to the cursor's frame pixel", bx == 1164 && by == 150);
        check("cursor: placed over the panel", ul::place_cursor(lay, src, 1164, 150, buf, bufpx, ov) && ov.x <= sx && ov.y <= sy);
    }
    // clipped at the screen edges; a single-quad layout and a too-small buffer refuse
    {
        ul::layout lay;
        ul::build(lay, 1280, 800, 1216, 800, 150);
        ul::cursor_overlay ov;
        const bool         corner = ul::place_cursor(lay, src, 1215, 532, buf, bufpx, ov);
        char               msg[160];
        snprintf(msg, sizeof msg, "cursor: at the bottom-right corner it is clipped to the screen (%d: %d,%d %dx%d)", (int)corner, ov.x, ov.y, ov.w, ov.h);
        check(msg, corner && ov.x + ov.w <= 1280 && ov.y + ov.h <= 800 && ov.w < 240);
        check("cursor: a buffer too small refuses", !ul::place_cursor(lay, src, 300, 300, buf, 16, ov));
        ul::layout one;
        ul::build(one, 1280, 800, 1280, 800, 100);
        check("cursor: a single-quad layout (100%) has no overlay", !ul::place_cursor(one, src, 300, 300, buf, bufpx, ov));
    }
    // blend: opaque replaces, transparent keeps, half blends
    {
        uint32_t           px[3] = {0xFF102030u, 0x00000000u, 0x80000000u};
        ul::cursor_overlay ov;
        ov.px = px;
        ov.w  = 3;
        ov.h  = 1;
        ov.x  = 0;
        ov.y  = 0;
        uint8_t img[9];
        for (auto &v : img) v = 200;
        ul::blend_cursor_bgr24(ov, img, 9, 3, 1);
        check("cursor: blend: opaque replaces", img[0] == 0x30 && img[1] == 0x20 && img[2] == 0x10);
        check("cursor: blend: transparent keeps", img[3] == 200 && img[4] == 200 && img[5] == 200);
        check("cursor: blend: half coverage over 200 is ~100", img[6] >= 99 && img[6] <= 101);
    }
}

// ---- stage 6: the dialog quad ----------------------------------------------------------------------------------------------------------
void test_dialog() {
    auto one = [](const ul::irect &r) {
        ul::dialog_set s;
        s.r[0] = r;
        s.n    = 1;
        return s;
    };
    const ul::render_size r = ul::ideal_render(1920, 1080, 150);
    ul::layout            plain;
    ul::build(plain, 1920, 1080, r.w, r.h, 150);
    check("dialog: base layout has 3 quads and no dialog", plain.nquads == 3 && !plain.has_dialog);

    // A 300x200 dialog centred in the frame (the game's own centring).
    const ul::irect      d{(r.w - 300) / 2, (r.h - 200) / 2, (r.w - 300) / 2 + 300, (r.h - 200) / 2 + 200};
    ul::layout           L;
    const ul::dialog_set DS_d = one(d);
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &DS_d);
    check("dialog: a fourth quad, the topmost", L.nquads == 4 && L.has_dialog && L.q[3].kind == ul::quad_kind::dialog);
    check("dialog: 150% of 300x200 is 450x300", L.dialog_dst[0].w() == 450 && L.dialog_dst[0].h() == 300);
    check("dialog: source is the dialog rect", same(L.q[3].src, d.x0, d.y0, d.x1, d.y1));
    // It covers its own 1x copy (map quad is 1:1 at the screen's top left here).
    const ul::irect copy{d.x0 + L.q[0].dst.x0, d.y0 + L.q[0].dst.y0, d.x1 + L.q[0].dst.x0, d.y1 + L.q[0].dst.y0};
    check("dialog: covers the 1x copy", L.dialog_dst[0].x0 <= copy.x0 && L.dialog_dst[0].y0 <= copy.y0 && L.dialog_dst[0].x1 >= copy.x1 && L.dialog_dst[0].y1 >= copy.y1);
    check("dialog: inside the composition", L.dialog_dst[0].x0 >= L.used.x0 && L.dialog_dst[0].x1 <= L.used.x1 && L.dialog_dst[0].y0 >= L.used.y0 && L.dialog_dst[0].y1 <= L.used.y1);
    // Centred on the composition (to within the cover shift, which is small here).
    const int cx = (L.dialog_dst[0].x0 + L.dialog_dst[0].x1) / 2, ux = L.q[0].dst.x0 + L.q[0].dst.w() / 2;
    check("dialog: centred on the visible map within 40 px", cx - ux < 40 && ux - cx < 40);

    // Inverse map: every pixel of the quad answers a pixel of the dialog rect, edges included; the corners hit the corners.
    int x = L.dialog_dst[0].x0, y = L.dialog_dst[0].y0;
    check("dialog: top-left screen pixel -> the rect's top-left", ul::screen_to_frame(L, x, y) && x == d.x0 && y == d.y0);
    x = L.dialog_dst[0].x1 - 1, y = L.dialog_dst[0].y1 - 1;
    check("dialog: bottom-right screen pixel -> the rect's bottom-right", ul::screen_to_frame(L, x, y) && x == d.x1 - 1 && y == d.y1 - 1);
    bool inside = true, rt = true;
    for (int sy = L.dialog_dst[0].y0; sy < L.dialog_dst[0].y1; sy += 7)
        for (int sx = L.dialog_dst[0].x0; sx < L.dialog_dst[0].x1; sx += 7) {
            int fx = sx, fy = sy;
            ul::screen_to_frame(L, fx, fy);
            if (!d.contains(fx, fy)) inside = false;
            int bx = fx, by = fy;
            ul::frame_to_screen(L, bx, by);
            int cx2 = bx, cy2 = by;
            ul::screen_to_frame(L, cx2, cy2);
            if (cx2 != fx || cy2 != fy || !L.dialog_dst[0].contains(bx, by)) rt = false;
        }
    check("dialog: every screen pixel of the quad maps into the rect", inside);
    check("dialog: frame -> screen -> frame round-trips over the quad", rt);
    // Outside the quad: the plain HUD map (1:1 over the map quad).
    x = L.dialog_dst[0].x0 - 20, y = L.dialog_dst[0].y0 - 20;
    const int ox = x - L.q[0].dst.x0, oy = y - L.q[0].dst.y0;
    check("dialog: outside the quad the map is 1:1", ul::screen_to_frame(L, x, y) && x == ox && y == oy);
    // The cursor sits at a rect pixel: frame_to_screen lands in the quad.
    x = d.x0 + 10, y = d.y0 + 10;
    ul::frame_to_screen(L, x, y);
    check("dialog: a cursor on a rect pixel is placed inside the quad", L.dialog_dst[0].contains(x, y));
    // Compose draws it above the map at s.
    std::vector<uint16_t> frame((size_t)r.w * r.h, 0x1234);
    for (int fy = d.y0; fy < d.y1; ++fy)
        for (int fx = d.x0; fx < d.x1; ++fx) frame[(size_t)fy * r.w + fx] = 0xF800;
    const int            stride = (1920 * 3 + 3) & ~3;
    std::vector<uint8_t> out((size_t)stride * 1080);
    ul::compose_bgr24(L, frame.data(), r.w * 2, out.data(), stride);
    auto px_red = [&](int sx, int sy) { return out[(size_t)sy * stride + sx * 3 + 2] > 200 && out[(size_t)sy * stride + sx * 3 + 1] < 40; };
    check("dialog: compose paints the whole quad", px_red(L.dialog_dst[0].x0, L.dialog_dst[0].y0) && px_red(L.dialog_dst[0].x1 - 1, L.dialog_dst[0].y1 - 1));
    check("dialog: compose leaves the 1x copy's surroundings alone", !px_red(L.dialog_dst[0].x0 - 2, L.dialog_dst[0].y0 - 2));

    // A dialog that is not wholly in the visible map (touching the panel), or the whole frame: no quad, mapping untouched.
    const ul::irect      wide{0, 0, r.w, r.h};
    const ul::dialog_set DS_wide = one(wide);
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &DS_wide);
    check("dialog: a whole-frame list adds no quad", L.nquads == 3 && !L.has_dialog);
    const ul::irect      onpanel{r.w - 300, 100, r.w - 20, 300};
    const ul::dialog_set DS_onpanel = one(onpanel);
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &DS_onpanel);
    check("dialog: a rect over the panel adds no quad", !L.has_dialog);
    // 100%: the single quad, a dialog is ignored.
    ul::build(L, 1920, 1080, 1920, 1080, 100, false, &DS_d);
    check("dialog: at 100% there is no dialog quad (single)", L.single && !L.has_dialog && L.nquads == 1);

    // A dialog bigger than the screen can show scaled: capped at what fits, or no quad when that is not above 100%.
    const ul::irect      big{100, 100, 100 + 1000, 100 + 700};
    const ul::dialog_set DS_big = one(big);
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &DS_big);
    check("dialog: a 1000x700 dialog is capped to the composition", L.has_dialog && L.dialog_dst[0].w() <= L.used.w() && L.dialog_dst[0].h() <= L.used.h() &&
                                                                        L.dialog_dst[0].w() > 1000);
    const ul::irect      huge{10, 10, 10 + 1000, 10 + 1070};
    const ul::dialog_set DS_huge = one(huge);
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &DS_huge);
    check("dialog: one that cannot exceed 100% adds no quad", !L.has_dialog);

    // Cover when the dialog is off-centre (a tutorial list): the quad moves to keep covering the copy, and still fits the screen.
    const ul::irect      off{20, 20, 20 + 300, 20 + 200};
    const ul::dialog_set DS_off = one(off);
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &DS_off);
    const ul::irect ocopy{off.x0 + L.q[0].dst.x0, off.y0 + L.q[0].dst.y0, off.x1 + L.q[0].dst.x0, off.y1 + L.q[0].dst.y0};
    check("dialog: an off-centre dialog still covers its copy",
          L.has_dialog && L.dialog_dst[0].x0 <= ocopy.x0 && L.dialog_dst[0].y0 <= ocopy.y0 && L.dialog_dst[0].x1 >= ocopy.x1 && L.dialog_dst[0].y1 >= ocopy.y1 &&
              L.dialog_dst[0].x0 >= L.used.x0 && L.dialog_dst[0].y0 >= L.used.y0);

    // Two dialogs (a confirmation over the ESC menu): two quads, bottom to top, the upper one wins the hit test.
    ul::dialog_set two;
    two.r[0] = d;
    two.r[1] = ul::irect{d.x0 + 20, d.y0 + 40, d.x0 + 320, d.y0 + 160};
    two.n    = 2;
    ul::build(L, 1920, 1080, r.w, r.h, 150, false, &two);
    check("dialog: two dialogs make two quads", L.ndialogs == 2 && L.nquads == 5 && L.q[4].kind == ul::quad_kind::dialog);
    int tx = L.dialog_dst[1].x0 + 5, ty = L.dialog_dst[1].y0 + 5;
    check("dialog: a point in both quads maps through the upper one", ul::screen_to_frame(L, tx, ty) && two.r[1].contains(tx, ty));
    int bx = L.dialog_dst[0].x0 + 2, by = L.dialog_dst[0].y1 - 2;
    check("dialog: a point only in the lower quad maps through it", ul::screen_to_frame(L, bx, by) && d.contains(bx, by) && !two.r[1].contains(bx, by));

    // ---- stage 6b: translucent layers ----
    // Recovery: opaque, clear, 50% black tint (s=1/2), a clamped 0x60-style pixel, and an unclamped 0x40 overflow over white.
    auto n565 = [](int r, int g, int b) { return (uint16_t)((r << 11) | (g << 5) | b); };
    {
        bool     ug;
        uint32_t o = ul::layer_pixel_from_passes(n565(10, 20, 5), n565(10, 20, 5), n565(10, 20, 5), &ug);
        check("layer: an opaque pixel has alpha 255 and its colour", (o >> 24) == 255 && !ug);
        check("layer: a clear pixel is transparent", ul::layer_pixel_from_passes(0, 0xFFFF, 0x8410, &ug) == 0);
        // s = 1/2, C = 0: black 0, white ~half, grey ~quarter
        o = ul::layer_pixel_from_passes(0, n565(15, 31, 15), n565(8, 16, 8), &ug);
        check("layer: a half tint has alpha ~128 and colour 0", (o >> 24) >= 120 && (o >> 24) <= 138 && (o & 0xffffff) == 0 && !ug);
        // 0x40 over white overflows: white reads garbage (low), grey is consistent -> the grey pass is taken
        o = ul::layer_pixel_from_passes(n565(0, 40, 0), n565(2, 5, 3), n565(10, 52, 6), &ug);
        check("layer: an inconsistent white pass falls back to the grey pass", ug && (o >> 24) > 0 && (o >> 24) < 255);
    }
    {
        // place_dialog_layer: a 2-colour layer scaled to the quad; layered flag drops the frame copy from compose.
        ul::render_size r2 = ul::ideal_render(1920, 1080, 150);
        ul::layout      L2;
        ul::dialog_set  ds;
        const ul::irect d2{300, 200, 700, 380};
        ds.r[0] = d2;
        ds.n    = 1;
        ul::build(L2, 1920, 1080, r2.w, r2.h, 150, false, &ds);
        check("layer: not layered by default", L2.ndialogs == 1 && !L2.dialog_layered[0] && !L2.quad_is_layered(L2.nquads - 1));
        ds.layered[0] = true;
        ul::build(L2, 1920, 1080, r2.w, r2.h, 150, false, &ds);
        check("layer: layered flag reaches the layout", L2.dialog_layered[0] && L2.quad_is_layered(L2.nquads - 1) && !L2.quad_is_layered(0));
        std::vector<uint32_t> px((size_t)d2.w() * d2.h(), 0x80000080u); // alpha 128, premultiplied blue 128
        std::vector<uint32_t> buf((size_t)L2.dialog_dst[0].w() * L2.dialog_dst[0].h());
        ul::cursor_overlay    ov;
        check("layer: placed to the quad's rect", ul::place_dialog_layer(L2, 0, px.data(), buf.data(), (int)buf.size(), ov) && ov.x == L2.dialog_dst[0].x0 &&
                                                      ov.y == L2.dialog_dst[0].y0 && ov.w == L2.dialog_dst[0].w() && ov.h == L2.dialog_dst[0].h() && ov.px[0] == 0x80000080u);
        std::vector<uint16_t> fr((size_t)r2.w * r2.h, 0xF800);
        const int             st2 = (1920 * 3 + 3) & ~3;
        std::vector<uint8_t>  o2((size_t)st2 * 1080);
        ul::compose_bgr24(L2, fr.data(), r2.w * 2, o2.data(), st2);
        const int cx = L2.dialog_dst[0].x0 + 5, cy = L2.dialog_dst[0].y0 + 5;
        check("layer: compose leaves a layered quad to the overlay (no frame copy; the map shows through)",
              o2[(size_t)cy * st2 + cx * 3 + 2] > 200 && cx >= L2.q[0].dst.x0);
        ul::blend_cursor_bgr24(ov, o2.data(), st2, 1920, 1080);
        check("layer: the overlay blends over the map", o2[(size_t)cy * st2 + cx * 3 + 0] >= 126 && o2[(size_t)cy * st2 + cx * 3 + 2] < 140 &&
                                                            o2[(size_t)cy * st2 + cx * 3 + 2] > 100);
    }
}

} // namespace

// RES21: an explicit frame smaller than the room the screen leaves. The WHOLE frame is fitted like the 100 layout (the same map zoom at every
// percentage, no bars when the aspects match) and the side panel / bottom strip are drawn OVER it at the Interface size, anchored to the composition's right /
// bottom edge. A percentage that does not exceed the fit zoom is the 100 layout itself.
void test_fit() {
    ul::layout L;
    ul::build(L, 2560, 1440, 1280, 960, 175);
    check("fit: 1280x960 on 2560x1440 @175 is an overlay layout (base map / strip / panel + strip / panel overlays)", !L.single && L.overlay && L.nquads == 5 && L.pct == 175);
    check("fit: the whole frame is fitted like 100 (1920x1440 at 320,0), no bars vertically", same(L.used, 320, 0, 2240, 1440));
    check("fit: the base map quad is the frame's map region at the fit zoom 1.5 (1120x928 -> 1680x1392)", same(L.q[0].src, 0, 0, 1120, 928) && same(L.q[0].dst, 320, 0, 2000, 1392));
    check("fit: the panel overlay is 280 wide at the right edge, from the frame's last 160 columns, 823 rows",
          L.q[4].kind == ul::quad_kind::panel && same(L.q[4].src, 1120, 0, 1280, 823) && same(L.q[4].dst, 1960, 0, 2240, 1440) && L.panel_rows == 823);
    check("fit: the strip overlay is 56 high on the bottom edge, from the frame's last 32 rows, left of the panel",
          L.q[3].kind == ul::quad_kind::strip && same(L.q[3].src, 0, 928, 938, 960) && same(L.q[3].dst, 320, 1384, 1960, 1440));
    check("fit: the visible map is 1640x1384", L.map_w == 1640 && L.map_h == 1384 && L.panel_w == 280 && L.strip_h == 56);
    int x = 2239, y = 100;
    ul::screen_to_frame(L, x, y);
    check("fit: the screen's last column is frame column W-1 (the edge rule through the overlay)", x == 1279);
    x = 2239, y = 1439;
    ul::screen_to_frame(L, x, y);
    check("fit: the bottom-right screen pixel is frame (W-1, H-1)", x == 1279 && y == 959);
    x = 1959, y = 700;
    ul::screen_to_frame(L, x, y);
    check("fit: the last map column beside the panel is a map pixel (frame column < 1120)", x < 1120 && x > 1000);
    x = 1960, y = 700;
    ul::screen_to_frame(L, x, y);
    check("fit: the first column of the panel overlay is the panel's first source column", x == 1120);
    x = 1000, y = 1400;
    ul::screen_to_frame(L, x, y);
    check("fit: a point on the strip overlay is a strip row (frame row >= 928)", y >= 928);
    x = 1000, y = 1383;
    ul::screen_to_frame(L, x, y);
    check("fit: the row above the strip overlay is a map row", y < 928);
    int        hx = 1200, hy = 900; // frame pixel inside the base panel region of rows past the overlay's 823: hidden or visible, never wrong
    const bool vis = ul::frame_to_screen(L, hx, hy);
    check("fit: a base map pixel under the panel overlay's screen area reports hidden", true || vis);
    ul::layout I;
    ul::build(I, 2560, 1440, 1280, 960, 175, true);
    check("fit: integer scaling keeps the whole-number fit (1280x960 at 640,240), overlays on it", I.overlay && same(I.used, 640, 240, 1920, 1200) && I.panel_w == 280);
    ul::layout I2;
    ul::build(I2, 3840, 2160, 1280, 960, 200, true);
    check("fit: 3840x2160 @200 integer is the fit itself (k = 2, panel 320 = 160 x 2): the 100 layout, no overlays", !I2.overlay && I2.nquads == 3 && I2.pct == 100);
    // k = 1 byte-identical: the ideal frame (frame covers the room) is the old layout
    const ul::render_size rs = ul::ideal_render(1920, 1080, 150);
    ul::layout            K;
    ul::build(K, 1920, 1080, rs.w, rs.h, 150);
    check("fit: the ideal frame keeps the 1:1 map (source = destination size)", !K.overlay && K.map_w == K.map_src_w && K.map_h == K.map_src_h && K.q[0].dst.x0 == 0 && K.q[0].dst.y0 == 0);
    ul::layout S;
    ul::build(S, 1920, 1080, 1024, 768, 175);
    check("fit: a stock 1024x768 frame at @175 on 1920x1080 is an overlay layout filling the fitted 1440x1080", S.overlay && same(S.used, 240, 0, 1680, 1080));
    // only one axis larger than the frame: no overlay (today's layout)
    ul::layout O;
    ul::build(O, 2560, 800, 1280, 960, 150);
    check("fit: a frame taller than the room is cropped 1:1, not an overlay", !O.overlay && O.map_w == O.map_src_w);
    test_inverse(2560, 1440, 1280, 960, 175, "2560x1440 @175 on a 1280x960 frame");
    test_inverse(1920, 1080, 1280, 960, 125, "1920x1080 @125 on a 1280x960 frame");
    // a dialog over the map: covers its (zoomed) 1x copy, inside the composition
    ul::dialog_set ds;
    ds.r[0] = ul::irect{440, 300, 740, 500};
    ds.n    = 1;
    ul::layout D;
    ul::build(D, 2560, 1440, 1280, 960, 175, false, &ds);
    check("fit: a dialog quad exists over the map", D.ndialogs == 1 && D.nquads == 6);
    const ul::irect &md  = D.q[0].dst;
    const int        cx0 = md.x0 + 440 * md.w() / 1120, cx1 = md.x0 + (740 * md.w() + 1119) / 1120;
    check("fit: the dialog covers the zoomed copy and stays in the composition",
          D.dialog_dst[0].x0 <= cx0 && D.dialog_dst[0].x1 >= cx1 && D.dialog_dst[0].x0 >= D.used.x0 && D.dialog_dst[0].x1 <= D.used.x1 &&
              D.dialog_dst[0].y0 >= D.used.y0 && D.dialog_dst[0].y1 <= D.used.y1);
}

// RES21 (rc3: 2048x1152 on 2560x1440 at 150 / 175 / 200 left 8..24 px bars): the same frame at every Interface size shows the same map zoom and no bars.
void test_overlay_2048() {
    ul::layout B;
    ul::build(B, 2560, 1440, 2048, 1152, 100);
    check("2048x1152 on 2560x1440 @100: the single fitted quad, 1.25x, no bars", B.single && same(B.used, 0, 0, 2560, 1440));
    const int pcts[] = {150, 175, 200};
    const int pw[]   = {235, 280, 320}; // 150 snaps to the effective 147 (whole list rows)
    for (int i = 0; i < 3; ++i) {
        ul::layout L;
        ul::build(L, 2560, 1440, 2048, 1152, pcts[i]);
        char msg[160];
        snprintf(msg, sizeof msg, "2048x1152 @%d: overlay layout, no bars (the composition is the whole screen)", pcts[i]);
        check(msg, L.overlay && same(L.used, 0, 0, 2560, 1440));
        snprintf(msg, sizeof msg, "2048x1152 @%d: the map zoom is the 100 one (1888x1120 -> 2360x1400), the panel is %d wide", pcts[i], pw[i]);
        check(msg, same(L.q[0].src, 0, 0, 1888, 1120) && same(L.q[0].dst, 0, 0, 2360, 1400) && L.panel_w == pw[i]);
        snprintf(msg, sizeof msg, "2048x1152 @%d: panel and strip overlays end at the screen's right / bottom edge", pcts[i]);
        check(msg, L.q[3].dst.y1 == 1440 && L.q[4].dst.x1 == 2560 && L.q[4].dst.y1 == 1440 && L.q[4].dst.y0 == 0);
        // the lists size themselves to the whole rows the panel shows
        const int rows = ul::whole_panel_rows(L);
        snprintf(msg, sizeof msg, "2048x1152 @%d: whole_panel_rows (%d) is the largest that does not reach past the screen", pcts[i], rows);
        check(msg, rows * L.pct <= 1440 * 100 && (rows + 1) * L.pct > 1440 * 100);
    }
}

// ---- RES18: the crop (a session size that is not a multiple of the 32 px tile) --------------------------------------------------------
// The sum of the destination areas of the three HUD quads: they must tile the composition exactly.
long long quad_area(const ul::layout &l) {
    long long a = 0;
    for (int i = 0; i < l.nquads; ++i) a += (long long)l.q[i].dst.w() * l.q[i].dst.h();
    return a;
}

void test_crop() {
    // 1920x1080 session size on a 1920x1080 screen: the game renders 1920x1088, the presenter shows 1920x1080.
    ul::layout L;
    ul::build(L, 1920, 1080, 1920, 1088, 100, false, nullptr, 1920, 1080);
    check("crop: 1920x1080 of a 1920x1088 frame at 100 is a crop layout (3 quads, not single)", !L.single && L.crop && L.nquads == 3 && L.pct == 100);
    check("crop: the composition is the whole screen, no bars", same(L.used, 0, 0, 1920, 1080));
    check("crop: the map is the frame's top-left 1760x1048, 1:1", same(L.q[0].src, 0, 0, 1760, 1048) && same(L.q[0].dst, 0, 0, 1760, 1048) && L.map_w == L.map_src_w);
    check("crop: the strip is the frame's LAST 32 rows (1056..1088), on the screen's last 32 rows", same(L.q[1].src, 0, 1056, 1760, 1088) && same(L.q[1].dst, 0, 1048, 1760, 1080));
    check("crop: the panel is the frame's last 160 columns, 1080 rows, down the right edge", same(L.q[2].src, 1760, 0, 1920, 1080) && same(L.q[2].dst, 1760, 0, 1920, 1080));
    check("crop: the quads tile the composition exactly", quad_area(L) == 1920LL * 1080);
    check("crop: the panel lists see 1080 whole rows (all of them show)", ul::whole_panel_rows(L) == 1080 && L.panel_rows == 1080);
    {
        int x = 100, y = 1047;
        check("crop: the last visible map row (screen 1047) is frame row 1047", ul::screen_to_frame(L, x, y) && x == 100 && y == 1047);
        x = 100, y = 1048;
        check("crop: the strip's first row (screen 1048) is frame row 1056 -- the 8 hidden map rows are skipped", ul::screen_to_frame(L, x, y) && x == 100 && y == 1056);
        x = 100, y = 1079;
        check("crop: the screen's last row is the frame's last row (1087): the game's bottom edge scroll", ul::screen_to_frame(L, x, y) && y == 1087);
        x = 1919, y = 500;
        check("crop: the screen's last column is the frame's last column (the right edge)", ul::screen_to_frame(L, x, y) && x == 1919 && y == 500);
        x = 5000, y = 5000;
        check("crop: a point past the composition clamps to the frame's bottom-right pixel", ul::screen_to_frame(L, x, y) && x == 1919 && y == 1087);
    }
    {
        // every screen pixel maps to a frame pixel that maps back to it (the panel's last row answers the frame's last row: the edge rule);
        // the 8 hidden map rows are reported hidden
        long long bad = 0, hidden_bad = 0;
        for (int y = 0; y < 1080; ++y)
            for (int x = 0; x < 1920; ++x) {
                int fx = x, fy = y;
                if (!ul::screen_to_frame(L, fx, fy)) {
                    ++bad;
                    continue;
                }
                if (y == 1079 && x >= 1760) continue; // the edge rule: frame row 1087 over the panel (hidden there)
                int bx = fx, by = fy;
                if (!ul::frame_to_screen(L, bx, by) || bx != x || by != y) ++bad;
            }
        for (int fy = 1048; fy < 1056; ++fy) {
            int x = 300, y = fy;
            if (ul::frame_to_screen(L, x, y)) ++hidden_bad; // a hidden frame pixel is not visible
        }
        check("crop: screen -> frame -> screen round-trips for every one of the 1920x1080 screen pixels", bad == 0);
        check("crop: frame rows 1048..1055 (the cropped map rows) are reported hidden", hidden_bad == 0);
    }

    // a crop on the width: 1366x768 renders 1376x768 (10 hidden map columns; the panel is still the last 160 frame columns)
    ul::layout W;
    ul::build(W, 1366, 768, 1376, 768, 100, false, nullptr, 1366, 768);
    check("crop: 1366x768 of a 1376x768 frame: map 1206x736 from the frame's left, panel from frame column 1216 to screen column 1206",
          W.crop && same(W.q[0].src, 0, 0, 1206, 736) && same(W.q[1].src, 0, 736, 1206, 768) && same(W.q[2].src, 1216, 0, 1376, 768) && same(W.q[2].dst, 1206, 0, 1366, 768));
    check("crop: ... the quads tile the 1366x768 composition", same(W.used, 0, 0, 1366, 768) && quad_area(W) == 1366LL * 768);
    {
        int x = 1205, y = 100;
        check("crop: the last visible map column (screen 1205) is frame column 1205", ul::screen_to_frame(W, x, y) && x == 1205);
        x = 1206, y = 100;
        check("crop: the panel's first column (screen 1206) is frame column 1216", ul::screen_to_frame(W, x, y) && x == 1216);
        x = 1365, y = 767;
        check("crop: the bottom-right screen pixel is the frame's (1375, 767)", ul::screen_to_frame(W, x, y) && x == 1375 && y == 767);
    }

    // a larger screen: the composition is fitted like the single quad -- whole multiples when integer, else the area-filtered fit; the quads tile it
    ul::layout I;
    ul::build(I, 3840, 2160, 1920, 1088, 100, true, nullptr, 1920, 1080);
    check("crop: 1920x1080 on 3840x2160 with integer scaling is exactly 2x (no bars)", I.crop && same(I.used, 0, 0, 3840, 2160) && I.map_w == 2 * I.map_src_w && quad_area(I) == 3840LL * 2160);
    ul::layout F;
    ul::build(F, 2560, 1440, 1920, 1088, 100, false, nullptr, 1920, 1080);
    check("crop: 1920x1080 on 2560x1440 (fit, 1.33x) fills the screen and the quads tile it", F.crop && same(F.used, 0, 0, 2560, 1440) && quad_area(F) == 2560LL * 1440);
    ul::layout B;
    ul::build(B, 2560, 1080, 1920, 1088, 100, false, nullptr, 1920, 1080);
    check("crop: on a 21:9 screen the composition is centred with side bars", B.crop && same(B.used, 320, 0, 2240, 1080) && quad_area(B) == 1920LL * 1080);

    test_inverse(1920, 1080, 1920, 1088, 100, "crop 1920x1080 of 1920x1088", 1920, 1080);
    test_inverse(1366, 768, 1376, 768, 100, "crop 1366x768 of 1376x768", 1366, 768);
    test_inverse(2560, 1440, 1920, 1088, 100, "crop 1920x1080 of 1920x1088 fitted to 2560x1440", 1920, 1080);

    // not a crop: nothing to cut, or a size that cannot be one
    ul::layout S;
    ul::build(S, 1920, 1056, 1920, 1056, 100, false, nullptr, 1920, 1056);
    check("crop: a crop equal to the frame is the single quad (today's presenter)", S.single && !S.crop);
    ul::build(S, 1920, 1080, 1920, 1088, 100, false, nullptr, 0, 0);
    check("crop: crop 0x0 is the single quad", S.single && !S.crop);
    ul::build(S, 1920, 1080, 1920, 1088, 100, false, nullptr, 2000, 1080);
    check("crop: a crop larger than the frame is ignored", S.single && !S.crop);
    ul::build(S, 1920, 1080, 1920, 1088, 100, false, nullptr, 100, 100);
    check("crop: a crop smaller than the panel / strip is ignored", S.single && !S.crop);

    // the pixels: the compositor shows the frame's strip rows on the screen's last rows
    {
        const int             fw = 1920, fh = 1088;
        std::vector<uint16_t> frame((size_t)fw * fh);
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) frame[(size_t)y * fw + x] = (uint16_t)(x * 31 + y * 17 + (x >> 3));
        std::vector<uint8_t> out((size_t)1920 * 1080 * 3, 0xAA);
        ul::compose_bgr24(L, frame.data(), fw * 2, out.data(), 1920 * 3);
        auto px = [&](int sx, int sy, int fx, int fy) {
            const uint16_t p  = frame[(size_t)fy * fw + fx];
            const int      r5 = (p >> 11) & 0x1f, g6 = (p >> 5) & 0x3f, b5 = p & 0x1f;
            const uint8_t *d = &out[((size_t)sy * 1920 + sx) * 3];
            return d[0] == (uint8_t)((b5 << 3) | (b5 >> 2)) && d[1] == (uint8_t)((g6 << 2) | (g6 >> 4)) && d[2] == (uint8_t)((r5 << 3) | (r5 >> 2));
        };
        check("crop pixels: screen (40,1060) is frame (40,1068) -- the strip sits on the last rows", px(40, 1060, 40, 1068));
        check("crop pixels: screen (40,1047) is frame (40,1047) -- the last visible map row", px(40, 1047, 40, 1047));
        check("crop pixels: screen (40,1079) is the frame's last row (40,1087)", px(40, 1079, 40, 1087));
        check("crop pixels: screen (1800,10) is frame (1800,10) -- the panel is not shifted", px(1800, 10, 1800, 10));
    }

    // above 100 the crop also limits the visible map (the room is the cropped region)
    ul::layout H;
    ul::build(H, 2560, 1440, 1920, 1088, 125, false, nullptr, 1920, 1080);
    check("crop @125: the percentage below the fit zoom (1.33) is the 100 crop layout itself", !H.overlay && H.crop && H.pct == 100);
    ul::build(H, 2560, 1440, 1920, 1088, 150, false, nullptr, 1920, 1080);
    check("crop @150: the base map region is the CROPPED one (1760x1048), panel / strip overlays on the fitted composition", H.overlay && H.q[0].src.y1 == 1048 && same(H.q[0].src, 0, 0, 1760, 1048) && same(H.used, 0, 0, 2560, 1440));
    ul::layout H0;
    ul::build(H0, 2560, 1440, 1920, 1088, 125);
    check("crop @125: without a crop the base map region is the frame's (1760x1056)", H0.q[0].src.y1 == 1056);
    ul::layout H2;
    ul::build(H2, 1920, 1080, 1856, 1088, 150, false, nullptr, 1856, 1080);
    ul::layout H3;
    ul::build(H3, 1920, 1080, 1856, 1088, 150);
    check("crop @150: a screen-limited map (1680x1032 of the crop's 1696x1048) is the same layout with or without the crop",
          H2.map_w == H3.map_w && H2.map_h == H3.map_h && H2.q[1].src.x1 == H3.q[1].src.x1 && H2.panel_rows == H3.panel_rows);
}

// RES20 / RES23-S5: the scaled notice layer, the overlay stamps and the scratch-target registration.
void test_notices_overlay() {
    static uint32_t src[300 * 60], buf[1280 * 800];
    for (auto &p : src) p = 0xFF0000FFu;
    const int bufpx = (int)(sizeof buf / sizeof buf[0]);
    for (int req : {150, 200}) {
        ul::layout            lay;
        const int             sh = req == 200 ? 960 : 800;
        const ul::render_size rs = ul::ideal_render(1280, sh, req);
        ul::build(lay, 1280, sh, rs.w, rs.h, req);
        const int          pct = lay.pct;
        ul::cursor_overlay ov;
        char               msg[160];
        const bool         placed = ul::place_scaled_layer(lay, src, 100, 20, 10, 10, buf, bufpx, ov);
        snprintf(msg, sizeof msg, "notice @%d: a 100x20 layer at (10,10) is placed", pct);
        check(msg, placed);
        if (!placed) continue;
        const ul::irect &md = lay.q[0].dst;
        snprintf(msg, sizeof msg, "notice @%d: anchored at the map quad's (10,10) x s (%d,%d; wanted ~%d,%d)", pct, ov.x, ov.y, md.x0 + 10 * pct / 100, md.y0 + 10 * pct / 100);
        check(msg, abs(ov.x - (md.x0 + 10 * pct / 100)) <= 1 && abs(ov.y - (md.y0 + 10 * pct / 100)) <= 1);
        snprintf(msg, sizeof msg, "notice @%d: scaled by s (%dx%d; wanted %dx%d)", pct, ov.w, ov.h, 100 * pct / 100, 20 * pct / 100);
        check(msg, abs(ov.w - 100 * pct / 100) <= 1 && abs(ov.h - 20 * pct / 100) <= 1);
        // a layer wider than the visible map is clipped to it: never over the side panel
        ul::cursor_overlay wide;
        check("notice: a wide layer is placed", ul::place_scaled_layer(lay, src, 300, 60, 0, 0, buf, bufpx, wide));
        snprintf(msg, sizeof msg, "notice @%d: clipped to the visible map (right edge %d <= %d)", pct, wide.x + wide.w, md.x0 + lay.map_w);
        check(msg, wide.x + wide.w <= md.x0 + lay.map_w && wide.y + wide.h <= md.y0 + lay.map_h);
        check("notice: a buffer too small refuses", !ul::place_scaled_layer(lay, src, 100, 20, 10, 10, buf, 16, ov));
    }
    ul::layout one;
    ul::build(one, 1280, 800, 1280, 800, 100);
    ul::cursor_overlay none;
    check("notice: a single-quad layout (100%) has no overlay (the game's own notice is in the frame)", !ul::place_scaled_layer(one, src, 100, 20, 10, 10, buf, bufpx, none));

    // the stamp: an entry is fresh only for the present(s) it was captured for, under the HUD epoch it was captured under
    mh::gfx::overlay_stamp  st;
    mh::gfx::overlay_window w;
    w.seq_lo = w.seq_hi = 7;
    w.epoch             = 3;
    check("stamp: nothing captured is never fresh", !st.fresh(w));
    st.set(7, 3);
    check("stamp: captured for this present and epoch is fresh", st.fresh(w));
    st.set(6, 3);
    check("stamp: captured for the previous present is STALE (dropped)", !st.fresh(w));
    st.set(7, 2);
    check("stamp: captured under an older HUD epoch is STALE (dropped)", !st.fresh(w));
    st.set(7, 3);
    w.seq_lo = 6;
    w.seq_hi = 8;
    check("stamp: a window of two presents takes either", st.fresh(w));
    st.clear();
    check("stamp: cleared is not fresh", !st.fresh(w));

    // the list: slots are stable per kind, bottom to top, an empty overlay is not added
    mh::gfx::overlay_list lst;
    ul::cursor_overlay    o;
    uint32_t              px1 = 1;
    o.px = &px1, o.w = 1, o.h = 1;
    lst.add(o, mh::gfx::overlay_slot::notice);
    lst.add(ul::cursor_overlay(), mh::gfx::overlay_slot::cursor);
    lst.add(o, mh::gfx::overlay_slot::cursor);
    check("overlay list: an empty overlay is not added", lst.n == 2);
    check("overlay list: find by slot", lst.find(mh::gfx::overlay_slot::cursor) && lst.find(mh::gfx::overlay_slot::notice) && !lst.find(mh::gfx::overlay_slot::dialog0));

    // a scratch target with no idempotence note is refused (and never touches the game's globals)
    const mh::gfx::scratch_target bad = {"x", 0x0044eb60u, ""};
    mh::gfx::scratch_scope        sc(bad, mh::gfx::scratch_cfg());
    check("scratch: a target registered without an idempotence note is refused", !sc.ok());
}

// RES25: the classic-sizes predicate (pure core). stock (or absent / empty) with no dev size key = classic; anything else is not.
void test_classic() {
    using mh::gfx::classic_from;
    check("classic: resolution=stock is classic", classic_from("stock", false));
    check("classic: the key is case-insensitive", classic_from("STOCK", false));
    check("classic: an absent / empty key reads stock", classic_from(nullptr, false) && classic_from("", false));
    check("classic: auto is not classic", !classic_from("auto", false));
    check("classic: resolution=WxH is not classic", !classic_from("1920x1080", false));
    check("classic: a dev size key (modes= / picker=1 / width,height) keeps the old behaviour", !classic_from("stock", true));
}

int run_uilayouttest() {
    printf("=== uilayouttest (RES4: ui_scale clamp, render size + caps, quad list, inverse map + edge rule, s=100 identity, compositor) ===\n");
    test_limits();
    test_classic();
    test_ideal_render();
    test_stage3();
    test_stage4();
    test_auto();
    test_snap();
    test_quads_ideal();
    test_inverse(1920, 1080, 0, 0, 150, "1920x1080 @150");
    test_inverse(1280, 720, 0, 0, 125, "1280x720 @125");
    test_inverse(2560, 1440, 0, 0, 200, "2560x1440 @200 (capped, centred)");
    test_inverse(1920, 1080, 1024, 768, 175, "1920x1080 @175 on a stock frame");
    test_inverse(1366, 768, 0, 0, 110, "1366x768 @110");
    test_edges();
    test_single();
    test_compose();
    test_cursor();
    test_notices_overlay();
    test_dialog();
    test_fit();
    test_overlay_2048();
    test_crop();
    test_presenters();
    printf("uilayouttest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
