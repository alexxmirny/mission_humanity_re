//
// lockstep/resync.cpp -- the five functions declared in resync.h (RI-CUTOVER / C8-c).
// Reviewed by the reimpl-verify adversarial pass (zero divergences) -- see the header's top comment,
// especially the "deliberately NOT bit-equivalent" section covering sync_delay_stub.
//
// Translated from tmp/c8c/llm_net_lockstep_sync_delay_stub_0049c02c.asm,
// tmp/c8c/llm_net_lockstep_sync_busywait_0049e6f5.asm,
// tmp/c8c/llm_net_lockstep_count_active_players_0049e3ea.asm,
// tmp/c8c/llm_net_lockstep_is_local_leader_peer_0049e653.asm and
// tmp/c8c/llm_net_lockstep_force_resync_0049d9d6.asm -- NOT from the .c beside each.
//
#include "lockstep/resync.h"
#include "lockstep/lt_player_by_side_id.h"     // LIB-TRANS-P direct edge
#include "sim/libtrans/sim_lt_menu_teardown.h" // LIB-TRANS-P direct edge

#include <cmath>

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_export.gen.h"     // MH_EXPORT_REPLACE -- the promotion seams (C8-c wiring, at the tail)
#include "lockstep/internal_call.h" // MH_INTERNAL_CALL -- C8-d, the intra-closure edges
#include "lockstep/turn_engine.h"   // PLAYER_ALIVE / PLAYER_HUMAN / MAX_PLAYERS -- one definition
#include "lockstep/tx_emit.h"       // send_lockstep_extend, the production entry point
#include "lockstep/tx_emit_ctrl.h"  // lockstep_broadcast_resync_state, likewise
#include "state/host_api.h"

