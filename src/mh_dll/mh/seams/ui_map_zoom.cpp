//
// seams/ui_map_zoom.cpp -- RES2: the strategic map zoom on the mouse wheel, anchored at the cursor.
//
// Full mechanism: include/mh_mapzoom_export.h. The arithmetic (crop size, limits, anchor, camera absorb,
// cursor translation, wheel accumulator) is gfx/map_zoom.{h,cpp}; the resampler is gfx/zoom_scale.{h,cpp};
// this file is the glue to the game's memory and functions.
//
// THE WHEEL IS READ, NOT HOOKED. Both of the game's mouse producers -- the DirectInput poll (own or system
// backend) and the window-message tap -- add the wheel to one cumulative int,
// _G_LLM_INPUT_MOUSE_WHEEL_TOTAL (0x00e69da0), that nothing else reads. The hook below takes its difference
// since the last call. That is backend-neutral, and the ImGui overlay's capture needs no code here: while
// the overlay owns the wheel it withholds the DirectInput record / swallows the message, so the total never
// moves. Wheel movement while the zoom cannot use it (a menu, the pause screen, the pointer over the HUD)
// is dropped, not queued.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <math.h>
#include <string.h>
#include <cstdarg>
#include <cstdint>

#include "include/mh_mapzoom_export.h"
#include "include/mh_run_context.h" // mh_run_path
#include "include/mh_config_dir.h"  // RL3: <config dir>mh_net.ini
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "addr/mh_calls.gen.h"      // llm_map_cam_set_col / _row / mark_viewport_dirty
#include "addr/mh_structs.gen.h"    // mh_llm_strat_map_geom (the camera wrap masks)
#include "config/ini_read.h"        // read_ini_string
#include "gfx/cpu_scale.h"          // the filter numbering
#include "gfx/ddraw_own.h"          // owned_ddraw_active / owned_filter
#include "gfx/map_zoom.h"
#include "gfx/zoom_scale.h"
#include "hook/detour.h"
#include "seams/strat_pre_input.h"
#include "en_guard.h"

#pragma comment(lib, "user32.lib") // wsprintfA / GetPrivateProfile*

