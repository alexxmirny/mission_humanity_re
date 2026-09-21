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
#include "state/region_view.h"
#include "state/host_api.h"
#include "state/host_events.h"

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
// One STATUS line per this many samples. At the shipped cadence (every=50) and ~50 sim steps/s that
// is a line roughly every 50 s -- often enough that a clean run is visibly clean and that a run
// killed by the rig still has recent evidence in it, rare enough that a 20-minute match adds ~24
// lines to mh_net.log.
constexpr int64_t SAMPLES_PER_STATUS_LINE = 50;

// Scratch, reused: 96 uint64 + a wire record are ~1.6 KB together and a sim step is not the place to
// touch the heap.
uint64_t    g_per[MAX_REGIONS];
sample_wire g_out;

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
    wsprintfW(static_cast<wchar_t *>(text_scratch),
              L"DESYNC DETECTED (step %lu) - this match is out of sync", (unsigned long)step);
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

// ---- the recv-thread handler --------------------------------------------------------------------
void on_hash_frame_rx(int sender, const unsigned char *buf, int len) {
    if (!g_running || !g_cs_init) return;
    // Materialise into a zeroed record FIRST: frame_is_sane reads header fields, and a short frame
    // must not be read through as a sample_wire. Anything that does not fit is a bad frame.
    if (len < wire_size(0) || len > (int)sizeof(sample_wire)) {
        InterlockedIncrement((volatile LONG *)&g_bad_frames);
        return;
    }
    sample_wire s;
    memset(&s, 0, sizeof(s));
    memcpy(&s, buf, (size_t)len);
    if (!frame_is_sane(s, len)) {
        InterlockedIncrement((volatile LONG *)&g_bad_frames);
        return;
    }
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
    g_pending.push(sender, s);
    LeaveCriticalSection(&g_cs);
}

