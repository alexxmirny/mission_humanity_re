//
// udp_snapshot_selftest.cpp -- `net_selftest.exe udpsnaptest`: THE CHUNKED SNAPSHOT PIPELINE
// (tracker mp:X1, plan D7).
//
// WHAT THIS SUITE IS FOR, and why it is not a second copy of udpbulktest. `udpbulktest` (mp:T2)
// proves the TRANSPORT: a megabyte of synthetic bytes crossing channel C at 5% loss, hash-verified,
// resuming across a dead link, never evicting a chunk. This suite proves the ARTEFACT: that the
// thing which crosses is a real `mh::state::world::capture()` blob -- 829 bound regions, the RNG
// channels among them -- and that what the receiver hands back imports into a poisoned world and
// reproduces the CAPTURING peer's hashes exactly, for every region.
//
// The difference is the whole item. A transport that delivers the right bytes and an application
// that reassembles them at the wrong offsets both pass a per-chunk check; only an end-to-end
// capture->import comparison catches the second.
//
// ---- WHY ONE PROCESS, ONE ARENA, AND WHY THAT IS STILL TWO PEERS -------------------------------
//
// `libmh_bind_regions` is process-global: there is exactly one bound world per process, so a "host
// arena" and a "client arena" cannot coexist here. worldtest solved the same problem the same way
// and the sequence is what makes it a claim rather than a coincidence:
//
//   fill the arena with structured content -> CAPTURE (this is the host at step N, and its hashes
//   are recorded) -> move the blob across two real UDP endpoints with real injected loss -> POISON
//   the arena with 0xCD, which agrees with nothing -> IMPORT what the receiver assembled -> the
//   hashes must be the recorded ones again.
//
// The poison is the peer boundary. After it, the only route from this process's memory to the
// capturing peer's numbers is through bytes that crossed a socket.
//
// TWO REAL ENDPOINTS, IN-PROCESS, for udpbulktest's reason repeated: a child process talking to
// 127.0.0.1 cannot lose a datagram, so "5% loss" would have no way to produce the 5%. Two Endpoint
// objects, each with its own socket and its own seeded `set_rx_loss`, put the dial exactly where
// the network would put it.
//
// ---- THE ARMS -----------------------------------------------------------------------------------
//
//   F.  FORMAT, no sockets, milliseconds. The manifest round-trips offline, and then seven
//       NEGATIVES: a flipped magic, a bumped format, a tampered chunk-hash entry, a bumped
//       body_len, a destination too small, a corrupted BODY chunk, and a missing chunk. Each is a
//       DIFFERENT refusal code, because "the transfer failed" is the diagnosis that helps nobody.
//   C.  CLEAN WIRE. The real snapshot crosses at 0% loss and imports. Also the non-vacuity arm for
//       the lossy one: a clean link must need NO retransmit.
//   L.  LOSSY WIRE at 5% both directions -- the item's headline clause. Asserts the injection fired
//       before it believes the hash.
//   R.  RE-REQUEST. The sender hands channel C a chunk that is WRONG BUT SELF-CONSISTENT: the
//       corruption happens inside the source callback, so the piece SHA that goes on the wire is
//       the SHA of the corrupt bytes and the transport is perfectly happy. Only the manifest --
//       committed to before the transfer started -- can refuse it. That refusal rewinds the
//       frontier and the chunk is re-sent. This arm is the reason the manifest exists.
//   T.  TRUNCATION. The link dies mid-transfer; the receiver keeps its verified prefix; a new link
//       comes up and the frontier is restored to `resume_chunk()`. The claim is not that the
//       transfer finishes -- it is that the already-verified chunks are NOT re-verified.
//   N.  ALL-OR-NOTHING, at both layers. The pipeline's gate never opens with a chunk missing, and
//       `world::import()` handed a truncated blob refuses and leaves the poisoned arena byte-for-
//       byte poisoned.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mh_net_udp/udp_endpoint.h"
#include "../mh_net_udp/udp_snapshot.h"

#include "mh_net_module.h" // mp:X1b: the three snapshot rows -- net_selftest.exe links the TCP bodies

#include "state/host_bind.h"      // libmh_bind_regions
#include "state/region_runtime.h" // clear_region / live_base
#include "state/world_snapshot.h"

namespace {

using mh::netudp::Config;
using mh::netudp::Counters;
using mh::netudp::Endpoint;
using mh::netudp::bulk::CHUNK_BYTES;
using Bulk     = mh::netudp::bulk::Stats;
namespace snap = mh::netudp::snapshot;

using namespace mh::state;
using namespace mh::state::world;

int g_checks = 0, g_fails = 0;

void checkf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    b[500];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", b);
    }
}

// ---- the arena, exactly worldtest's ------------------------------------------------------------
// The stock bases are game .bss addresses a console process cannot materialise, so every carried
// region is bound onto memory this process owns. That is also what the standalone host does, so the
// fixture exercises the real path.
struct arena {
    uint8_t *mem = nullptr;
    size_t   len = 0;

