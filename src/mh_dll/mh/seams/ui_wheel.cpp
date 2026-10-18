//
// seams/ui_wheel.cpp -- D15: the mouse wheel scrolls the game's lists.
//
// Mechanism, the three drivers and the direction rule: include/mh_uiwheel_export.h. The arithmetic (notches with
// the gap rule, rows per notch, the clamps) is gfx/wheel_list.{h,cpp}; this file is the glue to the game's
// memory (the list widget layout and the HUD list descriptors are described at their readers below).
//
// THE WHEEL IS READ, NOT HOOKED -- like the map zoom (ui_map_zoom.cpp): both of the game's mouse producers add
// the wheel to one cumulative int, _G_LLM_INPUT_MOUSE_WHEEL_TOTAL (0x00e69da0), and every driver here takes the
// difference since its own last call. Backend-neutral; the ImGui overlay withholds the wheel from the total
// while it owns it. Wheel movement nothing can use (the pointer over no list, a screen with no list, a gap
// between two calls) is DROPPED, never queued.
//
// UI ONLY. Every write is a view field of the retail UI: a menu list's top row (param_block[7]), the info
// screen's page index, a HUD list descriptor's top row. The selection is never touched. The sim, a save and the
// lockstep hash never read any of them.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "include/mh_uiwheel_export.h"
#include "include/mh_run_context.h" // mh_run_path
#include "include/mh_config_dir.h"  // RL3: <config dir>mh_net.ini
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "addr/mh_calls.gen.h"      // game_SetEvent
#include "gfx/wheel_list.h"
#include "hook/detour.h"
#include "seams/strat_pre_input.h"
#include "en_guard.h"

#pragma comment(lib, "user32.lib") // wvsprintfA

