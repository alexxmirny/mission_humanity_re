//
// video.cpp -- D13 display-mode selection ([video] size_mode in mh_net.ini).
//
// The game's own resolution machinery is intact and shipped:
//
//   llm_view_set_size_mode(m)          m = 0/1/2 -> llm_gfx_apply_window_resolution(640,480) /
//                                      (800,600) / (1024,768): releases the DirectDraw surfaces,
//                                      re-probes the mode, MoveWindow, re-inits the display.
//   llm_gfx_set_window_resolution()    snaps WindowWidth/WindowHeight to 640x480 / 800x576 /
//                                      1024x768 -- note the 800 mode renders 576 rows, not 600,
//                                      because the tile view needs a multiple of 32.
//   llm_gfx_view_metrics_init()        recomputes G_WIN_W/H (= window minus the 0xa0 side panel and
//                                      the 0x20 strip), pitch, tiles-per-view, panel offsets.
//   _G_LLM_VIEW_SIZE_MODE (0xae2aa8)   the PERSISTED preference: first int of the 0x58-byte block
//                                      llm_cfg_save_setup_dat LZW-writes to setup.dat, and the value
//                                      the in-game options-menu widget is bound to.
//   llm_ui_main_menu_screen_load()     copies it into _G_LLM_UI_VIEW_SIZE_MODE_CACHE, which
//                                      llm_map_view_size_mode_apply re-applies whenever gameplay
//                                      resumes (menus themselves always drop back to mode 0).
//
// So the only thing missing is an OUTSIDE-the-game way to choose the mode: setup.dat's settings block
// is LZW-compressed, so neither a test rig nor a shipped mod can edit the preference directly. This
// seam supplies exactly that and nothing more -- a run-before trampoline on the main-menu loader that
// writes _G_LLM_VIEW_SIZE_MODE moments before the loader reads it. The resulting state is
// indistinguishable from "the user picked this mode in the options menu last run": the options widget
// still edits it, and llm_cfg_save_setup_dat still persists it.
//
// Deliberately NOT done here: forcing the mode at every llm_view_set_size_mode call site. Roughly ten
// sites drop to mode 0 on purpose (dialogs, the outcome screen, tactical missions, the tutorial) and
// restore afterwards; overriding them would fight the game rather than configure it.
//
// Note for anyone tempted to make the MENUS run at the chosen mode: remapping those mode-0 requests is
// NOT sufficient. The boot main menu never calls llm_view_set_size_mode at all -- llm_game_init_subsystems
// calls llm_gfx_set_window_resolution(640,480) directly -- so nothing requests a mode until gameplay
// starts (measured 2026-07-25: with every mode-0 request remapped to 2, the menu captures were still
// byte-identical 640x480). Menus at a higher mode need the boot path forced too. See the game-mode notes.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "include/mh_video_export.h"
#include "include/mh_run_context.h"    // MH_RunDir
#include "addr/mh_addrs.gen.h"         // generated EN VAs
#include "include/mh_harness_export.h" // MH_Harness_OnMovieTick / _WantsMovieTick
#include "en_guard.h"                  // EN-only build gate
#include "hook/detour.h"               // install_trampoline / WATCOM_PROLOGUE
#include "hook/patch.h"                // patch_bytes_guarded

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

constexpr uintptr_t ADDR_MENU_LOAD  = mh::addr::llm_ui_main_menu_screen_load;
constexpr uintptr_t ADDR_SIZE_MODE  = mh::addr::_G_LLM_VIEW_SIZE_MODE;
constexpr uintptr_t ADDR_MODE_CACHE = mh::addr::_G_LLM_UI_VIEW_SIZE_MODE_CACHE;

int g_nmodes = 0;        // [video] picker list length (0 = picker not armed); declared early because
                         //     the HUD-chrome hook above the picker block needs to know it is armed
int   g_size_mode  = -1; // [video] size_mode; -1 = leave the game's persisted preference alone
int   g_custom_w   = 0;  // [video] width/height once validated + armed (0 = no custom mode)
int   g_custom_h   = 0;
void *g_menu_tramp = nullptr;

char g_log_path[MAX_PATH];
bool g_log_ready = false;

// Own log file (mh_video.log), like the debug overlay: the seam arm-log sequence in mh_net.log is
// diffed against a committed baseline by the refactor gate, so a new optional feature must not add
// lines to it.
void vid_log(const char *fmt, ...) {
    if (!g_log_ready) {
        wsprintfA(g_log_path, "%smh_video.log", MH_RunDir());
        g_log_ready = true;
    }
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)lstrlenA(line), &w, nullptr);
        CloseHandle(h);
    }
}

// Run-before the main-menu loader: seed the persisted preference so the loader's own
// `_G_LLM_UI_VIEW_SIZE_MODE_CACHE = _G_LLM_VIEW_SIZE_MODE` picks it up. The cache is written too, for
// the case where the loader has already run once in this process (it is guarded by MENU_STATE == 0,
// so a second menu entry does not re-read the preference).
void on_menu_load() {
    if (g_size_mode < 0) return;
    *(int *)ADDR_SIZE_MODE  = g_size_mode;
    *(int *)ADDR_MODE_CACHE = g_size_mode;
    vid_log("; [video] main-menu load: pinned view size_mode=%d", g_size_mode);
}

// clang-format off
__declspec(naked) void menu_load_detour() {
    __asm {
        pushad
        pushfd
        call on_menu_load
        popfd
        popad
        jmp  dword ptr [g_menu_tramp] // stolen 8-byte prologue + jmp back
    }
}
// clang-format on

// =================================================================================================
// Custom (non-stock) resolutions -- [video] width/height.
//
// Two independent obstacles, both handled here.
//
// (1) THE THREE HARDCODED IF-CHAINS. Everything between them is generic: llm_gfx_set_window_resolution
//     probes ANY DirectDraw mode (falling back to 640x480 if refused) and llm_gfx_view_metrics_init
//     derives every viewport/pitch/tile metric arithmetically from WindowWidth/WindowHeight. Only the
//     three lookup chains are closed sets, and an unlisted size fails in three different ways: the mode
//     table falls through to 640x480, the MoveWindow chain has NO else (the window is simply never
//     moved), and the snap chain discards the DirectDraw mode it just set. Rather than detour three
//     functions we REPURPOSE MODE 2: 13 immediates are rewritten so "mode 2" means the configured size.
//     Modes 0 and 1 keep their stock meaning, and the game's own options widget / F-keys / debug console
//     keep working -- picking "1024x768" now picks the custom mode.
//
//     The requested height and the RENDER height are patched separately because they legitimately
//     differ: the render height must be a multiple of the 32px tile, exactly as stock mode 1 asks
//     DirectDraw for 800x600 but renders 800x576.
//
// (2) THE ~1024-TILE CEILING. Five per-tile arrays are indexed `VIEW_TILES_W * row + col` over the whole
//     view grid and sit in fixed .bss slots big enough for 1024 tiles -- 1024x768 already uses 768 of
//     them, so anything larger overflows into neighbouring globals. relocate_view_arrays() moves them
//     into one VirtualAlloc'd block sized for MAX_TILES.
//
//     The rewrite is a self-verifying PATTERN scan rather than a hardcoded site table: every one of the
//     47 refsites encodes the array base as a plain 32-bit displacement/immediate (never base+offset),
//     and a full byte scan of every initialized block found ZERO occurrences outside those 47 (verified
//     2026-07-25 -- .bss is uninitialized so it cannot hold baked-in pointers either). So we scan the
//     one executable section for each base and require the EXACT expected hit count before writing
//     anything; a count mismatch means a different build or an already-relocated image, and we abort the
//     whole relocation rather than half-patch it.
// =================================================================================================

constexpr uintptr_t TEXT_LO = 0x00410000; // BEGTEXT -- the only executable block
constexpr uintptr_t TEXT_HI = 0x004f6400;

// 4096 tiles covers e.g. 1920x1200 (60 x 37 = 2220) with room to spare; the whole block is ~36 KB.
constexpr int MAX_TILES = 4096;
// NOTE there is deliberately NO hand-maintained size envelope here. What decides whether a custom
// resolution works is whether the DISPLAY can present that exact WxH -- see mode_is_available() below,
// which checks it at arm time. A fixed box was tried and was simply the wrong shape: 1600x1024 works
// while 2048x1152 (larger in neither... in one dimension only) does not, and the supported list is
// per-machine anyway. Verified end-to-end on the dev box at 1280x1024, 1600x1024, 1920x1440 and
// native 2560x1440 (80 x 45 = 3600 tiles -- MAX_TILES is now the nearer wall).

// The side panel is a fixed 0xa0 px = 5 tiles wide, so the surface table only grows with ROWS.
constexpr int PANEL_COLS    = 8;
constexpr int MAX_PANEL_ROW = 256;

struct ViewArray {
    uintptr_t   base;      // stock .bss base
    uint32_t    bytes;     // relocated size
    int         expect;    // exact number of refsites encoding `base` (measured in Ghidra)
    const char *name;      //
    uintptr_t   relocated; // filled in by relocate_view_arrays
};

ViewArray g_arrays[] = {
    {0x0070fc50u, MAX_TILES, 4, "LOS_CACHE", 0},
    {0x00712120u, PANEL_COLS *MAX_PANEL_ROW * 4, 6, "SURFACE_ROW_PTRS", 0},
    {0x00712920u, MAX_TILES * 4, 6, "FB_ROW_PTRS", 0},
    {0x00713920u, MAX_TILES, 12, "DRAWN_MAP", 0},
    {0x00713d20u, MAX_TILES, 19, "VIS_MAP", 0},
};
constexpr int N_ARRAYS = (int)(sizeof(g_arrays) / sizeof(g_arrays[0]));

// Count every 4-byte little-endian occurrence of `val` in the executable section, recording the sites.
int scan_text(uint32_t val, uintptr_t *out, int cap) {
    int n = 0;
    for (uintptr_t p = TEXT_LO; p + 4 <= TEXT_HI; ++p) {
        if (*(const uint32_t *)p == val) {
            if (n < cap) out[n] = p;
            ++n;
        }
    }
    return n;
}

bool relocate_view_arrays() {
    uintptr_t sites[N_ARRAYS][32];
    int       counts[N_ARRAYS];

    // Pass 1: locate + verify EVERYTHING before writing a single byte.
    for (int i = 0; i < N_ARRAYS; ++i) {
        counts[i] = scan_text((uint32_t)g_arrays[i].base, sites[i], 32);
        if (counts[i] != g_arrays[i].expect) {
            vid_log("; [video] view-array reloc ABORTED -- %s: found %d refsites, expected %d", g_arrays[i].name,
                    counts[i], g_arrays[i].expect);
            return false;
        }
    }

    uint32_t total = 0;
    for (int i = 0; i < N_ARRAYS; ++i) total += (g_arrays[i].bytes + 63u) & ~63u;
    uint8_t *blk = (uint8_t *)VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!blk) {
        vid_log("; [video] view-array reloc ABORTED -- VirtualAlloc(%u) failed", total);
        return false;
    }

    // Pass 2: commit. Every write is guarded on the old dword, so a surprise leaves that site alone.
    uint32_t off = 0;
    for (int i = 0; i < N_ARRAYS; ++i) {
        g_arrays[i].relocated = (uintptr_t)(blk + off);
        off += (g_arrays[i].bytes + 63u) & ~63u;
        uint32_t oldv = (uint32_t)g_arrays[i].base;
        uint32_t newv = (uint32_t)g_arrays[i].relocated;
        int      done = 0;
        for (int k = 0; k < counts[i]; ++k)
            if (mh::hook::patch_bytes_guarded(sites[i][k], (const uint8_t *)&oldv, (const uint8_t *)&newv, 4)) ++done;
        vid_log("; [video]   %-18s %08x -> %08x  (%u bytes, %d/%d refsites)", g_arrays[i].name, (unsigned)oldv,
                (unsigned)newv, g_arrays[i].bytes, done, counts[i]);
        if (done != counts[i]) {
            vid_log("; [video] view-array reloc INCOMPLETE for %s -- the image is now inconsistent", g_arrays[i].name);
            return false;
        }
    }
    vid_log("; [video] view-array reloc OK: %d arrays, %u bytes, ceiling now %d tiles", N_ARRAYS, total, MAX_TILES);
    return true;
}

