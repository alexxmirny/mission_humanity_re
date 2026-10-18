// seams/ui_bldg_rearm.cpp -- mods:D11: Shift+click keeps the just-placed building type armed. Mechanism,
// the patch site and the contract are in include/mh_bldgrearm_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_bldgrearm_export.h"
#include "addr/mh_addrs.gen.h"    // mh::addr::bldg_rearm_clear_site + the UI globals
#include "addr/mh_structs.gen.h"  // mh_llm_strat_player_profile::primary_mother_bldg
#include "bldg_afford.h"          // can_afford_build_cost (shared with the D35 probe)
#include "hook/patch.h"           // patch_bytes_guarded
#include "state/region_runtime.h" // mh::state::ptr<> -- ORDER_QUEUE_COUNT is a RELOCATABLE region
#include "hook/promoted.h"        // promoted_owner_of
#include "net_internal.h"         // seam_log; g_ini
#include "en_guard.h"             // EN-only build gate

namespace {

constexpr int SITE_LEN = 10; // MOV dword ptr [_G_LLM_BUILD_PLACEMENT_ID], 0

// C7 05 DC 01 5D 00 | 00 00 00 00 : the stock LMB-up clear, replaced by CALL rel32 + 5 NOPs.
const uint8_t SITE_EXPECT[SITE_LEN] = {0xC7, 0x05, 0xDC, 0x01, 0x5D, 0x00, 0x00, 0x00, 0x00, 0x00};

bool g_armed = false;

volatile int32_t *placement_id() { return reinterpret_cast<volatile int32_t *>(mh::addr::_G_LLM_BUILD_PLACEMENT_ID); }

bool shift_held() {
    return ((*reinterpret_cast<const volatile uint8_t *>(mh::addr::_G_LLM_KEY_LSHIFT_HELD) |
             *reinterpret_cast<const volatile uint8_t *>(mh::addr::_G_LLM_KEY_RSHIFT_HELD)) &
            1) != 0;
}

// llm_ui_print_queue_text_id(text_id) (0x0049653d, __watcall: EAX = text id), bracketed by the floating
// message queue flag exactly as llm_strat_bldg_try_begin_placement does for a refused arm click.
// clang-format off
void print_refusal(int text_id) {
    volatile int32_t *queue_active = reinterpret_cast<volatile int32_t *>(mh::addr::_G_LLM_STRAT_FLOATING_MSG_QUEUE_ACTIVE);
    const uintptr_t   fn           = mh::addr::llm_ui_print_queue_text_id;
    *queue_active = 0;
    __asm {
        pushad
        mov  eax, text_id
        mov  ecx, fn
        call ecx
        popad
    }
    *queue_active = 1;
}
// clang-format on

// Called by the thunk in place of `mov [PLACEMENT_ID], 0` at the LMB-up of a placement. __cdecl, no
// arguments; it writes the id itself (0 = stock, or keeps it). Pure local UI: one MF_VIEW global.
void rearm_decide() {
    const int32_t type = *placement_id();
    bool          keep = false;
    if (type > 0 && *reinterpret_cast<const volatile int32_t *>(mh::addr::_G_LLM_GAME_TUTORIAL_STEP) == 0 &&
        shift_held()) {
        const uint32_t player = *reinterpret_cast<const volatile uint16_t *>(mh::addr::PlayerSide);
        const int32_t  planet = *reinterpret_cast<const volatile int32_t *>(mh::addr::G_PLANET_INDEX);
        const auto    *profiles =
            reinterpret_cast<const mh::game::mh_llm_strat_player_profile *>(mh::addr::_G_LLM_STRAT_PLAYERS);
        const bool mother = planet >= 0 && planet < 32 &&
                            (profiles[player & 0xffffu].primary_mother_bldg[planet] != 0 || mh_mother_free_covers(player)); // mods:A1
        int32_t err = 0;
        // This click's own order pays in the next sim step. At the retail variable step (a step every frame) it had paid by now; at a
        // fixed step a frame can pass without one, so while an order is still queued the stock must cover this copy AND the next.
        const bool unpaid = *mh::state::ptr<const int32_t>(mh::state::RID_STRAT_ORDER_QUEUE_COUNT) != 0;
        if (mother) err = mh::seam::can_afford_build_cost(player, type, unpaid ? 2 : 1);
        keep = mother && err == 0;
        char m[160];
        wsprintfA(m, "; D11: shift re-arm type %d %s%s\n", (int)type, keep ? "kept armed" : "chain stopped",
                  !mother ? " (no mothership building)" : (err ? " (next copy refused)" : ""));
        seam_log(m);
        if (mother && err != 0) print_refusal(err);
    }
    if (!keep) *placement_id() = 0;
}

// clang-format off
// Replaces the 10-byte `MOV [PLACEMENT_ID],0` with CALL rel32 + 5 NOPs. Flags and every register are
// preserved, as the stock store clobbers none.
__declspec(naked) void rearm_thunk() {
    __asm {
        pushfd
        pushad
        call rearm_decide
        popad
        popfd
        ret
    }
}
// clang-format on

} // namespace

extern "C" int MH_BldgRearm_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    if (mh_ini_get_int("ui", "shift_rearm_build", 1, g_ini) == 0) {
        seam_log("; D11: shift re-arm OFF ([ui] shift_rearm_build=0): the LMB-up clears the armed building type, stock\n");
        return 0;
    }
    const uintptr_t site = mh::addr::bldg_rearm_clear_site;
    const char     *why  = nullptr;
    if (mh::hook::promoted_owner_of(site)) {
        why = "DISPLACED (llm_strat_input_update is promoted this run)";
    } else {
        uint8_t repl[SITE_LEN];
        repl[0]           = 0xE8;
        const int32_t rel = (int32_t)((uintptr_t)&rearm_thunk - (site + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        memset(repl + 5, 0x90, SITE_LEN - 5);
        g_armed = mh::hook::patch_bytes_guarded(site, SITE_EXPECT, repl, SITE_LEN);
        if (!g_armed) why = "NOT armed (bytes differ from MOV [0x005d01dc],0 or a write was refused)";
    }
    char m[200];
    wsprintfA(m, "; D11: shift re-arm %s at %08X\n", g_armed ? "armed" : why, (unsigned)site);
    seam_log(m);
    return g_armed ? 1 : 0;
}