    bool bind_blocks() {
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) len += WORLD_SNAPSHOT_BLOCKS[i].len;
        mem = (uint8_t *)malloc(len);
        if (mem == nullptr) return false;
        memset(mem, 0, len);
        libmh_region_bind *binds =
            (libmh_region_bind *)malloc(sizeof(libmh_region_bind) * WORLD_SNAPSHOT_BLOCK_COUNT);
        if (binds == nullptr) return false;
        size_t off = 0;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
            binds[i].region_id = (uint32_t)WORLD_SNAPSHOT_BLOCKS[i].rid;
            binds[i].base      = mem + off;
            binds[i].size      = WORLD_SNAPSHOT_BLOCKS[i].len;
            binds[i].count     = 0;
            off += WORLD_SNAPSHOT_BLOCKS[i].len;
        }
        const int rc = libmh_bind_regions(binds, WORLD_SNAPSHOT_BLOCK_COUNT);
        free(binds);
        return rc == WORLD_SNAPSHOT_BLOCK_COUNT;
    }
    void fill(uint8_t v) { memset(mem, v, len); }
    ~arena() { free(mem); }
};

uint8_t pattern(size_t i) {
    return (uint8_t)((i * 131u + (i >> 8) * 17u + 7u) & 0xffu);
}

// FORMAT 2's capture walks the live map-region pool from two bound regions, so a pattern-filled
// arena hands it a non-null pointer to nowhere. A synthetic arena has no pool; saying so is both the
// truth and the fix. (worldtest measured the alternative: rc=139, no output.)
void empty_nav_pool() {
    static const region_id NAV[] = {RID_MAP_REGION_LIST_HEAD, RID_MAP_REGION_POOL_FREE_HEAD,
                                    RID_MAP_REGION_BY_INDEX, RID_MAP_REGION_GRID};
    for (size_t i = 0; i < sizeof(NAV) / sizeof(NAV[0]); ++i) clear_region(NAV[i]);
}

// The three RNG channel regions, named here rather than derived, because the clause says "including
// rng_state" and a derived list could quietly become empty. Same three as worldtest's arm R0.
struct rng_region {
    region_id   rid;
    const char *what;
};
const rng_region RNG_REGIONS[] = {
    {RID_STRAT_RNG_STATE, "rng_state (uint[4]: strategic / fx / AI / unreachable)"},
    {RID_STRAT_RNG_NORM_DIVISOR, "the normalising divisor"},
    {RID_STRAT_RNG_SEED_BYTE, "the agreed ch0 seed byte"},
};

// ---- the capture under test --------------------------------------------------------------------
uint8_t *g_blob     = nullptr; // the host's step-N capture
size_t   g_blob_len = 0;
uint8_t *g_recv     = nullptr; // what the receiver assembles
uint64_t g_content  = 0;       // the capture's own content hash
uint64_t g_lock_c = 0, g_lock_s = 0;
uint32_t g_masks = 0;
uint8_t  g_rng_copy[3][64]; // the RNG channels' bytes at capture time
uint32_t g_rng_len[3];

const unsigned char US_PSK[32] = {'M', 'H', 't', 'e', 's', 't', 'k', 'e', 'y', ' ', 'u',
                                  'd', 'p', ' ', 's', 'n', 'a', 'p', ' ', 'X', '1', ' ',
                                  'f', 'i', 'x', 'e', 'd', '!', '!', '!', '!', '!'};

void us_log(void * /*ctx*/, const char *line) {
    if (getenv("MH_UDP_SNAP_VERBOSE")) printf("    | %s\n", line);
}

void us_cfg(Config &c, int role, int port, unsigned short bind_port, int rx_timeout_ms) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port          = port;
    c.net.player_id     = (role == 0) ? 0 : 1;
    c.net.log           = 1;
    c.net.host_assign   = 0;
    c.net.ping_ms       = 200;
    c.net.rx_timeout_ms = rx_timeout_ms;
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

template <class Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(2);
    }
}

Endpoint g_host, g_cl;

// THE PIPELINE OBJECTS ARE FILE-SCOPE, not locals, and the reason is arithmetic rather than style:
// a Sender carries the whole manifest (80 KiB) and a Receiver carries a second copy plus the
// held-chunk bitmap, so two of each on a 1 MiB thread stack -- under ASan's redzones -- is a stack
// overflow waiting for the first arm that nests them. Two of each is exactly what the arms need: one
// pair for the transfer under test, one for a second sender or a second receiver in the same arm.
snap::Sender   g_tx_a, g_tx_b;
snap::Receiver g_rx_a, g_rx_b;

bool link_up(int base, int host_timeout_ms, unsigned loss_pm) {
    new (&g_host) Endpoint();
    new (&g_cl) Endpoint();
    g_host.set_log(us_log, nullptr);
    g_cl.set_log(us_log, nullptr);
    // Distinct seeds: a correlated loss pattern is one no network produces, and it would hide a
    // repair path that only works in one direction.
    g_host.set_rx_loss(loss_pm, 0x51a9c30du);
    g_cl.set_rx_loss(loss_pm, 0x7e11beefu);
    Config ch, cc;
    us_cfg(ch, 0, base, (unsigned short)base, host_timeout_ms);
    us_cfg(cc, 1, base, (unsigned short)(base + 1), -1);
    if (!g_host.start(ch, US_PSK, true)) return false;
    if (!g_cl.start(cc, US_PSK, true)) return false;
    return wait_for([&] { return g_host.peer_count() == 1; }, 10000);
}

