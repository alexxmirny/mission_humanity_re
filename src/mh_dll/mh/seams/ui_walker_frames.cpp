// seams/ui_walker_frames.cpp -- the walker turn-frame fix (render only). Mechanism, the splice and the key are in
// include/mh_walkerframes_export.h; the pure fill rule is seams/walker_frames.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_walkerframes_export.h"
#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "net_internal.h"  // seam_log; g_ini
#include "en_guard.h"      // EN-only build gate
#include "seams/walker_frames.h"

namespace {

namespace wf = mh::seams::walker_frames;

// _G_LLM_STRAT_WALKER_FRAME_TABLE (.bss, 0x00ae1df8..0x00ae2a90): facing * 0x80 + microstep * 4.
constexpr uintptr_t TABLE_VA = 0x00ae1df8u;
// The CALL llm_strat_walker_frame_blend_table_init inside llm_strat_mode_init (boot stage 4).
constexpr uintptr_t INIT_CALL_SITE      = 0x0045f067u;
const uint8_t       INIT_CALL_EXPECT[5] = {0xE8, 0xB7, 0x19, 0x00, 0x00}; // rel32 -> 0x00460a23

uintptr_t g_init  = 0; // the retail init, the spliced CALL's own destination
bool      g_armed = false;

void __cdecl walker_frames_fill() {
    uint32_t *table = reinterpret_cast<uint32_t *>(TABLE_VA);
    char      m[200];
    if (!wf::looks_like_retail_init(table)) {
        seam_log("; walker turn frames: NOT filled -- the table is not in the retail-init shape (init did not run first?)\n");
        return;
    }
    const int n = wf::fill_unfilled(table);
    wsprintfA(m, "; walker turn frames: filled %d cells (16 unfilled facings x 32 microsteps) after the retail init\n", n);
    seam_log(m);
}

// clang-format off
// At the splice nothing is live (a void no-argument call): run the retail init, then fill, keeping every register.
__declspec(naked) void init_thunk() {
    __asm {
        call dword ptr [g_init]
        pushad
        call walker_frames_fill
        popad
        ret
    }
}
// clang-format on

} // namespace

extern "C" int MH_WalkerFrames_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    if (mh_ini_get_int("ui", "walker_turn_frames_fix", 1, g_ini) == 0) {
        seam_log("; walker turn frames: stock ([ui] walker_turn_frames_fix=0) -- a turning walker flashes the north image\n");
        return 0;
    }
    const char *refuse = "NOT armed (bytes differ from the expected CALL llm_strat_walker_frame_blend_table_init)";
    if (mh::hook::promoted_owner_of(INIT_CALL_SITE)) {
        refuse = "DISPLACED (llm_strat_mode_init is promoted this run; see the [interlock] line)";
    } else {
        int32_t orig;
        memcpy(&orig, INIT_CALL_EXPECT + 1, sizeof(orig));
        g_init = INIT_CALL_SITE + 5 + orig;
        uint8_t repl[5];
        repl[0]     = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)init_thunk - (INIT_CALL_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        g_armed = mh::hook::patch_bytes_guarded(INIT_CALL_SITE, INIT_CALL_EXPECT, repl, 5);
    }
    char m[200];
    wsprintfA(m, "; walker turn frames: %s at %08X ([ui] walker_turn_frames_fix=1)\n", g_armed ? "armed" : refuse,
              (unsigned)INIT_CALL_SITE);
    seam_log(m);
    return g_armed ? 1 : 0;
}
