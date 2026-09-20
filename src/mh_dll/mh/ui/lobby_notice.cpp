//
// ---- U23: tell the player WHY they were returned to the browser --------------------------------
//
// All three lobby exits converge on llm_lobby_finalize_transfer_or_enter -- host-left (a real 0x0e),
// link-lost (R-live-ui's SYNTHESISED 0x0e) and the player's own Cancel -- which is exactly why the
// cause has to be carried as a variable rather than read off the transition. The net side separates
// the three (U13's ConsumeHostLeft / MH_MP_ConsumeExitCause) and hands the answer to
// browser_notice_arm as an int; this file knows nothing about where a cause comes from, which is
// the whole of the D4 boundary here.
//
// WHERE it renders: _G_LLM_LOBBY_MAP_STATUS_LINE, the wchar_t[256] that is the `label` of widget
// 0x006503b3. That widget is a child of BOTH MP browser containers AND of the lobby container
// (_G_LLM_UI_SCREEN_LOBBY_WIDGET_LIST / mh::addr::lobby_widget_origin -- read live off the lobby's
// children array at U42, correcting the "both browsers" phrasing above), so it is on screen whichever
// browser finalize pushes, or the manual lobby the player Created; its draw_cb is the generic
// llm_ui_widget_draw, which renders any non-NULL label through llm_gfx_draw_formatted_text. Nothing
// else on a menu screen can show text: the floating-message queue is read only by the in-match
// strategic renderer and the lobby announce line belongs to the screen we are leaving (both settled
// 2026-08-29 -- do not retry them).
//
// WHY a per-frame repaint and not a one-shot write: the buffer is not ours. It is NULLed by
// llm_lobby_clear_map_status_text from inside llm_mp_session_browser_enter, and re-cleared by every
// later rescan that auto-selects a row (llm_lobby_apply_selected_session_map). Any single write, at
// any point, races those. Rewriting while the notice is live wins by construction and costs one
// pointer compare per frame on a menu screen. Determinism-neutral: menu-screen text only, no sim
// state, no RNG.
//
// The strings are English literals, per the U18 / R-live-ui precedent. cfg G_TEXT_PTRS[0xa6]
// ("Disconnected") exists but means "a named PEER disconnected mid-match" and is proven only in a
// live match -- forcing it here would be a worse lie than an untranslated line. No wrapping is
// needed: llm_gfx_draw_formatted_text only auto-wraps when flags&0x18 == 0x18 (justified) and this
// widget's flags are 0x04868042, so the line runs until it ends -- keep both strings short.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/ui_internal.h"
#include "addr/mh_addrs.gen.h" // generated EN VAs (tools/gen_dll_addrs.py)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::ui::detail::ui_log;
using mh::ui::detail::ui_verbose;

