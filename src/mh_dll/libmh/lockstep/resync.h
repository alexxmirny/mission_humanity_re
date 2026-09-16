//
// lockstep/resync.h -- the five stall-recovery / resync / leader-election functions, reimplemented
// (RI-CUTOVER / C8-c). Written against the disassembly only, then put through the reimpl-verify
// adversarial pass (run wf_05be3bde-b12, batch mode, three lens groups over all six of C8's residue
// functions): ZERO divergences, ZERO suspicions, verdict `equivalent` / `ready_to_arm`. Read that
// with the right weight -- a clean pass on six small functions is a plausible outcome, not proof.
// It clears these bodies for ARMING; mh_nettest's resynctest is the evidence that can go red.
//
// Scope -- five of C8's six residue functions, i.e. the last original code our own net/lockstep
// bodies still call. The sixth (llm_net_send_order) is an emitter and lives in tx_emit_order.h.
//
//   llm_net_lockstep_sync_delay_stub       @0x0049c02c   the busy-wait's duration source
//   llm_net_lockstep_sync_busywait         @0x0049e6f5   the "SYNCHRONIZING" spin
//   llm_net_lockstep_count_active_players  @0x0049e3ea   recompute ACTIVE_PLAYER_COUNT
//   llm_net_lockstep_is_local_leader_peer  @0x0049e653   stall-recovery leader election
//   llm_net_lockstep_force_resync          @0x0049d9d6   fire a mid-game rebaseline
//
// Translated from tmp/c8c/*.asm, NOT from the .c beside each. The two places the decompile is
// actively misleading are called out at the function that has them.
//
// THE STACK PROBE IS DROPPED -- every one of these opens with `PUSH n; CALL assert_stack_capacity
// (0x004cf46f)`, Watcom's stack-touch helper. Settled once in the reimpl-loop skill's "settled
// once" list: it touches zero tracked state regions, so omitting it can never produce a shadow
// divergence. Not reproduced, not re-litigated.
//
// =================================================================================================
// THE ONE PLACE THIS MODULE IS DELIBERATELY *NOT* BIT-EQUIVALENT, and why that is the point
// =================================================================================================
// `sync_delay_stub` in the original returns an UNINITIALISED STACK LOCAL -- `MOV EAX,[EBP-0x28]`
// with no prior write anywhere in the function. That garbage is the wait duration `sync_busywait`
// spins on, and it is the measured root cause of the known MP permanent hang (mh_hang.dmp: ~6.1M ms
// of spin, on the main thread, holding the game CS). The DLL has shipped a byte patch for it since
// 2026-07-10 -- `[net] resync_wait_fix`, DEFAULT ON -- which rewrites the load to `return 0`.
//
// OUR BODY RETURNS 0. So it reproduces `original + resync_wait_fix`, not `original`. That is the
// whole reason C8 pulled this function in: it is what lets the patch be retired (C8-e), and it is
// why the user's call was "reimplement sync_busywait INCLUDING the stub's behaviour" rather than
// the caller alone. Reimplementing only the caller and deleting the patch brings the hang back.
//
// TWO CONSEQUENCES A REVIEWER SHOULD CHECK RATHER THAN ASSUME:
//   * SHADOW. With resync_wait_fix INSTALLED the original arm also returns 0 and the two arms
//     agree. With it OFF they cannot agree, and the divergence is CORRECT -- it is the patch's
//     effect, measured. Do not "fix" a divergence seen in a resync_wait_fix=0 run.
//   * ORDERING. Retiring the patch (C8-e) is legal only once this body is PROMOTED, because until
//     then the original bytes are what runs. C8-e already sequences it that way; docs/reimpl-seam-
//     classes.md §4c is the rule that makes it non-negotiable (a fix whose absence is a hang can
//     never ride the "diagnostic config, degrade gracefully" exception).
//
#pragma once

#include <cstdint>

#include "addr/mh_structs.gen.h"

namespace mh::lockstep {

using player_profile_r = mh::game::mh_llm_strat_player_profile; // 0x740, same table turn_engine uses

// Everything these five read or write, as typed pointers. Bound to the live game by
// live_resync_state(); bound to plain locals by the selftest.
struct resync_state {
    // --- count_active_players / is_local_leader_peer ---
    const player_profile_r *players;      // _G_LLM_STRAT_PLAYERS[8]   (0x00cff060, stride 0x740)
    int32_t                *active_count; // _G_LLM_NET_ACTIVE_PLAYER_COUNT (0x005d54c4) -- WRITTEN
    const int32_t          *local_slot;   // _G_LLM_NET_LOCAL_PLAYER_SLOT   (0x005d55b8)

    // --- force_resync ---
    int32_t      *resync_trigger_count; // _G_LLM_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT (0x00e58791)
    int32_t      *resync_in_progress;   // _G_LLM_NET_RESYNC_IN_PROGRESS            (0x00e587ad)
    uint32_t     *resync_deadline_ms;   // _G_LLM_NET_LOCKSTEP_RESYNC_DEADLINE_MS   (0x00e58c38)
    double       *horizon;              // _G_LLM_STRAT_LOCKSTEP_HORIZON            (0x005d5594)
    const double *game_clock;           // _G_LLM_STRAT_GAME_CLOCK                  (0x005d0198)
    const double *step_size;            // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE          (0x005d55bc)
    // The two .rdata deadline constants, read out of the image rather than hardcoded so a different
    // build cannot silently disagree with us. MEASURED (EN, 2026-07-29): 2000.0 and 2.0, summing to
    // the 2002 ms the function's plate documents.
    const double *resync_timeout_ms; // 0x005017f4 = 2000.0
    const double *resync_pad_ms;     // 0x005017fc = 2.0
};

// The outward calls. Split out so the shadow arm can bind the INERT set: three of these escape the
// region rollback entirely (two hit the wire, two tear the session down), and one -- the busy-wait's
// own clock -- must not be spun twice.
struct resync_calls {
    // is_local_leader_peer: resolve a side_id to a player index. Pure, no rollback concern.
    int32_t (*player_by_side_id)(int32_t side_id);

