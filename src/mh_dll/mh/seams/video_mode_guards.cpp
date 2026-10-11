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
#include "gfx/ddraw_own.h"          // owned_ddraw_active / owned_filter

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

int panel_fill_y(int h, int top, int rowh, int y_capped) {
    if (h <= PANEL_ROWS_MAX_H || rowh <= 0 || h <= top) return y_capped;
    const int y = top + ((h - top) / rowh) * rowh;
    return y > y_capped ? y : y_capped;
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
// from g_list_cap instead: 768 until seams/ui_panel_rows.cpp has grown the tables, then the height they were grown
// for. They also notice a WindowHeight change (g_seen_h) and hand it to the grown tables' resize hook first, so the
// scroll bodies and the stale-row cleanup follow every size change whichever route made it. The other three
// (llm_ui_group_tab_on_click, llm_strat_ui_unit_panel_tick, llm_ui_selection_panel_refresh: the selected-unit
// view, which hides a fixed group by the control-group-0 row count) keep the 768 cap.
int g_list_cap                  = 768;
int g_seen_h                    = 0;
void(__cdecl *g_on_height)(int) = nullptr;

void __cdecl list_height_seen() {
    const int h = *(const int *)mh::addr::WindowHeight;
    g_seen_h    = h;
    if (g_on_height) g_on_height(h);
}

// clang-format off
__declspec(naked) void panel_list_h_edx() {
    __asm {
        mov edx, ds:[0x005202cc]
        cmp edx, g_seen_h
        je  seen
        pushad
        call list_height_seen
        popad
        mov edx, ds:[0x005202cc]
    seen:
        cmp edx, g_list_cap
        jle ok
        mov edx, g_list_cap
    ok:
        ret
    }
}
__declspec(naked) void panel_list_h_ebx() {
    __asm {
        mov ebx, ds:[0x005202cc]
        cmp ebx, g_seen_h
        je  seen
        pushad
        call list_height_seen
        popad
        mov ebx, ds:[0x005202cc]
    seen:
        cmp ebx, g_list_cap
        jle ok
        mov ebx, g_list_cap
    ok:
        ret
    }
}
__declspec(naked) void panel_rows_h_edx() {
    __asm {
        mov edx, ds:[0x005202cc]
        cmp edx, 768
        jle ok
        mov edx, 768
    ok:
        ret
    }
}
__declspec(naked) void panel_rows_h_ebx() {
    __asm {
        mov ebx, ds:[0x005202cc]
        cmp ebx, 768
        jle ok
        mov ebx, 768
    ok:
        ret
    }
}
// clang-format on
static_assert(mh::video::PANEL_ROWS_MAX_H == 768, "the thunks hardcode the cap");

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
int __cdecl panel_fill_y_impl(int y, const uint8_t *desc) {
    const int rowh = *(const int *)desc, top = *(const int *)(desc + 0x21);
    const int y_eff = mh::video::panel_fill_y(*(const int *)mh::addr::WindowHeight, top, rowh, y);
    if (y_eff > y && rowh > 0) {
        const int   x    = *(const int *)(desc + 0x1d);
        const void *icon = *(const void *const *)(0x0050e2b3u + (uintptr_t)*(const int *)(desc + 0x11) * 4u);
        for (int yy = y; yy < y_eff; yy += rowh) draw_icon_vclipped(x, yy, icon);
    }
    return y_eff;
}
// clang-format off
__declspec(naked) void panel_fill_thunk_global() {
    __asm {
        push ecx
        push edx
        push 0x00510082
        push eax
        call panel_fill_y_impl
        add  esp, 8
        pop  edx
        pop  ecx
        mov  ecx, 0x0041bd25
        jmp  ecx
    }
}
__declspec(naked) void panel_fill_thunk_frame() {
    __asm {
        push ecx
        push edx
        push dword ptr [ebp - 0x2c]
        push eax
        call panel_fill_y_impl
        add  esp, 8
        pop  edx
        pop  ecx
        mov  ecx, 0x0041bd25
        jmp  ecx
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
            bool      list; // RES7: a scrollable list's row count (g_list_cap), not the selected-unit view's
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
            mg_log("; [video] side-panel list rows capped at the 768 mode (%d sites): taller modes keep the stock panels", patched);
        } else
            ok = false;
    }
    return ok;
}

bool panel_rows_p6_armed() { return g_p6_armed; }

void set_panel_list_cap(int h, void(__cdecl *on_height)(int)) {
    g_on_height = on_height;
    g_seen_h    = 0; // the next list row count reports the current height
    g_list_cap  = h > PANEL_ROWS_MAX_H ? h : PANEL_ROWS_MAX_H;
}

} // namespace mh::video
