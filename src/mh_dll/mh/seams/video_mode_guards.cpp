//
// seams/video_mode_guards.cpp -- the mode-change crash guards for the resolution picker (and any other
// route that drops the display to a smaller mode while the cursor sits low). Three patches, each
// stock-identical wherever the original was in bounds, so they arm unconditionally.
//
// F1 -- STALE CURSOR READ ON A MODE CHANGE. llm_gfx_set_window_resolution (0x004285c7) snaps
//   WindowWidth/Height to the new mode and only then "rescales" the cursor (pct = cur*100/W_new;
//   cur = pct*W_new/100, an identity round-trip), then ends with CALL llm_gfx_hittest_begin (call site
//   0x004287f4), which reads one pixel at FRAMEBUFFER + CURSOR_Y*FB_PITCH + CURSOR_X*2. A cursor that was
//   valid in the larger mode survives unclamped. The owned DirectDraw's new 640x480 surface is 691200
//   bytes, so any CURSOR_Y >= 541 faults (observed: cursor (595,749) from 2560x1440). Not picker-only:
//   stock 1024x768 -> 640x480 with the cursor in the bottom ~30% hits it too (outcome dialog, tactical
//   mission start). P1 re-points that call to a clamp, which then continues into the original callee.
//
// F2 -- GALAXY BACKGROUND INDEX + UNCHECKED FULL-SCREEN COPY. llm_ui_planet_select_screen_build
//   (0x004c5de5) sets the list's background index to (CurrentSystem-1)*3 + 0x17 + _G_LLM_VIEW_SIZE_MODE --
//   three pre-authored backgrounds per system, one per STOCK mode -- and the picker stores its list
//   index (4 for 1280x768) there, walking into the next system's art or the NULL slots past 0x1f.
//   llm_ui_widget_list_draw_frame_layer (0x004b72d8) then copies WindowWidth*WindowHeight words from that
//   image (call site 0x004b7322) without looking at the image's own `u16 w, u16 h` header: an 800x576
//   image copied as 1280x768 reads off the end of its allocation. Even with the right index the largest
//   art is 1024x768, so any wider/taller mode overruns -- including the non-picker custom [video]
//   width/height mode, which reuses mode 2. P2 clamps the index to min(mode, 2); P3 makes the copy
//   header-aware and SCALES a mismatched image to fill the window with the game picture's own
//   [video] filter (the planet widgets are laid out across the whole window, so a filled background
//   stays registered with them).
//
// Arm: MH_Video_ApplyProcessAttrs (not behind the [net] gate). Each patch is expected-bytes guarded; a
// mismatch logs NOT armed and leaves that patch off. The kill switch is `[video] mode_guards=0`.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdarg>
#include <cstring>
#include <stdint.h>

#include "video_mode_guards.h"
#include "include/mh_run_context.h" // mh_proc_path
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "en_guard.h"               // EN-only build gate
#include "hook/patch.h"             // patch_bytes_guarded
#include "gfx/cpu_scale.h"          // the four filter kernels (shared with scalefiltertest)
#include "gfx/ddraw_own.h"          // owned_ddraw_active / owned_filter / owned_ui_panel_rows
#include "addr/mh_calls.gen.h"      // game_SetEvent (the panel redraw on a visible-rows change)
#include "seams/strat_pre_input.h"  // the strategic pump

#pragma comment(lib, "user32.lib") // wvsprintfA

