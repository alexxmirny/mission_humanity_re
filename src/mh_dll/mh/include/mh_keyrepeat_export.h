#pragma once
//
// U24 -- modal key-repeat fix (one dispatch per keystroke in menu/lobby text fields).
//
// SYMPTOM (reported from a player's machine, 2026-07-26; not reproducible on ours): typing into any
// menu/lobby text field -- player name, direct-IP address, lobby chat -- inserts the character many
// times ("1" -> "1111111111111"). In-game command hotkeys are UNAFFECTED, which is the clue that
// isolates the cause: hotkeys pop the key ring directly (llm_strat_input_update), one action per
// real event, while the modal text path dispatches from PUBLISHED GLOBALS that nothing clears.
//
// MECHANISM. llm_ui_edit_field_modal_tick (0x004bbd30) runs a modal loop that pumps
// llm_ui_modal_key_pump (0x004b528f -- renamed from llm_boot_intro_tick 2026-07-26: it is the SHARED
// modal key pump, and the intro is only one of its five callers) and, on every reported event,
// dispatches _G_LLM_UI_MODAL_KEY_SCANCODE + _G_LLM_UI_MODAL_KEY_ASCII. The
// pump latches one dequeued key into KEYREC (0x006542ca) and publishes it into that pair when its
// step timer expires -- but the pump is a 150 ms fixed-step CATCH-UP accumulator:
//
//     if (STEP_NEXT_MS < STEP_DEADLINE_MS) {
//         if (MENU_NOW_MS < STEP_NEXT_MS) return 0;                  // caught up -> loop exits
//         STEP_NEXT_MS += 150; EVENT_KIND = 1; return 1;             // one catch-up step
//     }
//
// and MENU_NOW_MS is wall-clock ms sampled once per menu frame (llm_ui_menu_state_tick, 0x004b7c3b).
// So while the accumulator is behind MENU_NOW_MS it keeps reporting events that carry no new key,
// and the edit field re-dispatches the SAME published payload for each. Note the publish branch
// re-arms `STEP_NEXT_MS = STEP_DEADLINE_MS + 400; STEP_DEADLINE_MS = 0xffffffff`, so the segment
// immediately AFTER a publish is precisely that catch-up regime. (Same accumulator family as the
// U3/U7 stalled-ms-clock lobby slide-in -- see the menu RE.)
//
// WHAT IS AND IS NOT ESTABLISHED (updated 2026-07-27). Established: the structure above, and that
// clearing the published pair cures the player's machine. NOT established: the repeat COUNT and why
// it differs per machine. The obvious model, chars = (MENU_NOW_MS - STEP_NEXT_MS)/150 + 1, was
// asserted before it was measured and has never been observed -- the first diagnostic excluded the
// very regime it meant to measure, so both a clean rig run and the player's run logged nothing and
// were misread as agreement. Worse, taken literally with STEP_DEADLINE_MS only ever written to
// 0xffffffff, that model predicts STEP_NEXT_MS wrapping to 399 and hundreds of repeats on EVERY
// machine, which plainly does not happen -- so either a writer to STEP_DEADLINE_MS exists that the
// reference manager cannot see (this binary's signature failure mode) or the model is wrong
// elsewhere. Treat the formula as a hypothesis until a publish line from a machine that reproduces
// is in hand.
//
// FIX. A run-before hook on the pump clears the PUBLISHED pair at entry. Every consumer performs
// exactly one pump call per dispatch, so at entry whatever is published has already been dispatched;
// zeroing it makes each publish dispatch exactly once and turns every extra catch-up step into a
// no-op (scancode 0 matches no branch, char 0 fails the `0x1f < ch < 0x100` insert test). KEYREC is
// NOT touched, so a key that is latched but not yet published still publishes normally. Animation
// pacing is untouched -- this only stops the stale re-read, it does not clamp the accumulator.
//
// Instrumentation: bursts (catch-up depth > 1) are logged to mh_input.log with the depth and the
// suppressed character, so a player's log shows the bug and the fix acting on their machine.
// Deliberately its OWN log file, so the mh_net.log arm-line sequence diffed by the refactor gate is
// unchanged.
//
#ifdef __cplusplus
extern "C" {
#endif

// Install the run-before hook on the modal key pump. EN-only, PROLOGUE-guarded (a mismatch disarms
// loudly and patches nothing). Default ON with no ini; `[ui] key_repeat_fix=0` disables it, which is
// how a machine that reproduces the bug can A/B the fix. Returns 1 if the hook armed.
int MH_KeyRepeat_Install(void);

#ifdef __cplusplus
}
#endif
