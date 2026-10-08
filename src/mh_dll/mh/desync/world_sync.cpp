//
// desync/world_sync.cpp -- mp:X3c: the product wiring of the host-authoritative world resync.
// Read world_sync.h first; every DECISION lives in world_sync_core.h (proven by `net_selftest.exe wstest`).
//
// THE SHAPE, in one paragraph. The host notices a peer that keeps mismatching (on_tick_verdict), asks the
// pure core whether it may act (host authority, confirm window, roster quiet, mode 3, ...), tells the peer
// (BEGIN), captures its own world at the top of its NEXT step S, ships the blob over channel C and the
// numbers the peer needs over the desync control channel (META). The diverged peer keeps playing live. When
// the blob lands it imports it (libmh_import_world_resync: keep-local regions survive, its own PENDING
// admission log is re-applied), rewinds the desync counters to S, and net_lockstep runs the catch-up with
// the horizon mirror and a per-frame step cap. DONE goes back when it is live again.
//
// THE BLOB "ROOT" IN META is an FNV-1a-64 of the whole blob, computed here on both sides. Channel C already
// verifies the body against its own merkle root, but the sender's module does not expose an outbound root
// (MH_NetSnapshotStatus.root_hex is the INBOUND manifest's), and a checksum both ends compute over the bytes
// they actually hold is the stronger end-to-end pairing of META with blob anyway.
//
#include "desync/world_sync.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../libmh/include/libmh.h" // libmh_import_world_resync + libmh_resync_report
#include "desync/desync_watch.h"
#include "include/mh_net_export.h"
#include "include/mh_net_module.h" // MH_Net_Snapshot{Send,Poll,Cancel} + MH_SNAP_*
#include "orders/admission_log.h"
#include "orders/order_queue.h" // promotion_active
#include "state/world_snapshot.h"