namespace mh::video {

// =================================================================================================
// Pure cores
// =================================================================================================

namespace {
int clamp1(int v, int extent) { return v < 0 ? 0 : v >= extent ? extent - 1
                                                               : v; }
} // namespace

int clamp_cursor_to_mode(int *x, int *y, int *menu_x, int *menu_y, int W, int H) {
    if (W <= 0 || H <= 0) return 0;
    int        n      = 0;
    int *const p[4]   = {x, y, menu_x, menu_y};
    const int  ext[4] = {W, H, W, H};
    for (int i = 0; i < 4; ++i) {
        if (!p[i]) continue;
        const int v = clamp1(*p[i], ext[i]);
        if (v != *p[i]) {
            *p[i] = v;
            ++n;
        }
    }
    return n;
}

int galaxy_bg_mode(int m) { return m > 2 ? 2 : m; }

int panel_rows_height(int h) { return h > PANEL_ROWS_MAX_H ? PANEL_ROWS_MAX_H : h; }

// RES23-S3 / RES26: THE panel fill rule. A list sizes itself to the rows the screen shows (Hp) and the game's own filler paints the panel base (pink) from the end of its
// rows (y_stock) down. Every slot of the frame that fits WHOLE -- rows of row_h from `top` down to the last whole slot of the frame (win_h) -- gets the teal empty-slot
// icon, EXCEPT the slot the screen's visible edge (vis_h) cuts: that is the leftover under the last whole visible row and stays pink (user, 2026-10-23: "teal only where
// a whole row fits; the leftover under the last whole visible row is pink"). `skip` = the top of that slot, or -1 (vis_h at or past the frame, or on a slot boundary).
// Nothing to add (false) at stock heights, where y_stock already is the last whole slot.
bool panel_fill_plan(int win_h, int vis_h, int top, int row_h, int y_stock, int &icons_from, int &icons_to, int &skip) {
    icons_from = icons_to = y_stock;
    skip                  = -1;
    if (row_h <= 0 || win_h <= top || y_stock >= win_h) return false;
    if (vis_h < win_h && vis_h > top) {
        const int y_cut = top + ((vis_h - top) / row_h) * row_h;
        if (y_cut < vis_h) skip = y_cut;
    }
    const int y_full = top + ((win_h - top) / row_h) * row_h; // the last whole slot of the frame
    const int from   = y_stock;
    if (from >= y_full) return false;
    icons_from = from;
    icons_to   = y_full;
    return true;
}

void bg_cache_release(bg_cache &c) {
    if (c.buf) VirtualFree(c.buf, 0, MEM_RELEASE);
    memset(&c, 0, sizeof(c));
}

namespace {

// A cheap content fingerprint over at most ~4096 words spread across the image (reads stay inside iw*ih).
uint32_t fingerprint(const uint16_t *s, uint32_t n) {
    uint32_t h    = 2166136261u;
    uint32_t step = n / 4096u;
    if (step == 0) step = 1;
    for (uint32_t i = 0; i < n; i += step) h = (h ^ s[i]) * 16777619u;
    return h ^ n;
}

} // namespace

bg_result bg_restore_core(bg_cache &c, uint16_t *dst, const uint16_t *src, uint32_t count_words, int W, int H,
                          int pitch, int filter, int fmt) {
    if ((uintptr_t)src < 0x10000u || W <= 0 || H <= 0 || !dst) return bg_result::skipped_noimage;
    const int iw = src[-2], ih = src[-1];
    // Stock pass-through: byte for byte what retail did whenever it was in bounds (a taller image is
    // cropped to its top H rows by the linear copy).
    if (iw == W && ih >= H && count_words == (uint32_t)W * (uint32_t)H && pitch == W * 2) {
        memcpy(dst, src, (size_t)count_words * 2);
        return bg_result::passthrough;
    }
    if (iw == 0 || ih == 0 || pitch < W * 2) return bg_result::skipped_bad;

    const uint32_t fp  = fingerprint(src, (uint32_t)iw * (uint32_t)ih);
    bg_result      res = bg_result::scaled_cached;
    if (!c.buf || c.src != src || c.hash != fp || c.iw != iw || c.ih != ih || c.W != W || c.H != H || c.filter != filter || c.fmt != fmt) {
        const uint32_t need = (uint32_t)W * (uint32_t)H * 2u;
        if (!c.buf || c.bytes < need) {
            if (c.buf) VirtualFree(c.buf, 0, MEM_RELEASE);
            c.buf   = (uint16_t *)VirtualAlloc(nullptr, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            c.bytes = c.buf ? need : 0;
        }
        c.src = nullptr; // invalid until the scale below completes
        if (!c.buf) return bg_result::skipped_bad;
        if (!mh::gfx::cpu_scale::scale_image(src, iw, ih, c.buf, W, H, filter, fmt == 555))
            return bg_result::skipped_bad;
        c.src    = src;
        c.hash   = fp;
        c.iw     = iw;
        c.ih     = ih;
        c.W      = W;
        c.H      = H;
        c.filter = filter;
        c.fmt    = fmt;
        res      = bg_result::scaled_new;
    }
    const uint8_t *from = (const uint8_t *)c.buf;
    uint8_t       *to   = (uint8_t *)dst;
    for (int y = 0; y < H; ++y) memcpy(to + (size_t)y * pitch, from + (size_t)y * W * 2, (size_t)W * 2);
    return res;
}

// =================================================================================================
// The patches
// =================================================================================================

namespace {

// ---- raw VAs with provenance (no manifest data entry: that would add a registry region and stale every
// recorded fixture; precedent gfx_font_guard.cpp / ui_net_indicator.cpp) --------------------------------
constexpr uintptr_t ADDR_CURSOR_MENU_X = 0x00e58cccu; // _G_LLM_CURSOR_MENU_X (int)
constexpr uintptr_t ADDR_CURSOR_MENU_Y = 0x00e58cc8u; // _G_LLM_CURSOR_MENU_Y (int) -- the LOWER address
constexpr uintptr_t ADDR_COLOR_FORMAT  = 0x00559a04u; // G_COLOR_FORMAT: 565 / 555, written by llm_gfx_set_window_resolution
constexpr uintptr_t ADDR_HITTEST_BEGIN = 0x004a6737u; // llm_gfx_hittest_begin (the original callee of the P1 call site)

static_assert(mh::addr::_G_LLM_VIEW_SIZE_MODE == 0x00ae2aa8u, "P2 thunk hardcodes the address in asm");

char          g_log_path[MAX_PATH];
unsigned long g_log_gen = 0;

// mh_video.log, same file as video.cpp's vid_log (a twin writer -- the pattern gfx_font_guard.cpp uses);
// never mh_net.log, whose arm-line sequence is diffed against a committed baseline.
void mg_log(const char *fmt, ...) {
    mh_proc_path(g_log_path, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[320];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_log_path, line, lstrlenA(line));
}

template <class T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void *)a, sizeof(T));
    return v;
}
template <class T>
void wr(uintptr_t a, T v) { memcpy((void *)a, &v, sizeof(T)); }

