//
// desync/state_ring.h -- mp:D41: the ship `net` state ring. A bounded RAM window of the last
// `[desync] state_ring_s` seconds of hash-manifest state (desync/state_ring_core.h: one base image at
// the window's oldest step + the STEP chunks after it), written to mh_desync_state.bin in the match
// folder -- docs/state-record.md v1 with flags RING -- when the desync watch detects its FIRST
// mismatch of the match, after recording `state_ring_tail_s` more seconds. It shows the RUN-UP to a
// divergence, which the D25 post-hoc grid dumps (mh_desync_snap_<step>.bin) never do.
//
// WHEN IT RUNS. `[desync] state_ring=1` is the ship default, but the ring only asks the shared
// tracker (state_tracker.h) for a step while the desync watch is armed and the session is a live
// multi-peer lockstep one (the `live` predicate the watch hands to configure(), the same gates its
// sampling has, minus the cadence). A solo game, a lobby, a replay with no peers: no step wanted, no
// update paid, and the ring's memory is not even allocated until the first live step. Once
// triggered, the TAIL keeps recording whether or not the session stays live -- a peer whose
// transport just dropped still owes its half of the evidence.
//
// SKIPPED WHEN THE WHOLE-MATCH RECORDER IS ON (`state_record=1`, the net-debug zips): the ring would
// be a strict subset of mh_match_state.bin, so configure() says so in mh_net.log and stays off.
//
// ONCE PER MATCH. trigger() latches; the dump happens once, and a second mismatch (or a second call)
// logs nothing and writes nothing. session_start() re-arms for the next match.
//
// THREADING. Main (sim) thread only, except the file write: the finished ring's storage is handed to
// a one-shot background thread that creates the file, streams header + KEYF + STEP chunks + END and
// frees it. The sim thread never waits on I/O.
//
#pragma once
#include <cstdint>

#include "desync/state_ring_core.h"
#include "desync/state_tracker.h"

namespace mh::desync::state_ring {

using log_fn = void (*)(const char *fmt, ...);

struct settings {
    int enabled = 1;    // [desync] state_ring
    int seconds = 30;   // [desync] state_ring_s: run-up kept before the mismatching step
    int tail_s  = 10;   // [desync] state_ring_tail_s: recorded after the trigger
    int max_kb  = 6144; // [desync] state_ring_max_kb: hard cap on STEP-chunk bytes (run-up + tail)
};

// Install time. `recorder_on` = the D40 whole-match recorder is configured on (then the ring stays
// off). `live` = is the session a live multi-peer lockstep one right now. Returns true if the ring is
// on (the caller then enables the shared tracker and registers listener()).
bool                 configure(const settings &s, bool recorder_on, uint64_t manifest_fp, bool (*live)(), log_fn log);
bool                 enabled();
state_hub::listener *listener();

// session_begin_multi: a new match; the window restarts at its first live step.
void session_start();

// THE HOOK: the desync watch's first detected mismatch of the match, at sample step `mismatch_step`.
// Freezes the window and starts the tail. Returns true if this call started a dump.
bool on_first_mismatch(uint32_t mismatch_step);

// Match end: a tail in progress is cut here and dumped; a clean match writes nothing. Logs the RAM
// and per-step cost either way.
void match_end();

// For the selftest: the core's live counters, and whether the background writer is idle.
const sring::core &core_view();
bool               writer_idle();

} // namespace mh::desync::state_ring
