#pragma once
//
// UI automation harness -- Phase 2: direct UI actions.
//
// Drives mh.exe's menu/dialog UI WITHOUT any OS input: it injects synthetic events straight into
// the game's own mouse ring buffer (_G_LLM_INPUT_MOUSE_EVENTS), so the game's real per-frame input
// dispatch (llm_ui_widget_input_tick) pops them, runs its real hit-test, and activates widgets
// exactly as a physical click would. Widget centers are resolved with the game's OWN layout helper
// (llm_ui_widget_layout_resolve_position), so a "click NEW GAME" lands in the same rect the game
// would hit-test. Pairs with the Phase-1 capture harness (gfx_capture.cpp) for self-verification.
//
// Determinism-neutral: acts only in the menu/UI input layer, never the lockstep sim. Gated behind
// the [uitest] ini section so normal runs are untouched. EN-only.
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [uitest] config from mh_net.ini + enable (EN-only). Returns 1 if enabled. Does NOT install a
// hook -- the per-frame driver piggybacks the lockstep present hook (like the capture harness).
int MH_UIDrive_Install(void);

// Per-present driver: hotkeys (F7 = dump the active widget list to mh_uidrive.log, F8 = click the
// configured [uitest] target) + the auto-click state predicate (fire once the target label appears).
// Cheap when idle. Called from net_lockstep on_present at present-flip entry.
void MH_UIDrive_OnPresent(void);

// --- direct UI actions (also the Phase-3 interpreter's primitives) ---
// Move the UI cursor to (x,y) by injecting a MOVE event (updates hover, no activation).
void MH_UIDrive_CursorTo(int x, int y);
// Full click at (x,y): MOVE + LBUTTONDOWN + LBUTTONUP injected into the mouse ring.
void MH_UIDrive_Click(int x, int y);

// 2026-09-20: the harness's SYNTHESISED key state, for the DLL's own GetAsyncKeyState-polled
// hotkeys ([debug] toggle_key & co.). GetAsyncKeyState reads 0 on the isolated desktop a headless
// lane runs on, so a poller ORs this into its read: `(GetAsyncKeyState(vk) & 0x8000) ||
// MH_UIDrive_SynthKeyDown(vk)`. Non-zero only while a running UI script holds a chord with the
// `hotkey <spec>` verb; an unarmed run never sets it. Only the OS read is substituted -- the
// binding parse, modifier check and rising-edge logic of the poller run unchanged.
int MH_UIDrive_SynthKeyDown(int vk);

// UI-REC: the ACTIVE SCREEN's identity -- the active dialog's widget-list VA, or the menu list's when
// no dialog is up. This is exactly what the `screen` WAIT predicate compares, exposed so the input
// journal can record it and a replay can synchronise on it.
//
// WHY A REPLAY NEEDS IT. A recorded click carries the frame it happened on, and a frame index is not
// a valid clock for the menu: how many presents pass before a screen is ready depends on asynchronous
// resource loading, so the same click replayed at the same present index can land on a screen that
// has not appeared yet. That is the same reason the script grammar bans frame/time waits and gates
// everything on UI state. Recording the screen turns the journal's frame numbers from a schedule into
// an ordering, with the screen as the barrier.
unsigned MH_UIDrive_ActiveScreen(void);

// UI-REC: nonzero once the active screen's geometry has held still for [uitest] settle_ms (default
// 120ms of WALL time) -- the same rule the `settled` WAIT predicate uses, and rate-independent for
// the same reason. A journal replay releases a screen barrier on this, not on a present count.
int MH_UIDrive_ScreenSettled(void);

// UI-REC: a CONTENT signature of the active screen (widget geometry + container), for journal
// barriers. MH_UIDrive_ActiveScreen alone cannot identify a screen -- it is the container, and every
// screen with no dialog open shares one value, which is why barriers on a real session could not be
// confirmed. Stable across runs: the widget layout is deterministic.
unsigned MH_UIDrive_ScreenSig(void);

// UI-REC / SPCAMP-SYNC: the screen identity a JOURNAL BARRIER matches on -- CONTENT, not geometry.
// Hashes the child count and, per child, `value` / `disp_idx` / `flags` / label text: everything the
// dialog BUILDER writes and nothing the DRAW pass overwrites. MH_UIDrive_ScreenSig is unusable for
// this because a screen with any moving decoration (the campaign race picker has one) hashes to a
// different number every present, so the recorded value names a moment rather than a screen -- and the
// same motion pins MH_UIDrive_ScreenSettled to false forever, which a barrier also requires. Together
// those made a barrier on such a screen structurally impossible to confirm.
unsigned MH_UIDrive_ScreenId(void);
// Nonzero once the SCREEN ID (above) has been unchanged for settle_ms. The barrier's settle test: the
// geometry variant never comes true on a screen that animates continuously.
int MH_UIDrive_ScreenIdSettled(void);
// G146: measure BOTH screen dwells above in the harness's PINNED clock instead of GetTickCount. Pass
// the harness's reader; null restores GetTickCount. A dwell in wall time costs a frame-rate-dependent
// number of PRESENTS, and a journal replay is denominated in presents -- which is how the menu's frame
// rate reached the sim. The pinned clock advances a fixed dt per present, so the same duration becomes
// the same number of presents at any rate. Does NOT affect the script `settled <target>` wait.
//
// It must be the HARNESS's counter, not the game's _G_LLM_TIME_TICKS_MS: that global is only written
// when the game CALLS the clock, which the menu does every frame and the in-game path does not, so a
// dwell measured on it stalls forever in-game (measured -- both arms held at the same barrier
// identically, a stall wearing determinism's clothes).
void MH_UIDrive_SetSettleClock(unsigned (*fn)(void));
// Split click halves (LBUTTONDOWN-only / LBUTTONUP-only) so a script can spread a click across FRAMES --
// needed for scrollable LIST rows, which commit on the render-time hover (a same-frame click is stale).
void MH_UIDrive_Press(int x, int y);
void MH_UIDrive_Release(int x, int y);
// Click the child widget at index idx in the active list. Returns 1 on success, 0 if out of range.
int MH_UIDrive_ClickWidget(int idx);
// Click the first visible child whose label CONTAINS `substr` (case-insensitive). Returns 1 if a
// matching widget was found + clicked, else 0. NOTE the main menu + other sprite-drawn screens have
// NO text labels (label==0); those widgets are identified by their `value` (selection id / hotkey) --
// use MH_UIDrive_ClickValue for them. click_label is for text dialogs (save/load/options/lobby rows).
int MH_UIDrive_ClickLabel(const char *substr);
// Click the first visible child whose `value` field (menu selection id / hotkey ASCII) equals `value`.
// This is the stable identifier for the sprite-drawn menus. Returns 1 if found + clicked, else 0.
// E.g. main menu: 'g'=103 NEW GAME, 'n'=110 NETWORK GAME, 'i'=105 INTRO, 't'=116 TUTORIAL, 'l'=108 LOAD.
int MH_UIDrive_ClickValue(int value);
// Dump the active widget list (index, label, resolved center, flags) to mh_uidrive.log. Debug aid.
void MH_UIDrive_DumpWidgets(void);

#ifdef __cplusplus
}
#endif