namespace mh::desync::world_sync {
namespace {

namespace w = mh::state::world;
using namespace mh::desync::v2;

void (*g_log)(const char *fmt, ...) = nullptr;
#define WSAY(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

ws::config    g_cfg;
hooks         g_h     = {};
uint64_t      g_fp2   = 0;
bool          g_on    = false;
tallies       g_t     = {};
constexpr int PLAYERS = 8;

// local rcs a DONE can carry besides the libmh ones (-30 ring short, -31 behind host, -32 log latched abort)
constexpr int RC_BAD_STEP = -34; // blob header step != META step
constexpr int RC_BAD_LEN  = -35; // blob length != META length (or shorter than a header)
constexpr int RC_BAD_ROOT = -36; // blob checksum != META checksum
constexpr int RC_TIMEOUT  = -37; // the peer gave up waiting for the blob (its own deadline)

// ---- small live readers ----------------------------------------------------------------------------
int session_mode() { return mh::desync::session_mode_now(); }

bool is_host() { return mh::desync::is_host_now(); }

uint8_t player_flags(int i) {
    return mh::desync::player_flags_now(i);
}

// The roster word the D26 exclusion watches. WIDER than ws::roster_word (the ALIVE bit only): a 3-peer
// clean quit runs mark_player_gone (clears HUMAN, sets GONE + DEFEATED) and leaves ALIVE alone until
// presence_lost, so the four bits ALIVE|HUMAN|GONE|DEFEATED are all folded in, 4 bits per slot. R2.
uint32_t wide_roster() {
    uint8_t f[PLAYERS];
    for (int i = 0; i < PLAYERS; ++i) f[i] = player_flags(i);
    return ws::roster_word_wide(f);
}

bool peer_active(int p) {
    int       ids[16];
    const int n = MH_Net_ActivePeerIds(ids, 16);
    for (int i = 0; i < n; ++i)
        if (ids[i] == p) return true;
    return false;
}

void send_frame(const uint8_t *b, int n) { MH_Net_SendHash(b, n); }

// ---- hash history: every judged comparison, per sender, so the host can group N peers at one step -----
ws::hash_hist g_hist; // [0..7] the senders' hashes, [8] our own
void          hist_put(int who, uint32_t s, uint64_t v) { g_hist.put(who, s, v); }
bool          hist_get(int who, uint32_t s, uint64_t *v) { return g_hist.get(who, s, v); }

// ---- HOST side ---------------------------------------------------------------------------------------
struct hostpeer {
    ws::fsm  f;
    DWORD    begin_ms;
    uint32_t next_attempt_step;
    bool     capture_due;
    int      last_skip;
    uint32_t last_skip_step;
};
hostpeer           g_hp[PLAYERS];
ws::roster_tracker g_roster;

void host_abort_actions(int p, const ws::action &a, uint32_t step) {
    hostpeer &hp = g_hp[p];
    switch (a.kind) {
        case ws::A_SEND_ABORT: {
            uint8_t        b[MAX_FRAME];
            const uint32_t s = hp.f.capture_step ? hp.f.capture_step : hp.f.begin_step;
            send_frame(b, ws_put_abort(b, g_fp2, s, p, a.reason));
            MH_Net_SnapshotCancel(p);
            hp.capture_due = false;
            ++g_t.aborts;
            WSAY("; [worldsync] ABORT peer=%d reason=%u step=%lu (begin=%lu S=%lu)\n", p, (unsigned)a.reason,
                 (unsigned long)step, (unsigned long)hp.f.begin_step, (unsigned long)hp.f.capture_step);
            break;
        }
        case ws::A_RETRY_CAPTURE: hp.next_attempt_step = step + (uint32_t)g_cfg.capture_retry_every; break;
        case ws::A_COOLDOWN_END:
            WSAY("; [worldsync] COOLDOWN over peer=%d step=%lu (resyncs so far %d/%d)\n", p, (unsigned long)step,
                 hp.f.resyncs, g_cfg.resync_max);
            break;
        default: break;
    }
}

void host_do_capture(int p, uint32_t S, uint64_t digest_prev) {
    hostpeer &hp   = g_hp[p];
    hp.capture_due = false;

    uint32_t n_src[LIBMH_RESYNC_SOURCES];
    mh::orders::admission::counts(n_src);

    const size_t need = w::capture_capacity();
    uint8_t     *blob = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, need));
    if (!blob) {
        WSAY("; [worldsync] CAPTURE peer=%d S=%lu FAILED: HeapAlloc(%lu)\n", p, (unsigned long)S, (unsigned long)need);
        ws::event e{ws::E_CAPTURE_FAIL};
        host_abort_actions(p, ws::fsm_step(g_cfg, hp.f, e), S);
        return;
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    size_t got = 0;
    int    rc  = 0;
    if (g_h.horizon_hold) g_h.horizon_hold(1);
    {
        const uint32_t mf   = w::MASK_CTRL_GROUP | w::MASK_SOLDIER_ANIM | w::MASK_PLANETS_GFX;
        uint64_t       comb = 0, state = 0;
        w::lockstep_hash(mf, &comb, &state);
        w::capture_params cp;
        cp.lockstep_combined = comb;
        cp.lockstep_state    = state;
        cp.game_clock        = mh::desync::game_clock_bits_now();
        cp.step              = S;
        cp.mask_flags        = mf;
        rc                   = w::capture(blob, need, &got, cp);
        if (rc == w::WORLD_OK && g_h.harness) g_h.harness(WS_CAPTURE, S, 0, blob, (uint32_t)got);
    }
    if (g_h.horizon_hold) g_h.horizon_hold(0);
    QueryPerformanceCounter(&t1);
    const int cap_ms = f.QuadPart ? (int)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart) : 0;