namespace {

namespace wl = mh::gfx::wheel_list;

// ---- game addresses (EN; each re-read from the disassembly / decompile 2026-10-13) --------------------------------
constexpr uintptr_t ADDR_WHEEL        = 0x00e69da0u; // _G_LLM_INPUT_MOUSE_WHEEL_TOTAL: cumulative, 120 per notch, forward = +
constexpr uintptr_t ADDR_INPUT_TICK   = 0x004b73f9u; // llm_ui_widget_input_tick   int(void) __watcall, once per frame from llm_ui_menu_state_tick
constexpr uintptr_t ADDR_ICON_LIST    = 0x00415c69u; // llm_ui_scroll_icon_list_draw void(EAX = desc, EDX = icon_entry_base) __watcall
constexpr uintptr_t ADDR_LIST_DRAW    = 0x004c18b0u; // llm_ui_list_widget_draw: a widget with this draw_cb is a scrollable list
constexpr uintptr_t ADDR_ACTIVE_DLG   = mh::addr::_G_LLM_UI_ACTIVE_DIALOG;
constexpr uintptr_t ADDR_MENU_LIST    = mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
constexpr uintptr_t ADDR_HOVERED      = mh::addr::_G_LLM_UI_WIDGET_HOVERED;
constexpr uintptr_t ADDR_SB_THUMB_PTR = 0x0064506fu; // llm_ui_scrollbar_bind_range: &pb[7] of the list the shared scrollbar drives
constexpr uintptr_t ADDR_SB_BAR       = 0x0064fffbu; // the shared list scrollbar widget
constexpr uintptr_t ADDR_SB_UP        = 0x0064ffb7u; // ...its up arrow
constexpr uintptr_t ADDR_SB_DOWN      = 0x0065003fu; // ...its down arrow
constexpr uintptr_t ADDR_INFO_INDEX   = 0x00654482u; // _G_LLM_UI_INFO_DESC_INDEX: the info screen's page
constexpr uintptr_t ADDR_INFO_COUNT   = 0x0065451eu; // _G_LLM_UI_INFO_DESC_COUNT
constexpr uintptr_t ADDR_MODE         = mh::addr::_G_LLM_GAME_MODE;
constexpr uintptr_t ADDR_WIN_W        = mh::addr::G_WIN_W; // the strategic viewport width; the side panel starts here
constexpr uintptr_t ADDR_WIN_H        = mh::addr::WindowHeight;
constexpr uintptr_t ADDR_CUR_MENU_X   = 0x00e58cccu; // _G_LLM_CURSOR_MENU_X: the untranslated cursor
constexpr uintptr_t ADDR_CUR_MENU_Y   = 0x00e58cc8u;
constexpr uintptr_t ADDR_HUD_WIDGETS  = 0x0050be63u; // _G_LLM_UI_HUD_WIDGETS[332], 28 B each; x +8, y +12, right +16, bottom +20
constexpr int       HUD_WIDGET_N      = 332;

// widget (llm_ui_widget) offsets
constexpr int      W_FLAGS = 0x08, W_DRAW_CB = 0x0c, W_PARAM = 0x30;
constexpr uint32_t W_HIDDEN = 0x80;
// list param_block (int[]): [3] visible rows, [6] entry count, [7] top row, [8] selected row
constexpr int PB_VISIBLE = 3, PB_COUNT = 6, PB_TOP = 7;
// llm_ui_scroll_list_descriptor (packed): +0 row_height, +0x15 scrollbar_up, +0x21 top_y,
// +0x25 subrow_scroll_px, +0x29 scroll_row_offset, +0x2d item_count; llm_ui_scroll_arrow_desc +0xc widget_id
constexpr int D_ROW_H = 0x00, D_UP = 0x15, D_TOP_Y = 0x21, D_SUBROW = 0x25, D_OFFSET = 0x29, D_COUNT = 0x2d;
constexpr int A_WIDGET_ID = 0x0c;

constexpr uintptr_t ADDR_PANEL_MODE          = mh::addr::_G_LLM_STRAT_UI_PANEL_MODE; // 0 map, 1 build/tech panel, 2 selection/info panel
constexpr uint32_t  EV_PANEL_REFRESH_CURRENT = 24;                                   // game_SetEvent: mark the CURRENT panel dirty (no tab switch)
constexpr int       HUD_PENDING_FRAMES       = 3;                                    // pump calls a read notch waits for its panel's redraw

constexpr uint8_t MODE_STRAT = 2;
constexpr uint8_t MODE_INFO  = 4;

template <class T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void *)a, sizeof(T));
    return v;
}
template <class T>
void wr(uintptr_t a, T v) {
    memcpy((void *)a, &v, sizeof(T));
}

// ---- state ------------------------------------------------------------------------------------------------------
bool           g_menu_armed = false; // drivers A + B
bool           g_hud_armed  = false; // driver C
void          *g_tramp_menu = nullptr;
void          *g_tramp_hud  = nullptr;
wl::feed       g_menu_feed;                // drivers A + B (called from the menu input tick)
wl::feed       g_hud_feed;                 // driver C (called from the strategic input pump)
int            g_hud_pending = 0;          // notches the strategic pump read, not yet taken by a list
int            g_hud_age     = 0;          // pump calls since they were read
int            g_hud_px = 0, g_hud_py = 0; // the pointer (panel x, screen y) when they were read -- see hud_draw_impl
long           g_logs = 0;
char           g_log[MAX_PATH];
unsigned long  g_log_gen = 0; // SES1: per-SESSION (shares mh_input.log with ui_map_zoom / ui_keyrepeat / ui_pause)
constexpr long LOG_MAX   = 400;

