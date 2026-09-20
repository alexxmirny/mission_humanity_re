#pragma once
//
// udp_snapshot.h -- THE CHUNKED SNAPSHOT PIPELINE (tracker mp:X1, plan D7).
//
// WHAT THIS IS. mp:T2 built channel C: a sequence of 16 KiB chunks crossing reliably, each verified
// by the SHA-256 the pieces themselves carried, resumable by chunk index, rate-limited so a
// lockstep datagram never queues behind a burst of bulk. What it deliberately does NOT have is an
// opinion about what the chunks MEAN -- its own header says so: "the BLOB's length and chunk count
// are the application's, not the wire's".
//
// This file is that application layer, and it is ONE pipeline with three consumers named in plan
// D7: join-in-progress, desync recovery, and MP save/load. The body it moves is
// `mh::state::world::capture()`'s blob -- 829 bound regions, 7,711,960 bytes of carried world plus
// its header, the RNG channels among them (mp:R-rng-snapshot: rng_state 16 B, the norm divisor
// 8 B, the seed byte) -- and mp:X2 will move a map file through the same three objects.
//
// ---- WHY THE MANIFEST EXISTS AT ALL, GIVEN CHANNEL C ALREADY HASHES EVERY CHUNK -----------------
//
// Channel C's per-chunk SHA-256 answers "did these bytes survive the network", and it answers it
// completely. It cannot answer the two questions this layer is for, because the hash it checks
// against arrived in the same frames as the data:
//
//   1. IS THIS THE RIGHT CHUNK? A sender whose build is wrong -- or whose blob moved under it
//      mid-transfer -- puts self-consistent garbage on the wire: correct pieces, correct SHA, wrong
//      content. T2's own header states the threat in the neighbouring case ("authenticated is not
//      the same as correct: the peer holding the session key is the peer whose build might be
//      wrong"). The manifest is the hash vector committed to ONCE, up front, under a root -- so
//      every later chunk is checked against a promise made before it was sent.
//   2. IS THIS THE WHOLE THING? `world::import()` validates completely before writing a byte, so a
//      half-transferred blob cannot half-import. But it would happily import a blob that is
//      structurally valid and 40% stale. Completeness is not a property any single chunk has.
//
// ---- THE IMAGE LAYOUT, AND WHY THE MANIFEST IS PADDED TO WHOLE CHUNKS ---------------------------
//
//   transfer chunk 0 .. manifest_chunks-1   the MANIFEST, zero-padded to a chunk boundary
//   transfer chunk manifest_chunks + i      body chunk i, whose SHA-256 must equal chunk_sha[i]
//
// The padding is the whole trick and it is worth one sentence: without it the body's chunk
// boundaries would be offset by the manifest's odd length, so `chunk_sha[i]` would describe bytes
// that no single transfer chunk contains, and a per-chunk check would be impossible -- the receiver
// would have to buffer the entire body before it could verify any of it, which is exactly the
// property "resumable by chunk index" is supposed to buy. Padding costs at most 16 KiB once.
//
// The manifest's own length is self-describing (`chunk_count` is in the first 24 bytes), so the
// receiver learns `manifest_chunks` from chunk 0 and never has to be told out of band.
//
// ---- THE ROOT HASH, AND WHAT IT IS AND IS NOT ---------------------------------------------------
//
// `root = SHA-256(the whole manifest with the 32-byte root field itself zeroed)` -- so it commits to
// magic, format, body_len, chunk_count, manifest_chunks, body_sha and the entire chunk-hash vector.
// It is a flat commitment, not a Merkle tree. A tree buys O(log n) proofs
// for a receiver that wants to verify one chunk without holding the others; this receiver holds the
// manifest and wants all of them, so the tree would be structure with no consumer. What the root
// does buy is a SINGLE 32-byte number that names a snapshot -- which is what `SESSION_INFO` will
// advertise for mp:X2, and what a resumed transfer compares to know it is resuming the same artefact
// rather than a newer one.
//
// `body_sha` is carried as well, and it is not redundant with the chunk vector: it is the ONE check
// that does not depend on this file's own chunking arithmetic being right. A bug that assembled the
// chunks at the wrong offsets would satisfy every per-chunk comparison and fail this one.
//
// ---- RE-REQUEST AND RESUME ARE THE SAME PRIMITIVE ------------------------------------------------
//
// There is no NAK frame and no resume message. T2 made the RECEIVER'S FRONTIER the only thing that
// steers the transfer, in both directions, so this layer drives both clauses through
// `Channel::rx_resume_at()`:
//
//   a chunk whose bytes disagree with the manifest  -> rewind the frontier to it -> the sender's
//   `tx_seek` backward path re-sends it, and the counter `chunks_hash_refused` says it happened.
//
//   a transfer truncated by a dead link             -> the receiver keeps its verified prefix, and
//   on reconnect the application restores the frontier to `resume_chunk()`; the first acknowledgement
//   carries it and the sender skips what is already held.
//
// ---- WHAT THIS FILE DOES NOT DO ------------------------------------------------------------------
//
// IT DOES NOT IMPORT. `world::import()` lives in libmh, inside mh.dll's address space, and this
// module must not reach it -- the module is absent-tolerant by design (a missing transport DLL is a
// refused match, not a failed process load) and libmh's outbound edge is measured and ruled
// (tools/check_libmh_outbound.py). So the pipeline's contract stops at `complete()`: the receiver
// says "this is the blob, whole and as the sender committed to it", and the APPLICATION imports. The
// all-or-nothing clause is therefore enforced twice, in two layers, on purpose -- `complete()` is
// false while anything is missing, and `world::import()` validates the whole blob before its first
// write. Neither is the other's backup; they refuse different things.
//
// IT ALLOCATES NOTHING. The sender borrows the caller's blob and composes the image through channel
// C's pull edge (`source_fn`); the receiver writes into a destination buffer the caller owns. The
// only storage here is the manifest itself and the held-chunk bitmap, both fixed-size.
//
#ifndef MH_NET_UDP_SNAPSHOT_H
#define MH_NET_UDP_SNAPSHOT_H