    if (rc != w::WORLD_OK) {
        // -8 (hashed slice missing) is known early in a match; the machine retries every 25 steps.
        WSAY("; [worldsync] CAPTURE peer=%d S=%lu FAILED rc=%d attempt=%d/%d (%d ms)\n", p, (unsigned long)S, rc,
             hp.f.attempts + 1, g_cfg.capture_retry_max, cap_ms);
        HeapFree(GetProcessHeap(), 0, blob);
        ws::event e{ws::E_CAPTURE_FAIL};
        host_abort_actions(p, ws::fsm_step(g_cfg, hp.f, e), S);
        return;
    }
    const uint64_t sum  = fnv1a(blob, got);
    const int      sent = MH_Net_SnapshotSend(p, blob, (int)got); // COPIES (mh_net_module.h)
    HeapFree(GetProcessHeap(), 0, blob);
    if (!sent) {
        MH_NetSnapshotStatus st;
        MH_Net_SnapshotStatus(&st);
        WSAY("; [worldsync] SEND peer=%d S=%lu NOT ARMED (supported=%d state=%d err=%d) attempt=%d/%d\n", p,
             (unsigned long)S, st.supported, st.state, st.last_err, hp.f.attempts + 1, g_cfg.capture_retry_max);
        ws::event e{ws::E_CAPTURE_FAIL};
        host_abort_actions(p, ws::fsm_step(g_cfg, hp.f, e), S);
        return;
    }
    ws_meta_body mb;
    memset(&mb, 0, sizeof(mb));
    mb.digest_prev = digest_prev;
    memcpy(mb.n_src, n_src, sizeof(mb.n_src));
    mb.blob_len = (uint32_t)got;
    memcpy(mb.root, &sum, WS_ROOT_BYTES);
    uint8_t b[MAX_FRAME];
    send_frame(b, ws_put_meta(b, g_fp2, S, p, mb));
    ws::event e{ws::E_CAPTURE_OK};
    e.a = S;
    ws::fsm_step(g_cfg, hp.f, e);
    ++g_t.captures;
    WSAY("; [worldsync] CAPTURE peer=%d S=%lu bytes=%lu capture_ms=%d sum=%08X%08X digest_prev=%08X%08X "
         "n_src=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
         p, (unsigned long)S, (unsigned long)got, cap_ms, (unsigned)(sum >> 32), (unsigned)sum,
         (unsigned)(digest_prev >> 32), (unsigned)digest_prev, (unsigned long)n_src[0], (unsigned long)n_src[1],
         (unsigned long)n_src[2], (unsigned long)n_src[3], (unsigned long)n_src[4], (unsigned long)n_src[5],
         (unsigned long)n_src[6], (unsigned long)n_src[7], (unsigned long)n_src[8]);
}

void host_step(uint32_t next_step, uint64_t digest_now) {
    const uint32_t roster = wide_roster();
    g_roster.observe(next_step, roster);
    const int mode = session_mode();
    for (int p = 0; p < PLAYERS; ++p) {
        hostpeer &hp = g_hp[p];
        if (hp.f.state == ws::P_IDLE) continue;
        if (hp.f.state == ws::P_BEGUN || hp.f.state == ws::P_SENT) {
            // R5: a PENDING overflow reset destroyed admitted records; the counters no longer mean anything.
            uint32_t                           n[LIBMH_RESYNC_SOURCES];
            mh::orders::admission::plan_report pr;
            mh::orders::admission::counts(n);
            if (mh::orders::admission::plan(n, &pr) == mh::orders::admission::ERR_ABORT) {
                ws::event e{ws::E_PENDING_OVERFLOW};
                host_abort_actions(p, ws::fsm_step(g_cfg, hp.f, e), next_step);
                continue;
            }
        }
        ws::event e{ws::E_STEP};
        e.elapsed_ms   = (int)(GetTickCount() - hp.begin_ms);
        e.roster       = roster;
        e.session_mode = mode;
        e.peer_active  = peer_active(p) && (player_flags(p) & 0x06) == 0x06;
        host_abort_actions(p, ws::fsm_step(g_cfg, hp.f, e), next_step);
        if (hp.f.state == ws::P_BEGUN && hp.capture_due && next_step >= hp.next_attempt_step)
            host_do_capture(p, next_step, digest_now);
    }
}

void host_on_done(int sender, uint32_t s, const ws_done_body &b, uint32_t host_step) {
    if (sender < 0 || sender >= PLAYERS || !is_host()) return;
    hostpeer &hp = g_hp[sender];
    if (hp.f.state != ws::P_SENT) {
        WSAY("; [worldsync] DONE peer=%d S=%lu ignored (no transfer in flight, state=%d)\n", sender,
             (unsigned long)s, (int)hp.f.state);
        return;
    }
    ws::event e{ws::E_DONE};
    e.a                = b.live_step;
    e.rc               = b.rc;
    const ws::action a = ws::fsm_step(g_cfg, hp.f, e);
    if (a.kind == ws::A_LOG_DONE) {
        ++g_t.dones;
        MH_Net_SnapshotCancel(sender); // frees the module's copy if the peer answered before the send ended
        WSAY("; [worldsync] DONE peer=%d rc=%d S=%lu live_step=%lu ff_ms=%lu incident=%lu host_step=%lu "
             "(incident to live: %ld steps)\n",
             sender, (int)b.rc, (unsigned long)s, (unsigned long)b.live_step, (unsigned long)b.ff_ms,
             (unsigned long)hp.f.begin_step, (unsigned long)host_step,
             (long)((int64_t)b.live_step - (int64_t)hp.f.begin_step));
    }
}

