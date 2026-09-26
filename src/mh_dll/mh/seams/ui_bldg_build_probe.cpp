// seams/ui_bldg_build_probe.cpp -- mp:D35: the HUD build-click's affordability probe charges
// nothing (the mh.dll port of libmh's mp:D25 fix). Mechanism, register contract and the reasons
// for the scope are in include/mh_buildprobe_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "include/mh_buildprobe_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::buildprobe_pay_call_site / buildprobe_grant_call_site
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_BUILDING / RID_PROGRESS / RID_PLAYER_RESOURCES
#include "addr/mh_structs.gen.h" // mh::game::mh_cfg_final_struct_Building / mh_game_progress
#include "config/config.h"       // mh::config::mode() -- mode=original keeps the retail probe
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate

namespace {

constexpr int CALL_LEN         = 5;
constexpr int RESOURCE_SLOTS   = 7;   // cfg_building.resource[7] (CFG_RESOURCE_SLOTS)
constexpr int INVENTIONS       = 300; // progress[8][300], 0x384 bytes per player
constexpr int RESOURCES_PER_PL = 10;  // player_resources int[8][10], 0x28 bytes per player

// The exact 5 bytes at each site (EN v406 disassembly of llm_strat_bldg_try_begin_placement).
const uint8_t PAY_CALL_EXPECT[CALL_LEN]   = {0xE8, 0x56, 0xA2, 0x04, 0x00}; // CALL 0x00492eb1 from 0x00448c5b
const uint8_t GRANT_CALL_EXPECT[CALL_LEN] = {0xE8, 0x2A, 0xAB, 0x04, 0x00}; // CALL 0x004937bf from 0x00448c95
const uint8_t NOPS[CALL_LEN]              = {0x90, 0x90, 0x90, 0x90, 0x90};

bool g_armed = false;

// llm_bldg_pay_build_cost's PASS 1 (0x00492ecb-0x00492f8e), read-only. Mirrors libmh's
// bldg_can_afford_build_cost instruction for instruction, including the id-read-before-bound-check
// order: resource[7].id is read (into build_time_2's low bytes, inside the record) before `i < 7`
// ends the walk, which is provably inert -- i reaches 7 only when slots 0..6 were all defined, and
// the bound check then exits whatever the id holds.
int32_t can_afford_build_cost(uint32_t player, int32_t building_type_id) {
    using mh::game::mh_cfg_final_struct_Building;
    using mh::game::mh_game_progress;
    const auto    &lv = mh::state::live();
    const uint32_t p  = player & 0xffffu; // MOVZX word at every use site (0x00492edf / 0x00492f43)

    const auto *cb = reinterpret_cast<const mh_cfg_final_struct_Building *>(
        lv.base[mh::state::RID_BUILDING] + (uint32_t)building_type_id * sizeof(mh_cfg_final_struct_Building));
    const auto *progress = reinterpret_cast<const mh_game_progress *>(lv.base[mh::state::RID_PROGRESS]);
    const auto *stock    = reinterpret_cast<const int32_t *>(lv.base[mh::state::RID_PLAYER_RESOURCES]);

    // 0x00492ece-0x00492efb: the prerequisite invention gate, before any resource is looked at.
    if (progress[p * INVENTIONS + cb->invention].available == 0) return 0x13;

    // 0x00492f0e-0x00492f80: every slot, no short-circuit; first shortage id+0x89, any later 0x89.
    int32_t error = 0;
    for (int32_t i = 0;; ++i) {
        const uint32_t id = cb->resource[i].id;                       // 0x00492f1d
        if (id == 0) break;                                           // 0x00492f2a: UNDEFINED
        if (!(i < RESOURCE_SLOTS)) break;                             // 0x00492f30: bound check, evaluated second
        if (stock[p * RESOURCES_PER_PL + id] < cb->resource[i].val) { // 0x00492f52-0x00492f5e (signed JGE)
            error = (error == 0) ? (int32_t)(id + 0x89u) : 0x89;
        }
    }
    return error; // 0 = affordable; pass 2 (game_SpendResource per slot) is deliberately absent
}

// __cdecl bridge for the thunk below. One line per click (a human's click rate): the evidence that
// the click reached the charge-free probe, read by tools/check_build_probe.py.
int32_t probe(uint32_t player, int32_t building_type_id) {
    const int32_t verdict = can_afford_build_cost(player, building_type_id);
    char          m[160];
    wsprintfA(m, "; D35: build-click probe player %u building %d -> verdict 0x%X, nothing charged\n",
              (unsigned)(player & 0xffffu), (int)building_type_id, (unsigned)verdict);
    seam_log(m);
    return verdict;
}

// clang-format off
// Replaces `CALL llm_bldg_pay_build_cost` (__watcall: EAX = player, EDX = building type, result in
// EAX). pushad/popad keeps every register but EAX, whose saved slot ([esp+28] after pushad) takes the
// verdict -- the caller stores EAX to [EBP-0x20] and tests it, exactly as after the original call.
__declspec(naked) void probe_thunk() {
    __asm {
        pushad
        push edx                     // building_type_id
        push eax                     // player
        call probe                   // __cdecl(uint32_t, int32_t) -> int32_t
        add  esp, 8
        mov  [esp + 28], eax         // pushad's EAX slot
        popad
        ret                          // back to 0x00448c5b: MOV [EBP-0x20],EAX
    }
}
// clang-format on

bool bytes_at(uintptr_t site, const uint8_t *expect) {
    return std::memcmp(reinterpret_cast<const void *>(site), expect, CALL_LEN) == 0;
}

} // namespace