// Guarded rewrite of one 32-bit immediate embedded at `site + off`.
bool poke_imm32(uintptr_t site, int off, uint32_t expect, uint32_t repl, const char *what) {
    if (mh::hook::patch_bytes_guarded(site + off, (const uint8_t *)&expect, (const uint8_t *)&repl, 4)) return true;
    vid_log("; [video] custom-res MISMATCH at %08x+%d (%s): expected %u, found %u", (unsigned)site, off, what, expect,
            *(const uint32_t *)(site + off));
    return false;
}

// Rewrite the 13 mode-2 immediates so "mode 2" == (w x h_req), rendered at (w x h_render).
// The 13 immediates, and the values LAST written to them. patch_mode2 seeds g_imm_now from the stock
// values; repoke_runtime() then rewrites the 11 runtime ones per mode switch, expecting what it last
// wrote (a fixed `expect` cannot work once the site has been written more than once).
uint32_t g_imm_now[16] = {0};
bool     g_imm_seeded  = false;

// boot_w/boot_h size the two ONE-SHOT boot buffers and must cover the LARGEST mode the picker can
// select -- they are allocated once in llm_game_init_subsystems and never resized, so sizing them for
// the initially-selected mode would re-create the overflow that hung 1280x800.
bool patch_mode2(int w, int h_req, int h_render, int boot_w, int boot_h) {
    namespace A = mh::addr;
    struct {
        uintptr_t   site;
        int         off;
        uint32_t    expect;
        uint32_t    repl;
        const char *what;
    } P[] = {
        {A::vres_mode2_w, 3, 0x400, (uint32_t)w, "mode2 width arg"},
        {A::vres_mode2_h, 3, 0x300, (uint32_t)h_req, "mode2 height arg"},
        {A::vres_mode2_readback, 3, 0x400, (uint32_t)w, "mode2 readback"},
        {A::vres_movewin_cmp, 3, 0x400, (uint32_t)w, "MoveWindow branch test"},
        {A::vres_movewin_h, 1, 0x300, (uint32_t)h_req, "MoveWindow height"},
        {A::vres_movewin_w, 1, 0x400, (uint32_t)w, "MoveWindow width"},
        {A::vres_snap_cmp_w, 3, 0x400, (uint32_t)w, "snap test width"},
        {A::vres_snap_cmp_h, 3, 0x300, (uint32_t)h_req, "snap test height"},
        {A::vres_snap_winw, 6, 0x400, (uint32_t)w, "WindowWidth"},
        {A::vres_snap_winh, 6, 0x300, (uint32_t)h_render, "WindowHeight (render)"},
        {A::vres_tactret_cmp, 6, 0x400, (uint32_t)w, "post-tactical test"},
        {A::vres_tactret_h, 1, 0x300, (uint32_t)h_req, "post-tactical height"},
        {A::vres_tactret_w, 1, 0x400, (uint32_t)w, "post-tactical width"},
        // The boot pair is NOT part of the mode-2 table -- it is what SIZES the two one-shot heap
        // buffers. llm_game_init_subsystems deliberately sets WindowWidth/Height to 1024x768 (the
        // largest STOCK mode) before llm_tact_gfx_init_view_surfaces allocates
        // _G_LLM_GFX_DRAW_SURFACE = (W - G_WIN_W) * H * 2 and DAT_00603b08 = G_WIN_W << 6, then drops
        // to 640x480 for the menu. A custom mode larger than 1024x768 overruns BOTH -- 1280x800 needed
        // 256000/71680 against the 245760/55296 allocated, and hung on entering gameplay (repro'd
        // 2026-07-25). Patching the boot pair to the custom size makes the allocation fit.
        {A::vres_boot_bufw, 6, 0x400, (uint32_t)boot_w, "boot buffer width"},
        {A::vres_boot_bufh, 6, 0x300, (uint32_t)boot_h, "boot buffer height"},
    };
    const int n = (int)(sizeof(P) / sizeof(P[0]));

    // Inflating WindowWidth/Height across the boot window (above) has a second, non-obvious effect:
    // llm_game_init_subsystems ALSO seeds the cursor from them -- CURSOR_X = W/2, CURSOR_Y = H/2 --
    // and llm_gfx_set_window_resolution(640,480) does NOT clamp that. Its "rescale" runs AFTER the
    // snap, so it is `pct = CURSOR_Y*100/480; CURSOR_Y = pct*480/100` -- an identity round-trip that
    // preserves the oversized value. Its last statement is llm_gfx_hittest_begin(), which reads
    // FRAMEBUFFER + CURSOR_Y*FB_PITCH + CURSOR_X*2 out of the 640x480 surface.
    //
    // Break-even is height > 960 (CURSOR_Y = H/2 > 480); past that the read is out of bounds and only
    // heap slack decides whether it faults. Measured under cdb (2026-07-25, 1280x1184): CURSOR_Y=590,
    // pitch=1280 -> offset 756480 against a 614400-byte surface, i.e. 111 rows past the end -> AV at
    // llm_gfx_hittest_begin+0x28. Height 1024 did NOT fault but was reading 28 rows past the end -- it
    // "worked" by luck, which is why this is fixed rather than merely bounded.
    //
    // Fix: NOP the four cursor stores. The globals then stay at their (verified) 0 initialisers, the
    // percentage round-trip maps 0 -> 0, and hittest_begin reads FRAMEBUFFER + 0 -- always in bounds.
    // llm_gfx_display_init re-seeds CURSOR_Y = height/2 immediately afterwards with the CORRECT 480,
    // and the input path seeds the rest on the first mouse event, so the only cost is the cursor
    // sitting at the origin for the boot frames, before anything is drawn. Crucially this leaves the
    // WindowWidth/Height inflation itself intact -- that is what sizes the two one-shot buffers.
    struct {
        uintptr_t   site;
        const char *what;
    } CUR[] = {
        {A::vres_boot_cur_mx, "boot CURSOR_MENU_X store"},
        {A::vres_boot_cur_x, "boot CURSOR_X store"},
        {A::vres_boot_cur_my, "boot CURSOR_MENU_Y store"},
        {A::vres_boot_cur_y, "boot CURSOR_Y store"},
    };
    const int     nc     = (int)(sizeof(CUR) / sizeof(CUR[0]));
    const uint8_t NOP5[] = {0x90, 0x90, 0x90, 0x90, 0x90};
    uint8_t       expect_cur[4][5];

    // Verify EVERYTHING first -- a partial rewrite would leave the chains disagreeing, or (worse)
    // inflate the boot metrics without neutralising the cursor they seed.
    for (int i = 0; i < n; ++i)
        if (*(const uint32_t *)(P[i].site + P[i].off) != P[i].expect) {
            vid_log("; [video] custom-res ABORTED -- %s at %08x+%d holds %u, expected %u", P[i].what,
                    (unsigned)P[i].site, P[i].off, *(const uint32_t *)(P[i].site + P[i].off), P[i].expect);
            return false;
        }
    for (int i = 0; i < nc; ++i) {
        const uint8_t *cur = (const uint8_t *)CUR[i].site;
        if (cur[0] != 0xa3) { // MOV [abs32], EAX -- the 5-byte accumulator form
            vid_log("; [video] custom-res ABORTED -- %s at %08x is not the expected 5-byte a3 form (%02x)",
                    CUR[i].what, (unsigned)CUR[i].site, cur[0]);
            return false;
        }
        for (int k = 0; k < 5; ++k) expect_cur[i][k] = cur[k];
    }

    for (int i = 0; i < n; ++i) {
        if (!poke_imm32(P[i].site, P[i].off, P[i].expect, P[i].repl, P[i].what)) return false;
        g_imm_now[i] = P[i].repl;
    }
    g_imm_seeded = true;
    for (int i = 0; i < nc; ++i)
        if (!mh::hook::patch_bytes_guarded(CUR[i].site, expect_cur[i], NOP5, 5)) {
            vid_log("; [video] custom-res -- FAILED to NOP %s at %08x", CUR[i].what, (unsigned)CUR[i].site);
            return false;
        }
    vid_log("; [video] boot cursor neutralised (%d stores NOPed) -- prevents the out-of-bounds "
            "hittest_begin read that the inflated boot metrics would otherwise seed",
            nc);
    return true;
}

// =================================================================================================
// The mode-change re-entrancy guard.
//
// Root-caused 2026-07-25 with a live cdb attach. `llm_gfx_apply_window_resolution` releases the
// display surfaces and THEN changes the DirectDraw mode -- and for some requested modes that mode
// change **synchronously dispatches a WM_PAINT into the game's own window proc**, which runs a
// complete game frame while the surfaces are gone. The engine re-enters itself through Windows,
// mid-teardown, and dereferences decommitted memory. Observed stack (single-threaded, so genuine
// re-entry rather than a race):
//
//   llm_gfx_hittest_check(0x4a677d) <- FUN_004a66c5 <- llm_gfx_draw_sprite_clipped_width
//     <- llm_ui_hud_topbar_tick <- llm_ui_icon_group_dispatch <- llm_ui_frame_tick
//     <- llm_strat_input_update <- llm_strat_frame <- llm_frame_dispatch
//     <- llm_wnd_proc (msg = 0x0F = WM_PAINT) <- USER32!_InternalCallWinProc ...
//
// The AV lands INSIDE llm_gfx_ddraw_set_display_mode (the probe-entry breakpoint fires, the
// probe-return one never does), so the 640x480 fallback loop is never even reached -- a mode
// *refusal* was never the story. Nor is it a size limit: 1920x1440 (60 x 45 = 2700 tiles) completes
// the whole menu -> lobby -> launch -> HUD walk, while 1280x1152, 1792x1344 and 2048x1152 re-enter
// and die. Which modes provoke the WM_PAINT is still unexplained (not height, width, tile count,
// pixel count, "classic mode", or dgVoodoo's AppControlledScreenMode -- all tested and killed).
//
// Guard shape:
//   * A COUNTER (not a bool) raised across BOTH llm_gfx_apply_window_resolution -- so the whole
//     release..recreate window is covered, including llm_gfx_display_init -- and
//     llm_gfx_set_window_resolution, which catches the BOOT call that llm_game_init_subsystems makes
//     directly rather than through apply_window_resolution. The two nest; the counter handles it.
//   * Suppression at llm_frame_dispatch, the SINGLE entry from llm_wnd_proc into a game frame and the
//     common ancestor of every route the re-entrant frame took. Guarding llm_gfx_hittest_check
//     instead is NOT sufficient: with that pointer neutralised the fault simply moved to a write
//     through _G_LLM_FRAMEBUFFER from llm_strat_render_view (proven empirically).
//
// The suppressed path still calls BeginPaint/EndPaint. Returning early without them would leave the
// update region unvalidated, so Windows would re-post WM_PAINT forever -- trading a crash for a hang
// inside the very mode-change call we are trying to complete. Skipping the frame BODY is
// semantically right: the surfaces are released, there is nothing to paint into, and
// llm_gfx_display_init plus the normal frame loop repaint immediately afterwards.
// =================================================================================================

int   g_in_mode_change    = 0; // nesting counter; > 0 = surfaces may be released, do not run a frame
long  g_frames_suppressed = 0;
void *g_awr_tramp         = nullptr;
void *g_swr_tramp         = nullptr;
void *g_fd_tramp          = nullptr;

