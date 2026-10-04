//
// desync/state_record.h -- the PURE encoder of the state-recording file format (mp:D40, reused by the
// mp:D41 ring): docs/state-record.md version 1 is the authoritative layout; this header is one
// implementation of it and changes nothing there.
//
// PURE ON PURPOSE. No Windows, no game memory, no threads: the recorder (state_recorder.cpp) and the
// selftest (`net_selftest.exe staterectest`) drive the SAME functions, so the round-trip the selftest
// proves is the encoder the game runs. What is NOT here is everything that decides WHEN to call it
// (the per-step hub, the writer thread, the session boundaries).
//
// THE PIECES.
//   buf            a malloc-backed growable byte buffer that reports OOM instead of throwing.
//   crc32          zlib's CRC-32 (poly 0xEDB88320, init/final xor ~0), chainable.
//   put_header     the fixed header + region table.
//   step_builder   one STEP chunk, streamed: begin(step) -> run(...) per changed run -> end(). It
//                  COALESCES runs whose gap is <= RUN_GAP bytes (the tracker hands it EXACT runs so
//                  the dirty probe's byte counts stay exact; the file wants fewer run headers).
//   put_keyf       one KEYF chunk from the per-region images, optionally left UNSEALED (crc 0) so the
//                  writer thread can pay the 2.8 MB CRC instead of the sim thread (seal_chunk).
//   put_end        the END chunk.
//   keyframe_due   the cadence: the first recorded step, then every `every` steps after it.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mh::desync::srec {

inline constexpr uint32_t MAGIC     = 0x5253484Du; // "MHSR" as bytes
inline constexpr uint16_t VERSION   = 1;
inline constexpr uint16_t FLAG_RING = 1u << 0; // a D41 desync ring dump, not a whole match

inline constexpr uint32_t TAG_KEYF = 0x4659454Bu; // "KEYF"
inline constexpr uint32_t TAG_STEP = 0x50455453u; // "STEP"
inline constexpr uint32_t TAG_END  = 0x20444E45u; // "END "

inline constexpr uint32_t CHUNK_HDR = 12;      // u32 tag, u32 payload_len, u32 crc32
inline constexpr uint32_t RUN_HDR   = 8;       // u16 region, u32 offset, u16 len
inline constexpr uint32_t RUN_GAP   = 8;       // merge runs separated by <= 8 unchanged bytes
inline constexpr uint32_t RUN_MAX   = 0xffffu; // a run's u16 length

inline constexpr uint32_t DEFAULT_KEYFRAME_EVERY = 3000; // one minute at 50 steps/s

// ---- CRC-32 (zlib) -----------------------------------------------------------------------------
struct crc_table_t {
    uint32_t t[256];
    constexpr crc_table_t() : t() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
inline constexpr crc_table_t CRC_TABLE{};

// crc32(b, crc32(a)) == crc32(a ++ b), exactly like zlib.crc32(data, value).
inline uint32_t crc32(const void *p, size_t n, uint32_t crc = 0) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    uint32_t       c = ~crc;
    for (size_t i = 0; i < n; ++i) c = CRC_TABLE.t[(c ^ b[i]) & 0xffu] ^ (c >> 8);
    return ~c;
}

// ---- the byte buffer ---------------------------------------------------------------------------
// Plain struct, zero-initialisable, no destructor (the recorder hands `p` to another thread, which
// free()s it). Every write goes through grow(); after an allocation failure `oom` is set, every
// further write is a no-op, and the owner decides what that means (the recorder stops, loudly).
struct buf {
    uint8_t *p;
    size_t   len, cap;
    bool     oom;

    bool reserve(size_t want) {
        if (oom) return false;
        if (want <= cap) return true;
        size_t n = cap ? cap : 4096;
        while (n < want) n *= 2;
        void *q = p ? realloc(p, n) : malloc(n);
        if (!q) {
            oom = true;
            return false;
        }
        p   = static_cast<uint8_t *>(q);
        cap = n;
        return true;
    }
    uint8_t *grow(size_t n) { // n bytes appended, uninitialised; nullptr on OOM
        if (!reserve(len + n)) return nullptr;
        uint8_t *at = p + len;
        len += n;
        return at;
    }
    void put(const void *src, size_t n) {
        if (uint8_t *d = grow(n)) memcpy(d, src, n);
    }
    void u8(uint8_t v) { put(&v, 1); }
    void u16(uint16_t v) { put(&v, 2); } // little-endian: this is an x86 build, and the format is LE
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void release() { // free the storage and zero the struct
        free(p);
        p   = nullptr;
        len = cap = 0;
        oom       = false;
    }
};

inline void poke32(uint8_t *at, uint32_t v) { memcpy(at, &v, 4); }
inline void poke16(uint8_t *at, uint16_t v) { memcpy(at, &v, 2); }

