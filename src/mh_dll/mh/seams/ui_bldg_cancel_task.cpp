// seams/ui_bldg_cancel_task.cpp -- mp:D28: the building dialog's "cancel task -> Yes" routed through
// a replicated building order in a lockstep match, instead of the retail local call. Mechanism,
// register contract and the cost are in include/mh_canceltask_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "include/mh_canceltask_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::canceltask_finish_call_site / _G_LLM_GAME_SESSION_MODE
#include "addr/mh_calls.gen.h"   // mh::call::llm_bldg_finish_current_order / llm_strat_order_dispatch
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_BUILDINGS / RID_BUILDING
#include "addr/mh_structs.gen.h" // mh::game::mh_map_object_building / mh_cfg_final_struct_Building
#include "hook/patch.h"          // patch_bytes_guarded
#include "state/roster_caps.h"   // live_roster_caps(): the per-player building cap the host BOUND (500 on a cap-raised exe)
#include "hook/promoted.h"       // promoted_owner_of
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate

namespace {

constexpr uint8_t  SESSION_MP_LOCKSTEP = 3;    // _G_LLM_GAME_SESSION_MODE is a byte
constexpr uint16_t KIND_BUILDING       = 0x40; // owner_and_kind: player nibble | building kind

// The exact 5 bytes at the splice: CALL llm_bldg_finish_current_order (rel32 from 0x004c7065).
const uint8_t FINISH_CALL_EXPECT[5] = {0xE8, 0x46, 0x9C, 0xFA, 0xFF};

bool g_spliced  = false;
bool g_by_order = true; // [net] cancel_task_order (default 1); 0 = retail local call, the reproduction arm
long g_routed   = 0;
long g_local    = 0;

bool in_lockstep_match() { return *(const volatile uint8_t *)mh::addr::_G_LLM_GAME_SESSION_MODE == SESSION_MP_LOCKSTEP; }

// The C++ half. `player` and `building_index` are the callee's own __watcall arguments (EAX/EDX at
// the spliced CALL). Runs with every register saved by the naked thunk below.
void cancel_task(uint32_t player, uint32_t building_index) {
    if (!g_by_order || !in_lockstep_match()) {
        ++g_local;
        mh::call::llm_bldg_finish_current_order(player, building_index);
        return;
    }
    using mh::game::mh_cfg_final_struct_Building;
    using mh::game::mh_map_object_building;
    const auto   &lv         = mh::state::live();
    const int32_t per_player = mh::state::live_roster_caps().buildings; // derived, not the stock 100
    const auto   *b          = reinterpret_cast<const mh_map_object_building *>(
        lv.base[mh::state::RID_BUILDINGS] +
        ((player & 0xf) * per_player + (building_index & 0xffff)) * sizeof(mh_map_object_building));
    const auto *cfg = reinterpret_cast<const mh_cfg_final_struct_Building *>(
        lv.base[mh::state::RID_BUILDING] + (uint32_t)b->building_id * sizeof(mh_cfg_final_struct_Building));
    const uint16_t idle = (uint16_t)(cfg->state_transition_ids[1] & 0xffff);
    ++g_routed;
    // (unit_id = building index, player = owner|kind, op_code -> param0 = idle, arg -> order_code = idle):
    // the same shape llm_strat_bldg_order_restart_construction issues for "Sell" (0x6b/0x6b).
    mh::call::llm_strat_order_dispatch((uint16_t)building_index, (uint32_t)((player & 0xffff) | KIND_BUILDING),
                                       idle, idle);
    char m[160];
    wsprintfA(m, "; D28: cancel-task Yes routed as order (bldg %u state 0x%02X -> idle 0x%04X) instead of the local call\n",
              (unsigned)building_index, (unsigned)b->state, (unsigned)idle);
    seam_log(m);
}

// clang-format off
__declspec(naked) void cancel_task_thunk() {
    __asm {
        pushad                       // the caller keeps EBX/ECX/EDX/ESI/EDI across the __watcall (it pushed them)
        push edx                     // building_index
        push eax                     // player
        call cancel_task             // __cdecl(uint32_t, uint32_t)
        add  esp, 8
        popad
        ret                          // back to 0x004c7065: CALL llm_ui_dlg_building_info_close_cb
    }
}
// clang-format on

} // namespace

extern "C" int MH_CancelTask_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_spliced) return 1;
    g_by_order           = GetPrivateProfileIntA("net", "cancel_task_order", 1, g_ini) != 0;
    const uintptr_t site = mh::addr::canceltask_finish_call_site;
    if (mh::hook::promoted_owner_of(site)) {
        seam_log("; D28: cancel-task splice DISPLACED: the building dialog callback is promoted this run -- our "
                 "body must carry the order routing (see the [interlock] line)\n");
        return 0;
    }
    uint8_t repl[5];
    repl[0]     = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)&cancel_task_thunk - (site + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    g_spliced = mh::hook::patch_bytes_guarded(site, FINISH_CALL_EXPECT, repl, 5);
    char m[240];
    // clang-format off
    const char *fmt = g_spliced
        ? "; D28: cancel-task Yes spliced at %08X -- in a lockstep match the cancel is a replicated building order%s\n"
        : "; D28: cancel-task Yes NOT spliced at %08X -- bytes differ from the expected CALL; retail local call kept\n";
    // clang-format on
    wsprintfA(m, fmt, (unsigned)site, g_by_order ? "" : " -- ROUTING OFF ([net] cancel_task_order=0): retail local call, the reproduction arm");
    seam_log(m);
    return g_spliced ? 1 : 0;
}
