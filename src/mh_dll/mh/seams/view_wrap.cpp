//
// seams/view_wrap.cpp -- RES3 / D13c: the biased wrap window for sprite placement.
//
// Mechanism and the reasoning for K: include/mh_viewwrap_export.h and gfx/wrap_window.h.
//
// WHY CALL SITES AND NOT THE ENTRY. The game's llm_strat_pixel_delta_wrapped has 24 callers. 21 are strategic
// RENDER code that wants a screen position; 3 want the stock short-way-round arithmetic and must keep it:
//
//   0x00448cf8  llm_strat_weapon_pixel_distance_ratio   SIM  (weapon range ratio, lockstep-hashed state)
//   0x0048c6e7  llm_strat_weapon_scatter_offset         SIM  (shot scatter, lockstep-hashed state)
//   0x0044eada  llm_strat_render_view                   UI   (hit-test: |dx|+|dy| < 40 between the cursor and last
//                                                             frame's candidates -- a distance, not a position)
//
// Splicing the 21 render calls leaves the callee byte-for-byte as the game (or libmh's promotion of it) has it,
// so the sim result cannot change, in the original configuration and under promotion alike; a detour of the
// entry would have collided with the promotion that owns it and needed a return-address test to tell the callers
// apart. The cost is one table of sites.
//
// Two of the 21 (the drag-select box's corners, 0x0044fbbd / 0x0044fbe5) are a PAIR whose results must stay ordered, so they
// are not in SITES[] and get their own thunks plus a guard on the draw call (RES12, further down).
//
// The thunk is the register shape the callers use (__watcall: EAX=from_x EDX=from_y EBX=to_x ECX=to_y, two stack
// out-pointers, callee pops 8), the same wrapper the generated export thunks use; EBX/ECX/EDX are preserved.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <string.h>
#include <cstdarg>
#include <cstdint>

#include "include/mh_viewwrap_export.h"
#include "include/mh_run_context.h" // mh_proc_path
#include "include/mh_config_dir.h"  // RL3: <config dir>mh_net.ini
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // mh::addr::general (the map geometry)
#include "addr/mh_structs.gen.h"    // mh_llm_strat_map_geom
#include "gfx/wrap_window.h"
#include "hook/patch.h"
#include "hook/promoted.h"
#include "en_guard.h"

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

namespace ww = mh::gfx::wrapwin;

constexpr uintptr_t ADDR_PIXEL_DELTA_WRAPPED = 0x004944f6u; // llm_strat_pixel_delta_wrapped

// The 21 render call sites (each `E8 rel32` to ADDR_PIXEL_DELTA_WRAPPED), by caller. The three stock callers are
// listed in the file header and are deliberately absent.
struct site {
    uintptr_t   addr;
    const char *caller;
};
constexpr site SITES[] = {
    {0x0044f107u, "llm_strat_render_selection_markers"},
    {0x0044f198u, "llm_strat_render_selection_markers"},
    {0x0044f285u, "llm_strat_render_selection_markers"},
    {0x0044f48fu, "llm_strat_render_selection_markers"},
    {0x0044f998u, "llm_strat_render_selection_markers"},
    {0x0044fa2du, "llm_strat_render_selection_markers"},
    // 0x0044fbbd / 0x0044fbe5 (llm_ui_map_drag_select_rect_draw, the box's two corners) are spliced separately below
    // (RES12): the second corner is derived from the first, not wrapped on its own.
    {0x004502e9u, "llm_strat_render_unit_sprite"},
    {0x004503d5u, "llm_strat_render_unit_sprite"},
    {0x0045043fu, "llm_strat_render_unit_sprite"},
    {0x00450a44u, "llm_strat_render_unit_shadow"},
    {0x004510e5u, "llm_strat_render_docked_unit_air"},
    {0x00451223u, "llm_strat_render_docked_unit_air"},
    {0x004515c2u, "llm_strat_render_docked_unit_ground"},
    {0x00451a3au, "llm_strat_render_bldg_docked_unit"},
    {0x00451be6u, "llm_strat_render_bldg_docked_unit"},
    {0x00451cf7u, "llm_strat_render_bldg_docked_unit"},
    {0x00453216u, "llm_strat_draw_mine_resource_overlay"},
    {0x0045382fu, "llm_strat_check_bldg_placement_valid"},
    {0x004539b1u, "llm_strat_check_bldg_placement_valid"},
};
constexpr int N_SITES = (int)(sizeof(SITES) / sizeof(SITES[0]));

