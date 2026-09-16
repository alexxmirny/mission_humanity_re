//
// sim/rng_trace.h -- the RNG DRAW-SEQUENCE TRACE (LIB-REF, the step-290 hunt).
//
// WHAT IT IS FOR. Two arms of a deterministic replay are bit-identical for 289 steps and then one
// AI unit takes a different order. Every comparable region matches at 289; the memory layout, the FP
// environment, the hosted-only code paths, the nav pool's pointer order and the path-slot allocator
// have each been excluded by measurement. What is left is that some shared function computes a
// different VALUE, and six successive hypotheses about WHICH one have each measured false.
//
// So this instrument stops hypothesising. The strategic PRNG is a single global recurrence that both
// arms run from the same carried seed, and EVERY draw is a fingerprint of the code path that asked
// for it: identical code paths produce an identical draw sequence, and the FIRST index at which the
// two sequences disagree is the first moment the arms did different work. That index names the
// divergence directly instead of testing a guess about it.
//
// WHY THE RETURN ADDRESS IS RECORDED BUT NOT DIFFED ACROSS ARMS. `_ReturnAddress()` names the caller,
// which is the answer we actually want -- but the two arms are different modules (mh.dll vs
// libref_host.exe) with different code layouts, so the same function has a different address in each
// and the raw values are NOT comparable. The workflow is therefore: diff the (channel, value)
// SEQUENCE, which is module-independent, to find the first differing INDEX; then symbolize that one
// draw's return address WITHIN ITS OWN ARM. One address to resolve, not a sequence to align.
//
// COST AND SAFETY. Off unless a window is armed, and the check is two integer compares on a path
// that is already a load, an add, a rotate and a store. It only ever READS state and appends to a
// fixed ring; it cannot move the sim. That claim is not left to inspection -- the acceptance is that
// the replay's first divergence and mismatch count are unchanged with the instrument compiled in and
// unarmed.
//
#pragma once
#include <cstdint>

namespace mh::sim {

struct rng_trace_entry {
    uint32_t    step;
    int32_t     channel;
    uint32_t    after; // the post-step state -- the draw's own fingerprint
    const void *ra;    // the PRNG core's caller, valid only within the arm that recorded it
    const void *site;  // the DRAW WRAPPER's caller -- the game-logic frame, which is what
                       // actually names the divergence. rand_below's detail:: half inlines
                       // into the public wrapper, so `ra` only ever says 'rand_below'.
};

// Record draws whose step is in [lo, hi]. lo > hi (the default) disarms.
void rng_trace_window(uint32_t lo, uint32_t hi);

// The sim step the NEXT draws belong to. Both hosts already know this number; the instrument does
// not try to derive it, because a second derivation of the step counter is a second thing to be
// wrong.
void rng_trace_set_step(uint32_t step);

// Called from the two PRNG recurrences. Cheap and inlineable to nothing when disarmed.
// Set by each public draw wrapper before it delegates, so the core can record the frame ABOVE
// itself. Stale-by-design if a draw ever bypasses the wrappers -- which is why both are kept.
void rng_trace_set_site(const void *site);

void rng_trace_record(int32_t channel, uint32_t after, const void *ra);

// A GENERIC GATED NOTE, on the same window and step as the draws. The draw sequence says WHICH step
// and WHICH function diverged; a note says what that function's own inputs and verdict were, so the
// next level down does not need a second instrument with a second window that could disagree.
struct rng_trace_note {
    uint32_t step, tag, a, b, c, d, e, f;
};
void                  rng_trace_add_note(uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                         uint32_t e, uint32_t f);
int                   rng_trace_note_count();
const rng_trace_note &rng_trace_note_at(int i);

int                    rng_trace_count();
const rng_trace_entry &rng_trace_at(int i);
bool                   rng_trace_overflowed();

} // namespace mh::sim