// ---- header ------------------------------------------------------------------------------------
// Names longer than 255 bytes are cut (the table stores a u8 length); no manifest name is close.
inline void put_header(buf &o, uint16_t flags, uint64_t manifest_fp, uint32_t keyframe_every,
                       const char *const *names, const uint32_t *lens, int n) {
    const size_t at = o.len;
    o.u32(MAGIC);
    o.u16(VERSION);
    o.u16(flags);
    o.u32(0); // header_len, patched below
    o.u64(manifest_fp);
    o.u32(keyframe_every);
    o.u32((uint32_t)n);
    for (int i = 0; i < n; ++i) {
        size_t nl = strlen(names[i]);
        if (nl > 255) nl = 255;
        o.u32(lens[i]);
        o.u8((uint8_t)nl);
        o.put(names[i], nl);
    }
    if (!o.oom) poke32(o.p + at + 8, (uint32_t)(o.len - at));
}

// ---- chunks ------------------------------------------------------------------------------------
// Compute and store the CRC of a complete chunk that starts at `chunk` (its payload_len says how far).
inline void seal_chunk(uint8_t *chunk) {
    uint32_t n;
    memcpy(&n, chunk + 4, 4);
    poke32(chunk + 8, crc32(chunk + CHUNK_HDR, n));
}

// One STEP chunk, streamed. `run()`'s `bytes` must point INTO A CONTIGUOUS IMAGE OF THE REGION at
// `off` -- coalescing reads the <= RUN_GAP unchanged bytes BEFORE it (bytes - gap), which is what
// the live memory, the tracker's shadow and every test image are. Runs must arrive in ascending
// (region, offset) order, which is the order inc::tracker::update emits them in; a run that does not
// follow the previous one is simply started fresh (never merged backwards).
struct step_builder {
    buf     *out       = nullptr;
    size_t   chunk_at  = 0;
    uint32_t runs      = 0;
    int      last_reg  = -1;
    uint32_t last_from = 0, last_to = 0;
    size_t   last_hdr = 0;

    void begin(buf &o, uint32_t step) {
        out      = &o;
        chunk_at = o.len;
        runs     = 0;
        last_reg = -1;
        o.u32(TAG_STEP);
        o.u32(0); // payload_len
        o.u32(0); // crc
        o.u32(step);
        o.u32(0); // run_count
    }

    void run(int region, uint32_t off, uint32_t len, const uint8_t *bytes) {
        while (len > 0) {
            if (region == last_reg && off >= last_to && off - last_to <= RUN_GAP &&
                off + len - last_from <= RUN_MAX) {
                const uint32_t gap = off - last_to;
                out->put(bytes - gap, (size_t)gap + len);
                last_to = off + len;
                if (!out->oom) poke16(out->p + last_hdr + 6, (uint16_t)(last_to - last_from));
                return;
            }
            const uint32_t n = len > RUN_MAX ? RUN_MAX : len;
            last_hdr         = out->len;
            out->u16((uint16_t)region);
            out->u32(off);
            out->u16((uint16_t)n);
            out->put(bytes, n);
            ++runs;
            last_reg  = region;
            last_from = off;
            last_to   = off + n;
            off += n;
            bytes += n;
            len -= n;
        }
    }

    // Patch the lengths and seal. Returns the chunk's total size (0 after an OOM).
    size_t end() {
        if (out->oom) return 0;
        uint8_t *c = out->p + chunk_at;
        poke32(c + 4, (uint32_t)(out->len - chunk_at - CHUNK_HDR));
        poke32(c + CHUNK_HDR + 4, runs);
        seal_chunk(c);
        return out->len - chunk_at;
    }
};

inline size_t keyf_chunk_bytes(uint64_t total_state) { return CHUNK_HDR + 4 + (size_t)total_state; }

// One KEYF chunk: `step`, then parts[0..n) (lens[i] bytes each) concatenated in table order.
// `seal` false leaves crc = 0 for the caller to seal_chunk() later (the writer thread does).
inline void put_keyf(buf &o, uint32_t step, const uint8_t *const *parts, const uint32_t *lens, int n, bool seal) {
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) total += lens[i];
    if (!o.reserve(o.len + keyf_chunk_bytes(total))) return;
    const size_t at = o.len;
    o.u32(TAG_KEYF);
    o.u32((uint32_t)(4 + total));
    o.u32(0);
    o.u32(step);
    for (int i = 0; i < n; ++i) o.put(parts[i], lens[i]);
    if (seal && !o.oom) seal_chunk(o.p + at);
}

// `raw_bytes_written`: the file's size BEFORE this END chunk (header + every chunk), which is what
// tools/state_record.py's own synthetic builder writes there.
inline void put_end(buf &o, uint32_t last_step, uint32_t steps_recorded, uint64_t raw_bytes_written) {
    const size_t at = o.len;
    o.u32(TAG_END);
    o.u32(16);
    o.u32(0);
    o.u32(last_step);
    o.u32(steps_recorded);
    o.u64(raw_bytes_written);
    if (!o.oom) seal_chunk(o.p + at);
}

// The first recorded step is a keyframe; then every `every` steps after it (every == 0: only the
// first). Relative to the first step, as the reader's own synthetic builder does it.
inline bool keyframe_due(uint32_t step, uint32_t first_step, uint32_t every) {
    if (step == first_step) return true;
    return every != 0 && step > first_step && ((step - first_step) % every) == 0;
}

} // namespace mh::desync::srec
