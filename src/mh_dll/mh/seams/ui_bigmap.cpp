// seams/ui_bigmap.cpp -- mods:MM4: a big minimap drawn over the strategic view while a key is held.
//
// RENDER AND INPUT ONLY. It reads game memory, writes pixels into the composed frame from the shared
// present hook (net_lockstep on_present, right before the net indicator and the debug overlay), and
// polls a key. It never writes sim state and moves the camera only through the stock camera setters on a
// LEFT click (the stock minimap click handler llm_strat_minimap_cam_goto 0x00427978 is NOT called). The one
// thing it sends is mods:MM4C's RIGHT click: the same order the side minimap's right click gives, through
// the game's own issuers (the lockstep queue), never a direct sim write -- so it needs no host sync and is
// MP-safe. It works with libmh absent (the shipped config): every state read goes through the region
// runtime, which answers the stock address when nothing is relocated.
//
//   [ui]
//   bigmap=1             ; on/off (launcher). Off = nothing is installed per frame but one int test
//   bigmap_center=camera ; camera | start | map  (launcher dropdown)
//   bigmap_key=Tab       ; launcher `hotkey` type; "none" = unbound
//   bigmap_dim=35        ; dev knob: percent the world under the overlay is darkened (user decides live)
//   bigmap_orders=1      ; mods:MM4C: a right click on the overlay gives the selected units the side minimap's order
//
// WHY IT OWNS A RENDERER. The stock minimap (llm_strat_minimap_render 0x004a17b3) is parameterless
// hand-written assembly bound to a 128x128 image drawn into the side-panel surface; it cannot draw a
// bigger one. The per-tile RULES below are copied from its zoom-1 arm:
//   tile  = tile_objects[(col << 8) | row], 8 bytes; byte +1 bit 7 = explored, bit 6 = remembered
//           (not currently visible); +0 flags & 0xfff indexes the terrain thumbnail (colour mode on) or
//           the flat colour (M key, mode 0); remembered tiles are halved and carry no markers
//   +2 building (low byte != 0) and class_owner (+6) bit 4 clear -> player colour LUT[class_owner & 15]
//   +4 unit word != 0 -> LUT[byte(+5) >> 4]
//   resources: per 4x4 block, 16 bytes/block, slot _G_LLM_STRAT_MINIMAP_RESOURCE_OVERLAY_SLOT; drawn as the
//   halve-mask colour on a checkerboard (the stock skips them at the top zoom; here they are always drawn)
//
// THE RESIDUE TRAP (mp:GX1). The strategic ground pass repaints a
// tile only when bit 0 of _G_LLM_TILE_VIS_MAP_PTR[row * VIEW_TILES_W + col] is set, and clears it when it
// does. Pixels we write into the frame without setting that bit would stay forever. So while the overlay
// is shown we set the bit on every view tile after drawing (the next frame's ground pass repaints them
// all, the sprites draw over that, and we dim + draw again), and once more on the frame it stops. That
// repaint is also what keeps the dim from compounding: if two presents happen with no ground pass between
// them (the bits we set are still set) the dim is skipped and the map is simply redrawn.
//
// THE START POSITION is players[PlayerSide].landing_x/landing_y[G_PLANET_INDEX]
// (llm_strat_player_profile +0x10 / +0x90, stride 0x740): llm_game_land_players_on_planet claims a landing
// spot for every player, spawns the human's start unit on it and centres the camera there.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "include/mh_bigmap_export.h"
#include "include/mh_mapzoom_export.h" // MH_MapZoom_Get: the crop the wheel zoom shows
#include "include/mh_uidrive_export.h" // MH_UIDrive_SynthKeyDown: the harness's held-key table
#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h" // llm_map_cam_set_col / _row / mark_viewport_dirty
#include "hook/patch.h"        // patch_bytes_guarded: the cursor-call splice
#include "seams/strat_pre_input.h"
#include "config/ini_read.h"
#include "state/region_runtime.h"
#include "caps/caps.h"    // mh::caps::rec / g: the building array the game addresses (PT-CAP8)
#include "net_internal.h" // seam_log, g_ini
#include "en_guard.h"

#pragma comment(lib, "user32.lib")

namespace {

constexpr uintptr_t ADDR_FB    = mh::addr::_G_LLM_FRAMEBUFFER;
constexpr uintptr_t ADDR_PITCH = mh::addr::_G_LLM_FB_PITCH;
constexpr uintptr_t ADDR_W     = mh::addr::WindowWidth;
constexpr uintptr_t ADDR_H     = mh::addr::WindowHeight;
constexpr uintptr_t ADDR_MODE  = mh::addr::_G_LLM_GAME_MODE;
// UI-only presentation state: raw VAs on purpose, not manifest entries (a manifest data entry becomes a
// state-registry region and renumbers every later RID -- the SES3b / fixture-fingerprint trap).
constexpr uintptr_t ADDR_CHAT_MODE    = 0x005d01d8u; // undefined4: 0 = no chat line open
constexpr uintptr_t ADDR_COLOR_LUT    = 0x00ae1948u; // _G_LLM_STRAT_PLAYER_COLOR_LUT: ushort[8] RGB565 by player
constexpr uintptr_t ADDR_THUMB_PTR    = 0x0052046du; // _G_LLM_PLANET_TLO_THUMBNAIL_PTR: ushort* terrain colour by (flags & 0xfff)
constexpr uintptr_t ADDR_COLOR_MODE   = 0x005586f6u; // _G_LLM_MINIMAP_TERRAIN_COLOR_MODE: byte, 0 = flat colour (the M key)
constexpr uintptr_t ADDR_FLAT_COLOR   = 0x005586fau; // _G_LLM_STRAT_MINIMAP_TERRAIN_FLAT_COLOR: ushort
constexpr uintptr_t ADDR_RES_SLOT     = 0x005586feu; // _G_LLM_STRAT_MINIMAP_RESOURCE_OVERLAY_SLOT: short
constexpr uintptr_t ADDR_RES_GRID     = 0x005d0ac8u; // _G_LLM_STRAT_MINIMAP_RESOURCE_GRID_PTR: map_resources* (16 B per 4x4 block)
constexpr uintptr_t ADDR_HALVE        = 0x00879668u; // _G_LLM_GFX_RGB565_HALVE_MASK2 (low word is the stock resource colour)
constexpr uintptr_t ADDR_VIEW_TILES_W = mh::addr::_G_LLM_VIEW_TILES_W;
constexpr uintptr_t ADDR_VIEW_TILES_H = mh::addr::_G_LLM_VIEW_TILES_H;

constexpr int      FRAME_PX = 6;       // the stock minimap's own border, outer -> inner below
constexpr uint16_t BOX_RGB  = 0xdeffu; // the stock camera-box colour
constexpr int      TILE_PX  = 32;      // one map tile on the main view

// --- config -------------------------------------------------------------------------------------
bool g_init   = false;
bool g_on     = false;
bool g_orders = true; // [ui] bigmap_orders: the right click on the overlay issues the minimap's order (off = swallowed, nothing happens)
int  g_center = 0;    // 0 camera, 1 start, 2 map
int  g_dim    = 35;
struct Key {
    int  vk   = 0;
    bool ctrl = false, alt = false, shift = false;
} g_key;

// --- state --------------------------------------------------------------------------------------
bool          g_shown  = false; // the last present drew the overlay
unsigned      g_frames = 0;     // presents drawn in this hold
uint64_t      g_sum_us = 0, g_dim_us = 0;
unsigned      g_logs           = 0;
uint32_t      g_last_sig       = 0;
LARGE_INTEGER g_qf             = {};
bool          g_spliced        = false; // the draw runs at the stock cursor call (so the cursor lands on top), not at present
bool          g_drawn_in_frame = false; // the splice already handled this frame; the present hook then has nothing to do

template <class T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void *)a, sizeof(T));
    return v;
}