// ---- MINORITY side ---------------------------------------------------------------------------------------
enum mstate : int { M_IDLE = 0,
                    M_BEGUN,
                    M_CATCHUP };
struct minority {
    mstate       st;
    int          host_id;
    uint32_t     incident;
    DWORD        begin_ms;
    bool         meta;
    uint32_t     meta_step;
    ws_meta_body mb;
    bool         abort_rx;
    uint32_t     abort_reason;
    uint8_t     *buf;
    size_t       cap;
    bool         blob;
    uint32_t     blob_len;
    int          refused_said;
    uint32_t     S, backlog;
    DWORD        import_ms;
    uint32_t     poll_steps;
    uint32_t     pend_peak, pend_peak_step, feed_calls; // ORDER_PENDING high-water mark during the staged catch-up
};
minority g_m;

void m_free() {
    if (g_m.buf) HeapFree(GetProcessHeap(), 0, g_m.buf);
    g_m.buf = nullptr;
    g_m.cap = 0;
}

void m_end() {
    mh::orders::admission::stage_cancel();
    m_free();
    memset(&g_m, 0, sizeof(g_m));
}

void m_send_done(uint32_t s, uint32_t live_step, int rc, uint32_t ff_ms) {
    uint8_t      b[MAX_FRAME];
    ws_done_body db = {live_step, rc, ff_ms};
    send_frame(b, ws_put_done(b, g_fp2, s, g_m.host_id, db));
}

// Feed the staged re-admission log to ORDER_PENDING: every record due within kFeedAhead sim steps of
// the clock. Ahead of the release (never after it) and only a few steps ahead, so PENDING holds a
// handful of records instead of the whole backlog. Returns 0 or the admission_log refusal.
constexpr double kFeedAhead = 4.0;
int              m_feed(mh::orders::admission::stage_report *rep) {
    if (!mh::orders::admission::stage_active()) return 0;
    const uint64_t cb = mh::desync::game_clock_bits_now();
    const uint64_t ib = mh::desync::sim_step_interval_bits_now();
    double         clk, iv;
    memcpy(&clk, &cb, sizeof(clk));
    memcpy(&iv, &ib, sizeof(iv));
    const int rc = mh::orders::admission::stage_feed(clk + kFeedAhead * iv, rep);
    ++g_m.feed_calls;
    if (rep->pending_after > g_m.pend_peak) {
        g_m.pend_peak      = rep->pending_after;
        g_m.pend_peak_step = g_m.feed_calls;
    }
    return rc;
}

const char *feed_why(int rc) {
    return rc == mh::orders::admission::ERR_RING_SHORT     ? "a staged record fell off the admission ring before it was fed"
           : rc == mh::orders::admission::ERR_PENDING_FULL ? "ORDER_PENDING has no room for a due staged record"
                                                           : "the admission log latched an overflow reset (R5)";
}

// Finish a transfer that will not import: DONE carries the reason, the buffer goes.
void m_fail(uint32_t s, uint32_t step, int rc, const char *why) {
    WSAY("; [worldsync] REFUSED S=%lu rc=%d at step %lu: %s\n", (unsigned long)s, rc, (unsigned long)step, why);
    m_send_done(s, step, rc, 0);
    m_end();
}

void m_on_begin(int sender, uint32_t incident) {
    if (is_host()) return;
    if (g_m.st != M_IDLE) {
        WSAY("; [worldsync] BEGIN from %d ignored (already %s)\n", sender, g_m.st == M_BEGUN ? "receiving" : "catching up");
        return;
    }
    m_end();
    MH_Net_SnapshotCancel(MH_SNAP_DISCARD_RX); // a partial or delivered earlier transfer must not steer this one
    g_m.st       = M_BEGUN;
    g_m.host_id  = sender;
    g_m.incident = incident;
    g_m.begin_ms = GetTickCount();
    WSAY("; [worldsync] BEGIN rx from=%d incident=%lu -- keep playing, polling channel C\n", sender, (unsigned long)incident);
}