// Validate the paint region without running a frame. Returning early WITHOUT BeginPaint/EndPaint would
// leave the update region unvalidated, so Windows would re-post WM_PAINT forever -- trading a crash for
// a hang inside the very mode-change call we are trying to complete.
void __cdecl on_suppressed_frame(HWND h) {
    // U34: SAY IT ONCE, the first time. Until now this counter was incremented and never printed, so
    // an armed guard that never fired and an armed guard doing its job produced identical logs -- the
    // vacuity shape the seam-arming lines exist to close. One line, not one per frame.
    if (++g_frames_suppressed == 1)
        vid_log("; [video] re-entrancy guard LIVE -- first WM_PAINT frame suppressed while the display "
                "surfaces were released (this is the crash it prevents, happening and being caught)");
    if (h) {
        PAINTSTRUCT ps;
        if (BeginPaint(h, &ps)) EndPaint(h, &ps);
    }
}

// clang-format off
// Wrap: raise the counter, run the original as a subroutine, lower it. __watcall args (EAX/EDX) pass
// straight through -- `inc`/`dec` on a memory operand touches neither.
//
// THE PAIRING IS UNCONDITIONAL BY CONSTRUCTION, which is what this shape has over the effects-gate
// AROUND participant it briefly shared the entry with (U34, deleted at fork F2F): the `dec` is the
// instruction after the `call`, so it runs on every path the original can return through. A `dec`
// that could be skipped would leave the counter stuck high and suppress every frame for the rest of
// the session -- the failure the participant's post half existed to avoid and this shape cannot have.
__declspec(naked) void apply_winres_detour() {
    __asm {
        inc  dword ptr [g_in_mode_change]
        call dword ptr [g_awr_tramp]
        dec  dword ptr [g_in_mode_change]
        ret
    }
}

__declspec(naked) void set_winres_detour() {
    __asm {
        inc  dword ptr [g_in_mode_change]
        call dword ptr [g_swr_tramp]
        dec  dword ptr [g_in_mode_change]
        ret
    }
}

// llm_frame_dispatch is void(HWND in EAX). On the suppressed path we never establish the stolen
// prologue's frame, so the stack is exactly as at the call site and a bare `ret` is correct.
__declspec(naked) void frame_dispatch_detour() {
    __asm {
        cmp  dword ptr [g_in_mode_change], 0
        je   run_frame
        pushad
        push eax                        // HWND, still live in EAX
        call on_suppressed_frame
        add  esp, 4
        popad
        ret
    run_frame:
        jmp  dword ptr [g_fd_tramp]
    }
}

// clang-format on

// Arms the guard. Independent of the custom-resolution machinery -- a stock size_mode switch takes
// the identical release..recreate path, so this protects those too.
// Whether the display can actually present this exact size.
//
// This -- not height, width, tile count, pixel count or "is it a classic mode" -- is what decides
// whether a custom resolution works. Measured 2026-07-25 with 10/10 correlation against the display's
// enumerable mode list: every config whose exact WxH was enumerable completed the full walk
// (1280x800, 1152x864, 1280x960, 1280x1024, 1600x1024, 1920x1440, 2560x1440), and every config whose
// was not, failed (1280x1152, 1280x1184, 1792x1344, 2048x1152).
//
// The failure mode is worth knowing: dgVoodoo puts up its own modal ("Error Setting FullScreen Mode:
// The display is currently in an unsupported mode.") and then exits. That modal runs its OWN message
// loop, which is what pumps the WM_PAINT that the guard above intercepts -- so the two findings are
// the same story seen from both ends.
//
// Checking here replaces a hand-maintained size box, which the data shows was never the right shape:
// 1600x1024 works while the larger-in-neither-dimension 2048x1152 does not, and the supported list is
// per-machine anyway (a VM enumerates something different from a desktop GPU). Match on WxH only --
// the game asks DirectDraw for 16bpp, but modern drivers enumerate 32bpp and dgVoodoo wraps it.
bool mode_is_available(int w, int h, char *near_out, int near_cap) {
    DEVMODEA dm;
    bool     found     = 0 != 0;
    int      n         = 0;
    int      seen_h[8] = {0};
    if (near_cap > 0) near_out[0] = 0;
    for (int i = 0;; ++i) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(nullptr, i, &dm)) break;
        if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h) found = 0 == 0;
        // Collect a few plausible alternatives for the refusal message: same width, 32-multiple height.
        // EnumDisplaySettings lists every size once PER REFRESH RATE (and per bpp), so dedupe on the
        // height or the hint reads "1280x768, 1280x768, 1280x768, ...".
        if (n < 6 && (int)dm.dmPelsWidth == w && (dm.dmPelsHeight % 32) == 0 && dm.dmPelsHeight >= 480) {
            bool dup = 0 != 0;
            for (int k = 0; k < n; ++k)
                if (seen_h[k] == (int)dm.dmPelsHeight) dup = 0 == 0;
            if (!dup) {
                char one[32];
                wsprintfA(one, "%s%ux%u", n ? ", " : "", dm.dmPelsWidth, dm.dmPelsHeight);
                if (lstrlenA(near_out) + lstrlenA(one) < near_cap - 1) {
                    lstrcatA(near_out, one);
                    seen_h[n++] = (int)dm.dmPelsHeight;
                }
            }
        }
    }
    return found;
}

bool install_reentrancy_guard() {
    // llm_gfx_apply_window_resolution's entry has had three owners in a row: this guard's naked
    // thunk, then a defer_entry hand-over to it (U30 -- which left the target UNGATED, because a
    // deferral's premise "the owner carries the exactly-once guarantee" is true of a promotion and
    // false of a wrap like this one), then an effects-gate AROUND participant (U34) with the thunk
    // kept as the `[effects] arm=0` fallback. Fork F2F deleted the gates (ruling D8), so the thunk
    // is the only owner again and the entry is uncontested -- which is exactly why the fallback was
    // never allowed to be optional: a re-entrancy guard that disappeared with a diagnostic seam
    // would bring back the WM_PAINT crash it exists to prevent.
    const uintptr_t AWR = mh::addr::llm_gfx_apply_window_resolution;

    struct {
        uintptr_t   site;
        void       *detour;
        void      **tramp;
        const char *what;
    } G[] = {
        {AWR, (void *)apply_winres_detour, &g_awr_tramp, "apply_window_resolution"},
        {mh::addr::llm_gfx_set_window_resolution, (void *)set_winres_detour, &g_swr_tramp, "set_window_resolution"},
        {mh::addr::llm_frame_dispatch, (void *)frame_dispatch_detour, &g_fd_tramp, "frame_dispatch"},
    };
    const int n = (int)(sizeof(G) / sizeof(G[0]));
    // U30: the two-pass shape STAYS -- checking all three before writing any is the point, since a
    // half-installed re-entrancy guard is worse than none -- but the first pass is now the
    // PRIMITIVE'S decision asked without the write, so an entry another mechanism owns is named as
    // such instead of being reported as an unexpected prologue.
    for (int i = 0; i < n; ++i)
        if (mh::hook::detour_refusal(G[i].site, mh::hook::entry_claim::exclusive,
                                     mh::hook::WATCOM_PROLOGUE) != mh::hook::refuse_reason::none) {
            vid_log("; [video] re-entrancy guard NOT armed -- entry unavailable at %s (the reason is "
                    "on the [interlock] line for that address)",
                    G[i].what);
            // Let the primitive word it and file it: with the entry unavailable it writes nothing.
            mh::hook::install_trampoline(G[i].site, G[i].detour, G[i].tramp, 8,
                                         mh::hook::entry_claim::exclusive,
                                         "the D13 WM_PAINT re-entrancy guard");
            return false;
        }
    for (int i = 0; i < n; ++i)
        if (!mh::hook::install_trampoline(G[i].site, G[i].detour, G[i].tramp, 8,
                                          mh::hook::entry_claim::exclusive,
                                          "the D13 WM_PAINT re-entrancy guard")) {
            vid_log("; [video] re-entrancy guard NOT armed -- trampoline failed at %s", G[i].what);
            return false;
        }
    vid_log("; [video] mode-change re-entrancy guard armed (WM_PAINT frames suppressed while the display "
            "surfaces are released); apply_window_resolution hosted by its own detour");
    return true;
}

// =================================================================================================
// HUD chrome widening -- the bottom status-bar extension.
//
// The strategic HUD's bottom bar is assembled from two blits: a fixed base at HUD widget 0xe (icon
// 0x42) and, for the wider modes only, an EXTENSION at widget 0xf -- `llm_ui_hud_topbar_tick` does
// `llm_ui_hud_widget_icon_blit(0xf, 0x42 + _G_LLM_VIEW_SIZE_MODE)`. Those extension bitmaps are
// pre-authored ART, one per stock mode (0x43 for 800, 0x44 for 1024), so at a custom width the widest
// one that exists still stops at the 1024-mode viewport edge and leaves a black gap before the side
// panel (measured at 1280 wide: content ends at x=863, panel starts at 1120).
//
// No art authoring and no engine change are needed, because the blit is HEADER-DRIVEN. An icon is
// simply `u16 width; u16 height; u16 pixels[w*h]` -- raw 16bpp with NO colour key -- and
// llm_gfx_blit_rect_rows reads the stride from `bitmap[0]` and memcpy's `w*2` bytes per row. Hand it a
// wider bitmap and it draws a wider bar.
//
// So: synthesize one. Keep the left half and the right end cap of the stock art and replicate a
// single middle column to fill the difference, which extends the flat part of the bar without
// duplicating its rounded end. Done once, lazily, at the first blit (by which point the icon table and
// the HUD widget rects are both populated), then cached.
// =================================================================================================

constexpr int HUD_W_EXTENSION = 0xf; // HUD widget slot of the bottom-bar extension

struct HudWidget { // llm_ui_hud_widget, 28 bytes
    void *action_callback;
    int   callback_arg;
    int   x, y, right, bottom;
    int   text_id;
};

void *g_hud_icon_tramp = nullptr;
void *g_widened_for    = nullptr; // the stock bitmap we widened (cache key)
long  g_widened_w      = 0;

// Returns the icon id the blit should ACTUALLY use.
//
// Two things go wrong for widget 0xf once the picker exists. llm_ui_hud_topbar_tick computes
// `0x42 + _G_LLM_VIEW_SIZE_MODE`, and only 0x42/0x43/0x44 are authored bottom-bar extensions -- with a
// picker index of, say, 18 it reaches icon 0x54, which is unrelated artwork (observed: a stray sprite
// fragment mid-bar and black to the panel). So clamp to the widest that exists. And the widening must
// key off "is the extension narrower than the viewport", not off the [video] width/height path, or the
// picker's own modes get an unwidened bar.
int __cdecl on_hud_icon_blit(int widget_id, int icon_id) {
    if (widget_id != HUD_W_EXTENSION) return icon_id;
    if (icon_id > 0x44) icon_id = 0x44;               // past the authored extensions -> use the widest
    if (!g_custom_w && g_nmodes == 0) return icon_id; // nothing of ours armed

    void          **icons = (void **)mh::addr::G_ICON_PTRS;
    unsigned short *src   = (unsigned short *)icons[icon_id];
    if (!src) return icon_id;
    if ((void *)src == g_widened_for) return icon_id; // already ours

    const HudWidget *hw   = (const HudWidget *)mh::addr::_G_LLM_UI_HUD_WIDGETS + HUD_W_EXTENSION;
    const int        winw = *(const int *)mh::addr::G_WIN_W; // viewport width (window minus the panel)
    const int        need = winw - hw->x;
    const int        w0 = (int)src[0], h0 = (int)src[1];
    if (w0 <= 0 || h0 <= 0 || need <= w0) return icon_id; // nothing to do (or nonsense header)

    const int       split = w0 / 2; // keep [0,split) at the left and [split,w0) as the right end cap
    unsigned short *dst =
        (unsigned short *)VirtualAlloc(nullptr, 4 + (unsigned)need * (unsigned)h0 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!dst) return icon_id;
    dst[0] = (unsigned short)need;
    dst[1] = (unsigned short)h0;

    const int fill = need - w0; // extra columns, filled by replicating column (split-1)
    for (int row = 0; row < h0; ++row) {
        const unsigned short *s = src + 2 + row * w0;
        unsigned short       *d = dst + 2 + row * need;
        for (int c = 0; c < split; ++c) *d++ = s[c];
        const unsigned short pad = s[split > 0 ? split - 1 : 0];
        for (int c = 0; c < fill; ++c) *d++ = pad;
        for (int c = split; c < w0; ++c) *d++ = s[c];
    }
    icons[icon_id] = dst;
    g_widened_for  = dst;
    g_widened_w    = need;
    vid_log("; [video] HUD bottom-bar extension icon 0x%x widened %d -> %d px (viewport %d, widget x %d)", icon_id, w0,
            need, winw, hw->x);
    return icon_id;
}

