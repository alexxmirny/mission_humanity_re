//
// desync/desync_watch.cpp -- the live half of D21: bind the pure detector (desync_watch.h) to the
// real hash manifest, the transport's out-of-band control channel and the HUD.
//
// SAMPLING POINT. The PRE-BODY boundary of llm_strat_sim_step -- the same point the determinism
// harness hashes at, so the in-band number and the one in mh_harness.log describe the same state.
//
// It has to be a per-STEP hook and not a per-FRAME one: the sample is keyed by a step number both
// peers assign to the same moment, and a frame boundary is not one (a frame runs 0..N sim steps, so
// two peers sampling "at present" almost never land on a common step and every sample would miss
// the other's ring).
//
// AND IT HAS TO BE TWO HOOKS, which is the part that is not obvious. The obvious candidate -- the
// turn engine's own `calls.sim_step` edge -- is DEAD AT SHIP: `llm_strat_sim_tick`, the mode-3
// catch-up loop that drives it, is not in the default `[promote] lockstep` closure ("sim_tick is
// ORIGINAL -- not in the default closure", C6), so the original binary runs the step loop and our
// edge never executes. Measured, not predicted: a clean 8000-step 2-peer run produced an ARMED line
// and not one sample. So:
//   harness armed -> harness.cpp's detour owns the entry and calls on_sim_step_hashed with the hash
//                    it just computed (one 2.79 MB walk per sample, not two)
//   otherwise     -> net_seams.cpp installs a trampoline on the same entry, calling on_sim_step
// Exactly one of the two is live in any run: the harness takes the entry in DllMain, before
// MH_Seam_Init runs, and the seam install is conditional on that entry still being free.
//
// mp:D32 -- A THIRD ROUTE ONTO THE SAME PRE-BODY POINT, AND WHY IT MUST NOT ADD A THIRD FEEDER.
// ROOTS-LIVE (2026-09-04) gave llm_strat_sim_step a promoted body (mh::sim::sim_step), reached either
// by DIRECT ENTRY INSTALL (no harness owns the entry -- the trampoline above is free, so this file's
// on_sim_step chains onto the promoted body via mh::sim::set_sim_step_pre_hook instead of a
// trampoline: same single feeder, different plumbing) or by HARNESS REBIND (the harness's own detour
// keeps the entry and falls through to the promoted body instead of the original -- see
// sim_step.cpp's promoted_arm namespace). The rebind case is the trap: the harness's detour still
// calls on_sim_step_hashed UNCONDITIONALLY, before ever reaching the promoted body, so if
// net_seams.cpp also chained on_sim_step onto the promoted body (as it did before D32), BOTH fired
// every step -- measured as "2999 steps seen" for a 1500-step run. The fix lives on the mh::sim side
// (sim_step.h/.cpp: sim_step_promoted_via_rebind(), and set_sim_step_pre_hook() itself refuses the
// rebind case) and on the seam side (net_seams.cpp install_desync_watch() only chains the pre-hook
// when NOT promoted-via-rebind), so the invariant below is restored: exactly one feeder, named per
// configuration, never two.
//
// THE SAMPLE RIDES A TRANSPORT CONTROL FRAME (FLAG_HASH), not the game's lockstep wire. The retail
// dispatcher bounds-checks the outer tag and treats anything outside 1..5 as a GARBLED STREAM,
// setting LS_SESSION_ENDED (turn_engine.h) -- so a detector carried on the game wire would end the
// session on any peer that did not understand it. FLAG_HASH is routed to a handler and never reaches
// the game's inbound queue, so it cannot perturb the lockstep input stream. Same determinism-neutral
// arrangement as SESSION_INFO / START / ANNOUNCE, with one addition: the host RELAYS it, so in an
// N-peer game every pair is compared rather than only each client against the host.
//
// THREADING. The frame handler runs on the transport's RECV thread; it does nothing but validate and
// queue under a critical section. Every judgement, every log line and the on-screen notice happen on
// the main thread inside on_sim_step.
//
#include "desync/desync_watch.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h"
#include "en_guard.h"
#include "include/mh_net_export.h"
#include "include/mh_run_context.h"       // MH_RunDir (the snapshot lands next to the run's logs)
#include "include/mh_transport_present.h" // F3F: gate the SAMPLING (not the arm) on a live module
#include "desync/desync_wire2.h"          // mp:D44: per-step comparison + live localisation (WIRE_VERSION 2)
#include "desync/world_sync.h"            // mp:X3c: the host-authoritative world resync (action=1)
#include "desync/state_recorder.h"        // mp:D40: the whole-match recorder, a listener on the hub
#include "desync/state_ring.h"            // mp:D41: the ship ring, dumped at the first mismatch
#include "desync/state_tracker.h"         // mp:D40: the ONE per-step incremental tracker (mp:D39's core)
#include "state/inc_state.h"
#include "state/region_view.h"
#include "state/host_api.h"
#include "state/host_events.h"
#include "ui/player_strings.h" // mods:LANG4: the on-screen alert is a table row