namespace {

namespace mz = mh::gfx::mapzoom;

// ---- game addresses (EN) ----------------------------------------------------------------------------
constexpr uintptr_t ADDR_APPLY      = 0x004a8084u; // llm_map_zoom_apply            void(void)
constexpr uintptr_t ADDR_TRANSLATE  = 0x004a8145u; // llm_map_zoom_translate_cursor void(void)
constexpr uintptr_t ADDR_STEP       = 0x0049a849u; // llm_map_view_zoom_step        void(int dir in EAX)
constexpr uintptr_t ADDR_MODE       = mh::addr::_G_LLM_GAME_MODE;
constexpr uintptr_t ADDR_WIN_W      = mh::addr::G_WIN_W; // the viewport (window minus the 160 px panel / 32 px strip)
constexpr uintptr_t ADDR_WIN_H      = mh::addr::G_WIN_H;
constexpr uintptr_t ADDR_RENDER_H   = mh::addr::WindowHeight;
constexpr uintptr_t ADDR_FB         = mh::addr::_G_LLM_FRAMEBUFFER;
constexpr uintptr_t ADDR_FB_PITCH   = mh::addr::_G_LLM_FB_PITCH; // bytes
constexpr uintptr_t ADDR_CUR_X      = mh::addr::_G_LLM_CURSOR_X;
constexpr uintptr_t ADDR_CUR_Y      = mh::addr::_G_LLM_CURSOR_Y;
constexpr uintptr_t ADDR_CUR_MENU_X = 0x00e58cccu;                       // _G_LLM_CURSOR_MENU_X: the untranslated cursor
constexpr uintptr_t ADDR_CUR_MENU_Y = 0x00e58cc8u;                       // _G_LLM_CURSOR_MENU_Y
constexpr uintptr_t ADDR_SHAKE_X    = mh::addr::_G_LLM_MAP_ZOOM_SCALE_X; // double: the screen-shake factor (1.0 at rest)
constexpr uintptr_t ADDR_SHAKE_Y    = 0x005d8b0cu;                       // _G_LLM_MAP_ZOOM_SCALE_Y
constexpr uintptr_t ADDR_WHEEL      = 0x00e69da0u;                       // _G_LLM_INPUT_MOUSE_WHEEL_TOTAL: cumulative, 120 per notch
constexpr uintptr_t ADDR_COLOR_FMT  = 0x00559a04u;                       // G_COLOR_FORMAT: 565 / 555
constexpr uintptr_t ADDR_GAME_TIME  = mh::addr::CURRENT_GAME_TIME;       // double (unaligned)
constexpr uintptr_t ADDR_CAM_COL    = mh::addr::_G_LLM_MAP_CAM_COL;
constexpr uintptr_t ADDR_CAM_ROW    = mh::addr::_G_LLM_MAP_CAM_ROW;
constexpr uintptr_t ADDR_GEOM       = mh::addr::general; // mh_llm_strat_map_geom

constexpr uint8_t MODE_STRAT  = 2;
constexpr uint8_t MODE_PAUSED = 5;
constexpr uint8_t MODE_TACT   = 6;

constexpr int LOG_FIRST = 60;  // log this many zoom changes, then go quiet
constexpr int PERF_LOG  = 300; // one perf line per this many resamples

// ---- state ------------------------------------------------------------------------------------------
bool            g_armed  = false;
bool            g_wheel  = true;       // [input] wheel_zoom
double          g_user_z = 1.0;        // what the player asked for (>= 1)
double          g_cur_z0 = 1.0;        // the zoom the crop origin below belongs to (before the shake factor)
int             g_left = 0, g_top = 0; // the crop origin in viewport pixels
int             g_ini_filter = 3;      // [video] filter, for the system backend (where the presenter's is not ours)
mz::wheel_acc   g_acc;
int             g_last_total = 0;
bool            g_have_total = false;
uint8_t         g_last_mode  = 0;   // the mode at the previous call of the pump
DWORD           g_last_tick  = 0;   // GetTickCount at the previous call
constexpr DWORD GAP_MS       = 750; // a longer silence than any frame: a menu or screen sat in between
int             g_last_w = 0, g_last_h = 0, g_last_rh = 0;
double          g_last_time = 0.0;

long     g_changes    = 0;
LONGLONG g_perf_ticks = 0;
long     g_perf_n     = 0;
LONGLONG g_qpf        = 0;

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0; // SES1: per-SESSION (shares mh_input.log with ui_keyrepeat / ui_pause)

void zlog(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_input.log", &g_log_gen);
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
    mh_logq_write(g_log, line, lstrlenA(line));
}

template <class T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void *)a, sizeof(T));
    return v;
}

// printf-free fixed point for the log (wsprintf has no %f): z as "1.20".
void fmt2(char *out, double v) {
    const int x = (int)floor(v * 100.0 + 0.5);
    wsprintfA(out, "%d.%02d", x / 100, x % 100);
}

const mh::game::mh_llm_strat_map_geom &geom() {
    return *(const mh::game::mh_llm_strat_map_geom *)ADDR_GEOM;
}

double shake() {
    const double s = rd<double>(ADDR_SHAKE_X);
    return (s >= 1.0 && s < 8.0) ? s : 1.0; // anything else is a torn read or garbage: no shake
}

int cur_filter() {
    return mh::gfx::owned_ddraw_active() ? mh::gfx::owned_filter() : g_ini_filter;
}

void reset(const char *why) {
    if (g_user_z != 1.0 || g_cur_z0 != 1.0 || g_left || g_top) zlog("; [zoom] reset (%s)", why);
    g_user_z = g_cur_z0 = 1.0;
    g_left = g_top = 0;
    g_acc          = mz::wheel_acc();
}

// Move the camera by whole tiles (the strategic camera has no sub-tile offset). The same three calls the
// stock scroll code makes: set the column, the row, mark the viewport dirty.
void move_camera(int dcol, int drow) {
    if (!dcol && !drow) return;
    const mh::game::mh_llm_strat_map_geom &g = geom();
    if (dcol) mh::call::llm_map_cam_set_col((int32_t)(g.width_mask & (uint32_t)(rd<int32_t>(ADDR_CAM_COL) + dcol)));
    if (drow) mh::call::llm_map_cam_set_row((int32_t)(g.height_mask & (uint32_t)(rd<int32_t>(ADDR_CAM_ROW) + drow)));
    mh::call::llm_map_cam_mark_viewport_dirty();
}