long long us_since(const LARGE_INTEGER &t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return g_qf.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000000 / g_qf.QuadPart : 0;
}

int parse_int(const char *s) { return (int)strtol(s, nullptr, 0); }

// "Tab", "Ctrl+Alt+N", "F9", "0x09", "none". Same grammar as the overlay's own parser.
bool parse_key(const char *spec, Key *out) {
    *out          = Key{};
    const char *s = spec;
    for (;;) {
        if (_strnicmp(s, "ctrl+", 5) == 0) {
            out->ctrl = true;
            s += 5;
        } else if (_strnicmp(s, "control+", 8) == 0) {
            out->ctrl = true;
            s += 8;
        } else if (_strnicmp(s, "alt+", 4) == 0) {
            out->alt = true;
            s += 4;
        } else if (_strnicmp(s, "shift+", 6) == 0) {
            out->shift = true;
            s += 6;
        } else break;
    }
    if ((s[0] == 'V' || s[0] == 'v') && (s[1] == 'K' || s[1] == 'k') && s[2] == '_') s += 3;
    static const struct {
        const char *name;
        int         vk;
    } NAMED[] = {
        {"tab", VK_TAB},
        {"pageup", VK_PRIOR},
        {"pagedown", VK_NEXT},
        {"space", VK_SPACE},
        {"pgup", VK_PRIOR},
        {"prior", VK_PRIOR},
        {"pgdn", VK_NEXT},
        {"next", VK_NEXT},
        {"home", VK_HOME},
        {"end", VK_END},
        {"ins", VK_INSERT},
        {"insert", VK_INSERT},
        {"del", VK_DELETE},
        {"delete", VK_DELETE},
        {"up", VK_UP},
        {"down", VK_DOWN},
        {"left", VK_LEFT},
        {"right", VK_RIGHT},
        {"capslock", VK_CAPITAL},
        {"caps", VK_CAPITAL},
    };
    for (const auto &e : NAMED)
        if (lstrcmpiA(e.name, s) == 0) {
            out->vk = e.vk;
            return true;
        }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') {
        const int n = parse_int(s + 1);
        if (n >= 1 && n <= 24) {
            out->vk = VK_F1 + n - 1;
            return true;
        }
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        out->vk = parse_int(s);
        return out->vk > 0 && out->vk < 256;
    }
    if (s[0] && !s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out->vk = (unsigned char)c;
        return true;
    }
    return false;
}

void init() {
    g_init = true;
    if (!mh::en_build_ok()) return;
    QueryPerformanceFrequency(&g_qf);
    g_on     = mh_ini_get_int("ui", "bigmap", 1, g_ini) != 0;
    g_orders = mh_ini_get_int("ui", "bigmap_orders", 1, g_ini) != 0;
    g_dim    = mh_ini_get_int("ui", "bigmap_dim", 35, g_ini);
    if (g_dim < 0) g_dim = 0;
    if (g_dim > 90) g_dim = 90;
    char buf[64];
    mh::config::read_ini_string("ui", "bigmap_center", "camera", buf, sizeof(buf), g_ini);
    g_center = lstrcmpiA(buf, "start") == 0 ? 1 : lstrcmpiA(buf, "map") == 0 ? 2
                                                                             : 0;
    mh::config::read_ini_string("ui", "bigmap_key", "Tab", buf, sizeof(buf), g_ini);
    bool bound = buf[0] && lstrcmpiA(buf, "none") != 0 && parse_key(buf, &g_key);
    if (!bound) {
        g_key = Key{};
        if (g_on) g_on = false;
    }
    char m[200];
    wsprintfA(m, "; BIGMAP: %s centre=%s key='%s' dim=%d%% orders=%d\n", g_on ? "armed" : "off", g_center == 1 ? "start" : g_center == 2 ? "map"
                                                                                                                                         : "camera",
              buf, g_dim, g_orders ? 1 : 0);
    seam_log(m);
}

bool foreground_is_us() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Held = the OS says so (and the game window is the foreground one: the key may be held in another app) OR
// the UI harness synthesises it (a headless lane's GetAsyncKeyState always answers 0).
bool key_held() {
    if (!g_key.vk) return false;
    bool vk_down = MH_UIDrive_SynthKeyDown(g_key.vk) != 0;
    if (!vk_down && (GetAsyncKeyState(g_key.vk) & 0x8000) != 0 && foreground_is_us()) vk_down = true;
    if (!vk_down) return false;
    auto mod = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 || MH_UIDrive_SynthKeyDown(vk) != 0; };
    if (g_key.ctrl && !mod(VK_CONTROL)) return false;
    if (g_key.alt && !mod(VK_MENU)) return false;
    if (g_key.shift && !mod(VK_SHIFT)) return false;
    return true;
}

