//
// mh_crewreminder_export.h -- hotfix rc9.4: GARAGE VEHICLES WAITING FOR CREW RE-SHOW THE ADVISOR TEXT,
// CARRIED BY mh.dll FOR THE RETAIL BODY.
//
// THE BUG (field report 01a13bb6, reproduced offline: tmp/report_triage_2026-10-14/garage_repro.md). A
// docked vehicle leaves its garage only when Unit[proto].human <= POP_STATS[player].human, the IDLE
// population (llm_strat_storage_can_exit 0x0048a548; soldiers and shuttle-reserved passengers excepted).
// With 0 idle people the vehicles sit in EXIT_WAIT (0x22) for good. Retail says why exactly ONCE -- "You
// have run out of people to drive vehicles" (text 0x7f human / 0xb0 alien) from
// llm_strat_unit_state_exit_storage_begin 0x0047efc6 via llm_strat_race_alert_text_emit 0x00425b2a --
// and llm_strat_unit_state_exit_wait 0x0047f270 then re-checks every tick in silence. The player clicked
// "Exit all" ten times over 50 s with no feedback.
//
// THE FIX ([ui] crew_shortage_reminder_s, default 30; 0 = retail). The rule and the throttle are in
// mh_common/include/mh_crew_reminder.h: while a LOCAL-player (PlayerSide) unit waits for crew, exit_wait
// calls the same emitter again at most once per 30 s of game clock. DISPLAY-ONLY.
//
// ONE guarded edit, 10 bytes at 0x0047f2d7, the first instruction of exit_wait's "still can't exit" arm:
//   C7 05 38 37 AE 00 00 00 00 00   MOV dword ptr [_G_LLM_STRAT_TICK_BUDGET],0
// ->
//   E9 <rel32> 90 90 90 90 90       JMP crew_wait_thunk (pushad; the reminder; popad; the displaced MOV;
//                                   back to 0x0047f2e1)
// Under a promoted sim body (configuration (2)) libmh's sim_unit_state_exit.cpp carries the same rule
// (reimpl_fixes::crew_shortage_reminder_s); the splice is then refused as DISPLACED if the site is dead.
// Not tied to [config] mode: a display fix covers the original configuration too.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Reads [ui] crew_shortage_reminder_s, hands it to libmh (reimpl_fixes) and arms the retail splice.
// Returns 1 when the splice is armed. Idempotent; logs one line.
int MH_CrewReminder_Install(void);

#ifdef __cplusplus
}
#endif