// clang-format off
// __watcall(widget_id in EAX, icon_id in EDX); run-before, then fall through to the original.
__declspec(naked) void hud_icon_blit_detour() {
    __asm {
        push ecx                    // __cdecl may clobber eax/ecx/edx; ebx/esi/edi/ebp are callee-saved
        push eax                    // save widget_id (the original wants it in EAX)
        pushfd
        push edx                    // icon_id
        push eax                    // widget_id
        call on_hud_icon_blit       // -> the icon id to actually use, in EAX
        add  esp, 8
        mov  edx, eax               // apply it
        popfd
        pop  eax                    // restore widget_id
        pop  ecx
        jmp  dword ptr [g_hud_icon_tramp]
    }
}
// clang-format on

// Stop the options menu from lying about the resolution.
//
// Because a custom size REPURPOSES mode 2, the game's own options-menu control still selects it
// correctly -- but it still renders the stock label "1024x768" while delivering e.g. 2560x1440. The
// label chain is pure data and re-pointable with a single write (verified 2026-07-25):
//
//   options widget w[6] @0x64fd53, +0x30 param_block -> spinner @0x6450c3
//     +0x00 options_table = 0x645093   (3 entries)      +0x04 count = 3
//     table[2] @0x64509b -> SLOT @0x64508f -> UTF-16 "1024x768" @0x00502c4f
//
// The extra hop exists so a localiser can rewrite the slot -- which is exactly what we do. Nothing
// validates the string; llm_ui_widget_draw_content just renders it. Guarded on the stock pointer, so
// a different build leaves the menu alone.
wchar_t g_res_label[16];