#include <stdint.h>

#include "mh_net_proto/net_crypto.h" // SHA256_LEN / sha256
#include "udp_channel_c.h"           // CHUNK_BYTES, source_fn -- the transport underneath

namespace mh {
namespace netudp {
namespace snapshot {

// "MHSNAP" + version. Distinct from the world blob's own MHWRLD: this magic names the TRANSFER
// envelope, and a world blob handed to the wrong reader must not look like a manifest.
inline constexpr uint8_t  MAGIC[8] = {'M', 'H', 'S', 'N', 'A', 'P', 0, 1};
inline constexpr uint32_t FORMAT   = 1u;

// magic(8) | format(4) | body_len(4) | chunk_count(4) | manifest_chunks(4) | body_sha(32) | root(32)
inline constexpr uint32_t HDR_BYTES = 8u + 4u + 4u + 4u + 4u + 32u + 32u; // 88

// THE CAP, and where the number comes from rather than a round figure: the world blob is 829 blocks
// and ~8.19 MB = 500 body chunks today, and the two consumers that will grow it are a wider region
// manifest and mp:X2's map files (115-300 KB, far smaller). 2048 chunks is 32 MiB -- four times the
// current blob -- and it is what fixes the two arrays below at 8 KiB (the bitmap) and 80 KiB (the
// manifest buffer) rather than leaving them to a runtime allocation this module does not make.
inline constexpr uint32_t MAX_BODY_CHUNKS = 2048u;
inline constexpr uint32_t MAX_BODY_BYTES  = MAX_BODY_CHUNKS * bulk::CHUNK_BYTES; // 33,554,432
// 88 + 32*2048 = 65,624 -> 5 chunks. Rounded UP to whole chunks because that is what is transferred.
inline constexpr uint32_t MANIFEST_CHUNKS_MAX =
    (HDR_BYTES + 32u * MAX_BODY_CHUNKS + bulk::CHUNK_BYTES - 1u) / bulk::CHUNK_BYTES;
inline constexpr uint32_t MANIFEST_BYTES_MAX = MANIFEST_CHUNKS_MAX * bulk::CHUNK_BYTES;

// Refusals. Negative, and distinct per reason, because "the transfer failed" is the one diagnosis
// that helps nobody -- a stale root and a corrupt chunk call for opposite responses.
enum err : int {
    OK              = 0,
    ERR_ARG         = -1, // a null buffer, a zero length, or a blob past MAX_BODY_BYTES
    ERR_MAGIC       = -2, // the manifest's first eight bytes are not this envelope
    ERR_FORMAT      = -3, // a manifest this build does not read
    ERR_ROOT        = -4, // the manifest does not hash to the root it carries: tampered or corrupt
    ERR_SHAPE       = -5, // chunk_count / manifest_chunks / body_len do not agree with each other
    ERR_CAPACITY    = -6, // the body does not fit the destination the caller supplied
    ERR_CHUNK_HASH  = -7, // a body chunk's bytes are not what the manifest committed to
    ERR_NO_MANIFEST = -8, // a body chunk arrived before the manifest was complete
    ERR_BODY_HASH   = -9, // every chunk verified and the assembled body still does not hash right
    ERR_INCOMPLETE  = -10 // asked for the blob while a chunk is missing: the all-or-nothing refusal
};

// =================================================================================================
// THE SENDER
//
// Builds the manifest over a blob the caller owns and keeps owning. The image is never materialised:
// `source()` composes it on demand, one chunk at a time, which is what keeps an 8.19 MB snapshot
// from being copied to prepend 16 KB of hashes.
// =================================================================================================
class Sender {
public:
    Sender();

