#pragma once
// mh_storagepurge_export.h -- mp:D38 row 5: THE STORAGE BUILDING PANEL'S DEAD-UNIT PURGE MAY NOT TOUCH
// SIM STATE IN A LOCKSTEP MATCH.
//
// THE BUG. llm_strat_ui_storage_bldg_panel (0x00417409) -- the HUD panel of a selected garage /
// barracks / airfield / helipad / port / shuttle -- calls llm_strat_storage_purge_dead_docked
// (0x0049b8f5) for PlayerSide + the building's storage slot on every panel refresh (the refresh latch
// is raised by selecting the building, by the panel switch, and by llm_strat_bldg_notify_ui for the
// selected building). The purge repairs docked units' home_storage_slot and removes every docked unit
// whose energy is <= 0 from unit_storage (docked_count, occupancy, the docked list) -- on the selecting
// peer only. The sim runs the same purge for every housing building each sub-tick A (5 game-s), so the
// other peers catch up later -- unless something reads the dead row first:
// llm_strat_hangar_recharge_pulse revives a dead docked unit (pays resources, energy > 0) on the peers
// that did not purge it, and storage_can_enter / prod_shuttle_depart read the roster. Transient at
// best, a permanent `unit_storage`/`units` divergence at worst. Static audit D38 row 5; how a docked
// unit dies in MP is not established -- this is a pre-emptive fix (user decision 2026-09-27).
//
// THE FIX. In a lockstep match (_G_LLM_GAME_SESSION_MODE == 3) the call site 0x0041748a is spliced to
// a thunk that SKIPS the UI-side purge: the sim's own purge removes the row within one sub-tick A on
// every peer at the same step. The panel then latches the docked count including the dead unit and
// may show its icon until the next refresh; nothing it draws or reads depends on the purge having run
// (the icon list reads unit_proto_id, which a dead docked unit keeps; a click on the row issues a
// replicated order that every peer evaluates alike). Outside a lockstep match the thunk calls the
// original untouched (single player stays retail). The panel itself is not promoted in configuration
// (2), so the one call-site splice covers both configurations.
//
// [net] storage_panel_purge_fix (default 1); 0 = no splice (retail, the reproduction arm). Not under
// [config] mode=original. Best-effort: bytes differ -> nothing written, logged.
#ifdef __cplusplus
extern "C" {
#endif
int MH_StoragePurge_Install(void);
#ifdef __cplusplus
}
#endif
