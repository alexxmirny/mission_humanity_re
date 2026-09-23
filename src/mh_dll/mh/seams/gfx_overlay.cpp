//
// gfx_overlay.cpp -- in-game debug overlay, P0.
//
// Paints live state onto the composed frame from the shared present hook, using our own RGB565 text
// blitter over an embedded 6x10 ASCII font (overlay_font.h). Route B of the task's rendering fork:
// DLL-native drawing works in EVERY mode -- boot, menu, lobby, strategic, tactical -- and keeps
// working while the sim is paused or stalled, which is exactly when we most want to read state. It
// also touches no game render state, so there is nothing to perturb and nothing to un-perturb.
//
// The layout is DATA. `[debug]` in mh_net.ini declares pages as ordered lists of *provider names*;
// the DLL ships a small registry of named providers. Adding a readout later means registering one
// provider, not editing a hardcoded panel -- that is what keeps P1/P2 additive.
//
//   [debug]
//   overlay=1                       ; 0 = installed but hidden (toggle key still works)
//   modes=2,6                       ; global gate on _G_LLM_GAME_MODE (empty = every mode)
//   rect=8,8                        ; x,y[,w,h] relative to `anchor`
//   anchor=tl                       ; tl|tr|bl|br
//   color=ffff80
//   box=1                           ; darken the backing rect for legibility
//   pages=dbg,net
//   page.dbg.title=DBG
//   page.dbg.modes=2                ; per-page gate (empty = inherit the global one)
//   page.dbg.items=mode,res,fps,clock
//   toggle_key=Ctrl+Alt+D           ; rebindable; Ctrl+Alt defaults avoid the game's command keys
//   next_page_key=Ctrl+Alt+PgDn
//   prev_page_key=Ctrl+Alt+PgUp
//
// Ship-safe: no [debug] section -> nothing installs and OnPresent returns on its first branch.
// Determinism: reads globals, writes pixels. Never calls sim code, never consumes RNG.
//
// Install order matters (README rule 4): this runs BEFORE MH_Capture_OnPresent so the UI-capture
// harness sees the overlay. Like the capture/ui-drive seams it installs no hook of its own (it
// piggybacks the lockstep present hook) and therefore has no arm/disarm state to report -- it logs
// to its own mh_overlay.log rather than adding a line to the diffed arm-log sequence.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "include/mh_overlay_export.h"
#include "include/mh_net_export.h"     // MH_Net_LocalPlayerId (sim.power's per-player index)
#include "include/mh_uidrive_export.h" // MH_UIDrive_SynthKeyDown (the `hotkey` verb's chord, 2026-09-20)
#include "include/mh_run_context.h"    // MH_RunDir
#include "addr/mh_addrs.gen.h"         // generated EN VAs
#include "config/ini_read.h"           // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "state/region_runtime.h"      // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                       // where it IS, not where the binary put it
#include "include/mh_tile_dirty.h"     // mp:GX1: mark_ground_tiles_dirty -- shared with ui_net_indicator.cpp
#include "en_guard.h"                  // EN-only build gate
#include "overlay_font.h"              // GENERATED 6x10 ASCII cells

#pragma comment(lib, "user32.lib") // wsprintfA / GetAsyncKeyState

namespace {

using mh::overlay::FONT;
using mh::overlay::FONT_FIRST;
using mh::overlay::FONT_H;
using mh::overlay::FONT_LAST;
using mh::overlay::FONT_W;

constexpr uintptr_t ADDR_FB      = mh::addr::_G_LLM_FRAMEBUFFER; // ptr to the locked RGB565 bits
constexpr uintptr_t ADDR_PITCH   = mh::addr::_G_LLM_FB_PITCH;    // bytes/row
constexpr uintptr_t ADDR_W       = mh::addr::WindowWidth;        // re-read per frame (resolution can change)
constexpr uintptr_t ADDR_H       = mh::addr::WindowHeight;
constexpr uintptr_t ADDR_MODE    = mh::addr::_G_LLM_GAME_MODE;         // byte: 2=strat 3=menu 6=tactical ...
constexpr uintptr_t ADDR_SESSION = mh::addr::_G_LLM_GAME_SESSION_MODE; // byte: 3=lockstep
constexpr uintptr_t ADDR_CLOCK   = mh::addr::_G_LLM_STRAT_GAME_CLOCK;  // double, game-SECONDS (cf. ms_of)

// sim.* family. Unlike net.*, these need no other seam's knowledge -- they are plain global reads of
// the same character as mode/session/clock above, so they stay built-in rather than going through the
// registration API (which exists for families that need a TU's own state, e.g. the transport stats).
constexpr uintptr_t ADDR_RNG   = mh::addr::_G_LLM_STRAT_RNG_STATE;   // strategic PRNG words
constexpr uintptr_t ADDR_POWER = mh::addr::_G_LLM_STRAT_POWER_STATS; // llm_strat_power_stats[8]
// SB-HOSTFREE: a FUNCTION, not a `constexpr uintptr_t`. This region is MOVABLE -- a
// relocating host puts it in its own arena and fills the .bss it left with 0xCD -- so a
// constant baked at compile time reads poison. tools/check_movable_addresses.py is the gate.
inline uintptr_t ADDR_ORD_QUEUE() { // int
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
}
inline uintptr_t ADDR_ORD_STAGING() { // int
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_STAGING_COUNT);
}
inline uintptr_t ADDR_ORD_PENDING() { // int
    return mh::state::live_base(mh::state::RID_STRAT_ORDER_PENDING_COUNT);
}
constexpr uintptr_t ADDR_CURSOR_X      = mh::addr::_G_LLM_CURSOR_X;
constexpr uintptr_t ADDR_CURSOR_Y      = mh::addr::_G_LLM_CURSOR_Y;
constexpr int       POWER_STATS_STRIDE = 0x18; // llm_strat_power_stats (docs/structs.md)

