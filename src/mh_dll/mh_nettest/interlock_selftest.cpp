//
// interlock_selftest.cpp -- the patch/seam interlock's logic, off the rig (reimpl C1/C4, and C9).
//
// WHY A UNIT TEST AND NOT A RIG RUN. Both halves of the interlock are about things NOT happening -- a
// byte that was not written, a second entry patch that was refused. A green run does not demonstrate
// either; it looks exactly like a run where the check was never reached. The rig can show the happy
// path (and does: a promoted run logs every displaced fix by name), but the interesting cases are the
// refusals, and several of them cannot be provoked on the rig at all without deliberately shipping a
// broken configuration.
//
// So the decision function is exercised directly here, including the cases that matter most:
//   * an address inside a promoted body is refused; the SAME address before promotion is allowed
//   * an address inside a KNOWN function that is NOT promoted is allowed (the check must not
//     over-refuse -- a false refusal silently drops a fix, which is the same damage in the mirror)
//   * boundary addresses: entry and end are INSIDE, entry-1 and end+1 are outside
//   * an entry claimed by a detour refuses a second entry patch BY ITS OWN RESULT CODE, distinct from
//     a byte mismatch, because those two need different actions from whoever reads the log
//   * (C9) the same adjudication for the DETOUR primitive, both ways: an exclusive claim onto a
//     promoted body is refused, a `rebind` claim on the same address is allowed. That second half is
//     not a formality -- C4's time_tick detour and C6's sim_tick/sim_step hooks legitimately hold
//     entries their promotion rebinds, and a guard without the escape hatch would break both.
//   * (U30) the WHOLE refusal decision now that the byte compare lives inside the primitive: the
//     three reasons stay three reasons, they are ORDERED so that a contested entry is never reported
//     as a wrong build, `expect_prologue == 0` really does skip the compare, and the run's ONE
//     enumerated summary line names every refusal -- including the affirmative form when there were
//     none, which is the half that stops the whole facility degenerating back into silence.
//
// Deliberately does NOT call patch_bytes_guarded, install_jmp or install_trampoline: those write to
// real addresses. What is under test is the ownership decision, which is the part that can be wrong.
//
#include "hook/detour.h"    // C9: the detour half of the same interlock
#include "hook/hookpoint.h" // D5: the named hook points layered on top of it
#include "hook/promoted.h"

#include "lockstep/internal_call.h" // C8-d: MH_INTERNAL_CALL + the edge-signature identity
#include "lockstep/lt_reload_snapshot_resync.h"
#include "lockstep/lt_time_query.h"
#include "lockstep/resync.h"
#include "lockstep/turn_engine.h" // C5: the seam-subset parser, which is pure and belongs under test
#include "lockstep/tx_emit.h"
#include "lockstep/tx_emit_ctrl.h"
#include "sim/sim_order_dispatch.h" // D5: dispatch_observer() -- proving a registration REACHED the domain
#include "sim/sim_step.h"           // ROOTS-LIVE: the strategic root's two install routes

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

using mh::hook::entry_owner_of;
using mh::hook::note_entry_owner;
using mh::hook::note_promoted;
using mh::hook::owner_range;
using mh::hook::promoted_owner_of;
using mh::hook::set_owner_table;

namespace {

// C8-d. One static_assert per converted edge: our production entry point and the generated
// mh::call:: wrapper must be the SAME function type, or MH_INTERNAL_CALL's unselected arm quietly
// stops compiling. Unevaluated operands throughout -- nothing here is odr-used, nothing links.
#define MH_EDGE_SAME_TYPE(FN, OURS)                                                                 \
    static_assert(std::is_same<decltype(&::mh::call::FN), decltype(&OURS)>::value,                  \
                  "C8-d edge " #FN ": our entry point's signature no longer matches the generated " \
                  "mh::call wrapper -- MH_INTERNAL_EDGES_ENTRY_ROUTED=1 would not compile")

void edge_signature_identity() {
    // turn_engine (4)
    MH_EDGE_SAME_TYPE(llm_net_lockstep_commit_horizon, mh::lockstep::commit_horizon);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_find_horizon_match_side, mh::lockstep::find_horizon_match_side);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_dispatch, mh::lockstep::dispatch);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_reset_player_horizon, mh::lockstep::reset_player_horizon);
    // LT1 wave-2 rebinds (2026-09-02) -- the 19th and 20th edges
    MH_EDGE_SAME_TYPE(llm_game_reload_snapshot_resync_clocks, mh::lockstep::reload_snapshot_resync_clocks);
    MH_EDGE_SAME_TYPE(time_GetCurrentTime, mh::lockstep::time_GetCurrentTime);
    // tx_emit / tx_emit_ctrl -- the wire seams (10)
    MH_EDGE_SAME_TYPE(llm_net_send_lockstep_extend, mh::lockstep::send_lockstep_extend);
    MH_EDGE_SAME_TYPE(llm_net_send_lockstep_keepalive, mh::lockstep::send_lockstep_keepalive);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_broadcast_player_leave, mh::lockstep::lockstep_broadcast_player_leave);
    MH_EDGE_SAME_TYPE(llm_net_player_remove, mh::lockstep::player_remove);
    MH_EDGE_SAME_TYPE(llm_net_player_remove_timeout, mh::lockstep::player_remove_timeout);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_send_slot_reset, mh::lockstep::send_slot_reset);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_send_horizon_ack, mh::lockstep::send_horizon_ack);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_send_horizon_desync, mh::lockstep::send_horizon_desync);
    MH_EDGE_SAME_TYPE(llm_net_send_lockstep_kick, mh::lockstep::send_lockstep_kick);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_broadcast_resync_state, mh::lockstep::lockstep_broadcast_resync_state);
    // resync -- the C8-c residue (4)
    MH_EDGE_SAME_TYPE(llm_net_lockstep_count_active_players, mh::lockstep::count_active_players);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_is_local_leader_peer, mh::lockstep::is_local_leader_peer);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_force_resync, mh::lockstep::force_resync);
    MH_EDGE_SAME_TYPE(llm_net_lockstep_sync_busywait, mh::lockstep::sync_busywait);
}
#undef MH_EDGE_SAME_TYPE

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// Two synthetic ranges rather than the real generated table: the test then states its own premises
// instead of silently inheriting whatever the current build happens to promote, and it keeps saying
// the same thing after the tenth seam is added.
constexpr uintptr_t A_ENTRY = 0x00400100, A_END = 0x004001ff;
constexpr uintptr_t B_ENTRY = 0x00400300, B_END = 0x004003ff;
const owner_range   RANGES[] = {{"fn_a", A_ENTRY, A_END}, {"fn_b", B_ENTRY, B_END}};

// An entry inside NO promotable range -- the plain detour-vs-detour collision, which is the shape
// U30 actually was (an effects gate and a lockstep detour, neither of them a promotion).
constexpr uintptr_t C_ENTRY = 0x00400500;

// U30: the summary is an OUTPUT, and the only way to test an output is to read it. The registry
// already has a logger seam (set_promotion_logger, which production points at seam_log), so the test
// points it at a buffer instead. Testing that report_entry_refusals() *runs* would prove nothing --
// the failure mode being guarded against is a report that is silent or that omits a site, and both
// look exactly like a healthy run from the outside.
char g_cap[4096];
void cap_log(const char *line) {
    const size_t used = strlen(g_cap);
    const size_t n    = strlen(line);
    if (used + n + 1 < sizeof(g_cap)) memcpy(g_cap + used, line, n + 1);
}
void cap_reset() { g_cap[0] = 0; }
bool cap_has(const char *needle) { return strstr(g_cap, needle) != nullptr; }

