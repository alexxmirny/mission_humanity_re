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
    {0x0044fbbdu, "llm_ui_map_drag_select_rect_draw"},
    {0x0044fbe5u, "llm_ui_map_drag_select_rect_draw"},
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
    vlog("; [view_wrap] armed -- biased window K=big-viewport per axis (floor %d,%d): %d/%d render sites spliced, %d displaced, %d mismatched "
         "(stock kept at the 2 sim callers and the hit-test)",
         ww::K_X, ww::K_Y, patched, N_SITES, displaced, mismatched);
    return patched;
}