// Strategic view on screen, no chat line, no modal dialog / foreign widget list.
bool screen_ok() {
    if (*(const uint8_t *)ADDR_MODE != 2) return false;
    if (rd<uint32_t>(ADDR_CHAT_MODE) != 0) return false;
    if (rd<uint32_t>(mh::addr::_G_LLM_UI_ACTIVE_DIALOG) != 0) return false;
    const uint32_t list = rd<uint32_t>(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST);
    return list == 0 || list == mh::addr::_G_LLM_UI_WGT_LIST_GAMEPLAY_HUD;
}

// ---- pixels ------------------------------------------------------------------------------------
struct Surf {
    uint8_t *fb;
    int      pitch;
    int      vw, vh; // the strategic view rect (0,0)-(vw,vh); everything is clipped to it
};

inline void put(const Surf &s, int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= s.vw || y >= s.vh) return;
    *(uint16_t *)(s.fb + (size_t)y * s.pitch + (size_t)x * 2) = c;
}

void ring(const Surf &s, int x0, int y0, int x1, int y1, uint16_t c) { // inclusive corners
    for (int x = x0; x <= x1; ++x) {
        put(s, x, y0, c);
        put(s, x, y1, c);
    }
    for (int y = y0 + 1; y < y1; ++y) {
        put(s, x0, y, c);
        put(s, x1, y, c);
    }
}

uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

inline uint16_t scale565(uint16_t p, uint32_t s) { // s in 0..32
    uint32_t x = (p | ((uint32_t)p << 16)) & 0x07E0F81Fu;
    x          = (x * s) >> 5;
    x &= 0x07E0F81Fu;
    return (uint16_t)(x | (x >> 16));
}

void dim_view(const Surf &s, int dim_pct) {
    const uint32_t k = (uint32_t)((100 - dim_pct) * 32 + 50) / 100;
    for (int y = 0; y < s.vh; ++y) {
        uint16_t *row = (uint16_t *)(s.fb + (size_t)y * s.pitch);
        for (int x = 0; x < s.vw; ++x) row[x] = scale565(row[x], k);
    }
}

int floor_mod(int a, int m) {
    int r = a % m;
    return r < 0 ? r + m : r;
}
int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// ---- the damage map ----------------------------------------------------------------------------
// Set bit 0 on every view tile; returns how many of them still had it set from the last stamp (= no ground
// pass has consumed them since).
void stamp_view(int vw, int vh, int *still_set, int *total) {
    if (still_set) *still_set = 0;
    if (total) *total = 0;
    uint8_t **pp = mh::state::ptr<uint8_t *>(mh::state::RID_TILE_VIS_MAP_PTR);
    if (!pp) return;
    uint8_t *vis = mh::state::translate_slot(*pp);
    if (!vis) return;
    const int tw = rd<int32_t>(ADDR_VIEW_TILES_W), th = rd<int32_t>(ADDR_VIEW_TILES_H);
    if (tw <= 0 || th <= 0 || tw > 512 || th > 512) return;
    int cols = (vw + 31) >> 5, rows = (vh + 31) >> 5;
    if (cols > tw) cols = tw;
    if (rows > th) rows = th;
    int set = 0, n = 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            uint8_t &b = vis[r * tw + c];
            if (b & 1) ++set;
            ++n;
            b |= 1;
        }
    if (still_set) *still_set = set;
    if (total) *total = n;
}

// ---- the map -----------------------------------------------------------------------------------
struct Geom {
    int mw, mh; // map tiles
    int k;      // px per tile
    int x0, y0; // map image top-left in the view
    int cx, cy; // the tile shown at the image centre
};

// What the last drawn frame showed: the click handler maps a mouse position through it. `zl..ch` is the
// visible world crop (the whole view, or the wheel zoom's crop).
struct Shown {
    bool valid = false;
    Geom g     = {};
    int  zl = 0, zt = 0, cw = 0, ch = 0;
} g_shown_geom;

int pick_k(int vw, int vh, int mw, int mh_, int margin) {
    const int tiles = mw > mh_ ? mw : mh_;
    int       k     = (vh * 2 / 3) / tiles; // integer px per tile ~ two thirds of the view height
    const int pad   = 2 * (margin + FRAME_PX + 1);
    int       kw = (vw - pad) / mw, kh = (vh - pad) / mh_;
    if (k > kw) k = kw;
    if (k > kh) k = kh;
    return k < 1 ? 1 : k;
}

