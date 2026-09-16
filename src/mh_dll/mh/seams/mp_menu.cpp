//
// MP menu restore (Workstream U / tracker U18) -- restore mh.exe's real, shipped-but-unwired
// NETWORK GAME main-menu button using the localized art already in mh_ex.rsr, WITHOUT touching
// gamedata. RE: U18, the lobby RE.
//
// Retail shipped TWO main-menu backgrounds: MENUBCK1.GFX (6 buttons -- what the game loads) and the
// unused MENUBCK2.GFX (the intended 7-button menu: INTRO / NEW GAME / NETWORK GAME / TUTORIAL /
// LOAD GAME / CREDITS / QUIT), plus NETGAMEH.GFX (the network button's hover sprite, a sibling of
// NEWGAMEH.GFX). Both assets exist in mh_ex.rsr but the exe references neither.
//
// The retail main menu (llm_ui_main_menu_screen_load 0x4b64c8) paints the button labels/frames INTO
// the background art; each button is a static widget record (0x64fddb, stride 0x44) whose default
// state draws nothing and whose HOVER blits a per-button sprite (llm_ui_widget_draw 0x4c1100 ->
// _G_LLM_UI_SCREEN_MAIN_MENU_ID[disp_idx_alt]). We restore the 7-button menu (Route B, no id-array
// slot clobber):
//   (1) redirect the bg filename table[0] (0x604288) MENUBCK1.GFX -> MENUBCK2.GFX (DGROUP-writable);
//   (2) load NETGAMEH.GFX ourselves and blit it on hover from a DLL-owned draw_cb;
//   (3) build the NETWORK widget (clone of New Game -> y=177, our draw_cb, action = the U4/U9 name
//       screen -> local games browser flow) and restack the 4 buttons below it down one 40px slot;
//   (4) set the container's array to [Intro,NewGame,Network,Tutorial,Load,Credits,Quit].
// All widget writes land in DGROUP (verified r/w); no VirtualProtect, no gamedata edits.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "include/mh_mpmenu_export.h"
#include "include/mh_run_context.h" // MH_RunDir (per-run log folder)
#include "addr/mh_addrs.gen.h"      // generated EN VAs (tools/gen_dll_addrs.py)
#include "en_guard.h"               // EN-only build gate
#include "hook/detour.h"            // install_trampoline (shared inline-detour toolkit)
#include "hook/watcall.h"           // call_watcall1 (Watcom __watcall(EAX) bridge)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::call_watcall1;