// ---- P1 -----------------------------------------------------------------------------------------
volatile LONG g_clamp_said = 0;

void __cdecl on_modechange_cursor_clamp() {
    const int W = *(const int *)mh::addr::WindowWidth, H = *(const int *)mh::addr::WindowHeight;
    if (W <= 0 || H <= 0) return;
    // CURSOR_X/Y are UNALIGNED (0x5204a1/a5): memcpy, never a typed pointer.
    int       x = rd<int>(mh::addr::_G_LLM_CURSOR_X), y = rd<int>(mh::addr::_G_LLM_CURSOR_Y);
    int       mx = rd<int>(ADDR_CURSOR_MENU_X), my = rd<int>(ADDR_CURSOR_MENU_Y);
    const int ox = x, oy = y;
    if (clamp_cursor_to_mode(&x, &y, &mx, &my, W, H) == 0) return;
    wr<int>(mh::addr::_G_LLM_CURSOR_X, x);
    wr<int>(mh::addr::_G_LLM_CURSOR_Y, y);
    wr<int>(ADDR_CURSOR_MENU_X, mx);
    wr<int>(ADDR_CURSOR_MENU_Y, my);
    if (InterlockedExchange(&g_clamp_said, 1) == 0)
        mg_log("; [video] mode-change cursor clamp LIVE -- cursor (%d,%d) outside the new %dx%d, clamped "
               "(the stale-cursor read it prevents)",
               ox, oy, W, H);
}

uintptr_t g_hittest_target = ADDR_HITTEST_BEGIN;

// clang-format off
// Runs where llm_gfx_hittest_begin was called: clamp, then continue INTO the original callee, whose RET
// returns to 0x004287f9 (the stack is exactly as at the original call).
__declspec(naked) void cursor_clamp_thunk() {
    __asm {
        pushad
        pushfd
        call on_modechange_cursor_clamp
        popfd
        popad
        jmp  dword ptr [g_hittest_target]
    }
}

