#pragma once
// mh_netcrit_export.h -- mp:D37: THE HUD NETWORK PANEL'S "IS THIS BUILDING CRITICAL" PROBE MAY NOT
// TOUCH SIM STATE IN A LOCKSTEP MATCH.
//
// THE BUG. llm_ui_hud_bldg_network_status_panel_draw (0x00418909) runs every frame on the peer that
// has a power-network building selected, and calls llm_strat_bldg_is_network_critical (0x00497405)
// for PlayerSide + the selected building. That probe zeroes the building's energy, runs
// llm_strat_bldg_power_network_recompute three times (re-flooding bit 0 of built_flags across the
// player's whole buildings row, re-electing the primary mother into _G_LLM_STRAT_PLAYERS[p]) and
// restores the energy -- but NOT the recomputed connectivity. The sim only recomputes on an unmap,
// so whenever the sim's flags are stale (e.g. a relay mid-dismantle) the selecting peer's buildings
// row silently diverges from every other peer's: rc4 Nortus, first diff `buildings` (built_flags,
// then cycle_progress through efficiency). Reproduced: registry row d37_relay_chain.
//
// THE FIX. In a lockstep match (_G_LLM_GAME_SESSION_MODE == 3) the call site 0x00418967 is spliced
// to a thunk that byte-snapshots everything the probe can write for the calling player --
// buildings[p][0..99] (0x6aa4 bytes, incl. the [0].energy offline counter) and
// _G_LLM_STRAT_PLAYERS[p] (0x740 bytes; mother_reelect_primary's primary_mother_unit/bldg) --
// calls the original probe, and restores both. The HUD gets its verdict, the sim sees zero change.
// Outside a lockstep match the thunk calls the probe untouched (single player stays retail).
// llm_strat_bldg_notify_ui's writes (general.change_flag, game_SetEvent) are UI-only and not undone.
//
// [net] netcrit_ui_pure (default 1); 0 = no splice (retail, the reproduction arm). Not under
// [config] mode=original. Best-effort: bytes differ -> nothing written, logged.
#ifdef __cplusplus
extern "C" {
#endif
int MH_NetCrit_Install(void);
#ifdef __cplusplus
}
#endif