    // force_resync: BOTH of these put bytes on the wire. INERT in the shadow arm, or every resync
    // and every horizon extension is advertised twice to every peer and the oracle desyncs the
    // match it is watching.
    void (*broadcast_resync_state)(double exec_time);
    void (*send_lockstep_extend)(double horizon);

    // force_resync + sync_busywait: GetTickCount, via the game's own counter-keeping wrapper.
    uint32_t (*ticks_ms)();

    // sync_busywait's abort path. Escapes the rollback -- menu_force_return_to_main destroys
    // the session, which is exactly what must not happen when our body is the thing being tested.
    // (The abort path's other original call, llm_teardown_hook_stub @0x0049bc44, is NOT reproduced
    // since SIMABI-HOOKS 2026-09-10: retail's body is a stack probe and a return, so calling it and
    // not calling it are the same machine state, and the host surface is one entry smaller for it.)
    void (*menu_force_return_to_main)();

    // sync_busywait -> sync_delay_stub.
    //
    // THIS ONE DELIBERATELY DEPARTS FROM THE tx_emit_ctrl PRECEDENT, which binds an intra-closure
    // callee to the RAW game thunk so the callee follows its own promotion state. Here that would
    // be actively unsafe: the pair (busywait, stub) is what carries resync_wait_fix, and C8-e
    // deletes that patch. A raw-thunk binding would mean "our promoted busywait spins on whatever
    // the ORIGINAL stub returns" -- and once the patch is gone that is uninitialised garbage again,
    // i.e. the hang, reintroduced through the one edge nobody would think to check. So
    // live_resync_calls() binds OUR stub. C8-d makes this a direct call anyway; the pointer stays
    // only so the selftest can substitute a duration.
    int32_t (*sync_delay)();
};

// The live bindings.
resync_state        live_resync_state();
const resync_calls &live_resync_calls();
// Shadow-arm bindings: the wire, the teardown and the spin all neutered. See the .cpp for exactly
// what each stub returns and why that choice cannot fake a pass.
const resync_calls &inert_resync_calls();

namespace detail {

// All five take their state and calls as parameters, so the whole module is pure over its state and
// testable without a game -- the reimpl-loop skill's "if a module's logic is pure over its state,
// this is almost always the cheap test". mh_nettest's resynctest is that test.

int32_t sync_delay_stub();
int32_t sync_busywait(const resync_calls &calls);
int32_t count_active_players(const resync_state &st);
int32_t is_local_leader_peer(const resync_state &st, const resync_calls &calls, int32_t exclude_side_id);
void    force_resync(const resync_state &st, const resync_calls &calls);

} // namespace detail

// ---- production entry points ---------------------------------------------------------------------
// detail::* over live_resync_state() / live_resync_calls(). These are what the seams install, and
// what a promoted intra-closure caller calls directly once C8-d converts the call sites.
int32_t sync_delay_stub();
int32_t sync_busywait();
int32_t count_active_players();
int32_t is_local_leader_peer(int32_t exclude_side_id);
void    force_resync();

// ---- seam installation ----------------------------------------------------------------------------
//
// THESE FIVE JOIN THE EXISTING `[promote] lockstep` CLOSURE rather than getting a key of their own.
// The wire emitters got a separate `[promote] wire` key because they are meant to be independently
// switchable from the turn engine (tx_emit.cpp argues it); these are not -- they are the RESIDUE the
// turn engine still calls into, C8-d merges the two halves anyway, and a sixth knob would only add a
// configuration nobody intends to run.
//
// WHY THE INSTALLERS LIVE HERE AND THE TABLE DOES NOT. MH_EXPORT_REPLACE defines a `static`
// installer in ITS OWN translation unit, so the macro and the thing that calls it cannot be split
// across files. But the seam VOCABULARY must stay one list (turn_engine.h SEAM_NAMES) or a name
// becomes valid in the `lockstep_seams=` parser and unknown at the install site. So: the macro and
// a one-line non-static wrapper live here, and turn_engine.cpp's single table drives them. That
// keeps the accounting, the SUBSET/SHIP banner and the REFUSED semantics in one place.
bool install_seam_sync_delay_stub();
bool install_seam_sync_busywait();
bool install_seam_count_active_players();
bool install_seam_is_local_leader_peer();
bool install_seam_force_resync();


// Per-seam liveness, for the same reason turn_engine and tx_emit keep theirs: an asymmetric test
// whose asymmetry evaporates does not fail, it passes. Slot order matches SEAM_NAMES' tail.
// ---- C8-d: the seam liveness counter, declared for the PRODUCTION ENTRY POINTS -------------------
//
// It used to be called only from the `promoted_resync::` entry-thunk wrappers, so it counted calls that
// ARRIVED AT THE ORIGINAL ENTRY ADDRESS. Once the intra-closure edges became direct C++ calls, a seam
// reached only from inside the closure stopped touching its entry -- and read zero in a run where it
// plainly ran. "READ THE LIVENESS LINES BEFORE BELIEVING A PASS" is the standing defence against the
// O3 failure (an asymmetric run that passed with the promotion never installed), and a counter that
// cannot tell "ran" from "was never armed" is not that defence. So the production entries call it,
// and the wrappers are pure forwards. Declared here because the entries live in sibling TUs.
namespace promoted_resync {
void live(int slot, const char *name);
}

long resync_promotion_calls(int slot);

} // namespace mh::lockstep
