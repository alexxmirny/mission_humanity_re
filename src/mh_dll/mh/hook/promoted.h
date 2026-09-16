//
// hook/promoted.h -- which game functions are currently PROMOTED, and what that forbids (C1).
//
// A promoted function has had its entry overwritten with a JMP to our C++ body, so every byte of the
// original body from entry to end is dead code. A byte patch aimed anywhere inside one therefore
// writes bytes that will never execute -- and, crucially, `patch_bytes_guarded` cannot tell: the
// expected bytes still match (we only rewrote the first 8), the write still succeeds, and the arming
// line still says "armed". The fix is inert and the log says otherwise. That silence is the whole
// bug this file exists to break.
//
// Two halves meet here. `install_export` reports every successful promotion in (note_promoted), and
// `patch_bytes_guarded` asks before every write (promoted_owner_of). Neither knows about the other;
// both know about this.
//
// Layering: `hook/` carries no feature knowledge and cannot reach for a generated address header, so
// the extent table is INSTALLED into it once at init (set_owner_table) exactly the way the logger is.
// Until that happens promoted_owner_of answers "no owner" -- which is the pre-C1 behaviour, i.e. a
// build that forgets to install the table degrades to exactly what it did before, not to something
// new and wrong.
//
#pragma once
#include <cstdint>