void fix_options_label(int w, int h) {
    const uint32_t expect = 0x00502c4fu; // the stock UTF-16 "1024x768"
    uint32_t       repl;
    int            n = 0;
    // Tiny formatter -- no CRT in this DLL's style, and wsprintfW would need shlwapi-ish linkage.
    int parts[2] = {w, h};
    for (int p = 0; p < 2; ++p) {
        char tmp[8];
        int  t = 0, v = parts[p];
        while (v > 0 && t < 7) {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (t > 0 && n < 15) g_res_label[n++] = (wchar_t)tmp[--t];
        if (p == 0 && n < 15) g_res_label[n++] = L'x';
    }
    g_res_label[n] = 0;
    repl           = (uint32_t)(uintptr_t)g_res_label;

    if (mh::hook::patch_bytes_guarded(mh::addr::ui_opt_res_label_slot2, (const uint8_t *)&expect,
                                      (const uint8_t *)&repl, 4))
        vid_log("; [video] options-menu resolution label repointed -> \"%dx%d\"", w, h);
    else
        vid_log("; [video] options-menu label NOT repointed -- slot %08x holds %08x, expected %08x (menu will "
                "still read \"1024x768\")",
                (unsigned)mh::addr::ui_opt_res_label_slot2, *(const uint32_t *)mh::addr::ui_opt_res_label_slot2,
                expect);
}

// =================================================================================================
// D13b -- the N-entry resolution PICKER in the game's own options menu.
//
// The options control is generic and its limits are pure DATA (verified live 2026-07-25):
//
//   options widget w[6] @0x64fd53, +0x30 param_block -> llm_ui_spinner @0x6450c3
//     +0x00 options_table -> 0x00645093   (N label-slot pointers)
//     +0x04 count         -> 3            <-- THE CLAMP
//     +0x08 ext_counter   -> &_G_LLM_VIEW_SIZE_MODE   (written by llm_ui_options_menu_open)
//
// Both consumers are generic: llm_ui_menu_pending_widget_option_advance does
// `*p += 1; while (count <= *p) *p -= count;` (modulo wrap, no hardcoded 2) and
// llm_ui_widget_draw_content indexes options_table with the same count. So a longer list is two
// guarded dword writes -- a relocated table and a new count. (Only 4 free bytes follow the stock
// table, so it must be relocated, not grown in place.)
//
// ENGINE SIDE: the index is DECOUPLED from the engine mode. Indices 0 and 1 stay the engine's own
// 640x480 / 800x576, so the ~10 `llm_view_set_size_mode(0)` sites, F5/F6/F7, the reverse-snap, the
// setup.dat round-trip and the HUD sprite arithmetic all keep working untouched. Index >= 2 is ours:
// the hook re-pokes the mode-2 immediates to that entry's size and passes 2 through, then reports the
// real index back so the preference (and setup.dat) stores it.
//
// The boot buffers are sized for the LARGEST entry, not the selected one -- see patch_mode2.
// =================================================================================================

struct ResMode {
    int w, h_req, h_render;
};
// 19 modes qualify on a 2560x1440 desktop, so 12 was not merely tight -- it truncated in ENUMERATION
// order (roughly ascending), which silently dropped the six LARGEST including the native one. Sized
// with headroom, and any overflow is now reported rather than swallowed.
constexpr int MAX_MODES = 24;
ResMode       g_modes[MAX_MODES];
int           g_pending_ix = -1; // index being applied through the mode-2 slot (-1 = none)
void         *g_vsm_tramp  = nullptr;
wchar_t       g_labels[MAX_MODES][16];
uint32_t      g_slots[MAX_MODES];
uint32_t      g_table[MAX_MODES];

int g_modes_dropped = 0;

void add_mode(int w, int h) {
    const int hr = h & ~31;
    if (w < 640 || h < 480 || (w % 32) || hr < 480) return;
    if ((w / 32) * (hr / 32) > MAX_TILES) return;
    for (int i = 0; i < g_nmodes; ++i)
        if (g_modes[i].w == w && g_modes[i].h_render == hr) return; // dedupe (one entry per SIZE)
    if (g_nmodes >= MAX_MODES) {
        ++g_modes_dropped; // never silently: reported at arm time
        return;
    }
    g_modes[g_nmodes].w        = w;
    g_modes[g_nmodes].h_req    = h;
    g_modes[g_nmodes].h_render = hr;
    ++g_nmodes;
}

// Entries 0/1 are the engine's native modes; the rest come from what the display actually offers, so
// the list can never contain a mode dgVoodoo will refuse. Ascending by area after the fixed two.
void build_mode_list(const char *ini) {
    g_nmodes        = 0;
    g_modes_dropped = 0;
    add_mode(640, 480);
    add_mode(800, 600); // renders 800x576

    // An explicit `modes=WxH,WxH,...` wins over enumeration. Enumeration is per-MACHINE, so a committed
    // UI baseline (or a mod that wants a fixed menu) cannot rely on index N meaning the same size
    // everywhere; an explicit list can. Entries are still filtered by add_mode's rules.
    char list[256];
    GetPrivateProfileStringA("video", "modes", "", list, sizeof(list), ini);
    if (list[0]) {
        int w = 0, h = 0, *cur = &w;
        for (const char *p = list;; ++p) {
            if (*p >= '0' && *p <= '9') {
                *cur = *cur * 10 + (*p - '0');
            } else if (*p == 'x' || *p == 'X') {
                cur = &h;
            } else { // separator or terminator
                if (w && h) add_mode(w, h);
                w = h = 0;
                cur   = &w;
                if (!*p) break;
            }
        }
        return;
    }

    DEVMODEA dm;
    // The CURRENT (native) mode goes in FIRST, before the bulk enumeration, so it can never be the one
    // a cap drops -- it is the entry a player is most likely to want.
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        add_mode((int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
    for (int i = 0;; ++i) {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(nullptr, i, &dm)) break;
        add_mode((int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
    }
    for (int i = 2; i < g_nmodes; ++i) // simple insertion sort, ascending by pixel count
        for (int j = i; j > 2; --j) {
            long a = (long)g_modes[j].w * g_modes[j].h_render, b = (long)g_modes[j - 1].w * g_modes[j - 1].h_render;
            if (a >= b) break;
            ResMode t      = g_modes[j];
            g_modes[j]     = g_modes[j - 1];
            g_modes[j - 1] = t;
        }
}

void fmt_label(wchar_t *out, int w, int h) {
    int n = 0, parts[2] = {w, h};
    for (int p = 0; p < 2; ++p) {
        char tmp[8];
        int  t = 0, v = parts[p];
        while (v > 0 && t < 7) {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (t > 0 && n < 15) out[n++] = (wchar_t)tmp[--t];
        if (p == 0 && n < 15) out[n++] = L'x';
    }
    out[n] = 0;
}

// Re-point the spinner at our table and widen its count. Two guarded dwords, nothing else.
bool install_picker() {
    const uint32_t exp_tbl = 0x00645093u, exp_cnt = 3u;
    for (int i = 0; i < g_nmodes; ++i) {
        fmt_label(g_labels[i], g_modes[i].w, g_modes[i].h_render); // label the RENDER size -- the truth
        g_slots[i] = (uint32_t)(uintptr_t)g_labels[i];
        g_table[i] = (uint32_t)(uintptr_t)&g_slots[i];
    }
    const uint32_t tbl = (uint32_t)(uintptr_t)g_table, cnt = (uint32_t)g_nmodes;
    if (!mh::hook::patch_bytes_guarded(mh::addr::ui_opt_res_options_table, (const uint8_t *)&exp_tbl,
                                       (const uint8_t *)&tbl, 4)) {
        vid_log("; [video] picker NOT armed -- options_table holds %08x, expected %08x",
                *(const uint32_t *)mh::addr::ui_opt_res_options_table, exp_tbl);
        return false;
    }
    if (!mh::hook::patch_bytes_guarded(mh::addr::ui_opt_res_count, (const uint8_t *)&exp_cnt, (const uint8_t *)&cnt,
                                       4)) {
        vid_log("; [video] picker NOT armed -- count holds %u, expected 3",
                *(const uint32_t *)mh::addr::ui_opt_res_count);
        return false;
    }
    vid_log("; [video] picker armed: %d entries in the options list", g_nmodes);
    if (g_modes_dropped)
        vid_log("; [video]   WARNING: %d further qualifying mode(s) did not fit MAX_MODES=%d and were "
                "DROPPED (the native mode is added first, so it is not among them)",
                g_modes_dropped, MAX_MODES);
    for (int i = 0; i < g_nmodes; ++i)
        vid_log("; [video]   [%d] %dx%d%s", i, g_modes[i].w, g_modes[i].h_render,
                i < 2 ? "  (engine-native)" : "");
    return true;
}

// Re-poke the 11 RUNTIME immediates (not the boot pair) to `m`'s size, expecting what we last wrote.
bool repoke_runtime(const ResMode &r) {
    namespace A = mh::addr;
    struct {
        uintptr_t site;
        int       off;
        int       slot;
        uint32_t  val;
    } P[] = {
        {A::vres_mode2_w, 3, 0, (uint32_t)r.w},
        {A::vres_mode2_h, 3, 1, (uint32_t)r.h_req},
        {A::vres_mode2_readback, 3, 2, (uint32_t)r.w},
        {A::vres_movewin_cmp, 3, 3, (uint32_t)r.w},
        {A::vres_movewin_h, 1, 4, (uint32_t)r.h_req},
        {A::vres_movewin_w, 1, 5, (uint32_t)r.w},
        {A::vres_snap_cmp_w, 3, 6, (uint32_t)r.w},
        {A::vres_snap_cmp_h, 3, 7, (uint32_t)r.h_req},
        {A::vres_snap_winw, 6, 8, (uint32_t)r.w},
        {A::vres_snap_winh, 6, 9, (uint32_t)r.h_render},
        {A::vres_tactret_cmp, 6, 10, (uint32_t)r.w},
        {A::vres_tactret_h, 1, 11, (uint32_t)r.h_req},
        {A::vres_tactret_w, 1, 12, (uint32_t)r.w},
    };
    const int n = (int)(sizeof(P) / sizeof(P[0]));
    for (int i = 0; i < n; ++i) {
        if (P[i].slot >= 13) continue; // boot pair: sized once for the largest mode, never re-poked
        if (g_imm_now[P[i].slot] == P[i].val) continue;
        if (!mh::hook::patch_bytes_guarded(P[i].site + P[i].off, (const uint8_t *)&g_imm_now[P[i].slot],
                                           (const uint8_t *)&P[i].val, 4))
            return false;
        g_imm_now[P[i].slot] = P[i].val;
    }
    return true;
}

// __watcall(uint mode in EAX) -> applied mode in EAX. For index >= 2 we retarget the mode-2 slot and
// pass 2 through; the cache is cleared first or the `cache == param` early-out would skip the apply
// entirely when switching between two custom entries.
extern "C" int __cdecl on_view_set_mode_pre(int m) {
    g_pending_ix = -1;
    if (!g_imm_seeded || m < 2 || m >= g_nmodes) return m < 0 ? 0 : m;
    if (!repoke_runtime(g_modes[m])) {
        vid_log("; [video] picker: re-poke FAILED for index %d -- falling back to the current mode", m);
        return 2;
    }
    *(int *)mh::addr::_G_LLM_VIEW_SIZE_MODE_CACHE = -1;
    g_pending_ix                                  = m;
    return 2;
}

extern "C" int __cdecl on_view_set_mode_post(int applied) {
    if (g_pending_ix < 0) return applied;
    int ix       = g_pending_ix;
    g_pending_ix = -1;
    return ix; // report the real index, so the preference (and setup.dat) stores it
}

// clang-format off
__declspec(naked) void view_set_size_mode_detour() {
    __asm {
        push eax
        call on_view_set_mode_pre
        add  esp, 4
        call dword ptr [g_vsm_tramp]   // original; returns the applied mode in EAX
        push eax
        call on_view_set_mode_post
        add  esp, 4
        ret
    }
}
// clang-format on

bool install_hud_chrome() {
    // U30: one call instead of a compare and a call -- the primitive distinguishes a wrong build from
    // an entry somebody else holds, which this site could not.
    if (!mh::hook::install_trampoline(mh::addr::llm_ui_hud_widget_icon_blit, (void *)hud_icon_blit_detour,
                                      &g_hud_icon_tramp, 8, mh::hook::entry_claim::exclusive,
                                      "the [video] HUD chrome widener")) {
        vid_log("; [video] HUD chrome NOT armed -- install refused (see the [interlock] line)");
        return false;
    }
    vid_log("; [video] HUD chrome armed (bottom-bar extension widened to the viewport on first blit)");
    return true;
}

} // namespace

// Declare the process DPI-aware, replacing the HIGHDPIAWARE compatibility shim.
//
// The shim is a per-exe-PATH registry entry, so it silently stops applying the moment the game is
// moved, renamed or installed on another machine -- exactly the fragility that makes a shipped copy
// "work here, not there". Setting the flag from inside the process is path-independent and travels
// with the build. Without it Windows DPI-virtualises the window: the game renders at its own
// resolution and the compositor bitmap-stretches it, which is both blurry and wrong-sized.
//
// Timing is why this lives here and not later: MH_Seam_Init (our caller) runs from DllMain, which
// for a statically-imported DLL executes before the exe's entry point -- so no window exists yet,
// which is SetProcessDPIAware's requirement.
//
// Resolved dynamically from the ALREADY-LOADED user32 (GetModuleHandle, never LoadLibrary): calling
// LoadLibrary under the loader lock is a documented deadlock, and the newer per-monitor-v2 entry
// point does not exist on older Windows. Per-monitor-v2 is preferred when present, since it also
// keeps the window correct across a monitor change; SetProcessDPIAware is the universal fallback.
void apply_dpi_awareness(const char *ini) {
    if (!GetPrivateProfileIntA("video", "dpi_aware", 1, ini)) {
        vid_log("; [video] dpi_aware=0 -- leaving DPI virtualisation to Windows (needs the HIGHDPIAWARE shim)");
        return;
    }
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return; // cannot happen for this game (it imports user32), but never assume
    typedef BOOL(WINAPI * PFN_SetCtx)(void *);
    typedef BOOL(WINAPI * PFN_SetAware)(void);
    PFN_SetCtx set_ctx = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (set_ctx && set_ctx((void *)-4)) { // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        vid_log("; [video] DPI awareness set (per-monitor v2) -- the HIGHDPIAWARE shim is not needed");
        return;
    }
    PFN_SetAware set_aware = (PFN_SetAware)GetProcAddress(u32, "SetProcessDPIAware");
    if (set_aware && set_aware())
        vid_log("; [video] DPI awareness set (system) -- the HIGHDPIAWARE shim is not needed");
    else
        vid_log("; [video] DPI awareness NOT set (API unavailable) -- keep the HIGHDPIAWARE shim");
}

// Build the exe-relative ini path. The DLL deliberately resolves its config next to the EXE rather
// than from the CWD -- that is what lets several install folders on one machine each carry their own
// configuration (parallel test lanes).
static void video_ini_path(char *ini) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);
}

// ---- headless: cut the presentation, keep the frame ----------------------------------------------
//
// The game renders in SOFTWARE into its own RGB565 framebuffer; DirectDraw is only the final push to
// screen. llm_gfx_present_flip @0x0042644a is three steps:
//
//     llm_gfx_surface_unlock(2);                     // release the renderer's lock
//     FUN_004da014(&_G_LLM_GFX_DISPLAY_MODE_DESC);   // <-- the ONLY display work
//     llm_gfx_surface_lock(2);                       // re-lock so the next frame can compose
//
// and that middle call is pure presentation: it guards three COM pointers, checks surface-lost, then
// takes either the windowed path (GetClientRect/ClientToScreen -> IDirectDrawSurface vtable +0x14 =
// Blt) or the fullscreen path (+0x2c = Flip). It touches NO game state and llm_gfx_present_flip
// ignores its return. So NOPing the 5-byte CALL removes presentation and nothing else.
//
// SCREENSHOTS ARE UNAFFECTED BY CONSTRUCTION: gfx_capture hooks llm_gfx_present_flip's ENTRY and
// reads _G_LLM_FRAMEBUFFER while the surface is still locked and the frame fully composed -- i.e.
// UPSTREAM of this cut. The unlock/lock pair is deliberately kept: it is what maintains the lock the
// software renderer depends on, and dropping it would break the next frame, not speed it up.
//
// WHAT THIS IS NOT: the framebuffer IS locked DirectDraw surface memory, so the device, the window
// and the surfaces are still created. This removes the PRESENT, not the graphics stack.
//
// CORRECTNESS RUNS ONLY. With no blit there is no vsync wait, so frames get faster -- which is the
// point for throughput, and is exactly what makes it wrong for PACING measurement
// (tools/mp_pacing_report.py, the adaptive lookahead controller). Same rule as parallel lanes:
// The parallel-lane notes.
static void apply_no_present(const char *ini) {
    if (!GetPrivateProfileIntA("video", "no_present", 0, ini)) return;

    auto *site = reinterpret_cast<uint8_t *>(mh::addr::gfx_present_blit_site);
    // Expected-bytes guard, as everywhere else in this DLL: E8 9E 3B 0B 00 = CALL rel32 -> 0x004da014.
    static const uint8_t expect[5] = {0xE8, 0x9E, 0x3B, 0x0B, 0x00};
    if (memcmp(site, expect, sizeof expect) != 0) {
        vid_log("; [video] no_present NOT applied -- blit call site bytes differ (wrong build?)");
        return;
    }
    DWORD prot = 0;
    if (!VirtualProtect(site, sizeof expect, PAGE_EXECUTE_READWRITE, &prot)) {
        vid_log("; [video] no_present NOT applied -- VirtualProtect failed");
        return;
    }
    memset(site, 0x90, sizeof expect); // 5x NOP
    VirtualProtect(site, sizeof expect, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof expect);
    vid_log("; [video] no_present=1 -- HEADLESS: blit cut at 0x%08x, frames still composed (capture works)",
            (unsigned)mh::addr::gfx_present_blit_site);
}

// ---- the offscreen keeper: run every frame IN PLACE OF the blit ----------------------------------
//
// Clearing WS_VISIBLE / WS_EX_TOPMOST and cutting WinMain's ShowWindow is not enough, because the
// dgVoodoo DDraw wrapper MAPS the window itself after the game creates it -- and patching the exe's
// IAT cannot reach that, since the wrapper's user32 imports live in DDraw.dll's own import table
// (measured 2026-07-28: window class "Mission: Humanity Class", visible and stable for a whole run).
//
// Rather than inline-hooking user32!ShowWindow, we reuse the instruction we are ALREADY rewriting:
// llm_gfx_present_flip's `CALL FUN_004da014`. With no_present that call becomes a NOP; with
// no_window it is re-pointed at this stub instead. That makes the keeper per-frame by construction
// -- no thread (DllMain must not spawn one), no second hook to collide with gfx_capture's present
// hook, and no ordering question about who runs first.
//
// SWP_NOACTIVATE is the part that matters for "it steals focus": moving a window normally does not
// activate it, but saying so explicitly keeps a later dgVoodoo SetWindowPos from being the thing that
// does. HWND_BOTTOM drops it behind everything else as well.
namespace {
constexpr int OFFSCREEN_XY = -32000; // asked-for position: well outside any virtual desktop
// WINDOWS CLAMPS IT. Measured: asking for -32000 lands the window at -25600, so an "already parked"
// test against OFFSCREEN_XY can never be true and the keeper would re-issue SetWindowPos every single
// frame. Compare against a threshold BELOW the clamped value instead -- then the call happens once and
// the per-frame cost is one GetWindowRect. (Re-asserting is still automatic if anything moves it back,
// which is the whole reason this lives on the present path rather than running once.)
constexpr int OFFSCREEN_SEEN = -20000;

// PARKING IS NOT HIDING, and that distinction is the whole 2026-07-28 focus bug: SetWindowPos with
// SWP_NOACTIVATE moves a window without activating it, but it equally does not DEACTIVATE one that is
// already focused -- and by the time the first frame runs, the dgVoodoo wrapper has long since mapped
// and activated the window. So the keeper also HIDES it: a window without WS_VISIBLE cannot hold the
// foreground, cannot be clicked, and is not a valid SetCursorPos/capture target. The park stays as the
// belt to that brace (anything that re-shows it finds it nowhere visible).
int           g_keeper_acts     = 0; // total interventions -- how busy the thing that keeps re-showing it is
int           g_keeper_reports  = 0; // logged interventions (capped: this runs per frame)
constexpr int KEEPER_REPORT_MAX = 8;

// [video] no_window_hide -- whether the keeper also HIDES the window or only parks it off-screen.
//
// Hiding is not free, and the price is severe: this game's frames are driven by WM_PAINT
// (llm_frame_dispatch is the SINGLE entry from llm_wnd_proc into a frame), and Windows does not paint
// an invisible window. Measured 2026-07-28, same menu_walk script, same machine:
//
//     visible : 83497 presents over  9.7s = 8591 fps, ZERO stalls
//     hidden  : 68102 presents over 79.8s =  854 fps, 40 stalls totalling 73.8s -- 92% of the run
//
// Identical work, 8x the wall clock, because the process sits in ~2 s waits for messages a hidden
// window never gets. That is a rig-wide throughput tax on every headless run, so hiding is an opt-in
// with a stated cost rather than the default.
int g_hide_window     = 0;
int g_no_window_armed = 0; // gates the KEEPER rider on the shared movie tick (see movie_tick_riders)

// Declared here rather than beside their stubs (further down this file) only because keep_offscreen
// logs them and runs first in translation order.
int g_swallowed_cursor = 0;
int g_swallowed_fg     = 0;
int g_swallowed_cap    = 0;
int g_swallowed_show   = 0;
int g_swallowed_move   = 0;

void keep_offscreen() {
    HWND h = *reinterpret_cast<HWND *>(mh::addr::hWnd_main);
    if (!h) return;
    // MEASURE VISIBILITY UNCONDITIONALLY; only the ACTION is gated on g_hide_window. Until 2026-08-02
    // this read `g_hide_window != 0 && IsWindowVisible(h)`, which folded a policy switch into an
    // observation: with hiding off (the default, because it costs 8x wall clock) `was_visible` was
    // false BY CONSTRUCTION, so the log below could never print " VISIBLE" no matter what was on
    // screen. Every "window was on-screen" line in every log to date is therefore silent about the
    // only thing a reader wants from it -- whether the window was actually mapped, or merely sitting
    // invisible at x=0. Same class as the gates that pass vacuously: the instrument could not report
    // the finding it existed to report.
    const bool was_visible = IsWindowVisible(h) != 0;
    if (was_visible && g_hide_window) ShowWindow(h, SW_HIDE);
    RECT       r;
    const bool moved = GetWindowRect(h, &r) && r.left > OFFSCREEN_SEEN;
    if (moved)
        SetWindowPos(h, HWND_BOTTOM, OFFSCREEN_XY, OFFSCREEN_XY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!moved) return; // nothing to do -- the steady state, one GetWindowRect
    ++g_keeper_acts;
    // WHEN it reappears is the diagnostic: the arm log alone cannot say whether something maps the
    // window once at device init or keeps doing it, and the UI suite can never tell us (it compares
    // captures and never looks at windows). Report the first few with the game mode attached.
    //
    // THE HWND IS LOGGED because "moved back to x=0" and "replaced by a NEW window at x=0" are
    // indistinguishable from position alone, and they have completely different fixes. A changing
    // handle means the resolution path destroys and re-creates the window (the exe imports both
    // CreateWindowExA and DestroyWindow), in which case the CreateWindowExA style patch is being
    // applied to a window that no longer exists.
    if (g_keeper_reports < KEEPER_REPORT_MAX) {
        ++g_keeper_reports;
        // The swallow counters ride along because "the game called ShowWindow and we blocked it, yet
        // the window is visible anyway" and "the game never called ShowWindow, so the swallow was
        // inert and the show came from somewhere else" are the two live hypotheses, and they are
        // distinguished by a number nobody was printing.
        vid_log("; [video] keeper #%d: window %s at x=%d -- parked%s (mode %d, hwnd=%p, swallowed show=%d move=%d fg=%d)",
                g_keeper_acts, was_visible ? "VISIBLE" : "invisible", (int)r.left,
                g_hide_window ? " + hidden" : "",
                (int)*reinterpret_cast<const uint8_t *>(mh::addr::_G_LLM_GAME_MODE), (void *)h,
                g_swallowed_show, g_swallowed_move, g_swallowed_fg);
        if (g_keeper_reports == KEEPER_REPORT_MAX)
            vid_log("; [video] keeper: further interventions not logged (per-frame)");
    }
}

// ---- [video] fps_cap: bound the headless frame rate --------------------------------------------
//
// WHY (2026-09-10, user): with no_present there is no vsync wait, so a lane spins the frame loop as
// fast as a core allows -- measured 800-14,000 fps in the UI suite's menu/wait phases. For a SOLO
// capture test that speed is the point (menus advance per present). For a MULTI-PEER test it buys
// nothing: the peers spend their lives waiting on each other (lobby handshakes) or on the lockstep
// clock ([net] lockstep_step_ms -- wall time), so the spin converts whole cores into heat while the
// concurrent gate units queue for them. The cap converts that wait back into Sleep.
//
// OWN SECTION [pacing], not [video]: per-test opt-in arrives through ui_test --extra-ini, and the
// profile API reads only the FIRST section with a given name -- a fragment appending a second
// [video] block would be silently dead against the [video] the lane ini already carries.
//
// COARSE ON PURPOSE. Sleep()'s default granularity is ~15.6 ms, so a cap of 60 lands anywhere in
// ~40-65 fps. The goal is freeing cores, not hitting a frame rate; taking timeBeginPeriod(1) for
// precision would raise the machine's timer rate for every process on the box -- the opposite of
// the point. PACING-MEASUREMENT runs must not use this, same rule as no_present itself.
int g_fps_cap = 0;

void pace_frame() {
    static LARGE_INTEGER freq = {};
    static LONGLONG      next = 0;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const LONGLONG period = freq.QuadPart / (g_fps_cap > 0 ? g_fps_cap : 60);
    LARGE_INTEGER  now;
    QueryPerformanceCounter(&now);
    if (next == 0) next = now.QuadPart;
    const LONGLONG wait = next - now.QuadPart;
    if (wait > 0) {
        const DWORD ms = (DWORD)(wait * 1000 / freq.QuadPart);
        if (ms) Sleep(ms);
        QueryPerformanceCounter(&now);
    }
    // No debt: a slow frame moves the schedule forward from NOW rather than sprinting to catch up
    // (a catch-up burst is exactly the spin this exists to remove).
    next = (now.QuadPart > next + period) ? now.QuadPart + period : next + period;
}

// The single C target of the present-site stub. Each rider gates itself: the keeper on no_window,
// the pacer on its cap -- so one stub serves any combination and arming order stops mattering.
void present_site_tick() {
    if (g_no_window_armed) keep_offscreen();
    if (g_fps_cap > 0) pace_frame();
}

// Replaces `CALL FUN_004da014` (a __watcall taking the display-mode desc in EAX whose return the
// caller ignores). Naked + full register preservation: we are standing in for a function the compiler
// believes may clobber the usual scratch set, and the surrounding code is the original's.
__declspec(naked) void blit_replacement() {
    __asm {
        pushad
        pushfd
        call present_site_tick
        popfd
        popad
        ret
    }
}

// Re-point the (already NOPed) blit call at blit_replacement. Shared by apply_no_window and
// apply_fps_cap -- whichever runs first installs it, the second finds it installed. Returns
// false when the site is not the expected 5xNOP (i.e. no_present did not run, or someone else
// owns the bytes).
static bool arm_present_stub() {
    auto *site = reinterpret_cast<uint8_t *>(mh::addr::gfx_present_blit_site);
    if (site[0] == 0xE8) {
        const int32_t rel = *reinterpret_cast<const int32_t *>(site + 1);
        if (mh::addr::gfx_present_blit_site + 5 + rel ==
            reinterpret_cast<uintptr_t>(&blit_replacement))
            return true; // already ours
    }
    if (site[0] != 0x90) return false; // no_present has not freed the site
    const uintptr_t target = reinterpret_cast<uintptr_t>(&blit_replacement);
    const int32_t   rel    = (int32_t)(target - (mh::addr::gfx_present_blit_site + 5));
    DWORD           prot   = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &prot)) return false;
    site[0] = 0xE8;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

static void apply_fps_cap(const char *ini) {
    // F2G: the key moved out of its own one-key `[pacing]` section into `[video]`, beside the
    // `no_present=1` it already requires -- the two are one decision, split across two sections
    // only by history. A `[pacing]` section is REFUSED from mh::config now.
    const int cap = GetPrivateProfileIntA("video", "fps_cap", 0, ini);
    if (cap <= 0) return;
    if (!arm_present_stub()) {
        vid_log("; [video] fps_cap=%d NOT armed -- needs [video] no_present=1 (a visible run "
                "paces through the dgVoodoo FPSLimit instead)",
                cap);
        return;
    }
    g_fps_cap = cap;
    vid_log("; [video] fps_cap=%d -- present path pacer armed (coarse: Sleep granularity)", cap);
}

// ---- the SECOND presentation path: the movie tick -------------------------------------------
//
// The keeper above rides on llm_gfx_present_flip's blit call, and that is not the only way this game
// puts pixels on screen. llm_ui_menu_async_tick (0x004c240c) -- the callback llm_game_boot_init
// installs to drive intro\LOGO.AVI under mode 7 -- decodes, blits and flips through its OWN
// llm_gfx_surface_lock/unlock + IDirectDrawSurface vtable calls and NEVER calls present_flip
// (verified from its callee list). So for the whole intro movie BOTH headless mechanisms are absent:
// no_present's cut is not on that path, and the keeper riding on it never runs.
//
// That is why the window kept coming back on launch. Fix in kind: arm the same idempotent keeper as a
// run-before trampoline on the movie tick, so the movie frames police the window exactly like normal
// frames do. Run-before (not wrap) -- we add to the tick, we do not change it.
void *g_avi_tramp = nullptr;

// TWO RIDERS ON ONE TRAMPOLINE, because there is only one entry to claim and both reasons are real.
// The keeper is this file's (a movie frame must police the window like any other frame); the harness
// wants the same anchor for [harness] skip_intro_avi, and for the same underlying fact -- a screen
// driven by this tick is invisible to every per-present hook in the DLL. Each rider is gated on its
// OWN reason: the keeper must not run in a visible session (it would park and hide the window the
// player is watching), and the harness half is inert unless its ini key is set.
void movie_tick_riders() {
    if (g_no_window_armed) keep_offscreen();
    MH_Harness_OnMovieTick();
}

// clang-format off
__declspec(naked) void avi_tick_detour() {
    __asm {
        pushad
        pushfd
        call movie_tick_riders
        popfd
        popad
        jmp  dword ptr [g_avi_tramp]
    }
}
// clang-format on
} // namespace

// ---- swallow the user32 calls that drag the process back to the foreground ------------------------
//
// Five of the exe's user32 imports are actively hostile to an unattended run, and all five are the
// GAME's own calls:
//
//   SetCursorPos        THE cursor thief, and it has two callers. llm_gfx_display_init warps the
//                       PHYSICAL cursor to (width/2, height/2) on every display init -- on a large
//                       desktop that is a jump into the top-left corner -- and
//                       llm_input_mouse_delta_pump re-warps it to the game's internal cursor on EVERY
//                       frame whose OS cursor is hidden (the warp-to-anchor technique it measures
//                       deltas off). Purely an output: nothing reads the OS cursor position back, so
//                       swallowing it cannot change what the game sees.
//   SetForegroundWindow WinMain's single-instance bail plus the three error/message dialogs.
//   SetCapture          llm_gfx_apply_window_resolution grabs the physical mouse to the window.
//   ShowWindow          NINE call sites. Only ONE (WinMain's, at 0x004a0c44) is NOPed by the byte
//                       patch below; the other eight are live, and four of them are in the
//                       0x004b50xx resolution-change block.
//   MoveWindow          SIX call sites, none of them previously touched -- including 0x004b516a in
//                       that same resolution-change block, which is what puts the window back at
//                       x=0.
//
// WHY THE LAST TWO WERE ADDED (2026-08-02), and the hypothesis it replaced. The comment here used to
// say the wrapper "resolves its user32 elsewhere -- that half is the keeper's job", and the note over
// apply_no_window named the dgVoodoo DDraw wrapper as the prime suspect for the window still being
// mapped. **That is refuted.** dgVoodoo's DDraw.dll imports exactly ONE user32 function, `GetDCEx`;
// it has no delay-import table and the only API-name string in the whole file is `GetProcAddress`
// itself, so it is not resolving window calls dynamically either. It never shows or moves the window.
//
// The game does. The evidence was already sitting in every run's `mh_video.log`: the keeper reports
// `window was on-screen -- parked (mode N, x=0)` repeatedly -- eight times in a two-peer lane, enough
// to hit its own report cap -- and the mode changes between reports. Something re-centres the window
// on each display-mode change, and the exe imports `MoveWindow` while dgVoodoo does not. Swallowing
// both makes the keeper's job vestigial instead of per-frame.
//
// THE KEEPER STILL WORKS, and the reason is worth stating because it looks like a bug: `iat_replace`
// patches the EXE's import table (`GetModuleHandleA(nullptr)`). `keep_offscreen` below is code in
// mh.dll, so its own `ShowWindow`/`SetWindowPos` calls go through MH.DLL's IAT, which nothing here
// touches. Swallowing ShowWindow for the game does not disarm the keeper. Do not "fix" this by
// widening iat_replace to every module -- that would silence the keeper too.
//
// Patching the IAT is the right instrument here rather than the per-call-site NOPs used everywhere
// else in this file: every call is an `FF 15 [slot]` through the import table, so ONE slot write
// covers all fifteen sites, needs no expected-bytes guard per site, and keeps working if a site
// moves. Safe from DllMain: the loader snaps every import in the graph BEFORE it calls any DllMain,
// so the slots are already resolved and nothing re-writes them afterwards.
namespace {
BOOL WINAPI no_SetCursorPos(int, int) {
    ++g_swallowed_cursor;
    return TRUE;
}
BOOL WINAPI no_SetForegroundWindow(HWND) {
    ++g_swallowed_fg;
    return TRUE;
}
HWND WINAPI no_SetCapture(HWND) {
    ++g_swallowed_cap;
    return nullptr;
}
// Returns the PREVIOUS visibility, and "hidden" is the honest answer for a window we are keeping
// unmapped. No caller in this binary branches on it (all nine sites discard the result), so this is
// about not lying rather than about behaviour.
BOOL WINAPI no_ShowWindow(HWND, int) {
    ++g_swallowed_show;
    return FALSE;
}
// TRUE = "moved", so a caller that checks does not treat the no-op as a failure. Nothing reads the
// rect back to derive geometry on the headless path -- the windowed blit (GetClientRect/ClientToScreen)
// is exactly what no_present cuts, and no_window is only ever emitted together with no_present.
BOOL WINAPI no_MoveWindow(HWND, int, int, int, int, BOOL) {
    ++g_swallowed_move;
    return TRUE;
}

// Overwrite one named import of the EXE (not of this DLL) and return the previous value, or null if
// the import is not there to patch.
void *iat_replace(const char *dll_name, const char *fn_name, void *repl) {
    auto      *base = reinterpret_cast<uint8_t *>(GetModuleHandleA(nullptr));
    const auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return nullptr;
    for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + rva); d->Name; ++d) {
        if (lstrcmpiA(reinterpret_cast<const char *>(base + d->Name), dll_name) != 0) continue;
        // Names come from OriginalFirstThunk (the untouched name table); the writable slots are
        // FirstThunk. Walk them in lockstep. A bound-only import has no name table -- skip it.
        if (!d->OriginalFirstThunk) continue;
        auto name_thunk = reinterpret_cast<const IMAGE_THUNK_DATA32 *>(base + d->OriginalFirstThunk);
        auto slot       = reinterpret_cast<void **>(base + d->FirstThunk);
        for (; name_thunk->u1.AddressOfData; ++name_thunk, ++slot) {
            if (name_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue; // imported by ordinal
            const auto by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + name_thunk->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<const char *>(by_name->Name), fn_name) != 0) continue;
            DWORD prot = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &prot)) return nullptr;
            void *old = *slot;
            *slot     = repl;
            VirtualProtect(slot, sizeof(void *), prot, &prot);
            return old;
        }
    }
    return nullptr;
}
} // namespace