// ---- THE DRIVER --------------------------------------------------------------------------------
//
// The five lines an application needs, in one place so every arm drives the pipeline the way a real
// consumer would rather than each inventing its own loop: pop a chunk from the never-evictable lane,
// hand it to the receiver, and on a manifest-hash refusal rewind the frontier so the sender re-sends
// it. That refusal-to-rewind edge IS the re-request protocol -- there is no NAK frame.
int drive_rx(Endpoint &ep, snap::Receiver &rx, long *out_rerequests) {
    int got = 0;
    for (;;) {
        uint32_t id  = 0;
        int      len = (int)CHUNK_BYTES;
        uint8_t  tmp[CHUNK_BYTES];
        if (!ep.bulk_recv(&id, tmp, &len)) return got;
        ++got;
        const int rc = rx.on_chunk(id, tmp, (uint32_t)len);
        if (rc == snap::ERR_CHUNK_HASH) {
            if (out_rerequests) ++*out_rerequests;
            ep.bulk_resume_at(id);
        }
    }
}

void print_rx(const char *who, const snap::Receiver::Stats &s) {
    printf("     %s: manifest %ld chunk(s) ready=%d | body %lu/%lu ok %ld refused-by-hash %ld dup "
           "%ld other %ld | manifest-refused %ld | COMPLETE=%d\n",
           who, s.chunks_manifest, (int)s.manifest_ready, (unsigned long)s.verified_prefix,
           (unsigned long)s.body_chunks, s.chunks_ok, s.chunks_hash_refused, s.chunks_dup,
           s.chunks_refused_other, s.manifest_refused, (int)s.complete);
}

// =================================================================================================
// ARM U -- THE MODULE SURFACE'S "UNSUPPORTED" PATH (mp:X1b)
//
// WHAT THIS SUITE CAN SEE OF THE MODULE ROWS, AND IT IS EXACTLY ONE THING. net_selftest.exe links
// mh_net/net_transport.cpp, so inside THIS process MH_Net_Snapshot* are the TCP module's bodies. The
// UDP bodies live in udp_transport.cpp, which this image deliberately never compiles (that TU defines
// all 26 MH_Net_* symbols and would be a duplicate of the 26 already here -- the two-TU rule
// check_module_bind --net-surface enforces). So the arm is not "the pipeline over the module rows";
// it is the OTHER half, and it is the half with no other oracle: that a transport which cannot move
// a snapshot says so, in a named way, without faulting.
//
// THE FAULT CLAUSE IS NOT RHETORICAL. These bodies are reachable from a real run: mh.dll's shims
// forward to whichever module `[net] transport` bound, so a lane on the shipped TCP default with a
// harness that arms `snapshot_at` lands here. A stub that dereferenced its arguments, or that
// answered MH_SNAP_IDLE and invited an endless poll, would turn a configuration mistake into a hang
// or a crash. Every call below is made with the arguments a real driver passes AND with nulls.
int arm_unsupported() {
    printf("  -- U: the TCP module answers the three snapshot rows as UNSUPPORTED\n");

    MH_NetSnapshotStatus st;
    memset(&st, 0xA5, sizeof(st)); // poison: every field below is WRITTEN, not left as it was
    MH_Net_SnapshotStatus(&st);
    checkf(st.size == (unsigned)sizeof(MH_NetSnapshotStatus),
           "U: status stamps its own size (%u)", st.size);
    checkf(st.supported == 0, "U: supported = 0 -- this transport has no channel C (%d)",
           st.supported);
    checkf(st.state == MH_SNAP_UNSUPPORTED, "U: state = MH_SNAP_UNSUPPORTED (%d)", st.state);
    checkf(st.tx_len == 0 && st.rx_len == 0 && st.rx_chunks == 0 && st.rx_verified == 0 &&
               st.rx_refused == 0,
           "U: every counter is 0 over the poison");
    checkf(st.root_hex[0] == '\0', "U: the root is the EMPTY string, not 64 zeros");

    uint8_t   body[64];
    const int sent = MH_Net_SnapshotSend(1, body, (int)sizeof(body));
    checkf(sent == 0, "U: Send refuses (%d)", sent);

    // THE STATE A POLL REPORTS IS THE POINT OF THE ARM. MH_SNAP_IDLE would mean "no transfer is
    // running YET" and a driver would keep polling for one that can never start; UNSUPPORTED is
    // terminal and is what a caller can log and stop on.
    int       len   = (int)sizeof(body);
    int       state = 12345; // a value neither IDLE nor UNSUPPORTED, so "written" is observable
    const int ready = MH_Net_SnapshotPoll(body, &len, &state);
    checkf(ready == 0, "U: Poll delivers nothing (%d)", ready);
    checkf(state == MH_SNAP_UNSUPPORTED, "U: ... and says UNSUPPORTED, not IDLE (%d)", state);
    checkf(len == 0, "U: ... and zeroes the length rather than leaving the caller's capacity (%d)",
           len);

    // Nulls everywhere. A caller that has nothing to offer yet must not be a fault.
    MH_Net_SnapshotStatus(nullptr);
    checkf(MH_Net_SnapshotSend(0, nullptr, 0) == 0, "U: Send(null, 0) refuses without faulting");
    checkf(MH_Net_SnapshotPoll(nullptr, nullptr, nullptr) == 0,
           "U: Poll(null, null, null) answers without faulting");
    return 0;
}