void draw_map(const Surf &s, const Geom &g) {
    const uint8_t *tobj = mh::state::ptr<const uint8_t>(mh::state::RID_TILE_OBJECTS);
    if (!tobj) return;
    const uint16_t halve    = rd<uint16_t>(ADDR_HALVE);
    const bool     col_mode = rd<uint8_t>(ADDR_COLOR_MODE) != 0;
    const uint16_t flat     = rd<uint16_t>(ADDR_FLAT_COLOR);
    const uint8_t *thumb    = (const uint8_t *)rd<uintptr_t>(ADDR_THUMB_PTR);
    if (col_mode && !thumb) return;
    const uint8_t *grid = (const uint8_t *)mh::state::translate_slot((uint8_t *)rd<uintptr_t>(ADDR_RES_GRID));
    int            slot = rd<int16_t>(ADDR_RES_SLOT);
    if (slot < 0 || slot > 7) slot = 0;
    uint16_t lut[8];
    memcpy(lut, (const void *)ADDR_COLOR_LUT, sizeof(lut));

    for (int ty = 0; ty < g.mh; ++ty) {
        const int row = floor_mod(g.cy - g.mh / 2 + ty, g.mh);
        for (int tx = 0; tx < g.mw; ++tx) {
            const int      col = floor_mod(g.cx - g.mw / 2 + tx, g.mw);
            const uint8_t *t   = tobj + (size_t)(((col & 255) << 8) | (row & 255)) * 8;
            uint16_t       c   = 0;
            bool           dot = false;
            const uint8_t  hi  = t[1];
            if (hi & 0x80) { // explored
                uint16_t base = flat;
                if (col_mode) {
                    const unsigned idx = (unsigned)(t[0] | (t[1] << 8)) & 0xfffu;
                    base               = (uint16_t)(thumb[idx * 2] | (thumb[idx * 2 + 1] << 8));
                }
                const bool remembered = (hi & 0x40) != 0;
                c                     = remembered ? (uint16_t)((base >> 1) & halve) : base;
                if (grid) {
                    int16_t v;
                    memcpy(&v, grid + (size_t)(row >> 2) * 16 + (size_t)(col >> 2) * 1024 + slot * 2, 2);
                    dot = v != 0;
                }
                if (!remembered) {
                    if (t[2] != 0 && (t[6] & 0x10) == 0) {
                        c   = lut[t[6] & 7];
                        dot = false;
                    }
                    if ((t[4] | (t[5] << 8)) != 0) {
                        c   = lut[(t[5] >> 4) & 7];
                        dot = false;
                    }
                }
            }
            const int px = g.x0 + tx * g.k, py = g.y0 + ty * g.k;
            if (px >= s.vw || py >= s.vh || px + g.k <= 0 || py + g.k <= 0) continue;
            for (int j = 0; j < g.k; ++j)
                for (int i = 0; i < g.k; ++i) {
                    bool d = dot && (g.k == 1 ? ((col ^ row) & 1) == 0 : ((i + j) & 1) == 0);
                    put(s, px + i, py + j, d ? halve : c);
                }
        }
    }
}

// The 6 px rust-metal border of the stock minimap (outer -> inner, measured at 1024x768) plus one dark outer line.
void draw_frame(const Surf &s, const Geom &g) {
    static const int RGB[FRAME_PX][3] = {{115, 65, 49}, {173, 130, 123}, {156, 121, 115}, {222, 219, 222}, {115, 97, 99}, {24, 20, 16}};
    const int        mx0 = g.x0, my0 = g.y0, mx1 = g.x0 + g.mw * g.k - 1, my1 = g.y0 + g.mh * g.k - 1;
    ring(s, mx0 - FRAME_PX - 1, my0 - FRAME_PX - 1, mx1 + FRAME_PX + 1, my1 + FRAME_PX + 1, rgb565(8, 8, 8));
    for (int i = 0; i < FRAME_PX; ++i) {
        const int d = FRAME_PX - i; // ring 0 is the outermost, 6 px out
        ring(s, mx0 - d, my0 - d, mx1 + d, my1 + d, rgb565(RGB[i][0], RGB[i][1], RGB[i][2]));
    }
}

// The camera rectangle: the main view's extent, wrapped on the torus (the image is a torus too).
void draw_camera_box(const Surf &s, const Geom &g, int view_left_px, int view_top_px, int cw, int ch) {
    const int cam_c = *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_COL);
    const int cam_r = *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_ROW);
    const int iw = g.mw * g.k, ih = g.mh * g.k;
    // world px of the view's top-left, relative to the image's top-left tile
    const int wx = cam_c * TILE_PX + view_left_px - (g.cx - g.mw / 2) * TILE_PX;
    const int wy = cam_r * TILE_PX + view_top_px - (g.cy - g.mh / 2) * TILE_PX;
    int       rx = floor_mod(floor_div(wx * g.k, TILE_PX), iw);
    int       ry = floor_mod(floor_div(wy * g.k, TILE_PX), ih);
    int       rw = cw * g.k / TILE_PX, rh = ch * g.k / TILE_PX;
    if (rw < 2) rw = 2;
    if (rh < 2) rh = 2;
    if (rw > iw) rw = iw;
    if (rh > ih) rh = ih;
    auto plot = [&](int x, int y) { put(s, g.x0 + floor_mod(x, iw), g.y0 + floor_mod(y, ih), BOX_RGB); };
    for (int i = 0; i < rw; ++i) {
        plot(rx + i, ry);
        plot(rx + i, ry + rh - 1);
    }
    for (int j = 1; j < rh - 1; ++j) {
        plot(rx, ry + j);
        plot(rx + rw - 1, ry + j);
    }
}

// The landing spot of the local player on the current planet (see the head comment); false if unusable.
bool start_tile(int mw, int mh_, int *col, int *row) {
    const uint8_t *pl = mh::state::ptr<const uint8_t>(mh::state::RID_STRAT_PLAYERS);
    if (!pl) return false;
    const unsigned side   = *mh::state::ptr<const uint16_t>(mh::state::RID_PLAYERSIDE);
    const unsigned planet = (unsigned)*mh::state::ptr<const int32_t>(mh::state::RID_G_PLANET_INDEX);
    if (side > 7 || planet > 31) return false;
    const uint8_t *p = pl + (size_t)side * 0x740;
    int32_t        x, y;
    memcpy(&x, p + 0x10 + planet * 4, 4);
    memcpy(&y, p + 0x90 + planet * 4, 4);
    if (x < 0 || y < 0 || x >= mw || y >= mh_) return false;
    *col = x;
    *row = y;
    return true;
}

void draw_start_marker(const Surf &s, const Geom &g, int col, int row) {
    const int ix = floor_mod(col - (g.cx - g.mw / 2), g.mw), iy = floor_mod(row - (g.cy - g.mh / 2), g.mh);
    const int px = g.x0 + ix * g.k + g.k / 2, py = g.y0 + iy * g.k + g.k / 2;
    for (int j = -5; j <= 5; ++j)
        for (int i = -5; i <= 5; ++i) {
            const int d = i * i + j * j;
            if (d >= 12 && d <= 25) put(s, px + i, py + j, rgb565(255, 230, 0));
        }
}