// ---- true headless: never show a window at all ---------------------------------------------------
//
// `[video] no_present=1` cuts the BLIT but the window is still created -- it takes focus, sits on top
// of everything (WS_EX_TOPMOST) and swallows mouse movement, which makes a machine running the suite
// unusable. Four byte patches (plus the IAT swallows and the keeper below) make the process
// genuinely windowless:
//
//   0x004a079b  PUSH 0x96000000  dwStyle = WS_POPUP|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN
//               -> 0x86000000, i.e. WS_VISIBLE cleared. This one is REQUIRED and is why suppressing
//               ShowWindow alone is not enough: WS_VISIBLE makes the window appear AT CreateWindowExA,
//               before anybody calls ShowWindow.
//   0x004a07ac  PUSH 0x8         dwExStyle = WS_EX_TOPMOST -> 0, the always-on-top/focus-stealing bit.
//   0x004a0c43  CALL [ShowWindow] in WinMain -- NOPed, or it would show the window we just hid.
//   0x004a0c12  CALL ShowSplashScreen in WinMain -- NOPed: a SECOND top-level window (class
//               'tgl_splash'), created WS_VISIBLE, shown, held for a 500 ms Sleep. The main-window
//               patches cannot touch it.
//
// The window still EXISTS (the framebuffer is locked DirectDraw surface memory, so the device and its
// surfaces are still needed) -- it is simply never mapped. Captures are unaffected for the same reason
// no_present does not affect them: the frame is composed in software upstream of any of this.
// The MOVIE path presents without present_flip (see avi_tick_detour), so anything that must run
// during an intro screen needs its own anchor here. ONE trampoline, TWO possible reasons to arm it --
// the no_window keeper, and [harness] skip_intro_avi -- so it is installed outside apply_no_window,
// which only knows about the first. Claiming this entry twice is not possible (entry_claim::exclusive)
// and was the reason the harness could not simply install its own.
static void install_movie_tick(const char *ini) {
    const bool want_keeper  = GetPrivateProfileIntA("video", "no_window", 0, ini) != 0;
    const bool want_harness = MH_Harness_WantsMovieTick() != 0;
    if (!want_keeper && !want_harness) return;
    const bool ok = mh::hook::install_trampoline(mh::addr::llm_ui_menu_async_tick, (void *)avi_tick_detour,
                                                 &g_avi_tramp, 8, mh::hook::entry_claim::exclusive,
                                                 "the movie-tick riders (no_window keeper / harness)");
    vid_log("; [video] movie tick %s (keeper=%d harness=%d)",
            ok ? "armed" : "NOT ARMED -- intro screens run unhooked", (int)want_keeper, (int)want_harness);
}