namespace mh::hook {

// One promotable function's body extent. `end` is INCLUSIVE (the last byte of the body).
//
// `domain` is the migration domain the row belongs to -- "sim", "tact", "ai", "orders", "lockstep",
// "save", "seams" -- taken mechanically from the owning TU's `mh/<domain>/...` path by
// gen_dll_patches.py, never from a hand table. It exists so a coverage claim can be made PER DOMAIN:
// a tombstone report that only totals says "364 armed, 0 hits" whether one domain contributed 364 and
// the other 0, and a domain contributing zero is precisely the hole the instrument is meant to find
// (that is the same shape one level up). nullptr where the row's origin is not a domain
// TU -- the generated dead-body table and the selftest fixtures both leave it unset, and every reader
// must treat nullptr as "unattributed" rather than as a domain name.
struct owner_range {
    const char *name;
    uintptr_t   entry;
    uintptr_t   end;
    const char *domain;
};

// One SECTION-5 RESIDUE row: a `verified` migration-ledger row whose ORIGINAL body stays reachable
// from host code ON PURPOSE, so X-TOMB's force-arm paths must SKIP it (X-SPINE (B), 2026-09-10).
// Table: mh::addr::tombstone_residue_exclusions, generated from
// tools/data/reconciliation_hostreach_residue.json -- the same file the promotion reconciliation's
// full `--check` reads, so an exclusion cannot outlive the adjudication that justifies it.
//
// WHY IT CARRIES ITS REASON AND NOT JUST ITS ADDRESS. The instrument's discipline is that a claim
// states itself: the armed set reports its size and its names, an empty set says so in words, a
// refusal names the function. An exclusion is the one thing that makes the armed set SMALLER, so it
// is the one that most needs saying out loud -- a silent skip and a body nobody thought to arm
// produce identical logs. Every skip prints `cls` and `why`.
struct residue_row {
    const char *name;
    uintptr_t   entry;
    const char *cls; // the disposition class (downstream_of_named_residue, register_hazard, ...)
    const char *why; // the one-line reason, as written in the adjudication file
};

// One registered byte patch. `carrier` says how the fix survives promotion of `owner` --
// "migrated:<ini key>" for a branch in our reimplemented body, or nullptr when the owner is not
// promotable and the question does not arise. Declared here rather than beside the generated table so
// hook/ owns the type it reports on.
struct patch_decl {
    const char *id;
    uintptr_t   addr;
    const char *owner;
    const char *carrier;
};

// Install the promotable-function extent table (mh::addr::promotable_ranges). Call once at init.
void set_owner_table(const owner_range *tbl, int count);

// Report, per registered patch whose owner is promoted in THIS run, which implementation carries the
// fix. Call once after the patches have had their chance to arm. Answers the question a suppression
// line alone cannot: not just "that patch did not land" but "and here is what covers it instead".
void report_interlock(const patch_decl *tbl, int count);

// Install the sink that suppression reports go through (the net seam's seam_log). A suppressed patch
// that logs nothing would be a NEW silence in place of the old one.
void set_promotion_logger(void (*fn)(const char *));

// Record that `entry` now runs our implementation. Called by install_export on success.
void note_promoted(uintptr_t entry);

// ---- one owner per entry point (C4) -------------------------------------------------------------
//
// Several functions we want to promote ALREADY carry a DLL trampoline doing seam work -- the pacing /
// adaptive / rx_spin / graceful_drop detour on llm_strat_time_tick is armed at ship defaults. Two
// writers at one entry means whichever patches second refuses, and which one that is depends on
// install order rather than on intent. Worse, the refusal reads "entry bytes differ (wrong build, or
// already hooked)" -- a wrong-build message emitted by a perfectly good build.
//
// The model, and it is a rule rather than a workaround: an entry has exactly ONE owner. Promoting a
// function that already has a detour means REBINDING WHAT THAT DETOUR FALLS THROUGH TO, so the detour
// keeps its seam work and only the destination changes. A second entry patch is then not a race to be
// won but a category error, and this registry is what lets install_export say so by name.

// Claim `entry` for `owner` (a short human description of the detour). Call when the detour arms.
void note_entry_owner(uintptr_t entry, const char *owner);

// Who owns `entry`, or nullptr if it is unclaimed and a direct entry patch is legitimate.
const char *entry_owner_of(uintptr_t entry);

// SIM1-P clause 6: record a claim, saying whether the claimant is willing to SHARE the entry
// (entry_claim::rebind) or demanded it outright (::exclusive). note_entry_owner is the exclusive form.
void note_entry_claim(uintptr_t entry, const char *owner, bool shared);

// ANY claimant of this entry, shared or exclusive, or nullptr. What decides whether a rebindable row
// must yield its libmh binding: a rebound caller calls our body DIRECTLY and so reaches no entry hook
// of either kind. entry_owner_of deliberately answers only for exclusive claims, because that is what
// detour_refusal means by "already owns it".
const char *entry_claimant_of(uintptr_t entry);

// Enumerate the claim table. Exists so the harness can DERIVE which rebindable rows must yield their
// binding instead of being handed a hand-kept list of names (SIM1-P clause 6, the G106 shape), and so
// that a claimed entry matching NO row can be REPORTED rather than silently ignored -- the half that
// makes the derivation trustworthy, since "the list is empty" and "the derivation is broken" otherwise
// look identical.
int  entry_claim_count();
bool entry_claim_at(int i, uintptr_t *entry, const char **owner, bool *shared);

// ---- U30: the refusals themselves, counted and named -------------------------------------------
//
// C1 and C4 each made ONE kind of collision loud, and both did it the same way: a line at the moment
// of refusal. That is necessary and it is not sufficient, and the reason is arithmetic -- an MP run
// writes a ~2 MB mh_net.log, so one line saying a default-ON fix disarmed itself is indistinguishable
// from the fix having worked. That is exactly how `[net] sync_gameover` (default 1) shipped OFF in
// every run since it was written: the effects gate took llm_ui_outcome_dialog's entry at T, the
// lockstep detour printed "NOT armed (unexpected prologue)" at T+61 ms, and nobody read line 61
// (U30).
//
// So the refusals accumulate here and are stated ONCE, enumerated, at the end of arming -- the same
// shape as report_interlock's "%d promotion(s) live, %d registered fix(es) displaced by them", which
// is the line that made the BYTE-PATCH half of this legible. A run with nothing refused says so
// affirmatively: silence is what U30 was made of, so silence is not an available answer.

// Why an install refused. THREE reasons, deliberately never conflated -- each demands a different
// action from whoever reads the log, and merging any two of them reproduces G68 (a detour collision
// reported as a wrong build).
enum class refuse_reason {
    none,        // not a refusal -- the install may proceed
    promoted,    // the target is inside a body PROMOTED in this run (C1/C9): carry the fix in our body
    entry_owned, // another DLL detour already holds this entry (C4): hand it over, or rebind
    // F4C-COMP / Q6: an IN-MEMORY STATIC PATCH already holds bytes this install would kill -- either
    // the whole body (a jmp install makes every byte after the entry dead) or the entry window a
    // trampoline steals. Distinct from `promoted` even though the consequence is the same shape,
    // because the ACTION is different: a promoted body means carry the fix in our C++ body, whereas
    // this means drop the manifest from the compile list, or give up the detour -- the two fixes
    // live in different files and merging the reasons would send the reader to the wrong one.
    patched,
    prologue, // the entry bytes are not what this site was generated against: wrong build / stray hook
    // U31: the two ways the PRIMITIVES themselves can fail after the adjudication says yes. They were
    // bare `return false`s with no line and no record -- the only installs in the tree that could fail
    // without saying anything, which is the exact silence the three reasons above exist to end. Kept
    // distinct from each other and from `prologue` because they mean something different again: not a
    // collision and not a wrong build, but the OS declining, which points at the environment.
    alloc_failed,   // VirtualAlloc could not obtain the 64-byte executable thunk
    protect_failed, // VirtualProtect could not make the entry writable -- nothing was patched
};

// Record one refused install. `who` is the name the summary will print; nullptr degrades to the same
// honest-but-unhelpful default claim_entry uses. Idempotent per (target, reason).
void note_entry_refusal(uintptr_t target, const char *who, refuse_reason why);

// Emit the ONE enumerated summary line: how many installs were refused and which sites, by name and
// address, with the reason for each. Call once, AFTER everything has had its chance to arm.
void report_entry_refusals();

// How many refusals have been recorded. For a caller that wants the count without the line.
int entry_refusal_count();

// ---- F4C-COMP: bodies an IN-MEMORY STATIC PATCH holds bytes inside (the Q6 composition rule) -----
//
// THE PROBLEM Q6 RULES ON. 11,035 of the 46,294 static-patch sites in src/patcher fall inside a
// function some other mechanism claims -- 10,127 inside a promotable body, 4,438 inside a body the
// DLL detours at its entry (tools/check_inmem_composition.py re-derives both from committed inputs,
// and llm_strat_order_queue_dispatch alone carries 3,281 of them). Per-manifest mutual exclusion
// cannot be AUTHORED at that scale: it would be a hand table with 619 rows for one manifest, kept in
// step with a generator by memory. So the applier REGISTERS instead, and the claimants ASK -- the
// same two halves, and the same reason, as note_promoted / promoted_owner_of one section above.
//
// WHAT A REGISTRATION MEANS depends on what the later claimant does to the body, and the distinction
// is measured rather than assumed:
//   * a JMP install (install_jmp, and so every promotion) redirects the entry, so EVERY byte of the
//     body is dead afterwards -- a manifest site anywhere inside it is inert. REFUSED.
//   * a TRAMPOLINE install (install_trampoline) steals `stolen` entry bytes and the body still runs
//     to its end, so only a site inside [entry, entry + stolen) is a real collision. Anything past
//     that window COMPOSES, and refusing it would disarm a working detour for no reason (U30's
//     failure shape). ALLOWED -- with the registration standing, so the composition is stated rather
//     than lucky. This is not a hypothetical class: `no_cd_EN` patches +0x2f into
//     llm_cd_locate_and_open_audio while seams/standalone.cpp trampolines that function's entry, and
//     the two have composed correctly, unremarked, since F1E.
//
// `entry_window` is the widest steal any install in this tree takes (install_jmp writes 8; every
// install_trampoline call site passes 8; the byte neuter writes 3), so a body whose sites all sit
// outside its first 8 bytes cannot collide with ANY entry install -- which is what makes the
// registration answerable at BODY granularity instead of needing every site extent in a table.

// Record that an in-memory manifest wrote bytes inside `name` (entry..end INCLUSIVE). `manifest` is
// the name a refusal will print; `hits_entry_window` is true when some site of that manifest lands
// in the body's first `entry_window_bytes()`. Idempotent per entry; a second manifest over the same
// body keeps the first name and ORs the window flag, because the reader needs to know a collision
// exists, not which of two manifests to blame first.
void note_patched_body(uintptr_t entry, uintptr_t end, const char *name, const char *manifest,
                       bool hits_entry_window);

// The manifest holding bytes inside the body that contains `addr`, or nullptr. `fn_out` (optional)
// receives the function's name. Answers the JMP-install question: everything in the body dies.
const char *patched_body_of(uintptr_t addr, const char **fn_out = nullptr);

// The manifest holding bytes in the ENTRY WINDOW of the body whose entry is exactly `entry`, or
// nullptr. Answers the TRAMPOLINE question: only the stolen bytes are contested.
const char *patched_entry_window_of(uintptr_t entry, const char **fn_out = nullptr);

// The widest entry steal any install in this tree takes. Exposed so a test asserts the constant
// rather than restating it, and so a future wider steal has one place to change.
int entry_window_bytes();

// How many distinct bodies the registry above can hold. PUBLIC since fork F4C-GATE, because the
// compile list stopped being one manifest: every generated manifest header carries a
// `static_assert` of its own body count against this, so widening the list past what the table can
// register is a BUILD error naming the manifest rather than a run-time "TABLE FULL" line in a lane
// nobody is reading. The worst shipped manifest today registers 530 of these 1024 slots
// (grand_all_caphike_storagecap_EN); the table costs 20 KB of static data.
inline constexpr int MAX_PATCHED_BODIES = 1024;

// How many bodies are registered, and the ONE enumerated line that states them -- affirmatively when
// there are none, for the reason report_entry_refusals() states its own empty case.
int  patched_body_count();
void report_patched_bodies();

// The name of the promoted function whose body contains `addr`, or nullptr if there is none.
// nullptr is the answer both when no function owns the address and when its owner is not promoted --
// the caller only ever needs to distinguish "may write" from "may not".
const char *promoted_owner_of(uintptr_t addr);

// How many promotions have been recorded. Used by the arming report so a run can state, in writing,
// whether the interlock had anything to interlock against.
int promoted_count();

// Clear the promoted / owned-entry / refusal / patched-body registries. TEST-ONLY: the registries are
// process-global and have no unnote in production (a promotion is for the life of the process), so a
// selftest driving several independent arming scenarios in one process must reset between them or a
// stale note_promoted arms a body a later scenario meant to leave alone. Never called in production.
void registry_reset_for_test();

// Copy up to `max` recorded promoted entries into `out`; returns the count copied. X-TOMB reads the
// registry this way to arm tombstones over exactly what THIS run promoted -- the runtime set, not a
// static claim, so an un-promoted function is never armed by accident (that is force_arm's job).
int promoted_entries(uintptr_t *out, int max);

// The extent row whose entry is exactly `entry`, or whose name is `name`; nullptr when absent.
// X-TOMB joins the runtime promoted set against the extent table with these -- the same table
// promoted_owner_of searches, exposed by row so the caller gets the END of the body, not only
// membership.
const owner_range *owner_range_of_entry(uintptr_t entry);
const owner_range *owner_range_by_name(const char *name);

// The installed extent table itself, for a caller that must WALK it rather than look one row up --
// X-TOMB's per-domain force-arm is the only such caller. Returns nullptr (and leaves `count` at 0)
// until set_owner_table has run, which is the same degrade-to-pre-C1 posture promoted_owner_of takes.
const owner_range *owner_table(int *count);

// Emit through the installed sink. Exposed so patch.cpp can report a suppression without hook/
// growing a second logger of its own.
void promotion_log(const char *line);

} // namespace mh::hook
