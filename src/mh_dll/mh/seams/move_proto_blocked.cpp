//
// seams/move_proto_blocked.cpp -- mods:M1X lane B: the BLOCKED-UNIT protocol prototype keys (movement plan 3b + 3c + 3d).
//
// One call-splice, ALWAYS installed (install_move_proto in net_lockstep.cpp calls install_move_proto_blocked), runtime-gated by
// three globals the gameplay setters flip; every key at its retail value = the retail bytes' own behaviour. Host-set ([net],
// F_HOSTSYNC, C_DEV): the sim changes, so every peer must run the same values.
//
// THE SITE: llm_strat_unit_state_move_walker 0x0047c903, the "destination tile is blocked" block. After
// `CALL 0x0049a23d` (llm_strat_path_step_check_and_request_detour: 0 = wait for the occupant, 1 = a detour was requested), retail
// runs (0x0047d50d..0x0047d532, 38 bytes):
//     if (eax) retry = 0xc9;  old = retry++;  if (old <= 0xc8) goto 0x0047d563 (activity_clock += 0.05)
//     else goto 0x0047d533 (retry = 0, state 0xa = group_marshal / replan, notify 1)
// and both paths end at 0x0047d574 (tick budget = 0). The 38 bytes become CALL mv_blocked_stub + 33 NOP; the stub never returns to
// the NOPs: it jumps to one of the three retail continuations 0x0047d533 / 0x0047d563 / 0x0047d574, each of which reloads every
// register it uses (EAX from CUR_UNIT, EBX/EDX set before the notify CALL). pushad/popad keep the rest.
//
// The keys (unit record offsets: +0x04 order, +0x08 activity_clock (double), +0x2c facing_target, +0xa6 path_cursor,
// +0xaa path_slot_id, +0xab path_blocked_retry_count; path buffers (relocated, mh::caps::path::waypoint) player + slot*0x258 + cursor*2 =
// {heading, run_length}; CUR_UNIT 0x00e162e0, CUR_PLAYER word 0x00e58144):
//   move_block_wait_ms (3b)       the wait-for-a-walking-friend budget: the retry limit becomes clamp(ms / 50, 1, 201) instead of
//                                 201 (each retry is +0.05 s on the activity clock), then the retail replan runs. 0 = retail.
//   move_keep_sidestep (3c)       when the detour request spliced a sidestep into THIS unit's own path (detected: the heading at the
//                                 unit's path cursor no longer equals facing_target, which is the blocked leg's heading -- the
//                                 splice writes heading +-3/+-6 there), do NOT force the replan. The path is ended right after
//                                 the sidestep (heading 0 sentinel: after the probe step for a cardinal leg, whose 2-step sidestep
//                                 nets zero; after both steps for a diagonal leg, whose pair nets one orthogonal tile), so the unit
//                                 walks the sidestep and then the retail path-end rule replans it from the new tile. Retail would
//                                 throw the spliced sidestep away by replanning first, and the kept rest of the path would be one
//                                 tile off its legs anyway (the splice also shortens the blocked leg by one). Bounded without new
//                                 state: a keep adds 0x40 to the retry byte (reset to 0 only by a step actually taken) and is refused
//                                 once the byte is >= 0xc0, so at most 3 sidesteps without progress before the retail replan. Not
//                                 applied to orders whose path end SETTLES instead of replanning (attack 0x1a/0x1b/0x1c, patrol
//                                 0x10, squad merge 0x33) -- they keep the retail replan.
//   move_repath_cooldown_ms (3d)  when this block sends the unit to replan (state 0xa, wait exhausted or detour forced), its
//                                 activity_clock is advanced by ms/1000 s (x87 FLD/FADD/FSTP, the same idiom retail uses for its
//                                 +0.05 and +2.0), so the marshal does not run for that long. 0 = retail.
//
// Determinism: integer decisions on sim state only (unit record, path buffer), table order, no clock/rand; the one double add is a
// constant computed once in the setter. No new persistent state: the retry byte, activity_clock and the path buffer are the unit's
// own saved/hashed fields that retail already writes at this site. If libmh owns move_walker the site is DISPLACED and the libmh twin
// (sim/move/blocked.h, sim_unit_state_move_walker.cpp) carries the keys; the late carrier report (move_carrier.cpp) says so.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "seams/net_internal.h"
#include "seams/move_carrier.h" // mods:M1H: site states, requested values, stub hit counters
#include "en_guard.h"           // EN-only build gate
#include "caps/path_pool.h"     // PT-CAP9-U4: the relocated path pool
#include "sim/move/blocked.h"   // mods:M1H lane C: the decision, shared with the libmh twin (sim_unit_state_move_walker.cpp)

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