namespace {

constexpr uintptr_t ADDR_MENU_ACTIVATE  = mh::addr::menu_activate_fn;  // menu activator (hooked)
constexpr uintptr_t ADDR_MENU_CONTAINER = mh::addr::mp_menu_container; // holds the widget-array pointer (+0x00)

// --- retail main-menu render model (U18 RE, 2026-07-24) ---
constexpr uintptr_t ADDR_GETMENUFILE  = mh::addr::GetMenuFile;               // char* name -> sprite handle (watcall EAX)
constexpr uintptr_t ADDR_NAME_TABLE   = mh::addr::menu_gfx_name_table;       // [0] = main-menu bg filename ptr
constexpr uintptr_t ADDR_WIDGET_HOVER = mh::addr::_G_LLM_UI_WIDGET_HOVERED;  // *-> currently-hovered widget
constexpr uintptr_t ADDR_WIDGET_SEL   = mh::addr::_G_LLM_UI_WIDGET_SELECTED; // *-> currently-selected widget
constexpr uintptr_t ADDR_DRAW_X       = mh::addr::_G_LLM_UI_WIDGET_DRAW_X;   // resolved widget draw X
constexpr uintptr_t ADDR_DRAW_Y       = mh::addr::_G_LLM_UI_WIDGET_DRAW_Y;   // resolved widget draw Y
constexpr uintptr_t ADDR_WINDOW_W     = mh::addr::WindowWidth;               // framebuffer width (clip arg)
constexpr uintptr_t BTN_INTRO         = mh::addr::menu_btn_intro;
constexpr uintptr_t BTN_NEWGAME       = mh::addr::menu_btn_newgame; // the clone template (uses NEWGAMEH hover)
constexpr uintptr_t BTN_TUTORIAL      = mh::addr::menu_btn_tutorial;
constexpr uintptr_t BTN_LOAD          = mh::addr::menu_btn_load;
constexpr uintptr_t BTN_CREDITS       = mh::addr::menu_btn_credits;
constexpr uintptr_t BTN_QUIT          = mh::addr::menu_btn_quit;

// widget field offsets (0x44 stride; verified 2026-07-11 diag + U18 RE)
constexpr int W_NAV = 0x00, W_ACTION = 0x04, W_FLAGS = 0x08, W_DRAW = 0x0c;
constexpr int W_IDX = 0x10, W_IDX_ALT = 0x14, W_IDX_SEL = 0x18;
constexpr int W_X = 0x1c, W_Y = 0x20, W_W = 0x24, W_H = 0x28, W_VALUE = 0x2c, W_MASK = 0x34;
constexpr int WIDGET_STRIDE = 0x44;

// 7-button MENUBCK2 layout Y (art-measured; = the MENUBCK1 records with the 4 below New Game +40)
constexpr int Y_NETWORK = 177, Y_TUTORIAL = 217, Y_LOAD = 257, Y_CREDITS = 297, Y_QUIT = 338;

static const char MENUBCK2_NAME[] = "MENUBCK2.GFX"; // DLL-owned; replaces filename-table[0]
static const char NETGAMEH_NAME[] = "NETGAMEH.GFX"; // network button hover sprite

void        **g_container = (void **)ADDR_MENU_CONTAINER;
void         *g_netgameh  = nullptr;       // cached GetMenuFile handle (sprite ptr) for the hover blit
unsigned char g_net_widget[WIDGET_STRIDE]; // the NETWORK GAME widget (clone of New Game)
void         *g_u18_array[9];              // [7 buttons + NULL] container array we own
bool          g_bg_redirected = false;

char g_log[MAX_PATH];
bool g_log_ready = false;

void menu_log(const char *fmt, ...) {
    if (!g_log_ready) {
        wsprintfA(g_log, "%smh_net.log", MH_RunDir());
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
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, line, lstrlenA(line), &w, nullptr);
    CloseHandle(h);
}

// ---- U18 network-button hover draw (Route B): a DLL-owned draw_cb that blits NETGAMEH when the
//      widget is hovered/selected, exactly as llm_ui_widget_draw would from an id-array slot -- but
//      without consuming one of the (full) 32 GFX-id slots.
static uintptr_t g_draw_fn = mh::addr::llm_gfx_draw_sprite_vclipped;

// Marshal cdecl args into __watcall llm_gfx_draw_sprite_vclipped(sprite=EAX, a1=EDX, a2=EBX, a3=ECX,
// a4=stack). The callee cleans its 1 stack arg (RET 4). EBX is cdecl callee-saved -> preserve it.
__declspec(naked) void net_blit(void * /*sprite*/, int /*a1*/, int /*a2*/, int /*a3*/, int /*a4*/) {
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push dword ptr [ebp+0x18] // a4 -> stack arg (callee RET 4 pops it)
        mov  eax, [ebp+0x08] // sprite -> EAX
        mov  edx, [ebp+0x0c] // a1    -> EDX
        mov  ebx, [ebp+0x10] // a2    -> EBX
        mov  ecx, [ebp+0x14] // a3    -> ECX
        call dword ptr [g_draw_fn]
        pop  ebx
        mov  esp, ebp
        pop  ebp
        ret
    }
}

void network_draw_body(void *widget) {
    void *hov = *(void **)ADDR_WIDGET_HOVER;
    void *sel = *(void **)ADDR_WIDGET_SEL;
    if (g_netgameh && (widget == hov || widget == sel)) {
        int x  = *(int *)ADDR_DRAW_X;
        int y  = *(int *)ADDR_DRAW_Y;
        int ww = *(int *)ADDR_WINDOW_W;
        net_blit(g_netgameh, x, y, 0, ww); // == llm_ui_widget_draw's hover-sprite blit
    }
}

// The widget draw_cb: called __watcall with the widget in EAX; must return with a plain ret and leave
// EBX/ESI/EDI/EBP intact (pushad/popad guarantees it). Default (non-hover) state draws nothing -- the
// "NETWORK GAME" label is painted into the MENUBCK2 background, like the other 6 buttons.
__declspec(naked) void network_widget_draw() {
    __asm {
        pushad
        mov  eax, [esp+0x1c] // recover widget (saved EAX; pushad order EDI..EAX)
        push eax
        call network_draw_body
        add  esp, 4
        popad
        ret
    }
}

// ---- run-then-extend inline hook (steal 8-byte prologue; trampoline runs the original as a subroutine)
void *g_menu_tramp = nullptr;

__declspec(naked) void menu_activate_detour() {
    __asm {
        call dword ptr [g_menu_tramp] // run the retail menu activator; returns here
        pushad // preserve its EAX/EDX return across our work
        call MH_Menu_Apply
        popad
        ret
    }
}