namespace mh::lockstep {

const resync_calls &live_resync_calls() {
    static const resync_calls cc = {
        MH_INTERNAL_CALL(llm_strat_player_by_side_id, mh::lockstep::player_by_side_id),
        // C8-d: both of these are OURS (tx_emit_ctrl / tx_emit), so the edge goes direct.
        MH_INTERNAL_CALL(llm_net_lockstep_broadcast_resync_state,
                         mh::lockstep::lockstep_broadcast_resync_state),
        MH_INTERNAL_CALL(llm_net_send_lockstep_extend, mh::lockstep::send_lockstep_extend),
        mh::host().ticks_ms,
        MH_INTERNAL_CALL(llm_menu_force_return_to_main, mh::sim::menu_force_return_to_main),
        // OUR stub, not the raw thunk -- the departure from precedent is argued at the field's
        // declaration in resync.h. Short version: this edge is what carries resync_wait_fix, and
        // routing it through the original would reintroduce the MP hang the moment C8-e deletes
        // that patch.
        detail::sync_delay_stub,
    };
    return cc;
}

namespace {

// The inert set. Each stub is chosen so that a shadow run CANNOT come back clean for the wrong
// reason -- see the note on ticks_ms in particular, which is the one that could have.
void inert_broadcast_resync_state(double) {}
void inert_send_lockstep_extend(double) {}
void inert_menu_force_return_to_main() {}

// The real clock IS safe to call in the shadow arm: llm_time_get_ticks_ms is a GetTickCount wrapper
// whose only side effect is bumping _G_LLM_TIME_TICK_QUERY_COUNT (0x00e654e8), a diagnostic counter
// no compared region contains. It is left REAL rather than pinned because force_resync's deadline is
// derived from it, and a pinned clock would make the two arms agree on a value neither would produce
// in the live game -- a clean verdict that proves nothing. The cost is that RESYNC_DEADLINE_MS can
// differ between arms by the microseconds between the two calls; that is real, and it is why the
// shadow entry for force_resync declares the deadline region and expects it to be noisy rather than
// silent. (Recorded here rather than discovered from a red run.)

} // namespace

const resync_calls &inert_resync_calls() {
    static const resync_calls cc = {
        MH_INTERNAL_CALL(llm_strat_player_by_side_id,
                         mh::lockstep::player_by_side_id), // pure lookup, safe to run for real
        inert_broadcast_resync_state,                      // ESCAPE: wire
        inert_send_lockstep_extend,                        // ESCAPE: wire
        mh::host().ticks_ms,                               // real, see the note above
        inert_menu_force_return_to_main,                   // ESCAPE: session teardown
        detail::sync_delay_stub,                           // returns 0 -> the spin below exits immediately
    };
    return cc;
}

namespace detail {

// ---- 1. llm_net_lockstep_sync_delay_stub @0x0049c02c ---------------------------------------------
//
// The original is `MOV EAX,[EBP-0x28]` over a local nothing ever wrote -- a stub whose return value
// is whatever happened to be on the stack. We return 0, which is `original + resync_wait_fix`. The
// full argument is in resync.h; the one line worth repeating at the code is that this is the ONLY
// intentional behavioural difference in this module, and it is intentional in BOTH directions --
// reproducing the garbage faithfully would be reproducing a hang.
int32_t sync_delay_stub() { return 0; }

// ---- 2. llm_net_lockstep_sync_busywait @0x0049e6f5 -----------------------------------------------
//
// `d = sync_delay_stub(); if (d < 0) { teardown; return_to_main; return 0; }
//  start = ticks(); while (ticks() < start + d) ; return 1`
//
// THE SPIN COMPARE IS UNSIGNED. The original is `CMP EAX,[EBP-0x1c]` followed by `JC` (jump if
// carry = unsigned below), not `JL`. That matters at the 49.7-day GetTickCount wrap: the unsigned
// compare keeps working across it for any sane duration, a signed one would spin for ~24 days.
// Ghidra's .c renders both operands as `uint` and so happens to agree here, but the guarantee comes
// from the JC, not from the decompile.
//
// The negative-duration branch is UNREACHABLE with our stub (0 is not < 0) and is translated anyway:
// it is reachable in the ORIGINAL, so the shadow arm needs it to compare, and a future change to the
// duration source must find the abort path already here rather than have to notice it is missing.
int32_t sync_busywait(const resync_calls &calls) {
    const int32_t delay = calls.sync_delay();
    if (delay < 0) {
        // (llm_teardown_hook_stub ran here in the original -- an empty body, not reproduced; see
        // resync.h)
        calls.menu_force_return_to_main();
        return 0;
    }
    const uint32_t start  = calls.ticks_ms();
    const uint32_t target = start + static_cast<uint32_t>(delay);
    while (calls.ticks_ms() < target) {
        // Deliberately empty, exactly as the original: no PeekMessage, no yield. Reproducing the
        // *absence* of a message pump matters -- the game's window goes "not responding" during this
        // wait, and a reimplementation that helpfully pumped would change observable behaviour and,
        // worse, could re-enter llm_wnd_proc from inside a held critical section.
    }
    return 1;
}

// ---- 3. llm_net_lockstep_count_active_players @0x0049e3ea ----------------------------------------
//
// Recompute, not a getter: it zeroes _G_LLM_NET_ACTIVE_PLAYER_COUNT, counts slots 0..7 that are
// ALIVE && HUMAN, and returns the same global it just wrote.
//
// THE GLOBAL IS THE ACCUMULATOR, not a local that gets stored at the end -- the original increments
// `[0x005d54c4]` in place inside the loop (`INC dword ptr [0x005d54c4]`). Faithful here for a reason
// that is not stylistic: any observer that reads the count while this is running (it is called from
// the dispatch RX path) sees a partial value in the original and must see one here too.
int32_t count_active_players(const resync_state &st) {
    *st.active_count = 0;
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        if ((st.players[i].status_flags & PLAYER_ALIVE) != 0 &&
            (st.players[i].status_flags & PLAYER_HUMAN) != 0) {
            *st.active_count += 1;
        }
    }
    return *st.active_count;
}

// ---- 4. llm_net_lockstep_is_local_leader_peer @0x0049e653 ----------------------------------------
//
// Stall-recovery leader election, so peers do not all force a resync at once. Resolve the passed
// side_id to a player index, find the FIRST slot 0..7 that is ALIVE && HUMAN && not that index, and
// answer whether that winner is us. No such slot -> 0.
//
// TWO THINGS THE SHAPE OF THE ASM MAKES EXPLICIT AND A PARAPHRASE WOULD LOSE:
//   * the scan STOPS at the first qualifying slot (the loop breaks, it does not keep looking for
//     the local slot). So this is "is the lowest-numbered other participant us", not "are we a
//     participant" -- a subtly different and much weaker predicate.
//   * exclude_side_id is a SIDE id, not a slot index, and callers meaning "exclude nobody" pass -1.
//     player_by_side_id(-1) returns a non-index, so the `!= idx` test then excludes nothing, which
//     is how that convention works. We call it unconditionally, as the original does, rather than
//     short-circuiting on -1 -- the callee is not pure enough to assume (it walks the roster) and a
//     skipped call is exactly the kind of "harmless" optimisation that turns into a divergence.
int32_t is_local_leader_peer(const resync_state &st, const resync_calls &calls, int32_t exclude_side_id) {
    const int32_t excluded = calls.player_by_side_id(exclude_side_id);
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        if ((st.players[i].status_flags & PLAYER_ALIVE) != 0 &&
            (st.players[i].status_flags & PLAYER_HUMAN) != 0 && i != excluded) {
            return (i == *st.local_slot) ? 1 : 0;
        }
    }
    return 0;
}