// Bring the crop origin and the camera in line with the zoom the player wants (g_user_z) -- anchored at viewport pixel (cx, cy), which is the cursor for a wheel step and the centre for
// everything else. Called from the pre-input hook and from the F8/F9 step, both before the pump.
void retarget(int cx, int cy, const char *why) {
    const int W = rd<int32_t>(ADDR_WIN_W), H = rd<int32_t>(ADDR_WIN_H), RH = rd<int32_t>(ADDR_RENDER_H);
    if (W < 64 || H < 64) return;
    const double zmax = mz::max_zoom(RH);
    if (g_user_z < 1.0) g_user_z = 1.0;
    if (g_user_z > zmax) g_user_z = zmax;
    const double z0 = g_user_z;
    if (fabs(z0 - g_cur_z0) < 1e-9) return;

    const double s = shake(), zo = g_cur_z0 * s, zn = z0 * s;
    if (cx < 0) cx = 0;
    if (cx >= W) cx = W - 1;
    if (cy < 0) cy = 0;
    if (cy >= H) cy = H - 1;
    const int cw_o = mz::crop_dim(W, zo), ch_o = mz::crop_dim(H, zo);
    const int l0 = mz::clamp_origin(g_left, W, zo), t0 = mz::clamp_origin(g_top, H, zo);
    int       nl, nt, dcol, drow;
    mz::anchor_axis(l0, W, zo, zn, cx, &nl, &dcol);
    mz::anchor_axis(t0, H, zo, zn, cy, &nt, &drow);
    const int cw_n = mz::crop_dim(W, zn), ch_n = mz::crop_dim(H, zn);

    if (g_changes < LOG_FIRST) {
        // The anchor check: the map pixel under the cursor before and after (camera tiles included). The two
        // agree to the pixel unless the crop had no slack left to hold the difference.
        const int wx0 = rd<int32_t>(ADDR_CAM_COL) * mz::TILE_PX + l0 + (int)((long long)cx * cw_o / W);
        const int wy0 = rd<int32_t>(ADDR_CAM_ROW) * mz::TILE_PX + t0 + (int)((long long)cy * ch_o / H);
        const int wx1 = (rd<int32_t>(ADDR_CAM_COL) + dcol) * mz::TILE_PX + nl + (int)((long long)cx * cw_n / W);
        const int wy1 = (rd<int32_t>(ADDR_CAM_ROW) + drow) * mz::TILE_PX + nt + (int)((long long)cy * ch_n / H);
        char      a[16], b[16];
        fmt2(a, g_cur_z0);
        fmt2(b, z0);
        zlog("; [zoom] %s z %s -> %s cursor=%d,%d origin %d,%d -> %d,%d crop %dx%d cam %d,%d tiles "
             "anchor err %d,%d px view=%dx%d",
             why, a, b, cx, cy, l0, t0, nl, nt, cw_n, ch_n, dcol, drow, wx1 - wx0, wy1 - wy0, W, H);
    }
    ++g_changes;
    g_left   = nl;
    g_top    = nt;
    g_cur_z0 = z0;
    move_camera(dcol, drow);
}

int cursor_menu_x() { return rd<int32_t>(ADDR_CUR_MENU_X); }
int cursor_menu_y() { return rd<int32_t>(ADDR_CUR_MENU_Y); }

// ---- the run-before hook ----------------------------------------------------------------------------

// A state line whenever the game mode or the game's scale doubles change (the shake factor; 1.0 at rest) or the
// zoom the module holds changes between two calls of the pump -- so a zoom that moves without a wheel step can
// be read back to the value that moved it. Bounded: the shake moves the doubles every frame while it runs.
void trace_state(uint8_t mode) {
    static long  s_n    = 0;
    static int   s_mode = -1, s_sx = 0, s_sy = 0, s_z = 0;
    const double sx = rd<double>(ADDR_SHAKE_X), sy = rd<double>(ADDR_SHAKE_Y);
    const int    ix = (int)floor(sx * 1000.0 + 0.5), iy = (int)floor(sy * 1000.0 + 0.5);
    const int    iz = (int)floor(g_cur_z0 * 1000.0 + 0.5);
    if (mode == s_mode && ix == s_sx && iy == s_sy && iz == s_z) return;
    s_mode = mode;
    s_sx   = ix;
    s_sy   = iy;
    s_z    = iz;
    if (++s_n > 200) return;
    zlog("; [zoom] state mode=%d game scale=%d,%d (x1000) player z=%d user z=%d (x1000) origin %d,%d", (int)mode, ix, iy, iz,
         (int)floor(g_user_z * 1000.0 + 0.5), g_left, g_top);
}

