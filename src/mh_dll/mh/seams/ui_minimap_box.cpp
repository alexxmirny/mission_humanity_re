// seams/ui_minimap_box.cpp -- mods:RES9: the strategic minimap's viewport box is drawn clipped to the
// 128x128 minimap. Mechanism, the patch site and the contract are in include/mh_minimapbox_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_minimapbox_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::llm_strat_minimap_draw_viewport_box + the surface/minimap globals
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate
#include "include/mh_log_sink.h" // mh_logq_flush

namespace {

// The three box globals llm_strat_minimap_frame writes and only the box draw reads. Raw VAs on
// purpose: UI-only presentation state, kept out of the address manifest's data section so they do
// not become hash regions.
constexpr uintptr_t BOX_W   = 0x00824fa0u; // _G_LLM_STRAT_MINIMAP_VIEWPORT_BOX_W
constexpr uintptr_t BOX_H   = 0x00824fa8u; // _G_LLM_STRAT_MINIMAP_VIEWPORT_BOX_H
constexpr uintptr_t BOX_OFS = 0x00824facu; // _G_LLM_STRAT_MINIMAP_VIEWPORT_BOX_OFS (bytes, from the minimap origin)
constexpr uintptr_t ZOOM    = 0x005586eeu; // _G_LLM_STRAT_MINIMAP_ZOOM_MODE (1/2/4; retail never writes 5)

constexpr int      MAP_PX  = 128;     // the minimap is 128x128 px at every zoom mode
constexpr uint16_t BOX_RGB = 0xdeffu; // the stock colour (mov ax,0xdeff)

// The entry's first instruction, MOV EDI,[_G_LLM_GFX_DRAW_SURFACE] (6 bytes), becomes JMP rel32 + NOP.
const uint8_t ENTRY_EXPECT[6] = {0x8B, 0x3D, 0x00, 0x3B, 0x60, 0x00};

int  g_mode      = 0; // 0 = stock bytes, 1 = clipped draw, 2 = stock body + measurement (diagnosis)
bool g_installed = false;

// ---- geometry ---------------------------------------------------------------------------------
struct geom {
    uint8_t *base;   // the side-panel surface
    int      pitch;  // bytes per row
    int      ox, oy; // minimap origin in the surface, px
    int      x0, y0; // box top-left relative to the minimap origin, px
    int      w, h;
    int      zoom;
};

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

geom read_geom() {
    geom g;
    g.base  = *(uint8_t **)mh::addr::_G_LLM_GFX_DRAW_SURFACE;
    g.pitch = *(const int *)mh::addr::_G_LLM_GFX_PANEL_ROW_SKIP;
    g.ox    = *(const int *)mh::addr::_G_LLM_TACT_UI_MINIMAP_ORIGIN_X;
    g.oy    = *(const int *)mh::addr::_G_LLM_TACT_UI_MINIMAP_ORIGIN_Y;
    g.w     = *(const int *)BOX_W;
    g.h     = *(const int *)BOX_H;
    g.zoom  = *(const int *)ZOOM;
    // OFS is one linear byte offset, pitch * row + 2 * col (llm_strat_minimap_frame); every mode's
    // column margin is under a quarter row, so rounding the row to nearest recovers both exactly.
    const int ofs = *(const int *)BOX_OFS;
    g.y0          = g.pitch > 0 ? floor_div(ofs + g.pitch / 2, g.pitch) : 0;
    g.x0          = (ofs - g.y0 * g.pitch) / 2;
    return g;
}

// The stock pixel order (0x004a2344): top edge left to right from (x0,y0), the right edge down
// from (x0+w,y0), the bottom edge right to left from (x0+w,y0+h), the left edge up from (x0,y0+h).
template <class F>
void for_each_box_px(const geom &g, F f) {
    int x = g.x0, y = g.y0;
    for (int i = 0; i < g.w; ++i) f(x++, y);
    for (int i = 0; i < g.h; ++i) f(x, y++);
    for (int i = 0; i < g.w; ++i) f(x--, y);
    for (int i = 0; i < g.h; ++i) f(x, y--);
}

// ---- the before-surface canary ------------------------------------------------------------------
// Snapshot the bytes the stock loop would reach BEFORE the surface, draw, then count 16-bit words
// there that turned into the box colour. Another thread may legitimately change those bytes (in the
// user's crash they were a worker's stack), so only a NEW 0xDEFF counts.
constexpr int CANARY_MAX = 64 * 1024;
uint8_t       g_snap[CANARY_MAX];
int           g_snap_len = 0;
uint8_t      *g_snap_at  = nullptr;

// How many bytes directly before `p` are committed and readable, walking back region by region, up
// to `want`. On the rig the surface starts 0x30 bytes into its own allocation, so this is short.
int readable_before(const uint8_t *p, int want) {
    int got = 0;
    while (got < want) {
        MEMORY_BASIC_INFORMATION mbi;
        const uint8_t           *q = p - got - 1;
        if (!VirtualQuery(q, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & 0xEE))
            break;
        got = (int)(p - (const uint8_t *)mbi.BaseAddress);
    }
    return got < want ? got : want;
}

// ---- per-geometry report ------------------------------------------------------------------------
struct report {
    int before;   // stock pixels that land before the surface
    int outside;  // stock pixels inside the surface but outside the 128x128 minimap
    int min_byte; // lowest byte offset the stock loop writes, relative to the surface
};

report measure(const geom &g) {
    report r = {0, 0, 0};
    for_each_box_px(g, [&](int x, int y) {
        const int off = g.pitch * (g.oy + y) + 2 * (g.ox + x);
        if (off < r.min_byte) r.min_byte = off;
        if (off < 0)
            ++r.before;
        else if (x < 0 || x >= MAP_PX || y < 0 || y >= MAP_PX)
            ++r.outside;
    });
    return r;
}

uint32_t g_last_sig = 0;
int      g_reports  = 0;
uint32_t sig_of(const geom &g) {
    uint32_t  s   = 2166136261u;
    const int v[] = {(int)(uintptr_t)g.base, g.pitch, g.ox, g.oy, g.x0, g.y0, g.w, g.h, g.zoom};
    for (int i : v) s = (s ^ (uint32_t)i) * 16777619u;
    return s;
}

void call_stock_body(); // the original body, all registers preserved (below)

void __cdecl box_impl() {
    const geom   g   = read_geom();
    const report r   = measure(g);
    const bool   sig = sig_of(g) != g_last_sig;

    // Canary only where the stock loop would leave the surface; a fitting box is never measured.
    g_snap_len = 0;
    if (r.min_byte < 0) {
        const int len = readable_before(g.base, -r.min_byte < CANARY_MAX ? -r.min_byte : CANARY_MAX);
        if (len > 0) {
            g_snap_at = g.base - len;
            memcpy(g_snap, g_snap_at, len);
            g_snap_len = len;
        }
    }

    int drawn = 0, clipped = 0;
    if (g_mode == 1) {
        // The stock outline, pixel for pixel, minus every pixel outside the 128x128 minimap.
        for_each_box_px(g, [&](int x, int y) {
            if (x < 0 || x >= MAP_PX || y < 0 || y >= MAP_PX) {
                ++clipped;
                return;
            }
            *(uint16_t *)(g.base + g.pitch * (g.oy + y) + 2 * (g.ox + x)) = BOX_RGB;
            ++drawn;
        });
    } else {
        if (sig && r.min_byte < 0) {
            // Logged BEFORE the stock body runs: when the bytes before the surface are not mapped, the
            // stock loop faults and this is the last line the process writes.
            MEMORY_BASIC_INFORMATION mbi = {};
            VirtualQuery(g.base - 1, &mbi, sizeof(mbi));
            char m[300];
            wsprintfA(m,
                      "; RES9: about to run the STOCK box draw: zoom %d box %dx%d at (%d,%d), lowest byte %d before "
                      "the surface %08X; the byte before it is in a region base %08X size %u state %X protect %X; "
                      "canary window %d bytes\n",
                      g.zoom, g.w, g.h, g.x0, g.y0, r.min_byte, (unsigned)(uintptr_t)g.base,
                      (unsigned)(uintptr_t)mbi.BaseAddress, (unsigned)mbi.RegionSize, (unsigned)mbi.State,
                      (unsigned)mbi.Protect, g_snap_len);
            seam_log(m);
            mh_logq_flush(1000); // on disk before the stock loop can fault
        }
        call_stock_body();
        drawn = 2 * (g.w + g.h);
    }

    int deff = -1;
    if (g_snap_len) {
        deff = 0;
        for (int i = (int)(((uintptr_t)g_snap_at - (uintptr_t)g.base) & 1); i + 1 < g_snap_len; i += 2) {
            const uint16_t was = (uint16_t)(g_snap[i] | (g_snap[i + 1] << 8));
            const uint16_t now = *(const uint16_t *)(g_snap_at + i);
            if (now == BOX_RGB && was != BOX_RGB) ++deff;
        }
    }

    if (sig && g_reports < 64) {
        g_last_sig = sig_of(g);
        ++g_reports;
        char m[400];
        wsprintfA(m,
                  "; RES9: minimap box zoom %d box %dx%d at (%d,%d) from the minimap origin (%d,%d), surface %08X "
                  "pitch %d: stock writes %d px before the surface (lowest byte %d) and %d px outside the minimap; "
                  "%s drew %d px, clipped %d px; canary %d new DEFF words before the surface (window %d bytes)\n",
                  g.zoom, g.w, g.h, g.x0, g.y0, g.ox, g.oy, (unsigned)(uintptr_t)g.base, g.pitch, r.before,
                  r.min_byte, r.outside, g_mode == 2 ? "stock body" : "clip", drawn, clipped, deff, g_snap_len);
        seam_log(m);
    }
}

// clang-format off
// The caller (llm_strat_minimap_render) is hand-written assembly inside one PUSHAD/POPAD pair, and the
// stock body preserves only ECX/ESI/EBP; saving everything is the superset.
__declspec(naked) void box_thunk() {
    __asm {
        pushad
        call box_impl
        popad
        ret
    }
}

// The stock body, entered past its patched first instruction: re-execute MOV EDI,[0x00603b00] and
// continue at 0x004a234a; its own RET returns to our caller.
__declspec(naked) void stock_body() {
    __asm {
        mov  edi, dword ptr ds:[0x00603b00]
        push 0x004a234a
        ret
    }
}

// The stock body clobbers EBX/EDI/EDX, which a __cdecl caller expects preserved.
__declspec(naked) void call_stock_body() {
    __asm {
        pushad
        call stock_body
        popad
        ret
    }
}
// clang-format on

} // namespace

