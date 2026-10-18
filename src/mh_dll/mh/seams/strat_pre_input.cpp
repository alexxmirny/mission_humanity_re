//
// seams/strat_pre_input.cpp -- see strat_pre_input.h.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "strat_pre_input.h"

#include "addr/mh_addrs.gen.h" // llm_strat_input_update
#include "hook/detour.h"       // install_trampoline + WATCOM_PROLOGUE

namespace {

using mh::hook::install_trampoline;

constexpr int MAX_FNS = 6;

mh::seams::pre_input_fn g_fns[MAX_FNS];
int                     g_n      = 0;
void                   *g_tramp  = nullptr;
bool                    g_failed = false;

void run_all() {
    for (int i = 0; i < g_n; ++i) g_fns[i]();
}

// clang-format off
__declspec(naked) void strat_input_detour() {
    __asm {
        pushad
        pushfd
        call run_all
        popfd
        popad
        jmp  dword ptr [g_tramp] // stolen prologue + jmp back to llm_strat_input_update+8
    }
}
// clang-format on

} // namespace

namespace mh::seams {

bool strat_pre_input_add(pre_input_fn fn, const char *who) {
    if (!fn || g_failed || g_n >= MAX_FNS) return false;
    if (!g_tramp) {
        // U30: the byte compare lives in the primitive, which is the only place that can tell a wrong build
        // from an entry somebody else already owns.
        if (!install_trampoline(mh::addr::llm_strat_input_update, (void *)strat_input_detour, &g_tramp, 8,
                                mh::hook::entry_claim::exclusive, who ? who : "the strategic pre-input detour")) {
            g_failed = true;
            return false;
        }
    }
    g_fns[g_n++] = fn;
    return true;
}

} // namespace mh::seams