namespace {

constexpr uintptr_t ADDR_MAP_STATUS_LINE = mh::addr::_G_LLM_LOBBY_MAP_STATUS_LINE;
constexpr uintptr_t ADDR_BROWSER_SESSION = mh::addr::_G_LLM_UI_WGT_LIST_MP_SESSION_BROWSER;
constexpr uintptr_t ADDR_BROWSER_LOCAL   = mh::addr::local_browser_widget_origin;
constexpr uintptr_t ADDR_LOBBY           = mh::addr::lobby_widget_origin; // U42: widget 0x6503b3 is ALSO a
                                                                          // child of the lobby container --
                                                                          // see no_module_notice_tick below.
constexpr DWORD NOTICE_DWELL_MS   = 10000;                                // how long it stays readable once the browser is up
constexpr DWORD NOTICE_ARM_MAX_MS = 30000;                                // give up if the exit never reaches a browser

const wchar_t *g_notice = nullptr;     // main thread only (finalize detour + present hook)
wchar_t        g_refused_line[256];    // F3c: "Join refused: <reason>", composed at arm time
DWORD          g_notice_armed = 0;     // GetTickCount at arm
DWORD          g_notice_until = 0;     // dwell deadline; meaningless until g_notice_shown
bool           g_notice_shown = false; // the browser has been on screen at least once

// ---- F3F / ruling Q2: the STANDING "no network module" line ------------------------------------
//
// The U23 notice is an EVENT ("the host left"), so it has a dwell and then hands the line back.
// This one is a standing FACT about the build: with no transport the browser can never list
// anything, and a permanently empty list with no explanation is the "silently absent" shape this
// repo keeps refusing elsewhere. So it is painted for as long as the player is on a browser, with
// no dwell, and it says the same thing every frame.
//
// It reuses U23's carrier and its per-frame-repaint argument unchanged (the buffer is not ours; see
// above). The two cannot contend in practice today -- g_notice is armed only by a LOBBY EXIT cause
// (host-left/link-lost), which requires a live session, and a no-transport peer never has one -- but
// the event still wins if it ever does, because an event is news and a standing fact is not.
//
// English literal, per the U18 / R-live-ui / U23 precedent, and short for the same reason: the
// widget's flags (0x04868042) do not request auto-wrap, so the line runs until it ends.
const wchar_t *const kNoModuleLine = L"No network module. Multiplayer is unavailable.";
bool                 g_no_module   = false; // armed once at arm time; main thread only

bool on_a_browser() {
    const void *list = *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    return list == (const void *)ADDR_BROWSER_SESSION || list == (const void *)ADDR_BROWSER_LOCAL;
}

// U42: on_host_advertise() (net_discovery.cpp) arms the manual host lobby unconditionally -- it is
// one of the seven protocol stubs F3F left ungated at INSTALL time, so a module=none host still
// reaches the real "Network players" screen (ruling Q8: ARM and explain, not gate). Until this fix
// that lobby carried no explanation for why Start never does anything useful -- the browser's
// standing line never painted there. It turns out it does not need a second buffer or a second
// widget: reading the lobby container's own child list (_G_LLM_UI_SCREEN_LOBBY_WIDGET_LIST,
// mh::addr::lobby_widget_origin) shows widget 0x6503b3 -- the SAME status-line widget the browser
// notice uses -- is ALSO one of its children (non-hidden, same flags 0x04868042), a fact the
// original U23 comment above ("child of BOTH MP browser containers") did not have. So the lobby
// gets the identical standing line through the identical mechanism: no new widget, no new buffer,
// just one more screen this predicate recognizes.
bool on_a_browser_or_lobby() {
    const void *list = *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    return list == (const void *)ADDR_BROWSER_SESSION || list == (const void *)ADDR_BROWSER_LOCAL ||
           list == (const void *)ADDR_LOBBY;
}

void no_module_notice_tick() {
    // Not on a browser or the lobby: the screen we ARE on may own this buffer (the map picker does).
    if (!on_a_browser_or_lobby()) return;
    lstrcpynW((wchar_t *)ADDR_MAP_STATUS_LINE, kNoModuleLine, 256);
}

} // namespace