void draw() {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    uint8_t  *fb    = *(uint8_t **)ADDR_FB;
    const int pitch = *(const int *)ADDR_PITCH, W = *(const int *)ADDR_W, H = *(const int *)ADDR_H;
    const int vw = *mh::state::ptr<const int32_t>(mh::state::RID_G_WIN_W), vh = *mh::state::ptr<const int32_t>(mh::state::RID_G_WIN_H);
    if (!fb || W <= 0 || H <= 0 || W > 4096 || H > 4096 || pitch < W * 2) return;
    if (vw <= 0 || vh <= 0 || vw > W || vh > H) return;
    Geom g;
    g.mw = *mh::state::ptr<const int32_t>(mh::state::RID_WIDTH);
    g.mh = *mh::state::ptr<const int32_t>(mh::state::RID_HEIGHT);
    if (g.mw < 8 || g.mh < 8 || g.mw > 256 || g.mh > 256) return;
    const int margin = W < 1024 ? 16 : 24;
    g.k              = pick_k(vw, vh, g.mw, g.mh, margin);
    g.x0             = (vw - g.mw * g.k) / 2;
    g.y0             = (vh - g.mh * g.k) / 2;

    // The visible part of the world: the whole viewport, or the zoom module's crop of it.
    int z100 = 100, zl = 0, zt = 0, zcw = 0, zch = 0;
    MH_MapZoom_Get(&z100, &zl, &zt, &zcw, &zch);
    const int cw = zcw > 0 ? zcw : vw, ch = zch > 0 ? zch : vh;
    const int cam_c = *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_COL);
    const int cam_r = *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_ROW);
    int       scol = 0, srow = 0;
    bool      have_start = false;
    if (g_center == 1) have_start = start_tile(g.mw, g.mh, &scol, &srow);
    if (g_center == 1 && have_start) {
        g.cx = scol;
        g.cy = srow;
    } else if (g_center == 2) {
        g.cx = g.mw / 2;
        g.cy = g.mh / 2;
    } else { // camera (also the fallback when no start position is recorded)
        g.cx = floor_mod(floor_div(cam_c * TILE_PX + zl + cw / 2, TILE_PX), g.mw);
        g.cy = floor_mod(floor_div(cam_r * TILE_PX + zt + ch / 2, TILE_PX), g.mh);
    }

    g_shown_geom = Shown{true, g, zl, zt, cw, ch};

    Surf s = {fb, pitch, vw, vh};
    // Was the ground repainted since our last stamp? If not, the frame still holds last present's dimmed
    // image: dimming again would compound.
    int        still = 0, total = 0;
    const bool had_stamp = g_shown;
    stamp_view(vw, vh, &still, &total); // sets the bits for the NEXT frame's repaint and reports the old state
    const bool    repainted = !(had_stamp && total > 0 && still * 10 >= total * 9);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    if (repainted) dim_view(s, g_dim);
    const long long dim_us = g_qf.QuadPart ? (long long)(us_since(t1)) : 0;

    draw_map(s, g);
    draw_frame(s, g);
    if (g_center == 1 && have_start) draw_start_marker(s, g, scol, srow);
    draw_camera_box(s, g, zl, zt, cw, ch);

    const long long us = us_since(t0);
    g_sum_us += (uint64_t)us;
    g_dim_us += (uint64_t)dim_us;
    ++g_frames;
    const uint32_t sig = (uint32_t)(g.k * 7919 + g.x0 * 104729 + g.y0 * 1299709 + g.cx * 15485863 + g.cy * 32452843 + g.mw * 49979687 + vw + W * 3 + g_center);
    if (sig != g_last_sig && g_logs < 40) {
        g_last_sig = sig;
        ++g_logs;
        char m[360];
        wsprintfA(m, "; BIGMAP: shown %dx%d map, %d px/tile at (%d,%d) in a %dx%d view of a %dx%d window, centre tile (%d,%d) [%s%s], camera (%d,%d) zoom %d%%, margin %d\n",
                  g.mw, g.mh, g.k, g.x0, g.y0, vw, vh, W, H, g.cx, g.cy, g_center == 1 ? "start" : g_center == 2 ? "map"
                                                                                                                 : "camera",
                  (g_center == 1 && !have_start) ? " -> no start position, camera" : "", cam_c, cam_r, z100, margin);
        seam_log(m);
    }
    if (g_frames == 60 && g_logs < 40) {
        ++g_logs;
        char m[200];
        wsprintfA(m, "; BIGMAP: cost over 60 frames: avg %u us per present (dim %u us)\n", (unsigned)(g_sum_us / g_frames), (unsigned)(g_dim_us / g_frames));
        seam_log(m);
    }
    g_shown = true;
}

// ---- the click ---------------------------------------------------------------------------------
// A left click on the overlay centres the view on the clicked tile (a left drag keeps doing it as the pointer
// moves); a right click is swallowed and, with [ui] bigmap_orders=1, gives the selection the side minimap's order. The events are rewritten in the ring BEFORE the game drains it (we run
// on the strategic pre-input detour), as plain pointer moves with the button bits cleared, so neither the
// world click detection nor the HUD ever sees a button over the overlay. A gesture keeps the geometry of its
// first frame: a camera-centred overlay re-centres under the pointer after each camera move, and re-reading
// it live would feed the move back into itself.
constexpr uintptr_t RING_EV   = mh::addr::_G_LLM_INPUT_MOUSE_EVENTS;
constexpr uintptr_t RING_W    = mh::addr::_G_LLM_INPUT_MOUSE_WRITE_IDX;
constexpr uintptr_t RING_R    = mh::addr::_G_LLM_INPUT_MOUSE_READ_IDX;
constexpr int       EV_STRIDE = 0x38;
bool                g_ldrag = false, g_rswallow = false;
Shown               g_gesture; // the geometry the left gesture started with

bool over_overlay(const Shown &S, int x, int y) {
    const int pad = FRAME_PX;
    return S.valid && x >= S.g.x0 - pad && y >= S.g.y0 - pad && x < S.g.x0 + S.g.mw * S.g.k + pad && y < S.g.y0 + S.g.mh * S.g.k + pad;
}