// =================================================================================================
// ARM F -- the format, offline
// =================================================================================================
// Feed a sender's composed image straight into a receiver, chunk by chunk, with one optional
// mutation. Returns the FIRST refusal code (0 if none) and keeps going.
//
// First, not last, and the distinction is load-bearing: a refused manifest makes every body chunk
// after it refuse too (with ERR_NO_MANIFEST, correctly), so "the last code" would report the
// CONSEQUENCE of the mutation on every negative arm instead of its diagnosis, and all five would
// assert the same uninformative number.
int feed(snap::Sender &tx, snap::Receiver &rx, long mutate_chunk, uint32_t mutate_off,
         uint8_t mutate_xor, long skip_chunk) {
    const uint32_t total = tx.manifest_chunks() + tx.body_chunks();
    int            first = 0;
    for (uint32_t c = 0; c < total; ++c) {
        if ((long)c == skip_chunk) continue;
        const uint32_t off = c * CHUNK_BYTES;
        const uint32_t len = (tx.image_len() - off > CHUNK_BYTES) ? CHUNK_BYTES : tx.image_len() - off;
        uint8_t        buf[CHUNK_BYTES];
        snap::Sender::source(&tx, off, buf, len);
        if ((long)c == mutate_chunk && mutate_off < len) buf[mutate_off] ^= mutate_xor;
        const int rc = rx.on_chunk(c, buf, len);
        if (rc != snap::OK && first == 0) first = rc;
    }
    return first;
}

int arm_format() {
    printf("  -- F: the manifest format, offline\n");
    snap::Sender &tx = g_tx_a;
    const int     rc = tx.begin(g_blob, (uint32_t)g_blob_len);
    checkf(rc == snap::OK, "F: the sender hashes an %lu-byte world capture (rc=%d)",
           (unsigned long)g_blob_len, rc);
    if (rc != snap::OK) return 1;
    printf("     image: body %lu B in %lu chunk(s), manifest %lu B padded to %lu chunk(s), total "
           "%lu B\n",
           (unsigned long)tx.body_len(), (unsigned long)tx.body_chunks(),
           (unsigned long)tx.manifest_len(), (unsigned long)tx.manifest_chunks(),
           (unsigned long)tx.image_len());
    char hex[65];
    snap::hex32(tx.root(), hex);
    printf("     root: %s\n", hex);

    // NON-VACUITY FIRST: a root of zero, or a body of zero chunks, would make every arm below
    // agree with itself about nothing.
    checkf(tx.body_chunks() > 1, "F: the capture is more than one chunk (%lu)",
           (unsigned long)tx.body_chunks());
    {
        uint8_t zero[32];
        memset(zero, 0, sizeof(zero));
        checkf(memcmp(tx.root(), zero, 32) != 0, "F: the root hash is not zero");
    }

    snap::Receiver &rx = g_rx_a;
    rx.reset(g_recv, (uint32_t)g_blob_len);
    memset(g_recv, 0xA5, g_blob_len);
    const int fed = feed(tx, rx, -1, 0, 0, -1);
    checkf(fed == snap::OK, "F: a clean feed refuses nothing (last rc=%d)", fed);
    checkf(rx.complete(), "F: the receiver completes");
    checkf(rx.body() != nullptr, "F: the gate opens");
    checkf(rx.body_len() == g_blob_len, "F: the body length survives");
    checkf(rx.body() != nullptr && memcmp(rx.body(), g_blob, g_blob_len) == 0,
           "F: the assembled body is byte-identical to the capture");
    checkf(memcmp(rx.root(), tx.root(), 32) == 0, "F: both ends agree on the root hash");
    snap::Receiver::Stats s;
    rx.stats(s);
    print_rx("offline", s);

    // ---- the negatives, one refusal code each ---------------------------------------------------
    struct neg {
        const char *what;
        long        chunk;
        uint32_t    off;
        uint8_t     x;
        long        skip;
        int         want;
    };
    // Offsets are the manifest's own field offsets: magic at 0, format at 8, body_len at 12, and
    // the chunk-hash vector at 88. A mutation inside the vector must be caught by the ROOT, which
    // is the check that makes the vector trustworthy at all.
    const neg NEGS[] = {
        {"a flipped magic byte", 0, 1, 0xff, -1, snap::ERR_MAGIC},
        {"a bumped format word", 0, 8, 0x01, -1, snap::ERR_FORMAT},
        // Byte 14 of body_len, not byte 12: a low-byte flip moves the length by at most 255 and
        // the chunk COUNT usually survives that, so the arm would land on ERR_ROOT and quietly stop
        // testing the structural check it was written for. 0x40 at byte 14 moves it by 4 MiB.
        {"a bumped body_len", 0, 14, 0x40, -1, snap::ERR_SHAPE},
        {"a tampered chunk-hash entry", 0, 88, 0x01, -1, snap::ERR_ROOT},
        {"a tampered root", 0, 56, 0x01, -1, snap::ERR_ROOT},
    };
    for (size_t i = 0; i < sizeof(NEGS) / sizeof(NEGS[0]); ++i) {
        snap::Receiver &r2 = g_rx_b;
        r2.reset(g_recv, (uint32_t)g_blob_len);
        const int got = feed(tx, r2, NEGS[i].chunk, NEGS[i].off, NEGS[i].x, NEGS[i].skip);
        checkf(got == NEGS[i].want, "F: %s is refused with %d (got %d)", NEGS[i].what, NEGS[i].want,
               got);
        checkf(!r2.complete(), "F: %s leaves the gate shut", NEGS[i].what);
    }

    // A corrupted BODY chunk: refused by the manifest hash, and the chunk is NOT held -- so the
    // prefix stops there and the transfer can only proceed by re-sending it.
    {
        snap::Receiver &r2 = g_rx_b;
        r2.reset(g_recv, (uint32_t)g_blob_len);
        const long bad = (long)tx.manifest_chunks() + 2;
        const int  got = feed(tx, r2, bad, 17, 0x5a, -1);
        checkf(got == snap::ERR_CHUNK_HASH, "F: a corrupted body chunk is refused by its manifest "
                                            "hash (got %d)",
               got);
        checkf(!r2.complete(), "F: ... and the gate stays shut");
        checkf(r2.verified_prefix() == 2, "F: ... and the verified prefix stops at it (%lu)",
               (unsigned long)r2.verified_prefix());
        checkf(r2.resume_chunk() == (uint32_t)bad, "F: ... so resume_chunk names it (%lu vs %ld)",
               (unsigned long)r2.resume_chunk(), bad);
        r2.stats(s);
        checkf(s.chunks_hash_refused == 1, "F: ... exactly once (%ld)", s.chunks_hash_refused);
    }

    // A MISSING chunk: the all-or-nothing gate, offline.
    {
        snap::Receiver &r2 = g_rx_b;
        r2.reset(g_recv, (uint32_t)g_blob_len);
        feed(tx, r2, -1, 0, 0, (long)tx.manifest_chunks() + 1);
        checkf(!r2.complete(), "F: one missing chunk leaves the transfer incomplete");
        checkf(r2.body() == nullptr, "F: ... and body() hands out nothing");
        checkf(r2.finish_status() == snap::ERR_INCOMPLETE,
               "F: ... and the status names it (%d)", r2.finish_status());
    }

    // A destination too small is refused AT THE MANIFEST, before a body byte exists.
    {
        snap::Receiver &r2 = g_rx_b;
        r2.reset(g_recv, (uint32_t)(g_blob_len / 2));
        const int got = feed(tx, r2, -1, 0, 0, -1);
        checkf(got == snap::ERR_CAPACITY || got == snap::ERR_NO_MANIFEST,
               "F: a destination too small is refused (got %d)", got);
        checkf(!r2.manifest_ready(), "F: ... at the manifest, so no body byte was ever written");
    }
    return 0;
}