void on_strat_input() {
    if (!g_armed) return;
    const uint8_t mode = rd<uint8_t>(ADDR_MODE);
    const int     tot  = rd<int32_t>(ADDR_WHEEL);
    // The pump only runs in the strategic modes, so wheel movement during a menu, a dialog or another screen is
    // not seen while it happens. Re-seed the baseline on the first call after any gap (another mode, or a long
    // silence) so that input is dropped, not applied as one step when the game returns.
    const DWORD   tick       = GetTickCount();
    const bool    gap        = g_have_total && ((g_last_mode != MODE_STRAT && g_last_mode != MODE_PAUSED) || tick - g_last_tick > GAP_MS);
    const int     delta      = (g_have_total && !gap) ? tot - g_last_total : 0;
    const int     prev_total = g_last_total;
    const uint8_t prev_mode  = g_last_mode;
    const DWORD   prev_tick  = g_last_tick;
    g_last_total             = tot;
    g_have_total             = true;
    g_last_tick              = tick;
    g_last_mode              = mode;
    if (gap) {
        g_acc                 = mz::wheel_acc();
        static int s_gap_logs = 0;
        if (tot != prev_total && s_gap_logs++ < 20)
            zlog("; [zoom] wheel total moved %d while the pump was away (last mode %d, %d ms): dropped", tot - prev_total,
                 (int)prev_mode, (int)(tick - prev_tick));
    }
    trace_state(mode);
    if (mode != MODE_STRAT) {
        g_acc = mz::wheel_acc();
        return;
    }

    // A new size or a restarted clock (a new game) starts from 1x, like the stock resets in
    // llm_view_set_size_mode / llm_strat_new_game_init.
    const int    W = rd<int32_t>(ADDR_WIN_W), H = rd<int32_t>(ADDR_WIN_H), RH = rd<int32_t>(ADDR_RENDER_H);
    const double now = rd<double>(ADDR_GAME_TIME);
    if (W != g_last_w || H != g_last_h || RH != g_last_rh) {
        reset("view size changed");
        g_last_w  = W;
        g_last_h  = H;
        g_last_rh = RH;
    } else if (now + 1.0 < g_last_time) {
        reset("game clock restarted");
    }
    g_last_time = now;

    int cx = W / 2, cy = H / 2;
    if (g_wheel && delta != 0) {
        const int mx = cursor_menu_x(), my = cursor_menu_y();
        if (mx >= 0 && my >= 0 && mx < W && my < H) { // over the map, not the panel / strip
            const int n = mz::wheel_notches(g_acc, delta);
            if (n != 0) {
                // In: a notch forward multiplies; out: divides, never below 1x (the zoom is magnify-only).
                g_user_z *= pow(mz::WHEEL_STEP, (double)n);
                cx = mx;
                cy = my;
                retarget(cx, cy, n > 0 ? "wheel in " : "wheel out");
                return;
            }
        } else {
            g_acc = mz::wheel_acc();
        }
    }
    retarget(cx, cy, "limit    "); // a changed limit (the view got smaller), anchored at the centre
}

// ---- the replaced functions ---------------------------------------------------------------------------

// F8 (dir 2, zoom in) / F9 (dir 1, zoom out): the stock additive step, the wheel's limits, anchored at the
// centre of the viewport (a key has no pointer).
void __cdecl step_impl(int dir) {
    if (dir == 2) g_user_z += mz::KEY_STEP;
    else if (dir == 1) g_user_z -= mz::KEY_STEP;
    else return;
    if (g_user_z < 1.0) g_user_z = 1.0;
    const int W = rd<int32_t>(ADDR_WIN_W), H = rd<int32_t>(ADDR_WIN_H);
    retarget(W / 2, H / 2, dir == 2 ? "key in    " : "key out   ");
}

// clang-format off
// __watcall passes the direction in EAX; the C++ body is cdecl. EBX/ECX/EDX are scratch in both.
__declspec(naked) void step_thunk() {
    __asm {
        push eax
        call step_impl
        add  esp, 4
        ret
    }
}
// clang-format on

// The raw game scale on one axis (the shake factor; 1.0 at rest). Garbage or a torn read counts as no shake.
double shake_axis(uintptr_t a) {
    const double s = rd<double>(a);
    return (s >= 1.0 && s < 8.0) ? s : 1.0;
}

