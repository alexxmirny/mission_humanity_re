//
// seams/ui_keyrepeat.cpp -- U24: one dispatch per keystroke in the modal key pump.
//
// Full mechanism + why the fix is shaped this way: include/mh_keyrepeat_export.h.
// In short: llm_ui_modal_key_pump publishes a dequeued key into a global pair that nothing ever
// clears, and its 150 ms catch-up accumulator keeps reporting events that carry no new key -- so the
// caller dispatches the same character again for each. We clear the published pair at pump ENTRY,
// where it has provably already been dispatched, leaving KEYREC (the not-yet-published latch) and
// the accumulator's pacing alone.
//
// CONFIRMED: the fix cured the player's machine (2026-07-27). NOT confirmed: how many characters and
// WHY that machine -- the first diagnostic never fired (see catch_up_depth) so the repeat count has
// never actually been measured, on either machine. That is what the publish lines below are for.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>

#include "include/mh_keyrepeat_export.h"
#include "include/mh_run_context.h" // MH_RunDir
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "hook/detour.h"            // install_trampoline + WATCOM_PROLOGUE
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / wvsprintfA

namespace {

using mh::hook::install_trampoline; // shared inline-detour toolkit (hook/detour.h)

constexpr uintptr_t ADDR_PUMP      = mh::addr::llm_ui_modal_key_pump;
constexpr uintptr_t ADDR_KEY_SC    = mh::addr::_G_LLM_UI_MODAL_KEY_SCANCODE;     // published scancode (dword)
constexpr uintptr_t ADDR_KEY_CH    = mh::addr::_G_LLM_UI_MODAL_KEY_ASCII;        // published ASCII (short)
constexpr uintptr_t ADDR_STEP_NEXT = mh::addr::_G_LLM_UI_MODAL_STEP_NEXT_MS;     // next scheduled 150 ms step
constexpr uintptr_t ADDR_STEP_DEAD = mh::addr::_G_LLM_UI_MODAL_STEP_DEADLINE_MS; // end of the pump segment
constexpr uintptr_t ADDR_NOW_MS    = mh::addr::_G_LLM_UI_MENU_NOW_MS;            // menu clock, 1 sample/frame

constexpr int32_t STEP_MS      = 0x96;  // the pump's fixed catch-up step (150 ms)
constexpr int     LOG_FIRST    = 40;    // log every burst up to this many...
constexpr int     LOG_EVERY    = 100;   // ...then one in LOG_EVERY, so a bad machine cannot flood
constexpr int     STEPS_ABSURD = 10000; // sanity clamp on the diagnostic (never gates the fix)

bool  g_enabled = true;    // [ui] key_repeat_fix
void *g_tramp   = nullptr; // stolen prologue + jmp back to llm_ui_modal_key_pump+8

// Diagnostics only.
long     g_bursts     = 0; // observed runs longer than one dispatch (fix OFF)
long     g_suppressed = 0; // extra characters, total
long     g_worst      = 0; // longest single run seen
long     g_publishes  = 0; // publishes seen (one per real keystroke)
long     g_run        = 0; // consecutive entries showing the SAME published pair
uint32_t g_last_sc    = 0;
uint16_t g_last_ch    = 0;

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0; // SES1: per-SESSION (input during a match belongs to that match)

void kr_log(const char *fmt, ...) {
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

// How many events the pump is about to report for the payload standing at entry, i.e. how many times
// the caller would dispatch it. 1 = healthy. -1 = "not computable, read the raw timers in the log".
//
// The comparisons are UNSIGNED. Proven by the pump's own publish branch, which re-arms
// `STEP_NEXT_MS = STEP_DEADLINE_MS + 400; STEP_DEADLINE_MS = 0xffffffff`: signed, `NEXT < DEADLINE`
// would then be false forever, the pump would republish on every call and never return 0, and the
// modal loop would hang. It does not hang, so 0xffffffff is "infinity" and the segment that follows
// a publish IS the catch-up regime.
//
// THE FIRST VERSION OF THIS FUNCTION NEVER FIRED (2026-07-26). It opened with
// `if (now == -1 || next == -1 || dead == -1) return 1;`, treating 0xffffffff as "not catch-up" --
// but per the re-arm above, `dead` is 0xffffffff at exactly the moment this runs, on every machine.
// So it reported "healthy" always, on our rig and on the machine that actually reproduces the bug,
// and two zero-burst logs were read as confirmation. Hence the rule this file now follows: LOG THE
// RAW INPUTS, and let a derived verdict be wrong out loud rather than silently.
int catch_up_depth(uint32_t now, uint32_t next, uint32_t dead) {
    if (next >= dead) return 1; // publish branch, not a catch-up step
    if (now < next) return 0;   // caught up -> the pump reports nothing
    const uint32_t steps = (now - next) / (uint32_t)STEP_MS + 1u;
    return (steps > (uint32_t)STEPS_ABSURD) ? -1 : (int)steps;
}

// Run-before body: called at every llm_ui_modal_key_pump entry, including the modal typing loop, so it
// stays branch-cheap. See the header for why clearing here is the correct point.
void on_modal_pump() {
    volatile uint32_t *sc = (volatile uint32_t *)ADDR_KEY_SC;
    volatile uint16_t *ch = (volatile uint16_t *)ADDR_KEY_CH;

    const uint32_t sc_v = *sc;
    const uint16_t ch_v = *ch;

    if (!sc_v && !ch_v) { // nothing published (the common case)
        // End of an observed run: with the fix OFF the pair survives, so consecutive entries that
        // saw the SAME pair are the caller's actual re-dispatches -- ground truth, no formula.
        if (g_run > 1) {
            ++g_bursts;
            g_suppressed += g_run - 1;
            if (g_run - 1 > g_worst) g_worst = g_run - 1;
            kr_log("; keyrepeat OBSERVED run=%ld (the field really got %ld chars) | bursts=%ld extra=%ld worst=%ld",
                   g_run, g_run, g_bursts, g_suppressed, g_worst);
        }
        g_run     = 0;
        g_last_sc = 0;
        g_last_ch = 0;
        return;
    }

    const uint32_t now  = *(volatile uint32_t *)ADDR_NOW_MS;
    const uint32_t next = *(volatile uint32_t *)ADDR_STEP_NEXT;
    const uint32_t dead = *(volatile uint32_t *)ADDR_STEP_DEAD;

    if (sc_v == g_last_sc && ch_v == g_last_ch) {
        ++g_run; // same payload still standing: the caller dispatched it again
    } else {
        g_run     = 1; // a fresh publish
        g_last_sc = sc_v;
        g_last_ch = ch_v;
        ++g_publishes;
        // One line per PUBLISH carrying the raw timers, whatever the derived depth says. This is the
        // line that answers "why this machine": everything needed to recompute the catch-up by hand
        // is here, so a wrong predicate can no longer hide the measurement.
        if (g_publishes <= LOG_FIRST || (g_publishes % LOG_EVERY) == 0) {
            const int printable = (ch_v > 0x1f && ch_v < 0x7f) ? (int)ch_v : (int)'.';
            kr_log("; keyrepeat publish #%ld %s: sc=0x%02x ch='%c'(0x%02x) now=%u next=%u dead=%u "
                   "now-next=%d step=%d -> depth=%d",
                   g_publishes, g_enabled ? "fix ON" : "OBSERVE-ONLY", sc_v, printable, ch_v, now,
                   next, dead, (int)(now - next), (int)STEP_MS,
                   catch_up_depth(now, next, dead));
        }
    }

    if (!g_enabled) return; // observe-only A/B mode: measure the bug without fixing it
    *sc = 0;
    *ch = 0;
}

// clang-format off
__declspec(naked) void modal_pump_detour() {
    __asm {
        pushad
        pushfd
        call on_modal_pump
        popfd
        popad
        jmp  dword ptr [g_tramp] // stolen prologue + jmp back to llm_ui_modal_key_pump+8
    }
}
// clang-format on

} // namespace

extern "C" int MH_KeyRepeat_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only

    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);
    // Default ON with no ini (ship semantics). 0 = observe-only: bursts are still logged, so a
    // machine that reproduces can measure the bug and then A/B the fix.
    g_enabled = GetPrivateProfileIntA("ui", "key_repeat_fix", 1, ini) != 0;

    // U30: one call, not a byte compare and then a call. "wrong build or already hooked" was this
    // site guessing between two causes it could not distinguish; the primitive knows which, says so,
    // and files it in the end-of-arming [interlock] summary.
    if (!install_trampoline(ADDR_PUMP, (void *)modal_pump_detour, &g_tramp, 8,
                            mh::hook::entry_claim::exclusive, "the U24 modal key-repeat pump fix")) {
        kr_log("; keyrepeat DISARMED: install refused at 0x%08x (the reason is on the [interlock] "
               "line for this address in mh_net.log)",
               (unsigned)ADDR_PUMP);
        return 0;
    }
    kr_log("; keyrepeat armed (%s) -- modal key pump 0x%08x, published pair 0x%08x/0x%08x",
           g_enabled ? "fix ON" : "OBSERVE-ONLY, [ui] key_repeat_fix=0", (unsigned)ADDR_PUMP,
           (unsigned)ADDR_KEY_SC, (unsigned)ADDR_KEY_CH);
    return 1;
}