constexpr uintptr_t ADDR_MV_BLOCKED       = 0x0047d50d; // llm_strat_unit_state_move_walker: the blocked-step retry block
const uint8_t       MV_BLOCKED_EXPECT[38] = {
    0x85, 0xC0,                               // TEST EAX,EAX
    0x74, 0x0C,                               // JZ 0x0047d51d
    0xA1, 0xE0, 0x62, 0xE1, 0x00,             // MOV EAX,[CUR_UNIT]
    0xC6, 0x80, 0xAB, 0x00, 0x00, 0x00, 0xC9, // MOV byte [EAX+0xab],0xc9
    0xA1, 0xE0, 0x62, 0xE1, 0x00,             // MOV EAX,[CUR_UNIT]
    0x8A, 0x90, 0xAB, 0x00, 0x00, 0x00,       // MOV DL,[EAX+0xab]
    0xFE, 0x80, 0xAB, 0x00, 0x00, 0x00,       // INC byte [EAX+0xab]
    0x80, 0xFA, 0xC8,                         // CMP DL,0xc8
    0x76, 0x30,                               // JBE 0x0047d563
};
constexpr uintptr_t TO_REPLAN = 0x0047d533; // retry = 0, set_state(0xa), notify(1), budget = 0
constexpr uintptr_t TO_WAIT   = 0x0047d563; // activity_clock += 0.05, budget = 0
constexpr uintptr_t TO_DONE   = 0x0047d574; // budget = 0

int g_bk_state = 0; // 1 carried, -1 MISMATCHED, -2 DISPLACED, 0 not installed

const char *bk_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "the splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns move_walker; its twin carries the keys, see the carrier line)" : "not installed"));
}

} // namespace

// Runtime gates (read by the stub). Retail values: limit 201, keep 0, cooldown 0.
extern "C" {
int        g_bk_wait_limit = 201;   // move_block_wait_ms -> retry limit
int        g_bk_keep       = 0;     // move_keep_sidestep
int        g_bk_cd_on      = 0;     // move_repath_cooldown_ms != 0
double     g_bk_cd_s       = 0.0;   // the cooldown in seconds
int        g_bk_reset      = 0;     // move_block_reset_moving (mods:M1DT)
int        g_bk_cap        = 0;     // move_replan_detour_cap percent (mods:M1DT); 0 = off
int        g_bk_cd_apply   = 0;     // this decision's cooldown applies (cd_on and the unit is not exempt, mods:M1BE)
extern int g_eq_on;                 // move_enter_queue in force (move_proto_nudge.cpp)
uintptr_t  g_cap_ret  = 0x00482cf5; // cap_stub: continue the retail good-path branch after the displaced MOVZX
uintptr_t  g_cap_skip = 0x004828ab; // cap_stub: route refused, next member
double     g_cap_two  = 2.0;        // the clock advance of a refused route (retail's no-path retry period, _DAT_0050145a)
}

// The decision: returns the retail continuation to jump to. Called with the detour result (EAX after CALL 0x0049a23d). The rule itself
// is mh::sim::mvc::blocked_decide (sim/move/blocked.h), the same code the libmh twin runs.
void      mv_blocker_info(uint32_t player, uint32_t x, uint32_t y, mh::sim::mvc::blocker_info *out); // move_proto_nudge.cpp
uintptr_t mv_cap_refuse(uint32_t player, uint32_t idx, int cap_pct);                                 // move_proto_nudge.cpp

