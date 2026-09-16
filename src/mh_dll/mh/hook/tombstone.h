//
// hook/tombstone.h -- X-TOMB: prove no original body we own is ever entered (the endgame plan
// D-E2; X-TOMB).
//
// A promoted function's body is dead from entry to end -- the C1 interlock already refuses byte
// patches into one on exactly that claim. This is the same claim pointed the other way: if the body
// is dead, then EXECUTION anywhere inside it is a bug by definition -- a caller we missed, a
// dispatch table still holding the original, a redirect that silently un-armed. A NOOP there would
// make the game quietly wrong; a TOMBSTONE (the body filled with INT3, a vectored handler that names
// the function and fails fast) can only pass for the right reason.
//
// Three armed classes, three sources of truth:
//   * PROMOTED bodies -- enumerated at RUNTIME from the note_promoted registry joined against the
//     extent table (set_owner_table). The fill starts at entry+8: the first 8 bytes hold the live
//     E9+NOP redirect (install_jmp/install_export) and must survive. Keying on note_promoted keeps
//     this exactly as honest as the interlock: a rebind-mediated promotion that does not
//     note_promoted (sim_step, tact_frame -- harness detours whose fall-through moved) is NOT
//     filled, because the tree does not claim those bodies dead either.
//   * DEAD bodies -- the generated mh::addr::tombstone_dead_ranges (gen_tombstones.py), every row a
//     migration ledger adjudicated `dead`. Filled whole, entry included. OPT-IN (arm_dead), NOT a
//     ship default, and the reason is a measured semantics gap: a ledger's `dead` claim is
//     "unreached within THAT domain's measured closure", not "never executes in any mode".
//     MEASURED 2026-09-01 -- llm_strat_ai_build_target_list is `dead` in the AI ledger yet runs in a
//     single-player strategic game against the AI opponent, so arming the dead set by default
//     terminated four SP-strategic scenarios. Arming it is therefore a DELIBERATE AUDIT of the dead
//     claims (where a hit is the wanted result), run per investigation, never the shipping config.
//   * FORCE-ARMED bodies -- [tombstone] force_arm=<name,...>: rows of the extent table armed even
//     though NOT promoted this run. This is the NEGATIVE ARM: un-promote a function, force-arm it,
//     and its tombstone must fire by name on the first real call -- a demonstration that the
//     instrument can go red, without which a zero-hit run proves nothing.
//
// The report follows report_entry_refusals' discipline: the armed set's SIZE and NAMES are stated
// once, affirmatively, at the end of arming -- a zero over an empty set is not a pass, so an empty
// armed set says so in words. A hit logs the function name, the faulting address and the return
// address, then fails fast (TerminateProcess) -- the body is trap-filled, so "continue" does not
// exist; a run that hits a tombstone has already executed code we claim never executes.
//
// Layering: like the rest of hook/, this file knows no generated addresses. The dead table is
// INSTALLED by the seams layer (net_seams.cpp), which owns the ini and the generated headers.
//
#pragma once
#include "hook/promoted.h" // owner_range -- the extent row this instrument arms over

#include <cstdint>

