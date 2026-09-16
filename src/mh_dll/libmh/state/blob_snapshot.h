//
// state/blob_snapshot.h -- the shared REGION-BLOB engine: capture live regions into a
// self-describing blob, and write one back through live_base() (LIB-BOOT, LIB-WORLD).
//
// WHY THIS FILE EXISTS. LIB-BOOT built one of these -- a rid-addressed block table, a schema
// fingerprint that refuses a stale blob, a canonical base-independent content hash, and an import
// that validates the WHOLE payload before writing a single byte. LIB-WORLD needs the same machine
// over a different block table (the bound registry at replay step 0 rather than the cfg cluster's
// output at boot stage 9). Copying it would be two copies of load-bearing REFUSAL machinery, which
// is the shape that rots asymmetrically: the copy that is exercised stays right and the copy that
// is not quietly stops matching it. So the engine is here, once, and each item supplies a POLICY.
//
// WHAT A POLICY OWES (see boot_snapshot.cpp / world_snapshot.cpp for the two live ones):
//
//   static constexpr uint8_t  MAGIC[8];        distinct per format -- a boot blob must not import
//   static constexpr uint32_t FORMAT;          as a world blob even if every block happened to line up
//   static int      count();                   how many blocks
//   static const snapshot_block &at(int);      the block table, in a FIXED order
//   static uint32_t canonical_len(const snapshot_block &);   the block's length IN THE STREAM,
//                                              which is not always its region extent (LIB-BOOT's
//                                              TLO registry is carried in a derived, name-inlined
//                                              form because its live bytes are image pointers)
//   template <class S> static void emit(const snapshot_block &, S &, capture_stats *);
//   static void     apply(const snapshot_block &, const uint8_t *src, uint32_t len);
//   static bool     unbound_ok(const snapshot_block &);  a block with no live_base is normally a
//                                              hard refusal; a policy says so when one is legal
//   static bool     refuse_import();           the ordering latch -> ERR_SESSION_BEGUN
//   static int      post_capture(const capture_stats &);  a policy's own capture-time assertion
//
// THE ONE RULE THE ENGINE ENFORCES AND NEITHER POLICY MAY RELAX: emit() is the ONLY place that
// decides what a block's bytes are, and BOTH the blob writer and the canonical hash go through it.
// A second encoding of that decision is the failure state_sink.h's banner is about -- the blob and
// the hash drift and the oracle stops watching part of the state without saying so.
//
// THE HEADER IS SPLIT, NOT SHARED WHOLE. `common_header` is the prefix every blob carries and the
// engine reads; each item's own header embeds it as its FIRST member and appends its own fields
// (LIB-BOOT: the text-pointer tallies + the capture's restore self-check. LIB-WORLD: the step-0
// lockstep hash pair, the hash-manifest fingerprint and the mask flags it was taken under). Nesting
// preserves the byte layout exactly -- the fields are in the same order at the same offsets -- so
// this split did not move a single blob byte, which is what let the refactor be proven rather than
// argued (the LIB-BOOT control blob's recorded hash reproduces byte-exactly across it).
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "addr/mh_regions.gen.h"
#include "state/state_sink.h"