extern "C" int MH_MinimapBox_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_installed) return 1;
    g_mode = mh_ini_get_int("ui", "minimap_box_clip", 1, g_ini);
    if (g_mode == 0) {
        seam_log("; RES9: minimap viewport box KEPT stock ([ui] minimap_box_clip=0): drawn unclipped, out of the "
                 "side-panel surface at heights >= 1152 in zoom mode 4 -- the reproduction arm\n");
        return 0;
    }
    if (g_mode != 2) g_mode = 1;

    const uintptr_t site   = mh::addr::llm_strat_minimap_draw_viewport_box;
    const char     *refuse = "NOT armed (entry bytes differ from MOV EDI,[0x00603b00])";
    if (mh::hook::promoted_owner_of(site)) {
        refuse = "DISPLACED (llm_strat_minimap_draw_viewport_box is promoted this run)";
    } else {
        uint8_t repl[6];
        repl[0]           = 0xE9;
        const int32_t rel = (int32_t)((uintptr_t)&box_thunk - (site + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        repl[5]     = 0x90;
        g_installed = mh::hook::patch_bytes_guarded(site, ENTRY_EXPECT, repl, 6);
    }
    char m[200];
    wsprintfA(m, "; RES9: minimap viewport box %s at %08X (mode %d: %s)\n", g_installed ? "armed" : refuse,
              (unsigned)site, g_mode, g_mode == 2 ? "stock body + measurement" : "clipped to the minimap");
    seam_log(m);
    return g_installed ? 1 : 0;
}
