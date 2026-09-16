//
// ai/ai_promote.h -- AI1-P: whole-domain E9 promotion of the AI closure (2026-09-01).
//
// The 167 rows tools/data/ai_migration.json marks "verified" (the "dead" rows -- 42 of them -- are
// dead roots by construction and are NOT promoted, same as every other domain's promotion glue).
// Each promoted original gets an MH_EXPORT_REPLACE seam whose IMPL is a named adapter in
// ai_promote.cpp's mh::ai::promoted_arm namespace: the adapter's own signature is
// ::mh::exp::sig_<orig> EXACTLY (so a wrong parameter list is a compile error, not a stack that
// comes apart at runtime -- see addr/mh_export.gen.h's own banner), and its body logs a FIRST-CALL
// liveness line, then calls the corresponding public mh::ai:: live wrapper (the state()-bound
// overload every AI translation unit already exposes alongside its detail:: bound-view variant).
//
// WHY THE WHOLE DOMAIN ARMS TOGETHER, not row by row: this cluster's internal calls run through the
// `ai_calls` struct (ai_state.h), not raw function pointers to the original addresses, so once ANY
// one seam is installed, every OTHER AI original that calls it is silently redirected to our body
// too, through the ordinary E9 patch -- there is no way to promote one row while 166 siblings still
// call the pre-patch bytes of that same row. Installing the set is what makes it coherent, not a
// convenience.
//
// ONE OF THE 167 IS NOT WIRED HERE (see ai_promote.cpp's unbindable note, and the writer's
// self-check report for the full reasoning):
//   llm_strat_ai_unit_squad_firepower_value      -- no public mh::ai wrapper exists; only a private
//                                                    detail:: helper of army_milestone_advance_or_attack.
// It still gets an MH_EXPORT_REPLACE-shaped seam IF a future session resolves the gap (a public
// wrapper, or the detail-only overload promoted directly) -- until then its pre-patch bytes stay
// live, same as any unarmed function in this binary.
//
// It used to be TWO. llm_strat_ai_engage_sort_candidates_by_dist was the other, because its public
// wrapper needs a THIRD parameter (inherited_sorted_flag) standing in for the ORIGINAL's ambient ESI
// and the seam's arity came from a 2-argument prototype. REBIND-AI-ESI (2026-09-10) resolved it the
// way this note asked for -- by RE, not by a default: the ESI is now a committed custom-storage
// parameter, so the seam is three arguments wide and the generated entry thunk forwards the caller's
// real register. It is wired.
//
// FIRST-CALL PROOF, not a milestone counter (the P0-EXPORT lesson order_queue.cpp's own banner
// records): every one of the 166 wired adapters logs
//   "; [promote] ai: <orig_name> call #1 (OURS is live)"
// on its own first invocation only (MH_AI_FIRST_CALL, ai_promote.cpp), so a run's log says per row
// which promoted bodies were actually ENTERED, not merely installed. llm_strat_ai_players_tick --
// the closure root, called once per AI player per strategic tick -- carries no special-cased line;
// it gets the same per-row proof as every other seam, which is sufficient: its first call being
// logged IS the proof the closure root itself was reached.
//
// ini gate: [promote] ai=<0|1> (via `default_on`, passed in by the caller -- this module carries no
// seams-layer include, same discipline as mh::orders::install_promotion). Per-function red-ladder
// override: [promote_skip] <orig_name>=1 skips installing that one seam even when the domain gate is
// on, without touching the other 165.
//
// THE ISLAND MOVE lives in ai_state.cpp (the AI's one addr-binding TU, per check_sim_addresses'
// binder rule) and runs only AFTER install_promotion has taken the entries -- moving storage
// before the closure is shown to run is exactly the ordering P2-RULES exists to prevent. See
// ai_state.cpp's island banner for the 48-region derivation and the named non-movers.
//
#pragma once

namespace mh::ai {

// True once install_promotion() has installed at least one seam.
bool promotion_active();

// AI1-P second half (ai_state.cpp): copy the 48 zero-original-accessor island regions into DLL
// storage, rebase the registry, poison the old .bss. Call ONLY when promotion_active(); returns
// the region count, 0 on refusal, -1 if already moved.
int island_move();
// The runtime untouched-check over the poisoned old VAs; logs at widening players_tick milestones.
void island_verify_tick(long call_no);

// Installs every wired AI seam whose ini gate says on. Returns the count actually installed (0 if
// the domain gate itself was off). See the banner above for the ini keys and the one row this
// cannot wire.
int install_promotion(int default_on);

} // namespace mh::ai