void m_on_meta(uint32_t s, const ws_meta_body &b) {
    if (g_m.st != M_BEGUN) return;
    g_m.meta      = true;
    g_m.meta_step = s;
    g_m.mb        = b;
    WSAY("; [worldsync] META rx S=%lu bytes=%lu digest_prev=%08X%08X\n", (unsigned long)s, (unsigned long)b.blob_len,
         (unsigned)(b.digest_prev >> 32), (unsigned)b.digest_prev);
}

void m_on_abort(uint32_t s, uint32_t reason) {
    if (g_m.st != M_BEGUN && g_m.st != M_CATCHUP) return;
    g_m.abort_rx     = true;
    g_m.abort_reason = reason;
    WSAY("; [worldsync] ABORT rx step=%lu reason=%lu\n", (unsigned long)s, (unsigned long)reason);
}

bool m_step(uint32_t next_step) {
    if (g_m.st == M_IDLE) return false;
    const int mode = session_mode();
    if (g_m.st == M_CATCHUP) {
        // R4: the diverged world can end its own match while we catch up. Nothing more to do here.
        if (mode != 3 || g_m.abort_rx) {
            WSAY("; [worldsync] CATCH-UP ABANDONED at step %lu (%s)\n", (unsigned long)next_step,
                 g_m.abort_rx ? "host aborted" : "the match left mode 3");
            if (g_h.ff_end) g_h.ff_end();
            m_end();
            return false;
        }
        mh::orders::admission::stage_report fr{};
        const int                           frc = m_feed(&fr);
        if (frc != 0) {
            WSAY("; [worldsync] STAGED RE-ADMISSION ABORTED at step %lu rc=%d remaining=%lu pending=%lu "
                 "pending_peak=%lu at_feed=%lu of %lu: %s\n",
                 (unsigned long)next_step, frc, (unsigned long)fr.remaining, (unsigned long)fr.pending_after,
                 (unsigned long)g_m.pend_peak, (unsigned long)g_m.pend_peak_step, (unsigned long)g_m.feed_calls,
                 feed_why(frc));
            if (g_h.ff_end) g_h.ff_end();
            m_send_done(g_m.S, next_step, frc, 0);
            m_end();
            return false;
        }
        if (g_h.ff_state && g_h.ff_state() == FF_DONE && !mh::orders::admission::stage_active()) {
            const uint32_t ms = GetTickCount() - g_m.import_ms;
            WSAY("; [worldsync] LIVE step=%lu blobstep=%lu ff_ms=%lu pending_peak=%lu at_feed=%lu of %lu\n",
                 (unsigned long)next_step, (unsigned long)g_m.S, (unsigned long)ms, (unsigned long)g_m.pend_peak,
                 (unsigned long)g_m.pend_peak_step, (unsigned long)g_m.feed_calls);
            m_send_done(g_m.S, next_step, 0, ms);
            if (g_h.ff_end) g_h.ff_end();
            m_end();
        }
        return false;
    }
    // M_BEGUN
    if (ws::minority_must_abort(mode, g_m.abort_rx)) {
        WSAY("; [worldsync] TRANSFER DROPPED at step %lu (%s)\n", (unsigned long)next_step,
             g_m.abort_rx ? "host aborted" : "this match left mode 3 (R4)");
        m_end();
        return false;
    }
    if ((int)(GetTickCount() - g_m.begin_ms) > g_cfg.timeout_ms + 10000) {
        m_fail(g_m.meta_step, next_step, RC_TIMEOUT, "no complete blob before the deadline");
        return false;
    }
    if (!g_m.buf) {
        g_m.cap = w::capture_capacity();
        g_m.buf = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, g_m.cap));
        if (!g_m.buf) {
            m_fail(0, next_step, RC_BAD_LEN, "cannot allocate the receive buffer");
            return false;
        }
    }
    ++g_m.poll_steps;
    if (!g_m.blob) {
        int       len   = (int)g_m.cap;
        int       state = 0;
        const int ready = MH_Net_SnapshotPoll(g_m.buf, &len, &state);
        if (!ready) {
            if (state == MH_SNAP_REFUSED && !g_m.refused_said) {
                g_m.refused_said = 1;
                MH_NetSnapshotStatus st;
                MH_Net_SnapshotStatus(&st);
                WSAY("; [worldsync] channel C REFUSED (err=%d) -- still waiting\n", st.last_err);
            }
            return false;
        }
        g_m.blob     = true;
        g_m.blob_len = (uint32_t)len;
        WSAY("; [worldsync] BLOB rx %lu bytes at step %lu\n", (unsigned long)len, (unsigned long)next_step);
    }
    if (!g_m.meta) return false; // the blob can beat META only if the datagram was lost; META repeats nothing

    // ---- pair META with the blob ----
    ws::import_input in{};
    in.blob_ready = true;
    in.meta_seen  = true;
    in.meta_step  = g_m.meta_step;
    in.meta_len   = g_m.mb.blob_len;
    in.blob_len   = g_m.blob_len;
    in.blob_step  = 0;
    in.root_match = false;
    if (g_m.blob_len >= sizeof(w::blob_header)) {
        in.blob_step       = reinterpret_cast<const w::blob_header *>(g_m.buf)->step;
        const uint64_t sum = fnv1a(g_m.buf, g_m.blob_len);
        in.root_match      = memcmp(&sum, g_m.mb.root, WS_ROOT_BYTES) == 0;
    } else {
        in.blob_len = 0; // shorter than a header can never equal META's length
    }
    mh::orders::admission::plan_report pr;
    in.ring_rc       = mh::orders::admission::plan(g_m.mb.n_src, &pr);
    const uint32_t S = g_m.meta_step;
    switch (ws::import_check(in)) {
        case ws::IMP_WAIT: return false;
        case ws::IMP_BAD_STEP: m_fail(S, next_step, RC_BAD_STEP, "blob header step != META step"); return false;
        case ws::IMP_BAD_LEN: m_fail(S, next_step, RC_BAD_LEN, "blob length != META length"); return false;
        case ws::IMP_BAD_ROOT: m_fail(S, next_step, RC_BAD_ROOT, "blob checksum != META checksum"); return false;
        case ws::IMP_RING_SHORT:
            m_fail(S, next_step, in.ring_rc,
                   in.ring_rc == ws::RC_RING_SHORT                  ? "the admission ring no longer reaches n_src+1"
                   : in.ring_rc == mh::orders::admission::ERR_AHEAD ? "this peer is BEHIND the host's admission counts"
                                                                    : "the admission log latched an abort (R5)");
            return false;
        case ws::IMP_GO: break;
    }
    if (S > next_step) { // we have not reached the capture step yet: nothing to rewind to
        m_fail(S, next_step, mh::orders::admission::ERR_AHEAD, "this peer has not reached the host's capture step");
        return false;
    }

    // ---- IMPORT ----
    const uint32_t backlog = next_step - S;
    LARGE_INTEGER  f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (g_h.horizon_hold) g_h.horizon_hold(1);
    libmh_resync_report rep;
    memset(&rep, 0, sizeof(rep));
    rep.struct_size = sizeof(rep);
    const int rc    = libmh_import_world_resync(g_m.buf, g_m.blob_len, g_m.mb.n_src, &rep);
    QueryPerformanceCounter(&t1);
    const int imp_ms = f.QuadPart ? (int)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart) : 0;
    if (rc != 0) {
        if (g_h.horizon_hold) g_h.horizon_hold(0);
        m_fail(S, next_step, rc, "libmh_import_world_resync refused (world untouched)");
        return false;
    }
    // From here the world IS the host's world at S. The re-admission is staged: arm-time cannot fail
    // (plan() passed before the first write); the per-step feed can, and aborts by name.
    mh::desync::rewind_for_world_sync(S, g_m.mb.digest_prev);
    mh::orders::admission::stage_report fr0{};
    const int                           frc0 = m_feed(&fr0); // records already due at the imported clock
    const int                           ff   = g_h.ff_begin ? g_h.ff_begin(g_cfg.ff_steps) : (int)FF_DONE;
    if (g_h.harness) g_h.harness(WS_IMPORT, S, (int)backlog, g_m.buf, g_m.blob_len);
    if (g_h.horizon_hold) g_h.horizon_hold(0);
    ++g_t.imports;
    WSAY("; [worldsync] IMPORT blobstep=%lu local=%lu backlog=%lu staged=%lu fed_now=%lu pending_after=%lu "
         "kept_regions=%lu rc=%d import_ms=%d ff=%s\n",
         (unsigned long)S, (unsigned long)next_step, (unsigned long)backlog, (unsigned long)rep.readmitted,
         (unsigned long)fr0.fed, (unsigned long)fr0.pending_after, (unsigned long)rep.kept_regions, rc, imp_ms,
         ff == FF_ACTIVE ? "catching up" : "already live");
    m_free(); // the 8 MB buffer is done
    g_m.st        = M_CATCHUP;
    g_m.S         = S;
    g_m.backlog   = backlog;
    g_m.import_ms = GetTickCount();
    if (frc0 != 0) {
        WSAY("; [worldsync] STAGED RE-ADMISSION ABORTED at import rc=%d: %s\n", frc0, feed_why(frc0));
        if (g_h.ff_end) g_h.ff_end();
        m_send_done(S, next_step, frc0, 0);
        m_end();
    }
    return true;
}

} // namespace