// RES12: the drag-select box (llm_ui_map_drag_select_rect_draw, 0x0044faae). Its two corner calls and the draw.
constexpr uintptr_t SITE_DRAG_C1     = 0x0044fbbdu; // wraps (left, top) -- the biased window
constexpr uintptr_t SITE_DRAG_C2     = 0x0044fbe5u; // wraps (right, bottom) -- derived from corner 1 instead
constexpr uintptr_t SITE_DRAG_DRAW   = 0x004276a2u; // E8 -> llm_gfx_draw_dashed_rect_outline_colored (reads the four statics)
constexpr uintptr_t ADDR_DASHED_DRAW = 0x004a5778u;
constexpr uintptr_t DASHED_BOTTOM = 0x00825028u, DASHED_TOP = 0x0082502cu, DASHED_RIGHT = 0x00825030u, DASHED_LEFT = 0x00825034u;
constexpr uintptr_t SCROLL_COL = 0x00e15e38u, SCROLL_ROW = 0x00e15e3cu;   // camera px (x, y)
constexpr uintptr_t ANCHOR_COL = 0x00e5899cu, ANCHOR_ROW = 0x00e589a0u;   // drag anchor, world px
constexpr uintptr_t CURSOR_WCOL = 0x00e589a4u, CURSOR_WROW = 0x00e589a8u; // cursor, world px

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

void vlog(const char *fmt, ...) {
    mh_proc_path(g_log, MAX_PATH, "%smh_video.log", &g_log_gen); // arm-time decision: process-scoped
    char    line[300];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_log, line, lstrlenA(line));
}

volatile LONG g_first = 0;

// ---- the effect / projectile view gate -------------------------------------------------------------------------
// llm_strat_tile_in_view_and_visible (0x00453c9f), __watcall(EAX = tile col, EDX = tile row) -> EAX: the visible
// byte of tile_objects[col][row] AND the tile inside the camera span [cam-1, cam+_G_LLM_VIEW_TILES + 1] on each
// axis. Its two callers are the effect and projectile draws (llm_strat_fx_anim_draw 0x00441a8e, which carries the
// landing footprint; llm_strat_projectile_draw). At a render width of 2048 on a 64-tile map the span is 67 columns of a
// 64-column torus and the stock test rejects all but three of them. Re-pointing the two CALL sites at a
// replacement that takes the whole axis as in view in that case (gfx/wrap_window.h) restores them; elsewhere it
// computes the stock result. Not a sim path: both callers only draw.
constexpr uintptr_t ADDR_TILE_IN_VIEW = 0x00453c9fu;
constexpr uintptr_t SITES_GATE[]      = {0x004419f7u, 0x00441af2u};
constexpr uintptr_t TILE_OBJECTS      = 0x00d1ec80u; // map_tile_object_data[256][256], 8 bytes each, visibility at +7

int __cdecl tile_in_view_impl(int col, int row) {
    const mh::game::mh_llm_strat_map_geom *g = (const mh::game::mh_llm_strat_map_geom *)mh::addr::general;
    if (*(const uint8_t *)(TILE_OBJECTS + ((uintptr_t)(uint32_t)col * 256u + (uint32_t)row) * 8u + 7u) == 0) return 0;
    const int mw = (int)(g->big_width >> 5), mh_ = (int)(g->big_height >> 5);
    const int cc = *(const int32_t *)mh::addr::_G_LLM_MAP_CAM_COL, cr = *(const int32_t *)mh::addr::_G_LLM_MAP_CAM_ROW;
    const int tw = *(const int32_t *)mh::addr::_G_LLM_VIEW_TILES_W, th = *(const int32_t *)mh::addr::_G_LLM_VIEW_TILES_H;
    return ww::tile_in_span(col, cc, tw, mw) && ww::tile_in_span(row, cr, th, mh_) ? 1 : 0;
}