namespace mh::state::blob {

// One carried region. Addressed BY RID, never by address: the capture reads and the importer writes
// through live_base(), which is the stock .bss hosted and the host's own allocation standalone.
struct snapshot_block {
    region_id   rid;
    uint32_t    len;  // the region's REACH -- what a host binds (host_bind.cpp note 2)
    const char *name; // the Ghidra symbol, for the capture/import log and the arms
};

// The prefix of every blob. An item's header embeds this as its first member.
struct common_header {
    uint8_t  magic[8];
    uint32_t format;
    uint32_t schema;      // fingerprint of the generated block table -- refuses a stale blob
    uint32_t block_count; // == the policy's count() at capture time
    uint32_t payload_len; // bytes after the FULL item header
    uint64_t content_hash;
};
static_assert(sizeof(common_header) == 32, "the blob prefix layout is a file format");
static_assert(offsetof(common_header, content_hash) == 24, "ditto");

// What a policy's emit() may tally as it walks. Two counters, deliberately generic: LIB-BOOT uses
// them for the G_TEXT_PTRS normalisation (how many entries landed inside the arena and how many did
// not), LIB-WORLD for its pointer census. Both are checks of a DECLARATION that only runtime can
// test, which is why they are counted rather than asserted at build time.
struct capture_stats {
    uint32_t ptrs_in_arena  = 0;
    uint32_t ptrs_out_arena = 0;
};

enum err : int {
    OK                = 0,
    ERR_ARG           = -1, // null pointer, or a buffer too small to hold the header
    ERR_MAGIC         = -2, // not this kind of blob, or a format this build does not know
    ERR_SCHEMA        = -3, // captured against a different block table
    ERR_SESSION_BEGUN = -4, // the policy's ordering latch refuses a late import
    ERR_TRUNCATED     = -5, // the payload ends inside a block, or a block length disagrees
    ERR_UNKNOWN_RID   = -6, // a block names a region this build does not have
    ERR_UNBOUND       = -7, // a carried region has no base -- the host never bound it
    ERR_POLICY        = -8, // the policy's own post_capture() assertion failed
};

namespace detail {

struct buf_sink final : state_sink {
    buf_sink(uint8_t *d, size_t cap) : state_sink(sink_mode::PERSIST), dst(d), cap(cap) {}
    uint8_t *dst;
    size_t   cap, n = 0;
    bool     overflow = false;
    void     raw(const void *p, uint32_t k) override {
        if (n + k > cap) {
            overflow = true;
            return;
        }
        std::memcpy(dst + n, p, k);
        n += k;
    }
};

} // namespace detail

// ---- the schema fingerprint ------------------------------------------------------------------
//
// (rid, canonical length, NAME) of every block, in order, plus the format and the count. The NAME is
// in it so a registry renumbering that happens to preserve every id still trips it: an id is a
// position, a name is what the position meant.
template <class P>
uint32_t schema_fingerprint() {
    uint32_t h   = 2166136261u;
    auto     mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<uint8_t>(v >> (i * 8));
            h *= 16777619u;
        }
    };
    mix(P::FORMAT);
    mix(static_cast<uint32_t>(P::count()));
    for (int i = 0; i < P::count(); ++i) {
        const snapshot_block &b = P::at(i);
        mix(static_cast<uint32_t>(b.rid));
        mix(P::canonical_len(b));
        for (const char *c = b.name; *c; ++c) {
            h ^= static_cast<uint8_t>(*c);
            h *= 16777619u;
        }
    }
    return h;
}

// The exact number of bytes capture<P, HDR>() will write. A caller sizes its buffer with this.
template <class P, class HDR>
size_t blob_size() {
    size_t n = sizeof(HDR);
    for (int i = 0; i < P::count(); ++i) n += 8u + P::canonical_len(P::at(i)); // rid + len + payload
    return n;
}

// The CANONICAL content hash, read out of live memory through the policy's emit(). Base-independent
// wherever the policy normalises (LIB-BOOT's pointer tagging), which is what makes an in-binary
// capture comparable with an import into a host's own arena.
template <class P>
uint64_t canonical_hash() {
    hash_sink s(sink_mode::PERSIST);
    for (int i = 0; i < P::count(); ++i) {
        const snapshot_block &b = P::at(i);
        // The rid and the length are IN the stream. Without them two different tables carrying the
        // same bytes in a different split would hash the same, and the hash would stop being a
        // statement about the schema it was taken under.
        const uint32_t head[2] = {static_cast<uint32_t>(b.rid), P::canonical_len(b)};
        s.bytes(head, sizeof(head));
        P::emit(b, s, nullptr);
    }
    return s.finish();
}

