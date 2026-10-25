// seams/ui_options_scale.cpp -- mods:RES4: the "Interface size" row of the in-game Options screen.
// Mechanism, the addresses and the apply rule: include/mh_optscale_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "include/mh_optscale_export.h"
#include "include/mh_config_dir.h"  // RL3: <config dir>mh_net.ini
#include "mh_ini_gate.h"            // RL2: the ship gate every ini read goes through
#include "include/mh_run_context.h" // mh_run_path
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // _G_LLM_GAME_SESSION_MODE
#include "gfx/classic_sizes.h"      // RES25
#include "gfx/ddraw_own.h"          // owned_ui_scale_choice / owned_set_ui_scale_choice
#include "hook/detour.h"            // install_trampoline
#include "hook/patch.h"             // patch_bytes_guarded
#include "ui/player_strings.h"      // the row's label and its Auto choice
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

// ---- the stock Options screen (EN VAs) ------------------------------------------------------------------------
constexpr uintptr_t ADDR_LIST_CHILDREN = 0x00654153u; // llm_ui_widget_list.children of the Options screen
constexpr uintptr_t ADDR_CHILDREN_OLD  = 0x006534c3u; // the stock NULL-terminated widget pointer array
constexpr uintptr_t ADDR_W_SCREEN_SIZE = 0x0064fd53u; // the Screen size row (a spinner widget, 0x44 bytes)
constexpr uintptr_t ADDR_OPEN          = 0x004b8383u; // llm_ui_options_menu_open
constexpr uintptr_t ADDR_OK            = 0x004b86a4u; // llm_ui_options_menu_ok_action_cb
constexpr int       WIDGET_SIZE        = 0x44;
constexpr int       W_LABEL_OFF        = 0x38; // llm_ui_widget.label (UTF-16 text pointer)
constexpr int       W_PARAM_OFF        = 0x30; // llm_ui_widget.param_block
constexpr int       W_X_OFF            = 0x1c; // llm_ui_widget.x (the flow offset from the previous child)
constexpr int       W_Y_OFF            = 0x20; // llm_ui_widget.y (likewise)
constexpr int       ROW_DY             = 30;   // one options row (the stock Screen size -> Flags step)
constexpr uint8_t   SESSION_SP         = 1;    // _G_LLM_GAME_SESSION_MODE
constexpr int       W_MAX_CHILDREN     = 16;

// llm_ui_spinner: options_table (pointer to label-slot pointers), count, ext_counter (the int the row edits), then two
// fields a pure-text spinner leaves zero (the value when ext_counter is null, the sprite offset of an icon spinner).
struct spinner {
    uint32_t table;
    uint32_t count;
    uint32_t ext_counter;
    uint32_t value;
    uint32_t sprite;
};

alignas(4) uint8_t g_widget[WIDGET_SIZE];
spinner  g_spin;
uint32_t g_children_mp[W_MAX_CHILDREN]; // title .. Screen size, Interface size, Flags, Ok, Cancel
uint32_t g_children_sp[W_MAX_CHILDREN]; // title .. Screen size, Flags (hidden in SP), Interface size, Ok, Cancel
uint32_t g_table[mh::gfx::OWNED_UI_SCALE_CHOICES];
uint32_t g_slots[mh::gfx::OWNED_UI_SCALE_CHOICES]; // each: a pointer to the choice's UTF-16 text
int      g_index = 0;                              // the row's value while the screen is up

const wchar_t *const PERCENT_TEXT[mh::gfx::OWNED_UI_SCALE_CHOICES] = {nullptr, L"100%", L"125%", L"150%", L"175%", L"200%"};
const char *const    INI_VALUE[mh::gfx::OWNED_UI_SCALE_CHOICES]    = {"auto", "100", "125", "150", "175", "200"};

void *g_open_tramp = nullptr;
void *g_ok_tramp   = nullptr;
bool  g_armed      = false;

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

void os_log(const char *fmt, ...) {
    mh_proc_path(g_log, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[300];
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

// After the original open: the row shows the real state, in the pack's language.
extern "C" void __cdecl on_open_post() {
    // Single player hides the unit-flags row (the stock screen leaves its 30 px EMPTY above Ok), so the new row takes that slot and the
    // screen is the stock height; a multiplayer game shows Flags, so the row goes above it and Ok / Cancel move down one row.
    const bool sp                    = *(const uint8_t *)mh::addr::_G_LLM_GAME_SESSION_MODE == SESSION_SP;
    *(uint32_t *)ADDR_LIST_CHILDREN  = (uint32_t)(uintptr_t)(sp ? g_children_sp : g_children_mp);
    *(int32_t *)(g_widget + W_Y_OFF) = sp ? 0 : ROW_DY;
    g_index                          = mh::gfx::owned_ui_scale_choice();
    for (int i = 0; i < mh::gfx::OWNED_UI_SCALE_CHOICES; ++i) {
        const wchar_t *t = i == 0 ? mh::ui::tr(mh::ui::Str::OPT_INTERFACE_AUTO) : PERCENT_TEXT[i];
        g_slots[i]       = (uint32_t)(uintptr_t)t;
        g_table[i]       = (uint32_t)(uintptr_t)&g_slots[i];
    }
    *(uint32_t *)(g_widget + W_LABEL_OFF) = (uint32_t)(uintptr_t)mh::ui::tr(mh::ui::Str::OPT_INTERFACE_SIZE);
}

// Before the original Ok: commit the choice. Persist it to the key the launcher writes, apply it live.
extern "C" void __cdecl on_ok_pre() {
    if (g_index < 0 || g_index >= mh::gfx::OWNED_UI_SCALE_CHOICES) return;
    if (g_index == mh::gfx::owned_ui_scale_choice()) return;
    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini);
    mh::gfx::owned_set_ui_scale_choice(g_index, ini);
    const BOOL ok       = WritePrivateProfileStringA("video", "ui_scale", INI_VALUE[g_index], ini);
    char       back[16] = {0};
    GetPrivateProfileStringA("video", "ui_scale", "", back, sizeof(back), ini);
    os_log("; [ui] options menu: Interface size -> %s (%s; the ini reads back [video] ui_scale=%s)", INI_VALUE[g_index],
           ok ? "saved" : "ini write FAILED", back);
}

// clang-format off
__declspec(naked) void open_detour() {
    __asm {
        call dword ptr [g_open_tramp]  // original; returns 1 in EAX
        pushad
        call on_open_post
        popad
        ret
    }
}

__declspec(naked) void ok_detour() {
    __asm {
        pushad
        call on_ok_pre
        popad
        jmp  dword ptr [g_ok_tramp]    // original; its return goes to our caller
    }
}
// clang-format on

} // namespace