// ---- report one judged sample (main thread) ----------------------------------------------------
void report(int sender, const verdict &v) {
    switch (v.kind) {
        case outcome::ok:
            ++g_compared;
            if (g_cfg.verbose)
                say("; [desync] step=%lu peer=%d MATCH state=%08X%08X\n", (unsigned long)v.step, sender,
                    (unsigned)(v.mine >> 32), (unsigned)v.mine);
            break;

        case outcome::mismatch: {
            ++g_compared;
            ++g_mismatches;
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
            if (should_notify(g_mismatches)) notify_once(v.step);
            // D25: arm the full-state dump. Arming is a latch, not a schedule -- the dump steps come
            // from the absolute grid so that peers which noticed at different moments still write
            // files for the SAME step, which is the only thing that makes them diffable.
            if (g_cfg.snapshot && !g_snap_armed) {
                g_snap_armed = true;
                say("; [desync] snapshot ARMED by the mismatch at step %lu (first_region=%d %s): up "
                    "to %d full-state dumps, on every step divisible by %lu\n",
                    (unsigned long)v.step, v.first_region, rname, g_cfg.snapshot_max,
                    (unsigned long)snapshot_grid(g_cfg.every));
            }
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
        default: break;
    }
}

// Judge everything queued that we can judge; put back what is still in the future.
void drain_pending() {
    // Bounded by PENDING_CAP: pop everything, judge, and re-push only the future ones. Doing it in
    // one pass under the lock would hold the recv thread off for the whole judgement.
    sample_wire s;
    int         from  = 0;
    int         guard = PENDING_CAP + 1;
    while (guard-- > 0) {
        EnterCriticalSection(&g_cs);
        const bool got = g_pending.pop(from, s);
        LeaveCriticalSection(&g_cs);
        if (!got) break;
        const verdict v = judge(g_ring, s, g_excluded, N, g_manifest_fp, g_ring.newest);
        if (v.kind == outcome::not_yet) {
            EnterCriticalSection(&g_cs);
            g_pending.push(from, s); // still ahead of us; look again next sample
            LeaveCriticalSection(&g_cs);
            break; // the queue is in arrival order, so everything behind it is at least as new
        }
        report(from, v);
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
    char cost[192];
    if (g_hash_samples && g_qpc_freq) {
        const double us          = (double)g_hash_ticks * 1000000.0 / (double)g_qpc_freq / (double)g_hash_samples;
        const double per_step_us = us / (double)(g_cfg.every > 0 ? g_cfg.every : 1);
        wsprintfA(cost, "own-hash mean %d.%02d us -> %d.%03d us amortised per SIM STEP", (int)us,
                  (int)((us - (int)us) * 100.0), (int)per_step_us,
                  (int)((per_step_us - (int)per_step_us) * 1000.0));
    } else if (g_reused_samples) {
        wsprintfA(cost, "0 us added -- every sample reused a hash this run was computing anyway");
    } else {
        wsprintfA(cost, "no hash timed yet");
    }
    say("; [desync] STATUS: samples=%d (own %d / reused %d) compared=%d mismatching=%d too_old=%d "
        "bad_frames=%d step=%lu | COST %s (every=%d, %d regions, %d wire bytes/sample)\n",
        (int)(g_hash_samples + g_reused_samples), (int)g_hash_samples, (int)g_reused_samples,
        g_compared, g_mismatches, g_too_old, g_bad_frames, (unsigned long)g_step, cost, g_cfg.every,
        N, wire_size(N));
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

    if (!g_cfg.enabled) {
        say("; [desync] NOT armed: [desync] enabled=0\n");
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

    say("; [desync] ARMED: every=%d steps, %d regions (%d excluded from the verdict), manifest "
        "fp=%08X%08X, %d wire bytes/sample, action=%d, snapshot=%d (max %d)\n",
        g_cfg.every, N, n_excluded, (unsigned)(g_manifest_fp >> 32), (unsigned)g_manifest_fp,
        wire_size(N), g_cfg.action, g_cfg.snapshot, g_cfg.snapshot_max);
    if (g_cfg.action != 0)
        say("; [desync] action=%d requested but NOT implemented -- D21 delivers detect+report only; "
            "anything that changes what the game does needs the reimpl_fixes discipline. Behaving as "
            "action=0.\n",
            g_cfg.action);
    cost_probe();
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
    if (g_compared || g_mismatches || g_hash_samples || g_reused_samples) {
        emit_status_line();
        say("; [desync] match end: %d mismatching / %d compared sample(s), %d dropped as too old, "
            "%d bad frame(s), %lu steps seen\n",
            g_mismatches, g_compared, g_too_old, g_bad_frames, (unsigned long)g_step);
    }
    g_mismatches     = 0;
    g_compared       = 0;
    g_too_old        = 0;
    g_bad_frames     = 0;
    g_hash_samples   = 0;
    g_reused_samples = 0;
    g_hash_ticks     = 0;
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
    g_ring.clear();
    if (g_cs_init) {
        EnterCriticalSection(&g_cs);
        g_pending.clear();
        LeaveCriticalSection(&g_cs);
    }
    g_running = true;
    say("; [desync] session reset -- sampling from step 1\n");
}

namespace {

// Everything a sample does once the hashes exist. `per` is in manifest order; `state` is the
// state-only fold of it.
void sample_and_judge(const uint64_t *per, uint64_t state) {
    g_ring.put(g_step, state, per, N);

    memset(&g_out, 0, sizeof(g_out));
    g_out.magic        = WIRE_MAGIC;
    g_out.version      = WIRE_VERSION;
    g_out.region_count = (uint16_t)N;
    g_out.manifest_fp  = g_manifest_fp;
    g_out.step         = g_step;
    g_out.reserved     = 0;
    g_out.state_hash   = state;
    memcpy(g_out.per, per, sizeof(uint64_t) * N);
    MH_Net_SendHash(reinterpret_cast<const unsigned char *>(&g_out), wire_size(N));

    if (!g_fp_reported) {
        g_fp_reported = true;
        say("; [desync] first sample sent: step=%lu state=%08X%08X (hash source: %s)\n",
            (unsigned long)g_step, (unsigned)(state >> 32), (unsigned)state,
            g_reuse_source ? "the harness's, reused" : "our own walk");
    }
    drain_pending();
    if (((g_hash_samples + g_reused_samples) % SAMPLES_PER_STATUS_LINE) == 0) emit_status_line();
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

// The gate every sample passes: armed, on the cadence, and in a live multi-peer lockstep session.
// A solo/skirmish game has nobody to disagree with, and hashing 2.79 MB for nobody is the cost this
// gate exists to not pay.
bool sampling_now() {
    // F3F: the transport-present question comes FIRST, and it is not the same question as the two
    // below it. `MH_Net_IsStarted()` asks whether a transport that EXISTS has been brought up; with
    // no network module there is nothing to ask, and at F4 asking would be a call through an import
    // that never resolved. Cached and branch-predictable, so the per-step cost is nil.
    if (!mh::net::transport_present()) return false;
    if ((g_step % (uint32_t)g_cfg.every) != 0) return false;
    if (*mh::state::ptr<const uint8_t>(mh::state::RID_GAME_SESSION_MODE) != SESSION_MP_LOCKSTEP)
        return false;
    return MH_Net_IsStarted() && MH_Net_PeerCount() > 0;
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

void on_sim_step() {
    if (!g_armed || !g_running) return;
    ++g_step;
    snapshot_tick();
    if (!sampling_now()) {
        if ((g_step % (uint32_t)g_cfg.every) == 0) say_gate_once();
        return;
    }
    sample_and_judge(g_per, hash_own());
}

void on_sim_step_hashed(const uint64_t *per, int n, uint64_t state) {
    if (!g_armed || !g_running) return;
    ++g_step;
    snapshot_tick();
    if (!sampling_now()) return;
    // A caller offering a DIFFERENT manifest than ours cannot be reused -- its region order is not
    // one we can interpret. Fall back to our own walk rather than sampling something whose columns
    // we would be guessing at, and say so once.
    if (!per || n != N) {
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
    sample_and_judge(per, state);
}

bool detected() { return g_mismatches > 0; }

} // namespace mh::desync