static void apply_no_window(const char *ini) {
    if (!GetPrivateProfileIntA("video", "no_window", 0, ini)) return;
    g_no_window_armed = 1;
    // Off by default -- see g_hide_window: hiding costs 8x wall clock because WM_PAINT drives frames.
    g_hide_window = GetPrivateProfileIntA("video", "no_window_hide", 0, ini);

    struct patch {
        uintptr_t   va;
        const char *what;
        int         len;
        uint8_t     expect[8];
        uint8_t     with[8];
    };
    const patch P[] = {
        {mh::addr::gfx_main_window_style_site, "WS_VISIBLE", 5, {0x68, 0x00, 0x00, 0x00, 0x96}, {0x68, 0x00, 0x00, 0x00, 0x86}},
        {mh::addr::gfx_main_window_exstyle_site, "WS_EX_TOPMOST", 2, {0x6A, 0x08}, {0x6A, 0x00}},
        {mh::addr::gfx_main_window_show_site, "ShowWindow", 7, {0x2E, 0xFF, 0x15, 0x88, 0x02, 0x07, 0x01}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
        // The SPLASH: a second top-level window nobody had looked at. WinMain calls ShowSplashScreen
        // before anything else, and that function creates its own 'tgl_splash' popup with WS_VISIBLE
        // ALREADY IN THE STYLE, shows it, and Sleeps 500 ms holding the foreground. None of the three
        // main-window patches above can reach it -- different window, different class, created and
        // destroyed inside one call. Cut the call; the callee ends in a bare RET (no stack args), so
        // 5 NOPs leave the stack balanced. Costs the launch nothing but the half-second Sleep.
        {mh::addr::winmain_splash_call_site, "splash", 5, {0xE8, 0x2C, 0xFE, 0xFF, 0xFF}, {0x90, 0x90, 0x90, 0x90, 0x90}},
    };
    for (const patch &p : P) {
        auto *site = reinterpret_cast<uint8_t *>(p.va);
        if (memcmp(site, p.expect, p.len) != 0) {
            vid_log("; [video] no_window: %s site bytes differ -- NOT patched (wrong build?)", p.what);
            return; // all-or-nothing: a half-hidden window is worse than a visible one
        }
    }
    for (const patch &p : P) {
        auto *site = reinterpret_cast<uint8_t *>(p.va);
        DWORD prot = 0;
        if (!VirtualProtect(site, p.len, PAGE_EXECUTE_READWRITE, &prot)) {
            vid_log("; [video] no_window: VirtualProtect failed at %s", p.what);
            return;
        }
        memcpy(site, p.with, p.len);
        VirtualProtect(site, p.len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), site, p.len);
    }
    // Deliberately NOT worded as "headless achieved": measured 2026-07-28, the window is STILL
    // mapped after all four patches land, so something later shows it. Say what was PATCHED, not what
    // was accomplished; a log line that claims more than it verified is how a half-working feature
    // gets treated as done.
    //
    // WHO shows it WAS ALREADY ANSWERED, IN THIS REPO, ON 2026-07-28 -- read
    // The parallel-lane notes before adding anything to the sequence below. It says it plainly: "the
    // dgVoodoo DDraw wrapper maps the window itself afterwards -- and patching the *exe's* IAT cannot
    // reach it, since the wrapper's user32 imports live in DDraw.dll's own import table", and
    // make_lane.py's tame_dgvoodoo repeats it. The 2026-08-02 sequence below re-derived that from
    // scratch, contradicted it twice, and arrived back at it. One `grep -rn dgVoodoo docs/` would
    // have cost seconds. Kept in full anyway, because the two wrong turns are each instructive and
    // because the measurements are new even where the conclusion is not:
    //
    //   1. This comment named the dgVoodoo wrapper as prime suspect for four days on purely
    //      structural grounds ("it owns presentation and routinely activates the target window"),
    //      never checked. A named suspect reads as a diagnosis.
    //   2. Checking the DLL's imports (one user32 function, GetDCEx; no delay imports; no window-API
    //      name strings) was then read as EXONERATING it, and the game's own resolution-change block
    //      -- ShowWindow x9, MoveWindow x6 -- was blamed instead. Also wrong: with both swallowed at
    //      the exe IAT the counters read show=0 move=0 fg=0 for a whole run. The game never calls
    //      them. The swallow below is retained as hygiene and fixes nothing here.
    //   3. DDraw.dll IS dgVoodoo (the polygon carries dgVoodoo.conf) and is not a shim in front of a
    //      real ddraw -- it IS the DirectDraw implementation. So step 2's evidence proves only that
    //      its window handling cannot be reached by patching an import table, not that it has none.
    //      Absence of a static import is not absence of behaviour.
    //
    // The keeper below still catches it once per display-mode change, which is the detector. The
    // rig's actual answer is not in this file: tools/ui_test.py --desktop runs the game on its own
    // Windows desktop object, which is correct without knowing any of the above.

    // The game's own foreground/cursor/window grabs, killed at the import table rather than per call
    // site. ShowWindow and MoveWindow are the 2026-08-02 additions -- see the block comment above for
    // why dgVoodoo was the wrong suspect and why this does not disarm the keeper.
    const bool sw_cur  = iat_replace("user32.dll", "SetCursorPos", (void *)&no_SetCursorPos) != nullptr;
    const bool sw_fg   = iat_replace("user32.dll", "SetForegroundWindow", (void *)&no_SetForegroundWindow) != nullptr;
    const bool sw_cap  = iat_replace("user32.dll", "SetCapture", (void *)&no_SetCapture) != nullptr;
    const bool sw_show = iat_replace("user32.dll", "ShowWindow", (void *)&no_ShowWindow) != nullptr;
    const bool sw_move = iat_replace("user32.dll", "MoveWindow", (void *)&no_MoveWindow) != nullptr;
    if (sw_cur && sw_fg && sw_cap && sw_show && sw_move) {
        vid_log("; [video] no_window: SetCursorPos + SetForegroundWindow + SetCapture + ShowWindow + "
                "MoveWindow swallowed (exe IAT)");
    } else {
        // Partial is worth a loud line: it means the import table is not what we read it to be, and
        // whichever one failed is a symptom that will look like the feature not working.
        vid_log("; [video] no_window: IAT swallow INCOMPLETE -- SetCursorPos=%d SetForegroundWindow=%d "
                "SetCapture=%d ShowWindow=%d MoveWindow=%d",
                (int)sw_cur, (int)sw_fg, (int)sw_cap, (int)sw_show, (int)sw_move);
    }


    // Something maps the window regardless of the style patches, so ALSO re-point the blit call at
    // the offscreen keeper. Requires no_present (which is what freed that call site); if the caller
    // did not ask for it, say so rather than silently leaving the window on screen.
    //
    // KEEP THIS EVEN NOW THAT ShowWindow/MoveWindow ARE SWALLOWED. The swallow should make the keeper
    // vestigial, and the keeper's own log line is how we find out whether it did -- an intervention
    // count that stays above zero means a path nobody has enumerated is still mapping the window.
    // Removing the keeper because the swallow "should" cover it would delete the only detector.
    if (arm_present_stub()) {
        // The movie-tick half is armed separately now (install_movie_tick, which serves the
        // harness too) and logs its own line, so this one stops claiming to know about it.
        vid_log("; [video] no_window=1 -- WS_VISIBLE + WS_EX_TOPMOST cleared, ShowWindow + splash cut, "
                "hide/park keeper armed on the present path");
        return;
    }
    vid_log("; [video] no_window=1 -- window styles patched, but the PRESENT-PATH KEEPER is NOT armed "
            "(needs [video] no_present=1); the wrapper will still map the window");
}