static uintptr_t __cdecl bk_decide(int detour, uint32_t nx, uint32_t ny) {
    mh::sim::mvc::blocked_params p;
    p.reset_moving = g_bk_reset != 0;
    p.detour_cap   = g_bk_cap;
    p.wait_limit   = g_bk_wait_limit;
    p.keep         = g_bk_keep != 0;
    p.cd_on        = g_bk_cd_on != 0;
    p.cd_s         = g_bk_cd_s;
    p.enter_queue  = g_eq_on != 0;
    if (p.active()) mv_hit(MVH_BLOCKED); // the decision path runs (a key of this site is non-retail)
    mh::game::mh_map_object_unit *u   = *reinterpret_cast<mh::game::mh_map_object_unit **>(0x00e162e0u);
    g_bk_cd_apply                     = (p.cd_on && !mh::sim::mvc::blocked_enter_exempt(p, *u)) ? 1 : 0; // mods:M1BE: an entering unit has no cooldown
    const uint32_t             player = *reinterpret_cast<const volatile uint16_t *>(0x00e58144u);
    uint8_t                   *pbase  = mh::caps::path::waypoint(player, 0, 0); // this player's path slots (the relocated pool), 0x258 bytes each
    mh::sim::mvc::blocker_info bi;
    if (p.reset_moving) mv_blocker_info(player, nx, ny, &bi);
    switch (mh::sim::mvc::blocked_decide(*u, detour != 0, p, [pbase](uint32_t slot, int entry) -> mh::game::mh_llm_strat_path_waypoint & { return *reinterpret_cast<mh::game::mh_llm_strat_path_waypoint *>(pbase + slot * 0x258u + static_cast<uint32_t>(entry) * 2u); }, bi)) {
        case mh::sim::mvc::blocked_next::wait: return TO_WAIT;
        case mh::sim::mvc::blocked_next::done: return TO_DONE;
        default: return TO_REPLAN;
    }
}

// clang-format off
__declspec(naked) static void mv_blocked_stub() {
    __asm {
        add   esp, 4                             // never return to the NOP pad
        pushad
        push  dword ptr [ebp - 0x20]             // the destination tile (mods:M1DT: the blocker is read off it): y, x
        push  dword ptr [ebp - 0x24]
        push  eax
        call  bk_decide
        add   esp, 12
        mov   dword ptr [esp + 28], eax          // the target rides out in the saved EAX slot
        cmp   eax, 0x0047d533                    // TO_REPLAN
        jne   bk_go
        cmp   g_bk_cd_apply, 0
        je    bk_go
        mov   eax, dword ptr ds:[0x00e162e0]     // CUR_UNIT
        fld   qword ptr [eax + 8]                // activity_clock += cooldown (retail idiom)
        fadd  qword ptr g_bk_cd_s
        fstp  qword ptr [eax + 8]
    bk_go:
        popad
        jmp   eax
    }
}
// clang-format on

// ---- mods:M1DT fix 4b: the marshal good-path check --------------------------------------------------------------------------
// llm_strat_unit_state_group_marshal 0x00482451, per-member loop: the good-path branch opens with `MOVZX EAX,word [CUR_PLAYER]`
// (0F B7 05 44 81 E5 00, 7 bytes at 0x00482cee, the sibling of the two no-path sites in move_proto_nudge.cpp). The stub asks
// cap_decide(player, [EBP-0x48] = the member's unit index): non-zero = the route is discarded (slot freed, clock += 2.0 here in x87 like
// retail, jump to the loop's per-member continue 0x004828ab), 0 = run the displaced MOVZX and carry on.
namespace {
constexpr uintptr_t ADDR_CAP_SITE = 0x00482cee;
const uint8_t       CAP_EXPECT[7] = {0x0F, 0xB7, 0x05, 0x44, 0x81, 0xE5, 0x00};
int                 g_cap_state   = 0; // 1 carried, -1 MISMATCHED, -2 DISPLACED, 0 not installed
} // namespace

static uintptr_t __cdecl cap_decide(uint32_t player, uint32_t idx) {
    if (g_bk_cap <= 0) return 0;
    return mv_cap_refuse(player, idx, g_bk_cap);
}