// ---- P2 -----------------------------------------------------------------------------------------
// Replaces `MOV EAX,[_G_LLM_VIEW_SIZE_MODE]` (the background-index read); preserves all but EAX/flags
// (the next instructions are ADD EDX,EAX and a MOV, so flags are dead).
__declspec(naked) void galaxy_mode_thunk() {
    __asm {
        mov  eax, dword ptr ds:[0x00ae2aa8]
        cmp  eax, 2
        jle  done
        mov  eax, 2
    done:
        ret
    }
}
// clang-format on

// ---- P3 -----------------------------------------------------------------------------------------
bg_cache      g_bg;
volatile LONG g_bg_skip_said = 0;

void __cdecl mh_bg_restore(uint16_t *dst, const uint16_t *src, uint32_t count_words) {
    const int W = *(const int *)mh::addr::WindowWidth, H = *(const int *)mh::addr::WindowHeight;
    const int pitch = *(const int *)mh::addr::_G_LLM_FB_PITCH;
    const int fmt   = *(const int *)ADDR_COLOR_FORMAT;
    // Under the owned DirectDraw the presenter's filter applies; under the system backend dgVoodoo owns
    // the picture and the art is point-scaled.
    const int       filter = mh::gfx::owned_ddraw_active() ? mh::gfx::owned_filter() : 0;
    const bg_result r      = bg_restore_core(g_bg, dst, src, count_words, W, H, pitch, filter, fmt);
    switch (r) {
        case bg_result::scaled_new:
            mg_log("; [video] background %ux%u scaled to %dx%d (%s) -- art smaller than the mode", (unsigned)g_bg.iw,
                   (unsigned)g_bg.ih, W, H, mh::gfx::owned_filter_name(filter));
            break;
        case bg_result::skipped_noimage:
        case bg_result::skipped_bad:
            if (InterlockedExchange(&g_bg_skip_said, 1) == 0)
                mg_log("; [video] background restore SKIPPED -- no image (index past the table?)");
            break;
        default:
            break;
    }
}

// clang-format off
// The caller pushed (dst, src, count) __cdecl and pops them itself; the original preserved every GPR.
// After pushad (0x20 bytes) the args sit at [esp+0x24]/[esp+0x28]/[esp+0x2c]; each push moves esp by 4,
// so the same displacement 0x2c reads count, src, dst in turn.
__declspec(naked) void bg_restore_thunk() {
    __asm {
        pushad
        push dword ptr [esp+0x2c]   // count
        push dword ptr [esp+0x2c]   // src
        push dword ptr [esp+0x2c]   // dst
        call mh_bg_restore
        add  esp, 12
        popad
        ret
    }
}
// clang-format on

// ---- P6: the side-panel list row count ------------------------------------------------------------
// Six sites compute a list's visible rows as (WindowHeight - top_y) / row_height: the scroll-list draw
// (llm_ui_scroll_icon_list_draw 0x00415c69, which the building / unit / project / storage panels all go
// through), its body and arrow handlers (0x00412129, 0x00412fe4), llm_ui_group_tab_on_click (0x00419221),
// llm_strat_ui_unit_panel_tick (0x00419549) and llm_ui_selection_panel_refresh (0x004197ba). Each row writes
// one 5-byte icon-table entry (llm_ui_icon_entry.kind) at base + row + 1, and every group in that table is
// closed by a kind-1 end marker placed for the tallest stock mode (768). At WindowHeight 1056 the rows ran
// past it: the end marker became "hidden", the dispatcher walked into the next group, and the selected
// ship's panel lost its card, tabs and resource rows (only the empty slot row was left). Each site is a
// 6-byte `mov reg, [WindowHeight]`, replaced by `call thunk; nop`, the thunk returning min(WindowHeight,
// 768) in that register; stock modes are unchanged. The panel's background fill (0x0041bbd2, 0x0041bd25)
// and the scroll list's clip bottom (0x00415db3) keep the real height, so the area under a capped list is
// filled with the panel background.
// RES7: the three sites that size a scrollable LIST (the body and arrow handlers and the list draw) take their cap
// from the committed geometry (hud_geometry.list_cap) instead: 768 until seams/ui_panel_rows.cpp has grown the tables, then the height they were grown
// for. They also notice a WindowHeight change (g_seen_h) and hand it to the grown tables' resize hook first, so the
// scroll bodies and the stale-row cleanup follow every size change whichever route made it. The other three
// (llm_ui_group_tab_on_click, llm_strat_ui_unit_panel_tick, llm_ui_selection_panel_refresh: the selected-unit
// view, which hides a fixed group by the control-group-0 row count) keep the 768 cap.
int g_seen_h                    = 0;
void(__cdecl *g_on_height)(int) = nullptr;