// ---- U9/U4: the retail player-NAME entry screen prepended to the MP button flow (UNCHANGED) --------
// The retail net-setup screen (llm_lobby_network_setup_screen) is a reusable single-field input screen;
// entered this way it edits _G_LLM_MP_PLAYER_NAME (a recent-names MRU + host IP) -- a "choose your name"
// screen -- and persists to setup.dat. Retail shipped no menu path to it; our button routes THROUGH it,
// then continues into the existing browser/create flow.
constexpr uintptr_t ADDR_NETSETUP_SCREEN     = mh::addr::llm_lobby_network_setup_screen; // name field + host IP
constexpr uintptr_t ADDR_SAVE_SETUP_DAT      = mh::addr::llm_cfg_save_setup_dat;         // persist name/game-name
constexpr uintptr_t ADDR_CONFIRM_CB_SLOT     = mh::addr::confirm_cb_slot;                // the active screen's live confirm-cb
constexpr uintptr_t ADDR_RETAIL_NAME_CONFIRM = mh::addr::llm_mp_netsetup_name_confirm;   // retail transition
typedef void (*retail_vfn)(void);

// Name confirmed on the net-setup screen -> persist, then run the RETAIL name-confirm transition
// (name screen -> local games browser SAVELOAD_LIST); we only prepend the setup.dat save.
void mp_after_name_confirmed() {
    ((retail_vfn)ADDR_SAVE_SETUP_DAT)();      // write _G_LLM_MP_PLAYER_NAME to setup.dat (survives relaunch)
    ((retail_vfn)ADDR_RETAIL_NAME_CONFIRM)(); // retail: name screen -> local games browser
    menu_log("; MP name confirmed -> persisted setup.dat + retail transition to local games browser");
}

__declspec(naked) void mp_name_confirm_cb() { // installed into the confirm-cb slot; __watcall-compatible
    __asm {
        pushad
        pushfd
        call mp_after_name_confirmed
        popfd
        popad
        mov  eax, 1
        ret
    }
}

// MP button action: show the retail name-entry screen, then hijack its confirm callback to ours (the
// documented retail lever -- write the confirm-cb slot -- used exactly as retail's own builders do).
void mp_show_name_entry() {
    ((retail_vfn)ADDR_NETSETUP_SCREEN)();                        // push NET_SETUP wired for the name field
    *(void **)ADDR_CONFIRM_CB_SLOT = (void *)mp_name_confirm_cb; // override retail name_confirm -> ours
    menu_log("; MP button -> name-entry screen (confirm cb hijacked to mp_name_confirm_cb)");
}

__declspec(naked) void mp_name_entry_detour() { // widget action_cb: eax=1, plain ret
    __asm {
        pushad
        pushfd
        call mp_show_name_entry
        popfd
        popad
        mov  eax, 1
        ret
    }
}

} // namespace

extern "C" void MH_Menu_SetContainer(void **c) {
    if (c) g_container = c;
}
extern "C" void  MH_Menu_SetGeometry(int, int) { /* U18: geometry is fixed by the MENUBCK2 art; no-op */ }
extern "C" void *MH_Menu_GetWidget(void) { return (void *)g_net_widget; }

// One-time: redirect the main-menu background MENUBCK1.GFX -> MENUBCK2.GFX so the retail loader paints
// the 7-button art. The filename table [0] is in DGROUP (writable); guarded on the current pointer so a
// wrong build / already-redirected state is a safe no-op.
static void mp_redirect_bg_once() {
    if (g_bg_redirected) return;
    const char **tbl = (const char **)ADDR_NAME_TABLE;
    if (tbl[0] && lstrcmpiA(tbl[0], "MENUBCK1.GFX") == 0) {
        tbl[0]          = MENUBCK2_NAME;
        g_bg_redirected = true;
        menu_log("; U18 bg redirected: MENUBCK1.GFX -> MENUBCK2.GFX");
    } else if (tbl[0] && lstrcmpiA(tbl[0], "MENUBCK2.GFX") == 0) {
        g_bg_redirected = true; // already ours (re-arm)
    } else {
        menu_log("; U18 bg redirect SKIPPED (table[0]=%s)", (tbl[0] ? tbl[0] : "(null)"));
    }
}