constexpr int MAX_PAGES = 8;
constexpr int MAX_ITEMS = 20;
constexpr int MAX_NAME  = 40;
constexpr int MAX_LINE  = 72;
constexpr int LABEL_MAX = 14; // cap on the per-page label column, in characters
constexpr int PAD       = 2;  // px of breathing room inside the backing box

// RGB565 "halve every channel" mask -- the game's own dimming trick (_G_LLM_RGB565_HALVE_MASK).
constexpr uint16_t HALVE_MASK = 0x7bef;

struct KeyBind {
    int  vk    = 0; // 0 = unbound
    bool ctrl  = false;
    bool alt   = false;
    bool shift = false;
    bool prev  = false; // rising-edge latch
};

struct Page {
    char     title[MAX_NAME]            = {0};
    unsigned mode_mask                  = 0; // bit N = visible in game mode N; 0 = inherit the global gate
    char     items[MAX_ITEMS][MAX_NAME] = {{0}};
    int      nitems                     = 0;
};

bool     g_installed = false;                    // a [debug] section existed
bool     g_visible   = false;                    // [debug] overlay=, flipped by the toggle key
unsigned g_modes     = 0;                        // global mode gate (0 = every mode)
int      g_rx = 8, g_ry = 8, g_rw = 0, g_rh = 0; // rect=x,y[,w,h]; w/h 0 = size to the text
int      g_anchor = 0;                           // 0=tl 1=tr 2=bl 3=br
uint16_t g_color  = 0xffff;                      // RGB565 text colour
bool     g_box    = true;                        // darken behind the text

Page g_pages[MAX_PAGES];
int  g_npages = 0;
int  g_page   = 0;

KeyBind g_key_toggle, g_key_next, g_key_prev;

// ---- mp:GX1: the ground-tile damage-map stamp -----------------------------------------------------
//
// This overlay writes RGB565 straight into the framebuffer (draw_text/dim_rect below) and stamps
// nothing on its own -- unlike every retail primitive, which marks the ground layer's damage map
// (_G_LLM_TILE_VIS_MAP) as it draws so the next ground pass repaints the tile. Without that mark,
// whatever we last put in the box's rect sticks forever once nothing else has a reason to touch that
// tile again. g_prev_r* is the LAST rect actually painted (box included), tracked so a frame whose
// box is smaller/moved/gone can still release what an EARLIER frame covered -- the vacated strip is
// not part of THIS frame's own rect and would otherwise never get marked at all.
bool g_prev_painted = false;
int  g_prev_rx = 0, g_prev_ry = 0, g_prev_rw = 0, g_prev_rh = 0;

// fps is measured over presents, not sim steps -- it is what the player sees.
long  g_frames     = 0;
int   g_fps_tenths = 0;
DWORD g_fps_mark   = 0;
long  g_fps_base   = 0;

// P1 perf family. Frame times come from QPC, not GetTickCount: the interesting frames are the ~2 s
// overlay hitches and the sub-frame jitter, and a 10-15 ms tick quantum cannot see either (it is the
// same quantum that aliased the lockstep step target). Stored in
// TENTHS of a millisecond so the whole family stays integer -- wsprintfA has no float support.
constexpr int  PERF_WINDOW              = 120; // rolling window, ~2 s at 60 fps
constexpr long HITCH_TENTHS             = 500; // >50 ms = a visible stutter
LARGE_INTEGER  g_qpc_freq               = {0};
LARGE_INTEGER  g_qpc_prev               = {0};
int            g_perf_ring[PERF_WINDOW] = {0};
int            g_perf_n                 = 0; // samples held (<= PERF_WINDOW)
int            g_perf_head              = 0;
int            g_perf_last              = 0; // last frame, tenths of a ms
long           g_perf_hitches           = 0; // frames over HITCH_TENTHS since install

// Registered providers (P1): other seams own their own readouts -- the net/lockstep family lives in
// net_lockstep.cpp, which has the addresses and the transport stats. Keeping them there means this TU
// stays free of game/net knowledge and a new family is a registration, not an edit here.
constexpr int MAX_EXTRA = 32;
struct Extra {
    char                 name[MAX_NAME];
    MH_OverlayProviderFn fn;
};
Extra g_extra[MAX_EXTRA];
int   g_nextra = 0;

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0; // SES1: PROCESS-scoped -- arm-time banners, written before any session

