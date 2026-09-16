//
// sim/resid/resid_promote.h -- SIM-RESID-P (2026-09-01): the PROMOTION install glue for the sim
// residual domain (tools/data/sim_resid_migration.json).
//
// WHAT THIS FILE IS. The 32 rows the ledger carries at `"state": "verified"` (all evidence_tier T2,
// all reachable, none of the demoted `"dead"` rows) are the sim_resid domain's whole promotable set --
// there is no partial-domain promotion here, unlike orders/order_queue.cpp's nine-of-eleven split.
// This is a WHOLE-DOMAIN E9 promotion: every verified row's game entry point is redirected (the
// generated `MH_EXPORT_REPLACE` naked entry thunk, an E9 near-jump over the original's prologue, per
// mh/hook's install_export_ok) onto a named adapter in sim/resid/resid_promote.cpp that forwards into
// this same TU's already-verified `mh::sim::` public wrapper. Ours becomes the function; there is no
// original arm left running for any of these 32 seams once installed.
//
// THE INI GATE is `[promote] sim_resid` (mirrors mh::orders's `[promote] orders` / mh::tact's
// per-function keys), read by `install_promotion_resid`'s caller-supplied `default_on` -- this module
// carries NO seams-layer include (no `net_internal.h`), so it cannot read `SHIP_PROMOTE_*` itself; the
// seams layer passes the shipping default in at the one call site, keeping "what ships" answerable by
// reading one block instead of two (same discipline as `mh::orders::install_promotion`).
//
// LOGGING goes through `mh::ai::ai_say` (ai/ai_state.h), NOT a new module-local say()/set_logger()
// pair. That header's own comment already calls it "the shared trace sink, not AI state", and every
// OTHER `mh::sim::promoted_arm` promotion TU in this domain (sim_order_dispatch.cpp,
// sim_bldg_tick_animation_state.cpp, sim_building_tick.cpp, sim_unit_tick.cpp, sim_step.cpp) already
// logs through it -- it is already wired to seam_log (seams/reimpl_probe.cpp's
// `mh::ai::set_logger(seam_log)`), so reusing it means this file's log lines work the moment it is
// linked in, with no further seams-layer change needed.
//
// PER-ROW SKIP: `[promote_skip] <orig_name>` (default 0 = not skipped) lets one row be held back
// without touching the other 31 -- e.g. to isolate a divergence during an A/B run, or to keep a single
// seam original while the rest of the domain goes live. A skipped row is neither installed nor counted
// against the "ALL N installed" summary line; it is logged as skipped, not as refused.
//
// PER-ROW FIRST-CALL LIVENESS. All 32 rows are T2 (offline-oracle-only -- none is shadow-armed, so
// installing this domain is the FIRST time any of them runs against the real game image). Each
// adapter in resid_promote.cpp's `promoted_arm` namespace logs, on its first invocation only,
// `; [promote] sim_resid: <orig_name> call #1 (OURS is live)` -- exact text, grepped by
// tools/test_ui.py. The install-time `+ <orig_name>` line (below) says the ENTRY WAS TAKEN; this
// per-row first-call line says the BODY RAN. A promoted row that only ever prints the first is a
// seam that installed but went unentered in that run -- the acceptance done_when needs both lines.
//
// WHY THE G21 SELF-REACH IN sim_bldg_network_critical.cpp IS LEFT ALONE. That TU's one outward call
// (`llm_strat_bldg_power_network_recompute`) is reached through its `calls` struct's
// `bldg_power_network_recompute` thunk -- i.e. still the ORIGINAL game function, not a promoted
// sim_resid sibling (bldg_power_network_recompute is not itself one of the 32 verified rows). This is
// deliberate, not a gap this file should close: per Law 4, a callee shared with code outside this wave
// stays original until ITS OWN row is verified and promoted -- promoting the caller does not obligate
// promoting what it calls. The thunk self-heals the day a later wave promotes
// `llm_strat_bldg_power_network_recompute`: nothing here needs to change, because the call already goes
// through `mh::call::` to the GAME'S entry point, and installing that function's own
// `MH_EXPORT_REPLACE` seam redirects every caller (this one included) without this file's knowledge.
//
#pragma once

namespace mh::sim {

// True once install_promotion_resid has installed at least one of the 32 seams this run. Mirrors
// mh::orders::promotion_active()'s shape: a shadow arm over any of these entries would compare against
// an original that is no longer installed, so a caller deciding whether shadow-arming sim_resid is safe
// this run should check this first.
bool resid_promotion_active();

// Reads `[promote] sim_resid` (falling back to `default_on` when the key or the file is absent) and,
// if armed, installs all 32 verified sim_resid seams (skipping any `[promote_skip] <orig_name>=1`
// row). Returns the number actually installed. Logs a REFUSED line per failed install (an entry-byte
// guard mismatch -- the DLL was built against a different image), a PARTIAL warning if the installed
// count falls short of the attempted count, and on full success a summary line naming the domain and
// the installed count, followed by one `; [promote] sim_resid: + <orig_name>` line per installed row
// -- the promoted set is named, not just counted.
int install_promotion_resid(int default_on);

} // namespace mh::sim
