// seams/ui_crew_reminder.cpp -- hotfix rc9.4: a local vehicle waiting in its garage for crew re-shows
// retail's "You have run out of people to drive vehicles" every [ui] crew_shortage_reminder_s game
// seconds. Mechanism, the patch site and the contract are in include/mh_crewreminder_export.h; the rule
// and the throttle in mh_common/include/mh_crew_reminder.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_crewreminder_export.h"
#include "mh_crew_reminder.h"
#include "addr/mh_addrs.gen.h"    // mh::addr::PlayerSide, _G_LLM_STRAT_* globals
#include "addr/mh_calls.gen.h"    // mh::call::llm_strat_race_alert_text_emit
#include "caps/caps.h"            // mh::caps::rec: the storage + building arrays the game addresses (PT-CAP8)
#include "config/config.h"        // mh::config::ours_run()
#include "lockstep/turn_engine.h" // reimpl_fixes::crew_shortage_reminder_s (libmh's twin)
#include "hook/patch.h"           // patch_bytes_guarded
#include "hook/promoted.h"        // promoted_owner_of
#include "net_internal.h"         // seam_log; g_ini
#include "en_guard.h"             // EN-only build gate

namespace {

constexpr uintptr_t SITE                  = 0x0047f2d7u; // llm_strat_unit_state_exit_wait: MOV [TICK_BUDGET],0 (the can't-exit arm)
constexpr uintptr_t SITE_NEXT             = 0x0047f2e1u; // MOV [TICK_BUDGET+4],0
constexpr int       SITE_LEN              = 10;
const uint8_t       SITE_EXPECT[SITE_LEN] = {0xC7, 0x05, 0x38, 0x37, 0xAE, 0x00, 0x00, 0x00, 0x00, 0x00};

// Record layouts, EN v406 (the offsets llm_strat_unit_state_exit_storage_begin 0x0047f0b7-0x0047f23b reads).
constexpr uintptr_t UNIT_PROTO_OFF        = 0x02;  // map_object_unit.unit_proto_id (ushort)
constexpr uintptr_t UNIT_STORAGE_SLOT_OFF = 0x30;  // map_object_unit.home_storage_slot (byte)
constexpr uintptr_t CFG_UNIT_STRIDE       = 0x23f; // Unit[100]
constexpr uintptr_t CFG_UNIT_HUMAN_OFF    = 0x223; // Unit[].human (int), 0x00e4a2bb
constexpr uintptr_t CFG_UNIT_SOLDIER_OFF  = 0x227; // Unit[].soldier_count (int), 0x00e4a2bf
constexpr uintptr_t POP_STRIDE            = 0x34;  // _G_LLM_STRAT_POP_STATS[8]
constexpr uintptr_t POP_HUMAN_OFF         = 0x0c;  // .human (idle people), 0x00c3a37c
// unit_storage / buildings: base, row width and record size come from mh::caps (PT-CAP8); .b_index is the int at +0.
constexpr uintptr_t BLDG_ID_OFF                    = 0x02;        // .building_id (ushort)
constexpr uintptr_t BLDG_SHUTTLE_SLOT_OFF          = 0xc5;        // .shuttle_slot (byte), 0x00c3d365
constexpr uintptr_t CFG_BLDG_TYPE                  = 0x00d9ec88u; // Building[id].type (byte), stride 0x842
constexpr uintptr_t CFG_BLDG_STRIDE                = 0x842;
constexpr uint8_t   BTYPE_H_SHUTTLE                = 0x21;
constexpr uint8_t   BTYPE_A_SHUTTLE                = 0x0d;
constexpr uintptr_t SHUTTLE_PSTRIDE                = 0x1f18; // _G_LLM_PROD_SHUTTLE_SLOTS, 10 per player
constexpr uintptr_t SHUTTLE_STRIDE                 = 0x31c;
constexpr uintptr_t SHUTTLE_RESERVED_OFF           = 0x58;        // .passengers_reserved (int), 0x00bd21b4
constexpr uintptr_t RACE_ALERT_TEXT_LAST_PLAY_TIME = 0x005202b4u; // _G_LLM_STRAT_RACE_ALERT_TEXT_LAST_PLAY_TIME (double, plain static)
constexpr uintptr_t RACE_ALERT_TEXT_COOLDOWN       = 0x005001aeu; // _G_LLM_STRAT_RACE_ALERT_TEXT_COOLDOWN_SEC (double, 2.0)

int                         g_period = 0; // [ui] crew_shortage_reminder_s
bool                        g_armed  = false;
mh::crew_reminder::throttle g_throttle;

template <class T>
T rd(uintptr_t a) { return *reinterpret_cast<const T *>(a); }

// exit_wait's "still can't exit" arm, before its first instruction. Every value is read, nothing written
// but the throttle (a DLL static) and -- through the emitter -- its plain statics and the print queue.
void __cdecl crew_wait_reminder() {
    const uint32_t player = rd<uint16_t>(mh::addr::_G_LLM_STRAT_CUR_PLAYER);
    if (player >= 8 || player != rd<uint16_t>(mh::addr::PlayerSide)) return;
    const uintptr_t unit = rd<uintptr_t>(mh::addr::_G_LLM_STRAT_CUR_UNIT);
    if (!unit) return;
    const uint32_t  proto    = rd<uint16_t>(unit + UNIT_PROTO_OFF);
    const uint32_t  slot     = rd<uint8_t>(unit + UNIT_STORAGE_SLOT_OFF);
    const uintptr_t cfg      = mh::addr::Unit + proto * CFG_UNIT_STRIDE;
    const int32_t   need     = rd<int32_t>(cfg + CFG_UNIT_HUMAN_OFF);
    const int32_t   soldiers = rd<int32_t>(cfg + CFG_UNIT_SOLDIER_OFF);
    if (soldiers >= 1) return;
    const int32_t   idle     = rd<int32_t>(mh::addr::_G_LLM_STRAT_POP_STATS + player * POP_STRIDE + POP_HUMAN_OFF);
    const int32_t   b_index  = rd<int32_t>(reinterpret_cast<uintptr_t>(mh::caps::rec<uint8_t>(mh::caps::UNIT_STORAGE, player, slot)));
    const uintptr_t bldg     = reinterpret_cast<uintptr_t>(mh::caps::rec<uint8_t>(mh::caps::BUILDINGS, player, (uint32_t)b_index));
    const uint32_t  bid      = rd<uint16_t>(bldg + BLDG_ID_OFF);
    const uint8_t   btype    = rd<uint8_t>(CFG_BLDG_TYPE + bid * CFG_BLDG_STRIDE);
    int32_t         reserved = 0;
    if (btype == BTYPE_H_SHUTTLE || btype == BTYPE_A_SHUTTLE)
        reserved = rd<int32_t>(mh::addr::_G_LLM_PROD_SHUTTLE_SLOTS + player * SHUTTLE_PSTRIDE +
                               rd<uint8_t>(bldg + BLDG_SHUTTLE_SLOT_OFF) * SHUTTLE_STRIDE + SHUTTLE_RESERVED_OFF);
    if (idle + reserved >= need) return; // waiting for the door, not for crew
    const double now = rd<double>(mh::addr::_G_LLM_STRAT_GAME_CLOCK);
    // The emitter's own stamp: a retail show restarts our period, and a due reminder inside the emitter's
    // cooldown waits for a later tick instead of being suppressed (and counted toward its beep).
    const double shown = rd<double>(RACE_ALERT_TEXT_LAST_PLAY_TIME);
    if (!mh::crew_reminder::due(g_throttle, now, g_period, shown, rd<double>(RACE_ALERT_TEXT_COOLDOWN))) return;
    mh::call::llm_strat_race_alert_text_emit();
    const bool queued = rd<double>(RACE_ALERT_TEXT_LAST_PLAY_TIME) == now; // the emitter stamps a queued text
    char       m[220];
    wsprintfA(m, "; crew reminder: player %u vehicle proto %u waits for crew: idle %d + reserved %d < %d at clock %d ms (last shown %d ms) -> %s\n",
              player, proto, idle, reserved, need, (int)(now * 1000), (int)(shown * 1000),
              queued ? "advisor text queued" : "NOT queued (sim inactive?)");
    seam_log(m);
}

// clang-format off
__declspec(naked) void crew_wait_thunk() {
    __asm {
        pushad
        call crew_wait_reminder
        popad
        mov  dword ptr ds:[0x00ae3738], 0   // the displaced MOV [_G_LLM_STRAT_TICK_BUDGET],0
        push 0x0047f2e1                     // SITE_NEXT: MOV [TICK_BUDGET+4],0
        ret
    }
}
// clang-format on

} // namespace