// clang-format off
__declspec(naked) static void cap_stub() {
    __asm {
        pushad
        push  dword ptr [ebp - 0x48]            // the member's unit index (the marshal's loop local)
        movzx eax, word ptr ds:[0x00e58144]     // _G_LLM_STRAT_CUR_PLAYER
        push  eax
        call  cap_decide
        add   esp, 8
        test  eax, eax
        jnz   cap_refused
        popad
        movzx eax, word ptr ds:[0x00e58144]     // the displaced instruction
        jmp   dword ptr [g_cap_ret]
    cap_refused:
        fld   qword ptr [eax + 8]               // activity_clock += 2.0 (retail idiom)
        fadd  qword ptr g_cap_two
        fstp  qword ptr [eax + 8]
        popad
        jmp   dword ptr [g_cap_skip]
    }
}
// clang-format on

int mv_bk_state() { return g_bk_state; }

void install_move_proto_blocked() {
    if (mh::hook::promoted_owner_of(ADDR_CAP_SITE)) {
        g_cap_state = -2;
    } else {
        uint8_t       repl[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        const int32_t rel     = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&cap_stub) - (ADDR_CAP_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        g_cap_state = mh::hook::patch_bytes_guarded(ADDR_CAP_SITE, CAP_EXPECT, repl, 7) ? 1 : -1;
    }
    mh::seams::mv::note_site(mh::seams::mv::SITE_CAPOK, g_cap_state);
    if (mh::hook::promoted_owner_of(ADDR_MV_BLOCKED)) {
        g_bk_state = -2;
    } else {
        uint8_t       repl[38];
        const int32_t rel = (int32_t)((uintptr_t)&mv_blocked_stub - (ADDR_MV_BLOCKED + 5));
        repl[0]           = 0xE8;
        memcpy(repl + 1, &rel, sizeof(rel));
        for (int i = 5; i < 38; ++i) repl[i] = 0x90;
        g_bk_state = mh::hook::patch_bytes_guarded(ADDR_MV_BLOCKED, MV_BLOCKED_EXPECT, repl, 38) ? 1 : -1;
    }
    mh::seams::mv::note_site(mh::seams::mv::SITE_BLOCKED, g_bk_state);
    char b[200];
    wsprintfA(b, "; move prototype splice (mods:M1X blocked-unit protocol): %s\n", bk_state_name(g_bk_state));
    seam_log(b);
    install_move_proto_respond(); // mods:M1G 4c (seams/move_proto_respond.cpp): reverse_now + turn_rate
    install_move_proto_nudge();   // mods:M1N (seams/move_proto_nudge.cpp): the sibling splice on the CALL just before this block
    install_door_hold();          // mods:M1BE (seams/door_hold.cpp): the storage door state handlers
}

// The setters set the stub's gate and tell the carrier report (move_carrier.cpp) the requested value; the report -- late, once the
// unit-state table is filled -- is the one line per key ("retail splice" / "libmh twin" / "NOT ACTIVE: ...").
void mh_set_move_block_wait_ms(int ms) {
    ms              = mh::sim::mvc::blocked_clamp(ms, 0, 10050);
    g_bk_wait_limit = 201;
    mh::seams::mv::request(mh::seams::mv::KEY_BLOCK_WAIT_MS, ms, 0);
    if (ms == 0 || g_bk_state != 1) return;
    g_bk_wait_limit = mh::sim::mvc::make_blocked_params(ms, false, 0).wait_limit;
}

// mods:M1DT fix 4a: the wait budget restarts when the unit in the way changes to a walking friend.
void mh_set_move_block_reset_moving(int on) {
    on         = on != 0;
    g_bk_reset = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_BLOCK_RESET_MOVING, on, 0);
    if (!on || g_bk_state != 1) return;
    g_bk_reset = 1;
}

// mods:M1DT fix 4b: a block-wait replan longer than pct % of the straight distance is discarded (at most 3 times in a row).
void mh_set_move_replan_detour_cap(int pct) {
    pct      = mh::sim::mvc::blocked_clamp(pct, 0, 1000);
    g_bk_cap = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_REPLAN_DETOUR_CAP, pct, 0);
    mh::seams::mv::note_site(mh::seams::mv::SITE_CAPOK, g_cap_state);
    if (pct == 0 || g_bk_state != 1 || g_cap_state != 1) return;
    g_bk_cap = pct;
}

