//
// fix/resync_clamp.h -- the MP D14/D24 resync-order scheduling arithmetic, owned by NEITHER side.
//
// WHY IT IS ITS OWN HEADER (fork F1A residue R4, relocated at F3D).
//
// These two functions are the one thing the net seam and the reimplemented lockstep closure genuinely
// SHARE. Both compute the exec_time the synthetic `0xf0` CTL_RESYNC_BEGIN order is stamped with, and
// they must compute it IDENTICALLY or the two carriers of one fix disagree on the wire:
//
//   * seams/net_lockstep.cpp's broadcast_resync_state detour applies it when the ORIGINAL
//     llm_net_send_lockstep_keepalive path is live (the `original` configuration, and any brokered
//     run where the site was not promoted);
//   * lockstep/tx_emit_ctrl.cpp applies it inside our own body when it is.
//
// They lived in `libmh/lockstep/turn_engine.h` as `mh::lockstep::detail::`, which made the net seam
// `#include` a closure header and NAME a closure symbol to reach a pure double comparison. F1A's
// enumeration classified that as the hardest config-(1) residue of the nine -- link-INVISIBLE (an
// `inline` is never an undefined external, so the object-symbol mechanism is blind to it) and
// execution-REAL (the detour it serves installs by default, under `[net] resync_order_horizon`,
// in every configuration). Neither hoisting it behind the selector nor guarding it would have been
// honest: the original configuration really does run this arithmetic, and that is correct.
//
// So the FUNCTION moved instead of the call. `mh::fix` is deliberately a neutral namespace: it
// belongs to whoever needs the number, which today is both sides of the fork boundary and tomorrow is
// mh_net.dll alone if the closure ever stops carrying its own copy of the fix.
//
// THE ARITHMETIC IS BYTE-IDENTICAL TO WHAT MOVED. It is determinism-relevant -- the value goes on the
// wire and every peer enqueues it -- so this relocation is a cut-and-paste with no rewrite, no
// `std::max`, no reassociation. lockstest's `test_d14_resync_order_exec_time` (eight cases, incl. both
// NaN directions) and `test_d24_resync_order_barrier` drive these same bodies and were not touched.
//
// `mh::lockstep::detail` re-exports both names with `using`, so the closure's own call sites and the
// two lockstest suites still spell them where they always did -- and a net TU that goes back to
// naming the `mh::lockstep::` spelling is caught by check_net_lockstep_refs as an UNRULED reference
// rather than passing quietly.
//
#pragma once

namespace mh::fix {

// ---- MP D14: the resync-begin order's exec_time ----
// The clamp `llm_strat_order_schedule` applies to every replicated order (`exec_time < HORIZON` ->
// raise it to HORIZON, @0x0046637e), applied to the ONE order that bypasses that function: the
// synthetic `0xf0` CTL_RESYNC_BEGIN record, which the stock game stamps with a hardcoded 2.0.
// See seams/net_lockstep.cpp install_resync_order_horizon for the mechanism and the disassembly.
//
// It is a pure function of two doubles so it can be tested without a rig -- which is the point. The
// rig can show the clamp is LIVE (the recorded exec_time stops being 2.0), but it cannot be made to
// reproduce the pre-fix failure on demand: at ship pacing the single resync of a match fires around
// step 100, where the clock is ~2.0 s and the stock value is not yet in the past. The interesting
// cases are therefore reachable only here.
//
// NOT `std::max`. The comparison is written to mirror order_schedule's own asymmetry -- it raises
// ONLY when strictly below -- so a NaN horizon leaves exec_time untouched rather than propagating.
inline double resync_order_exec_time(double exec_time, double horizon) {
    return exec_time < horizon ? horizon : exec_time;
}

// ---- MP D24: WHAT the resync order must be clamped TO, which is not the bare horizon ----
// D17 wired the clamp above to `*es.horizon`, and it is still not enough. The horizon is the barrier
// every peer is ALREADY sitting at -- committed = min(own request, every peer's advertised horizon),
// so committed <= our horizon, and a peer's clock can reach committed exactly. release_due fires on
// `!(exec_time > now)`, i.e. on EQUALITY too. So an order stamped at our own advertised horizon is
// due the instant each peer arrives there, and whether it is in that peer's pending array by then is
// a race with the packet. The leader, which enqueues its local mirror with zero latency, always wins
// that race; the others lose it whenever the link is slower than the remaining sim time. That is the
// host-vs-both-clients split D24 recorded, and it is D14's fingerprint one clamp later.
//
// THE BARRIER IS ONE STEP PAST THAT. `max(horizon, game_clock) + step_size` is strictly greater than
// every peer's reachable clock: peer_clock <= peer_committed <= our advertised horizon (our horizon
// only ever increases, so a stale advertisement cannot exceed it) < barrier. No peer can be past it
// when the packet lands, so all of them release the order on the SAME step -- which is the whole
// property D14 wanted. `game_clock` is in the max only as a floor for the pathological case where
// the clock has somehow outrun the horizon; in a healthy match `horizon` wins it.
//
// WHY NOT REORDER force_resync INSTEAD. The tempting fix is to hoist force_resync's
// `horizon = game_clock + step_size` + send_lockstep_extend above the broadcast so the existing
// clamp reads a FRESH horizon. That is a bigger deviation from the original (it swaps two wire
// sends) and it is still not sufficient on its own: the fresh horizon is `our clock + step`, and a
// peer that is AHEAD of us can already be past it. Dominating every peer requires the max above, so
// the reorder buys nothing the max does not already buy. Left alone deliberately.
//
// Composes with resync_order_exec_time rather than replacing it: that function keeps its
// raises-only, NaN-transparent semantics (and its eight cases), and this only changes the value it
// raises TO. Pure, so the regime the rig cannot reach is still reachable here.
inline double resync_order_barrier(double horizon, double game_clock, double step_size) {
    return (horizon > game_clock ? horizon : game_clock) + step_size;
}

} // namespace mh::fix
