#pragma once
//
// D19 -- a hotkey for the orphaned PAUSE screen (game mode 5).
//
// mh.exe ships a COMPLETE pause screen with no way to reach it. `llm_pause_frame` (0x0043ee0c) is
// `llm_pause_input_wait` + `llm_strat_render_present`: the strategic view is re-rendered every frame
// but `llm_strat_sim_tick` is NOT called, so the simulation and the game clock stand still. The
// identity is settled by the game's own text -- `llm_strat_draw_floating_messages` ends with
// `if (_G_LLM_GAME_MODE == 5) llm_ui_hud_panel_title_draw(G_TEXT_PTRS[0x94])`, and TEXT[0x94] reads
// **"Game paused. Press any key to continue..."** (probed at runtime through this DLL, EN build).
//
// The RESUME half already works: `llm_pause_input_wait` latches any key-down (`event_type & 0x100`)
// or fresh LMB/RMB press, sets `_G_LLM_GAME_MODE = 2` and calls `llm_strat_time_resync_and_tick()`
// (the resync absorbs the paused wall-clock interval). Only the ENTRY is missing: nothing in either
// the EN or the frozen RU binary ever stores 5 into `_G_LLM_GAME_MODE` (46 references, immediates
// 0,1,2,3,4,6,7,8; nothing takes the address into a register, so there is no indirect store either).
// Full evidence: the game-mode notes "Mode 5".
//
// So this seam supplies the missing half and nothing else -- one key, one store.
//
// WHY IT HOOKS THE INPUT FUNCTION AND NOT THE PRESENT HOOK. The trigger reads the game's own key
// EVENT RING (`_G_LLM_INPUT_KEY_EVENTS`), which is what the UI harness's `key` action writes into --
// that is what makes the feature testable by tools/test_ui.py at all. But the ring is drained to
// empty by `llm_strat_input_update` every strategic frame, long before the present flip, so a
// present-time poll would see nothing. We therefore run as a run-before detour on
// `llm_strat_input_update` and inspect the ring while it is still full. (GetAsyncKeyState, the debug
// overlay's mechanism, is invisible to the harness and was rejected for that reason.)
//
// The matching event is REMOVED from the ring (compacted out) rather than merely observed, so the
// game's own hotkey ladder never sees the key -- the bound key cannot double-fire a stock action,
// whatever that ladder would have made of it.
//
// Gates, both non-negotiable:
//   * `_G_LLM_GAME_MODE == 2` -- only the live strategic view may pause; `llm_strat_frame` is also
//     called to redraw the world behind dialogs, and pausing then would strand the dialog.
//   * `_G_LLM_GAME_SESSION_MODE != 3` -- a lockstep match must never pause one side: mode 5 skips
//     `llm_strat_sim_tick`, so entering it mid-match would stall the peer. SP only. (The stock game
//     agrees: its speed-up/slow-down keys are gated on the same check.)
//
// KNOWN COSMETIC GAP: the "Game paused" banner does not actually appear on the paused frame. The
// title goes to the HUD panel layer (`llm_ui_panel_text_draw` target 1, HUD widget 0x10) which mode 5
// never re-composites, since it runs no HUD tick -- forcing `game_SetEvent(HUD_REDRAW_ALL)` on entry
// did not change the captured frame either. Stock behaviour, unrelated to this seam; tracked in
// the game-modes backlog.
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [pause] from mh_net.ini and arm the detour. EN-only. Returns 1 if armed, 0 otherwise
// (disabled by ini, wrong build, or the entry prologue did not match). Best-effort, like every
// other seam: a failure here never blocks the rest of the DLL.
int MH_Pause_Install(void);

#ifdef __cplusplus
}
#endif