// The picture's crop this frame: the player's crop (g_left, g_top, crop_dim of g_cur_z0) magnified further by
// the game's shake scale about its own centre. With no player zoom that is exactly the stock centred crop
// (W - cw) / 2, which is what the stock apply / translate compute from the scale doubles.
struct frame_crop {
    bool   active; // false: the picture is not zoomed
    int    cw, ch; // the crop size
    int    L, T;   // the crop origin in viewport pixels
    double zx, zy; // the total zoom per axis
};

frame_crop current_crop(int W, int H) {
    frame_crop c{};
    if (rd<uint8_t>(ADDR_MODE) == MODE_TACT) return c;
    c.zx = g_cur_z0 * shake_axis(ADDR_SHAKE_X);
    c.zy = g_cur_z0 * shake_axis(ADDR_SHAKE_Y);
    if (c.zx <= 1.0 + 1e-9 && c.zy <= 1.0 + 1e-9) return c;
    if (W < 64 || H < 64) return c;
    const int cwp = mz::crop_dim(W, g_cur_z0), chp = mz::crop_dim(H, g_cur_z0);
    const int l0 = mz::clamp_origin(g_left, W, g_cur_z0), t0 = mz::clamp_origin(g_top, H, g_cur_z0);
    c.cw = mz::crop_dim(W, c.zx);
    c.ch = mz::crop_dim(H, c.zy);
    if (c.cw >= W && c.ch >= H) return c;
    c.L      = l0 + (cwp - c.cw) / 2;
    c.T      = t0 + (chp - c.ch) / 2;
    c.active = true;
    return c;
}

void __cdecl apply_impl() {
    const int        W = rd<int32_t>(ADDR_WIN_W), H = rd<int32_t>(ADDR_WIN_H);
    const frame_crop fc = current_crop(W, H);
    if (!fc.active) return;
    const int    cw = fc.cw, ch = fc.ch, L = fc.L, T = fc.T;
    const double z     = fc.zx;
    uint16_t    *fb    = rd<uint16_t *>(ADDR_FB);
    const int    pitch = rd<int32_t>(ADDR_FB_PITCH) / 2;
    if (!fb || pitch < W) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool ok = mh::gfx::zoom::stretch_crop(fb, pitch, L, T, cw, ch, W, H, cur_filter(), rd<int32_t>(ADDR_COLOR_FMT) == 555);
    QueryPerformanceCounter(&t1);
    if (ok) {
        if (!g_qpf) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_qpf = f.QuadPart ? f.QuadPart : 1;
        }
        g_perf_ticks += t1.QuadPart - t0.QuadPart;
        if (++g_perf_n >= PERF_LOG) {
            char zs[16];
            fmt2(zs, z);
            zlog("; [zoom] resample avg %d us over %ld frames: z %s crop %dx%d at %d,%d -> %dx%d filter=%s",
                 (int)(g_perf_ticks * 1000000 / g_qpf / g_perf_n), g_perf_n, zs, cw, ch, L, T, W, H,
                 mh::gfx::owned_filter_name(cur_filter()));
            g_perf_ticks = 0;
            g_perf_n     = 0;
        }
    }
    mh::call::llm_map_cam_mark_viewport_dirty(); // the stretch destroyed the frame: repaint every tile next time
}

void __cdecl translate_impl() {
    // The stock first step, unconditionally: keep the raw pointer for the HUD and the cursor sprite.
    const int32_t cx = rd<int32_t>(ADDR_CUR_X), cy = rd<int32_t>(ADDR_CUR_Y);
    memcpy((void *)ADDR_CUR_MENU_X, &cx, sizeof(cx));
    memcpy((void *)ADDR_CUR_MENU_Y, &cy, sizeof(cy));
    const uint8_t mode = rd<uint8_t>(ADDR_MODE);
    if (mode == MODE_TACT) { // a zoom never carries into a mission (the stock code would skew this cursor)
        if (g_cur_z0 != 1.0 || g_user_z != 1.0) reset("tactical");
        return;
    }
    if (mode != MODE_STRAT && mode != MODE_PAUSED) return;
    const int        W = rd<int32_t>(ADDR_WIN_W), H = rd<int32_t>(ADDR_WIN_H);
    const frame_crop fc = current_crop(W, H);
    if (!fc.active) return;
    int32_t nx, ny;
    if (g_cur_z0 == 1.0) {
        // No player zoom: only the game's own shake scale is in play. Reproduce the stock arithmetic exactly
        // (x' = trunc((W - cw) / 2 + x / scale), the viewport test is only the upper bounds, a result equal to
        // the origin becomes 0), so a recorded session replays on the same tiles.
        if (cx >= W || cy >= H) return;
        nx = (int32_t)((double)fc.L + (double)cx / fc.zx);
        ny = (int32_t)((double)fc.T + (double)cy / fc.zy);
        if (nx == fc.L) nx = 0;
        if (ny == fc.T) ny = 0;
    } else {
        if (cx < 0 || cy < 0 || cx >= W || cy >= H) return; // over the panel / strip: not the zoomed picture
        nx = mz::translate_axis(cx, W, fc.L, fc.cw);
        ny = mz::translate_axis(cy, H, fc.T, fc.ch);
    }
    memcpy((void *)ADDR_CUR_X, &nx, sizeof(nx));
    memcpy((void *)ADDR_CUR_Y, &ny, sizeof(ny));
}