// ---- U31: the injectable page ops, and why they are worth production carrying a seam for --------
//
// The two paths under test -- VirtualAlloc returning null, VirtualProtect declining a game entry --
// have NEVER been taken by any run in this tree, and cannot be provoked on the rig without shipping
// a deliberately broken environment. An untaken path is exactly where a fix that "obviously works"
// is wrong, and what U31 claims is about the state the primitives LEAVE BEHIND when they fail: the
// caller's trampoline pointer, the thunk, and whether anything got said. None of that is observable
// from a successful install. So detour.cpp routes its three page operations through a table that
// production leaves null, and the test supplies one.
//
// The fake also lets the SUCCESS arm be asserted at all. install_jmp/install_trampoline write E9
// bytes at `target`, so until now interlock_selftest deliberately called neither ("those write to
// real addresses") and tested only the decision in front of them. With a test-owned buffer as the
// target the write is safe, and clause (d)'s "no behaviour change on the success path" becomes a
// check instead of a hope.
uint8_t g_fake_entry[16];
// A SECOND entry, because note_entry_refusal is idempotent per (target, reason) -- deliberately, so
// an installer that runs twice files one fact. install_jmp's protect failure is the same reason as
// install_trampoline's, so aiming both at one address would file one refusal and the second
// primitive's silence would be invisible. Two targets, two records.
uint8_t g_fake_entry2[16];
uint8_t g_fake_thunk[64];
uint8_t g_fake_dest[16];

bool  g_fail_alloc   = false;
bool  g_fail_protect = false;
int   g_n_alloc = 0, g_n_release = 0;
void *g_last_alloc = nullptr, *g_last_release = nullptr;

void *fake_alloc(unsigned int bytes) {
    ++g_n_alloc;
    if (g_fail_alloc || bytes > sizeof(g_fake_thunk)) return nullptr;
    g_last_alloc = g_fake_thunk;
    return g_fake_thunk;
}
void fake_release(void *q) {
    ++g_n_release;
    g_last_release = q;
}
bool fake_protect(void *, unsigned int, unsigned long, unsigned long *old_out) {
    if (old_out) *old_out = 0x40; // PAGE_EXECUTE_READWRITE, the value the real call hands back
    return !g_fail_protect;
}
const mh::hook::page_ops FAKE_OPS = {&fake_alloc, &fake_release, &fake_protect};

// The entry a healthy run must find untouched after a refused install: a byte pattern that is not
// 0xE9 and not a NOP, so "nothing was written" is distinguishable from "something was written".
void arm_fake_entry() {
    memset(g_fake_entry, 0xCC, sizeof(g_fake_entry));
    memset(g_fake_entry2, 0xCC, sizeof(g_fake_entry2));
}
bool untouched(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != 0xCC) return false;
    return true;
}
bool fake_entry_untouched() { return untouched(g_fake_entry, sizeof(g_fake_entry)); }

// D5: two DISTINGUISHABLE callbacks for the hook-point registration checks. Two, because the one
// registration an owner can refuse (sim_step_pre's single-subscriber slot) is only really tested if
// the rejected second candidate is a different pointer from the accepted first -- re-offering the
// same one would pass a registry that silently overwrote.
void noop_observer() {}
void noop_observer2() {}

// mp:D32's counting arm: stands in for mh::desync's step++ without pulling in the whole live module
// (install()/session_reset() need an ini, a manifest and an armed transport -- machinery unrelated to
// the thing under test, which is a REGISTRATION decision, not the detector's own bookkeeping).
int  g_pre_hook_calls = 0;
void count_pre_hook_call() { ++g_pre_hook_calls; }

} // namespace