// The map tile under a pointer position (clamped to the image, wrapped on the torus).
void tile_at(const Shown &S, int x, int y, int *tc, int *tr) {
    int ix = floor_div(x - S.g.x0, S.g.k), iy = floor_div(y - S.g.y0, S.g.k);
    if (ix < 0) ix = 0;
    if (ix >= S.g.mw) ix = S.g.mw - 1;
    if (iy < 0) iy = 0;
    if (iy >= S.g.mh) iy = S.g.mh - 1;
    *tc = floor_mod(ix + S.g.cx - S.g.mw / 2, S.g.mw);
    *tr = floor_mod(iy + S.g.cy - S.g.mh / 2, S.g.mh);
}

void cam_goto_xy(const Shown &S, int x, int y, bool drag) {
    int tc, tr;
    tile_at(S, x, y, &tc, &tr);
    // The view centre is at world px cam*32 + crop origin + crop/2 (the same sum draw() inverts).
    const int cc = floor_mod(tc - floor_div(S.zl + S.cw / 2, TILE_PX), S.g.mw), cr = floor_mod(tr - floor_div(S.zt + S.ch / 2, TILE_PX), S.g.mh);
    if (*mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_COL) == cc && *mh::state::ptr<const int32_t>(mh::state::RID_MAP_CAM_ROW) == cr) return; // a held pointer that has not moved
    mh::call::llm_map_cam_set_col((int32_t)cc);
    mh::call::llm_map_cam_set_row((int32_t)cr);
    mh::call::llm_map_cam_mark_viewport_dirty();
    if (g_logs < 40) {
        ++g_logs;
        char m[160];
        wsprintfA(m, "; BIGMAP: %s at (%d,%d) -> tile (%d,%d), camera (%d,%d)\n", drag ? "drag" : "click", x, y, tc, tr, cc, cr);
        seam_log(m);
    }
}

// ---- the right click: the side minimap's order ---------------------------------------------------
// mods:MM4C. A right click on the overlay does what a right click on the side minimap does: the ladder of
// llm_strat_input_update 0x00441b88 that runs when _G_LLM_MINIMAP_CLICK_VALID is set, over the clicked tile.
// Transcribed with the same tests in the same order, calling the same helpers and the same issuers (which
// schedule the order through llm_strat_order_dispatch, so it is lockstep-safe: no sim write happens here).
// "iu.c N" cites the line of tmp/mm4c/iu.c, the decompile of that function. With libmh present the issuers are
// whichever body owns the stock entry (the wrappers call the stock address; a promoted body is a jmp there).
//
// Not reproduced, on purpose: the stock sets _G_LLM_RMB_GESTURE_STATE = 1 (cleared by its own button-up edge,
// which the overlay eats, so a copy would stick) and the 5-frame cursor icon flash (a local icon id the
// overlay's own cursor does not use).
constexpr uintptr_t ADDR_RMB_T_CUR  = 0x005d0bacu;                      // _G_LLM_RMB_DBLCLICK_T_CUR: double, game clock of the last RMB press
constexpr uintptr_t ADDR_RMB_T_PREV = 0x005d0bb4u;                      // _G_LLM_RMB_DBLCLICK_T_PREV: double, the press before it
constexpr uintptr_t ADDR_WIN_C4F    = 0x00500c4fu;                      // _G_LLM_RMB_DBLCLICK_WINDOW_C4F: double 0.4 (own-unit tile move)
constexpr uintptr_t ADDR_WIN_C57    = 0x00500c57u;                      // ..._C57: enter-building window
constexpr uintptr_t ADDR_WIN_C5F    = 0x00500c5fu;                      // ..._C5F: empty-tile move window
constexpr unsigned  SEL_UNIT = 0x20, SEL_UNIT2 = 0x80, SEL_BLDG = 0x40; // type masks of the tile finders
constexpr int       BLDG_TYPE_A_TURRET = 5, BLDG_TYPE_H_TURRET = 25;
constexpr int       CFG_BUILDING_STRIDE = 0x842;
constexpr int       TILE_VISIBILITY_OFF = 7; // tile_objects element +7: the visibility refcount (0 = not in sight)

int g_dt_milli = 0, g_win_milli = 0; // T_CUR - T_PREV and game_speed * the empty-tile window, x1000, for the log line

void log_order(int x, int y, int tc, int tr, const char *what, int group, int dbl, int shift) {
    char m[300];
    wsprintfA(m, "; BIGMAP: order at (%d,%d) -> tile (%d,%d): %s [group %d, double click %d, shift %d, dt %d window %d]\n", x, y, tc, tr, what, group, dbl, shift, g_dt_milli, g_win_milli);
    seam_log(m);
}