// RES4 stage 3 / RES23-S3: the height the panel lists size themselves to = min(WindowHeight, Hp), Hp being the frame rows of the side panel the screen shows when
// [video] ui_scale is in force (else unlimited). Hp and the list cap come from the COMMITTED geometry (before the first commit: the live value / 768); min'ed with
// WindowHeight BEFORE the caps, so no list computes a row nobody sees.
static int hp_limit() {
    const int hp = mh::gfx::owned_ui_panel_rows();
    return hp > 0 ? hp : 0x7fffffff;
}
static int list_cap() { return mh::gfx::owned_hud_list_cap(); }

// The scrollable lists' row-count height for `wh`: min(wh, Hp), reported to the RES7 resize hook when it changed, then the list cap.
int __cdecl panel_list_rows_for(int wh) {
    const int hp = hp_limit();
    int       h  = wh < hp ? wh : hp;
    if (h != g_seen_h) {
        g_seen_h = h;
        if (g_on_height) g_on_height(h);
    }
    const int cap = list_cap();
    return h < cap ? h : cap;
}
// The selected-unit view (it hides a fixed group by the control-group-0 row count) keeps the stock 768 cap.
int __cdecl panel_unit_rows_for(int wh) {
    const int hp = hp_limit();
    const int h  = wh < hp ? wh : hp;
    return h < PANEL_ROWS_MAX_H ? h : PANEL_ROWS_MAX_H;
}

// clang-format off
// Each thunk replaces `mov REG, [WindowHeight]` and returns the row-count height in that register (every other register preserved). After pushad the saved
// EBX sits at [esp+0x10] and EDX at [esp+0x14] (and one dword of argument pushed on top until `add esp, 4`).
__declspec(naked) void panel_list_h_edx() {
    __asm {
        mov edx, ds:[0x005202cc]
        pushad
        push edx
        call panel_list_rows_for
        add  esp, 4
        mov  [esp + 0x14], eax
        popad
        ret
    }
}
__declspec(naked) void panel_list_h_ebx() {
    __asm {
        mov ebx, ds:[0x005202cc]
        pushad
        push ebx
        call panel_list_rows_for
        add  esp, 4
        mov  [esp + 0x10], eax
        popad
        ret
    }
}
__declspec(naked) void panel_rows_h_edx() {
    __asm {
        mov edx, ds:[0x005202cc]
        pushad
        push edx
        call panel_unit_rows_for
        add  esp, 4
        mov  [esp + 0x14], eax
        popad
        ret
    }
}
__declspec(naked) void panel_rows_h_ebx() {
    __asm {
        mov ebx, ds:[0x005202cc]
        pushad
        push ebx
        call panel_unit_rows_for
        add  esp, 4
        mov  [esp + 0x10], eax
        popad
        ret
    }
}
// clang-format on