// =================================================================================================
// THE WIRE ARMS
// =================================================================================================
// A source that corrupts ONE chunk the FIRST time it is pulled (arm R). The corruption happens
// inside the composer, so channel C hashes the corrupt bytes and puts a perfectly self-consistent
// piece on the wire: the transport has no way to object. Only the manifest can.
struct evil_src {
    snap::Sender *tx;
    uint32_t      bad_off;   // the image offset of the chunk to spoil
    int           remaining; // spoil this many more pulls of it
    long          fired;
};
evil_src g_evil;

void evil_source(void *ctx, uint32_t off, uint8_t *out, uint32_t len) {
    evil_src *e = (evil_src *)ctx;
    snap::Sender::source(e->tx, off, out, len);
    if (off == e->bad_off && e->remaining > 0 && len > 3) {
        out[3] ^= 0x7fu;
        --e->remaining;
        ++e->fired;
    }
}

struct wire_result {
    bool     ok;
    DWORD    ms;
    long     rerequests;
    long     retx;
    long     lost;
    long     sha_fail;
    uint32_t chunks;
};

// One transfer, end to end. `src`/`ctx` let arm R substitute the spoiling composer.
bool wire_transfer(const char *name, int base, const uint8_t *blob, uint32_t len, unsigned loss_pm,
                   uint8_t *dst, mh::netudp::bulk::source_fn src, void *ctx, DWORD budget_ms,
                   wire_result &out) {
    memset(&out, 0, sizeof(out));
    snap::Sender &tx = g_tx_a;
    if (tx.begin(blob, len) != snap::OK) {
        checkf(false, "%s: the sender armed", name);
        return false;
    }
    out.chunks = tx.body_chunks();
    printf("  -- %s: %lu B, %lu body chunk(s) + %lu manifest chunk(s), loss %u/1000\n", name,
           (unsigned long)len, (unsigned long)tx.body_chunks(),
           (unsigned long)tx.manifest_chunks(), loss_pm);

    snap::Receiver &rx = g_rx_a;
    rx.reset(dst, len);
    memset(dst, 0xA5, len);

    if (!link_up(base, -1, loss_pm)) {
        checkf(false, "%s: the link came up", name);
        g_host.stop();
        g_cl.stop();
        return false;
    }
    if (ctx != nullptr) g_evil.tx = &tx; // arm R's composer wraps THIS sender
    const DWORD t0 = GetTickCount();
    // The host learns the client's player id from a FLAG_HELLO that rides the reliable stream a
    // moment after admission, so the send is retried rather than raced.
    const bool started = wait_for(
        [&] {
            return ctx != nullptr
                       ? g_host.bulk_send_src(1, src, ctx, tx.image_len())
                       : g_host.bulk_send_src(1, &snap::Sender::source, &tx, tx.image_len());
        },
        5000);
    checkf(started, "%s: the transfer started", name);
    if (!started) {
        g_host.stop();
        g_cl.stop();
        return false;
    }
    const bool done = wait_for(
        [&] {
            drive_rx(g_cl, rx, &out.rerequests);
            return rx.complete();
        },
        budget_ms);
    drive_rx(g_cl, rx, &out.rerequests);
    out.ms = GetTickCount() - t0;

    Bulk bh, bc;
    g_host.bulk_stats(bh);
    g_cl.bulk_stats(bc);
    Counters kh, kc;
    g_host.counters(kh);
    g_cl.counters(kc);
    out.retx     = bh.tx_retx;
    out.lost     = kh.dgram_dropped_sim + kc.dgram_dropped_sim;
    out.sha_fail = bc.rx_sha_fail;

    snap::Receiver::Stats s;
    rx.stats(s);
    print_rx("client", s);
    printf("     wire: %lu ms | tx pieces %ld (retx %ld) | rx chunks %ld sha-fail %ld | synthetic "
           "loss %ld | lane evicted %ld refused %ld | re-requests %ld\n",
           (unsigned long)out.ms, bh.tx_pieces, bh.tx_retx, bc.rx_chunks, bc.rx_sha_fail, out.lost,
           bc.lane_evicted, bc.lane_refused, out.rerequests);

    checkf(done && rx.complete(), "%s: the pipeline completed", name);
    checkf(bc.lane_evicted == 0, "%s: NO CHUNK WAS EVICTED (%ld)", name, bc.lane_evicted);
    checkf(kh.malformed + kc.malformed == 0, "%s: no malformed frames (%ld)", name,
           kh.malformed + kc.malformed);
    out.ok = rx.complete() && rx.body() != nullptr && memcmp(rx.body(), blob, len) == 0;
    checkf(out.ok, "%s: the assembled body is byte-identical to the source", name);
    if (loss_pm > 0) {
        checkf(out.lost > 0, "%s: the loss injection actually fired (%ld datagrams destroyed)",
               name, out.lost);
        checkf(out.retx > 0, "%s: the repair path ran (%ld repeats)", name, out.retx);
    } else if (ctx == nullptr) {
        checkf(out.retx == 0, "%s: a clean link needed NO retransmit (%ld)", name, out.retx);
    }
    g_host.stop();
    g_cl.stop();
    return out.ok;
}