void right_click_order(const Shown &S, int x, int y) {
    int tc, tr;
    tile_at(S, x, y, &tc, &tr);
    // iu.c 277 / 511 (the placement clear in the stock world and non-minimap arms): the stock right click disarms a building (BUILD_PLACEMENT_ID = 0). User decision
    // 2026-10-17: on the overlay that is ALL it does -- no order follows.
    int32_t *placing = mh::state::ptr<int32_t>(mh::state::RID_BUILD_PLACEMENT_ID);
    if (placing && *placing != 0) {
        *placing = 0;
        log_order(x, y, tc, tr, "cancel placement, no order", 0, 0, 0);
        return;
    }
    // iu.c 243-246: the press edge stamps the double-click clock (T_PREV = T_CUR; T_CUR = now) before the ladder reads it.
    const double now = *mh::state::ptr<const double>(mh::state::RID_CURRENT_GAME_TIME);
    const double cur = rd<double>(ADDR_RMB_T_CUR);
    memcpy((void *)ADDR_RMB_T_PREV, &cur, 8);
    memcpy((void *)ADDR_RMB_T_CUR, &now, 8);

    const unsigned side     = *mh::state::ptr<const uint16_t>(mh::state::RID_PLAYERSIDE);
    const int      group    = *mh::state::ptr<const int32_t>(mh::state::RID_STRAT_CTRL_GROUPS); // CTRL_GROUPS[0].count
    const bool     shift    = (rd<uint8_t>(mh::addr::_G_LLM_KEY_RSHIFT_HELD) & 1) != 0 || (rd<uint8_t>(mh::addr::_G_LLM_KEY_LSHIFT_HELD) & 1) != 0;
    const double   gspeed   = *mh::state::ptr<const double>(mh::state::RID_GAME_SPEED);
    const double   since    = rd<double>(ADDR_RMB_T_CUR) - rd<double>(ADDR_RMB_T_PREV);
    g_dt_milli              = (int)(since * 1000.0);
    g_win_milli             = (int)(gspeed * rd<double>(ADDR_WIN_C5F) * 1000.0);
    auto           dbl      = [&](uintptr_t win) { return since < gspeed * rd<double>(win); }; // T_CUR - T_PREV < game_speed * WINDOW
    const uint8_t *tobj     = mh::state::ptr<const uint8_t>(mh::state::RID_TILE_OBJECTS);
    const bool     visible  = tobj && tobj[(size_t)(((tc & 255) << 8) | (tr & 255)) * 8 + TILE_VISIBILITY_OFF] != 0;
    uint32_t       found    = 0;
    uint16_t       owner[4] = {};

    if (group == 0) {
        // iu.c 307-352: nothing selected. Only a selected TURRET with a visible enemy on the tile acts.
        const unsigned bsel = *mh::state::ptr<const uint16_t>(mh::state::RID_STRAT_UI_SELECTED_BLDG_INDEX);
        if (bsel == 0 || !visible || side > 7 || bsel >= mh::caps::g(mh::caps::BUILDINGS).limit) {
            log_order(x, y, tc, tr, "no order (nothing selected)", group, 0, shift);
            return;
        }
        const uint8_t *cfgb = mh::state::ptr<const uint8_t>(mh::state::RID_BUILDING);
        if (!cfgb) return;
        const uint8_t *b  = mh::caps::rec<const uint8_t>(mh::caps::BUILDINGS, side, bsel);
        const unsigned id = (unsigned)(b[2] | (b[3] << 8));                            // map_object_building.building_id
        const int      ty = id < 100 ? cfgb[(size_t)id * CFG_BUILDING_STRIDE + 8] : 0; // cfg Building.type
        if (ty != BLDG_TYPE_A_TURRET && ty != BLDG_TYPE_H_TURRET) {
            log_order(x, y, tc, tr, "no order (nothing selected, not a turret)", group, 0, shift);
            return;
        }
        if (!mh::call::llm_map_tile_has_enemy(tc, tr)) {
            log_order(x, y, tc, tr, "no order (turret, no enemy)", group, 0, shift);
            return;
        }
        if (mh::call::llm_map_tile_find_enemy_of_type(tc, tr, &found, owner, SEL_UNIT)) {
            mh::call::llm_strat_turret_order_target_unit((uint16_t)side, (uint16_t)bsel, owner[0], found);
            log_order(x, y, tc, tr, "turret target unit", group, 0, shift);
        } else if (mh::call::llm_map_tile_find_enemy_of_type(tc, tr, &found, owner, SEL_UNIT2)) {
            mh::call::llm_strat_turret_order_target_unit((uint16_t)side, (uint16_t)bsel, owner[0], found);
            log_order(x, y, tc, tr, "turret target unit", group, 0, shift);
        } else if (mh::call::llm_map_tile_find_enemy_of_type(tc, tr, &found, owner, SEL_BLDG)) {
            mh::call::llm_strat_turret_order_target_building((uint16_t)side, (uint16_t)bsel, owner[0], found);
            log_order(x, y, tc, tr, "turret target building", group, 0, shift);
        } else {
            log_order(x, y, tc, tr, "no order (turret, enemy not found)", group, 0, shift);
        }
        return;
    }

    // iu.c 353-461: units selected (the else of CTRL_GROUPS[0].count == 0 at 307).
    if (!visible) { // iu.c 356-359
        mh::call::llm_strat_group_issue_move_order_deferred(tc, tr, 0x8000);
        log_order(x, y, tc, tr, "move (deferred, tile not visible)", group, 0, shift);
        return;
    }
    if (mh::call::llm_map_tile_has_enemy(tc, tr)) { // iu.c 437-458: attack, finder order 0x20, 0x80, 0x40
        const unsigned sels[3] = {SEL_UNIT, SEL_UNIT2, SEL_BLDG};
        for (unsigned sel : sels)
            if (mh::call::llm_map_tile_find_enemy_of_type(tc, tr, &found, owner, (int16_t)sel)) {
                mh::call::llm_strat_group_issue_attack_order(found, owner[0], (uint16_t)sel);
                char what[64];
                wsprintfA(what, "attack 0x%x target %u side %u", sel, (unsigned)found, (unsigned)owner[0]);
                log_order(x, y, tc, tr, what, group, 0, shift);
                return;
            }
        log_order(x, y, tc, tr, "no order (enemy not found)", group, 0, shift);
        return;
    }
    if (!mh::call::llm_map_tile_has_own(tc, tr)) { // iu.c 365-379: empty tile
        const bool d = dbl(ADDR_WIN_C5F);
        if (d || shift) {
            mh::call::llm_strat_group_issue_move_order_confirmed(tc, tr);
            log_order(x, y, tc, tr, "move (confirmed)", group, d, shift);
        } else {
            mh::call::llm_strat_group_issue_move_order_deferred(tc, tr, 0x8000);
            log_order(x, y, tc, tr, "move (deferred)", group, 0, shift);
        }
        return;
    }
    // iu.c 380-436: an own unit or building on the tile.
    if (mh::call::llm_map_tile_find_own_of_type(tc, tr, &found, SEL_UNIT2)) { // iu.c 414-425: an own unit
        const bool d = dbl(ADDR_WIN_C4F);
        if (d || shift) {
            mh::call::llm_strat_group_issue_move_order(tc, tr);
            log_order(x, y, tc, tr, "move (own unit tile, confirmed)", group, d, shift);
        } else {
            mh::call::llm_strat_group_issue_move_order_deferred(tc, tr, 0x8000);
            log_order(x, y, tc, tr, "move (own unit tile, deferred)", group, 0, shift);
        }
        return;
    }
    if (!mh::call::llm_map_tile_find_own_of_type(tc, tr, &found, SEL_BLDG)) { // iu.c 389-396
        mh::call::llm_strat_group_issue_move_order_deferred(tc, tr, 0x8000);
        log_order(x, y, tc, tr, "move (deferred, own tile, nothing found)", group, 0, shift);
        return;
    }
    // iu.c 397-413: an own building: a double click or Shift enters it when it has a vehicle sub id.
    const bool d   = dbl(ADDR_WIN_C57);
    uint32_t   sub = 0;
    if (d || shift) sub = mh::call::llm_strat_get_vehicle_building_sub_id(side, (int32_t)found);
    if (sub == 0) {
        mh::call::llm_strat_group_issue_move_order_deferred(tc, tr, 0x8000);
        log_order(x, y, tc, tr, "move (deferred, own building)", group, d, shift);
    } else {
        mh::call::llm_strat_group_issue_enter_building_order(sub, tc, tr);
        log_order(x, y, tc, tr, "enter building", group, d, shift);
    }
}

