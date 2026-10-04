//
// desync/state_tracker.h -- THE ONE per-step incremental state tracker of a process (mp:D40), shared
// by every consumer of "what changed in the hash manifest this sim step".
//
// WHY ONE. mh::state::inc::tracker (libmh/state/inc_state.h, mp:D39) keeps a 2.8 MB shadow of the
// manifest and a masked sum of 64-byte block hashes, and one update() is a memcmp of the whole
// manifest against that shadow -- ~270 us per step measured on a 66,586-step field replay. Paying it
// once per consumer would multiply the only real cost of the design, and two shadows of one manifest
// can only ever disagree. So the desync module owns exactly one, updates it once per sim step at the
// PRE-BODY boundary, and fans the changed-byte runs out to its listeners:
//   the dirty probe   ([desync] dirty_probe=1)  -- measurement: bytes/grains/cost per step
//   the recorder      ([desync] state_record=1) -- mp:D40, mh_match_state.bin (state_recorder.h)
//   mp:D44            per-step in-band detection -- reads state_hash() / region_hash(i) after step()
//   mp:D41            the ship ring              -- the same listener shape as the recorder
//
// ALLOCATED ONLY WHEN WANTED. enable() is called at install time by each consumer that is configured
// on; the arena (the tracker's shadow + block hashes, inc::tracker::arena_bytes()) is VirtualAlloc'd
// on the first call and never freed. With no consumer enabled nothing is allocated and step() is a
// branch.
//
// PER STEP. step(s) asks every listener whether it wants this step. If none does, the tracker is NOT
// updated and is marked unprimed (a later update would otherwise report a multi-step delta as one
// step). Otherwise: the first step after reset() (or after a skipped step) PRIMES -- shadow := live,
// no runs -- and every later step UPDATES, handing each listener the EXACT changed runs (gap 0: the
// probe's byte counts must stay exact; the recorder coalesces for itself). Listeners see:
//     step_begin(s, primed) -> run(...)* / rebased(...)* -> step_end(s, primed)
// and read the tracker (shadow(i), region_hash(i), state_hash(), last) in step_end.
//
// MAIN THREAD ONLY -- the sim thread, from the desync watch's per-step hook.
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "state/inc_state.h"

namespace mh::desync::state_hub {

class listener {
public:
    virtual ~listener() = default;
    // Whether this consumer needs the tracker updated THIS step (recorder: a file is open; probe: on).
    virtual bool wants_step() const = 0;
    virtual void step_begin(uint32_t step, bool primed) {
        (void)step;
        (void)primed;
    }
    // Bytes [off, off+len) of hash slice `region` changed; `bytes` is the LIVE memory at that offset,
    // and the whole slice is contiguous around it (the recorder's coalescing relies on that).
    virtual void run(int region, uint32_t off, uint32_t len, const uint8_t *bytes) {
        (void)region;
        (void)off;
        (void)len;
        (void)bytes;
    }
    // The slice's live base moved (the runs are still relative to the old shadow content).
    virtual void rebased(int region) { (void)region; }
    virtual void step_end(uint32_t step, bool primed) {
        (void)step;
        (void)primed;
    }
};

inline constexpr int MAX_LISTENERS = 4;

// Install time. `who` names the consumer in the one allocation line. Returns false (and logs) if the
// arena could not be allocated. Idempotent.
bool   enable(const char *who, void (*log)(const char *fmt, ...));
bool   allocated();
size_t arena_bytes();

// Register a consumer (install time, once each). Fan-out order = registration order.
void add_listener(listener *l);

// mp:D44: the tracker's undo journal (inc_state.h `journal`), or null. Install time.
void set_journal(mh::state::inc::journal *j);

// Session start (session_begin_multi): the next step primes.
void reset();

// One sim step. Returns true if the tracker was updated or primed this step.
bool step(uint32_t step);

// The tracker (valid after allocated()); and whether the LAST step() primed rather than updated.
const mh::state::inc::tracker &tracker();
bool                           last_primed();

// QPC ticks the last step() spent in prime/update INCLUDING every listener's run() callbacks.
int64_t last_ticks();

} // namespace mh::desync::state_hub