// ---- the import half, shared by arms C and L ---------------------------------------------------
//
// THE PEER BOUNDARY. Poison first, and assert the poison actually disagrees -- an import compared
// against memory that already held the right answer proves nothing.
void import_and_compare(arena &a, const char *name, const uint8_t *received, size_t n) {
    a.fill(0xCD);
    empty_nav_pool();
    uint64_t poisoned_lock = 0;
    lockstep_hash(g_masks, &poisoned_lock, nullptr);
    checkf(canonical_hash() != g_content, "%s: the poisoned world does not already agree (content)",
           name);
    checkf(poisoned_lock != g_lock_c, "%s: ... nor by lockstep hash", name);

    const int irc = import(received, n);
    checkf(irc == WORLD_OK, "%s: the transferred blob imports (rc=%d)", name, irc);
    checkf(canonical_hash() == g_content, "%s: the imported world reproduces the CONTENT hash",
           name);
    uint64_t c = 0, st = 0;
    lockstep_hash(g_masks, &c, &st);
    checkf(c == g_lock_c, "%s: ... and the capturing peer's lockstep hash", name);
    checkf(st == g_lock_s, "%s: ... and its state-only sibling", name);

    // THE CLAUSE'S OWN WORDS: "for every region including rng_state". Every region is the content
    // hash above -- it is an FNV fold over all 829 blocks -- and the RNG channels are called out
    // byte-for-byte here because they are the three the item names and the three a dispositions
    // edit could silently drop.
    for (size_t i = 0; i < sizeof(RNG_REGIONS) / sizeof(RNG_REGIONS[0]); ++i) {
        const void *live = (const void *)(uintptr_t)live_base(RNG_REGIONS[i].rid);
        checkf(live != nullptr && memcmp(live, g_rng_copy[i], g_rng_len[i]) == 0,
               "%s: %s is byte-identical after the transfer (%u B)", name, RNG_REGIONS[i].what,
               g_rng_len[i]);
    }
}