extern "C" int MH_OptScale_Install(void) {
    if (g_armed) return 1;
    if (!mh::en_build_ok()) return 0;
    char ini_path[MAX_PATH];
    mh::cfgdir::ini_path(ini_path);
    if (mh::gfx::classic_sizes(ini_path)) { // RES25: classic sizes only has no Interface size scaling, so the stock Options screen is left alone
        os_log("; [ui] options Interface size row OFF (classic sizes only)");
        return 0;
    }
    if (!mh_ini_get_int("video", "ui_scale_row", 1, ini_path)) { // dev kill switch: the stock Options screen, untouched
        os_log("; [ui] options Interface size row OFF ([video] ui_scale_row=0)");
        return 0;
    }

    // Everything is checked before anything is written: the stock array is exactly what ReVA read, and the list still points at it.
    const uint32_t *old       = (const uint32_t *)ADDR_CHILDREN_OLD;
    int             n         = 0;
    int             insert_at = -1;
    while (n < W_MAX_CHILDREN - 2 && old[n]) {
        if (old[n] == ADDR_W_SCREEN_SIZE) insert_at = n + 1;
        ++n;
    }
    if (insert_at < 0 || old[n] != 0 || n != 9 || *(const uint32_t *)ADDR_LIST_CHILDREN != ADDR_CHILDREN_OLD) {
        os_log("; [ui] options Interface size row NOT armed -- the Options widget array is not the stock one (%d children)", n);
        return 0;
    }

    memcpy(g_widget, (const void *)ADDR_W_SCREEN_SIZE, WIDGET_SIZE); // same flags, callbacks, size and flow offset as Screen size
    g_spin.table                          = (uint32_t)(uintptr_t)g_table;
    g_spin.count                          = mh::gfx::OWNED_UI_SCALE_CHOICES;
    g_spin.ext_counter                    = (uint32_t)(uintptr_t)&g_index;
    g_spin.value                          = 0;
    g_spin.sprite                         = 0;
    *(uint32_t *)(g_widget + W_PARAM_OFF) = (uint32_t)(uintptr_t)&g_spin;
    *(int32_t *)(g_widget + W_X_OFF)      = 0; // Screen size sits 30 px left of its slider; this row and the ones below stay in ITS column
    *(uint32_t *)(g_widget + W_LABEL_OFF) = 0; // set by every open, in the pack's language

    // `old` holds ... Screen size, Flags, Ok, Cancel (n == 9): the MP order inserts after Screen size, the SP order after Flags.
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (i == insert_at) g_children_mp[k++] = (uint32_t)(uintptr_t)g_widget;
        g_children_mp[k++] = old[i];
    }
    g_children_mp[k] = 0;
    int ks           = 0;
    for (int i = 0; i < n; ++i) {
        g_children_sp[ks++] = old[i];
        if (i == insert_at) g_children_sp[ks++] = (uint32_t)(uintptr_t)g_widget; // after Flags (old[insert_at] is Flags)
    }
    g_children_sp[ks] = 0;

    if (!mh::hook::install_trampoline(ADDR_OPEN, (void *)open_detour, &g_open_tramp, 8, mh::hook::entry_claim::exclusive,
                                      "the in-game Options Interface size row (open)")) {
        os_log("; [ui] options Interface size row NOT armed -- the open hook was refused (see the [interlock] line)");
        return 0;
    }
    if (!mh::hook::install_trampoline(ADDR_OK, (void *)ok_detour, &g_ok_tramp, 8, mh::hook::entry_claim::exclusive,
                                      "the in-game Options Interface size row (Ok)")) {
        os_log("; [ui] options Interface size row: the Ok hook was refused -- the row shows but never applies");
    }
    const uint32_t expect = (uint32_t)ADDR_CHILDREN_OLD, repl = (uint32_t)(uintptr_t)g_children_mp;
    if (!mh::hook::patch_bytes_guarded(ADDR_LIST_CHILDREN, (const uint8_t *)&expect, (const uint8_t *)&repl, 4)) {
        os_log("; [ui] options Interface size row NOT armed -- the children pointer could not be swapped");
        return 0;
    }
    g_armed = true;
    os_log("; [ui] options Interface size row armed: %d children (the row follows Screen size)", k);
    return 1;
}