    // Hash the blob into a manifest. `blob` must stay alive and UNCHANGED until the transfer ends --
    // the hashes are taken now and the bytes are read later, so a blob that moves underneath turns
    // into `ERR_CHUNK_HASH` at the receiver, which is the correct diagnosis of exactly that bug.
    int  begin(const void *blob, uint32_t len);
    void reset();

    bool     armed() const { return m_armed; }
    uint32_t body_len() const { return m_body_len; }
    uint32_t body_chunks() const { return m_body_chunks; }
    uint32_t manifest_chunks() const { return m_man_chunks; }
    // What `Channel::start_send_src` must be given as the transfer length: the padded manifest plus
    // the body, so the last transfer chunk is the body's short tail rather than a padded full one.
    uint32_t       image_len() const { return m_man_chunks * bulk::CHUNK_BYTES + m_body_len; }
    const uint8_t *root() const { return m_root; }
    const uint8_t *manifest() const { return m_man; }
    uint32_t       manifest_len() const { return m_man_len; }

    // The pull edge channel C reads through. `ctx` is the Sender.
    static void source(void *ctx, uint32_t off, uint8_t *out, uint32_t len);

private:
    void fill(uint32_t off, uint8_t *out, uint32_t len) const;

    bool           m_armed;
    const uint8_t *m_body;
    uint32_t       m_body_len;
    uint32_t       m_body_chunks;
    uint32_t       m_man_chunks;
    uint32_t       m_man_len; // the manifest's REAL length; the rest of the chunk is zero
    uint8_t        m_root[mh_net_proto::SHA256_LEN];
    uint8_t        m_man[MANIFEST_BYTES_MAX];
};

// =================================================================================================
// THE RECEIVER
//
// Fed one completed chunk at a time, exactly as `Endpoint::bulk_recv` pops them. Channel C admits
// chunks strictly in order (its window only ever admits `base`), so in practice the manifest arrives
// first and the body in sequence -- but nothing here assumes it: a body chunk before the manifest is
// refused with ERR_NO_MANIFEST rather than mis-parsed, and the held-chunk bitmap means a repeat is a
// duplicate rather than a corruption.
// =================================================================================================
class Receiver {
public:
    struct Stats {
        long     chunks_manifest;      // manifest chunks admitted
        long     chunks_ok;            // body chunks that matched their manifest hash
        long     chunks_hash_refused;  // ...and those that did not -- the re-request counter
        long     chunks_dup;           // a chunk index already held
        long     chunks_refused_other; // out of range, no manifest yet, past capacity
        long     manifest_refused;     // a manifest that failed magic / format / root / shape
        uint32_t verified_prefix;      // body chunks held contiguously from 0 -- the resume point
        uint32_t body_chunks;          // 0 until the manifest is in
        bool     manifest_ready;
        bool     complete;
    };