void mh_set_move_keep_sidestep(int on) {
    on        = on != 0;
    g_bk_keep = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_KEEP_SIDESTEP, on, 0);
    if (!on || g_bk_state != 1) return;
    g_bk_keep = 1;
}

void mh_set_move_repath_cooldown_ms(int ms) {
    ms         = mh::sim::mvc::blocked_clamp(ms, 0, 10000);
    g_bk_cd_on = 0;
    g_bk_cd_s  = 0.0;
    mh::seams::mv::request(mh::seams::mv::KEY_REPATH_COOLDOWN_MS, ms, 0);
    if (ms == 0 || g_bk_state != 1) return;
    const mh::sim::mvc::blocked_params bp = mh::sim::mvc::make_blocked_params(0, false, ms);
    g_bk_cd_s                             = bp.cd_s;
    g_bk_cd_on                            = bp.cd_on ? 1 : 0;
}

// ---- [net] move_attack_heading_fix (mods:M1X, 2026-10-14; tmp/m1/attack_stall.md) -----------------------------------------
// llm_strat_pathfind_build_steps 0x0041e836 is the greedy path a SINGLE-unit attack wave uses (group_move_order_pathfind step
// 4, mode 0, one member). When the direct step is blocked it steps along the -3/+3 turn candidate ([EBP-0x1c]) but records
// the ORIGINAL heading: `MOV AL,[EBP-0x20]` at 0x0041ec1d. The walker then follows the blocked heading into the building or
// cliff, the marshal replans the same path, and after 4 x 2 s the unit stops (STOP_TO_DEFAULT); passive engage re-commits
// it ~7 s later and the loop repeats. With the key on the instruction reads [EBP-0x1c] (one byte, E0 -> E4 at 0x0041ec1f),
// so the path records the heading it actually took. RUNTIME-FLIPPED by the setter (off = the retail byte), guarded by the
// expected bytes; the libmh twin (sim_path_group_steps.cpp, gated by reimpl_fixes::move_attack_heading_fix) carries it
// when libmh promotes the function. Integer only, no new state.
namespace {
constexpr uintptr_t ADDR_MV_ATK_HEADING      = 0x0041ec1d; // MOV AL,[EBP-0x20] inside llm_strat_pathfind_build_steps
const uint8_t       MV_ATK_HEADING_RETAIL[3] = {0x8A, 0x45, 0xE0};
const uint8_t       MV_ATK_HEADING_FIXED[3]  = {0x8A, 0x45, 0xE4};
int                 g_ah_on                  = 0; // the byte currently in place: 0 retail, 1 fixed
} // namespace

// A runtime-flipped byte, not a stub: no hit counter. The carrier report learns the site state here (the byte is only touched when the key
// is asked for): 1 flipped / already in place, -1 the bytes MISMATCHED, -2 the function's entry is claimed.
int mh_set_move_attack_heading_fix(int on) {
    on = on != 0;
    mh::seams::mv::request(mh::seams::mv::KEY_ATTACK_HEADING, on, 0);
    if (on == g_ah_on) {
        if (on) mh::seams::mv::note_site(mh::seams::mv::SITE_ATK_HEADING, 1);
        return 1;
    }
    if (mh::hook::promoted_owner_of(ADDR_MV_ATK_HEADING)) {
        mh::seams::mv::note_site(mh::seams::mv::SITE_ATK_HEADING, -2);
        return 0;
    }
    bool ok;
    if (on) ok = mh::hook::patch_bytes_guarded(ADDR_MV_ATK_HEADING, MV_ATK_HEADING_RETAIL, MV_ATK_HEADING_FIXED, 3);
    else ok = mh::hook::patch_bytes_guarded(ADDR_MV_ATK_HEADING, MV_ATK_HEADING_FIXED, MV_ATK_HEADING_RETAIL, 3);
    if (ok) g_ah_on = on;
    if (on) mh::seams::mv::note_site(mh::seams::mv::SITE_ATK_HEADING, ok ? 1 : -1);
    return ok ? 1 : 0;
}
