//
// desync/order_capture.h -- mp:Q3: mh.dll's own ORDER RECORDING, written beside a session's logs at
// `[log] level=debug` (`[desync] order_record=1`, a level-table observer key), in single player and
// multiplayer alike, with no mh_harness.dll needed.
//
// WHAT IS WRITTEN. The determinism harness's two replay inputs, in their own formats (harness.cpp
// order_record / clock_record, the formats tools/mp_order_diff.py and the harness's `order_mode=2`
// replay read), into the OPEN SESSION FOLDER (MH_RunDir):
//   mh_rec_orders.bin   MHOR v1: {u32 magic 'MHOR', u32 version 1} then flat {i32 step; u8 order[0x44]}
//                       records -- for every step, the due-now queue (llm_strat_order[0..count)) as it
//                       stands at llm_strat_order_queue_dispatch's entry, i.e. the orders that step runs.
//   mh_rec_clock.bin    raw doubles, the game clock at every step's pre-body boundary; track[0] = step 1.
// Step k = the k-th sim step that ran with the session folder open -- the axis of the harness's own
// per-match segment (mh_match_orders.bin / mh_match_clock.bin), so the two compare record for record.
// Different file NAMES from the harness's on purpose: no existing reader may mistake one for the other.
//
// SNAPSHOT, NOT DELTA. The measurement that motivated a delta form: 92-94% of the records repeat the
// previous step's queue, because an order stays in the due queue until the step that executes it.
// MHOR v1 cannot say "unchanged since step k": the replay (harness.cpp order_replay_inject) REPLACES
// the queue with each step's records, so a step without records is an EMPTY queue to it. Keeping the
// format is what keeps the file replayable by the existing tools; the repeats are what gzip is for
// (the close compresses the file, state_compress.h). The close line reports the repeat share.
//
// THREADING. Sim thread only (dispatch + the step tick + the session close all run on the game's main
// thread). The records are staged in memory and handed to the async log sink (mh_log_sink.h) in one
// batch per FLUSH_STEPS steps or FLUSH_BYTES, as RELIABLE binary records (never dropped by the text
// bound). No file handle and no WriteFile on the sim thread. At the session close the tail is handed
// over and the gzip thread compresses each file once the sink reports it written (a log-sink ticket).
//
// LIFETIME. A file opens on the first sim step that sees a session folder (any folder generation not
// already closed); it closes on the session's close (session_close, from the session layer's rollups --
// a quit, a game over, a timeout, the harness stop) or when a step sees the folder change. A closed
// generation is never reopened: steps after a close in place (the harness stop keeps the folder) are
// not recorded, exactly like the state recorder's "Continue game".
//
// NOT WITH THE HARNESS RECORDER. When mh_harness.dll's own `order_mode` is non-zero the harness owns
// the dispatch entry (record or replay) and this module stays off (configure says why) -- one writer
// of one stream.
//
// NO ADDRESSES HERE. The queue arrives as a pointer + count from desync_watch.cpp, the only file in
// mh/desync/ that may bind one.
//
#pragma once
#include <cstdint>

namespace mh::desync::orders {

inline constexpr int      ORDER_SIZE    = 0x44;        // sizeof(llm_strat_order)
inline constexpr int      ORDER_QCAP    = 4096;        // the due-now queue's LARGEST capacity (300 vanilla; 4096 with the order layer ON, PT-CAP9-U5)
inline constexpr uint32_t MAGIC         = 0x524f484du; // "MHOR"
inline constexpr uint32_t VERSION       = 1;
inline constexpr uint32_t FLUSH_STEPS   = 50; // ~1 s at 50 steps/s
inline constexpr uint32_t FLUSH_BYTES   = 256u * 1024u;
inline constexpr char     ORDERS_NAME[] = "mh_rec_orders.bin";
inline constexpr char     CLOCK_NAME[]  = "mh_rec_clock.bin";

#pragma pack(push, 1)
struct record {
    int32_t step;
    uint8_t order[ORDER_SIZE];
};
#pragma pack(pop)
static_assert(sizeof(record) == 0x48, "the MHOR v1 record is 0x48 bytes");

using log_fn = void (*)(const char *fmt, ...);

// Install time. `enabled` = [desync] order_record. `refused` non-null = a reason it must stay off even
// though asked (the harness owns the stream); it is logged. Returns whether the module is on.
bool configure(int enabled, const char *refused, log_fn log);
bool enabled();
void set_compress(bool on); // tests: keep the raw files

// Once per sim step at the PRE-BODY boundary. `session_dir` = MH_RunDir() when a session folder is open
// (trailing backslash), else null; `gen` = MH_RunDirGeneration(); `settled` = false when the folder was
// switching while it was read (the open/close decision then waits a step); `clock_bits` = the game
// clock's bits.
void step(const char *session_dir, unsigned long gen, bool settled, uint64_t clock_bits);

// At llm_strat_order_queue_dispatch's entry: the due-now queue about to run this step.
void dispatch(const uint8_t *queue, int count);

// The session's close (the session layer's rollups). Hands the tail over, closes, starts compression.
void session_close(const char *why);

// Counters of the file open now (or the last one closed), for the close line and the selftest.
struct stats {
    uint32_t files;   // files opened by this process
    uint32_t steps;   // steps recorded in the current/last file
    uint32_t records; // order records written
    uint32_t repeats; // records found in the previous step's snapshot
    uint32_t calls;   // dispatch() calls while the file was open (empty queues included)
    uint64_t bytes;   // orders-file bytes handed to the sink (header included)
    int64_t  ticks;   // QPC ticks spent in dispatch() + step() on the sim thread
    bool     open;
};
stats current();

} // namespace mh::desync::orders