// The four calls of llm_ui_panel_background_tile_draw (0x0041bd25, __watcall EAX = y_start) that follow a list's rows: the
// scroll-list draw (0x00415f2d; its descriptor is the frame's [ebp-0x2c]) and the three unit-panel ones (0x004193c0,
// 0x004196f2, 0x00419963; descriptor _G_LLM_UI_SCROLL_DESC_UNIT_UNASSIGNED, 0x00510082). Their y_start came from the
// capped row count, and the rows below it are what painted the panel's teal body: the stock code draws an "empty row"
// icon (descriptor +0x11) for every row down to the uncapped end, and the filler then starts there. With the rows
// capped neither happens, so the area from the capped end to the window bottom stays black / brown. The thunk draws the
// missing empty rows (the icon only: they are not clickable, so no icon-table entry is written, which is the whole
// point of the cap) and gives the filler the start the uncapped count would have, then tail-jumps into the original.
// Descriptor layout (llm_ui_scroll_list_descriptor): +0 row height, +0x11 empty-row icon index, +0x1d icon x, +0x21 top y;
// icons are G_ICON_PTRS[] (0x0050e2b3), drawn by llm_ui_icon_draw_vclipped (0x0041b9a4, __watcall EAX = x, EDX = y,
// EBX = icon).
// clang-format off
__declspec(naked) void draw_icon_vclipped(int x, int y, const void *icon) {
    __asm {
        mov eax, [esp + 4]
        mov edx, [esp + 8]
        mov ecx, [esp + 12]
        pushad
        mov ebx, ecx
        mov ecx, 0x0041b9a4
        call ecx
        popad
        ret
    }
}
// clang-format on
// RES23-S3: THE panel fill, one pass. The thunk (1) asks the one rule (panel_fill_plan, fed by the committed geometry only) which slots get teal icons, (2) lets the game's
// filler paint the panel base from the stock start -- the single fill call, as in retail -- and (3) draws the teal empty-slot icons over it. Order matters: the base is
// pink all the way down, the icons are laid on top; only the partial gap stays pink.
void *const fill_tile_draw = (void *)0x0041bd25u; // llm_ui_panel_background_tile_draw (__watcall EAX = y_start): the panel base from y_start to the window bottom
struct fill_job {
    int         from = 0, to = 0, rowh = 0, x = 0, skip = -1;
    const void *icon = nullptr;
} g_fill;
int __cdecl panel_fill_prepare(int y, const uint8_t *desc) {
    const int                    rowh = *(const int *)desc, top = *(const int *)(desc + 0x21);
    const mh::gfx::hud_geometry &g     = mh::gfx::owned_hud_geometry();
    const int                    win_h = g.epoch && g.frame_h > 0 ? g.frame_h : *(const int *)mh::addr::WindowHeight;
    const int                    vis_h = g.epoch ? g.vis_h : (win_h < hp_limit() ? win_h : hp_limit());
    g_fill.rowh                        = 0;
    int from = 0, to = 0, skip = -1;
    if (mh::video::panel_fill_plan(win_h, vis_h, top, rowh, y, from, to, skip)) {
        g_fill.skip = skip;
        g_fill.from = from;
        g_fill.to   = to;
        g_fill.rowh = rowh;
        g_fill.x    = *(const int *)(desc + 0x1d);
        g_fill.icon = *(const void *const *)(0x0050e2b3u + (uintptr_t)*(const int *)(desc + 0x11) * 4u);
    }
    return y; // the filler starts where the game's rows end
}
void __cdecl panel_fill_icons() {
    if (g_fill.rowh <= 0) return;
    for (int yy = g_fill.from; yy < g_fill.to; yy += g_fill.rowh)
        if (yy != g_fill.skip) draw_icon_vclipped(g_fill.x, yy, g_fill.icon);
    g_fill.rowh = 0;
}
// clang-format off
__declspec(naked) void panel_fill_thunk_global() {
    __asm {
        push ecx
        push edx
        push 0x00510082
        push eax
        call panel_fill_prepare
        add  esp, 8
        pop  edx
        pop  ecx
        call dword ptr [fill_tile_draw]
        pushad
        call panel_fill_icons
        popad
        ret
    }
}
__declspec(naked) void panel_fill_thunk_frame() {
    __asm {
        push ecx
        push edx
        push dword ptr [ebp - 0x2c]
        push eax
        call panel_fill_prepare
        add  esp, 8
        pop  edx
        pop  ecx
        call dword ptr [fill_tile_draw]
        pushad
        call panel_fill_icons
        popad
        ret
    }
}
// clang-format on

// ---- arming -------------------------------------------------------------------------------------
bool g_armed    = false;
bool g_p6_armed = false; // all ten P6 sites armed (RES7 grows the tables only on top of them)

