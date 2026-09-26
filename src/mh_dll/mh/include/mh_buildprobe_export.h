//
// mh_buildprobe_export.h -- mp:D35: THE D25 BUILD-CLICK FIX, CARRIED BY mh.dll.
//
// THE BUG (mp:D25, dead-ends G246). The HUD build-menu click, `llm_strat_bldg_try_begin_placement`
// (0x00448c0b), probes affordability by PAYING the cost (`CALL llm_bldg_pay_build_cost` at
// 0x00448c56) and, on success, immediately RE-GRANTING it (`CALL llm_strat_bldg_grant_type_resources`
// at 0x00448c90). The stock (`player_resources`) nets to exactly zero -- pay_build_cost's pass 2 only
// runs when every slot is covered, so game_SpendResource's never-negative clamp never bites, and
// llm_resource_add has no clamp at all -- but game_UpdateResourceStats books positive deltas only,
// so `resource_spent[1..4]` (player record +0x10054, hashed as pK_ai_econ) grows by the building's
// full cost on every click. The click runs on the CLICKING peer only (HUD widget callback), so in a
// lockstep match it is a peer-local write into hashed state: a permanent pK_ai_econ desync from the
// human's first build click. The real payment is build order 0x19's, on every peer.
//
// WHY mh.dll HAS TO CARRY IT. D25 was fixed only in libmh's promoted body (it probes with
// bldg_can_afford_build_cost). The player zips run CONFIGURATION (1) -- mh.dll + mh_net.dll +
// mh_harness.dll, no libmh.dll -- where nothing is promoted and the original body runs, so the
// shipped build still desynced (the 2026-09-26 rc3 match: DESYNC step=550 first_region=11
// p1_ai_econ, never healed).
//
// THE FIX. Two guarded edits inside the original body, applied BOTH OR NEITHER:
//   0x00448c56  E8 56 A2 04 00  CALL llm_bldg_pay_build_cost
//               -> CALL a thunk running pay_build_cost's PASS 1 ALONE: the invention gate (0x13),
//                  the non-short-circuiting shortage scan (first short id -> id+0x89, any second
//                  -> bare 0x89), 0 when every slot is covered. Same return in EAX, every other
//                  register preserved, and it writes nothing.
//   0x00448c90  E8 2A AB 04 00  CALL llm_strat_bldg_grant_type_resources
//               -> five NOPs. The grant only ever undid the payment the probe no longer makes.
//                  The next instruction (0x00448c95 MOV EAX,[EBP-0x1c]) redefines EAX, and nothing
//                  after it reads a register the call could have changed.
// Every other observable -- the mothership gate, the refusal text + its suppression bracket, the
// three success-path writes, the return -- is the original's, byte for byte. This is exactly the
// semantics of libmh's promoted body (sim/resid/sim_bldg_try_begin_placement.cpp), so configuration
// (1) and configuration (2) run the same rule.
//
// SINGLE PLAYER TOO, DELIBERATELY. Charging nothing is correct everywhere: the only thing the
// pay + re-grant ever changed was the gains-only lifetime counter, which then counted build-menu
// CLICKS as income. libmh's promoted body has applied the same rule in single player since
// 2026-09-19, so gating this to lockstep matches would only make configuration (1)'s single player
// disagree with configuration (2)'s.
//
// NOT UNDER `[config] mode=original`. That selector means "the game's own bodies", and for THIS fix
// the original body is the bug by definition: the UI-REC A/B/C oracle's B arm and its declared
// excusal (tools/data/abc_excusals.json, D25-resource_spent) both rely on mode=original paying and
// re-granting. The fix belongs to the ours-configurations: the promoted body in (2), this seam in
// (1).
//
// THE INTERLOCK. When libmh promotes llm_strat_bldg_try_begin_placement, both sites are inside a
// JMP'd-away body: this install says DISPLACED and writes nothing, and the promoted body carries the
// fix (registered in tools/data/dll_patch_manifest.json with that carrier).
//
// Off with `[net] build_probe_free=0` -- the retail probe, the reproduction arm.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Apply both edits (best-effort: a byte mismatch at either site, a promoted parent, mode=original,
// the knob, or a refused VirtualProtect leaves the function untouched and logs it). Returns 1 when
// armed, 0 otherwise. Never fails the process.
int MH_BuildProbe_Install(void);

#ifdef __cplusplus
}
#endif