// ---- capture ---------------------------------------------------------------------------------
//
// `hdr` is the caller's fully-typed header: the engine fills its `base` prefix and leaves every
// item-specific field to the caller, which is also handed the capture stats so it can record them.
template <class P, class HDR>
int capture(void *buf, size_t cap, size_t *out_len, HDR *hdr, capture_stats *stats_out) {
    if (buf == nullptr || out_len == nullptr || hdr == nullptr) return ERR_ARG;
    const size_t need = blob_size<P, HDR>();
    if (cap < need) return ERR_ARG;

    uint8_t      *d = static_cast<uint8_t *>(buf);
    capture_stats stats;

    detail::buf_sink s(d + sizeof(HDR), cap - sizeof(HDR));
    for (int i = 0; i < P::count(); ++i) {
        const snapshot_block &b = P::at(i);
        if (live_base(b.rid) == 0 && !P::unbound_ok(b)) return ERR_UNBOUND;
        const uint32_t head[2] = {static_cast<uint32_t>(b.rid), P::canonical_len(b)};
        s.raw(head, sizeof(head));
        P::emit(b, s, &stats);
    }
    if (s.overflow) return ERR_ARG;

    // The policy's own capture-time assertion, run BEFORE anything is stamped: LIB-BOOT refuses a
    // capture in which nothing pointed into the arena it carries on a `pointed-into` claim, because
    // that claim has then stopped being true and this capture is not the snapshot the accounting
    // describes.
    const int prc = P::post_capture(stats);
    if (prc != OK) return prc;

    std::memcpy(hdr->base.magic, P::MAGIC, sizeof(hdr->base.magic));
    hdr->base.format       = P::FORMAT;
    hdr->base.schema       = schema_fingerprint<P>();
    hdr->base.block_count  = static_cast<uint32_t>(P::count());
    hdr->base.payload_len  = static_cast<uint32_t>(s.n);
    hdr->base.content_hash = canonical_hash<P>();
    std::memcpy(d, hdr, sizeof(HDR));
    if (stats_out) *stats_out = stats;
    *out_len = sizeof(HDR) + s.n;
    return OK;
}

// ---- import ----------------------------------------------------------------------------------
//
// Validation is COMPLETE before the first byte is written. A half-applied import is worse than a
// rejected one: the world would be part carried values and part poison and nothing downstream could
// tell. Same rule host_bind.cpp's bind_all follows, for the same reason.
template <class P, class HDR>
int import(const void *blob, size_t n) {
    if (blob == nullptr || n < sizeof(HDR)) return ERR_ARG;
    const uint8_t *d = static_cast<const uint8_t *>(blob);
    common_header  h;
    std::memcpy(&h, d, sizeof(h));

    if (std::memcmp(h.magic, P::MAGIC, sizeof(h.magic)) != 0 || h.format != P::FORMAT)
        return ERR_MAGIC;
    if (h.schema != schema_fingerprint<P>()) return ERR_SCHEMA;
    if (h.block_count != static_cast<uint32_t>(P::count())) return ERR_SCHEMA;
    if (P::refuse_import()) return ERR_SESSION_BEGUN;
    if (static_cast<size_t>(h.payload_len) + sizeof(HDR) > n) return ERR_TRUNCATED;

    {
        size_t off = 0;
        for (int i = 0; i < P::count(); ++i) {
            const snapshot_block &b = P::at(i);
            if (off + 8u > h.payload_len) return ERR_TRUNCATED;
            uint32_t rid = 0, len = 0;
            std::memcpy(&rid, d + sizeof(HDR) + off, 4);
            std::memcpy(&len, d + sizeof(HDR) + off + 4, 4);
            if (rid >= static_cast<uint32_t>(RID_COUNT)) return ERR_UNKNOWN_RID;
            if (rid != static_cast<uint32_t>(b.rid) || len != P::canonical_len(b)) return ERR_SCHEMA;
            if (off + 8u + len > h.payload_len) return ERR_TRUNCATED;
            if (!P::unbound_ok(b)) {
                if (live_base(b.rid) == 0) return ERR_UNBOUND;
                if (live_size(b.rid) < b.len) return ERR_UNBOUND;
            }
            off += 8u + len;
        }
        if (off != h.payload_len) return ERR_TRUNCATED;
    }

    size_t off = 0;
    for (int i = 0; i < P::count(); ++i) {
        const snapshot_block &b   = P::at(i);
        const uint32_t        len = P::canonical_len(b);
        P::apply(b, d + sizeof(HDR) + off + 8u, len);
        off += 8u + len;
    }
    return OK;
}

} // namespace mh::state::blob