void wlog(const char *fmt, ...) {
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

// ---- drivers A + B: the menu input tick -----------------------------------------------------------------------------

// A scrollable list widget that is shown. The 0x40 DISABLED flag is NOT a filter: it only means "not clickable" -- the
// lobby's text log (0x00650083, flags 0x00960052) is disabled by design and is exactly the list the wheel has to scroll.
bool is_list_widget(uintptr_t w) {
    return w && rd<uintptr_t>(w + W_DRAW_CB) == ADDR_LIST_DRAW && rd<uintptr_t>(w + W_PARAM) != 0 &&
           (rd<uint32_t>(w + W_FLAGS) & W_HIDDEN) == 0;
}

// The widget list the input tick will use this frame (the dialog wins, like the tick itself).
uintptr_t active_list() {
    const uintptr_t d = rd<uintptr_t>(ADDR_ACTIVE_DLG);
    return d ? d : rd<uintptr_t>(ADDR_MENU_LIST);
}

// Is `w` one of `list`'s children? (children: NULL-terminated widget* array at list+0)
bool in_list(uintptr_t list, uintptr_t w) {
    if (!list || !w) return false;
    const uintptr_t ch = rd<uintptr_t>(list);
    if (!ch) return false;
    for (int i = 0; i < 64; ++i) {
        const uintptr_t c = rd<uintptr_t>(ch + 4 * i);
        if (!c) return false;
        if (c == w) return true;
    }
    return false;
}

// The list widget the wheel drives, or 0: the list under the pointer; the list the shared scrollbar (or its
// arrows) under the pointer drives; else the screen's only list.
uintptr_t pick_target(uintptr_t list) {
    const uintptr_t ch = rd<uintptr_t>(list);
    if (!ch) return 0;
    const uintptr_t hov = rd<uintptr_t>(ADDR_HOVERED);
    if (is_list_widget(hov) && in_list(list, hov)) return hov;
    uintptr_t only  = 0;
    int       lists = 0;
    for (int i = 0; i < 64; ++i) {
        const uintptr_t c = rd<uintptr_t>(ch + 4 * i);
        if (!c) break;
        if (is_list_widget(c)) {
            only = c;
            ++lists;
        }
    }
    if (hov == ADDR_SB_BAR || hov == ADDR_SB_UP || hov == ADDR_SB_DOWN) {
        const uintptr_t thumb = rd<uintptr_t>(ADDR_SB_THUMB_PTR); // &pb[7]
        if (thumb) {
            for (int i = 0; i < 64; ++i) {
                const uintptr_t c = rd<uintptr_t>(ch + 4 * i);
                if (!c) break;
                if (is_list_widget(c) && rd<uintptr_t>(c + W_PARAM) + 4 * PB_TOP == thumb) return c;
            }
        }
    }
    return lists == 1 ? only : 0;
}

void scroll_menu_list(uintptr_t w, int n) {
    const uintptr_t pb  = rd<uintptr_t>(w + W_PARAM);
    const int       cnt = rd<int32_t>(pb + 4 * PB_COUNT), vis = rd<int32_t>(pb + 4 * PB_VISIBLE), top = rd<int32_t>(pb + 4 * PB_TOP);
    if (vis < 1 || cnt <= vis) return; // nothing to scroll
    const int nt = wl::scroll_top(top, n, wl::MENU_ROWS_PER_NOTCH, cnt, vis);
    if (nt == top) return;
    wr<int32_t>(pb + 4 * PB_TOP, nt);
    if (g_logs++ < LOG_MAX) wlog("; [wheel] list 0x%08x top %d -> %d (n=%d count=%d rows=%d)", (unsigned)w, top, nt, n, cnt, vis);
}

void __cdecl menu_tick_impl() {
    int       dropped = 0;
    const int n       = wl::feed_notches(g_menu_feed, rd<int32_t>(ADDR_WHEEL), GetTickCount(), &dropped);
    if (dropped && g_logs++ < LOG_MAX) wlog("; [wheel] menu: wheel total moved %d while the tick was away: dropped", dropped);
    if (!n) return;
    if (rd<uint8_t>(ADDR_MODE) == MODE_INFO) { // driver B: the unit/building info screen pages
        const int idx = rd<int32_t>(ADDR_INFO_INDEX), cnt = rd<int32_t>(ADDR_INFO_COUNT);
        const int ni = wl::page_after(idx, n, cnt);
        if (ni != idx) {
            wr<int32_t>(ADDR_INFO_INDEX, ni);
            if (g_logs++ < LOG_MAX) wlog("; [wheel] info page %d -> %d (n=%d count=%d)", idx, ni, n, cnt);
        }
        return;
    }
    const uintptr_t list = active_list();
    if (!list) return;
    const uintptr_t t = pick_target(list);
    if (t) scroll_menu_list(t, n);
}

// clang-format off
// A run-before detour: every register preserved, then the original runs from the stolen-prologue thunk.
__declspec(naked) void menu_tick_thunk() {
    __asm {
        pushad
        call menu_tick_impl
        popad
        jmp  dword ptr [g_tramp_menu]
    }
}
// clang-format on

// ---- driver C: the strategic side-panel lists -----------------------------------------------------------------------

// THE LISTS ARE REDRAWN ONLY WHEN THEIR PANEL IS DIRTY (the tab dispatchers run the draw behind a REFRESH_PENDING flag), so
// "the draw ran this frame" cannot be waited for: the pump asks for it. A notch read while the pointer is on the side panel
// marks the current panel dirty (game_SetEvent PANEL_REFRESH_CURRENT -- no tab switch), the panel's dispatcher then runs
// llm_ui_scroll_icon_list_draw for the lists it shows, and the wrapper below applies the notch to the one under the pointer.
// A notch nobody takes within HUD_PENDING_FRAMES pump calls is dropped, never queued.
void on_strat_pump() {
    int       dropped = 0;
    const int n       = wl::feed_notches(g_hud_feed, rd<int32_t>(ADDR_WHEEL), GetTickCount(), &dropped);
    if (g_hud_pending && ++g_hud_age >= HUD_PENDING_FRAMES) g_hud_pending = 0;
    if (!n) return;
    g_hud_pending += n;
    g_hud_age = 0;
    // The pointer is taken HERE, with the wheel: the list draw runs later (when the panel redraws), and by then the game's
    // cursor globals may have moved on (a rig's synthetic cursor is back at the DirectInput position after a raw wheel packet).
    const int mode = rd<int32_t>(ADDR_PANEL_MODE);
    const int px   = rd<int32_t>(ADDR_CUR_MENU_X) - rd<int32_t>(ADDR_WIN_W);
    g_hud_px       = px;
    g_hud_py       = rd<int32_t>(ADDR_CUR_MENU_Y);
    if (rd<uint8_t>(ADDR_MODE) == MODE_STRAT && (mode == 1 || mode == 2) && px >= 0) mh::call::game_SetEvent(EV_PANEL_REFRESH_CURRENT);
}

// One call per list the dirty panel redraws (OWNED / AVAILABLE and the units / storage / projects lists share rectangles,
// only the open one is drawn, so "this draw ran" is the visibility test a static rectangle test cannot give). The list takes
// the pending notches only when the pointer is inside its body rectangle, which also tells the selection column (x 0..0x50)
// from the control-group column (x 0x50..0xa0).
void __cdecl hud_draw_impl(uintptr_t desc, int icon_entry_base) {
    (void)icon_entry_base;
    const int n = g_hud_pending;
    if (!n || !desc) return;
    if (rd<uint8_t>(ADDR_MODE) != MODE_STRAT) return;
    const uintptr_t up = rd<uintptr_t>(desc + D_UP);
    if (!up) return;
    const int body = rd<int32_t>(up + A_WIDGET_ID) - 1; // the arrow ids follow the body's (llm_ui_hud_scroll_list_define)
    if (body < 0 || body >= HUD_WIDGET_N) return;
    const uintptr_t wd = ADDR_HUD_WIDGETS + (uintptr_t)body * 28;
    const int       wx = rd<int32_t>(wd + 8), wright = rd<int32_t>(wd + 16);
    const int       top_y = rd<int32_t>(desc + D_TOP_Y), bottom = rd<int32_t>(ADDR_WIN_H);
    const int       px = g_hud_px, py = g_hud_py;
    if (!wl::in_rect(px, py, wx, top_y, wright, bottom)) return;
    g_hud_pending   = 0; // taken: the first list under the pointer consumes the notches
    const int row_h = rd<int32_t>(desc + D_ROW_H), cnt = rd<int32_t>(desc + D_COUNT), top = rd<int32_t>(desc + D_OFFSET);
    // The draw clamps the offset to [0, count - visible] with its own (P6-capped) visible count right after this
    // returns, so only the lower bound and the list length are ours.
    const int nt = wl::scroll_top(top, n, wl::hud_rows_per_notch(row_h), cnt, 1);
    wr<int32_t>(desc + D_SUBROW, 0);
    if (nt == top) return;
    wr<int32_t>(desc + D_OFFSET, nt);
    if (g_logs++ < LOG_MAX)
        wlog("; [wheel] hud list 0x%08x top %d -> %d (n=%d rows/notch=%d count=%d row_h=%d body=%d cursor=%d,%d)", (unsigned)desc, top, nt,
             n, wl::hud_rows_per_notch(row_h), cnt, row_h, body, px, py);
}

// clang-format off
// __watcall: EAX = desc, EDX = icon_entry_base. The __cdecl body gets them on the stack; every register is
// restored for the original (the callers keep values in EBX/ECX/ESI/EDI across the call).
__declspec(naked) void hud_draw_thunk() {
    __asm {
        pushad
        push edx
        push eax
        call hud_draw_impl
        add  esp, 8
        popad
        jmp  dword ptr [g_tramp_hud]
    }
}
// clang-format on

} // namespace