void ovl_log(const char *fmt, ...) {
    mh_proc_path(g_log, MAX_PATH, "%smh_overlay.log", &g_log_gen);
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, line, lstrlenA(line), &w, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------------------------
// Value providers. Each writes just the VALUE; the page renderer supplies the label column.
// Keep them read-only and allocation-free: they run inside the present hook, every frame.
// NOTE for test authors: `fps`/`frames`/`clock` vary run-to-run, so a page used as a committed
// pixel baseline must stick to the stable ones (`mode`, `session`, `res`).
// ---------------------------------------------------------------------------------------------

typedef void (*ProviderFn)(char *buf, int cap);

void prov_mode(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d", (int)*(const uint8_t *)ADDR_MODE);
}

void prov_session(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d", (int)*(const uint8_t *)ADDR_SESSION);
}

void prov_res(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%dx%d", *(const int *)ADDR_W, *(const int *)ADDR_H);
}

void prov_fps(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d.%d", g_fps_tenths / 10, g_fps_tenths % 10);
}

void prov_frames(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", g_frames);
}

// ---- perf family (P1) ---------------------------------------------------------------------------
// Every value is tenths of a millisecond internally; `fmt_tenths` renders "12.3".

void fmt_tenths(char *b, int t) { wsprintfA(b, "%d.%d", t / 10, t % 10); }

void perf_stats(int *out_avg, int *out_min, int *out_max) {
    if (g_perf_n <= 0) {
        *out_avg = *out_min = *out_max = 0;
        return;
    }
    long lo = g_perf_ring[0], hi = g_perf_ring[0], sum = 0;
    for (int i = 0; i < g_perf_n; ++i) {
        long v = g_perf_ring[i];
        sum += v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *out_avg = (int)(sum / g_perf_n);
    *out_min = (int)lo;
    *out_max = (int)hi;
}

void prov_perf_ms(char *b, int cap) {
    (void)cap;
    fmt_tenths(b, g_perf_last);
}

void prov_perf_avg(char *b, int cap) {
    (void)cap;
    int a, lo, hi;
    perf_stats(&a, &lo, &hi);
    fmt_tenths(b, a);
}

// min/max over the window -- the pair that actually localises a stutter: a healthy 16.7 avg with a
// 2000.0 max is a hitch, not a slow frame rate.
void prov_perf_min(char *b, int cap) {
    (void)cap;
    int a, lo, hi;
    perf_stats(&a, &lo, &hi);
    fmt_tenths(b, lo);
}

void prov_perf_max(char *b, int cap) {
    (void)cap;
    int a, lo, hi;
    perf_stats(&a, &lo, &hi);
    fmt_tenths(b, hi);
}

void prov_perf_hitches(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", g_perf_hitches);
}

// Game clock, rendered as seconds.milliseconds. The global is a double in game-SECONDS (the same
// convention as every other game timing double here -- net_internal.h's ms_of scales by 1000), NOT
// milliseconds; reading it as ms made this render ~1000x low. wsprintfA has no float support, so
// convert to integer ms and split rather than pulling in the CRT's formatter.
void prov_clock(char *b, int cap) {
    (void)cap;
    double c;
    memcpy(&c, (const void *)ADDR_CLOCK, sizeof(c));
    long ms = (long)(c * 1000.0);
    if (ms < 0) ms = 0;
    wsprintfA(b, "%ld.%03ld", ms / 1000, ms % 1000);
}

// ---- sim family (P1) ----------------------------------------------------------------------------

// The strategic PRNG words.
//
// DO NOT read a cross-peer difference here as a desync: this channel is PROVEN not to feed game state
// (2026-07-09) and the determinism harness deliberately EXCLUDES rng_state from its state-only verdict
// for exactly that reason. Observed live 2026-07-25: two peers showed different second words during a
// run that was determinism-clean over all 800 steps. It is worth watching as a cheap "is the sim
// ticking at all" pulse, and as the first thing to check when a REAL desync is already established --
// not as evidence of one.
void prov_sim_rng(char *b, int cap) {
    (void)cap;
    const uint32_t *r = (const uint32_t *)ADDR_RNG;
    wsprintfA(b, "%08x %08x", r[0], r[1]);
}

// Power for the local player: last completed step's generated/consumed. NOT the same concept as
// "energy" -- energy is the HP-like stat; POWER is the generated resource
// (docs/conventions.md#energy-is-not-power).
//
// Reads prev_generated/prev_consumed (+0x08/+0x0c), NOT generated/consumed (+0x00/+0x04): the live pair
// is an accumulator "zeroed per step" and refilled during the step, so sampling it from the present hook
// catches a partial (usually 0) total. The prev_* pair is latched at step start and is therefore the
// only stable thing to show once per frame. (docs/structs.md llm_strat_power_stats.)
//
// Indexed by the local NET player id -- player_id == side_id by construction in this engine (the N1/N2
// lobby work). In single-player that is 0, the human player.
//
// UNVERIFIED NONZERO (2026-07-25): every observation so far is a bare ~5 s match where all four fields
// read a clean 0 for player 0 -- consistent with "no power plant built yet" (and unlike a wrong offset,
// which would show garbage), but not proof the pair is the right one. Confirm against a developed base
// (load a save) before trusting the number.
void prov_sim_power(char *b, int cap) {
    int slot = MH_Net_LocalPlayerId();
    if (slot < 0 || slot > 7) { // out of range -> say so rather than reading past the array
        lstrcpynA(b, "-", cap);
        return;
    }
    const int *ps = (const int *)(ADDR_POWER + (uintptr_t)slot * POWER_STATS_STRIDE);
    wsprintfA(b, "%d/%d", ps[2], ps[3]); // prev_generated / prev_consumed
}

// The order pipeline depths: queue (local), staging (being packed), pending (scheduled lockstep cmds).
// There is a benign one-step `order_pending` skew at match start -- this makes that
// visible live instead of only in a post-hoc log diff.
void prov_sim_orders(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "q%d s%d p%d", *(const int *)ADDR_ORD_QUEUE(), *(const int *)ADDR_ORD_STAGING(),
              *(const int *)ADDR_ORD_PENDING());
}

// Screen-space cursor. The task sketch wanted TILE coords under the cursor; that needs the map
// transform, so this is the honest subset until the tile mapping is wired.
void prov_sim_cursor(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d,%d", *(const int *)ADDR_CURSOR_X, *(const int *)ADDR_CURSOR_Y);
}

const struct {
    const char *name;
    ProviderFn  fn;
} PROVIDERS[] = {
    {"mode", prov_mode},       // _G_LLM_GAME_MODE  (the game-mode notes)
    {"session", prov_session}, // _G_LLM_GAME_SESSION_MODE (3 = lockstep)
    {"res", prov_res},         // framebuffer WxH
    {"fps", prov_fps},         // presents/sec, measured over a ~500 ms window
    {"frames", prov_frames},   // presents since install (liveness proof)
    {"clock", prov_clock},     // _G_LLM_STRAT_GAME_CLOCK, s.mmm
    // perf family: QPC present-to-present frame times, ms with one decimal
    {"perf.ms", prov_perf_ms},           // last frame
    {"perf.avg", prov_perf_avg},         // mean over the rolling window
    {"perf.min", prov_perf_min},         // best frame in the window
    {"perf.max", prov_perf_max},         // worst frame in the window -- the hitch detector
    {"perf.hitches", prov_perf_hitches}, // frames over 50 ms since install
    // sim family: strategic-side state (plain global reads)
    {"sim.rng", prov_sim_rng},       // strategic PRNG words (benign cross-peer drift -- see the provider)
    {"sim.power", prov_sim_power},   // local player's last-step power generated/consumed
    {"sim.orders", prov_sim_orders}, // order pipeline depths: queue/staging/pending
    {"sim.cursor", prov_sim_cursor}, // screen-space cursor px
};

ProviderFn find_provider(const char *name) {
    for (const auto &p : PROVIDERS)
        if (lstrcmpiA(p.name, name) == 0) return p.fn;
    for (int i = 0; i < g_nextra; ++i) // families registered by other seams (net.*)
        if (lstrcmpiA(g_extra[i].name, name) == 0) return g_extra[i].fn;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

// Darken an axis-aligned rect in place (halve every RGB565 channel), clipped to the surface.
void dim_rect(uint8_t *fb, int pitch, int W, int H, int x0, int y0, int w, int h) {
    if (x0 < 0) {
        w += x0;
        x0 = 0;
    }
    if (y0 < 0) {
        h += y0;
        y0 = 0;
    }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    for (int y = 0; y < h; ++y) {
        uint16_t *row = (uint16_t *)(fb + (size_t)(y0 + y) * pitch) + x0;
        for (int x = 0; x < w; ++x)
            row[x] = (uint16_t)((row[x] >> 1) & HALVE_MASK);
    }
}

// mp:GX1: a HARD (single-frame, not decaying) reset of an axis-aligned rect to black, clipped to the
// surface. This is what paint() runs, once, over whatever the LAST painted frame covered that this
// frame's box no longer does -- a page switch or a page whose items shrank the box. `dim_rect` above
// only HALVES, which needs several more painted frames to converge to nothing, and a mode with no
// ground pass at all (menu/pause/lobby -- this file's head comment) never gets those extra frames for
// free the way a ground-pass mode's own repaint would give them; nothing there re-invokes this seam's
// darken step for a rect that no longer intersects the current box. A solid reset is not the true
// game background (this seam keeps no second copy of the frame to restore from), but it is
// deterministic and immediate, which is what "repaint its own box's background" (mp:GX1's own choice
// of words for this alternative) needs to mean without a framebuffer copy this seam does not have.
void clear_rect(uint8_t *fb, int pitch, int W, int H, int x0, int y0, int w, int h) {
    if (x0 < 0) {
        w += x0;
        x0 = 0;
    }
    if (y0 < 0) {
        h += y0;
        y0 = 0;
    }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    for (int y = 0; y < h; ++y) {
        uint16_t *row = (uint16_t *)(fb + (size_t)(y0 + y) * pitch) + x0;
        for (int x = 0; x < w; ++x)
            row[x] = 0;
    }
}

// One glyph, per-pixel clipped. Chars outside the table render as blanks rather than garbage.
void draw_glyph(uint8_t *fb, int pitch, int W, int H, int px, int py, unsigned char ch, uint16_t col) {
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const uint8_t *g = FONT[ch - FONT_FIRST];
    for (int row = 0; row < FONT_H; ++row) {
        int y = py + row;
        if (y < 0 || y >= H) continue;
        uint16_t *dst  = (uint16_t *)(fb + (size_t)y * pitch);
        uint8_t   bits = g[row];
        if (!bits) continue;
        for (int cx = 0; cx < FONT_W; ++cx) {
            if (!(bits & (0x20 >> cx))) continue;
            int x = px + cx;
            if (x < 0 || x >= W) continue;
            dst[x] = col;
        }
    }
}

void draw_text(uint8_t *fb, int pitch, int W, int H, int px, int py, const char *s, uint16_t col) {
    for (int i = 0; s[i]; ++i)
        draw_glyph(fb, pitch, W, H, px + i * FONT_W, py, (unsigned char)s[i], col);
}

// ---------------------------------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------------------------------

// Split `s` in place on `sep`, trimming spaces. Returns the field count (empty fields dropped).
int split(char *s, char sep, char **out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') ++s;
        char *start = s;
        while (*s && *s != sep) ++s;
        char *end = s;
        if (*s) *s++ = 0;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (*start) out[n++] = start;
    }
    return n;
}

int parse_int(const char *s) {
    int v = 0, sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    }
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
    return v * sign;
}

