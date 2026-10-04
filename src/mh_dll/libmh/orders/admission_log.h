//
// orders/admission_log.h -- the PENDING-ADMISSION LOG (tracker mp:X3c).
//
// WHAT IT IS FOR. A world resync ships the host's world as of step S to a peer that keeps playing
// live and is therefore at step L > S when the blob lands. The imported world's ORDER_PENDING holds
// the host's scheduled-but-unreleased orders as of S. The peer must also replay every external input
// the HOST admitted after S -- and those are exactly the orders that reached THIS peer through the
// only doors external inputs enter by: writes into ORDER_PENDING. This log records those admissions
// as they happen, with a per-source ordinal, so the resync can append "everything from source s with
// ordinal > n_src[s]" to the imported PENDING and let the ENGINE's own catch-up loop do the rest.
// (It replaces the earlier plan of replaying the host's dispatched ORDER_QUEUE, which is only exact under a harness-grade contract.)
//
// THE DOORS (all four end in mh::orders::detail):
//   * detail::pending_enqueue -- remote human orders (MSG_ORDER), the CTL_RESYNC_BEGIN synthetic
//     0xf0 event (rx side), and the leader's own local 0xf0 copy (tx side);
//   * detail::schedule        -- this peer's own human orders.
// The AI is not logged: it runs on every peer and is deterministic from state.
//
// SOURCE KEY. owner_and_kind == 0xf0 ("global event") is source 8; every other record is keyed by
// its owner nibble (`owner_and_kind & 0xf`), which is exactly what release_due partitions by. The
// known edge that 0xf0 and a player-0 order share owner nibble 0 in RELEASE is a pre-existing
// known ambiguity of the release partition and does not matter here: the log keys them apart.
//
// THREE PROPERTIES THE HOT PATH DEPENDS ON
//   1. IT ONLY OBSERVES. note() reads the record the container just stored and writes ONLY this
//      module's own static storage. It touches no game region, calls no game function and consumes
//      no RNG, so it cannot move a hash slice or a step. (The storage is not a registry region.)
//   2. BOUNDED MEMORY. One fixed ring of RING_CAP entries (~1.2 MB, static .bss) plus nine counters.
//      No allocation, ever. When the ring wraps, the oldest entry is forgotten and the per-source
//      `dropped_upto` records the highest ordinal lost, so a resync that needs a lost record is
//      REFUSED (-30) rather than silently replaying a gap.
//   3. ~ZERO COST. One 68-byte copy, a handful of integer increments, no branch on game state,
//      per order. Orders arrive at human-click rates.
//
// SINGLE-THREADED BY CONTRACT, the same as the ORDER_PENDING array it shadows: every writer runs on
// the game thread (the rx drain and the order issue path), so there is no lock.
//
// R5 (design memo): detail::pending_enqueue RESETS pending_count to 0 on overflow, silently dropping
// every scheduled order. That breaks the counters' meaning (they would say "admitted" for records that
// were destroyed), so note_overflow() LATCHES `abort_worthy`; plan()/readmit() then refuse (-32).
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "orders/order_queue.h"