// ================================================================================================
// public
// ================================================================================================
void set_hooks(const hooks &h) { g_h = h; }

void configure(const char *ini_path, int action, uint64_t fp2, void (*log)(const char *fmt, ...)) {
    (void)ini_path; // every tuning value is fixed; `action` is the only [desync] key world sync reads
    g_log        = log;
    g_fp2        = fp2;
    g_cfg        = ws::config{};
    g_cfg.action = action;
    g_on         = (action == 1);
    if (!g_on) return;
    WSAY("; [worldsync] ARMED (action=1): confirm=%d steps, roster_quiet=%d steps, max=%d per peer, timeout=%d ms, "
         "ff=%d steps/frame, cooldown=%d steps; only the HOST acts; wire=desync v2 types 6..9 + channel C\n",
         g_cfg.confirm_steps, g_cfg.roster_quiet_steps, g_cfg.resync_max, g_cfg.timeout_ms, g_cfg.ff_steps,
         g_cfg.cooldown_steps);
}

bool              enabled() { return g_on; }
const ws::config &cfg() { return g_cfg; }
tallies           counters() { return g_t; }

void session_reset() {
    if (!g_on) return;
    if (g_m.st == M_CATCHUP && g_h.ff_end) g_h.ff_end();
    if (g_m.st != M_IDLE || g_t.begins || g_t.imports)
        WSAY("; [worldsync] match end: begins=%d captures=%d dones=%d aborts=%d imports=%d skips=%d\n", g_t.begins,
             g_t.captures, g_t.dones, g_t.aborts, g_t.imports, g_t.skips);
    m_end();
    memset(g_hp, 0, sizeof(g_hp));
    g_hist.clear();
    g_roster = ws::roster_tracker{};
    g_t      = tallies{};
}