unsigned parse_hex(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned v = 0;
    for (; *s; ++s) {
        unsigned d;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else break;
        v = v * 16 + d;
    }
    return v;
}

// "2,6" -> a bitmask over _G_LLM_GAME_MODE values. Empty -> 0 (= no gate). Modes above 31 are
// outside the mask and are ignored (the game only defines 1..8, the game-mode notes).
unsigned parse_modes(char *csv) {
    char    *f[16];
    int      n = split(csv, ',', f, 16);
    unsigned m = 0;
    for (int i = 0; i < n; ++i) {
        int v = parse_int(f[i]);
        if (v >= 0 && v < 32) m |= 1u << v;
    }
    return m;
}

uint16_t rgb565(unsigned rgb) {
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// "Ctrl+Alt+D" / "Alt+F9" / "VK_F10" / "0x79" / "D". Returns false if the key part is unusable.
bool parse_key(const char *spec, KeyBind *out) {
    char buf[64];
    lstrcpynA(buf, spec, sizeof(buf));
    char *f[6];
    int   n = split(buf, '+', f, 6);
    if (n <= 0) return false;
    *out = KeyBind{};
    for (int i = 0; i < n - 1; ++i) {
        if (lstrcmpiA(f[i], "ctrl") == 0 || lstrcmpiA(f[i], "control") == 0) out->ctrl = true;
        else if (lstrcmpiA(f[i], "alt") == 0) out->alt = true;
        else if (lstrcmpiA(f[i], "shift") == 0) out->shift = true;
        else return false;
    }
    const char *k = f[n - 1];
    if ((k[0] == 'V' || k[0] == 'v') && (k[1] == 'K' || k[1] == 'k') && k[2] == '_') k += 3; // accept VK_ names
    static const struct {
        const char *name;
        int         vk;
    } NAMED[] = {
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
        {"space", VK_SPACE},
        {"tab", VK_TAB},
    };
    for (const auto &e : NAMED)
        if (lstrcmpiA(e.name, k) == 0) {
            out->vk = e.vk;
            return true;
        }
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '0' && k[1] <= '9') {
        int f_n = parse_int(k + 1);
        if (f_n >= 1 && f_n <= 24) {
            out->vk = VK_F1 + f_n - 1;
            return true;
        }
    }
    if (k[0] == '0' && (k[1] == 'x' || k[1] == 'X')) {
        out->vk = (int)parse_hex(k);
        return out->vk != 0;
    }
    if (k[0] && !k[1]) { // single character: VK codes match ASCII for A-Z / 0-9
        char c = k[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out->vk = (unsigned char)c;
        return true;
    }
    return false;
}

// A key is HELD if the OS says so OR the UI harness is synthesising it (2026-09-20). The second arm
// exists because GetAsyncKeyState answers 0 for a process on a non-input desktop -- every headless
// lane -- so the toggle could never be exercised by the suite; the `hotkey` script verb holds a chord
// in the harness's own table and this is the one place the overlay consults it. Zero cost and zero
// effect outside a running script (the table is all-zero unless a script sets it).
bool vk_held(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0 || MH_UIDrive_SynthKeyDown(vk) != 0;
}

// Rising edge, with the modifier state required by the binding. Our GetAsyncKeyState path is
// independent of the game's DirectInput ring, so a collision DOUBLE-FIRES rather than overriding --
// which is exactly why these are rebindable and default to Ctrl+Alt.
bool key_fired(KeyBind *k) {
    if (!k->vk) return false;
    bool down = vk_held(k->vk);
    if (down) {
        if (k->ctrl && !vk_held(VK_CONTROL)) down = false;
        if (k->alt && !vk_held(VK_MENU)) down = false;
        if (k->shift && !vk_held(VK_SHIFT)) down = false;
    }
    bool fired = down && !k->prev;
    k->prev    = down;
    return fired;
}

void load_key(const char *ini, const char *key, const char *def, KeyBind *out, const char *what) {
    char spec[64];
    mh::config::read_ini_string("debug", key, def, spec, sizeof(spec), ini); // TL-HARN4
    if (!spec[0] || lstrcmpiA(spec, "none") == 0) return;                    // explicitly unbound
    if (!parse_key(spec, out)) ovl_log("; [debug] %s='%s' not understood -- %s left unbound", key, spec, what);
}

void load_pages(const char *ini) {
    char list[256];
    mh::config::read_ini_string("debug", "pages", "", list, sizeof(list), ini); // TL-HARN4
    char *names[MAX_PAGES];
    int   n = split(list, ',', names, MAX_PAGES);
    if (n == 0) {
        // No pages declared: ship a useful default so `[debug] overlay=1` alone shows something.
        Page &p = g_pages[0];
        lstrcpynA(p.title, "DBG", MAX_NAME);
        static const char *DEF[] = {"mode", "session", "res", "fps", "clock"};
        for (const char *d : DEF) lstrcpynA(p.items[p.nitems++], d, MAX_NAME);
        g_npages = 1;
        return;
    }
    for (int i = 0; i < n; ++i) {
        Page &p = g_pages[g_npages];
        char  name[MAX_NAME];
        lstrcpynA(name, names[i], MAX_NAME); // bound the name before it goes into a key buffer
        char key[MAX_NAME + 16];
        wsprintfA(key, "page.%s.title", name);
        mh::config::read_ini_string("debug", key, name, p.title, MAX_NAME, ini); // TL-HARN4
        wsprintfA(key, "page.%s.modes", name);
        char modes[64];
        mh::config::read_ini_string("debug", key, "", modes, sizeof(modes), ini); // TL-HARN4
        p.mode_mask = parse_modes(modes);
        wsprintfA(key, "page.%s.items", name);
        char items[512];
        mh::config::read_ini_string("debug", key, "", items, sizeof(items), ini); // TL-HARN4
        char *f[MAX_ITEMS];
        int   ni = split(items, ',', f, MAX_ITEMS);
        for (int j = 0; j < ni; ++j) lstrcpynA(p.items[p.nitems++], f[j], MAX_NAME);
        ++g_npages;
    }
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

bool mode_allowed(unsigned mask, int mode) { return mask == 0 || (mode >= 0 && mode < 32 && (mask & (1u << mode)) != 0); }

// Pick the next page (dir = +1/-1) that is allowed in the current mode; -1 if none is.
int next_visible_page(int from, int dir, int mode) {
    for (int i = 0; i < g_npages; ++i) {
        int idx = ((from + dir * (i + 1)) % g_npages + g_npages) % g_npages;
        if (mode_allowed(g_pages[idx].mode_mask ? g_pages[idx].mode_mask : g_modes, mode)) return idx;
    }
    return mode_allowed(g_pages[from].mode_mask ? g_pages[from].mode_mask : g_modes, mode) ? from : -1;
}

void paint() {
    uint8_t *fb    = *(uint8_t **)ADDR_FB;
    int      pitch = *(const int *)ADDR_PITCH;
    int      W     = *(const int *)ADDR_W;
    int      H     = *(const int *)ADDR_H;
    if (!fb || W <= 0 || H <= 0 || W > 4096 || H > 4096 || pitch < W * 2) return; // same validity gate as capture

    const Page &p = g_pages[g_page];

    // The value column is sized to THIS page's longest item name (capped), so a page of long names
    // like net.committed / perf.hitches reads properly instead of being truncated to a fixed width.
    int label_w = 0;
    for (int i = 0; i < p.nitems; ++i) {
        int len = lstrlenA(p.items[i]);
        if (len > label_w) label_w = len;
    }
    if (label_w > LABEL_MAX) label_w = LABEL_MAX;
    ++label_w; // one space before the value

    // Build the block: an optional title line, then "<label> <value>" per item.
    char lines[1 + MAX_ITEMS][MAX_LINE];
    int  nlines = 0, widest = 0;
    if (p.title[0]) lstrcpynA(lines[nlines++], p.title, MAX_LINE);
    for (int i = 0; i < p.nitems && nlines < 1 + MAX_ITEMS; ++i) {
        char       *out  = lines[nlines];
        const char *name = p.items[i];
        int         k    = 0;
        while (name[k] && k < label_w) {
            out[k] = name[k];
            ++k;
        }
        while (k < label_w) out[k++] = ' ';
        out[k] = 0;
        char value[MAX_LINE];
        value[0]      = 0;
        ProviderFn fn = find_provider(name);
        if (fn) fn(value, sizeof(value));
        else lstrcpynA(value, "?", sizeof(value)); // unknown provider: show it, don't hide it
        lstrcpynA(out + k, value, MAX_LINE - k);
        ++nlines;
    }
    for (int i = 0; i < nlines; ++i) {
        int len = lstrlenA(lines[i]);
        if (len > widest) widest = len;
    }
    if (!nlines) return;

    int bw = g_rw > 0 ? g_rw : widest * FONT_W + 2 * PAD;
    int bh = g_rh > 0 ? g_rh : nlines * FONT_H + 2 * PAD;
    int bx = (g_anchor == 1 || g_anchor == 3) ? W - g_rx - bw : g_rx;
    int by = (g_anchor == 2 || g_anchor == 3) ? H - g_ry - bh : g_ry;

    // mp:GX1. Two independent steps, in order, and neither may be dropped:
    //
    // (1) If the last PAINTED frame's rect differs from this one -- a page switch, or a page whose
    // item count/value width shrank bw/bh -- HARD-CLEAR that earlier rect to black, ONE frame, before
    // touching the current box at all. The vacated strip sits OUTSIDE this frame's own (bx,by,bw,bh),
    // so the existing per-current-rect dim_rect call below never reaches it; clear_rect's own comment
    // has the reasoning for why this is a solid reset rather than another halve. This step alone is
    // what keeps the overlay from smearing in a mode with no ground pass at all (menu/pause/lobby --
    // this file's head comment), on the very frame the shrink happens, not after several more frames
    // this seam is not guaranteed to get.
    if (g_prev_painted &&
        (g_prev_rx != bx || g_prev_ry != by || g_prev_rw != bw || g_prev_rh != bh))
        clear_rect(fb, pitch, W, H, g_prev_rx, g_prev_ry, g_prev_rw, g_prev_rh);

    // (2) Mark the ground layer's damage map over the UNION of the two rects (item 2's shared helper).
    // A no-op write in a mode with no ground pass (step 1 already handled those); in strategic mode 2
    // or tactical mode 6 it lets the NEXT ground-pass frame repaint real terrain over whatever step 1
    // just reset to black, upgrading a one-frame black patch into the correct picture -- better than
    // step 1 alone can do on its own, which is why both run rather than either replacing the other.
    {
        int ux0 = bx, uy0 = by, ux1 = bx + bw, uy1 = by + bh;
        if (g_prev_painted) {
            if (g_prev_rx < ux0) ux0 = g_prev_rx;
            if (g_prev_ry < uy0) uy0 = g_prev_ry;
            if (g_prev_rx + g_prev_rw > ux1) ux1 = g_prev_rx + g_prev_rw;
            if (g_prev_ry + g_prev_rh > uy1) uy1 = g_prev_ry + g_prev_rh;
        }
        mh::gfx::mark_ground_tiles_dirty(ux0, uy0, ux1 - ux0, uy1 - uy0);
    }

    // Unchanged from before this item: halve the CURRENT box for legibility (this is the
    // tested/baselined "halved terrain" look the committed debug_overlay UI-test captures show; step
    // 1 above never touches this rect, so that look does not move). `box=0` skips this, as before --
    // a page run with box=0 in a mode with no ground pass is not covered by this item.
    if (g_box) dim_rect(fb, pitch, W, H, bx, by, bw, bh);
    g_prev_rx = bx, g_prev_ry = by, g_prev_rw = bw, g_prev_rh = bh;
    g_prev_painted = true;

    for (int i = 0; i < nlines; ++i) {
        int ty = by + PAD + i * FONT_H;
        if (ty >= H) break;             // below the surface: nothing further can land
        if (ty + FONT_H <= 0) continue; // fully above it
        draw_text(fb, pitch, W, H, bx + PAD, ty, lines[i], g_color);
    }
}

} // namespace

// Register a value provider from another seam. Safe to call before or after MH_Overlay_Install (page
// items are resolved by name at paint time, so registration order does not matter) and harmless when
// no [debug] section exists. Not thread-safe by design: call it from install, on the main thread.
extern "C" void MH_Overlay_RegisterProvider(const char *name, MH_OverlayProviderFn fn) {
    if (!name || !fn || g_nextra >= MAX_EXTRA) return;
    lstrcpynA(g_extra[g_nextra].name, name, MAX_NAME);
    g_extra[g_nextra].fn = fn;
    ++g_nextra;
}

extern "C" void MH_Overlay_OnPresent(void) {
    if (!g_installed) return;

    // Frame time (QPC). Sampling stays outside the visibility check so perf.* still reflects real
    // frame pacing after the overlay is toggled on -- otherwise the first reading after a toggle is
    // one huge bogus delta spanning the hidden period.
    if (g_qpc_freq.QuadPart) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (g_qpc_prev.QuadPart) {
            long long d      = t.QuadPart - g_qpc_prev.QuadPart;
            int       tenths = (int)((d * 10000) / g_qpc_freq.QuadPart);
            if (tenths < 0) tenths = 0;
            g_perf_last              = tenths;
            g_perf_ring[g_perf_head] = tenths;
            g_perf_head              = (g_perf_head + 1) % PERF_WINDOW;
            if (g_perf_n < PERF_WINDOW) ++g_perf_n;
            if (tenths > HITCH_TENTHS) ++g_perf_hitches;
        }
        g_qpc_prev = t;
    }

    ++g_frames;
    DWORD now = GetTickCount();
    if (now - g_fps_mark >= 500) {
        if (g_fps_mark) {
            DWORD dt = now - g_fps_mark;
            if (dt) g_fps_tenths = (int)(((long long)(g_frames - g_fps_base) * 10000) / dt);
        }
        g_fps_mark = now;
        g_fps_base = g_frames;
    }

    if (key_fired(&g_key_toggle)) {
        g_visible = !g_visible;
        ovl_log("; overlay %s (toggle key)", g_visible ? "ON" : "OFF");
    }
    int mode = (int)*(const uint8_t *)ADDR_MODE;
    if (key_fired(&g_key_next)) g_page = next_visible_page(g_page, +1, mode);
    if (key_fired(&g_key_prev)) g_page = next_visible_page(g_page, -1, mode);
    if (g_page < 0) g_page = 0;

    // mp:GX1: the three gates below decide whether paint() runs at all this frame -- collapsed into
    // one `will_paint` (rather than three bare `return`s, as before) so the "nothing will draw"
    // outcome has exactly one exit, which is where the release-stamp belongs: whatever the LAST
    // painted frame covered needs a ground-pass repaint the moment this seam stops covering it,
    // whether that is because the player hid the overlay, no pages are configured, or the mode gate
    // (global or per-page) just closed.
    bool will_paint = g_visible && g_npages > 0;
    if (will_paint && !mode_allowed(g_modes, mode)) will_paint = false; // global mode gate
    if (will_paint) {
        const Page &p = g_pages[g_page];
        if (p.mode_mask && !mode_allowed(p.mode_mask, mode)) will_paint = false; // per-page gate
    }
    if (!will_paint) {
        if (g_prev_painted) {
            // Same pair as paint()'s step (1)+(2): a hard clear now (this is the LAST frame anything
            // here will touch these pixels, so there is no later frame to halve them away in), plus
            // the ground-pass stamp for whichever mode actually has one.
            uint8_t *fb    = *(uint8_t **)ADDR_FB;
            int      pitch = *(const int *)ADDR_PITCH;
            int      W     = *(const int *)ADDR_W;
            int      H     = *(const int *)ADDR_H;
            if (fb && W > 0 && H > 0 && W <= 4096 && H <= 4096 && pitch >= W * 2)
                clear_rect(fb, pitch, W, H, g_prev_rx, g_prev_ry, g_prev_rw, g_prev_rh);
            mh::gfx::mark_ground_tiles_dirty(g_prev_rx, g_prev_ry, g_prev_rw, g_prev_rh);
            g_prev_painted = false;
        }
        return;
    }
    paint();
}

