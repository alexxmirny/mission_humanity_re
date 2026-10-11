//
// desync/state_recorder.h -- mp:D40: record the WHOLE match's hash-manifest state into
// mh_match_state.bin (docs/state-record.md v1). `[desync] state_record=1`, a player setting since mp:Q4
// (the launcher's Diagnostics page, default OFF; `[log] level=debug` no longer turns it on).
//
// WHAT IS WRITTEN. The raw bytes of every hash slice at every sim step's PRE-BODY boundary: a KEYF
// (all slices) at the first recorded step and every `state_keyframe_every` steps after it, a STEP
// chunk (the runs that changed since the previous step, coalesced over gaps <= 8 bytes) for every
// step after the first -- empty when nothing changed -- and an END chunk when the match ends
// normally. The bytes come from the shared tracker (state_tracker.h): the runs it reports and, for a
// keyframe, its shadow, which equals live memory after the step's update.
//
// THREADING AND BOUNDS. The sim thread only appends to a batch buffer (the STEP chunk is built in
// place, sealed with its CRC) and hands the batch to a writer thread every FLUSH_STEPS steps (so the
// file on disk trails the game by about a second) or when it passes FLUSH_BYTES. The writer owns the
// file: it CREATES it (mp:Q4 -- the name probe and CreateFile used to run on the sim thread at the
// first step), writes every job and closes it. The first keyframe is copied from the tracker's shadow
// on the sim thread (one 2.8 MB copy at the match's first step) and becomes the writer's IMAGE; the
// writer applies every STEP chunk it writes to that image, so a later keyframe is the image re-stamped
// and re-sealed on the writer thread (mp:Q4 -- the sim thread used to copy the shadow at every
// keyframe). The bytes handed over and not yet written are capped (QUEUE_CAP); if the writer falls that
// far behind, or the file cannot be created, or a write fails, or an allocation fails, recording STOPS
// with a log line naming the step -- the file already written is a valid prefix (every chunk is
// complete and CRC-sealed; it just has no END). Nothing is dropped silently. At match end the sim
// thread appends END, hands it over and returns (mp:Q4: it used to wait up to 2 s for the writer to
// drain, on the quit-to-menu frame); the writer logs its own closing line. A process that ends before
// the writer finishes leaves a valid prefix; the game's own exit waits a bounded time for it
// (desync::drain_writers, from the WM_DESTROY witness).
//
// ONE FILE PER MATCH -- or per single-player SESSION (mp:Q2: desync::solo_session_begin, at the solo
// session folder's open, on a step counter of its own). session_start() (session_begin_multi) arms a new file, opened at the first
// step; match_end() (the session close: gameover / quit / timeout / leave / the harness stop, and the
// next session_begin_multi) finishes it. Steps after match_end -- a "Continue game" after an outcome
// -- are not recorded: that is no longer the match the file describes, and the next
// session_begin_multi starts the next file. A second match writing into a folder that already holds
// mh_match_state.bin (a force-entry run with no session folder) gets mh_match_state_2.bin, and so on. A name whose `.gz` exists counts as taken (mp:D46).
//
// COMPRESSION (mp:D46). When the writer thread has closed the file, it starts state_compress.h's thread:
// raw -> `.gz.tmp` -> verify -> `.gz` -> delete raw. Nothing on the sim or render thread waits for it, and
// match_end() does not wait for either.
//
#pragma once
#include <cstdint>

#include "desync/state_tracker.h"

namespace mh::desync::recorder {

inline constexpr uint32_t FLUSH_STEPS = 50;                  // ~1 s at 50 steps/s
inline constexpr uint32_t FLUSH_BYTES = 256u * 1024u;        // or when the batch passes this
inline constexpr long     QUEUE_CAP   = 64L * 1024L * 1024L; // handed over but not yet written

using log_fn = void (*)(const char *fmt, ...);

// Install time. `enabled` = [desync] state_record; `keyframe_every` = [desync] state_keyframe_every
// (a value < 1 is refused: the default is used and the log says so).
// Returns true if the recorder is on (the caller then enables the shared tracker and registers
// listener()).
bool configure(int enabled, int keyframe_every, uint64_t manifest_fp, log_fn log);
bool enabled();

// mp:D46: gzip each finished file on a background thread (state_compress.h). On by default; `[desync]
// state_compress=0` turns it off (the raw file stays), as does a test that wants to read the raw bytes.
void                 set_compress(bool on);
state_hub::listener *listener();

// The step-axis facts the open line reports (desync_watch.cpp measures them; see session_start).
struct axis_info {
    bool     session_folder; // MH_RunDir() named a session folder at session_begin_multi
    uint32_t pre_steps;      // sim steps already seen in THAT folder before session_begin_multi
    bool     solo = false;   // mp:Q2: a single-player session (desync::solo_session_begin), not a match
};

// Arm a new file for the match that session_begin_multi just began (opened at its first step).
void session_start(const axis_info &axis);

// Finish the current file (END + close + rollup). No-op when nothing is being recorded. Never waits:
// the writer finishes the file in the background.
void match_end();

// mp:Q4: wait (bounded) until every writer thread has closed its file. For the exit drain and the
// selftests; NEVER from the sim thread during play. True when idle.
bool wait_idle(unsigned ms);
long active(); // writer threads not finished yet

} // namespace mh::desync::recorder