// __watcall(EAX, EDX) -> EAX; every other register is the caller's.
__declspec(naked) void tile_in_view_thunk() {
    __asm {
        push ecx
        push edx
        push edx
        push eax
        call tile_in_view_impl
        add  esp, 8
        pop  edx
        pop  ecx
        ret
    }
}

// (x1,y1) = camera px, (x2,y2) = the sprite's top-left px; out = the screen offset. cdecl: the thunk below
// adapts the game's register convention to it.
void __cdecl wrap_impl(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx, int32_t *out_dy) {
    const mh::game::mh_llm_strat_map_geom *g = (const mh::game::mh_llm_strat_map_geom *)mh::addr::general;
    // K per axis = big - viewport (G_WIN_W / G_WIN_H are the map viewport, panel and strip excluded).
    const int kx = ww::bias_for((int)g->big_width, *(const int32_t *)mh::addr::G_WIN_W, ww::K_X);
    const int ky = ww::bias_for((int)g->big_height, *(const int32_t *)mh::addr::G_WIN_H, ww::K_Y);
    *out_dx      = ww::biased_axis(x1, x2, (int)g->big_width, kx);
    *out_dy      = ww::biased_axis(y1, y2, (int)g->big_height, ky);
    if (!g_first && InterlockedCompareExchange(&g_first, 1, 0) == 0)
        vlog("; [view_wrap] first placement: cam %d,%d  sprite %d,%d  big %u x %u K %d,%d -> offset %d,%d (stock would give %d,%d)",
             x1, y1, x2, y2, (unsigned)g->big_width, (unsigned)g->big_height, kx, ky, *out_dx, *out_dy,
             ww::stock_axis(x1, x2, (int)g->big_width), ww::stock_axis(y1, y2, (int)g->big_height));
}

void(__cdecl *g_impl)(int32_t, int32_t, int32_t, int32_t, int32_t *, int32_t *) = wrap_impl;

// __watcall(EAX, EDX, EBX, ECX, [stack] out_dx, out_dy), RET 8. Same frame as the generated export thunks.
__declspec(naked) void wrap_thunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push edx
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        push ecx
        push ebx
        push edx
        push eax
        call dword ptr [g_impl]
        add  esp, 24
        pop  edx
        pop  ecx
        pop  ebp
        ret  8
    }
}


// ---- RES12: the drag-select box ---------------------------------------------------------------------------------
// The game orders the two WORLD corners itself (left = the start of the shorter arc, so right - left (mod big) <= big/2)
// and converts each to screen px with llm_strat_pixel_delta_wrapped. Two independent biased wraps can put the corners
// on opposite sides of the window (an anchor scrolled more than K px off the left lands at the far right) and invert
// the box; the outline's dirty-map memset then runs for ~134 MB (player crash, rc9). The box itself is the stock one --
// its world rectangle and the units it selects (llm_strat_selection_drag_finish works on the world corners only) are not
// touched. Only WHERE on screen it is drawn is decided here: the near corner takes the stock position when the box still
// reaches into the view (the outline a retail view draws) and the biased window otherwise (gfx/wrap_window.h
// drag_near_axis), and the far corner is that plus the stock modular width / height, so the box is always ordered. The
// draw call is guarded against any box that is still inverted ([video] drag_box_fix=0 restores the D13c independent wraps).
int32_t g_c1_world_x = 0, g_c1_world_y = 0, g_c1_sx = 0, g_c1_sy = 0;
bool    g_c1_valid = false;