extern "C" int MH_Overlay_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);

    char probe[8];
    if (GetPrivateProfileSectionA("debug", probe, sizeof(probe), ini) == 0) return 0; // no section -> no overlay
    g_installed = true;
    QueryPerformanceFrequency(&g_qpc_freq); // perf family; 0 freq simply disables perf.* sampling

    g_visible = GetPrivateProfileIntA("debug", "overlay", 0, ini) != 0;
    g_box     = GetPrivateProfileIntA("debug", "box", 1, ini) != 0;

    char buf[128];
    mh::config::read_ini_string("debug", "modes", "", buf, sizeof(buf), ini); // TL-HARN4
    g_modes = parse_modes(buf);

    mh::config::read_ini_string("debug", "rect", "8,8", buf, sizeof(buf), ini); // TL-HARN4
    char *f[4];
    int   nf = split(buf, ',', f, 4);
    if (nf > 0) g_rx = parse_int(f[0]);
    if (nf > 1) g_ry = parse_int(f[1]);
    if (nf > 2) g_rw = parse_int(f[2]);
    if (nf > 3) g_rh = parse_int(f[3]);

    mh::config::read_ini_string("debug", "anchor", "tl", buf, sizeof(buf), ini); // TL-HARN4
    if (lstrcmpiA(buf, "tr") == 0) g_anchor = 1;
    else if (lstrcmpiA(buf, "bl") == 0) g_anchor = 2;
    else if (lstrcmpiA(buf, "br") == 0) g_anchor = 3;
    else g_anchor = 0;

    mh::config::read_ini_string("debug", "color", "ffffff", buf, sizeof(buf), ini); // TL-HARN4
    g_color = rgb565(parse_hex(buf));

    load_pages(ini);
    load_key(ini, "toggle_key", "Ctrl+Alt+D", &g_key_toggle, "toggle");
    load_key(ini, "next_page_key", "Ctrl+Alt+PgDn", &g_key_next, "next-page");
    load_key(ini, "prev_page_key", "Ctrl+Alt+PgUp", &g_key_prev, "prev-page");

    ovl_log("; overlay installed visible=%d modes=0x%x rect=%d,%d,%d,%d anchor=%d color=0x%04x box=%d pages=%d",
            g_visible, g_modes, g_rx, g_ry, g_rw, g_rh, g_anchor, g_color, g_box, g_npages);
    for (int i = 0; i < g_npages; ++i)
        ovl_log(";   page[%d] '%s' modes=0x%x items=%d", i, g_pages[i].title, g_pages[i].mode_mask, g_pages[i].nitems);
    return 1;
}