namespace mh {
namespace ui {

void browser_notice_arm(int cause) {
    if (cause == 2) g_notice = L"Connection to the host was lost.";
    else if (cause == 1) g_notice = L"The host left the game.";
    else return; // 0 = no involuntary cause recorded: say nothing rather than guess
    g_notice_armed = GetTickCount();
    g_notice_shown = false;
    if (ui_verbose()) {
        char b[96];
        wsprintfA(b, "; U23: browser notice armed (cause=%d)\n", cause);
        ui_log(b);
    }
}

// F3c. The reason is the host's ASCII text (mh_net_proto::join_refusal_text), widened byte for byte
// -- no codepage question arises, which is the point of the host composing it from numbers and
// English words. Same dwell + same per-frame repaint as the two U23 causes, and the same width
// budget: this widget draws one unwrapped line of ~32 characters (U23's "Connection to the host was
// lost." fills it exactly), so the prefix is 9 and the protocol caps the reason at 20
// (JOIN_REFUSAL_TEXT_MAX). The first cut of this line ("Join refused: input codepage mismatch (host
// 1252, yours 1251)") was measured clipped at the panel edge -- the long form is the host's log line.
void browser_notice_arm_refused(const char *reason) {
    const wchar_t *pfx = L"Refused: ";
    int            n   = 0;
    while (pfx[n]) {
        g_refused_line[n] = pfx[n];
        ++n;
    }
    for (const char *p = reason ? reason : ""; *p && n < 255; ++p) g_refused_line[n++] = (wchar_t)(unsigned char)*p;
    g_refused_line[n] = 0;
    g_notice          = g_refused_line;
    g_notice_armed    = GetTickCount();
    g_notice_shown    = false;
    if (ui_verbose()) {
        char b[160];
        wsprintfA(b, "; F3c: browser notice armed (join refused: %s)\n", reason ? reason : "");
        ui_log(b);
    }
}

// mp:R4a. The relay-level notice. Same buffer discipline as F3c's (widened byte for byte, composed
// at arm time, one unwrapped line): the UDP module composes "Relay outdated (protocol 0 < 1)" -- 31
// characters against the line's ~32 -- and this side adds nothing, because the module is the party
// that knows both numbers and mh.dll must not learn what a relay is. Shares g_refused_line: the two
// cannot be live at once (a JOIN refusal needs a lobby, a relay dial happens on the browser before
// one), and if they ever were the later arm is the newer news.
void browser_notice_arm_relay(const char *line) {
    int n = 0;
    for (const char *p = line ? line : ""; *p && n < 255; ++p) g_refused_line[n++] = (wchar_t)(unsigned char)*p;
    g_refused_line[n] = 0;
    g_notice          = g_refused_line;
    g_notice_armed    = GetTickCount();
    g_notice_shown    = false;
    char b[128];
    wsprintfA(b, "; R4a: browser notice armed (relay: %s)\n", line ? line : "");
    ui_log(b);
}

void browser_notice_arm_no_module() {
    g_no_module = true;
    ui_log("; [net] no-module notice armed -- with no transport the MP browsers list nothing and a "
           "hosted lobby is one nobody else can reach (its own Start works vs AI since U43), so both "
           "carry the same standing line saying why instead of an unexplained empty list (ruling Q2, "
           "lobby coverage U42)\n");
}

// Called every frame from the lockstep present hook. Cheap when idle.
void browser_notice_tick() {
    if (!g_notice) {
        if (g_no_module) no_module_notice_tick();
        return;
    }
    const DWORD now        = GetTickCount();
    const bool  on_browser = on_a_browser();

    if (!on_browser) {
        // Before the browser: the exit takes a few frames to land, so DON'T start the dwell yet --
        // otherwise a slow transition spends the notice on frames nobody can read. After it: the
        // player navigated away, which is consent enough; drop the notice without touching the
        // buffer (the screen we are now on may own it -- the map picker writes the same global).
        if (g_notice_shown || now - g_notice_armed > NOTICE_ARM_MAX_MS) g_notice = nullptr;
        return;
    }
    if (!g_notice_shown) {
        // A separate flag rather than `g_notice_until != 0`: GetTickCount wraps, so 0 is a deadline
        // a run can legitimately compute, and using it as the "not started yet" sentinel would
        // re-arm the dwell every frame on the one browser visit in ~49.7 days that lands on it.
        g_notice_shown = true;
        g_notice_until = now + NOTICE_DWELL_MS;
        if (ui_verbose()) ui_log("; U23: browser notice visible\n");
    }
    if ((int)(now - g_notice_until) >= 0) {
        *(wchar_t *)ADDR_MAP_STATUS_LINE = L'\0'; // expired: hand the line back to retail map status
        g_notice                         = nullptr;
        return;
    }
    lstrcpynW((wchar_t *)ADDR_MAP_STATUS_LINE, g_notice, 256);
}

} // namespace ui
} // namespace mh