void __cdecl corner1_impl(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx, int32_t *out_dy) {
    // The game has staged the two WORLD corners in the dashed-rect statics (left/top = x2/y2, right/bottom still world).
    const mh::game::mh_llm_strat_map_geom *g  = (const mh::game::mh_llm_strat_map_geom *)mh::addr::general;
    const int                              kx = ww::bias_for((int)g->big_width, *(const int32_t *)mh::addr::G_WIN_W, ww::K_X);
    const int                              ky = ww::bias_for((int)g->big_height, *(const int32_t *)mh::addr::G_WIN_H, ww::K_Y);
    *out_dx                                   = ww::drag_near_axis(x1, x2, *(const volatile int32_t *)DASHED_RIGHT, (int)g->big_width, kx);
    *out_dy                                   = ww::drag_near_axis(y1, y2, *(const volatile int32_t *)DASHED_BOTTOM, (int)g->big_height, ky);
    g_c1_world_x                              = x2;
    g_c1_world_y                              = y2;
    g_c1_sx                                   = *out_dx;
    g_c1_sy                                   = *out_dy;
    g_c1_valid                                = true;
}

void __cdecl corner2_impl(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx, int32_t *out_dy) {
    const mh::game::mh_llm_strat_map_geom *g = (const mh::game::mh_llm_strat_map_geom *)mh::addr::general;
    if (!g_c1_valid) { // corner 1 was not seen (cannot happen: both sites arm together) -- the plain biased wrap
        wrap_impl(x1, y1, x2, y2, out_dx, out_dy);
        return;
    }
    g_c1_valid = false;
    *out_dx    = g_c1_sx + ww::span_axis(g_c1_world_x, x2, (int)g->big_width);
    *out_dy    = g_c1_sy + ww::span_axis(g_c1_world_y, y2, (int)g->big_height);
}

void(__cdecl *g_impl_c1)(int32_t, int32_t, int32_t, int32_t, int32_t *, int32_t *) = corner1_impl;
void(__cdecl *g_impl_c2)(int32_t, int32_t, int32_t, int32_t, int32_t *, int32_t *) = corner2_impl;

// Same frame as wrap_thunk, one per corner.
__declspec(naked) void corner1_thunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push edx
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        push ecx
        push ebx
        push edx
        push eax
        call dword ptr [g_impl_c1]
        add  esp, 24
        pop  edx
        pop  ecx
        pop  ebp
        ret  8
    }
}

__declspec(naked) void corner2_thunk() {
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push edx
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        push ecx
        push ebx
        push edx
        push eax
        call dword ptr [g_impl_c2]
        add  esp, 24
        pop  edx
        pop  ecx
        pop  ebp
        ret  8
    }
}

volatile LONG g_box_logged = 0, g_box_skipped = 0;
int32_t       g_last_box[4] = {0x7fffffff, 0, 0, 0};

// Returns 1 to draw the outline, 0 to skip a box that is still inverted (left > right or top > bottom): the dirty-map
// memset of llm_gfx_draw_dashed_rect_outline_colored takes its count from right - left. The stock function already draws
// nothing unless left != right, top != bottom, right >= 0, bottom >= 0, left < G_WIN_W and top < G_WIN_H; an inverted box
// that fails one of those is left to it (stock behaviour), only one that passes them all is the crash and is skipped.
int __cdecl drag_draw_impl() {
    const int32_t l = *(const volatile int32_t *)DASHED_LEFT, t = *(const volatile int32_t *)DASHED_TOP;
    const int32_t r = *(const volatile int32_t *)DASHED_RIGHT, b = *(const volatile int32_t *)DASHED_BOTTOM;
    const bool    stock_draws = l != r && t != b && r >= 0 && b >= 0 && l < *(const int32_t *)mh::addr::G_WIN_W && t < *(const int32_t *)mh::addr::G_WIN_H;
    if (stock_draws && (r < l || b < t)) {
        if (InterlockedIncrement(&g_box_skipped) <= 20)
            vlog("; [drag_box] SKIPPED an inverted outline: left %d top %d right %d bottom %d (cam %d,%d anchor %d,%d cursor %d,%d)", l, t, r, b,
                 *(const int32_t *)SCROLL_COL, *(const int32_t *)SCROLL_ROW, *(const int32_t *)ANCHOR_COL, *(const int32_t *)ANCHOR_ROW,
                 *(const int32_t *)CURSOR_WCOL, *(const int32_t *)CURSOR_WROW);
        return 0;
    }
    // One line per distinct box (a held drag redraws the same box every frame): the differential's record. The world
    // box is the stock one (the game's own ordering), the screen box is where it is drawn.
    if ((l != g_last_box[0] || t != g_last_box[1] || r != g_last_box[2] || b != g_last_box[3]) && g_box_logged < 4000) {
        g_last_box[0] = l, g_last_box[1] = t, g_last_box[2] = r, g_last_box[3] = b;
        ++g_box_logged;
        const mh::game::mh_llm_strat_map_geom *g  = (const mh::game::mh_llm_strat_map_geom *)mh::addr::general;
        const int32_t                          ac = *(const int32_t *)ANCHOR_COL, ar = *(const int32_t *)ANCHOR_ROW, cc = *(const int32_t *)CURSOR_WCOL,
                      cr = *(const int32_t *)CURSOR_WROW, bw = (int)g->big_width, bh = (int)g->big_height;
        // the stock ordering of the world corners (left = start of the shorter arc)
        const bool    a_right = ww::span_axis(cc, ac, bw) < ww::span_axis(ac, cc, bw), a_bottom = ww::span_axis(cr, ar, bh) < ww::span_axis(ar, cr, bh);
        const int32_t wl = a_right ? cc : ac, wr = a_right ? ac : cc, wt = a_bottom ? cr : ar, wb = a_bottom ? ar : cr;
        vlog("; [drag_box] world %d,%d..%d,%d (w %d h %d) cam %d,%d screen %d,%d..%d,%d", wl, wt, wr, wb, ww::span_axis(wl, wr, bw),
             ww::span_axis(wt, wb, bh), *(const int32_t *)SCROLL_COL, *(const int32_t *)SCROLL_ROW, l, t, r, b);
    }
    return 1;
}