// =================================================================================================
// ARM T -- the truncation
// =================================================================================================
int arm_truncation(int base) {
    const uint32_t len = 320u * 1024u; // 20 body chunks: enough to die in the middle of
    printf("  -- T: a transfer truncated mid-flight, resumed at the verified prefix (%lu B)\n",
           (unsigned long)len);
    uint8_t *blob = (uint8_t *)malloc(len);
    uint8_t *dst  = (uint8_t *)malloc(len);
    for (uint32_t i = 0; i < len; ++i) blob[i] = pattern(i * 7u + 3u);

    snap::Sender &tx = g_tx_b;
    if (tx.begin(blob, len) != snap::OK) {
        checkf(false, "T: the sender armed");
        free(blob);
        free(dst);
        return 1;
    }
    snap::Receiver &rx = g_rx_b;
    rx.reset(dst, len);
    memset(dst, 0xA5, len);

    long rer = 0;
    // 1.5 s host link timeout, as udprelinktest uses: the host must retire the corpse of the old
    // link before the same player dials again.
    if (!link_up(base, 1500, 0)) {
        checkf(false, "T: the link came up");
        g_host.stop();
        g_cl.stop();
        free(blob);
        free(dst);
        return 1;
    }
    checkf(wait_for([&] { return g_host.bulk_send_src(1, &snap::Sender::source, &tx,
                                                      tx.image_len()); },
                    5000),
           "T: the transfer started");
    const bool part = wait_for(
        [&] {
            drive_rx(g_cl, rx, &rer);
            return rx.verified_prefix() >= 5;
        },
        30000);
    checkf(part, "T: at least five chunks were verified before the kill (%lu)",
           (unsigned long)rx.verified_prefix());
    const uint32_t        prefix = rx.verified_prefix();
    snap::Receiver::Stats s0;
    rx.stats(s0);
    const long ok_before = s0.chunks_ok;
    g_host.stop();
    g_cl.stop();

    // A NEW LINK, and a receiver that kept its prefix. Nothing was persisted and nothing was
    // renegotiated: the application restores the frontier it already knows, and the sender follows
    // it because the frontier is the only thing that steers a channel-C transfer.
    if (!link_up(base + 2, -1, 0)) {
        checkf(false, "T: the link came back");
        g_host.stop();
        g_cl.stop();
        free(blob);
        free(dst);
        return 1;
    }
    g_cl.bulk_resume_at(rx.resume_chunk());
    // A FRESH SENDER, deliberately: the host restarted too, so nothing about the first transfer
    // survives on that side. Everything the resume needs is on the receiver's.
    snap::Sender &tx2 = g_tx_a;
    tx2.begin(blob, len);
    checkf(wait_for([&] { return g_host.bulk_send_src(1, &snap::Sender::source, &tx2,
                                                      tx2.image_len()); },
                    5000),
           "T: the transfer restarted");
    const bool done = wait_for(
        [&] {
            drive_rx(g_cl, rx, &rer);
            return rx.complete();
        },
        60000);
    drive_rx(g_cl, rx, &rer);

    snap::Receiver::Stats s1;
    rx.stats(s1);
    print_rx("client", s1);
    printf("     resumed at chunk %lu of %lu (prefix before the kill: %lu; body chunks verified: "
           "%ld before, %ld total)\n",
           (unsigned long)(tx.manifest_chunks() + prefix), (unsigned long)tx.body_chunks(),
           (unsigned long)prefix, ok_before, s1.chunks_ok);

    checkf(done && rx.complete(), "T: the resumed transfer completed");
    checkf(rx.body() != nullptr && memcmp(rx.body(), blob, len) == 0,
           "T: the assembled body is byte-identical across the outage");
    // THE ACTUAL CLAIM, and the one a "it finished anyway" run would hide: a chunk verified before
    // the outage is not verified again. `chunks_ok` counts first-time verifications only (a repeat
    // is `chunks_dup`), so a restart from zero would make this equal body_chunks + prefix.
    checkf(s1.chunks_ok == (long)tx.body_chunks(),
           "T: every body chunk was verified EXACTLY ONCE across the restart (%ld of %lu)",
           s1.chunks_ok, (unsigned long)tx.body_chunks());
    g_host.stop();
    g_cl.stop();
    free(blob);
    free(dst);
    return 0;
}

} // namespace

// =================================================================================================
int run_udpsnaptest(int port);

