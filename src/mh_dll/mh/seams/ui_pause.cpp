//
// seams/ui_pause.cpp -- D19: a hotkey that enters the orphaned PAUSE screen (game mode 5).
//
// Full mechanism, evidence and the two gates: include/mh_pause_export.h. In short: mode 5 is a
// finished pause screen (the strategic view re-rendered with the sim frozen; the game's own banner
// TEXT[0x94] reads "Game paused. Press any key to continue...") that nothing in the retail binary
// ever selects. Its resume path already works, so the whole feature is "store 5 into
// _G_LLM_GAME_MODE when the bound key is pressed in the live strategic view".
//
// The key is taken OUT of the game's key ring so the stock hotkey ladder in llm_strat_input_update
// never sees it -- one press does one thing.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdint>

#include "include/mh_pause_export.h"
#include "include/mh_run_context.h" // MH_RunDir
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "addr/mh_structs.gen.h"    // mh::game::mh_llm_input_key_event (0x38 stride)
#include "hook/detour.h"            // install_trampoline + WATCOM_PROLOGUE
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / GetPrivateProfile*

namespace {

using mh::game::mh_llm_input_key_event;
using mh::hook::install_trampoline;

constexpr uintptr_t ADDR_INPUT   = mh::addr::llm_strat_input_update;
constexpr uintptr_t ADDR_MODE    = mh::addr::_G_LLM_GAME_MODE;
constexpr uintptr_t ADDR_SESSION = mh::addr::_G_LLM_GAME_SESSION_MODE;
constexpr uintptr_t KEVENTS      = mh::addr::_G_LLM_INPUT_KEY_EVENTS;
constexpr uintptr_t KREADIDX     = mh::addr::_G_LLM_INPUT_KEY_READ_IDX;
constexpr uintptr_t KWRITEIDX    = mh::addr::_G_LLM_INPUT_KEY_WRITE_IDX;

constexpr uint32_t RING_MASK           = 0x7f; // 128 slots, indices advance &0x7f (both producers do this)
constexpr uint32_t EV_DOWN             = 0x100;
constexpr uint8_t  MODE_STRAT          = 2;
constexpr uint8_t  MODE_PAUSED         = 5;
constexpr uint8_t  SESSION_MP_LOCKSTEP = 3;

constexpr uint32_t DEFAULT_KEY = 0x19; // P (PC set-1) -- the conventional pause key, and free: the
                                       // stock strategic hotkey ladder never compares against 0x19
                                       // (the hotkey map lists the claimed set). `key=0x45` binds
                                       // the real Pause/Break key instead, at the cost of NumLock
                                       // aliasing onto it (both arrive as 0x45 after the ring's
                                       // &0x7f mask).
constexpr int LOG_FIRST = 20;          // log this many entries, then go quiet

uint32_t g_key    = DEFAULT_KEY;
void    *g_tramp  = nullptr;
long     g_enters = 0;

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0; // SES1: per-SESSION (shares mh_input.log with ui_keyrepeat)

void pz_log(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_input.log", &g_log_gen);
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
    DWORD wrote = 0;
    WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
    CloseHandle(h);
}

mh_llm_input_key_event *slot(uint32_t i) {
    return (mh_llm_input_key_event *)(KEVENTS + (size_t)i * sizeof(mh_llm_input_key_event));
}

// Delete the event at `i` from the ring by shifting every later entry one slot down and retreating
// the WRITE index -- the mirror of the producers' `write = (write + 1) & 0x7f`. Consuming rather
// than peeking is what keeps the bound key from ALSO firing whatever the stock ladder does with it.
// Returns the index to re-examine (the entry that moved into `i`).
uint32_t ring_erase(uint32_t i) {
    uint32_t w = *(volatile uint32_t *)KWRITEIDX;
    for (uint32_t j = i; j != ((w + RING_MASK) & RING_MASK); j = (j + 1) & RING_MASK)
        *slot(j) = *slot((j + 1) & RING_MASK);
    *(volatile uint32_t *)KWRITEIDX = (w + RING_MASK) & RING_MASK; // == (w - 1) & 0x7f
    return i;
}

// Run-before body: called at every llm_strat_input_update entry, i.e. once per strategic frame and
// again whenever a UI screen redraws the world behind a dialog. Cheap: it walks only the events
// pending this frame (usually none).
void on_strat_input() {
    if (!g_key) return;
    if (*(volatile uint8_t *)ADDR_MODE != MODE_STRAT) return;             // live strategic view only
    if (*(volatile uint8_t *)ADDR_SESSION == SESSION_MP_LOCKSTEP) return; // never pause one side of a match

    const uint32_t r     = *(volatile uint32_t *)KREADIDX;
    bool           enter = false;

    for (uint32_t i = r; i != *(volatile uint32_t *)KWRITEIDX;) {
        mh_llm_input_key_event *ev = slot(i);
        if (ev->scancode != g_key) {
            i = (i + 1) & RING_MASK;
            continue;
        }
        if (ev->event_type & EV_DOWN) enter = true;
        i = ring_erase(i); // both the down and its up: the ladder must not see this key at all
    }

    if (!enter) return;
    *(volatile uint8_t *)ADDR_MODE = MODE_PAUSED;
    ++g_enters;
    if (g_enters <= LOG_FIRST)
        pz_log("; pause ENTER #%ld (scancode 0x%02x) -- mode 2 -> 5", g_enters, g_key);
}

// clang-format off
__declspec(naked) void strat_input_detour() {
    __asm {
        pushad
        pushfd
        call on_strat_input
        popfd
        popad
        jmp  dword ptr [g_tramp] // stolen prologue + jmp back to llm_strat_input_update+8
    }
}
// clang-format on

} // namespace

extern "C" int MH_Pause_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only

    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);

    // Default ON with no ini (ship semantics -- the point of the feature is that a player gets the
    // pause screen back). `key=0` unbinds it and leaves the game exactly as it shipped.
    g_key = (uint32_t)GetPrivateProfileIntA("pause", "key", (int)DEFAULT_KEY, ini);
    if (g_key > 0x7f) {
        pz_log("; pause DISARMED: [pause] key=0x%02x is outside the 7-bit scancode range", g_key);
        return 0;
    }
    if (!g_key) return 0; // explicitly unbound

    // U30: see ui_keyrepeat.cpp -- the compare moved into the primitive, which is the only place
    // that can tell a wrong build from an entry somebody else already owns.
    if (!install_trampoline(ADDR_INPUT, (void *)strat_input_detour, &g_tramp, 8,
                            mh::hook::entry_claim::exclusive, "the D19 pause-screen input detour")) {
        pz_log("; pause DISARMED: install refused at 0x%08x (the reason is on the [interlock] line "
               "for this address in mh_net.log)",
               (unsigned)ADDR_INPUT);
        return 0;
    }
    pz_log("; pause armed -- scancode 0x%02x enters game mode 5 (pause) from the strategic view "
           "(input 0x%08x, ring 0x%08x)",
           g_key, (unsigned)ADDR_INPUT, (unsigned)KEVENTS);
    return 1;
}