volatile LONG   g_draw_ok       = 1;
const uintptr_t g_dashed_target = ADDR_DASHED_DRAW;

// The call replacement: no arguments, every register the caller's. Skip = return without calling.
__declspec(naked) void drag_draw_thunk() {
    __asm {
        pushad
        call drag_draw_impl
        mov  g_draw_ok, eax
        popad
        cmp  g_draw_ok, 0
        je   skip
        jmp  dword ptr [g_dashed_target]
    skip:
        ret
    }
}

} // namespace

extern "C" int MH_ViewWrap_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only

    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini); // RL3: <config dir>mh_net.ini
    // [video] fx_view_gate=0 leaves the stock effect / projectile view test (the fix-off UI row).
    if (mh_ini_get_int("video", "fx_view_gate", 1, ini)) {
        int armed = 0;
        for (const uintptr_t a : SITES_GATE) {
            const int32_t rel       = (int32_t)(ADDR_TILE_IN_VIEW - (a + 5));
            const int32_t rel_new   = (int32_t)((uintptr_t)&tile_in_view_thunk - (a + 5));
            uint8_t       expect[5] = {0xE8, 0, 0, 0, 0};
            uint8_t       repl[5]   = {0xE8, 0, 0, 0, 0};
            memcpy(expect + 1, &rel, sizeof(rel));
            memcpy(repl + 1, &rel_new, sizeof(rel_new));
            if (!mh::hook::promoted_owner_of(a) && mh::hook::patch_bytes_guarded(a, expect, repl, 5))
                ++armed;
            else
                vlog("; [view_wrap] effect view gate NOT armed at %08x", (unsigned)a);
        }
        vlog("; [view_wrap] effect / projectile view gate armed (%d/2 call sites): a view spanning the whole map axis shows every tile", armed);
    } else {
        vlog("; [view_wrap] effect view gate OFF ([video] fx_view_gate=0): the stock test");
    }
    // [video] drag_box_fix=0 leaves the stock draw call and (with wrap_window=1) the D13c independent corner wraps
    // (the fix-off UI row: the inverted box faults at 0x004a583d).
    const int drag_fix = mh_ini_get_int("video", "drag_box_fix", 1, ini);
    if (drag_fix) {
        const int32_t rel = (int32_t)(ADDR_DASHED_DRAW - (SITE_DRAG_DRAW + 5)), rel_new = (int32_t)((uintptr_t)&drag_draw_thunk - (SITE_DRAG_DRAW + 5));
        uint8_t       expect[5] = {0xE8, 0, 0, 0, 0}, repl[5] = {0xE8, 0, 0, 0, 0};
        memcpy(expect + 1, &rel, sizeof(rel));
        memcpy(repl + 1, &rel_new, sizeof(rel_new));
        if (!mh::hook::promoted_owner_of(SITE_DRAG_DRAW) && mh::hook::patch_bytes_guarded(SITE_DRAG_DRAW, expect, repl, 5))
            vlog("; [drag_box] draw guard armed at %08x: an inverted drag-select outline is skipped, each distinct box is logged", (unsigned)SITE_DRAG_DRAW);
        else
            vlog("; [drag_box] draw guard NOT armed at %08x", (unsigned)SITE_DRAG_DRAW);
    } else {
        vlog("; [drag_box] OFF ([video] drag_box_fix=0): the stock draw and the independent corner wraps");
    }
    // [video] wrap_window=0 leaves the stock placement (the fix-off UI row).
    if (!mh_ini_get_int("video", "wrap_window", 1, ini)) {
        vlog("; [view_wrap] OFF ([video] wrap_window=0): the stock short-way-round placement");
        return 0;
    }

    int patched = 0, displaced = 0, mismatched = 0;
    for (const site &s : SITES) {
        if (mh::hook::promoted_owner_of(s.addr)) { // a render body promoted into libmh would carry its own placement
            ++displaced;
            continue;
        }
        const int32_t rel       = (int32_t)(ADDR_PIXEL_DELTA_WRAPPED - (s.addr + 5));
        const int32_t rel_new   = (int32_t)((uintptr_t)&wrap_thunk - (s.addr + 5));
        uint8_t       expect[5] = {0xE8, 0, 0, 0, 0};
        uint8_t       repl[5]   = {0xE8, 0, 0, 0, 0};
        memcpy(expect + 1, &rel, sizeof(rel));
        memcpy(repl + 1, &rel_new, sizeof(rel_new));
        if (mh::hook::patch_bytes_guarded(s.addr, expect, repl, 5)) {
            ++patched;
        } else {
            ++mismatched;
            vlog("; [view_wrap] MISMATCH at %08x (%s): not a call to the stock placement", (unsigned)s.addr, s.caller);
        }
    }
    // The drag-select box's two corners (RES12): corner 1 through the biased window, corner 2 derived from it. With
    // drag_box_fix=0 both go through the plain biased thunk, which is what D13c shipped.
    {
        const uintptr_t sites[2]  = {SITE_DRAG_C1, SITE_DRAG_C2};
        const uintptr_t thunks[2] = {(uintptr_t)(drag_fix ? &corner1_thunk : &wrap_thunk), (uintptr_t)(drag_fix ? &corner2_thunk : &wrap_thunk)};
        int             armed     = 0;
        if (!mh::hook::promoted_owner_of(sites[0]) && !mh::hook::promoted_owner_of(sites[1])) {
            for (int i = 0; i < 2; ++i) {
                const int32_t rel       = (int32_t)(ADDR_PIXEL_DELTA_WRAPPED - (sites[i] + 5));
                const int32_t rel_new   = (int32_t)(thunks[i] - (sites[i] + 5));
                uint8_t       expect[5] = {0xE8, 0, 0, 0, 0}, repl[5] = {0xE8, 0, 0, 0, 0};
                memcpy(expect + 1, &rel, sizeof(rel));
                memcpy(repl + 1, &rel_new, sizeof(rel_new));
                if (mh::hook::patch_bytes_guarded(sites[i], expect, repl, 5)) ++armed;
            }
        }
        patched += armed;
        vlog("; [view_wrap] drag-select box corners: %d/2 spliced (%s)", armed,
             drag_fix ? "corner 2 = corner 1 + the stock modular span" : "independent biased wraps, fix off");
    }
    vlog("; [view_wrap] armed -- biased window K=big-viewport per axis (floor %d,%d): %d/%d render sites spliced (the 2 drag-box corners add to the first number), %d displaced, %d mismatched "
         "(stock kept at the 2 sim callers and the hit-test)",
         ww::K_X, ww::K_Y, patched, N_SITES, displaced, mismatched);
    return patched;
}