void on_tick_verdict(const v2::tick_verdict &v, uint32_t host_step) {
    if (!g_on) return;
    if (v.sender < 0 || v.sender >= PLAYERS) return;
    if (v.kind == v2::tick_verdict::ok || v.kind == v2::tick_verdict::mismatch) {
        hist_put(v.sender, v.step, v.theirs);
        hist_put(PLAYERS, v.step, v.mine);
    }
    if (v.kind != v2::tick_verdict::mismatch || !is_host()) return;
    if (v.consecutive < g_cfg.confirm_steps) return;
    const int p  = v.sender;
    hostpeer &hp = g_hp[p];
    if (hp.f.state != ws::P_IDLE) return; // BEGUN / SENT / COOLDOWN: the incident is already being handled
    const int me = MH_Net_LocalPlayerId();
    if (me < 0 || me >= PLAYERS || me == p) return;

    // who diverged: every hash the host holds for the incident step (its own + each sender's)
    uint64_t state[PLAYERS] = {};
    uint32_t have           = 0;
    if (hist_get(PLAYERS, v.step, &state[me])) have |= 1u << me;
    for (int s = 0; s < PLAYERS; ++s) {
        if (s == me) continue;
        if (hist_get(s, v.step, &state[s])) have |= 1u << s;
    }
    const ws::diverge_verdict dv       = ws::pick_diverged(state, have, me, PLAYERS);
    auto                      skip_log = [&](int reason, const char *extra) {
        if (hp.last_skip == reason && host_step - hp.last_skip_step < 250u) return;
        hp.last_skip      = reason;
        hp.last_skip_step = host_step;
        ++g_t.skips;
        WSAY("; [worldsync] SKIP peer=%d: %s%s (step=%lu incident=%lu consecutive=%d)\n", p, ws::skip_name((ws::skip_reason)reason),
                                  extra, (unsigned long)host_step, (unsigned long)v.step, v.consecutive);
    };
    if (dv.kind == ws::V_HOST_MINORITY) {
        skip_log(ws::SKIP_HOST_MINORITY, "");
        return;
    }
    if (dv.kind != ws::V_RESYNC || !((dv.resync_mask >> p) & 1u)) return;

    ws::gate_input gi;
    gi.is_host  = true;
    gi.libmh_ok = g_h.libmh_ok ? g_h.libmh_ok() : false;
    MH_NetSnapshotStatus ss;
    MH_Net_SnapshotStatus(&ss);
    gi.transport_ok         = ss.supported == 1;
    gi.session_mode         = session_mode();
    gi.peer_human_alive     = (player_flags(p) & 0x06) == 0x06;
    gi.peer_active          = peer_active(p);
    gi.consecutive          = v.consecutive;
    gi.step                 = host_step;
    gi.roster               = g_roster;
    gi.state                = hp.f.state;
    gi.resyncs_done         = hp.f.resyncs;
    const ws::skip_reason r = ws::gate_check(g_cfg, gi);
    if (r != ws::SKIP_NONE) {
        if (r != ws::SKIP_BUSY && r != ws::SKIP_CONFIRM) skip_log(r, "");
        return;
    }
    ws::event e{ws::E_INCIDENT};
    e.a                = v.step;
    e.roster           = g_roster.word;
    const ws::action a = ws::fsm_step(g_cfg, hp.f, e);
    if (a.kind != ws::A_SEND_BEGIN) return;
    uint8_t b[MAX_FRAME];
    send_frame(b, ws_put_begin(b, g_fp2, v.step, p));
    hp.begin_ms          = GetTickCount();
    hp.capture_due       = true;
    hp.next_attempt_step = 0;
    hp.last_skip         = 0;
    ++g_t.begins;
    WSAY("; [worldsync] BEGIN peer=%d incident=%lu consecutive=%d groups=%d host_group=%d best_group=%d host_step=%lu "
         "roster=%08X resync=%d/%d\n",
         p, (unsigned long)v.step, v.consecutive, dv.groups, dv.host_group, dv.best_group, (unsigned long)host_step,
         (unsigned)g_roster.word, hp.f.resyncs, g_cfg.resync_max);
}