namespace mh::orders::admission {

inline constexpr int      SOURCES            = 9; // players 0..7, plus source 8 = the 0xf0 global event
inline constexpr int      SRC_GLOBAL_EVENT   = 8;
inline constexpr uint32_t RING_CAP           = 16384; // ~1.2 MB; 4096 held only ~1200 steps at ~3.3 records/step
inline constexpr uint16_t GLOBAL_EVENT_OWNER = 0xf0;  // rx_dispatch.cpp / tx_emit_ctrl.cpp

// The source key of a STORED (masked) record.
inline int source_of(const order &stored) {
    const uint16_t oak = stored.owner_and_kind;
    return oak == GLOBAL_EVENT_OWNER ? SRC_GLOBAL_EVENT : static_cast<int>(oak & 0xf);
}

struct status {
    uint32_t total;                 // records logged since reset()
    uint32_t n_src[SOURCES];        // per-source ordinal of the most recent record (== count)
    uint32_t dropped_upto[SOURCES]; // highest ordinal of each source that fell off the ring (0 = none)
    uint32_t ring_wraps;            // entries overwritten
    uint32_t overflow_resets;       // PENDING overflow resets observed (R5)
    bool     abort_worthy;          // latched by note_overflow(); cleared only by reset()
    bool     muted;                 // note() is a no-op while set
};

// Per-match reset. Called from sim_session_begin_multi (llm_strat_session_begin_multi, the MP
// session start -- the same point that resets the lockstep peer-timing table) so a new match starts
// counting from zero. Idempotent.
void reset();

// The taps. `stored` is the record AS STORED in ORDER_PENDING (after the & 0xff masking).
void note(const order &stored);
// pending_enqueue found PENDING full and emptied it.
void note_overflow();

void          set_muted(bool on);
const status &current();
void          counts(uint32_t out[SOURCES]);

// ---- resync side -------------------------------------------------------------------------------

// Result codes. They are the numbers state/world_snapshot.h names WORLD_ERR_RESYNC_* (static_asserted in
// state/spine.cpp); orders must not include state/world_snapshot.h, so the values live here too.
inline constexpr int ERR_RING_SHORT   = -30;
inline constexpr int ERR_AHEAD        = -31;
inline constexpr int ERR_ABORT        = -32;
inline constexpr int ERR_PENDING_FULL = -33;

struct plan_report {
    uint32_t need;    // records with ordinal > n_src[s] that a readmit would append
    int32_t  bad_src; // the first source that made plan() refuse, or -1
};

// Read-only feasibility check, run BEFORE the import writes a byte.
//   0                                   ok
//   ERR_ABORT (-32)                    the log latched abort_worthy
//   ERR_AHEAD (-31)                    n_src[s] > this peer's own count for some s
//   ERR_RING_SHORT (-30)             a needed ordinal (> n_src[s]) was dropped from the ring
int plan(const uint32_t n_src[SOURCES], plan_report *rep);

struct readmit_report {
    uint32_t readmitted; // records appended
    uint32_t pending_after;
};

// Append, in admission order, every logged record with ordinal > n_src[src] straight onto ORDER_PENDING.
// RAW append: it does not go through pending_enqueue and is not itself logged (the tap is bypassed, and
// the log's counters keep describing this peer's own admission history, which does not change).
// Returns 0, or ERR_PENDING_FULL (-33) if PENDING runs out of room (partial append; the
// report says how many landed).
int readmit(const container_state &st, const uint32_t n_src[SOURCES], readmit_report *rep);

// ---- STAGED re-admission (the product path) -------------------------------------------------------
//
// readmit() appends the WHOLE backlog at once, which cannot work at the rate a live match admits:
// ORDER_PENDING holds PENDING_CAP (1000) records and the rig measured about 3.3 records per step, so
// a backlog of a few hundred steps overflows it (-33) on the very first append. The engine does not
// need them all at once: a record only matters when release_due reaches its exec_time. So the log is
// FED to PENDING a few steps ahead of the clock, and PENDING holds only what is about to be due.
//
//   stage_begin(n_src)   snapshot the target: every record with ordinal > n_src[s] that the log holds
//                        NOW (records admitted after this moment reach PENDING through the normal
//                        door). Same refusals as plan(). Clears any previous stage.
//   stage_feed(horizon)  append, in admission order, every not-yet-fed target record whose
//                        exec_time <= horizon. Called once per sim step by the resync driver with
//                        horizon = clock + a few steps, so a record is always in PENDING before the
//                        step that releases it and never long before. Exact because release_due is
//                        gated on exec_time alone and orders one player's records by PENDING index,
//                        which is admission order here as on the host.
//   stage_remaining()    target records not yet fed. The stage is DONE at 0.
//
// stage_feed refuses (and the driver must ABORT the resync, naming the reason) when it cannot keep
// the promise: ERR_RING_SHORT (a target record fell off the ring before it was fed), ERR_PENDING_FULL
// (no room for a due record), ERR_ABORT (the log latched an overflow reset). Never a silent gap.
struct stage_report {
    uint32_t fed;           // records appended by this call
    uint32_t remaining;     // target records still waiting
    uint32_t pending_after; // ORDER_PENDING count when the call returned
};

int      stage_begin(const uint32_t n_src[SOURCES], plan_report *rep);
int      stage_feed(const container_state &st, double horizon, stage_report *rep);
int      stage_feed(double horizon, stage_report *rep); // over the live container (mh::orders::state())
bool     stage_active();
uint32_t stage_remaining();
void     stage_cancel();

} // namespace mh::orders::admission
