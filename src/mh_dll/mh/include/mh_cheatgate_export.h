//
// mh_cheatgate_export.h -- mp:CH1: THE SINGLE-PLAYER CHEAT CONSOLE IS REACHABLE FROM A NETWORK GAME.
//
// THE PATH. In the strategic view Shift+Enter opens the same text line the chat uses, with
// `_G_LLM_CHAT_MODE = 3` (llm_strat_input_update, the branch at 0x00441c82: plain Enter is the chat
// -- team, or all with Ctrl/Alt -- and is gated on `SESSION_MODE == 3`; the Shift arm is NOT gated on
// the session mode at all). On the second Enter a mode-3 line goes to `llm_debug_console_dispatch`
// (0x0044b67f), which uppercases it, XORs every byte with 0x7a and str_cmp's it against the 47-slot
// table `llm_game_cheats_table_init` builds -- the hidden developer console of the single-player
// game (the 47 XOR-0x7a strings: `_TECHLAND RULEZ ` enables, `_MAP`, `_UNIT`, `_BUILDING`,
// `_REPAIR`, `_DESTROY`, `_NUCLEAR BOMB`, `_RESOURCES`, ...). Nothing in the dispatcher asks what kind
// of session it is running in, and only `_BAJADERA PARTY` (slot 3) checks for single player itself.
//
// WHAT A MATCH DOES IN A LOCKSTEP MATCH. The table splits two ways, and both are wrong for a
// network game:
//   * the ORDER cheats (`_NUCLEAR BOMB`, `_RESOURCES`, `_UNIT`, `_BUILDING`, `_REPAIR`, `_DESTROY`,
//     `_MAP`, `HARA KIRI`, the `_SWIATLO PRAWDY` family) issue through `llm_strat_order_dispatch`,
//     the REPLICATED lane: in `SESSION_MODE == 3` the order is staged for the lockstep horizon and
//     broadcast, so every peer executes it. Not a desync -- an unearned, unanswerable order.
//     `_NUCLEAR BOMB` (order 0xfa, `llm_combat_credit_planet_conquest_kills`) kills every unit
//     (2000 damage) and building (5000) of the first other player alive on the planet, per line.
//   * the LOCAL writes (`_SPY` flips every player's controller to human, so the AI stops ticking on
//     ONE peer; `_NETDELAY UP` / `_NETDELAY DOWN` scale this peer's lockstep step size, which sets
//     every order's exec time; `_NETDELAY SYNC` forces a resync from the leader) diverge the peers
//     by construction. `_TAP`, the CD/SOUND/resolution settings and the message browser are
//     lockstep-neutral, and are refused anyway: one rule ("the console is single player") is what a
//     player can be told; a per-slot allow list is not.
//
// THE GATE. One hook, a run-AFTER on `llm_chat_history_push` (0x00415475) -- the call every Enter
// in the text line makes before the mode split, and before the `str_cmp(line, "")` that guards the
// send/dispatch block in every consumer (llm_strat_input_update 0x00441c27, llm_net_chat_input_
// process, the scancode drain's 0x1c arm). In a lockstep match (`_G_LLM_GAME_SESSION_MODE == 3`) a
// console-mode line (`_G_LLM_CHAT_MODE == 3`) that matches any table slot other than `_NETDELAY`
// (slot 0xd -- prints this peer's delay and ping, writes nothing) is BLANKED (`line[0] = 0`) so the
// caller skips its whole block: the dispatcher never sees it, no chat send sees it, and the line is
// DROPPED -- a refused cheat is not chat (a mode-3 line never was chat anyway). It is logged
// (`; CH1: cheat '<cmd>' (idx 0x..) refused in a network game`) and the player sees a red floating
// "cheats are single player" (the desync notice's path). Outside a lockstep match, and for a plain
// chat line in one, nothing changes. The `idx` printed is the cheat CATALOG index (0 = `_NEW
// POWER_`, 0xb = `_NETDELAY`, 0x25 = `_NUCLEAR BOMB`, 0x2b = `_RESOURCES`),
// which is the table slot minus 2 (the switch's `case k` is `table[k]`; the catalog numbers from
// `&table[2]`).
//
// WHY NOT THE DISPATCHER'S ENTRY. llm_debug_console_dispatch is a named hook point the determinism
// harness observes (hook/hookpoint.h `point::console`, armed in MH_Harness_Init before any seam),
// and one entry has one owner: a gate installed there is refused in every harness-armed lane
// (measured 2026-09-21) -- i.e. exactly the lanes the determinism gate runs. history_push has no
// other owner and sits upstream of every consumer, which is also what makes ONE hook cover the
// three submit paths.
//
// THE SUBMIT LOG. The same hook writes `; [chat] submit mode=<m> sess=<s> len=<n> first='<c>'
// cheat_idx=<0x..|->` for EVERY submit -- the text REDACTED to its length and first character, plus
// the catalog index when it matches the table. The 2026-09-20 report's crash could not be
// attributed on first read because mh_input.log holds no chat evidence at all (four keyrepeat
// lines); this is the line that would have said "a mode-3 line matching 0x25 was submitted at
// 09:54:37" -- or that no such line ever was.
//
// `[input] cheat_gate=0` (mh_net.example.ini) is the REPRODUCTION ARM: the hook still logs, writes
// `; CH1: cheat '<cmd>' (idx 0x..) RUNS in a network game` and lets retail run the line.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Arm the hook. Reads `[input] cheat_gate` from mh_net.ini. Returns 1 if the gate is live, 0 if it
// disarmed -- best-effort, like every other seam: a refused install leaves retail behaviour, it
// never fails the process.
int MH_CheatGate_Install(void);

// The catalog index (= table slot - 2; 0x25 is `_NUCLEAR BOMB`) `line` would match in the console, or
// -1. Pure: works on a private copy, reads the live table, writes nothing. Exposed for the
// selftest and for anything that wants to classify a line without running the console.
int MH_CheatGate_MatchIndex(const char *line);

// Counters for the end-of-run report: console lines refused, console lines passed (`_NETDELAY`,
// or every match while the gate is off).
void MH_CheatGate_Stats(long *refused, long *passed);

#ifdef __cplusplus
}
#endif