extern "C" int MH_CrewReminder_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    int period = mh_ini_get_int("ui", "crew_shortage_reminder_s", 30, g_ini);
    if (period < 0) period = 0;
    g_period = period;
    if (mh::config::ours_run()) { // libmh's exit_wait twin reads the same value
        mh::lockstep::reimpl_fixes fx = mh::lockstep::fixes();
        fx.crew_shortage_reminder_s   = period;
        mh::lockstep::set_fixes(fx);
    }
    char m[260];
    if (period == 0) {
        seam_log("; crew reminder: OFF ([ui] crew_shortage_reminder_s=0) -- retail: the crew-shortage advisor text "
                 "is shown once, when a vehicle starts to leave its garage\n");
        return 0;
    }
    if (mh::hook::promoted_owner_of(SITE)) {
        wsprintfA(m, "; crew reminder: every %d s -- retail splice DISPLACED (llm_strat_unit_state_exit_wait is promoted; "
                     "libmh's twin carries it)\n",
                  period);
        seam_log(m);
        return 0;
    }
    uint8_t repl[SITE_LEN];
    repl[0]     = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)&crew_wait_thunk - (SITE + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    memset(repl + 5, 0x90, SITE_LEN - 5);
    g_armed = mh::hook::patch_bytes_guarded(SITE, SITE_EXPECT, repl, SITE_LEN);
    wsprintfA(m, "; crew reminder: every %d s of game clock while a local vehicle waits for crew -- splice %s at %08X\n", period,
              g_armed ? "armed" : "NOT armed (bytes differ from the expected MOV [TICK_BUDGET],0)", (unsigned)SITE);
    seam_log(m);
    return g_armed ? 1 : 0;
}
