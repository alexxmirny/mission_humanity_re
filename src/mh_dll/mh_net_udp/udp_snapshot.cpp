//
// udp_snapshot.cpp -- the snapshot pipeline's two objects (tracker mp:X1). The WHY is in
// udp_snapshot.h; this file is the arithmetic.
//
#include "udp_snapshot.h"

#include <string.h>

namespace mh {
namespace netudp {
namespace snapshot {

namespace {

// Explicit little-endian, as everything in mh_net_proto is: a packed struct would be a different
// format under a different compiler, and this one is written by the host and read by the joiner.
inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

constexpr uint32_t OFF_FORMAT    = 8u;
constexpr uint32_t OFF_BODY_LEN  = 12u;
constexpr uint32_t OFF_CHUNKS    = 16u;
constexpr uint32_t OFF_MAN_CHNKS = 20u;
constexpr uint32_t OFF_BODY_SHA  = 24u;
constexpr uint32_t OFF_ROOT      = 56u;
// Where the per-chunk hash vector starts: immediately past the fixed header.
constexpr uint32_t OFF_VECTOR = HDR_BYTES;

uint32_t chunks_of(uint32_t len) {
    return (len + bulk::CHUNK_BYTES - 1u) / bulk::CHUNK_BYTES;
}

// THE ROOT IS A HASH OF THE MANIFEST WITH ITS OWN ROOT FIELD ZEROED. That is the whole rule, and it
// is stated as a rule because the alternatives are all worse here: net_crypto's `sha256` is
// one-shot, so hashing "everything except bytes 56..87" would mean either stitching two ranges into
// a 64 KB scratch copy (an allocation this module does not make) or moving the root field to the end
// of the manifest (which would make the header un-readable until the whole vector had arrived).
// Zero-then-restore hashes the real buffer in place, in one call, and a reader re-derives it by
// doing exactly the same thing -- no range arithmetic to get wrong on either side.
//
// It mutates `man` for the duration and puts it back. Single-threaded by construction: the endpoint
// holds m_conn_cs across every call into this layer.
void root_over(uint8_t *man, uint32_t man_len, uint8_t out[mh_net_proto::SHA256_LEN]) {
    uint8_t save[mh_net_proto::SHA256_LEN];
    memcpy(save, man + OFF_ROOT, mh_net_proto::SHA256_LEN);
    memset(man + OFF_ROOT, 0, mh_net_proto::SHA256_LEN);
    mh_net_proto::sha256(man, man_len, out);
    memcpy(man + OFF_ROOT, save, mh_net_proto::SHA256_LEN);
}

} // namespace

void hex32(const uint8_t *h, char *out) {
    static const char D[] = "0123456789abcdef";
    for (int i = 0; i < (int)mh_net_proto::SHA256_LEN; ++i) {
        out[i * 2]     = D[(h[i] >> 4) & 0xf];
        out[i * 2 + 1] = D[h[i] & 0xf];
    }
    out[mh_net_proto::SHA256_LEN * 2] = '\0';
}

// =================================================================================================
// THE SENDER
// =================================================================================================
Sender::Sender() {
    reset();
}

void Sender::reset() {
    m_armed       = false;
    m_body        = nullptr;
    m_body_len    = 0;
    m_body_chunks = 0;
    m_man_chunks  = 0;
    m_man_len     = 0;
    memset(m_root, 0, sizeof(m_root));
}

int Sender::begin(const void *blob, uint32_t len) {
    reset();
    if (blob == nullptr || len == 0) return ERR_ARG;
    if (len > MAX_BODY_BYTES) return ERR_ARG;

    m_body        = (const uint8_t *)blob;
    m_body_len    = len;
    m_body_chunks = chunks_of(len);
    m_man_len     = HDR_BYTES + 32u * m_body_chunks;
    m_man_chunks  = chunks_of(m_man_len);

    // The padding is zeroed, not left as whatever the last transfer left behind: the manifest chunk
    // is HASHED BY CHANNEL C, so uninitialised tail bytes would make two captures of the same world
    // produce two different chunk hashes, and a resumed transfer would refuse its own manifest.
    memset(m_man, 0, m_man_chunks * bulk::CHUNK_BYTES);
    memcpy(m_man, MAGIC, sizeof(MAGIC));
    put_u32(m_man + OFF_FORMAT, FORMAT);
    put_u32(m_man + OFF_BODY_LEN, m_body_len);
    put_u32(m_man + OFF_CHUNKS, m_body_chunks);
    put_u32(m_man + OFF_MAN_CHNKS, m_man_chunks);
    mh_net_proto::sha256(m_body, m_body_len, m_man + OFF_BODY_SHA);
    for (uint32_t i = 0; i < m_body_chunks; ++i) {
        const uint32_t off = i * bulk::CHUNK_BYTES;
        const uint32_t n   = (m_body_len - off > bulk::CHUNK_BYTES) ? bulk::CHUNK_BYTES : m_body_len - off;
        mh_net_proto::sha256(m_body + off, n, m_man + OFF_VECTOR + 32u * i);
    }
    root_over(m_man, m_man_len, m_root);
    memcpy(m_man + OFF_ROOT, m_root, sizeof(m_root));
    m_armed = true;
    return OK;
}

// The composed image, one chunk's worth at a time. `off` is always a chunk boundary and `len` the
// chunk's length -- channel C loads whole chunks -- but the code does not depend on that, because a
// source that is only correct for the caller's current loop is a source that breaks when the loop
// changes.
void Sender::fill(uint32_t off, uint8_t *out, uint32_t len) const {
    const uint32_t man_bytes = m_man_chunks * bulk::CHUNK_BYTES;
    uint32_t       done      = 0;
    while (done < len) {
        const uint32_t at = off + done;
        if (at < man_bytes) {
            const uint32_t take = (man_bytes - at < len - done) ? man_bytes - at : len - done;
            memcpy(out + done, m_man + at, take);
            done += take;
            continue;
        }
        const uint32_t boff = at - man_bytes;
        if (boff >= m_body_len) {
            // Past the image. Not reachable through `image_len()`, and zero-filled rather than left
            // as stack litter so a future caller that overshoots gets a deterministic wrong answer
            // instead of an undeterministic one.
            memset(out + done, 0, len - done);
            return;
        }
        const uint32_t take = (m_body_len - boff < len - done) ? m_body_len - boff : len - done;
        memcpy(out + done, m_body + boff, take);
        done += take;
    }
}

void Sender::source(void *ctx, uint32_t off, uint8_t *out, uint32_t len) {
    Sender *s = (Sender *)ctx;
    if (s == nullptr || !s->m_armed) {
        memset(out, 0, len);
        return;
    }
    s->fill(off, out, len);
}

// =================================================================================================
// THE RECEIVER
// =================================================================================================
Receiver::Receiver() {
    reset(nullptr, 0);
}

void Receiver::reset(void *body_buf, uint32_t cap) {
    m_body      = (uint8_t *)body_buf;
    m_body_cap  = cap;
    m_man_ready = false;
    m_complete  = false;
    // ONE, not zero, and this is the only place the manifest's length is guessed. The receiver must
    // be able to classify chunk 0 before it knows how many manifest chunks there are, and chunk 0 is
    // a manifest chunk in every legal image -- so the assumption is "at least one", corrected the
    // moment chunk 0 is parsed. Guessing zero instead would classify chunk 0 as body chunk 0 and
    // refuse it with ERR_NO_MANIFEST, which is a deadlock rather than a diagnosis.
    m_man_chunks  = 1;
    m_man_have    = 0;
    m_body_len    = 0;
    m_body_chunks = 0;
    m_prefix      = 0;
    memset(m_root, 0, sizeof(m_root));
    memset(m_body_sha, 0, sizeof(m_body_sha));
    memset(m_held, 0, sizeof(m_held));
    // The manifest buffer is cleared too, and the argument for NOT clearing it is exactly why it
    // is: `parse_manifest` only ever hashes `[0, HDR_BYTES + 32*chunk_count)`, and the shape check
    // ahead of it proves that range lies inside the chunks actually received -- so the stale bytes
    // beyond are unreachable, by an argument two checks deep in another function. 80 KB of memset
    // per transfer buys not having to re-make that argument after the next edit to either.
    memset(m_man, 0, sizeof(m_man));
    memset(&m_s, 0, sizeof(m_s));
}

uint32_t Receiver::resume_chunk() const {
    if (!m_man_ready) return m_man_have;
    return m_man_chunks + m_prefix;
}

int Receiver::parse_manifest() {
    if (memcmp(m_man, MAGIC, sizeof(MAGIC)) != 0) return ERR_MAGIC;
    if (get_u32(m_man + OFF_FORMAT) != FORMAT) return ERR_FORMAT;
    const uint32_t body_len = get_u32(m_man + OFF_BODY_LEN);
    const uint32_t chunks   = get_u32(m_man + OFF_CHUNKS);
    const uint32_t manch    = get_u32(m_man + OFF_MAN_CHNKS);
    if (body_len == 0 || body_len > MAX_BODY_BYTES) return ERR_SHAPE;
    if (chunks == 0 || chunks > MAX_BODY_CHUNKS) return ERR_SHAPE;
    // The three fields are REDUNDANT with each other by construction, so disagreement is the cheap
    // structural check that catches a manifest assembled from two different transfers before any
    // hash work is done.
    if (chunks != chunks_of(body_len)) return ERR_SHAPE;
    const uint32_t man_len = HDR_BYTES + 32u * chunks;
    if (manch != chunks_of(man_len) || manch == 0 || manch > MANIFEST_CHUNKS_MAX) return ERR_SHAPE;
    if (manch != m_man_chunks) return ERR_SHAPE; // it changed under us mid-manifest

    uint8_t want[mh_net_proto::SHA256_LEN];
    root_over(m_man, man_len, want);
    if (memcmp(want, m_man + OFF_ROOT, mh_net_proto::SHA256_LEN) != 0) return ERR_ROOT;

    // Capacity is checked HERE, before a body byte exists, for the reason in the header: a receiver
    // that runs out of room halfway through has already spent the transfer.
    if (m_body == nullptr || body_len > m_body_cap) return ERR_CAPACITY;

    m_body_len    = body_len;
    m_body_chunks = chunks;
    memcpy(m_body_sha, m_man + OFF_BODY_SHA, sizeof(m_body_sha));
    memcpy(m_root, m_man + OFF_ROOT, sizeof(m_root));
    m_man_ready = true;
    return OK;
}

int Receiver::on_chunk(uint32_t chunk_id, const void *data, uint32_t len) {
    if (data == nullptr || len == 0 || len > bulk::CHUNK_BYTES) {
        ++m_s.chunks_refused_other;
        return ERR_ARG;
    }
    const uint8_t *p = (const uint8_t *)data;

    // ---- a manifest chunk ------------------------------------------------------------------------
    if (chunk_id < m_man_chunks) {
        if (chunk_id < m_man_have) {
            ++m_s.chunks_dup;
            return OK;
        }
        if (chunk_id != m_man_have) {
            // Out of order inside the manifest. Channel C admits in order, so this is not a network
            // condition -- it is a caller feeding chunks from somewhere else, and merging them would
            // leave a hole that the root check would blame on corruption.
            ++m_s.chunks_refused_other;
            return ERR_SHAPE;
        }
        memcpy(m_man + chunk_id * bulk::CHUNK_BYTES, p, len);
        if (len < bulk::CHUNK_BYTES)
            memset(m_man + chunk_id * bulk::CHUNK_BYTES + len, 0, bulk::CHUNK_BYTES - len);
        ++m_man_have;
        ++m_s.chunks_manifest;
        if (chunk_id == 0) {
            // Learn the real manifest length from the header the first chunk carries. Refuse an
            // absurd one now rather than reading 2048 chunks of it.
            const uint32_t manch = get_u32(m_man + OFF_MAN_CHNKS);
            if (manch == 0 || manch > MANIFEST_CHUNKS_MAX) {
                ++m_s.manifest_refused;
                return ERR_SHAPE;
            }
            m_man_chunks = manch;
        }
        if (m_man_have == m_man_chunks) {
            const int rc = parse_manifest();
            if (rc != OK) {
                ++m_s.manifest_refused;
                m_man_have = 0; // refuse the whole manifest, do not half-hold it
                return rc;
            }
            m_s.body_chunks    = m_body_chunks;
            m_s.manifest_ready = true;
        }
        return OK;
    }

    // ---- a body chunk ----------------------------------------------------------------------------
    if (!m_man_ready) {
        ++m_s.chunks_refused_other;
        return ERR_NO_MANIFEST;
    }
    const uint32_t i = chunk_id - m_man_chunks;
    if (i >= m_body_chunks) {
        ++m_s.chunks_refused_other;
        return ERR_SHAPE;
    }
    const uint32_t off  = i * bulk::CHUNK_BYTES;
    const uint32_t want = (m_body_len - off > bulk::CHUNK_BYTES) ? bulk::CHUNK_BYTES : m_body_len - off;
    if (len != want) {
        // The length the manifest implies for THIS index. Channel C already refuses a piece whose
        // length disagrees with its index inside a chunk; this is the same check one level up, and
        // it is the check that makes the memcpy below provably in bounds.
        ++m_s.chunks_refused_other;
        return ERR_SHAPE;
    }
    if (held(i)) {
        ++m_s.chunks_dup;
        return OK;
    }

    // THE PROMISE MADE BEFORE THE CHUNK WAS SENT. Channel C already proved these bytes are the bytes
    // the SENDER hashed; this proves they are the bytes the sender COMMITTED TO in the manifest --
    // which is a different claim, and the only one that survives a sender whose blob moved.
    uint8_t h[mh_net_proto::SHA256_LEN];
    mh_net_proto::sha256(p, len, h);
    if (memcmp(h, m_man + OFF_VECTOR + 32u * i, mh_net_proto::SHA256_LEN) != 0) {
        ++m_s.chunks_hash_refused;
        return ERR_CHUNK_HASH; // the caller rewinds the frontier to chunk_id and it is re-sent
    }

    memcpy(m_body + off, p, len);
    set_held(i);
    ++m_s.chunks_ok;
    while (m_prefix < m_body_chunks && held(m_prefix)) ++m_prefix;
    m_s.verified_prefix = m_prefix;

    if (m_prefix == m_body_chunks && !m_complete) {
        // THE ALL-OR-NOTHING GATE OPENS HERE AND NOWHERE ELSE, and it asks the one question the
        // per-chunk checks structurally cannot: were the chunks put back at the right offsets? A
        // reassembly bug satisfies every chunk hash and fails this.
        uint8_t bh[mh_net_proto::SHA256_LEN];
        mh_net_proto::sha256(m_body, m_body_len, bh);
        m_complete   = (memcmp(bh, m_body_sha, mh_net_proto::SHA256_LEN) == 0);
        m_s.complete = m_complete;
    }
    return OK;
}

int Receiver::finish_status() const {
    if (!m_man_ready) return ERR_NO_MANIFEST;
    if (m_prefix != m_body_chunks) return ERR_INCOMPLETE;
    if (!m_complete) return ERR_BODY_HASH;
    return OK;
}

void Receiver::stats(Stats &out) const {
    out                 = m_s;
    out.verified_prefix = m_prefix;
    out.body_chunks     = m_body_chunks;
    out.manifest_ready  = m_man_ready;
    out.complete        = m_complete;
}

} // namespace snapshot
} // namespace netudp
} // namespace mh