// ---- 5. llm_net_lockstep_force_resync @0x0049d9d6 ------------------------------------------------
//
// Fire a mid-game lockstep rebaseline. One-shot on RESYNC_IN_PROGRESS.
//
// THE TRIGGER-COUNT RESET IS UNCONDITIONAL AND COMES FIRST. `MOV dword ptr [0x00e58791],0x0` is at
// 0x0049d9ee, BEFORE the `CMP [0x00e587ad],1` at 0x0049d9f8. So a call that finds a resync already
// in progress still clears the counter and returns. That ordering is load-bearing -- it is why the
// counter is described as "reset only here and at match start" in the spurious-resync analysis --
// and it is the easiest thing in this function to get wrong by tidying the early-out to the top.
//
// THE DEADLINE ARITHMETIC is `trunc((double)ticks + 2000.0 + 2.0)` stored as int64, of which the low
// 32 bits are kept. Both constants are read from .rdata rather than written as literals. The x87
// original computes in 80-bit; every intermediate here is an integer well under 2^53, so double is
// exact and the two agree bit for bit. The int64->uint32 narrowing reproduces the original's
// `FISTP qword` + `MOV EAX,[low dword]`, which is a wrap, not a saturate -- relevant only near the
// GetTickCount rollover, where it must wrap.
void force_resync(const resync_state &st, const resync_calls &calls) {
    *st.resync_trigger_count = 0;
    if (*st.resync_in_progress == 1) return;
    *st.resync_in_progress = 1;

    calls.broadcast_resync_state(2.0); // PUSH 0x40000000 / PUSH 0x0 = the double 2.0

    const uint32_t now      = calls.ticks_ms();
    const double   deadline = std::trunc(static_cast<double>(now) + *st.resync_timeout_ms + *st.resync_pad_ms);
    *st.resync_deadline_ms  = static_cast<uint32_t>(static_cast<uint64_t>(deadline));

    *st.horizon = *st.game_clock + *st.step_size;
    calls.send_lockstep_extend(*st.horizon);
}

} // namespace detail
} // namespace mh::lockstep