int run_udpsnaptest(int port) {
    // udploopbacktest holds the argument's own band, udprelinktest takes +100 and udpbulktest +200,
    // so this suite takes +300 and spaces its arms ten apart.
    const int base = port + 300;
    printf("=== udpsnaptest (mp:X1: the chunked snapshot pipeline) on ports %d.. ===\n", base);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    arena a;
    if (!a.bind_blocks()) {
        printf("  FAIL: could not bind the %d carried regions\n", WORLD_SNAPSHOT_BLOCK_COUNT);
        return 1;
    }
    for (size_t i = 0; i < a.len; ++i) a.mem[i] = pattern(i);
    empty_nav_pool();

    // ---- the host's step-N capture --------------------------------------------------------------
    const size_t need = capture_capacity();
    g_blob            = (uint8_t *)malloc(need);
    g_recv            = (uint8_t *)malloc(need);
    if (g_blob == nullptr || g_recv == nullptr) {
        printf("  FAIL: out of memory for two %lu-byte blobs\n", (unsigned long)need);
        return 1;
    }
    capture_params p;
    p.step       = 137u; // "step N" -- an arbitrary mid-match step, not step 0
    p.mask_flags = MASK_CTRL_GROUP | MASK_SOLDIER_ANIM | MASK_PLANETS_GFX;
    p.game_clock = 0x1122334455667788ULL;
    g_masks      = p.mask_flags;
    lockstep_hash(g_masks, &p.lockstep_combined, &p.lockstep_state);
    size_t    got = 0;
    const int crc = capture(g_blob, need, &got, p);
    checkf(crc == WORLD_OK, "capture succeeds over a fully bound arena (rc=%d)", crc);
    if (crc != WORLD_OK) {
        printf("=== udpsnaptest: %d checks, %d failures ===\n", g_checks, g_fails);
        return 1;
    }
    g_blob_len = got;
    {
        blob_header h;
        memcpy(&h, g_blob, sizeof(h));
        g_content = h.base.content_hash;
        g_lock_c  = h.lockstep_combined;
        g_lock_s  = h.lockstep_state;
    }
    for (size_t i = 0; i < sizeof(RNG_REGIONS) / sizeof(RNG_REGIONS[0]); ++i) {
        g_rng_len[i] = REGIONS[RNG_REGIONS[i].rid].reach;
        if (g_rng_len[i] > sizeof(g_rng_copy[0])) g_rng_len[i] = (uint32_t)sizeof(g_rng_copy[0]);
        memcpy(g_rng_copy[i], (const void *)(uintptr_t)live_base(RNG_REGIONS[i].rid), g_rng_len[i]);
    }
    printf("  host capture at step %u: %lu bytes, content=%08lX%08lX lockstep=%08lX%08lX\n", p.step,
           (unsigned long)g_blob_len, (unsigned long)(g_content >> 32),
           (unsigned long)(g_content & 0xffffffffu), (unsigned long)(g_lock_c >> 32),
           (unsigned long)(g_lock_c & 0xffffffffu));

    arm_unsupported();
    arm_format();

    // ---- arm C: the clean wire ------------------------------------------------------------------
    //
    // A HALF-MEBIBYTE SYNTHETIC BLOB, not the 7.86 MB capture, and the split is deliberate. This arm
    // asserts exactly one thing the lossy arm cannot: that a CLEAN link needs no retransmit at all
    // (a clean run that used the repair path means the window arithmetic is wrong rather than that
    // the link was lucky). That claim is about the transport's behaviour per chunk and is as true of
    // 32 chunks as of 480. What it would COST at full size is measured: 28.4 s, and the gate runs
    // every suite twice (ASan and plain). The item's identity clause -- capture, transfer, import,
    // same hashes -- belongs to the harder arm below, which is where the done_when puts it.
    wire_result rc_clean;
    {
        const uint32_t n     = 512u * 1024u;
        uint8_t       *small = (uint8_t *)malloc(n);
        for (uint32_t i = 0; i < n; ++i) small[i] = pattern(i * 5u + 1u);
        uint8_t *dst = (uint8_t *)malloc(n);
        wire_transfer("C (clean)", base, small, n, 0, dst, nullptr, nullptr, 60000, rc_clean);
        free(small);
        free(dst);
    }

    // ---- arm L: 5% loss, both directions -- the headline clause ---------------------------------
    wire_result rc_lossy;
    if (wire_transfer("L (5% loss)", base + 10, g_blob, (uint32_t)g_blob_len, 50, g_recv, nullptr,
                      nullptr, 300000, rc_lossy))
        import_and_compare(a, "L (5% loss)", g_recv, g_blob_len);

    // ---- arm N: all-or-nothing, at the IMPORT layer too ------------------------------------------
    //
    // Arm F proved the PIPELINE's gate. This proves the second layer, which is the one the world
    // blob's own contract promises: a blob that is missing bytes is refused whole, and the poisoned
    // world is still poisoned afterwards -- byte for byte, not merely "hashes differently".
    {
        printf("  -- N: the import writes NOTHING when the blob is short\n");
        a.fill(0xCD);
        uint8_t *witness = (uint8_t *)malloc(a.len);
        memcpy(witness, a.mem, a.len);
        // ONE CHUNK SHORT OF THE BLOCK PAYLOAD, not of the whole blob -- and the difference is a
        // measurement, not a nicety. A FORMAT 2 capture is `blob_size()` of blocks plus a NAV
        // TRAILER (here 7,718,688 + 139,304 = 7,857,992 bytes), and `import()` is deliberately the
        // BYTE half: it does not read the trailer at all. So cutting 16 KiB off the END of the blob
        // removes only trailer bytes and imports CLEANLY -- measured, rc=0, and it is exactly the
        // kind of arm that would have sat green while testing nothing. The cut has to reach the
        // blocks for "a chunk is missing" to be a statement about the world.
        const size_t short_n = blob_size() - CHUNK_BYTES;
        const int    irc     = import(g_recv, short_n);
        checkf(irc != WORLD_OK, "N: a blob one chunk short is refused (rc=%d)", irc);
        checkf(memcmp(a.mem, witness, a.len) == 0,
               "N: ... and the world is byte-for-byte untouched -- not one block was written");
        free(witness);
    }

    // ---- arm R: a wrong-but-self-consistent chunk, refused by the manifest and re-requested ------
    {
        const uint32_t len  = 256u * 1024u; // 16 body chunks
        uint8_t       *blob = (uint8_t *)malloc(len);
        uint8_t       *dst  = (uint8_t *)malloc(len);
        for (uint32_t i = 0; i < len; ++i) blob[i] = pattern(i * 3u + 11u);
        // Only to learn manifest_chunks, which is a function of the blob's length -- the transfer
        // itself arms its own sender inside wire_transfer.
        snap::Sender &probe = g_tx_b;
        probe.begin(blob, len);
        memset(&g_evil, 0, sizeof(g_evil));
        g_evil.bad_off   = (probe.manifest_chunks() + 4u) * CHUNK_BYTES;
        g_evil.remaining = 1;
        wire_result r;
        wire_transfer("R (a corrupt chunk the wire cannot see)", base + 20, blob, len, 0, dst,
                      &evil_source, &g_evil, 60000, r);
        checkf(g_evil.fired == 1, "R: the spoiling composer fired exactly once (%ld)", g_evil.fired);
        checkf(r.rerequests == 1, "R: the manifest hash refused it exactly once and re-requested "
                                  "it (%ld)",
               r.rerequests);
        checkf(r.sha_fail == 0, "R: the TRANSPORT saw nothing wrong (%ld wire sha failures) -- "
                                "which is the point of the arm",
               r.sha_fail);
        checkf(r.ok, "R: the re-sent chunk repaired the transfer");
        free(blob);
        free(dst);
    }

    // ---- arm T: truncation and resume ------------------------------------------------------------
    arm_truncation(base + 30);

    printf("  measured: world snapshot %lu B in %lu chunks crossed at 5%% loss in %lu ms (%ld "
           "datagrams destroyed, %ld piece retransmits); the clean 512 KiB reference took %lu ms\n",
           (unsigned long)g_blob_len, (unsigned long)rc_lossy.chunks, (unsigned long)rc_lossy.ms,
           rc_lossy.lost, rc_lossy.retx, (unsigned long)rc_clean.ms);
    printf("=== udpsnaptest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