namespace mh::desync {
namespace {

// ---- logging (module owns the pointer; the seam layer wires it to seam_log at arm time) ---------
void (*g_log)(const char *) = nullptr;

void say(const char *fmt, ...) {
    if (!g_log) return;
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(line);
}

// ---- config ([desync] in mh_net.ini) ------------------------------------------------------------
struct Config {
    // The detector is ON by ship default: clause (e) of D21 is "default behaviour is log + notify",
    // which is only true of a detector that is actually running. Set 0 to silence it entirely.
    int enabled = 1;
    // Sample cadence, in SIM STEPS. CHOSEN FROM THE MEASURED COST, not assumed (D21 clause (d)).
    // The measurement is the COST PROBE line this module emits at arm time, and on the two rig peers
    // it reads 4.34 ms and 5.42 ms for one walk of the 56-region, 2.79 MB manifest -- an order of
    // magnitude more than the "it is only a hash" intuition, which is exactly why the clause demands
    // a number. At 50 steps/s a sim step has ~20 ms of budget, so:
    //     every=1   ~4.9 ms EVERY step         -- ~25% of the budget. Not viable.
    //     every=20  ~245 us amortised, 4.9 ms spike every ~0.4 s
    //     every=50  ~98 us amortised (~0.5%), 4.9 ms spike every ~1 s   <-- shipped
    // 50 buys a detection latency of at most 50 steps (~1 s) plus one link RTT, against a failure
    // that has so far gone unnoticed for EIGHT MINUTES. Trading a second of latency for a fifth of
    // the cost is the right side of that; a run that wants tighter can set the key.
    int every = 50;
    // Beyond log+notify. 0 = nothing else, and that is what this module implements; a non-zero value
    // is READ, REPORTED and otherwise ignored, because a halt changes what the game does and would
    // have to go through the reimpl_fixes / migrated_fix_knobs discipline (D21 clause (e), U20 (f)).
    int action = 0;
    // 1 = also log every AGREEING comparison. Off: a clean 20-minute match would write ~3000 lines
    // saying nothing happened.
    int verbose = 0;
    // D25: a mismatch ARMS this peer, which then dumps the FULL region set (every region's VERDICT
    // byte stream -- the exact preimage of the compared hashes) on the absolute step grid in
    // desync_watch.h, so the per-peer files land on the SAME step and are byte-diffable offline.
    // That is what names the diverging OFFSETS, which a hash never can. ~2.7 MB per dump (the
    // manifest is 2.66 MB of state), written only after a desync was already detected, so a clean
    // match writes nothing. ON by ship default because the runs that need it most are exactly the
    // un-instrumented user sessions -- the 2026-09-01 ai_econ desync cost a session of inference
    // that one file would have answered. Set 0 to keep the detector log-only.
    int snapshot = 1;
    // How many dumps one match may write on this peer before it stops.
    int snapshot_max = 3;
    // MEASUREMENT ONLY (desync_watch.h "the dirty-block probe"): every step, diff the raw manifest
    // against a shadow copy and report how much changed, per region and per block grain, plus what
    // the diff itself costs. Writes mh_dirty_probe.csv (one row per step) and a rollup at match end.
    // Read-only on game memory; OFF by default because it costs a 2.79 MB compare every step.
    int dirty_probe = 0;
    // mp:D40: record the WHOLE match's raw manifest state (keyframe + per-step deltas) into
    // mh_match_state.bin in the match folder (desync/state_recorder.h, docs/state-record.md). OFF in
    // the ship `net` ini; ON in the net-debug / brokered-debug ones (tools/release_package.py).
    int state_record = 0;
    // Steps between keyframes in that file (the first recorded step is always one).
    int state_keyframe_every = 3000;
    // mp:D41: the ship ring (desync/state_ring.h) -- the last state_ring_s seconds of state in RAM,
    // dumped with state_ring_tail_s more to mh_desync_state.bin at the first mismatch. ON by default;
    // off by itself when state_record=1 (that file already holds the run-up).
    mh::desync::state_ring::settings ring;
    // mp:D44: compare EVERY sim step on the shared incremental tracker's hashes (WIRE_VERSION 2) and
    // localise a divergence live. 0 = the v1 behaviour exactly: an FNV walk every `every` steps, v1
    // frames only, and a received v2 frame is dropped as a bad frame (what an older build does --
    // which also makes this the rig's stand-in for an old peer).
    int per_step = 1;
    // Steps per TICK frame. 5 = 10 frames/s at 50 steps/s; every step is still compared and named
    // exactly -- the batch only delays when the peer hears about it (~100 ms).
    int tick_batch = 5;
    // Incidents per match that get the live localisation exchange (REGIONS -> GROUPS -> BLOCKS ->
    // BYTES). A later one still gets its `*** DESYNC` line, with first_region=-1.
    int localise_max = 4;
    // The undo journal the localisation rebuilds the first bad step from (76 B per changed 64-byte
    // block; ~75 blocks per step measured, so 2048 KB reaches back ~370 steps, ~7 s).
    int loc_history_kb = 2048;
};

Config g_cfg;

// ---- live manifest bindings ---------------------------------------------------------------------
constexpr int N = mh::state::HASH_REGION_COUNT;
static_assert(N <= MAX_REGIONS, "the hash manifest outgrew the desync sample's wire record");

// The manifest's exclusion column, materialised once as a plain array so the pure core can take it
// as a `const bool *` without knowing what a hash_region is.
bool     g_excluded[N];
uint64_t g_manifest_fp = 0;

// ---- state --------------------------------------------------------------------------------------
bool g_armed   = false; // config said yes and the manifest fits
bool g_running = false; // ...and we are in a live multi-peer lockstep session

uint32_t g_step = 0; // sim steps since session_reset(); the sample key
ring     g_ring;

// ---- D31 clause C: the cumulative order digest ---------------------------------------------------
// Folded EVERY sim step (order_digest_tick(), called from on_sim_step()/on_sim_step_hashed() before
// the sampling-cadence gate) over the "due-now" order queue -- llm_strat_order_queue_dispatch's own
// input, the closest thing to "what the sim applies this step" a PRE-BODY hook can read without a
// new hook: on_sim_step already fires every step (see the file header's SAMPLING POINT note), so
// this rides it rather than needing a second one. Resolved to region-table indices at install() time
// so a rename/reindex of the manifest cannot silently start hashing the wrong bytes; -1 means "not
// found in this build" and the digest stays inert (0, never sent -- see sample_and_judge()).
uint64_t g_order_digest          = FNV_OFFSET;
int      g_order_queue_idx       = -1;
int      g_order_queue_count_idx = -1;
int      g_order_mismatches      = 0; // R2: log + rollup only, never on-screen
int      g_order_absent_peer     = 0; // compared samples where the peer's frame carried no digest
// R2: the on-screen state notice fires on the SECOND CONSECUTIVE mismatching sample, not the first
// (should_notify's new meaning) -- reset to 0 by an intervening agreeing sample.
int g_consecutive_state_mismatches = 0;

CRITICAL_SECTION g_cs;
bool             g_cs_init = false;
pending_queue    g_pending; // guarded by g_cs

// D25 snapshot state. Once a mismatch is seen the peer is ARMED for the rest of the match and dumps
// on the absolute grid (desync_watch.h snapshot_due) until the budget runs out -- so peers that
// noticed at different moments still write files for the same steps.
bool g_snap_armed = false;
int  g_snaps_done = 0;

int  g_mismatches     = 0;
int  g_compared       = 0;
int  g_too_old        = 0;
int  g_manifest_bad   = 0;
int  g_bad_frames     = 0;
int  g_rx_while_inert = 0;     // samples received while WE have produced none -- see on_hash_frame_rx
bool g_notified       = false; // D21 (f): ONE user-visible notice per match
bool g_fp_reported    = false;
bool g_gate_said      = false; // mp:RM1: ONE "why not sampling" line per match, at the first cadence step

// Which hook is driving us, and how many samples it handed over already hashed. Reported rather than
// assumed: the two hooks pay very different costs, so a COST line that did not say which one it was
// measuring would be a number with no unit.
bool    g_reuse_source        = false;
bool    g_reuse_mismatch_said = false;
int64_t g_reused_samples      = 0;

// Cost accounting (D21 (d)): the hash is the only thing this adds to a sim step, so measure exactly
// it. QPC ticks are accumulated and reported as microseconds.
int64_t g_hash_ticks   = 0;
int64_t g_hash_samples = 0;
int64_t g_qpc_freq     = 0;
// D31 clause A: the STATUS-line proof-of-life cadence (SAMPLES_PER_STATUS_LINE / status_line_due())
// now lives in desync_watch.h, as a pure function desynctest can exercise -- see its comment there
// for why it moved and was lowered from 50.

// Scratch, reused: 96 uint64 + a wire record are ~1.6 KB together and a sim step is not the place to
// touch the heap.
uint64_t    g_per[MAX_REGIONS];
sample_wire g_out;

// ---- mp:D44: WIRE_VERSION 2 (desync_wire2.h) ------------------------------------------------------
// The v2 path reads the per-region hashes of the ONE shared per-step tracker (state_tracker.h) every
// step, so the walk above only runs for a v1 peer (the FALLBACK) or with per_step=0.
static_assert(N <= 255, "a REGIONS frame carries the region count in one byte");
constexpr bool all_regions_fit_v2() {
    for (int i = 0; i < N; ++i)
        if (mh::state::HASH_REGIONS[i].len > v2::MAX_BLOCKS * 64u) return false;
    return true;
}
static_assert(all_regions_fit_v2(), "a hash slice outgrew the v2 localisation's 128 x 64 blocks (512 KB)");

uint64_t g_fp2      = 0;     // v2::fp_v2(g_manifest_fp, inc::HASH_KIND)
bool     g_v2_armed = false; // per_step=1 and the tracker + journal are allocated
bool     g_v2_step  = false; // THIS step's tracker update is ours (set before state_hub::step)
uint32_t g_jr_last  = 0;     // the last step the journal was fed
void    *g_jr_mem   = nullptr;
uint8_t *g_ev       = nullptr; // the rebuilt state at an incident step (lazily, sum of slice lens)
uint32_t g_ev_off[N];
uint32_t g_ev_oq_live = 0;

v2::ticker       g_tick;
v2::localiser    g_loc;
v2::undo_journal g_jr;

// The peers' wire versions, as their frames reveal them (recv thread writes, main thread reads).
constexpr int VPEERS = 16;
volatile LONG g_peer_v1[VPEERS];
volatile LONG g_peer_v2[VPEERS];
volatile LONG g_v1_seen      = 0;
volatile LONG g_inert_steps  = 0;     // steps of v2 TICKs received before our first step
bool          g_legacy_on    = false; // the FNV walk runs for a v1 peer this match
uint32_t      g_legacy_from  = 0;     // v1 samples below this step predate the fallback
int           g_pre_fallback = 0;     // ...and are skipped, not called too_old

// Inbound v2 frames, validated on the recv thread, judged on the main thread (guarded by g_cs).
constexpr int V2Q_CAP = 64;
struct v2_frame {
    int     sender;
    int     len;
    uint8_t b[v2::MAX_FRAME];
};
v2_frame g_v2q[V2Q_CAP];
int      g_v2q_head = 0, g_v2q_count = 0, g_v2q_dropped = 0;

// The outgoing TICK batch.
uint64_t g_batch[v2::TICK_MAX_STEPS];
uint32_t g_batch_first = 0;
int      g_batch_n     = 0;

// The v2 tallies. `compared` / `mismatching` are STEPS here, not samples.
int      g2_compared = 0, g2_mismatches = 0, g2_too_old = 0, g2_incidents = 0, g2_manifest_bad = 0;
int64_t  g2_steps = 0, g2_ticks = 0, g2_max_ticks = 0;
uint64_t g2_tx_bytes = 0, g2_tx_frames = 0;
bool     g_first_mismatch_seen = false; // on_first_mismatch_detected() fired this match
int      g_consec_steps[VPEERS];        // consecutive mismatching steps per sender (notify persistence)

// An incident's `*** DESYNC` line waits for the first diverging region, which the REGIONS exchange
// names a link latency later; it is written with first_region=-1 if that never comes.
constexpr uint32_t PEND_WAIT_STEPS = 250;
struct pend_line {
    bool     on;
    uint32_t step, deadline;
    int      sender, no;
    uint64_t mine, theirs;
};
pend_line g_pend[VPEERS];
bool      g_v2_fp_said[VPEERS]; // a v2 peer on a different manifest was named once

// The last region vector each peer's REGIONS named, so an incident line that opens AFTER it (a
// peer's frames can overtake our own per-step verdict for that step) still gets its region.
struct regions_seen {
    bool     on;
    uint32_t step;
    int      first, nd;
};
regions_seen g_regions_seen[VPEERS];

void pend_resolve(int sender, uint32_t s, int first_region, int ndiff, const char *why);

// The tracker's undo journal feed. Only OUR steps are journaled: a step the hub updated for another
// consumer (recorder, probe) with the v2 path idle has no tick-ring entry to rebuild against.
struct JournalTap final : mh::state::inc::journal {
    void old_block(int region, uint32_t block, const uint8_t *old_bytes, uint32_t n) override {
        if (g_v2_step) g_jr.add(region, block, old_bytes, n);
    }
};
JournalTap g_jtap;

struct V2Listener final : mh::desync::state_hub::listener {
    bool wants_step() const override { return g_v2_step; }
    void step_begin(uint32_t s, bool primed) override {
        // A prime, or a gap in OUR steps: the shadow after this update is the oldest state we can
        // rebuild. The entries this update adds are tagged s and describe s-1 -- never needed.
        if (primed || g_jr_last + 1 != s) g_jr.clear(s);
        g_jr.cur  = s;
        g_jr_last = s;
    }
};
V2Listener g_v2_listener;

constexpr uint8_t SESSION_MP_LOCKSTEP = 3; // the global is a BYTE

// ---- the on-screen notice -----------------------------------------------------------------------
// Deliberately NOT a cfg::G_TEXT_PTRS string: retail has no localized text for a condition it never
// detected, and inventing an index would print whatever else lives there. Written into G_TEXT_TMP
// and printed red, the same path U17's "player dropped" notice uses. Hash-neutral: the floating
// message queue is not a region in the manifest, so showing it cannot itself move a compared value.
void notify_once(uint32_t step) {
    if (g_notified) return;
    g_notified = true;
    // F3D: resolved straight from the region registry rather than through
    // mh::lockstep::host_binds(). This was the LAST edge from the instrument back into the closure
    // (two uses, this one and sampling_now's session-mode read), and it bought nothing: both fields
    // are plain `mh::state::ptr<>` reads of a region this module can name for itself. Reading them
    // here keeps the SB-BIND property that mattered -- the address is re-resolved from the live
    // registry on every call, so a host that binds a relocated region is followed -- while leaving
    // mh/desync/ able to build with no libmh/lockstep/ header in sight.
    void *const text_scratch = mh::state::ptr<void>(mh::state::RID_G_TEXT_TMP);
    wsprintfW(static_cast<wchar_t *>(text_scratch), mh::ui::tr(mh::ui::Str::DESYNC_ALERT), (unsigned long)step);
    mh::state::evt::text_float_red(text_scratch);
    // Logged so the one-shot is OBSERVABLE from a run's artifacts. D21 (f) is a bound on how many
    // times this happens, and a bound nobody can count is not a bound -- grep the log for this line
    // and there must be exactly one per match however many thousand steps mismatched.
    say("; [desync] NOTICE SHOWN (once per match): floating red HUD message at step %lu\n",
        (unsigned long)step);
}

// ---- the D25 full-state snapshot ----------------------------------------------------------------
// Every region's VERDICT-mode byte stream (the exact preimage of the hashes the verdict compared:
// local() fields are zero-substituted and masked() fields masked, so a cross-peer diff can only
// show bytes the verdict actually judges), written at one lockstep-aligned step. Format:
//   header { magic 'MHSN', ver, step, region_count, manifest_fp }
//   then per region, in manifest order: { uint32 stream_len, stream bytes }
// The length prefix is measured with a counting pass first -- an emitted region's stream length is
// a property of its traversal, not of its manifest len. Decoder: tools/mp_desync_snap_diff.py.
constexpr uint32_t SNAP_MAGIC = 0x4e53484du; // 'MHSN' little-endian

// THE DUMP MUST NOT STALL THE SIM. The first shape of this function wrote straight to the file
// from the emit callback -- thousands of small unbuffered WriteFile calls per region, two emit
// passes per region (one to count) -- and the first real internet match to trip the watch
// (2026-09-19) measured what that costs: 2.7 MB per dump, 5.5 s of frozen game per dump on the
// host (three dumps, `stall=7` on the peer each time), 2 s on the client. Players called them
// "stalls"; they were the evidence collector. So the stream is now built in memory in ONE pass
// (the length prefix is patched in after the region is emitted) and handed to a thread that owns
// the buffer and the handle; the sim thread's cost is the emit, which is the same walk the hash
// already pays. A machine that cannot allocate 3 MB writes nothing and says so.
struct SnapJob {
    HANDLE   h;
    uint8_t *buf;
    uint32_t len;
    uint32_t step;
    int      no;
    char     path[MAX_PATH];
};

DWORD WINAPI snapshot_writer(LPVOID arg) {
    SnapJob *j  = static_cast<SnapJob *>(arg);
    DWORD    w  = 0;
    BOOL     ok = WriteFile(j->h, j->buf, j->len, &w, nullptr);
    CloseHandle(j->h);
    if (ok && w == j->len)
        say("; [desync] SNAPSHOT: %d-region VERDICT-stream dump at step %lu -> %s (dump #%d, %lu "
            "bytes, written off the sim thread; diff the peers' files with "
            "tools/mp_desync_snap_diff.py)\n",
            N, (unsigned long)j->step, j->path, j->no, (unsigned long)j->len);
    else
        say("; [desync] SNAPSHOT FAILED: short write to %s (%lu of %lu bytes, err %lu)\n", j->path,
            (unsigned long)w, (unsigned long)j->len, GetLastError());
    HeapFree(GetProcessHeap(), 0, j->buf);
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

struct SnapBuf {
    uint8_t *p;
    uint32_t len, cap;
    bool     overflow;
};

void snap_put(SnapBuf &b, const void *src, uint32_t n) {
    if (b.overflow) return;
    if (b.len + n > b.cap) {
        // Grow geometrically; the first dump of a run sizes the next one exactly.
        uint32_t ncap = b.cap ? b.cap : (4u << 20);
        while (ncap < b.len + n) ncap *= 2;
        uint8_t *np = static_cast<uint8_t *>(
            b.p ? HeapReAlloc(GetProcessHeap(), 0, b.p, ncap) : HeapAlloc(GetProcessHeap(), 0, ncap));
        if (np == nullptr) {
            b.overflow = true;
            return;
        }
        b.p   = np;
        b.cap = ncap;
    }
    memcpy(b.p + b.len, src, n);
    b.len += n;
}

void do_snapshot(uint32_t step) {
    char path[MAX_PATH];
    wsprintfA(path, "%smh_desync_snap_%lu.bin", MH_RunDir(), (unsigned long)step);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        say("; [desync] SNAPSHOT FAILED: cannot create %s (err %lu)\n", path, GetLastError());
        return;
    }
    struct {
        uint32_t magic, ver, step, region_count;
        uint64_t manifest_fp;
    } hdr     = {SNAP_MAGIC, 1, step, (uint32_t)N, g_manifest_fp};
    SnapBuf b = {nullptr, 0, 0, false};
    snap_put(b, &hdr, sizeof(hdr));
    for (int i = 0; i < N; ++i) {
        const uint32_t at   = b.len; // where this region's length prefix goes
        uint32_t       zero = 0;
        snap_put(b, &zero, sizeof(zero));
        mh::state::fn_sink out(
            mh::state::sink_mode::VERDICT,
            [](void *ctx, const void *p, uint32_t n) { snap_put(*static_cast<SnapBuf *>(ctx), p, n); },
            &b);
        mh::state::emit_slice(i, out);
        if (b.overflow) break;
        const uint32_t len = b.len - at - sizeof(zero);
        memcpy(b.p + at, &len, sizeof(len));
    }
    if (b.overflow) {
        say("; [desync] SNAPSHOT FAILED: out of memory building the %lu-byte stream for %s\n",
            (unsigned long)b.len, path);
        if (b.p) HeapFree(GetProcessHeap(), 0, b.p);
        CloseHandle(h);
        return;
    }
    SnapJob *j = static_cast<SnapJob *>(HeapAlloc(GetProcessHeap(), 0, sizeof(SnapJob)));
    if (j == nullptr) {
        HeapFree(GetProcessHeap(), 0, b.p);
        CloseHandle(h);
        return;
    }
    j->h    = h;
    j->buf  = b.p;
    j->len  = b.len;
    j->step = step;
    j->no   = g_snaps_done + 1;
    lstrcpynA(j->path, path, MAX_PATH);
    HANDLE t = CreateThread(nullptr, 0, snapshot_writer, j, 0, nullptr);
    if (t == nullptr) {
        snapshot_writer(j); // no thread: still write it, on this thread, and say so through the line
        return;
    }
    CloseHandle(t);
}

// The per-step gate, shared by both hooks. Runs BEFORE the sampling-cadence gate: the dump must not
// depend on sampling_now()'s session checks -- a peer whose transport just dropped still owes its
// half of the evidence.
void snapshot_tick() {
    if (!g_snap_armed || g_snaps_done >= g_cfg.snapshot_max) return;
    if (!snapshot_due(g_step, g_cfg.every)) return;
    ++g_snaps_done;
    do_snapshot(g_step);
}

// ---- mp:D41: the state ring's two touch points -------------------------------------------------
// The session gate the ring records under: the same questions sampling_now() asks, minus the cadence
// (a live multi-peer lockstep session on a present, started transport).
bool ring_lockstep_live() {
    if (!mh::net::transport_present()) return false;
    if (*mh::state::ptr<const uint8_t>(mh::state::RID_GAME_SESSION_MODE) != SESSION_MP_LOCKSTEP) return false;
    return MH_Net_IsStarted() && MH_Net_PeerCount() > 0;
}

// THE hook: called once, at the FIRST detected state mismatch of the match (`step` = the mismatching
// sample's step). Freezes the ring and starts its tail; the ring itself latches once per match.
void on_first_mismatch(uint32_t step) { mh::desync::state_ring::on_first_mismatch(step); }

// ---- THE FIRST MISMATCH OF THE MATCH -------------------------------------------------------------
// The ONE place that reacts to "this match has diverged", from either comparison path (the v1 sampled
// verdict or the v2 per-step one), exactly once per match. `first_region` is -1 when it is not known
// yet (v2: the live localisation names it a link latency later). What reacts today: the D25 snapshot
// latch. mp:D41's ring flush belongs here too.
void on_first_mismatch_detected(uint32_t step, int sender, int first_region) {
    if (g_first_mismatch_seen) return;
    g_first_mismatch_seen = true;
    (void)sender;
    on_first_mismatch(step); // mp:D41: freeze the ship ring's run-up and start its tail
    // D25: arm the full-state dump. Arming is a latch, not a schedule -- the dump steps come from the
    // absolute grid so that peers which noticed at different moments still write files for the SAME
    // step, which is the only thing that makes them diffable.
    if (g_cfg.snapshot && !g_snap_armed) {
        g_snap_armed = true;
        say("; [desync] snapshot ARMED by the mismatch at step %lu (first_region=%d %s): up to %d full-state "
            "dumps, on every step divisible by %lu\n",
            (unsigned long)step, first_region,
            (first_region >= 0 && first_region < N) ? mh::state::HASH_REGIONS[first_region].name
                                                    : "(pending: live localisation)",
            g_cfg.snapshot_max, (unsigned long)snapshot_grid(g_cfg.every));
    }
}

// The order-digest verdict, shared by both paths. D31 R2: LOG + ROLLUP ONLY, never on-screen -- it is
// a strictly weaker signal (the order stream disagreed at or before this step; state itself may already
// be back in agreement) and, unlike the state channel, it never re-converges once it has fired, so an
// on-screen notice for it would be permanent for the rest of the match.
void order_verdict(uint32_t step, int sender, order_outcome oo, const char *state_here) {
    if (oo == order_outcome::mismatch) {
        ++g_order_mismatches;
        if (should_log_full(g_order_mismatches))
            say("; [desync] ORDER-DIGEST mismatch step=%lu peer=%d (mismatch #%d) -- the order stream "
                "disagreed at or before this step (state here is %s). LOG+ROLLUP ONLY (D31 R2): never "
                "an on-screen notice.\n",
                (unsigned long)step, sender, g_order_mismatches, state_here);
        else if (should_log_rollup(g_order_mismatches))
            say("; [desync] ORDER-DIGEST mismatch continues: %d mismatching sample(s), latest step=%lu "
                "peer=%d\n",
                g_order_mismatches, (unsigned long)step, sender);
    } else if (oo == order_outcome::absent) {
        ++g_order_absent_peer; // an rc2 peer, or a build with no order_queue region resolved
    }
}

// ---- the recv-thread handler --------------------------------------------------------------------
bool v2_frame_rx(int sender, const unsigned char *buf, int len);

void on_hash_frame_rx(int sender, const unsigned char *buf, int len) {
    if (!g_running || !g_cs_init) return;
    // mp:D44: a WIRE_VERSION 2 frame. With per_step=0 (or no tracker) this build answers exactly as a
    // v1 build does -- the v1 checks below refuse the version and count a bad frame.
    if (g_v2_armed && v2::peek_version(buf, len) == (int)v2::VERSION) {
        if (!v2_frame_rx(sender, buf, len)) InterlockedIncrement((volatile LONG *)&g_bad_frames);
        return;
    }
    // The v1 acceptance rule (desync_watch.h parse_v1_frame -- the rc builds' own code, moved there
    // unchanged so desynctest can run a v2 frame through exactly what an older peer does with it).
    sample_wire s;
    uint64_t    order_digest = 0;
    bool        has_digest   = false;
    if (!parse_v1_frame(buf, len, s, order_digest, has_digest)) {
        InterlockedIncrement((volatile LONG *)&g_bad_frames);
        return;
    }
    // mp:D44: a sane v1 frame means that peer runs an older build. Only a v2 build acts on it (it falls
    // back to the FNV walk for that peer); with per_step=0 the walk runs anyway.
    if (sender >= 0 && sender < VPEERS && InterlockedExchange(&g_peer_v1[sender], 1) == 0)
        InterlockedExchange(&g_v1_seen, 1);
    // A peer that RECEIVES samples while producing none is not desynced -- it is not RUNNING the
    // sampler, because the only per-step hook this has is the turn engine's live sim_step edge and
    // `[promote] lockstep` is off here, so on_sim_step never executes and nothing below the recv
    // thread ever runs. That is a real configuration (the ASYMMETRIC determinism shape deliberately
    // runs the original engine on one peer), and it is reported HERE because there is no main-thread
    // tick left to report it from. Saying it once beats the silence, which is indistinguishable from
    // a clean match.
    if (g_step == 0 && ++g_rx_while_inert == 8) {
        say("; [desync] INERT on this peer: 8 samples received but zero sim steps seen -- the turn "
            "engine is not promoted here ([promote] lockstep=0), so there is no per-step sampling "
            "point. Not a desync; no verdict is produced on this side.\n");
        g_running = false;
        return;
    }
    EnterCriticalSection(&g_cs);
    g_pending.push(sender, s, order_digest, has_digest);
    LeaveCriticalSection(&g_cs);
}

// ---- report one judged sample (main thread) ----------------------------------------------------
// `theirs_order_digest`/`theirs_has_order_digest` are the D31 clause C payload that rode alongside
// this sample (see on_hash_frame_rx) -- independent of which STATE outcome `v` carries, because an
// order disagreement can hide behind a re-converged state (the whole point of the digest: it never
// re-converges, so it still shows at the next sample after the state healed).
void report(int sender, const verdict &v, uint64_t theirs_order_digest, bool theirs_has_order_digest) {
    switch (v.kind) {
        case outcome::ok:
            ++g_compared;
            g_consecutive_state_mismatches = 0; // R2: an agreeing sample breaks a mismatch run
            if (g_cfg.verbose)
                say("; [desync] step=%lu peer=%d MATCH state=%08X%08X\n", (unsigned long)v.step, sender,
                    (unsigned)(v.mine >> 32), (unsigned)v.mine);
            break;

        case outcome::mismatch: {
            ++g_compared;
            ++g_mismatches;
            ++g_consecutive_state_mismatches;
            const char *rname = (v.first_region >= 0 && v.first_region < N)
                                    ? mh::state::HASH_REGIONS[v.first_region].name
                                    : "(none -- state hashes differ but every compared region agrees)";
            if (should_log_full(g_mismatches))
                say("; [desync] *** DESYNC step=%lu peer=%d mine=%08X%08X theirs=%08X%08X "
                    "first_region=%d %s (mismatch #%d)\n",
                    (unsigned long)v.step, sender, (unsigned)(v.mine >> 32), (unsigned)v.mine,
                    (unsigned)(v.theirs >> 32), (unsigned)v.theirs, v.first_region, rname, g_mismatches);
            else if (should_log_rollup(g_mismatches))
                say("; [desync] *** DESYNC continues: %d mismatching samples, latest step=%lu peer=%d "
                    "first_region=%d %s\n",
                    g_mismatches, (unsigned long)v.step, sender, v.first_region, rname);
            // R2 (user ruling, 2026-09-24): the on-screen notice needs PERSISTENCE, not just a first
            // mismatching sample -- see should_notify's header comment. A state mismatch that
            // reconverges at the very next sample never reaches g_consecutive_state_mismatches==2.
            if (should_notify(g_consecutive_state_mismatches)) notify_once(v.step);
            on_first_mismatch_detected(v.step, sender, v.first_region);
            break;
        }

        case outcome::too_old:
            // Evidence we no longer hold. Counted, never reported as a desync -- see the header.
            ++g_too_old;
            if ((g_too_old % 64) == 1)
                say("; [desync] sample for step=%lu from peer=%d arrived after its ring entry was "
                    "evicted -- dropped (%d so far; raise [desync] every or RING_CAP if this is not "
                    "rare)\n",
                    (unsigned long)v.step, sender, g_too_old);
            break;

        case outcome::manifest_mismatch:
            ++g_manifest_bad;
            if (g_manifest_bad == 1)
                say("; [desync] DISABLED: peer=%d hashes a DIFFERENT manifest (theirs fp/count vs "
                    "ours %08X%08X/%d). This is a build mismatch, NOT a desync -- comparing would "
                    "report one on every sample.\n",
                    sender, (unsigned)(g_manifest_fp >> 32), (unsigned)g_manifest_fp, N);
            g_running = false; // stop comparing; a mixed-build pair has nothing comparable to say
            break;

        case outcome::bad_frame:
        case outcome::not_yet:
        default: return; // no ring entry was judged against -- nothing for the order channel below either
    }

    // D31 clause C, R2: the order-digest channel (order_verdict above).
    order_verdict(v.step, sender, judge_order(g_ring, v.step, theirs_order_digest, theirs_has_order_digest),
                  v.kind == outcome::ok ? "back in agreement" : "ALSO mismatching");
}

// Judge everything queued that we can judge; put back what is still in the future.
void drain_pending() {
    // Bounded by PENDING_CAP: pop everything, judge, and re-push only the future ones. Doing it in
    // one pass under the lock would hold the recv thread off for the whole judgement.
    sample_wire s;
    int         from   = 0;
    uint64_t    od     = 0;
    bool        has_od = false;
    int         guard  = PENDING_CAP + 1;
    while (guard-- > 0) {
        EnterCriticalSection(&g_cs);
        const bool got = g_pending.pop(from, s, &od, &has_od);
        LeaveCriticalSection(&g_cs);
        if (!got) break;
        // mp:D44: a v1 sample for a step before this build fell back to the walk has no partner in the
        // ring by construction -- skipped and counted, never called too_old.
        if (g_v2_armed && s.step < g_legacy_from) {
            ++g_pre_fallback;
            continue;
        }
        const verdict v = judge(g_ring, s, g_excluded, N, g_manifest_fp, g_ring.newest);
        if (v.kind == outcome::not_yet) {
            EnterCriticalSection(&g_cs);
            g_pending.push(from, s, od, has_od); // still ahead of us; look again next sample
            LeaveCriticalSection(&g_cs);
            break; // the queue is in arrival order, so everything behind it is at least as new
        }
        report(from, v, od, has_od);
        if (!g_running) break; // manifest mismatch shut us down
    }
}

// The periodic proof of life, and it is not decoration. A detector that speaks only when it finds
// something is indistinguishable from one that never ran -- which is the shape this ledger refuses
// everywhere else, and the reason clause (b) of D21 is "zero detections over a full clean run"
// rather than "no complaints". `compared=N mismatching=0` is the difference between a clean verdict
// and no verdict.
//
// The COST half reports TWO populations separately, because they are different measurements.
// `g_hash_samples` are the samples this module walked the 2.79 MB manifest for -- the real added
// cost. `g_reused_samples` came pre-hashed from the determinism harness, which had already paid for
// its own log; those add nothing and must not be averaged into the mean or the number loses meaning.
void emit_status_line() {
    char cost[320];
    if (g_v2_armed && g2_steps && g_qpc_freq) {
        // mp:D44: the per-step path's cost is the shared tracker's update (with every listener's run
        // sink and the undo journal in it) plus the fold, the compare and the TICK -- per sim step.
        const double f    = 1e6 / (double)g_qpc_freq;
        const double mean = (double)g2_ticks * f / (double)g2_steps;
        const double bps  = (double)g2_tx_bytes * 50.0 / (double)g2_steps; // at 50 steps/s
        _snprintf_s(cost, sizeof(cost), _TRUNCATE,
                    "per-step %.0f us mean, %.0f us max over %ld step(s) (tracker update + journal + compare, "
                    "HASH_KIND %lu); v2 wire %.0f B/s sent at 50 steps/s, %d step(s) per TICK%s",
                    mean, (double)g2_max_ticks * f, (long)g2_steps, (unsigned long)mh::state::inc::HASH_KIND, bps,
                    g_cfg.tick_batch, g_legacy_on ? "; FALLBACK walk on for a v1 peer" : "");
    } else if (g_hash_samples && g_qpc_freq) {
        const double us          = (double)g_hash_ticks * 1000000.0 / (double)g_qpc_freq / (double)g_hash_samples;
        const double per_step_us = us / (double)(g_cfg.every > 0 ? g_cfg.every : 1);
        wsprintfA(cost, "own-hash mean %d.%02d us -> %d.%03d us amortised per SIM STEP", (int)us, // mh-str-ok: log
                  (int)((us - (int)us) * 100.0), (int)per_step_us,
                  (int)((per_step_us - (int)per_step_us) * 1000.0));
    } else if (g_reused_samples) {
        wsprintfA(cost, "0 us added -- every sample reused a hash this run was computing anyway"); // mh-str-ok: log
    } else {
        wsprintfA(cost, "no hash timed yet"); // mh-str-ok: log
    }
    // `compared` / `mismatching` / `too_old` are the SUM of the two paths (v1 samples + v2 steps) --
    // the verdict a reader wants; the v2_* keys split the per-step half out.
    say("; [desync] STATUS: samples=%d (own %d / reused %d) compared=%d mismatching=%d too_old=%d "
        "bad_frames=%d order_mismatching=%d order_absent=%d step=%lu per_step=%d v2_compared=%d "
        "v2_mismatching=%d v2_incidents=%d localised=%d fallback=%d | COST %s (every=%d, %d "
        "regions, %d wire bytes/sample)\n",
        (int)(g_hash_samples + g_reused_samples + g2_steps), (int)g_hash_samples, (int)g_reused_samples,
        g_compared + g2_compared, g_mismatches + g2_mismatches, g_too_old + g2_too_old, g_bad_frames,
        g_order_mismatches, g_order_absent_peer, (unsigned long)g_step, g_v2_armed ? 1 : 0, g2_compared,
        g2_mismatches, g2_incidents, g_loc.nresults(), g_legacy_on ? 1 : 0, cost, g_cfg.every, N, wire_size(N));
}

// D21 clause (d): the cost has to be MEASURED and the cadence chosen from that number rather than
// assumed. Measured HERE, at arm time, and not left to a live sample, for one reason: in every run
// that has a determinism harness the live samples reuse the harness's hash and cost nothing, so the
// one configuration that can never measure itself is the shipped one -- where nobody is watching and
// nobody will go looking. Three walks of the real manifest at real addresses; the bytes are the
// game's own .bss either way and FNV is branch-free over their values, so an early-boot walk costs
// what an in-match one costs.
void cost_probe() {
    if (!g_qpc_freq) return;
    constexpr int REPS = 3;
    // One WARM-UP walk outside the timing. The manifest is .bss and this runs at DLL arm time, so the
    // first pass eats the demand-zero page faults for 2.79 MB and would inflate the mean by
    // whatever the fault cost happened to be -- a number about the OS, reported as a number about
    // the hash.
    for (int i = 0; i < N; ++i) g_per[i] = mh::state::hash_slice(i, true, true);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint64_t sink = 0;
    for (int r = 0; r < REPS; ++r) {
        for (int i = 0; i < N; ++i) g_per[i] = mh::state::hash_slice(i, true, true);
        sink ^= fold_state(g_per, g_excluded, N);
    }
    QueryPerformanceCounter(&t1);
    const double us =
        (double)(t1.QuadPart - t0.QuadPart) * 1000000.0 / (double)g_qpc_freq / (double)REPS;
    const double per_step_us = us / (double)(g_cfg.every > 0 ? g_cfg.every : 1);
    say("; [desync] COST PROBE: %d walks of the %d-region manifest, mean %d.%02d us each -> %d.%03d "
        "us amortised per SIM STEP at every=%d (fingerprint %08X). This is what the SHIPPED path "
        "pays; a run with the determinism harness reuses its hash and adds nothing.\n",
        REPS, N, (int)us, (int)((us - (int)us) * 100.0), (int)per_step_us,
        (int)((per_step_us - (int)per_step_us) * 1000.0), g_cfg.every, (unsigned)sink);
}

// ---- the dirty-block probe (desync_watch.h "the dirty-block probe") ------------------------------
// The shadow is every manifest slice laid end to end. The FIRST tick of a match only primes it (a
// delta against nothing is not a step's writes); every later tick measures one step: the bytes the
// previous step's body wrote, since both ticks sit at the same PRE-BODY boundary.
//
// SINCE mp:D40 THE TRACKER IS SHARED (desync/state_tracker.h): the probe is one listener on the hub
// the recorder also listens on, so a run with both pays one scan per step, and the probe's scan_us
// then includes the recorder's run appends (the hub times the update with every sink in it).
//
// SINCE mp:D39 THE SCAN IS THE INCREMENTAL STATE CORE (libmh/state/inc_state.h), not dirty_scan: the
// one memcmp-against-a-shadow a step can afford now also maintains the masked sum-of-block-hashes
// (HASH_KIND 2) that the recorder (D40), per-step detection (D44) and the harness (TL-HARN-INCHASH)
// will read. The core hands back EXACT changed-byte runs (update gap 0), from which the probe's own
// numbers are rebuilt unchanged: bytes per region, first changed offset, and the address-aligned
// 64/256/1K/4K grains (dirty_grains_add, asserted against dirty_scan in desynctest). So `scan_us` in
// the CSV is now scan + copy + block re-hash -- the real per-step cost of the core.
//
// D39 clause (d): every INC_VERIFY_EVERY steps the incremental hashes are checked against a
// from-scratch recompute over LIVE memory (not the shadow, so a stale shadow shows up), and the
// agreement count, the recompute cost and the per-step update cost are in the match-end rollup.
struct DirtyRegion {
    uint64_t bytes;
    uint64_t grains[DIRTY_NGRAINS];
    uint32_t steps_dirty;
    uint32_t max_bytes;
    uint64_t tail; // sum over dirty steps of (len - first changed block): a checkpointed FNV's re-hash
};

constexpr uint32_t INC_VERIFY_EVERY = 1000;

bool        g_dp_on    = false; // [desync] dirty_probe=1 and the shared tracker is allocated
uint32_t    g_dp_total = 0;
FILE       *g_dp_csv   = nullptr;
DirtyRegion g_dp_reg[N];
uint32_t    g_dp_step_bytes[N]; // this step's per-region exact bytes, for the CSV row
uint64_t    g_dp_steps     = 0;
uint64_t    g_dp_sum_bytes = 0;
uint64_t    g_dp_sum_grains[DIRTY_NGRAINS];
uint64_t    g_dp_max_grain_bytes[DIRTY_NGRAINS]; // worst single step, as grains x grain size
uint32_t    g_dp_max_bytes = 0;
int64_t     g_dp_ticks = 0, g_dp_max_ticks = 0;
uint32_t    g_dp_first[N]; // this step's per-region first changed offset (len = clean)
// D39 (d): the periodic self-check, and what the core did on the way.
uint64_t g_inc_checks = 0, g_inc_bad_checks = 0, g_inc_bad_regions = 0;
int64_t  g_inc_verify_ticks = 0, g_inc_verify_max_ticks = 0;
uint64_t g_inc_rehashed = 0, g_inc_rebased = 0;
int      g_inc_first_bad      = -1;
uint32_t g_inc_first_bad_step = 0;

// Turns the core's exact runs back into the probe's per-step grain counts.
struct ProbeRunSink {
    uint32_t  grains[DIRTY_NGRAINS];
    int       region = -1;
    uintptr_t last[DIRTY_NGRAINS];
    uint32_t  region_grains[N][DIRTY_NGRAINS];
    void      begin() {
        memset(grains, 0, sizeof(grains));
        memset(region_grains, 0, sizeof(region_grains));
        region = -1;
    }
    void run(int r, uint32_t off, uint32_t len, const uint8_t *) {
        if (r != region) { // dirty_scan resets its grain memory per region; so do we
            region = r;
            for (int g = 0; g < DIRTY_NGRAINS; ++g) last[g] = ~(uintptr_t)0;
        }
        uint32_t before[DIRTY_NGRAINS];
        memcpy(before, region_grains[r], sizeof(before));
        dirty_grains_add((uintptr_t)mh::state::hash_base(r) + off, len, last, region_grains[r]);
        for (int g = 0; g < DIRTY_NGRAINS; ++g) grains[g] += region_grains[r][g] - before[g];
    }
    void rebased(int r) {
        ++g_inc_rebased;
        say("; [desync] INC HASH: slice %d %s moved to %08X -- re-primed at its new address\n", r,
            mh::state::HASH_REGIONS[r].name, (unsigned)mh::state::hash_base(r));
    }
};
ProbeRunSink g_dp_runs;

// THE CHAINED-HASH ARM (what a CHECKPOINTED FNV would cost). FNV folds each word into everything
// before it, so a changed word cannot be patched in place: keep FNV's running value at every
// 64-byte boundary and re-run from the first changed block to the region's end. That keeps today's
// hash values bit-identical. Its cost per step = hashing those tails, timed below with the same
// hash_sink the harness uses (raw bytes: the VERDICT per-field emission of units/tile_objects is
// slower, so this is a slight UNDER-estimate for those two). Compared against the two full walks,
// sampled every DP_FULL_EVERY steps so the probe does not itself double the cost of a step.
constexpr uint32_t DP_FULL_EVERY   = 100;
uint64_t           g_dp_ckpt_bytes = 0, g_dp_ckpt_max_bytes = 0;
int64_t            g_dp_ckpt_ticks = 0, g_dp_ckpt_max_ticks = 0;
int64_t            g_dp_raw_ticks = 0, g_dp_ver_ticks = 0; // full raw hash_sink walk / full hash_slice walk
uint64_t           g_dp_full_samples = 0;
volatile uint64_t  g_dp_sink         = 0; // keeps the timed hashes from being optimised away

const mh::state::inc::tracker &dp_inc() { return mh::desync::state_hub::tracker(); }

void dirty_probe_arm() {
    uint32_t total = 0;
    for (int i = 0; i < N; ++i) total += mh::state::HASH_REGIONS[i].len;
    if (!mh::desync::state_hub::enable("the dirty probe", &say)) {
        say("; [desync] DIRTY PROBE NOT armed: no shared state tracker\n");
        return;
    }
    g_dp_on    = true;
    g_dp_total = total;
    say("; [desync] DIRTY PROBE armed: %d regions, %lu-byte manifest, on the shared state tracker (%lu-byte "
        "arena, HASH_KIND %lu); per-step rows -> mh_dirty_probe.csv, rollup at match end, incremental vs "
        "from-scratch recompute every %lu steps\n",
        N, (unsigned long)total, (unsigned long)mh::desync::state_hub::arena_bytes(),
        (unsigned long)mh::state::inc::HASH_KIND, (unsigned long)INC_VERIFY_EVERY);
    // D39's flat-bytes premise, said at arm time: an owned slice whose owner is not proven to emit
    // its own bytes would get a kind-2 value that is NOT the verdict.
    const int nonflat = mh::state::inc::first_nonflat_slice();
    if (nonflat >= 0)
        say("; [desync] INC HASH WARNING: slice %d %s is served by an owner not proven to emit flat bytes -- "
            "its incremental hash does NOT describe the VERDICT stream\n",
            nonflat, mh::state::HASH_REGIONS[nonflat].name);
}

void dirty_probe_clear() {
    memset(g_dp_reg, 0, sizeof(g_dp_reg));
    g_dp_steps     = 0;
    g_dp_sum_bytes = 0;
    memset(g_dp_sum_grains, 0, sizeof(g_dp_sum_grains));
    memset(g_dp_max_grain_bytes, 0, sizeof(g_dp_max_grain_bytes));
    g_dp_max_bytes  = 0;
    g_dp_ticks      = 0;
    g_dp_max_ticks  = 0;
    g_dp_ckpt_bytes = g_dp_ckpt_max_bytes = 0;
    g_dp_ckpt_ticks = g_dp_ckpt_max_ticks = 0;
    g_dp_raw_ticks = g_dp_ver_ticks = 0;
    g_dp_full_samples               = 0;
    g_inc_checks = g_inc_bad_checks = g_inc_bad_regions = 0;
    g_inc_verify_ticks = g_inc_verify_max_ticks = 0;
    g_inc_rehashed = g_inc_rebased = 0;
    g_inc_first_bad                = -1;
    g_inc_first_bad_step           = 0;
}

void dirty_probe_open_csv() {
    if (!g_dp_on || g_dp_csv) return;
    char path[MAX_PATH];
    wsprintfA(path, "%smh_dirty_probe.csv", MH_RunDir());
    if (fopen_s(&g_dp_csv, path, "w") != 0 || !g_dp_csv) {
        g_dp_csv = nullptr;
        say("; [desync] DIRTY PROBE: cannot create %s -- rollup only\n", path);
        return;
    }
    setvbuf(g_dp_csv, nullptr, _IOFBF, 256 * 1024);
    fprintf(g_dp_csv, "step,scan_us,bytes,g64,g256,g1k,g4k,ckpt_bytes,ckpt_us");
    for (int i = 0; i < N; ++i) fprintf(g_dp_csv, ",%s", mh::state::HASH_REGIONS[i].name);
    fprintf(g_dp_csv, "\n");
    say("; [desync] DIRTY PROBE: per-step rows -> %s\n", path);
}

// D39 (d): incremental vs from-scratch, over LIVE memory. Outside the per-step timing.
void inc_verify_tick() {
    LARGE_INTEGER v0, v1;
    QueryPerformanceCounter(&v0);
    int       first = -1;
    const int bad   = dp_inc().verify(&first);
    QueryPerformanceCounter(&v1);
    const int64_t dt = v1.QuadPart - v0.QuadPart;
    g_inc_verify_ticks += dt;
    if (dt > g_inc_verify_max_ticks) g_inc_verify_max_ticks = dt;
    ++g_inc_checks;
    if (bad == 0) return;
    ++g_inc_bad_checks;
    g_inc_bad_regions += (uint64_t)bad;
    if (g_inc_first_bad < 0) {
        g_inc_first_bad      = first;
        g_inc_first_bad_step = g_step;
    }
    if (g_inc_bad_checks <= 4)
        say("; [desync] INC HASH DISAGREES with a from-scratch recompute at step %lu: %d region(s), first %d %s "
            "(check #%lu)\n",
            (unsigned long)g_step, bad, first, mh::state::HASH_REGIONS[first].name, (unsigned long)g_inc_checks);
}

// One step's accounting, AFTER the hub updated the shared tracker (gap 0: EXACT runs, so the byte and
// grain counts stay exact) and fed this probe's run sink. A priming step is not a delta and is skipped.
void dirty_probe_account() {
    const mh::state::inc::tracker &inc = dp_inc();
    const int64_t                  dt  = mh::desync::state_hub::last_ticks();
    g_dp_ticks += dt;
    if (dt > g_dp_max_ticks) g_dp_max_ticks = dt;
    g_inc_rehashed += inc.last.rehashed_blocks;
    dirty_counts step = {};
    step.bytes        = inc.last.changed_bytes;
    for (int g = 0; g < DIRTY_NGRAINS; ++g) step.grains[g] = g_dp_runs.grains[g];
    for (int i = 0; i < N; ++i) {
        const uint32_t b   = inc.changed_bytes(i);
        g_dp_step_bytes[i] = b;
        g_dp_first[i]      = inc.first_changed(i);
        if (b) {
            DirtyRegion &r = g_dp_reg[i];
            r.bytes += b;
            for (int g = 0; g < DIRTY_NGRAINS; ++g) r.grains[g] += g_dp_runs.region_grains[i][g];
            ++r.steps_dirty;
            if (b > r.max_bytes) r.max_bytes = b;
        }
    }
    ++g_dp_steps;
    g_dp_sum_bytes += step.bytes;
    if (step.bytes > g_dp_max_bytes) g_dp_max_bytes = step.bytes;
    for (int g = 0; g < DIRTY_NGRAINS; ++g) {
        g_dp_sum_grains[g] += step.grains[g];
        const uint64_t gb = (uint64_t)step.grains[g] * DIRTY_GRAIN[g];
        if (gb > g_dp_max_grain_bytes[g]) g_dp_max_grain_bytes[g] = gb;
    }
    if ((g_step % INC_VERIFY_EVERY) == 0) inc_verify_tick();
    // The chained-hash arm: re-hash each dirty region from its first changed 64-byte block.
    uint64_t      ckpt_bytes = 0;
    LARGE_INTEGER c0, c1;
    QueryPerformanceCounter(&c0);
    for (int i = 0; i < N; ++i) {
        const uint32_t len = mh::state::HASH_REGIONS[i].len;
        if (g_dp_first[i] >= len) continue;
        const uint32_t       from = g_dp_first[i] & ~63u;
        mh::state::hash_sink hs;
        hs.raw(reinterpret_cast<const uint8_t *>(mh::state::hash_base(i)) + from, len - from);
        g_dp_sink ^= hs.finish();
        ckpt_bytes += len - from;
        g_dp_reg[i].tail += len - from;
    }
    QueryPerformanceCounter(&c1);
    const int64_t cdt = c1.QuadPart - c0.QuadPart;
    g_dp_ckpt_ticks += cdt;
    if (cdt > g_dp_ckpt_max_ticks) g_dp_ckpt_max_ticks = cdt;
    g_dp_ckpt_bytes += ckpt_bytes;
    if (ckpt_bytes > g_dp_ckpt_max_bytes) g_dp_ckpt_max_bytes = ckpt_bytes;
    // The two baselines, sampled: a full raw walk (same sink, all bytes) and the real VERDICT walk.
    if ((g_step % DP_FULL_EVERY) == 0) {
        LARGE_INTEGER f0, f1, f2;
        QueryPerformanceCounter(&f0);
        for (int i = 0; i < N; ++i) {
            mh::state::hash_sink hs;
            hs.raw(reinterpret_cast<const uint8_t *>(mh::state::hash_base(i)), mh::state::HASH_REGIONS[i].len);
            g_dp_sink ^= hs.finish();
        }
        QueryPerformanceCounter(&f1);
        for (int i = 0; i < N; ++i) g_dp_sink ^= mh::state::hash_slice(i, true, true);
        QueryPerformanceCounter(&f2);
        g_dp_raw_ticks += f1.QuadPart - f0.QuadPart;
        g_dp_ver_ticks += f2.QuadPart - f1.QuadPart;
        ++g_dp_full_samples;
    }
    if (g_dp_csv) { // outside the timed window: the CSV is the probe's cost, not the recorder's
        const double us = g_qpc_freq ? (double)dt * 1e6 / (double)g_qpc_freq : 0.0;
        fprintf(g_dp_csv, "%lu,%.1f,%lu,%lu,%lu,%lu,%lu", (unsigned long)g_step, us, (unsigned long)step.bytes,
                (unsigned long)step.grains[0], (unsigned long)step.grains[1], (unsigned long)step.grains[2],
                (unsigned long)step.grains[3]);
        fprintf(g_dp_csv, ",%lu,%.1f", (unsigned long)ckpt_bytes,
                g_qpc_freq ? (double)cdt * 1e6 / (double)g_qpc_freq : 0.0);
        for (int i = 0; i < N; ++i) fprintf(g_dp_csv, ",%lu", (unsigned long)g_dp_step_bytes[i]);
        fprintf(g_dp_csv, "\n");
    }
}

// The rollup, then zero. Mean bytes per step at each grain is what a delta ring holds per step, so
// it is also printed as the RAM one 30 s window (1500 steps at 50 steps/s) would need.
void dirty_probe_report() {
    if (g_dp_csv) {
        fclose(g_dp_csv);
        g_dp_csv = nullptr;
    }
    if (!g_dp_on || g_dp_steps == 0) {
        dirty_probe_clear();
        return;
    }
    const double steps = (double)g_dp_steps;
    const double f     = g_qpc_freq ? 1e6 / (double)g_qpc_freq : 0.0;
    say("; [desync] DIRTY PROBE: %lu steps over a %lu-byte manifest | scan+copy mean %.0f us, max %.0f "
        "us | changed per step: exact mean %.0f B (max %lu)\n",
        (unsigned long)g_dp_steps, (unsigned long)g_dp_total, (double)g_dp_ticks * f / steps,
        (double)g_dp_max_ticks * f, (double)g_dp_sum_bytes / steps, (unsigned long)g_dp_max_bytes);
    for (int g = 0; g < DIRTY_NGRAINS; ++g) {
        const double mean_b = (double)g_dp_sum_grains[g] * DIRTY_GRAIN[g] / steps;
        say("; [desync] DIRTY PROBE grain %4lu B: mean %.0f B/step (%.1f grains), max %lu B/step -> "
            "30 s ring ~%.1f MB\n",
            (unsigned long)DIRTY_GRAIN[g], mean_b, (double)g_dp_sum_grains[g] / steps,
            (unsigned long)g_dp_max_grain_bytes[g], mean_b * 1500.0 / (1024.0 * 1024.0));
    }
    if (g_dp_full_samples) {
        const double ck_us  = (double)g_dp_ckpt_ticks * f / steps;
        const double raw_us = (double)g_dp_raw_ticks * f / (double)g_dp_full_samples;
        const double ver_us = (double)g_dp_ver_ticks * f / (double)g_dp_full_samples;
        say("; [desync] DIRTY PROBE chained-hash arm (checkpointed FNV, today's values kept): re-hash mean "
            "%.0f B/step (max %lu) = %.1f%% of the manifest, %.0f us/step (max %.0f) | full walk: raw %.0f us, "
            "VERDICT (hash_slice) %.0f us (%lu samples) | sum-of-blocks arm = the scan, %.0f us\n",
            (double)g_dp_ckpt_bytes / steps, (unsigned long)g_dp_ckpt_max_bytes,
            100.0 * (double)g_dp_ckpt_bytes / steps / (double)g_dp_total, ck_us, (double)g_dp_ckpt_max_ticks * f,
            raw_us, ver_us, (unsigned long)g_dp_full_samples, (double)g_dp_ticks * f / steps);
    }
    int order[N];
    int m = 0;
    for (int i = 0; i < N; ++i)
        if (g_dp_reg[i].bytes) order[m++] = i;
    for (int a = 0; a < m; ++a) // m <= 63, once per match: selection sort by 256-B grain volume
        for (int b = a + 1; b < m; ++b)
            if (g_dp_reg[order[b]].grains[1] > g_dp_reg[order[a]].grains[1]) {
                const int t = order[a];
                order[a]    = order[b];
                order[b]    = t;
            }
    for (int k = 0; k < m; ++k) {
        const int          i = order[k];
        const DirtyRegion &r = g_dp_reg[i];
        say("; [desync] DIRTY region %-18s len=%7lu dirty %lu/%lu steps | mean exact %.0f B, 256B-grain "
            "%.0f B, 4K pages %.2f | max exact %lu B | chained re-hash %.0f B/step\n",
            mh::state::HASH_REGIONS[i].name, (unsigned long)mh::state::HASH_REGIONS[i].len,
            (unsigned long)r.steps_dirty, (unsigned long)g_dp_steps, (double)r.bytes / steps,
            (double)r.grains[1] * 256.0 / steps, (double)r.grains[3] / steps, (unsigned long)r.max_bytes,
            (double)r.tail / steps);
    }
    say("; [desync] DIRTY PROBE: %d of %d regions changed at least once\n", m, N);
    // D39 (d): the incremental core's own verdict on itself. `disagreeing` must be 0 on every run; the
    // update cost is the scan+copy+re-hash above (the number the recorder and per-step detection pay).
    {
        const double vf = g_inc_checks ? (double)g_inc_verify_ticks * f / (double)g_inc_checks : 0.0;
        say("; [desync] INC HASH (kind %lu): %lu recompute checks (every %lu steps), %lu DISAGREEING (%lu region "
            "mismatches, first %s at step %lu) | update mean %.0f us, max %.0f us, %.1f blocks re-hashed/step | "
            "from-scratch recompute mean %.0f us, max %.0f us | %lu slice rebase(s) | final state %08X%08X\n",
            (unsigned long)mh::state::inc::HASH_KIND, (unsigned long)g_inc_checks, (unsigned long)INC_VERIFY_EVERY,
            (unsigned long)g_inc_bad_checks, (unsigned long)g_inc_bad_regions,
            g_inc_first_bad >= 0 ? mh::state::HASH_REGIONS[g_inc_first_bad].name : "-",
            (unsigned long)g_inc_first_bad_step, (double)g_dp_ticks * f / steps, (double)g_dp_max_ticks * f,
            (double)g_inc_rehashed / steps, vf, (double)g_inc_verify_max_ticks * f, (unsigned long)g_inc_rebased,
            (unsigned)(dp_inc().state_hash() >> 32), (unsigned)dp_inc().state_hash());
    }
    dirty_probe_clear();
}

// The probe as a listener on the shared tracker (desync/state_tracker.h).
struct ProbeListener final : mh::desync::state_hub::listener {
    bool wants_step() const override { return g_dp_on; }
    void step_begin(uint32_t, bool) override { g_dp_runs.begin(); }
    void run(int r, uint32_t off, uint32_t len, const uint8_t *b) override { g_dp_runs.run(r, off, len, b); }
    void rebased(int r) override { g_dp_runs.rebased(r); }
    void step_end(uint32_t, bool primed) override {
        if (!primed) dirty_probe_account();
    }
};
ProbeListener g_dp_listener;

// ---- the step-axis measurement for the recorder's open line (mp:D40) -----------------------------
// How many sim steps ran in the CURRENT session folder before session_begin_multi. The harness opens
// its per-match segment (mh_match_harness.log, step_base) on the first sim step that sees the session
// folder, and this module's step 1 is the first step after session_begin_multi, so this count IS the
// offset between the two axes -- measured per match, not assumed. Only counted while the recorder is
// configured on.
unsigned long g_axis_gen   = 0;
uint32_t      g_axis_steps = 0;

void axis_tick() {
    if (!MH_RunDir_SessionActive()) {
        g_axis_gen   = 0;
        g_axis_steps = 0;
        return;
    }
    const unsigned long gen = MH_RunDirGeneration();
    if (gen != g_axis_gen) {
        g_axis_gen   = gen;
        g_axis_steps = 0;
    }
    ++g_axis_steps;
}

} // namespace

// ================================================================================================
// public API
// ================================================================================================

void set_logger(void (*fn)(const char *)) { g_log = fn; }

int install(const char *ini_path) {
    g_cfg.enabled      = GetPrivateProfileIntA("desync", "enabled", g_cfg.enabled, ini_path);
    g_cfg.every        = GetPrivateProfileIntA("desync", "every", g_cfg.every, ini_path);
    g_cfg.action       = GetPrivateProfileIntA("desync", "action", g_cfg.action, ini_path);
    g_cfg.verbose      = GetPrivateProfileIntA("desync", "verbose", g_cfg.verbose, ini_path);
    g_cfg.snapshot     = GetPrivateProfileIntA("desync", "snapshot", g_cfg.snapshot, ini_path);
    g_cfg.snapshot_max = GetPrivateProfileIntA("desync", "snapshot_max", g_cfg.snapshot_max, ini_path);
    g_cfg.dirty_probe  = GetPrivateProfileIntA("desync", "dirty_probe", g_cfg.dirty_probe, ini_path);
    g_cfg.state_record = GetPrivateProfileIntA("desync", "state_record", g_cfg.state_record, ini_path);
    g_cfg.state_keyframe_every =
        GetPrivateProfileIntA("desync", "state_keyframe_every", g_cfg.state_keyframe_every, ini_path);
    g_cfg.ring.enabled   = GetPrivateProfileIntA("desync", "state_ring", g_cfg.ring.enabled, ini_path);
    g_cfg.ring.seconds   = GetPrivateProfileIntA("desync", "state_ring_s", g_cfg.ring.seconds, ini_path);
    g_cfg.ring.tail_s    = GetPrivateProfileIntA("desync", "state_ring_tail_s", g_cfg.ring.tail_s, ini_path);
    g_cfg.ring.max_kb    = GetPrivateProfileIntA("desync", "state_ring_max_kb", g_cfg.ring.max_kb, ini_path);
    g_cfg.per_step       = GetPrivateProfileIntA("desync", "per_step", g_cfg.per_step, ini_path);
    g_cfg.tick_batch     = GetPrivateProfileIntA("desync", "tick_batch", g_cfg.tick_batch, ini_path);
    g_cfg.localise_max   = GetPrivateProfileIntA("desync", "localise_max", g_cfg.localise_max, ini_path);
    g_cfg.loc_history_kb = GetPrivateProfileIntA("desync", "loc_history_kb", g_cfg.loc_history_kb, ini_path);
    if (g_cfg.tick_batch < 1) g_cfg.tick_batch = 1;
    if (g_cfg.tick_batch > v2::TICK_MAX_STEPS) g_cfg.tick_batch = v2::TICK_MAX_STEPS;
    if (g_cfg.localise_max < 0) g_cfg.localise_max = 0;
    if (g_cfg.loc_history_kb < 64) g_cfg.loc_history_kb = 64;
    if (g_cfg.loc_history_kb > 65536) g_cfg.loc_history_kb = 65536;

    if (!g_cfg.enabled) {
        say("; [desync] NOT armed: [desync] enabled=0%s\n",
            g_cfg.state_record ? " (so state_record=1 records nothing: the recorder rides this module's step hook)"
                               : "");
        return 0;
    }
#ifndef MH_LIBMH_BUILD
    // HOSTED ONLY, and the guard is not hygiene (LIB-REF, 2026-09-11). `mh::en_build_ok` lives in
    // mh/en_guard.cpp, which is NOT one of gen_libmh_vcxproj's MODULES -- so this call made the
    // standalone artifact reference a symbol it does not contain. It is also meaningless there:
    // the check asks whether the process is the EN mh.exe image, and a standalone host has no
    // image to be. lint_libmh_layering forbids reaching `seams/` from a module and could not see
    // this, because en_guard.h is not under seams/; the VA census and the object-byte scan cannot
    // see it either, because a declared-never-defined symbol is not a VA. Only the link does.
    if (!mh::en_build_ok()) { // the notice calls into the EN image
        say("; [desync] NOT armed: not the EN build\n");
        return 0;
    }
#endif
    if (g_cfg.every <= 0) {
        say("; [desync] NOT armed: [desync] every=%d must be >= 1\n", g_cfg.every);
        return 0;
    }

    int n_excluded = 0;
    for (int i = 0; i < N; ++i) {
        g_excluded[i] = mh::state::HASH_REGIONS[i].excluded;
        if (g_excluded[i]) ++n_excluded;
    }
    // D31 clause C: resolve the order-digest observation point by NAME, not by a hardcoded index --
    // the manifest is regenerated from the Ghidra DB and indices shift. "order_queue" is the due-now
    // queue (llm_strat_order[300], what llm_strat_order_queue_dispatch is about to apply THIS step);
    // "order_queue_count" is its 4-byte length. order_staging ("proposed this frame") and
    // order_pending/order_pending_arr ("scheduled", not yet due) are deliberately NOT folded in --
    // they describe orders that have not been applied yet.
    for (int i = 0; i < N; ++i) {
        if (lstrcmpA(mh::state::HASH_REGIONS[i].name, "order_queue") == 0) g_order_queue_idx = i;
        else if (lstrcmpA(mh::state::HASH_REGIONS[i].name, "order_queue_count") == 0)
            g_order_queue_count_idx = i;
    }
    if (g_order_queue_idx < 0)
        say("; [desync] D31: 'order_queue' region not found in this build's manifest -- the "
            "cumulative order digest stays inert (0, never appended to the wire); state-only "
            "comparison is unaffected\n");
    {
        // The fingerprint wants three parallel arrays; the manifest is an array of structs. Build the
        // views on the stack -- this runs once.
        const char *names[N];
        uint32_t    lens[N];
        for (int i = 0; i < N; ++i) {
            names[i] = mh::state::HASH_REGIONS[i].name;
            lens[i]  = mh::state::HASH_REGIONS[i].len;
        }
        g_manifest_fp = manifest_fingerprint(names, lens, g_excluded, N);
    }

    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    g_ring.clear();
    g_pending.clear();

    LARGE_INTEGER f;
    g_qpc_freq = QueryPerformanceFrequency(&f) ? f.QuadPart : 0;

    // F3F: the SAMPLING is transport-dependent; the MODULE is not. The detector still arms with no
    // network module -- it keeps its manifest, its fingerprint, its snapshot machinery and the
    // [desync] ARMED line -- because arming is what makes the instrument's state legible, and an
    // instrument that silently vanishes with a build option is the failure this file's own
    // "armed but never sampling" pair exists to name. What it does NOT do is bind a handler on a
    // transport that is not there (at F4 that is a call into a module that did not load) or hash
    // 2.79 MB for a wire that cannot carry the result -- sampling_now() asks the same question.
    if (mh::net::transport_present()) MH_Net_SetHashHandler(on_hash_frame_rx);
    g_armed = true;

    g_fp2 = v2::fp_v2(g_manifest_fp, mh::state::inc::HASH_KIND);
    say("; [desync] ARMED: every=%d steps, %d regions (%d excluded from the verdict), manifest "
        "fp=%08X%08X, %d wire bytes/sample, action=%d, snapshot=%d (max %d), per_step=%d\n",
        g_cfg.every, N, n_excluded, (unsigned)(g_manifest_fp >> 32), (unsigned)g_manifest_fp,
        wire_size(N), g_cfg.action, g_cfg.snapshot, g_cfg.snapshot_max, g_cfg.per_step);
    // mp:X3c: action=1 is the world resync (mh/desync/world_sync.cpp); any other non-zero value is unknown
    // and behaves as 0. world_sync logs its own ARMED line for action=1; action=0 says nothing.
    if (g_cfg.action != 0 && g_cfg.action != 1)
        say("; [desync] action=%d is not a known action (0 = report only, 1 = world resync) -- behaving as "
            "action=0\n",
            g_cfg.action);
    mh::desync::world_sync::configure(ini_path, g_cfg.action == 1 ? 1 : 0, g_fp2, &say);
    cost_probe();
    // mp:D40: the shared per-step tracker exists only if a consumer asked for it. Each consumer
    // enables it (the first allocates) and registers its listener; with none, nothing is allocated.
    if (g_cfg.dirty_probe && !g_dp_on) {
        dirty_probe_arm();
        if (g_dp_on) mh::desync::state_hub::add_listener(&g_dp_listener);
    }
    if (mh::desync::recorder::configure(g_cfg.state_record, g_cfg.state_keyframe_every, g_manifest_fp, &say)) {
        if (mh::desync::state_hub::enable("the state recorder (mp:D40, mh_match_state.bin)", &say))
            mh::desync::state_hub::add_listener(mh::desync::recorder::listener());
    }
    if (mh::desync::state_ring::configure(g_cfg.ring, mh::desync::recorder::enabled(), g_manifest_fp, &ring_lockstep_live,
                                          &say)) {
        if (mh::desync::state_hub::enable("the state ring (mp:D41, mh_desync_state.bin)", &say))
            mh::desync::state_hub::add_listener(mh::desync::state_ring::listener());
    }
    // mp:D44: per-step detection is the tracker's third consumer, and the one that makes the ship `net`
    // build allocate it. Only with a network module: with none there is nobody to compare against.
    if (g_cfg.per_step && !mh::net::transport_present()) {
        say("; [desync] PER-STEP not armed: no network module in this build -- nothing to compare against\n");
    } else if (g_cfg.per_step &&
               mh::desync::state_hub::enable("per-step desync detection (mp:D44, WIRE_VERSION 2)", &say)) {
        const size_t jb = (size_t)g_cfg.loc_history_kb * 1024u;
        g_jr_mem        = VirtualAlloc(nullptr, jb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_jr_mem) g_jr.attach(g_jr_mem, jb);
        mh::desync::state_hub::set_journal(&g_jtap);
        mh::desync::state_hub::add_listener(&g_v2_listener);
        g_tick.clear(N);
        g_loc.reset(g_cfg.localise_max);
        g_v2_armed = true;
        say("; [desync] PER-STEP ARMED (WIRE_VERSION 2): every sim step compared on the shared tracker's "
            "HASH_KIND %lu hashes, %d step(s) per TICK frame (%d B), live localisation for up to %d "
            "incident(s) per match from a %d KB undo journal (%lu block entries%s); a v1 peer is compared "
            "on the FNV walk every %d steps (FALLBACK)\n",
            (unsigned long)mh::state::inc::HASH_KIND, g_cfg.tick_batch, v2::tick_size(g_cfg.tick_batch),
            g_cfg.localise_max, g_cfg.loc_history_kb, (unsigned long)g_jr.cap,
            g_jr_mem ? "" : ": ALLOCATION FAILED, region level only", g_cfg.every);
    }
    if (!mh::desync::state_hub::allocated())
        say("; [desync] STATE TRACKER not allocated: state_record=%d dirty_probe=%d per_step=%d -- no shadow, no "
            "per-step state update, no mh_match_state.bin\n",
            g_cfg.state_record, g_cfg.dirty_probe, g_cfg.per_step);
    return 1;
}

// The outgoing match's rollup, ALWAYS -- including the all-clean case. A detector that only
// speaks when it found something is indistinguishable from one that never ran, which is the
// failure this ledger refuses elsewhere; "0 mismatching / 397 compared" is the evidence that a
// clean run was actually looked at. Zeroes the reported counters so the caller that follows
// (session_reset at the following session_begin_multi) cannot report the same match twice; a rollup with
// nothing in it is not written (the lobby-level session closes -- leave / host_left -- reach the
// same boundary with no match behind them).
void match_end() {
    if (!g_armed) return;
    for (int i = 0; i < VPEERS; ++i) // an incident line still waiting for its region: write it now
        if (g_pend[i].on)
            pend_resolve(g_pend[i].sender, g_pend[i].step, -1, 0, "the match ended before the region vector came");
    if (g_compared || g_mismatches || g_hash_samples || g_reused_samples || g_order_mismatches || g2_steps) {
        emit_status_line();
        say("; [desync] match end: %d mismatching / %d compared sample(s), %d dropped as too old, "
            "%d bad frame(s), %d order-digest mismatching (R2: log+rollup only, never shown), per-step: "
            "%d incident(s), %d localised finding(s), %lu B in %lu v2 frame(s) sent, %d v1 sample(s) "
            "predating the fallback, %lu steps seen\n",
            g_mismatches + g2_mismatches, g_compared + g2_compared, g_too_old + g2_too_old, g_bad_frames,
            g_order_mismatches, g2_incidents, g_loc.nresults(), (unsigned long)g2_tx_bytes,
            (unsigned long)g2_tx_frames, g_pre_fallback, (unsigned long)g_step);
    }
    dirty_probe_report();
    mh::desync::recorder::match_end();   // mp:D40: END + close; a Continue-game after this is not recorded
    mh::desync::state_ring::match_end(); // mp:D41: a tail in progress is dumped; the RAM/cost line
    g_mismatches        = 0;
    g_compared          = 0;
    g_too_old           = 0;
    g_bad_frames        = 0;
    g_hash_samples      = 0;
    g_reused_samples    = 0;
    g_hash_ticks        = 0;
    g_order_mismatches  = 0;
    g_order_absent_peer = 0;
    g2_compared = g2_mismatches = g2_too_old = g2_incidents = 0;
    g2_steps = g2_ticks = g2_max_ticks = 0;
    g2_tx_bytes = g2_tx_frames = 0;
    g_pre_fallback             = 0;
}

void stop_sampling() {
    if (!g_armed || !g_running) return;
    g_running = false;
    say("; [desync] sampling STOPPED at the harness stop (step %lu) -- the match end line above is this "
        "match's final verdict; session_begin_multi re-arms sampling\n",
        (unsigned long)g_step);
}

void session_reset() {
    if (!g_armed) return;
    match_end(); // the rollup, in case no session boundary reported it (a process with one match)
    g_step           = 0;
    g_mismatches     = 0;
    g_compared       = 0;
    g_too_old        = 0;
    g_manifest_bad   = 0;
    g_bad_frames     = 0;
    g_rx_while_inert = 0;
    g_notified       = false;
    g_fp_reported    = false;
    g_gate_said      = false;
    g_snap_armed     = false;
    g_snaps_done     = 0;
    g_hash_ticks     = 0;
    g_hash_samples   = 0;
    g_reused_samples = 0;
    // D31 clause C: the order digest is "since session start" (this module's session == one match --
    // see match_end()'s own comment), so it resets here alongside g_step, not in match_end().
    g_order_digest                 = FNV_OFFSET;
    g_order_mismatches             = 0;
    g_order_absent_peer            = 0;
    g_consecutive_state_mismatches = 0;
    g_ring.clear();
    if (g_cs_init) {
        EnterCriticalSection(&g_cs);
        g_pending.clear();
        g_v2q_head = g_v2q_count = g_v2q_dropped = 0;
        LeaveCriticalSection(&g_cs);
    }
    // mp:D44: a new match's peers announce their wire versions afresh.
    g_tick.clear(N);
    g_loc.reset(g_cfg.localise_max);
    for (int i = 0; i < VPEERS; ++i) {
        InterlockedExchange(&g_peer_v1[i], 0);
        InterlockedExchange(&g_peer_v2[i], 0);
        g_consec_steps[i] = 0;
        g_pend[i].on      = false;
        g_v2_fp_said[i]   = false;
        g_regions_seen[i] = {};
    }
    InterlockedExchange(&g_v1_seen, 0);
    InterlockedExchange(&g_inert_steps, 0);
    g_legacy_on           = false;
    g_legacy_from         = 0;
    g_batch_n             = 0;
    g_jr_last             = 0;
    g_first_mismatch_seen = false;
    g2_manifest_bad       = 0;
    dirty_probe_clear();
    dirty_probe_open_csv();
    // mp:D40: the shared tracker primes at this match's step 1, and the recorder arms a new file.
    mh::desync::state_hub::reset();
    {
        mh::desync::recorder::axis_info axis;
        axis.session_folder = MH_RunDir_SessionActive() != 0;
        axis.pre_steps      = (axis.session_folder && MH_RunDirGeneration() == g_axis_gen) ? g_axis_steps : 0;
        mh::desync::recorder::session_start(axis);
    }
    mh::desync::state_ring::session_start(); // mp:D41
    world_sync::session_reset();             // mp:X3c: any transfer in flight belongs to the match that just ended
    g_running = true;
    say("; [desync] session reset -- sampling from step 1\n");
}

namespace {

// Everything a sample does once the hashes exist. `per` is in manifest order; `state` is the
// state-only fold of it.
void sample_and_judge(const uint64_t *per, uint64_t state) {
    g_ring.put(g_step, state, per, N, g_order_digest);

    memset(&g_out, 0, sizeof(g_out));
    g_out.magic        = WIRE_MAGIC;
    g_out.version      = WIRE_VERSION;
    g_out.region_count = (uint16_t)N;
    g_out.manifest_fp  = g_manifest_fp;
    g_out.step         = g_step;
    g_out.reserved     = 0;
    g_out.state_hash   = state;
    memcpy(g_out.per, per, sizeof(uint64_t) * N);
    // D31 clause C / WIRE COMPATIBILITY (desync_watch.h, above judge_order()): the order digest is
    // NOT a field of `g_out` -- it rides ORDER_DIGEST_BYTES of trailing bytes appended after the
    // base (state-only) frame, so an rc2 peer's own wire_size(N) math never has to see it exist.
    const int base_len = wire_size(N);
    uint8_t   send_buf[sizeof(sample_wire) + ORDER_DIGEST_BYTES];
    memcpy(send_buf, &g_out, (size_t)base_len);
    memcpy(send_buf + base_len, &g_order_digest, ORDER_DIGEST_BYTES);
    MH_Net_SendHash(send_buf, base_len + ORDER_DIGEST_BYTES);

    if (!g_fp_reported) {
        g_fp_reported = true;
        say("; [desync] first sample sent: step=%lu state=%08X%08X (hash source: %s)\n",
            (unsigned long)g_step, (unsigned)(state >> 32), (unsigned)state,
            g_reuse_source ? "the harness's, reused" : "our own walk");
    }
    drain_pending();
    if (!g_v2_armed && status_line_due(g_hash_samples + g_reused_samples)) emit_status_line();
}

// Hash the whole manifest ourselves, timed. Returns the state-only fold; leaves the per-region
// hashes in g_per.
uint64_t hash_own() {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    // The SAME masks the shipped harness defaults to (mask_ctrl_group=1, mask_soldier_anim=1), so
    // this hash and the one in mh_harness.log are the same number for the same state. A run that
    // overrides either harness mask produces a harness hash that is NOT comparable with these; the
    // in-band verdict stays self-consistent either way, since both peers use these defaults.
    for (int i = 0; i < N; ++i) g_per[i] = mh::state::hash_slice(i, true, true);
    const uint64_t state = fold_state(g_per, g_excluded, N);
    QueryPerformanceCounter(&t1);
    g_hash_ticks += (t1.QuadPart - t0.QuadPart);
    ++g_hash_samples;
    return state;
}

// D31 clause C: fold THIS step's due-now order queue into the running digest. Called from
// on_sim_step()/on_sim_step_hashed() UNCONDITIONALLY, i.e. on every sim step this hook fires for
// (armed+running), NOT gated by sampling_now() -- that is the whole point: the state hash only ever
// re-derives the CURRENT state at a sampled step, so a short order-region divergence that heals
// before the next sample is invisible to it (mp:D30, mp:D31's motivating case). A cumulative fold
// carries a divergent step's contribution forward forever, so it still shows at the next sample even
// after the queue itself has drained back to agreement.
//
// COST. Reuses the SAME hash_slice() machinery the full manifest walk uses, but over ONLY the
// order_queue (20400 bytes) + order_queue_count (4 bytes) regions -- roughly 0.7% of the 2.79 MB
// manifest the COST PROBE measured at ~4 ms, i.e. low tens of microseconds. That is what makes EVERY
// STEP viable here where the config comment above rejects every=1 for the FULL manifest (~25% of a
// sim step's budget): this is two small regions, not sixty-two.
void order_digest_tick() {
    if (g_order_queue_idx < 0) return; // resolved at install(); -1 means the digest stays inert
    const uint64_t h1 = mh::state::hash_slice(g_order_queue_idx, true, true);
    const uint64_t h2 =
        (g_order_queue_count_idx >= 0) ? mh::state::hash_slice(g_order_queue_count_idx, true, true) : 0;
    const uint64_t buf[3] = {(uint64_t)g_step, h1, h2};
    g_order_digest        = fnv1a(buf, sizeof(buf), g_order_digest);
}

// The gate every sample passes: armed, on the cadence, and in a live multi-peer lockstep session.
// A solo/skirmish game has nobody to disagree with, and hashing 2.79 MB for nobody is the cost this
// gate exists to not pay.
bool session_live() {
    // F3F: the transport-present question comes FIRST, and it is not the same question as the two
    // below it. `MH_Net_IsStarted()` asks whether a transport that EXISTS has been brought up; with
    // no network module there is nothing to ask, and at F4 asking would be a call through an import
    // that never resolved. Cached and branch-predictable, so the per-step cost is nil.
    if (!mh::net::transport_present()) return false;
    if (*mh::state::ptr<const uint8_t>(mh::state::RID_GAME_SESSION_MODE) != SESSION_MP_LOCKSTEP)
        return false;
    return MH_Net_IsStarted() && MH_Net_PeerCount() > 0;
}

bool sampling_now() { return (g_step % (uint32_t)g_cfg.every) == 0 && session_live(); }

// ================================================================================================
// mp:D44 -- the WIRE_VERSION 2 binding (desync_wire2.h): per-step comparison on the shared tracker,
// live localisation, and the v1 FALLBACK
// ================================================================================================

// "record 12 +0x1B of map_object_unit": the record arrays of the manifest, by struct size. The FIELD
// name needs the parsed struct layout, which lives offline (tools/state_record.py name REGION OFFSET).
void describe_offset(int r, uint32_t off, char *out, size_t cap) {
    using namespace mh::game;
    namespace st = mh::state;
    struct layout {
        int         idx;
        uint32_t    stride;
        const char *type;
    };
    static const layout L[] = {
        {st::HIDX_BUILDINGS, sizeof(mh_map_object_building), "map_object_building"},
        {st::HIDX_PRODUCTIONS, sizeof(mh_map_object_production), "map_object_production"},
        {st::HIDX_MINES, sizeof(mh_map_object_mine), "map_object_mine"},
        {st::HIDX_TURRETS, sizeof(mh_map_object_turret), "map_object_turret"},
        {st::HIDX_UNIT_STORAGE, sizeof(mh_map_object_unit_storage), "map_object_unit_storage"},
        {st::HIDX_LABS, sizeof(mh_map_object_lab), "map_object_lab"},
        {st::HIDX_STRAT_PLAYERS, sizeof(mh_llm_strat_player_profile), "llm_strat_player_profile"},
        {st::HIDX_PLANETS, sizeof(mh_cfg_final_struct_Planet), "cfg_final_struct_Planet"},
        {st::HIDX_PROD_SLOTS, sizeof(mh_llm_prod_shuttle_slot), "llm_prod_shuttle_slot"},
        {st::HIDX_PROJECTILE_POOL, sizeof(mh_llm_strat_projectile), "llm_strat_projectile"},
        {st::HIDX_UNITS, sizeof(mh_map_object_unit), "map_object_unit"},
        {st::HIDX_TILE_OBJECTS, sizeof(mh_map_tile_object_data), "map_tile_object_data"},
        {st::HIDX_SOLDIERS, sizeof(mh_llm_strat_crew_soldier), "llm_strat_crew_soldier"},
        {st::HIDX_ORDER_QUEUE, sizeof(mh_llm_strat_order), "llm_strat_order"},
        {st::HIDX_ORDER_STAGING, sizeof(mh_llm_strat_order), "llm_strat_order"},
        {st::HIDX_ORDER_PENDING_ARR, sizeof(mh_llm_strat_order), "llm_strat_order"},
        {st::HIDX_PLAYERS, sizeof(mh_llm_strat_player_desc), "llm_strat_player_desc"},
        {st::HIDX_RNG_STATE, 4u, "rng slot (uint32)"},
    };
    if (cap == 0) return;
    out[0]                    = 0;
    const st::hash_region &hr = st::HASH_REGIONS[r];
    if (hr.rid == st::RID_PLAYER_DATA) { // the pN_* slices are windows into one record per player
        const uint32_t x = hr.offset + off;
        const uint32_t z = (uint32_t)sizeof(mh_game_player_data);
        _snprintf_s(out, cap, _TRUNCATE, "player_data[%lu] +0x%lX of game_player_data", (unsigned long)(x / z),
                    (unsigned long)(x % z));
        return;
    }
    for (const layout &l : L)
        if (l.idx == r && l.stride) {
            _snprintf_s(out, cap, _TRUNCATE, "record %lu +0x%lX of %s", (unsigned long)(off / l.stride),
                        (unsigned long)(off % l.stride), l.type);
            return;
        }
}


void tick_flush();

struct LiveLocHost final : v2::loc_host {
    int         nregions() const override { return N; }
    uint32_t    region_len(int r) const override { return mh::state::HASH_REGIONS[r].len; }
    const char *region_name(int r) const override { return mh::state::HASH_REGIONS[r].name; }
    bool        excluded(int r) const override { return g_excluded[r]; }
    uint64_t    fp() const override { return g_fp2; }
    bool        per_at(uint32_t s, uint64_t *out) override {
        const v2::ticker::mine_entry *m = g_tick.find_mine(s);
        if (!m) return false;
        memcpy(out, m->per, sizeof(uint64_t) * N);
        return true;
    }
    // The state at step s: the tracker's shadow (the state at THIS step, updated a moment ago) with the
    // journal's entries for the steps after s replayed backwards over it.
    bool materialize(uint32_t s) override {
        if (!g_jr.covers(s) || s > g_step || g_jr_last != g_step) return false;
        if (!g_ev) {
            uint32_t total = 0;
            for (int i = 0; i < N; ++i) {
                g_ev_off[i] = total;
                total += (mh::state::HASH_REGIONS[i].len + 63u) & ~63u;
            }
            g_ev = static_cast<uint8_t *>(VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!g_ev) {
                say("; [desync] LOCALISE: cannot allocate the %lu-byte evidence buffer (err %lu) -- region level "
                    "only\n",
                    (unsigned long)total, GetLastError());
                return false;
            }
        }
        const mh::state::inc::tracker &tr = mh::desync::state_hub::tracker();
        for (int i = 0; i < N; ++i) memcpy(g_ev + g_ev_off[i], tr.shadow(i), mh::state::HASH_REGIONS[i].len);
        g_jr.undo_to(s, [](int r, uint32_t b, const uint8_t *bytes, uint32_t n) {
            memcpy(g_ev + g_ev_off[r] + b * 64u, bytes, n);
        });
        int32_t c;
        memcpy(&c, g_ev + g_ev_off[mh::state::HIDX_ORDER_QUEUE_COUNT], sizeof(c));
        g_ev_oq_live = mh::state::inc::order_queue_live_bytes(c, mh::state::HASH_REGIONS[mh::state::HIDX_ORDER_QUEUE].len);
        return true;
    }
    const uint8_t *evidence(int r) override { return g_ev + g_ev_off[r]; }
    uint64_t       block_hash(int r, uint32_t b) override {
        return mh::state::inc::block_hash_masked(r, b, evidence(r), mh::state::HASH_REGIONS[r].len,
                                                       mh::desync::state_hub::tracker().mask_knobs(),
                                                 r == mh::state::HIDX_ORDER_QUEUE ? g_ev_oq_live : 0);
    }
    uint8_t keep(int r, uint32_t off) override {
        return mh::state::inc::keep_byte(r, off, mh::state::HASH_REGIONS[r].len,
                                         mh::desync::state_hub::tracker().mask_knobs(), g_ev_oq_live);
    }
    void send(const uint8_t *f, int len) override {
        // Every TICK we hold goes out FIRST: a peer must see our state for step S before our REGIONS for
        // S, or it would start localising an incident its own per-step verdict has not opened yet.
        tick_flush();
        MH_Net_SendHash(f, len);
        ++g2_tx_frames;
        g2_tx_bytes += (uint64_t)len;
    }
    void log(const char *line) override { say("%s", line); }
    void describe(int r, uint32_t off, char *out, size_t cap) override { describe_offset(r, off, out, cap); }
    void on_regions(int sender, uint32_t s, int first_region, int ndiff) override {
        const int si       = (sender >= 0 && sender < VPEERS) ? sender : 0;
        g_regions_seen[si] = {true, s, first_region, ndiff};
        pend_resolve(sender, s, first_region, ndiff, nullptr);
    }
};
LiveLocHost g_host;

int peer_slot(int sender) { return (sender >= 0 && sender < VPEERS) ? sender : 0; }

// An incident's `*** DESYNC` line -- the same shape as v1's, so every existing reader keeps working:
// step, peer, both hashes, first_region, and "(mismatch #N)" (the incident number here).
void pend_resolve(int sender, uint32_t s, int first_region, int ndiff, const char *why) {
    pend_line &p = g_pend[peer_slot(sender)];
    if (!p.on || p.step != s) return;
    p.on = false;
    if (first_region >= 0)
        say("; [desync] *** DESYNC step=%lu peer=%d mine=%08X%08X theirs=%08X%08X first_region=%d %s (mismatch #%d, "
            "per-step: %d region(s) differ at this step)\n",
            (unsigned long)s, sender, (unsigned)(p.mine >> 32), (unsigned)p.mine, (unsigned)(p.theirs >> 32),
            (unsigned)p.theirs, first_region, mh::state::HASH_REGIONS[first_region].name, p.no, ndiff);
    else
        say("; [desync] *** DESYNC step=%lu peer=%d mine=%08X%08X theirs=%08X%08X first_region=-1 (unknown: %s) "
            "(mismatch #%d, per-step)\n",
            (unsigned long)s, sender, (unsigned)(p.mine >> 32), (unsigned)p.mine, (unsigned)(p.theirs >> 32),
            (unsigned)p.theirs, why ? why : "no compared region differs", p.no);
}

void pend_tick() {
    for (int i = 0; i < VPEERS; ++i)
        if (g_pend[i].on && g_step >= g_pend[i].deadline)
            pend_resolve(g_pend[i].sender, g_pend[i].step, -1, 0, "no region vector from the peer in time");
}

uint32_t g_inc_start[VPEERS]; // the step the sender's current incident opened at

void on_tick_verdict(const v2::tick_verdict &v) {
    const int si = peer_slot(v.sender);
    world_sync::on_tick_verdict(v, g_step); // mp:X3c: the host's incident detector (inert at action=0)
    switch (v.kind) {
        case v2::tick_verdict::ok:
            ++g2_compared;
            if (g_consec_steps[si] > 0 && should_log_full(g2_incidents))
                say("; [desync] incident at step %lu vs peer=%d RECONVERGED at step %lu after %d mismatching "
                    "step(s)\n",
                    (unsigned long)g_inc_start[si], v.sender, (unsigned long)v.step, g_consec_steps[si]);
            g_consec_steps[si] = 0;
            if (g_cfg.verbose && (v.step % (uint32_t)g_cfg.every) == 0) // v1's volume, not one line a step
                say("; [desync] step=%lu peer=%d MATCH state=%08X%08X\n", (unsigned long)v.step, v.sender,
                    (unsigned)(v.mine >> 32), (unsigned)v.mine);
            break;
        case v2::tick_verdict::mismatch:
            ++g2_compared;
            ++g2_mismatches;
            g_consec_steps[si] = v.consecutive;
            if (v.incident_start) {
                ++g2_incidents;
                g_inc_start[si] = v.step;
                on_first_mismatch_detected(v.step, v.sender, -1);
                if (should_log_full(g2_incidents)) {
                    pend_line &p = g_pend[si];
                    p            = {true, v.step, g_step + PEND_WAIT_STEPS, v.sender, g2_incidents, v.mine, v.theirs};
                    say("; [desync] per-step MISMATCH at step %lu vs peer=%d, judged at local step %lu (incident #%d) "
                        "-- localising live\n",
                        (unsigned long)v.step, v.sender, (unsigned long)g_step, g2_incidents);
                }
                g_loc.on_local_mismatch(g_host, v.step);
                if (g_pend[si].on && g_regions_seen[si].on && g_regions_seen[si].step == v.step)
                    pend_resolve(v.sender, v.step, g_regions_seen[si].first, g_regions_seen[si].nd, nullptr);
                if (g_pend[si].on && g_pend[si].step == v.step && !(g_loc.active() && g_loc.step() == v.step))
                    pend_resolve(v.sender, v.step, -1, 0,
                                 g_loc.incidents() >= g_cfg.localise_max ? "not localised: localise_max reached"
                                                                         : "not localised: an earlier incident "
                                                                           "is still being localised");
            } else if (should_log_rollup(g2_mismatches)) {
                say("; [desync] *** DESYNC continues: %d mismatching step(s), latest step=%lu peer=%d (incident from "
                    "step %lu)\n",
                    g2_mismatches, (unsigned long)v.step, v.sender, (unsigned long)g_inc_start[si]);
            }
            if (v2::should_notify_steps(v.consecutive)) notify_once(v.step);
            break;
        case v2::tick_verdict::too_old:
            ++g2_too_old;
            if ((g2_too_old % 64) == 1)
                say("; [desync] per-step sample for step=%lu from peer=%d arrived after its ring entry was "
                    "evicted -- dropped (%d so far)\n",
                    (unsigned long)v.step, v.sender, g2_too_old);
            break;
        case v2::tick_verdict::order_mismatch:
            order_verdict(v.step, v.sender, order_outcome::mismatch, "judged per step, see above");
            break;
        case v2::tick_verdict::order_ok:
        default: break;
    }
}

bool v2_frame_rx(int sender, const unsigned char *buf, int len) {
    v2::hdr h;
    if (!v2::frame_ok(buf, len, N, h)) return false;
    if (sender >= 0 && sender < VPEERS) InterlockedExchange(&g_peer_v2[sender], 1);
    // The v1 INERT rule, in STEPS: v1 waited for 8 samples, i.e. 8 x `every` steps of a peer's lead,
    // and a v2 TICK carries steps, not samples -- counting frames would call a peer that merely started
    // a few batches ahead of us inert, and switch the watch off for the whole match.
    const LONG inert_at = (LONG)(8 * g_cfg.every);
    LONG       before   = inert_at;
    if (g_step == 0 && h.type == v2::T_TICK) before = InterlockedExchangeAdd(&g_inert_steps, (LONG)h.count);
    if (before < inert_at && before + (LONG)h.count >= inert_at) {
        say("; [desync] INERT on this peer: 8 samples received but zero sim steps seen -- the turn "
            "engine is not promoted here ([promote] lockstep=0), so there is no per-step sampling "
            "point. Not a desync; no verdict is produced on this side.\n");
        g_running = false;
        return true;
    }
    EnterCriticalSection(&g_cs);
    if (g_v2q_count == V2Q_CAP) {
        g_v2q_head = (g_v2q_head + 1) % V2Q_CAP;
        --g_v2q_count;
        ++g_v2q_dropped;
    }
    v2_frame &f = g_v2q[(g_v2q_head + g_v2q_count) % V2Q_CAP];
    f.sender    = sender;
    f.len       = len;
    memcpy(f.b, buf, (size_t)len);
    ++g_v2q_count;
    LeaveCriticalSection(&g_cs);
    return true;
}

void v2_drain() {
    static v2_frame f; // main thread only; ~1 KB kept off the stack
    for (int guard = V2Q_CAP + 1; guard-- > 0;) {
        EnterCriticalSection(&g_cs);
        const bool got = g_v2q_count > 0;
        if (got) {
            f          = g_v2q[g_v2q_head];
            g_v2q_head = (g_v2q_head + 1) % V2Q_CAP;
            --g_v2q_count;
        }
        LeaveCriticalSection(&g_cs);
        if (!got) break;
        v2::hdr h;
        memcpy(&h, f.b, sizeof(h));
        if (h.manifest_fp != g_fp2) {
            ++g2_manifest_bad;
            const int si = peer_slot(f.sender);
            if (!g_v2_fp_said[si]) {
                g_v2_fp_said[si] = true;
                say("; [desync] DISABLED: peer=%d speaks WIRE_VERSION 2 over a DIFFERENT manifest or hash kind "
                    "(theirs %08X%08X, ours %08X%08X). A build mismatch, NOT a desync -- its v2 frames are "
                    "ignored.\n",
                    f.sender, (unsigned)(h.manifest_fp >> 32), (unsigned)h.manifest_fp, (unsigned)(g_fp2 >> 32),
                    (unsigned)g_fp2);
            }
            continue;
        }
        // mp:X3c: world-resync control frames (types 6-9) are not localiser traffic: the world_sync machine
        // takes them (and drops them at once when [desync] action is not 1).
        if (h.type >= v2::T_WS_BEGIN) {
            world_sync::on_frame(f.sender, f.b, f.len, g_step);
            continue;
        }
        const uint8_t *body = f.b + v2::HDR_BYTES;
        if (h.type == v2::T_TICK) {
            uint64_t st[v2::TICK_MAX_STEPS];
            uint64_t od;
            memcpy(st, body, sizeof(uint64_t) * h.count);
            memcpy(&od, body + 8 * h.count, sizeof(od));
            g_tick.put_theirs(f.sender, h.step, (int)h.count, st, od, on_tick_verdict);
            continue;
        }
        // An incident frame for a step we have not reached cannot be answered yet (a third peer ahead of
        // us): put it back and look again next step. Everything behind it waits one step.
        if (h.step > g_tick.newest) {
            EnterCriticalSection(&g_cs);
            if (g_v2q_count < V2Q_CAP) {
                g_v2q_head        = (g_v2q_head + V2Q_CAP - 1) % V2Q_CAP;
                g_v2q[g_v2q_head] = f;
                ++g_v2q_count;
            }
            LeaveCriticalSection(&g_cs);
            break;
        }
        g_loc.on_frame(g_host, f.sender, h, body);
    }
}

void tick_flush() {
    if (g_batch_n == 0) return;
    uint8_t buf[v2::HDR_BYTES + 8 * v2::TICK_MAX_STEPS + 8];
    v2::put_hdr(buf, v2::T_TICK, (uint8_t)g_batch_n, g_fp2, g_batch_first, 0, 0);
    memcpy(buf + v2::HDR_BYTES, g_batch, sizeof(uint64_t) * (size_t)g_batch_n);
    memcpy(buf + v2::HDR_BYTES + 8 * g_batch_n, &g_order_digest, sizeof(g_order_digest));
    const int len = v2::tick_size(g_batch_n);
    MH_Net_SendHash(buf, len);
    ++g2_tx_frames;
    g2_tx_bytes += (uint64_t)len;
    if (!g_fp_reported) {
        g_fp_reported = true;
        say("; [desync] first sample sent: step=%lu state=%08X%08X (hash source: the shared incremental "
            "tracker, HASH_KIND %lu, every step)\n",
            (unsigned long)g_batch_first, (unsigned)(g_batch[0] >> 32), (unsigned)g_batch[0],
            (unsigned long)mh::state::inc::HASH_KIND);
    }
    g_batch_n = 0;
}

void tick_add(uint64_t state) {
    if (g_batch_n > 0 && g_batch_first + (uint32_t)g_batch_n != g_step) tick_flush(); // a gap: new batch
    if (g_batch_n == 0) g_batch_first = g_step;
    g_batch[g_batch_n++] = state;
    if (g_batch_n >= g_cfg.tick_batch) tick_flush();
}

// Every known peer speaks v1 only: nothing would read a TICK, so the tracker is not needed for us.
bool v2_wanted() {
    if (!g_v2_armed) return false;
    int v1 = 0, v2n = 0;
    for (int i = 0; i < VPEERS; ++i) {
        if (g_peer_v2[i]) ++v2n;
        else if (g_peer_v1[i]) ++v1;
    }
    return !(v2n == 0 && v1 > 0 && v1 >= MH_Net_PeerCount());
}

// One step of the v2 path, right after the shared tracker was updated for this step.
void v2_step() {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint64_t per[N];
    mh::desync::state_hub::tracker().region_hashes(per);
    const uint64_t st = fold_state(per, g_excluded, N);
    tick_add(st); // before judging: an incident frame this step triggers must follow our TICK for it
    g_tick.put_mine(g_step, st, g_order_digest, per, on_tick_verdict);
    v2_drain();
    pend_tick();
    QueryPerformanceCounter(&t1);
    const int64_t dt = (t1.QuadPart - t0.QuadPart) + mh::desync::state_hub::last_ticks();
    g2_ticks += dt;
    if (dt > g2_max_ticks) g2_max_ticks = dt;
    ++g2_steps;
}

// A v1 peer was heard: from now on this match also runs the FNV walk at the v1 cadence, for it.
void fallback_check() {
    if (!g_v2_armed || g_legacy_on || InterlockedCompareExchange(&g_v1_seen, 0, 0) == 0) return;
    g_legacy_on   = true;
    g_legacy_from = g_step;
    char peers[64];
    int  at  = 0;
    peers[0] = 0;
    for (int i = 0; i < VPEERS && at < (int)sizeof(peers) - 4; ++i)
        if (g_peer_v1[i]) at += _snprintf_s(peers + at, sizeof(peers) - (size_t)at, _TRUNCATE, " %d", i);
    say("; [desync] FALLBACK: peer(s)%s speak desync WIRE_VERSION 1 (an older build) -- comparing with them on "
        "the v1 FNV walk every %d steps from step %lu (the COST PROBE above is what each walk costs); per-step "
        "(v2) comparison continues with v2 peers\n",
        peers, g_cfg.every, (unsigned long)g_step);
}

} // namespace

// mp:RM1 -- WHY an armed detector is not sampling, said ONCE per match at the first cadence step.
// "Armed and sampling nothing" is the shape this repo keeps mistaking for a clean verdict (G68/U30,
// and the install_desync_watch branches name it explicitly), and until this line existed the only
// evidence was an ABSENT `first sample sent` -- which is nothing, and nothing is what every log of a
// run that never reached the sim also shows. Each gate of sampling_now() is printed by name.
void say_gate_once() {
    if (g_gate_said) return;
    g_gate_said = true;
    say("; [desync] step %lu reached the sampling cadence but NO sample was taken: transport_present=%d "
        "session_mode=%d (want %d) net_started=%d peers=%d -- the detector is armed and sampling "
        "nothing until these hold\n",
        (unsigned long)g_step, mh::net::transport_present() ? 1 : 0,
        (int)*mh::state::ptr<const uint8_t>(mh::state::RID_GAME_SESSION_MODE), (int)SESSION_MP_LOCKSTEP,
        MH_Net_IsStarted() ? 1 : 0, MH_Net_PeerCount());
}

namespace {

// The per-step body both hooks share. `offered` is the caller's already-computed v1 hash set (the
// harness, kind 1) or null.
void step_common(const uint64_t *offered, int n, uint64_t offered_state, bool hashed_caller) {
    if (g_armed && mh::desync::recorder::enabled()) axis_tick();
    if (!g_armed || !g_running) return;
    // mp:X3c: the world resync's per-step hook -- BEFORE the step counter advances and before this step's
    // digest fold, so a host capture and a minority import both sit at the pre-body point of step g_step+1.
    // An import rewinds g_step / the digest (rewind_for_world_sync), so the ++ below then yields S.
    if (world_sync::enabled() && world_sync::step_begin(g_step + 1, g_order_digest)) {
        offered       = nullptr; // the caller's hashes describe the world we just replaced
        n             = 0;
        hashed_caller = false;
    }
    ++g_step;
    order_digest_tick(); // D31 clause C: every step, unconditionally -- see the function's own comment
    snapshot_tick();
    const bool live = session_live();
    fallback_check();
    // mp:D44: ask the shared tracker for THIS step (and journal it) only when a v2 comparison will read it.
    g_v2_step      = live && v2_wanted();
    const bool upd = mh::desync::state_hub::step(g_step); // mp:D40: probe + recorder + mp:D44; read-only
    if (g_v2_step && upd) v2_step();
    g_v2_step = false;
    if (g_v2_armed && live && (g_step % (uint32_t)(SAMPLES_PER_STATUS_LINE * g_cfg.every)) == 0)
        emit_status_line();
    const bool cadence = (g_step % (uint32_t)g_cfg.every) == 0;
    if (!live) {
        if (cadence && !hashed_caller) say_gate_once();
        return;
    }
    if (!cadence) return;
    if (g_v2_armed && !g_legacy_on) return; // v2 compares every step without the walk
    if (!hashed_caller) {
        sample_and_judge(g_per, hash_own());
        return;
    }
    // A caller offering a DIFFERENT manifest than ours cannot be reused -- its region order is not
    // one we can interpret. Fall back to our own walk rather than sampling something whose columns
    // we would be guessing at, and say so once.
    if (!offered || n != N) {
        if (!g_reuse_mismatch_said) {
            g_reuse_mismatch_said = true;
            say("; [desync] the caller offered %d region hashes but the manifest has %d -- hashing "
                "independently instead\n",
                n, N);
        }
        sample_and_judge(g_per, hash_own());
        return;
    }
    // Cost here is honestly ZERO added: the caller had already walked these bytes for its own log.
    // The timing counters stay untouched and emit_status_line says which source it is reporting, so
    // nobody reads a reused sample as evidence about what the hash costs.
    g_reuse_source = true;
    ++g_reused_samples;
    sample_and_judge(offered, offered_state);
}

} // namespace

void rewind_for_world_sync(uint32_t capture_step, uint64_t digest_prev) {
    const uint32_t was = g_step;
    g_step             = capture_step > 0 ? capture_step - 1 : 0;
    g_order_digest     = digest_prev;
    // The tracker re-primes at the next step (its shadow describes the world we replaced); the journal
    // clears on the primed step_begin.
    mh::desync::state_hub::reset();
    // Our own hashes at steps >= S describe the diverged world; the catch-up re-produces them. Host ticks
    // for steps we have not (re)reached are held in the peer rings and judged as we get there.
    for (int i = 0; i < v2::TICK_RING; ++i)
        if (g_tick.mine[i].step >= capture_step) g_tick.mine[i].step = 0;
    g_tick.newest = g_step;
    g_batch_n     = 0; // an unsent batch carries hashes of the replaced world
    g_loc.reset(g_cfg.localise_max);
    for (int i = 0; i < VPEERS; ++i) g_pend[i].on = false;
    mh::desync::state_ring::session_start();
    if (mh::desync::recorder::enabled()) {
        say("; [worldsync] the D40 state recorder cannot represent a discontinuity (step %lu -> %lu): stopping it "
            "here; the file up to this line is valid\n",
            (unsigned long)was, (unsigned long)g_step);
        mh::desync::recorder::match_end();
    }
    say("; [worldsync] desync counters rewound: step %lu -> %lu, order digest -> %08X%08X\n", (unsigned long)was,
        (unsigned long)g_step, (unsigned)(digest_prev >> 32), (unsigned)digest_prev);
}

int     session_mode_now() { return (int)*mh::state::ptr<const uint8_t>(mh::state::RID_GAME_SESSION_MODE); }
bool    is_host_now() { return *mh::state::ptr<const uint32_t>(mh::state::RID_NET_IS_HOST) != 0; }
uint8_t player_flags_now(int slot) {
    constexpr uint32_t PLAYER_STRIDE = 0x740; // _G_LLM_STRAT_PLAYERS element size
    return *(mh::state::ptr<const uint8_t>(mh::state::RID_STRAT_PLAYERS) + (uint32_t)slot * PLAYER_STRIDE);
}
uint64_t game_clock_bits_now() { return *mh::state::ptr<const uint64_t>(mh::state::RID_STRAT_GAME_CLOCK); }
uint64_t sim_step_interval_bits_now() { return *mh::state::ptr<const uint64_t>(mh::state::RID_STRAT_SIM_STEP_INTERVAL); }

void on_sim_step() { step_common(nullptr, 0, 0, false); }

void on_sim_step_hashed(const uint64_t *per, int n, uint64_t state) { step_common(per, n, state, true); }

bool detected() { return g_mismatches > 0 || g2_mismatches > 0; }

} // namespace mh::desync