extern "C" void MH_UiWheel_Reseed(void) {
    g_menu_feed   = wl::feed();
    g_hud_feed    = wl::feed();
    g_hud_pending = 0;
}

extern "C" int MH_UiWheel_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only
    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini);
    if (!mh_ini_get_int("input", "wheel_lists", 1, ini)) {
        wlog("; [wheel] off ([input] wheel_lists=0)");
        return 0;
    }
    using mh::hook::entry_claim;
    int armed = 0;
    // Each driver is independent: a refused entry (a promoted body, another detour) costs only that driver, and
    // the refusal is on the [interlock] line in mh_net.log.
    if (mh::hook::install_trampoline(ADDR_INPUT_TICK, (void *)menu_tick_thunk, &g_tramp_menu, 8, entry_claim::exclusive,
                                     "the D15 menu-list wheel")) {
        g_menu_armed = true;
        ++armed;
    } else {
        wlog("; [wheel] menu lists + info pages NOT armed: llm_ui_widget_input_tick was not takeable (see [interlock] in mh_net.log)");
    }
    if (mh::seams::strat_pre_input_add(on_strat_pump, "the D15 side-panel wheel pump")) {
        if (mh::hook::install_trampoline(ADDR_ICON_LIST, (void *)hud_draw_thunk, &g_tramp_hud, 8, entry_claim::exclusive,
                                         "the D15 side-panel list wheel")) {
            g_hud_armed = true;
            ++armed;
        } else {
            wlog("; [wheel] side-panel lists NOT armed: llm_ui_scroll_icon_list_draw was not takeable (see [interlock] in mh_net.log)");
        }
    } else {
        wlog("; [wheel] side-panel lists NOT armed: the strategic pre-input hook was refused (see [interlock] in mh_net.log)");
    }
    wlog("; [wheel] armed: menu lists + info pages=%d side-panel lists=%d (3 rows / notch in menus, ~48 px / notch in the panel)",
         g_menu_armed ? 1 : 0, g_hud_armed ? 1 : 0);
    return armed;
}