extern "C" void MH_Menu_Apply(void) {
    void **cur = (void **)(*g_container);
    if (!cur || cur == (void **)g_u18_array) return; // not built yet, or already ours
    if (!cur[0]) return;                             // menu not populated yet

    mp_redirect_bg_once(); // belt-and-braces (primary redirect is at install, before the first load)

    if (!g_netgameh) { // load the NETGAMEH hover sprite once (rsr is up by now)
        g_netgameh = (void *)(uintptr_t)call_watcall1(ADDR_GETMENUFILE, (void *)NETGAMEH_NAME);
        menu_log("; U18 NETGAMEH.GFX -> handle %08X", (unsigned)(uintptr_t)g_netgameh);
    }

    // restack the 4 stock buttons below New Game to the MENUBCK2 7-button layout (idempotent absolute set)
    *(int *)(BTN_TUTORIAL + W_Y) = Y_TUTORIAL;
    *(int *)(BTN_LOAD + W_Y)     = Y_LOAD;
    *(int *)(BTN_CREDITS + W_Y)  = Y_CREDITS;
    *(int *)(BTN_QUIT + W_Y)     = Y_QUIT;

    // build the NETWORK GAME widget = clone of New Game, overriding action / draw / geometry / hotkey.
    // x/w/h/flags(0x100 mask)/field_0x34(mask slot 0, correct 219x30 shape) are inherited (correct).
    memcpy(g_net_widget, (const void *)BTN_NEWGAME, WIDGET_STRIDE);
    *(void **)(g_net_widget + W_NAV)    = nullptr;                      // no keyboard-nav target
    *(void **)(g_net_widget + W_ACTION) = (void *)mp_name_entry_detour; // -> name screen -> browser (U4/U9)
    *(void **)(g_net_widget + W_DRAW)   = (void *)network_widget_draw;  // DLL draw_cb: hover blits NETGAMEH
    *(int *)(g_net_widget + W_IDX)      = 0;                            // default state draws nothing
    *(int *)(g_net_widget + W_IDX_ALT)  = 0;                            // (hover handled by our draw_cb)
    *(int *)(g_net_widget + W_IDX_SEL)  = 0;
    *(int *)(g_net_widget + W_Y)        = Y_NETWORK; // slot 3 (between New Game + Tutorial)
    g_net_widget[W_VALUE]               = 'n';       // hotkey (no g/t/l/c/i/q collision)

    // assemble the 7-button array (display order) + NULL terminator
    g_u18_array[0] = (void *)BTN_INTRO;
    g_u18_array[1] = (void *)BTN_NEWGAME;
    g_u18_array[2] = (void *)g_net_widget;
    g_u18_array[3] = (void *)BTN_TUTORIAL;
    g_u18_array[4] = (void *)BTN_LOAD;
    g_u18_array[5] = (void *)BTN_CREDITS;
    g_u18_array[6] = (void *)BTN_QUIT;
    g_u18_array[7] = nullptr;
    *g_container   = (void *)g_u18_array;
    menu_log("; U18 7-button network menu applied (MENUBCK2 + NETGAMEH hover; net widget %08X)",
             (unsigned)(uintptr_t)g_net_widget);
}

extern "C" int MH_Menu_Install(void) {
    if (!mh::en_build_ok()) { // EN-only: arm nothing on any other image
        menu_log("; MP menu restore NOT armed: not the EN build");
        return 0;
    }
    // U30: the pre-check STAYS here (it is not the `&&` pattern -- mp_redirect_bg_once() runs
    // between it and the install, and on a build we are not hooking that redirect should not happen),
    // but it is no longer a bare byte compare. detour_refusal is the primitive's OWN decision without
    // the write, so an entry another mechanism owns is named as such instead of being reported as a
    // wrong build.
    if (mh::hook::detour_refusal(ADDR_MENU_ACTIVATE, mh::hook::entry_claim::exclusive,
                                 mh::hook::WATCOM_PROLOGUE) != mh::hook::refuse_reason::none) {
        // Let the primitive word it and file it in the end-of-arming summary: with the entry
        // contested it writes nothing, so this call is the refusal report and nothing else.
        mh::hook::install_trampoline(ADDR_MENU_ACTIVATE, (void *)menu_activate_detour, &g_menu_tramp, 8,
                                     mh::hook::entry_claim::exclusive,
                                     "the U18 MP main-menu button restore");
        menu_log("; MP menu restore NOT armed -- see the [interlock] line for the reason");
        return 0;
    }
    mp_redirect_bg_once(); // redirect the bg BEFORE the first main-menu load
    bool ok = mh::hook::install_trampoline(ADDR_MENU_ACTIVATE, (void *)menu_activate_detour, &g_menu_tramp, 8,
                                           mh::hook::entry_claim::exclusive,
                                           "the U18 MP main-menu button restore");
    menu_log(ok ? "; MP menu restore armed (U18: 7-button MENUBCK2 menu)" : "; MP menu restore install failed");
    return ok ? 1 : 0;
}
