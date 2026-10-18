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
// +0xaa path_slot_id, +0xab path_blocked_retry_count; path buffers 0x00aee8c0 + player*0xea60 + slot*0x258 + cursor*2 =
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
// own saved/hashed fields that retail already writes at this site. If libmh promotes move_walker the site is DISPLACED and the
// keys log "NOT ACTIVE" (no libmh twin in this phase).
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
#include "en_guard.h" // EN-only build gate

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
    return s == 1 ? "carried" : (s == -1 ? "the splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns move_walker; no twin)" : "not installed"));
}

} // namespace

// Runtime gates (read by the stub). Retail values: limit 201, keep 0, cooldown 0.
extern "C" {
int    g_bk_wait_limit = 201; // move_block_wait_ms -> retry limit
int    g_bk_keep       = 0;   // move_keep_sidestep
int    g_bk_cd_on      = 0;   // move_repath_cooldown_ms != 0
double g_bk_cd_s       = 0.0; // the cooldown in seconds
}

// The decision: returns the retail continuation to jump to. Called with the detour result (EAX after CALL 0x0049a23d).
static uintptr_t __cdecl bk_decide(int detour) {
    uint8_t *u     = *reinterpret_cast<uint8_t **>(0x00e162e0u);
    uint8_t  retry = u[0xab];
    if (detour) {
        if (!g_bk_keep || retry >= 0xc0) return TO_REPLAN;
        const uint16_t order = *reinterpret_cast<uint16_t *>(u + 0x04);
        if (order == 0x1a || order == 0x1b || order == 0x1c || order == 0x10 || order == 0x33) return TO_REPLAN;
        const uint8_t slot = u[0xaa];
        if (slot >= 100) return TO_REPLAN;
        const int32_t cur = *reinterpret_cast<int32_t *>(u + 0xa6);
        if (cur < 0 || cur + 2 >= 300) return TO_REPLAN;
        const uint32_t player = *reinterpret_cast<const volatile uint16_t *>(0x00e58144u);
        uint8_t       *pb     = reinterpret_cast<uint8_t *>(0x00aee8c0u + player * 0xea60u + slot * 0x258u);
        const uint8_t  leg    = u[0x2c]; // facing_target == the blocked leg's heading (move_walker only steps when they match)
        const uint8_t  h      = pb[cur * 2];
        if (h == 0 || h == leg) return TO_REPLAN;                               // no sidestep of OUR path was spliced (blocked by terrain/building, or the occupant got it)
        const int k           = ((static_cast<int>(leg) - 1) % 6 == 0) ? 1 : 2; // cardinal: keep the probe step only
        pb[(cur + k) * 2]     = 0;                                              // heading 0 = path end -> retail replan from there
        pb[(cur + k) * 2 + 1] = 0;
        u[0xab]               = static_cast<uint8_t>(retry + 0x40);
        return TO_DONE;
    }
    if (static_cast<int>(retry) < g_bk_wait_limit) {
        u[0xab] = static_cast<uint8_t>(retry + 1);
        return TO_WAIT;
    }
    return TO_REPLAN;
}

// clang-format off
__declspec(naked) static void mv_blocked_stub() {
    __asm {
        add   esp, 4                             // never return to the NOP pad
        pushad
        push  eax
        call  bk_decide
        add   esp, 4
        mov   dword ptr [esp + 28], eax          // the target rides out in the saved EAX slot
        cmp   eax, 0x0047d533                    // TO_REPLAN
        jne   bk_go
        cmp   g_bk_cd_on, 0
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

void install_move_proto_blocked() {
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
    char b[200];
    wsprintfA(b, "; move prototype splice (mods:M1X blocked-unit protocol): %s\n", bk_state_name(g_bk_state));
    seam_log(b);
    install_move_proto_nudge(); // mods:M1N (seams/move_proto_nudge.cpp): the sibling splice on the CALL just before this block
}

static int bk_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Each setter logs one line when its key is non-retail: "active" with what it does, or "NOT ACTIVE" with the reason.
static bool bk_refuse(const char *key, int v) {
    if (v == 0 || g_bk_state == 1) return false;
    char b[260];
    wsprintfA(b, "; move prototype [net] %s=%d NOT ACTIVE (mods:M1X): %s\n", key, v, bk_state_name(g_bk_state));
    seam_log(b);
    return true;
}

void mh_set_move_block_wait_ms(int ms) {
    ms              = bk_clamp(ms, 0, 10050);
    g_bk_wait_limit = 201;
    if (ms == 0 || bk_refuse("move_block_wait_ms", ms)) return;
    g_bk_wait_limit = bk_clamp(ms / 50, 1, 201);
    char b[260];
    wsprintfA(b, "; move prototype [net] move_block_wait_ms=%d active (mods:M1X): a unit blocked by a walking/regrouping friend replans after %d retries (retail 201)\n", ms,
              g_bk_wait_limit);
    seam_log(b);
}

void mh_set_move_keep_sidestep(int on) {
    on        = on != 0;
    g_bk_keep = 0;
    if (!on || bk_refuse("move_keep_sidestep", on)) return;
    g_bk_keep = 1;
    seam_log("; move prototype [net] move_keep_sidestep=1 active (mods:M1X): a spliced sidestep is walked (path ends after it), then the unit replans; max 3 "
             "sidesteps without a step taken\n");
}

void mh_set_move_repath_cooldown_ms(int ms) {
    ms         = bk_clamp(ms, 0, 10000);
    g_bk_cd_on = 0;
    g_bk_cd_s  = 0.0;
    if (ms == 0 || bk_refuse("move_repath_cooldown_ms", ms)) return;
    g_bk_cd_s  = static_cast<double>(ms) / 1000.0;
    g_bk_cd_on = 1;
    char b[260];
    wsprintfA(b, "; move prototype [net] move_repath_cooldown_ms=%d active (mods:M1X): a blocked unit sent to replan sleeps %d ms first\n", ms, ms);
    seam_log(b);
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

int mh_set_move_attack_heading_fix(int on) {
    on = on != 0;
    char b[260];
    if (on == g_ah_on) {
        if (on) {
            wsprintfA(b, "; move prototype [net] move_attack_heading_fix=1 active (mods:M1X): a single unit's attack path records the heading it "
                         "actually stepped\n");
            seam_log(b);
        }
        return 1;
    }
    if (mh::hook::promoted_owner_of(ADDR_MV_ATK_HEADING)) {
        if (on) seam_log("; move prototype [net] move_attack_heading_fix=1 NOT ACTIVE (mods:M1X): DISPLACED -- libmh owns "
                         "llm_strat_pathfind_build_steps (its twin carries the key)\n");
        return 0;
    }
    bool ok;
    if (on) ok = mh::hook::patch_bytes_guarded(ADDR_MV_ATK_HEADING, MV_ATK_HEADING_RETAIL, MV_ATK_HEADING_FIXED, 3);
    else ok = mh::hook::patch_bytes_guarded(ADDR_MV_ATK_HEADING, MV_ATK_HEADING_FIXED, MV_ATK_HEADING_RETAIL, 3);
    if (ok) g_ah_on = on;
    if (on) {
        if (ok)
            wsprintfA(b, "; move prototype [net] move_attack_heading_fix=1 active (mods:M1X): a single unit's attack path records the heading it "
                         "actually stepped\n");
        else
            wsprintfA(b, "; move prototype [net] move_attack_heading_fix=1 NOT ACTIVE (mods:M1X): the byte at 0x0041ec1d MISMATCHED\n");
        seam_log(b);
    }
    return ok ? 1 : 0;
}