void on_frame(int sender, const uint8_t *buf, int len, uint32_t host_step) {
    if (!g_on) return;
    hdr h;
    if (!frame_ok(buf, len, 1, h)) return;
    if ((int)h.aux != MH_Net_LocalPlayerId()) return; // addressed to another player
    switch (h.type) {
        case T_WS_BEGIN: {
            ws_begin_body b;
            if (ws_get_begin(buf, len, g_fp2, h, b)) m_on_begin(sender, h.step);
            break;
        }
        case T_WS_META: {
            ws_meta_body b;
            if (ws_get_meta(buf, len, g_fp2, h, b)) m_on_meta(h.step, b);
            break;
        }
        case T_WS_ABORT: {
            ws_abort_body b;
            if (ws_get_abort(buf, len, g_fp2, h, b)) m_on_abort(h.step, b.reason);
            break;
        }
        case T_WS_DONE: {
            ws_done_body b;
            if (ws_get_done(buf, len, g_fp2, h, b)) host_on_done(sender, h.step, b, host_step);
            break;
        }
        default: break;
    }
}

bool step_begin(uint32_t next_step, uint64_t digest_now) {
    if (!g_on) return false;
    if (is_host()) {
        host_step(next_step, digest_now);
        return false;
    }
    return m_step(next_step);
}

} // namespace mh::desync::world_sync