    Receiver();

    // Point the receiver at the caller's destination buffer and forget everything. `cap` must be at
    // least the body's length or the manifest is refused with ERR_CAPACITY -- refused at the
    // MANIFEST, before a single body byte is written, because a receiver that discovers it has no
    // room halfway through has already spent the transfer.
    void reset(void *body_buf, uint32_t cap);

    // One completed, wire-verified chunk. Returns OK, or a negative `err`. ERR_CHUNK_HASH is the
    // caller's cue to rewind the frontier to `chunk_id` (see the header) -- it is a refusal of THIS
    // delivery, not of the transfer.
    int on_chunk(uint32_t chunk_id, const void *data, uint32_t len);

    bool     manifest_ready() const { return m_man_ready; }
    bool     complete() const { return m_complete; }
    uint32_t body_len() const { return m_body_len; }
    uint32_t body_chunks() const { return m_body_chunks; }
    uint32_t manifest_chunks() const { return m_man_chunks; }
    uint32_t verified_prefix() const { return m_prefix; }
    // The TRANSFER chunk index to resume at: past the manifest once it is in, and the manifest's own
    // frontier while it is not. A restarted link restores this and the sender follows it.
    uint32_t       resume_chunk() const;
    const uint8_t *root() const { return m_root; }

    // The blob, or null while anything is missing. THE ALL-OR-NOTHING GATE: this is the only way out
    // of the receiver, and it does not open until every body chunk is held and the assembled bytes
    // hash to the `body_sha` the manifest committed to.
    const uint8_t *body() const { return m_complete ? m_body : nullptr; }
    // The same gate as a diagnosable call: OK, or ERR_INCOMPLETE / ERR_BODY_HASH.
    int  finish_status() const;
    void stats(Stats &out) const;

private:
    int  parse_manifest();
    bool held(uint32_t i) const { return (m_held[i >> 5] & (1u << (i & 31u))) != 0u; }
    void set_held(uint32_t i) { m_held[i >> 5] |= (1u << (i & 31u)); }

    uint8_t *m_body;
    uint32_t m_body_cap;

    bool     m_man_ready;
    bool     m_complete;
    uint32_t m_man_chunks; // 0 until chunk 0 says otherwise; 1 is the working assumption before that
    uint32_t m_man_have;   // manifest chunks received contiguously from 0
    uint32_t m_body_len;
    uint32_t m_body_chunks;
    uint32_t m_prefix;
    uint8_t  m_root[mh_net_proto::SHA256_LEN];
    uint8_t  m_body_sha[mh_net_proto::SHA256_LEN];
    uint32_t m_held[MAX_BODY_CHUNKS / 32u];
    uint8_t  m_man[MANIFEST_BYTES_MAX];
    Stats    m_s;
};

// Render 32 hash bytes as 64 lowercase hex plus a terminator. `out` must hold 65. Used by the log
// line and by the suites, so a root in a log and a root in a report are the same string.
void hex32(const uint8_t *h, char *out);

} // namespace snapshot
} // namespace netudp
} // namespace mh

#endif // MH_NET_UDP_SNAPSHOT_H