bool arm_call(uintptr_t site, const uint8_t *expect, int len, void *thunk, const char *what) {
    uint8_t repl[8];
    memcpy(repl, expect, (size_t)len);
    repl[0] = 0xE8;
    wr<int32_t>((uintptr_t)&repl[1], (int32_t)((uintptr_t)thunk - (site + 5)));
    if (!mh::hook::patch_bytes_guarded(site, expect, repl, len)) {
        mg_log("; [video] %s NOT armed (expected bytes not found at %08x)", what, (unsigned)site);
        return false;
    }
    return true;
}

} // namespace

static void hud_commit_pump(); // below

bool install_mode_change_guards(const char *ini) {
    if (!mh::en_build_ok()) return false; // EN-only
    if (g_armed) return true;
    g_armed = true;
    if (mh_ini_get_int("video", "mode_guards", 1, ini) == 0) {
        mg_log("; [video] mode-change guards DISABLED ([video] mode_guards=0)");
        return false;
    }
    bool ok = true;
    {
        static const uint8_t e[5] = {0xE8, 0x3E, 0xDF, 0x07, 0x00};
        if (arm_call(mh::addr::vres_hittest_call_site, e, 5, (void *)cursor_clamp_thunk, "mode-change cursor clamp"))
            mg_log("; [video] mode-change cursor clamp armed (call site %08x -> clamp, then llm_gfx_hittest_begin)",
                   (unsigned)mh::addr::vres_hittest_call_site);
        else
            ok = false;
    }
    {
        static const uint8_t e[5] = {0xA1, 0xA8, 0x2A, 0xAE, 0x00};
        if (arm_call(mh::addr::ui_galaxy_bg_mode_read, e, 5, (void *)galaxy_mode_thunk, "galaxy background index clamp"))
            mg_log("; [video] galaxy background index clamp armed (site %08x -> min(mode,2))",
                   (unsigned)mh::addr::ui_galaxy_bg_mode_read);
        else
            ok = false;
    }
    {
        static const uint8_t e[5] = {0xE8, 0xBC, 0xD4, 0xFF, 0xFF};
        if (arm_call(mh::addr::ui_bg_restore_call_site, e, 5, (void *)bg_restore_thunk, "background restore scaler"))
            mg_log("; [video] background restore scaler armed (call site %08x -> header-aware, filtered scale)",
                   (unsigned)mh::addr::ui_bg_restore_call_site);
        else
            ok = false;
    }
    {
        struct site {
            uintptr_t addr;
            bool      ebx;
            bool      list; // RES7: a scrollable list's row count (hud_geometry.list_cap), not the selected-unit view's
        };
        // each `mov reg, [0x005202cc]` (8B 15 / 8B 1D + disp32) in a row-count computation
        static const site SITES[] = {{0x00412163u, false, true}, {0x0041301cu, false, true}, {0x00415c86u, true, true}, {0x0041927cu, false, false}, {0x004195aeu, false, false}, {0x0041981fu, false, false}};
        int               patched = 0;
        for (const site &st : SITES) {
            const uint8_t e[6] = {0x8B, (uint8_t)(st.ebx ? 0x1D : 0x15), 0xCC, 0x02, 0x52, 0x00};
            uint8_t       r[6] = {0xE8, 0, 0, 0, 0, 0x90};
            const void   *t    = st.list ? (st.ebx ? (const void *)panel_list_h_ebx : (const void *)panel_list_h_edx)
                                         : (st.ebx ? (const void *)panel_rows_h_ebx : (const void *)panel_rows_h_edx);
            wr<int32_t>((uintptr_t)&r[1], (int32_t)((uintptr_t)t - (st.addr + 5)));
            if (mh::hook::patch_bytes_guarded(st.addr, e, r, 6))
                ++patched;
            else
                mg_log("; [video] panel list rows NOT armed at %08x (expected bytes not found)", (unsigned)st.addr);
        }
        struct fsite {
            uintptr_t addr;
            bool      frame;
        };
        static const fsite FILL_SITES[] = {{0x00415f2du, true}, {0x004193c0u, false}, {0x004196f2u, false}, {0x00419963u, false}};
        for (const fsite &f : FILL_SITES) {
            uint8_t     e[5] = {0xE8, 0, 0, 0, 0};
            uint8_t     r[5] = {0xE8, 0, 0, 0, 0};
            const void *t    = f.frame ? (const void *)panel_fill_thunk_frame : (const void *)panel_fill_thunk_global;
            wr<int32_t>((uintptr_t)&e[1], (int32_t)(0x0041bd25u - (f.addr + 5)));
            wr<int32_t>((uintptr_t)&r[1], (int32_t)((uintptr_t)t - (f.addr + 5)));
            if (mh::hook::patch_bytes_guarded(f.addr, e, r, 5))
                ++patched;
            else
                mg_log("; [video] panel list filler NOT armed at %08x (expected bytes not found)", (unsigned)f.addr);
        }
        if (patched == (int)(sizeof(SITES) / sizeof(SITES[0]) + sizeof(FILL_SITES) / sizeof(FILL_SITES[0]))) {
            g_p6_armed = true;
            mh::seams::strat_pre_input_add(hud_commit_pump, "the RES4 panel-rows redraw");
            mg_log("; [video] side-panel list rows capped at the 768 mode (%d sites): taller modes keep the stock panels", patched);
        } else
            ok = false;
    }
    return ok;
}