bool takeable(uintptr_t a) {
    return mh::hook::detour_refusal(a, mh::hook::entry_claim::exclusive, mh::hook::WATCOM_PROLOGUE, true) ==
           mh::hook::refuse_reason::none;
}

} // namespace

extern "C" void MH_MapZoom_Get(int *z100, int *left, int *top, int *cw, int *ch) {
    const int    W = rd<int32_t>(ADDR_WIN_W), H = rd<int32_t>(ADDR_WIN_H);
    const double z = g_armed ? g_cur_z0 : 1.0;
    if (z100) *z100 = (int)floor(z * 100.0 + 0.5);
    if (left) *left = g_armed ? g_left : 0;
    if (top) *top = g_armed ? g_top : 0;
    if (cw) *cw = g_armed && W > 0 ? mz::crop_dim(W, z) : 0;
    if (ch) *ch = g_armed && H > 0 ? mz::crop_dim(H, z) : 0;
}

extern "C" void MH_MapZoom_ReseedWheel(void) {
    g_have_total = false;
    g_acc        = mz::wheel_acc();
}

extern "C" int MH_MapZoom_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only

    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini); // RL3: <config dir>mh_net.ini

    // [video] zoom_module=0 leaves the stock zoom exactly as it shipped (the rig's fix-off rows).
    if (!mh_ini_get_int("video", "zoom_module", 1, ini)) return 0;
    g_wheel = mh_ini_get_int("input", "wheel_zoom", 1, ini) != 0;
    char f[32];
    mh::config::read_ini_string("video", "filter", "area", f, sizeof(f), ini);
    g_ini_filter = lstrcmpiA(f, "point") == 0 ? 0 : lstrcmpiA(f, "linear") == 0 ? 1
                                                : lstrcmpiA(f, "sharp") == 0    ? 2
                                                                                : 3;

    // All or nothing: the crop the picture uses and the crop the cursor is mapped through must be the same
    // one, so refuse to arm unless all three entries can be taken.
    if (!takeable(ADDR_APPLY) || !takeable(ADDR_TRANSLATE) || !takeable(ADDR_STEP)) {
        zlog("; [zoom] DISARMED: an entry of the stock zoom is not takeable (the reason is on the [interlock] line in mh_net.log)");
        return 0;
    }
    if (!mh::seams::strat_pre_input_add(on_strat_input, "the RES2 map-zoom input hook")) {
        zlog("; [zoom] DISARMED: the strategic pre-input hook was refused (the reason is on the [interlock] line in mh_net.log)");
        return 0;
    }
    using mh::hook::entry_claim;
    const bool ok = mh::hook::install_jmp(ADDR_APPLY, (void *)apply_impl, entry_claim::exclusive, "the RES2 zoom resampler") &&
                    mh::hook::install_jmp(ADDR_TRANSLATE, (void *)translate_impl, entry_claim::exclusive, "the RES2 zoom cursor map") &&
                    mh::hook::install_jmp(ADDR_STEP, (void *)step_thunk, entry_claim::exclusive, "the RES2 zoom step");
    if (!ok) {
        zlog("; [zoom] DISARMED: install_jmp refused (see the [interlock] line in mh_net.log)");
        return 0;
    }
    g_armed = true;
    zlog("; [zoom] armed -- wheel=%d filter=%s (system backend) player zoom 1x..2*H/768, anchored at the cursor; "
         "apply 0x%08x translate 0x%08x step 0x%08x",
         g_wheel ? 1 : 0, mh::gfx::owned_filter_name(g_ini_filter), (unsigned)ADDR_APPLY, (unsigned)ADDR_TRANSLATE,
         (unsigned)ADDR_STEP);
    return 1;
}