int run_interlocktest() {
    printf("== interlock selftest (C1 patch/seam interlock + C4 entry ownership) ==\n");
    set_owner_table(RANGES, 2);

    // ---- BEFORE any promotion, nothing is refused. This is the "promotion off leaves behaviour
    // unchanged" half of C1's acceptance, asserted rather than assumed.
    ck(promoted_owner_of(A_ENTRY) == nullptr, "un-promoted: entry of fn_a is writable");
    ck(promoted_owner_of(A_ENTRY + 8) == nullptr, "un-promoted: mid-body of fn_a is writable");
    ck(promoted_owner_of(B_ENTRY + 8) == nullptr, "un-promoted: mid-body of fn_b is writable");

    // ---- promote fn_a ONLY.
    note_promoted(A_ENTRY);

    const char *o = promoted_owner_of(A_ENTRY + 8);
    ck(o != nullptr, "promoted: a mid-body address is refused");
    ck(o != nullptr && o[3] == 'a', "promoted: the refusal names the RIGHT function (fn_a)");

    // Boundaries. `end` is INCLUSIVE, so it must be inside; one past it must not be. An off-by-one
    // here fails in the dangerous direction -- the last instruction of a promoted body is exactly
    // where a tail-patch would sit.
    ck(promoted_owner_of(A_ENTRY) != nullptr, "promoted: the entry itself is inside");
    ck(promoted_owner_of(A_END) != nullptr, "promoted: the last body byte is inside");
    ck(promoted_owner_of(A_ENTRY - 1) == nullptr, "promoted: one byte BEFORE the entry is outside");
    ck(promoted_owner_of(A_END + 1) == nullptr, "promoted: one byte AFTER the end is outside");

    // MUST NOT OVER-REFUSE. fn_b is a known function that is not promoted; refusing a patch there
    // would silently drop a fix that was perfectly good -- the same damage as the bug, mirrored.
    ck(promoted_owner_of(B_ENTRY) == nullptr, "not promoted: fn_b's entry is still writable");
    ck(promoted_owner_of(B_ENTRY + 8) == nullptr, "not promoted: fn_b's body is still writable");

    // An address in no known function is not refused either -- most of the image is not promotable and
    // the data patches (format strings, option tables) live there.
    ck(promoted_owner_of(0x00500000) == nullptr, "unknown address: not refused");

    // Promoting the same entry twice must not double-count or corrupt the table.
    note_promoted(A_ENTRY);
    ck(promoted_owner_of(A_ENTRY + 8) != nullptr, "idempotent: re-promoting keeps the refusal");

    // ---- C4: entry ownership is a SEPARATE question from promotion.
    ck(entry_owner_of(B_ENTRY) == nullptr, "unclaimed entry has no owner");
    note_entry_owner(B_ENTRY, "test_detour");
    const char *w = entry_owner_of(B_ENTRY);
    ck(w != nullptr, "claimed entry reports an owner");
    ck(w != nullptr && w[0] == 't', "the owner is reported BY NAME, not as a bare flag");
    ck(entry_owner_of(B_ENTRY + 8) == nullptr, "ownership is per-ENTRY, not per-body");
    ck(entry_owner_of(A_ENTRY) == nullptr, "claiming one entry does not claim another");

    // And ownership must not imply promotion: an entry can be owned by an instrument while the
    // function still runs the original, which is precisely the shipped pre-C4 state of time_tick.
    ck(promoted_owner_of(B_ENTRY) == nullptr, "an OWNED entry is not thereby a PROMOTED one");

    // ---- C9: the SAME question for the detour primitive, which had no answer at all until now.
    // This is the gap D17 fell through: `patch_bytes_guarded` asked promoted_owner_of, install_jmp /
    // install_trampoline asked nothing, and the caller's `== WATCOM_PROLOGUE` guard failed for the
    // right reason while reporting the wrong one ("already hooked?").
    // Driving the DECISION rather than the installer, for the reason at the top of this file: the
    // installers write to real addresses, and what can be wrong here is the adjudication.
    using mh::hook::detour_would_be_displaced;
    using mh::hook::entry_claim;
    ck(detour_would_be_displaced(A_ENTRY, entry_claim::exclusive),
       "C9: a detour onto a PROMOTED entry is refused");
    ck(detour_would_be_displaced(A_ENTRY + 8, entry_claim::exclusive),
       "C9: refusal is per-BODY, not just the entry (a mid-body detour is dead code too)");
    // The mirror failure is as bad and much quieter: over-refusing silently drops a fix that was
    // never in conflict. B is a known function that nothing promotes.
    ck(!detour_would_be_displaced(B_ENTRY, entry_claim::exclusive),
       "C9: a detour onto an UNPROMOTED function is allowed (no over-refusal)");
    ck(!detour_would_be_displaced(A_ENTRY - 1, entry_claim::exclusive),
       "C9: one byte BEFORE a promoted body is outside it");
    ck(!detour_would_be_displaced(0x00500000, entry_claim::exclusive),
       "C9: an address in no known function is not refused");
    // And the escape hatch has to actually open, or C4/C6 stop working: time_tick's pacing detour and
    // the harness's sim_tick/sim_step hooks legitimately hold entries their promotion rebinds.
    ck(!detour_would_be_displaced(A_ENTRY, entry_claim::rebind),
       "C9: entry_claim::rebind is allowed onto a promoted entry (the C4/C6 protocol)");

    // ---- C9(c): the MIRROR. Everything above stops a detour landing on a promoted body; this is
    // what stops a PROMOTION landing on an entry a detour already holds. It had exactly one
    // registrant (time_tick, by an explicit call), so everywhere else install_export reported
    // "entry bytes ... (wrong build, or already hooked)" -- G68 with the roles swapped, and it cost
    // two measured runs: the order recorder vs [promote] sim_dispatch, and replay_suppress_enqueue
    // turning [promote] orders into "8/9 seams installed -- PARTIAL, treat this run as invalid".
    //
    // The decision under test is claim_entry's, driven through the same public registry
    // install_export consults. B is un-promoted, so these say nothing about promotion.
    ck(entry_owner_of(B_ENTRY + 0x10) == nullptr, "C9(c): an unclaimed entry has no owner");
    note_entry_owner(B_ENTRY + 0x10, "an exclusive test detour");
    const char *dw = entry_owner_of(B_ENTRY + 0x10);
    ck(dw != nullptr, "C9(c): an EXCLUSIVE detour's entry reports an owner");
    ck(dw != nullptr && dw[0] == 'a', "C9(c): the owner is a NAME the refusal can print, not a flag");
    // Re-claiming the same entry must not consume a second slot -- ~45 detours now register into a
    // fixed table, and a leak here would fill it and silently stop refusing (the table-full line in
    // promoted.cpp exists for the same reason).
    note_entry_owner(B_ENTRY + 0x10, "a second claim on the same entry");
    const char *dw2 = entry_owner_of(B_ENTRY + 0x10);
    ck(dw2 != nullptr && dw2[0] == 'a', "C9(c): re-claiming one entry keeps it claimed (last wins)");
    ck(entry_owner_of(B_ENTRY + 0x14) == nullptr, "C9(c): claiming one entry does not claim its neighbour");

    // ---- D5: THE ENUMERATION API, which nothing tested until now ---------------------------------
    //
    // `entry_claim_count` / `entry_claim_at` are what harness.cpp walks to derive SIM1-P clause 6's
    // rebind yield -- "this row's original entry is claimed by an instrument, so its libmh binding
    // must be turned OFF or the instrument stops seeing the calls". Everything else in this file
    // tested the LOOKUP side (entry_owner_of / entry_claimant_of), and the derivation does not use
    // it: it walks the table. So the half that decides whether a determinism run is valid had no
    // check at all, and the failure it would produce is the quiet one -- a walk that yields nothing
    // and a walk over an empty table read identically (G106's shape, and exactly why the derivation
    // reports BOTH directions in the log).
    //
    // Driven with two claims of DIFFERENT kinds, because the asymmetry is the point: the derivation
    // must yield for a SHARED claim too (a rebound caller reaches no entry hook of either kind), and
    // entry_owner_of deliberately cannot see one.
    {
        using mh::hook::entry_claim_at;
        using mh::hook::entry_claim_count;
        using mh::hook::entry_claimant_of;
        using mh::hook::note_entry_claim;

        const uintptr_t E_EXCL   = B_ENTRY + 0x20; // an exclusive claim
        const uintptr_t E_SHARED = B_ENTRY + 0x24; // a ::rebind claim (shared)
        const int       before   = entry_claim_count();
        ck(before > 0, "D5: the claim table is non-empty here -- the walk below is not vacuous");

        note_entry_owner(E_EXCL, "an exclusive instrument");
        note_entry_claim(E_SHARED, "a rebinding instrument", true);
        ck(entry_claim_count() == before + 2, "D5: two new entries add exactly two enumerable rows");

        // Walk the table the way harness.cpp does and JOIN back to the lookup API. If these two ever
        // disagree the derivation yields the wrong row, which is worse than yielding none.
        bool saw_excl = false, saw_shared = false;
        bool join_ok = true;
        for (int i = 0; i < entry_claim_count(); ++i) {
            uintptr_t   e     = 0;
            const char *who   = nullptr;
            bool        share = true;
            ck(entry_claim_at(i, &e, &who, &share), "D5: every index below the count enumerates");
            if (entry_claimant_of(e) != who) join_ok = false;
            if (e == E_EXCL) {
                saw_excl = (who && who[0] == 'a' && !share);
            } else if (e == E_SHARED) {
                saw_shared = (who && who[0] == 'a' && share);
            }
        }
        ck(saw_excl, "D5: an EXCLUSIVE claim enumerates with its name and shared=false");
        ck(saw_shared, "D5: a SHARED (::rebind) claim enumerates too, flagged shared");
        ck(join_ok, "D5: every enumerated row agrees with entry_claimant_of -- one table, one answer");

        // ...and the asymmetry the derivation depends on. entry_owner_of answers only for exclusive
        // claims, so a yield derived from IT would silently skip every rebind-claimed row -- which is
        // the population C4/C6 exists for (time_tick, sim_tick, sim_step, land_players).
        ck(entry_owner_of(E_SHARED) == nullptr, "D5: a shared claim has no EXCLUSIVE owner...");
        ck(entry_claimant_of(E_SHARED) != nullptr, "D5: ...but it IS a claimant, so the walk sees it");

        // Bounds. The walk is `for (i = 0; i < count; ++i)`, so an off-by-one here writes through
        // uninitialised out-params into a log line -- or, in the yield loop, compares garbage against
        // a real row address.
        uintptr_t   e_oob = 0xdeadbeefu;
        const char *w_oob = (const char *)0x1;
        bool        s_oob = true;
        ck(!entry_claim_at(entry_claim_count(), &e_oob, &w_oob, &s_oob),
           "D5: index == count does not enumerate");
        ck(!entry_claim_at(-1, &e_oob, &w_oob, &s_oob), "D5: a negative index does not enumerate");
        ck(e_oob == 0xdeadbeefu && w_oob == (const char *)0x1 && s_oob,
           "D5: ...and a refused enumeration writes NOTHING through its out-params");

        // Every out-param is optional -- documented in promoted.h, never exercised.
        ck(entry_claim_at(0, nullptr, nullptr, nullptr), "D5: all three out-params may be null");

        // Re-claiming must not grow the table: ~45 detours register into a fixed array, and a leak
        // fills it, drops later claims, and the derivation then silently stops yielding.
        const int n_before_reclaim = entry_claim_count();
        note_entry_owner(E_EXCL, "the same entry, claimed again");
        ck(entry_claim_count() == n_before_reclaim,
           "D5: re-claiming an entry does NOT add a row (the table cannot leak)");
    }

    // ---- U30: the whole refusal decision, and the line that reports it ---------------------------
    //
    // C9 moved the PROMOTION half of this question into the primitive and left the byte compare at
    // ~20 call sites, each writing `*(uint32_t *)ADDR == PROLOGUE && install_trampoline(...)`. The
    // `&&` short-circuits, so on a contested entry the primitive is never reached and the site's own
    // else-branch names the only cause its author imagined -- "unexpected prologue", i.e. the build.
    // That is how `[net] sync_gameover` (default ON) spent its entire shipped life disarmed: an
    // effects gate held llm_ui_outcome_dialog's entry, and the log said wrong build.
    //
    // So the compare moved in, and the decision below is what the primitives now ask. Driving the
    // DECISION rather than the installer, for the reason at the top of this file.
    using mh::hook::detour_refusal;
    using mh::hook::entry_refusal_count;
    using mh::hook::note_entry_refusal;
    using mh::hook::refuse_reason;
    using mh::hook::report_entry_refusals;
    using mh::hook::set_promotion_logger;

    set_promotion_logger(&cap_log);

    // THE AFFIRMATIVE LINE, ASSERTED FIRST -- before anything has filed a refusal, which is the only
    // moment it can be observed. A summary that speaks only when something is wrong reproduces U30
    // one level up: an absent line is not evidence of a clean run, it is no evidence at all.
    cap_reset();
    report_entry_refusals();
    ck(cap_has("0 detour install(s) refused"),
       "U30: a run with NOTHING refused still says so, affirmatively and in one line");
    ck(entry_refusal_count() == 0, "U30: ...and the count agrees with the line");

    // Two real, dereferenceable addresses, so the byte-compare arm can be driven without writing to
    // anything. `probe_bytes` deliberately is NOT a Watcom prologue.
    volatile uint32_t probe_bytes = 0x11223344u;
    volatile uint32_t probe_owned = 0x55667788u;
    const uintptr_t   P_BYTES     = (uintptr_t)&probe_bytes;
    const uintptr_t   P_OWNED     = (uintptr_t)&probe_owned;
    const uint32_t    PRO         = mh::hook::WATCOM_PROLOGUE;

    // (iii) A byte mismatch on an entry NOBODY claims is a byte mismatch -- the check must not
    // over-refuse into an ownership story it has no evidence for.
    ck(detour_refusal(P_BYTES, entry_claim::exclusive, PRO) == refuse_reason::prologue,
       "U30: wrong entry bytes on an unclaimed entry -> prologue MISMATCH");
    ck(detour_refusal(P_BYTES, entry_claim::exclusive, 0x11223344u) == refuse_reason::none,
       "U30: matching bytes are not a refusal");

    // expect_prologue == 0 = NO BYTE EXPECTATION. This opt-out is not a convenience: llm_rand has no
    // Watcom frame at all, utils_abort steals nine bytes and guards on an exact eight, and every
    // generated-entry8 site (effects, shadow, install_export) already checks something strictly
    // stronger. Handing those a prologue expectation would silently disarm working hooks.
    ck(detour_refusal(P_BYTES, entry_claim::exclusive, 0) == refuse_reason::none,
       "U30: expect_prologue == 0 skips the byte compare entirely");
    // ...and `rebind` is exempt from ownership, NOT from bytes. Whoever you are, the bytes are the
    // bytes: a promoter writing over an entry that does not hold what it compiled against is a bug
    // in every claim.
    ck(detour_refusal(P_BYTES, entry_claim::rebind, PRO) == refuse_reason::prologue,
       "U30: entry_claim::rebind is NOT exempt from the byte compare");
    ck(detour_refusal(P_BYTES, entry_claim::rebind, 0) == refuse_reason::none,
       "U30: ...and with no byte expectation a rebind claim is clear");

    // (ii) THE DOUBLE CLAIM -- U30's own shape. One detour owns the entry; a second wants it
    // exclusively and must be refused, by ownership, not by a byte compare.
    ck(detour_refusal(C_ENTRY, entry_claim::exclusive, 0) == refuse_reason::none,
       "U30: an unclaimed, unpromoted entry is free");
    note_entry_owner(C_ENTRY, "the effects gate that got there first");
    ck(detour_refusal(C_ENTRY, entry_claim::exclusive, 0) == refuse_reason::entry_owned,
       "U30: a SECOND exclusive detour on a claimed entry is refused -- the collision U30 was");
    ck(detour_refusal(C_ENTRY, entry_claim::rebind, 0) == refuse_reason::none,
       "U30: entry_claim::rebind still shares a claimed entry (C4/C6 protocol intact)");

    // THE ORDERING, and it is the whole point rather than a detail. A contested entry has an E9
    // written over it, so it fails the byte compare TOO -- ask bytes first and every detour collision
    // in the tree reports itself as a wrong build. That inversion IS G68, and it is what this
    // assertion exists to make unrepeatable.
    note_entry_owner(P_OWNED, "the detour that already holds this entry");
    ck(detour_refusal(P_OWNED, entry_claim::exclusive, PRO) == refuse_reason::entry_owned,
       "U30: an OWNED entry whose bytes ALSO mismatch reports OWNERSHIP, not a byte mismatch (G68)");
    // And promotion outranks ownership, for the same reason: "carry the fix in our body" is a
    // sharper instruction than "hand the entry over".
    note_entry_owner(A_ENTRY, "a detour that claimed a promoted entry");
    ck(detour_refusal(A_ENTRY, entry_claim::exclusive, 0) == refuse_reason::promoted,
       "U30: a PROMOTED body that is ALSO claimed reports the promotion first");

    // THE SUMMARY NAMES THEM. A silent skip must not be reachable, so file one refusal of each kind
    // and read the line back.
    note_entry_refusal(A_ENTRY, "a detour onto a promoted body", refuse_reason::promoted);
    note_entry_refusal(C_ENTRY, "the game-over leave-lockstep detour", refuse_reason::entry_owned);
    note_entry_refusal(P_BYTES, "a detour onto a wrong-build entry", refuse_reason::prologue);
    // Idempotent: the same (site, reason) filed twice is ONE fact, not two. ~45 install sites feed a
    // fixed table and several of them are idempotent installers that run more than once.
    note_entry_refusal(C_ENTRY, "the game-over leave-lockstep detour", refuse_reason::entry_owned);
    ck(entry_refusal_count() == 3, "U30: three distinct refusals, and a repeat is not a fourth");

    cap_reset();
    report_entry_refusals();
    ck(cap_has("3 detour install(s) REFUSED"), "U30: the summary states the COUNT");
    ck(cap_has("the game-over leave-lockstep detour"),
       "U30: the summary NAMES the double-claimed site -- a silent skip is not reachable");
    ck(cap_has("a detour onto a promoted body") && cap_has("a detour onto a wrong-build entry"),
       "U30: ...and names the other two as well");
    // THREE REASONS, THREE WORDS, NO CONFLATION. Each demands a different action from the reader --
    // carry the fix in our body / hand the entry over / check the build -- so a summary that blurred
    // any two of them would be worse than none.
    ck(cap_has("target PROMOTED"), "U30: the promotion reason is named as a promotion");
    ck(cap_has("entry OWNED"), "U30: the ownership reason is named as ownership");
    ck(cap_has("prologue MISMATCH"), "U30: the byte reason is named as a byte mismatch");
    // Exactly one line: the failure this replaces was one line per refusal, 61 ms apart, in a 2 MB
    // log. Anything that scatters again has lost the property.
    {
        int newlines = 0;
        for (const char *p = g_cap; *p; ++p)
            if (*p == '\n') ++newlines;
        ck(newlines == 1, "U30: the whole refusal report is ONE line, not one line per refusal");
    }

    // ---- U31: the two failures the PRIMITIVES themselves can hit ---------------------------------
    //
    // Everything above tests the DECISION in front of an install. These two are the failures AFTER
    // it says yes, and until now they were the only installs in the tree that could fail without
    // saying anything -- a bare `return false` in each primitive. Worse, install_trampoline
    // published *tramp_out BEFORE it owned the entry, so the caller was left holding a live,
    // executable thunk over an UNPATCHED prologue; launch.cpp's installers are idempotent by testing
    // that very pointer (`if (g_tramp) return;`), so a failed install read as a completed one and
    // was never retried. That is the same shape as G68, D17 and U30 itself: a mechanism reporting
    // success while not being installed.
    {
        using mh::hook::install_jmp;
        using mh::hook::install_trampoline;
        using mh::hook::set_page_ops;

        set_page_ops(&FAKE_OPS);
        const uintptr_t F_ENTRY = (uintptr_t)g_fake_entry;

        // (i) ALLOC FAILS. Nothing to roll back, but the caller must still be told, and the pointer
        //     it uses as its "already armed" flag must not move.
        void *tramp = nullptr;
        arm_fake_entry();
        g_fail_alloc   = true;
        g_fail_protect = false;
        g_n_alloc = g_n_release = 0;
        cap_reset();
        bool ok = install_trampoline(F_ENTRY, (void *)g_fake_dest, &tramp, 8,
                                     entry_claim::exclusive, "a detour whose thunk alloc fails", 0);
        ck(!ok, "U31: install_trampoline fails when the thunk cannot be allocated");
        ck(tramp == nullptr, "U31: ...and *tramp_out is UNTOUCHED, so an idempotent caller retries");
        ck(fake_entry_untouched(), "U31: ...and the entry was not written");
        ck(cap_has("a detour whose thunk alloc fails"),
           "U31: ...and the failure is REPORTED by name -- it used to be a silent return false");
        ck(entry_owner_of(F_ENTRY) == nullptr,
           "U31: ...and a failed install claims no entry, so a later install is not refused");

        // (ii) PROTECT FAILS, which is the arm that used to leak. The thunk IS allocated here, so
        //      the rollback has something to do and both halves are checkable.
        arm_fake_entry();
        g_fail_alloc   = false;
        g_fail_protect = true;
        g_n_alloc = g_n_release = 0;
        g_last_alloc = g_last_release = nullptr;
        cap_reset();
        ok = install_trampoline(F_ENTRY, (void *)g_fake_dest, &tramp, 8, entry_claim::exclusive,
                                "the game-over leave-lockstep detour, protect refused", 0);
        ck(!ok, "U31: install_trampoline fails when the entry cannot be made writable");
        ck(tramp == nullptr,
           "U31: ...and *tramp_out is STILL untouched -- the leak this item was opened for");
        ck(g_n_alloc == 1 && g_n_release == 1 && g_last_release == g_last_alloc,
           "U31: ...and the thunk it had already allocated is freed, exactly once");
        ck(fake_entry_untouched(), "U31: ...and the entry was not written");
        ck(cap_has("the game-over leave-lockstep detour, protect refused"),
           "U31: ...and this failure is reported by name too");

        // (iii) THE SUCCESS ARM IS UNCHANGED. A fix whose failure path is beautiful and whose happy
        //       path stopped working is a worse bug than the leak.
        arm_fake_entry();
        g_fail_protect = false;
        g_n_alloc = g_n_release = 0;
        ok                      = install_trampoline(F_ENTRY, (void *)g_fake_dest, &tramp, 8, entry_claim::exclusive,
                                                     "a detour that installs cleanly", 0);
        ck(ok, "U31: a clean install still succeeds");
        ck(tramp == g_fake_thunk, "U31: ...and NOW the trampoline pointer is published");
        ck(g_n_release == 0, "U31: ...and nothing is freed on the success path");
        ck(g_fake_entry[0] == 0xE9, "U31: ...and the entry really was patched");
        ck(g_fake_thunk[0] == 0xCC && g_fake_thunk[7] == 0xCC,
           "U31: ...and the thunk holds a copy of the ORIGINAL 8 stolen bytes");
        ck(entry_owner_of(F_ENTRY) != nullptr, "U31: ...and a successful install claims the entry");

        // (iv) install_jmp's protect failure, the same silence in the other primitive. On its own
        //      unclaimed entry: (iii) owns the first one, and the refusal registry keys on
        //      (target, reason), so reusing that address would report the ownership check instead of
        //      the OS declining -- a different fact, and not the one under test here.
        arm_fake_entry();
        g_fail_protect = true;
        cap_reset();
        ok = install_jmp((uintptr_t)g_fake_entry2, (void *)g_fake_dest, entry_claim::exclusive,
                         "a whole-body replace whose protect fails", 0);
        ck(!ok, "U31: install_jmp fails when the entry cannot be made writable");
        ck(untouched(g_fake_entry2, sizeof(g_fake_entry2)), "U31: ...and it wrote nothing");
        ck(cap_has("a whole-body replace whose protect fails"),
           "U31: ...and it says so -- install_jmp's protect failure was silent as well");

        // THE SUMMARY CARRIES THEM, with reasons distinct from U30's three. "The OS declined" needs a
        // different response from "another mechanism owns this entry", so it must not read as either.
        g_fail_protect = false;
        set_page_ops(nullptr);
        cap_reset();
        report_entry_refusals();
        ck(cap_has("thunk ALLOC FAILED") && cap_has("entry PROTECT FAILED"),
           "U31: both new failures reach the ONE enumerated summary, under their own reasons");
        ck(cap_has("a detour whose thunk alloc fails") &&
               cap_has("a whole-body replace whose protect fails"),
           "U31: ...naming the sites, so a reader knows which fix is not in the run");
    }

    set_promotion_logger(nullptr);

    // ---- C5: the seam-subset parser. Per-seam gating is a DIAGNOSTIC facility, and every way a subset
    // can quietly become something other than what its author wrote is a way a bisect run gets read as
    // a ship verdict.
    using mh::lockstep::seam_count;
    using mh::lockstep::SEAM_NAMES;
    namespace D = mh::lockstep::detail;

    const int N = seam_count();
    // Nine seams install through install_promotion's table; TWO arrive by rebinding an existing detour's
    // fall-through instead of competing for the entry -- time_tick onto the pacing detour (C4) and
    // sim_tick onto the determinism harness's (C6). This count is deliberately a hard assertion: the
    // vocabulary and the install table are one list precisely so a name cannot be valid in the parser
    // and unknown at the install site, and a silent drift between them is how a subset run promotes
    // something other than what its author wrote.
    // 35 since NET-SESSION (2026-09-01) added session_globals_reset and wait_screen_frame to the
    // table -- the two original netcode writers SB-NET and SB-BOOT could not except.
    ck(N == 35, "thirty-five seam names (33 installed + time_tick and sim_tick by rebind)");
    ck(strcmp(SEAM_NAMES[N - 1], "wait_screen_frame") == 0,
       "NET-SESSION's leader resync frame is now the last seam name");
    ck(strcmp(SEAM_NAMES[N - 2], "session_globals_reset") == 0,
       "NET-SESSION's session bootstrap sits beside it");
    // The merge is only real if the wire vocabulary is reachable from THIS list -- one list is the
    // whole invariant, and a half-merge would leave `lockstep_seams=send_buf_flush` a valid-looking
    // token that the install table has never heard of.
    {
        bool found_wire = false;
        for (int i = 0; i < N; ++i)
            if (strcmp(SEAM_NAMES[i], "send_buf_flush") == 0) found_wire = true;
        ck(found_wire, "a wire seam is nameable in the ONE seam vocabulary (C8-d merge)");
    }

    // SIZE THE MASK FROM A STATED CAP, AND ASSERT IT. This array used to be `bool m[16]` against a
    // list of 11, and when C8-c took the vocabulary to 17 the overrun crashed this test before it
    // could print a single line -- no output at all, which reads as a tooling oddity rather than as
    // the failure it is. The identical bug existed in install_promotion (fixed there as MASK_CAP).
    // Twice in one change is enough to make the buffer state its own contract.
    constexpr int MASK_CAP = 64;
    ck(N <= MASK_CAP, "the seam list still fits the selection mask");
    bool m[MASK_CAP];
    char bad[64];
    if (N > MASK_CAP) { // refuse to run the parser cases rather than smash the stack proving it
        printf("%d checks, %d failures\n", g_checks, g_fails);
        return 1;
    }

    // ABSENT and EMPTY-STRING both mean the shipping default. That is the pre-existing meaning of
    // `lockstep=1` and changing it would silently alter every archived run's config.
    ck(D::parse_seam_subset(nullptr, m, N, bad, sizeof(bad)) == D::subset_result::full, "null = full set");
    ck(m[0] && m[N - 1], "full set selects the first and last seam");
    ck(D::parse_seam_subset("", m, N, bad, sizeof(bad)) == D::subset_result::full, "empty string = full set");

    // A valid subset selects EXACTLY what it names -- no more (a stray extra would over-promote) and no
    // fewer (a missing one would under-promote); both are silent.
    ck(D::parse_seam_subset("pump,dispatch", m, N, bad, sizeof(bad)) == D::subset_result::ok, "valid subset");
    {
        int n_on = 0;
        for (int i = 0; i < N; ++i)
            if (m[i]) ++n_on;
        ck(n_on == 2, "a two-name subset selects exactly two seams");
    }
    ck(m[5] && m[6], "pump,dispatch selects pump and dispatch");
    ck(!m[0] && !m[9], "and selects nothing else");

    // Whitespace and case are the two things a human types wrong without meaning anything by it.
    ck(D::parse_seam_subset("  PUMP , dispatch  ", m, N, bad, sizeof(bad)) == D::subset_result::ok,
       "whitespace and case are tolerated");
    ck(m[5] && m[6], "...and select the same two seams");

    // THE TYPO CASE. This used to match nothing and silently narrow the closure.
    ck(D::parse_seam_subset("pump,dispach", m, N, bad, sizeof(bad)) == D::subset_result::unknown_token,
       "a misspelled seam is REPORTED, not ignored");
    ck(bad[0] == 'd' && bad[6] == 'h', "the refusal names the offending token verbatim");
    {
        int n_on = 0;
        for (int i = 0; i < N; ++i)
            if (m[i]) ++n_on;
        ck(n_on == 0, "a refused subset selects NOTHING -- refuse, never narrow");
    }

    // PREFIX must not match: a near-miss that promotes something the author did not name is the same
    // class of silent narrowing, wearing the opposite sign.
    ck(D::parse_seam_subset("commit", m, N, bad, sizeof(bad)) == D::subset_result::unknown_token,
       "a PREFIX of a real seam name is not a match");
    ck(D::parse_seam_subset("commit_horizon_2", m, N, bad, sizeof(bad)) == D::subset_result::unknown_token,
       "a real seam name with a suffix is not a match either");

    // THE EMPTY-SUBSET CASE, which is the L1-P P0 lesson verbatim: an asymmetric run passed with the
    // promotion never installed. Present-but-selecting-nothing must be distinct from absent.
    ck(D::parse_seam_subset(",,", m, N, bad, sizeof(bad)) == D::subset_result::empty,
       "a list that selects nothing is EMPTY, not the full set");
    ck(D::parse_seam_subset("  ,  ", m, N, bad, sizeof(bad)) == D::subset_result::empty,
       "...whitespace between separators does not rescue it");

    // Duplicates are harmless but must not double-count into "selected", which is what distinguishes
    // ok from empty.
    ck(D::parse_seam_subset("pump,pump", m, N, bad, sizeof(bad)) == D::subset_result::ok, "duplicates are ok");
    ck(D::parse_seam_subset("time_tick", m, N, bad, sizeof(bad)) == D::subset_result::ok,
       "time_tick is a nameable seam (it arrives by rebind, but the vocabulary is one list)");
    ck(m[9] && !m[5], "...and naming it selects only it");

    // ---- C8-d: the intra-closure edges (lockstep/internal_call.h) --------------------------------
    //
    // MH_INTERNAL_CALL binds an edge DIRECT by default and back through the original entry under
    // MH_INTERNAL_EDGES_ENTRY_ROUTED=1, so that a future shadow campaign over the closure interior
    // has a way in. The whole scheme rests on ONE property: for every converted edge, our production
    // entry point and the generated mh::call:: wrapper have the SAME function type. If that ever
    // stops holding, the arm nobody is currently compiling stops compiling -- and it would be found
    // years later, by the session that needed it.
    //
    // The static_asserts below pin that property BY NAME for all 18 edges, in every build, in both
    // arms. They are unevaluated operands, so they cost nothing and link against nothing. A rename
    // or a signature change on either side is a build failure right here, with the edge named.
    edge_signature_identity();

    // And the selection itself, on one representative edge: the macro must yield OUR function in the
    // default build and the generated wrapper under the flag. Asserting the value rather than merely
    // that it compiles is the difference between testing the mechanism and testing the syntax.
    //
    // AND IT SAYS WHICH ARM IT COMPILED FOR. Both arms pass the identical check count -- they must,
    // that is the point -- so the pass count is NOT evidence that the flag took effect. (The literal
    // number that used to stand here went stale twice; read it off the run.) Without this line, building with
    // MH_INTERNAL_EDGES_ENTRY_ROUTED=1 and building without it produce indistinguishable output, and
    // a mis-plumbed define would read as a successful verification of the arm nobody actually built.
    {
        printf("  C8-d internal edges: %s\n",
               MH_INTERNAL_EDGES_ENTRY_ROUTED ? "ENTRY-ROUTED (MH_INTERNAL_EDGES_ENTRY_ROUTED=1)"
                                              : "DIRECT (default / ship)");
        void (*bound)() = MH_INTERNAL_CALL(llm_net_lockstep_force_resync, mh::lockstep::force_resync);
#if MH_INTERNAL_EDGES_ENTRY_ROUTED
        ck(bound == &mh::call::llm_net_lockstep_force_resync,
           "MH_INTERNAL_EDGES_ENTRY_ROUTED=1 routes the edge through the ORIGINAL ENTRY");
        ck(bound != &mh::lockstep::force_resync, "...and not to our body");
#else
        ck(bound == &mh::lockstep::force_resync, "by default an internal edge binds OUR body DIRECTLY");
        ck(bound != &mh::call::llm_net_lockstep_force_resync,
           "...and not through the original entry address");
#endif
    }

    // ---- ROOTS-LIVE: the DOMAIN ROOTS have two install routes, and exactly one may take ------------
    //
    // Each root (llm_strat_sim_step, llm_tact_frame) can now become live either by rebinding a
    // harness detour or -- when nothing owns its entry -- by a direct entry patch. The routes are
    // tried in that order in one if/else, so today they cannot both run; this is the guard that keeps
    // that true when someone edits reimpl_probe, and it belongs here rather than in a domain oracle
    // because it is an ENTRY-OWNERSHIP decision, the same subject as everything above.
    //
    // WHY THE ORDER OF THE CALLS BELOW MATTERS AND CANNOT BE REVERSED. The direct route is only
    // exercised in its REFUSING direction. Taking it for real means mh_export_install_* writing an
    // E9 over 0x0043f512 / 0x00429b1a -- real game addresses that do not exist in this process. The
    // guard is checked BEFORE that write, so "already promoted -> 0" is testable and "not yet
    // promoted -> installs" is not, off the rig. That asymmetry is the point: the refusing direction
    // is the one that can be silently wrong, and the installing direction is what every no-harness
    // rig run demonstrates by logging the route it took.
    {
        using mh::sim::install_promotion_sim_step_direct;
        using mh::sim::register_promotion_sim_step_rebound;
        using mh::sim::sim_step_promoted;
        using mh::sim::sim_step_promotion_reset_for_test;

        sim_step_promotion_reset_for_test();
        ck(!sim_step_promoted(), "ROOTS-LIVE: sim_step starts unpromoted");
        ck(register_promotion_sim_step_rebound() == 1,
           "ROOTS-LIVE: the sim_step REBIND route takes on a free root");
        ck(sim_step_promoted(), "ROOTS-LIVE: ...and the root then reports itself LIVE");
        ck(register_promotion_sim_step_rebound() == 0,
           "ROOTS-LIVE: a SECOND rebind registration is refused -- one owner, one install");
        ck(install_promotion_sim_step_direct() == 0,
           "ROOTS-LIVE: the DIRECT route refuses a root the rebind already took (and refuses BEFORE "
           "touching the entry -- reaching mh_export_install_* here would patch a real game VA)");
        sim_step_promotion_reset_for_test();
        ck(!sim_step_promoted(), "ROOTS-LIVE: the test reset clears the one-owner flag");
    }
    // ---- THE TACT_FRAME ROUTE CASES ARE GONE (fork F2E) ------------------------------------------
    //
    // Five checks stood here, the tactical twin of the sim_step block above: the rebind route takes
    // on a free root, a second registration is refused, the direct entry patch refuses a root the
    // rebind already owns, and the registry holds exactly ONE entry for it so the served count
    // cannot double. They are deleted rather than re-specified because what they governed is
    // deleted: tactical mode is demoted permanently, and both routes onto llm_tact_frame's entry
    // went with libmh/tact/tact_promote.{h,cpp}. A one-owner rule over zero owners arbitrates nothing.
    //
    // The sim_step block above is the SAME mechanism over the root that survives, so the one-owner
    // contract is still tested here. What left is a second instance of it, not the rule.

    // ---- D5: THE NAMED HOOK POINTS (hook/hookpoint.h) ---------------------------------------------
    //
    // The registry that took the harness off the raw primitives. What can be WRONG here is the TABLE
    // and the SHAPE MATCH, not the install: the install is the same install_jmp/install_trampoline
    // every assertion above already drives, and it is the row handed to it that is new.
    //
    // DELIBERATELY DOES NOT ARM ANYTHING WITH A REAL TARGET, for the reason at the top of this file
    // and the one the ROOTS-LIVE block states: arming `point::sim_step` means writing an E9 over
    // 0x0043f512, a game VA that does not exist in this process. Every point exercised below is
    // either REGISTER-ONLY (target 0, nothing dereferenced) or asked in its REFUSING direction, which
    // is the direction that can be silently wrong -- a shape mismatch that installed anyway would put
    // a trampoline's stolen prologue over a whole-body pin.
    {
        using mh::hook::arm_neuter;
        using mh::hook::arm_observer;
        using mh::hook::arm_replacement;
        using mh::hook::callback_of;
        using mh::hook::entry_bytes_match;
        using mh::hook::point;
        using mh::hook::point_id;
        using mh::hook::point_target;
        using mh::hook::point_who;
        using mh::hook::register_callback;

        // THE TABLE IS COMPLETE AND UNIQUE. A point added to the enum with no row would read past the
        // array and hand install_jmp a garbage target; two points sharing an id would make every
        // report ambiguous. The static_assert in hookpoint.cpp covers the COUNT at build time; this
        // covers the contents, which it cannot.
        int  n_points = (int)point::count_;
        bool ids_ok = true, who_ok = true, ids_unique = true;
        for (int i = 0; i < n_points; ++i) {
            const char *id = point_id((point)i);
            const char *wh = point_who((point)i);
            if (!id || !id[0]) ids_ok = false;
            if (!wh || !wh[0]) who_ok = false;
            for (int j = i + 1; j < n_points; ++j)
                if (strcmp(id, point_id((point)j)) == 0) ids_unique = false;
        }
        ck(n_points > 20, "D5: the point table is populated (the walk below is not vacuous)");
        ck(ids_ok, "D5: every point has a greppable id");
        ck(who_ok, "D5: every point has the human string the interlock summary prints");
        ck(ids_unique, "D5: point ids are unique -- a report can name exactly one row");

        // An out-of-range point answers, and answers HARMLESSLY. The enum is the contract, but a cast
        // from an int (a config value, a loop bound) is the way one gets violated.
        ck(strcmp(point_id(point::count_), "(invalid)") == 0, "D5: an out-of-range point is named so");
        ck(point_target(point::count_) == 0, "D5: ...and has no target to write to");
        ck(!arm_replacement(point::count_, (const void *)&n_points),
           "D5: ...and cannot be armed at all");

        // TARGETS. Everything that hooks an entry has one; a register-only point has none, which is
        // what makes it safe to ask the rest of this block about it.
        ck(point_target(point::sim_step) != 0, "D5: an OBSERVE point carries a target VA");
        ck(point_target(point::pin_rand) != 0, "D5: a REPLACE point carries a target VA");
        ck(point_target(point::order_enqueue) != 0, "D5: the NEUTER point carries a target VA");
        ck(point_target(point::dispatch_observer) == 0,
           "D5: a REGISTER-ONLY point hooks no entry, so it has no target");

        // THE SHAPE MATCH, in its refusing direction. Each of the three arming calls must decline a
        // point of another shape BEFORE it computes an address -- so these are safe to run here, and
        // an arm that leaked through would be a write to a game VA in a unit test.
        ck(!arm_observer(point::pin_rand, (void *)&n_points, (void **)&ids_ok),
           "D5: arm_observer REFUSES a whole-body REPLACE point (no stolen prologue over a pin)");
        ck(!arm_replacement(point::sim_step, (const void *)&n_points),
           "D5: arm_replacement REFUSES an OBSERVE point (the original must keep running)");
        ck(!arm_neuter(point::sim_step), "D5: arm_neuter REFUSES anything but the neuter point");
        ck(!arm_observer(point::dispatch_observer, (void *)&n_points, (void **)&ids_ok),
           "D5: a REGISTER-ONLY point cannot be armed as a detour");
        // ...and the null-argument guards, because a caller that forgot its trampoline slot would
        // otherwise have install_trampoline publish through a null pointer.
        ck(!arm_observer(point::sim_step, nullptr, (void **)&ids_ok), "D5: a null detour is refused");
        ck(!arm_observer(point::sim_step, (void *)&n_points, nullptr),
           "D5: a null trampoline slot is refused");
        ck(!arm_replacement(point::pin_rand, nullptr), "D5: a null replacement body is refused");

        // entry_bytes_match on a point with NO entry8 answers true -- there is nothing to disagree
        // with -- so a caller may ask unconditionally. Asked on a register-only point because the
        // real ones would dereference a game VA.
        ck(entry_bytes_match(point::dispatch_observer),
           "D5: a point with no entry8 guard reports its bytes as matching");

        // REGISTRATION, and the half that matters: it must REACH THE DOMAIN. A registry that stored
        // the pointer and forwarded nothing would pass every check that only reads the registry back
        // -- which is the D17 lesson this file already carries for the observers themselves.
        mh::sim::set_dispatch_observer(nullptr);
        ck(mh::sim::dispatch_observer() == nullptr, "D5: the domain slot starts clear");
        ck(register_callback(point::dispatch_observer, &noop_observer),
           "D5: a register-only point accepts a callback");
        ck(callback_of(point::dispatch_observer) == &noop_observer,
           "D5: ...the registry reports what was registered");
        ck(mh::sim::dispatch_observer() == &noop_observer,
           "D5: ...AND the domain that owns the slot actually received it");
        mh::sim::set_dispatch_observer(nullptr);

        // A non-register-only point refuses a callback rather than silently storing one nothing will
        // ever fire -- the same "refuse, never narrow" rule the seam-subset parser follows.
        ck(!register_callback(point::sim_step, &noop_observer),
           "D5: an ARMABLE point refuses a callback registration");
        ck(callback_of(point::sim_step) == nullptr, "D5: ...and records nothing");

        // THE ONE REGISTRATION AN OWNER CAN REFUSE. mh::sim::set_sim_step_pre_hook takes a single
        // subscriber, only while the root is promoted, and (mp:D32) only via the DIRECT-INSTALL
        // route -- the registry must propagate ALL THREE refusals rather than reporting a
        // subscription the body will never fire.
        {
            using mh::sim::register_promotion_sim_step_rebound;
            using mh::sim::sim_step_promotion_force_direct_for_test;
            using mh::sim::sim_step_promotion_reset_for_test;

            sim_step_promotion_reset_for_test();
            ck(!register_callback(point::sim_step_pre, &noop_observer),
               "D5: sim_step_pre is REFUSED while the root is unpromoted (there is no body to chain)");
            ck(callback_of(point::sim_step_pre) == nullptr,
               "D5: ...and a refused registration is not recorded");

            // mp:D32. Promoted BY REBIND: the harness's own detour still owns the real entry and
            // already calls mh::desync::on_sim_step_hashed unconditionally (harness.cpp on_sim_step,
            // BEFORE the promoted/original branch), so this slot must stay REFUSED here too -- taking
            // it fed the detector twice per step (measured: "2999 steps seen" for a 1500-step run).
            register_promotion_sim_step_rebound();
            ck(!register_callback(point::sim_step_pre, &noop_observer),
               "D32: sim_step_pre is REFUSED when the root is promoted BY REBIND -- the harness's own "
               "detour is already the feeder, chaining here would double it");
            ck(callback_of(point::sim_step_pre) == nullptr,
               "D32: ...and a refused rebind-configuration registration is not recorded");
            sim_step_promotion_reset_for_test();

            // Promoted BY DIRECT INSTALL: no harness owns the entry, so this pre-hook is the ONLY
            // feeder and the slot must accept it -- this is the case the slot exists for.
            sim_step_promotion_force_direct_for_test();
            ck(register_callback(point::sim_step_pre, &noop_observer),
               "D5: ...accepted once the root is promoted BY DIRECT INSTALL");
            ck(!register_callback(point::sim_step_pre, &noop_observer2),
               "D5: ...and a SECOND subscriber is refused, not silently substituted");
            ck(callback_of(point::sim_step_pre) == &noop_observer,
               "D5: ...the first registration survives the refused second");
            sim_step_promotion_reset_for_test();
        }

        // mp:D32. THE COUNTING ARM: with the registration contract above enforcing "one feeder", a
        // simulated run of STEPS sim-steps must be counted exactly STEPS times in EITHER promoted
        // configuration -- never STEPS*2. This is the offline stand-in for the rig clause (a promoted
        // 2-peer determinism run's "steps seen" must equal steps run): it drives the SAME production
        // decision (set_sim_step_pre_hook's accept/refuse) the real net_seams.cpp/harness.cpp call
        // sites key off, with a counter standing in for mh::desync::on_sim_step[_hashed]'s step++.
        //
        // MUTATION RED (verified by hand, not committed as a toggle -- see the mp:D32 session note):
        // with set_sim_step_pre_hook()'s `if (promoted_arm::g_promoted_via_rebind) return 0;` line
        // removed, the REBIND arm below reads STEPS*2 instead of STEPS, because register_callback then
        // ALSO wires the pre-hook that harness.cpp's unconditional feed already counted for. Restoring
        // the line turns it back to STEPS. The DIRECT arm is unaffected either way (it never had a
        // second feeder), which is exactly why only the rebind arm is the discriminating one.
        {
            using mh::sim::register_promotion_sim_step_rebound;
            using mh::sim::set_sim_step_pre_hook;
            using mh::sim::sim_step_promotion_force_direct_for_test;
            using mh::sim::sim_step_promotion_reset_for_test;

            const int STEPS = 1500;

            // REBIND configuration: harness.cpp's on_sim_step feeds unconditionally every step
            // (modelled here as `harness_feeds_unconditionally = true`); the pre-hook chain must add
            // NOTHING on top of it.
            sim_step_promotion_reset_for_test();
            register_promotion_sim_step_rebound();
            g_pre_hook_calls  = 0;
            bool pre_wired    = set_sim_step_pre_hook(&count_pre_hook_call) != 0;
            int  rebind_total = 0;
            for (int i = 0; i < STEPS; ++i) {
                ++rebind_total;                       // harness.cpp's on_sim_step -> mh::desync::on_sim_step_hashed, always
                if (pre_wired) count_pre_hook_call(); // what would ALSO fire if D32 were unfixed
            }
            rebind_total += g_pre_hook_calls; // 0 when correctly refused above
            ck(!pre_wired, "D32: the rebind configuration never wires the pre-hook (checked again)");
            ck(rebind_total == STEPS,
               "D32: the rebind configuration counts each sim step exactly once (not doubled)");
            sim_step_promotion_reset_for_test();

            // DIRECT configuration: no harness, so the pre-hook chain is the ONLY feeder.
            sim_step_promotion_force_direct_for_test();
            g_pre_hook_calls = 0;
            pre_wired        = set_sim_step_pre_hook(&count_pre_hook_call) != 0;
            int direct_total = 0;
            for (int i = 0; i < STEPS; ++i)
                if (pre_wired) count_pre_hook_call(); // the harness is NOT present, so no second term
            direct_total = g_pre_hook_calls;
            ck(pre_wired, "D32: the direct-install configuration wires the pre-hook");
            ck(direct_total == STEPS,
               "D32: the direct-install configuration counts each sim step exactly once");
            sim_step_promotion_reset_for_test();
        }
    }

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