bool panel_rows_p6_armed() { return g_p6_armed; }

// RES23-S2: THE commit point of the HUD geometry. Runs first thing in every strategic frame (modes 2 and 5), before anything of the frame is drawn:
// it takes the pending Interface size and recomputes the whole panel rows (mh::gfx::owned_hud_commit), so the six row-count thunks, the filler and the
// RES7 resize of this frame all read ONE geometry, and the presenter stamps the frame with its epoch.
//
// RES4/RES19: the panels are drawn only when dirty (the tab dispatchers draw behind a refresh flag), and the bottom-strip extension only on the game's
// view-resize-pending flag. A commit that changed the percentage or the rows the screen shows therefore owes EVERY panel mode (selected unit, ship,
// building lists, ...) and the strip -- extension and corner -- a redraw in the same frame: game_SetEvent(HUD_REDRAW_ALL = 0x0a) sets the panel-switch and
// view-resize flags the stock code redraws all of that from (the tutorial start and every picker apply use it). The panel height the lists size
// themselves to is part of the same debt: a window or mode change that moved min(WindowHeight, Hp) is paid the same way. Owed in mode 5 (paused), paid at
// the next mode 2 frame.
static bool g_redraw_owed = false;
static void hud_commit_pump() {
    const int m = *(const uint8_t *)mh::addr::_G_LLM_GAME_MODE;
    if (m != 2 && m != 5) return;
    if (mh::gfx::owned_hud_commit(true)) g_redraw_owed = true; // the commit also owes it when the rows the lists size to (min(frame, Hp)) moved (RES23-S3: was a second tracker here)
    if (g_redraw_owed && m == 2) {
        g_redraw_owed = false;
        mh::call::game_SetEvent(0x0a);
    }
}

// The picker applied a mode (seams/video.cpp on_view_set_mode_post): recompute the frame-dependent part of the geometry for the new frame at the
// percentage already committed. Any redraw it owes is paid by the next strategic frame's pump.
void panel_geometry_recommit(bool applied_from_options) {
    if (applied_from_options ? mh::gfx::owned_hud_commit_applied() : mh::gfx::owned_hud_commit(false)) {
        g_redraw_owed = true;
        // RES26: in a dialog mode (3, 4) the strategic pump does not run, so the debt would wait for the resume: ask for the panel redraw now, the game repaints
        // the (hidden) frame behind the menu from the flags like it does after a picker apply.
        const int m = *(const uint8_t *)mh::addr::_G_LLM_GAME_MODE;
        if (applied_from_options && (m == 3 || m == 4)) {
            g_redraw_owed = false;
            mh::call::game_SetEvent(0x0a);
        }
    }
}

int panel_effective_height() {
    const int wh = *(const int *)mh::addr::WindowHeight;
    const int hp = hp_limit();
    return wh < hp ? wh : hp;
}

void set_panel_list_cap(int h, void(__cdecl *on_height)(int)) {
    g_on_height = on_height;
    g_seen_h    = 0;                 // the next list row count reports the current height
    mh::gfx::owned_hud_set_built(h); // the list cap lives in the committed geometry (max(768, h))
}

} // namespace mh::video