namespace mh::lockstep {

int32_t sync_delay_stub() {
    // C8-d: the liveness counter is at the PRODUCTION ENTRY, not the entry thunk -- see the long
    // note at turn_engine.cpp's production entry points. A seam reached only from inside the
    // closure never touches its entry once the internal edges are direct.
    promoted_resync::live(0, "resync/sync_delay_stub");
    return detail::sync_delay_stub();
}
int32_t sync_busywait() {
    promoted_resync::live(1, "resync/sync_busywait");
    return detail::sync_busywait(live_resync_calls());
}
int32_t count_active_players() {
    promoted_resync::live(2, "resync/count_active_players");
    return detail::count_active_players(live_resync_state());
}
int32_t is_local_leader_peer(int32_t exclude_side_id) {
    promoted_resync::live(3, "resync/is_local_leader_peer");
    return detail::is_local_leader_peer(live_resync_state(), live_resync_calls(), exclude_side_id);
}
void force_resync() {
    promoted_resync::live(4, "resync/force_resync");
    detail::force_resync(live_resync_state(), live_resync_calls());
}

} // namespace mh::lockstep


// ---- promotion -------------------------------------------------------------------------------------
//
// PROVE OUR CODE ACTUALLY RAN. Same rule and same shape as turn_engine's and tx_emit's counters:
// log the FIRST call and then widen. It matters more here than anywhere else in the closure, because
// three of these five are EXPECTED to be rare -- force_resync fires ~never on an idle LAN (the
// recorded determinism-oracle finding), and a zero-call seam is indistinguishable from a clean one
// in every log the rig produces.
namespace mh::lockstep::promoted_resync {

// Exactly five, sized by hand like the neighbours so adding a seam without adding a slot is a
// visible edit rather than a silent overwrite.
long g_calls[5];

// NOT `inline`: since C8-d the production entry points call this too, from this and sibling TUs.
void live(int slot, const char *name) {
    const long n = ++g_calls[slot];
    if (n == 1 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        mh::lockstep::say("; [promote] %s: call #%ld (OURS is live)\n", name, n);
}

// clang-format off
int32_t sync_delay_stub()                  { return mh::lockstep::sync_delay_stub(); }
int32_t sync_busywait()                    { return mh::lockstep::sync_busywait(); }
int32_t count_active_players()             { return mh::lockstep::count_active_players(); }
int32_t is_local_leader_peer(int32_t side) { return mh::lockstep::is_local_leader_peer(side); }
void    force_resync()                     { mh::lockstep::force_resync(); }
// clang-format on

} // namespace mh::lockstep::promoted_resync

// clang-format off
MH_EXPORT_REPLACE(llm_net_lockstep_sync_delay_stub,      mh::lockstep::promoted_resync::sync_delay_stub)
MH_EXPORT_REPLACE(llm_net_lockstep_sync_busywait,        mh::lockstep::promoted_resync::sync_busywait)
MH_EXPORT_REPLACE(llm_net_lockstep_count_active_players, mh::lockstep::promoted_resync::count_active_players)
MH_EXPORT_REPLACE(llm_net_lockstep_is_local_leader_peer, mh::lockstep::promoted_resync::is_local_leader_peer)
MH_EXPORT_REPLACE(llm_net_lockstep_force_resync,         mh::lockstep::promoted_resync::force_resync)
// clang-format on

namespace mh::lockstep {

// The non-static wrappers turn_engine.cpp's table drives. One line each, no logic -- any decision
// about WHETHER to install belongs to the single gate over there.
bool install_seam_sync_delay_stub() { return mh_export_install_llm_net_lockstep_sync_delay_stub(); }
bool install_seam_sync_busywait() { return mh_export_install_llm_net_lockstep_sync_busywait(); }
bool install_seam_count_active_players() {
    return mh_export_install_llm_net_lockstep_count_active_players();
}
bool install_seam_is_local_leader_peer() {
    return mh_export_install_llm_net_lockstep_is_local_leader_peer();
}
bool install_seam_force_resync() { return mh_export_install_llm_net_lockstep_force_resync(); }


long resync_promotion_calls(int slot) {
    return (slot >= 0 && slot < 5) ? promoted_resync::g_calls[slot] : -1;
}

} // namespace mh::lockstep
