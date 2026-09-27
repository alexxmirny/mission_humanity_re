// seams/sim_pioneer_refill.cpp -- mp:D37a: in a lockstep session a pioneer re-landing refills the
// starting stock for AI players only (retail: every player but the local side). Mechanism, register
// contract and scope are in include/mh_relanding_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "include/mh_relanding_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::pioneer_refill_cmp_site
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_PLAYERSIDE / RID_GAME_SESSION_MODE / RID_STRAT_PLAYERS
#include "addr/mh_structs.gen.h" // mh::game::mh_llm_strat_player_profile
#include "config/config.h"       // mh::config::mode() -- mode=original keeps the retail compare
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate

namespace {

constexpr int      SITE_LEN            = 13;
constexpr int32_t  SESSION_MP_LOCKSTEP = 3;
constexpr uint32_t STATUS_AI           = 0x8u; // llm_strat_player_profile.status_flags bit 3
constexpr uint32_t SKIP_TARGET         = 0x00479a3eu;

// EN v406 disassembly of llm_strat_bldg_completion_dispatch at 0x004798f1.
const uint8_t SITE_EXPECT[SITE_LEN] = {0x66, 0x3B, 0x05, 0x54, 0x83, 0xE5, 0x00, // CMP AX,[PlayerSide]
                                       0x0F, 0x84, 0x40, 0x01, 0x00, 0x00};      // JZ 0x00479a3e

bool g_armed = false;

// 1 = credit the template stock (fall through to 0x004798fe), 0 = skip (JZ 0x00479a3e). Reached only
// on a RE-landing (mother_established != 0).
int32_t decide(uint32_t player) {
    using mh::game::mh_llm_strat_player_profile;
    const auto    &lv     = mh::state::live();
    const uint32_t p      = player & 0xffffu; // CMP AX: the low word is the operand
    const uint16_t side   = *reinterpret_cast<const uint16_t *>(lv.base[mh::state::RID_PLAYERSIDE]);
    const int32_t  mode   = *reinterpret_cast<const int32_t *>(lv.base[mh::state::RID_GAME_SESSION_MODE]);
    const auto    *prof   = reinterpret_cast<const mh_llm_strat_player_profile *>(lv.base[mh::state::RID_STRAT_PLAYERS]);
    const bool     retail = p != side;
    if (mode != SESSION_MP_LOCKSTEP || p >= 8) return retail ? 1 : 0;
    const uint32_t flags  = prof[p].status_flags;
    const bool     credit = (flags & STATUS_AI) != 0;
    // One line per re-landing in a lockstep match (a pioneer lift-off cycle): the evidence read by
    // tools/check_pioneer_refill.py.
    char m[200];
    wsprintfA(m, "; D37a: pioneer re-landing player %u (local %u) status 0x%X -> %s (retail: %s)\n", (unsigned)p,
              (unsigned)side, (unsigned)flags, credit ? "refill (AI)" : "no refill",
              retail ? "refill" : "no refill");
    seam_log(m);
    return credit ? 1 : 0;
}

// clang-format off
// Replaces `CMP AX,[PlayerSide]` (EAX = [EBP-0x18], the player). pushad/popad keeps every register;
// TEST sets ZF from decide's verdict and neither POPAD nor RET touches the flags, so the JZ after the
// CALL skips the credit exactly when decide returned 0.
__declspec(naked) void refill_thunk() {
    __asm {
        pushad
        push eax                     // player
        call decide                  // __cdecl(uint32_t) -> int32_t
        add  esp, 4
        test eax, eax                // ZF = 1 -> skip
        popad
        ret                          // back to 0x004798f6: JZ 0x00479a3e
    }
}
// clang-format on

} // namespace

extern "C" int MH_Relanding_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    const uintptr_t site = mh::addr::pioneer_refill_cmp_site;
    if (GetPrivateProfileIntA("net", "pioneer_refill_fix", 1, g_ini) == 0) {
        seam_log("; D37a: pioneer re-landing refill KEPT ([net] pioneer_refill_fix=0): every non-owner peer re-credits a "
                 "human's starting stock on a re-landing -- the reproduction arm\n");
        return 0;
    }
    if (mh::config::mode() == mh::config::mode_t::original) {
        seam_log("; D37a: pioneer re-landing refill KEPT ([config] mode=original): the original compare runs\n");
        return 0;
    }
    if (mh::hook::promoted_owner_of(site)) {
        seam_log("; D37a: pioneer re-landing refill DISPLACED: llm_strat_bldg_completion_dispatch is promoted this run -- "
                 "its body applies mother_relanding_credits (see the [interlock] line)\n");
        return 0;
    }
    char m[240];
    if (std::memcmp(reinterpret_cast<const void *>(site), SITE_EXPECT, SITE_LEN) != 0) {
        wsprintfA(m, "; D37a: pioneer re-landing refill NOT patched at %08X -- bytes differ from the expected CMP/JZ; "
                     "retail compare kept\n",
                  (unsigned)site);
        seam_log(m);
        return 0;
    }
    uint8_t repl[SITE_LEN];
    repl[0]      = 0xE8;
    int32_t call = (int32_t)((uintptr_t)&refill_thunk - (site + 5));
    memcpy(repl + 1, &call, sizeof(call));
    repl[5]    = 0x0F;
    repl[6]    = 0x84;
    int32_t jz = (int32_t)(SKIP_TARGET - (site + 11));
    memcpy(repl + 7, &jz, sizeof(jz));
    repl[11] = 0x90;
    repl[12] = 0x90;
    g_armed  = mh::hook::patch_bytes_guarded(site, SITE_EXPECT, repl, SITE_LEN);
    // clang-format off
    const char *fmt = g_armed
        ? "; D37a: pioneer re-landing refill gated at %08X -- in a lockstep match a re-landing refills AI players only\n"
        : "; D37a: pioneer re-landing refill NOT patched at %08X -- the write was refused; retail compare kept\n";
    // clang-format on
    wsprintfA(m, fmt, (unsigned)site);
    seam_log(m);
    return g_armed ? 1 : 0;
}