namespace mh::hook {

// `residue_row` lives in hook/promoted.h beside owner_range -- the generated tables' shared home,
// which mh_tombstones.gen.h already includes and which must not include this header.

struct tomb_options {
    bool        arm_promoted; // fill the dead remainder (entry+8..end) of every note_promoted body
    bool        arm_dead;     // fill the generated ledger-dead bodies whole (entry..end)
    const char *force_arm;    // comma-separated extent-table names to arm UNPROMOTED (negative arm),
                              // or nullptr. A force-armed body keeps its live entry bytes and is
                              // filled from entry+8, so the original prologue runs INTO the trap.
    // The same negative arm, addressed BY DOMAIN rather than by name: a comma-separated list of
    // owner_range::domain values ("sim,tact"), arming every extent row of each named domain that is
    // not already armed. It exists because the by-name key cannot express this: the list is parsed
    // through a bounded stack buffer, and one domain's names run to several kilobytes, so a
    // whole-domain sweep written by hand would silently TRUNCATE -- an under-armed set reporting
    // itself armed, which is the exact failure mode the instrument is built to expose.
    const char *force_arm_domain;
    // The set the domain sweep walks FIRST: mh::addr::verified_ranges, every migration-ledger row at
    // state `verified`. Without it the sweep can only reach the extent table, whose rows are exactly
    // the functions carrying an MH_EXPORT_REPLACE -- ELEVEN of tact's 98 -- so "force_arm_domain=tact"
    // armed a tenth of the domain and said `armed`. That is the coverage illusion TACT1-P was
    // reopened over (C4, 2026-09-04), and a per-domain zero over a tenth of the set is exactly the
    // pass that proves nothing. Rows in both tables are armed once; the extent pass skips them.
    const owner_range *verified_tbl;
    int                verified_n;
    // SAFE-END caps (mh::addr::tombstone_tail_caps): functions whose flat [entry..end] contains
    // bytes that are NOT exclusively theirs -- a disjoint-body gap holding another function, or a
    // Watcom shared-epilogue tail that live outside code jumps into (the 2026-09-01 arm_dead
    // false hit: llm_strat_ai_build_target_list's last 10 bytes are two LIVE siblings' epilogue).
    // Every arm clamps its high bound to the cap's `end` when the entry has a row here.
    const owner_range *tail_caps;
    int                tail_cap_count;
    // The section-5 residue (mh::addr::tombstone_residue_exclusions). Rows here are skipped by BOTH
    // force-arm paths -- by-name and by-domain -- and by neither promoted nor dead arming, which
    // need no exemption: a residue row is unpromoted by definition and is not a `dead` claim.
    //
    // MEASURED, and this is why the exclusion exists rather than a policy of running the right
    // scenarios: with all four armed, `tombstone_full.ini` passed `res_hud` (986/0) and
    // `tutorial_enter` (985/0) and TERMINATED on `tact_panel` -- the mission load runs the
    // un-promoted `map_LoadPlanetFromDisk`, which calls the original `llm_map_fog_of_war_recompute`,
    // exactly as that row's disposition says it will. A green bought by scenario selection is the
    // shape this ledger keeps having to retract, so the set is narrowed to the claim instead.
    const residue_row *residue;
    int                residue_count;
};

// Arm the tombstones. Call once, AFTER every promotion has had its chance to install (the runtime
// promoted set is read here) and before the arming reports. Returns the number of bodies armed.
// Idempotent per process: a second call is refused with a log line rather than double-filled.
int tomb_install(const owner_range *dead_tbl, int dead_n, const tomb_options &opt);

// The ONE enumerated summary: armed count + names (chunked), skipped count + reasons, and the
// affirmative empty-set line. Call beside report_entry_refusals().
void tomb_report();

int tomb_armed_count();
int tomb_hit_count();

// The name of the armed body whose FILLED extent contains `addr`, or nullptr. This is the lookup
// the vectored handler does on a breakpoint; exposed so the arming DECISION -- which bodies got
// armed, over what extent, with what boundaries -- can be tested off-rig, the way interlock_selftest
// tests the C1 decision without writing a real byte.
const char *tomb_armed_name(uintptr_t addr);

// ---- test seams (interlock_selftest / tombstone_selftest) --------------------------------------
//
// Both halves of this instrument are about things NOT happening -- a body that was armed but never
// entered, a fill that landed on the right bytes. A green rig run cannot demonstrate the arming
// decision, only the absence of a hit, so the decision is exercised directly off-rig.

// Route the trap fill through an injectable op instead of VirtualProtect+memset over a real game
// address. Production leaves it null (the real fill). A test supplies a stub that records (lo,hi)
// and returns true, so the whole arming decision runs against synthetic extents with no real memory
// touched. The same injectability pattern as detour.h's page_ops.
struct tomb_mem_ops {
    bool (*fill)(uintptr_t lo, uintptr_t hi); // CC-fill [lo,hi] inclusive; false = environment refusal
};
void set_tomb_mem_ops(const tomb_mem_ops *ops);

// The default fail handler logs the hit line and TerminateProcess (fail fast -- the body is
// trap-filled, resuming is not a thing). A test installs one that RETURNS, in which case the
// vectored handler declines the exception (EXCEPTION_CONTINUE_SEARCH) so the test's own SEH catches
// it. nullptr restores default.
void set_tomb_fail_handler(void (*fn)(const char *line));

// Reset all instrument state (armed table, skips, hits, the g_installed latch). Test-only -- lets
// one selftest process drive several arming scenarios. Never called in production.
void tomb_reset_for_test();

} // namespace mh::hook