void on_pre_input() {
    if (!g_on) return;
    const int w = rd<int32_t>(RING_W) & 0x7f;
    for (int i = rd<int32_t>(RING_R) & 0x7f; i != w; i = (i + 1) & 0x7f) {
        uint8_t       *ev   = (uint8_t *)(RING_EV + (uintptr_t)i * EV_STRIDE);
        const uint32_t type = rd<uint32_t>((uintptr_t)ev + 4);
        const int      x = rd<int32_t>((uintptr_t)ev + 0x14), y = rd<int32_t>((uintptr_t)ev + 0x18);
        const bool     live = g_shown && g_shown_geom.valid;
        bool           eat  = false;
        if (type == 2) { // left down
            if (live && over_overlay(g_shown_geom, x, y)) {
                g_ldrag   = true;
                g_gesture = g_shown_geom;
                cam_goto_xy(g_gesture, x, y, false);
                eat = true;
            }
        } else if (type == 4) { // left up
            if (g_ldrag) {
                g_ldrag = false;
                eat     = true;
            }
        } else if (type == 8) { // right down
            if (live && over_overlay(g_shown_geom, x, y)) {
                g_rswallow = true;
                eat        = true;
                if (g_orders) right_click_order(g_shown_geom, x, y);
            }
        } else if (type == 0x10) { // right up
            if (g_rswallow) {
                g_rswallow = false;
                eat        = true;
            }
        } else if (type == 1) { // move
            if (g_ldrag) {
                if (rd<uint32_t>((uintptr_t)ev) & 1) cam_goto_xy(g_gesture, x, y, true);
                eat = true;
            } else if (g_rswallow) {
                eat = true;
            }
        }
        if (eat) {
            const uint32_t b = rd<uint32_t>((uintptr_t)ev) & ~3u;
            memcpy(ev, &b, 4);
            const uint32_t mv = 1;
            memcpy(ev + 4, &mv, 4);
        }
    }
}

// ---- the draw point ----------------------------------------------------------------------------
void frame_step() {
    const bool want = key_held() && screen_ok();
    if (want) {
        draw();
        return;
    }
    if (g_shown) { // it just stopped (key released, chat opened, a dialog, the mode changed): repaint under it once more
        const int vw = *mh::state::ptr<const int32_t>(mh::state::RID_G_WIN_W), vh = *mh::state::ptr<const int32_t>(mh::state::RID_G_WIN_H);
        stamp_view(vw, vh, nullptr, nullptr);
        g_shown            = false;
        g_shown_geom.valid = false;
        if (g_logs < 40) {
            ++g_logs;
            char m[160];
            wsprintfA(m, "; BIGMAP: hidden after %u frame(s), avg %u us per present\n", g_frames, g_frames ? (unsigned)(g_sum_us / g_frames) : 0u);
            seam_log(m);
        }
        g_frames = 0;
        g_sum_us = g_dim_us = 0;
    }
}

void __cdecl splice_impl() {
    if (!g_init) init();
    if (!g_on) return;
    g_drawn_in_frame = true;
    frame_step();
}

// llm_frame_present calls llm_gfx_draw_cursor_menu (0x004a5b4d) at 0x004263ba; this runs first, so the overlay
// is in the framebuffer when the cursor is drawn over it. Every register and the flags survive (the stock call
// takes register arguments); the return address stays, so the cursor routine returns into llm_frame_present.
constexpr uintptr_t SPLICE_SITE = 0x004263bau;
uintptr_t           g_cursor_fn = 0x004a5b4du;
// clang-format off
__declspec(naked) void splice_thunk() {
    __asm {
        pushad
        pushfd
        call splice_impl
        popfd
        popad
        jmp  dword ptr [g_cursor_fn]
    }
}
// clang-format on

} // namespace

extern "C" int MH_BigMap_Install(void) {
    if (!g_init) init();
    if (!g_on) return 0;
    if (mh::seams::strat_pre_input_add(on_pre_input, "the mods:MM4 big-map click hook")) seam_log(g_orders ? "; BIGMAP: click armed (left click/drag centres the camera, right click orders the selection)\n" : "; BIGMAP: click armed (left click/drag centres the camera, right click swallowed)\n");
    else seam_log("; BIGMAP: click NOT armed (pre-input hook refused)\n");
    static const uint8_t expect[5] = {0xE8, 0x8E, 0xF7, 0x07, 0x00};
    uint8_t              repl[5];
    repl[0]           = 0xE8;
    const int32_t rel = (int32_t)((uintptr_t)&splice_thunk - (SPLICE_SITE + 5));
    memcpy(repl + 1, &rel, 4);
    g_spliced = mh::hook::patch_bytes_guarded(SPLICE_SITE, expect, repl, 5);
    seam_log(g_spliced ? "; BIGMAP: drawn before the cursor (splice at 0x004263ba)\n" : "; BIGMAP: cursor splice NOT armed (bytes differ); drawn at present, over the cursor\n");
    return g_spliced ? 1 : 0;
}

extern "C" void MH_BigMap_OnPresent(void) {
    if (!g_init) init();
    if (!g_on) return;
    if (g_spliced && g_drawn_in_frame) { // the splice already ran this frame
        g_drawn_in_frame = false;
        return;
    }
    frame_step();
}