// See the header: this is the part that must NOT sit behind the [net] enable gate.
extern "C" void MH_Video_ApplyProcessAttrs(void) {
    static bool done = false;
    if (done) return;
    done = true;
    if (!mh::en_build_ok()) return; // EN-only
    char ini[MAX_PATH];
    video_ini_path(ini);
    apply_dpi_awareness(ini); // before any window exists (we are inside DllMain)
    apply_no_present(ini);    // headless: cut the blit, keep the composed frame
    apply_no_window(ini);     // true headless: never map a window at all
    apply_fps_cap(ini);       // [video] fps_cap: bound the headless spin (multi-peer lanes)
    install_movie_tick(ini);  // the one trampoline both the keeper and [harness] skip_intro_avi need
}

extern "C" int MH_Video_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only

    char ini[MAX_PATH];
    video_ini_path(ini);

    MH_Video_ApplyProcessAttrs(); // idempotent -- normally already done before the [net] gate

    char nearby[128];
    g_size_mode = GetPrivateProfileIntA("video", "size_mode", -1, ini);

    // A custom size takes over mode 2 and implies it: [video] width/height is the "or higher" half of
    // D13. Both must be given; everything below is validated BEFORE anything is written.
    int cw = GetPrivateProfileIntA("video", "width", 0, ini);
    int ch = GetPrivateProfileIntA("video", "height", 0, ini);
    if (cw > 0 || ch > 0) {
        int h_render = ch & ~31; // the render height must be a whole number of 32px tiles
        int tiles    = (cw / 32) * (h_render / 32);
        if (cw <= 0 || ch <= 0) {
            vid_log("; [video] custom res IGNORED -- give BOTH width and height (got %d x %d)", cw, ch);
        } else if (cw < 640 || ch < 480) {
            vid_log("; [video] custom res IGNORED -- %dx%d is below the 640x480 the UI is authored for", cw, ch);
        } else if (cw % 32) {
            vid_log("; [video] custom res IGNORED -- width %d is not a multiple of the 32px tile", cw);
        } else if (tiles > MAX_TILES) {
            vid_log("; [video] custom res IGNORED -- %dx%d needs %d tiles, ceiling is %d", cw, h_render, tiles,
                    MAX_TILES);
        } else if (cw == 640 || cw == 800 || cw == 1024) {
            vid_log("; [video] custom res IGNORED -- %d is a stock width; use size_mode instead", cw);
        } else if (!mode_is_available(cw, h_render, nearby, sizeof(nearby)) &&
                   GetPrivateProfileIntA("video", "unverified", 0, ini) == 0) {
            // The display cannot present this exact size -- arming would hand dgVoodoo a mode it
            // refuses, which ends in its modal + an exit rather than anything diagnosable.
            vid_log("; [video] custom res REFUSED -- %dx%d is not an available display mode on this "
                    "machine.%s%s  (Set [video] unverified=1 to arm anyway.)",
                    cw, h_render, nearby[0] ? "  Available at this width: " : "", nearby);
            if (h_render != ch)
                vid_log("; [video]   note: height %d was snapped down to %d for the 32px tile grid -- the "
                        "snap itself can turn a supported mode into an unsupported one",
                        ch, h_render);
        } else if (!install_reentrancy_guard() || !install_hud_chrome()) {
            vid_log("; [video] custom res NOT armed -- re-entrancy guard failed (see above)");
        } else if (!relocate_view_arrays()) {
            vid_log("; [video] custom res NOT armed -- view-array relocation failed (see above)");
        } else if (!patch_mode2(cw, ch, h_render, cw, h_render)) {
            vid_log("; [video] custom res NOT armed -- immediate rewrite failed (see above)");
        } else {
            if (h_render != ch)
                vid_log("; [video] custom res %dx%d: rendering %dx%d (height snapped to a 32px tile multiple)", cw, ch,
                        cw, h_render);
            vid_log("; [video] custom res ARMED as mode 2: %dx%d, %d x %d tiles (%d of %d)", cw, h_render, cw / 32,
                    h_render / 32, tiles, MAX_TILES);
            fix_options_label(cw, h_render);
            g_custom_w  = cw;
            g_custom_h  = h_render;
            g_size_mode = 2; // a custom size only means anything if the game actually selects mode 2
        }
    }

    // D13b -- the options-menu PICKER. Independent of [video] width/height: it needs the same engine
    // plumbing (guard, relocated arrays, mode-2 immediates), so if a custom size did not already arm
    // them, arm them here for the largest entry in the list.
    if (GetPrivateProfileIntA("video", "picker", 0, ini)) {
        build_mode_list(ini);
        if (g_nmodes <= 3) {
            vid_log("; [video] picker NOT armed -- only %d usable modes found, no more than the stock 3", g_nmodes);
        } else {
            // Size the ONE-SHOT boot buffers for the widest and tallest entry, so ANY pick fits.
            int maxw = 0, maxh = 0;
            for (int i = 0; i < g_nmodes; ++i) {
                if (g_modes[i].w > maxw) maxw = g_modes[i].w;
                if (g_modes[i].h_render > maxh) maxh = g_modes[i].h_render;
            }
            const int start = (g_custom_w ? 2 : 2); // entry 2 is the first non-native mode
            bool      ok    = g_custom_w != 0;      // the custom-size path already armed everything below
            if (!ok)
                ok = install_reentrancy_guard() && install_hud_chrome() && relocate_view_arrays() &&
                     patch_mode2(g_modes[start].w, g_modes[start].h_req, g_modes[start].h_render, maxw, maxh);
            if (!ok) {
                vid_log("; [video] picker NOT armed -- engine plumbing failed (see above)");
            } else if ((maxw / 32) * (maxh / 32) > MAX_TILES) {
                vid_log("; [video] picker NOT armed -- boot sizing %dx%d needs %d tiles, ceiling %d", maxw, maxh,
                        (maxw / 32) * (maxh / 32), MAX_TILES);
            } else if (!mh::hook::install_trampoline(mh::addr::llm_view_set_size_mode,
                                                     (void *)view_set_size_mode_detour, &g_vsm_tramp, 8,
                                                     mh::hook::entry_claim::exclusive,
                                                     "the D13b [video] size-mode picker")) {
                vid_log("; [video] picker NOT armed -- view_set_size_mode trampoline failed");
            } else if (!install_picker()) {
                vid_log("; [video] picker NOT armed -- spinner rewrite failed (see above)");
            } else {
                vid_log("; [video] picker: boot buffers sized for %dx%d (largest entry)", maxw, maxh);
                if (g_size_mode < 0) g_size_mode = 0; // arm the menu-load pin so the preference is honoured
            }
        }
    }

    // The upper bound depends on whether the picker widened the list: with it armed, size_mode selects
    // any entry (which is also how a resolution is pinned for a test without clicking through the menu).
    const int max_mode = g_nmodes > 0 ? g_nmodes - 1 : 2;
    if (g_size_mode < 0) return 0; // absent / explicitly off -- nothing installs
    if (g_size_mode > max_mode) {
        vid_log("; [video] size_mode=%d out of range (0..%d) -- IGNORED", g_size_mode, max_mode);
        g_size_mode = -1;
        return 0;
    }
    if (g_size_mode >= 2 && g_nmodes > 0 && !repoke_runtime(g_modes[g_size_mode]))
        vid_log("; [video] size_mode=%d: could not retarget the mode-2 immediates", g_size_mode);

    // U30: collapsed -- the byte compare is the primitive's, and only the primitive can tell a
    // wrong build from an entry the effects gate (or anything else) got to first.
    if (!mh::hook::install_trampoline(ADDR_MENU_LOAD, (void *)menu_load_detour, &g_menu_tramp, 8,
                                      mh::hook::entry_claim::exclusive,
                                      "the [video] size_mode menu-load pin")) {
        vid_log("; [video] size_mode=%d NOT armed -- install refused (see the [interlock] line)", g_size_mode);
        g_size_mode = -1;
        return 0;
    }
    if (g_nmodes > 0 && g_size_mode < g_nmodes)
        vid_log("; [video] armed: size_mode=%d (%dx%d, entry of the %d-item picker list)", g_size_mode,
                g_modes[g_size_mode].w, g_modes[g_size_mode].h_render, g_nmodes);
    else if (g_custom_w)
        vid_log("; [video] armed: size_mode=2 (custom %dx%d)", g_custom_w, g_custom_h);
    else
        vid_log("; [video] armed: size_mode=%d (%s)", g_size_mode,
                g_size_mode == 0 ? "640x480" : g_size_mode == 1 ? "800x576"
                                                                : "1024x768");
    return 1;
}