extern "C" int MH_BuildProbe_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    const uintptr_t pay   = mh::addr::buildprobe_pay_call_site;
    const uintptr_t grant = mh::addr::buildprobe_grant_call_site;
    if (GetPrivateProfileIntA("net", "build_probe_free", 1, g_ini) == 0) {
        seam_log("; D35: build-click probe KEPT ([net] build_probe_free=0): the click pays and re-grants, booking "
                 "+cost into resource_spent on this peer only -- the reproduction arm\n");
        return 0;
    }
    if (mh::config::mode() == mh::config::mode_t::original) {
        seam_log("; D35: build-click probe KEPT ([config] mode=original): the original body pays and re-grants\n");
        return 0;
    }
    if (mh::hook::promoted_owner_of(pay)) {
        seam_log("; D35: build-click probe DISPLACED: llm_strat_bldg_try_begin_placement is promoted this run -- "
                 "its body probes with bldg_can_afford_build_cost (see the [interlock] line)\n");
        return 0;
    }
    char m[240];
    // BOTH OR NEITHER: a splice without the NOP would grant resources on every click, a NOP without
    // the splice would charge them -- so both sites are checked before either is written, and a
    // second write that fails rolls the first back.
    if (!bytes_at(pay, PAY_CALL_EXPECT) || !bytes_at(grant, GRANT_CALL_EXPECT)) {
        wsprintfA(m, "; D35: build-click probe NOT patched at %08X/%08X -- bytes differ from the expected CALLs; "
                     "retail pay + re-grant kept\n",
                  (unsigned)pay, (unsigned)grant);
        seam_log(m);
        return 0;
    }
    uint8_t repl[CALL_LEN];
    repl[0]     = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)&probe_thunk - (pay + CALL_LEN));
    memcpy(repl + 1, &rel, sizeof(rel));
    bool ok = mh::hook::patch_bytes_guarded(pay, PAY_CALL_EXPECT, repl, CALL_LEN);
    if (ok && !mh::hook::patch_bytes_guarded(grant, GRANT_CALL_EXPECT, NOPS, CALL_LEN)) {
        mh::hook::patch_bytes_guarded(pay, repl, PAY_CALL_EXPECT, CALL_LEN); // roll the splice back
        ok = false;
    }
    g_armed = ok;
    // clang-format off
    const char *fmt = g_armed
        ? "; D35: build-click probe charge-free at %08X (pay -> can_afford) + %08X (re-grant NOPed) -- the HUD click books nothing into resource_spent\n"
        : "; D35: build-click probe NOT patched at %08X/%08X -- a write was refused; retail pay + re-grant kept\n";
    // clang-format on
    wsprintfA(m, fmt, (unsigned)pay, (unsigned)grant);
    seam_log(m);
    return g_armed ? 1 : 0;
}
